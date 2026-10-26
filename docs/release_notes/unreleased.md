# Unreleased

Changes merged since the last release. Rewritten into the next version's
notes at release time (this one is planned as 2.0.1).

## Fixed

- **`write_annotations` no longer deletes stored annotations.** Every call used
  to replace the records at its level: the files were truncated and only the
  new batch was written, so each call silently deleted what earlier calls (and
  earlier recordings) had stored, and a power cut in the middle could leave an
  empty or short file. Now `write_annotations` (and the same call in the C and
  MATLAB interfaces, and `MefWriter.write_annotations`) **always appends**: it
  creates the files when there are none, adds the new records in place and
  never replaces or deletes a stored record. The cost of a call no longer
  depends on how many annotations are stored (a 4 MB file and a 50-record file
  both read under 32 KB and write 2.1 KB per appended record).
  - A record **earlier** than the stored ones is appended too (the file keeps
    the order of writing); `records()` returns the records sorted by time,
    equal times in the order written.
  - `recover_session` now also repairs an interrupted annotation append: it
    keeps every whole record (one that fails its CRC is reported, never
    removed), drops only a torn fragment after the last one, and rebuilds the
    `.ridx` and the headers. The dry run lists such files (`record_files`);
    healthy and legacy files are never listed. A damaged record *header*
    (e.g. a corrupt length field) is never mistaken for a torn tail: a tail is
    dropped only if the index exists, has no entry at or beyond the stopping point whose bytes are present and
    non-zero (an index ahead of the data is recovered), and no intact record follows it; otherwise (and always without an
    index) both files are left untouched and a note says so. Until then the next
    `write_annotations` to a torn file is refused, naming `recover_session`.
  - A `strict=False` reader no longer skips a whole annotation file for one
    damaged record: it returns every intact record (resynchronising through
    the `.ridx`) and lists each damaged one in `problems` (kind `"records"`,
    with its byte offset). Strict still raises.
  - **Behaviour change, with migration advice:** code that called
    `write_annotations` twice expecting the second call to *replace* the first
    now keeps both sets. To start with no annotations, write to a new session.
    mef3io no longer offers a way to delete stored annotations.
  - Appends keep a legacy file honest: a stored body CRC of 0 ("never
    computed", mef_tools) stays 0; an unset largest-record size is measured
    from the existing records; session-level records go to the file readers
    use (the sole `.rdat`, whatever its name; several candidates are refused);
    an index without its `.rdat` is never replaced.
  - The order is within each level; legacy readers (mef_tools, pymef) return
    file order, so they show an out-of-order append unsorted.
