# Unreleased

*Draft; not released yet.*

## Reading

- **A `Reader` keeps data files open between calls.** Paging through a
  recording no longer reopens the same `.tdat` for every read: up to
  `open_files` (default 128, fewer where the process may hold few file
  descriptors) read-only files are kept, least recently used first, and read
  with positional reads. Each read first asks the file system for the file's
  size and identity, so an appended file is read to its new end and a file
  that was replaced or removed is never served from the old open file. The
  samples are identical at every setting. Measured (local disk, request
  counts): the 20 reads after the first of a 16-channel viewer window went from
  400 opens + 400 closes to 96 opens + 0 closes.
  New, all optional: `Reader(open_files=)`, `Reader.open_files`,
  `Reader.release_files()`, the same on `MultiReader` (one budget for all its
  recordings). `open_files=0` is the previous behaviour. A tar archive is read
  through one kept file instead of being opened per request.
- Not changed: the first read of a channel still reads the block index of
  every segment of that channel, because skipping a segment needs its index
  (a start or end time in the metadata can be stale; see
  `for_agents/INVARIANTS.md` section 12).

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
