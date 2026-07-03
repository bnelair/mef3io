#!/usr/bin/env python3
"""File-size / compression-ratio comparison for the SAME stored signal.

Signal: 1/f (pink) noise, per channel, scaled to [-amplitude, +amplitude] float.
Stored three ways and compared by on-disk size and compression ratio vs the raw
float32 input:

  * MEF (mef3io) at precision 3  -> values quantized to 1e-3, RED lossless-int
  * MEF (mef3io) at precision 2  -> values quantized to 1e-2, RED lossless-int
  * NWB-Zarr float32 + Blosc/zstd

Fidelity note: the MEF variants are lossy at the quantization step (1e-3 / 1e-2
of a unit), then losslessly compressed; NWB stores the float32 input essentially
as-is (Blosc is lossless). So a smaller MEF file at precision 2 also means
coarser values — the point of showing both precisions.

Run in the active conda env:
    python benchmarks/compression_test.py --hours 1 --fs 512 --channels 64
"""
from __future__ import annotations

import argparse
import os
import shutil
import sys
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "python"))  # mef3io dev tree

BASE_UUTC = 1_577_836_800_000_000


def human_bytes(n: float) -> str:
    for unit in ("B", "KB", "MB", "GB", "TB"):
        if n < 1024:
            return f"{n:.1f} {unit}"
        n /= 1024
    return f"{n:.1f} PB"


def dir_size(path: str) -> int:
    total = 0
    for root, _, files in os.walk(path):
        for f in files:
            try:
                total += os.path.getsize(os.path.join(root, f))
            except OSError:
                pass
    return total


def pink_noise(n: int, rng: np.random.Generator) -> np.ndarray:
    """1/f (pink) noise of length n: white noise shaped so power ~ 1/f."""
    white = rng.standard_normal(n)
    spectrum = np.fft.rfft(white)
    f = np.fft.rfftfreq(n)
    f[0] = f[1] if len(f) > 1 else 1.0  # avoid div-by-zero at DC
    spectrum = spectrum / np.sqrt(f)    # amplitude ~ 1/sqrt(f)  => power ~ 1/f
    x = np.fft.irfft(spectrum, n=n)
    return x


def generate_signal(n_samples: int, n_channels: int, amplitude: float, seed: int) -> np.ndarray:
    """(T, C) float32 pink noise, each channel scaled to [-amplitude, amplitude]."""
    arr = np.empty((n_samples, n_channels), dtype=np.float32)
    rng = np.random.default_rng(seed)
    for c in range(n_channels):
        x = pink_noise(n_samples, rng)
        x = x / np.max(np.abs(x)) * amplitude
        arr[:, c] = x.astype(np.float32)
    return arr


def channel_name(i: int) -> str:
    return f"ch{i:03d}"


def write_mef(signal: np.ndarray, path: str, fs: float, precision: int):
    import mef3io

    if os.path.exists(path):
        shutil.rmtree(path)
    with mef3io.Writer(path, overwrite=True) as w:
        for i in range(signal.shape[1]):
            col = np.ascontiguousarray(signal[:, i], dtype=np.float64)
            w.write(channel_name(i), col, BASE_UUTC, fs, precision=precision)


def write_nwb(signal: np.ndarray, path: str, fs: float, segment_samples: int):
    from datetime import datetime, timezone

    import numcodecs
    from hdmf_zarr.backend import ZarrDataIO
    from hdmf_zarr.nwb import NWBZarrIO
    from pynwb import NWBFile
    from pynwb.ecephys import ElectricalSeries

    if os.path.exists(path):
        shutil.rmtree(path)
    nwb = NWBFile("compression test", "cmp", datetime.now(timezone.utc))
    dev = nwb.create_device("sim")
    grp = nwb.create_electrode_group("array", description="sim", location="sim", device=dev)
    for _ in range(signal.shape[1]):
        nwb.add_electrode(location="sim", group=grp)
    region = nwb.create_electrode_table_region(list(range(signal.shape[1])), "all")
    wrapped = ZarrDataIO(
        data=signal,
        chunks=(segment_samples, 1),
        compressor=numcodecs.Blosc(cname="zstd", clevel=3, shuffle=numcodecs.Blosc.SHUFFLE),
    )
    es = ElectricalSeries("eeg", wrapped, region, starting_time=0.0, rate=fs)
    nwb.add_acquisition(es)
    with NWBZarrIO(path, "w") as io:
        io.write(nwb)


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--hours", type=float, default=1.0)
    p.add_argument("--fs", type=float, default=512.0)
    p.add_argument("--channels", type=int, default=64)
    p.add_argument("--amplitude", type=float, default=200.0, help="signal range +/- this")
    p.add_argument("--precisions", type=int, nargs="+", default=[3, 2])
    p.add_argument("--segment-minutes", type=float, default=5.0)
    p.add_argument("--outdir", default="")
    p.add_argument("--keep-files", action="store_true")
    p.add_argument("--seed", type=int, default=12345)
    a = p.parse_args()

    n_samples = int(round(a.hours * 3600 * a.fs))
    seg_samples = int(round(a.segment_minutes * 60 * a.fs))
    out = Path(a.outdir or (Path(os.environ.get("TMPDIR", "/tmp")) / "mef_compression"))
    out.mkdir(parents=True, exist_ok=True)

    raw_bytes = n_samples * a.channels * 4  # float32 input reference
    print(f"Signal: 1/f pink noise, {a.hours}h @ {a.fs}Hz x {a.channels}ch, "
          f"range +/-{a.amplitude:g}")
    print(f"        {n_samples:,} samples/ch, raw float32 = {human_bytes(raw_bytes)}\n")
    print("Generating pink-noise signal ...", flush=True)
    signal = generate_signal(n_samples, a.channels, a.amplitude, a.seed)

    rows = []
    for prec in a.precisions:
        path = str(out / f"mef_p{prec}.mefd")
        print(f"Writing MEF precision {prec} ...", flush=True)
        write_mef(signal, path, a.fs, prec)
        size = dir_size(path)
        rows.append((f"MEF (mef3io) p{prec}", size, f"1e-{prec}"))
        if not a.keep_files:
            shutil.rmtree(path, ignore_errors=True)

    print("Writing NWB-Zarr (float32 + zstd) ...", flush=True)
    npath = str(out / "nwb.nwb.zarr")
    write_nwb(signal, npath, a.fs, seg_samples)
    nsize = dir_size(npath)
    rows.append(("NWB-Zarr float32", nsize, "float32"))
    if not a.keep_files:
        shutil.rmtree(npath, ignore_errors=True)

    # --- report ---
    print("\n" + "=" * 74)
    print(f"{'store':22}{'size':>12}{'ratio vs raw':>16}{'quantization':>18}")
    print("-" * 74)
    print(f"{'raw float32 input':22}{human_bytes(raw_bytes):>12}{'1.00x':>16}{'—':>18}")
    for name, size, quant in rows:
        ratio = raw_bytes / size if size else float("nan")
        print(f"{name:22}{human_bytes(size):>12}{ratio:>15.2f}x{quant:>18}")
    print("=" * 74)
    print("ratio = raw float32 input bytes / on-disk bytes (higher = smaller file)")


if __name__ == "__main__":
    main()
