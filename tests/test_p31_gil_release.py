"""P31: opening a session lets other Python threads run (#79).

Opening scans the session tree and parses every segment's metadata. It used to
run with the GIL held, so every other Python thread, including a GUI's event
loop, froze for the whole open (measured: 613 ms of a 616 ms open on 64 x 60
segments). The constructors of ``Reader``, ``Session`` and ``SessionWriter``
now construct the C++ object with the GIL released.

Measured the way the issue asks: the open runs on a worker thread while the
main thread sleeps in short steps and records its longest gap. With the GIL
held the gap is the whole open; released, it is a few milliseconds. The bound
is generous (half the open, never under 100 ms) so a loaded CI machine does
not flake, and it still fails by a wide margin when the GIL is held.
"""
from __future__ import annotations

import threading
import time

import numpy as np
import pytest

import mef3io
from mef3io import _mef3io as m

START = 1577836800000000
CHANNELS, SEGMENTS = 48, 50   # 2400 .tmet files: an open of a few hundred ms


@pytest.fixture(scope="module")
def many_segments(tmp_path_factory):
    path = tmp_path_factory.mktemp("gil") / "many.mefd"
    w = m.SessionWriter(str(path), True)
    x = np.arange(256, dtype=np.int32)
    for ch in range(CHANNELS):
        for seg in range(SEGMENTS):
            w.write_int32(f"ch{ch:03d}", x, 1.0, START + seg * 10_000_000, 256.0, None, seg > 0)
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


@pytest.mark.parametrize("open_", [
    pytest.param(lambda p: mef3io.Reader(p), id="mef3io.Reader"),
    pytest.param(lambda p: mef3io.Reader(p, strict=False), id="mef3io.Reader-lenient"),
    pytest.param(lambda p: m.Reader(p), id="_mef3io.Reader"),
    pytest.param(lambda p: m.Session(p), id="_mef3io.Session"),
])
def test_an_open_does_not_hold_the_gil(many_segments, open_):
    open_(many_segments)  # warm the page cache: measure parsing, not the disk
    duration, stall = _longest_stall(lambda: open_(many_segments))
    if duration < 0.1:
        pytest.skip(f"the open took {duration * 1e3:.0f} ms: too fast to tell a stall apart")
    bound = max(0.1, duration / 2)
    assert stall < bound, (
        f"the main thread stalled {stall * 1e3:.0f} ms during a {duration * 1e3:.0f} ms open: "
        "the open holds the GIL")


def test_a_writer_open_does_not_hold_the_gil(many_segments, tmp_path):
    """Reopening for append (and overwrite=True's removal) runs without the GIL too.
    Hydration is lazy, so this open is short; the bound only rules out a hold."""
    _, stall = _longest_stall(lambda: m.SessionWriter(many_segments, False))
    assert stall < 0.1
    import shutil
    copy = tmp_path / "copy.mefd"
    shutil.copytree(many_segments, copy)
    duration, stall = _longest_stall(lambda: m.SessionWriter(str(copy), True))
    assert not any(copy.glob("*.timd"))  # overwrite really removed the old tree
    assert stall < max(0.1, duration / 2)


def test_errors_still_raise_after_the_gil_comes_back(tmp_path):
    """A constructor that throws re-acquires the GIL while unwinding."""
    with pytest.raises(mef3io.IoError):
        m.Reader(str(tmp_path / "nope.mefd"))
    with pytest.raises(mef3io.IoError):
        m.Session(str(tmp_path / "not-a-session"))
    with pytest.raises(mef3io.IoError):
        m.SessionWriter(str(tmp_path / "x.mefd.tar"), True)


def test_concurrent_opens_agree(many_segments):
    """Opens on several threads at once, now that they can overlap."""
    out = [None] * 4

    def work(i):
        r = m.Reader(many_segments)
        out[i] = (len(r.channels), r.info("ch000")["number_of_samples"])

    threads = [threading.Thread(target=work, args=(i,)) for i in range(len(out))]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    assert out == [(CHANNELS, 256 * SEGMENTS)] * len(out)
