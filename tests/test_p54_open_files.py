"""P54: a Reader keeps a bounded number of data files open between calls.

Paging through a recording on a network drive used to cost an open, a size
query, a read and a close per channel and segment on every call. A Reader now
keeps up to ``open_files`` read-only ``.tdat`` handles (for a tar session, the
one archive), reads positionally, and checks every handle against its path on
every use. Pinned here (the C++ side in core/tests/test_open_files.cpp):

* ``open_files`` / ``release_files`` on Reader and MultiReader: argument rules,
  the default, settable at runtime, ``0`` = nothing kept;
* results never depend on it: cache off / 1 / small / default, every thread
  count, a directory and a tar;
* the cap is never exceeded (counted on the backend, and as process descriptors
  where ``/proc`` exists); ``release_files`` and ``close`` let go of everything;
* a kept handle never blocks another writer, a delete or a move of the file
  (the Windows share flags, exercised on every platform; Windows itself refuses
  to REPLACE a file anyone holds open, or to rename a folder with open files in
  it), and a file removed or replaced under a handle is never served from the
  old handle;
* a MultiReader's ``open_files`` is a TOTAL over all its recordings.
"""
from __future__ import annotations

import os
import sys
import threading
import warnings
from pathlib import Path

import numpy as np
import pytest

import mef3io

START = 1_577_880_000_000_000
SECOND = 1_000_000
FS = 250.0
CHANNELS = ("a", "b", "c", "d")
ON = 64  # open_files is off by default; the tests that need kept files ask for them


def _quiet(fn, *a, **kw):
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        return fn(*a, **kw)


def _write(path, segments=3, start=START, seed=7, channels=CHANNELS):
    rng = np.random.default_rng(seed)
    with mef3io.Writer(str(path), overwrite=True) as w:
        for s in range(segments):
            for ch in channels:
                x = rng.integers(-500, 500, 1000).astype(np.float64)
                w.write(ch, x, start + s * 10 * SECOND, FS, precision=0, new_segment=s > 0)
    return Path(path)


def _tdat(path, ch, n):
    stem = f"{ch}-{n:06d}"
    return Path(path) / f"{ch}.timd" / f"{stem}.segd" / f"{stem}.tdat"


def _battery(r, start=START):
    """Windows inside one segment, across two, over everything, and a repeat."""
    ch = list(r.channels)
    wins = [(start + 1 * SECOND, start + 3 * SECOND),
            (start + 12 * SECOND, start + 14 * SECOND),
            (start + 3 * SECOND, start + 12 * SECOND),
            (start, start + 40 * SECOND),
            (start + 1 * SECOND, start + 3 * SECOND)]
    out = []
    for t0, t1 in wins:
        out.append(r.read(ch, t0, t1))
        out.append(r.read(ch[0], t0, t1))
        out.append(r.read(ch[:2], t0, t1, dtype="float32"))
    return out


def _same(a, b):
    assert len(a) == len(b)
    for x, y in zip(a, b):
        assert x.dtype == y.dtype and x.shape == y.shape
        assert x.tobytes() == y.tobytes()


def _fds():
    p = Path("/proc/self/fd")
    return len(os.listdir(p)) if p.exists() else None


def _kept(r):
    return r._impl.open_file_count()


# --------------------------------------------------------------------------
# the option
# --------------------------------------------------------------------------

def test_default_reader_holds_no_descriptor_after_a_read(tmp_path):
    s = _write(tmp_path / "s.mefd")
    with mef3io.Reader(str(s)) as r:
        base = _fds()
        r.read(list(CHANNELS), START, START + 25 * SECOND)
        r.block_envelope("a")
        assert _kept(r) == 0
        if base is not None:
            assert _fds() == base


def test_default_and_setting(tmp_path):
    s = _write(tmp_path / "s.mefd")
    assert mef3io._mef3io.default_open_files() == 0
    with mef3io.Reader(str(s)) as r:
        assert r.open_files == 0  # off unless asked for (as in 2.3.0)
        r.read("a", START, START + SECOND)
        assert _kept(r) == 0
        r.open_files = 5
        assert r.open_files == 5
        r.read("a", START, START + SECOND)
        assert _kept(r) == 1
        r.open_files = 0
        assert r.open_files == 0 and _kept(r) == 0
    with mef3io.Reader(str(s), open_files=3) as r:
        assert r.open_files == 3
    with mef3io.Reader(str(s), open_files=0) as r:
        r.read("a", START, START + SECOND)
        assert _kept(r) == 0


def test_argument_rules(tmp_path):
    s = _write(tmp_path / "s.mefd")
    for bad, exc in ((-1, ValueError), (True, TypeError), (1.5, TypeError), ("3", TypeError)):
        with pytest.raises(exc):
            mef3io.Reader(str(s), open_files=bad)
        with mef3io.Reader(str(s), open_files=ON) as r:
            with pytest.raises(exc):
                r.open_files = bad
    with mef3io.Reader(str(s), open_files=ON) as r:
        with pytest.raises(TypeError):
            r.open_files = None  # the default is for the constructor only
        with pytest.raises(TypeError):
            mef3io.MultiReader([str(s)], open_files=True)


def test_a_warm_reader_applies_the_value_when_its_backend_is_built(tmp_path):
    s = _write(tmp_path / "s.mefd")
    cache = tmp_path / "c.json"
    mef3io.Reader(str(s), cache=str(cache)).close()
    with mef3io.Reader(str(s), cache=str(cache), open_files=2) as r:
        assert r._impl is None  # warm: no backend yet
        assert r.open_files == 2
        r.read(list(CHANNELS), START, START + SECOND)
        assert r._impl.open_files() == 2 and _kept(r) == 2


# --------------------------------------------------------------------------
# results do not depend on it
# --------------------------------------------------------------------------

@pytest.mark.parametrize("threads", [1, 2, 8])
@pytest.mark.parametrize("cap", [1, 2, 3, 500, ON])
@pytest.mark.parametrize("which", ["dir", "tar"])
def test_results_are_identical_with_the_cache_off_tiny_and_default(tmp_path, which, cap, threads):
    s = _write(tmp_path / "s.mefd")
    path = str(s) if which == "dir" else mef3io.archive_session(str(s))
    with mef3io.Reader(str(s), open_files=0, n_threads=1) as off:
        want = _battery(off)
    with mef3io.Reader(str(path), open_files=cap, n_threads=threads) as r:
        _same(_battery(r), want)
        if cap is not None:
            assert _kept(r) <= cap


def test_read_raw_and_envelope_are_identical(tmp_path):
    s = _write(tmp_path / "s.mefd")
    with mef3io.Reader(str(s), open_files=0) as off, mef3io.Reader(str(s), open_files=2) as r:
        for ch in CHANNELS:
            a = off.read_raw(ch, START, START + 40 * SECOND)
            b = r.read_raw(ch, START, START + 40 * SECOND)
            assert a["samples"].tobytes() == b["samples"].tobytes()
        e0 = off.block_envelope("a", START, START + 40 * SECOND)
        e1 = r.block_envelope("a", START, START + 40 * SECOND)
        assert e0.keys() == e1.keys()
        for k in e0:
            assert np.array_equal(np.asarray(e0[k]), np.asarray(e1[k]), equal_nan=True)


# --------------------------------------------------------------------------
# the cap, release, close
# --------------------------------------------------------------------------

def test_the_cap_is_never_exceeded_and_close_lets_go(tmp_path):
    s = _write(tmp_path / "s.mefd")
    before = _fds()
    r = mef3io.Reader(str(s), open_files=3, n_threads=4)
    base = _fds()
    for seg in range(3):
        for ch in CHANNELS:
            r.read(ch, START + seg * 10 * SECOND + SECOND, START + seg * 10 * SECOND + 3 * SECOND)
            assert _kept(r) <= 3
            if base is not None:
                assert _fds() <= base + 3
    assert _kept(r) == 3
    r.close()
    if before is not None:
        assert _fds() <= before  # nothing the Reader opened is left


def test_release_files_closes_everything_and_the_reader_goes_on(tmp_path):
    s = _write(tmp_path / "s.mefd")
    with mef3io.Reader(str(s), open_files=ON) as r:
        first = r.read(list(CHANNELS), START, START + 25 * SECOND)
        assert _kept(r) >= len(CHANNELS)
        base = _fds()
        r.release_files()
        assert _kept(r) == 0
        if base is not None:
            assert _fds() <= base - len(CHANNELS)
        again = r.read(list(CHANNELS), START, START + 25 * SECOND)
        assert again.tobytes() == first.tobytes()
        assert _kept(r) >= len(CHANNELS)
    r.release_files()  # closed: harmless


def test_release_before_the_backend_exists_is_harmless(tmp_path):
    s = _write(tmp_path / "s.mefd")
    cache = tmp_path / "c.json"
    mef3io.Reader(str(s), cache=str(cache)).close()
    with mef3io.Reader(str(s), cache=str(cache), open_files=ON) as r:
        r.release_files()


def test_threads_sharing_one_reader_with_a_tiny_cap(tmp_path):
    s = _write(tmp_path / "s.mefd")
    with mef3io.Reader(str(s), open_files=0) as off:
        want = _battery(off)
    errors = []
    with mef3io.Reader(str(s), open_files=2, n_threads=4) as r:
        def work():
            try:
                for _ in range(3):
                    _same(_battery(r), want)
            except BaseException as e:  # noqa: BLE001
                errors.append(e)

        ts = [threading.Thread(target=work) for _ in range(6)]
        [t.start() for t in ts]
        [t.join() for t in ts]
        assert _kept(r) <= 2
    assert not errors, errors[0]


# --------------------------------------------------------------------------
# a kept handle never blocks, and never serves a file that is gone
# --------------------------------------------------------------------------

def test_a_kept_handle_does_not_block_a_writer_appending_to_the_session(tmp_path):
    s = _write(tmp_path / "s.mefd", segments=1)
    with mef3io.Reader(str(s), open_files=ON) as r:
        r.read(list(CHANNELS), START, START + 2 * SECOND)
        assert _kept(r) == len(CHANNELS)
        # The Writer opens the same .tdat files for writing while we hold them
        # (Windows: our handles share read, write and delete).
        with mef3io.Writer(str(s), overwrite=False) as w:
            for ch in CHANNELS:
                w.write(ch, np.arange(1000, dtype=np.float64), START + 4 * SECOND, FS, precision=0)
            # annotations go into record files; also fine with the data files held.
            w.write_annotations([{"time": START + SECOND, "text": "n"}])
    with mef3io.Reader(str(s), open_files=ON) as fresh:
        assert fresh.info("a")["number_of_samples"] == 2000


def test_a_kept_handle_does_not_block_a_rename_or_delete_of_the_file(tmp_path):
    s = _write(tmp_path / "s.mefd", segments=2)
    other = _write(tmp_path / "o.mefd", segments=2, seed=99)
    with mef3io.Reader(str(s), open_files=ON) as r:
        before = r.read("a", START + SECOND, START + 3 * SECOND)
        assert _kept(r) == 1
        # Replace a's segment-0 data with another recording's by renaming a copy
        # over it (what a sync tool does), and delete b's: neither is blocked.
        replaced = sys.platform != "win32"  # Windows refuses to replace a file anyone holds open
        if replaced:
            new = Path(str(_tdat(s, "a", 0)) + ".new")
            new.write_bytes(_tdat(other, "a", 0).read_bytes())
            os.replace(new, _tdat(s, "a", 0))
        os.remove(_tdat(s, "b", 0))
        # The index in memory still describes the old file; the point here is
        # that the old handle is not what answers: the new bytes are read (and
        # the missing file raises), never the file the handle was opened on.
        try:
            after = r.read("a", START + SECOND, START + 3 * SECOND)
        except Exception:  # the old index may not fit the new bytes: also not stale data
            after = None
        if replaced and after is not None:
            assert after.tobytes() != before.tobytes()
        with pytest.raises(Exception):
            r.read("b", START + SECOND, START + 3 * SECOND)


@pytest.mark.skipif(sys.platform == "win32", reason="Windows cannot rename a folder with files open")
def test_a_renamed_session_fails_like_a_reader_that_keeps_nothing_open(tmp_path):
    s = _write(tmp_path / "s.mefd")
    moved = tmp_path / "moved.mefd"

    def failure(r):
        with pytest.raises(Exception) as e:
            r.read("a", START + SECOND, START + 3 * SECOND)
        return type(e.value), str(e.value)

    with mef3io.Reader(str(s), open_files=ON) as kept, mef3io.Reader(str(s), open_files=0) as none:
        good = kept.read("a", START + SECOND, START + 3 * SECOND)
        none.read("a", START + SECOND, START + 3 * SECOND)
        os.rename(s, moved)
        assert failure(kept) == failure(none)
        assert _kept(kept) == 0
        os.rename(moved, s)
        assert kept.read("a", START + SECOND, START + 3 * SECOND).tobytes() == good.tobytes()


def test_release_lets_a_folder_be_renamed_even_on_windows(tmp_path):  # (Windows: only after release)
    s = _write(tmp_path / "s.mefd")
    with mef3io.Reader(str(s), open_files=ON) as r:
        r.read(list(CHANNELS), START, START + 25 * SECOND)
        r.release_files()
        moved = tmp_path / "moved.mefd"
        os.rename(s, moved)
        os.rename(moved, s)
        assert r.read("a", START, START + SECOND).shape[0] > 0


@pytest.mark.parametrize("threads", [1, 4])
def test_a_file_deleted_and_recreated_under_the_same_name_is_never_read_through_the_old_handle(
        tmp_path, threads):
    """Same size, other content, same name: the identity rule must see a new file.

    (Windows' NTFS gives a recreated file back its old creation time within ~15 s,
    so the rule there also compares the last-write time and size; where the OS keeps
    the name reserved for a deleted file that is still held, the recreate cannot
    happen and there is nothing to check.)"""
    rng = np.random.default_rng(1)
    base = rng.integers(-500, 500, 1000)
    a = tmp_path / "a.mefd"
    b = tmp_path / "b.mefd"
    with mef3io.Writer(str(a), overwrite=True) as w:
        w.write("a", base.astype(np.float64), START, FS, precision=0)
    old, new = _tdat(a, "a", 0), _tdat(b, "a", 0)
    for offset in range(1, 40):  # another recording of the same size (RED sizes follow the values)
        with mef3io.Writer(str(b), overwrite=True) as w:
            w.write("a", (base + offset).astype(np.float64), START, FS, precision=0)
        if old.stat().st_size == new.stat().st_size:
            break
    assert old.stat().st_size == new.stat().st_size
    with mef3io.Reader(str(a), open_files=ON, n_threads=threads) as r:
        before = r.read("a", START, START + 4 * SECOND)
        assert _kept(r) == 1
        os.remove(old)
        try:
            old.write_bytes(new.read_bytes())
        except OSError:
            pytest.skip("this OS keeps the name of a deleted file that is still held")
        after = r.read("a", START, START + 4 * SECOND)
        assert after.tobytes() != before.tobytes()
        with mef3io.Reader(str(a), open_files=0) as fresh:
            assert after.tobytes() == fresh.read("a", START, START + 4 * SECOND).tobytes()


_EXHAUST = """
import resource, sys, warnings
warnings.simplefilter("ignore")
import mef3io
path, limit = sys.argv[1], int(sys.argv[2])
resource.setrlimit(resource.RLIMIT_NOFILE, (limit, limit))
readers = [mef3io.Reader(path, open_files=64, n_threads=1) for _ in range(6)]
chs = readers[0].channels
want = None
for rnd in range(2):
    for r in readers:
        got = r.read(chs, %d, %d)
        if want is None:
            want = got.tobytes()
        assert got.tobytes() == want
open(path + ".ok", "w").write("ok")
"""


@pytest.mark.skipif(sys.platform == "win32", reason="needs resource.setrlimit")
def test_running_out_of_descriptors_heals(tmp_path):
    import subprocess

    s = _write(tmp_path / "s.mefd", segments=2, channels=tuple(f"c{i:02d}" for i in range(32)))
    script = tmp_path / "exhaust.py"
    script.write_text(_EXHAUST % (START, START + 40 * SECOND))
    # 6 Readers x 64 files would be 384 descriptors: far over the limit of 120,
    # which each Reader runs into in turn and recovers from by letting go.
    env = dict(os.environ)
    out = subprocess.run([sys.executable, str(script), str(s), "120"], capture_output=True, text=True,
                         env=env, timeout=300)
    assert out.returncode == 0, out.stderr[-2000:]
    assert Path(str(s) + ".ok").exists()


# --------------------------------------------------------------------------
# MultiReader: one budget
# --------------------------------------------------------------------------

def _recordings(tmp_path, n=4):
    return [str(_write(tmp_path / f"r{i}.mefd", segments=2, start=START + i * 100 * SECOND, seed=i))
            for i in range(n)]


def test_multireader_open_files_is_a_total(tmp_path):
    paths = _recordings(tmp_path)
    with mef3io.MultiReader(paths, open_files=0) as off:
        wins = [(START + i * 100 * SECOND + SECOND, START + i * 100 * SECOND + 4 * SECOND)
                for i in range(4)] + [(START, START + 400 * SECOND)]
        want = [off.read(list(CHANNELS), a, b) for a, b in wins]
    with mef3io.MultiReader(paths, open_files=6, n_threads=4) as m:
        assert m.open_files == 6
        for (a, b), w in zip(wins, want):
            assert m.read(list(CHANNELS), a, b).tobytes() == w.tobytes()
            assert sum(_kept(r) for r in m._readers) <= 6
        # One recording in use gets the whole budget, the others let go.
        m.read(list(CHANNELS), *wins[1])
        assert [_kept(r) for r in m._readers].count(0) >= 3
        m.open_files = 2
        assert sum(_kept(r) for r in m._readers) <= 2
        m.release_files()
        assert sum(_kept(r) for r in m._readers) == 0


def test_multireader_default_is_off(tmp_path):
    paths = _recordings(tmp_path, 3)
    with mef3io.MultiReader(paths) as m:
        assert m.open_files == 0
        m.read(list(CHANNELS), START, START + 5 * SECOND)
        assert sum(_kept(r) for r in m._readers) == 0


def test_a_missing_file_does_not_release_other_readers_files(tmp_path):
    s1 = _write(tmp_path / "one.mefd")
    s2 = _write(tmp_path / "two.mefd")
    with mef3io.Reader(str(s1), open_files=ON) as other, mef3io.Reader(str(s2), open_files=ON) as r:
        other.read(list(CHANNELS), START, START + 25 * SECOND)
        held = _kept(other)
        assert held >= len(CHANNELS)
        r.read("a", START, START + 2 * SECOND)
        os.remove(_tdat(s2, "b", 0))
        with pytest.raises(Exception):
            r.read("b", START, START + 2 * SECOND)
        assert _kept(other) == held
