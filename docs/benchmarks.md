# Benchmarks — 2026-10-05

Every benchmark in `benchmarks/` and `examples/08`, rerun on one machine in
one session, against the legacy stack (pymef / mef_tools) and NWB-Zarr.

!!! warning "Read the ratios, not the absolute numbers"
    This machine is **much slower** than the Apple-silicon Mac behind the
    figures in [Performance & legacy comparison](legacy_comparison.md). The
    absolute times are therefore 3–4× longer. Compare libraries within this
    page, and compare across pages only by ratio.

## Machine and conditions

| | |
|---|---|
| CPU | Intel Xeon E5-1650 v2 @ 3.50 GHz (2013, Ivy Bridge-EP): 6 cores / 12 threads |
| Memory | 31 GB |
| Disk | SATA SSD (Apple SM0512G), ext4, Linux 6.8 |
| Software | Python 3.13.16, numpy 2.5.3. **mef3io 1.1.4** (`main` @ `e3f39d4`, the benchmark changes on top). mef-tools 1.2.3, pymef 1.4.8, pynwb 4.2.0, zarr 3.4.0, hdmf-zarr 0.14.0 |

**Background load.** The machine was running an interactive desktop
throughout, so the benchmarks were not alone. A monitor sampled every
process's CPU use every 10 s. Processes other than the benchmark (mostly
gnome-shell, Firefox and htop) used on average:

| benchmark | other processes, mean CPU |
|---|---|
| bindings, compression | 3.5–4.5 threads (few samples; both run under a minute) |
| long session | 1.8 threads |
| access modes | 1.5 threads |
| full comparison + append | 0.7 threads |

Out of 12, this costs the multi-threaded mef3io results the most. Two
benchmarks were also run in an earlier, quieter window, as a cross-check:

- **access modes:** every cell agrees within ~10%;
- **long session:** mef3io agrees within 2%, mef_tools does not (see below).

## Access modes: cold vs warm, directory vs tar, snapshot cache

`benchmarks/access_modes_benchmark.py`. The session is 64 channels × 12 h at
256 Hz, one segment per channel per hour (768 segments, 2.2 GB). Every cell
runs in a fresh process. **Cold** means every session file was evicted from the
OS page cache first (`posix_fadvise(DONTNEED)`; the script verifies bytes
really came off the disk); **warm** means it was read just before. mef3io
decodes on all cores; mef_tools is single-threaded.

| reader | open cold | open warm | first data cold | first data warm | 64 × 5-min windows cold | warm | 8 channels full cold | warm |
|---|---|---|---|---|---|---|---|---|
| mef3io, directory | 193 ms | 142 ms | 198 ms | 174 ms | 564 ms | 428 ms | 4.97 s | 4.76 s |
| mef3io, directory + snapshot | 522 ms | 544 ms | 575 ms | 694 ms | 934 ms | 1.01 s | 5.27 s | 5.31 s |
| mef3io, `.mefd.tar` | 506 ms | 164 ms | 477 ms | 164 ms | 873 ms | 430 ms | 5.26 s | 4.84 s |
| mef3io, `.mefd.tar` + snapshot | 9 ms | 6 ms | 487 ms | 165 ms | 867 ms | 397 ms | 5.21 s | 4.85 s |
| mef_tools, directory | 819 ms | 804 ms | 1.03 s | 747 ms | 2.36 s | 3.45 s | 44.7 s | 42.2 s |
| mef3io, directory written by mef_tools | 200 ms | 173 ms | 191 ms | 165 ms | 610 ms | 440 ms | 4.97 s | 4.72 s |

- **The cache state hardly matters for a directory.** On this SSD, cold is
  within ~30% of warm, so reads are limited by decoding, not by the disk.
- **mef3io vs mef_tools:** 4–6× faster to open, 4–8× on windowed reads,
  about 9× on full reads. It is just as fast on a session mef_tools wrote.
- **A tar archive reads like a directory once open.** A cold *open* is
  ~2.5× slower (0.5 s vs 0.2 s), because the archive's member index has to
  be read from disk. Warm, the two are the same.
- **The snapshot cache helps tar archives and hurts directories.** On a tar,
  `Reader(cache=...)` opens in under 10 ms, because validating the snapshot
  is a single `stat` of the archive. On a directory it is ~2.7× *slower* than
  no cache, cold and warm. Validating there hashes every segment's `.tmet`
  (16 KB) and the start of every `.tidx`, which reads at least as much as a
  plain open, and the JSON parse comes on top. This is a known issue, to be
  fixed separately.
- Packing the 2.2 GB session into a tar took 2.6–4.2 s.

## Long session: the real workload

`benchmarks/long_session_benchmark.py --with-mef-tools`. 24 h × 16 channels at
512 Hz, written as 144 ten-minute appends, then 40 windowed reads across all
channels. mef3io uses `durability="full"` (three fsync barriers per channel
per append) and all cores.

| | mef3io | mef_tools |
|---|---|---|
| write, wall clock | 101 s | 139 s |
| on disk | **1.0 GB** (1.54 B/sample) | 1.8 GB |
| per-block append, median | 708 ms | 970 ms |
| growth (last ÷ first) | 0.99× | 0.86× |
| write amplification | 1.04× | 1.02× |

Read phase (mef3io): median window 91 ms, p95 147 ms, 61.5 M samples/s, read
amplification 0.338×. Validating the whole session takes 3.5 s and finds it
clean; a recovery dry run finds nothing to do.

**The per-block ratio is not a stable number on this machine.** mef3io's
append time was ~700 ms in all three runs: 696, 698 and 708 ms. mef_tools
took 656, 1218 and 970 ms, so the ratio moved between **0.94× and 1.74×** in
mef3io's favour. mef_tools makes no durability barriers, so its time depends
on when the kernel happens to write back its dirty pages. mef3io's barriers
make it pay that cost deterministically.

The stable results are the file size (1.8× smaller) and flat growth.

## Full comparison and append scaling

`benchmarks/mef_benchmark.py` (defaults): 64 channels × 12 h at 256 Hz,
precision 3, 5-minute segments.

- `seq` reads the whole recording single-threaded, segment by segment.
- `par` serves 256 random windows from 8 worker processes, each
  single-threaded.

| backend | write | size | open | seq read | par reads |
|---|---|---|---|---|---|
| mef_tools | 218 s | 2.2 GB | 97.6 ms | 341 s (15.8 MB/s) | 130 reads/s |
| mef3io | **135 s** | 2.2 GB | **18.7 ms** | 362 s (14.9 MB/s) | 164 reads/s |
| NWB-Zarr (float32, Blosc/zstd) | 99 s | 2.0 GB | 175 ms | **30.7 s** (88 MB/s) | **855 reads/s** |

**Read this table for what it isolates: single-threaded decode.** Both
scenarios pin mef3io to one thread per process for fairness. Under that
constraint:

- **mef3io and meflib decode RED at the same speed**, about 2 M samples/s
  per core on this CPU. mef3io's read advantage everywhere else on this page
  comes from decoding blocks in parallel.
- **NWB-Zarr reads ~11× faster per core.** Its chunks are plain zstd, while
  RED is an entropy coder. MEF stores quantised integers losslessly; the Zarr
  store here holds float32. The fidelity differs, so this is not an
  equal-information comparison (see `benchmarks/README.md`).

**Append scaling** (24 × 10 min × 8 channels, mef3io on 1 thread, full
durability):

| | first | median | last | growth |
|---|---|---|---|---|
| mef_tools | 0.375 s | 0.276 s | 0.325 s | 0.87× |
| mef3io | 0.383 s | 0.395 s | 0.385 s | **1.00×** |

On one thread with barriers, mef3io appends are ~1.4× slower than mef_tools
without any. [Long recordings](long_recordings.md#the-durability-knob) breaks down
where that time goes, and when to use `durability="fast"` and threads to beat
the legacy stack instead.

## Compression

`benchmarks/compression_test.py`: 1/f noise, 64 channels × 1 h at 512 Hz.

| store | size | ratio vs raw float32 | quantization |
|---|---|---|---|
| raw float32 | 450.0 MB | 1.00× | — |
| MEF (mef3io), precision 3 | 367.4 MB | 1.22× | 1e-3 |
| MEF (mef3io), precision 2 | 331.9 MB | 1.36× | 1e-2 |
| NWB-Zarr float32 | 379.2 MB | 1.19× | float32 |

## Python binding

`benchmarks/bindings_benchmark.py`: 5 channels × 5 h at 512 Hz
(46.1 M samples), with one NaN gap.

| | write | read | size |
|---|---|---|---|
| plain | 2.16 s (21.4 M samples/s) | 1.41 s (32.7 M samples/s) | 80.2 MB |
| encrypted | 2.08 s (22.2 M samples/s) | 1.40 s (33.0 M samples/s) | 80.2 MB |

Encryption costs nothing, because it covers only metadata. MATLAB could not
be benchmarked on this machine; the
[MATLAB vs Python table](legacy_comparison.md#matlab-vs-python-binding-same-c-core)
comes from the earlier one.

## Legacy compatibility example

`examples/08_legacy_compatibility.py`: 5 channels × 5 h at 512 Hz, written
and read by both stacks.

| | mef_tools | mef3io | speedup |
|---|---|---|---|
| write | 5.70 s | 2.73 s | 2.1× |
| read all data, mef_tools-written file | 11.26 s | 1.21 s | 9.3× |
| read all data, mef3io-written file | 10.89 s | 1.22 s | 8.9× |
| file size | 87.81 MB | 87.81 MB | identical |

Both readers return identical arrays with matching NaN positions on both
writers' files.

## Reproducing

```bash
python benchmarks/bindings_benchmark.py
PYTHONPATH=python python examples/08_legacy_compatibility.py
python benchmarks/compression_test.py
python benchmarks/long_session_benchmark.py --with-mef-tools
python benchmarks/access_modes_benchmark.py
python benchmarks/mef_benchmark.py
```

`pip install "mef3io[bench]"` installs everything these need.
