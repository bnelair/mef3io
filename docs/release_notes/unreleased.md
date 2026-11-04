# Unreleased

Changes since 2.1.1. This page becomes the next version's notes at release.

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
