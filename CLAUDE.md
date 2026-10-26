# mef3io — assistant context

A single C++17/20 core for **MEF 3.0** read+write, wrapped for Python via
nanobind. The high-level semantics of the legacy `mef_tools` (float scaling,
NaN discontinuities, precision inference, the int32-counts + conversion-factor
primitive path) live in **C++** so all bindings behave identically. Video is out
of scope. The legacy `mef_tools`/`pymef` stack is the correctness oracle.

Status: read + write complete, cross-validated **both directions** vs
pymef/mef_tools (values, NaN gaps, times, encryption none/L1+L2, fractional fs,
records). In-segment append + per-segment map implemented. Tar session
archives (single-file `.mefd.tar`, read in place) implemented. Session
validator + targeted repair + recovery implemented (+ an open-time warning).
`inspect` (metadata-only description, #67), cancel/progress on long
operations, de-identified copies (`anonymize_session` +
`anonymize.check`/`verify`, #70/#111) and modified copies (`transform_session`
+ `transform.check`/`verify`, #73) implemented — all Python + C++ only, not
MATLAB. Reads/toc/segments/records on one Reader/Session may overlap across
threads (#79; GIL released on open and every long call; the Python races of
#110 fixed, rules in for_agents/INVARIANTS.md §12). ONE thread rule
(docs/threads.md): `n_threads` per object covers open, file access, decode,
encode; settable at runtime; no per-call count (2.0). Parallel open (#141),
decode, encode, all byte-/result-deterministic across thread counts.
Python (pytest) + standalone C++ Catch2 suites.
Wheel builds via `python -m build`.

## Build & test (use the active conda env for everything)

```bash
scripts/dev_build.sh              # builds build/dev, symlinks _mef3io*.so into python/mef3io/
python -m pytest tests            # conftest.py sets up paths; pymef needed as oracle
# C++ unit tests:
cmake -S core -B build-core -DMEF3IO_BUILD_TESTS=ON && cmake --build build-core && ctest --test-dir build-core
python -m build --wheel           # wheel
```

This is the standalone mef3io repo (github.com/bnelair/mef3io), migrated out of
the original `mef_tools` repo in July 2026. The legacy `mef_tools` oracle now
comes from the pip-installed `mef-tools` package (`pip install mef3io[test]`);
`tests/conftest.py` still prefers a local checkout if one exists in a parent
directory. `reference_files/` (meflib/pymef/mef3_dump) stayed in the old repo.

## Module map

`core/` (C++): `types` (aliases + format constants), `byteio` (LE field IO, no
packed-struct casts), `crc` (Koopman-32), `crypto` (SHA-256, AES-128-ECB,
two-level password), `headers` (UniversalHeader, MetadataSection1/2/3,
TimeSeriesIndex, RedBlockHeader — parse/serialize by explicit offset),
`metadata` (.tmet loader: CRC→password→decrypt), `red` (decode + encode),
`validate` (check registry + targeted repair; see below),
`session` (lazy .mefd/.timd/.segd tree, indexed reads, `collect_blocks`; ALL
read-path file access funnels through `source`), `source` (SessionSource
abstraction: DirectorySource/TarSource; `read_head` = one bounded read), `tar` (uncompressed .mefd.tar session
archives: TarIndex/TarSource/`archive_session`), `reader` (gridding, NaN fill,
scaling, parallel decode), `records` (read+write), `writer` (segment writer),
`session_writer` (precision inference, quantization, NaN splitting, segments),
`parallel.hpp` (the thread rule: `resolve_threads(n, Work::Cpu|Io)` is the
ONLY place "automatic" is decided — hardware concurrency, >= 8 for file
access; `check_threads` rejects negatives at every public entry;
`parallel_for` degrades when a thread cannot start (never std::terminate;
`detail::thread_start_hook` is the TEST hook that counts/fails starts);
`parallel_try_each` = every item runs, per-item exception, for work whose
error must be the serial one), `anonymize.hpp` (`anonymize_session` + `verify_anonymized`,
#70; see below) and `transform.hpp` (`transform_session` + `check_transform`
+ `verify_transform`, #73, and the types EVERY copy shares: `CopyOptions`,
`CopyRefusal`/`CopyError` = `AnonymizeRefusal`/`AnonymizeError` aliases,
`CopyCheck`, `CopyReport`, `VerifyReport`), both implemented by ONE engine in
`core/src/session_copy.cpp` (was anonymize.cpp; see "Copy engine" below),
`segment_copy.hpp` (internal streaming block-copy engine:
per-block edit/drop hook, re-seals block CRCs, repacks + rewrites `.tidx`
offsets, renumbers start_sample and flags the block after a drop as a
discontinuity, returns section-2 stats; built so the planned cut/merge #103 —
not implemented — can reuse it), `inspect` (describe a session from
.tmet/.rdat [+ .tidx], never .tdat), `control.hpp` (CancelToken + progress in
one `Control`; `Cancelled` is deliberately NOT a std::exception), `recover`,
`errors` (MefError hierarchy; copy refusals add the `CopyError`
code mixin), `time` (stored <-> user time), `c_api.h` (flat C ABI under the
MEX); internal `durability.hpp`, `last_segment.hpp`.

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

Tar session archives: `archive_session(dir)` packs a session into ONE
deterministic uncompressed ustar (`name.mefd.tar`); readers in all languages
accept the tar path transparently in the existing `path` argument (random
access into member byte ranges, no extraction; foreign GNU/PAX/bsdtar/tarfile
archives tolerated, `./` prefixes + missing dir entries OK, compressed input
rejected with a clear error). `extract_session(tar)` is the exact inverse
(archive→extract→archive byte-identical; strips the in-archive session root;
rejects `..` member traversal; cleans up on failure) and yields a writable
session again. Writers REJECT `.tar` paths — the guard sits BEFORE the
overwrite/remove_all in SessionWriter's ctor so an archive can never be
deleted. Python `mef3io.archive_session`/`extract_session`; MATLAB
`mef3io.archiveSession`/`extractSession`; C ABI `mef3io_archive_session`/
`mef3io_extract_session`. Session NAMING IS ENFORCED in core (all bindings):
dirs must end `.mefd`, archives `.mefd.tar` — reader (`open_session_source`),
writer ctor, archive and extract all throw IoError otherwise
(case-insensitive, trailing separators OK; `path_has_suffix` in source.hpp).
`SegmentInfo.path` for tar reads is `"<archive>::<member>"`. cache.py fingerprints a tar session as the single
file (an empty fingerprint dict would validate stale caches forever).
Benchmarked (docs/benchmarks.md, `benchmarks/access_modes_benchmark.py`,
2.2 GB / 768 segments): once open a tar reads like the dir (full and warm
windowed identical); a COLD open is ~2.5x slower (0.5 s vs 0.2 s — the member
index comes off disk); archive ~0.6 GB/s. Tests:
core/tests/test_tar.cpp, tests/test_p11_tar.py, tar block in
matlab/test_mef3io.m.

Warm-start snapshot cache (`python/mef3io/cache.py`, `Reader(cache=)`, cache
format 7, #64; 7 = info ufact None when segments differ, #150). RULE: a snapshot only makes sense if it is SMALL and CHEAPER
than reading the original — both are test-pinned in tests/test_p22_cache_
snapshot.py (size flat from 10 to 500 segments/channel; the cached open reads
fewer bytes AND opens fewer files than a plain open at 10/150/500; a fresh
session is not slower with cache=). It once hashed every .tmet and made a dir
open 2.7x SLOWER; never reintroduce per-file work (the open/stat counter in
p22 wraps os.open/os.stat/os.lstat/os.scandir/open). Validity is decided ONLY
on a cached open, O(1) per channel: exact mef3io BUILD (version + the CMake
build id `_mef3io.__build_id__` — git SHA, else configure time — + the
extension binary's size/mtime, so dev builds and reinstalled wheels never share
snapshots; TODO in `_version`: PyInstaller onefile gets a per-run identity);
session identity is `session_key` = SHA-256 of the abs path, NEVER the path
(subject names); the cache="auto" FILE NAME is `<hash16>.mef3cache`, hash only
(#82; it carried the session stem until 2.0.0 — an auto open unlinks that
exact old name via discard_if_ours, `_discard_legacy_auto_cache`, no listing); per channel the .segd count (one listing of the .timd — always
listed, never trust dir mtimes: FUSE/object stores report constant ones),
start/end from the first/last segment's .tmet universal header plus both
headers' random file_UUIDs (offset 836 — catches overwrite=True with the same
samples but new units/ufact/subject), the last segment's .tdat/.tidx sizes,
its provenance region (offset 960; the modification count is the change
marker); plus channel folders and session .rdat/.ridx sizes. Sizes via
open+fstat, not stat (NFS attribute cache). Tar: size+mtime of the archive.
Signature taken BEFORE the backend reads metadata (test-pinned via a write
injected mid-open). NO SNAPSHOT when total segments <= channels
(`worth_caching`; measured 128x1/256x1: cached open == plain, first open ~2x
slower) — the layout listing decides before any signature work, and a MARKER
(`build_marker`, no metadata) is saved so later cache= opens skip even that
listing (~17% of a 1-seg/channel open) for one tiny JSON read; the Reader
drops it when its plain open's infos show the session grew past the threshold.
DOCUMENTED BLIND SPOT: an in-place edit of a NON-last segment that moves none
of the above is not detected — open without cache= for a full read. It covers
(a) foreign tools, and (b) mef3io's own repair/recover of a non-last segment
WHEN the segment-repaired stamp was skipped (foreign region or bad CRC in the
last segment, or a writer's .mef3io-tmp next to it — the report says so and
that cached opens will miss it). Otherwise the stamp makes them visible
(provenance rule below).
WRITE PATHS MUST NOT KNOW ABOUT THE CACHE: no invalidation calls from Writer,
repair, recover or any future write path (the Writer's old auto-cache delete
and `cache.invalidate()` were removed). ENCRYPTED SESSIONS ARE NEVER CACHED
(infos hold decrypted subject fields): no snapshot or marker written. Load
order: parse the JSON; a well-formed current marker returns at once (no
metadata; the Reader deletes it if its plain open finds encryption); anything
else gets the encryption check BEFORE format/version, and for an encrypted
session a file that is a JSON object with a `format` key (any mef3io snapshot
or marker, 1.1.x format-4 included) is deleted — NEVER any other file, since
cache=<path> can name anything (`discard_if_ours`). Dirs check the UH password
fields; tars peek at the first .tmet member; the Reader also refuses via the
backend's `password_protected()`, failing closed. save() uses a unique temp
file; a failed save never fails an open. Measured 64 ch x 150 seg: snapshot
60 KB, cached open 25-54 ms vs plain 1.3-3.0 s; x 600 seg: 60 KB, 95-108 ms vs
6.6-30 s.

Validator/repair (`core/{include,src}/…/validate.{hpp,cpp}`, `python/mef3io/
validate.py`, `python/mef3io/__main__.py`): a REGISTRY of 18 checks comparing a
session's declarations against its data, run in a fixed order (integrity →
structure → sizing → times → headers). Adding a check = ONE entry with
`detect` + optional `repair` lambdas; ordering/filtering/reporting/bindings/CLI
pick it up free. THREE invariants, all test-pinned: (1) **a Validator only
validates** — it has NO repair/fix method, writing lives in the separate
`mef3io.repair_session(path, ids)` function, and a test asserts the class never
regrows one, so nobody mutates a session through an object they opened to
inspect; (2) `repair_session()` requires an explicit non-empty check-id list
(no "fix everything"; empty/unknown/non-repairable → `std::invalid_argument` →
ValueError); (3) `Finding.repaired` / `segments_repaired` count ONLY what was
actually written — `RepairFn` returns whether it changed a declaration, so a
repair that declines is reported as still outstanding (this bit: the caller used
to mark every selected finding repaired, so a declined one reported success and
left the defect on disk). Repairs rewrite ONLY declarations (s2 + the three
universal headers) — never samples or the index — back up to
`<session>.repair-backup/`, skip any segment whose CRC fails, and reject tar
archives. `Report.ok` ignores findings repaired in the same pass. CLI:
`python -m mef3io validate <path> [--check ID] [--list-checks] [--fast]` reads,
`python -m mef3io repair <path> --check ID [--no-backup]` writes and refuses an
empty selection — separate subcommands, neither reachable from the other
(`__main__.py` exists so `-m` doesn't double-import the package). On READ,
`Session::declaration_issues()` scans the already-parsed s2 for unset sizes
(free — no extra I/O, sees only missing, not wrong) and `Reader` emits one
`SessionDeclarationWarning` per open saying reads are unaffected; it rides in
the cache snapshot so warm opens still warn; `warn_declarations=False` or a
category filter silences it (pyproject filters it by MESSAGE for the suite —
naming the class there imports the wrong mef3io at pytest config time). Checks
validated against the meflib C source: `recording_duration` is a SPAN
(meflib.c:5479), `.tdat maximum_entry_size` is BYTES (pymef writes samples —
its bug), and times must be compared as ABSOLUTE uUTC because pymef negates
block times but not universal-header times. Docs: `docs/validation.md`. Tests:
`tests/test_p13_validate.py` (asserts every repairable check has a corruption
case, so a new one cannot ship untested) + a Catch2 case.

Precision step (#81, `core/src/quantize_step.cpp`, `SessionWriter::write_float`
with `Precision::detect_step()`, Python `precision="step"`, also
`MefWriter.write_data`; NOT C ABI/MATLAB): OPT-IN, the default
(infer_precision) is unchanged. `detect_step` ESTIMATES from a BOUNDED sample
(first 65,536 + 16 windows of 2,048; only an all-0/NaN sample scans on):
ascending unique |v|, real Euclid (coarse tol 1e-6 step) + least-squares refit
at power-of-two indices, then SNAPPED (shortest decimal 1..17 digits, then
max|v|/k_max and its float64 neighbours) to the candidate reproducing every
sampled value bitwise. `quantize_to_step` DECIDES: one pass (the conversion
pass, no second read), every value must satisfy
`|x - double(k)*step| <= 64 eps |x|` (the reader's own product) and |k| <=
INT32_MAX (INT32_MIN = RED_NAN), else false. Fresh channel failing either ->
the default path for that write (byte-identical to a default write's
counts), `step_fallback` reason + Python `StepFallbackWarning`. MINIMUM
EVIDENCE: a sample with < 16 distinct non-zero magnitudes (DC, square wave,
a few levels) gives no step (the default path) — the level itself would
become the channel's factor. A CHANNEL KEEPS ONE FACTOR: the reader scales a
whole channel by its FIRST segment's factor (session.cpp
load_channel_basic_info; per-segment scaling is #150) — a new segment with
another factor reads back silently mis-scaled (PRE-EXISTING for the default
path with new_segment=True; #150) — so "step" on a channel with segments
(append OR new_segment) uses the channel's factor itself (never via a
`precision` int: factors >= 10 have p < 0, the "infer" sentinel — review
round 1 found that): exact if the data fits, else REFUSED
(WriteConflictError, nothing written), never rounded (decided in review;
once #150 lands a later change may start a new segment instead — not now).
The default `precision=-1` append onto a non-power-of-ten factor is refused
with a WriteConflictError naming "step"/write_int32. NO INT32 WRAP anywhere — CLAMP (maintainer decision, #149 round 3):
`quantize_to_int32` and `quantize_to_step` store a count beyond
+-INT32_MAX, and +-inf, as +-(2^31 - 1) (meflib RED_POSITIVE/NEGATIVE_
INFINITY, meflib.h:1060; never INT32_MIN = RED_NAN) via
`detail::clamp_count` (core/src/clamp.hpp) and count it in a `ClampCount`;
`WriteSummary::clamped` / `clamped_infinite` -> Python summary keys and ONE
`ClampedSamplesWarning` per write (`warn_clamped`, _writer.py; Writer.write
and MefWriter.write_data). NaN stays a gap. A finite value beyond int32 must
still lie ON the step for the step path (else the usual off-step refusal).
infer_precision and detect_step ignore +-inf like NaN. `int32_range_ok`
requires BOTH ends to fit (it was mef_tools' "both ends overflow" rule, which
let one-sided data wrap; pinned against the oracle as test_p16
`CASE_DIVERGENCES["one-sided offset"]`, a per-case ledger so
units_conversion_factor stays unlisted in KNOWN_DIVERGENCES), so ordinary
data never clamps (test_p43 pins it). The estimator's least-squares refit sums in units of
the step (no overflow at 1e300). WriteSummary gained
`units_conversion_factor`, `quantization` (Counts/Decimal/Step),
`step_fallback`; the Python dict the same keys. `Precision` is implicitly
constructible from int so every `write_float(..., 3)` caller compiles.
Tests: tests/test_p43_write_step.py (exactness, fallbacks incl. an off-step
value OUTSIDE the sample and int32 overflow, evidence cases, refusals on
append/new segment incl. factors 10 and 1e6, clamp + one warning on every path, size
within 5% of write_int32, detection on 50 M < 0.25 s, a loose 2.5x write-time
sanity bound; the tight ratios (step <= 1.10x default, worst-case fallback
<= 1.60x) are `@pytest.mark.benchmark`, opt-in via `--run-benchmark`,
because wall-clock ratios flake on shared runners), core/tests/
test_write_step.cpp. Gotcha for tests: a filter with short-decimal or dyadic
taps keeps data on a finer grid (found, stored exactly) — use irrational
taps to force a fallback.

Session metadata (subject/acquisition): `mef3io.Metadata`/`Subject`/
`Acquisition` dataclasses (`python/mef3io/metadata.py`), settable via
`Writer(metadata=)` / `set_metadata`, read via `Reader.metadata`. Threaded
through C++ `SessionMetadata` (writer.hpp) → SegmentSpec → writer.cpp s2/s3
population; `ChannelInfo` carries the fields on read. C ABI
`mef3io_writer_set_metadata` + MEX `writer_set_metadata`; MATLAB
`Writer(Metadata=struct)` / `Reader.metadata`. Subject fields are L2-gated.

`python/mef3io/`: `Reader`, `MultiReader` (`_multi`), `Writer` (incl. `write_annotations`), `compat`
(mef_tools.io drop-in; `MefReader`/`MefWriter` are also re-exported at the top
level — `from mef3io import MefReader, MefWriter` — via lazy `__getattr__`),
`cache` (opt-in warm start), `anonymize` (de-identified copy, `check`,
`verify`, `reference_shift`, `REFUSAL_CODES`, CLI), `transform` (modified
copy, `check`, `verify`, `REFUSAL_CODES`, `METADATA_FIELDS`, CLI), `_copy`
(what both copies share), `_inspect`
(`mef3io.inspect`; kept out of `__all__` so `import *` does not shadow the
stdlib module), `_cancel` (CancelToken/Cancelled), `_warnings` (MefWarning +
`emit`), `validate`, `metadata`, `_archive`, `__main__` (CLI: inspect,
validate, repair, recover, anonymize, transform), `pure` (stub). `password_interop_issue`
lives in `_writer.py`. `bindings/python/mef3io_ext.cpp` =
nanobind. `matlab/` = MEX + `+mef3io` classes. `examples/` = runnable scripts
01-13 (listed in examples/README.md; 13 = anonymize, smoke-tested in
test_p24_examples.py). Docs site: MkDocs Material from `docs/` (`mkdocs.yml`
holds the nav: guides, how-to, reference, release notes),
deployed to GitHub Pages by `.github/workflows/docs.yml` on push to main —
keep `PYTHONPATH=python mkdocs build --strict` green. API reference is
autogenerated: Python via mkdocstrings (NumPy-style docstrings on every
public module; docs/api/python.md lists them), C++ via Doxygen (`docs/Doxyfile` →
`site/api/cpp/html`, run after mkdocs; public headers use `///`/`@param`),
MATLAB via `docs/api/matlab.md` + `matlab/test_api_parity.m` (asserts MATLAB
mirrors Python method-for-method with help text; in the release MATLAB job).
`for_agents/` (repo root) holds agent handoff docs, outside the site.
`docs/design.md` = full design; `docs/encryption_model.md` = crypto model;
`docs/mef3_format.md` = format reference.

## Format gotchas (do NOT relearn the hard way)

- **Times are stored NEGATED on disk** as meflib's "recording-time-offset
  applied" marker. User uUTC = `(t>=0)?t:(-t+rto)` (read), `disk = rto-absolute`
  (write). Applies to UH start/end AND block/record times. rto=0 in fixtures.
  Missing this makes every time-range read return 0 samples.
- **Encryption is all-or-nothing pairing**: section 2 → L1 key, section 3 → L2
  key, by password presence. "Level-1 only" is not a valid file. Decrypt a
  section only when its stored level is strictly positive; unencrypted files
  carry -1/-2 (`0xFF`/`0xFE`). See docs/encryption_model.md.
- **Password scheme**: bytes = LOW BYTE OF EACH CHARACTER'S UNICODE CODE
  POINT (meflib `extract_terminal_password_bytes` via `UTF8_nextchar`,
  meflib.c:2439/8855; "é" -> 0xE9), 16, zero-pad. NOT the last UTF-8 byte —
  that was mef3io <= 1.2.0's bug (#84): equal on ASCII, wrong on everything
  else. Chars > U+00FF collide (low byte only) — matched exactly, warned on
  write (`PasswordInteropWarning`, also > 15 chars / > 15 UTF-8 bytes: pymef
  strcpy's into a 16-byte buffer; the same rule is public as
  `mef3io.password_interop_issue(pw) -> str | None`, Python only, #111). L1 field = SHA256(L1)[:16]; L2 field =
  SHA256(L2)[:16] XOR L1. L2 access derives putative L1 = SHA256(pwd)[:16]
  XOR L2field, checks its hash. READ FALLBACK (`validate_password`): meflib
  rule first; only if it fails AND the legacy bytes differ, the old
  last-UTF-8-byte rule (`extract_password_bytes_legacy`), which sets
  `AccessKeys::legacy_password_mapping` -> `LegacyPasswordWarning` /
  `mef3io:legacyPassword`. Never WRITE with the old rule; an append to an
  old segment keeps its keys (and warns), new segments use meflib's.
- **RED data blocks are UNENCRYPTED** even in encrypted sessions (meflib
  default); only metadata s2/s3 and record bodies are encrypted.
- **Index file_offset is FILE-relative** (includes the 1024 B UH); first block
  offset = 1024. Windowed reads must read only the needed byte range
  (`collect_blocks` uses `read_file_range`), not the whole `.tdat`.
- **`.tmet` is a FIXED-length record** (1024 B UH + 15360 B sections =
  `METADATA_FILE_BYTES`); the size check is `>=`, and foreign writers do append
  trailing bytes past its end. The body CRC must be bounded by
  `METADATA_FILE_BYTES`, NOT taken to EOF — hashing the padding rejects intact
  metadata as "corrupted" and, since it throws in the ctor, kills the whole
  session (reported in 1.1.1: 1094 padded files, 0 actually corrupt).
- **NEVER READ OR COPY A `.tdat` WHOLE. ~30 GB is ONE CHANNEL**, so a session
  runs to hundreds of GB or TBs. This is the single easiest way to make the
  library unusable on the files it exists for, and it has been reintroduced
  more than once. The rules: (a) a READ takes the byte extent the request needs
  — select the index entries first, compute min/max offset, then ONE
  `source_->read_range`, and check that extent against `file_size` so a damaged
  index fails loudly instead of reading wild (`collect_blocks` and `read_runs`
  both do this; `read_runs` did NOT until 2026-09-21 and pulled the whole file
  in for a one-minute window — 83.5 MB read for 0.47 MB of data). (b) VALIDATE
  never reads the body: `SegmentState` keeps `tdat_uh` (1024 B) and
  `tdat_size` only. (c) RECOVERY streams — it needs the SIZE plus a 304-byte
  RED header (and the one block it describes) at a few offsets. (d) BACK UP
  WHAT CHANGES, NOT THE FILE: the repair path saves the `.tdat`'s 1024-byte
  header, and recovery saves the header plus the sub-block fragment it drops.
  Copying 30 GB to undo 200 bytes is not a backup, it is an outage. (e) `tar`
  archive/extract stream in 1 MB chunks — keep it that way. Measure with
  `/proc/self/io` `rchar`, which counts bytes read exactly and does not flake
  the way RSS does; `tests/test_p17_large_files.py` pins the read paths.
  (f) `Session` CACHES each segment's `.tidx`, and the index is ~2% of the
  data, so an unbounded cache is O(session) — several GB resident on a
  few-hundred-GB recording. Capped via `set_index_cache_bytes` (default
  256 MB) with LRU eviction, done at the end of a public call.
  `segment_index` returns a `shared_ptr` PIN (the cache holds one reference,
  each call reading the index another), so an eviction — including one by
  ANOTHER THREAD's call on the same Session — never frees bytes in use. It
  used to hand out a raw span, and 4 threads reading one Session with a tiny
  budget crashed ("double free or corruption"). Cache state is under
  `index_mu_`; problems/failed segments/legacy record files under
  `state_mu_`; never held across file I/O or a decode, never both at once.
  Segment METADATA is loaded by the open and IMMUTABLE afterwards:
  `segment_metadata` is const and lock-free (#110); never add a lazy load.
  Pinned by core/tests/test_concurrency.cpp (TSan recipe in its header) and
  tests/test_p31. (g) Benchmark the real shape with
  `benchmarks/long_session_benchmark.py` (24 h x 16 ch @ 512 Hz, 10-min
  appends, windowed reads): watch GROWTH (must stay ~1.00), write/read
  AMPLIFICATION and duty cycle. It validates + dry-run recovers at the end
  and exits non-zero on either failure. Measured there: 1.6x faster per
  block than mef_tools, in a file 1.8x smaller, growth 1.00x — WITH the
  durability barriers the legacy stack does not have.
- **EVERY potentially long binding call RELEASES THE GIL** (#79). Opening is
  where all the metadata parsing happens (64 ch x 60 seg: a 616 ms open froze
  every other Python thread for 613 ms). Reads/writes and the session-level
  functions run through `PyHooks::run`; `toc`, `segments`, `records`,
  `write_records`, `Session.read_runs` and `infer_precision` convert their
  arguments, then wrap the core call in `nb::gil_scoped_release`; the
  `Reader`, `Session` and `SessionWriter` CONSTRUCTORS are bound as
  `__init__` lambdas that call `construct_nogil` (placement-new under
  `nb::gil_scoped_release`), NOT `nb::init<...>`, which holds the GIL.
  Building the result (dicts, lists) happens after, with the GIL. Rule while
  released: touch no Python object (arguments are already C++ values; `self`
  is invisible to other threads until __init__ returns); a callback
  re-acquires the GIL (`PyHooks::call`). Cheap accessors (`info`,
  `channels`, `problems`) keep the GIL. CONSEQUENCE: calls on ONE
  Reader/Session can now really overlap, so its mutable state must be locked
  (see (f) above) and core globals must be immutable (the AES inverse S-box
  is `constexpr` now; it was lazily filled behind an unsynchronised flag, a
  TSan-confirmed race). A new long call or constructor must release the GIL
  too, and add a case to `tests/test_p31_gil_release.py`, which measures the
  main thread's longest stall (best of 3; skips a call under 40 ms) while a
  worker runs the call on a 2400-segment channel.
- **A LIBRARY CALL IS NOT A ROUND TRIP — measure the open at the OS level
  (#141).** On an SMB share each request is ~110 ms and the open is serial
  (209 channels: 11-24 s). Round 1 of the fix replaced the three exists() per
  segment in `discover_layout` with one listing of each `.segd`: that HALVED
  the SessionSource call count and was REVERTED, because a listing is
  opendir + getdents (x2) + fcntl + close (+ an lstat per entry where d_type
  is missing) while exists() is one stat — strace showed ~10% MORE syscalls,
  a local open 10-20% slower, a 2 ms/request FUSE mount only -12% with about
  the same kernel-request count, and per-segment TarSource::list_dir made
  tar opens quadratic. DO NOT RETRY IT. What stayed: (a) the `.tmet` is read
  with `SessionSource::read_head(rel, METADATA_FILE_BYTES)` = first
  min(size, n) bytes, one open + one read, no seek-to-end (strace, 209 ch x
  10 seg: 20,454 -> 14,184 syscalls, all 6,270 lseeks gone; FUSE without
  attribute caching: one getattr less per segment, ~-6%; with caching:
  identical). A short file comes back short, so the loader's
  ">= METADATA_FILE_BYTES" check fails with the same message; padding past
  the record is never read (rchar-pinned in test_p17). (b) TarSource builds a
  dir -> children index once (`children_`), so list_dir is O(log + children),
  not a scan of the whole archive per call. (c) `CountingSource`
  (core/tests/counting_source.hpp: counts list_dir/exists/file_size/
  read_all/read_head/read_range, optional fixed delay per call) +
  `Session`/`Reader(shared_ptr<const SessionSource>)`; test_open_calls.cpp
  pins the open at `1 + channels` listings, `3 x segments` exists and
  `segments` read_head, zero read_all/file_size/read_range (dir and tar),
  and TIMES the open at 100 vs 3200 segments (per-segment cost flat; tar
  within 2x of dir) because a count pin cannot see a call that got more
  expensive. The Python `Reader.__init__` makes no per-segment call
  (test_p17, with a positive control). The real gain for #141 is step 2: the
  metadata pass in parallel with n_threads (latency hiding), then a
  multi-channel read.
- **The open is PARALLEL but must behave SERIALLY** (#141). `discover_layout`
  (session_layout.hpp; inspect calls it with 1 thread) lists channel folders,
  then checks every segment's files, then the records-only `.rdat`, each pass
  on the object's threads (`Work::Io`); `Session::discover` then reads +
  parses every `.tmet` the same way and builds the ChannelInfos SERIALLY,
  rethrowing each segment's stored load error inside `with_segment` in
  segment order. Every pass uses `parallel_try_each` and assembles in listing
  order, so strict raises the failure a serial walk meets first (never the
  first in time) and lenient `problems()` keeps the serial order. Request
  counts are identical at any thread count (the #142 pins hold). Sources must
  be safe for concurrent calls: DirectorySource opens a stream per call,
  TarSource's tables are read-only after its ctor (its member index scan is
  serial — each header gives the next offset — and is now most of a parallel
  tar open). The "open scales linearly" timing pin runs with 1 thread on
  purpose. Measured (CountingSource, 20 ms/request): 209 ch x 10 seg 173 s ->
  10.9 s on 16 threads (docs/benchmarks.md). The `shared_ptr<const
  SessionSource>` constructors of Session/Reader are INTERNAL (tests): they
  skip the naming rule, so applications use the path constructors.
- **The MAIN use case is a session appended to for DAYS TO MONTHS** (5-20 min
  blocks per channel). Two consequences, both load-bearing. (1) An append must
  be O(NEW data), never O(total blocks) — section 2 describes ALL of a segment's
  blocks, so the obvious implementation re-reads/CRCs/walks/rewrites the whole
  `.tidx` every time, which is QUADRATIC over the recording (~14 MB `.tidx` per
  channel at one month). `AppendIndexCache` (writer.hpp, held per channel in
  `SessionWriter::ChannelState`) carries the index summary across appends, so
  entries are appended IN PLACE and the body CRC is EXTENDED over just the new
  bytes — the Koopman CRC is a rolling state with no final inversion, so
  `crc(A||B) == crc(B, crc(A))`, which the `.tdat` path had always used and the
  `.tidx` now does too. Measured: growth over 10 h went 1.77x -> 1.11x. The
  FIRST append after opening a segment still walks it once (the existing blocks
  are only described there), so keep ONE Writer open across an acquisition. The
  fast and slow paths MUST produce identical bytes —
  `test_append_index_cache_matches_the_full_walk` pins exactly that. The
  index is NEVER rewritten whole on either path — the slow path still READS
  it (the totals and the open contiguous run can only come from the
  existing entries) but appends in place like the fast one, because a
  caller that reopens its Writer per block is a natural Python pattern and
  the rewrite was ~47x write amplification for a 10-min block against a
  month-long index. `test_an_append_never_rewrites_the_index_or_the_data`
  pins it for BOTH durability settings, self-calibrating against actual
  file growth. (2) `durability="fast"` (`Writer(durability=)`) drops the
  barriers. NOTE this is the DEFAULT (changed 2026-09-21) and it is NOT an
  "atomic writes" story: `.tdat` and `.tidx` are extended IN PLACE, so without
  barriers a crash can leave a torn tail on either, an index referencing data
  that never landed, or blocks the index never mentioned. Only `.tmet` is
  published by rename. Every one of those is detectable (`index.block-offsets`,
  `index.data-coverage`, the body CRCs, `header.entry-count`) and fixed by
  `recover_session`, which is what makes the default defensible — the failure
  mode is "run recover after an unclean shutdown", and an EARLIER append is
  never at risk because nothing rewrites it. Measured on the real workload
  (24 h x 16 ch @ 512 Hz): 1.6x faster per block than mef_tools even with
  `durability="full"`, because the encode parallelises. (3) `recover_session` (core/src/
  recover.cpp, `python -m mef3io recover`) is the ONLY thing that may touch
  the index or data — repair_session never does, which is what keeps it safe
  on anything. Index-ahead-of-data drops entries; data-ahead-of-index
  REBUILDS them from the RED block headers (CRC-checked). Dry run by
  default. ONE declaration it does write (#83): at a NON-ZERO rto, where the
  validator's time checks stand down, it corrects the END TIME of the three
  UHs (.tmet = what info() reports) from the last kept block
  (`plan_end_times`, needs rto + fs from the .tmet, so `password=` matters).
  Compared in USER time; stale ONLY if before last block START + half a
  sample, or (blocks rebuilt) before computed end - half — mef_tools
  truncates its end, inclusive-end writers are a period short, neither is
  stale (negative control in test_p18 with mef_tools record_offset at
  499.7 Hz); NEVER moved earlier; each header in its OWN convention (only a
  negated end is rewritten, a plain one -> note); the last block's time must
  be negated. Off rto != 0 the .tidx/.tdat end keeps the old behaviour (last
  block start; repair fixes it at rto 0). READS: a .tmet is read only for a
  segment recovery writes anyway, plus the 1 KB UH of each channel's LAST
  segment (the only one a lost .tmet rename can hit), the full record only
  when the three UH ends differ (pinned by chmod-000 non-last .tmets). .tmet
  header backed up, stamped Recovery. The .tdat body CRC is reset to NO_ENTRY
  only on truncation, rebuilt blocks, or a stale .tdat entry count — a
  header-only fix keeps it. At rto 0 the .tmet stays byte-identical. (4) Docs: `docs/long_recordings.md`, `docs/validation.md`,
  `for_agents/INVARIANTS.md`.
- **Durability is scoped, deliberately** (`core/src/durability.hpp`, shared by
  writer.cpp and validate.cpp — they had drifted, and the path writing SAMPLES
  was less careful than the one rewriting declarations). Three barriers are
  load-bearing on APPEND and must not be removed for speed: fsync the `.tdat`
  BEFORE writing the `.tidx` that points at those bytes (otherwise a power cut
  leaves an index running past the real end of the data file — a state the
  in-process rollback cannot reach, because the process is gone); fsync the
  appended `.tidx` before patching its header; and fsync the `.tmet` temp
  file before renaming it over the original (a rename is ORDERED, not durable, so otherwise it can publish a name over bytes
  still in cache, and a short `.tmet` throws from the metadata loader and kills
  the WHOLE session). Two things are deliberately NOT fsynced: a FRESH write
  (nothing underneath to lose — a crash leaves an incomplete session either
  way), and the DIRECTORY entry after a rename (losing the rename just keeps
  the previous complete, consistent file, which is worth more than keeping the
  last append). Cost on ext4, measured: fresh write unaffected (~80 MB/s); an
  append pays ~3 fsyncs per channel, so a 128-channel append is ~5 s where the
  fresh write is ~0.4 s. That is the price of not corrupting a clinical
  archive; do not "optimise" it away without replacing the guarantee.
  THE FLUSH IS THREE DIFFERENT CALLS with three different rules, and the
  platform differences are load-bearing: Linux `fdatasync`, macOS
  `F_FULLFSYNC` (with an ENOTSUP fallback), Windows `FlushFileBuffers`.
  **Windows requires the handle to carry GENERIC_WRITE** — unlike POSIX, where
  fsync on an O_RDONLY descriptor is legal — and opening for read there fails
  ERROR_ACCESS_DENIED. That shipped: `durability="full"` threw on the first
  append, and the callers that IGNORE the result (`durability="fast"`, the
  recovery backups) silently had no durability at all, which is the worse half.
  Use `CreateFileW`, not `CreateFileA`, or a non-ASCII session path cannot be
  opened. A round trip can never catch this class of bug — the bytes land
  either way on an orderly shutdown — so the primitive is tested directly in
  `core/tests/test_durability.cpp`, and `fsync_file` returns the OS error
  through its `why` out-param because "the write is not durable" with no cause
  is a bug report nobody can act on.
  WHY IT REACHED A RELEASE: no Windows job had ever executed a WRITE. `cpp-tests`
  was Linux-only, `windows-smoke` only imported the module, and cibuildwheel's
  `test-command` was also just an import. `tests/test_p18_recovery.py` already
  covered a `durability="full"` append and would have caught it — on a platform
  that ran it. Now: `cpp-tests` and `python-tests` both include windows-latest
  (pymef ships win_amd64 wheels, mef-tools is pure Python, so the oracle stack
  installs there), and cibuildwheel runs the five oracle-free suites against the
  built wheel on every arch.
- **NEVER `finalize_crcs` a `.tmet`, and never `s2.serialize` one you did not
  build.** Both are write-side traps that mirror read-side gotchas above, and
  the append fell into both for a long time (fixed 2026-09-21). (a) `.tmet` is
  FIXED-length, so its body CRC must be bounded by `METADATA_FILE_BYTES`
  (`finalize_metadata_crcs`), not taken to EOF — foreign writers pad past the
  record, and hashing the padding writes a CRC the READER rejects, which throws
  from the metadata loader and takes the WHOLE SESSION down. mef3io destroying a
  file its own validator had just called clean. (b) Section 2 is 10752 B but
  `TimeSeriesMetadataSection2` models only up to offset 6432, and `serialize`
  zero-fills: a full re-serialize wipes meflib's protected region (6432, 2160 B)
  and discretionary region (8592, 2160 B) — 4320 bytes of a foreign writer's
  metadata — and re-NUL-terminates every text field, shortening one that filled
  its field exactly. Edit the STORED image with `serialize_derived_fields`
  instead (decrypt → edit → re-encrypt when the section is encrypted), which is
  what the validator's repair path already did.
- **`block_envelope` (Session/Reader, viewers)** reads ONLY `.tidx` (rchar-
  pinned in test_p38). Skipping (`Session::skip_segment`, shared with
  collect_blocks; #110 item 3: repeated 1-s reads of 2400 segments 4.7 ms ->
  0.2-0.28 ms) has ONE rule: the learnt span of the CACHED index
  (`learn_span`: every entry's [start, start + n/fs) by the walks' own
  expressions, unbounded if any count is 0/NO_ENTRY; stored only if that pin
  is still the cached index; DROPPED in trim_index_cache with the index;
  under index_mu_) misses the window — exactly the test every entry of the
  cached index would fail, so the result equals walking it, and nothing is
  read either way. NO METADATA TIME IS EVER USED TO SKIP (correctness over
  speed, maintainer 2026-10-25): a .tmet END is stale after an interrupted
  append + new segment, = last block's start for some writers, the caller's
  end_uutc from mef_tools; a .tmet START can be late (any sign convention);
  a span outliving its index hid an append forever. #146 review rounds 1-2
  lost samples on every one; test_p42 + test_parallel_open.cpp pin them. A
  cold windowed read therefore walks every index once, as before.
  Per-block min/max are the INDEX
  copies (raw counts, scaled by the block's OWN SEGMENT's ufact like read(),
  #150; swapped when negative; unset ufact = counts, NaN/inf refused); RED_NAN ->
  NaN. Times come from the index entries, not the RED headers (reading those
  would mean touching .tdat); read() places by the header copy, which agrees
  on any consistent file. Python + C++ only (no C ABI/MATLAB yet).
- **Multi-channel reads (#90; part of #68).** `Reader::read_many` /
  `read_many_f32` (-> `ChannelRows<T>`: one uninitialized `new T[]`
  row-major buffer, handed to numpy as 2-D with NO copy) and `read_f32`; ONE
  implementation `read_many_impl<T>` serves read() too (one channel, a
  std::vector NaN-filled at assign; its path, error and progress are main's:
  collect_blocks, plan, factor refusal, then progress = decode bytes).
  Arguments FIRST, before any request: empty -> invalid_argument; each name
  channel_info -> out_of_range; for a LIST only (one channel has no rate rule
  — a NaN-rate single read returns empty as before) a non-finite rate, then
  a rate != the first's -> invalid_argument (MultiReader mirrors both texts:
  `_one_rate_message`, `_unusable_rate_message`, rates as %.17g). Unset
  t0/t1 = union of the channels' spans. `collect_blocks` = `select_blocks`
  (index only; a segment's walk error is STORED in the BlockSelection and
  raised/contained by `fetch_blocks` when it reaches that segment, so the
  serial order of errors and lenient problems() is unchanged; strict stops
  the walk at the first error, `will_fail`) + `fetch_blocks` (data). A list:
  (A) select every channel on Work::Io threads (progress total = every
  selected block's bytes, constant; blocks not decoded are added after each
  group); (B) groups of resolve_threads(n, Io) channels: fetch + plan +
  factor refusal per channel on Io threads, then ONE parallel_for over the
  group's (channel, job) pairs, plans freed per group (peak = one group's
  compressed window). ERRORS = the LOWEST failing channel in list order
  across all stages: per-channel exception slot + atomic `limit` (lowest
  known to fail, incl. a select `will_fail`); nothing starts for i > limit,
  channels below it run on (pinned in test_read_many.cpp: corrupt block of
  an earlier channel beats a later truncated .tidx; a damaged first channel
  fetches no data). Each slot = T(double(count) * its job's effective factor)
  = read_raw's spans applied (spans merge only EQUAL factors); float32 =
  static_cast of that double = numpy astype. Rows' NaN fill runs in parallel
  (serial first touch was 0.53 of 0.79 s). `block_envelope_many` =
  per-channel block_envelope on Work::Io threads, first error in list
  order. Python: `read(str|list|tuple, dtype=)` (2-D for a list;
  `_channels_argument` / `_float32_argument`); `block_envelope(list)` -> dict
  keyed by name (duplicates once). MultiReader: a LOOP of its own single
  reads (budget pinned in test_p42), progress in UNITS OF CHANNELS (2^20
  each, constant total), warnings after success; equals the merged Reader
  (test_p45). compat get_data(list): grouped by (fs, resolved t0, t1) ->
  read_many, each row COPIED (own array), on any error re-run as the old
  per-channel loop so the error is the first in the requested order.
  read_raw stays single-channel; C ABI/MATLAB not yet. Tests:
  tests/test_p45_read_many.py, core/tests/test_read_many.cpp, GIL in
  test_p31; bench "[.read-many-bench]" + benchmarks/read_many_benchmark.py.
- **Conversion factors are PER SEGMENT (#150).** Section 2 of each segment
  stores its own units_conversion_factor and one channel may mix them
  (`new_segment=True` with another precision, foreign recorders; meflib's
  channel aggregate is NO_ENTRY = 0.0 then, meflib.c:5494/6176, pymef shows
  it). NEVER scale by a channel-level value: `BlockJob` carries its segment's
  factor, `read()` scales by `RawData::factor_spans`, block_envelope and the
  copy's native min/max use the segment's, `read_runs` splits runs where it
  changes. `ChannelInfo`/`RawData::units_conversion_factor` are
  `std::optional` (nullopt = differ; Python None; C ABI/MATLAB NaN).
  `factor_spans` are CANONICAL (valid slots grouped into maximal runs of one
  factor, first valid to last valid + 1) so a MultiReader can rebuild the
  merged session's exactly. MAINTAINER DECISION (#151): a factor NOT SET
  (0 = NO_ENTRY, meflib/pymef template default, so real files have it) is
  read as 1 (`effective_units_conversion_factor`): read(), block_envelope,
  MultiReader and compat get_data return the RAW COUNTS, the core records
  the channel (`Session::unscaled_channels`, under state_mu_) and Python
  emits ONE `UnscaledSamplesWarning` per Reader/MultiReader and channel
  (`_announced` "unscaled"); spans/info/segments keep the STORED 0. NaN/inf
  are invalid: REFUSED by read()/block_envelope (FormatError, before
  decoding; block_envelope decides by read()'s window + grid
  (`detail::grid_slot`) and throws OUTSIDE with_segment so a lenient
  session never marks the segment failed); read_raw/toc still give counts.
  C ABI/MATLAB: raw counts, no warning yet. `mef3io_reader_segment_table`
  = every segment's info/provenance/factor from ONE segment map (the MEX
  uses it; per-index loops were O(n^2)). pymef oracle in test_p44:
  its session_md arrays point into memory close() frees — copy them first.
  C ABI: `mef3io_reader_segment_units_conversion_factor`,
  `mef3io_reader_read_raw_factors` + `mef3io_factor_span` (no struct grew).
  Tests: tests/test_p44_segment_factors.py, Catch2 "per-segment conversion
  factors".
- **Grid placement ties (#129).** `reader.cpp` has TWO roundings and they
  must stay different: `grid_count` (window length, `llround`, argument never
  negative; the C ABI's `mef3io_reader_n_samples` mirrors it) and
  `grid_slot` (where a block's first sample lands: nearest slot, a TIE to the
  EARLIER slot whatever the sign, `ceil(x - 0.5)`, so slot 0 is the first
  sample at or after t0). `llround` for placement rounded halves away from
  zero: the block holding t0 (negative offset) and later blocks (positive)
  went opposite ways, leaving one unclaimed slot = a NaN at the first block
  boundary of every window starting exactly half a sample off the grid
  (500/250/1000/200 Hz with ms event times). `tests/test_p37_grid_ties.py`
  scans every integer-us start offset across a sample period. KNOWN, left
  as is: when BOTH t0 and t1 are exact ties, `grid_count` can be one more
  than the samples in [t0, t1), so the last slot is NaN (2 of ~154k swept
  windows). Using the slot rule for the length would change ordinary
  on-grid lengths (500 Hz, 1 ms window: 1 -> 0) and the C ABI's
  `mef3io_reader_n_samples` contract.
- **Block ranges can OVERLAP on the sample grid.** Only writers that put every
  block start exactly on the grid (mef3io's own) tile the output cleanly;
  foreign recorders carry acquisition jitter + per-block us rounding, so a
  block can start a few samples before the previous one ends — occasionally a
  short block lands entirely inside its predecessor. `read_raw` therefore
  partitions the output into disjoint per-block pieces BEFORE decoding
  (`claim_range`, descending job order = last block wins = what a serial
  scatter gives). Never let workers scatter by block offset directly: that is a
  silent data race on the overlapped samples, not just a tie-break question.
  Partitioning up front also means a block a later one covers outright owns
  nothing, so it is not decoded at all (halves decode time on such geometry).
- **Section-2 `maximum_*` fields are an ALLOCATION CONTRACT, not statistics.**
  `0` is NOT the NO_ENTRY sentinel for any of them (`maximum_difference_bytes`
  / `maximum_block_samples` → `0xFFFFFFFF`; the si8 ones → `-1`), so a reader
  cannot tell unset from measured. CAREFUL WITH THE MECHANISM — and do NOT
  conclude "no reader consumes this" from `reference_files/`. THAT IS ONE
  MEFLIB BUILD. In the copy vendored here `RED_allocate_processing_struct` has
  no call site at all (only the prototype meflib.h:1184 and the definition
  meflib.c:6453) and the fields are merely initialised, rolled up and printed —
  but the meflib build behind CyberPSG DOES allocate from section-2 sizes, and
  that is where the crash was reproduced. That build is not available to us, so
  the vendored source can prove a mechanism EXISTS and can never prove one does
  not. Treat all six as load-bearing; prefer over-declaring (wastes memory) to
  under-declaring (truncates a buffer). The two known mechanisms: (a)
  `RED_allocate_processing_struct` skips the alloc on size 0 → NULL
  `difference_buffer` → `RED_decode` writes through it, and the guard meflib
  ships for this (`RED_check_RPS_allocation`, meflib.h:1186 / meflib.c:6534)
  is NEVER CALLED — no error path at all; (b) `find_discontinuity_indices`
  (meflib.c:3548) mallocs `number_of_discontinuities` entries then writes one
  per FLAGGED block — the legacy `mef_tools` `0` is a straight heap overflow
  (hence Error severity), though established by reading the C source, NOT by
  reproducing a crash. (a) is the one with a post-mortem: an access violation
  inside RED decoding was reproduced against a meflib-based reader, and an
  in-memory-only patch of that single field decoded byte-identically to the
  on-disk repair — which isolates the cause to it. Do not rank (b) above (a).
  FULL MATRIX CONFIRMED (2026-09-21, `scripts/make_reader_compat_check.py`): nine
  sessions opened in CyberPSG, and ONLY 05 CRASHED. 05 is the deliberate
  negative control carrying the 1.1.2 zeros, so the matrix is meaningful rather
  than vacuous; 06 is BYTE-FOR-BYTE THE SAME SAMPLES with only the declarations
  repaired and it decodes, which isolates the cause to metadata section 2 about
  as tightly as it can be isolated. Everything else decoded: a fresh write (01),
  one through the in-segment append path (02), a legacy `mef_tools` session
  repaired by `repair_session` (04), encrypted (07), a `.tmet` carrying foreign
  padding across an append (08 — the critical bug fixed this round), and a
  session rebuilt by `recover_session` after an interrupted write (09).
  MECHANISM (b) DID NOT FIRE. 03 is an untouched legacy session with real gaps
  and `number_of_discontinuities = 0`, which by `meflib.c:3548` should overflow
  a zero-length malloc — and it decoded fine. Do NOT read that as "(b) is not
  real": the same rule applies in both directions as for the vendored source —
  one build tolerating it proves that build does not take the path (CyberPSG may
  simply never call `find_discontinuity_indices`), not that no build does. So
  KEEP `times.discontinuities` at Error severity; the reasoning for it is
  unchanged, it is simply still unreproduced. What this DOES settle is that (a)
  is the mechanism behind the reported crash, and that mef3io's output, its
  repairs and its recovery are all acceptable to that reader.
  THE FIX IS CONFIRMED AGAINST THAT READER FAMILY (2026-09-17): a session
  written by this version, and a legacy `mef_tools` session brought up to date
  by `repair_session`, were both opened in CyberPSG and DECODED — traces drawn,
  no access violation. That is the half that matters: the original failure let
  `ReadSession` succeed and blew up later inside `RED_decode`, so "it opens" is
  not evidence. The gap also landed in the right place and with the right
  duration (3.0 s at 47-53% of a 49.875 s record), so block placement by
  timestamp agrees there too — a third independent reader backing the
  reconciliation below. Note this covers the repaired file, in which
  `maximum_contiguous_block_bytes` was LOWERED from the legacy whole-file total
  to the longest run; that was the only declaration made smaller rather than
  larger, and it is the one now known to be safe in practice.
  (Details of that investigation are held privately; do not restate them in
  tracked files — this repo is public.)
  pymef passes
  neither (it sizes from `RED_MAX_DIFFERENCE_BYTES(maximum_block_samples)`),
  which is why the oracle never saw any of this. Fixed in 1.1.3 (reported against 1.1.2, which left
  `maximum_difference_bytes` and `maximum_contiguous_block_bytes` at 0 and set
  `maximum_contiguous_blocks`/`_samples` to the channel totals). The writer now
  measures all six: `maximum_difference_bytes` from each encoded block's RED
  header (`RedBlockHeader::DIFFERENCE_BYTES_OFFSET`, read back rather than
  threaded out of the encoder), the contiguous trio from runs delimited by the
  `.tidx` discontinuity flag — the same flag a reader uses — via the
  `ContiguousRun` accumulator in writer.cpp. Each maximum is tracked
  independently: over-declaring only wastes a reader's allocation,
  under-declaring truncates its buffer. `maximum_contiguous_*` is repaired in
  BOTH directions — the declaration must state what the index holds. (It was
  grow-only until 2026-09-17, on the reasoning that no reader in
  reference_files consumes the trio so longest-run is mef3io's inference;
  reverted because that left over-declaration unfixable — recorders in the
  field over-declare these by orders of magnitude, and the wasted allocation
  scales with channel count — and because it made mef3io disagree with an
  independent third-party patcher, which lowers, on the same file.) On APPEND the contiguous trio is
  recomputed exactly from the full `.tidx` (so appending repairs a segment
  written by an older mef3io), but `maximum_difference_bytes` lives in `.tdat`
  headers — folding old blocks in exactly would cost a seek per block and break
  the O(new data) append. So the append takes it from the one source that costs
  nothing: `SessionWriter` carries the exact running maximum in `ChannelState`
  for every segment IT encoded, and passes it as `SegmentSpec::
  known_difference_bytes`, which keeps a chunked write exact. Only a segment
  REOPENED from disk (or written by anyone else) leaves that unknown, and there
  the stored value is UNVERIFIABLE — it may be honest, or 0, or NO_ENTRY, or a
  plausible-looking number that is simply wrong — so meflib's
  `RED_MAX_DIFFERENCE_BYTES` = `5 × maximum_block_samples` is taken as a FLOOR,
  which bounds every block whatever the stored value meant. Screening the
  stored value against the blocks being APPENDED is NOT enough and was the bug:
  it catches a stored 1, but a stored 3000 against a true 12488 rides through
  whenever the new blocks are smaller. `5 × samples` is also exactly what the
  third-party patcher writes in its DEFAULT `--diff-bytes bound` mode.
  READ PATH NEVER CONSULTS THESE — mef3io sizes from each block's own header,
  so zeros/sentinels/nonsense still read fine; `test_p12_sizing.py` pins both
  halves. Cross-checked against an independent third-party patcher in exact
  mode → "already consistent".
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
- **RED encode**: first emitted byte is junk (meflib overwrites stats[255] then
  restores) → drop emitted[0], payload = emitted[1:] at offset 304; stored
  difference_bytes = generated+1. Lossless no-detrend/no-scale, pymef-readable.
  Constants: TOP_VALUE 0x80000000, CARRY_CHECK 0x7F800000, SHIFT_BITS 23,
  EXTRA_BITS 7, BOTTOM_VALUE 0x800000, PAD_BYTE 0x7e, 8-byte alignment.
- **Records**: header 24 B (crc@0,type[4]@4,vmaj@9,vmin@10,enc@11,bytes@12,
  time@16). Body padded to 16-byte multiple with 0x7e. L2-encrypted when the
  session is encrypted. `.ridx` entry 24 B (type@0,vmaj@5,vmin@6,enc@7,
  offset@8,time@16). file_offset FILE-relative.
- **Fixed-width strings**: text fields (units_description 128 B,
  channel/session_description 2048 B, subject_* 128 B, …) are NUL-terminated,
  so max content is field_len - 1. `byteio::write_string` enforces that AND
  backs the cut off to a UTF-8 character boundary — a half-written multi-byte
  char makes the whole session throw `UnicodeDecodeError` on open in Python.
  The 4-byte record type code ("EDFA"/"Note") is the exception: it fills the
  field with no terminator, so it uses `byteio::write_fixed_code` instead —
  which REQUIRES an exact-width value. Padding/trimming a type code writes a
  header claiming a type the body was not built for (writing "Notes" stored a
  "Note" header with an empty body, silently dropping the text), so
  `write_records` rejects any type that is not 4 ASCII bytes up front, before
  opening a file. Unknown 4-char types still pass through with an empty body.
- **`reference_files/` IS PRESENT in this repo** (gitignored):
  `meflib-multiplatform/` (authoritative C), `pymef-develop/`, `mef_tools/`,
  `mef3_dump-main/`. Settle every format question against it — do not reason
  from memory, and do not trust a doc comment that cites it.
- **Sign conventions can be MIXED WITHIN ONE FILE — always compare via
  `to_user_time`.** Verified 2026-10-14 against the installed legacy stack
  (mef-tools 1.2.3 / pymef 1.4.8): it stores EVERY time meflib-style as
  −(t − rto) — `.tmet`/`.tidx`/`.tdat` universal headers, index entries, RED
  block headers and record headers alike (−t at rto 0). Older fixtures were
  seen with `.tidx`/`.tdat` universal-header times NOT negated, and files that
  passed through other writers can mix conventions, so never assume one. What
  makes `rto != 0` hazardous is that a time at or before `rto` cannot be stored
  as −(t − rto) (it turns non-negative and every reader takes it as absolute).
  So the validator's time checks still stand down entirely when `rto != 0`
  rather than risk rewriting a correct file.
- **Oracle**: use `pymef` `read_ts_channels_sample([ch],[0,nsamp])` for decoded
  int32 (no gap NaN) and `read_ts_channels_uutc` for gap-filled. `mef3_dump` is
  NOT usable (reads the encryption sentinel byte unsigned). Manifest `nsamp` !=
  stored nsamp (it's get_raw_data length incl. gaps) — compare vs pymef
  basic_info. Oracle agreement is exact for grid-aligned block times; when
  block timestamps drift off the grid the two layouts differ *by design* —
  meflib packs blocks contiguously by sample count within a run, mef3io places
  each block at its own timestamp. Not reconciled (see next section).

## Known limitations / next steps

- **In-segment append implemented** (`append_time_series_segment` +
  `SessionWriter` hydration): non-first writes extend the channel's last
  segment in place (.tdat streamed-CRC append, .tidx extend, .tmet s2 rewrite);
  `new_segment=True` forces a fresh segment. Appends validate fs/ufact/start
  time vs on-disk metadata (`WriteConflictError` → Python RuntimeError); float
  appends reuse the segment's precision. The append extends the last segment
  folder by its ON-DISK name (`ChannelState::last_segment_stem`, chosen by the
  last_segment_dir rule; append_time_series_segment names the files after
  the folder), so foreign un-padded `ch1-5.segd` works; new segments are
  always `%06d`. ARGUMENT RULES (#83, writer.hpp `check_signal_parameters` /
  `check_block_length` / `MAX_BLOCK_SAMPLES`, called by SessionWriter AND the
  free segment functions BEFORE anything is read or written): fs finite > 0;
  ufact finite and != 0 (0 = meflib NO_ENTRY, meflib.h:439; NEGATIVE ALLOWED =
  polarity inversion, pymef3_file.c:972-978 swaps native min/max, and so do
  the writer and block_envelope — oracle-tested in test_p41); block <=
  MAX_BLOCK_SAMPLES (5n+311 must stay < ui4 NO_ENTRY), the fs-derived block
  decided in double before the cast and clamped to >= 1 (fs < 0.1 Hz used to
  loop forever); n/fs*1e6 must stay in si8 range (fs=1e-300); valid mask
  length = data length. Python Writer checks block_length BEFORE its ctor
  creates the folder; MATLAB Writer.m does the same via MEX `check_block_length`
  -> C ABI `mef3io_check_block_length` (also `mef3io_check_signal_parameters`,
  `MEF3IO_MAX_BLOCK_SAMPLES` static_asserted equal to the core's): writer_open
  with Overwrite removes the session, so nothing may be refused after it. MATLAB Writer.m passes full() arrays and converts
  string annotation fields; the MEX refuses sparse/complex BUFFERS
  (`require_dense_real`); scalars accept sparse (get_scalar refuses complex only).
  `Reader.segments(ch)` maps what data
  is where per segment. First appended block keeps discontinuity=true (readers
  are time-gridded so contiguous appends stay seamless).
- **Off-grid block times: RECONCILED — the old entry here was wrong.** It
  claimed pymef `read_ts_channels_uutc` packs blocks contiguously by sample
  count while mef3io grids by timestamp, and that the two are unreconciled on
  real files. Not so. pymef takes `times_specified`: `read_ts_channels_uutc`
  passes `True` (mef_session.py:1401) and places EVERY block by its own
  timestamp — `decomp_data + ((block_start_time_offset - start_time)/1e6 * fs
  + 0.5)`, pymef3_file.c:2250. The contiguous `sample_counter` packing is the
  `else` branch (:2258), reached only by `read_ts_channels_sample`
  (mef_session.py:1325, no 4th arg). Same rule as mef3io's, so on a real file
  they AGREE — verified: jitter every block off-grid and both readers return
  identical samples.
  WHAT THE OLD REPRODUCTION ACTUALLY SHOWED: `tests/test_p6_threads.py`'s
  `_shift_block_times` rewrites ONLY the `.tidx`. A block's start time is
  stored TWICE — in the index entry and in its RED header (`.tdat` +40) — and
  that helper desynchronises them. mef3io reads the index copy; pymef's uutc
  path reads the RED-header copy. Two readers, two different copies of one
  value, by construction. Nothing about placement philosophy.
  RESOLVED 2026-09-21 (#11 + #13). **The RED BLOCK HEADER is now the authority
  for where a block's samples go and how many there are; the `.tidx` only
  SELECTS blocks** — which is exactly what meflib does. Both values are stored
  twice (index entry, and block header at `.tdat` +40 / +32) and nothing in the
  format keeps them in step, but the copies are not equally trustworthy: the
  header is covered by the per-block CRC that the decoder verifies, the index
  only by the `.tidx` body CRC, which the read path never checks. Read in
  `collect_blocks` as two field reads at fixed offsets — NOT
  `RedBlockHeader::parse`, which copies the 256-byte statistics table per
  block. Disagreements are collected into `BlockJobs::mismatches` →
  `RawData::block_copy_mismatches` → `read_raw()["block_copy_mismatches"]` and
  a Python `BlockCopyWarning`; `read()` applies the same placement rule but
  returns a bare array, so inspect via `read_raw` or the Validator.
  Verified: 0 differing samples vs pymef on a jittered self-consistent file,
  and an index count that understates a block used to turn 5000 real samples
  into NaN and now loses none. `_shift_block_times` moves BOTH copies and
  re-seals the block CRC; moving only the index made an INCONSISTENT file, not
  a jittered one, which is what made the two libraries look like they disagreed
  about layout. Tests: `tests/test_p19_block_copies.py`.
- **Do NOT `pip install -e .` for C++ dev** — scikit-build-core's editable hook
  loads an install-time extension snapshot that shadows the dev_build symlink
  (meta-path beats sys.path). Keep mef3io uninstalled; use scripts/dev_build.sh.
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
- **MATLAB binding implemented**: flat C ABI (`core/include/mef3io/c_api.h`,
  Catch2-tested) → single command-dispatch MEX (`matlab/mef3io_mex.cpp`) →
  `+mef3io` Reader/Writer classes. Build with `matlab/build_mex.m` (C++20
  compiler; do NOT add -fvisibility=hidden — it hides the MEX version symbols
  → "not supported in current release"). `matlab/test_mef3io.m` = round trip;
  validated cross-language both directions vs Python/pymef with R2026a.
  Append-overlap check has half-a-sample-period slack (per-block half-us time
  rounding can store a segment end ~1 us past the grid-exact end).
- Distribution: **version single source of truth = repo-root `VERSION` file**
  (pyproject regex provider → wheel metadata; CMake → `mef3io::version()` and
  the extension's `__version__`; `mef3io.__version__` prefers installed dist
  metadata). Release flow: manually run the `bump-version` workflow
  (patch/minor/major or explicit) → commits VERSION, tags vX.Y.Z, dispatches
  `release.yml` → cibuildwheel on linux x86_64+aarch64, windows AMD64+ARM64
  (cp311+ on ARM), macOS arm64+x86_64, + sdist → twine upload using the
  `PYPI_Token_General` secret. `ci.yml` = tests on push/PR (main, dev).
  Published on PyPI as `mef3io`; mef_tools is archived and mef3io replaces it
  (`from mef3io import MefReader, MefWriter`). Before every release, a full
  docs revision PR (docs/releasing.md); notes collect in
  docs/release_notes/unreleased.md between releases and are renamed to
  X.Y.Z.md then. Keep mef3io brand-neutral; brainmaze-mef3-server should
  depend on it (see docs/design.md).

MultiReader (`python/mef3io/_multi.py`, Python only, no C++): several
recordings read as ONE without merging on disk. A DROP-IN for `Reader`: every
public member, same signatures (test-pinned), same return types/keys/dtypes —
extra info lives ONLY in members Reader lacks (`paths`, `metadata_by_source`).
CONTRACT (test_p39): `read`/`read_raw` == a Reader over the same parts merged
with `merge_sessions`, sample for sample, and a battery of every Reader call
on the merged session == the same on a MultiReader over its parts. How it
stays exact: every touched recording is read with the SAME t0 as the outer
window (any other t0 re-rounds the grid at 256 Hz / 1000/3 Hz), with t1
shortened to its end + 2 samples (re-read with the full t1 if its last slot
is valid = clipped); n uses C's llround (halves AWAY from zero, not Python's
round). Slots two recordings both fill (touching within half a sample) go to
the block that STARTS later (merged segments are time-ordered, later wins),
from block_envelope around those slots only. Combined values: agree -> value,
differ -> None + ONE `MultiReaderDifferenceWarning` naming fields only
(never the first file's value). Patient fields + session_description are
combined over EVERY recording (info and metadata share `_session_wide`); a
session_description equal to its own recording's name is the writer default
and is ignored (else day files warn on every open, and -W error breaks the
constructor). read() DROPS the parts' collected BlockCopyWarnings (Reader.read
never emits one); read_raw emits them. Errors MultiReader raises itself use
Reader's exact text. Refused on open (ValueError): rate differs,
overlapping data (merge's rule: spans first, block_envelope only where spans
meet, half-sample tolerance). Different ufact (#150): as a Reader, read
scales by the parts' read_raw `units_conversion_factors` spans (per-slot
factor via the owner array), read_raw returns the canonical spans rebuilt
over the combined window (== the merged session's), and refuses an unusable
factor with Reader's exact text (`_unusable_factor_message` in _reader.py
mirrors `unusable_factor_message` in session.cpp). Sub-readers are opened with the private
`Reader(_defer_warnings=True)` and their warnings re-emitted by the
MultiReader at ITS caller; Reader's `_read_raw`/`_records` collect warnings
into a `pending` list for the same reason (`_flush` emits them).
`segments()` renumbers from 0 in time order (records(segment=) and
declaration_issues use that numbering; problems keep the folder number).
THREADS: `n_threads` is a TOTAL budget (resolved by the core's
`resolve_threads`, never by Python): the open runs min(n, budget_io) opens at
once with budget_io // workers threads each; a read gives each touched
recording budget // touched through the PRIVATE `Reader._read_raw(_split=)`
-> binding keyword `_n_threads` -> C++ `Reader::read_raw_split`; a read
inside one recording gets the whole budget. Setting `n_threads` re-splits
(sub-readers' counts updated). The private per-call path exists ONLY for
this: never re-expose a public per-call count (test_p42 pins signatures and
that the budget is never exceeded on open or read).

Per-segment isolation (#12): a Session/Reader opened `strict=False` CONTAINS a
segment-level failure instead of letting it fail the whole session — the
segment is skipped, its span reads back as a gap, and it lands in
`problems()`. Strict is the DEFAULT everywhere and must stay so: a skipped
segment's samples are NaN, indistinguishable in the array from a real
recording gap, so silence is how missing data reaches an analysis. The seam is
`Session::with_segment(channel, seg, fn)` — it no-ops for an already-failed
segment (so a bad file is not retried per call), rethrows when strict, and
records a `SegmentProblem` when not. EVERY caller must ROLL BACK what `fn`
appended before it threw (`out.resize(mark)`; collect_blocks restores buffers,
jobs AND mismatches; read_runs also drops `current`, which points into the
vector it resizes) — a half-written segment is worse than a skipped one. Note
`continue` inside those lambdas means the inner loop; use `return` for "this
segment contributes nothing". problems() entries carry `kind` = "segment" |
"records" (C ABI: `mef3io_reader_problem_kind`, struct layout unchanged);
record-file problems have segment_number −1 at session/channel level and
never mark a segment's data unreadable. `info.n_segments` counts READABLE segments so it
cannot out-count `segment_map`. Surfaced as Python `Reader(strict=False)` /
`Reader.problems` / `UnreadableSegmentWarning`, C ABI
`mef3io_reader_open_ex` + `mef3io_reader_n_problems`/`_problem` (the original
`mef3io_reader_open` keeps its signature), MEX `reader_problems`, MATLAB
`Reader(path, pwd, nThreads, false)` / `r.problems()` / `mef3io:unreadableSegment`.
`test_api_parity.m`'s map must list any new method or the release MATLAB job
fails. Tests: `tests/test_p20_segment_isolation.py`, block in
`matlab/test_mef3io.m`.

Warnings (#96): EVERY Python warning is a `MefWarning` (`python/mef3io/
_warnings.py`, a UserWarning) emitted through `_warnings.emit(cls, msg,
path=, details=, stacklevel=)` — NEVER a bare `warnings.warn` (test_p30 bans
importing `warnings` outside _warnings.py, by AST). The session goes in `w.message.path`, the structured
items in `.details`; the TEXT carries no path: `emit` scrubs the session's
LOCATION and NAME to `<session>`: the path as given/abs/real, the backend's
own location and name (`Reader.session_identity()` — a RENAMED tar keeps
its original inner folder name), `<session>::<first component>`, and the name
as a WHOLE WORD anywhere (it turns up after a space, e.g. "no <name>.rdat").
ONLY those: channel and file names inside the session still appear and can
be identifying — never claim more. `emit` keys a warning registry by
(session, calling file) (`warn_explicit`), so "default"/"module" stay once
per session now that the path is out of the text. `stacklevel` means what it means to
`warnings.warn` at the call site. The pyproject filter matches the text after
the old path prefix, so it still works. `inspect()` emits NO Python warnings:
it reports conditions as `SummaryWarning` data (incl. `legacy-password` +
`SessionSummary.legacy_password_mapping`, from the .tmet and the record
files). `ChannelSummary.channel_description` is an identity value: only with
`include_values`, None when section 2 is locked. inspect's `SummaryWarning`
messages and `problems` still contain paths (they are data, documented as
identifiable). Tests: tests/test_p30_warning_paths.py, p26, p27.

NAMING (#72 first part, `core/src/naming.cpp`, `mef3io.naming`):
`time_range` reads ONLY the first and last segment's .tmet of each channel
(load_time_series_metadata_partial, so no password works and reports
offset_known=false; a wrong one throws), rchar-pinned in test_p35;
offset_known=false also for rto != 0 with non-negated stored times.
`name_drift` reads every file's 1 KB UH (session_name) + root *.rdat names;
read-only. `rename_archive` uses `detail::rename_no_replace`
(core/src/no_replace.hpp: renameat2 RENAME_NOREPLACE / renamex_np RENAME_EXCL
/ MoveFileExW without replace) and REFUSES where unsupported, never falling
back; name <= 246 bytes and a PORTABLE file name (`check_portable_name`: no
control char — a NUL once dropped the .mefd.tar suffix —, none of
`/\:*?"<>|`, no Windows device name, no trailing dot/space); refused while
`<archive>.tmp` (archive_session's temp) or `.partial` is beside it.
time_range takes min/max over BOTH files read (segments numbered against
time are still handled; a middle segment earlier than both is not seen).
name_drift's `consistent` includes the session .ridx name.
RENAME (#72 part 2, `core/src/rename.cpp`; rules in for_agents/INVARIANTS.md
§15): `rename_session` / `fix_name_drift` change the folder, the root
`<name>.rdat/.ridx`, every UH session_name and s2 session_description where it
EQUALS the old name; provenance op 7 `rename`. Journal (backup_dir, default
`<session>.repair-backup`, found again via every `*.repair-backup` beside the
session because the folder may have moved) -> marker `.mef3io-operation` ->
changes -> marker removed -> journal removed. The next call rolls an
interrupted one back, then does its own work. Marker without journal refuses
every write (`detail::refuse_if_mid_operation`; SessionWriter also checks on
each write call and refuses if its folder vanished, so an open Writer cannot
recreate a renamed session at the old path). Reader warns
`SessionOperationWarning`. `rename_session` refuses a drifted session
(fix_name_drift first) and same name; `fix_name_drift` never renames the
folder. Undo = `RenameReport.undo()` (logical, `descriptions=` restricts to
what changed). `durability.hpp` now also holds `finish_stream`,
`write_all_atomic` (were duplicated in writer/validate; `flush=false` for
batching) and `flush_files` (ONE syncfs per file system on Linux, else per
file + folder). PERFORMANCE RULE: never flush per header — backups go into
ONE `<id>.backup` blob (sizes in the journal) with one fsync, patches are
written unflushed and flushed in one batch before the names move; per-file
fsync made a 600-segment rename 35.7 s, batched 0.8 s. ROLLBACK IS
FIELD-WISE: a file is restored byte-exact only when it differs from its
backup in nothing but CRCs/name/provenance/description; otherwise only the
name and description bytes go back (s2 is AES-ECB and the description is
block-aligned, so its ciphertext is copied without a password) — a byte
restore would erase an append made meanwhile. A marker whose journal
describes ANOTHER folder (a copy made mid-operation) is refused, never
rolled back. Folder names are taken from the DISK entry (case-insensitive
volumes) and converted as UTF-8 (`u8`/`str` helpers; path::string() is ANSI
on Windows — repo-wide gap elsewhere). Known gap: a Writer appending DURING
a rename can race it (needs a cross-process write lock, filed as an issue). Not done: NFD length
check on HFS+ (the file system refuses at the folder step, which rolls back),
per-segment end time/entry counts in the report (`files` has size+mtime).
COVERAGE runs ONLY on a release (user decision 2026-10-19, never per-push):
`scripts/coverage.sh` (atomic gcov counters: the decoder's threads otherwise
race into negative hits) → gcovr for core/src + core/include (both suites
exercise it) and coverage.py for python/mef3io → coverage/ HTML + shields
endpoint JSONs; release.yml's `coverage` job attaches them to the GitHub
release (never blocking it) and re-runs docs.yml, which copies the latest
report (and the badge JSONs) into the site; README badges read
bnelair.github.io/mef3io/coverage/*.json (no redirect, unlike release
downloads). The script restores the dev extension link on exit.
VERIFY BEFORE PUBLISHING: `scripts/verify_local.sh` (`--full-bench`,
`--no-bench`) is the single entry point — builds, asserts the IN-TREE extension
is the one under test (not an installed wheel), runs the C++ + Python suites,
then the two gates that catch what round trips cannot, and finally the
benchmark. It EXITS NON-ZERO if the oracle is missing rather than reporting a
pass it cannot justify. The two gates: `tests/test_p15_oracle_acceptance.py`
(bidirectional vs mef_tools/pymef/meflib, including the MIXED sequences where
one stack modifies the other's session) and `tests/test_p16_header_parity.py` —
a KNOWN-DIVERGENCE LEDGER that writes the same signal with both stacks and
compares EVERY modelled header field; each difference must be zero or listed in
`KNOWN_DIVERGENCES` with a written reason, anything else fails. That ledger is
the test that would have caught `maximum_difference_bytes = 0`: it was invisible
to every round trip (mef3io and pymef both read the files perfectly) because
nothing compared the DECLARATIONS against the oracle's. It also documents four
fields where mef3io is RIGHT and the legacy stack is wrong (the filter settings:
-1.0 IS meflib's NO_ENTRY, meflib.h:431-437, while mef_tools writes made-up
values).

`scripts/make_reader_compat_check.py` regenerates the CyberPSG/meflib check set
(9 sessions + README into `~/mef3io_reader_compat_check`): fresh write, append,
legacy before/after repair, encrypted, padded-`.tmet`-then-appended,
recovered-after-interrupted-write, plus TWO DELIBERATELY BROKEN CONTROLS —
05 (the 1.1.2 zeros, mechanism (a)) and 03 (the legacy stack's own
`number_of_discontinuities = 0`, mechanism (b), never yet reproduced). A
matrix where everything passes proves nothing, which is why the controls are
there. All parameters are constants at the top of the script and are written
into the README; derived figures are read off the generated files, never
hard-coded. Every session is verified through mef3io AND pymef before the
script exits.

Benchmarks: `benchmarks/mef_benchmark.py` (write/open/seq/parallel vs mef_tools
& NWB-Zarr) and `benchmarks/compression_test.py` (file size / compression).

Public text: `tests/test_p25_public_text.py` (and the docs workflow's check of
the built site) fails if README, docs, examples, the package, bindings, MATLAB
or core sources name the private third-party reader; CLAUDE.md, tests and
scripts are exempt. Keep it out of for_agents/ too (public, not scanned).
Institution mentions allowed: the affiliation line ("Maintained by Filip
Mivalt, Bioelectronics Neurophysiology and Engineering Laboratory (BNEL) at
Mayo Clinic.") in README, docs/index.md and CONTRIBUTORS.md, and the
CITATION.cff author `affiliation` field (maintainer-approved). Nothing else.
