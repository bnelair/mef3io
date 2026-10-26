# Tar archives, naming and rename

> Read before touching tar/source, session naming rules, naming.cpp, rename.cpp or archive/extract.
> Split out of CLAUDE.md verbatim; "the private reader" = the private third-party reader named in CLAUDE.md.

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
