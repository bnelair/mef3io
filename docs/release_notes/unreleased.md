# Unreleased

*Draft; not released yet.*

## Fixes

- **Windowed checks and copies no longer read every part's block index
  (regression in 2.3.0).** `merge.check`, `transform.check` and the copies
  with a time window opened the `.tidx` of every segment of a recording to
  decide that most of them are nowhere near the window. A segment whose
  header times are more than a day away from the window is now skipped
  without reading its index; only the segments that reach the window, and the
  ones within a day of it (a header time can be stale or late), are read.
  Which data is kept is unchanged.
- **Far fewer file-system requests on network drives.** The copy checks list
  each segment folder once instead of asking for every file separately (a
  request per `.tmet`, `.tidx`, `.tdat` and `.rdat` of every segment), and a
  file whose size is needed is opened for reading directly instead of being
  asked about first. `mef3io.time_range` looks at the first and last complete
  segment of each channel only (it used to ask about every segment's files).
  `Reader` and `Session` open as before.
- Results, refusals and the files written are unchanged.
