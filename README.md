# mef3io

Read and write **MEF 3.0** (Multiscale Electrophysiology Format) — the
compressed, optionally encrypted format used for long intracranial and scalp
EEG recordings — from **Python**, **MATLAB** and **C++**. One C++ core does
the work, so every language reads and writes the same files the same way.
Sessions written by mef3io open in the established MEF 3.0 tools (pymef,
mef_tools, and viewers built on meflib, the reference C library), and
sessions written by those tools open in mef3io.

[![PyPI](https://img.shields.io/pypi/v/mef3io)](https://pypi.org/project/mef3io/)
[![Python](https://img.shields.io/python/required-version-toml?tomlFilePath=https%3A%2F%2Fraw.githubusercontent.com%2Fbnelair%2Fmef3io%2Fmain%2Fpyproject.toml)](https://pypi.org/project/mef3io/)
[![CI](https://img.shields.io/github/actions/workflow/status/bnelair/mef3io/ci.yml?branch=main&label=CI)](https://github.com/bnelair/mef3io/actions/workflows/ci.yml)
[![Docs](https://img.shields.io/github/actions/workflow/status/bnelair/mef3io/docs.yml?branch=main&label=docs)](https://bnelair.github.io/mef3io/)
[![License](https://img.shields.io/github/license/bnelair/mef3io)](https://github.com/bnelair/mef3io/blob/main/LICENSE)

**Documentation: <https://bnelair.github.io/mef3io/>**

## Install

```bash
pip install mef3io
```

Prebuilt wheels for Linux, macOS and Windows (Python 3.10+); numpy is the only
dependency. For **MATLAB**, download `mef3io-matlab-vX.Y.Z.zip` from the
[latest release](https://github.com/bnelair/mef3io/releases/latest) and
`addpath` it, or build the MEX yourself. For **C++**, add `core/` to your CMake
project. Details for all three: [Install](https://bnelair.github.io/mef3io/install/).

## Usage

```python
import numpy as np
import mef3io

fs = 256.0
start = 1_700_000_000_000_000            # microseconds since 1970 (uUTC)
signal = np.random.randn(60 * int(fs))   # one minute
signal[1000:1500] = np.nan               # a gap: NaN is stored as missing data

with mef3io.Writer("session.mefd", overwrite=True, units="uV") as w:
    w.write("eeg1", signal, start, fs=fs)
    w.write_annotations([{"time": start, "text": "recording start"}], channel="eeg1")

with mef3io.Reader("session.mefd") as r:
    print(r.channels)                     # ['eeg1']
    x = r.read("eeg1", start, start + 10_000_000)  # first 10 s, float64, gaps = NaN
    print(r.segments("eeg1"))             # what data is where
    print(r.records("eeg1"))              # annotations
```

The same session in MATLAB:

```matlab
r = mef3io.Reader('session.mefd');
x = r.read('eeg1');
```

More: the [Python](https://bnelair.github.io/mef3io/python/),
[MATLAB](https://bnelair.github.io/mef3io/matlab/) and
[C++](https://bnelair.github.io/mef3io/cpp/) guides, and runnable
[examples](https://bnelair.github.io/mef3io/examples/).

## What it is built for

- **Recordings far larger than memory.** A single channel of a long
  recording can be tens of gigabytes. A read loads only the blocks the
  requested time window needs, and decoding runs on all cores.
- **Missing data as a first-class value.** Gaps in a recording come back as
  `NaN` on the sampling grid, never silently stitched together;
  `read_raw` returns the raw integer samples with a validity mask.
- **Recordings that keep growing.** A session can be appended to for days or
  months; with one `Writer` kept open, each append costs time in proportion
  to the new data, not to the recording so far. If a write is interrupted,
  the session can be checked and recovered — see [Long recordings](https://bnelair.github.io/mef3io/long_recordings/) and
  [Validating and repairing](https://bnelair.github.io/mef3io/validation/).
- **Existing data and code.** It reads MEF 3.0 sessions from other writers,
  and `from mef3io import MefReader, MefWriter` runs code written for the
  legacy `mef_tools` package with only the import changed.
- **Protected recordings.** Both MEF 3.0 password levels work as specified:
  the level-1 password opens the signal while the subject's identity stays
  behind the level-2 password.
- **One file instead of a folder.** A session can be packed into a single
  `.mefd.tar` and read in place, without unpacking.

## Upgrading from mef3io 1.1.2 or earlier

Sessions written by mef3io 1.1.2 and earlier read correctly in mef3io and
pymef, but declare some buffer sizes as `0`, which can crash meflib-based
readers. Opening one warns once. Appending with a current version repairs the
segment it extends; `mef3io.repair_session` fixes the rest in place — see
[Validating and repairing](https://bnelair.github.io/mef3io/validation/).
Changes in every version are listed in the [release notes](https://github.com/bnelair/mef3io/releases).

## Scope and limitations

- **Video** files (`.vidd`) are out of scope; a session's video folders are
  skipped.
- **Encryption** is all or nothing per session: unencrypted, or both password
  levels set. See the
  [encryption model](https://bnelair.github.io/mef3io/encryption_model/).
- **Records** (annotations): every record type MEF 3.0 defines is read;
  `Note`, `SyLg` and `EDFA` can be written with their text.
- **De-identification**: meflib's recording-time offset does not hide the
  recording date (it stays in every metadata file, in plain text unless the
  session is encrypted), so mef3io never creates such sessions; it reads and
  extends existing ones. A real de-identified copy is planned
  ([#70](https://github.com/bnelair/mef3io/issues/70)) — see
  [De-identification](https://bnelair.github.io/mef3io/anonymization/).
- **Pure-Python backend** is not implemented (`backend="pure"` raises); the
  wheels carry the compiled core.

## Contributing and support

Bug reports, feature requests and questions go to
[issues](https://github.com/bnelair/mef3io/issues). To build and
test from source, see
[CONTRIBUTING.md](https://github.com/bnelair/mef3io/blob/main/CONTRIBUTING.md);
the design, the on-disk
[format reference](https://bnelair.github.io/mef3io/mef3_format/) and
architecture notes for contributors (including
[`CLAUDE.md`](https://github.com/bnelair/mef3io/blob/main/CLAUDE.md)) are in the
repository. Report vulnerabilities privately as described in
[SECURITY.md](https://github.com/bnelair/mef3io/blob/main/SECURITY.md).

```
core/        C++ library (the format, read, write, validation) + Catch2 tests
bindings/    Python extension (nanobind)
python/      the mef3io Python package
matlab/      MEX gateway over the C ABI + the +mef3io MATLAB classes
examples/    runnable scripts
tests/       pytest suites, cross-checked against pymef / mef_tools
benchmarks/  performance comparisons
docs/        documentation site source
```

## Citing and license

If you use mef3io in published work, please cite it via
[CITATION.cff](https://github.com/bnelair/mef3io/blob/main/CITATION.cff).
Licensed under [Apache 2.0](https://github.com/bnelair/mef3io/blob/main/LICENSE).
Developed at the Mayo Clinic Bioelectronics Neurophysiology and Engineering
Lab (BNEL).
