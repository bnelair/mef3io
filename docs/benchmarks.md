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
| mef3io, directory + snapshot¹ | 49 ms | 25 ms | 243 ms | 162 ms | 602 ms | 371 ms | 5.13 s | 4.67 s |
| mef3io, `.mefd.tar` | 506 ms | 164 ms | 477 ms | 164 ms | 873 ms | 430 ms | 5.26 s | 4.84 s |
| mef3io, `.mefd.tar` + snapshot¹ | 15 ms | 11 ms | 563 ms | 143 ms | 969 ms | 398 ms | 6.32 s | 4.70 s |
| mef_tools, directory | 819 ms | 804 ms | 1.03 s | 747 ms | 2.36 s | 3.45 s | 44.7 s | 42.2 s |
| mef3io, directory written by mef_tools | 200 ms | 173 ms | 191 ms | 165 ms | 610 ms | 440 ms | 4.97 s | 4.72 s |

- **The cache state hardly matters for a directory.** On this SSD, cold is
  within ~30% of warm, so reads are limited by decoding, not by the disk.
- **mef3io vs mef_tools:** 4–6× faster to open, 4–8× on windowed reads,
  about 9× on full reads. It is just as fast on a session mef_tools wrote.
- **A tar archive reads like a directory once open.** A cold *open* is
  ~2.5× slower (0.5 s vs 0.2 s), because the archive's member index has to
  be read from disk. Warm, the two are the same.
- **The snapshot cache makes the *open* cheap; the first data read still pays
  for the metadata.** A cached open (directory 49/25 ms cold/warm, tar
  15/11 ms) serves `channels` and `info` without reading the session. Reading
  data builds the backend, which reads every segment's metadata as an
  uncached open would. That is why the snapshot rows match the uncached ones
  from "first data" onward.
- ¹ The two snapshot rows come from a re-run after issue #64 was fixed
  (cache format 6). The other rows in that run agreed with the published ones
  within noise. The original "directory + snapshot" row read **522 ms / 544 ms**
  to open, ~2.7× *slower* than no cache. Validation then hashed every
  segment's `.tmet` and the head of every `.tidx`, which is more I/O than the
  open it replaced. Validation now costs the same whatever the number of
  segments: per channel, one listing, two 1 KB header reads and two sizes
  (see [the Python guide](python.md#warm-start-cache-opt-in)).

  Measured on 64 channels with many short segments, each timing in a fresh
  process:

  | segments per channel | snapshot | plain open cold / warm | cached open cold / warm | bytes read, plain / cached |
  |---|---|---|---|---|
  | 150 (9,600 segments) | 60 KB | 3.0 s / 1.26 s | 54 ms / 25 ms | 157 MB / 0.6 MB |
  | 600 (38,400 segments) | 60 KB | 29.6 s / 6.6 s | 108 ms / 95 ms | 629 MB / 0.6 MB |

  The snapshot does not grow with the segment count. The cached open grows
  only with the cost of listing each channel folder.
- Packing the 2.2 GB session into a tar took 2.6–4.2 s.

## Long session: the real workload

`benchmarks/long_session_benchmark.py --with-mef-tools`. 24 h × 16 channels at
512 Hz, written as 144 ten-minute appends, then 40 windowed reads across all
channels. Measured with 1.x `durability="full"` (five flushes per channel per
append; 2.0 flushes only the `.tmet`, so these append times are an upper bound)
and all cores.

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
made it pay that cost deterministically (2.0 keeps one: the `.tmet`).

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

**Append scaling** (24 × 10 min × 8 channels, mef3io on 1 thread, 1.x full
durability):

| | first | median | last | growth |
|---|---|---|---|---|
| mef_tools | 0.375 s | 0.276 s | 0.325 s | 0.87× |
| mef3io | 0.383 s | 0.395 s | 0.385 s | **1.00×** |

On one thread with the 1.x barriers, mef3io appends were ~1.4× slower than
mef_tools without any. [Long recordings](long_recordings.md#durability-and-what-it-costs)
gives the 2.0 cost (one flush per channel per append).

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

Encryption costs nothing, because mef3io encrypts only metadata; it leaves
data blocks unencrypted, which is not the design of MEF 3.0 but an artifact
of past implementations. MATLAB could not
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

## Opening over a slow file system

On a network share every file-system request is a round trip (around
110 ms over SMB in one report), and opening a session makes its requests one
after another: a listing of the session and of each channel folder, three
existence checks per segment, and a read of each segment's metadata file.

Counting the library's own requests is not the same as counting round trips.
Replacing the three existence checks with one listing of each segment folder
halves the library's count, but a listing is several system calls (open,
read the entries, close), where an existence check is one. Measured, it
increased the system calls by about 10%, slowed a local open by 10-20%, and
gained only 12% on a pass-through FUSE file system with 2 ms added per
request. It was not kept.

What was kept is reading each metadata file with one bounded read of its
fixed size. 209 channels x 10 segments, 2026-10-25:

| | before | after |
|---|---|---|
| system calls per open (local disk, strace) | 20,454 | 14,184 |
| FUSE, 2 ms per request, attributes cached 1 s: kernel requests / time | 17,826 / 43.8-48.0 s | 17,822 / 43.4-43.8 s |
| FUSE, 2 ms per request, no attribute caching: kernel requests / time | 47,255 / 115.1-116.5 s | 45,165 / 108.8-110.9 s |

The local saving is the three seeks per segment that sized the file. A file
system that re-checks attributes on a seek to the end saves one request per
segment. The large gain for slow shares comes from making the requests in
parallel.

### The open on several threads

The open now makes those requests on the Reader's threads (`n_threads`; see
[Threads](threads.md)): the channel listings, the segment file checks, and
the read and parse of every metadata file. The number of requests is the same
at every thread count, and so is the result. Measured through the test
suite's counting source, which adds a fixed delay to every library request
(2026-10-25, 12-thread Linux workstation, local disk underneath):

| delay per request | session | requests | 1 thread | 4 threads | 16 threads |
|---|---|---|---|---|---|
| 5 ms | 64 ch x 1 seg | 321 | 1.66 s | 0.42 s | 0.11 s |
| 5 ms | 64 ch x 10 seg | 2,625 | 13.53 s | 3.38 s | 0.86 s |
| 5 ms | 209 ch x 1 seg | 1,046 | 5.44 s | 1.38 s | 0.37 s |
| 5 ms | 209 ch x 10 seg | 8,570 | 44.94 s | 11.24 s | 2.84 s |
| 20 ms | 64 ch x 1 seg | 321 | 6.51 s | 1.67 s | 0.46 s |
| 20 ms | 64 ch x 10 seg | 2,625 | 53.28 s | 13.33 s | 3.39 s |
| 20 ms | 209 ch x 1 seg | 1,046 | 21.21 s | 5.41 s | 1.44 s |
| 20 ms | 209 ch x 10 seg | 8,570 | 173.02 s | 43.29 s | 10.89 s |

The speed-up is close to the thread count (3.9-4.0x on 4 threads, 14-16x on
16), because the delay, not the CPU, is what the open waits on. A real share
answers several requests at once only up to its own limit, so a real speed-up
can be smaller. The automatic count (`n_threads=0`) uses at least 8 threads for
this pass, also on a machine with fewer cores.

On a local disk with 16 threads (same machine), a 1-channel x 3,200-segment directory opens
in about 9 µs per segment instead of 95 µs. A `.mefd.tar` archive opens in
about 33 µs per segment instead of 130 µs. There the archive's member index,
which is read one header after another when the archive is opened, takes most
of the remaining time.

### Reads skip segments outside the window

A windowed read used to walk the block index of every segment of the
channel, even for a one-second window and with every index already in
memory. It now remembers, for each index it has in memory, the time span that
index covers, and does not walk an index whose span misses the window. The
span is forgotten when its index leaves the memory cache, so it always
describes the index a walk would read, and the result is the same as walking
it. Segment metadata times are never used to skip: a declared start or end
can be stale or wrong, and skipping on one loses samples silently. So the
first windowed read of a session still walks every index once, as before.
1 channel x 2,400 segments, 3,000 one-second reads, 1 thread, best of 5 runs
of the loop (2026-10-25): **4.7-4.9 ms per read before, 0.20-0.28 ms after**.
The first read of a freshly opened Reader takes about 45 ms here, because it
walks all 2,400 indexes.

## Reading many channels at once

`Reader.read([...])` reads a window of many channels in one call (#90): every
channel's block index is read at once on the Reader's threads, then the data
a group of channels at a time (as many as the file-access threads), each
group decoded by one pool and written straight into one 2-D array. Measured
2026-10-26 on a 12-thread Linux workstation with an interactive desktop
running (load average 3-4), a 10-minute window at 512 Hz, one segment per
channel, 10-second blocks, warm page cache.

**Through the C++ test suite's counting source**, which adds a fixed delay to
every file request (a slow network share, simulated; local disk
underneath). "loop" reads the channels one after the other with `read()`,
keeping every row; "list" is one `read_many()`. Both make exactly the same
requests (3 per channel):

| delay per request | channels | n_threads | loop | list | speed-up |
|---|---|---|---|---|---|
| none | 64 | 1 | 1.64 s | 1.43 s | 1.2x |
| none | 64 | 4 | 0.46 s | 0.33 s | 1.4x |
| none | 64 | 16 | 0.20 s | 0.11 s | 1.9x |
| none | 209 | 1 | 4.70 s | 4.73 s | 1.0x |
| none | 209 | 4 | 1.36 s | 1.04 s | 1.3x |
| none | 209 | 16 | 0.58 s | 0.32 s | 1.8x |
| 5 ms | 64 | 1 | 2.59 s | 2.55 s | 1.0x |
| 5 ms | 64 | 4 | 1.43 s | 0.61 s | 2.3x |
| 5 ms | 64 | 16 | 1.25 s | 0.19 s | 6.7x |
| 5 ms | 209 | 1 | 8.75 s | 7.79 s | 1.1x |
| 5 ms | 209 | 4 | 4.64 s | 1.81 s | 2.6x |
| 5 ms | 209 | 16 | 4.09 s | 0.58 s | 7.0x |
| 20 ms | 64 | 1 | 5.99 s | 6.02 s | 1.0x |
| 20 ms | 64 | 4 | 4.27 s | 1.29 s | 3.3x |
| 20 ms | 64 | 16 | 4.15 s | 0.37 s | 11.4x |
| 20 ms | 209 | 1 | 18.80 s | 18.56 s | 1.0x |
| 20 ms | 209 | 4 | 13.89 s | 4.16 s | 3.3x |
| 20 ms | 209 | 16 | 13.46 s | 1.20 s | 11.2x |

A loop cannot hide the delay: each channel's requests are made one after the
other whatever the thread count, so with 20 ms per request the loop takes
13.5 s on 16 threads and the list 1.2 s. With one thread the two make the
same requests in the same order. Locally the list is 1.3-1.9x faster from 4
threads on.

**From Python** (`benchmarks/read_many_benchmark.py`, same session shape,
local disk, best of 3). "loop" is `np.stack([r.read(ch, t0, t1) for ch in
channels])`; "pool" the same reads from a pool of n Python threads with the
Reader on one thread (what a caller had to build before); "list" is
`r.read(channels, t0, t1)`:

| channels | n | loop | pool | list | list vs loop | list vs pool |
|---|---|---|---|---|---|---|
| 64 | 1 | 2.01 s | 1.89 s | 1.42 s | 1.4x | 1.3x |
| 64 | 4 | 0.51 s | 0.36 s | 0.37 s | 1.4x | 1.0x |
| 64 | 16 | 0.30 s | 0.18 s | 0.12 s | 2.6x | 1.5x |
| 209 | 1 | 6.43 s | 3.92 s | 3.55 s | 1.8x | 1.1x |
| 209 | 4 | 1.49 s | 1.13 s | 1.04 s | 1.4x | 1.1x |
| 209 | 16 | 1.03 s | 0.67 s | 0.35 s | 2.9x | 1.9x |

The Python loop also pays for `np.stack`, a second copy of the whole window,
which the list read does not make. The rows are byte-identical to the single
reads in every case (the benchmark checks it). Beside the result, a list
read holds the compressed blocks of one group of channels at a time.

## Reproducing

```bash
python benchmarks/bindings_benchmark.py
PYTHONPATH=python python examples/08_legacy_compatibility.py
python benchmarks/compression_test.py
python benchmarks/long_session_benchmark.py --with-mef-tools
python benchmarks/access_modes_benchmark.py
python benchmarks/mef_benchmark.py
# open cost with a delay per library request (C++ test suite, hidden cases)
build-core/mef3io_tests "[.open-bench]"
build-core/mef3io_tests "[.open-bench-threads]"   # the thread-count table
build-core/mef3io_tests "[.read-many-bench]"      # multi-channel read, delay per request
python benchmarks/read_many_benchmark.py          # multi-channel read from Python
```

`pip install "mef3io[bench]"` installs everything these need.
