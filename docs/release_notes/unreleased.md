# Unreleased

*Draft; not released yet.*

## Fixes

- **Fewer stat requests on network drives (more directory requests locally).** The copy checks list
  each segment folder once instead of asking for every file separately (a
  request per `.tmet`, `.tidx`, `.tdat` and `.rdat` of every segment), and a
  file whose size is needed is opened for reading directly instead of being
  asked about first. `mef3io.time_range` looks at the first and last complete
  segment of each channel only (it used to ask about every segment's files).
  `Reader` and `Session` open as before. A listing costs an open, two
  directory reads and a close where a stat is one call, so on a local disk
  the TOTAL number of system calls is not lower: measured on 64 channels x 12
  parts, `merge.check` and `transform.check` make about a quarter more calls,
  `merge_sessions` about a fifth more, `transform_session` about 4% fewer and
  `time_range` about 47% fewer. The gain is on network drives, where a stat
  is a round trip.
- **A header time is never used to skip a part.** A segment's start/end
  header can be stale or late by any amount (#146), so a copy, its check and
  its verification judge every part by its block index, as in 2.3.0. A
  dangling symlink named like a segment file counts as missing in a
  listing, as for `Reader`; on case-insensitive file systems a name the
  listing lacks is asked of the file system.
- Results, refusals and the files written are unchanged.
