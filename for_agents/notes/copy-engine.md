# Copy engine: anonymize, transform, cut, merge

> Read before touching session_copy.cpp, segment_copy.hpp, anonymize/transform/merge (C++ or Python), their refusal codes or their verify.
> Split out of CLAUDE.md verbatim; "the private reader" = the private third-party reader named in CLAUDE.md.

Copy engine (#73, `core/src/session_copy.cpp`): anonymize_session and
transform_session are ONE driver (`run_copy`: preflight -> Copier pass into
`<dst>.partial` -> fsync -> `verify_impl` -> rename), ONE `preflight()`, ONE
`Copier` and ONE `Verifier`, steered by `Rules` (`anon` set = anonymize;
unset = transform with `edits`; anonymize either SHIFTS, `Rules::shifting()`
= `AnonymizeOptions::shift_us` set, or KEEPS every time,
`Rules::keep_times()`, the 2.0.0 DEFAULT, which is the transform's verbatim
time path inside the same rules). NEVER fork a second copy path for a new
operation (cut/merge #103, re-encode #121): add rules or options to this one.
CUT (#103, `CopyOptions::time_range` + `exact`, both operations): decided per
block from the RED HEADER's time and count; sample i of a block at t lies at
t + i*1e6/fs (`sample_time`), a block keeps [i0, i1) (`window_slice`); fully
outside = dropped (engine flags the next kept block), straddling + exact =
`trim_block` decodes, slices and RE-ENCODES with red::encode_block (source's
encryption level with the copy's keys, plain without passwords; time in the
source's stored convention via `stored_like`, shifted when anonymizing with a
shift) and
hands it to the engine as `CopyBlock::replacement` (the engine sets offset,
block_bytes, CRC; requires `start_sample`). A segment's data is now copied
BEFORE its .tmet (`copy_segment`), so section 2's derived fields
(serialize_derived_fields, never re-serialize) and every file's UH start/end
(`seg_span_`) and segment number (UH @48, `seg_number_`) describe what was
kept; segments outside the window are not read, ones that keep no block are
removed, the rest renumbered from 0 with channel-wide start_sample. Records:
`record_in_window` (time in window; Seiz/Epoc also by span; no time = kept).
ONLY THE BLOCKS AROUND THE WINDOW ARE READ: `window_entries` binary-searches
the .tidx start times (O(log n) 8-byte reads) for [first block starting after
t0 - longest_block_us (the segment's declared longest block + 1 s; an hour
when unset), minus 2; first block >= t1, plus 1] — reaching back by the
longest block is what keeps a long block overlapped by later short ones in
range (a 2-block margin alone silently lost its tail) and `SegmentCopySpec::entries` restricts the engine
to it (rchar-pinned in test_p33: a 1-min cut of 4 h reads ~1 MB of 11.6 MB,
the rest is the validator reading indexes). The pre-flight uses the same
index range to find kept channels with NO sample in the window; they are
left out (`channels_without_samples`, also excluded: Seiz entries dropped,
names redacted when anonymizing) — an empty channel folder failed verify.
Section 2 block_interval is recomputed with maximum_block_samples (the
validator's times.block-interval refused cuts otherwise).
Verify (`check_segment_data_cut`) walks the source blocks of the same range
in LOCKSTEP with the copy's, placing each by its HEADER (authority): every
source block with samples in the window must be matched by the copy's NEXT
block — kept whole: its entry as the source's (time via time_field, count)
and byte-identical (check_block, discontinuity bit free); trimmed: decodes
to exactly its window samples (check_trimmed_block) with the time
stored_like + shift; leftover/missing/duplicated/reordered copy blocks fail;
the source blocks just outside the range must hold nothing of the window.
Copy segments are matched to source segments by TIME (each one's own rto).
OFFSETS IN A CUT (review of #144; transform had the bug too): everything is
placed by the offset ITS file is stored against, never one session-wide
value (plan.rto is 0 when segments disagree): the pre-flight's first look
and window per segment (`segment_rto_of`; `SourcePlan::segment_rtos` read
only when inspect says "disputed"), records per level (`level_rto_of` =
Session::agreed_rto's rule: segment's own; channel/session = what all their
segments agree on, a segment-less channel takes the session's) in the
Copier (`set_level_rto`) and in verify (its OWN source-metadata reads, not
the plan). A channel/session record file with an offset-dependent (negated)
time and no agreed offset is refused under a cut:
`time-range-offset-disputed` (shared code; the Reader refuses such files
too). verify also re-checks every channel run_copy left out as "no samples
in the window" against the source (each segment's own rto, block HEADERS),
and a standalone verify with time_range NOTES a missing channel that has
samples. A cut segment's UH start/end (.tmet/.tidx/.tdat) is checked by
verify itself against the source blocks it matched in lockstep (first kept
sample .. end of the last, + shift when shifting, read with the copy's own
rto): the validator's time checks stand down at rto != 0. Other derived s2
fields are left to the validator (verify runs it), so a cut NEVER excuses validator findings more loosely than the
exact key (check|channel|segment|field|message): a source already failing
e.g. times.recording-duration gets its cut refused (repair first). A
check+channel key was tried and rejected in review: on legacy mef_tools
sessions, which carry sizing.contiguous / times.discontinuities /
block-interval / recording-duration on every channel, it would have excused
the cut's own block_interval bug. Reader.read(t0, t1) can put a sample up to half a period before t0
in slot 0, so tests compare the cut read whole against the source read from
the cut's start time.
MERGE (#103 second half, `merge_sessions` / `check_merge` / `verify_merge`,
MergeOptions = TransformOptions + per-source `passwords`): the SAME engine —
one Copier per source (transform rules), one `preflight()` per source run
with that source's share of the selection and of the per-channel metadata
(a channel only some sources hold is not "unknown" to the others), then the
merge's own checks in `preflight_merge`: rto equal, one rate per channel,
subject fields equal unless set by metadata, and OVERLAP ON RECORDED DATA —
only segments whose UH spans overlap across sources have their indexes read
(`index_blocks`), and blocks overlap only beyond half a sample (a source's own
jitter never counts). Pieces: a segment is split where another source's
data starts in one of its gaps (`MergePiece.entries`, block boundaries, no
decoding); pieces sorted by time, numbered from 0, each copied with
`Copier::copy_segment(..., piece)` (the cut's reshape path: derived s2,
spans, segment number). Level UUIDs are the merge's own
(`set_level_uuid`: one per channel and session, new per piece); Verifier
`set_merge` relaxes only the 1:1 level check. Annotations: session/channel
level merged by time (`prepare_records` + `write_records`, the split of
copy_records), segment level with their piece (time bounds lo/hi).
ONE half-sample tolerance everywhere (recorders round times to whole µs):
the overlap check, the split (foreign data starting within half a sample
before a block's end lies in the gap after it) and verify's
segments-in-order check — they disagreed once and accepted merges failed
late. Segment spans = UH times widened to the first/last index entry (a
foreign UH may be unset or stale); segments without blocks are left out
(noted). Merged record files' UH start/end = min/max record time
(`set_record_span`).
`verify_merge_impl` (does NOT trust the plan): the pieces of each source
segment must tile [0, n) exactly; one level UUID per result segment (all its
files), per channel and for the session, all distinct; each segment against
its piece in lockstep (`check_segment_data_cut(..., piece)`); merged
annotations against the sources' sorted by time, PLUS their headers
(check_uh as a copy of the first source's, counts, span) and every .ridx
entry (by offset: type/version/encryption, the source index's time);
segments never overlapping (with the tolerance); the validator with NO
excusal (renumbered: repair sources first). The Seiz entry filter is one
function (`seiz_without`) for the copy and this check. time_range is refused with a
merge for now (`merge-time-range`); anonymize after merging.
Transform rules, test-pinned (`tests/test_p33_transform.py`,
`core/tests/test_transform.cpp`): every stored time VERBATIM (Copier::time
returns it unchanged; no Shifter, so no rto refusals: rto, DST, GMT kept);
RED blocks untouched except re-keyed encrypted statistics; meflib
protected/discretionary regions KEPT (UH, s1-s3, entries, block headers);
record bodies byte for byte (unknown types too) except the Seiz entries of
left-out channels (`kept_body`); the session name written only when it
differs (no zero-fill of untouched fields); new file UUID, level UUIDs
remapped, provenance_UUID = the source's (or the source's file UUID when
unset); provenance op 8 `Transform` via stamp_modified (declines a foreign
region, which is then left as is). Metadata edits (`MetadataEdits`: subject
names/ID, location, session description, per-channel channel/reference
descriptions) go into the STORED section images (never re-serialized); a
value that does not fit is REFUSED (`metadata-too-long`), never cut. Its
verification compares every other byte to the source: UH except CRCs,
session name, UUIDs (file new, level remapped 1:1, provenance_UUID = source),
password fields, provenance (= the source's region after stamp_modified),
and 32..48 outside .tmet: entry count / max entry size are checked against
what the copy HOLDS (`check_counts`, both modes; the validator reports a
wrong maximum only as Info and never reads record files, so it cannot be
relied on); record index entries' type/version/encryption against their
record; records keep a source's own "not encrypted" byte; s1 beyond the levels; s2/s3
= source + the edits exactly; entries/blocks beyond offset, CRC and
encryption bits; bodies padded with 0x7e only. Python: shared report and
refusal plumbing in `python/mef3io/_copy.py` (COPY_REFUSAL_CODES +
VerifyReport/CheckReport/Refusal + argument checks + CLI helpers);
anonymize.REFUSAL_CODES = shared + anonymize-only, transform.REFUSAL_CODES =
shared + metadata-*. The shared codes are documented ONCE, in
docs/anonymization.md's table; docs/transform.md documents only the
transform's own (test_p33 checks both).

Anonymize (#70) invariants, all test-pinned (`tests/test_p29_anonymize.py`):
(1) `verify` is FIELD-WISE and INDEPENDENT of the rewrite (own matcher, so
a rewrite gap fails closed). Rewritten fields: plain case-insensitive
substring scan; excused only inside the copy's own name or a kept channel
name (<4 chars only), a kept channel name, a <4-digit number inside a
longer number (`,` separates), or a <4-letter word inside a longer word --
all NOTES. Kept channel names (+ paths, UH channel_name, Seiz entries):
tokens split on non-alphanumerics incl. `_`; REFUSE a token EQUAL to an
identity value of 3+ chars; for subject/location (not the source session
name) also a 3+-letter value equal to a letter/digit part ("Roe1") or a
4+-char value inside a token ("JaneRoe"); else NOTE (256-channel montages
hit "12"/"P3"/"EEG"). That rule (`Scanner::channel`, paths via
`Scanner::path`) is ALSO run by the pre-flight (#143) on each kept channel's
name and the folder/file names the copy writes from it (`Scanner::path`
drops the segment number of `<ch>-NNNNNN`: mef3io's counter, checked on its
own; ID "PT0001" vs "-000001" refused every multi-segment copy) ->
`channel-name-identity` before any write;
verify still applies it to what the copy holds (the pre-flight calls the
verifier's matcher, as text-unremovable does; never the other way round). The copy name/pseudonym holding an identity value
(3+ substring, shorter whole token; own-name excuse only for <3 chars) is a
ValueError before any write. Needles: values normalized (trim + collapse
spaces); subject names + location ALSO word by word (3+ letters) -- but
location WORDS never in the channel rule ("EEG Lab" vs EEG1) -- and the ID
whole plus each 3+-digit run as a whole number ("EEG-0042" -> "0042"; its
letters are a prefix). Engine: dropping a segment's FIRST block requires
`SegmentCopySpec::start_sample` (invalid_argument otherwise; #103 must pass the
channel-wide count). Case fold covers ASCII + Latin-1 +
Latin Ext-A; `scan_folded` additionally strips those diacritics. BOTH the
scanner and the Redactor match in that one space (#115: the removal must
cover what the check looks for — "Muller" for "Müller"); `scan_folded`'s
start/end maps take a match back to whole original characters, which the
Redactor replaces, in ONE pass per rule (linear; re-folding per match was
quadratic: 400 KB of notes took minutes). Its match MODE is still decided by
the value's case-folded BYTE length (`folded(v).size() < 4` = whole words),
so "Zoë" (4 bytes) is searched anywhere, as before #115 — deciding it on the
accent-free length let "Zoë1" through while verify only NOTED it. Kept
channel names protect only where the text spells them exactly but for case
(`protect` = folded, located in the original text): "PER" does not shield
"Pérez". The MATCHING rules stay separate from the scanner's (independence).
Text the removal still cannot take out (a left-out channel "a3" in "a3a3") is
refused in the PRE-FLIGHT (`text-unremovable`): it runs copy_redactor +
build_scanner on every kept field — record text with text="keep"
(`record_text_fields`), the kept channels' units (always rewritten), and,
with descriptions kept, the kept channels' OWN descriptions read from their
.tmets (never a left-out channel's) — so check() reports it and nothing is
written; check() passes the clear flags for that. A text callback cannot be
predicted (verify still checks). `redact_text()` (C++ + Python, #114) = that
same copy_redactor on caller texts, and refuses whatever the copy would
(locked source, unknown channel, identity in a name, incl. a kept channel's:
`channel-name-identity`) except dst-exists /
partial-exists / text-unremovable: no rule mirror in applications. Identity checks run before excusing the copy's own name ("S0012"
vs copy "S001"). Epoc/Curs id_number: absolute window only (the rto window
reaches small ints). Redactor: <4 chars = whole tokens, numeric = whole
numbers, never inside kept channel names. Segment number = `.tmet` UH @48,
folder suffix must agree; times are checked one field at a time against the
source range +-1 day, plain or negated -- never a sliding window, never
size/count fields, RED payload + stats compared, not scanned. (2) Build in
`<dst>.partial`, fsync, verify, THEN rename; any failure/cancel removes it.
(3) Never re-seal a failing block or record CRC. (4) Source-encrypted RED
blocks are re-encrypted at their level with the new keys, decrypted only
with no output passwords; never silently downgraded. KEEP-TIMES MODE (no
shift, the default since 2.0.0; `tests/test_p40_anonymize_keep_times.py`,
test_p32 `test_check_agrees_without_a_shift`): `std::optional<si8>
shift_us` is the ONE switch (never a separate flag; 0 stays `shift-zero`);
Copier::time returns every stored time verbatim, each segment's s3 rto is
KEPT (the stored times are read against it, so user times = the source's;
no rto-disputed check), GMT 0 / DST NO_ENTRY in both modes (location-
revealing, no returned time uses them). Verify: `keep_times_` -> every
time_field must EQUAL the source's stored value and each segment's rto the
source segment's; no time-leak window, no id_number window, no shift floor;
everything else (names, regions, UUIDs, provenance 6, scrub_times) as when
shifting. Skipped without a shift (pre-flight, `shifting`): shift-zero/
floor/not-positive, the time-only inspect blockers (rto-disputed,
time-before-rto, rto-unreadable), the per-field Shifter run (time-overflow),
id-number-time. check(shift_us=None) = check of THIS copy (it used to mean
"shifted, shift not chosen yet"; C++ check_anonymize lost `check_shift`;
preflight keeps an internal check_shift=false for redact_text only). The
report's source_* key is handled the same in both modes. (5) WHEN SHIFTING:
|shift| >= span + 1 day, all times stay > 0. (6) WHEN SHIFTING: Sentinels
(UUTC_NO_ENTRY, 0, body -1) never shifted; every time keeps its stored
sign; output rto 0, GMT 0, DST NO_ENTRY. (7) Every text field is REWRITTEN whole (`write_string`
zero-fills; a guarded "if changed" left hidden bytes after the NUL), and
foreign regions are zeroed: UH, s1/s2/s3, `.tidx` entry 40/45/48, RED
header 5/8. Segment folders renamed canonically. Unknown record bodies are
dropped. (8) Inner inspect/validate calls get a cancel-only `Control`
(passing progress restarts the caller's sequence). (9) pymef SEGFAULTS on
unknown or `UnRc` record types: oracle tests must not include them.
(10) #111 ONE PRE-FLIGHT: `preflight()` in session_copy.cpp makes EVERY check
decidable before writing (options, paths, source open, records ambiguity,
inspect plan, password access, inspect blockers, channel selection, shift,
names, segment numbering from each .tmet UH @48, kept channel names (#143),
record files incl. the Epoc/Curs id_number window). `anonymize_session` runs it with
collect=false (throws the first, BEFORE any file or progress report);
`check_anonymize` / `mef3io.anonymize.check` with collect=true. Never add an
up-front check anywhere else — it would let the two disagree; test_p32
parametrizes every refusal case through BOTH and requires the same first
code/type/message. Reads metadata only (rchar-pinned < 2% of .tdat, <= 3x
inspect). Blockers thrown as ONE exception (message lists all, code = first)
— collect mode lists each with `inspect_code`. (11) Every refusal throws
`CodedError<Base>` = the old exception type + the `CopyError` mixin
(code, kind, channel, field, inspect_code); `refusal_of()` gives uncoded
shared-code errors a generic code at preflight stages and at the
anonymize_session boundary. Python: a translator registered LAST sets
`.code`/`.details` on the usual type. The code table lives in THREE places
that test_p32 keeps equal: `REFUSAL_CODES` (python/mef3io/anonymize.py),
the table in docs/anonymization.md (code, type, check|copy|defensive), and
the cases in test_p32 (every non-defensive code must be reached, no
defensive one may be). Copy-only: block-crc, data-damaged, verify-failed
(and a time the Shifter refuses in a .tidx/.tdat UH or a block; when
shifting, every .tmet UH and record-file time goes through the SAME Shifter
in preflight). The docs table's "(shift only)" markers = test_p32's
SHIFT_ONLY + time-overflow (pinned in test_every_code_is_documented_and_reached).
Messages NEVER carry a source time (or rto): built without them, and
`scrub_times()` replaces any 10+-digit number in the source window in
VerifyIssue text, inherited findings, inspect blocker and shared-code
messages; test_p32 greps every refusal/verify message. Damaged record files surface as
source-damaged/source-crc (strict inspect fails first), so record-damaged/
record-crc are defensive. (12) `AnonymizeReport.source_start_us/_end_us/
source_channel_times` = THE RE-IDENTIFICATION KEY: in memory only; out of
repr, summary() and to_dict() (unless include_original_times=True) and
PICKLE (`__reduce__` drops them unless `with_original_times_in_pickle()`;
copy/deepcopy keep them; asdict includes them); never in a message or
warning; test_p32 greps the copy's bytes for them. (13) verify: a kept
source DATA channel must be a data channel in the copy ("has no segments in
the copy"); a records-only channel is accepted only if records-only in the
source; anonymize_session also requires verify's channels == kept.
