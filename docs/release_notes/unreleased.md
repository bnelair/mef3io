# Unreleased

Changes merged since the last release. Rewritten into the next version's
notes at release time. These changes target **2.0.0** (breaking).

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

- **Opening a session reads each segment's metadata file with one bounded
  read** of its fixed record size. There is no longer a separate seek to find
  the file's size, and padding that other writers append after the record is
  not read. Locally this removes three system calls per segment. On a network
  file system that re-checks file attributes, it saves one request per
  segment: about 6% on a simulated slow mount. It is a small step: the large
  gain for slow network shares comes from making these requests in parallel,
  which comes next. Results and messages are unchanged. Part of #141.
- **Opening a `.mefd.tar` archive no longer scans the whole archive for every
  folder it lists** (the open lists the root and each channel folder once).
  Folder listings are now indexed once when the archive is opened.
- C++: `Session` and `Reader` can be opened from a `SessionSource`, and
  `SessionSource::read_head(path, n)` reads the first `n` bytes of a file in
  one request.

## Changed — check before upgrading (2.0.0)

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

## Fixed

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

