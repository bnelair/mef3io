# Unreleased

*Draft; not released yet.*

## Highlights

- **`mef3io.remove_annotations`: take a stored annotation back (Python and
  C++).** `write_annotations` is unchanged and still only appends; removal is
  a separate, explicitly named call. Name the records to remove by what
  `Reader.records()` returns (level, channel, segment, time, type and every
  body field) and, in the same call, optionally `add=` the corrected ones, so
  changing a note is one rewrite. A selector that matches nothing, more than it
  expects (identical duplicates need `expect=` or `all_copies=True`) or fewer is
  refused with a stable `.code` and **nothing is removed**, across all levels
  of the call.
  - Only the annotation files (`.rdat` + `.ridx`) of the levels named are
    rewritten; signal files and metadata are never opened for writing. Kept
    records are carried over byte for byte (including their encryption level);
    headers are made right and stamped with the new provenance operation
    `records-edit` (code 10). When the last record of a level goes, its two
    files go (the state of a level that never had annotations).
  - Crash safe: the new files are written beside the old ones, flushed and read
    back, then swapped under the session's operation marker, which carries the
    plan. After a crash or power cut at any step the session is either as it
    was or carries the marker: opening it warns (`SessionOperationWarning`),
    writes are refused, the validator reports the new check
    `records.interrupted-edit`, and `recover_session(path, apply=True)`
    finishes the swap (or rolls it back when a new file is not intact; the
    result is reported in `RecoveryReport.record_edits`). The next
    `remove_annotations` does the same first. Unused temporary files are
    swept by `recover_session`.
  - A torn annotation file (bytes after the last counted record, an index that
    disagrees) is refused (`record-torn`), never silently trimmed. While an
    interrupted edit's marker is present, every reader of annotations
    (`Reader`, `MultiReader`, `MefReader.get_annotations`, the C API and so
    MATLAB, C++ `Session::read_records`) raises `edit-interrupted` instead of
    returning a half-swapped level, and `inspect` says the records are not
    reported. A marker whose
    plan cannot be read is settled from the files alone; nothing is ever deleted
    (leftovers move to `<session>.edit-leftovers`).
  - Recovery refuses (`edit-in-progress`) while the program that started the edit is still
    running on the same machine; readers' error says "in progress or was interrupted".
  - Password-protected sessions: pass both passwords as for `Writer`; added
    records are level-2 encrypted. Tar archives are refused
    (`tar-read-only`). Cancel and progress work until the swap starts.
  - **A removed annotation cannot be recovered**: there is no undo and no
    history. There is no cross-process write lock: stop other writers of the
    session first (see the [Python guide](../python.md#remove_annotations)).
  - No MATLAB or C API equivalent (like the copies and `rename_session`).
- Record files edited this way carry provenance operation `records-edit`
  (code 10; see [the format reference](../mef3_format.md)).

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
