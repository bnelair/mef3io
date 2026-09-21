#!/usr/bin/env python3
"""The real workload: a full day of multi-channel recording, block by block.

Default shape — **24 h x 16 channels @ 512 Hz**, written in 10-minute blocks
across all channels, then read back in 5-20 minute windows across all channels.
That is ~708 M samples, a few GB on disk, and it is what these files actually
look like: not one big write, but 144 appends and then windowed reads.

This is deliberately NOT part of the unit tests. It takes minutes, it writes
gigabytes, and it is the thing you run before a release — or after touching the
append path, the index, or anything that could turn per-append work into
per-session work.

What it measures, for mef3io and (optionally) the legacy mef_tools stack:

  WRITE   per-block append time, its GROWTH over the session (the number that
          matters: anything above ~1.0 keeps climbing for the life of the
          recording), duty cycle against real time, and bytes actually written
          — so a whole-file rewrite shows up as write amplification rather than
          just as "slower".

  READ    windowed reads of 5-20 min across all channels, with the bytes
          actually read. A windowed read must touch a small fraction of the
          file; if it reads everything, that is fatal at 30 GB per channel and
          this is where it shows.

  INTEGRITY  the session is validated at the end, and `recover_session` is run
          as a dry run, so a run that produces a fast but inconsistent session
          fails loudly rather than looking good.

Examples
--------
    python benchmarks/long_session_benchmark.py --plan
    python benchmarks/long_session_benchmark.py
    python benchmarks/long_session_benchmark.py --hours 2 --quick-read
    python benchmarks/long_session_benchmark.py --durability fast --threads 0
    python benchmarks/long_session_benchmark.py --with-mef-tools
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import statistics
import sys
import time
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "python"))

import mef3io  # noqa: E402

START = 1577836800000000  # 2020-01-01T00:00:00Z


# --------------------------------------------------------------------------- #
def io_counters() -> tuple[int, int]:
    """(bytes read, bytes written) by this process, or (0, 0) off Linux."""
    try:
        d = {}
        for line in open("/proc/self/io"):
            k, v = line.split()
            d[k.rstrip(":")] = int(v)
        return d["rchar"], d["wchar"]
    except OSError:
        return 0, 0


def human(n: float) -> str:
    for unit in ("B", "KB", "MB", "GB", "TB"):
        if abs(n) < 1024 or unit == "TB":
            return f"{n:,.1f} {unit}" if unit != "B" else f"{int(n)} B"
        n /= 1024
    return f"{n:.1f} TB"


def dir_size(path: Path) -> int:
    return sum(f.stat().st_size for f in path.rglob("*") if f.is_file())


def make_block(n: int, ch: int, rng: np.random.Generator) -> np.ndarray:
    """EEG-shaped rather than white noise.

    Compressibility drives the on-disk size and the RED encode cost, so random
    noise would measure the wrong thing — real recordings compress to roughly
    2.5-3 bytes/sample and this does too.
    """
    t = np.arange(n) / 512.0
    sig = (
        40 * np.sin(2 * np.pi * (9.5 + 0.1 * ch) * t)
        + 18 * np.sin(2 * np.pi * 6.0 * t + ch)
        + 9 * np.sin(2 * np.pi * 21.0 * t)
        + rng.normal(0, 7, n)
    )
    return np.round(sig * 10).astype(np.int32)  # 0.1 uV/bit


# --------------------------------------------------------------------------- #
def write_phase(cfg, path: Path) -> dict:
    """Append `block_minutes` of every channel, for `hours`."""
    block_samples = int(cfg.fs * cfg.block_minutes * 60)
    n_blocks = int(round(cfg.hours * 60 / cfg.block_minutes))
    step_us = int(block_samples / cfg.fs * 1e6)
    rng = np.random.default_rng(cfg.seed)
    blocks = [make_block(block_samples, c, rng) for c in range(cfg.channels)]

    times: list[float] = []
    r0, w0 = io_counters()
    t_start = time.perf_counter()

    # ONE writer held open, which is what a real acquisition does and what
    # keeps each append O(new data) instead of re-walking the index.
    w = mef3io.Writer(
        str(path), n_threads=cfg.threads, durability=cfg.durability,
        block_length=cfg.block_length or None,
    )
    try:
        for i in range(n_blocks):
            t0 = time.perf_counter()
            for c in range(cfg.channels):
                w.write_int32(f"ch{c:02d}", blocks[c], 0.1, START + i * step_us, cfg.fs)
            times.append(time.perf_counter() - t0)
            if cfg.progress and (i + 1) % max(1, n_blocks // 20) == 0:
                done = (i + 1) / n_blocks
                elapsed = time.perf_counter() - t_start
                eta = elapsed / done - elapsed
                print(f"    write {done * 100:5.1f}%  "
                      f"{times[-1] * 1000:7.1f} ms/block  eta {eta:5.0f}s", flush=True)
    finally:
        w.close()

    wall = time.perf_counter() - t_start
    r1, w1 = io_counters()
    on_disk = dir_size(path)
    samples = block_samples * cfg.channels * n_blocks

    # Growth compares appends with appends: the first block CREATES the
    # session, so including it would measure create-vs-append, not scaling.
    appends = times[1:] or times
    half = max(1, len(appends) // 4)
    return {
        "blocks": n_blocks,
        "wall_s": wall,
        "samples": samples,
        "on_disk_bytes": on_disk,
        "bytes_per_sample": on_disk / samples,
        "block_first_s": appends[0],
        "block_median_s": statistics.median(appends),
        "block_last_s": appends[-1],
        "growth": statistics.median(appends[-half:]) / statistics.median(appends[:half]),
        "duty_cycle": statistics.median(appends) / (cfg.block_minutes * 60),
        "bytes_read": r1 - r0,
        "bytes_written": w1 - w0,
        "write_amplification": (w1 - w0) / on_disk if on_disk else float("nan"),
        "throughput_MBps": on_disk / 1e6 / wall,
    }


def read_phase(cfg, path: Path) -> dict:
    """Random windows of 5-20 minutes, every channel, as an analysis pass does."""
    rng = np.random.default_rng(cfg.seed + 1)
    total_us = int(cfg.hours * 3600 * 1e6)
    lat: list[float] = []
    samples = 0
    r0, w0 = io_counters()
    t_start = time.perf_counter()

    with mef3io.Reader(str(path), n_threads=cfg.threads) as r:
        channels = r.channels
        for _ in range(cfg.reads):
            span_us = int(rng.uniform(cfg.read_min_minutes, cfg.read_max_minutes) * 60 * 1e6)
            t0 = START + int(rng.uniform(0, max(1, total_us - span_us)))
            mark = time.perf_counter()
            for ch in channels:                       # all channels per window
                samples += len(r.read(ch, t0, t0 + span_us))
            lat.append(time.perf_counter() - mark)

    wall = time.perf_counter() - t_start
    r1, w1 = io_counters()
    on_disk = dir_size(path)
    return {
        "windows": cfg.reads,
        "channels_per_window": len(channels),
        "wall_s": wall,
        "samples": samples,
        "window_median_s": statistics.median(lat),
        "window_p95_s": sorted(lat)[int(0.95 * (len(lat) - 1))],
        "bytes_read": r1 - r0,
        "read_amplification": (r1 - r0) / on_disk if on_disk else float("nan"),
        "Msamples_per_s": samples / 1e6 / wall,
    }


def integrity_phase(path: Path) -> dict:
    """A fast run that produced an inconsistent session is not a fast run."""
    t0 = time.perf_counter()
    report = mef3io.Validator(str(path)).validate()
    validate_s = time.perf_counter() - t0
    t0 = time.perf_counter()
    rec = mef3io.recover_session(str(path))            # dry run
    recover_s = time.perf_counter() - t0
    return {
        "valid": bool(report.ok),
        "findings": [f"{f.check_id}:{f.field}" for f in report.findings][:8],
        "validate_s": validate_s,
        "recover_dry_run_s": recover_s,
        "needs_recovery": [s.action for s in rec.segments],
    }


def mef_tools_write(cfg, path: Path) -> dict:
    """The same append workload through the legacy stack, for comparison."""
    from mef_tools.io import MefWriter

    block_samples = int(cfg.fs * cfg.block_minutes * 60)
    n_blocks = int(round(cfg.hours * 60 / cfg.block_minutes))
    step_us = int(block_samples / cfg.fs * 1e6)
    rng = np.random.default_rng(cfg.seed)
    blocks = [make_block(block_samples, c, rng) for c in range(cfg.channels)]

    times: list[float] = []
    r0, w0 = io_counters()
    t_start = time.perf_counter()
    w = MefWriter(str(path), overwrite=True)
    try:
        for i in range(n_blocks):
            t0 = time.perf_counter()
            for c in range(cfg.channels):
                # reload_metadata=False is the legacy stack's own fast-append
                # switch; leaving it on re-reads the session every block.
                w.write_data(blocks[c], f"ch{c:02d}", START + i * step_us, cfg.fs,
                             precision=1, reload_metadata=False)
            times.append(time.perf_counter() - t0)
    finally:
        del w

    wall = time.perf_counter() - t_start
    r1, w1 = io_counters()
    on_disk = dir_size(path)
    appends = times[1:] or times
    half = max(1, len(appends) // 4)
    return {
        "blocks": n_blocks,
        "wall_s": wall,
        "on_disk_bytes": on_disk,
        "block_median_s": statistics.median(appends),
        "growth": statistics.median(appends[-half:]) / statistics.median(appends[:half]),
        "duty_cycle": statistics.median(appends) / (cfg.block_minutes * 60),
        "bytes_written": w1 - w0,
        "write_amplification": (w1 - w0) / on_disk if on_disk else float("nan"),
        "throughput_MBps": on_disk / 1e6 / wall,
    }


# --------------------------------------------------------------------------- #
def plan(cfg) -> dict:
    block_samples = int(cfg.fs * cfg.block_minutes * 60)
    n_blocks = int(round(cfg.hours * 60 / cfg.block_minutes))
    samples = block_samples * cfg.channels * n_blocks
    raw = samples * 4
    est = int(samples * 1.8)  # RED on EEG-shaped data; measured ~1.55 B/sample
    return {
        "channels": cfg.channels, "fs": cfg.fs, "hours": cfg.hours,
        "block_minutes": cfg.block_minutes, "blocks": n_blocks,
        "samples": samples, "raw_bytes": raw, "estimated_on_disk": est,
    }


def main(argv=None) -> int:
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    p.add_argument("--hours", type=float, default=24.0)
    p.add_argument("--channels", type=int, default=16)
    p.add_argument("--fs", type=float, default=512.0)
    p.add_argument("--block-minutes", type=float, default=10.0,
                   help="data appended per block, per channel (the real cadence)")
    p.add_argument("--block-length", type=int, default=0,
                   help="RED block length in samples (0 = the fs-derived default)")
    p.add_argument("--threads", type=int, default=0,
                   help="mef3io encode/decode threads; 0 = all cores, 1 = single-core "
                        "(the fair comparison against single-threaded meflib)")
    p.add_argument("--durability", choices=["full", "fast"], default="full")
    p.add_argument("--reads", type=int, default=40, help="windowed reads in the read phase")
    p.add_argument("--read-min-minutes", type=float, default=5.0)
    p.add_argument("--read-max-minutes", type=float, default=20.0)
    p.add_argument("--quick-read", action="store_true", help="fewer, smaller read windows")
    p.add_argument("--with-mef-tools", action="store_true",
                   help="also run the write phase through the legacy stack (doubles the disk)")
    p.add_argument("--outdir", default="", help="default: $TMPDIR/mef3io_long_bench")
    p.add_argument("--keep", action="store_true", help="do not delete the session afterwards")
    p.add_argument("--json", default="", help="write results here")
    p.add_argument("--seed", type=int, default=7)
    p.add_argument("--no-progress", dest="progress", action="store_false")
    p.add_argument("--plan", action="store_true", help="print the plan and exit")
    cfg = p.parse_args(argv)
    if cfg.quick_read:
        cfg.reads, cfg.read_min_minutes, cfg.read_max_minutes = 10, 1.0, 5.0

    info = plan(cfg)
    print("=" * 78)
    print(f"LONG SESSION — {cfg.channels} ch x {cfg.hours:g} h @ {cfg.fs:g} Hz, "
          f"{cfg.block_minutes:g}-minute blocks")
    print(f"  {info['blocks']} appends x {cfg.channels} channels, "
          f"{info['samples']:,} samples")
    print(f"  raw {human(info['raw_bytes'])}, estimated on disk ~{human(info['estimated_on_disk'])}")
    print(f"  threads={cfg.threads or 'all'}  durability={cfg.durability}")
    print("=" * 78)
    if cfg.plan:
        return 0

    outdir = Path(cfg.outdir or (os.environ.get("TMPDIR", "/tmp") + "/mef3io_long_bench"))
    outdir.mkdir(parents=True, exist_ok=True)
    need = info["estimated_on_disk"] * (2.4 if cfg.with_mef_tools else 1.2)
    free = shutil.disk_usage(outdir).free
    if free < need:
        print(f"ERROR: need ~{human(need)} free in {outdir}, have {human(free)}.",
              file=sys.stderr)
        print("       Lower --hours/--channels, or point --outdir at a bigger volume.",
              file=sys.stderr)
        return 2

    results = {"config": vars(cfg), "plan": info}
    path = outdir / "long.mefd"
    shutil.rmtree(path, ignore_errors=True)
    try:
        print("\n--- write ---", flush=True)
        results["write"] = write_phase(cfg, path)
        w = results["write"]
        print(f"  {w['wall_s']:.1f}s   {human(w['on_disk_bytes'])} on disk   "
              f"{w['bytes_per_sample']:.2f} B/sample   {w['throughput_MBps']:.1f} MB/s")
        print(f"  per block: first {w['block_first_s'] * 1000:.0f} ms  "
              f"median {w['block_median_s'] * 1000:.0f} ms  "
              f"last {w['block_last_s'] * 1000:.0f} ms  "
              f"GROWTH {w['growth']:.2f}x")
        print(f"  duty cycle {w['duty_cycle'] * 100:.2f}%   "
              f"write amplification {w['write_amplification']:.2f}x   "
              f"(wrote {human(w['bytes_written'])})")

        print("\n--- read (windowed, all channels) ---", flush=True)
        results["read"] = read_phase(cfg, path)
        r = results["read"]
        print(f"  {r['windows']} windows x {r['channels_per_window']} ch   "
              f"median {r['window_median_s'] * 1000:.0f} ms   p95 "
              f"{r['window_p95_s'] * 1000:.0f} ms   {r['Msamples_per_s']:.1f} Msamples/s")
        print(f"  read {human(r['bytes_read'])} of a {human(w['on_disk_bytes'])} session "
              f"= {r['read_amplification']:.3f}x")
        if r["read_amplification"] > 1.0:
            print("  WARNING: read more than the session holds — a read path is not windowed.")

        print("\n--- integrity ---", flush=True)
        results["integrity"] = integrity_phase(path)
        i = results["integrity"]
        print(f"  validate {i['validate_s']:.1f}s -> {'OK' if i['valid'] else 'FAILED'}"
              f"   recovery dry run {i['recover_dry_run_s']:.1f}s -> "
              f"{'nothing to do' if not i['needs_recovery'] else i['needs_recovery']}")
        if not i["valid"]:
            print(f"  findings: {i['findings']}")

        if cfg.with_mef_tools:
            print("\n--- write, legacy mef_tools stack ---", flush=True)
            legacy = outdir / "long_legacy.mefd"
            shutil.rmtree(legacy, ignore_errors=True)
            try:
                results["mef_tools_write"] = mef_tools_write(cfg, legacy)
                m = results["mef_tools_write"]
                print(f"  {m['wall_s']:.1f}s   {human(m['on_disk_bytes'])} on disk   "
                      f"{m['throughput_MBps']:.1f} MB/s")
                print(f"  per block median {m['block_median_s'] * 1000:.0f} ms   "
                      f"GROWTH {m['growth']:.2f}x   duty {m['duty_cycle'] * 100:.2f}%   "
                      f"write amplification {m['write_amplification']:.2f}x")
                print(f"\n  mef3io / mef_tools per-block: "
                      f"{m['block_median_s'] / w['block_median_s']:.2f}x "
                      f"(>1 means mef3io is faster)")
            except Exception as exc:  # a missing oracle should not kill the run
                print(f"  skipped: {type(exc).__name__}: {exc}")
            finally:
                if not cfg.keep:
                    shutil.rmtree(legacy, ignore_errors=True)
    finally:
        if not cfg.keep:
            shutil.rmtree(path, ignore_errors=True)

    if cfg.json:
        Path(cfg.json).write_text(json.dumps(results, indent=2, default=str))
        print(f"\nresults -> {cfg.json}")
    return 0 if results.get("integrity", {}).get("valid") else 1


if __name__ == "__main__":
    sys.exit(main())
