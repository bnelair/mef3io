# Section-2 maximum_* fields (allocation contract)

> Read before touching section-2 maximum_* declarations, the sizing/times validator checks or the append's maximum recomputation.
> Split out of CLAUDE.md verbatim; "the private reader" = the private third-party reader named in CLAUDE.md.

- **Section-2 `maximum_*` fields are an ALLOCATION CONTRACT, not statistics.**
  `0` is NOT the NO_ENTRY sentinel for any of them (`maximum_difference_bytes`
  / `maximum_block_samples` → `0xFFFFFFFF`; the si8 ones → `-1`), so a reader
  cannot tell unset from measured. CAREFUL WITH THE MECHANISM — and do NOT
  conclude "no reader consumes this" from `reference_files/`. THAT IS ONE
  MEFLIB BUILD. In the copy vendored here `RED_allocate_processing_struct` has
  no call site at all (only the prototype meflib.h:1184 and the definition
  meflib.c:6453) and the fields are merely initialised, rolled up and printed —
  but the meflib build behind the private reader DOES allocate from section-2 sizes, and
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
  sessions opened in the private reader, and ONLY 05 CRASHED. 05 is the deliberate
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
  one build tolerating it proves that build does not take the path (the private reader may
  simply never call `find_discontinuity_indices`), not that no build does. So
  KEEP `times.discontinuities` at Error severity; the reasoning for it is
  unchanged, it is simply still unreproduced. What this DOES settle is that (a)
  is the mechanism behind the reported crash, and that mef3io's output, its
  repairs and its recovery are all acceptable to that reader.
  THE FIX IS CONFIRMED AGAINST THAT READER FAMILY (2026-09-17): a session
  written by this version, and a legacy `mef_tools` session brought up to date
  by `repair_session`, were both opened in the private reader and DECODED — traces drawn,
  no access violation. That is the half that matters: the original failure let
  `ReadSession` succeed and blew up later inside `RED_decode`, so "it opens" is
  not evidence. The gap also landed in the right place and with the right
  duration (3.0 s at 47-53% of a 49.875 s record), so block placement by
  timestamp agrees there too — a third independent reader backing the
  reconciliation in reader-placement-and-factors.md ("Off-grid block times"). Note this covers the repaired file, in which
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
