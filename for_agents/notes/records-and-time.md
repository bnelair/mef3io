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
