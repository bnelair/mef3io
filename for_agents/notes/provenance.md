# Provenance region

> Read before writing ANY universal header, or adding a write path that edits a segment.
> Split out of CLAUDE.md verbatim; "the private reader" = the private third-party reader named in CLAUDE.md.

- **Provenance region** (`fmt::provenance`, headers.hpp; spec in
  docs/mef3_format.md — a FROZEN format, change only additively): the
  universal header's 64 B discretionary region (offset 960) = magic `M3IO`,
  layout ver, last op code, created-by / last-modified-by versions (20 B
  each), ops-ever mask, modification count (SATURATES, never wraps). NO TIMES
  — the UH is unencrypted and a write time would date a de-identified
  recording. EVERY site that writes a universal header must stamp: fresh
  files `stamp_created`; append `Operation::Append`, repair (both
  `overwrite_universal_header` and the .tmet rewrite) `HeaderRepair`,
  recovery `Recovery`. RULE (core/src/last_segment.hpp): when repair or
  recovery modifies a NON-last segment, it ALSO stamps the channel's LAST
  segment's .tmet with `SegmentRepaired` (code 9; 6 = `Anonymize`, stamped
  on every file of an anonymized copy; 8 = `Transform`, on every file of a
  transform_session copy whose region is mef3io's or empty; 7 stays reserved
  for rename #72 and deliberately unassigned).
  Header-only (region + header CRC), backed up. Always reported, naming the
  file: repair = an Info finding `provenance.last-segment`, recovery =
  `RecoveryReport.notes`. `provenance.last-segment` is NOT a registered check,
  so `Report.summary()` (and the CLI) renders every finding whose id is not in
  `checks_run` in a "Notes:" block — the check loop alone silently dropped it.
  SKIPPED with a note, never an error, on a foreign
  region, a failed CRC, or a writer's `.mef3io-tmp` in the last segment (the
  note names it, says it can be removed if no writer runs, and that cached
  opens will miss the change). NEVER REFUSE a repair on "looks active"
  heuristics: after `cp -r`/extract_session every mtime is fresh (the main
  legacy-repair workflow) and a crash leaves the temp file forever; the race
  is harmless anyway, a concurrent append moves the last segment's sizes.
  C++ `segment_number_of`/`last_segment_dir` and Python cache `_segment_key`
  use the SAME rule (ASCII digits only after the last '-', else -1, also on
  long-long overflow; ties by full folder name; case-sensitive `.segd`/`.timd`
  like the reader) — pinned by core/tests/test_last_segment.cpp and the same
  cases in test_p22.
  Do not reuse HeaderRepair for it: provenance describes what happened to THAT file.
  A future write path that edits a non-last segment must do the same. Tests:
  tests/test_p23_segment_repaired.py. All-zero region = unstamped (every other writer) →
  initialised WITHOUT created-by; any other non-magic bytes = another app's →
  never written. Unknown/reserved bytes are preserved. Bounds are
  static_asserted. Consequence for tests: a repair no longer restores the
  writer's bytes EXACTLY — `_assert_restored_but_for_provenance` (test_p13)
  compares everything outside 0..4 and 960..1024 and requires the rewritten
  file to say header-repair. Read via `SegmentInfo.provenance`; C ABI
  `mef3io_reader_segment_provenance` (NOT new fields in
  `mef3io_segment_info` — callers allocate that struct). Tests:
  `tests/test_p21_writer_stamp.py` + the Catch2 "provenance region" case
  (golden bytes, code table, saturation, hostile version strings, foreign
  region, newer layout).
