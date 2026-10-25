# Unreleased

Changes merged since the last release. Rewritten into the next version's
notes at release time.

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

