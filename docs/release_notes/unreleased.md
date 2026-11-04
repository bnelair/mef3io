# Unreleased

Changes since 2.1.1. This page becomes the next version's notes at release.

## Added

- **`records="drop"` for `anonymize_session`, `transform_session` and
  `merge_sessions`.** Pass it to leave every annotation out of the copy, at
  the session, channel and segment level. The copy then has no annotation
  files at all, like a recording that never had any; signal data, times,
  channels and details are exactly as without it. The default,
  `records="keep"`, behaves as before, and any other value is refused with
  the code `records-invalid`. The reports gain `records_dropped` (how many
  annotations were left out; `records` is then 0), and `check`/`verify`
  take the same argument: a verification of a dropped copy expects no
  annotations and still fails if it finds any. A channel that held only
  annotations is left out of such a copy (it would be an empty folder). On
  the command line: `--drop-records`.
