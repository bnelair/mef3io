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

## `append` — the long-recording workload

The other scenarios write a session once. Real sessions are **extended block by
block for days to months**, and that is a different workload: anything the
append does in `O(total blocks)` becomes quadratic over the recording.

```bash
python benchmarks/mef_benchmark.py --backends mef_tools mef3io \
       --append-chunks 32 --append-minutes 10 --append-channels 8
```

| column | meaning |
|---|---|
| `first` / `median` / `last` | time to append one block across all channels |
| `growth` | `last / first`. **Near 1.00 is the property that matters**: append cost independent of how long the recording already is. Well above 1.00 keeps climbing for the life of the session. |
| `duty` | median append time ÷ wall-clock duration of the data appended. Below 100 % the writer keeps up with a live acquisition. |
| `.tidx` | largest block index on disk at the end |

The first chunk *creates* the session, so it is excluded from `growth` — that
would measure the create/append difference rather than scaling.

`--append-threads` defaults to **1**. meflib and pymef are single-threaded, so
single-core is the honest comparison; pass `0` to use every core.

Expect mef3io to be **slower per append** than `mef_tools` and to **scale
better**. It is paying for durability barriers that the legacy stack does not
have at all (see [docs/long_recordings.md](../docs/long_recordings.md)), while
keeping the append `O(new data)`.

## Run this before publishing

```bash
scripts/verify_local.sh --full-bench
```

Builds, runs every suite, checks bidirectional compatibility and the header
parity ledger against the legacy stack, then benchmarks. It fails loudly if the
oracle (`mef-tools`, `pymef`) is not installed rather than reporting a pass it
cannot justify.

## `long_session_benchmark.py` — the real workload

The other benchmarks write a session once. This one runs the shape these files
actually have: **24 h × 16 channels @ 512 Hz**, written in 10-minute blocks
across all channels, then read back in 5–20 minute windows across all channels.
~708 M samples, a few GB on disk, 144 appends.

Not a unit test — it takes minutes and writes gigabytes. Run it before a
release, or after touching the append path, the index, or anything that could
turn per-append work into per-session work.

```bash
python benchmarks/long_session_benchmark.py --plan      # what it would do
python benchmarks/long_session_benchmark.py
python benchmarks/long_session_benchmark.py --with-mef-tools
python benchmarks/long_session_benchmark.py --hours 2 --quick-read   # short run
```

### The numbers that matter

| metric | meaning |
|---|---|
| **growth** | last-quarter median ÷ first-quarter median append time. **Near 1.00 is the whole point**: append cost independent of how long the recording already is. Anything above keeps climbing for the life of the session. |
| **write amplification** | bytes written ÷ bytes the session gained. Near 1.0 means nothing is being rewritten; a whole-file rewrite shows up here rather than merely as "slower". |
| **read amplification** | bytes read ÷ session size, for the windowed read phase. A windowed read must touch a fraction of the file — at 30 GB per channel, reading everything is fatal. |
| **duty cycle** | append time ÷ wall-clock duration of the data appended. Below 100 % the writer keeps up with a live acquisition. |

It also validates the session and runs `recover_session` as a dry run at the
end, and **exits non-zero if either fails** — a fast run that produced an
inconsistent session is not a fast run.

### Measured (12-core laptop, ext4, default `durability="full"`)

24 h × 16 ch @ 512 Hz, 10-minute blocks:

| | mef3io | mef_tools (legacy) |
|---|---|---|
| write wall clock | **109 s** | 174 s |
| on disk | **1.0 GB** (1.54 B/sample) | 1.8 GB |
| per-block append | **763 ms** | 1230 ms |
| growth | **1.00×** | 1.07× |
| duty cycle | 0.13 % | 0.20 % |
| write amplification | 1.04× | 1.02× |

Read phase: 40 windows × 16 channels, median **89 ms**, p95 144 ms,
62.7 Msamples/s, **read amplification 0.338×**. Validation of the whole session
3.0 s; recovery dry run instant, nothing to do.

mef3io is **1.6× faster per block with full durability barriers the legacy
stack does not have at all**, in a file **1.8× smaller**, with flat growth.
