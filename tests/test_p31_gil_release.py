"""P31: long binding calls let other Python threads run (#79).

Opening scans the session tree and parses every segment's metadata. It used to
run with the GIL held, so every other Python thread, including a GUI's event
loop, froze for the whole open (measured: 613 ms of a 616 ms open on 64 x 60
segments). The constructors of ``Reader``, ``Session`` and ``SessionWriter``
now construct the C++ object with the GIL released, and so do ``toc``,
``segments``, ``records`` and ``write_records``.

Measured the way the issue asks: the call runs on a worker thread while the
main thread sleeps in short steps and records its longest gap. With the GIL
held the gap is the whole call; released, it is a few milliseconds. Each case
takes the BEST of three runs (an OS hiccup is random, a held GIL is not) and
asserts the stall is under half the call. A call too short to tell a stall
from scheduling noise skips, rather than passing without proving anything.

Also here: concurrent calls on ONE Session/Reader, which releasing the GIL
makes reachable (the block-index cache used to free an index another thread
was still reading).
"""
from __future__ import annotations

import shutil
import threading
import time

import numpy as np
import pytest

import mef3io
from mef3io import _mef3io as m

START = 1577836800000000
SEGMENTS = 2400   # one channel: an open parses 2400 .tmet, a toc reads 2400 .tidx
MIN_CALL = 0.04   # below this a stall cannot be told from scheduling noise


@pytest.fixture(scope="module")
def many_segments(tmp_path_factory):
    path = tmp_path_factory.mktemp("gil") / "many.mefd"
    w = m.SessionWriter(str(path), True)
    x = np.arange(256, dtype=np.int32)
    for seg in range(SEGMENTS):
        w.write_int32("ch", x, 1.0, START + seg * 10_000_000, 256.0, None, seg > 0)
    del w
    return str(path)


def _longest_stall(fn):
    """Run ``fn`` on a worker thread; return (its duration, the main thread's longest gap)."""
    done = threading.Event()
    result = {}

    def work():
        t0 = time.perf_counter()
        try:
            fn()
        except BaseException as e:  # noqa: BLE001 - re-raised below, on the main thread
            result["error"] = e
        result["duration"] = time.perf_counter() - t0
        done.set()

    worker = threading.Thread(target=work)
    worst = 0.0
    last = time.perf_counter()
    worker.start()
    while not done.is_set():
        time.sleep(0.002)
        now = time.perf_counter()
        worst = max(worst, now - last)
        last = now
    worker.join()
    if "error" in result:
        raise result["error"]
    return result["duration"], worst


def _assert_releases(make_call, runs=3):
    """``make_call()`` returns the zero-argument call to measure (set up with the GIL)."""
    best = None
    for _ in range(runs):
        duration, stall = _longest_stall(make_call())
        if best is None or stall / duration < best[1] / best[0]:
            best = (duration, stall)
    duration, stall = best
    if duration < MIN_CALL:
        pytest.skip(f"the call took {duration * 1e3:.0f} ms: too short to tell a stall apart")
    assert stall < duration / 2, (
        f"the main thread stalled {stall * 1e3:.0f} ms during a {duration * 1e3:.0f} ms call: "
        "it holds the GIL")


@pytest.mark.parametrize("open_", [
    pytest.param(lambda p: mef3io.Reader(p), id="mef3io.Reader"),
    pytest.param(lambda p: mef3io.Reader(p, strict=False), id="mef3io.Reader-lenient"),
    pytest.param(lambda p: m.Reader(p), id="_mef3io.Reader"),
    pytest.param(lambda p: m.Session(p), id="_mef3io.Session"),
])
def test_an_open_does_not_hold_the_gil(many_segments, open_):
    open_(many_segments)  # warm the page cache: measure parsing, not the disk
    _assert_releases(lambda: (lambda: open_(many_segments)))


def test_toc_does_not_hold_the_gil(many_segments):
    """toc reads every segment's .tidx. A fresh Reader each run, so the index
    cache is cold and the reads really happen."""
    _assert_releases(lambda: (lambda r=m.Reader(many_segments): r.toc("ch")))


def test_a_writer_open_does_not_hold_the_gil(many_segments, tmp_path):
    """overwrite=True removes the old tree first: without the GIL too. (A
    reopen for append hydrates lazily and is too short to measure.)"""
    def overwrite():
        copy = tmp_path / "copy.mefd"
        if copy.exists():
            shutil.rmtree(copy)
        shutil.copytree(many_segments, copy)
        return lambda: m.SessionWriter(str(copy), True)
    _assert_releases(overwrite, runs=2)
    assert not any((tmp_path / "copy.mefd").glob("*.timd"))  # it really removed the tree


def test_errors_still_raise_after_the_gil_comes_back(tmp_path):
    """A constructor that throws re-acquires the GIL while unwinding."""
    with pytest.raises(mef3io.IoError):
        m.Reader(str(tmp_path / "nope.mefd"))
    with pytest.raises(mef3io.IoError):
        m.Session(str(tmp_path / "not-a-session"))
    with pytest.raises(mef3io.IoError):
        m.SessionWriter(str(tmp_path / "x.mefd.tar"), True)


def test_concurrent_opens_agree(many_segments):
    out = [None] * 4

    def work(i):
        r = m.Reader(many_segments)
        out[i] = (list(r.channels), r.info("ch")["number_of_samples"])

    threads = [threading.Thread(target=work, args=(i,)) for i in range(len(out))]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    assert out == [(["ch"], 256 * SEGMENTS)] * len(out)


@pytest.fixture(scope="module")
def small(tmp_path_factory):
    path = tmp_path_factory.mktemp("conc") / "small.mefd"
    with mef3io.Writer(str(path), overwrite=True) as w:
        for seg in range(12):
            w.write("ch", np.sin(np.arange(2560) / 9.0), START + seg * 20_000_000, 256.0,
                    precision=3, new_segment=seg > 0)
        w.write_annotations([{"type": "Note", "time": START + 1, "text": "n"}], "ch")
    return str(path)


def test_concurrent_reads_on_one_session_with_eviction(small):
    """A 1-byte index budget evicts every index at the end of every call while
    other threads are still reading theirs. That freed bytes in use ("double
    free or corruption") until each call pinned its index."""
    s = m.Session(small)
    s.set_index_cache_bytes(1)
    expected = [(r["start_uutc"], r["samples"].copy()) for r in s.read_runs("ch")]
    errors = []

    def work():
        try:
            for _ in range(15):
                got = s.read_runs("ch")
                assert [g["start_uutc"] for g in got] == [e[0] for e in expected]
                for g, e in zip(got, expected):
                    np.testing.assert_array_equal(g["samples"], e[1])
        except BaseException as e:  # noqa: BLE001
            errors.append(e)

    threads = [threading.Thread(target=work) for _ in range(6)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    assert not errors, errors[0]


def test_concurrent_calls_on_one_reader(small):
    """Reads, toc, segments and records from several threads on one Reader."""
    with mef3io.Reader(small) as r:
        ref = (r.read("ch"), r.toc("ch"), r.segments("ch"), r.records("ch"))
        errors = []

        def work():
            try:
                for _ in range(10):
                    np.testing.assert_array_equal(r.read("ch"), ref[0])
                    assert r.toc("ch") == ref[1]
                    assert r.segments("ch") == ref[2]
                    assert r.records("ch") == ref[3]
            except BaseException as e:  # noqa: BLE001
                errors.append(e)

        threads = [threading.Thread(target=work) for _ in range(6)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        assert not errors, errors[0]


def test_a_rename_does_not_hold_the_gil(many_segments, tmp_path):
    """fix_name_drift (the copy's folder name differs from the stored one)
    reads and rewrites every file's header, as rename_session does."""
    def make(i=[0]):
        i[0] += 1
        copy = tmp_path / f"r{i[0]}.mefd"
        shutil.copytree(many_segments, copy)
        return lambda: mef3io.fix_name_drift(copy)
    _assert_releases(make, runs=2)


def test_block_envelope_does_not_hold_the_gil(many_segments):
    """block_envelope reads every segment's .tidx (fresh Reader: cold index cache)."""
    _assert_releases(lambda: (lambda r=m.Reader(many_segments): r.block_envelope("ch", None, None)))
