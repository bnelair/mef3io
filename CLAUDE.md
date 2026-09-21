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
validator + targeted repair implemented (+ an open-time warning). ~253 Python
tests + standalone C++ Catch2 tests. Wheel builds via `python -m build`.
Parallel decode/encode, byte-deterministic across threads.

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
abstraction: DirectorySource/TarSource), `tar` (uncompressed .mefd.tar session
archives: TarIndex/TarSource/`archive_session`), `reader` (gridding, NaN fill,
scaling, parallel decode), `records` (read+write), `writer` (segment writer),
`session_writer` (precision inference, quantization, NaN splitting, segments),
`parallel.hpp`.

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
Benchmarked on a ~2 GB session: full-read throughput identical to the dir,
windowed reads ~8% slower, open faster; archive ~0.56 GB/s. Tests:
core/tests/test_tar.cpp, tests/test_p11_tar.py, tar block in
matlab/test_mef3io.m.

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

Session metadata (subject/acquisition): `mef3io.Metadata`/`Subject`/
`Acquisition` dataclasses (`python/mef3io/metadata.py`), settable via
`Writer(metadata=)` / `set_metadata`, read via `Reader.metadata`. Threaded
through C++ `SessionMetadata` (writer.hpp) → SegmentSpec → writer.cpp s2/s3
population; `ChannelInfo` carries the fields on read. C ABI
`mef3io_writer_set_metadata` + MEX `writer_set_metadata`; MATLAB
`Writer(Metadata=struct)` / `Reader.metadata`. Subject fields are L2-gated.

`python/mef3io/`: `Reader`, `Writer` (incl. `write_annotations`), `compat`
(mef_tools.io drop-in; `MefReader`/`MefWriter` are also re-exported at the top
level — `from mef3io import MefReader, MefWriter` — via lazy `__getattr__`),
`cache` (opt-in warm start), `pure` (stub). `bindings/python/mef3io_ext.cpp` =
nanobind. `matlab/` = MEX + `+mef3io` classes. `examples/` = runnable scripts
(write/read, int32, append, segment map, annotations, encryption, legacy
style). Docs site: MkDocs Material from `docs/` (`mkdocs.yml`; nav pages
index/install/python/matlab/cpp/examples/releasing + the reference docs),
deployed to GitHub Pages by `.github/workflows/docs.yml` on push to main —
keep `PYTHONPATH=python mkdocs build --strict` green. API reference is
autogenerated: Python via mkdocstrings (NumPy-style docstrings on
`_reader.py`/`_writer.py`/`compat.py`), C++ via Doxygen (`docs/Doxyfile` →
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
- **Password scheme**: bytes = terminal byte of each UTF-8 char, 16, zero-pad.
  L1 field = SHA256(L1)[:16]; L2 field = SHA256(L2)[:16] XOR L1. L2 access
  derives putative L1 = SHA256(pwd)[:16] XOR L2field, checks its hash.
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
- **Sign conventions are MIXED WITHIN ONE FILE.** For a pymef/mef_tools
  session: `.tmet` universal-header times are NEGATED, `.tidx`/`.tdat`
  universal-header times are NOT, and index block times ARE. So every time
  comparison must go through `to_user_time` first. Worse, with a non-zero
  `rto` the legacy stack stores a POSITIVE delta where meflib negates — the two
  are indistinguishable from the bytes, so the validator's time checks stand
  down entirely when `rto != 0` rather than risk rewriting a correct file.
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
  appends reuse the segment's precision. `Reader.segments(ch)` maps what data
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
  THE REAL REMAINDER is that mef3io never cross-checks the two copies, so a
  file whose copies disagree is read without complaint (open issue #11). The
  thread-invariance tests that use the helper are still valid — they exercise
  overlapping output ranges, which is all they claim to.
- **Do NOT `pip install -e .` for C++ dev** — scikit-build-core's editable hook
  loads an install-time extension snapshot that shadows the dev_build symlink
  (meta-path beats sys.path). Keep mef3io uninstalled; use scripts/dev_build.sh.
- Pure-Python backend is a stub. Records write builds bodies for Note/SyLg
  (text) and EDFA (duration+text) only; Seiz is read-only (`parse_records`
  decodes onset/offset/duration, `record_body` has no Seiz branch, and the
  bindings expose no onset/offset fields). Any other 4-char type writes a
  header with an empty body. `write_records` rejects a record carrying a
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
  `PYPI_Token_General` secret. `ci.yml` = tests on push/PR (main, dev). PyPI
  name not yet reserved. Then: benchmark vs legacy, cut mef_tools 3.0 as a
  compat re-export. Keep mef3io brand-neutral; brainmaze-mef3-server should
  depend on it (see docs/design.md).

Benchmarks: `benchmarks/mef_benchmark.py` (write/open/seq/parallel vs mef_tools
& NWB-Zarr) and `benchmarks/compression_test.py` (file size / compression).
