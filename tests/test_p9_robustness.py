"""P9: malformed sessions must raise Python exceptions — never crash the
interpreter. Models what flaky network mounts produce: truncated index files,
corrupted block headers."""
import os
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


def _make_session(tmp_path):
    path = str(tmp_path / "s.mefd")
    w = m.SessionWriter(path, True)
    w.write_float("ch1", np.ascontiguousarray(np.sin(np.arange(5000) / 20)), START, FS, 3)
    del w
    return path


def _seg_file(path, ext):
    return Path(path) / "ch1.timd" / "ch1-000000.segd" / f"ch1-000000.{ext}"


def test_truncated_tidx_raises(tmp_path):
    path = _make_session(tmp_path)
    with open(_seg_file(path, "tidx"), "r+b") as f:
        f.truncate(500)  # smaller than the universal header

    r = mef3io.Reader(path)  # open stays lazy and must succeed
    with pytest.raises(RuntimeError, match="truncated"):
        r.read("ch1", START, START + int(1e6))  # the windowed (compat) path
    with pytest.raises(RuntimeError, match="truncated"):
        r.toc("ch1")


def test_tidx_not_multiple_of_entry_raises(tmp_path):
    path = _make_session(tmp_path)
    size = os.path.getsize(_seg_file(path, "tidx"))
    with open(_seg_file(path, "tidx"), "r+b") as f:
        f.truncate(size - 13)  # mid-entry cut

    with pytest.raises(RuntimeError, match="truncated"):
        mef3io.Reader(path).read("ch1")


def test_corrupt_tmet_body_raises(tmp_path):
    # Section 2 holds fs/ufact: undetected corruption there would silently
    # mis-scale every sample. The body CRC must catch it at open.
    path = _make_session(tmp_path)
    with open(_seg_file(path, "tmet"), "r+b") as f:
        f.seek(1024 + 1536 + 6160)  # section-2 sampling_frequency field
        f.write(b"\xde\xad\xbe\xef")

    with pytest.raises(RuntimeError, match="body CRC"):
        mef3io.Reader(path)


def test_corrupt_red_counters_raise(tmp_path):
    path = _make_session(tmp_path)
    tdat = _seg_file(path, "tdat")
    with open(tdat, "r+b") as f:
        f.seek(1024 + 28)  # first block's difference_bytes field
        f.write((0xFFFFFFFF).to_bytes(4, "little"))

    # Either the block CRC or the counter sanity check must reject it.
    with pytest.raises(RuntimeError):
        mef3io.Reader(path).read("ch1")


def test_float_and_numpy_timestamps(tmp_path):
    # Regression: float t0/t1 used to be nb::cast inside a GIL-released block
    # -> SIGSEGV. Ints, floats, and numpy scalars must all work and agree.
    path = _make_session(tmp_path)
    t0, t1 = START, START + 2_000_000

    with mef3io.Reader(path) as r:
        ref = r.read("ch1", t0, t1)
        for a, b in [
            (float(t0), float(t1)),
            (t0, t0 + 2 * 1e6),                      # the classic t + 10*1e6 shape
            (np.int64(t0), np.int64(t1)),
            (np.float64(t0), np.float64(t1)),
            (np.int64(t0), float(t1)),
        ]:
            got = r.read("ch1", a, b)
            assert np.array_equal(got, ref, equal_nan=True), (type(a), type(b))
        raw = r.read_raw("ch1", float(t0), float(t1))
        assert len(raw["samples"]) == len(ref)
        with pytest.raises(TypeError):
            r.read("ch1", "not a time", t1)

    # the legacy compat path takes the same types
    from mef3io import MefReader

    lr = MefReader(path)
    got = np.asarray(lr.get_data("ch1", float(t0), t0 + 2 * 1e6), float)
    assert np.array_equal(got, ref, equal_nan=True)


def test_truncated_tdat_raises(tmp_path):
    path = _make_session(tmp_path)
    with open(_seg_file(path, "tdat"), "r+b") as f:
        f.truncate(1500)  # index now points past the end

    with pytest.raises(RuntimeError, match="tdat"):
        mef3io.Reader(path).read("ch1")


def test_write_accepts_any_numeric_input(tmp_path):
    # Lists, integer arrays, float32, scalars — everything numeric is coerced.
    path = str(tmp_path / "s.mefd")
    with mef3io.Writer(path, overwrite=True) as w:
        w.write("lst", [1.5, 2.5, float("nan"), 4.0], START, FS, precision=1)
        w.write("ints", np.arange(100, dtype=np.int16), START, FS, precision=0)
        w.write("f32", np.linspace(0, 1, 100, dtype=np.float32), START, FS, precision=3)
        w.write("scalar", 7.25, START, FS, precision=2)
        w.write("f16", np.linspace(0, 1, 50, dtype=np.float16), START, FS, precision=2)

    with mef3io.Reader(path) as r:
        y = r.read("lst")
        assert np.isnan(y[2]) and y[0] == 1.5
        assert np.allclose(r.read("ints"), np.arange(100))
        assert len(r.read("scalar")) == 1 and r.read("scalar")[0] == 7.25


def test_write_int32_strictness_and_safety(tmp_path):
    path = str(tmp_path / "s.mefd")
    with mef3io.Writer(path, overwrite=True) as w:
        # the primitive path never alters data: floats must be rejected ...
        with pytest.raises(TypeError, match="integer data"):
            w.write_int32("ch1", [10.6, 11.4, 13.0], 0.5, START, FS)
        with pytest.raises(TypeError, match="integer data"):
            w.write_int32("ch1", np.zeros(4, dtype=np.float32), 0.5, START, FS)
        # ... as must out-of-range integers (no silent wrap)
        with pytest.raises(ValueError, match="int32 range"):
            w.write_int32("ch1", np.array([2**31], dtype=np.int64), 1.0, START, FS)
        # any integer dtype in range is fine (lists give int64)
        w.write_int32("ch1", [10, 11, 13], 0.5, START, FS)
        w.write_int32("ch2", np.array([2**31 - 1, -(2**31)], dtype=np.int64), 1.0,
                      START, FS)
        w.write_int32("ch3", np.arange(4, dtype=np.int16), 1.0, START, FS,
                      valid=[1.0, 0.0, 1.0, 1.0])  # numeric mask: nonzero = valid

    with mef3io.Reader(path) as r:
        assert list(r.read_raw("ch1")["samples"]) == [10, 11, 13]
        assert list(r.read_raw("ch2")["samples"]) == [2**31 - 1, -(2**31)]
        assert not r.read_raw("ch3")["valid"][1]


def test_empty_and_inverted_windows(tmp_path):
    path = _make_session(tmp_path)
    with mef3io.Reader(path) as r:
        assert len(r.read("ch1", START, START)) == 0            # empty window
        assert len(r.read("ch1", START + 1_000_000, START)) == 0  # inverted
        raw = r.read_raw("ch1", START, START)
        assert len(raw["samples"]) == 0 and len(raw["valid"]) == 0


def test_annotation_time_types(tmp_path):
    path = str(tmp_path / "s.mefd")
    with mef3io.Writer(path, overwrite=True) as w:
        w.write("ch1", np.zeros(100), START, FS, precision=0)
        w.write_annotations([
            {"time": np.float64(START + 1e6), "text": "np float time"},
            {"time": float(START + 2e6), "type": "EDFA", "text": "d",
             "duration": np.float64(5e5)},
            {"time": START + 3_000_000, "text": None, "duration": None},
        ], "ch1")
    with mef3io.Reader(path) as r:
        recs = r.records("ch1")
    assert recs[0]["time"] == START + 1_000_000
    assert recs[1]["duration"] == 500_000
    assert recs[2]["time"] == START + 3_000_000
