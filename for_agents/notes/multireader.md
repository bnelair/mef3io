# MultiReader

> Read before touching python/mef3io/_multi.py.
> Split out of CLAUDE.md (verbatim except retargeted cross-references); "the private reader" = the private third-party reader named in CLAUDE.md.

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
