# Unreleased

Changes merged since the last release. Rewritten into the next version's
notes at release time.

## Added

- **Multi-channel writes** (#165): `Writer.write_int32_many(channels, data,
  ufact, start_uutc, fs)` and `Writer.write_many(...)` (float, NaN gaps,
  precision per channel) write the same run for many channels in one call:
  byte-identical to a loop of per-channel calls, channels in parallel on the
  Writer's `n_threads`, and ONE combined `.tmet` barrier per call instead of
  one per channel. A failure before the `.tmet` files are published rolls
  every channel back; one while publishing raises `PartialWriteError`
  (`written_channels`, `incomplete_channels`). If a roll-back itself fails,
  `RollbackIncompleteError` names the channels left incomplete (never a
  silent "nothing written"). Refusals (rate, factor, start before the
  stored end) are decided before any file is touched. C++ `SessionWriter::write_int32_many` /
  `write_float_many`, C ABI `mef3io_writer_write_int32_many`, MATLAB
  `Writer.writeInt32Many` (integer counts only; the float counterpart is
  Python/C++).
- **`Writer(flush="close")`** (#166, opt-in): no waits for the disk during
  writes; `close()`/`flush()` does one combined wait. A power cut during
  writing can leave the whole session unreadable and `recover_session` cannot
  be relied on: only for output you rebuild after any failure. Default
  `flush="each"` is unchanged; refused with `wait_for_disk=True` and on a
  session that existed when the Writer opened (earlier recordings are never
  put at risk).

## Changed

- Writing a fresh segment copies the encoded blocks once less.
