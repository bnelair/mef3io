---
description: "Recording for days to months in MEF 3.0 with mef3io: appends that cost only the new data, what is flushed, and recovery after a crash."
---

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
The same goes for the recording-time offset a Writer must establish before
the first session-level annotation or new channel: that reads and
CRC-checks every segment's `.tmet` once per Writer (about 1.4–1.8 s and
157 MB at 64 channels × 150 segments) and nothing afterwards.

## Durability, and what it costs

An append updates four things across three files: the `.tdat` body, the `.tidx`
entries, the `.tdat` header, and the `.tmet` declarations. It flushes **exactly
one** of them to stable storage: the `.tmet` is written to a temp file,
flushed, and only then renamed over the previous one. A rename is ordered but
not durable, so without that flush a power cut could publish the new name over
bytes still in cache — a short `.tmet`, which makes the **whole session** fail
to open, not just one segment. That barrier stays, whatever it costs.

Nothing else is flushed. The `.tdat` and `.tidx` are extended **in place**
(rewriting either whole would be `O(session)`), and the headers patched in
place, without a barrier between them. So a power cut or OS crash mid-append
(a crash of only the *process* is harmless — the OS still writes what it was
given) can leave the **last append of a segment** torn:

| state after a power cut | detected by | repaired by |
|---|---|---|
| index references bytes that never landed | `index.block-offsets` | `recover_session` drops the entries |
| blocks in `.tdat` the index never mentioned | `index.data-coverage` | `recover_session` rebuilds them from the RED headers |
| torn tail on either file | the body CRCs | `recover_session` drops the fragment |
| stale universal-header counts | `header.entry-count` | `recover_session`, then `repair_session` |
| the `.tmet` rename lost (old `.tmet` kept) | the checks above | `recover_session` |

So the failure mode is **"run `recover` after an unclean shutdown"**, not "lose
the recording" — and data committed by an *earlier* append is never at risk,
because nothing rewrites it. A fresh write flushes nothing at all: there is
nothing underneath it to lose, and a crash leaves an incomplete session either
way.

!!! note "Changed in 2.0"
    1.x had a `durability=` option (`"fast"`, the default, or `"full"`, which
    also flushed the `.tdat` before the `.tidx` that points at it and the
    `.tidx` before its header). 2.0 removed it: every write takes the path
    above, and passing `durability=` raises `TypeError`. The torn states the
    extra barriers prevented are exactly the ones `recover_session` repairs.

!!! note "Still more than the reference implementation"
    meflib does no durability work at all. Its single `fflush` is explicitly
    *"to update stat structure after write"* — a stdio flush so `fstat` reports
    the right size, not a barrier. There is no `fsync`, no `fdatasync`, no
    `FlushFileBuffers`, and it writes files directly rather than through a
    temp-and-rename, so a power cut can leave it a short `.tmet`.

The cost is one `fdatasync` per channel per append. Measured on ext4, 128
channels × 10-minute blocks at 256 Hz, one `Writer` held open (median of five
appends, three runs each):

| | per append (128 channels) | flushes per channel |
|---|---|---|
| 1.x `durability="full"` | 6.0–6.5 s | 5 (data, index, both headers, `.tmet`) |
| 1.x `durability="fast"` (the old default) | 3.8–3.9 s | 2 (both headers, best-effort) |
| **2.0** | **2.5–2.8 s** | **1** (the `.tmet`) |
| no flush at all (for reference, not shipped) | 0.74 s | 0 |

(1.x's `"fast"` was documented as flushing nothing, but it still flushed the
two headers each append patches, best-effort; 2.0 drops those too.) The
remaining `.tmet` flush is about 15 ms per channel. In the units that matter
it is small: the **duty cycle** — time spent committing divided by the
wall-clock duration of the data committed — is 2.7 s / 600 s ≈ 0.45 % here, so
with 5–20 minute blocks there are two orders of magnitude of headroom.

## Recovering from an interrupted write

```bash
python -m mef3io recover SESSION.mefd            # dry run, writes nothing
python -m mef3io recover SESSION.mefd --apply
```

An interrupted append leaves one of two shapes, and they are **not** treated the
same, because one has lost data and the other has not:

- **Index ahead of data** — entries reference bytes that never landed. Those
  samples do not exist, so the entries are dropped.
- **Data ahead of index** — blocks reached `.tdat` but the index was not
  extended. Those samples *do* exist, so they are **recovered**: a RED block
  header carries the sample count, byte count, start time and discontinuity
  flag, which is everything an index entry needs. Only blocks whose CRC
  verifies are indexed — a torn tail is not a block.

Recovery is deliberately separate from `repair_session`. Repair only ever
rewrites declarations, which is what makes it safe to run on anything; recovery
may change the index, so it is a dry run by default and backs up what it
changes first. The CLI re-derives the declarations afterwards for you.

On a session of hundreds of gigabytes, validation (with exact
`maximum_difference_bytes`) and a recovery that has a long unindexed tail to
verify can take a while. Both accept `cancel=` and `progress=`; a cancel during
the read-only pass leaves the session exactly as it was — see
[Cancelling and progress](python.md#cancelling-and-progress).

!!! warning "It backs up what changes, not the file"
    The `.tdat` may be tens of gigabytes — and that is **one channel**. Recovery
    saves the `.tidx`, the `.tdat`'s 1024-byte header, and any dropped
    sub-block fragment. Copying the data file to undo a header patch would make
    the tool unusable on exactly the sessions it is for.

## Large files are the point

About 30 GB is **one channel**, so a session runs to hundreds of gigabytes or
terabytes. Nothing on a read path may touch a whole `.tdat`:

- a windowed read selects its index entries first, then issues **one** read of
  exactly the byte extent they span;
- validation reads the 1024-byte universal header and the file's *size*, never
  the body;
- recovery streams — the size, plus a 304-byte RED header and the single block
  it describes, at a handful of offsets;
- `tar` archive and extract stream in 1 MB chunks.

`tests/test_p17_large_files.py` pins this with `/proc/self/io`, counting the
bytes actually read. A one-minute window out of a ten-hour channel reads
**0.57 %** of the file; before that gate existed, `read_runs` read 100 %.

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

`Writer(..., n_threads=N)` and `Reader(..., n_threads=N)` set how many
threads each may use (RED encoding; the open and RED decoding): `0` =
automatic, `1` = no extra threads, changeable later through the `n_threads`
property ([Threads](threads.md)). Output is **byte-identical regardless of
thread count**, so threading is purely a speed knob. Use `n_threads=1` when comparing against meflib or pymef, which are
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
