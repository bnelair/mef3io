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
  (`written_channels`). C++ `SessionWriter::write_int32_many` /
  `write_float_many`, C ABI `mef3io_writer_write_int32_many`, MATLAB
  `Writer.writeInt32Many` (integer counts only; the float counterpart is
  Python/C++).

## Changed

- Writing a fresh segment copies the encoded blocks once less.
