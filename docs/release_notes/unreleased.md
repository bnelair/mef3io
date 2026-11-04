# Unreleased

Changes since 2.1.1. This page becomes the next version's notes at release.

## Added

- **One-call merge pipeline.** `merge_sessions` can now trim each recording to
  its own time range (`time_ranges=[(start, end), None, ...]`, one entry per
  source), make the result anonymous (`anonymize=True` or the options of
  `anonymize_session`: `shift_us`, `pseudonym`, `clear`, `text`), leave
  channels and annotations out, and write one new session, all in a single
  pass over the data. Overlap is judged on the trimmed data. With
  `anonymize`, the identity of every source is removed and searched for in the
  result, patient fields that are cleared may differ between the sources, and
  one shift applies to all of them. The report says which pseudonym and shift
  were used. `check` and `verify` take the same arguments. New refusals:
  `merge-time-ranges-count`, `merge-anonymize-metadata`,
  `merge-threads-invalid`; the single `time_range` is still refused.
- **Faster merges with `n_threads`.** Sources are opened and checked at once,
  channels are copied at the same time, and the final verification is
  parallel too. The output is the same for any thread count. A merge
  also reports the channels it left out for lack of samples in their range
  (`channels_without_samples`), edge blocks it trimmed and annotations it
  left out for lying outside the ranges. A merge now checks overlap on the
  blocks it actually keeps, including those that extend past a range end, and
  an anonymized merge names no source time in any message. Its report keeps
  the source paths in memory only (empty `sources` and `source N` in notes in
  `to_dict()`, pickles and `summary()`; `source_count` says how many), and
  reports from 2.1.1 still load.
- **C++:** the merge declarations moved to `mef3io/merge.hpp`;
  `transform.hpp` and `anonymize.hpp` still include it, so existing code
  compiles unchanged.

- **`records="drop"` for `anonymize_session`, `transform_session` and
  `merge_sessions`.** Pass it to leave every annotation out of the copy, at
  the session, channel and segment level. The copy then has no annotation
  files at all, like a recording that never had any; signal data, times,
  channels and details are exactly as without it. The default,
  `records="keep"`, behaves as before, and any other value is refused with
  the code `records-invalid`. The reports gain `records_dropped` (how many
  annotations of the channels and segments the copy keeps, and of the
  session, were left out; `records` is then 0, and `summary()` says so), and `check`/`verify`
  take the same argument: a verification of a dropped copy expects no
  annotations and still fails if it finds any. A channel that held only
  annotations is left out of such a copy (it would be an empty folder). On
  the command line: `--drop-records`.

## Fixed

- **Joining a recording cut from another one, with notes, could fail its own
  check.** In the released Linux wheels, `merge_sessions` of a copy made by
  `transform_session` (for example a trimmed, password-protected part) with
  another recording failed with "universal header provenance_UUID does not
  name the source file" for the session notes (`.rdat` / `.ridx`) when the
  recording carried session- or channel-level notes. Nothing was published (the
  check refused it), so no data was damaged; a build from source did not show
  it. The merged note files now always name the file they came from, as
  every other copied file does, and a core test covers it.
