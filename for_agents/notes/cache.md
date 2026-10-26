# Warm-start snapshot cache

> Read before touching python/mef3io/cache.py or Reader(cache=), or anything that affects an open's file/stat count.
> Split out of CLAUDE.md verbatim; "the private reader" = the private third-party reader named in CLAUDE.md.

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
(provenance rule: provenance.md).
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
