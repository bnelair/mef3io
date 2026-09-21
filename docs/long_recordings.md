# Long recordings: days to months

The workload mef3io is built around is not "write a file". It is **a session
extended block by block for a very long time** — an acquisition appending five
to twenty minutes of every channel, over days or months, into one session.

That shapes two things: what has to stay fast, and what has to stay true.

## Why the append path gets special treatment

A segment's metadata section 2 describes **all** of its blocks — the totals, the
maxima, the longest contiguous run. The obvious way to keep those correct on an
append is to recompute them from the block index, which means reading, CRC-ing,
walking and rewriting the whole `.tidx` every time.

That is `O(total blocks)` per append, so **quadratic over the recording**. At one
month, 256 channels and 256 Hz the index is about 14 MB per channel, and
rewriting all of them on every append is gigabytes of pointless I/O — for data
that has not changed since the last append.

mef3io keeps the index summary in memory instead (`AppendIndexCache`), appends
new entries in place, and **extends the body CRC over just the new bytes**. The
MEF CRC (Koopman-32) is a plain rolling state with no final inversion, so
`crc(A‖B) == crc(B, crc(A))` — the `.tdat` path has always relied on this, and
the `.tidx` now does too. An append is `O(new data)`, whatever the session
length.

Measured, one channel, 10-minute appends over a simulated 10 hours:

| | first appends | last appends | growth |
|---|---|---|---|
| before | 38.7 ms | 68.5 ms | **1.77×**, and still climbing |
| after | 41.1 ms | 45.7 ms | **1.11×** |

The first append after a writer opens a segment still does the full walk once —
it has to, since the blocks already on disk are only described there. Every
append after that is incremental. **Keep one `Writer` open** across a long
acquisition rather than reopening it per block, and that cost is paid once.

## Durability, and what it costs

An append updates four things across three files: the `.tdat` body, the `.tidx`
entries, the `.tdat` header, and the `.tmet` declarations. mef3io orders them
with real barriers:

- the `.tdat` body is flushed **before** the `.tidx` that points at those bytes
  is written — otherwise a power cut can leave an index running past the real
  end of the data file, which no in-process rollback can repair;
- the `.tidx` and `.tmet` are written to a temp file, flushed, then renamed, so
  a crash leaves either the old file or the new one, never a torn one.

Two things are deliberately **not** flushed: a fresh write (there is nothing
underneath to lose — a crash leaves an incomplete session either way), and the
directory entry after a rename (losing the rename keeps the previous complete
file, which is worth more than keeping the last append).

!!! note "This is stronger than the reference implementation"
    meflib does no durability work at all. Its single `fflush` is explicitly
    *"to update stat structure after write"* — a stdio flush so `fstat` reports
    the right size, not a barrier. There is no `fsync`, no `fdatasync`, no
    `FlushFileBuffers`, and it writes files directly rather than through a
    temp-and-rename. A session written by the legacy stack has no crash
    guarantee; one written by mef3io does.

The cost is latency, not throughput: roughly three `fdatasync` calls per channel
per append, each a disk round trip. On ext4 that is about 8 ms each, so an
append is ~2.5× slower than one with no barriers — and a fresh write is
unaffected.

In the units that matter it is small. The **duty cycle** — time spent committing
divided by the wall-clock duration of the data committed — is what decides
whether a writer keeps up with a live acquisition:

| channels | block | commit | duty cycle |
|---|---|---|---|
| 64 @ 250 Hz | 60 s | 2.15 s | 3.6 % |
| 128 @ 256 Hz | 10 min | ~5 s | ~0.8 % |

With 5–20 minute blocks there is one to two orders of magnitude of headroom.

## Accuracy at length

Long sessions are also where declarations drift, because every append rewrites
them. Three properties are pinned by tests:

- **Nothing is ever under-declared.** Every buffer-size field a meflib reader
  allocates from is checked after every operation, at every step of
  `tests/test_p14_coherence.py`. Over-declaring costs a reader memory;
  under-declaring truncates the buffer it decodes into.
- **`maximum_difference_bytes` stays exact across a chunked write.** The writer
  carries the running maximum for blocks it encoded itself. Only a segment
  *reopened* from disk — where the earlier blocks' RED headers would cost a seek
  each to read — falls back to meflib's worst-case bound, and that bound is a
  floor, never a lowering.
- **Appending repairs, never degrades.** Extending a segment written by an older
  mef3io, or by the legacy stack, brings its contiguous maxima up to date in
  passing. The legacy writer's own output carries
  `maximum_contiguous_samples = 0`; one mef3io append fixes it without moving a
  sample.

## Segments

MEF segments exist to bound exactly this. Rotating — `new_segment=True`, say
once a day — keeps each `.tidx` bounded and each segment independently
verifiable, which also limits the blast radius of any single corrupted file.
With the incremental append above it is no longer needed *for speed*, but it is
still good operational practice for a recording that runs for months.

```python
w.write_int32(ch, block, ufact, t, fs, new_segment=start_of_new_day)
```

## Threads

`Writer(..., n_threads=N)` and `Reader(..., n_threads=N)` control the RED
encode/decode pool: `0` uses every core, `1` is single-threaded. Output is
**byte-identical regardless of thread count**, so threading is purely a speed
knob. Use `n_threads=1` when comparing against meflib or pymef, which are
single-threaded — the benchmark defaults to it for the append scenario for
exactly that reason.

## Verifying a build before you publish

```bash
scripts/verify_local.sh              # suites, oracle parity, quick benchmark
scripts/verify_local.sh --full-bench # ...with the full benchmark
```

It builds the extension, confirms the in-tree build is the one under test (not
an installed wheel), runs the C++ and Python suites, and then the two gates that
matter most for this format:

- **`tests/test_p15_oracle_acceptance.py`** — bidirectional compatibility with
  `mef_tools` → `pymef` → `meflib`, including the mixed sequences where one
  stack modifies the other's session.
- **`tests/test_p16_header_parity.py`** — the **known-divergence ledger**. It
  writes the same signal with both stacks and compares *every* modelled header
  field. Each difference must be either zero or listed in `KNOWN_DIVERGENCES`
  with a written reason; anything else fails the run.

That second gate is the one that would have caught the bug this design note
exists because of. `maximum_difference_bytes = 0` was invisible to every
round-trip test — mef3io read its own files perfectly, and so did pymef —
because nothing compared the *declarations* against the oracle's. If the oracle
is not installed, the script says so and exits non-zero rather than reporting a
pass it cannot justify.
