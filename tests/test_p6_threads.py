"""P6 gate: parallel decode/encode is deterministic (results independent of
thread count) and still cross-compatible with pymef."""
import glob
import warnings

import numpy as np
import pytest

from mef3io import _mef3io as m

warnings.filterwarnings("ignore")
START = 1577836800000000
FS = 1000.0


def _signal(n=200000):
    return 200 * np.sin(np.arange(n) / 50) + np.random.default_rng(0).normal(0, 5, n)


def _write(path, threads, data):
    w = m.SessionWriter(path, True)
    w.set_threads(threads)
    w.write_float("ch1", np.ascontiguousarray(data), START, FS, 2)
    del w


def _tdat_blocks(path):
    return open(glob.glob(path + "/ch1.timd/*/*.tdat")[0], "rb").read()[1024:]


def _tidx_body(path):
    return open(glob.glob(path + "/ch1.timd/*/*.tidx")[0], "rb").read()[1024:]


@pytest.mark.parametrize("threads", [1, 2, 4, 8])
def test_encode_byte_identical_across_threads(tmp_path, threads):
    data = _signal()
    ref = str(tmp_path / "ref.mefd")
    _write(ref, 1, data)
    other = str(tmp_path / f"t{threads}.mefd")
    _write(other, threads, data)
    # RED block payloads and index entries must be identical regardless of
    # thread count (only the random per-file UUIDs in the header differ).
    assert _tdat_blocks(other) == _tdat_blocks(ref)
    assert _tidx_body(other) == _tidx_body(ref)


def test_decode_deterministic_across_threads(tmp_path):
    data = _signal()
    path = str(tmp_path / "s.mefd")
    _write(path, 0, data)
    r = m.Reader(path, "")
    base = np.asarray(r.read_raw("ch1", n_threads=1)["samples"])
    for threads in (2, 4, 8, 0):
        got = np.asarray(r.read_raw("ch1", n_threads=threads)["samples"])
        assert np.array_equal(got, base)


def test_threaded_write_readable_by_pymef(tmp_path):
    pytest.importorskip("pymef")
    from pymef.mef_session import MefSession

    data = _signal(50000)
    path = str(tmp_path / "s.mefd")
    _write(path, 8, data)
    ms = MefSession(path, None, True)
    try:
        got = np.asarray(ms.read_ts_channels_sample(["ch1"], [0, len(data)])[0]).astype(np.int64)
        assert np.array_equal(got, np.round(data * 100).astype(np.int64))
    finally:
        ms.close()
