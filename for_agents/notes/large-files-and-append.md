# Large files, append, durability

> Read before ANY read/write path that touches .tdat/.tidx, an append, flush/close/wait_for_disk, fsync, recovery backups or tar streaming.
> Split out of CLAUDE.md (verbatim except retargeted cross-references); "the private reader" = the private third-party reader named in CLAUDE.md.

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
  block than mef_tools, in a file 1.8x smaller, growth 1.00x — measured WITH
  1.x's full durability barriers (2.0 keeps only the .tmet one).

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
  pins it, self-calibrating against actual file growth. (2) NO DURABILITY
  OPTION since 2.0 (see the next gotcha): it is NOT an "atomic writes" story:
  `.tdat` and `.tidx` are extended IN PLACE unflushed, so a power cut can
  leave a torn tail on either, an index referencing data that never landed,
  or blocks the index never mentioned. Only `.tmet` is published by a
  flushed rename. Every one of those is detectable (`index.block-offsets`,
  `index.data-coverage`, the body CRCs, `header.entry-count`) and fixed by
  `recover_session` — the failure mode is "run recover after an unclean
  shutdown", and an EARLIER append is never at risk because nothing rewrites
  it. (3) `recover_session` (core/src/
  recover.cpp, `python -m mef3io recover`) is the ONLY thing that may touch
  the index or data — repair_session never does, which is what keeps it safe
  on anything. Index-ahead-of-data drops entries; data-ahead-of-index
  REBUILDS them from the RED block headers (CRC-checked). Dry run by
  default. CRASH STATES (review of #153, all pinned in
  tests/test_p46_crash_states.py, built by mixing old/new file images):
  an index failing its OWN CRC (header + entries from different writes) is
  NOT refused — entries kept up to the longest prefix the stored body CRC
  covers (rolling CRC), then each only if the RED header at its offset agrees
  (304 B per entry), rest rebuilt from .tdat; in each channel's LAST segment
  the last append's blocks (back to its discontinuity-flagged first block,
  <= 64 MiB) are block-CRC-checked, so entries pointing at zero pages drop.
  DECLARATIONS: with apply, every segment it changed + every last segment
  whose .tmet counts disagree with the index (index.block-count /
  sample-count / header.entry-count, checked in the scan with
  exact_difference_bytes=false; the .tmet NEWER than its data is the
  dominant post-crash state since 1.x fast's best-effort header fdatasyncs
  — which flushed the whole files — are gone) get ALL their repairable
  findings fixed by CALLING repair_session per segment (exact difference
  bytes; `RecoveredSegment.declarations`; the dry run lists the EXACT ids for
  declaration-only segments via the same `segment_repairable_ids`). NEVER
  repair a segment recovery does not report (review r2: the CLI's
  session-wide chain rewrote all 12 files of a HEALTHY mef_tools session;
  removed, with `--no-repair`; test_p46 pins a legacy session byte-identical).
  LOST .tmet of a NON-FIRST segment (`rebuild_tmet`): previous segment's .tmet
  byte for byte + this segment's .tdat UH (segment number, times, level UUID)
  + first index entry's start_sample + native extremes from the index
  min/max, new file UUID, Recovery stamp; then the declaration step. A
  channel's FIRST segment has no source, hence the writer flushes it (below).
  `block_looks_real` reads each byte ONCE (rolling CRC over the header in
  hand) and the .tdat streams are UNBUFFERED (`pubsetbuf(nullptr, 0)`; a
  buffered stream refilled after every seek) — test_p17 pins a healthy dry
  run at <= 1.05x the checked bytes + index overhead. ONE
  declaration recovery writes ITSELF (#83): at a NON-ZERO rto, where the
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
- **When data is on disk: flush()/close()/wait_for_disk; ONE default barrier**
  (`core/src/durability.hpp`, shared by writer.cpp, validate.cpp,
  recover/rename/copies — they had drifted once). History: 2.0 REMOVED the
  `durability` option everywhere (Python Writer/MefWriter -> TypeError, C++
  `set_durable`/`SegmentSpec::durable`, C ABI `mef3io_writer_set_durable`, MEX,
  MATLAB `Durability=`). Maintainer decision 2026-10-26 (#153), user-facing
  section docs/long_recordings.md "When is my data on disk?":
  (1) DEFAULT `write()` returns once the data is handed to the OS. It flushes
  only the `.tmet` before its rename: on EVERY append (write_file_atomic,
  REPLACING) and for a channel's FIRST segment in any session plus any new
  segment/channel in a session that existed when the SessionWriter opened it
  (`SegmentSpec::flush_new_metadata`, `session_preexisted_ || n_segments==0`)
  — the .tmet only, not the .tdat/.tidx. THAT BARRIER STAYS — history: a
  rename is ORDERED, not durable, so without it a power cut can publish the
  new name over bytes still in cache, and a SHORT `.tmet` throws from the
  metadata loader and kills the WHOLE session (a MISSING .tmet only hides its
  segment — verified); a first segment has no previous .tmet to rebuild from.
  (2) `flush()` (Python Writer/MefWriter, C++ SessionWriter, C ABI
  `mef3io_writer_flush`, MATLAB `w.flush()`): returns when EVERY file the
  writer created or extended since the last flush is on disk, ONE combined
  wait (`unflushed_` set -> `detail::flush_files`: one syncfs per FS on Linux,
  `flush_each_concurrently` (<=16 threads) + folder fsyncs elsewhere).
  close()/`with`/the destructor/`mef3io_writer_close`/MATLAB delete flush too
  (destructor and C close swallow a failure; Python close() raises it).
  (3) OPT-IN `wait_for_disk` (`Writer(wait_for_disk=True)`, MefWriter,
  `set_wait_for_disk`, C ABI `mef3io_writer_set_wait_for_disk`, MATLAB
  `WaitForDisk`): each write's .tdat/.tidx/.tmet temp in ONE flush_files batch
  BEFORE the .tmet rename (so the .tmet never declares data not on disk);
  records (.rdat/.ridx) likewise. A single caller's calls are sequential, so
  one call's wait CANNOT overlap the next call's work (a background flusher
  would not help: write() must not return before its own data is on disk);
  the batched alternative is default + flush() per round.
  Measured ext4, 128 ch x 10-min 256 Hz rounds: default 2.2-2.3 s, +flush()
  per round 2.5-2.6 s, wait_for_disk 3.6-3.7 s (1.x full 6.0-6.5 s, 1.x fast
  3.8 s, no flush at all 0.74 s); fresh session first write 2.4-2.5 s (0.85 s
  with no flush); reopened new segment x128 2.55 s. Rejected on the way:
  flushing all three first-segment files (5.0 s fresh), per-call
  flush_files batch of them (3.6 s), cross-call deferred .tmet publication
  (~0.9 s est., but readers would not see new channels until close: the
  maintainer objected to write() returning before data is on disk without an
  explicit way to wait — hence flush()). 1.x's default "fast" was documented
  as "no flushes" but flushed both patched headers best-effort (fdatasync per
  FILE: it made appended data/index durable before the .tmet; dropping it is
  why ".tmet newer than its data" is the common crash state — recovery
  handles it). Never flushed by default: .tdat/.tidx, later segments of a
  self-created session, the DIRECTORY entry after a rename. A write /
  flush() / close() THROWS if a flush fails (1.x fast ignored it). Recovery
  for what the default leaves: short first-segment .tidx (or zeroed .tdat
  header) REBUILT from the .tdat blocks under a header synthesized from the
  segment's .tmet (`synthesize_header`); lost later-segment .tmet rebuilt
  from the previous segment's (+ .tidx from data); dataless last segment
  removed. ROUND 3 (review of 4269ef9): (a) wait_for_disk flushes the
  PUBLISHED .tmet after the rename too (`flush_files({target})` in
  write_file_atomic: a second batch per write) — before, a power cut right
  after write() left data + a flushed `.tmet.mef3io-tmp` and no `.tmet`;
  every touched file is ALSO added to `unflushed_` in every mode so
  flush()/close() cover them. Recovery ADOPTS a complete CRC-valid
  `<seg>.tmet.mef3io-tmp` when the `.tmet` is missing/short
  (`adoptable_tmet_temp`, provenance Recovery, temp removed). (b) non-Linux
  `flush_files` flushes each file's folders up 4 levels (.segd .timd .mefd
  parent); `fsync_directory` = F_FULLFSYNC (macOS, fsync fallback) / a
  FILE_FLAG_BACKUP_SEMANTICS handle + FlushFileBuffers (Windows; NTFS often
  refuses, the refusal is ignored there and documented) and returns bool.
  (c) A failed flush is STICKY (`flush_failure_`, also set by a failed
  wait_for_disk flush inside write()): flush()/close() rethrow it forever, the
  destructor skips (a retry only looks like success). (d) Recovery: a MISSING
  .tidx = empty; a .tdat header whose type is not "tdat" on a full-size
  segment is replaced by `synthesize_header` (not re-sealed); a rebuilt index
  numbers samples from the segment's .tmet start_sample (channel-wide).
  (e) Python `Writer.__del__`/`MefWriter.__del__` flush with the GIL
  released and emit `WriterFlushWarning` on failure (`_flush_on_collect`).
  Pinned: core/tests/test_durability.cpp (`fsync_observer` per
  file, `flush_batch_observer` per batch: fresh session = 1 .tmet flush per
  channel, later self-created segment 0, reopened new segment/channel 1,
  append 1; flush() = ONE batch with every .tdat/.tidx/.tmet/.rdat/.ridx,
  destructor = one batch; wait_for_disk = TWO batches per write (the .tmet TEMP before
  publish, the published .tmet after), sticky-failure cases; flush_each_concurrently 8 x
  100 ms < 400 ms; rollback .tidx flushed), tests/test_p46_crash_states.py,
  test_p18 (API), test_p31 (flush releases the GIL). Repair, rename,
  recovery and the copies keep their OWN flush rules.
  THE FLUSH IS THREE DIFFERENT CALLS with three different rules, and the
  platform differences are load-bearing: Linux `fdatasync`, macOS
  `F_FULLFSYNC` (with an ENOTSUP fallback), Windows `FlushFileBuffers`.
  **Windows requires the handle to carry GENERIC_WRITE** — unlike POSIX, where
  fsync on an O_RDONLY descriptor is legal — and opening for read there fails
  ERROR_ACCESS_DENIED. That shipped (1.x): `durability="full"` threw on the
  first append, and the callers that IGNORE the result (then
  `durability="fast"`, still the recovery backups) silently had no durability
  at all, which is the worse half. The 2.0 `.tmet` barrier uses the same
  primitive (`fsync_file_or_throw`), so this still matters.
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
  build.** Both are write-side traps that mirror read-side gotchas in format-gotchas.md, and
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

- **Multi-channel write (#165).** `SessionWriter::write_int32_many`/
  `write_float_many` (Python `Writer.write_int32_many`/`write_many`, C ABI
  `mef3io_writer_write_int32_many`, MATLAB `writeInt32Many`; float-many is
  Python/C++ only) drive the SAME `plan_write` -> `create_plan_dirs` ->
  `run_plan` -> `commit_plan` steps as the single-channel `write_blocks` —
  never fork a second write path. Plans are made serially (every refusal;
  nothing on disk), `run_plan` runs per channel in parallel (outer channels x
  inner block threads <= n_threads: ONE thread budget, no nested pools) and
  leaves the `.tmet` as a temp (`PendingPublish`, writer.hpp), then ONE
  `flush_pending` batch, then the renames, then the serial commit of the
  writer's state. A failure before the renames rolls EVERY channel back
  (`AppendRollback` for appends, folder removal for fresh segments; the
  append's `AppendIndexCache` is invalidated); during them
  `PartialWriteError` (+ Python `written_channels`) names the published
  channels. An append conflict is found INSIDE `append_time_series_segment`
  (it reads the `.tmet`), i.e. during phase A: still all-or-nothing, by
  rollback. Memory: the call holds data + a block copy + the encoded bytes of
  the channels running at once. Tests: core/tests/test_write_many.cpp
  (byte identity vs a loop with the random UUIDs masked, one batch per call,
  rollback), tests/test_p48_write_many.py, GIL case in test_p31.
