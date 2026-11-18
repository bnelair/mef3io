# Unreleased

*Draft; not released yet.*

## Reading

- **Optional: a `Reader` can keep data files open between calls**
  (`Reader(open_files=N)`, **off by default**: nothing changes unless you ask).
  Paging through a recording then no longer reopens the same `.tdat` for every
  read: up to `N` read-only files are kept, least recently used first, and read
  with positional reads. Each read first asks the file system for the file's
  size and identity, so an appended file is read to its new end and a file that
  was replaced, or deleted and recreated, is never served from the old open
  file. The samples are identical at every setting. Measured (local disk,
  request counts, option on): the 20 reads after the first of a 16-channel
  viewer window went from 400 opens + 400 closes to 96 opens + 0 closes.
  Also new: `Reader.open_files`, `Reader.release_files()`, the same on
  `MultiReader` (one budget for all its recordings). A tar archive is then read
  through one kept file.
  **When to use it:** one or a few Readers rereading the same files on a slow or
  network drive. **What it costs:** descriptors held until `release_files()` /
  `close()` (if the process runs out, the library tries to recover by letting
  go of the kept files and retrying once; best effort); on Windows a folder cannot be renamed or removed, nor a file
  replaced by renaming over it, while files are held; and a single call that
  touches more files than `open_files` reopens them (raise `open_files`).
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
