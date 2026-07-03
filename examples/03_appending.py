"""Appending data.

The first write to a channel creates segment 0. Later writes append
IN-SEGMENT: the existing segment's files are extended in place (same layout
the legacy mef_tools writer produces). Pass new_segment=True to start a fresh
segment instead. A session can be reopened later (overwrite=False) and
appended to — state is recovered from the files on disk.

Appends are validated against the segment: the sampling frequency and
conversion factor must match, and the data must start at or after the
segment's end (a RuntimeError is raised otherwise).
"""
import tempfile
from pathlib import Path

import numpy as np

import mef3io

FS = 256.0
START = 1_577_836_800_000_000
CHUNK = 2560                                   # 10 s of data
CHUNK_US = int(CHUNK / FS * 1e6)

session = str(Path(tempfile.mkdtemp()) / "grow.mefd")
sig = lambda k: np.sin(np.arange(CHUNK) / 20 + k)  # noqa: E731

# --- write, then append contiguously (stays one segment) ------------------
with mef3io.Writer(session, overwrite=True) as w:
    w.write("ch1", sig(0), START, FS, precision=3)
    s = w.write("ch1", sig(1), START + CHUNK_US, FS)   # in-segment append
    print("append landed in segment", s["segment"])    # -> 0

# --- reopen the session later and keep appending ---------------------------
with mef3io.Writer(session, overwrite=False) as w:     # note: overwrite=False
    w.write("ch1", sig(2), START + 2 * CHUNK_US, FS)   # still segment 0

# --- a gap is fine: it becomes a discontinuity inside the segment ----------
with mef3io.Writer(session) as w:
    gap_us = 5_000_000
    w.write("ch1", sig(3), START + 3 * CHUNK_US + gap_us, FS)

# --- force a new segment when you want one ---------------------------------
with mef3io.Writer(session) as w:
    w.write("ch1", sig(4), START + 5 * CHUNK_US, FS, new_segment=True)

# --- appends that would corrupt the segment raise --------------------------
with mef3io.Writer(session) as w:
    try:
        w.write("ch1", sig(5), START, FS)              # overlaps existing data
    except RuntimeError as e:
        print("rejected overlap:", e)

with mef3io.Reader(session) as r:
    info = r.info("ch1")
    print("segments:", info["n_segments"], "samples:", info["number_of_samples"])
    y = r.read("ch1")
    n_gap = round((5_000_000) * FS / 1e6)
    assert info["n_segments"] == 2
    assert info["number_of_samples"] == 5 * CHUNK
    assert np.isnan(y[3 * CHUNK:3 * CHUNK + n_gap]).all()   # the 5 s gap
print("read", len(y), "grid samples (data + NaN gaps)")
