# Unreleased

*Draft; not released yet.*

## Fixes

- **`mef3io.time_range` is faster.** It looks only at the first and last
  complete segment of each channel (three existence checks, then two metadata
  reads) and no longer touches the other segments at all. Measured on 64
  channels x 12 parts: 3272 -> 1352 system calls in total (-59%). The result
  is the same as before.
- The copy checks, `merge_sessions`, `transform_session` and `Reader` open
  are unchanged: a cheaper way of asking which segment files exist (a folder
  listing) was tried and dropped because it made more system calls in total
  than asking for each file.
