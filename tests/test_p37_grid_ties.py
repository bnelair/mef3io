"""#129 — a read starting exactly half a sample off the grid.

std::llround rounds halves away from zero, so the block holding t0 (a
negative offset from it) and every later block (positive offsets) were
rounded in opposite directions: one output slot between them belonged to no
block and read back as NaN, and the samples before it sat one slot early.
Every whole-microsecond start offset across a sample period is scanned here,
at rates whose half sample is a whole number of microseconds (where the tie
happens) and at ones where it is not.
"""
from __future__ import annotations

import math

import numpy as np
import pytest

import mef3io

START = 1_700_000_000_000_000


@pytest.fixture(scope="module")
def ramps(tmp_path_factory):
    """One ramp per (fs, block) (value == sample index), 3 blocks long."""
    out = {}
    root = tmp_path_factory.mktemp("ties")
    for fs, block in ((500.0, 5000), (500.0, 777), (250.0, 2500), (1000.0, 10000),
                      (200.0, 2000), (256.0, 2560)):
        path = root / f"r{int(fs)}_{block}.mefd"
        with mef3io.Writer(str(path), overwrite=True, block_length=block) as w:
            w.write("ch", np.arange(3 * block, dtype=float), START, fs=fs, precision=0)
        out[(fs, block)] = path
    return out


@pytest.mark.parametrize("fs, block", [(500.0, 5000), (500.0, 777), (250.0, 2500),
                                       (1000.0, 10000), (200.0, 2000), (256.0, 2560)])
def test_every_start_offset_reads_a_clean_ramp(ramps, fs, block):
    period = 1e6 / fs
    with mef3io.Reader(str(ramps[(fs, block)])) as r:
        for base in (295, block - 1):  # inside the first block / just before the second
            for off in range(int(base * period), int((base + 1) * period) + 1):
                t0 = START + off
                y = r.read("ch", t0, t0 + int(round(1.5 * block * period)))
                assert not np.isnan(y).any(), (fs, block, off)
                assert (np.diff(y) == 1).all(), (fs, block, off)
                # One rule: the nearest sample, a tie going to the one at or
                # after t0 (the other lies before the window).
                assert y[0] == math.floor(off * fs / 1e6 + 0.5), (fs, block, off)


def test_the_reported_case(ramps):
    """The issue's example: 500 Hz, t0 = start + 591 ms = sample 295.5."""
    with mef3io.Reader(str(ramps[(500.0, 5000)])) as r:
        for off, first in ((590_000, 295), (590_999, 295), (591_000, 296), (591_001, 296)):
            y = r.read("ch", START + off, START + off + 20_000_000)
            assert len(y) == 10_000 and not np.isnan(y).any()
            assert y[0] == first and y[-1] == first + 9_999
        # Half a sample before the second block starts: no NaN at y[0].
        y = r.read("ch", START + 9_999_000, START + 29_999_000)
        assert y[0] == 5000 and not np.isnan(y).any()


def test_ties_before_the_start_and_around_a_gap(tmp_path):
    """A tie start before the channel, inside a gap and just before data
    resumes: NaN exactly where there is no data, a unit ramp elsewhere."""
    fs, period = 500.0, 2000
    path = tmp_path / "gap.mefd"
    with mef3io.Writer(str(path), overwrite=True, block_length=1000) as w:
        w.write("ch", np.arange(10_000, dtype=float), START, fs=fs, precision=0)
        # 4 s gap, then 10 000 more samples continuing the ramp's values
        w.write("ch", np.arange(10_000, 20_000, dtype=float), START + 24_000_000, fs=fs,
                precision=0)
    with mef3io.Reader(str(path)) as r:
        for t0 in (START - 1_001_000, START + 19_001_000, START + 23_999_000):
            y = r.read("ch", t0, t0 + 10_000_000)
            have = ~np.isnan(y)
            assert have.any()
            # NaN only as one run (before the start, or the gap), data a unit ramp
            edges = np.flatnonzero(np.diff(have.astype(int)))
            assert len(edges) <= 2, t0
            assert (np.diff(y[have]) == 1).all() or len(edges) == 2, t0
            for run in np.split(y[have], np.flatnonzero(np.diff(y[have]) != 1) + 1):
                assert (np.diff(run) == 1).all()
            # every sample placed at its own time: index -> sample time
            for i in np.flatnonzero(have)[:: max(1, have.sum() // 50)]:
                v = int(y[i])
                t_sample = START + (v * period if v < 10_000 else 24_000_000 + (v - 10_000) * period)
                assert abs((t0 + i * period) - t_sample) <= period // 2, (t0, i, v)
