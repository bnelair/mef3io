# Validating and repairing a session

A MEF 3.0 session declares, in metadata section 2 and the universal headers, a
number of quantities that are really re-derivable from the data: how many
blocks there are, how many samples, how large a buffer a reader must allocate.
Readers trust those declarations. meflib itself does not allocate from them,
but it exposes helpers that take one as a buffer size and validate none of
them, and applications built on it (CyberPSG among them) pass them in before
decoding anything — so a wrong declaration is a defect in the file even when
every sample on disk is intact. The mechanics are spelled out
[below](#notes-on-individual-checks).

`mef3io.Validator` checks those declarations against the data, and repairs the
ones the data can prove.

```python
from mef3io import Validator

v = Validator("subject.mefd")
report = v.validate()
print(report.summary())
```

```
mef3io validation report — subject.mefd
=======================================
254 segment(s) checked, 15 check(s) run, 254 error(s), 508 warning(s)

sizing.difference-bytes  [error]  254 segment(s)
    Difference buffer size is declared
    ch1 seg0: maximum_difference_bytes = 0  ->  38372
    ch2 seg0: maximum_difference_bytes = 0  ->  38400
    ch3 seg0: maximum_difference_bytes = 0  ->  38366
    ... and 251 more

Still repairable. To fix, pass the ids explicitly:
    mef3io.repair_session(path, ['sizing.difference-bytes', ...])
```

## Three rules

**A validator only validates.** `Validator` cannot write a byte — it has no
repair method at all. Writing lives in `mef3io.repair_session()`, a separate
function with a name that says what it does, so a session cannot be modified
by someone who opened it to look.

**Nothing is repaired implicitly.** `repair_session()` takes an explicit list
of check ids and touches nothing else — there is deliberately no "fix
everything" shortcut, and an empty selection raises `ValueError`.

**Every call returns the full report.** A repair still runs every check, so you
see the whole picture and not only the part you chose to fix. `Finding.repaired`
marks what was **actually written** — a repair that declines to change anything
(see [`sizing.contiguous`](#the-checks)) is reported as still outstanding, and
does not count towards `segments_repaired`.

```python
report = v.check("sizing.difference-bytes")        # one check, read-only

mef3io.repair_session(path, ["sizing.difference-bytes"])
mef3io.repair_session(path, ["sizing.contiguous", "times.block-interval"])
mef3io.repair_session(path, v.validate().repairable_check_ids)  # all of them
```

From the command line — `validate` reads, `repair` writes, and neither can be
reached from the other:

```bash
python -m mef3io validate SESSION.mefd
python -m mef3io validate SESSION.mefd --check sizing.difference-bytes
python -m mef3io repair   SESSION.mefd --check sizing.difference-bytes
python -m mef3io validate --list-checks
```

`repair` requires at least one `--check`; run without one it exits 2 and writes
nothing.

The exit status follows `Report.ok`: `0` when no **error**-severity finding is
left outstanding and no segment was skipped, `1` otherwise. Warnings alone do
not fail the command — they describe files that are wasteful or misleading but
that every reader still handles — and a finding repaired in the same invocation
no longer counts against it. A skipped segment always does: one that could not
be checked is not one that passed.

## What gets written

Only declarations: metadata section 2 and the universal headers of `.tmet`,
`.tidx` and `.tdat`. **Sample data and the block index are never touched.**
Section 2 of an encrypted session is decrypted, edited and re-encrypted with
the same key, so the session stays exactly as protected as it was.

Each rewritten file is first copied to `<session>.repair-backup/`, outside the
session tree so no reader mistakes a backup for data. For a `.tdat` only the
1024-byte universal header is copied — that is all a repair can touch. Copies
are staged as `.part` and renamed, so an interrupted copy is never mistaken for
a pristine backup, and a completed backup is never overwritten. Pass
`backup=False` (or `--no-backup`) to skip it.

A segment whose CRCs do not verify is **reported and left alone** — if the
bytes cannot be trusted, neither can anything derived from them. Tar archives
can be validated but not repaired; extract one first with
[`extract_session`](python.md).

## The warning you get on open

Opening a session that leaves size declarations unset emits a
`SessionDeclarationWarning`:

```
SessionDeclarationWarning: subject.mefd: metadata section 2 leaves size
declarations unset across 254 channel(s): maximum_difference_bytes (254
segment(s)). This does NOT affect reading with mef3io — every buffer is sized
from the block headers themselves, and the data is intact. It does affect
meflib-based readers (e.g. CyberPSG), which allocate from these fields. Run
mef3io.Validator(path).validate() for detail, or python -m mef3io repair
<path> --check <check-id> to fix the file.
```

It costs nothing: every segment's metadata is already parsed at open, so the
scan is pure arithmetic on values in memory — no extra file access, no block
index walk. For the same reason it only sees what is *missing* (a `0` or
`NO_ENTRY` where a size belongs), never what is merely wrong; `validate()` is
the thorough version.

The warning fires once per session open, travels through the warm-start cache
so a cached open still reports it, and is silenced the usual ways:

```python
mef3io.Reader(path, warn_declarations=False)                       # this open
warnings.filterwarnings("ignore", category=mef3io.SessionDeclarationWarning)
```

Sessions written by the legacy stack trip it (`maximum_contiguous_samples` is
never assigned), as do sessions written by mef3io ≤ 1.1.2. Sessions written by
mef3io ≥ 1.1.3 do not. `Reader.declaration_issues` returns the same list
structured, without the warning.

## The checks

Checks run in registry order: integrity first, then structure, then the
declarations a reader allocates from, then the time fields. Ids are stable API.

| Check | Severity | Repairable | What it compares |
|---|---|---|---|
| `crc.metadata` | error | no | The `.tmet` header and body CRCs |
| `crc.index` | error | no | The `.tidx` header and body CRCs |
| `index.block-offsets` | error | no | Every block lies inside `.tdat`, in increasing order |
| `index.block-count` | error | yes | `number_of_blocks` vs the `.tidx` entry count |
| `index.sample-count` | error | yes | `number_of_samples` vs the sum over the index |
| `index.start-sample` | warning | no | `start_sample` vs the first index entry (writers disagree; report only) |
| `sizing.block-maxima` | error | yes | `maximum_block_bytes` / `maximum_block_samples` |
| `sizing.difference-bytes` | error | yes | `maximum_difference_bytes` vs the RED block headers |
| `sizing.contiguous` | warning | yes | The `maximum_contiguous_*` trio vs the longest run (raised only, never lowered) |
| `times.segment-bounds` | warning | yes | Universal-header start/end vs the blocks |
| `times.recording-duration` | warning | yes | `recording_duration` vs the segment's span |
| `times.block-interval` | warning | yes | `block_interval` vs the block geometry |
| `times.discontinuities` | error | yes | `number_of_discontinuities` vs the index flags |
| `header.entry-count` | warning | yes | `number_of_entries` in each file |
| `header.max-entry-size` | info | yes | `maximum_entry_size` in each file (`.tdat` value informational) |

`Validator.available_checks()` returns the same table at runtime, with each
check's full description; `--list-checks` prints it.

### Notes on individual checks

**How these fields actually reach a reader.** meflib does not allocate from
them internally; it exposes helpers that take a section-2 declaration as a
buffer size and validate none of them, and it is the *application* built on
meflib that passes one in. Two such helpers matter here:

- `RED_allocate_processing_struct` takes `difference_buffer_size` from its
  caller. For a size of `0` it skips the allocation entirely, leaving
  `difference_buffer` NULL for `RED_decode` to write through. meflib ships a
  guard for exactly this, `RED_check_RPS_allocation`, but **never calls it**
  (declared `meflib.h:1186`, defined `meflib.c:6534`, zero call sites), so
  there is no error path at all — just the NULL write. This is the route the
  reported CyberPSG crash took.
- `find_discontinuity_indices` (`meflib.c:3548`) `malloc`s exactly
  `number_of_discontinuities` entries and then writes one per *flagged block*,
  looping to `number_of_blocks`. A declared `0` with the flags present — which
  is what the legacy `mef_tools` writer produces — is a straight heap overflow.

The legacy `pymef` reader passes neither: it sizes from
`RED_MAX_DIFFERENCE_BYTES(maximum_block_samples)`. That is why the oracle never
saw any of this, and it is also why `times.discontinuities` is rated an error
here despite looking cosmetic.

The two hazards are evidenced differently, and the difference is worth keeping
straight. `sizing.difference-bytes` has been **reproduced**: against a
meflib-based reader, a declared `0` raised an access violation inside RED
decoding, and patching that one field *in memory only* — same file, same
channel, same blocks — produced decoded output byte-identical to the on-disk
repair, which pins the cause to this field and nothing else about the file.
`times.discontinuities` rests on reading the C source above; the overflow is
plain in `meflib.c`, but no crash has been reproduced against it. Both are
errors; only one has a post-mortem.

mef3io **≤ 1.1.2** left `maximum_difference_bytes` at `0`. See
[the format reference](mef3_format.md#the-buffer-sizing-declarations).

It is also the one declaration not derivable from the block index — it lives in
the RED block headers inside `.tdat` — so the exact answer costs one small read
per block. On a very large session pass `exact_difference_bytes=False` (or
`--fast`): the check then only reports a clearly-unset value and repairs it to
meflib's worst case, `maximum_block_samples × 5`, without touching `.tdat` at
all. That over-declares by a few bytes, which is the safe direction.

**Times are compared as absolute uUTC.** A stored time may be negated (meflib's
"recording-time offset applied" marker) or not, and both mean the same instant;
writers mix the two within a segment. Comparisons allow one sample period of
slack, because per-block microsecond rounding moves the end slightly.

**Some checks fire on perfectly readable legacy files.** The legacy
`mef_tools` wrapper zeroes `block_interval` and `number_of_discontinuities`
(pymef's own default is `-1`); pymef stores `recording_duration` as
`number_of_samples / fs`, which omits gaps where meflib's rollup computes the
span; `maximum_contiguous_block_bytes` gets the whole `.tdat` body and
`maximum_contiguous_samples` is never assigned; and `.tdat`'s
`maximum_entry_size` gets a sample count. Of these only
`number_of_discontinuities` is reader-fatal (see above). See the
[legacy comparison](legacy_comparison.md#section-2-buffer-sizing-mef3io-is-stricter-than-pymef).

## Adding a check

The registry lives in `core/src/validate.cpp` and is the only thing to touch: a
check is one entry with an id, a severity, a `detect` lambda and — when the
truth can be written back — a `repair` lambda. Everything else (ordering,
filtering, reporting, the bindings, the CLI) picks it up automatically. The
Python test `tests/test_p13_validate.py` asserts that every repairable check
has a corruption case, so a new one cannot ship untested.
