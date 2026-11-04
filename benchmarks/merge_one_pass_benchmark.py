"""One-call merge (ranges + anonymize + records drop) on synthetic sessions.

Writes SOURCES synthetic recordings (CHANNELS channels, MINUTES minutes at FS
Hz, taking turns in time), then runs merge_sessions with each thread count and
reports wall time, bytes and read() syscalls of this process (/proc/self/io:
rchar, syscr) and the write amplification. Synthetic data only.

    python benchmarks/merge_one_pass_benchmark.py [--channels 64] [--minutes 10]
        [--sources 4] [--threads 1 4 8]
"""
from __future__ import annotations

import argparse
import shutil
import tempfile
import time
import warnings
from pathlib import Path

import numpy as np

import mef3io

START = 1_577_880_000_000_000
SECOND = 1_000_000


def _io():
    d = {}
    for line in Path("/proc/self/io").read_text().splitlines():
        k, v = line.split(":")
        d[k] = int(v)
    return d["rchar"], d["syscr"], d["wchar"]


def _make(root: Path, k: int, channels: int, minutes: int, fs: float):
    rng = np.random.default_rng(k)
    path = root / f"src{k}.mefd"
    n = int(minutes * 60 * fs)
    t0 = START + k * (minutes + 1) * 60 * SECOND
    meta = mef3io.Metadata(subject=mef3io.Subject(name_1=f"Patient{k}", id=f"ID{k:03d}"))
    with mef3io.Writer(str(path), overwrite=True, metadata=meta) as w:
        for c in range(channels):
            x = np.cumsum(rng.integers(-5, 6, n)).astype(float)
            w.write(f"ch{c:03d}", x, t0, fs, precision=0)
        w.write_annotations([{"type": "Note", "time": t0 + SECOND, "text": f"note {k}"}])
    return path, t0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--channels", type=int, default=64)
    ap.add_argument("--minutes", type=int, default=10)
    ap.add_argument("--sources", type=int, default=4)
    ap.add_argument("--fs", type=float, default=1000.0)
    ap.add_argument("--threads", type=int, nargs="+", default=[1, 4, 8])
    a = ap.parse_args()
    warnings.simplefilter("ignore")
    root = Path(tempfile.mkdtemp(prefix="mef3io_bench_"))
    try:
        made = [_make(root, k, a.channels, a.minutes, a.fs) for k in range(a.sources)]
        paths = [p for p, _ in made]
        # each source trimmed to the middle 80 % of its recording
        span = a.minutes * 60 * SECOND
        ranges = [(t + span // 10, t + span * 9 // 10) for _, t in made]
        src_bytes = sum(f.stat().st_size for p in paths for f in p.rglob("*") if f.is_file())
        print(f"{a.sources} sources x {a.channels} channels x {a.minutes} min @ {a.fs:g} Hz: "
              f"{src_bytes / 1e6:.0f} MB")
        print(f"{'threads':>7} {'wall s':>8} {'read MB':>9} {'read calls':>11} {'written MB':>11}")
        for n in a.threads:
            dst = root / f"out{n}.mefd"
            r0 = _io()
            t = time.perf_counter()
            rep = mef3io.merge_sessions(paths, dst, time_ranges=ranges, records="drop",
                                        anonymize={"shift_us": 50 * 365 * 86400 * SECOND},
                                        n_threads=n)
            wall = time.perf_counter() - t
            r1 = _io()
            assert rep.verification.ok
            print(f"{n:>7} {wall:>8.2f} {(r1[0] - r0[0]) / 1e6:>9.0f} {r1[1] - r0[1]:>11} "
                  f"{(r1[2] - r0[2]) / 1e6:>11.0f}")
            shutil.rmtree(dst)
    finally:
        shutil.rmtree(root, ignore_errors=True)


if __name__ == "__main__":
    main()
