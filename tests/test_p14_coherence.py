"""P14 gate: the operations agree with each other.

Every other suite tests one operation. This one tests the SEAMS — that writing,
appending, validating, repairing and archiving do not fight, in any order, on
sessions from any origin. The defects that motivated it were all of that shape:
an append that destroyed a file its own validator had just called clean; a
repair that derived truth from an index another check had already condemned; a
declaration one path wrote exactly and another quietly lowered.

The invariants asserted here, for every origin x operation sequence:

  1. SAMPLES ARE NEVER LOST. The oracle reads the same values throughout.
  2. NO ALLOCATION FIELD IS EVER UNDER-DECLARED, at any point. This is the
     crash contract: over-declaring costs a reader memory, under-declaring
     truncates the buffer it decodes into.
  3. REPAIR CONVERGES and is IDEMPOTENT — a second pass writes nothing.
  4. AN OPERATION NEVER INVALIDATES WHAT ANOTHER JUST BLESSED. Anything the
     validator calls clean stays readable after an append, and anything the
     repair fixes stays fixed after one.
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

START = 1577836800000000
FS = 250.0
UH = 1024
S2 = 2560
TSI = 56
DIFF_OFF = 28
UI4_NO_ENTRY = 0xFFFFFFFF
SI8_NO_ENTRY = -1

# The six fields a meflib-based reader allocates from.
ALLOC = {
    "maximum_block_bytes": (6376, "<q"),
    "maximum_block_samples": (6384, "<I"),
    "maximum_difference_bytes": (6388, "<I"),
    "maximum_contiguous_blocks": (6408, "<q"),
    "maximum_contiguous_block_bytes": (6416, "<q"),
    "maximum_contiguous_samples": (6424, "<q"),
}


def _segments(path):
    return sorted(Path(path).rglob("*.tmet"))


def _declared(tmet):
    raw = Path(tmet).read_bytes()
    return {n: struct.unpack_from(f, raw, S2 + o)[0] for n, (o, f) in ALLOC.items()}


def _truth(tmet):
    """Recompute the allocation fields from the blocks actually on disk."""
    seg = Path(tmet).parent
    tidx = next(seg.glob("*.tidx")).read_bytes()
    tdat = next(seg.glob("*.tdat")).read_bytes()
    n = (len(tidx) - UH) // TSI
    out = dict.fromkeys(ALLOC, 0)
    run_blocks = run_bytes = run_samples = 0
    for i in range(n):
        base = UH + i * TSI
        offset, = struct.unpack_from("<q", tidx, base)
        samples, = struct.unpack_from("<I", tidx, base + 24)
        block_bytes, = struct.unpack_from("<I", tidx, base + 28)
        flags, = struct.unpack_from("<B", tidx, base + 44)
        diff, = struct.unpack_from("<I", tdat, offset + DIFF_OFF)
        if flags & 0x01:                      # discontinuity: a run restarts
            run_blocks = run_bytes = run_samples = 0
        run_blocks += 1
        run_bytes += block_bytes
        run_samples += samples
        out["maximum_block_bytes"] = max(out["maximum_block_bytes"], block_bytes)
        out["maximum_block_samples"] = max(out["maximum_block_samples"], samples)
        out["maximum_difference_bytes"] = max(out["maximum_difference_bytes"], diff)
        out["maximum_contiguous_blocks"] = max(out["maximum_contiguous_blocks"], run_blocks)
        out["maximum_contiguous_block_bytes"] = max(
            out["maximum_contiguous_block_bytes"], run_bytes
        )
        out["maximum_contiguous_samples"] = max(out["maximum_contiguous_samples"], run_samples)
    return out


def assert_allocation_contract(path, where):
    """Invariant 2, checked at every step of every sequence."""
    for tmet in _segments(path):
        declared, truth = _declared(tmet), _truth(tmet)
        for name in ALLOC:
            value = declared[name]
            assert value not in (0, UI4_NO_ENTRY, SI8_NO_ENTRY), (
                f"{where}: {tmet.name} leaves {name} unset ({value}) — a reader "
                f"cannot tell that from a measurement"
            )
            assert value >= truth[name], (
                f"{where}: {tmet.name} UNDER-DECLARES {name}: {value} < {truth[name]}"
            )


def _digest(path):
    import hashlib

    h = hashlib.sha256()
    for f in sorted(Path(path).rglob("*")):
        if f.is_file():
            h.update(f.relative_to(path).as_posix().encode())
            h.update(f.read_bytes())
    return h.hexdigest()


def _read(path, channel="ch1", password=""):
    kw = {"password": password} if password else {}
    with mef3io.Reader(str(path), **kw) as r:
        return r.read(channel)


def _same(a, b):
    return np.array_equal(np.nan_to_num(a, nan=-98765.0), np.nan_to_num(b, nan=-98765.0))


# --- session origins ---------------------------------------------------------


def _mef3io_single(path, n=4000):
    x = np.random.default_rng(1).normal(0, 3000, n).astype(np.int32)
    w = mef3io.Writer(str(path))
    w.write_int32("ch1", x, 0.5, START, FS)
    w.close()
    return x


def _mef3io_chunked(path, n=4000):
    x = np.random.default_rng(2).normal(0, 3000, n).astype(np.int32)
    w = mef3io.Writer(str(path))
    w.write_int32("ch1", x, 0.5, START, FS)
    w.write_int32("ch1", x, 0.5, START + int(n / FS * 1e6), FS)
    w.close()
    return x


def _mef3io_gapped(path, n=4000):
    """A discontinuity, so the contiguous trio is not the whole channel."""
    x = np.random.default_rng(3).normal(0, 3000, n).astype(np.int32)
    w = mef3io.Writer(str(path))
    w.write_int32("ch1", x, 0.5, START, FS)
    w.write_int32("ch1", x, 0.5, START + int(n / FS * 1e6) + int(5e6), FS)
    w.close()
    return x


def _mef3io_reopened(path, n=4000):
    """Two Writer objects: the second adopts the segment from disk, which is
    the path that cannot verify the stored difference-bytes maximum."""
    x = np.random.default_rng(5).normal(0, 3000, n).astype(np.int32)
    w = mef3io.Writer(str(path))
    w.write_int32("ch1", x, 0.5, START, FS)
    w.close()
    w = mef3io.Writer(str(path))
    w.write_int32("ch1", x, 0.5, START + int(n / FS * 1e6), FS)
    w.close()
    return x


ORIGINS = {
    "single-call": _mef3io_single,
    "chunked": _mef3io_chunked,
    "with-a-gap": _mef3io_gapped,
    "reopened": _mef3io_reopened,
}


# --- the matrix --------------------------------------------------------------


@pytest.mark.parametrize("origin", sorted(ORIGINS))
def test_a_session_mef3io_writes_satisfies_its_own_validator(tmp_path, origin):
    """Invariant 4, the base case: nothing we write is something we condemn."""
    path = tmp_path / "s.mefd"
    ORIGINS[origin](path)

    report = mef3io.Validator(str(path)).validate()
    assert report.ok, report.summary()
    assert not report.findings, report.summary()
    assert_allocation_contract(path, f"as written ({origin})")


@pytest.mark.parametrize("origin", sorted(ORIGINS))
def test_repair_is_a_no_op_on_a_session_we_just_wrote(tmp_path, origin):
    """A repair must not "fix" output that was already correct.

    Every repairable check is selected deliberately, so a disagreement between
    what the writer declares and what a repair would declare shows up as
    changed bytes here.
    """
    path = tmp_path / "s.mefd"
    ORIGINS[origin](path)
    before = _digest(path)

    ids = [c.id for c in mef3io.available_checks() if c.repairable]
    report = mef3io.repair_session(str(path), ids)

    assert report.segments_repaired == 0, report.summary()
    assert _digest(path) == before, "a repair rewrote a session that was already correct"


@pytest.mark.parametrize("origin", sorted(ORIGINS))
def test_append_never_invalidates_a_clean_session(tmp_path, origin):
    """Invariant 4: the append must not break what the validator just blessed.

    This is the shape of the two worst defects found on this branch — a `.tmet`
    CRC hashed past the end of a fixed-length record, and an index extended
    across foreign padding — both of which made a session the validator called
    clean unreadable.
    """
    path = tmp_path / "s.mefd"
    x = ORIGINS[origin](path)
    assert mef3io.Validator(str(path)).validate().ok
    before = _read(path)

    w = mef3io.Writer(str(path))
    w.write_int32("ch1", x, 0.5, START + int(20 * len(x) / FS * 1e6), FS)
    w.close()

    report = mef3io.Validator(str(path)).validate()
    assert report.ok, f"the append broke a clean session: {report.summary()}"
    assert_allocation_contract(path, f"after append ({origin})")

    after = _read(path)
    assert len(after) > len(before)
    assert _same(after[: len(before)], before), "the append disturbed existing samples"


@pytest.mark.parametrize("origin", sorted(ORIGINS))
def test_repair_then_append_then_repair_converges(tmp_path, origin):
    """Invariant 3 and 4 together, on a session that genuinely needed repair.

    The declarations are zeroed the way mef3io <= 1.1.2 left them, then the
    sequence repair -> append -> repair has to end somewhere correct, with the
    second repair finding nothing left to do.
    """
    path = tmp_path / "s.mefd"
    x = ORIGINS[origin](path)
    expected = _read(path)

    # Simulate the 1.1.2 defect: the two fields that crash a meflib reader.
    import hashlib  # noqa: F401  (kept local; _fix_crcs needs the module below)

    for tmet in _segments(path):
        raw = bytearray(tmet.read_bytes())
        struct.pack_into("<I", raw, S2 + ALLOC["maximum_difference_bytes"][0], 0)
        struct.pack_into("<q", raw, S2 + ALLOC["maximum_contiguous_block_bytes"][0], 0)
        _fix_crcs(raw)
        tmet.write_bytes(bytes(raw))

    assert not mef3io.Validator(str(path)).validate().ok, "the corruption must register"

    ids = [c.id for c in mef3io.available_checks() if c.repairable]
    mef3io.repair_session(str(path), ids)
    assert mef3io.Validator(str(path)).validate().ok, "repair did not converge"
    assert_allocation_contract(path, f"after repair ({origin})")

    w = mef3io.Writer(str(path))
    w.write_int32("ch1", x, 0.5, START + int(20 * len(x) / FS * 1e6), FS)
    w.close()
    assert mef3io.Validator(str(path)).validate().ok, "append undid the repair"
    assert_allocation_contract(path, f"after repair+append ({origin})")

    # ...and there is nothing left for a second repair to do.
    digest = _digest(path)
    second = mef3io.repair_session(str(path), ids)
    assert second.segments_repaired == 0, second.summary()
    assert _digest(path) == digest, "repair is not idempotent"

    assert _same(_read(path)[: len(expected)], expected), "samples changed"


def _fix_crcs(raw):
    from mef3io import _mef3io as m

    struct.pack_into("<I", raw, 4, m.crc32(bytes(raw[UH:16384])))
    struct.pack_into("<I", raw, 0, m.crc32(bytes(raw[4:UH])))


@pytest.mark.parametrize("origin", sorted(ORIGINS))
def test_archive_round_trip_preserves_validity(tmp_path, origin):
    """Archiving is an operation too, and must not launder a session's state."""
    path = tmp_path / "s.mefd"
    ORIGINS[origin](path)
    expected = _read(path)
    before = _digest(path)

    archive = mef3io.archive_session(str(path))
    report = mef3io.Validator(str(archive)).validate()
    assert report.ok, report.summary()
    assert _same(_read(archive), expected), "the archive reads differently"

    extracted = mef3io.extract_session(str(archive), str(tmp_path / "out.mefd"))
    assert _digest(extracted) == before, "archive -> extract was not lossless"
    assert mef3io.Validator(str(extracted)).validate().ok


def test_the_oracle_agrees_at_every_step(tmp_path):
    """The whole sequence, checked against pymef rather than against ourselves."""
    pymef = pytest.importorskip("pymef.mef_session", reason="oracle not installed")
    path = tmp_path / "s.mefd"
    x = _mef3io_chunked(path)
    n = 2 * len(x)
    expected = np.concatenate([x, x])

    def oracle():
        session = pymef.MefSession(str(path), "")
        try:
            return np.asarray(session.read_ts_channels_sample("ch1", [0, n])).astype(
                np.int32
            ).ravel()
        finally:
            session.close()

    assert np.array_equal(oracle(), expected), "as written"

    ids = [c.id for c in mef3io.available_checks() if c.repairable]
    mef3io.repair_session(str(path), ids)
    assert np.array_equal(oracle(), expected), "after repair"

    w = mef3io.Writer(str(path))
    w.write_int32("ch1", x, 0.5, START + int(20 * len(x) / FS * 1e6), FS)
    w.close()
    assert np.array_equal(oracle(), expected), "after append (the first n samples)"
    assert_allocation_contract(path, "oracle sequence")


def test_an_encrypted_session_survives_the_same_sequence(tmp_path):
    """Encryption must not be a hole in any of the above: section 2 is
    ciphertext, so every repair and every append has to decrypt, edit and
    re-encrypt it without disturbing section 3."""
    path = tmp_path / "enc.mefd"
    x = np.random.default_rng(7).normal(0, 3000, 4000).astype(np.int32)
    w = mef3io.Writer(str(path), password1="lvl1", password2="lvl2")
    w.write_int32("ch1", x, 0.5, START, FS)
    w.close()

    expected = _read(path, password="lvl2")
    assert mef3io.Validator(str(path), password="lvl2").validate().ok

    w = mef3io.Writer(str(path), password1="lvl1", password2="lvl2")
    w.write_int32("ch1", x, 0.5, START + int(len(x) / FS * 1e6), FS)
    w.close()

    report = mef3io.Validator(str(path), password="lvl2").validate()
    assert report.ok, report.summary()
    # The allocation contract cannot be checked by reading section 2 directly
    # here — it is ciphertext. The validator decrypts it, so a clean report
    # from it IS the contract: `sizing.*` fire on exactly these fields.
    assert not [f for f in report.findings if f.check_id.startswith("sizing.")]
    assert _same(_read(path, password="lvl2")[: len(expected)], expected)

    # And the protection is still in place: no password, no section 2.
    blind = mef3io.Validator(str(path)).validate()
    assert blind.skipped, "an unencrypted read should not see section 2"
