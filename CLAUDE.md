# mef3io — assistant context

A single C++17/20 core for **MEF 3.0** read+write, wrapped for Python via
nanobind. The high-level semantics of the legacy `mef_tools` (float scaling,
NaN discontinuities, precision inference, the int32-counts + conversion-factor
primitive path) live in **C++** so all bindings behave identically. Video is out
of scope. The legacy `mef_tools`/`pymef` stack is the correctness oracle.

Status: read + write complete, cross-validated **both directions** vs
pymef/mef_tools (values, NaN gaps, times, encryption none/L1+L2, fractional fs,
records). In-segment append + per-segment map implemented. ~112 Python tests +
standalone C++ Catch2 tests. Wheel builds via `python -m build`. Parallel
decode/encode, byte-deterministic across threads.

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
`session` (lazy .mefd/.timd/.segd tree, indexed reads, `collect_blocks`),
`reader` (gridding, NaN fill, scaling, parallel decode), `records` (read+write),
`writer` (segment writer), `session_writer` (precision inference, quantization,
NaN splitting, segments), `parallel.hpp`.

`python/mef3io/`: `Reader`, `Writer` (incl. `write_annotations`), `compat`
(mef_tools.io drop-in; `MefReader`/`MefWriter` are also re-exported at the top
level — `from mef3io import MefReader, MefWriter` — via lazy `__getattr__`),
`cache` (opt-in warm start), `pure` (stub). `bindings/python/mef3io_ext.cpp` =
nanobind. `matlab/` = MEX + `+mef3io` classes. `examples/` = runnable scripts
(write/read, int32, append, segment map, annotations, encryption, legacy
style). Docs site: MkDocs Material from `docs/` (`mkdocs.yml`; nav pages
index/install/python/matlab/cpp/examples/releasing + the reference docs),
deployed to GitHub Pages by `.github/workflows/docs.yml` on push to main —
keep `mkdocs build --strict` green; `for_agents/` (repo root) holds agent handoff docs, outside the site. `docs/design.md` = full design; `docs/encryption_model.md` = crypto
model; `docs/mef3_format.md` = format reference.

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
- **RED encode**: first emitted byte is junk (meflib overwrites stats[255] then
  restores) → drop emitted[0], payload = emitted[1:] at offset 304; stored
  difference_bytes = generated+1. Lossless no-detrend/no-scale, pymef-readable.
  Constants: TOP_VALUE 0x80000000, CARRY_CHECK 0x7F800000, SHIFT_BITS 23,
  EXTRA_BITS 7, BOTTOM_VALUE 0x800000, PAD_BYTE 0x7e, 8-byte alignment.
- **Records**: header 24 B (crc@0,type[4]@4,vmaj@9,vmin@10,enc@11,bytes@12,
  time@16). Body padded to 16-byte multiple with 0x7e. L2-encrypted when the
  session is encrypted. `.ridx` entry 24 B (type@0,vmaj@5,vmin@6,enc@7,
  offset@8,time@16). file_offset FILE-relative.
- **Oracle**: use `pymef` `read_ts_channels_sample([ch],[0,nsamp])` for decoded
  int32 (no gap NaN) and `read_ts_channels_uutc` for gap-filled. `mef3_dump` is
  NOT usable (reads the encryption sentinel byte unsigned). Manifest `nsamp` !=
  stored nsamp (it's get_raw_data length incl. gaps) — compare vs pymef
  basic_info.

## Known limitations / next steps

- **In-segment append implemented** (`append_time_series_segment` +
  `SessionWriter` hydration): non-first writes extend the channel's last
  segment in place (.tdat streamed-CRC append, .tidx extend, .tmet s2 rewrite);
  `new_segment=True` forces a fresh segment. Appends validate fs/ufact/start
  time vs on-disk metadata (`WriteConflictError` → Python RuntimeError); float
  appends reuse the segment's precision. `Reader.segments(ch)` maps what data
  is where per segment. First appended block keeps discontinuity=true (readers
  are time-gridded so contiguous appends stay seamless).
- **Do NOT `pip install -e .` for C++ dev** — scikit-build-core's editable hook
  loads an install-time extension snapshot that shadows the dev_build symlink
  (meta-path beats sys.path). Keep mef3io uninstalled; use scripts/dev_build.sh.
- Pure-Python backend is a stub. Records write covers Note/EDFA/SyLg/Seiz.
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
