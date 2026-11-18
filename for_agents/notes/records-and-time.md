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
stored; the old docs said "Replaces"). `write_records` / `write_annotations`
have NO replace mode, no truncation and no whole-file rewrite — do not add one.
The 2.4 decision (a clinical viewer must be able to take back a wrong note)
did NOT relax that: removal is a SEPARATE, explicitly named call,
`remove_annotations` (next section), and nothing else may rewrite a record file.

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
  provably torn: ALL of (a) readable .ridx, (b) no entry at/after the stop points at
  present non-zero bytes (bytes past EOF or zero to EOF = index ahead of data =
  TORN, recoverable; present but failing length/CRC, or CRC-valid with the
  length restored by the remaining size or a 1-bit flip = DAMAGE), (c) no CRC-valid record at any later byte position and the rest is not
  one complete record, (d) the rest is short or fails as a record. Else both
  files stay byte-identical and a note ("record file damaged, not modified")
  is the only output; without a .ridx nothing is ever dropped. Damage messages never
  embed raw type bytes (`safe_type` hex-escapes: a non-UTF-8 byte raised
  UnicodeDecodeError in lenient mode). The header count is NOT evidence (without barriers it lands first).
  LENIENT READ (`Session::read_record_file`): strict parse fails -> retry with
  `RecordParseInfo::tolerate_damage` + `.ridx` offsets: intact records are
  returned, each damaged one is a `problems()` entry (kind "records", reason
  with byte offset, key rdat@offset); strict unchanged. Appends: stored body CRC 0 stays 0; unset max entry size is measured from the index; session level targets `session_record_file` (ambiguous = refuse); orphan `.ridx` refused (never truncated; recovery notes it); rollback restores both headers. Reported as `RecoveredRecordFile` in
  `RecoveryReport.record_files`. No false positives on legacy files (test).

## Removing annotations (`remove_annotations`, core/src/annotation_edit.cpp)

Maintainer decision (2.4): a user must be able to REMOVE a stored annotation,
and to change one (= remove + add in ONE rewrite). Python + C++ only (no C ABI,
no MATLAB, like the copies), a free function (`mef3io.remove_annotations`),
never a Writer method: `write_annotations` stays append-only and unchanged.

SELECTION (by content, never by position): a `RecordSelector` = level (+ channel
/ segment), `time`, `type` and EVERY body field as `records()` returns it
(`selector_for(record)` builds it). A field the stored record has a value for
must be named (`selector-incomplete`), a field the type does not carry is
refused (`selector-unknown-field`); `expect` (default 1) is the exact number of
stored records it must match, `all_copies` lifts the count (>= 1 still). 0 =
`no-match`, more = `ambiguous`, fewer = `count-mismatch`, one stored record named
by two selectors = `selectors-overlap`. ALL OR NOTHING: everything is selected
and built in memory before the first byte is written. Messages never quote note
text (PHI), only type, level and counts. Matching uses the decoded `Record`
(`parse_records` with the level's rto, the same one the Reader resolves; an
unknown/disputed offset = `offset-unknown`, never a guess), carrying uses the RAW
bytes (`core/src/record_raw.hpp`, ONE reader shared with the copy engine's
`read_record_file`).

WHAT IS REWRITTEN: only the `.rdat` + `.ridx` of the levels named (a call may name
several: one journal covers all pairs). Each kept record is copied byte for byte
(header, body, CRC, encryption level, time) in file order; its index entry is the
stored one with the offset rewritten. Headers are made right as for a fresh file
(count, span from the kept records' absolute times via `to_stored_time`, body CRC
recomputed whole, header CRC, provenance op 10 `records-edit`); `maximum_entry_size`
never goes DOWN (INVARIANTS §3, same rule as an append). Added records go through
`encode_records_for_file` = the very bytes `write_records` stores (shared encoder),
appended after the kept ones in time order, level-2 encrypted in a protected
session; they may only go to a level whose file exists (`add-level-missing`: use
`write_annotations` to create one). Passwords: both or neither, judged by the
session (`require_record_write_access`, #167): wrong/missing = `password`. A kept
record whose body the passwords cannot open = `record-locked`, nothing touched
(selection needs to read bodies). The LAST record of a level removed = the pair is
REMOVED (state of a level that never had annotations: every reader/validator/the
oracle accepts it, pymef answers it exactly as for a never-annotated session; a
zero-record pair is a state no writer produces and has a NO_ENTRY span); a later
`write_annotations` creates a fresh pair.

CRASH SAFETY (steps in the order they run; `Steps::point`, test hooks `_crash_at`
/ `_fail_at` stop at the k-th as a crash / ordinary failure):
 A. read, decode, select, build both images per pair and verify them in memory
    (`check_images`: what a reader finds == kept + added; index offsets/times; CRCs);
 B. per new file: write `<file>.<id>.new` (a crash mid-write leaves half of it),
    fsync it; then flush the folders;  C. read the new files back, byte-compare with
    the images, parse again;  D. write the OPERATION MARKER (it carries the plan:
    `edit-format 1`, `file <had_old> <has_new> <hex rel path>`; marker = journal, no
    separate journal file) with the repo's `write_operation_marker` (part file ->
    fsync -> no-replace rename -> folder flush);  E. re-check that every old file
    still has the size and header it was read with (`records-changed`: a Writer that
    appended meanwhile; the marker is already down so its next write refuses);
    F. swap per file: old -> `<file>.<id>.old` (rename), new -> `<file>` (rename);
    G. flush folders;  H. remove the `.old` files;  I. flush, remove the marker.
STATE AFTER A CRASH: before D = the old session, plus unused `.new` files (inert:
the Reader ignores them; swept by `recover_session(apply)` / reported in its dry
run, never by `remove_annotations` itself, so a concurrent edit's staging is not
deleted). From D to I = the marker is there: reads still work but the open warns
(`SessionOperationWarning`, text specialised in `mid_operation_message`), every
write refuses (`refuse_if_mid_operation`), the validator has the non-repairable
check `records.interrupted-edit`, and `recover_session(apply=True)` runs
`finish_interrupted_record_edit` FIRST (before it looks at anything else): every
file's state is read from what exists (T/new/old, `state_of`: replace S0 T+N, S1 N+O,
S2 T+O, S3 T), FORWARD when every new file still to be moved is intact (CRCs), else
BACK. THE DIRECTION IS DECIDED ONCE AND MADE DURABLE (`direction back` line, marker
rewritten atomically) before anything is undone: a half-undone file cannot be told
from a half-done one by what exists (this bit: a rollback that removed a `.new`
looked like a finished swap). Every step is idempotent, so a crash inside recovery
is finished by the next call; the next `remove_annotations` does the same first
(`report.resumed`). A state no step leaves = `edit-journal-inconsistent`, nothing
touched. An ordinary failure after D (e.g. a Windows sharing violation on a rename)
rolls back in process the same way (BACK) unless the `.old` files are already gone
(then it finishes forward); the error is `edit-failed`. Cancel is honoured until the
read-back is verified (`ctl.check()`, progress `finish()` is the last point), never
after D. The fault-injection matrix: tests/test_p54_remove_annotations.py (crash at
EVERY step, then recovery crashed at every step of ITS own, plain and L1+L2; ordinary
failure at every step; damaged new file -> rollback to the exact old bytes) and
core/tests/test_annotation_edit.cpp.

RENAMES use `detail::replace_file` after checking the target name is free (our
temp names are 32 random hex characters: no clobbering possible, and a file system
without no-replace rename still works). WINDOWS: renaming a file another process
holds open without FILE_SHARE_DELETE fails; the failing rename changes nothing for
that file, the earlier ones are rolled back, the call raises `edit-failed` naming
the cause and the session is the old one. NOT SOLVED (same gap as rename_session):
there is no cross-process write lock. The rule shared with rename (`detail::
refuse_if_being_written`: `.mef3io-tmp`, `.mef3io-repair-tmp`, `<dst>.partial`) sees
a writer in the middle of a write; the marker + step E catch a writer that appends
between the read and the swap; a Writer that is open and idle, or an append landing
inside the few microseconds between E and the rename, is lost. Documented as "stop
other writers first". Two processes running `remove_annotations`/`recover_session`
on one session at the same time are not supported either.

REVIEW ROUND 1 ADDITIONS (same call):
- NOTHING IS DROPPED SILENTLY: `record_raw.hpp` stops at the header count, so after the
  walk `remove_annotations` refuses (`record-torn`) when the `.rdat` holds ANY byte after
  the last counted record (a whole record or fragment an interrupted append left; only an
  all-zero tail of a count -1 file passes), when the `.ridx` header count disagrees with
  its entries, or when the index does not list exactly the records the data holds
  (`record-index-mismatch`). The rebuild also asserts kept + removed record bytes ==
  old record bytes (`verify-failed` if not). The message says to run `recover_session`
  (which keeps whole records). Tests: whole / fragment / index-ahead tails and the
  byte-sum invariant in test_p54.
- The marker's `file` lines carry the CRC of the whole NEW file: "only the target is
  left" (S3) is a finished swap only when the target IS that file; otherwise the new
  file was lost and recovery rolls back, reporting that the removal was NOT done
  (`InterruptedRecordEdit.reason`, `.not_done`, CLI exit 1).
- UNREADABLE PLAN: `settle_from_files` decides from the files alone (no password: whole-pair
  consistency by CRCs/counts/offsets): whole target pair kept, mixed/missing restored from
  `.old` (each half = its `.old` else the target), otherwise `edit-unsettled`. NEVER
  deletes: leftovers move to `<session>.edit-leftovers`. Also used when `settle` meets a
  state no step leaves. `clear_operation_marker` refuses this marker.
- READER: `Reader.records()` (Python) raises `edit-interrupted` while the marker is
  present (one stat per call); samples keep reading. C++ `Session::read_records` is
  unchanged (the read path belongs to another change). Chosen because it is the simplest
  correct rule: no per-level plan logic in a reader.
- RACES: step E (marker down, size + header of every old file) plus `require_unchanged`
  (size + mtime) immediately before each old file is renamed away; a difference rolls back
  (`records-changed`). A write call that passed its marker check before the marker existed
  is not detectable (no per-session registry or lock exists: `being-written` is file-name
  based) and can still append after the last check; documented in one sentence.
- TEST SEAM: `mef3io::testing::set_edit_observer` / `set_edit_skip_flush` (Python
  `_set_edit_observer`, `_set_edit_skip_flush`) record every durable step (fsync-file,
  flush-dir, rename, remove, marker-durable, direction-durable, marker-removed, point); the
  order test asserts the invariants and its negative control (flushes skipped) must fail.

REVIEW ROUND 2 ADDITIONS:
- ONE guard for every reader: `require_no_interrupted_edit` (annotation_edit.cpp) is called
  by `Session::read_records`, so Reader / MultiReader / the raw extension / the C ABI
  (MATLAB gets the message) / compat all refuse (`edit-interrupted`); the Python-level check
  is gone. compat's `get_annotations` swallows errors by design and re-raises this one.
  `inspect_session` reads no record while the marker is present and says so (warning
  `annotation-edit-interrupted`; the text report prints "records: NOT reported").
- UNREADABLE PLAN, ONE DIRECTION FOR ALL LEVELS (`settle_from_files`): levels = groups with
  `.new`/`.old` leftovers; no `.new` anywhere and every target a whole pair -> keep new (done);
  else every swapped/half-swapped level has an intact old pair (each half = its `.old`, else
  the target) and every unswapped level a whole target -> restore all (NOT done); else
  `edit-unsettled`. A level whose `.new` AND plan are gone is invisible (documented limit).
  The marker-with-plan decision (`decide`): an S3-looking file (only the target left) that
  stands before a swapped-but-not-cleaned file (S2) in plan order is applied (cleanup runs in
  plan order after all swaps); elsewhere the recorded new-file CRC decides.
- TEST SEAMS: `set_edit_skip_flush` exists only under `MEF3IO_TEST_SEAMS` (CMake option,
  OFF by default, forced on by MEF3IO_BUILD_TESTS, OFF in pyproject.toml): the order test's
  negative control is in the C++ suite. The observer stays in every build (it only observes).
  The marker is placed by `place_marker` (not `write_operation_marker`) so its fsync, rename and
  folder flush are visible to the observer.
- `SessionWriter::write_records` looks at the marker again right before the append and once after.
- Kept handles of `open_files` (#191) are `.tdat` only (`DirectorySource::read_extent`): record
  files are read with plain opens and renamed by the edit, nothing of this PR holds them.
