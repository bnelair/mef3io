# mef3io — invariants that must not be broken

Read this before changing the writer, the append path, the validator or the
read path. Everything here was learned by shipping the mistake first. Each entry
says what the rule is, what went wrong without it, and what pins it now.

`CLAUDE.md` is the working context; this is the short list of things that have
already cost a release cycle.

---

## 1. Never read or copy a `.tdat` whole — ~30 GB is ONE CHANNEL

A session runs to hundreds of gigabytes or terabytes. Reading one file whole
makes the library unusable on exactly the data it exists for, and this has been
reintroduced more than once.

- **Reads take the extent the request needs.** Select the index entries first,
  compute min/max byte offset, issue **one** `source_->read_range`, and check
  that extent against `file_size` so a damaged index fails loudly rather than
  reading wild. `collect_blocks` always did this; `read_runs` did **not** until
  2026-09-21 and pulled the entire file in to serve a one-minute window
  (83.5 MB read for 0.47 MB of data).
- **Validation never reads the body.** `SegmentState` holds the 1024-byte
  universal header and the file *size*, nothing more.
- **Recovery streams.** It needs the size, plus a 304-byte RED header and the
  single block that header describes, at a handful of offsets.
- **Back up what CHANGES, not the file.** Repair saves the `.tdat`'s 1024-byte
  header; recovery saves that header plus the sub-block fragment it drops.
  Copying 30 GB to undo 200 bytes is not a backup, it is an outage.
- **Tar archive/extract stream in 1 MB chunks.** Keep it that way.

Measure with `/proc/self/io` `rchar` — it counts bytes read exactly and does not
flake the way RSS does. Pinned by `tests/test_p17_large_files.py`.

### The audit, and what it found

Done properly once (2026-09-21) across the whole library. Clean: offsets are
`si8`/`size_t` everywhere with no narrowing; `tar` archive and extract stream in
1 MB chunks; the warm-start cache (format 6) reads only the first and last
segment's 1024-byte `.tmet` header and a few file sizes per channel, and never
touches `.tdat`; validation keeps only the
1024-byte universal header plus the file size; the repair path backs up the
`.tdat` header rather than the file.

Two things were **not** clean and are now fixed: `read_runs` read the whole
`.tdat` for any window, and `Session` cached every segment's block index
forever. The index is **~2 % of the data**, so a session of a few hundred
gigabytes held several GB of it resident. It is now capped
(`Session::set_index_cache_bytes`, default 256 MB) with least-recently-used
eviction, done at the END of a public call. `segment_index` hands out a
`shared_ptr` pin, so evicting an index that another call (or another THREAD's
call on the same Session) is still reading never frees bytes in use; a raw
span here crashed four threads sharing one Session. The cache is guarded by
`index_mu_`, other mutable state by `state_mu_`; neither is held across I/O,
and they are never held together. Pinned by `tests/test_p17_large_files.py`
(the bound, and correct reads across eviction), `core/tests/test_concurrency.cpp`
and `tests/test_p31_gil_release.py`.

## 2. An append must be O(new data), never O(total blocks)

Section 2 describes **all** of a segment's blocks, so the obvious
implementation re-reads, re-CRCs, re-walks and rewrites the whole `.tidx` every
append. That is quadratic over a recording that runs for months.

`AppendIndexCache` carries the index summary across appends; entries are
appended in place and the body CRC is **extended** over just the new bytes (the
MEF Koopman-32 CRC is a rolling state with no final inversion, so
`crc(A‖B) == crc(B, crc(A))`).

The fast and slow paths **must** produce identical bytes —
`test_append_index_cache_matches_the_full_walk` writes the same data both ways
and compares entries, block data and the body CRC.

## 3. Section-2 `maximum_*` fields are an ALLOCATION CONTRACT, not statistics

A meflib-based reader allocates buffers from them **before** decoding anything.
`0` is not the NO_ENTRY sentinel for any of them, so a reader cannot tell unset
from measured.

- Over-declaring costs memory. **Under-declaring truncates the buffer a reader
  decodes into.** Never move one of these fields downwards on a write path.
- Do **not** conclude "no reader consumes this" from `reference_files/`. That is
  one meflib build; the deployed one allocates from these sizes, and that is
  where the original crash was reproduced.
- The bound helper saturates strictly **below** `UI4_NO_ENTRY`. Note
  `UI4_NO_ENTRY / 5 * 5` is `0xFFFFFFFF` exactly, so the obvious saturation
  still returns the sentinel — that exact mistake was made and caught by test.

## 4. Durability is scoped deliberately, and meflib has none

meflib does **no** durability work: its single `fflush` is "to update stat
structure after write", not a barrier. mef3io's `durability="full"` is
stronger.

Load-bearing on append with `durability="full"`, do not remove for speed:
- the `.tdat` is flushed **before** the `.tidx` that references those bytes;
- the appended `.tidx` is flushed before its header is patched, and the
  `.tmet` temp file before it is renamed over the original.

Deliberately skipped: a fresh write (nothing underneath to lose) and the
directory entry after a rename (losing the rename keeps the previous complete
file). The DEFAULT is `durability="fast"`, which drops the barriers. That is
NOT atomic: `.tdat` and `.tidx` are extended in place, so a crash can leave a
torn tail, or an index ahead of or behind the data; only `.tmet` is published
by rename. Every such state is detectable and fixed by `recover_session`,
which is what makes the default defensible.

## 5. Repair rewrites declarations; recovery may touch the index

Keep these apart. `repair_session` only ever rewrites section 2 and the
universal headers — never samples, never the index — which is what makes it safe
to run on anything. `recover_session` may truncate the index, rebuild entries
from `.tdat` block headers, and drop a sub-block fragment; it is dry-run by
default and backed up before it writes. Its one declaration write: at a
non-zero recording-time offset, where repair's time checks stand down, it sets
the segment's end time (all three universal headers) from the last block it
kept, and only when the stored times are in the negated form (#83).

## 6. Round trips cannot prove compatibility

`maximum_difference_bytes = 0` survived every test: mef3io read its own files
perfectly, pymef read them perfectly, round trips were bit-exact. Nothing
compared the **declarations** against the oracle's.

Two gates now do:
- `tests/test_p15_oracle_acceptance.py` — bidirectional against
  `mef_tools` → `pymef` → `meflib`, including the mixed sequences where one
  stack modifies the other's session.
- `tests/test_p16_header_parity.py` — the **known-divergence ledger**. Every
  header field must be equal to the oracle's or listed with a written reason.
  Adding an entry should take an argument; that is the point.

## 7. Run the verification script before publishing

```bash
scripts/verify_local.sh              # suites, oracle parity, quick benchmark
scripts/verify_local.sh --full-bench
```

It builds, asserts the **in-tree** extension is under test rather than an
installed wheel, runs both suites and all the gates, and **exits non-zero if the
oracle is not installed** rather than reporting a pass it cannot justify.

## 8. Platform-specific syscalls must be guarded — and CI is the only proof

`fdatasync` does not exist on macOS, and swapping `fsync` for it broke every
macOS build for four commits before anyone looked. Worse, the obvious repair —
plain `fsync` — would have *silently weakened* the guarantee the docs promise,
because on macOS `fsync` does not force the drive's own write cache;
`fcntl(fd, F_FULLFSYNC)` does, with a fallback to `fsync` when a filesystem
returns ENOTSUP.

Two rules. Guard anything POSIX-adjacent per platform rather than assuming
Linux. And **do not push a syscall change and walk away** — the matrix
(ubuntu/macos/windows) is the only thing that can tell you, and it takes
minutes.

## 9. Reproduce before believing

Every finding in the two review rounds was reproduced before being acted on, and
several confident-sounding ones dissolved under it. Agent output is a lead, not
a verdict. When a fix lands, reproduce the original failure against the fix —
the sentinel-saturation fix in §3 looked right and was wrong.

## 10. A warning's text never carries a path

Every Python warning is a `MefWarning` raised through `_warnings.emit`, which
puts the session in `w.message.path`, the structured items in `.details`, and
scrubs the session's location and name out of the text (including a renamed
archive's inner folder name and the bare name after a space). Only those:
channel and file names inside the session still appear, and can be
identifying — the docs must not promise more. Until #96 every
message started with the full path, so a caller that must keep paths out of
its logs (mef3io-studio) had to swallow every mef3io warning — including the
ones that say data was skipped. A bare `warnings.warn` reintroduces that, and
`tests/test_p30_warning_paths.py` fails on one (it bans importing `warnings`
anywhere else in the package). `emit` also keeps one warning registry per
(session, calling file), so the default filter still shows a warning once
per session rather than once per process.

## 11. A de-identified copy is checked by something that does not trust the copier

`anonymize_session` (#70) publishes nothing `verify` has not passed, and
`verify` must not share logic with the rewrite it checks. The first review
round found six leaks that survived with `verify().ok == True` — text after a
field's NUL, foreign per-block regions, segment folder names, names glued to
other words — each because the check asked the same question the rewrite had
answered. Rules:

- The name scan is a dumb case-insensitive substring search; refusing a copy
  over a false alarm is acceptable, publishing a leak is not.
- The scan and the rewrite share ONE thing only: the folded space they match
  in (`scan_folded`: case and Latin accents). Otherwise the rewrite leaves
  "Muller" that the scan finds for "Müller" and the copy is refused for
  nothing (#115). Their matching rules stay separate. What the rewrite still
  cannot remove is refused before writing (`text-unremovable`), never
  discovered after a long copy.
- Rewrite every text field whole, and zero every region another writer may
  fill; never "leave it if unchanged".
- Every new field the copy carries needs a planted-leak test in
  `tests/test_p29_anonymize.py` that shows `verify` catching it in the copy
  AND the rewrite removing it from a source.
- ONE pre-flight (`preflight()` in `core/src/session_copy.cpp`) decides every
  refusal for both `anonymize_session` (throws the first, before any file or
  progress) and `check` (collects all). Never add an up-front check anywhere
  else; `tests/test_p32_anonymize_check.py` runs every refusal through both.
- Refusal codes are stable API, kept equal in three places by test_p32:
  `REFUSAL_CODES` (`python/mef3io/anonymize.py`), the table in
  `docs/anonymization.md`, and the test cases.
- No message, warning or `VerifyIssue` carries a source time or the
  recording-time offset. `AnonymizeReport.source_*` times are the
  re-identification key: in memory only, left out of `repr`, `summary()`,
  `to_dict()` (unless asked) and pickles.

## 12. Calls on one Reader can overlap

The bindings release the GIL on open and on every long call, so several
threads may call one `Reader`/`Session` at once: reads, toc, segments and
records. NOT yet safe in Python (#110): `Reader._ensure_impl` with `cache=` is
an unlocked check-then-set (5 warm Readers x 8 threads built 39 backends);
`close()` racing a first read can reopen the session; `records()` warning
dedup diffs the cumulative problems list, so concurrent calls can double-warn;
the C++ setters (`set_threads`, `set_index_cache_bytes`) are unsynchronised.
The docs say exactly this — do not widen the claim before #110 lands. A
Writer is not shared-safe. A new long call must release
the GIL too (and get a `tests/test_p31_gil_release.py` case); any state it
mutates must be locked; core globals must be `constexpr` or immutable (the AES
inverse S-box used to be filled lazily, a data race).

## 13. No private product name in public text

`tests/test_p25_public_text.py` (and the docs workflow's site check) fail if
public text names a private third-party product. CLAUDE.md, tests and scripts
are exempt; this folder is public, so keep such names out of it.

## 14. One copy engine: a transform changes only what it was asked to

`transform_session` (#73) is the same engine as `anonymize_session`
(`core/src/session_copy.cpp`: one `preflight`, one `Copier`, one `Verifier`,
one `run_copy`), steered by `Rules`. Rules:

- Never add a second copy path. Cut/merge (#103) and re-encoding (#121) are
  options of this engine, so every copy keeps the partial-directory publish,
  the pre-flight/check pair, the verification and the refusal codes.
- A transform copies every stored time, every sample, every region another
  writer fills and every record body VERBATIM. The only changes are the
  channels, passwords, name and the requested metadata edits, and its
  verification compares everything else byte for byte. A transform must
  never "normalise" a session (no rto folding, no region clearing):
  that is anonymize's job, and doing it silently would change a recording
  the user asked to copy.
- Metadata edits are written into the stored section images, never by
  re-serializing them, and a value that does not fit is refused, never cut.
- Shared refusal codes are documented once (docs/anonymization.md), and
  docs/transform.md documents only the transform's own;
  `tests/test_p33_transform.py` keeps both equal to the code tables.

## 15. An in-place operation that takes several steps is journaled and marked

`rename_session` / `fix_name_drift` (#72, `core/src/rename.cpp`) change many
files and up to three names. Order, all load-bearing:

1. Check everything first (CRCs, the password, the names, nothing being
   written); refuse with nothing changed.
2. Back up the old headers (whole `.tmet`: 16 KB) and write the JOURNAL to
   the backup folder, flushed.
3. Write the MARKER (`<session>/.mef3io-operation`, fixed name, holds only
   tool / operation / id) and flush it with its directory entry.
4. Change: headers, then the session-level record files, then the folder.
5. Remove the marker FIRST, then the journal.

Marker + journal = interrupted: the next call rolls it back (old bytes
restored, renames undone; idempotent). Journal alone = never started or
finished: discarded. Marker alone: every write refuses
(`detail::refuse_if_mid_operation`, called by SessionWriter on open AND on
every write call, repair, recover --apply, archive, and the copy pre-flight as
`source-mid-operation`); `clear_operation_marker` only when `name_drift` is
consistent. A new in-place write path must call `refuse_if_mid_operation`.
Rollback is FIELD-WISE (exact bytes only when nothing else wrote the file),
flushes are BATCHED (one backup blob, one `flush_files` pass), and a marker
whose journal describes another folder is refused (a copy). Every rename is
no-replace (`no_replace.hpp`), temp names are 32-char tokens,
and an undo is a LOGICAL inverse (rename back, only the descriptions changed),
never a byte restore. Headers are patched as bytes (name field, provenance
region, CRCs); `UniversalHeader::serialize` would cut a 256-byte name that
fills its field. Tests: `tests/test_p36_rename.py` (a crash at EVERY point via
the test-only `_crash_at`, which leaves the disk as a crash would).
