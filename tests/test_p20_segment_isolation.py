"""P20: one unreadable file must not take down a whole session.

Reported against 1.1.1 on long-term iEEG archives: 12 bad `.tmet` files made
all 1190 segments of one session unreadable, locking out 98,128 already
exported analysis windows. The specific cause there was a mef3io bug (the
`.tmet` body CRC was taken to EOF over foreign padding), and that is fixed —
but the BLAST RADIUS is the point. A genuinely damaged segment would have had
exactly the same effect.

Strict remains the default: an unreported CRC failure is how corrupt scaling
reaches an analysis unnoticed. `strict=False` is the documented way to salvage
the intact remainder, and it is never silent — a skipped segment's span reads
back as NaN, which the returned array cannot distinguish from a real gap.

Issue #12.
"""
from __future__ import annotations

import glob
import sys
import warnings
from pathlib import Path

import numpy as np
import pytest

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO))

import mef3io  # noqa: E402
from mef3io import _mef3io as m  # noqa: E402

FS = 256.0
START = 1577836800000000
CHUNK = 2000


def _write_segments(path, n_segments=3):
    """n separate segments, each its own .segd, with known contents."""
    data = []
    w = m.SessionWriter(str(path), True)
    for i in range(n_segments):
        block = np.arange(i * CHUNK, (i + 1) * CHUNK, dtype=np.int32)
        t = START + int(round(i * CHUNK / FS * 1e6)) + i * int(1e6)  # gap between segments
        w.write_int32("ch1", block, 1.0, t, FS, None, True)  # new_segment=True
        data.append(block)
    del w
    return data


def _segd_dirs(path):
    return sorted(glob.glob(str(path) + "/ch1.timd/*.segd"))


def _corrupt_tmet(segd):
    """Break the metadata body so the CRC check rejects it."""
    tmet = glob.glob(segd + "/*.tmet")[0]
    raw = bytearray(open(tmet, "rb").read())
    raw[2000] ^= 0xFF  # inside the section-2 body, past the universal header
    open(tmet, "wb").write(bytes(raw))
    return tmet


def test_strict_is_the_default_and_still_fails_loudly(tmp_path):
    path = tmp_path / "s.mefd"
    _write_segments(path)
    _corrupt_tmet(_segd_dirs(path)[1])

    with pytest.raises(Exception):
        mef3io.Reader(str(path)).read("ch1")
    # ... and explicitly, not just by default
    with pytest.raises(Exception):
        mef3io.Reader(str(path), strict=True).read("ch1")


def test_lenient_reads_the_intact_segments_and_reports_the_bad_one(tmp_path):
    path = tmp_path / "s.mefd"
    data = _write_segments(path)
    _corrupt_tmet(_segd_dirs(path)[1])

    with pytest.warns(mef3io.UnreadableSegmentWarning, match="SKIPPED"):
        r = mef3io.Reader(str(path), strict=False)

    problems = r.problems
    assert len(problems) == 1, problems
    assert problems[0]["channel"] == "ch1"
    assert problems[0]["segment_number"] == 1
    assert problems[0]["reason"]                      # must say WHY
    assert ".tmet" in problems[0]["segment"]          # ... and WHERE

    got = r.read("ch1")
    # The intact segments' samples are still there, at their real values.
    present = got[~np.isnan(got)]
    assert len(present) == 2 * CHUNK, len(present)
    np.testing.assert_array_equal(
        np.sort(present.astype(np.int64)),
        np.sort(np.concatenate([data[0], data[2]]).astype(np.int64)),
    )


def test_a_healthy_session_is_identical_in_both_modes(tmp_path):
    """Lenient mode must change nothing when there is nothing to contain."""
    path = tmp_path / "s.mefd"
    _write_segments(path)
    with warnings.catch_warnings():
        warnings.simplefilter("error")  # no warning may fire on a clean session
        lenient = mef3io.Reader(str(path), strict=False)
        assert lenient.problems == []
        a = lenient.read("ch1")
    b = mef3io.Reader(str(path)).read("ch1")
    np.testing.assert_array_equal(np.nan_to_num(a, nan=-1), np.nan_to_num(b, nan=-1))


def test_problems_is_empty_in_strict_mode(tmp_path):
    """Strict never contains anything, so it can never have something to list."""
    path = tmp_path / "s.mefd"
    _write_segments(path)
    r = mef3io.Reader(str(path))
    r.read("ch1")
    assert r.problems == []


def test_the_segment_map_and_count_agree_after_a_skip(tmp_path):
    """n_segments must not out-count what segments() will actually return, or
    the map looks truncated."""
    path = tmp_path / "s.mefd"
    _write_segments(path)
    _corrupt_tmet(_segd_dirs(path)[1])

    with pytest.warns(mef3io.UnreadableSegmentWarning):
        r = mef3io.Reader(str(path), strict=False)
    assert r.info("ch1")["n_segments"] == 2
    assert len(r.segments("ch1")) == 2
    assert [s["segment"] for s in r.segments("ch1")] == [0, 2]


def test_a_skipped_segment_is_not_retried_on_every_call(tmp_path):
    """The failure is recorded on the segment, so a session with many bad files
    does not pay the exception cost once per call per segment."""
    path = tmp_path / "s.mefd"
    _write_segments(path)
    _corrupt_tmet(_segd_dirs(path)[1])

    with pytest.warns(mef3io.UnreadableSegmentWarning):
        r = mef3io.Reader(str(path), strict=False)
    before = len(r.problems)
    for _ in range(5):
        r.read("ch1")
        r.segments("ch1")
    assert len(r.problems) == before, "the same segment was reported more than once"
