"""P23: repairing or recovering a NON-last segment stamps the channel's LAST one.

Provenance describes what happened to each file, so a repair of segment 1 is
recorded in segment 1's own region (`header-repair` / `recovery`). Nothing about
it was visible from the channel's last segment — the one file a reader that
looks at one segment per channel (the warm-start snapshot check, for one) sees.
So repair_session and recover_session also stamp the last segment's `.tmet`
with the dedicated code `segment-repaired` (9): "a segment of this channel was
repaired or recovered". Header-only, backed up, and listed in the report. It
is skipped with a note — never an error, never a refusal — when the last
segment's region belongs to another application, its CRC fails, or a writer's
temp file sits in it; the note says cached opens will not see the change.
Nothing is refused on "looks active" heuristics (fresh mtimes after `cp -r` or
extract_session are the normal legacy-repair workflow).
"""
from __future__ import annotations

import struct
import sys
from pathlib import Path

import numpy as np
import pytest

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO))

import mef3io  # noqa: E402
from mef3io import _mef3io as m  # noqa: E402
from mef3io import cache as C  # noqa: E402

START = 1577836800000000
FS = 256.0
SEG_SAMPLES = 512
SEG_US = int(SEG_SAMPLES / FS * 1e6)
N_SEG = 3

UH = 1024
TMET = 16384
S2 = 1024 + 1536
MAX_DIFF_BYTES = 6388
REGION = 960
SEGMENT_REPAIRED = 9
HEADER_REPAIR = 3



def _write(path: Path):
    rng = np.random.default_rng(0)
    with mef3io.Writer(str(path), overwrite=True) as w:
        for s in range(N_SEG):
            x = rng.normal(0, 300, SEG_SAMPLES).astype(np.int32)
            w.write_int32("ch1", x, 0.5, START + s * SEG_US, FS, new_segment=True)


def _tmets(path: Path):
    return sorted((path / "ch1.timd").glob("*.segd/*.tmet"))


def _rewrite_tmet(tmet: Path, edit):
    raw = bytearray(tmet.read_bytes())
    edit(raw)
    struct.pack_into("<I", raw, 4, m.crc32(bytes(raw[UH:TMET])))
    struct.pack_into("<I", raw, 0, m.crc32(bytes(raw[4:UH])))
    tmet.write_bytes(bytes(raw))


def _break_difference_bytes(tmet: Path):
    _rewrite_tmet(tmet, lambda raw: struct.pack_into("<I", raw, S2 + MAX_DIFF_BYTES, 0))


def _region(tmet: Path):
    r = tmet.read_bytes()[REGION:REGION + 64]
    return {
        "magic": r[:4],
        "last_op": r[5],
        "mask": struct.unpack_from("<I", r, 48)[0],
        "count": struct.unpack_from("<I", r, 52)[0],
    }


@pytest.fixture
def session(tmp_path):
    path = tmp_path / "s.mefd"
    _write(path)
    return path


def test_middle_repair_stamps_the_last_segment(session):
    middle, last = _tmets(session)[1], _tmets(session)[-1]
    _break_difference_bytes(middle)
    before = _region(last)
    before_bytes = after_bytes(last)
    body = last.read_bytes()[UH:]

    report = mef3io.repair_session(str(session), ["sizing.difference-bytes"], backup=True)
    assert report.segments_repaired == 1

    after = _region(last)
    assert after["magic"] == b"M3IO"
    assert after["last_op"] == SEGMENT_REPAIRED
    assert after["count"] == before["count"] + 1
    assert after["mask"] & (1 << SEGMENT_REPAIRED)
    assert last.read_bytes()[UH:] == body, "the stamp must be header-only"
    # The middle segment records its own repair, as before.
    assert _region(middle)["last_op"] == HEADER_REPAIR
    # Readers name the code.
    prov = mef3io.Reader(str(session)).segments("ch1")[-1]["provenance"]
    assert prov["last_operation"] == "segment-repaired"
    # Backed up like the repair's own writes: the pre-stamp original.
    backup = Path(str(session) + ".repair-backup") / last.relative_to(session)
    assert backup.read_bytes()[REGION:REGION + 64] == before_bytes


def after_bytes(tmet):
    return tmet.read_bytes()[REGION:REGION + 64]


def test_last_segment_repair_adds_no_second_stamp(session):
    last = _tmets(session)[-1]
    _break_difference_bytes(last)
    before = _region(last)
    mef3io.repair_session(str(session), ["sizing.difference-bytes"], backup=False)
    after = _region(last)
    assert after["last_op"] == HEADER_REPAIR
    assert after["count"] == before["count"] + 1
    assert not after["mask"] & (1 << SEGMENT_REPAIRED)


def test_cached_open_rebuilds_after_a_middle_repair(session, tmp_path):
    middle = _tmets(session)[1]
    _break_difference_bytes(middle)
    cache = tmp_path / "snap.mef3cache"
    mef3io.Reader(str(session), cache=str(cache), warn_declarations=False)
    assert C.load_valid(cache, str(session)) is not None
    mef3io.repair_session(str(session), ["sizing.difference-bytes"], backup=False)
    assert C.load_valid(cache, str(session)) is None
    r = mef3io.Reader(str(session), cache=str(cache))
    assert r._impl is not None, "the stale snapshot was served"


def test_session_stays_valid_and_readable(session):
    middle = _tmets(session)[1]
    _break_difference_bytes(middle)
    expected = mef3io.Reader(str(session)).read("ch1")
    mef3io.repair_session(str(session), ["sizing.difference-bytes"], backup=False)

    assert mef3io.validate_session(str(session)).ok
    np.testing.assert_array_equal(mef3io.Reader(str(session)).read("ch1"), expected)

    pytest.importorskip("pymef")
    from pymef.mef_session import MefSession

    n = N_SEG * SEG_SAMPLES
    got = MefSession(str(session), "").read_ts_channels_sample(["ch1"], [0, n])[0]
    assert len(got) == n


def test_repair_right_after_extract_session_succeeds_and_stamps(session, tmp_path):
    """The legacy-repair workflow: unpack (or `cp -r`) a session, repair it at
    once. Every mtime is fresh, and nothing may be refused for that."""
    _break_difference_bytes(_tmets(session)[1])
    tar = mef3io.archive_session(str(session))
    out = Path(mef3io.extract_session(tar, str(tmp_path / "x.mefd")))
    report = mef3io.repair_session(str(out), ["sizing.difference-bytes"], backup=False)
    assert report.segments_repaired == 1
    last = _tmets(out)[-1]
    assert _region(last)["last_op"] == SEGMENT_REPAIRED
    stamped = [f for f in report.findings if f.check_id == "provenance.last-segment"]
    assert len(stamped) == 1 and "stamped" in stamped[0].message
    assert Path(stamped[0].path).name == last.name


def test_stale_writer_temp_file_skips_only_the_stamp(session, tmp_path):
    """A crash leaves `<file>.mef3io-tmp` behind for good. The repair must
    still run; only the stamp is skipped, and the report says why, that the
    file can be removed when no writer is running, and what cached opens will
    miss."""
    middle, last = _tmets(session)[1], _tmets(session)[-1]
    _break_difference_bytes(middle)
    tmp = last.with_name(last.name + ".mef3io-tmp")
    tmp.write_bytes(b"")
    before = after_bytes(last)
    cache = tmp_path / "snap.mef3cache"
    mef3io.Reader(str(session), cache=str(cache), warn_declarations=False)

    report = mef3io.repair_session(str(session), ["sizing.difference-bytes"], backup=False)
    assert report.segments_repaired == 1
    assert _region(middle)["last_op"] == HEADER_REPAIR
    assert after_bytes(last) == before, "stamped despite a writer's temp file"
    notes = [f for f in report.findings if f.check_id == "provenance.last-segment"]
    assert len(notes) == 1 and notes[0].severity == "info"
    assert tmp.name in notes[0].message and "can be removed" in notes[0].message
    assert "Cached opens" in notes[0].message
    assert not report.skipped
    # Exactly the documented consequence: the cached open does not see it.
    assert C.load_valid(cache, str(session)) is not None


def test_foreign_region_is_skipped_with_a_note(session):
    middle, last = _tmets(session)[1], _tmets(session)[-1]
    _break_difference_bytes(middle)

    def foreign(raw):
        raw[REGION:REGION + 64] = b"XYZW" + bytes(range(60))

    _rewrite_tmet(last, foreign)
    report = mef3io.repair_session(str(session), ["sizing.difference-bytes"], backup=False)
    assert report.segments_repaired == 1
    assert after_bytes(last) == b"XYZW" + bytes(range(60)), "another application's region"
    notes = [f for f in report.findings if f.check_id == "provenance.last-segment"]
    assert notes and notes[0].severity == "info"
    assert not report.skipped


def test_recovery_of_a_middle_segment_stamps_the_last(session):
    middle_dir = _tmets(session)[1].parent
    tdat = next(middle_dir.glob("*.tdat"))
    with open(tdat, "r+b") as f:  # an interrupted write: the last block is torn
        f.truncate(tdat.stat().st_size - 10)
    last = _tmets(session)[-1]
    before = _region(last)
    report = mef3io.recover_session(str(session), apply=True, backup=True)
    assert report.segments, report.summary()
    after = _region(last)
    assert after["last_op"] == SEGMENT_REPAIRED
    assert after["count"] == before["count"] + 1
    assert after["mask"] & (1 << SEGMENT_REPAIRED)
    assert any("stamped segment-repaired" in n and last.name in n for n in report.notes)


def test_recovery_with_a_stale_temp_file_skips_only_the_stamp(session):
    middle_dir = _tmets(session)[1].parent
    tdat = next(middle_dir.glob("*.tdat"))
    with open(tdat, "r+b") as f:
        f.truncate(tdat.stat().st_size - 10)
    last = _tmets(session)[-1]
    (last.parent / (last.name + ".mef3io-tmp")).write_bytes(b"")
    before = after_bytes(last)
    report = mef3io.recover_session(str(session), apply=True, backup=False)
    assert report.segments and report.applied
    assert after_bytes(last) == before
    assert any("can be removed" in n and "Cached opens" in n for n in report.notes)


def test_cache_misses_a_middle_repair_when_the_stamp_is_skipped(session, tmp_path):
    """Documented, not hidden: a foreign provenance region in the last segment
    means the stamp is skipped, so a cached open still serves the pre-repair
    snapshot. A plain open always sees the repair."""
    middle, last = _tmets(session)[1], _tmets(session)[-1]
    _break_difference_bytes(middle)

    def foreign(raw):
        raw[REGION:REGION + 64] = b"XYZW" + bytes(range(60))

    _rewrite_tmet(last, foreign)
    cache = tmp_path / "snap.mef3cache"
    mef3io.Reader(str(session), cache=str(cache), warn_declarations=False)
    report = mef3io.repair_session(str(session), ["sizing.difference-bytes"], backup=False)
    note = [f for f in report.findings if f.check_id == "provenance.last-segment"][0]
    assert "Cached opens" in note.message
    assert C.load_valid(cache, str(session)) is not None
