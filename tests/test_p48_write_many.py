"""P48: multi-channel write (#165) and Writer(flush="close") (#166).

write_int32_many / write_many are a performance API: the result is the same
as a loop of per-channel calls (compared here by reading the sessions back,
and by file bytes except the random UUIDs in the C++ suite). No oracle needed.
"""
from __future__ import annotations

import os

import numpy as np
import pytest

import mef3io

START = 1_600_000_000_000_000
FS = 1000.0


def _walk(rng, shape):
    return np.cumsum(rng.integers(-50, 50, size=shape), axis=-1).astype(np.int32)


def _masked_files(root):
    """{relative path: bytes} with the random UUIDs (and the CRC over them) zeroed."""
    out = {}
    for d, _, files in os.walk(root):
        for f in files:
            p = os.path.join(d, f)
            b = bytearray(open(p, "rb").read())
            if f.endswith((".tdat", ".tidx", ".tmet")):
                b[0:8] = bytes(8)
                b[820:868] = bytes(48)
            out[os.path.relpath(p, root)] = bytes(b)
    return out


@pytest.mark.parametrize("n_threads", [1, 4])
def test_int32_many_is_the_loop(tmp_path, n_threads):
    rng = np.random.default_rng(1)
    chs = [f"c{i}" for i in range(6)]
    (tmp_path / "a").mkdir()
    (tmp_path / "b").mkdir()
    ufact = [0.1 * (i + 1) for i in range(6)]
    with mef3io.Writer(str(tmp_path / "a" / "s.mefd"), overwrite=True, n_threads=n_threads) as wa, \
            mef3io.Writer(str(tmp_path / "b" / "s.mefd"), overwrite=True, n_threads=n_threads) as wb:
        for r in range(3):
            d = _walk(rng, (6, 20_000))
            st = START + r * 20_000_000
            for i, c in enumerate(chs):
                wa.write_int32(c, d[i], ufact[i], st, FS)
            sums = wb.write_int32_many(chs, d, ufact, st, FS)
            assert [s["samples_written"] for s in sums] == [20_000] * 6
    assert _masked_files(tmp_path / "a" / "s.mefd") == _masked_files(tmp_path / "b" / "s.mefd")
    with mef3io.Reader(str(tmp_path / "b" / "s.mefd")) as rd:
        x = rd.read("c2", START, START + 1_000_000)
        assert x.shape == (1000,)


def test_float_many_equals_write_per_channel_with_nan_gaps(tmp_path):
    rng = np.random.default_rng(2)
    chs = ["a", "b", "c"]
    d = rng.standard_normal((3, 30_000)) * np.array([[0.01], [1.0], [100.0]])
    d[:, 5000:5400] = np.nan
    d[1, 20000:20010] = np.nan
    (tmp_path / "a").mkdir()
    (tmp_path / "b").mkdir()
    with mef3io.Writer(str(tmp_path / "a" / "s.mefd"), overwrite=True) as wa, \
            mef3io.Writer(str(tmp_path / "b" / "s.mefd"), overwrite=True, n_threads=3) as wb:
        for r in range(2):
            st = START + r * 30_000_000
            for i, c in enumerate(chs):
                wa.write(c, d[i], st, FS)
            sums = wb.write_many(chs, d, st, FS)
            assert sums[0]["gaps_skipped"] >= 1
    assert _masked_files(tmp_path / "a" / "s.mefd") == _masked_files(tmp_path / "b" / "s.mefd")
    with mef3io.Reader(str(tmp_path / "b" / "s.mefd")) as rd:
        got = rd.read("a", START, START + 30_000_000)
        assert np.isnan(got[5000:5400]).all()
        assert np.allclose(got[:5000], d[0, :5000], atol=1e-2)


def test_validation_writes_nothing(tmp_path):
    path = tmp_path / "s.mefd"
    d = np.zeros((2, 100), dtype=np.int32)
    with mef3io.Writer(str(path), overwrite=True) as w:
        with pytest.raises(ValueError, match="more than once"):
            w.write_int32_many(["x", "x"], d, 1.0, START, FS)
        with pytest.raises(ValueError, match="shape"):
            w.write_int32_many(["x", "y", "z"], d, 1.0, START, FS)
        with pytest.raises(ValueError, match="ufact"):
            w.write_int32_many(["x", "y"], d, [1.0, 1.0, 1.0], START, FS)
        with pytest.raises(TypeError):
            w.write_int32_many(["x", "y"], d.astype(float), 1.0, START, FS)
        with pytest.raises(ValueError):
            w.write_many(["x", "y"], np.zeros((3, 10)), START, FS)
        with pytest.raises(TypeError):
            w.write_int32_many("xy", d, 1.0, START, FS)
        assert w.write_int32_many([], np.zeros((0, 5), dtype=np.int32), 1.0, START, FS) == []
    assert not list(path.glob("*.timd"))


def test_append_conflict_rolls_every_channel_back(tmp_path):
    path = tmp_path / "s.mefd"
    d = np.ones((3, 5000), dtype=np.int32)
    chs = ["c0", "c1", "c2"]
    with mef3io.Writer(str(path), overwrite=True, n_threads=3) as w:
        w.write_int32_many(chs, d, 1.0, START, FS)
        w.write_int32("c2", d[2], 1.0, START + 5_000_000, FS)  # c2 is ahead
        w.flush()
        before = _masked_files(path)
        with pytest.raises(mef3io.WriteConflictError):
            w.write_int32_many(chs, d, 1.0, START + 5_000_000, FS)
        assert _masked_files(path) == before
        w.write_int32_many(["c0", "c1"], d[:2], 1.0, START + 5_000_000, FS)  # still usable
    assert issubclass(mef3io.PartialWriteError, mef3io.IoError)


def test_append_stays_o_new_data(tmp_path):
    """An append reads the fast path: file growth is the new data only."""
    path = tmp_path / "s.mefd"
    d = _walk(np.random.default_rng(3), (4, 10_000))
    chs = [f"c{i}" for i in range(4)]
    sizes = []
    with mef3io.Writer(str(path), overwrite=True) as w:
        for r in range(4):
            w.write_int32_many(chs, d, 1.0, START + r * 10_000_000, FS)
            w.flush()
            sizes.append(sum(os.path.getsize(os.path.join(dd, f))
                             for dd, _, fs in os.walk(path) for f in fs if f.endswith(".tdat")))
    grow = np.diff(sizes)
    assert grow.max() < grow.min() * 1.5  # linear, no rewrite of the whole tdat


def test_flush_close_option(tmp_path):
    d = _walk(np.random.default_rng(4), (3, 5000))
    chs = ["a", "b", "c"]
    with pytest.raises(ValueError, match="wait_for_disk"):
        mef3io.Writer(str(tmp_path / "x.mefd"), overwrite=True, flush="close", wait_for_disk=True)
    with pytest.raises(ValueError, match="flush"):
        mef3io.Writer(str(tmp_path / "x.mefd"), overwrite=True, flush="never")
    assert not (tmp_path / "x.mefd").exists()  # refused before anything is created
    with mef3io.Writer(str(tmp_path / "s.mefd"), overwrite=True, flush="close") as w:
        w.write_int32_many(chs, d, 1.0, START, FS)
        w.write_int32_many(chs, d, 1.0, START + 5_000_000, FS)
    with mef3io.Reader(str(tmp_path / "s.mefd")) as rd:
        assert rd.read("b", START, START + 10_000_000).shape == (10_000,)


def test_channel_names_must_be_str(tmp_path):
    with mef3io.Writer(str(tmp_path / "s.mefd"), overwrite=True) as w:
        with pytest.raises(TypeError, match="str"):
            w.write_int32_many([1, 2], np.zeros((2, 10), dtype=np.int32), 1.0, START, FS)
        with pytest.raises(TypeError, match="str"):
            w.write_many([b"a", b"b"], np.zeros((2, 10)), START, FS)


def test_flush_close_option(tmp_path):
    d = _walk(np.random.default_rng(4), (3, 5000))
    chs = ["a", "b", "c"]
    with pytest.raises(ValueError, match="wait_for_disk"):
        mef3io.Writer(str(tmp_path / "x.mefd"), overwrite=True, flush="close", wait_for_disk=True)
    with pytest.raises(ValueError, match="flush"):
        mef3io.Writer(str(tmp_path / "x.mefd"), overwrite=True, flush="never")
    assert not (tmp_path / "x.mefd").exists()  # refused before anything is created
    with mef3io.Writer(str(tmp_path / "s.mefd"), overwrite=True, flush="close") as w:
        w.write_int32_many(chs, d, 1.0, START, FS)
        w.write_int32_many(chs, d, 1.0, START + 5_000_000, FS)
    with mef3io.Reader(str(tmp_path / "s.mefd")) as rd:
        assert rd.read("b", START, START + 10_000_000).shape == (10_000,)


def test_flush_close_refuses_an_existing_session(tmp_path):
    path = str(tmp_path / "s.mefd")
    with mef3io.Writer(path, overwrite=True) as w:
        w.write_int32("a", np.arange(1000, dtype=np.int32), 1.0, START, FS)
    before = _masked_files(path)
    with pytest.raises(ValueError, match="this Writer creates"):
        mef3io.Writer(path, flush="close")
    assert _masked_files(path) == before
    # overwrite=True creates it anew: allowed
    mef3io.Writer(path, overwrite=True, flush="close").close()


def test_append_conflict_touches_no_file(tmp_path):
    import time

    path = tmp_path / "s.mefd"
    d = np.ones((3, 5000), dtype=np.int32)
    chs = ["c0", "c1", "c2"]
    with mef3io.Writer(str(path), overwrite=True, n_threads=3) as w:
        w.write_int32_many(chs, d, 1.0, START, FS)
        w.flush()
        stamp = {p: (p.stat().st_size, p.stat().st_mtime_ns) for p in path.rglob("*") if p.is_file()}
        time.sleep(0.03)
        with pytest.raises(mef3io.WriteConflictError):
            w.write_int32_many(chs, d, [1.0, 1.0, 2.0], START + 5_000_000, FS)  # c2: other factor
        with pytest.raises(mef3io.WriteConflictError):
            w.write_int32_many(chs, d, 1.0, START + 1_000_000, FS)  # starts before the end
        assert stamp == {p: (p.stat().st_size, p.stat().st_mtime_ns)
                         for p in path.rglob("*") if p.is_file()}


_PEAK = r"""
import sys, resource, numpy as np
sys.path.insert(0, sys.argv[1])
import mef3io
mode, out = sys.argv[2], sys.argv[3]
rng = np.random.default_rng(0)
nch, n = 16, 1_000_000
d = rng.integers(-3000, 3000, size=(nch, n), dtype=np.int32)  # no big temporaries: maxrss is a high-water mark
chs = [f"c{i}" for i in range(nch)]
base = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
w = mef3io.Writer(out, overwrite=True, n_threads=2)
if mode == "many":
    w.write_int32_many(chs, d, 1.0, 1_600_000_000_000_000, 1000.0)
else:
    for i, c in enumerate(chs):
        w.write_int32(c, d[i], 1.0, 1_600_000_000_000_000, 1000.0)
w.close()
print((resource.getrusage(resource.RUSAGE_SELF).ru_maxrss - base) * 1024)
"""


def test_peak_memory_stays_near_the_loop(tmp_path):
    """The multi-channel call frees each channel's block copies once it is
    written: peak growth is that of ~n_threads channels, not of all of them."""
    import subprocess
    import sys

    root = os.path.dirname(os.path.dirname(os.path.abspath(mef3io.__file__)))
    grow = {}
    for mode in ("loop", "many"):
        r = subprocess.run([sys.executable, "-c", _PEAK, root, mode, str(tmp_path / f"{mode}.mefd")],
                           capture_output=True, text=True, check=True)
        grow[mode] = int(r.stdout.strip())
    one_channel = 4 * 1_000_000
    all_channels = 16 * one_channel
    # far below a copy of everything, and not much above the loop
    assert grow["many"] < all_channels * 0.6, grow
    assert grow["many"] < grow["loop"] + 6 * one_channel, grow
