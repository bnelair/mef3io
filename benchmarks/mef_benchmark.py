#!/usr/bin/env python3
"""Official read/write benchmark: legacy mef_tools (pymef) vs mef3io vs NWB-Zarr.

Measures, on a synthetic multi-channel recording:
  * write time + resulting on-disk size,
  * metadata open time (no signal decoded),
  * sequential read of the whole recording in fixed-size segments,
  * simulated parallel deep-learning read (a process pool of random-access
    segment reads), with the SAME worker-process count for every backend.

Fairness choices (all configurable):
  * The exact same float32 signal (one on-disk memmap) is fed to every writer.
  * Zarr chunks are aligned to the read segment: time-chunk = one segment,
    one channel per chunk. The DL read window is the same size and aligned to
    the segment grid, so storage-chunk == read-segment == DL-window.
  * The parallel scenario uses processes (like a real DataLoader), the same
    count for all backends, and each worker reader is single-threaded, so the
    parallelism is purely process-level and comparable across backends.
  * MEF uses its native RED block length (idiomatic); override with
    --mef-block-samples to force it equal to the segment if you want.

Fidelity note: MEF stores values quantized to 10**-precision as losslessly
compressed integers; NWB-Zarr here stores float32 (configurable) compressed
with Blosc. These are different fidelities — the size column is reported as-is,
not as an equal-information comparison.

Run inside the active conda env, e.g.:
    python benchmarks/mef_benchmark.py --hours 12 --fs 256 --channels 64
    python benchmarks/mef_benchmark.py --quick        # tiny smoke config
"""
from __future__ import annotations

import argparse
import gc
import json
import os
import shutil
import sys
import time
from concurrent.futures import ProcessPoolExecutor
from dataclasses import asdict, dataclass, field
from pathlib import Path

import numpy as np

MEF3IO_ROOT = Path(__file__).resolve().parent.parent  # mef3io/
sys.path.insert(0, str(MEF3IO_ROOT / "python"))  # mef3io (dev tree)
# legacy mef_tools baseline: prefer a local checkout (original repo); otherwise
# rely on a pip-installed `mef-tools` (`pip install mef3io[bench]`).
for _parent in [MEF3IO_ROOT, *MEF3IO_ROOT.parents]:
    if (_parent / "mef_tools" / "__init__.py").exists():
        sys.path.insert(0, str(_parent))
        break

BASE_UUTC = 1_577_836_800_000_000  # 2020-01-01


# --------------------------------------------------------------------------- #
# Config
# --------------------------------------------------------------------------- #
@dataclass
class Config:
    hours: float = 12.0
    fs: float = 256.0
    channels: int = 64
    precision: int = 3
    segment_minutes: float = 5.0
    parallel_workers: int = 8
    dl_reads: int = 256
    seq_max_segments: int = 0           # 0 = read the whole recording; else cap
    seq_threads: int = 1                # mef3io internal decode threads in the
                                        # sequential scenario (1 = single-thread
                                        # for cross-backend fairness; 0 = all cores)
    nwb_dtype: str = "float32"          # storage dtype for the NWB ElectricalSeries
    mef_block_samples: int = 0          # 0 -> mef3io's fs-derived native block length
    backends: list[str] = field(default_factory=lambda: ["mef_tools", "mef3io", "nwb_zarr"])
    outdir: str = ""                    # default: scratch/<auto>
    keep_files: bool = False
    seed: int = 12345

    @property
    def n_samples(self) -> int:
        return int(round(self.hours * 3600 * self.fs))

    @property
    def segment_samples(self) -> int:
        return int(round(self.segment_minutes * 60 * self.fs))

    @property
    def n_segments(self) -> int:
        return self.n_samples // self.segment_samples


# --------------------------------------------------------------------------- #
# Dataset: generated deterministically on the fly (no giant shared file), so the
# benchmark scales to any size with bounded RAM/disk. Every backend generates
# the SAME values because generation is a pure, sliceable function of
# (channel, sample) — a low-frequency sinusoid plus splitmix64 hash "noise".
# --------------------------------------------------------------------------- #
_AMP = 50.0
_NOISE = 3.0


def _hash_noise(idx: np.ndarray, chan: np.ndarray, seed: int) -> np.ndarray:
    """Deterministic white-ish noise in [-0.5, 0.5] per (chan, sample), vectorized.
    idx: (T,) uint64, chan: (C,) uint64 -> (T, C) float64 via splitmix64 mixing."""
    mask = (1 << 64) - 1
    a = np.uint64(0x9E3779B97F4A7C15)
    b = np.uint64(0xD1B54A32D192ED03)
    s = np.uint64((seed * 0x2545F4914F6CDD1D + 1) & mask)
    x = (idx[:, None] * a) ^ (chan[None, :] * b) ^ s  # (T, C) uint64, wraps mod 2^64
    x ^= x >> np.uint64(30)
    x *= np.uint64(0xBF58476D1CE4E5B9)
    x ^= x >> np.uint64(27)
    x *= np.uint64(0x94D049BB133111EB)
    x ^= x >> np.uint64(31)
    return x.astype(np.float64) / 2.0**64 - 0.5


def gen_block(t_start: int, t_count: int, c_start: int, c_count: int, cfg: Config) -> np.ndarray:
    idx = (t_start + np.arange(t_count)).astype(np.uint64)
    chans = (c_start + np.arange(c_count)).astype(np.uint64)
    t = idx.astype(np.float64) / cfg.fs
    freqs = 8.0 + chans.astype(np.float64) * 0.1
    block = _AMP * np.sin(2 * np.pi * freqs[None, :] * t[:, None])
    block += _NOISE * _hash_noise(idx, chans, cfg.seed)
    return block.astype(np.float32)


def gen_channel(cfg: Config, ch: int) -> np.ndarray:
    """Full channel as float64 (what the MEF writers consume)."""
    return gen_block(0, cfg.n_samples, ch, 1, cfg)[:, 0].astype(np.float64)


# --------------------------------------------------------------------------- #
# Backends. Each provides: availability, write(), a picklable reader `spec`,
# and module-level open/read used both in-process and in worker processes.
# --------------------------------------------------------------------------- #
def channel_name(i: int) -> str:
    return f"ch{i:03d}"


def dir_size(path: str) -> int:
    total = 0
    for root, _, files in os.walk(path):
        for f in files:
            try:
                total += os.path.getsize(os.path.join(root, f))
            except OSError:
                pass
    return total


# ---- write functions -------------------------------------------------------
def write_mef_tools(cfg: Config, path: str):
    from mef_tools.io import MefWriter

    w = MefWriter(path, overwrite=True)
    if cfg.mef_block_samples > 0:
        w.mef_block_len = int(cfg.mef_block_samples)
    for i in range(cfg.channels):
        col = np.ascontiguousarray(gen_channel(cfg, i))
        # Each channel is new (not an append), so skip the O(n^2) metadata reload
        # between channels; reload once on the final write.
        w.write_data(col, channel_name(i), BASE_UUTC, cfg.fs, precision=cfg.precision,
                     reload_metadata=(i == cfg.channels - 1))
        del col
    del w


def write_mef3io(cfg: Config, path: str):
    import mef3io

    with mef3io.Writer(path, overwrite=True) as w:
        if cfg.mef_block_samples > 0:
            w._impl.set_block_length(int(cfg.mef_block_samples))
        for i in range(cfg.channels):
            col = np.ascontiguousarray(gen_channel(cfg, i))
            w.write(channel_name(i), col, BASE_UUTC, cfg.fs, precision=cfg.precision)
            del col


def write_nwb_zarr(cfg: Config, path: str):
    from datetime import datetime, timezone

    import numcodecs
    from hdmf.data_utils import GenericDataChunkIterator
    from hdmf_zarr.backend import ZarrDataIO
    from hdmf_zarr.nwb import NWBZarrIO
    from pynwb import NWBFile
    from pynwb.ecephys import ElectricalSeries

    # Streams the signal in (segment, 1) chunks, generated on demand — no giant
    # in-memory or on-disk array. Chunk == one segment, one channel wide.
    class SignalIterator(GenericDataChunkIterator):
        def __init__(self, cfg: Config):
            self._cfg = cfg
            super().__init__(chunk_shape=(cfg.segment_samples, 1),
                             buffer_shape=(cfg.segment_samples, cfg.channels))

        def _get_data(self, selection):
            tsl, csl = selection
            return gen_block(tsl.start, tsl.stop - tsl.start, csl.start, csl.stop - csl.start,
                             self._cfg).astype(cfg.nwb_dtype)

        def _get_maxshape(self):
            return (self._cfg.n_samples, self._cfg.channels)

        def _get_dtype(self):
            return np.dtype(cfg.nwb_dtype)

    nwb = NWBFile(
        session_description="mef benchmark",
        identifier="mefbench",
        session_start_time=datetime.now(timezone.utc),
    )
    dev = nwb.create_device("sim")
    grp = nwb.create_electrode_group("array", description="sim", location="sim", device=dev)
    for _ in range(cfg.channels):
        nwb.add_electrode(location="sim", group=grp)
    region = nwb.create_electrode_table_region(list(range(cfg.channels)), "all")

    wrapped = ZarrDataIO(
        data=SignalIterator(cfg),
        compressor=numcodecs.Blosc(cname="zstd", clevel=3, shuffle=numcodecs.Blosc.SHUFFLE),
    )
    es = ElectricalSeries("eeg", wrapped, region, starting_time=0.0, rate=cfg.fs)
    nwb.add_acquisition(es)
    with NWBZarrIO(path, "w") as io:
        io.write(nwb)


WRITERS = {"mef_tools": write_mef_tools, "mef3io": write_mef3io, "nwb_zarr": write_nwb_zarr}


# ---- reader open/read (used in-process and in worker processes) ------------
# Kept module-level and dispatched on spec['backend'] so they are picklable.
_G: dict = {}


def open_reader(spec: dict):
    backend = spec["backend"]
    path = spec["path"]
    if backend == "mef_tools":
        from mef_tools.io import MefReader

        return MefReader(path)
    if backend == "mef3io":
        import mef3io

        return mef3io.Reader(path, n_threads=1)  # single-thread: fairness vs processes
    if backend == "nwb_zarr":
        from hdmf_zarr.nwb import NWBZarrIO

        io = NWBZarrIO(path, "r")
        nwb = io.read()
        es = nwb.acquisition["eeg"]
        return {"io": io, "es": es}
    raise ValueError(backend)


def read_window(spec: dict, reader, ch: int, a: int, b: int, t0: int, t1: int,
                n_threads: int | None = None) -> int:
    """Read one channel window; return bytes read (forces materialization).
    `n_threads` (mef3io only) lets the sequential scenario enable internal
    multithreaded block decode."""
    backend = spec["backend"]
    if backend == "mef_tools":
        arr = reader.get_data(channel_name(ch), t0, t1)
        arr = np.asarray(arr)
    elif backend == "mef3io":
        arr = np.asarray(reader.read(channel_name(ch), t0, t1, n_threads=n_threads))
    elif backend == "nwb_zarr":
        arr = np.asarray(reader["es"].data[a:b, ch])
    else:
        raise ValueError(backend)
    return int(arr.astype(np.float64, copy=False).sum() != np.inf) * 0 + arr.nbytes


def read_metadata(spec: dict) -> dict:
    """Open + read channel metadata only (no signal)."""
    backend = spec["backend"]
    if backend == "mef_tools":
        from mef_tools.io import MefReader

        r = MefReader(spec["path"])
        return {"channels": len(r.channels)}
    if backend == "mef3io":
        import mef3io

        r = mef3io.Reader(spec["path"])
        return {"channels": len(r.channels)}
    if backend == "nwb_zarr":
        from hdmf_zarr.nwb import NWBZarrIO

        with NWBZarrIO(spec["path"], "r") as io:
            nwb = io.read()
            es = nwb.acquisition["eeg"]
            return {"shape": tuple(es.data.shape), "rate": float(es.rate)}
    raise ValueError(backend)


# worker-process hooks -------------------------------------------------------
def _worker_init(spec: dict):
    _G["spec"] = spec
    _G["reader"] = open_reader(spec)


def _worker_read(job) -> int:
    ch, a, b, t0, t1 = job
    return read_window(_G["spec"], _G["reader"], ch, a, b, t0, t1)


# --------------------------------------------------------------------------- #
# Scenarios
# --------------------------------------------------------------------------- #
def seg_bounds(cfg: Config, seg: int):
    a = seg * cfg.segment_samples
    b = a + cfg.segment_samples
    t0 = BASE_UUTC + int(round(a / cfg.fs * 1e6))
    t1 = BASE_UUTC + int(round(b / cfg.fs * 1e6))
    return a, b, t0, t1


def bench_open(spec: dict, repeats: int = 5) -> float:
    times = []
    for _ in range(repeats):
        t = time.perf_counter()
        read_metadata(spec)
        times.append(time.perf_counter() - t)
        gc.collect()
    return float(np.median(times))


def bench_sequential(cfg: Config, spec: dict) -> tuple[float, int, int]:
    """Read the recording, all channels, segment by segment, single-thread.
    Optionally cap the number of segments (throughput is scale-invariant)."""
    reader = open_reader(spec)
    n_seg = cfg.n_segments if cfg.seq_max_segments <= 0 else min(cfg.n_segments, cfg.seq_max_segments)
    # mef3io may use internal decode threads here if seq_threads != 1.
    seq_nt = cfg.seq_threads if spec["backend"] == "mef3io" else None
    nbytes = 0
    t = time.perf_counter()
    for seg in range(n_seg):
        a, b, t0, t1 = seg_bounds(cfg, seg)
        for ch in range(cfg.channels):
            nbytes += read_window(spec, reader, ch, a, b, t0, t1, n_threads=seq_nt)
    return time.perf_counter() - t, nbytes, n_seg


def bench_parallel(cfg: Config, spec: dict) -> tuple[float, int, int]:
    """Random-access (channel, segment) reads across a fixed process pool —
    the same worker count for every backend."""
    rng = np.random.default_rng(cfg.seed + 1)
    jobs = []
    for _ in range(cfg.dl_reads):
        ch = int(rng.integers(0, cfg.channels))
        seg = int(rng.integers(0, cfg.n_segments))
        a, b, t0, t1 = seg_bounds(cfg, seg)
        jobs.append((ch, a, b, t0, t1))

    t = time.perf_counter()
    with ProcessPoolExecutor(
        max_workers=cfg.parallel_workers, initializer=_worker_init, initargs=(spec,)
    ) as ex:
        sizes = list(ex.map(_worker_read, jobs, chunksize=max(1, len(jobs) // (cfg.parallel_workers * 4))))
    elapsed = time.perf_counter() - t
    return elapsed, int(sum(sizes)), len(jobs)


# --------------------------------------------------------------------------- #
# Runner
# --------------------------------------------------------------------------- #
def human_bytes(n: float) -> str:
    for unit in ("B", "KB", "MB", "GB", "TB"):
        if n < 1024:
            return f"{n:.1f} {unit}"
        n /= 1024
    return f"{n:.1f} PB"


def run(cfg: Config) -> dict:
    out = Path(cfg.outdir)
    out.mkdir(parents=True, exist_ok=True)
    print(f"Config: {cfg.hours}h @ {cfg.fs}Hz x {cfg.channels}ch, precision={cfg.precision}, "
          f"segment={cfg.segment_minutes}min ({cfg.segment_samples} samples), "
          f"{cfg.n_segments} segments, workers={cfg.parallel_workers}, dl_reads={cfg.dl_reads}")
    in_mb = cfg.n_samples * cfg.channels * 4 / 1024 / 1024
    print(f"Input signal: {cfg.n_samples:,} samples/ch, ~{human_bytes(in_mb * 1024 * 1024)} "
          f"float32 (generated on the fly)\n")

    results: dict[str, dict] = {}
    for backend in cfg.backends:
        if backend not in WRITERS:
            print(f"  ! unknown backend {backend}, skipping")
            continue
        suffix = "mefd" if "mef" in backend else "nwb.zarr"
        path = str(out / f"{backend}.{suffix}")
        if os.path.exists(path):
            shutil.rmtree(path)
        spec = {"backend": backend, "path": path}
        r: dict = {}
        print(f"[{backend}]")
        try:
            t = time.perf_counter()
            WRITERS[backend](cfg, path)
            r["write_s"] = time.perf_counter() - t
            r["size_bytes"] = dir_size(path)
            r["write_MBps"] = in_mb / r["write_s"]
            print(f"  write   {r['write_s']:8.2f}s  ({r['write_MBps']:6.1f} MB/s in)  "
                  f"size {human_bytes(r['size_bytes'])}", flush=True)

            r["open_s"] = bench_open(spec)
            print(f"  open    {r['open_s'] * 1000:8.2f}ms  (metadata, median of 5)", flush=True)

            seq_s, seq_bytes, seq_segs = bench_sequential(cfg, spec)
            r["seq_read_s"] = seq_s
            r["seq_segments"] = seq_segs
            r["seq_MBps"] = seq_bytes / 1024 / 1024 / seq_s
            print(f"  seq     {seq_s:8.2f}s  ({r['seq_MBps']:6.1f} MB/s out, "
                  f"{seq_segs}/{cfg.n_segments} segments)", flush=True)

            par_s, par_bytes, n = bench_parallel(cfg, spec)
            r["par_read_s"] = par_s
            r["par_reads"] = n
            r["par_reads_per_s"] = n / par_s
            r["par_MBps"] = par_bytes / 1024 / 1024 / par_s
            print(f"  par     {par_s:8.2f}s  ({r['par_reads_per_s']:6.1f} reads/s, "
                  f"{r['par_MBps']:6.1f} MB/s, {cfg.parallel_workers} procs)\n", flush=True)
        except Exception as e:  # a backend failing shouldn't kill the run
            import traceback

            r["error"] = f"{type(e).__name__}: {e}"
            print(f"  ERROR: {r['error']}")
            traceback.print_exc()
        results[backend] = r

        # Free disk before the next backend (only one session on disk at a time).
        if not cfg.keep_files and os.path.exists(path):
            shutil.rmtree(path, ignore_errors=True)

    _print_summary(cfg, results)
    payload = {"config": asdict(cfg), "results": results}
    (out / "results.json").write_text(json.dumps(payload, indent=2))
    _write_csv(out / "results.csv", results)
    print(f"\nResults written to {out/'results.json'} and {out/'results.csv'}")

    return results


def _print_summary(cfg: Config, results: dict):
    cols = [
        ("write_s", "write s", "{:.2f}"),
        ("size_bytes", "size", None),
        ("open_s", "open ms", "{:.1f}"),
        ("seq_read_s", "seq s", "{:.2f}"),
        ("seq_MBps", "seq MB/s", "{:.1f}"),
        ("par_reads_per_s", "par rd/s", "{:.1f}"),
        ("par_MBps", "par MB/s", "{:.1f}"),
    ]
    print("=" * 88)
    header = f"{'backend':12}" + "".join(f"{c[1]:>12}" for c in cols)
    print(header)
    print("-" * len(header))
    for backend, r in results.items():
        if "error" in r:
            print(f"{backend:12}  ERROR: {r['error']}")
            continue
        row = f"{backend:12}"
        for key, _, fmt in cols:
            v = r.get(key)
            if v is None:
                cell = "-"
            elif key == "size_bytes":
                cell = human_bytes(v)
            elif key == "open_s":
                cell = fmt.format(v * 1000)
            else:
                cell = fmt.format(v)
            row += f"{cell:>12}"
        print(row)
    print("=" * 88)


def _write_csv(path: Path, results: dict):
    keys = ["write_s", "write_MBps", "size_bytes", "open_s", "seq_read_s", "seq_MBps",
            "par_read_s", "par_reads", "par_reads_per_s", "par_MBps", "error"]
    lines = ["backend," + ",".join(keys)]
    for backend, r in results.items():
        lines.append(backend + "," + ",".join(str(r.get(k, "")) for k in keys))
    path.write_text("\n".join(lines) + "\n")


# --------------------------------------------------------------------------- #
def parse_args() -> Config:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    d = Config()
    p.add_argument("--hours", type=float, default=d.hours)
    p.add_argument("--fs", type=float, default=d.fs)
    p.add_argument("--channels", type=int, default=d.channels)
    p.add_argument("--precision", type=int, default=d.precision)
    p.add_argument("--segment-minutes", type=float, default=d.segment_minutes)
    p.add_argument("--parallel-workers", type=int, default=d.parallel_workers)
    p.add_argument("--dl-reads", type=int, default=d.dl_reads)
    p.add_argument("--seq-max-segments", type=int, default=d.seq_max_segments,
                   help="0 = read whole recording; else cap sequential-read segments")
    p.add_argument("--seq-threads", type=int, default=d.seq_threads,
                   help="mef3io internal decode threads in sequential read "
                        "(1 = single-thread/fair; 0 = all cores)")
    p.add_argument("--nwb-dtype", default=d.nwb_dtype, choices=["float32", "int16", "int32"])
    p.add_argument("--mef-block-samples", type=int, default=d.mef_block_samples,
                   help="0 = mef3io native fs-derived block length")
    p.add_argument("--backends", nargs="+", default=d.backends,
                   choices=["mef_tools", "mef3io", "nwb_zarr"])
    p.add_argument("--outdir", default=d.outdir)
    p.add_argument("--keep-files", action="store_true")
    p.add_argument("--seed", type=int, default=d.seed)
    p.add_argument("--quick", action="store_true",
                   help="tiny smoke config (0.05h, 4ch) overriding size args")
    a = p.parse_args()

    cfg = Config(
        hours=a.hours, fs=a.fs, channels=a.channels, precision=a.precision,
        segment_minutes=a.segment_minutes, parallel_workers=a.parallel_workers,
        dl_reads=a.dl_reads, seq_max_segments=a.seq_max_segments, seq_threads=a.seq_threads,
        nwb_dtype=a.nwb_dtype, mef_block_samples=a.mef_block_samples,
        backends=list(a.backends), outdir=a.outdir, keep_files=a.keep_files, seed=a.seed,
    )
    if a.quick:
        cfg.hours, cfg.channels, cfg.segment_minutes, cfg.dl_reads = 0.05, 4, 0.5, 32
    if not cfg.outdir:
        scratch = os.environ.get("TMPDIR", "/tmp")
        cfg.outdir = str(Path(scratch) / "mef_benchmark")
    return cfg


if __name__ == "__main__":
    run(parse_args())
