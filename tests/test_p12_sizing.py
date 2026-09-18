"""P12: the section-2 buffer-sizing declarations.

Metadata section 2 tells a reader how large a buffer to allocate for the
segment. meflib-based readers (CyberPSG and friends) trust those numbers, and
`0` is not the NO_ENTRY sentinel for any of them — a reader cannot tell an
unset field from a real zero, so an under-declared value truncates or NULLs its
buffer. mef3io <= 1.1.2 left `maximum_difference_bytes` and
`maximum_contiguous_block_bytes` at 0 and over-declared the other
`maximum_contiguous_*` fields.

These tests pin the written values against the blocks actually on disk, and pin
that *reading* never depends on the fields — sessions written by older mef3io
(zeros) or by a foreign writer (NO_ENTRY) must keep reading identically.
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
from mef3io import _mef3io as m  # noqa: E402

START = 1577836800000000
FS = 256.0

UH_BYTES = 1024
METADATA_FILE_BYTES = 16384
S2 = 1024 + 1536  # universal header + section 1
TIDX_RECORD_BYTES = 56
RED_DIFFERENCE_BYTES_OFFSET = 28  # ui4 within the 304-byte RED block header
DISCONTINUITY_MASK = 0x01

UI4_NO_ENTRY = 0xFFFFFFFF
SI8_NO_ENTRY = -1

# name -> (section-relative offset, struct format)
FIELDS = {
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

SIZING_FIELDS = (
    "maximum_block_bytes",
    "maximum_block_samples",
    "maximum_difference_bytes",
    "maximum_contiguous_blocks",
    "maximum_contiguous_block_bytes",
    "maximum_contiguous_samples",
)


def _segments(path):
    return sorted(Path(path).rglob("*.tmet"))


def _read_section2(tmet):
    raw = Path(tmet).read_bytes()
    return {n: struct.unpack_from(f, raw, S2 + off)[0] for n, (off, f) in FIELDS.items()}


def _patch_section2(tmet, **values):
    """Overwrite section-2 fields in place, repairing both universal-header CRCs.

    The body CRC covers raw[1024:METADATA_FILE_BYTES] and lives inside the
    header CRC's window, so it has to be written first.
    """
    tmet = Path(tmet)
    raw = bytearray(tmet.read_bytes())
    for name, value in values.items():
        off, fmt = FIELDS[name]
        struct.pack_into(fmt, raw, S2 + off, value)
    struct.pack_into("<I", raw, 4, m.crc32(bytes(raw[UH_BYTES:METADATA_FILE_BYTES])))
    struct.pack_into("<I", raw, 0, m.crc32(bytes(raw[4:UH_BYTES])))
    tmet.write_bytes(bytes(raw))


def _real_stats(tmet):
    """Recompute the sizing statistics from the blocks on disk.

    `maximum_difference_bytes` comes from the RED block headers in .tdat; the
    contiguous run maxima come from the .tidx, delimited by the same
    discontinuity flag a reader uses.
    """
    tmet = Path(tmet)
    entries = tmet.with_suffix(".tidx").read_bytes()[UH_BYTES:]
    stats = dict.fromkeys(SIZING_FIELDS, 0)
    stats["number_of_blocks"] = len(entries) // TIDX_RECORD_BYTES
    run_blocks = run_bytes = run_samples = 0
    with open(tmet.with_suffix(".tdat"), "rb") as tdat:
        for i in range(stats["number_of_blocks"]):
            e = entries[i * TIDX_RECORD_BYTES : (i + 1) * TIDX_RECORD_BYTES]
            (file_offset,) = struct.unpack_from("<q", e, 0)
            (n_samples,) = struct.unpack_from("<I", e, 24)
            (block_bytes,) = struct.unpack_from("<I", e, 28)
            (flags,) = struct.unpack_from("<B", e, 44)

            tdat.seek(file_offset + RED_DIFFERENCE_BYTES_OFFSET)
            (difference_bytes,) = struct.unpack("<I", tdat.read(4))

            stats["maximum_block_bytes"] = max(stats["maximum_block_bytes"], block_bytes)
            stats["maximum_block_samples"] = max(stats["maximum_block_samples"], n_samples)
            stats["maximum_difference_bytes"] = max(
                stats["maximum_difference_bytes"], difference_bytes
            )

            if flags & DISCONTINUITY_MASK:
                run_blocks = run_bytes = run_samples = 0
            run_blocks += 1
            run_bytes += block_bytes
            run_samples += n_samples
            stats["maximum_contiguous_blocks"] = max(stats["maximum_contiguous_blocks"], run_blocks)
            stats["maximum_contiguous_block_bytes"] = max(
                stats["maximum_contiguous_block_bytes"], run_bytes
            )
            stats["maximum_contiguous_samples"] = max(
                stats["maximum_contiguous_samples"], run_samples
            )
    return stats


def _write(path, gap_us=int(5e6), n=4000, channels=("ch1",)):
    rng = np.random.default_rng(0)
    x = rng.normal(0, 3000, n).astype(np.int32)
    w = mef3io.Writer(path)
    for ch in channels:
        w.write_int32(ch, x, 0.5, START, FS)
        w.write_int32(ch, x, 0.5, START + int(n / FS * 1e6) + gap_us, FS)
    w.close()
    return x


# --- what gets written -------------------------------------------------------


def test_sizing_fields_match_the_blocks_on_disk(tmp_path):
    path = str(tmp_path / "s.mefd")
    _write(path, channels=("ch1", "ch2"))

    for tmet in _segments(path):
        stored = _read_section2(tmet)
        real = _real_stats(tmet)
        for name in SIZING_FIELDS:
            assert stored[name] == real[name], f"{name} in {tmet.name}"


def test_no_sizing_field_is_zero_or_no_entry(tmp_path):
    """0 is indistinguishable from 'unset' to a reader, and NO_ENTRY makes it
    allocate nothing (or 4 GB). Every field must carry a real measurement."""
    path = str(tmp_path / "s.mefd")
    _write(path)

    for tmet in _segments(path):
        stored = _read_section2(tmet)
        for name in SIZING_FIELDS:
            assert stored[name] > 0, f"{name} left unset"
        assert stored["maximum_difference_bytes"] != UI4_NO_ENTRY
        assert stored["maximum_block_samples"] != UI4_NO_ENTRY
        assert stored["maximum_contiguous_block_bytes"] != SI8_NO_ENTRY


def test_contiguous_fields_respect_discontinuities(tmp_path):
    """A gap splits the segment: the contiguous maxima describe the longest run,
    not the whole channel (which is what <= 1.1.2 declared)."""
    path = str(tmp_path / "s.mefd")
    _write(path, gap_us=int(5e6))

    stored = _read_section2(_segments(path)[0])
    assert stored["number_of_blocks"] > stored["maximum_contiguous_blocks"]
    assert stored["number_of_samples"] > stored["maximum_contiguous_samples"]
    assert stored["maximum_contiguous_samples"] == 4000


def test_difference_bytes_within_meflib_worst_case(tmp_path):
    """meflib sizes its difference buffer at 5 bytes/sample worst case; a real
    maximum above that would mean a reader trusting the bound overflows."""
    path = str(tmp_path / "s.mefd")
    _write(path)

    stored = _read_section2(_segments(path)[0])
    assert stored["maximum_difference_bytes"] <= 5 * stored["maximum_block_samples"]


# --- appends -----------------------------------------------------------------


def test_append_keeps_sizing_fields_exact(tmp_path):
    path = str(tmp_path / "s.mefd")
    x = _write(path, gap_us=0)
    w = mef3io.Writer(path)
    w.write_int32("ch1", x, 0.5, START + int(3 * 4000 / FS * 1e6), FS)
    w.close()

    tmet = _segments(path)[0]
    stored = _read_section2(tmet)
    real = _real_stats(tmet)
    for name in SIZING_FIELDS:
        assert stored[name] == real[name], name


def test_append_repairs_fields_left_unset_by_older_mef3io(tmp_path):
    """A segment written by mef3io <= 1.1.2 carries 0s. Appending must not
    propagate them: the result has to cover every block, old ones included."""
    path = str(tmp_path / "s.mefd")
    x = _write(path, gap_us=0)
    tmet = _segments(path)[0]
    before = _real_stats(tmet)

    _patch_section2(
        tmet,
        maximum_difference_bytes=0,
        maximum_contiguous_block_bytes=0,
        maximum_contiguous_blocks=before["number_of_blocks"],
    )
    assert _read_section2(tmet)["maximum_difference_bytes"] == 0

    w = mef3io.Writer(path)
    w.write_int32("ch1", x, 0.5, START + int(3 * 4000 / FS * 1e6), FS)
    w.close()

    stored = _read_section2(tmet)
    real = _real_stats(tmet)
    # The old blocks' difference_bytes are only in .tdat, so the append bounds
    # them by meflib's worst case instead of re-reading: never below the truth.
    assert stored["maximum_difference_bytes"] >= real["maximum_difference_bytes"]
    assert stored["maximum_difference_bytes"] <= 5 * real["maximum_block_samples"]
    # Everything the index describes is repaired exactly.
    assert stored["maximum_contiguous_block_bytes"] == real["maximum_contiguous_block_bytes"]
    assert stored["maximum_contiguous_blocks"] == real["maximum_contiguous_blocks"]
    assert stored["maximum_contiguous_samples"] == real["maximum_contiguous_samples"]


def test_append_does_not_preserve_a_no_entry_block_maximum(tmp_path):
    """A foreign segment may carry NO_ENTRY in maximum_block_samples. Folding
    the new blocks in with max() would keep 0xFFFFFFFF forever — and
    block_interval, derived from it, would be nonsense."""
    path = str(tmp_path / "s.mefd")
    x = _write(path, gap_us=0)
    tmet = _segments(path)[0]
    _patch_section2(
        tmet, maximum_block_samples=UI4_NO_ENTRY, maximum_block_bytes=SI8_NO_ENTRY
    )

    w = mef3io.Writer(path)
    w.write_int32("ch1", x, 0.5, START + int(3 * 4000 / FS * 1e6), FS)
    w.close()

    stored = _read_section2(tmet)
    real = _real_stats(tmet)
    assert stored["maximum_block_samples"] == real["maximum_block_samples"]
    assert stored["maximum_block_bytes"] == real["maximum_block_bytes"]
    # Derived from the real block geometry, not from 0xFFFFFFFF samples (which
    # would put block_interval around 1.7e13 us instead of ~1e7).
    assert stored["block_interval"] == round(real["maximum_block_samples"] * 1e6 / FS)


def test_append_bounds_no_entry_difference_bytes(tmp_path):
    """A foreign writer may leave NO_ENTRY; that must not survive an append
    either, or a reader sizing from it tries to allocate 4 GB."""
    path = str(tmp_path / "s.mefd")
    x = _write(path, gap_us=0)
    tmet = _segments(path)[0]
    _patch_section2(tmet, maximum_difference_bytes=UI4_NO_ENTRY)

    w = mef3io.Writer(path)
    w.write_int32("ch1", x, 0.5, START + int(3 * 4000 / FS * 1e6), FS)
    w.close()

    stored = _read_section2(tmet)
    real = _real_stats(tmet)
    assert stored["maximum_difference_bytes"] != UI4_NO_ENTRY
    assert stored["maximum_difference_bytes"] >= real["maximum_difference_bytes"]


# --- reading stays independent of the fields ---------------------------------


@pytest.mark.parametrize(
    "patch",
    [
        pytest.param(
            dict(
                maximum_difference_bytes=0,
                maximum_contiguous_block_bytes=0,
                maximum_contiguous_blocks=0,
                maximum_contiguous_samples=0,
                maximum_block_bytes=0,
                maximum_block_samples=0,
            ),
            id="zeros-as-written-by-1.1.2",
        ),
        pytest.param(
            dict(
                maximum_difference_bytes=UI4_NO_ENTRY,
                maximum_block_samples=UI4_NO_ENTRY,
                maximum_contiguous_block_bytes=SI8_NO_ENTRY,
                maximum_contiguous_blocks=SI8_NO_ENTRY,
                maximum_contiguous_samples=SI8_NO_ENTRY,
                maximum_block_bytes=SI8_NO_ENTRY,
            ),
            id="no-entry-sentinels",
        ),
        pytest.param(
            dict(maximum_difference_bytes=1, maximum_contiguous_samples=1),
            id="absurdly-under-declared",
        ),
    ],
)
def test_reader_ignores_section2_sizing_fields(tmp_path, patch):
    """Backwards compatibility: mef3io sizes from the block headers themselves,
    so every session it could read before it still reads, byte for byte."""
    path = str(tmp_path / "s.mefd")
    _write(path, gap_us=int(5e6))

    with mef3io.Reader(path) as r:
        expected = r.read("ch1")
        expected_raw = r.read_raw("ch1")

    for tmet in _segments(path):
        _patch_section2(tmet, **patch)

    with mef3io.Reader(path) as r:
        got = r.read("ch1")
        got_raw = r.read_raw("ch1")
    np.testing.assert_array_equal(
        np.nan_to_num(got, nan=-12345), np.nan_to_num(expected, nan=-12345)
    )
    np.testing.assert_array_equal(got_raw["samples"], expected_raw["samples"])
    np.testing.assert_array_equal(got_raw["valid"], expected_raw["valid"])


# --- backwards compatibility with sessions written by mef3io <= 1.1.2 --------


def test_a_1_1_2_style_session_still_reads_and_repairs_losslessly(tmp_path):
    """Sessions already on disk from mef3io <= 1.1.2 must keep working.

    Those exist in the field and cannot be rewritten from source, so three
    things have to hold, and none of them may regress quietly:

      * mef3io reads them bit-identically — the read path sizes every buffer
        from each block's own header and must never consult these fields;
      * the validator flags them rather than passing them silently;
      * repairing them changes declarations ONLY. `.tdat` must come back
        byte-identical, and the samples must survive unchanged.

    The declarations reproduced here are exactly what 1.1.2's writer emitted:
    `maximum_difference_bytes` and `maximum_contiguous_block_bytes` left at 0
    (0 is not the NO_ENTRY sentinel for either), and the other two
    `maximum_contiguous_*` fields set to the channel totals rather than to the
    longest run between discontinuities.
    """
    path = tmp_path / "s.mefd"
    written = _write(path, gap_us=int(5e6))
    tmet = _segments(path)[0]
    before = _read_section2(tmet)
    tdat = Path(tmet).with_suffix(".tdat")
    tdat_before = tdat.read_bytes()

    with mef3io.Reader(str(path)) as r:
        samples_before = r.read_raw("ch1")["samples"].copy()

    _patch_section2(
        tmet,
        maximum_difference_bytes=0,
        maximum_contiguous_block_bytes=0,
        maximum_contiguous_blocks=before["number_of_blocks"],
        maximum_contiguous_samples=before["number_of_samples"],
    )

    # 1. Reads are unaffected by the bad declarations.
    with mef3io.Reader(str(path)) as r:
        assert np.array_equal(r.read_raw("ch1")["samples"], samples_before)

    # 2. The validator does not wave it through.
    ids = {f.check_id for f in mef3io.Validator(str(path)).validate().findings}
    assert "sizing.difference-bytes" in ids
    assert "sizing.contiguous" in ids

    # 3. Repair rewrites declarations and nothing else.
    mef3io.repair_session(str(path), ["sizing.difference-bytes", "sizing.contiguous"])
    assert tdat.read_bytes() == tdat_before, "sample data must not be touched"
    after = _read_section2(tmet)
    assert after["maximum_difference_bytes"] > 0
    assert after["maximum_contiguous_block_bytes"] > 0
    assert mef3io.Validator(str(path)).validate().ok

    with mef3io.Reader(str(path)) as r:
        assert np.array_equal(r.read_raw("ch1")["samples"], samples_before)
    assert np.array_equal(samples_before[: len(written)], written)


def _patch_uh_field(file, offset, value):
    """Set one si8 universal-header field, repairing the header CRC."""
    raw = bytearray(Path(file).read_bytes())
    struct.pack_into("<q", raw, offset, value)
    struct.pack_into("<I", raw, 0, m.crc32(bytes(raw[4:UH_BYTES])))
    Path(file).write_bytes(bytes(raw))


def test_append_sets_universal_header_counts_from_the_index(tmp_path):
    """Both .tdat/.tidx header fields must describe the segment, not be folded
    onto whatever the old header claimed.

    meflib clamps number_of_blocks DOWN to number_of_entries
    (meflib.c:5983-5984, :6005-6006) with no floor, so an entry count folded
    onto meflib's own NO_ENTRY (-1) makes the segment read short — empty, when
    a single block is appended. maximum_entry_size folded onto a stored value
    keeps the legacy writer's sample count, which is far below the real
    largest block in bytes.
    """
    path = tmp_path / "s.mefd"
    _write(path, gap_us=0, n=4000)
    tmet = _segments(path)[0]
    tidx, tdat = Path(tmet).with_suffix(".tidx"), Path(tmet).with_suffix(".tdat")

    # meflib's own convention on both headers, as a foreign writer leaves them.
    NUMBER_OF_ENTRIES, MAXIMUM_ENTRY_SIZE = 32, 40
    for f in (tidx, tdat):
        _patch_uh_field(f, NUMBER_OF_ENTRIES, -1)
        _patch_uh_field(f, MAXIMUM_ENTRY_SIZE, -1)

    w = mef3io.Writer(str(path))
    w.write_int32("ch1", np.arange(100, dtype=np.int32), 0.5,
                  START + int(8000 / FS * 1e6) + int(1e6), FS)
    w.close()

    blocks = (len(tidx.read_bytes()) - UH_BYTES) // TIDX_RECORD_BYTES
    true_max_bytes = _real_stats(tmet)["maximum_block_bytes"]
    for f in (tidx, tdat):
        raw = Path(f).read_bytes()
        entries = struct.unpack_from("<q", raw, NUMBER_OF_ENTRIES)[0]
        assert entries == blocks, f"{Path(f).suffix}: {entries} != {blocks} blocks on disk"
    tdat_max = struct.unpack_from("<q", tdat.read_bytes(), MAXIMUM_ENTRY_SIZE)[0]
    assert tdat_max == true_max_bytes, f".tdat maximum_entry_size {tdat_max} != {true_max_bytes}"

    # And mef3io's own output must satisfy mef3io's own validator.
    assert mef3io.Validator(str(path)).validate().ok


# --- acceptance: bidirectional oracle, including the allocation contract -----


ALLOCATION_FIELDS = (
    "maximum_block_bytes",
    "maximum_block_samples",
    "maximum_difference_bytes",
    "number_of_discontinuities",
    "maximum_contiguous_blocks",
    "maximum_contiguous_block_bytes",
    "maximum_contiguous_samples",
)


def _assert_allocation_contract(session):
    """Every field a meflib-based reader allocates from is set and exact.

    `0` is not the NO_ENTRY sentinel for any of these, so an unset one is
    indistinguishable from a measured one and the reader sizes a buffer from
    it. Checked against the blocks actually on disk, not merely for
    non-zeroness, so an over-declaration fails too — the requirement is exact,
    not merely safe.
    """
    for tmet in _segments(session):
        declared, real = _read_section2(tmet), _real_stats(tmet)
        for name in ALLOCATION_FIELDS:
            v = declared[name]
            assert v not in (0, UI4_NO_ENTRY, -1), f"{tmet.name}: {name} is unset ({v})"
            if name in real:
                assert v == real[name], f"{tmet.name}: {name} declared {v}, on disk {real[name]}"


def test_acceptance_mef3io_written_session_is_exact_and_oracle_readable(tmp_path):
    """A session this version writes: declarations exact, and the oracle reads
    every sample of it bit-identically through both of its APIs."""
    pymef = pytest.importorskip("pymef.mef_session", reason="oracle not installed")

    path = tmp_path / "s.mefd"
    x = _write(path, gap_us=int(3e6), n=6000, channels=("ch1", "ch2"))
    assert mef3io.Validator(str(path)).validate().ok
    _assert_allocation_contract(path)

    n = 2 * len(x)
    s = pymef.MefSession(str(path), "")
    try:
        for ch in ("ch1", "ch2"):
            got = np.asarray(s.read_ts_channels_sample(ch, [0, n])).astype(np.int32)
            assert np.array_equal(got, np.concatenate([x, x])), f"{ch}: sample read"
            span = [START, START + int(len(x) / FS * 1e6) + int(3e6) + int(len(x) / FS * 1e6)]
            g = np.asarray(s.read_ts_channels_uutc(ch, span), dtype=float)
            present = ~np.isnan(g)
            assert present.sum() > 0 and (~present).sum() > 0, "the gap must survive as NaN"
            assert np.array_equal(g[present].astype(np.int32),
                                  np.concatenate([x, x])[: int(present.sum())]), f"{ch}: uutc read"
    finally:
        s.close()


def test_acceptance_legacy_session_reads_and_upgrades_losslessly(tmp_path):
    """A session the legacy stack wrote: mef3io reads it bit-identically, the
    fixer brings its declarations up to exact, and BOTH readers still return
    the same samples afterwards."""
    pymef = pytest.importorskip("pymef.mef_session", reason="oracle not installed")
    pytest.importorskip("mef_tools", reason="legacy writer not installed")
    from mef_tools.io import MefWriter

    path = tmp_path / "legacy.mefd"
    rng = np.random.default_rng(3)
    x = rng.integers(-30000, 30000, 6000).astype(np.int32)
    w = MefWriter(str(path), overwrite=True)
    w.write_data(x[:3000], "ch1", START, FS, precision=0)
    w.write_data(x[3000:], "ch1", START + int(3000 / FS * 1e6) + int(3e6), FS, precision=0)
    del w

    def read_mef3io():
        with mef3io.Reader(str(path)) as r:
            d = r.read_raw("ch1")
        return d["samples"][d["valid"].astype(bool)].astype(np.int32)

    def read_oracle():
        s = pymef.MefSession(str(path), "")
        try:
            return np.asarray(s.read_ts_channels_sample("ch1", [0, len(x)])).astype(np.int32)
        finally:
            s.close()

    assert np.array_equal(read_mef3io(), x), "mef3io must read the legacy session as written"
    assert np.array_equal(read_oracle(), x)

    report = mef3io.Validator(str(path)).validate()
    assert not report.ok, "the legacy declarations really are wrong"
    mef3io.repair_session(str(path), report.repairable_check_ids)

    assert mef3io.Validator(str(path)).validate().ok, "the fixer must leave it clean"
    _assert_allocation_contract(path)
    assert np.array_equal(read_mef3io(), x), "repair must not disturb the samples"
    assert np.array_equal(read_oracle(), x), "the oracle must still read it after repair"


def test_append_onto_a_padded_index_keeps_the_session_readable(tmp_path):
    """Trailing `.tidx` padding is tolerated on READ, so it must survive a write.

    Foreign writers pad past the last index entry and `crc.index` deliberately
    accepts that (it bounds its hash by the declared entry count). But the
    append used to insert new entries after the padding, putting every one of
    them at a broken stride — the reader then rejected the whole file with
    "index file body is not a whole number of entries". mef3io corrupting a
    session it considers valid, with its own writer.
    """
    path = tmp_path / "s.mefd"
    _write(path, gap_us=0, n=4000)
    tmet = _segments(path)[0]
    tidx = Path(tmet).with_suffix(".tidx")

    raw = bytearray(tidx.read_bytes()) + bytes(16)  # under one entry: tolerated
    struct.pack_into("<I", raw, 4, m.crc32(bytes(raw[UH_BYTES:])))
    struct.pack_into("<I", raw, 0, m.crc32(bytes(raw[4:UH_BYTES])))
    tidx.write_bytes(bytes(raw))
    assert mef3io.Validator(str(path)).validate().ok, "the padding must be tolerated"

    w = mef3io.Writer(str(path))
    w.write_int32("ch1", np.arange(500, dtype=np.int32), 0.5,
                  START + int(8000 / FS * 1e6), FS)
    w.close()

    body = len(tidx.read_bytes()) - UH_BYTES
    assert body % TIDX_RECORD_BYTES == 0, "the index must stay a whole number of entries"
    assert mef3io.Validator(str(path)).validate().ok
    with mef3io.Reader(str(path)) as r:
        assert r.read_raw("ch1")["samples"].size > 0


def test_append_refuses_an_index_entry_with_an_unknown_size(tmp_path):
    """An entry at NO_ENTRY cannot be totalled, so the append must not guess.

    Coercing the sentinel to 0 keeps it from inflating a total, but then every
    figure derived from the index is too SMALL — and the append writes those
    figures back as the segment's declarations, under-declaring a reader's
    buffer. Refusing is the safe answer; the validator names the segment.
    """
    path = tmp_path / "s.mefd"
    _write(path, gap_us=0, n=4000)
    tmet = _segments(path)[0]
    tidx = Path(tmet).with_suffix(".tidx")

    raw = bytearray(tidx.read_bytes())
    struct.pack_into("<I", raw, UH_BYTES + 24, UI4_NO_ENTRY)  # entry 0 sample count
    struct.pack_into("<I", raw, 4, m.crc32(bytes(raw[UH_BYTES:])))
    struct.pack_into("<I", raw, 0, m.crc32(bytes(raw[4:UH_BYTES])))
    tidx.write_bytes(bytes(raw))

    before = _read_section2(tmet)["number_of_samples"]
    w = mef3io.Writer(str(path))
    with pytest.raises(RuntimeError, match="NO_ENTRY|unset"):
        w.write_int32("ch1", np.arange(100, dtype=np.int32), 0.5,
                      START + int(8000 / FS * 1e6), FS)
    try:
        w.close()
    except Exception:
        pass
    assert _read_section2(tmet)["number_of_samples"] == before, "declarations must not shrink"


def test_append_replaces_an_under_declared_difference_bytes(tmp_path):
    """Appending must not carry an existing under-declaration forward.

    Screening only 0 and NO_ENTRY treated a stored `1` as a real measurement,
    so appending to an already-defective segment preserved the crash-class
    defect rather than repairing it. Anything below the worst case for the
    largest block cannot be verified without re-reading every old block header,
    so the bound is taken instead.
    """
    path = tmp_path / "s.mefd"
    _write(path, gap_us=0, n=4000)
    tmet = _segments(path)[0]
    _patch_section2(tmet, maximum_difference_bytes=1)  # a real under-declaration

    w = mef3io.Writer(str(path))
    w.write_int32("ch1", np.arange(500, dtype=np.int32), 0.5,
                  START + int(8000 / FS * 1e6), FS)
    w.close()

    declared = _read_section2(tmet)
    assert declared["maximum_difference_bytes"] > 1, "the under-declaration rode forward"
    assert declared["maximum_difference_bytes"] >= _real_stats(tmet)["maximum_difference_bytes"]
    assert mef3io.Validator(str(path)).validate().ok
