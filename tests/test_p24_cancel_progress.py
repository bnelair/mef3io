"""P24: cancellable reads and long operations, with progress (#69).

What is pinned here:

* a cancel from ANOTHER thread stops a read, archive, extract, validate,
  repair and recover part-way, raising mef3io.Cancelled;
* a cancelled call leaves NOTHING behind — no partial archive, no staging
  directory, no modified session file, no backup directory — and an existing
  target survives an overwrite that was cancelled;
* progress(done, total) starts at (0, total), never decreases, never runs past
  total, and ends with (total, total) exactly once — on success only;
* a progress callback that raises stops the operation like a cancel and its
  own exception reaches the caller;
* Cancelled is not a MefError: code that catches MefError to report damage, and
  a lenient Reader that contains per-segment failures, must not swallow it.

"Mid-operation" is made deterministic, not timed: the progress callback blocks
on its first strictly-intermediate report until the main thread has cancelled.
"""
import hashlib
import shutil
import struct
import sys
import threading
import warnings
from pathlib import Path

import numpy as np
import pytest

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO))
warnings.filterwarnings("ignore")

import mef3io  # noqa: E402
from mef3io import _mef3io as m  # noqa: E402

START = 1_600_000_000_000_000
FS = 1000.0
N = 150_000          # samples per segment
CHANNELS = ("a", "b", "c")
SEGMENTS = 3
UH = 1024
TSI = 56


# --- fixtures ------------------------------------------------------------------


@pytest.fixture(scope="module")
def pristine(tmp_path_factory):
    path = tmp_path_factory.mktemp("p24") / "s.mefd"
    rng = np.random.default_rng(24)
    with mef3io.Writer(str(path)) as w:
        for ch in CHANNELS:
            for s in range(SEGMENTS):
                x = rng.integers(-20000, 20000, N, dtype=np.int32)
                w.write_int32(ch, x, 0.5, START + int(s * (N / FS + 10) * 1e6), FS,
                              new_segment=s > 0)
    return path


@pytest.fixture
def session(pristine, tmp_path):
    dst = tmp_path / "s.mefd"
    shutil.copytree(pristine, dst)
    return dst


def _digest(root: Path) -> dict:
    """Every file under `root` -> sha256, so 'nothing changed' is exact."""
    return {str(f.relative_to(root)): hashlib.sha256(f.read_bytes()).hexdigest()
            for f in sorted(root.rglob("*")) if f.is_file()}


def _listing(directory: Path) -> set:
    return {p.name for p in directory.iterdir()}


class Recorder:
    """A progress callback that records every call."""

    def __init__(self):
        self.calls = []
        self.threads = set()

    def __call__(self, done, total):
        self.calls.append((done, total))
        self.threads.add(threading.get_ident())

    def check(self):
        calls = self.calls
        assert calls, "progress was never called"
        total = calls[0][1]
        assert calls[0] == (0, total), calls[:3]
        assert all(t == total for _, t in calls), "total changed mid-operation"
        dones = [d for d, _ in calls]
        assert dones == sorted(dones), "progress went backwards"
        assert calls[-1] == (total, total), calls[-3:]
        assert sum(1 for d in dones if d == total) == 1, \
            "(total, total) must be reported exactly once"
        assert all(0 <= d <= total for d in dones)
        return total


def cancel_mid_flight(fn):
    """Run fn(cancel=, progress=) in a worker thread; cancel it from THIS thread
    once it is demonstrably part-way (its callback reported 0 < done < total).
    Returns (exception raised in the worker, progress calls seen)."""
    token = mef3io.CancelToken()
    midway = threading.Event()
    resume = threading.Event()
    seen = []
    outcome = {}

    def progress(done, total):
        seen.append((done, total))
        if 0 < done < total and not midway.is_set():
            midway.set()
            resume.wait(30)  # releases the GIL; the work is paused right here

    def worker():
        try:
            outcome["value"] = fn(cancel=token, progress=progress)
        except BaseException as e:  # noqa: BLE001 - the test inspects it
            outcome["error"] = e

    t = threading.Thread(target=worker)
    t.start()
    try:
        assert midway.wait(60), f"never reached the middle of the operation: {seen}"
        token.cancel()
    finally:
        resume.set()
        t.join(60)
    assert not t.is_alive()
    assert "value" not in outcome, "the operation finished despite the cancel"
    return outcome.get("error"), seen


# --- the token and the exception -------------------------------------------------


def test_token_is_one_shot_and_thread_safe():
    t = mef3io.CancelToken()
    assert not t.cancelled
    t.raise_if_cancelled()
    threading.Thread(target=t.cancel).start()
    for _ in range(1000):
        if t.cancelled:
            break
        threading.Event().wait(0.001)
    assert t.cancelled
    t.cancel()  # idempotent
    with pytest.raises(mef3io.Cancelled):
        t.raise_if_cancelled()


def test_cancelled_is_not_a_failure_type():
    assert issubclass(mef3io.Cancelled, Exception)
    assert not issubclass(mef3io.Cancelled, mef3io.MefError)
    assert not issubclass(mef3io.Cancelled, RuntimeError)


def test_bad_hook_arguments_are_type_errors(session):
    with mef3io.Reader(str(session)) as r:
        with pytest.raises(TypeError):
            r.read("a", cancel=object())
        with pytest.raises(TypeError):
            r.read("a", progress=42)
    with pytest.raises(TypeError):
        mef3io.validate_session(str(session), cancel=True)


# --- reads -------------------------------------------------------------------------


def test_read_progress_and_result_unchanged(session):
    with mef3io.Reader(str(session)) as r:
        plain = r.read("a")
        rec = Recorder()
        hooked = r.read("a", cancel=mef3io.CancelToken(), progress=rec)
        np.testing.assert_array_equal(plain, hooked)
        assert rec.check() > 0
        raw_rec = Recorder()
        raw = r.read_raw("b", progress=raw_rec)
        raw_rec.check()
        assert raw["samples"].size > 0


def test_read_cancelled_from_another_thread(session):
    with mef3io.Reader(str(session), n_threads=1) as r:
        err, seen = cancel_mid_flight(lambda **kw: r.read("a", **kw))
        assert isinstance(err, mef3io.Cancelled), err
        assert seen[-1][0] < seen[-1][1]
        # The reader is still usable and complete afterwards.
        assert np.isfinite(r.read("a")).sum() == SEGMENTS * N


def test_parallel_read_stops_when_cancelled(session):
    token = mef3io.CancelToken()

    def progress(done, total):
        if 0 < done < total:  # the first intermediate report: most blocks remain
            token.cancel()

    with mef3io.Reader(str(session), n_threads=4) as r:
        with pytest.raises(mef3io.Cancelled):
            r.read("a", cancel=token, progress=progress)
        with pytest.raises(mef3io.Cancelled):
            r.read_raw("a", cancel=token)  # already cancelled: stops at once


def test_lenient_reader_does_not_report_a_cancel_as_damage(session):
    token = mef3io.CancelToken()
    token.cancel()
    with mef3io.Reader(str(session), strict=False) as r:
        with pytest.raises(mef3io.Cancelled):
            r.read("a", cancel=token)
        assert r.problems == []
        assert np.isfinite(r.read("a")).sum() == SEGMENTS * N


def test_a_raising_callback_aborts_the_read_with_its_own_exception(session):
    class Boom(Exception):
        pass

    def progress(done, total):
        if done:
            raise Boom("stop here")

    with mef3io.Reader(str(session)) as r:
        with pytest.raises(Boom, match="stop here"):
            r.read("a", progress=progress)
        assert np.isfinite(r.read("a")).sum() == SEGMENTS * N


# --- archive / extract ---------------------------------------------------------------


def test_archive_and_extract_progress(session, tmp_path):
    rec = Recorder()
    tar = mef3io.archive_session(str(session), progress=rec)
    total = rec.check()
    assert total == sum(f.stat().st_size for f in session.rglob("*") if f.is_file())

    rec2 = Recorder()
    out = tmp_path / "back.mefd"
    mef3io.extract_session(tar, str(out), progress=rec2)
    assert rec2.check() == total
    assert _digest(out) == _digest(session)


def test_archive_cancelled_from_another_thread_leaves_nothing(session):
    parent = session.parent
    before = _listing(parent)
    tar = parent / "s.mefd.tar"
    err, _ = cancel_mid_flight(
        lambda **kw: mef3io.archive_session(str(session), str(tar), **kw))
    assert isinstance(err, mef3io.Cancelled), err
    assert _listing(parent) == before, "a partial archive or temp file was left behind"


def test_cancelled_archive_overwrite_keeps_the_old_archive(session):
    tar = session.parent / "s.mefd.tar"
    tar.write_bytes(b"previous archive")
    err, _ = cancel_mid_flight(
        lambda **kw: mef3io.archive_session(str(session), str(tar), overwrite=True, **kw))
    assert isinstance(err, mef3io.Cancelled), err
    assert tar.read_bytes() == b"previous archive"
    assert not Path(str(tar) + ".tmp").exists()


def test_extract_cancelled_from_another_thread_leaves_nothing(session, tmp_path):
    tar = mef3io.archive_session(str(session), str(tmp_path / "t.mefd.tar"))
    out_parent = tmp_path / "out"
    out_parent.mkdir()
    err, _ = cancel_mid_flight(lambda **kw: mef3io.extract_session(
        tar, str(out_parent / "x.mefd"), **kw))
    assert isinstance(err, mef3io.Cancelled), err
    assert list(out_parent.iterdir()) == [], "a partial or staging directory was left behind"


def test_cancelled_extract_overwrite_keeps_the_existing_session(session, tmp_path):
    tar = mef3io.archive_session(str(session), str(tmp_path / "t.mefd.tar"))
    existing = tmp_path / "keep.mefd"
    shutil.copytree(session, existing)
    (existing / "marker.txt").write_text("the user's own file")
    before = _digest(existing)
    err, _ = cancel_mid_flight(lambda **kw: mef3io.extract_session(
        tar, str(existing), overwrite=True, **kw))
    assert isinstance(err, mef3io.Cancelled), err
    assert _digest(existing) == before, "the existing session was damaged by a cancelled overwrite"
    assert sorted(p.name for p in tmp_path.iterdir()) == ["keep.mefd", "s.mefd", "t.mefd.tar"]


def test_completed_extract_overwrite_replaces_the_target(session, tmp_path):
    tar = mef3io.archive_session(str(session), str(tmp_path / "t.mefd.tar"))
    existing = tmp_path / "keep.mefd"
    existing.mkdir()
    (existing / "stale.txt").write_text("old")
    mef3io.extract_session(tar, str(existing), overwrite=True)
    assert _digest(existing) == _digest(session)
    assert sorted(p.name for p in tmp_path.iterdir()) == ["keep.mefd", "s.mefd", "t.mefd.tar"]


def test_a_raising_callback_leaves_no_archive(session):
    def progress(done, total):
        if done == total:
            raise KeyError("at the very end")

    before = _listing(session.parent)
    with pytest.raises(KeyError):
        mef3io.archive_session(str(session), progress=progress)
    assert _listing(session.parent) == before


# --- validate / repair -------------------------------------------------------------------


def test_validate_progress_and_report_unchanged(session):
    plain = mef3io.validate_session(str(session))
    rec = Recorder()
    hooked = mef3io.validate_session(str(session), progress=rec, cancel=mef3io.CancelToken())
    rec.check()
    assert hooked.findings == plain.findings and hooked.segments_checked == plain.segments_checked


def test_validate_cancelled_from_another_thread(session):
    err, _ = cancel_mid_flight(lambda **kw: mef3io.Validator(str(session)).validate(**kw))
    assert isinstance(err, mef3io.Cancelled), err


def _break_difference_bytes(session):
    """Zero maximum_difference_bytes in every segment: one repair per segment."""
    s2 = 1024 + 1536 + 6388
    for tmet in sorted(session.rglob("*.tmet")):
        raw = bytearray(tmet.read_bytes())
        struct.pack_into("<I", raw, s2, 0)
        struct.pack_into("<I", raw, 4, m.crc32(bytes(raw[UH:16384])))
        struct.pack_into("<I", raw, 0, m.crc32(bytes(raw[4:UH])))
        tmet.write_bytes(bytes(raw))


def test_repair_cancelled_mid_pass_writes_nothing(session):
    _break_difference_bytes(session)
    before = _digest(session)
    parent_before = _listing(session.parent)
    err, _ = cancel_mid_flight(lambda **kw: mef3io.repair_session(
        str(session), ["sizing.difference-bytes"], **kw))
    assert isinstance(err, mef3io.Cancelled), err
    assert _digest(session) == before, "a cancelled repair modified the session"
    assert _listing(session.parent) == parent_before, "a backup directory was created"

    # Uncancelled, the same call does repair every segment.
    rec = Recorder()
    report = mef3io.repair_session(str(session), ["sizing.difference-bytes"], progress=rec)
    rec.check()
    assert report.segments_repaired == len(CHANNELS) * SEGMENTS


def test_a_cancel_after_repair_started_writing_is_ignored(session):
    _break_difference_bytes(session)
    token = mef3io.CancelToken()

    def progress(done, total):
        if done == total:  # reported after every write has landed
            token.cancel()

    report = mef3io.repair_session(str(session), ["sizing.difference-bytes"],
                                   cancel=token, progress=progress)
    assert token.cancelled
    assert report.segments_repaired == len(CHANNELS) * SEGMENTS


def test_pre_cancelled_token_stops_everything_before_any_write(session, tmp_path):
    token = mef3io.CancelToken()
    token.cancel()
    _break_difference_bytes(session)
    before = _digest(session)
    parent_before = _listing(session.parent)
    with pytest.raises(mef3io.Cancelled):
        mef3io.archive_session(str(session), cancel=token)
    with pytest.raises(mef3io.Cancelled):
        mef3io.validate_session(str(session), cancel=token)
    with pytest.raises(mef3io.Cancelled):
        mef3io.repair_session(str(session), ["sizing.difference-bytes"], cancel=token)
    with pytest.raises(mef3io.Cancelled):
        mef3io.recover_session(str(session), apply=True, cancel=token)
    assert _digest(session) == before
    assert _listing(session.parent) == parent_before


# --- recover -----------------------------------------------------------------------------


def _drop_index_tail(session, keep_fraction=0.5):
    """Simulate an interrupted fast append in EVERY segment: blocks reached
    .tdat, the .tidx extension did not. Recovery would rebuild the entries."""
    for tidx in sorted(session.rglob("*.tidx")):
        raw = bytearray(tidx.read_bytes())
        n = (len(raw) - UH) // TSI
        keep = max(1, int(n * keep_fraction))
        raw = raw[: UH + keep * TSI]
        struct.pack_into("<q", raw, 32, keep)
        struct.pack_into("<I", raw, 4, m.crc32(bytes(raw[UH:])))
        struct.pack_into("<I", raw, 0, m.crc32(bytes(raw[4:UH])))
        tidx.write_bytes(bytes(raw))


def test_recover_cancelled_mid_scan_writes_nothing(session):
    _drop_index_tail(session)
    before = _digest(session)
    parent_before = _listing(session.parent)
    err, _ = cancel_mid_flight(lambda **kw: mef3io.recover_session(
        str(session), apply=True, **kw))
    assert isinstance(err, mef3io.Cancelled), err
    assert _digest(session) == before, "a cancelled recovery modified the session"
    assert _listing(session.parent) == parent_before, "a backup directory was created"

    # Uncancelled, it recovers every segment and the data reads whole again.
    rec = Recorder()
    report = mef3io.recover_session(str(session), apply=True, progress=rec)
    rec.check()
    assert len(report.segments) == len(CHANNELS) * SEGMENTS
    assert all(s.blocks_recovered > 0 for s in report.segments)
    with mef3io.Reader(str(session)) as r:
        assert np.isfinite(r.read("c")).sum() == SEGMENTS * N


def test_recover_dry_run_progress_on_a_healthy_session(session):
    rec = Recorder()
    report = mef3io.recover_session(str(session), progress=rec)
    assert report.nothing_to_do
    total = rec.check()
    assert total == sum(f.stat().st_size for f in session.rglob("*")
                        if f.suffix in (".tidx", ".tdat"))


def test_repair_skips_a_segment_that_changed_between_the_pass_and_the_write(session):
    """The write phase re-checks each segment against what the pass read, so a
    writer that touched it in between is not undone by a stale derivation."""
    _break_difference_bytes(session)
    first_tdat = sorted(session.rglob("a-*.tdat"))[0]
    touched = []

    def progress(done, total):
        if 0 < done < total and not touched:
            # Still inside the first segment (a, 0), whose files were already
            # read: change its .tdat header the way a concurrent writer would.
            raw = bytearray(first_tdat.read_bytes()[:UH])
            raw[100] ^= 0x01
            struct.pack_into("<I", raw, 0, m.crc32(bytes(raw[4:UH])))
            with open(first_tdat, "r+b") as f:
                f.write(bytes(raw))
            touched.append(done)

    report = mef3io.repair_session(str(session), ["sizing.difference-bytes"], progress=progress)
    assert touched
    assert report.segments_repaired == len(CHANNELS) * SEGMENTS - 1
    changed = [s for s in report.skipped if "changed while it was being validated" in s.reason]
    assert len(changed) == 1 and changed[0].channel == "a", report.skipped
    still = [f for f in report.findings
             if f.check_id == "sizing.difference-bytes" and f.channel == changed[0].channel
             and f.segment == changed[0].segment]
    assert still and not any(f.repaired for f in still)


@pytest.mark.parametrize("op", ["repair", "recover"])
def test_a_callback_raising_at_the_end_of_the_pass_writes_nothing(session, op):
    """(total, total) is reported when the read-only pass ends, BEFORE any
    write — so a callback that raises there (a UI's abort, a Ctrl-C) cannot
    leave files rewritten with the report of what changed lost."""
    if op == "repair":
        _break_difference_bytes(session)
    else:
        _drop_index_tail(session)
    before = _digest(session)
    parent_before = _listing(session.parent)

    def progress(done, total):
        if done == total:
            raise KeyError("abort at the end")

    with pytest.raises(KeyError):
        if op == "repair":
            mef3io.repair_session(str(session), ["sizing.difference-bytes"], progress=progress)
        else:
            mef3io.recover_session(str(session), apply=True, progress=progress)
    assert _digest(session) == before
    assert _listing(session.parent) == parent_before


def test_an_empty_read_reports_zero_of_zero_once(session):
    with mef3io.Reader(str(session)) as r:
        rec = Recorder()
        t = r.info("a")["start_time"]
        assert r.read("a", t, t, progress=rec).size == 0
        assert rec.calls == [(0, 0)]


def test_extract_without_overwrite_never_deletes_a_target_that_appears(session, tmp_path):
    tar = mef3io.archive_session(str(session), str(tmp_path / "t.mefd.tar"))
    target = tmp_path / "late.mefd"
    created = []

    def progress(done, total):
        if 0 < done < total and not created:
            target.mkdir()
            (target / "theirs.txt").write_text("someone else's")
            created.append(True)

    with pytest.raises(mef3io.IoError, match="appeared"):
        mef3io.extract_session(tar, str(target), progress=progress)
    assert (target / "theirs.txt").read_text() == "someone else's"
    assert sorted(p.name for p in tmp_path.iterdir()) == ["late.mefd", "s.mefd", "t.mefd.tar"]
