"""P42: ONE thread rule (docs/threads.md), the parallel open (#141), #110.

``n_threads`` is how many threads an object may use for everything it does
(opening, file access, decoding, decrypting, encoding); 0 = automatic, 1 = no
extra threads. Pinned here:

* the open's result does not depend on it: channels, infos, segment maps,
  ``problems`` (lenient, same order) and the strict error, on a directory and
  on a tar, with damaged and incomplete segments;
* it is a settable property on Reader, MultiReader and Writer, applied from
  the next call, also while other threads read;
* there is no per-call ``n_threads`` any more (removed in 2.0);
* a MultiReader's ``n_threads`` is a TOTAL: never more threads at once than
  the budget, on open or on read, and a read inside one recording gets all of
  it (the split is private: ``Reader._read_raw(_split=)``);
* #110: one backend per warm Reader however many threads race to build it, a
  close() racing the first build stays closed, and a skipped record file is
  warned about once, not once per concurrent call.
"""
from __future__ import annotations

import inspect as _inspect
import os
import threading
import time
import warnings
from pathlib import Path

import numpy as np
import pytest

import mef3io
from mef3io import _mef3io as ext
from mef3io import _multi

START = 1_577_880_000_000_000
SECOND = 1_000_000
THREADS = (1, 2, 8, 32)


def _quiet(fn, *a, **kw):
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        return fn(*a, **kw)


def _write(path, channels=("a", "b", "c", "d"), segments=4, start=START):
    rng = np.random.default_rng(7)
    with mef3io.Writer(str(path)) as w:
        for s in range(segments):
            for ch in channels:
                x = rng.integers(-500, 500, 1000).astype(np.float64)
                w.write(ch, x, start + s * 5 * SECOND, 250.0, precision=0, new_segment=s > 0)
    return Path(path)


def _seg(path, ch, n, ext_):
    stem = f"{ch}-{n:06d}"
    return Path(path) / f"{ch}.timd" / f"{stem}.segd" / f"{stem}{ext_}"


def _damage(path):
    os.truncate(_seg(path, "a", 1, ".tmet"), 2000)                  # short .tmet
    with open(_seg(path, "b", 3, ".tmet"), "r+b") as f:             # body CRC
        f.seek(6000)
        f.write(b"\x5a")
    os.remove(_seg(path, "c", 2, ".tdat"))                          # incomplete
    os.truncate(_seg(path, "d", 0, ".tmet"), 10)
    return path


@pytest.fixture(scope="module")
def damaged(tmp_path_factory):
    d = tmp_path_factory.mktemp("p42")
    path = _damage(_write(d / "dmg.mefd"))
    tar = mef3io.archive_session(str(path))
    return [str(path), str(tar)]


def _describe(r):
    out = []
    for ch in r.channels:
        out.append((ch, sorted(r.info(ch).items())))
        out.append((ch, [sorted(s.items(), key=lambda kv: kv[0]) for s in r.segments(ch)]))
    out.append(("problems", r.problems))
    out.append(("decl", r.declaration_issues))
    return out


# --------------------------------------------------------------------------
# the open does not depend on the count
# --------------------------------------------------------------------------

@pytest.mark.parametrize("which", [0, 1], ids=["dir", "tar"])
def test_lenient_open_is_identical_at_every_thread_count(damaged, which):
    path = damaged[which]
    with _quiet(mef3io.Reader, path, strict=False, n_threads=1) as r:
        want = _describe(r)
        assert len(r.problems) == 3
        assert sorted((p["channel"], p["segment_number"]) for p in r.problems) == [
            ("a", 1), ("b", 3), ("d", 0)]  # in channel LISTING order, which is the OS's
    for t in THREADS:
        with _quiet(mef3io.Reader, path, strict=False, n_threads=t) as r:
            assert _describe(r) == want, t


@pytest.mark.parametrize("which", [0, 1], ids=["dir", "tar"])
def test_strict_open_raises_the_same_error_at_every_thread_count(damaged, which):
    path = damaged[which]

    def failure(t):
        with pytest.raises(Exception) as e:
            mef3io.Reader(path, n_threads=t)
        return type(e.value), str(e.value)

    want = failure(1)
    for t in THREADS:
        assert failure(t) == want, t


def test_reads_are_identical_at_every_thread_count(tmp_path):
    path = _write(tmp_path / "s.mefd")
    with mef3io.Reader(str(path), n_threads=1) as r:
        want = r.read_raw("a")
    for t in THREADS + (0,):
        with mef3io.Reader(str(path), n_threads=t) as r:
            got = r.read_raw("a")
            np.testing.assert_array_equal(got["samples"], want["samples"])
            np.testing.assert_array_equal(got["valid"], want["valid"])


# --------------------------------------------------------------------------
# one count per object, settable, no per-call override
# --------------------------------------------------------------------------

def test_resolve_threads_is_the_core_s():
    assert ext.resolve_threads(0) >= 1
    assert ext.resolve_threads(0, True) >= max(8, ext.resolve_threads(0))
    assert ext.resolve_threads(3) == ext.resolve_threads(3, True) == 3
    assert not hasattr(_multi, "_threads_total")  # the old Python duplicate is gone


def test_reader_n_threads_property(tmp_path):
    path = _write(tmp_path / "s.mefd")
    with mef3io.Reader(str(path)) as r:
        assert r.n_threads == 0
        r.read("a")
        r.n_threads = 3
        assert r.n_threads == 3 and r._impl.threads() == 3
        r.n_threads = 0
        assert r._impl.threads() == 0
    with mef3io.Reader(str(path), n_threads=5) as r:
        assert r.n_threads == 5 and r._impl.threads() == 5
    for bad, exc in ((-1, ValueError), (1.5, TypeError), (True, TypeError), ("2", TypeError)):
        with pytest.raises(exc):
            mef3io.Reader(str(path), n_threads=bad)
        with mef3io.Reader(str(path)) as r, pytest.raises(exc):
            r.n_threads = bad


def test_a_warm_reader_builds_its_backend_with_the_current_count(tmp_path):
    path = _write(tmp_path / "s.mefd", segments=6)
    cache = tmp_path / "snap.json"
    _quiet(mef3io.Reader, str(path), cache=str(cache)).close()
    with mef3io.Reader(str(path), cache=str(cache)) as r:
        assert r._impl is None  # served from the snapshot
        r.n_threads = 2
        r.read("a")
        assert r._impl.threads() == 2


def test_there_is_no_per_call_n_threads(tmp_path):
    path = _write(tmp_path / "s.mefd")
    for cls in (mef3io.Reader, mef3io.MultiReader):
        for name in ("read", "read_raw"):
            params = _inspect.signature(getattr(cls, name)).parameters
            assert "n_threads" not in params and "_n_threads" not in params, (cls, name)
            assert "_split" not in params
    with mef3io.Reader(str(path)) as r:
        with pytest.raises(TypeError):
            r.read("a", None, None, 2)
        with pytest.raises(TypeError):
            r.read_raw("a", n_threads=2)
    with _quiet(mef3io.MultiReader, [str(path)]) as mr, pytest.raises(TypeError):
        mr.read("a", n_threads=2)


def test_setting_n_threads_while_other_threads_read(tmp_path):
    path = _write(tmp_path / "s.mefd", segments=8)
    with mef3io.Reader(str(path), n_threads=2) as r:
        want = r.read_raw("a")
        stop = threading.Event()
        errors, wrong = [], []

        def worker():
            try:
                while not stop.is_set():
                    got = r.read_raw("a")
                    if not (np.array_equal(got["samples"], want["samples"])
                            and np.array_equal(got["valid"], want["valid"])):
                        wrong.append(1)
            except BaseException as e:  # noqa: BLE001
                errors.append(e)

        ts = [threading.Thread(target=worker) for _ in range(4)]
        for t in ts:
            t.start()
        for i in range(200):
            r.n_threads = i % 5
        stop.set()
        for t in ts:
            t.join()
        assert not errors and not wrong


def test_writer_n_threads_property(tmp_path):
    x = np.arange(25_000, dtype=np.float64)
    outs = []
    for n in (1, 4):
        path = tmp_path / f"w{n}.mefd"
        with mef3io.Writer(str(path), n_threads=n) as w:
            assert w.n_threads == n
            w.n_threads = 3
            assert w.n_threads == 3 and w._impl.threads() == 3
            w.n_threads = n
            w.write("a", x, START, 250.0, precision=0)
        outs.append(_seg(path, "a", 0, ".tdat").read_bytes()[1024:])  # past the UH (UUIDs)
    assert outs[0] == outs[1]
    with pytest.raises(ValueError):
        mef3io.Writer(str(tmp_path / "x.mefd"), n_threads=-2)


# --------------------------------------------------------------------------
# MultiReader: a TOTAL budget, split, never multiplied
# --------------------------------------------------------------------------

class _Meter:
    """The thread counts DECLARED to concurrent opens and reads, summed.

    A MODEL check, not a measured peak: it adds up the count each sub-Reader
    open is given (resolved for file access) and the share each read is
    passed (``_split``), while those calls overlap (each sleeps 20 ms so they
    do). The core runs at most that many threads per call (parallel_for), so
    the sum bounds the real total. Real thread starts are counted in C++
    (``thread_start_hook``, core/tests/test_parallel_open.cpp).
    """

    def __init__(self):
        self.lock = threading.Lock()
        self.now = 0
        self.peak = 0
        self.shares = []

    def enter(self, n):
        with self.lock:
            self.now += n
            self.peak = max(self.peak, self.now)
            self.shares.append(n)

    def leave(self, n):
        with self.lock:
            self.now -= n


@pytest.fixture
def days(tmp_path):
    return [str(_write(tmp_path / f"d{i}.mefd", channels=("a",), segments=2,
                       start=START + i * 60 * SECOND)) for i in range(6)]


def _metered(monkeypatch):
    opens, reads = _Meter(), _Meter()
    real_init = mef3io.Reader.__init__
    real_read = mef3io.Reader._read_raw

    def init(self, path, password="", backend="cpp", n_threads=0, *a, **kw):
        share = ext.resolve_threads(n_threads, True)
        opens.enter(share)
        try:
            time.sleep(0.02)  # make concurrent opens overlap
            real_init(self, path, password, backend, n_threads, *a, **kw)
        finally:
            opens.leave(share)

    def read(self, channel, t0, t1, hooks, pending, _split=None):
        share = _split if _split is not None else ext.resolve_threads(self.n_threads)
        reads.enter(share)
        try:
            time.sleep(0.02)
            return real_read(self, channel, t0, t1, hooks, pending, _split=_split)
        finally:
            reads.leave(share)

    monkeypatch.setattr(mef3io.Reader, "__init__", init)
    monkeypatch.setattr(mef3io.Reader, "_read_raw", read)
    return opens, reads


@pytest.mark.parametrize("budget", [1, 2, 4, 5, 16])
def test_multireader_never_uses_more_than_its_budget(monkeypatch, days, budget):
    """The declared shares of overlapping opens/reads never sum past the budget
    (see _Meter), and a read inside one recording is given all of it."""
    opens, reads = _metered(monkeypatch)
    with _quiet(mef3io.MultiReader, days, n_threads=budget) as mr:
        assert opens.peak <= max(budget, 1)
        mr.read("a")  # touches all six recordings
        assert reads.peak <= budget
        assert len(reads.shares) >= len(days)
        # A read inside ONE recording gets the whole budget.
        reads.shares.clear()
        info = mr._readers[2].info("a")
        mr.read("a", info["start_time"], info["start_time"] + SECOND)
        assert reads.shares == [budget]


@pytest.mark.parametrize("budget", [1, 2, 5])
def test_a_multireader_list_read_stays_within_its_budget(monkeypatch, days, budget):
    """A multi-channel read (#90) is a loop of the MultiReader's own reads:
    each splits the ONE budget, they never overlap each other."""
    opens, reads = _metered(monkeypatch)
    with _quiet(mef3io.MultiReader, days, n_threads=budget) as mr:
        x = mr.read(["a", "a", "a"])
        assert x.shape[0] == 3
        assert reads.peak <= budget
        assert len(reads.shares) >= 3 * len(days)


def test_multireader_automatic_budget_is_the_core_s(monkeypatch, days):
    opens, reads = _metered(monkeypatch)
    with _quiet(mef3io.MultiReader, days) as mr:
        assert opens.peak <= ext.resolve_threads(0, True)
        mr.read("a")
        assert reads.peak <= ext.resolve_threads(0)
        reads.shares.clear()
        info = mr._readers[0].info("a")
        mr.read("a", info["start_time"], info["start_time"] + SECOND)
        assert reads.shares == [ext.resolve_threads(0)]


def test_setting_multireader_n_threads_resplits(monkeypatch, days):
    opens, reads = _metered(monkeypatch)
    with _quiet(mef3io.MultiReader, days, n_threads=12) as mr:
        assert mr.n_threads == 12
        assert [r.n_threads for r in mr._readers] == [2] * 6
        mr.n_threads = 3
        assert mr.n_threads == 3
        assert [r.n_threads for r in mr._readers] == [1] * 6
        reads.peak = 0
        mr.read("a")
        assert reads.peak <= 3
        with pytest.raises(ValueError):
            mr.n_threads = -1


def test_multireader_results_do_not_depend_on_the_budget(days):
    with _quiet(mef3io.MultiReader, days, n_threads=1) as mr:
        want = mr.read("a")
    for n in (0, 2, 7, 32):
        with _quiet(mef3io.MultiReader, days, n_threads=n) as mr:
            np.testing.assert_array_equal(mr.read("a"), want)


# --------------------------------------------------------------------------
# #110: the Python Reader's own races
# --------------------------------------------------------------------------

def test_one_backend_per_warm_reader_whatever_the_threads(monkeypatch, tmp_path):
    path = _write(tmp_path / "s.mefd", segments=6)
    cache = tmp_path / "snap.json"
    _quiet(mef3io.Reader, str(path), cache=str(cache)).close()
    built = []
    real = ext.Reader

    def counting(*a, **kw):
        built.append(1)
        time.sleep(0.01)  # widen the window two threads used to slip through
        return real(*a, **kw)

    monkeypatch.setattr(ext, "Reader", counting)
    readers = [mef3io.Reader(str(path), cache=str(cache)) for _ in range(5)]
    assert all(r._impl is None for r in readers)
    barrier = threading.Barrier(8 * len(readers))

    def go(r):
        barrier.wait()
        r.toc("a")

    ts = [threading.Thread(target=go, args=(r,)) for r in readers for _ in range(8)]
    for t in ts:
        t.start()
    for t in ts:
        t.join()
    assert len(built) == len(readers)


def test_close_racing_the_first_build_stays_closed(monkeypatch, tmp_path):
    path = _write(tmp_path / "s.mefd", segments=6)
    cache = tmp_path / "snap.json"
    _quiet(mef3io.Reader, str(path), cache=str(cache)).close()
    real = ext.Reader
    started = threading.Event()

    def slow(*a, **kw):
        started.set()
        time.sleep(0.1)
        return real(*a, **kw)

    monkeypatch.setattr(ext, "Reader", slow)
    r = mef3io.Reader(str(path), cache=str(cache))
    t = threading.Thread(target=lambda: r.toc("a"))
    t.start()
    started.wait()
    r.close()  # waits for the build, then closes
    t.join()
    assert r._impl is None
    with pytest.raises(ValueError, match="closed"):
        r.read("a")


def test_a_skipped_record_file_is_warned_about_once(tmp_path):
    path = _write(tmp_path / "s.mefd")
    with mef3io.Writer(str(path)) as w:
        w.write_annotations([{"type": "Note", "time": START + SECOND, "text": "x"}], channel="a")
    rdat = Path(path) / "a.timd" / "a.rdat"
    raw = bytearray(rdat.read_bytes())
    raw[1024 + 30] ^= 0xFF  # inside the record body: its CRC fails
    rdat.write_bytes(bytes(raw))
    with _quiet(mef3io.Reader, str(path), strict=False) as r:
        barrier = threading.Barrier(8)
        with warnings.catch_warnings(record=True) as caught:
            warnings.simplefilter("always")

            def go():
                barrier.wait()
                r.records("a", level="channel")

            ts = [threading.Thread(target=go) for _ in range(8)]
            for t in ts:
                t.start()
            for t in ts:
                t.join()
        skipped = [w for w in caught if issubclass(w.category, mef3io.UnreadableSegmentWarning)]
        assert len(skipped) == 1, [str(w.message) for w in skipped]
        assert sum(p["kind"] == "records" for p in r.problems) == 1


def test_multireader_with_one_thread_starts_no_thread(monkeypatch, days):
    """n_threads=1 = no extra thread, also for the MultiReader's own open/read."""
    started = []
    real_start = threading.Thread.start

    def start(self):
        started.append(self.name)
        return real_start(self)

    monkeypatch.setattr(threading.Thread, "start", start)
    with _quiet(mef3io.MultiReader, days, n_threads=1) as mr:
        mr.read("a")
        mr.segments("a")
        mr.toc("a")
    assert started == []


def test_a_negative_count_is_refused_by_the_backend_too(tmp_path):
    path = _write(tmp_path / "s.mefd")
    with pytest.raises(ValueError):
        ext.Reader(str(path), "", -1)
    r = ext.Reader(str(path), "", 2)
    with pytest.raises(ValueError):
        r.set_threads(-3)
    assert r.threads() == 2


# --------------------------------------------------------------------------
# #110 item 3 without data loss (#146 review): a windowed read returns what
# the same window of a FULL read holds, even when a segment's .tmet end time
# understates its data. Each case failed with the .tmet-end skip.
# --------------------------------------------------------------------------

def _finite(x):
    return int(np.isfinite(x).sum())


def _assert_window_matches_full_read(path, channel, t0, t1, expected):
    with _quiet(mef3io.Reader, str(path)) as r:
        start = r.info(channel)["start_time"]
        full = r.read(channel, start, t1 + 10 * SECOND)
        fs = r.info(channel)["sampling_frequency"]
        i0 = int(round((t0 - start) * fs / 1e6))
        want = full[i0:i0 + int(round((t1 - t0) * fs / 1e6))]
        got = r.read(channel, t0, t1)
        assert _finite(got) == _finite(want) == expected
        np.testing.assert_array_equal(got, want)
        env = r.block_envelope(channel, t0, t1)
        assert len(env["start_uutc"]) > 0  # the envelope sees those blocks too


def _stale_tmet_session(path, fs, block, first_s, appended_s, then_s):
    """Segment 0 written for `first_s` seconds, its .tmet saved, then appended
    to `first_s + appended_s` s in place; a NEW segment at `then_s`; segment
    0's .tmet put back. Exactly what an interrupted append leaves (the .tdat
    and .tidx extended, the .tmet not yet renamed) once the recorder restarts
    in a new segment — and what a writer with a short header end produces."""
    rng = np.random.default_rng(3)
    with mef3io.Writer(str(path), block_length=block) as w:
        w.write("ch", rng.integers(-99, 99, int(first_s * fs)).astype(float), START, fs,
                precision=0)
    tmet = _seg(path, "ch", 0, ".tmet")
    saved = tmet.read_bytes()
    with mef3io.Writer(str(path), block_length=block) as w:
        w.write("ch", rng.integers(-99, 99, int(appended_s * fs)).astype(float),
                START + first_s * SECOND, fs, precision=0)
        w.write("ch", rng.integers(-99, 99, int(10 * fs)).astype(float),
                START + then_s * SECOND, fs, precision=0, new_segment=True)
    tmet.write_bytes(saved)
    return path


def test_window_inside_an_interrupted_append_of_a_non_last_segment(tmp_path):
    # (a): .tmet end 80 s, data to 100 s, then a new segment at 120 s.
    path = _stale_tmet_session(tmp_path / "a.mefd", 256.0, 2560, 80, 20, 120)
    _assert_window_matches_full_read(path, "ch", START + 90 * SECOND, START + 100 * SECOND,
                                     2560)


def test_window_inside_a_long_last_block_past_the_header_end(tmp_path):
    # (b): 10 s blocks; the header ends where the LAST block starts (50 s).
    path = _stale_tmet_session(tmp_path / "b.mefd", 100.0, 1000, 50, 10, 100)
    _assert_window_matches_full_read(path, "ch", START + 52 * SECOND, START + 58 * SECOND, 600)


def test_window_past_a_short_end_written_by_mef_tools(tmp_path):
    # (c): mef_tools writes the caller's end_uutc verbatim into the header.
    pytest.importorskip("mef_tools", reason="legacy oracle not installed")
    from mef_tools.io import MefWriter

    path = tmp_path / "c.mefd"
    fs = 256.0
    w = MefWriter(str(path))
    x = np.arange(int(60 * fs), dtype=np.float64) % 50
    w.write_data(x, "ch", START, fs, end_uutc=START + 30 * SECOND, precision=0,
                 discont_handler=False)
    w.write_data(x[: int(10 * fs)], "ch", START + 100 * SECOND, fs, precision=0,
                 new_segment=True)
    del w
    _assert_window_matches_full_read(path, "ch", START + 40 * SECOND, START + 50 * SECOND, 2560)


@pytest.mark.parametrize("negated", [True, False], ids=["negated", "positive"])
def test_a_late_tmet_start_does_not_hide_the_segment(tmp_path, negated):
    # #146 round-2 N2: no skip may trust a .tmet START either. Segment 1's
    # header start is put 100 s after its first block (both sign conventions
    # a file can store, rto 0); a window at the real data start must still
    # return it, from read() and from block_envelope().
    import struct

    path = tmp_path / "late.mefd"
    fs = 256.0
    rng = np.random.default_rng(5)
    s1 = START + 3600 * SECOND
    with mef3io.Writer(str(path)) as w:
        w.write("ch", rng.integers(-99, 99, int(10 * fs)).astype(float), START, fs, precision=0)
        w.write("ch", rng.integers(-99, 99, int(10 * fs)).astype(float), s1, fs, precision=0,
                new_segment=True)
    tmet = _seg(path, "ch", 1, ".tmet")
    raw = bytearray(tmet.read_bytes())
    late = s1 + 100 * SECOND
    struct.pack_into("<q", raw, 16, -late if negated else late)
    struct.pack_into("<I", raw, 0, ext.crc32(bytes(raw[4:1024])))
    tmet.write_bytes(bytes(raw))
    with _quiet(mef3io.Reader, str(path)) as r:
        got = r.read("ch", s1, s1 + 10 * SECOND)
        assert _finite(got) == 2560
        env = r.block_envelope("ch", s1, s1 + 10 * SECOND)
        assert len(env["start_uutc"]) == 1
