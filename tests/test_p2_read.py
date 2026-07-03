"""P2 gate: the C++ Session must reproduce pymef's decoded samples, channel
list, and basic info for every golden session, including partial-range reads."""
import json
from pathlib import Path

import numpy as np
import pytest
from pymef.mef_session import MefSession

from mef3io import _mef3io as m

GOLDEN = Path(__file__).parent / "golden"
MANIFEST = json.loads((GOLDEN / "manifest.json").read_text())
CASES = MANIFEST["cases"]


def _session(case):
    path = str(GOLDEN / f"{case['name']}.mefd")
    return m.Session(path, case["password"] or ""), path


@pytest.mark.parametrize("case", CASES, ids=lambda c: c["name"])
def test_channels_match(case):
    s, _ = _session(case)
    assert set(s.channels) == set(case["channels"].keys())


@pytest.mark.parametrize("case", CASES, ids=lambda c: c["name"])
def test_channel_info_matches_pymef(case):
    # pymef basic info is the oracle: fs, ufact, absolute start/end, and the
    # *stored* sample count (manifest's nsamp is get_raw_data length, which
    # includes gap-filled samples and is therefore a different quantity).
    s, path = _session(case)
    ms = MefSession(path, case["password"], True)
    try:
        bi = {b["name"]: b for b in ms.read_ts_channel_basic_info()}
        for ch in s.channels:
            ci = s.channel_info(ch)
            b = bi[ch]
            assert ci["sampling_frequency"] == pytest.approx(float(b["fsamp"][0]))
            assert ci["units_conversion_factor"] == pytest.approx(float(b["ufact"][0]))
            assert ci["start_time"] == int(b["start_time"][0])
            assert ci["end_time"] == int(b["end_time"][0])
            assert ci["number_of_samples"] == int(b["nsamp"][0])
    finally:
        ms.close()


@pytest.mark.parametrize("case", CASES, ids=lambda c: c["name"])
def test_decoded_samples_match_pymef(case):
    s, path = _session(case)
    ms = MefSession(path, case["password"], True)
    try:
        bi = {b["name"]: b for b in ms.read_ts_channel_basic_info()}
        for ch in s.channels:
            runs = s.read_runs(ch)
            cpp = (
                np.concatenate([r["samples"] for r in runs]).astype(np.int64)
                if runs
                else np.array([], dtype=np.int64)
            )
            nsamp = int(bi[ch]["nsamp"][0])
            ref = ms.read_ts_channels_sample([ch], [0, nsamp])[0].astype(np.int64)
            assert cpp.shape == ref.shape
            assert np.array_equal(cpp, ref)
    finally:
        ms.close()


def test_run_splitting_semantics():
    """Long NaN gap -> discontinuity -> 2 runs; short NaN gap -> embedded
    sentinel -> single run."""
    long_case = next(c for c in CASES if c["name"] == "nan_gap_long")
    s, _ = _session(long_case)
    assert len(s.read_runs("ch1")) == 2

    short_case = next(c for c in CASES if c["name"] == "nan_gap_short")
    s, _ = _session(short_case)
    assert len(s.read_runs("ch1")) == 1


def test_partial_range_read():
    """Time-windowed reads select blocks by overlap. Reads are at block
    granularity in P2 (exact-sample trimming is the high-level Reader's job)."""
    # multi_segment spans two segments with a gap; a window over only the first
    # segment returns fewer samples than the full read.
    case = next(c for c in CASES if c["name"] == "multi_segment")
    s, _ = _session(case)
    ci = s.channel_info("ch1")
    start, end = ci["start_time"], ci["end_time"]
    n_full = sum(len(r["samples"]) for r in s.read_runs("ch1"))

    first_seg = s.read_runs("ch1", start, start + 1_000_000)  # ~ first second
    n_first = sum(len(r["samples"]) for r in first_seg)
    assert 0 < n_first < n_full

    # A window entirely before the data returns nothing.
    assert s.read_runs("ch1", start - 10_000_000, start - 5_000_000) == []
