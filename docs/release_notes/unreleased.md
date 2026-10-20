# Unreleased

Changes merged since the last release. Rewritten into the next version's
notes at release time.

## Fixes

- **A read starting exactly half a sample off the sampling grid no longer
  returns a spurious NaN** (#129). At rates where half a sample is a whole
  number of microseconds (500, 250, 1000, 200 Hz...), a window whose start
  fell exactly between two samples (at 500 Hz, any millisecond time ending in
  an odd millisecond) came back with one NaN at the first block boundary.
  The samples before that NaN also sat one place early. Analyses that treat
  NaN as a gap split there. The first sample of such a window is now the one
  at or after its start, and every later block follows it without a hole.
  Windows that start on the grid, or anywhere else off it, read exactly as
  before. All 1.x versions were affected.
