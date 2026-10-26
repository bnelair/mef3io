"""P18 gate: crash recovery after an interrupted write.

An append flushes only the `.tmet` before its rename (2.0 removed the
`durability` option and the other barriers), so a power cut can leave a
segment's index and data disagreeing. That trade is only reasonable
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
    w = mef3io.Writer(str(path))
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
    # Recovery re-derives the declarations itself (2.0): nothing is left to repair.
    assert mef3io.Validator(str(path)).validate().repairable_check_ids == []

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

    assert mef3io.Validator(str(path)).validate().ok, "recovery alone must leave a valid session"
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


def test_the_durability_option_is_gone(tmp_path):
    """2.0 removed `durability=` (Writer, MefWriter): passing it is a TypeError,
    raised before the constructor can remove an `overwrite=True` session.

    Every append now takes one path: only the `.tmet` is flushed before its
    rename (pinned per file in core/tests/test_durability.cpp); the torn tails
    a power cut can leave are what the rest of this suite recovers.
    """
    import inspect

    from mef3io.compat import MefWriter

    assert "durability" not in inspect.signature(mef3io.Writer.__init__).parameters
    assert "durability" not in inspect.signature(MefWriter.__init__).parameters

    path = tmp_path / "s.mefd"
    w = mef3io.Writer(str(path))
    x = np.arange(2000, dtype=np.int32)
    w.write_int32("ch1", x, 0.5, START, FS)
    w.write_int32("ch1", x, 0.5, START + int(2000 / FS * 1e6), FS)   # append
    w.close()
    assert mef3io.Validator(str(path)).validate().ok
    before = sorted(p.name for p in Path(path).rglob("*") if p.is_file())

    for durability in ("fast", "full"):
        with pytest.raises(TypeError, match="durability"):
            mef3io.Writer(str(path), overwrite=True, durability=durability)
        with pytest.raises(TypeError, match="durability"):
            MefWriter(str(path), overwrite=True, durability=durability)
    assert sorted(p.name for p in Path(path).rglob("*") if p.is_file()) == before, (
        "the session was destroyed by a rejected argument"
    )


def test_an_index_that_fails_its_own_crc_is_rebuilt_from_the_blocks(tmp_path):
    """An index that does not pass its own CRC is not trusted (1.x refused it).

    No entry is kept on trust: past the prefix the stored CRC covers, an entry
    stays only if the RED block header at its offset (the placement
    authority, covered by the block CRC) agrees, and from the first that does
    not the index is rebuilt from the CRC-checked blocks in .tdat. A corrupted
    offset therefore cannot drop real blocks: here the very first entry is
    damaged, and every sample still reads back.
    """
    path, tidx, tdat = _session(tmp_path)
    with mef3io.Reader(str(path)) as r:
        expected = r.read("ch1")
    data_before = tdat.read_bytes()[UH:]
    raw = bytearray(tidx.read_bytes())
    struct.pack_into("<q", raw, UH + 0, 999_999_999)      # first entry file_offset
    tidx.write_bytes(bytes(raw))

    report = mef3io.recover_session(str(path), apply=True)
    assert not report.skipped, report.summary()
    assert report.segments and "CRC" in report.segments[0].action, report.summary()
    assert report.segments[0].blocks_recovered == _entries(tidx)
    assert tdat.read_bytes()[UH:] == data_before, "no sample byte may change"
    assert mef3io.Validator(str(path)).validate().ok
    with mef3io.Reader(str(path)) as r:
        np.testing.assert_array_equal(r.read("ch1"), expected)


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
    three never landed — what a power cut mid-append can leave (the .tmet's
    rename itself is not flushed, so the old one survives)."""
    path = tmp_path / "s.mefd"
    rng = np.random.default_rng(3)
    n = 2000
    with mef3io.Writer(str(path)) as w:
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


def test_at_offset_zero_recovery_rederives_the_metadata_through_repair(tmp_path):
    """At rto 0 the validator owns the times (its segment-bounds repair fixes
    the end). Since 2.0 recovery runs that repair itself on the segments it
    touched, so recovery ALONE gives the right end, and the .tmet says it was
    rewritten by a header repair (not by recovery's own end-time path)."""
    path, tmet, true_end = _crashed_append(tmp_path, 0)
    mef3io.recover_session(str(path), apply=True)
    assert mef3io.Validator(str(path)).validate().ok
    with mef3io.Reader(str(path)) as r:
        prov = r.segments("ch1")[-1]["provenance"]
    assert "header-repair" in str(prov), prov
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


@pytest.mark.parametrize("fs,seconds", [(499.7, 37.3), (256.0, 61.7), (511.3, 37.3),
                                        (1000 / 3, 61.7)])
def test_a_healthy_legacy_session_with_an_offset_needs_no_recovery(tmp_path, fs, seconds):
    """Negative control (review of #145): mef_tools truncates its end time
    (int64(start + n/fs*1e6)) and pymef rounds block times its own way, so the
    stored end can be a few us off the computed one. That is an END
    CONVENTION, not a stale end: nothing to do, nothing written. (On the first
    version of the fix, (256, 61.7) and (511.3, 37.3) were reported stale.)"""
    pytest.importorskip("pymef", reason="legacy oracle not installed")
    mef_tools = pytest.importorskip("mef_tools.io", reason="legacy oracle not installed")
    path = tmp_path / "legacy.mefd"
    x = np.random.default_rng(0).normal(0, 50, int(seconds * fs))
    w = mef_tools.MefWriter(str(path), overwrite=True)
    w.record_offset = 1_000_000_000
    w.write_data(x, "ch1", START, fs, precision=2)
    del w
    before = {p: p.read_bytes() for p in path.rglob("*") if p.is_file()}
    dry = mef3io.recover_session(str(path))
    assert dry.nothing_to_do, dry.summary()
    assert not dry.notes, dry.notes
    done = mef3io.recover_session(str(path), apply=True)
    assert not done.segments
    assert {p: p.read_bytes() for p in path.rglob("*") if p.is_file()} == before


def _uh_end(f):
    return struct.unpack_from("<q", Path(f).read_bytes(), 24)[0]


def _seg_files(tmet):
    d = tmet.parent
    return [d / (d.stem + ext) for ext in (".tmet", ".tidx", ".tdat")]


def test_an_end_time_fix_alone_keeps_the_data_crc(tmp_path):
    """Only the .tmet was lost: the .tdat body did not change, so its stored
    body CRC (valid, over tens of GB in real files) must stay."""
    rto = 1_000_000_000
    path, tmet, true_end = _crashed_append(tmp_path, rto, lose_index=False)
    tdat = _seg_files(tmet)[2]
    crc_before = tdat.read_bytes()[4:8]
    assert crc_before != b"\xff\xff\xff\xff"
    report = mef3io.recover_session(str(path), apply=True)
    assert report.segments and report.segments[0].blocks_recovered == 0
    assert tdat.read_bytes()[4:8] == crc_before
    assert _uh_end(tmet) == rto - true_end


def test_an_end_is_never_moved_earlier(tmp_path):
    """Index ahead of data at a non-zero offset: entries are dropped, but the
    end times are left where they were (a later end only widens the range a
    reader seeks; an earlier one hides samples)."""
    rto = 1_000_000_000
    path, tidx, tdat = _session(tmp_path)
    _set_rto(path, rto)
    tmet = tidx.with_suffix(".tmet")
    ends = [_uh_end(f) for f in _seg_files(tmet)]
    raw = tdat.read_bytes()
    tdat.write_bytes(raw[: len(raw) // 2])
    report = mef3io.recover_session(str(path), apply=True)
    assert report.segments[0].blocks_dropped > 0
    assert [_uh_end(f) for f in _seg_files(tmet)] == ends


def test_a_plain_stored_end_keeps_its_convention(tmp_path):
    """A .tidx whose end is stored plain (as older writers did) next to a
    negated .tmet: the plain one is left with a note, never rewritten into
    the other convention; the .tmet is still fixed."""
    rto = 1_000_000_000
    path, tmet, true_end = _crashed_append(tmp_path, rto)
    tidx = _seg_files(tmet)[1]
    raw = bytearray(tidx.read_bytes())
    stale_plain = START + rto + 1_000_000           # far before the data's end
    struct.pack_into("<q", raw, 24, stale_plain)
    struct.pack_into("<I", raw, 0, m.crc32(bytes(raw[4:UH])))
    tidx.write_bytes(bytes(raw))
    report = mef3io.recover_session(str(path), apply=True)
    assert any(".tidx end time is not stored in the offset-applied" in n for n in report.notes)
    assert _uh_end(tidx) == stale_plain
    assert _uh_end(tmet) == rto - true_end


def test_only_the_last_segment_metadata_is_read_on_a_healthy_session(tmp_path):
    """#7 of the review: recovery reads no .tmet of a segment it does not
    write, except the 1 KB header of each channel's last one. Pinned by
    making every earlier .tmet unreadable."""
    import os
    if hasattr(os, "geteuid") and os.geteuid() == 0:
        pytest.skip("root reads anything")
    path = tmp_path / "s.mefd"
    x = np.arange(2560, dtype=np.int32)
    with mef3io.Writer(str(path)) as w:
        for i in range(4):
            w.write_int32("ch1", x, 0.5, START + i * 20_000_000, FS, new_segment=True)
    tmets = sorted(path.glob("ch1.timd/*.segd/*.tmet"))
    try:
        for t in tmets[:-1]:
            t.chmod(0)
        report = mef3io.recover_session(str(path))
        assert report.nothing_to_do and not report.notes, (report.summary(), report.notes)
    finally:
        for t in tmets:
            t.chmod(0o644)


def test_flush_close_and_wait_for_disk(tmp_path):
    """When data is on disk (2.0): write() returns once the data is handed to
    the OS; flush()/close() wait for everything written; wait_for_disk=True
    makes each write wait for its own data. Which files are flushed when is
    pinned in core/tests/test_durability.cpp; here: the API and that every
    mode writes the same session."""
    import inspect

    from mef3io.compat import MefWriter

    assert inspect.signature(mef3io.Writer.__init__).parameters["wait_for_disk"].default is False
    assert inspect.signature(MefWriter.__init__).parameters["wait_for_disk"].default is False
    x = np.arange(4000, dtype=np.int32) - 2000
    got = {}
    for mode in ("default", "flush", "wait"):
        path = tmp_path / f"{mode}.mefd"
        w = mef3io.Writer(str(path), wait_for_disk=(mode == "wait"))
        w.write_int32("ch1", x, 0.5, START, FS)
        w.write_int32("ch1", x, 0.5, START + int(4000 / FS * 1e6), FS)
        if mode == "flush":
            w.flush()
            w.flush()                     # nothing new: returns at once
        w.close()
        w.close()                         # idempotent
        w.flush()                         # after close: a no-op
        assert mef3io.Validator(str(path)).validate().ok
        with mef3io.Reader(str(path)) as r:
            got[mode] = r.read("ch1")
    np.testing.assert_array_equal(got["default"], got["flush"])
    np.testing.assert_array_equal(got["default"], got["wait"])

    mw = MefWriter(str(tmp_path / "compat.mefd"), overwrite=True, wait_for_disk=True)
    mw.write_data(x.astype(float), "ch1", START, FS, precision=0)
    mw.flush()
    mw.close()
    assert mef3io.Validator(str(tmp_path / "compat.mefd")).validate().ok
