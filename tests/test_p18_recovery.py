"""P18 gate: crash recovery after an interrupted write.

`durability="fast"` trades the ordering barriers for speed, so a power cut can
leave a segment's index and data disagreeing. That trade is only reasonable
because the disagreement is detectable and repairable — this is the suite that
says so.

Two shapes, handled differently because one has lost data and the other has not:

  INDEX AHEAD OF DATA — entries reference bytes that never landed. Those samples
  do not exist; the entries go.

  DATA AHEAD OF INDEX — blocks reached .tdat but the index was not extended.
  Those samples DO exist and are recovered from the RED block headers.
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
UH = 1024
TSI = 56


def _session(tmp_path, chunks=6, n=2000):
    path = tmp_path / "s.mefd"
    rng = np.random.default_rng(3)
    w = mef3io.Writer(str(path), durability="fast")
    for i in range(chunks):
        w.write_int32("ch1", rng.integers(-9000, 9000, n, dtype=np.int32), 0.5,
                      START + int(i * n / FS * 1e6), FS)
    w.close()
    seg = next(Path(path).glob("*.timd/*.segd"))
    return path, next(seg.glob("*.tidx")), next(seg.glob("*.tdat"))


def _entries(tidx):
    return (tidx.stat().st_size - UH) // TSI


def _fix_index_header(tidx, n_entries):
    """Make the .tidx header self-consistent after truncating entries."""
    raw = bytearray(tidx.read_bytes())
    struct.pack_into("<q", raw, 32, n_entries)
    struct.pack_into("<I", raw, 4, m.crc32(bytes(raw[UH:])))
    struct.pack_into("<I", raw, 0, m.crc32(bytes(raw[4:UH])))
    tidx.write_bytes(bytes(raw))


def test_data_ahead_of_index_is_recovered_not_discarded(tmp_path):
    """Blocks that reached .tdat but were never indexed hold real samples."""
    path, tidx, tdat = _session(tmp_path)
    full = _entries(tidx)
    with mef3io.Reader(str(path)) as r:
        expected = r.read("ch1")

    # Simulate: the .tdat write landed, the .tidx extension did not.
    keep = full - 3
    raw = tidx.read_bytes()[: UH + keep * TSI]
    tidx.write_bytes(raw)
    _fix_index_header(tidx, keep)
    # The reader grids by time, so the array stays the same length — the lost
    # blocks appear as NaN. Count real samples, not array length.
    live = lambda a: int(np.sum(np.isfinite(a)))
    with mef3io.Reader(str(path)) as r:
        assert live(r.read("ch1")) < live(expected), "the fixture must really lose samples"

    dry = mef3io.recover_session(str(path))
    assert not dry.applied
    assert dry.segments and dry.segments[0].blocks_recovered == 3, dry.summary()
    assert _entries(tidx) == keep, "a dry run must not write"

    done = mef3io.recover_session(str(path), apply=True)
    assert done.applied
    assert _entries(tidx) == full
    mef3io.repair_session(str(path), mef3io.Validator(str(path)).validate().repairable_check_ids)

    with mef3io.Reader(str(path)) as r:
        got = r.read("ch1")
    assert live(got) == live(expected), "recovery did not restore every sample"
    np.testing.assert_array_equal(
        np.nan_to_num(got, nan=-1.0), np.nan_to_num(expected, nan=-1.0)
    )
    assert mef3io.Validator(str(path)).validate().ok


def test_index_ahead_of_data_drops_the_unbacked_entries(tmp_path):
    """Entries referencing bytes that never landed describe samples that do not
    exist. They have to go, and the rest of the segment must survive."""
    path, tidx, tdat = _session(tmp_path)
    full = _entries(tidx)

    # Simulate: the .tidx was extended, the .tdat write did not land.
    raw = tdat.read_bytes()
    tdat.write_bytes(raw[: len(raw) // 2])

    report = mef3io.recover_session(str(path), apply=True)
    assert report.segments, report.summary()
    seg = report.segments[0]
    assert seg.blocks_dropped > 0
    assert _entries(tidx) < full

    mef3io.repair_session(str(path), mef3io.Validator(str(path)).validate().repairable_check_ids)
    assert mef3io.Validator(str(path)).validate().ok, "recovery must leave a valid session"
    with mef3io.Reader(str(path)) as r:
        assert len(r.read("ch1")) > 0, "the surviving blocks must still read"


def test_a_healthy_session_is_left_completely_alone(tmp_path):
    """Recovery must be a no-op on anything it does not need to touch."""
    path, tidx, tdat = _session(tmp_path)
    before = {f: f.read_bytes() for f in Path(path).rglob("*") if f.is_file()}

    report = mef3io.recover_session(str(path), apply=True)
    assert report.nothing_to_do, report.summary()
    assert report.segments_examined > 0
    for f, data in before.items():
        assert f.read_bytes() == data, f"{f.name} was modified"


def test_recovery_backs_up_what_it_changes_but_not_the_data_file(tmp_path):
    """A .tdat can be tens of gigabytes. Copying it to undo a header patch
    would make the tool unusable on the files it exists for."""
    path, tidx, tdat = _session(tmp_path)
    raw = tdat.read_bytes()
    tdat.write_bytes(raw[: len(raw) // 2])
    tdat_size = tdat.stat().st_size

    mef3io.recover_session(str(path), apply=True, backup=True)
    backup = Path(str(path) + ".recover-backup")
    assert backup.is_dir(), "a backup should have been taken"
    saved = list(backup.rglob("*"))
    names = {f.name for f in saved if f.is_file()}
    assert any(n.endswith(".tidx") for n in names), names
    assert not any(n.endswith(".tdat") for n in names), (
        f"the whole data file was copied: {names}"
    )
    total = sum(f.stat().st_size for f in saved if f.is_file())
    assert total < tdat_size, (
        f"backup is {total} bytes against a {tdat_size}-byte .tdat — it is copying the data"
    )


def test_recovery_refuses_a_tar_archive(tmp_path):
    path, _, _ = _session(tmp_path)
    archive = mef3io.archive_session(str(path))
    with pytest.raises(RuntimeError, match="tar"):
        mef3io.recover_session(archive, apply=True)


def test_fast_is_the_default_and_full_is_opt_in(tmp_path):
    """The default changed to "fast" deliberately (2026-09-21).

    These files are built by appending for days to months, so the append is the
    hot path and the barriers cost ~2.5x on it. Pinned because flipping it back
    silently would be a performance regression nobody would attribute, and
    flipping it the other way silently would weaken a documented guarantee.
    """
    import inspect

    assert inspect.signature(mef3io.Writer.__init__).parameters["durability"].default == "fast"
    from mef3io.compat import MefWriter

    assert inspect.signature(MefWriter.__init__).parameters["durability"].default == "fast"

    # Both settings must produce a session that reads back identically — the
    # knob buys crash behaviour, not different output.
    rng = np.random.default_rng(17)
    x = rng.integers(-9000, 9000, 3000, dtype=np.int32)
    got = {}
    for durability in ("fast", "full"):
        path = tmp_path / f"{durability}.mefd"
        w = mef3io.Writer(str(path), durability=durability)
        w.write_int32("ch1", x, 0.5, START, FS)
        w.write_int32("ch1", x, 0.5, START + int(3000 / FS * 1e6), FS)   # append
        w.close()
        assert mef3io.Validator(str(path)).validate().ok
        with mef3io.Reader(str(path)) as r:
            got[durability] = r.read("ch1")
    np.testing.assert_array_equal(got["fast"], got["full"])


def test_an_unknown_durability_does_not_destroy_the_session_first(tmp_path):
    """`overwrite=True` removes the existing session in the constructor.

    Validating the option afterwards meant a typo could delete a good session
    and only then raise, which is the worst possible order for a mistake that
    is purely a typo.
    """
    path = tmp_path / "s.mefd"
    w = mef3io.Writer(str(path))
    w.write_int32("ch1", np.arange(2000, dtype=np.int32), 0.5, START, FS)
    w.close()
    before = sorted(p.name for p in Path(path).rglob("*") if p.is_file())
    assert before

    with pytest.raises(ValueError, match="durability"):
        mef3io.Writer(str(path), overwrite=True, durability="sometimes")
    assert sorted(p.name for p in Path(path).rglob("*") if p.is_file()) == before, (
        "the session was destroyed before the option was validated"
    )

    from mef3io.compat import MefWriter

    with pytest.raises(ValueError, match="durability"):
        MefWriter(str(path), overwrite=True, durability="sometimes")
    assert sorted(p.name for p in Path(path).rglob("*") if p.is_file()) == before


def test_recovery_refuses_an_index_that_fails_its_own_crc(tmp_path):
    """Recovery DECIDES what to keep from the index, then truncates.

    Declaration repair never touches the index, so it can afford to work on one
    it has not verified. Recovery cannot: a corrupted offset produces the same
    shape as an interrupted append, and acting on it would drop real blocks or
    truncate real samples.
    """
    path, tidx, tdat = _session(tmp_path)
    raw = bytearray(tidx.read_bytes())
    # Corrupt an entry and leave the body CRC stale — the index no longer
    # describes itself.
    struct.pack_into("<q", raw, UH + 0, 999_999_999)      # first entry file_offset
    tidx.write_bytes(bytes(raw))
    before = {f: f.read_bytes() for f in Path(path).rglob("*") if f.is_file()}

    report = mef3io.recover_session(str(path), apply=True)
    assert report.skipped, "a segment whose index fails its CRC must be reported"
    assert any("CRC" in s for s in report.skipped), report.summary()
    assert not report.segments, "nothing should have been recovered from it"
    for f, data in before.items():
        assert f.read_bytes() == data, f"{f.name} was modified despite the bad CRC"


def test_recovery_reports_a_stale_universal_header_count(tmp_path):
    """A crash between publishing the .tidx header and patching the .tdat one
    leaves every block aligned but the declarations stale.

    Looking only at block alignment reported "nothing to do" and the CLI then
    skipped the repair — while meflib clamps the block count down to that
    header, so the segment reads SHORT.
    """
    path, tidx, tdat = _session(tmp_path)
    raw = bytearray(tdat.read_bytes()[:UH])
    real = struct.unpack_from("<q", raw, 32)[0]
    struct.pack_into("<q", raw, 32, real - 2)            # stale .tdat entry count
    struct.pack_into("<I", raw, 0, m.crc32(bytes(raw[4:UH])))
    with open(tdat, "r+b") as fh:
        fh.write(bytes(raw))

    dry = mef3io.recover_session(str(path))
    assert dry.segments, "a stale header count must be surfaced, not passed over"
    assert "stale" in dry.segments[0].action, dry.summary()

    mef3io.recover_session(str(path), apply=True)
    ids = mef3io.Validator(str(path)).validate().repairable_check_ids
    if ids:
        mef3io.repair_session(str(path), ids)
    assert mef3io.Validator(str(path)).validate().ok
    assert struct.unpack_from("<q", tdat.read_bytes(), 32)[0] == real


# --- #83: the end time at a non-zero recording-time offset -------------------
# At rto != 0 every validator time check stands down (writers disagree on the
# sign convention there), so repair_session never fixed the end time recovery
# left behind, and info() kept reporting the end from BEFORE the recovery.

S3_RTO = 1024 + 1536 + 10752          # section 3, field 0 (si8)
TMET_BYTES = 16384


def _set_rto(path, rto):
    """Give every .tmet this offset. The writer stored every time as -t, so
    the session simply moves by rto — a consistent rto session."""
    for tmet in Path(path).glob("*.timd/*.segd/*.tmet"):
        raw = bytearray(tmet.read_bytes())
        struct.pack_into("<q", raw, S3_RTO, rto)
        struct.pack_into("<I", raw, 4, m.crc32(bytes(raw[UH:TMET_BYTES])))
        struct.pack_into("<I", raw, 0, m.crc32(bytes(raw[4:UH])))
        tmet.write_bytes(bytes(raw))


def _crashed_append(tmp_path, rto, lose_index=True):
    """Six appends; the .tmet (and, with lose_index, the index) of the last
    three never landed — what `durability="fast"` can leave after a crash."""
    path = tmp_path / "s.mefd"
    rng = np.random.default_rng(3)
    n = 2000
    with mef3io.Writer(str(path), durability="fast") as w:
        for i in range(6):
            w.write_int32("ch1", rng.integers(-9000, 9000, n, dtype=np.int32), 0.5,
                          START + int(i * n / FS * 1e6), FS)
            if i == 2:
                tmet = next(path.glob("*.timd/*.segd/*.tmet"))
                old_tmet = tmet.read_bytes()
                tidx = next(path.glob("*.timd/*.segd/*.tidx"))
                old_entries = _entries(tidx)
    tmet.write_bytes(old_tmet)
    if lose_index:
        tidx.write_bytes(tidx.read_bytes()[: UH + old_entries * TSI])
        _fix_index_header(tidx, old_entries)
    if rto:
        _set_rto(path, rto)
    true_end = START + rto + int(6 * n / FS * 1e6)
    return path, tmet, true_end


def _recover_and_repair(path):
    report = mef3io.recover_session(str(path), apply=True)
    ids = mef3io.Validator(str(path)).validate().repairable_check_ids
    if ids:
        mef3io.repair_session(str(path), ids)
    return report


@pytest.mark.parametrize("lose_index", [True, False], ids=["index-lost", "only-tmet-lost"])
def test_recovery_sets_the_end_time_at_a_nonzero_offset(tmp_path, lose_index):
    rto = 1_000_000_000
    path, tmet, true_end = _crashed_append(tmp_path, rto, lose_index)
    with mef3io.Reader(str(path)) as r:
        assert r.info("ch1")["end_time"] < true_end - 10_000_000, "the fixture must be stale"
    before = tmet.read_bytes()
    dry = mef3io.recover_session(str(path))
    assert dry.segments and "end time" in dry.segments[0].action, dry.summary()
    assert tmet.read_bytes() == before and not dry.applied, "a dry run must not write"

    _recover_and_repair(path)
    with mef3io.Reader(str(path)) as r:
        info = r.info("ch1")
        assert info["end_time"] == true_end
        assert np.isfinite(r.read("ch1")).sum() == 6 * 2000
        # The three universal headers agree, and the value is stored in the
        # offset-applied form: rto - t, never the real date in plain.
        seg = r.segments("ch1")[0]
        assert "recovery" in seg["provenance"]["operations"]
    seg_dir = tmet.parent
    for f in (seg_dir / (seg_dir.stem + ext) for ext in (".tmet", ".tidx", ".tdat")):
        stored = struct.unpack_from("<q", f.read_bytes(), 24)[0]
        assert stored == rto - true_end, f.name
    assert mef3io.Validator(str(path)).validate().ok
    # Idempotent: a second pass finds nothing to do.
    assert mef3io.recover_session(str(path)).nothing_to_do


def test_at_offset_zero_recovery_leaves_the_metadata_to_repair(tmp_path):
    """Negative control: at rto 0 the validator owns the times (its
    segment-bounds repair fixes the end), so recovery does not touch .tmet,
    and the end comes out the same either way."""
    path, tmet, true_end = _crashed_append(tmp_path, 0)
    before = tmet.read_bytes()
    mef3io.recover_session(str(path), apply=True)
    assert tmet.read_bytes() == before
    ids = mef3io.Validator(str(path)).validate().repairable_check_ids
    mef3io.repair_session(str(path), ids)
    with mef3io.Reader(str(path)) as r:
        assert r.info("ch1")["end_time"] == true_end


def test_an_ambiguous_stored_convention_is_left_alone_with_a_note(tmp_path):
    """A .tmet end time stored as a plain (positive) value at rto != 0 is
    ambiguous (absolute, or a legacy positive delta?): never guessed."""
    rto = 1_000_000_000
    path, tmet, _ = _crashed_append(tmp_path, rto)
    raw = bytearray(tmet.read_bytes())
    stored_end = struct.unpack_from("<q", raw, 24)[0]
    struct.pack_into("<q", raw, 24, -stored_end + rto)   # same instant, absolute form
    struct.pack_into("<I", raw, 0, m.crc32(bytes(raw[4:UH])))
    tmet.write_bytes(bytes(raw))
    report = mef3io.recover_session(str(path), apply=True)
    assert any("end time left as is" in n for n in report.notes), report.notes
    assert struct.unpack_from("<q", tmet.read_bytes(), 24)[0] == -stored_end + rto
    assert report.segments[0].blocks_recovered == 3, "the index is still recovered"
