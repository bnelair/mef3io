# Unreleased

Changes merged since the last release. Rewritten into the next version's
notes at release time. These changes target **2.0.0** (breaking).

## Breaking: one thread count per object (migration)

- **The per-call `n_threads` argument of `Reader.read` and `Reader.read_raw`
  is removed** (also on `MultiReader`), with no deprecation period. Set the
  object's count instead, which is now a settable property:

  ```python
  # before
  x = r.read("ch1", t0, t1, n_threads=1)
  # now
  r.n_threads = 1          # from the next call on
  x = r.read("ch1", t0, t1)
  ```

  `cancel=` and `progress=` are keyword-only on the C++ binding's
  `read`/`read_raw` (the Python `Reader` already required that).
- **`n_threads` must be an int >= 0** (`0` = automatic). A bool or float
  raises `TypeError`; for negative values see below.
- **`MultiReader(n_threads=)` is a total**, shared by its recordings and never
  multiplied by them: the open splits it between the recordings it opens at
  once, a read between the recordings it touches, and a read inside one
  recording gets all of it. Before, every recording was opened with the full
  count, so a MultiReader of 10 recordings could run 10 times its budget.
  Setting `MultiReader.n_threads` re-splits from the next call.
- **`n_threads=1` now makes the open serial too**, not just decoding. If you
  passed 1 for comparisons with meflib or pymef, the open on a network share
  is now as slow as before this release's parallel open. Leave the open
  automatic and set `r.n_threads = 1` after opening if you only want serial
  decoding.
- **The default open starts threads**: at least 8 per open pass, also on
  small machines (file access waits on round trips, not on cores). An
  application that opens many Readers from its own thread pool multiplies
  that; pass a small `n_threads` there. If a thread cannot be started (a
  process or container thread limit), the work continues on the threads
  already running, and in the worst case on the calling thread alone. It
  never aborts.
- **Negative counts are errors everywhere**: `ValueError` in Python,
  `std::invalid_argument` in C++ (`Session`, `Reader`, `set_threads`,
  `SessionWriter::set_threads`), `MEF3IO_ERR_ARGUMENT` from the C ABI
  (`mef3io_reader_open`/`_open_ex`, `mef3io_reader_set_threads`,
  `mef3io_writer_set_threads`). Before, a negative count meant "all cores".
- C++: `Reader::read` / `read_raw` take `(channel, t0, t1, ctl)`, and
  `Reader::kUseDefaultThreads` is gone. `Session`'s constructors take an
  optional `n_threads` after `strict`. The constructors taking a
  `shared_ptr<const SessionSource>` are internal (for tests): they skip the
  `.mefd`/`.mefd.tar` naming rule.

See [Threads](../threads.md) for the rule.

## New

- **One thread rule, everywhere.** An object's `n_threads` is how many threads
  it may use for everything it does: opening, file access, decoding,
  decrypting, encoding. `0` = automatic, `1` = no extra threads. "Automatic"
  is decided in one place in the core: hardware concurrency for CPU work, at
  least 8 for file access. It is changeable at runtime as `Reader.n_threads`,
  `MultiReader.n_threads` and `Writer.n_threads` (Python), `set_threads` (C++,
  now atomic), `mef3io_reader_set_threads` / `mef3io_reader_threads` /
  `mef3io_writer_threads` (C ABI; `mef3io_writer_set_threads` existed), and
  `setThreads` / `threads` on the MATLAB `Reader` and `Writer`. A change
  applies from the next call; calls in flight keep their count.
- **Sharing a Reader is safe in every case now** (#110). A `Reader` opened
  with `cache=` builds its backend once, whichever thread asks first (before,
  racing threads each built one). `close()` racing that first build stays
  closed (before, the session could silently reopen). A skipped record file
  or one opened through the old password mapping is warned about once per
  Reader, not once per concurrent call. C++ `set_threads` and
  `set_index_cache_bytes` are atomic, and the metadata loaded at open is read
  without a lock afterwards.


## De-identified copies keep the times unless you ask for a shift

`anonymize_session` no longer requires a shift. **Without one (the new
default) the copy keeps every date and time exactly**, so it still shows when
the recording was made; the patient's identity is removed exactly as before
(names, ID, location, session name, descriptions, annotation text by default,
new identifiers, other programs' private data cleared, provenance
`anonymize`). Give a shift, as before, to hide the dates.

- **Python:** `mef3io.anonymize_session(src, dst, shift_us=None, ...)`,
  `mef3io.anonymize.check(src, dst, shift_us=None, ...)` and
  `mef3io.anonymize.verify(src, dst, shift_us=None, ...)`. `None` (or leaving
  the argument out) = times kept; an int = every time moved by it, with all
  the rules that applied before. `AnonymizeReport.shift_us` is `None` for a
  copy that kept the times, and `summary()` says so.
- **Command line:** `python -m mef3io anonymize SRC DST` now works without
  `--shift-us` / `--reference-date` and keeps the times.
- **C++:** `AnonymizeOptions::shift_us` and `AnonymizeReport::shift_us` are
  `std::optional<si8>` (unset = times kept); `verify_anonymized` takes a
  `std::optional<si8>`; `check_anonymize` lost its `check_shift` parameter
  (the options' shift decides).
- **What a copy that keeps the times holds:** every stored time byte for
  byte as in the source, and each segment's `recording_time_offset` (the
  stored times are read against it, so keeping it is what keeps the times).
  `GMT_offset` and the daylight-saving fields are still cleared: they can say
  where the recording was made, and no time a reader returns depends on them.
  Its verification requires every time (file headers, block index, data
  block headers, annotation headers and index, the times inside
  annotations) and every offset to equal the source's.
- **Only with a shift:** the shift rules (`shift-zero`, `shift-below-floor`,
  `shift-not-positive`), the recording-time-offset refusals
  (`rto-disputed`, `rto-unreadable`, `time-before-rto`), `id-number-time`,
  `time-overflow` and the time leak scan. A copy that keeps the times copies
  each time and offset as stored, so a session with segments that disagree
  on the offset, or with times in the ambiguous legacy convention, can now
  be de-identified without a shift (its times read back exactly as the
  source's).
- **Cuts under a disputed offset** (`time_range`, both `anonymize_session`
  and `transform_session`): each segment, and each annotation file, is now
  placed by the offset ITS file is stored against (a segment's own; for a
  channel or the session, the one all their segments agree on, as the Reader
  reads it). Before, a transform's cut read everything against one
  session-wide offset (0 when segments disagree), which could leave out a
  channel or an annotation inside the window while the copy still passed
  verification. A channel- or session-level annotation file whose time
  depends on an offset its segments disagree on cannot be placed and is
  refused with the new shared code **`time-range-offset-disputed`** (copy it
  without `time_range`).
- **Verification of cuts** now checks every cut segment's header start and
  end against the blocks it holds itself (the validator's time checks stand
  down when a recording-time offset is set), and that every channel left out
  for "no samples in the window" really has none. A standalone
  `verify(..., time_range=)` notes a source channel with samples in the
  window that the copy does not hold (it may have been excluded on purpose).
- **Text callbacks** (`text=callable`) get `info["time"] = None` for a record
  whose time cannot be known: without a shift, a time stored against an
  offset the file's segments disagree on. `AnonymizeReport.
  source_recording_time_offset` is 0 when the segments disagree (there is
  no single one), and a note says so.

### Migration

- **Code that always passes a shift is unaffected**: a shifted copy is made,
  checked and verified exactly as before.
- **Anything that relied on the shift being required** (a script or form
  that treated a missing shift as an error) now makes a copy with the REAL
  dates. If the dates must not be shared, pass `shift_us` (or
  `--reference-date` / `--shift-us`) explicitly.
- **`anonymize.check(src, dst)` without a shift changed meaning.** It used
  to check a shifted copy with the shift still to be chosen; it now checks
  the copy that keeps the times, so it no longer reports the shift-only
  refusals listed above (for example `rto-disputed` or `time-before-rto`). To
  check a shifted copy, pass the shift you will use.
- **`anonymize.verify(src, dst)` without a shift** checks a copy that keeps
  the times; pass the shift to verify a shifted copy (it was required
  before).
- `shift_us=0` is still refused (`shift-zero`); leave the shift out to keep
  the times.
- **`anonymize.redact_text`** takes no shift, so it now follows the copy that
  keeps the times: it no longer raises the shift-only refusals
  (`rto-disputed`, `rto-unreadable`, `time-before-rto`) for such sources. A
  caller that used it as a pre-check for a shifted copy should call
  `anonymize.check(..., shift_us=...)` instead.

## Faster

- **Opening a session is parallel** (#141). The channel listings, the
  segment file checks and every metadata file's read and parse run on the
  Reader's threads. With 20 ms per request (a slow network share, simulated),
  209 channels x 10 segments opens in 10.9 s on 16 threads instead of 173 s on
  one, and 64 x 1 in 0.46 s instead of 6.5 s
  ([benchmarks](../benchmarks.md#the-open-on-several-threads)). The result
  does not depend on the thread count. The channels, metadata, the lenient
  `problems()` (in the same order) and the strict error (the first in listing
  order, as a serial open meets it) are all the same.
- **Windowed reads skip segments outside the window** before reading their
  block index (#110). A segment is skipped when it starts at least a second
  after the window, or when the span its index covers (learnt the first time
  a read walks it) misses the window. Repeated one-second reads of a
  2,400-segment channel went from 3.9 ms to 0.24 ms each.
- **Fixed: `block_envelope` could leave out blocks** of a segment whose
  metadata states an end time earlier than its data. This happens with files
  from writers that store a shorter end, and after an interrupted append that
  was followed by a new segment. It skipped segments by that end time; it now
  uses the rule above, which never relies on a segment's declared end.

- **Opening a session reads each segment's metadata file with one bounded
  read** of its fixed record size. There is no longer a separate seek to find
  the file's size, and padding that other writers append after the record is
  not read. Locally this removes three system calls per segment. On a network
  file system that re-checks file attributes, it saves one request per
  segment: about 6% on a simulated slow mount. Results and messages are
  unchanged. Part of #141.
- **Opening a `.mefd.tar` archive no longer scans the whole archive for every
  folder it lists** (the open lists the root and each channel folder once).
  Folder listings are now indexed once when the archive is opened.
- C++: `Session` and `Reader` can be opened from a `SessionSource`, and
  `SessionSource::read_head(path, n)` reads the first `n` bytes of a file in
  one request.
