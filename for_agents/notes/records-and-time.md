# Records, stored time and rto

> Read before touching records read/write, to_stored_time, rto handling or record-file damage handling.
> Split out of CLAUDE.md (verbatim except retargeted cross-references); "the private reader" = the private third-party reader named in CLAUDE.md.

- Pure-Python backend is a stub. Records are READ at all three levels
  (session / channel / segment, each with its own on-disk offset) and every
  mefrec.h body type is decoded read-only (Seiz fully, incl. per-channel
  entries; Epoc, Curs, CSti, ESti, LNTP, UnRc), gated on version 1.0 like
  meflib. Records WRITE builds bodies for Note/SyLg (text) and EDFA
  (duration+text) only; any other 4-char type writes a header with an empty
  body. TIMES ARE UTC (reading always returns UTC). Every written time goes
  through `to_stored_time` (time.hpp): −(t − rto), i.e. −t at rto 0
  (byte-identical to earlier releases) and rto − t in a foreign ANONYMIZED
  session, which keeps the real date hidden (user decision, 2026-10-14). A
  write is REFUSED rather than reveal or misplace a date: t ≤ rto
  (WriteConflictError), a level whose segments disagree on rto
  (WriteConflictError), or a locked section 3 (L1 password only →
  PasswordError; an unknown rto is never treated as 0). New
  segments/channels inherit the agreed rto (own channel first, then the
  session); an unreadable .tmet is skipped and reported
  (`SessionWriter::unreadable_metadata`), never thrown. The reader refuses
  only offset-dependent record files whose segments disagree — a deliberate
  divergence from meflib, which uses the last-read global offset.
  Session-level records are `<session>.rdat` (meflib.c:6335); another name
  only if it is the sole .rdat, else ambiguous. A damaged record file
  (mid-record truncation, declaring MORE records than present — fewer is
  tolerated as in meflib — or a per-record CRC mismatch) fails and is
  contained: strict raises; lenient skips it and lists it in problems() with
  kind "records" (segment_number −1 at session/channel level), samples still
  readable. `write_records` rejects a record carrying a
  payload its type cannot store, so the gap fails loudly instead of silently.
  Cache is Python-level (a C++ warm-start is future).

## Annotation writes APPEND, never replace (#156, 2.0.1)

Maintainer decision, firm: "WE NEED ONLY APPEND. CREATE IF THERE'S NO PREV
FILE. WE CANNOT BE DELETING DATA LIKE THIS." Before 2.0.1 `write_records`
truncated `.rdat`/`.ridx` and wrote only the batch (every call deleted what was
stored; the old docs said "Replaces"). There is NO replace mode, no
truncation and no whole-file rewrite anywhere — do not add one.

- `write_records` (core/src/records.cpp): file absent -> create the pair (a
  batch is written in time order, stable); present -> APPEND in place, in the
  `.tidx` order: record bytes, `.ridx` entries, `.ridx` header, `.rdat` header.
  Body CRCs are EXTENDED (`crc::calculate(new, stored)`), count/max entry
  size/span updated, provenance stamped Append. O(new records): reads two UHs
  plus the last index entry and last record header (rchar/wchar pinned in
  tests/test_p47_annotation_append.py). Failure rolls both files back to
  their sizes/headers.
- The O(1) health check (index whole entries, UH counts == index count, last
  index entry's record ends exactly at EOF, header CRCs) REFUSES a damaged
  pair (FormatError naming recover_session) instead of appending past a
  torn record. Encryption must match the file (WriteConflictError).
- Out-of-order time: appended anyway (file order = write order);
  `Session::read_record_file` stable-sorts by time. The copy engine keeps FILE
  order (ridx matched by offset, merge sorts by `sort_time`), pymef/meflib do
  not assume sorted records (oracle-tested).
- Recovery (`recover_record_files` in recover.cpp): per `.rdat` (root,
  channel, segment): walk whole records (a CRC-failing one is DAMAGE, counted + reported, KEPT, never ends the walk; only bytes after the last whole record may be dropped); healthy = index whole and
  matching every offset + counts right + header CRC ok + nothing after the
  last record. Else keep every whole record, drop the rest (backed up),
  rebuild `.ridx` + both headers. The tail is dropped ONLY when
  provably torn: if the old `.ridx` lists a record AFTER the stopping point
  whose header+body sit whole in the file, or the bytes from the stop to EOF
  are one complete record (CRC matches: damaged length field), both files stay
  byte-identical and a note ("record file damaged, not modified") is the only
  output. The header count is NOT evidence (without barriers it lands first).
  LENIENT READ (`Session::read_record_file`): strict parse fails -> retry with
  `RecordParseInfo::tolerate_damage` + `.ridx` offsets: intact records are
  returned, each damaged one is a `problems()` entry (kind "records", reason
  with byte offset, key rdat@offset); strict unchanged. Appends: stored body CRC 0 stays 0; unset max entry size is measured from the index; session level targets `session_record_file` (ambiguous = refuse); orphan `.ridx` refused (never truncated; recovery notes it); rollback restores both headers. Reported as `RecoveredRecordFile` in
  `RecoveryReport.record_files`. No false positives on legacy files (test).
