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

## When is my data on disk?

Writing to a file and the data being *on the disk* are two different moments.
The operating system keeps what a program writes in memory and stores it a
little later — usually within half a minute. If the computer loses power in
between, what was still in memory is lost. mef3io lets you choose when to wait
for the disk:

| you call | it returns when | cost (128 channels, 10-minute blocks, ext4) |
|---|---|---|
| `w.write(...)` (default) | the data is handed to the operating system | 2.2–2.3 s per round of 128 appends |
| `w.flush()` | **everything** this `Writer` wrote so far is on disk — one combined wait for all channels | about 0.3 s more per round |
| `w.close()`, or leaving a `with` block | the same as `flush()`, then the `Writer` is closed | about 0.6–0.85 s at the end |
| `Writer(..., wait_for_disk=True)`, then `w.write(...)` | **that write's** data is on disk | 3.6–3.7 s per round (1.x `durability="full"`: 6.0–6.5 s) |

```python
with mef3io.Writer("recording.mefd") as w:   # leaving the block waits for the disk
    for block, t0 in acquisition():
        for ch in channels:
            w.write(ch, block[ch], t0, fs)
        w.flush()        # optional: every channel of this round is on disk now
```

- **Default.** `write()` is as fast as it can be. If the power fails, the most
  recent writes — at most the last half minute or so — may be lost or left
  half-written. Earlier data is never at risk: nothing rewrites it.
- **`flush()` / `close()`.** Call `flush()` whenever you want a point after
  which everything written so far survives a power cut, for example after
  each round of channels. It waits once for all of them together, so it costs
  little more than a single wait. `close()` always does it, and a `with` block
  calls `close()`.
- **`wait_for_disk=True`.** Every `write()` waits for its own data. Use it
  when each call must be safe the moment it returns. It costs one wait per
  call; calling `flush()` once per round is cheaper for the same guarantee at
  the end of the round.
- **After a power cut**, in any mode, run
  `python -m mef3io recover SESSION.mefd --apply`. It puts every half-written
  last append back into a valid session that holds every sample whose bytes
  reached the disk (the table below says what it does in each case).

Two things are flushed even in the default mode, because without them a power
cut could stop the **whole session** from opening:

- every append's `.tmet` (the segment's description), before it replaces the
  old one;
- a channel's **first** segment's `.tmet` (in any session), and a new
  segment's or channel's `.tmet` in a session that already held data when the
  `Writer` opened it.

A failed flush is an error: `write()`, `flush()` and `close()` raise instead
of claiming the data is safe.

## Durability, and what it costs

An append updates four things across three files: the `.tdat` body, the `.tidx`
entries, the `.tdat` header, and the `.tmet` declarations. It flushes **exactly
one** of them to stable storage: the `.tmet` is written to a temp file,
flushed, and only then renamed over the previous one. A rename is ordered but
not durable, so without that flush a power cut could publish the new name over
bytes still in cache — a short `.tmet`, which makes the **whole session** fail
to open, not just one segment. That barrier stays, whatever it costs.

A segment is visible only through its `.tmet`: one that is missing is
skipped, one that is SHORT fails the strict open of the whole session, days of
data included. So a channel's FIRST segment (in any session), and a new
segment or channel in a session that already held data when the `Writer`
opened it, also flush their `.tmet` before it is renamed into place — once per
channel. Their `.tdat` and `.tidx` are not flushed; a short one is rebuilt by
recovery (below). Measured on ext4: a fresh 128-channel session's first write,
2.4–2.5 s (0.85 s with no flush at all); a new segment on all 128 channels of
a reopened session, 2.55 s. Later segments of a session the `Writer` created
itself are not flushed at all (a conversion writes thousands of them);
recovery rebuilds what a power cut takes from them.

`flush()`, `close()` and `wait_for_disk=True` add the rest (see
[When is my data on disk?](#when-is-my-data-on-disk)): `flush()` and `close()`
wait for every file the `Writer` created or extended since the last flush in
ONE combined wait — one `syncfs` per file system on Linux, every file at once
on macOS and Windows; `wait_for_disk=True` does that for the files of each
write before its `.tmet` is published, so the `.tmet` never describes data
that is not on disk.

Nothing else is flushed. The `.tdat` and `.tidx` are extended **in place**
(rewriting either whole would be `O(session)`), and their headers patched in
place, with no barrier between them. So a power cut or OS crash mid-append (a
crash of only the *process* is harmless — the OS still writes what it was
given) can leave the **last append of a segment** in any mix of old and new
bytes. `recover --apply` restores each of them to a valid session holding every
sample whose bytes reached the disk (pinned state by state in
`tests/test_p46_crash_states.py`, built from real old/new file images):

| what reached the disk | what you see before recovery | what `recover --apply` does |
|---|---|---|
| the `.tmet` only (data and index not) — the most likely state on ext4 for some seconds after an append, since the rename is committed before the in-place extensions | the end time covers samples that read as NaN; a recorder re-sending its buffer is refused as overlapping | reports the segment; re-derives its declarations from the index (the validator's own repairs) |
| data and index, not the `.tdat`/`.tidx` header patches | `header.entry-count` (a meflib reader reads the segment short) | rewrites the headers' counts |
| data and index, not the `.tmet` (its rename lost) | the appended samples are invisible | reports the segment; re-derives its declarations |
| `.tidx` entries without their header, a header without its entries, or entries zero-extended (the index fails its CRC) | the appended samples read as NaN; the validator skips the segment | keeps the entries the stored CRC covers, and past them each entry whose RED block header agrees; rebuilds the rest from the CRC-checked blocks in `.tdat` |
| the index, not all of the data (entries past the end, or pointing at pages that read back as zeros) | a read may fail, or return NaN | CRC-checks the last append's blocks, drops the entries from the first bad one, truncates the torn tail (backed up) |
| the data, not the index | the appended samples read as NaN | indexes the CRC-valid blocks from their RED headers |
| part of the data (a torn `.tdat`) | | keeps every whole, CRC-valid block, drops the fragment |
| a later segment's `.tmet` empty or short, its data and index landed (a long-lived `Writer`'s new segment) | the strict open of the whole session fails | rebuilds the `.tmet` from the previous segment's (everything constant along the channel, byte for byte) plus this segment's own files (number, times, level UUID, start sample, native extremes), then re-derives its declarations; backed up, stamped Recovery |
| a first (or any) segment's `.tidx` empty, short or with a zeroed header, its `.tmet` flushed | the segment's read fails | rebuilds the index from the CRC-checked blocks in `.tdat` under a header taken from the `.tmet` (a zeroed `.tdat` header likewise); every whole block is kept |
| a later segment's `.tmet` AND `.tidx` lost, its data landed | the strict open of the whole session fails | rebuilds both: the `.tmet` from the previous segment's, the index from the data |
| a new segment's files empty, its data never landed | the strict open of the whole session fails | removes the segment (it holds no sample; moved to the backup) |

Every segment recovery changes, and every last segment whose `.tmet` disagrees
with its index, then has its declarations re-derived from the index — that
segment only, and exactly the checks the dry run lists — so
recovery alone leaves a session that validates clean, and a recorder can carry
on — or re-send what was lost — from where the data really ends. Data
committed by an *earlier* append is never at risk, because nothing rewrites it.

Limits, stated exactly:

- Within the torn append, blocks that follow a hole (a block that did not land
  while later ones did) are dropped with the torn tail; they are in the backup.
- The block CRC check covers each channel's last append, back to its first
  block and **at most 64 MiB per channel**. Every checked byte is read once
  (pinned with `/proc/self/io`), so a dry run of a healthy session reads up to
  64 MiB per channel when its last segment was written in one call (a
  conversion), and only the last append otherwise (a few hundred KB at
  10-minute appends). Damage further back than that window is not looked for.
- A channel's FIRST segment whose `.tmet` is lost has nothing to be rebuilt
  from. Its `.tmet` is flushed when written, so only another writer's session
  or a disk that lied about a flush gets there: recovery skips the segment and
  says so, and the session opens with `strict=False`.
- Recovery repairs only the segments it reports. A healthy segment's own
  findings (a legacy writer's declarations, for example) are left to
  `python -m mef3io repair`, by explicit check id.
- A write, `flush()` or `close()` FAILS if a flush it waits for fails (1.x's
  default ignored flush errors). On a mount whose flush returns an error, the
  call raises the OS error rather than claim a durability it does not have.
- `wait_for_disk=True` waits once per `write()` call. The calls of a single
  thread come one after another, so one call's wait cannot overlap with the
  next call's work; for one combined wait per round, use `flush()`.

!!! note "Changed in 2.0"
    1.x had a `durability=` option (`"fast"`, the default, or `"full"`, which
    also flushed the `.tdat` before the `.tidx` that points at it and the
    `.tidx` before its header). 2.0 removed it: every write takes the path
    above, and passing `durability=` raises `TypeError`. The torn states the
    extra barriers prevented are the ones in the table above; recovery
    (extended in 2.0 for the ones 1.x could not reach) repairs them.

!!! note "Still more than the reference implementation"
    meflib does no durability work at all. Its single `fflush` is explicitly
    *"to update stat structure after write"* — a stdio flush so `fstat` reports
    the right size, not a barrier. There is no `fsync`, no `fdatasync`, no
    `FlushFileBuffers`, and it writes files directly rather than through a
    temp-and-rename, so a power cut can leave it a short `.tmet`.

The cost is one `fdatasync` per channel per append (Linux; macOS uses
`F_FULLFSYNC`, Windows `FlushFileBuffers` — not measured, and typically slower
per call). Measured on ext4, 128
channels × 10-minute blocks at 256 Hz, one `Writer` held open (median of five
appends, three runs each):

| | per append (128 channels) | flushes per channel |
|---|---|---|
| 1.x `durability="full"` | 6.0–6.5 s | 5 (data, index, both headers, `.tmet`) |
| 1.x `durability="fast"` (the old default) | 3.8–3.9 s | 2 (both headers, best-effort) |
| **2.0**, default | **2.2–2.3 s** | **1** (the `.tmet`) |
| 2.0, default + `flush()` once per round | 2.5–2.6 s | 1, plus one combined wait per round |
| 2.0, `wait_for_disk=True` | 3.6–3.7 s | one combined wait per write (data, index, `.tmet`) |
| no flush at all (for reference, not shipped) | 0.74 s | 0 |

(1.x's `"fast"` was documented as flushing nothing, but it still flushed the
two headers each append patches, best-effort; 2.0 drops those too.) The
remaining `.tmet` flush is about 15 ms per channel. In the units that matter
it is small: the **duty cycle** — time spent committing divided by the
wall-clock duration of the data committed — is 2.3 s / 600 s ≈ 0.4 % here, so
with 5–20 minute blocks there are two orders of magnitude of headroom.

## Recovering from an interrupted write

```bash
python -m mef3io recover SESSION.mefd            # dry run, writes nothing
python -m mef3io recover SESSION.mefd --apply
```

What it does per state is in the table above. Two shapes are **not** treated
the same, because one has lost data and the other has not:

- **Index ahead of data** — entries reference bytes that never landed. Those
  samples do not exist, so the entries are dropped.
- **Data ahead of index** — blocks reached `.tdat` but the index was not
  extended. Those samples *do* exist, so they are **recovered**: a RED block
  header carries the sample count, byte count, start time and discontinuity
  flag, which is everything an index entry needs. Only blocks whose CRC
  verifies are indexed — a torn tail is not a block.

Recovery alone may change the index and the data. Its declaration step goes
through `repair_session`, which only ever rewrites declarations — so the
declarations are fixed by exactly the code the validator's repairs use (stored
images edited, backups, provenance). Recovery is a dry run by default and
backs up what it changes first. It never repairs a segment it does not
report: the dry run lists, per segment, what `--apply` will change.

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
