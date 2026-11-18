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
  - Password-protected sessions: pass both passwords as for `Writer`; added
    records are level-2 encrypted. Tar archives are refused
    (`tar-read-only`). Cancel and progress work until the swap starts.
  - **A removed annotation cannot be recovered**: there is no undo and no
    history. There is no cross-process write lock: stop other writers of the
    session first (see the [Python guide](../python.md#remove_annotations)).
  - No MATLAB or C API equivalent (like the copies and `rename_session`).
- Record files edited this way carry provenance operation `records-edit`
  (code 10; see [the format reference](../mef3_format.md)).

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
