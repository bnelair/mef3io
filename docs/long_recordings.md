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

### The `durability` knob

**`durability="fast"` is the default.** These files are built by appending for
days to months, so the append is the hot path, and the barriers cost ~2.5x on
it. It matches what every MEF writer before this one did — the reference C
library flushes nothing at all.

```python
w = mef3io.Writer(path)                          # fast: the default
w = mef3io.Writer(path, durability="full")       # add the barriers
```

Be precise about what fast gives up. The `.tdat` and `.tidx` are extended **in
place** (rewriting either whole would be `O(session)`), so this is *not* a
temp-and-rename story for them: without the barriers, a crash mid-append can
leave a torn tail on either file, an index referencing data that never landed,
or blocks in `.tdat` the index never got to mention. Only the `.tmet` is
published by atomic rename.

What makes that an acceptable trade rather than a footgun is that **every one
of those states is detectable and repairable**:

| state after a crash | detected by | repaired by |
|---|---|---|
| index references bytes that never landed | `index.block-offsets` | `recover_session` drops the entries |
| blocks in `.tdat` the index never mentioned | `index.data-coverage` | `recover_session` rebuilds them from the RED headers |
| torn tail on either file | the body CRCs | `recover_session` drops the fragment |
| stale universal-header counts | `header.entry-count` | `recover_session`, then `repair_session` |

So the failure mode is **"run `recover` after an unclean shutdown"**, not "lose
the recording" — and data committed by an *earlier* append is never at risk,
because nothing rewrites it.

Choose `durability="full"` when an unclean shutdown must need no operator
action at all. It makes each append all-or-nothing across the three files.

Measured, 8 channels × 10-minute blocks, against the legacy stack:

| | per append | vs meflib |
|---|---|---|
| mef_tools / meflib (single-threaded, no barriers) | 289 ms | 1.00× |
| mef3io `durability="full"`, 1 thread | 586 ms | 0.49× |
| mef3io `durability="full"`, all cores | 354 ms | 0.82× |
| mef3io `durability="fast"`, 1 thread | 399 ms | 0.73× |
| **mef3io `durability="fast"`, all cores** | **229 ms** | **1.26×** |

The encode dominates, and mef3io parallelises it where meflib cannot — so
`durability="fast"` with threads is *faster* than the reference implementation
while still never tearing a file.

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
