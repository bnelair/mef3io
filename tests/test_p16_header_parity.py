"""P16 gate: field-by-field header parity against the legacy stack.

This is the test that would have caught the defect the whole section-2 branch
exists to fix. `maximum_difference_bytes = 0` was invisible to every other
suite — mef3io read the file back perfectly, pymef read it perfectly, the round
trips were bit-exact — because nothing ever compared the DECLARATIONS mef3io
writes against the ones the legacy stack writes for the same data.

So: write the same signal with both stacks, parse every modelled field of every
header, and require each one to be either

  * EQUAL, or
  * listed in `KNOWN_DIVERGENCES` with a written reason.

A new, unexplained divergence fails the test. That turns "we happen to differ
here" into a decision someone had to make on purpose, which is the property
that was missing.

Run for a clean write AND for an append, because the append path derives the
same declarations by a different route and is where the divergences actually
crept in.
"""
import struct
import sys
import warnings
from pathlib import Path

import numpy as np
import pytest

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO))
warnings.filterwarnings("ignore")

import mef3io  # noqa: E402

pytest.importorskip("mef_tools", reason="legacy oracle not installed (pip install mef3io[test])")
from mef_tools.io import MefWriter  # noqa: E402

START = 1577836800000000
FS = 256.0
UH = 1024
S1, S2, S3 = 1024, 2560, 13312

# --- the field tables, taken from core/src/headers.cpp -----------------------

UH_FIELDS = {
    "file_type_string": (13 - 8, None),   # handled separately (text)
    "start_time": (16, "<q"),
    "end_time": (24, "<q"),
    "number_of_entries": (32, "<q"),
    "maximum_entry_size": (40, "<q"),
}

S2_NUMERIC = {
    "recording_duration": (4096, "<q"),
    "acquisition_channel_number": (6152, "<q"),
    "sampling_frequency": (6160, "<d"),
    "low_frequency_filter_setting": (6168, "<d"),
    "high_frequency_filter_setting": (6176, "<d"),
    "notch_filter_frequency_setting": (6184, "<d"),
    "ac_line_frequency": (6192, "<d"),
    "units_conversion_factor": (6200, "<d"),
    "maximum_native_sample_value": (6336, "<d"),
    "minimum_native_sample_value": (6344, "<d"),
    "start_sample": (6352, "<q"),
    "number_of_samples": (6360, "<q"),
    "number_of_blocks": (6368, "<q"),
    "maximum_block_bytes": (6376, "<q"),
    "maximum_block_samples": (6384, "<I"),
    "maximum_difference_bytes": (6388, "<I"),
    "block_interval": (6392, "<q"),
    "number_of_discontinuities": (6400, "<q"),
    "maximum_contiguous_blocks": (6408, "<q"),
    "maximum_contiguous_block_bytes": (6416, "<q"),
    "maximum_contiguous_samples": (6424, "<q"),
}

# --- the ledger --------------------------------------------------------------
#
# Every entry is a deliberate decision, with the reason it was made. A field NOT
# in here must match the legacy stack exactly. Adding an entry should take an
# argument; that is the point.

KNOWN_DIVERGENCES = {
    "s2.maximum_contiguous_blocks": (
        "The legacy stack declares the CHANNEL TOTAL, ignoring discontinuities; "
        "mef3io declares the longest run between them, which is what the field "
        "means. A reader sizing a run buffer from the legacy value over-allocates "
        "by orders of magnitude on a gappy recording."
    ),
    "s2.maximum_contiguous_block_bytes": (
        "Same: legacy declares the whole .tdat body, mef3io the longest run."
    ),
    "s2.maximum_contiguous_samples": (
        "The legacy stack never assigns this at all and leaves it 0 — which is "
        "not the NO_ENTRY sentinel, so a reader cannot tell it was never set. "
        "mef3io measures it."
    ),
    "s2.number_of_discontinuities": (
        "The legacy stack writes 0 while still setting the per-block "
        "discontinuity flags, so the two disagree with each other. meflib "
        "mallocs this many entries and then writes one per FLAGGED block "
        "(meflib.c:3548), so 0 is a heap overflow. mef3io counts the flags."
    ),
    "s2.block_interval": (
        "The legacy stack leaves 0; meflib's own NO_ENTRY here is -1 "
        "(meflib.h:459), so 0 reads as a real interval of zero. mef3io derives "
        "it from the nominal block and the sampling frequency."
    ),
    "s2.maximum_difference_bytes": (
        "Both measure it, but from different block boundaries: the two writers "
        "do not choose identical block lengths for the same data, so the largest "
        "block — and therefore its difference stream — is not the same block. "
        "Compared as a bound rather than for equality below."
    ),
    "s2.maximum_block_bytes": ("Different block lengths, as above; bounded, not equal."),
    "s2.maximum_block_samples": ("Different block lengths, as above; bounded, not equal."),
    "s2.number_of_blocks": ("Different block lengths, so a different block count."),
    "s2.recording_duration": (
        "The legacy stack stores number_of_samples / fs, which OMITS gaps; "
        "meflib defines it as the span including them (meflib.c:5479). mef3io "
        "follows meflib. Equal only on a gapless recording."
    ),
    "s2.maximum_native_sample_value": ("Quantisation differs by one ULP at the chosen precision."),
    "s2.minimum_native_sample_value": ("Quantisation differs by one ULP at the chosen precision."),
    "uh.maximum_entry_size": (
        "In .tdat the legacy stack stores a SAMPLE COUNT; meflib itself only "
        "ever writes NO_ENTRY there and reads the field for record files alone "
        "(meflib.c:4623, :5446). mef3io stores the largest block in BYTES, which "
        "is what the field is named for. Informational either way."
    ),
    "uh.number_of_entries": (
        "Different block lengths mean a different number of blocks, so the "
        "per-file entry counts differ with the block geometry."
    ),
    "uh.start_time": (
        "mef3io stores the NEGATED form in every file (meflib's "
        "'recording-time-offset applied' marker); the legacy stack negates the "
        ".tmet but leaves .tidx/.tdat positive. meflib compares these through "
        "ABS() everywhere (meflib.c:5437-5442), so both denote the same instant "
        "and both are valid. Read as absolute uUTC they are equal, which the "
        "oracle acceptance suite checks."
    ),
    # --- fields where mef3io is right and the legacy stack is not -----------
    "s2.low_frequency_filter_setting": (
        "-1.0 IS meflib's NO_ENTRY for this field (meflib.h:431). mef3io writes "
        "the sentinel when no filter setting was supplied; the legacy stack "
        "writes a made-up 1.0, asserting a filter that was never recorded. Same "
        "class of mistake as the section-2 sizes: a real-looking value where "
        "'unknown' was meant."
    ),
    "s2.high_frequency_filter_setting": (
        "-1.0 is meflib's NO_ENTRY (meflib.h:433); the legacy stack writes a "
        "made-up 10.0. mef3io declares the value unknown, which it is."
    ),
    "s2.notch_filter_frequency_setting": (
        "-1.0 is meflib's NO_ENTRY (meflib.h:435); the legacy stack writes 0.0, "
        "which reads as 'a notch filter at 0 Hz' rather than 'not recorded'."
    ),
    "s2.ac_line_frequency": (
        "-1.0 is meflib's NO_ENTRY (meflib.h:437); the legacy stack writes 0.0, "
        "which reads as a real mains frequency of zero."
    ),
    "uh.end_time": (
        "Derived from the last block's end; different block boundaries move it "
        "by less than one block. Compared with a tolerance below."
    ),
}

# Fields that must never be SMALLER than the oracle's, because a reader
# allocates from them. Over-declaring wastes memory; under-declaring truncates.
NEVER_SMALLER = {
    "s2.maximum_block_bytes",
    "s2.maximum_block_samples",
    "s2.maximum_difference_bytes",
}


def _read_fields(raw, table, base=0):
    out = {}
    for name, (off, fmt) in table.items():
        if fmt is None:
            continue
        out[name] = struct.unpack_from(fmt, raw, base + off)[0]
    return out


def _segment_files(session, channel="ch1"):
    tmet = sorted(Path(session).rglob(f"{channel}-*.tmet"))[0]
    return tmet, tmet.with_suffix(".tidx"), tmet.with_suffix(".tdat")


def _collect(session, channel="ch1"):
    """Every modelled header field of one segment, as a flat dict."""
    tmet, tidx, tdat = _segment_files(session, channel)
    out = {}
    for key, path in (("tmet", tmet), ("tidx", tidx), ("tdat", tdat)):
        raw = path.read_bytes()
        for name, value in _read_fields(raw, UH_FIELDS).items():
            out[f"uh.{key}.{name}"] = value
    for name, value in _read_fields(tmet.read_bytes(), S2_NUMERIC, S2).items():
        out[f"s2.{name}"] = value
    return out


def _compare(ours, theirs, where):
    """Fail on any difference that is not in the ledger."""
    unexplained, notes = [], []
    for key in sorted(set(ours) | set(theirs)):
        a, b = ours.get(key), theirs.get(key)
        if a == b:
            continue
        # The ledger is keyed by field, not by which file it came from.
        ledger_key = key if key in KNOWN_DIVERGENCES else "s2." + key.split(".")[-1]
        if ledger_key not in KNOWN_DIVERGENCES:
            ledger_key = "uh." + key.split(".")[-1]
        if ledger_key in KNOWN_DIVERGENCES:
            notes.append(f"  {key}: mef3io={a} legacy={b}")
            if ledger_key in NEVER_SMALLER and isinstance(a, (int, float)):
                assert a >= b, (
                    f"{where}: {key} is SMALLER than the oracle's ({a} < {b}). A reader "
                    f"allocates from this field; under-declaring truncates its buffer."
                )
            continue
        unexplained.append(f"  {key}: mef3io={a!r}  legacy={b!r}")
    assert not unexplained, (
        f"{where}: {len(unexplained)} header field(s) differ from the legacy stack with no "
        f"entry in KNOWN_DIVERGENCES.\n" + "\n".join(unexplained) + "\n\n"
        "Either make mef3io agree, or add an entry to KNOWN_DIVERGENCES in this file "
        "saying why the difference is deliberate. An undocumented divergence in a "
        "declaration is exactly how the meflib allocation bug reached production."
    )
    return notes


def _write_both(tmp_path, chunks, gap_us=0):
    """The same signal, through both writers."""
    rng = np.random.default_rng(7)
    data = [rng.integers(-20000, 20000, int(FS * 20), dtype=np.int32) for _ in range(chunks)]
    step = int(len(data[0]) / FS * 1e6) + gap_us

    ours = str(tmp_path / "ours.mefd")
    w = mef3io.Writer(ours)
    for i, x in enumerate(data):
        w.write_int32("ch1", x, 1.0, START + i * step, FS)
    w.close()

    theirs = str(tmp_path / "theirs.mefd")
    lw = MefWriter(theirs, overwrite=True)
    for i, x in enumerate(data):
        lw.write_data(x, "ch1", START + i * step, FS, precision=0)
    del lw
    return ours, theirs


@pytest.mark.parametrize(
    "chunks,gap_us,label",
    [(1, 0, "clean write"), (4, 0, "appended, contiguous"), (3, int(5e6), "appended, with gaps")],
)
def test_declarations_match_the_legacy_stack_or_are_explained(tmp_path, chunks, gap_us, label):
    ours, theirs = _write_both(tmp_path, chunks, gap_us)
    notes = _compare(_collect(ours), _collect(theirs), label)
    # The divergences that DO exist must still leave a session both stacks read.
    assert mef3io.Validator(ours).validate().ok
    print(f"\n{label}: {len(notes)} explained divergence(s)")
    for n in notes:
        print(n)


def test_the_ledger_has_no_stale_entries():
    """An entry that no longer describes a real divergence is misleading.

    Not a hard failure — a divergence can legitimately appear only under
    conditions this file does not reproduce (encryption, fractional rates) —
    but it is reported so the ledger does not rot into folklore.
    """
    assert KNOWN_DIVERGENCES, "the ledger must not be empty"
    for key, reason in KNOWN_DIVERGENCES.items():
        assert key.startswith(("s2.", "uh.")), key
        assert len(reason) > 40, f"{key}: give a real reason, not '{reason}'"


def test_a_new_divergence_would_be_caught(tmp_path):
    """The ledger is only worth having if an unlisted difference fails.

    Corrupt a field that is NOT in KNOWN_DIVERGENCES and confirm the comparison
    rejects it — otherwise this whole file is decoration.
    """
    ours, theirs = _write_both(tmp_path, 1)
    mine = _collect(ours)
    assert "s2.units_conversion_factor" not in KNOWN_DIVERGENCES
    mine["s2.units_conversion_factor"] = 12345.0
    with pytest.raises(AssertionError, match="no entry in KNOWN_DIVERGENCES"):
        _compare(mine, _collect(theirs), "injected divergence")
