# Reader: placement, factors, envelope, isolation

> Read before touching reader.cpp (grid_slot/grid_count/claim_range), block_envelope, read_many, per-segment conversion factors or strict=False segment isolation.
> Split out of CLAUDE.md verbatim; "the private reader" = the private third-party reader named in CLAUDE.md.

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
