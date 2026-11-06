# Unreleased

*Not released yet*

## Fixes

- **A windowed merge no longer drops data when a segment's header end time is
  too early.** When a recording's metadata file said a segment ended before its
  data really did (what an interrupted append or a recorder that writes a
  short end time leaves behind), `merge_sessions(time_ranges=...)` left that
  source's whole window out of the result without any message. A late start
  time in the header did the same, and `transform_session(time_range=...)`
  refused such a source at its final check. Segments are now judged by the
  blocks they hold, as reading does, so the copy holds exactly the samples a
  read of that window returns (#184).
