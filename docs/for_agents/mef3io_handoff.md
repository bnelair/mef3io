# mef3io — Handoff / Repo Extraction Guide

This document is the single orientation point for moving `mef3io` into its own
repository and for re-seeding an assistant's (or a new contributor's) context
there. It intentionally duplicates knowledge that otherwise lives only in a
Claude Code session's path-keyed memory, so that knowledge travels with the
code. Drop the "Knowledge base" section below into the new repo's `CLAUDE.md`.

---

## 0. The package is already self-contained

mef3io now lives as a single self-contained directory (`mef3io/`) inside the
original `mef_tools` repo, laid out as its own project root (`CMakeLists.txt`,
`pyproject.toml`, `core/`, `bindings/`, `python/`, `tests/`, `docs/`,
`benchmarks/`, `scripts/`, `.github/`, `CLAUDE.md`, `README.md`, `LICENSE`).

Nothing was ever committed, so extraction to its own repo is a one-liner — no
file copy, no `git filter-repo`/`subtree`:

```bash
mv mef3io ~/code/mef3io && cd ~/code/mef3io && git init -b main
git add -A && git commit -m "Initial import of mef3io"
```

The legacy `mef_tools/` package (the pymef wrapper) **stays** in the old repo as
the benchmark/oracle baseline. `reference_files/` (meflib/pymef/mef3_dump) also
stays there; keep a copy next to the new repo to run golden regeneration or read
the reference C. The manifest and `export_mef3io.sh` script below are now mostly
historical (kept for reference) — the split is just moving the folder.

## 1. What moves vs. what stays

**Move to the new `mef3io` repo (the export set):**

```
core/                         # C++ library + Catch2 tests
bindings/python/              # nanobind extension
python/mef3io/                # Reader, Writer, compat, cache, pure
tests/generate_golden.py      # golden fixture generator (dev-only; needs legacy stack)
tests/test_p1_headers.py … test_p7_compat_cache.py
tests/golden/                 # COMMIT these fixtures (see §3)
docs/encryption_model.md
docs/mef3io_readme.md
docs/mef3io_handoff.md        # this file
requirements/mef3io_design.md # the agreed design (rename dir to docs/ if preferred)
scripts/dev_build.sh
CMakeLists.txt                # root (scikit-build entry)
pyproject.toml
.github/workflows/wheels.yml
```

**Stays in the old repo (legacy):**

```
mef_tools/                    # the pymef-backed package — your benchmark baseline
setup.py, INSTALL.rst, README.rst, dev.py, requirements.txt
```

**Do NOT commit to the new repo (kept locally / gitignored):**

```
reference_files/              # meflib, pymef, mef3_dump — upstream oracles.
                              # Keep locally for dev; optionally add as git
                              # submodules. They are gitignored today.
build/, build-core/, dist/, wheelhouse/, *.egg-info/
```

Use `scripts/export_mef3io.sh <target-dir>` to copy the export set mechanically.

## 2. The test/benchmark oracle (mostly solved via pip)

The P1–P7 tests and the benchmarks validate against the legacy stack:

- `pymef` (pip-installable — the primary read/write oracle),
- the legacy `mef_tools` package — **also pip-installable as `mef-tools`**
  (PyPI; import name `mef_tools`; it depends on `pymef`).

Both are declared as extras in `pyproject.toml`:
`pip install "mef3io[test]"` (adds `pymef`, `mef-tools`, `pandas`, `pytest`) and
`pip install "mef3io[bench]"` (adds the NWB-Zarr stack too). So a standalone
checkout runs the full suite and benchmarks with no local `mef_tools` needed.

Path resolution (`tests/conftest.py`, `generate_golden.py`, `mef_benchmark.py`)
**prefers a local `mef_tools` checkout** when running inside the original repo —
because that matches the committed golden fixtures (the local dev version) — and
otherwise falls through to the pip-installed `mef-tools`. Nothing to configure.

Remaining nicety (optional): the few tests that `import mef_tools` at module
level could be guarded with `pytest.importorskip("mef_tools")` so the suite
degrades gracefully if the oracle is absent; correctness is otherwise pinned by
the committed `tests/golden/` fixtures + `pymef`.

Note: PyPI `mef-tools` (1.2.3) may lag the local dev version used to generate
the goldens; if a write-direction test diverges, regenerate the goldens against
whichever `mef_tools` you intend to baseline against.

## 3. Extraction steps

```bash
# 1. create the target and copy the export set
scripts/export_mef3io.sh ~/code/mef3io

# 2. initialise the new repo
cd ~/code/mef3io
git init -b main
# copy the Knowledge base section (below) into CLAUDE.md
git add -A && git commit -m "Initial import of mef3io (extracted from mef_tools working tree)"

# 3. build + test in the active env
scripts/dev_build.sh
PYTHONPATH=python pytest tests -q      # needs pymef; legacy tests importorskip
```

Then in the **old** repo, once you are happy: delete the moved paths (or keep
them on a branch) so the two projects don't drift.

## 4. How the assistant's context transfers

A Claude Code session keys its memory to the repo path, so a session started in
the new repo begins with none of this project's memory. To carry it over:

- Put the **Knowledge base** below into `mef3io/CLAUDE.md` (loaded every
  session). That is the real transfer mechanism.
- Keep `requirements/mef3io_design.md` and `docs/encryption_model.md` in the new
  repo — they are the canonical design + the encryption explanation.
- Optionally, in a fresh session in the new repo, ask the assistant to re-create
  its memory files from `CLAUDE.md` + the design doc.

---

## Knowledge base (seed for the new repo's CLAUDE.md)

### What mef3io is

A single C++17/20 core for MEF 3.0 read+write, wrapped for Python via nanobind,
with the high-level semantics of the legacy `mef_tools` (float scaling, NaN
discontinuities, precision inference, the int32-counts + conversion-factor
primitive path) living in C++ so all bindings behave identically. Video is out
of scope. The legacy `mef_tools`/`pymef` stack is the correctness oracle.

Status: read + write complete, cross-validated **both directions** vs
pymef/mef_tools (values, NaN gaps, times, encryption none/L1+L2, fractional fs,
records). ~94 Python tests + standalone C++ Catch2 tests. Wheel builds via
`python -m build`. Parallel decode ~9.5×, byte-deterministic across thread
counts.

### Module map

`core/include/mef3io`, `core/src`:
- `types.hpp` — si1..sf8 aliases + all format constants/offsets.
- `byteio.hpp` — little-endian field read/write (no packed-struct casts).
- `crc` — CRC-32 Koopman (table in `crc.cpp`).
- `crypto` — SHA-256, AES-128-ECB, two-level password scheme (self-contained).
- `headers` — UniversalHeader, MetadataSection1/2/3, TimeSeriesIndex,
  RedBlockHeader: `parse`/`serialize` by explicit offset.
- `metadata` — `.tmet` loader: CRC validate → password check → decrypt s2/s3.
- `red` — RED `decode_block` and `encode_block` (lossless, no detrend/scale).
- `session` — `.mefd/.timd/.segd` lazy tree, indexed reads, `collect_blocks`.
- `reader` — high-level Reader: gridding, NaN gap fill, scaling, parallel decode.
- `records` — record (.rdat/.ridx) read + write.
- `writer` — low-level segment writer (tmet/tidx/tdat).
- `session_writer` — high-level writer: precision inference, quantization, NaN
  splitting, segment management, records.
- `parallel.hpp` — deterministic `parallel_for`.

`python/mef3io`: `Reader`, `Writer`, `compat` (mef_tools.io drop-in), `cache`
(opt-in warm start), `pure` (stub). `bindings/python/mef3io_ext.cpp` = nanobind.

### Format gotchas (hard-won — do not relearn these the hard way)

- **Times are stored NEGATED on disk** as meflib's "recording-time-offset
  applied" marker. User uUTC = `(t>=0) ? t : (-t + rto)` (read),
  `disk = rto - absolute` (write). Applies to universal-header start/end AND
  block/record times. Our fixtures use rto=0. Missing this makes every
  time-range read return 0 samples.
- **Encryption is an all-or-nothing pairing**, not three modes: section 2 → L1
  key, section 3 → L2 key, driven by password presence. "Level-1 only" is not a
  valid file (breaks the legacy writer). Decrypt a section only when its stored
  level is strictly positive; unencrypted files carry -1/-2 (`0xFF`/`0xFE`).
  Full explanation: `docs/encryption_model.md`.
- **Password scheme**: password bytes = terminal byte of each UTF-8 char, 16,
  zero-padded. L1 field = SHA256(L1)[:16]. L2 field = SHA256(L2)[:16] XOR L1.
  L2 access derives putative L1 = SHA256(pwd)[:16] XOR L2field, check its hash.
- **RED data blocks are written UNENCRYPTED** even in encrypted sessions
  (meflib default); only metadata s2/s3 and record bodies are encrypted.
- **Index `file_offset` is FILE-relative** (includes the 1024 B universal
  header) — first block's offset is 1024, not 0.
- **RED encode**: first emitted byte is a junk byte (meflib overwrites
  stats[255] then restores) → drop `emitted[0]`, payload = `emitted[1:]` at
  offset 304; stored `difference_bytes = generated + 1` (uncoded initial
  keysample flag). Encoder is lossless, no detrend/scale — fully pymef-readable.
  Constants: TOP_VALUE 0x80000000, CARRY_CHECK 0x7F800000, SHIFT_BITS 23,
  EXTRA_BITS 7, BOTTOM_VALUE 0x800000, PAD_BYTE 0x7e, 8-byte block alignment.
- **Records**: header 24 B (crc@0, type[4]@4, vmaj@9, vmin@10, enc@11,
  bytes@12, time@16). Body padded to a 16-byte multiple with 0x7e. Records
  default to L2 encryption when the session is encrypted. `.ridx` entry 24 B
  (type@0, vmaj@5, vmin@6, enc@7, file_offset@8, time@16). Times negated.
- **`mef3_dump` is NOT a usable oracle** for mef_tools-written files (it reads
  the encryption level byte as unsigned 255 instead of si1 -1). Use `pymef`
  `read_ts_channels_sample([ch],[0,nsamp])` for the decoded-int32 oracle (no
  time-gap NaN insertion) and `read_ts_channels_uutc` for the gap-filled oracle.
- **Manifest `nsamp` != stored nsamp**: `generate_golden` stored nsamp as
  `get_raw_data` length (time span incl. gaps). Compare channel_info nsamp/
  start/end against pymef `read_ts_channel_basic_info`, not the manifest.
- Metadata file = 16384 B: UH(1024) + S1(1536)@1024 + TS-S2(10752)@2560 +
  S3(3072)@13312. TS index entry 56 B. RED block header 304 B.

### Build / test (use the active conda env for everything)

- `scripts/dev_build.sh` → builds `build/dev`, symlinks `_mef3io*.so` into
  `python/mef3io/`. Test with `PYTHONPATH=python pytest tests`.
- C++ tests: `cmake -S core -B build-core -DMEF3IO_BUILD_TESTS=ON &&
  cmake --build build-core && ctest --test-dir build-core`.
- Wheel: `python -m build --wheel`.

### Known limitations / future work

- Pure-Python backend is a stub (raises). pymef is the oracle; a pure backend is
  future work (assemble from `reference_files/mef3_dump` + reimplementation).
- Append currently writes a NEW segment rather than extending an existing
  segment's `.tdat` (valid, reads fine, not byte-same as mef_tools). This is the
  gating item before `mef_tools` could silently run on mef3io.
- Records write covers Note/EDFA/SyLg/Seiz.
- MATLAB MEX binding not started (design: flat C ABI shim over the C++ API).
- Metadata cache is Python-level (channel metadata + fingerprints); a deeper
  C++ warm-start that skips per-segment `.tmet` reads during data reads is a
  future optimization.

### Release strategy (agreed direction)

mef3io ships as its own package; legacy `mef_tools` stays as the benchmark
baseline/oracle. Downstream trials via `from mef3io.compat import MefReader`.
Only once benchmarks + real-data soak hold, cut `mef_tools` 3.0 whose `io`
re-exports `mef3io.compat` (dropping the pymef dependency) — after resolving the
in-segment append gap.
