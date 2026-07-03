# MEF read/write benchmark

`bindings_benchmark.py` measures the Python binding on the examples/08
workload (5 ch × 5 h @ 512 Hz, plain + encrypted); its twin
`matlab/benchmark_mef3io.m` runs the identical workload through the MATLAB
MEX — run both on one machine to compare the bindings (results in
[docs/legacy_comparison.md](../docs/legacy_comparison.md)).

`mef_benchmark.py` compares three MEF-3 / ephys storage backends on the same
synthetic recording:

- **mef_tools** — the legacy pymef-backed wrapper (baseline).
- **mef3io** — the new C++ core.
- **nwb_zarr** — NWB with a Zarr backend (`pynwb` + `hdmf-zarr`), Blosc/zstd.

## What it measures

| Scenario | Meaning |
|---|---|
| `write` | time to write the whole recording + resulting on-disk size |
| `open` | time to open the file and read channel **metadata only** (no signal), median of 5 |
| `seq` | sequential read of the entire recording, all channels, in fixed segments, single-threaded |
| `par` | simulated parallel DL read: random `(channel, segment)` windows served by a **process pool**, the **same worker count for every backend**, one thread per worker |

## Fairness rules baked in

- Every backend writes the **same** float32 signal (one shared on-disk memmap).
- **Zarr chunk == read segment**: the Zarr time-chunk is one segment, one
  channel wide; the DL read window is the same size and aligned to the segment
  grid, so storage-chunk = read-segment = DL-window.
- The parallel scenario uses **processes** (like a real PyTorch DataLoader), the
  same count for all backends; each worker's reader is single-threaded, so the
  parallelism is purely process-level and comparable. (mef3io's internal thread
  pool is therefore *not* used here — this measures per-process decode cost.)
- MEF uses its native RED block length by default; `--mef-block-samples` can
  force it equal to the segment.

## Fidelity caveat (read the size column with this in mind)

MEF stores values quantized to `10**-precision` (default 1e-3) as **losslessly
compressed integers**. NWB-Zarr here stores **float32** (configurable) with
Blosc/zstd. These are different fidelities, so the size comparison is
"what each tool actually does", not equal-information. Set `--nwb-dtype int16`
or `int32` for a closer-to-integer comparison.

## Usage (inside the active conda env)

```bash
# the requested default: 12 h, 256 Hz, 64 channels, precision 3, 5-min segments
python benchmarks/mef_benchmark.py

# quick smoke config (seconds, tiny)
python benchmarks/mef_benchmark.py --quick

# scale it up / tweak
python benchmarks/mef_benchmark.py --hours 24 --fs 512 --channels 128 \
    --segment-minutes 5 --precision 3 --parallel-workers 16 --dl-reads 512 \
    --keep-files --outdir /data/bench

# only some backends
python benchmarks/mef_benchmark.py --backends mef3io nwb_zarr
```

Key knobs: `--hours --fs --channels --precision --segment-minutes
--parallel-workers --dl-reads --nwb-dtype --mef-block-samples --backends
--outdir --keep-files --seed`.

Outputs a summary table to stdout plus `results.json` and `results.csv` in the
output directory. By default the generated files are deleted after the run;
pass `--keep-files` to inspect them.

## Notes on interpreting results

- **Write / open / parallel**: mef3io is designed to win these (C++ encode,
  lazy metadata open, GIL-released decode). The old mef_tools is GIL-bound, so
  the process pool is what lets it scale at all in the parallel scenario.
- **Sequential decode throughput**: RED is CPU-bound to decode; float32 Zarr is
  just decompression, so Zarr can lead on raw MB/s while producing larger,
  lower-fidelity files. This is the core storage tradeoff, shown honestly.
- Disk: the default 12 h × 64 ch generates a ~2.8 GB shared signal plus one
  compressed copy per backend. Ensure the output filesystem has room (use
  `--outdir` to point at a big disk).

## Requirements

Install the benchmark stack (legacy MEF baseline + NWB-Zarr) as an extra:

```bash
pip install "mef3io[bench]"     # mef-tools, pymef, pynwb, hdmf-zarr, zarr, numcodecs, pandas
# or explicitly:
pip install mef-tools pymef pynwb hdmf-zarr zarr numcodecs numpy pandas
```

The scripts prefer a local `mef_tools` checkout when run inside the original
repo (it matches the golden fixtures); otherwise they use the pip-installed
`mef-tools`. Build the `mef3io` extension first with `scripts/dev_build.sh` (dev
tree) or install the package with `pip install mef3io`.
