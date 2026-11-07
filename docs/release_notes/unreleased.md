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

## Faster checks and copies of a short window of a long session

`merge.check`, `transform.check` and the copies themselves no longer make
thousands of small requests when only a short window of a large session is
kept (many channels and segments, e.g. on a network drive). A small block
index is now read in one request instead of about fifteen, a channel's
segments outside the window are looked at only when nothing inside it holds
samples, and each small metadata file is read once per operation. Every source
file is opened at most once per check and per copy. Results and refusals are
unchanged; the copy is byte-for-byte what it was.
