# Unreleased

Changes merged since the last release. Rewritten into the next version's
notes at release time.

## New

- **`Reader.block_envelope(channel, t0=None, t1=None)`**: the minimum and
  maximum of every data block in a window, from the block index alone. No
  signal is decoded and segments outside the window are skipped, so a viewer
  can draw a whole day as an envelope at the cost of reading the small index
  files. Returns NumPy arrays (`start_uutc`, `end_uutc`, `min`, `max`), scaled
  like `read()`. Python and C++ (`Reader::block_envelope`).
