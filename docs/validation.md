# Validating and repairing a session

A MEF 3.0 session declares, in metadata section 2 and the universal headers, a
number of quantities that are really re-derivable from the data: how many
blocks there are, how many samples, how large a buffer a reader must allocate.
Readers trust those declarations — meflib-based ones (CyberPSG and most
established MEF tooling) allocate from them *before* decoding anything — so a
wrong declaration is a defect in the file even when every sample on disk is
intact.

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

Repairable — nothing was changed. To fix, pass the ids explicitly:
    Validator(path).repair(['sizing.difference-bytes', ...])
```

## Two rules

**Nothing is repaired implicitly.** `validate()` only reads. `repair()` takes
an explicit list of check ids and touches nothing else — there is deliberately
no "fix everything" shortcut, and an empty selection raises `ValueError`.

**Every call returns the full report.** A repair still runs every check, so you
see the whole picture and not only the part you chose to fix.
`Finding.repaired` marks what was actually written.

```python
report = v.check("sizing.difference-bytes")        # one check, read-only
report = v.fix("sizing.difference-bytes")          # one repair
report = v.repair(["sizing.contiguous", "times.block-interval"])
report = v.repair(v.validate().repairable_check_ids)   # opt in to all of them
```

From the command line:

```bash
python -m mef3io validate SESSION.mefd
python -m mef3io validate SESSION.mefd --check sizing.difference-bytes
python -m mef3io validate SESSION.mefd --repair sizing.difference-bytes
python -m mef3io validate --list-checks
```

The exit status is `0` when nothing is left outstanding and `1` otherwise, so
the report form drops straight into a pipeline.

## What gets written

Only declarations: metadata section 2 and the universal headers of `.tmet`,
`.tidx` and `.tdat`. **Sample data and the block index are never touched.**
Section 2 of an encrypted session is decrypted, edited and re-encrypted with
the same key, so the session stays exactly as protected as it was.

Each rewritten file is first copied to `<session>.repair-backup/`, outside the
session tree so no reader mistakes a backup for data. A pristine backup is
never overwritten by a later repair. Pass `backup=False` (or `--no-backup`) to
skip it.

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
mef3io.Validator(path).validate() for detail, or python -m mef3io validate
<path> --repair <check-id> to fix the file.
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

Sessions written by the legacy pymef stack trip it (they leave
`maximum_contiguous_samples` at `0`), as do sessions written by mef3io ≤ 1.1.2.
Sessions written by mef3io ≥ 1.1.3 do not.

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
| `index.start-sample` | warning | yes | `start_sample` vs the first index entry |
| `sizing.block-maxima` | error | yes | `maximum_block_bytes` / `maximum_block_samples` |
| `sizing.difference-bytes` | error | yes | `maximum_difference_bytes` vs the RED block headers |
| `sizing.contiguous` | warning | yes | The `maximum_contiguous_*` trio vs the longest run |
| `times.segment-bounds` | warning | yes | Universal-header start/end vs the blocks |
| `times.recording-duration` | warning | yes | `recording_duration` vs the segment's span |
| `times.block-interval` | warning | yes | `block_interval` vs the block geometry |
| `times.discontinuities` | warning | yes | `number_of_discontinuities` vs the index flags |
| `header.entry-count` | warning | yes | `number_of_entries` in each file |
| `header.max-entry-size` | warning | yes | `maximum_entry_size` in each file |

`Validator.available_checks()` returns the same table at runtime, with each
check's full description; `--list-checks` prints it.

### Notes on individual checks

**`sizing.difference-bytes` is the one that crashes readers.** meflib's
`RED_allocate_processing_struct` skips the allocation entirely for a size of
`0`, leaving `difference_buffer` NULL for `RED_decode` to write through — and
`0` is not this field's NO_ENTRY sentinel (`0xFFFFFFFF`), so a reader cannot
tell it was never set. mef3io **≤ 1.1.2** left it at `0`. See
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

**Some checks fire on perfectly readable legacy files.** The pymef writer
leaves `block_interval` and `number_of_discontinuities` at `0`, stores
`recording_duration` as `number_of_samples / fs` (which omits gaps, where
meflib defines it as the span), computes the contiguous maxima as if the
segment had no discontinuities, and writes a *sample count* into `.tdat`'s
`maximum_entry_size` where the field is a byte size. None of those are
reader-fatal; all are repairable. See the
[legacy comparison](legacy_comparison.md#section-2-buffer-sizing--mef3io-is-stricter-than-pymef).

## Adding a check

The registry lives in `core/src/validate.cpp` and is the only thing to touch: a
check is one entry with an id, a severity, a `detect` lambda and — when the
truth can be written back — a `repair` lambda. Everything else (ordering,
filtering, reporting, the bindings, the CLI) picks it up automatically. The
Python test `tests/test_p13_validate.py` asserts that every repairable check
has a corruption case, so a new one cannot ship untested.
