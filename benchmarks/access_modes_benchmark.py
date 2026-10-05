#!/usr/bin/env python
"""How a session is opened matters as much as how it is decoded.

Measures the same session under every access mode a user actually meets:

  layout      .mefd directory  vs  the same session packed into one .mefd.tar
  page cache  COLD (every file evicted from the OS page cache — a session not
              touched since boot, or bigger than RAM)  vs  WARM (just read)
  snapshot    mef3io's opt-in metadata cache (`Reader(cache=...)`) off vs on

plus the legacy `mef_tools` reader on the directory as the baseline (it cannot
read a tar). Every cell runs in a FRESH PROCESS, so nothing carries over in
memory, and records the bytes that actually came off the block device
(`/proc/self/io` read_bytes) — a "cold" cell that reads ~0 bytes was not cold,
and the script refuses to report it.

Operations, each timed from inside the child after imports:

  open     construct the reader, list channels, read every channel's info
  first    open + read ONE 1-minute window of one channel (time to first data)
  windows  open + N random windows (channel, 5 min) — a viewer / DL loader
  full     open + read K channels end to end

Eviction uses posix_fadvise(DONTNEED), which needs no root but only drops
CLEAN pages, so the session is fsynced after it is written.

    python benchmarks/access_modes_benchmark.py                # defaults
    python benchmarks/access_modes_benchmark.py --quick        # smoke run
    python benchmarks/access_modes_benchmark.py --hours 24 --channels 128
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(REPO / "python"))

BASE_UUTC = 1_600_000_000_000_000


# --------------------------------------------------------------------------- #
# page cache
# --------------------------------------------------------------------------- #
def session_files(path: Path) -> list[Path]:
    if path.is_file():
        return [path]
    return [p for p in path.rglob("*") if p.is_file()]


def evict(path: Path) -> None:
    for p in session_files(path):
        fd = os.open(p, os.O_RDONLY)
        try:
            os.posix_fadvise(fd, 0, 0, os.POSIX_FADV_DONTNEED)
        finally:
            os.close(fd)


def fsync_tree(path: Path) -> None:
    for p in session_files(path):
        fd = os.open(p, os.O_RDONLY)
        try:
            os.fsync(fd)
        finally:
            os.close(fd)


def prime(path: Path) -> None:
    """Pull every byte into the page cache."""
    for p in session_files(path):
        with open(p, "rb") as f:
            while f.read(1 << 24):
                pass


# --------------------------------------------------------------------------- #
# child: one measurement in a clean process
# --------------------------------------------------------------------------- #
def _io() -> dict:
    out = {}
    with open("/proc/self/io") as f:
        for line in f:
            k, v = line.split(":")
            out[k.strip()] = int(v)
    return out


def child(spec: dict) -> dict:
    backend, path, op = spec["backend"], spec["path"], spec["op"]
    if backend == "mef3io":
        import mef3io

        def open_():
            r = mef3io.Reader(path, cache=spec.get("cache"), n_threads=spec["threads"])
            chans = r.channels
            infos = {c: r.info(c) for c in chans}
            return r, chans, infos

        def read(r, ch, t0, t1):
            return r.read(ch, t0, t1)
    else:
        from mef_tools.io import MefReader

        def open_():
            r = MefReader(path)
            chans = list(r.channels)
            infos = {c: (r.get_property("fsamp", c), r.get_property("start_time", c),
                         r.get_property("end_time", c)) for c in chans}
            return r, chans, infos

        def read(r, ch, t0, t1):
            return np.asarray(r.get_data(ch, t0, t1))

    io0 = _io()
    t = time.perf_counter()
    r, chans, _ = open_()
    t_open = time.perf_counter() - t
    samples = 0
    if op == "first":
        x = read(r, chans[0], BASE_UUTC + 3600 * 10**6, BASE_UUTC + 3660 * 10**6)
        samples += len(x)
    elif op == "windows":
        rng = np.random.default_rng(spec["seed"])
        span_us = int(spec["hours"] * 3600e6) - 300 * 10**6
        for _ in range(spec["n_windows"]):
            ch = chans[int(rng.integers(len(chans)))]
            t0 = BASE_UUTC + int(rng.integers(span_us))
            samples += len(read(r, ch, t0, t0 + 300 * 10**6))
    elif op == "full":
        for ch in chans[: spec["full_channels"]]:
            samples += len(read(r, ch, None, None))
    total = time.perf_counter() - t
    io1 = _io()
    return {
        "open_s": t_open,
        "total_s": total,
        "samples": samples,
        "disk_bytes": io1["read_bytes"] - io0["read_bytes"],
        "rchar": io1["rchar"] - io0["rchar"],
    }


def run_child(spec: dict) -> dict:
    out = subprocess.run(
        [sys.executable, __file__, "--child", json.dumps(spec)],
        check=True, capture_output=True, text=True,
    )
    return json.loads(out.stdout.strip().splitlines()[-1])


# --------------------------------------------------------------------------- #
# dataset
# --------------------------------------------------------------------------- #
def write_sessions(args, work: Path) -> dict:
    from mef_benchmark import Config, gen_block  # same deterministic signal

    import mef3io

    cfg = Config(hours=args.hours, fs=args.fs, channels=args.channels, precision=3)
    seg_n = int(round(args.segment_hours * 3600 * args.fs))
    n = cfg.n_samples
    names = [f"ch{i:03d}" for i in range(args.channels)]
    paths = {}

    t = time.perf_counter()
    d = work / "s.mefd"
    with mef3io.Writer(str(d), overwrite=True) as w:
        for i, name in enumerate(names):
            for k, a in enumerate(range(0, n, seg_n)):
                b = min(n, a + seg_n)
                x = gen_block(a, b - a, i, 1, cfg)[:, 0]
                w.write(name, x, BASE_UUTC + int(round(a / args.fs * 1e6)), args.fs,
                        precision=3, new_segment=k > 0)
    paths["write_mef3io_s"] = time.perf_counter() - t
    paths["mefd"] = d

    t = time.perf_counter()
    paths["tar"] = Path(mef3io.archive_session(str(d)))
    paths["archive_s"] = time.perf_counter() - t

    if "mef_tools" in args.backends:
        from mef_tools.io import MefWriter

        t = time.perf_counter()
        ld = work / "legacy.mefd"
        w = MefWriter(str(ld), overwrite=True)
        for i, name in enumerate(names):
            for k, a in enumerate(range(0, n, seg_n)):
                b = min(n, a + seg_n)
                x = gen_block(a, b - a, i, 1, cfg)[:, 0]
                w.write_data(x, name, BASE_UUTC + int(round(a / args.fs * 1e6)), args.fs,
                             precision=3, new_segment=k > 0,
                             reload_metadata=(i == len(names) - 1 and b == n))
        del w
        paths["write_mef_tools_s"] = time.perf_counter() - t
        paths["legacy"] = ld

    for key in ("mefd", "tar", "legacy"):
        if key in paths:
            fsync_tree(paths[key])
    return paths


def dir_bytes(p: Path) -> int:
    return sum(f.stat().st_size for f in session_files(p))


# --------------------------------------------------------------------------- #
# the matrix
# --------------------------------------------------------------------------- #
def cells(args, paths: dict) -> list[dict]:
    out = []
    for layout, key in (("dir", "mefd"), ("tar", "tar")):
        for snap in (False, True):
            out.append({"label": f"mef3io {layout}" + (" +snapshot" if snap else ""),
                        "backend": "mef3io", "path": str(paths[key]), "layout": layout,
                        "snapshot": snap})
    if "legacy" in paths:
        out.append({"label": "mef_tools dir", "backend": "mef_tools",
                    "path": str(paths["legacy"]), "layout": "dir", "snapshot": False})
        out.append({"label": "mef3io dir (legacy-written)", "backend": "mef3io",
                    "path": str(paths["legacy"]), "layout": "dir", "snapshot": False})
    return out


def measure(args, cell: dict, op: str, cold: bool, work: Path) -> dict:
    spec = dict(cell, op=op, threads=args.threads, seed=args.seed, hours=args.hours,
                n_windows=args.windows, full_channels=args.full_channels)
    cache_dir = work / "snapcache" / (cell["layout"] + ("-legacy" if "legacy" in cell["path"] else ""))
    spec["cache"] = str(cache_dir / "snap.json") if cell["snapshot"] else None
    path = Path(cell["path"])
    runs = []
    for rep in range(args.repeats):
        if cell["snapshot"]:
            # make sure a valid snapshot exists (built by a normal open)
            run_child(dict(spec, op="open"))
        if cold:
            evict(path)
        else:
            prime(path)
        runs.append(run_child(spec))
    med = lambda k: statistics.median(r[k] for r in runs)
    res = {k: med(k) for k in ("open_s", "total_s", "disk_bytes", "rchar")}
    res["samples"] = runs[0]["samples"]
    if cold and res["disk_bytes"] == 0 and op != "open":
        raise SystemExit(f"{cell['label']} {op}: a COLD run read 0 bytes from disk — "
                         "eviction did not work, refusing to report it as cold")
    return res


def fmt_s(s: float) -> str:
    return f"{s * 1e3:.0f} ms" if s < 1 else f"{s:.2f} s"


def fmt_b(n: float) -> str:
    for u in ("B", "KB", "MB", "GB"):
        if n < 1024 or u == "GB":
            return f"{n:.0f} {u}" if u == "B" else f"{n:.1f} {u}"
        n /= 1024


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--hours", type=float, default=12.0)
    p.add_argument("--fs", type=float, default=256.0)
    p.add_argument("--channels", type=int, default=64)
    p.add_argument("--segment-hours", type=float, default=1.0,
                   help="one segment per channel per this many hours (open cost scales with it)")
    p.add_argument("--windows", type=int, default=64)
    p.add_argument("--full-channels", type=int, default=8)
    p.add_argument("--threads", type=int, default=0, help="mef3io decode threads (0 = all)")
    p.add_argument("--repeats", type=int, default=3)
    p.add_argument("--backends", nargs="+", default=["mef3io", "mef_tools"])
    p.add_argument("--ops", nargs="+", default=["open", "first", "windows", "full"])
    p.add_argument("--outdir", default="")
    p.add_argument("--keep-files", action="store_true")
    p.add_argument("--seed", type=int, default=7)
    p.add_argument("--quick", action="store_true")
    p.add_argument("--child", help=argparse.SUPPRESS)
    args = p.parse_args()

    if args.child:
        print(json.dumps(child(json.loads(args.child))))
        return
    if args.quick:
        args.hours, args.channels, args.windows, args.full_channels, args.repeats = 2, 8, 8, 2, 1

    work = Path(args.outdir or tempfile.mkdtemp(prefix="mef3io_access_"))
    work.mkdir(parents=True, exist_ok=True)
    import mef3io

    print(f"mef3io {mef3io.__version__}  |  {args.channels} ch x {args.hours:g} h @ {args.fs:g} Hz, "
          f"segment every {args.segment_hours:g} h  |  {os.cpu_count()} cpus  |  {work}")
    paths = write_sessions(args, work)
    sizes = {k: dir_bytes(paths[k]) for k in ("mefd", "tar", "legacy") if k in paths}
    print(f"wrote mef3io in {paths['write_mef3io_s']:.1f} s ({fmt_b(sizes['mefd'])}), "
          f"archived in {paths['archive_s']:.1f} s ({fmt_b(sizes['tar'])})"
          + (f", mef_tools in {paths['write_mef_tools_s']:.1f} s ({fmt_b(sizes['legacy'])})"
             if "legacy" in paths else ""))

    results = []
    for cell in cells(args, paths):
        for cold in (True, False):
            for op in args.ops:
                r = measure(args, cell, op, cold, work)
                row = {"label": cell["label"], "cache": "cold" if cold else "warm", "op": op, **r}
                results.append(row)
                print(f"  {row['label']:<30} {row['cache']:<4} {op:<8} total {fmt_s(r['total_s']):>9}"
                      f"  open {fmt_s(r['open_s']):>8}  disk {fmt_b(r['disk_bytes']):>9}", flush=True)

    meta = {"version": mef3io.__version__, "args": vars(args), "sizes": sizes,
            "write_mef3io_s": paths["write_mef3io_s"], "archive_s": paths["archive_s"],
            "write_mef_tools_s": paths.get("write_mef_tools_s"), "cpus": os.cpu_count()}
    (work / "access_modes.json").write_text(json.dumps({"meta": meta, "results": results}, indent=1))

    # pivot: rows = label, columns = op x cache
    print("\n| reader | " + " | ".join(f"{op} cold | {op} warm" for op in args.ops) + " |")
    print("|---|" + "---|---|" * len(args.ops))
    for label in dict.fromkeys(r["label"] for r in results):
        cellv = {(r["op"], r["cache"]): r["total_s"] for r in results if r["label"] == label}
        print(f"| {label} | " + " | ".join(f"{fmt_s(cellv[(op, 'cold')])} | {fmt_s(cellv[(op, 'warm')])}"
                                           for op in args.ops) + " |")
    print(f"\nresults: {work / 'access_modes.json'}")
    if not args.keep_files:
        for k in ("mefd", "tar", "legacy"):
            if k in paths:
                shutil.rmtree(paths[k]) if paths[k].is_dir() else paths[k].unlink()


if __name__ == "__main__":
    main()
