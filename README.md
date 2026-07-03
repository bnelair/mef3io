# mef3io

A single C++ core for MEF 3.0 read/write, wrapped for Python (nanobind). The
high-level semantics of the legacy `mef_tools` — float scaling, NaN
discontinuities, precision inference, the int32-counts + conversion-factor
primitive write path — live in C++, so every language binding behaves
identically.

## Status

Read and write are implemented and cross-validated against the legacy
`pymef` / `mef_tools` stack in both directions (~103 Python tests + standalone
C++ tests). Scope notes:

- **Video** (`.vidd/.vmet/.vidx`) is out of scope (traversal skips it).
- **Encryption** is none or fully-encrypted (section 2 → L1 key, section 3 →
  L2 key); "level-1 only" is not a valid MEF file — see
  [docs/encryption_model.md](docs/encryption_model.md). Unlike the legacy
  reader, an L1 password actually works (signal + technical metadata, subject
  metadata locked) — see
  [docs/legacy_comparison.md](docs/legacy_comparison.md).
- **Records**: read/write Note, EDFA, SyLg, Seiz (others read as header-only).
- **Pure-Python backend**: not yet implemented (`backend="pure"` raises).
- **Append** extends the channel's last segment in place (legacy semantics);
  `new_segment=True` forces a fresh segment.

More docs: [docs/design.md](docs/design.md) (full design),
[docs/legacy_comparison.md](docs/legacy_comparison.md) (measured old-vs-new
comparison: performance, L1 encryption, annotation protection, quantization),
[docs/mef3io_handoff.md](docs/mef3io_handoff.md) (repo-extraction + context),
[benchmarks/README.md](benchmarks/README.md), runnable
[examples/](examples/README.md), and `CLAUDE.md`.

## Install

```bash
pip install mef3io                 # runtime (numpy only)
pip install "mef3io[test]"         # + oracle tests (pymef, mef-tools, pandas, pytest)
pip install "mef3io[bench]"        # + NWB-Zarr benchmark stack
```

Development from the source tree (uses the active env's Python):

```bash
scripts/dev_build.sh            # builds build/dev, symlinks the extension into python/mef3io
python -m pytest tests          # conftest sets up paths; needs pymef/mef-tools as oracle
python -m build --wheel         # build a wheel
```

The legacy `mef-tools` package (PyPI; import name `mef_tools`) is the
correctness oracle and benchmark baseline: the test/benchmark scripts use a
local `mef_tools` checkout when run inside the original repo, and otherwise the
pip-installed `mef-tools` — so a standalone checkout just needs
`pip install "mef3io[test]"` (or `[bench]`).

## Usage

```python
import numpy as np, mef3io

# Write. NaN = discontinuity gap; precision inferred if omitted.
with mef3io.Writer("session.mefd", overwrite=True, units="uV") as w:
    w.write("eeg1", float_signal, start_uutc, fs=256.0)
    # amplifier counts + V/bit, stored verbatim:
    w.write_int32("eeg2", counts_int32, ufact=2.5e-7, start_uutc=start_uutc, fs=256.0)
    w.write_annotations([{"time": start_uutc, "text": "note"}], channel="eeg1")

# Read. Partial reads by uUTC; gaps are NaN; scaled by the conversion factor.
with mef3io.Reader("session.mefd", n_threads=0) as r:   # 0 = all cores
    r.channels
    r.info("eeg1")
    x  = r.read("eeg1", t0, t1)       # float64, NaN gaps
    xi = r.read_raw("eeg1", t0, t1)   # {samples: int32, valid: mask, ...}
    r.segments("eeg1")                # per-segment map: what data is where
    r.toc("eeg1")                     # block index for viewers/seeking
    r.records("eeg1")                 # annotations
```

### Warm-start metadata cache (opt-in)

Off by default. `cache="auto"` snapshots channel metadata into the per-user OS
cache dir (safe for read-only sessions); a path makes it persistent. Warm opens
serve `channels`/`info` without touching the tree and defer the backend until a
data read. Invalidated automatically on any size/mtime change or on write.

```python
mef3io.Reader("session.mefd", cache="auto")          # temp/OS cache dir
mef3io.Reader("session.mefd", cache="/data/s.cache") # persistent
```

### Drop-in for existing pipelines

```python
# from mef_tools.io import MefReader, MefWriter
from mef3io import MefReader, MefWriter
```

Same call shapes and semantics as the legacy classes (NaN-gap splitting,
precision inference, int32 arrays via the primitive path, in-segment appends).
See [examples/07](examples/07_legacy_mef_tools_style.py) and
[docs/legacy_comparison.md](docs/legacy_comparison.md).

## Parallelism

RED block decode and encode run on an internal thread pool (`n_threads`: 0 =
hardware concurrency, 1 = serial; per-call override on reads). Output is
byte-identical regardless of thread count. Bindings release the GIL, so
external dataloader parallelism is safe too.

## Layout

```
core/        C++17/20 library (types, byteio, crc, crypto, headers, metadata,
             red, session, reader, records, writer, session_writer) + Catch2 tests
bindings/    nanobind extension (_mef3io)
python/mef3io/  Reader, Writer, compat (mef_tools.io shim), cache, pure (stub)
examples/    runnable scripts: write/read, int32, append, segment map,
             annotations, encryption, legacy drop-in, replicability checks
tests/       golden fixture generator + P1–P8 pytest suites (pymef oracle)
docs/        design.md, encryption_model.md, legacy_comparison.md
```
