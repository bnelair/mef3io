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


## Reading many channels in one call (#90; part of #68)

- **`Reader.read(channels, t0, t1)` takes a list of channel names** and
  returns a 2-D array, one row per name in the order given. Fixes #90. Every
  row is byte-identical to `read()` of that channel alone (same grid, NaN
  gaps, per-segment scaling, warnings, errors); the channels must share one
  sampling rate (`ValueError` otherwise: read each rate in its own call).
  All channels' file reads run at once on the Reader's threads and one decode
  pool works through every block of every channel: with 20 ms per file
  request (a slow share, simulated), 209 channels x 10 minutes read in 1.2 s
  on 16 threads instead of 13.8 s for a loop; locally about 2x faster from 16
  threads, and 3.7x faster than a Python loop with `np.stack`
  ([benchmarks](../benchmarks.md#reading-many-channels-at-once)). `cancel=`
  and `progress=` cover the whole call. Without `t0`/`t1` the window is the
  union of the listed channels' spans.
- **`dtype="float32"`** on `read` (one channel or many): the float64 value
  rounded to the nearest float32, exactly `read(...).astype(numpy.float32)`,
  in half the memory.
- **`block_envelope(channels)`** takes a list too and returns a dict keyed by
  channel name, each value the single-channel dict; the indexes are read
  concurrently, still index only. For #68 (a multi-channel form for viewers).
- **`MultiReader`** has the same arguments and returns what a Reader of the
  merged session returns (it reads the listed channels one after the other,
  within its thread budget). The legacy **`MefReader.get_data(list)`** now uses
  the multi-channel read (one call per sampling rate and window), with the
  same arrays as before.
- **C++:** `Reader::read_many` / `read_many_f32` (returning
  `mef3io::ChannelRows<T>`: one row-major buffer), `Reader::read_f32`,
  `Reader::block_envelope_many`. `read_raw` stays one channel per call. Not in
  the C ABI or MATLAB yet (Python and C++ only).

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

## A kept channel named after the patient is refused before copying

A kept channel whose name holds the patient's name or ID, the recording
location or the original session name (in a form the copy's check refuses:
"Roe" or "Jane_C3" for the patient Jane Roe, "Roe1", "JaneRoe") used to pass
`anonymize.check` and was refused only by the copy's verification, after
every block had been copied (`verify-failed`). It is now refused **before
anything is written**, by `check`, `anonymize_session` and `redact_text`
alike, with the new code **`channel-name-identity`**
(`WriteConflictError`, `details["channel"]` = the channel). The pre-flight
applies the verification's own channel-name rule to each kept channel's
name and to the folder and file names the copy makes from it (#143).
Overlaps that the rule only notes ("12" in "LAH12", "P3", a session called
"EEG" vs "EEG1") still pass. Leave such a channel out with
`exclude_channels`. Transforms are unaffected.

- **Segment numbers are no longer read as the subject ID.** The copy's
  check matched the number in a segment folder's name (`Fz-000001.segd`)
  against the ID's digits, so a subject `PT0001` with more than one segment
  per channel could not be anonymized at all (`verify-failed`). The number
  is mef3io's own counter and is checked on its own; only the channel part
  of those names is searched now, by the check and by the verification.
- **Migration:** code that caught `verify-failed` for this case now gets
  `channel-name-identity`, raised as the same `WriteConflictError`, before
  the copy starts.

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
- **Repeated windowed reads no longer walk every block index** (#110). For
  each index it holds in memory, a Reader remembers the time span the index
  covers, and skips an index that misses the window. The span is forgotten
  with its index, so the result is always the same as walking it.
  Repeated one-second reads of a 2,400-segment channel went from 4.7 ms to
  0.2-0.28 ms each; the first read still walks every index once.
- **Fixed: `block_envelope` could leave out blocks** of a segment whose
  metadata states an end time earlier than its data, or a start later than
  it. This happens with files from writers that store a shorter end, and
  after an interrupted append that was followed by a new segment. It skipped
  segments by those times; no metadata time is used to skip any more.

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

## Changed — check before upgrading (2.0.0)

- **Conversion factors are per segment** (#150). MEF stores the
  units-conversion factor in each segment's section 2, and segments of one
  channel may store different ones (a `new_segment=True` write with another
  `precision`, or a foreign recorder). The API now says so:
  - `Reader.info(ch)["units_conversion_factor"]` (and `MultiReader.info`) is
    `None` when the channel's segments store different factors (meflib's
    channel aggregate reads `NO_ENTRY` there). C++ `ChannelInfo::
    units_conversion_factor` is a `std::optional`; the C ABI's
    `mef3io_channel_info` and MATLAB's `info` give NaN.
  - `Reader.segments(ch)` has each segment's `units_conversion_factor` (C ABI
    `mef3io_reader_segment_units_conversion_factor`, MATLAB `segments`);
    `inspect()`'s segment summaries too, and `ChannelSummary.
    units_conversion_factor` is `None` when they differ.
  - `read_raw()` gains `units_conversion_factors`: which factor applies to
    which samples, as `start_index`/`end_index`/`units_conversion_factor`
    spans. Its `units_conversion_factor` is `None` when the window holds
    samples of segments with different factors (C++ `RawData::
    units_conversion_factor` is a `std::optional`, `RawData::factor_spans`
    the spans; C ABI `mef3io_reader_read_raw_factors`; MATLAB `readRaw`'s
    `units_conversion_factors`, 1-based and inclusive). Code that computed
    `samples * units_conversion_factor` must use the spans, or `read()`.
  - `MultiReader.read_raw()` no longer refuses a window over recordings with
    different factors: it returns the same spans a `Reader` of the merged
    session does.
  - `Session.read_runs()` runs carry `units_conversion_factor` and are split
    where the factor changes.
  - The compat `MefReader`'s `ufact` is `[None]` for such a channel, and
    `get_data` scales each sample by its own segment's factor.
- **A segment whose conversion factor is not set reads as raw counts, with
  a warning; NaN or infinite factors are refused.** 0 is MEF's "no entry"
  for the factor ("not set"). `read()`, `block_envelope()`, `MultiReader`
  and the compat `MefReader.get_data` now return such a segment's samples
  as the **raw stored counts** (factor 1) and emit one new
  `mef3io.UnscaledSamplesWarning` per Reader and channel the first time
  such a segment is read. Before, they read back as all zeros. In a channel
  that mixes such a segment with real factors, every segment is scaled by
  its own (1 for the unset one). `info()`, `segments()` and `read_raw()`'s
  `units_conversion_factors` keep reporting the stored value (0). A factor
  that is NaN or infinite is invalid: `read()` and `block_envelope()` raise
  `mef3io.FormatError` (C ABI `MEF3IO_ERR_FORMAT`) for a window holding such
  samples, before anything is decoded. `read_raw()` and `toc()` still return
  the counts, and a lenient Reader does not count it as a damaged segment.
  The C ABI and MATLAB read unset factors as raw counts too, without a
  warning (C++: `Session::unscaled_channels()`).

  **Such files exist — check yours (migration).** 0 is what meflib's and
  pymef's section-2 templates hold until a writer sets the factor
  (meflib.c:4564; pymef's `MefSession._initialize_tmd2`). So a session
  written through pymef or by a meflib-based recorder that never set it,
  or by mef3io before 2.0 with `write_int32(ufact=0)`, stores 0. mef_tools
  read those as all zeros (`counts * 0`), and so did mef3io before 2.0; now
  they read as the raw counts, with the warning. mef_tools and mef3io's own
  writers never store 0 by default (mef_tools writes `10**-precision`, 1.0
  for precision 0). If you know the real factor, scale the counts yourself:

  ```python
  raw = r.read_raw("ch1", t0, t1)
  x = np.where(raw["valid"] == 1, raw["samples"] * known_factor, np.nan)
  ```

  `r.segments("ch1")` shows which segments store 0
  (`units_conversion_factor`).

- **Invalid sampling frequencies and conversion factors are refused** (#83).
  Every write path (`Writer.write`, `write_int32`, appends, the C ABI and
  MATLAB) now raises `ValueError` (C++ `std::invalid_argument`, C ABI
  `MEF3IO_ERR_ARGUMENT`, a MATLAB error) **before anything is written** when
  the sampling frequency is 0, negative, NaN or infinite, or the conversion
  factor (`ufact`) is 0, NaN or infinite. Before, a rate of 0 or NaN made the
  write never return, and `ufact=0` was stored, after which every sample read
  back as 0: the signal could not be recovered. 0 is also meflib's "no
  entry" value for the conversion factor (meflib.h:439). **A negative
  conversion factor is still accepted**: it is a polarity inversion, and pymef
  handles it explicitly (pymef3_file.c:972-978); mef3io, pymef and mef_tools
  read it back identically (test-pinned). Code that wrote `ufact=0` as a
  placeholder must pass the real factor (or `1.0`).
- **Block length is capped** at 858 993 396 samples (a block's byte size is a
  32-bit field; the worst case is 5 bytes per sample). `Writer(block_length=)`
  above it, or a rate whose default block would be above it (for example
  1e12 Hz without `block_length`), raises `ValueError`. Rates below 0.1 Hz,
  whose default block is less than one sample, now write one sample per
  block instead of never returning. The MATLAB `Writer` checks `BlockLength`
  before it opens (with `Overwrite=true`, opening removes the existing
  session); C callers can do the same with the new `mef3io_check_block_length`
  / `mef3io_check_signal_parameters` (and `MEF3IO_MAX_BLOCK_SAMPLES`). The
  compat `MefWriter.mef_block_len` no longer keeps a refused value.
- A `write_int32` `valid` mask whose length differs from the data raises
  `ValueError` (it was read past its end).
- **Auto-cache file names no longer contain the session name** (#82). The
  `cache="auto"` snapshot is now `<hash>.mef3cache` in the OS cache folder,
  not `<session name>-<hash>.mef3cache`: session folders are often named
  after the subject. The first `cache="auto"` open of a session after
  upgrading deletes that session's old-named snapshot (exactly that file, and
  only if it is a mef3io snapshot) and builds a new one.

## Float writes never wrap a count beyond int32: out-of-range samples are clamped

Before, a float write whose value needed a count outside int32 at the
conversion factor stored a wrapped, wrong value silently (an append of
`3.0e6` to a channel at factor 0.001 read back as `-1294967.296`), and an
infinite value wrapped the same way.

Now every float write (`Writer.write`, `MefWriter.write_data`, C++
`write_float`), with the default precision or `precision="step"`, stores
such a sample, and `+inf`/`-inf`, as the nearest storable value:
`±(2**31 - 1)` counts, meflib's own `RED_POSITIVE_INFINITY` /
`RED_NEGATIVE_INFINITY`, which is also what meflib's encoder clamps to. The
write succeeds; one out-of-range artifact (say a saturation spike in an
append) no longer costs the chunk. The write summary gains `clamped` and
`clamped_infinite` (C++ `WriteSummary::clamped` / `clamped_infinite`), and
a write that clamped anything emits ONE `ClampedSamplesWarning` (a
`MefWarning`; counts only in its text, `details` per channel). `NaN` is
unchanged: a gap, never clamped. If you prefer to mark such samples as
missing, replace them with `NaN` before writing.

The inferred precision now requires both ends of the data to fit (it used
to accept data where only one end overflowed, as mef_tools did), so a
default write of one-sided large data picks a smaller precision instead of
needing a clamp, and `+-inf` no longer drags the inferred precision to 0
(it is ignored there, like NaN). `precision="step"` ignores `+-inf` when
estimating the step too.

## Smaller files for data from an amplifier: `precision="step"` (#81)

`Writer.write(..., precision="step")` (also `MefWriter.write_data`) finds the
data's own quantization step, such as an amplifier's 0.195 µV per count, and
stores the original counts with that step as the conversion factor. One hour
at 512 Hz of 16-bit counts × 0.195: 1.52 MB instead of 6.03 MB with the
default path, the same as `write_int32` of the counts. Opt-in; the default is
unchanged.

- The step is estimated from a fixed-size sample (about 10 ms on 50 million
  samples) and every value is checked during the conversion the write makes
  anyway: a value stored on its step reads back exactly when the data was
  `counts * step`, and within float64 rounding otherwise. The whole write was
  faster than the default one in our measurements (10 million samples:
  0.36 s against 0.57 s).
- Data not on one step (filtered, resampled, noise), with too few distinct
  values to establish one (a DC level, a square wave), or whose counts would
  not fit int32, is written with the default path, with a new
  `StepFallbackWarning`.
- A channel keeps one conversion factor: appends and new segments with
  `"step"` store data on the channel's factor exactly, and refuse
  (`WriteConflictError`, nothing written) data off it rather than round it.
  A default-precision append to a channel whose factor is not a power of ten
  is refused with a message saying to use `"step"` or `write_int32`.
- The write summary has three new keys for every write:
  `units_conversion_factor`, `quantization` (`"step"`, `"decimal"` or
  `"int32"`) and `step_fallback`. C++:
  `SessionWriter::write_float` takes a `Precision` (an `int` still works;
  `Precision::detect_step()` for the step), `WriteSummary` gains the same
  fields, plus `detect_step` / `quantize_to_step`. Not in the C ABI or MATLAB.

## Fixed

- **A channel whose segments store different conversion factors read back
  wrong** (#150). The reader scaled the whole channel by its FIRST segment's
  factor, so after `write(..., precision=2)` and `write(..., precision=4,
  new_segment=True)` the second segment read `999.0` where `9.99` was
  written, without a warning. `read()`, `block_envelope()` and every value
  derived from them now scale each block by its own segment's factor
  (negative factors included); pymef agrees (test-pinned on a session with
  0.01, 0.195 and -0.5 in one channel). Copies (`transform_session` cuts,
  `anonymize_session`, `merge_sessions`) already kept each segment's factor;
  a cut segment with a negative factor now declares its section-2 native
  maximum and minimum the right way round.

- **Appending to a segment folder whose number is not zero-padded**
  (`ch1-5.segd`, as some writers name them) now extends that folder; it used
  to look for `ch1-000005.segd` and refuse. New segments keep the
  `<channel>-NNNNNN` form.
- **Recovery at a non-zero recording-time offset now sets the end time.** At a
  non-zero offset (meflib's date-masking) `repair_session` leaves times alone,
  so after `recover_session` `info()` still reported the end from before the
  recovery. Recovery now corrects it from the last block in the three
  universal headers: only an end that lies before the data's last block (or
  before rebuilt blocks), never moving it earlier, each header in its own
  stored form; it notes what it leaves alone. A healthy session is untouched.
- **Recovery keeps the `.tdat` checksum when it fixes only headers.** It used
  to reset it to "not computed" whenever it rewrote the `.tdat` header.
- `block_envelope()` returned `min` above `max` for a channel with a negative
  conversion factor; the pair is now swapped after scaling, as the section-2
  native extremes already were.
- MATLAB: `Writer.write` / `writeInt32` with a **sparse** array (data or
  `Valid` mask) crashed MATLAB or stored the wrong validity; they now take
  sparse input, and the MEX refuses sparse or complex array arguments with a
  clear error (sparse scalars, such as an element of a sparse matrix, are
  accepted). `writeAnnotations` accepts string-class (`"..."`) `type`, `text` and
  channel.

