"""Finding what data is where: the segment map and the block TOC.

Long recordings often have huge gaps (electrode disconnects, battery swaps).
Reader.segments() maps each segment's time range, sample range and block
count from metadata alone — nothing is decoded, so it is cheap on huge
sessions. Reader.toc() is the finer, block-level view, including
discontinuities *inside* a segment. Use them to target windowed reads instead
of scanning through gaps.
"""
import tempfile
from datetime import datetime, timezone
from pathlib import Path

import numpy as np

import mef3io

FS = 256.0
START = 1_577_836_800_000_000
DAY_US = 86_400_000_000
CHUNK = 25600                                   # 100 s of data

session = str(Path(tempfile.mkdtemp()) / "sparse.mefd")

# Three islands of data: day 0, day 1, day 7 — in separate segments.
with mef3io.Writer(session, overwrite=True) as w:
    for day in (0, 1, 7):
        data = np.sin(np.arange(CHUNK) / 20 + day)
        w.write("ch1", data, START + day * DAY_US, FS, precision=3,
                new_segment=(day > 0))

with mef3io.Reader(session) as r:
    print("segment map:")
    for s in r.segments("ch1"):
        t0 = datetime.fromtimestamp(s["start_time"] / 1e6, tz=timezone.utc)
        t1 = datetime.fromtimestamp(s["end_time"] / 1e6, tz=timezone.utc)
        print(f"  seg {s['segment']}: {t0:%Y-%m-%d %H:%M:%S} .. {t1:%H:%M:%S}"
              f"  samples {s['start_sample']}..{s['start_sample'] + s['number_of_samples']}"
              f"  ({s['number_of_blocks']} blocks)  {s['path']}")

    # Jump straight to the day-7 island instead of reading through the gap.
    seg = r.segments("ch1")[-1]
    y = r.read("ch1", seg["start_time"], seg["end_time"])
    assert len(y) == CHUNK and not np.isnan(y).any()
    print(f"read the last island directly: {len(y)} samples, no NaN")

    # Block-level TOC: per-RED-block start times, sample counts, extrema,
    # and discontinuity flags (gaps *within* a segment show up here).
    toc = r.toc("ch1")
    print(f"toc: {len(toc)} blocks; first:", toc[0])
