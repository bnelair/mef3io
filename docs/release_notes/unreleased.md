# Unreleased

Changes merged since the last release. Rewritten into the next version's
notes at release time.

## New

- **`mef3io.MultiReader(paths)`**: several recordings that follow each other
  (a recorder that starts a new session every day) read as ONE, without
  merging them on disk. A drop-in for `Reader`: the same methods, arguments,
  return types and keys. A read equals a `Reader` read of the same recordings
  merged with `merge_sessions`, sample for sample, and touches only the
  recordings in the window (read in parallel). Values the recordings disagree
  on (a patient field, units, a description) read as `None`, with one
  `MultiReaderDifferenceWarning` naming the fields; each recording's own
  values are in `metadata_by_source`. Different sampling rates for a channel,
  or two recordings holding data at the same time, are refused on open.
  Python only.
