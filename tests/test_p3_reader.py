"""P3 gate: the high-level Reader (read/read_raw/info/toc/records) reproduces
the legacy mef_tools/pymef read path across every golden session."""
import json
from pathlib import Path

import numpy as np
import pytest
from pymef.mef_session import MefSession

import mef3io

REPO = Path(__file__).resolve().parent.parent
GOLDEN = REPO / "tests" / "golden"
MANIFEST = json.loads((GOLDEN / "manifest.json").read_text())
CASES = MANIFEST["cases"]


def _nan_equal(a, b):
    a = np.asarray(a, float)
    b = np.asarray(b, float)
    if a.shape != b.shape:
        return False
    ma, mb = np.isnan(a), np.isnan(b)
    return np.array_equal(ma, mb) and np.allclose(a[~ma], b[~mb])


@pytest.mark.parametrize("case", CASES, ids=lambda c: c["name"])
def test_read_matches_mef_tools_get_data(case):
    path = str(GOLDEN / f"{case['name']}.mefd")
    pwd = case["password"]
    ms = MefSession(path, pwd, True)
    try:
        bi = {b["name"]: b for b in ms.read_ts_channel_basic_info()}
        with mef3io.Reader(path, pwd or "") as r:
            for ch in r.channels:
                ci = r.info(ch)
                st, en = ci["start_time"], ci["end_time"]
                uf = float(bi[ch]["ufact"][0])
                # full read
                ref = np.asarray(ms.read_ts_channels_uutc(ch, [st, en]), float) * uf
                assert _nan_equal(r.read(ch), ref)
                # windowed read
                mid = st + (en - st) // 3
                ref2 = np.asarray(ms.read_ts_channels_uutc(ch, [st, mid]), float) * uf
                assert _nan_equal(r.read(ch, st, mid), ref2)
    finally:
        ms.close()


@pytest.mark.parametrize("case", CASES, ids=lambda c: c["name"])
def test_read_raw_consistent_with_read(case):
    path = str(GOLDEN / f"{case['name']}.mefd")
    pwd = case["password"] or ""
    with mef3io.Reader(path, pwd) as r:
        for ch in r.channels:
            raw = r.read_raw(ch)
            samples = np.asarray(raw["samples"])
            valid = np.asarray(raw["valid"]).astype(bool)
            uf = raw["units_conversion_factor"]
            expected = np.where(valid, samples.astype(np.float64) * uf, np.nan)
            assert _nan_equal(r.read(ch), expected)


def test_records_match_mef_tools():
    path = str(GOLDEN / "with_annotations.mefd")
    with mef3io.Reader(path) as r:
        recs = r.records("ch1")
    assert [(x["type"], x["time"], x["text"]) for x in recs] == [
        ("Note", 1577836800100000, "onset"),
        ("Note", 1577836800500000, "offset"),
    ]


def test_toc_reports_blocks():
    path = str(GOLDEN / "multi_segment.mefd")
    with mef3io.Reader(path) as r:
        toc = r.toc("ch1")
    # multi_segment: two segments, at least one block each, all with samples.
    assert len(toc) >= 2
    assert all(b["number_of_samples"] > 0 for b in toc)
    # sample indices are monotonically non-decreasing across the channel
    starts = [b["start_sample"] for b in toc]
    assert starts == sorted(starts)


def test_context_manager_and_backend_flag():
    path = str(GOLDEN / "plain_single.mefd")
    assert mef3io.have_cpp_backend()
    with mef3io.Reader(path) as r:
        assert r.channels == ["ch1"]
