"""P5 gate: the high-level SessionWriter (float precision inference, int32+ufact
primitive path, NaN discontinuity splitting, encryption) produces sessions that
the legacy mef_tools reader decodes back to the original values."""
import shutil
import sys
import warnings
from pathlib import Path

import numpy as np
import pytest

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO))
warnings.filterwarnings("ignore")

from mef3io import _mef3io as m  # noqa: E402

pytest.importorskip("mef_tools", reason="legacy oracle not installed (pip install mef3io[test])")
from mef_tools.io import MefReader  # noqa: E402

START = 1577836800000000
FS = 256.0


def _write_float(tmp_path, data, precision=-1, p1="", p2=""):
    path = str(tmp_path / "w.mefd")
    w = m.SessionWriter(path, True, p1, p2)
    summary = w.write_float("ch1", np.ascontiguousarray(data, dtype=np.float64), START, FS, precision)
    del w
    return path, summary


def _readback(path, data, p1="", p2=""):
    r = MefReader(path, password2=(p2 or p1) or None)
    got = np.asarray(r.get_data("ch1"), dtype=np.float64)
    uf = r.get_channel_info("ch1")["ufact"][0]
    prec = int(round(-np.log10(uf)))
    ref = np.asarray(data, dtype=np.float64)
    exp = np.where(np.isnan(ref), np.nan, np.round(ref, prec))
    assert got.shape == exp.shape
    assert np.array_equal(np.isnan(got), np.isnan(exp))
    assert np.allclose(got[~np.isnan(got)], exp[~np.isnan(exp)], atol=10.0 ** -prec * 0.6)


@pytest.mark.parametrize(
    "name,data",
    [
        ("smooth", 50 * np.sin(np.arange(3000) / 20) + np.random.default_rng(0).normal(0, 1, 3000)),
        ("multiblock", 30 * np.sin(np.arange(9000) / 40)),
        ("bigvals", np.random.default_rng(1).normal(0, 50000, 2000)),
        ("small_zscored", np.random.default_rng(2).normal(0, 1, 2000)),
    ],
)
def test_float_write_read_by_mef_tools(tmp_path, name, data):
    path, summary = _write_float(tmp_path, data)
    assert summary["samples_written"] == len(data)
    assert summary["gaps_skipped"] == 0
    _readback(path, data)


def test_nan_gap_splits_and_reads_back(tmp_path):
    data = 50 * np.sin(np.arange(3000) / 20)
    data[1000:1500] = np.nan
    path, summary = _write_float(tmp_path, data)
    assert summary["samples_written"] == 2500
    assert summary["gaps_skipped"] == 1
    _readback(path, data)


def test_multiple_nan_gaps(tmp_path):
    data = 20 * np.sin(np.arange(4000) / 15)
    data[500:700] = np.nan
    data[2000:2600] = np.nan
    path, summary = _write_float(tmp_path, data)
    assert summary["gaps_skipped"] == 2
    _readback(path, data)


def test_all_nan_is_noop(tmp_path):
    path, summary = _write_float(tmp_path, np.full(500, np.nan))
    assert summary["samples_written"] == 0
    assert summary["blocks"] == 0
    # Nothing was written to disk.
    assert not any(Path(path).glob("*.timd"))


def test_encrypted_float_write(tmp_path):
    data = np.random.default_rng(3).normal(0, 20, 2500)
    path, _ = _write_float(tmp_path, data, p1="pass1", p2="pass2")
    _readback(path, data, p1="pass1", p2="pass2")


def test_int32_primitive_path(tmp_path):
    # Amplifier counts + V/bit conversion factor, stored verbatim.
    counts = np.random.default_rng(4).integers(-2000, 2000, 3000).astype(np.int32)
    ufact = 2.5e-7
    path = str(tmp_path / "i.mefd")
    w = m.SessionWriter(path, True)
    w.write_int32("ch1", np.ascontiguousarray(counts), ufact, START, FS)
    del w
    r = MefReader(path)
    raw = np.asarray(r.get_raw_data("ch1"), dtype=np.int64).ravel()
    assert np.array_equal(raw, counts.astype(np.int64))
    assert r.get_channel_info("ch1")["ufact"][0] == pytest.approx(ufact)


def test_precision_inference_matches_mef_tools(tmp_path):
    # mef3io.infer_precision should agree with mef_tools' inference.
    from mef_tools.io import infer_conversion_factor

    for seed in range(5):
        data = np.random.default_rng(seed).normal(0, 10 ** (seed - 2), 1000)
        assert m.infer_precision(np.ascontiguousarray(data)) == infer_conversion_factor(data)


def test_new_segment_appends(tmp_path):
    path = str(tmp_path / "seg.mefd")
    w = m.SessionWriter(path, True)
    a = np.sin(np.arange(2000) / 20)
    b = np.sin(np.arange(2000) / 20 + 5)
    w.write_float("ch1", np.ascontiguousarray(a), START, FS)
    # second write starts right after the first, as a new segment
    t2 = START + int(2000 / FS * 1e6)
    w.write_float("ch1", np.ascontiguousarray(b), t2, FS)
    del w
    r = MefReader(path)
    got = np.asarray(r.get_data("ch1"), dtype=np.float64)
    assert len(got) == 4000
    assert not np.isnan(got).any()  # contiguous in time -> no gap
