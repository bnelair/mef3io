"""P8: in-segment append and the per-segment map. Appends must extend the
channel's last segment in place (legacy mef_tools semantics), stay readable by
the pymef/mef_tools oracle, and the segment map must locate data across gaps."""
import glob
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

pytest.importorskip("mef_tools", reason="legacy oracle not installed (pip install mef3io[test])")
from mef_tools.io import MefReader  # noqa: E402

START = 1577836800000000
FS = 256.0


def _segd_dirs(path, ch="ch1"):
    return sorted(glob.glob(f"{path}/{ch}.timd/*.segd"))


def _write_two(path, gap_us=0, new_segment=False, p1="", p2=""):
    a = np.sin(np.arange(2000) / 20)
    b = np.sin(np.arange(2000) / 20 + 5)
    w = m.SessionWriter(path, True, p1, p2)
    w.write_float("ch1", np.ascontiguousarray(a), START, FS)
    t2 = START + int(2000 / FS * 1e6) + gap_us
    w.write_float("ch1", np.ascontiguousarray(b), t2, FS, -1, new_segment)
    del w
    return a, b


def test_append_extends_segment_in_place(tmp_path):
    path = str(tmp_path / "s.mefd")
    a, b = _write_two(path)
    assert len(_segd_dirs(path)) == 1  # no second segment on disk

    with mef3io.Reader(path) as r:
        info = r.info("ch1")
        assert info["n_segments"] == 1
        assert info["number_of_samples"] == 4000
        got = r.read("ch1")
        ufact = info["units_conversion_factor"]
    ref = np.round(np.concatenate([a, b]) / ufact) * ufact
    assert len(got) == 4000 and not np.isnan(got).any()
    assert np.allclose(got, ref, atol=ufact / 2)

    # oracle reads the appended segment identically
    legacy = np.asarray(MefReader(path).get_data("ch1"), float)
    assert np.allclose(legacy, got)


def test_append_with_gap_keeps_single_segment(tmp_path):
    path = str(tmp_path / "s.mefd")
    gap_us = int(5e6)
    _write_two(path, gap_us=gap_us)
    assert len(_segd_dirs(path)) == 1

    with mef3io.Reader(path) as r:
        got = r.read("ch1")
        segs = r.segments("ch1")
    n_gap = round(gap_us * FS / 1e6)
    assert len(got) == 4000 + n_gap
    assert np.isnan(got[2000:2000 + n_gap]).all()
    assert not np.isnan(got[:2000]).any() and not np.isnan(got[-2000:]).any()
    # the map still shows one segment spanning the gap, with only stored samples
    assert len(segs) == 1
    assert segs[0]["number_of_samples"] == 4000


def test_new_segment_flag_forces_second_segment(tmp_path):
    path = str(tmp_path / "s.mefd")
    a, b = _write_two(path, new_segment=True)
    assert len(_segd_dirs(path)) == 2

    with mef3io.Reader(path) as r:
        assert r.info("ch1")["n_segments"] == 2
        got = r.read("ch1")
    legacy = np.asarray(MefReader(path).get_data("ch1"), float)
    assert len(got) == 4000 and np.allclose(legacy, got)


def test_reopened_session_appends_in_segment(tmp_path):
    path = str(tmp_path / "s.mefd")
    a = np.sin(np.arange(2000) / 20)
    b = np.sin(np.arange(2000) / 20 + 5)
    w = m.SessionWriter(path, True)
    w.write_float("ch1", np.ascontiguousarray(a), START, FS)
    del w

    w = m.SessionWriter(path, False)  # reopen: state hydrated from disk
    t2 = START + int(2000 / FS * 1e6)
    s = w.write_float("ch1", np.ascontiguousarray(b), t2, FS)
    del w
    assert s["segment"] == 0
    assert len(_segd_dirs(path)) == 1

    with mef3io.Reader(path) as r:
        got = r.read("ch1")
        segs = r.segments("ch1")
    legacy = np.asarray(MefReader(path).get_data("ch1"), float)
    assert len(got) == 4000 and np.allclose(legacy, got)
    # channel-wide sample numbering continued across the reopen
    assert segs[0]["start_sample"] == 0 and segs[0]["number_of_samples"] == 4000


def test_reopened_append_reuses_precision(tmp_path):
    # Appended floats must reuse the segment's conversion factor even when
    # fresh inference on the appended data would pick a different precision.
    path = str(tmp_path / "s.mefd")
    fine = np.random.default_rng(0).normal(0, 0.01, 2000)   # infers high precision
    coarse = np.random.default_rng(1).normal(0, 1000.0, 2000)  # would infer lower
    w = m.SessionWriter(path, True)
    w.write_float("ch1", np.ascontiguousarray(fine), START, FS)
    del w
    w = m.SessionWriter(path, False)
    t2 = START + int(2000 / FS * 1e6)
    w.write_float("ch1", np.ascontiguousarray(coarse), t2, FS)
    del w

    assert len(_segd_dirs(path)) == 1
    with mef3io.Reader(path) as r:
        ufact = r.info("ch1")["units_conversion_factor"]
        got = r.read("ch1")
    ref = np.round(np.concatenate([fine, coarse]) / ufact) * ufact
    assert np.allclose(got, ref, atol=ufact / 2)


def test_append_encrypted_session(tmp_path):
    path = str(tmp_path / "s.mefd")
    p1, p2 = "pass1", "pass2"
    a, b = _write_two(path, p1=p1, p2=p2)
    assert len(_segd_dirs(path)) == 1

    with mef3io.Reader(path, p2) as r:
        got = r.read("ch1")
    legacy = np.asarray(MefReader(path, password2=p2).get_data("ch1"), float)
    assert len(got) == 4000 and np.allclose(legacy, got)

    # reopen + append also works through the encryption path
    w = m.SessionWriter(path, False, p1, p2)
    t3 = START + 2 * int(2000 / FS * 1e6)
    w.write_float("ch1", np.ascontiguousarray(a), t3, FS)
    del w
    assert len(_segd_dirs(path)) == 1
    with mef3io.Reader(path, p2) as r:
        info = r.info("ch1")
        assert info["number_of_samples"] == 6000
        # L2 access exposes section-3 (subject) metadata ...
        assert info["section3_available"] and info["subject_id"] is not None
    with mef3io.Reader(path, p1) as r:
        info = r.info("ch1")
        # ... L1 access does not, but still reads the signal
        assert not info["section3_available"] and info["subject_id"] is None
        assert len(r.read("ch1")) > 0


def test_append_conflicts_raise(tmp_path):
    path = str(tmp_path / "s.mefd")
    a, _ = _write_two(path)
    w = m.SessionWriter(path, False)
    # overlapping start time
    with pytest.raises(RuntimeError, match="before segment end"):
        w.write_float("ch1", np.ascontiguousarray(a), START, FS)
    # sampling-frequency mismatch
    t3 = START + 2 * int(2000 / FS * 1e6)
    with pytest.raises(RuntimeError, match="fs"):
        w.write_float("ch1", np.ascontiguousarray(a), t3, 512.0)
    # conversion-factor mismatch on the primitive path
    counts = np.arange(2000, dtype=np.int32)
    with pytest.raises(RuntimeError, match="conversion_factor"):
        w.write_int32("ch1", counts, 0.5, t3, FS)
    del w
    assert len(_segd_dirs(path)) == 1  # nothing was written by the failures


def test_segment_map_locates_data_across_huge_gap(tmp_path):
    path = str(tmp_path / "s.mefd")
    a = np.sin(np.arange(2000) / 20)
    day = int(86400e6)
    w = m.SessionWriter(path, True)
    w.write_float("ch1", np.ascontiguousarray(a), START, FS)
    w.write_float("ch1", np.ascontiguousarray(a), START + day, FS, -1, True)
    del w

    with mef3io.Reader(path) as r:
        segs = r.segments("ch1")
        toc0 = r.toc("ch1")
    assert [s["segment"] for s in segs] == [0, 1]
    dur = int(2000 / FS * 1e6)
    assert segs[0]["start_time"] == START
    assert segs[0]["end_time"] == START + dur
    assert segs[1]["start_time"] == START + day
    assert segs[1]["end_time"] == START + day + dur
    assert segs[0]["number_of_samples"] == segs[1]["number_of_samples"] == 2000
    assert segs[0]["start_sample"] == 0 and segs[1]["start_sample"] == 2000
    assert all(Path(s["path"]).is_dir() for s in segs)
    # block-level toc covers the same span
    assert toc0[0]["start_uutc"] == START and toc0[-1]["start_uutc"] >= START + day


def test_compat_writer_appends_in_segment(tmp_path):
    from mef3io.compat import MefWriter as CompatWriter

    path = str(tmp_path / "s.mefd")
    a = 40 * np.sin(np.arange(3000) / 20)
    cw = CompatWriter(path, overwrite=True)
    cw.write_data(a, "ch1", START, FS, precision=3)
    t2 = START + int(3000 / FS * 1e6)
    cw.write_data(a, "ch1", t2, FS, precision=3)
    cw.close()

    assert len(_segd_dirs(path)) == 1
    legacy = np.asarray(MefReader(path).get_data("ch1"), float)
    assert len(legacy) == 6000
    assert np.allclose(legacy, np.round(np.concatenate([a, a]), 3))
