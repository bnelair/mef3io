#!/usr/bin/env python
"""A window of many channels: one multi-channel read vs reading them one by one.

Compares, on a warm local session (10-minute window at 512 Hz, one segment
per channel, 10-second blocks, the result kept as one 2-D float64 array):

  loop    np.stack([r.read(ch, t0, t1) for ch in channels])   (Reader threads = n)
  pool    the same reads from a ThreadPoolExecutor of n Python threads over
          channels, the Reader on 1 thread (parallel across channels; what a
          caller had to build before #90)
  list    r.read(channels, t0, t1)                             (Reader threads = n)

Best of --repeat runs each. The C++ test suite's hidden case
"[.read-many-bench]" measures the same with an artificial delay per file
request (a slow network share).

    python benchmarks/read_many_benchmark.py
    python benchmarks/read_many_benchmark.py --channels 64 209 --threads 1 4 16
"""
from __future__ import annotations

import argparse
import shutil
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import numpy as np

import mef3io

START = 1577836800000000
FS = 512.0
WINDOW_S = 600


def build(path: Path, channels: int) -> list[str]:
    n = int(FS * (WINDOW_S + 60))
    i = np.arange(n)
    names = [f"ch{c:03d}" for c in range(channels)]
    with mef3io.Writer(str(path), overwrite=True) as w:
        for c, name in enumerate(names):
            x = (800 * np.sin(i / (7.0 + c)) + (i * 2654435761 % 32)).astype(np.int32)
            w.write_int32(name, x, 1.0, START, FS)
    return names


def best(fn, repeat: int) -> float:
    out = []
    for _ in range(repeat):
        t = time.perf_counter()
        fn()
        out.append(time.perf_counter() - t)
    return min(out)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--channels", type=int, nargs="+", default=[64, 209])
    ap.add_argument("--threads", type=int, nargs="+", default=[1, 4, 16])
    ap.add_argument("--repeat", type=int, default=3)
    args = ap.parse_args()
    t0 = START + 30_000_000
    t1 = t0 + WINDOW_S * 1_000_000
    tmp = Path(tempfile.mkdtemp(prefix="mef3io_read_many_"))
    try:
        for channels in args.channels:
            path = tmp / f"s{channels}.mefd"
            names = build(path, channels)
            with mef3io.Reader(str(path)) as r:
                ref = r.read(names, t0, t1)  # warm the page cache and the indexes
                for n in args.threads:
                    r.n_threads = n
                    loop = best(lambda: np.stack([r.read(c, t0, t1) for c in names]),
                                args.repeat)
                    r.n_threads = 1
                    with ThreadPoolExecutor(n) as ex:
                        pool = best(lambda: np.stack(list(ex.map(
                            lambda c: r.read(c, t0, t1), names))), args.repeat)
                    r.n_threads = n
                    many = best(lambda: r.read(names, t0, t1), args.repeat)
                    assert r.read(names, t0, t1).tobytes() == ref.tobytes()
                    print(f"{channels:4d} ch  n={n:2d}  loop {loop:7.3f} s  pool {pool:7.3f} s  "
                          f"list {many:7.3f} s  ({loop / many:.1f}x loop, {pool / many:.1f}x pool)",
                          flush=True)
            shutil.rmtree(path)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
