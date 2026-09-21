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
1 MB chunks; the warm-start cache hashes only bounded prefixes (16 KB of
`.tmet`, 1 KB of `.tidx`) and never touches `.tdat`; validation keeps only the
1024-byte universal header plus the file size; the repair path backs up the
`.tdat` header rather than the file.

Two things were **not** clean and are now fixed: `read_runs` read the whole
`.tdat` for any window, and `Session` cached every segment's block index
forever. The index is **~2 % of the data**, so a session of a few hundred
gigabytes held several GB of it resident. It is now capped
(`Session::set_index_cache_bytes`, default 256 MB) with least-recently-used
eviction — done only at the END of an operation, because a caller holds a span
into those bytes while reading and evicting one mid-read is a use-after-free.
`tests/test_p17_large_files.py` checks both the bound and that reads stay
correct across eviction.

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
structure after write", not a barrier. mef3io's default is stronger.

Load-bearing on append, do not remove for speed:
- the `.tdat` is flushed **before** the `.tidx` that references those bytes;
- `.tidx`/`.tmet` replacements are flushed before the rename.

Deliberately skipped: a fresh write (nothing underneath to lose) and the
directory entry after a rename (losing the rename keeps the previous complete
file). `durability="fast"` drops the barriers for speed; writes stay atomic, so
the failure mode is a recoverable inconsistency, not a torn file — which is why
`recover_session` exists.

## 5. Repair rewrites declarations; recovery may touch the index

Keep these apart. `repair_session` only ever rewrites section 2 and the
universal headers — never samples, never the index — which is what makes it safe
to run on anything. `recover_session` may truncate the index, rebuild entries
from `.tdat` block headers, and drop a sub-block fragment; it is dry-run by
default and backed up before it writes.

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

## 8. Reproduce before believing

Every finding in the two review rounds was reproduced before being acted on, and
several confident-sounding ones dissolved under it. Agent output is a lead, not
a verdict. When a fix lands, reproduce the original failure against the fix —
the sentinel-saturation fix in §3 looked right and was wrong.
