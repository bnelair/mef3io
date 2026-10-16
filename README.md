# mef3io

Read and write **MEF 3.0 (Multiscale Electrophysiology Format)** files from
**Python**, **MATLAB** and **C++**. MEF 3.0 is a compressed, optionally
encrypted file format for long electrophysiology recordings: mainly EEG,
intracranial EEG (iEEG) and ECG, but it suits any regularly sampled signal,
such as EMG, other biosignals or sensor data. One C++ core does the work, so
every language reads and writes the same files the same way. Sessions
written by mef3io open in the established MEF 3.0 tools listed below, and
sessions written by those tools open in mef3io.

[![PyPI](https://img.shields.io/pypi/v/mef3io)](https://pypi.org/project/mef3io/)
[![Python](https://img.shields.io/python/required-version-toml?tomlFilePath=https%3A%2F%2Fraw.githubusercontent.com%2Fbnelair%2Fmef3io%2Fmain%2Fpyproject.toml)](https://pypi.org/project/mef3io/)
[![CI](https://img.shields.io/github/actions/workflow/status/bnelair/mef3io/ci.yml?branch=main&label=CI)](https://github.com/bnelair/mef3io/actions/workflows/ci.yml)
[![Docs](https://img.shields.io/github/actions/workflow/status/bnelair/mef3io/docs.yml?branch=main&label=docs)](https://bnelair.github.io/mef3io/)
[![License](https://img.shields.io/github/license/bnelair/mef3io)](https://github.com/bnelair/mef3io/blob/main/LICENSE)

**Documentation: <https://bnelair.github.io/mef3io/>**

## MEF 3.0 and the original libraries

mef3io grew out of an earlier stack of MEF libraries and stays compatible
with it: files written by either open in the other. **MEF 3.0 support is
maintained here, in mef3io, as open source (Apache-2.0).**

- **MEF 3.0 specification**: the header of meflib,
  [`meflib.h`](https://github.com/msel-source/meflib/blob/multiplatform/meflib/meflib.h),
  is titled "Specification for Multiscale Electrophysiology Format (MEF)
  version 3.0". The format's design is described in Brinkmann et al.,
  *Large-scale electrophysiology: acquisition, compression, encryption, and
  storage of big data*, J Neurosci Methods 180(1):185-192, 2009,
  [doi:10.1016/j.jneumeth.2009.03.022](https://doi.org/10.1016/j.jneumeth.2009.03.022).
- **[meflib](https://github.com/msel-source/meflib)**: the original reference
  C library for MEF 3.0, from the msel-source project.
- **[pymef](https://github.com/msel-source/pymef)**
  ([docs](https://pymef.readthedocs.io/)): a Python wrapper over meflib.
- **[mef_tools](https://github.com/bnelair/mef_tools)** (archived; on PyPI as
  [`mef-tools`](https://pypi.org/project/mef-tools/)): high-level Python
  reading and writing built on pymef. mef3io stands in for it:
  `from mef3io import MefReader, MefWriter`.
- **MATLAB (legacy):**
  [MEF_import](https://github.com/jiecui/MEF_import) (EEGLAB plugin) and
  [mef_reader_fieldtrip](https://github.com/jiecui/mef_reader_fieldtrip)
  (FieldTrip reader for MEF 2.1, MEF 3.0 and MED 1.0), published by
  Richard Jie Cui. mef3io has its own MATLAB interface.
- The older **MEF 2.1** format has its own library and specification,
  [mef_lib_2_1](https://github.com/benbrinkmann/mef_lib_2_1); mef3io reads
  and writes MEF 3.0 only.

mef3io is an independent implementation of the specification, tested against
meflib, pymef and mef_tools. New to the format? See
[MEF 3.0 for students](https://bnelair.github.io/mef3io/students/).

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
[C++](https://bnelair.github.io/mef3io/cpp/) guides, runnable
[examples](https://bnelair.github.io/mef3io/examples/), and short how-to
pages such as
[converting EDF, EDF+ or BDF to MEF 3.0](https://bnelair.github.io/mef3io/howto/edf_to_mef/).

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
  the level-1 password opens the technical metadata while the subject's
  identity stays behind the level-2 password.
- **Looking before reading.** `mef3io.inspect(path)` (or
  `python -m mef3io inspect`) describes a session — channels, segments,
  gaps, annotations, encryption, what identifying fields it holds — without
  reading any samples, and long reads and operations can report progress and
  be cancelled from another thread (Python and C++).
- **One file instead of a folder.** A session can be packed into a single
  `.mefd.tar` and read in place, without unpacking.

## Upgrading from mef3io 1.1.2 or earlier

Sessions written by mef3io 1.1.2 and earlier read correctly in mef3io and
pymef, but declare some buffer sizes as `0`, which can crash meflib-based
readers. Opening one warns once. Appending with a current version repairs the
segment it extends; `mef3io.repair_session` fixes the rest in place — see
[Validating and repairing](https://bnelair.github.io/mef3io/validation/).
Changes in every version, and what an upgrade asks of you, are listed in the
[release notes](https://bnelair.github.io/mef3io/release_notes/).

## Scope and limitations

- **Video** files (`.vidd`) are out of scope; a session's video folders are
  skipped.
- **Encryption** is all or nothing per session: unencrypted, or both password
  levels set. It protects the subject's identity and annotation text, not the
  signal: the samples and their timing can be decoded without a password, so
  keep confidential recordings on protected storage. See
  [what encryption protects](https://bnelair.github.io/mef3io/encryption_model/#what-encryption-protects).
- **Data-block encryption.** The MEF 3.0 specification defines encrypted
  data blocks. Leaving them unencrypted is a habit of past implementations,
  not the format's design: meflib does not encrypt them unless asked, and
  the legacy readers mishandle encrypted blocks (pymef crashes on them, and
  meflib requires the level-2 password even for level-1 blocks). mef3io
  reads such blocks (some third-party tools write them): a block encrypted at
  level 1 with either password, one at level 2 with the level-2 password.
  Recordings mef3io writes today leave data blocks unencrypted and stay fully
  compatible; encrypted *metadata* works in every direction. mef3io intends
  to follow the specification here later, which could produce recordings
  that older tools cannot read; that is not decided yet
  ([#94](https://github.com/bnelair/mef3io/issues/94),
  [#92](https://github.com/bnelair/mef3io/issues/92)).
- **Single-file `.mefd.tar` sessions** are a mef3io addition, not part of
  MEF 3.0, so meflib, pymef and mef_tools cannot open them directly. They
  are plain uncompressed tar files, so they can always be unpacked:
  `mef3io.extract_session`, `tar -xf` (built into Windows 10 and later,
  macOS and Linux) or the free 7-Zip on Windows. The unpacked `.mefd` folder
  works with the legacy tools.
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
the on-disk
[format reference](https://bnelair.github.io/mef3io/mef3_format/) and the
[design](https://bnelair.github.io/mef3io/design/) are on the documentation
site. Report vulnerabilities privately as described in
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

## Related projects

- [mef3io-studio](https://github.com/bnelair/mef3io-studio): a desktop app for
  exploring and anonymizing MEF 3.0 recordings; installers are on its
  Releases page.
- [mef3io-server](https://github.com/bnelair/mef3io-server): a gRPC server
  that streams MEF 3.0 data to viewers on a local network.

## Citing and license

If you use mef3io in published work, please cite it via
[CITATION.cff](https://github.com/bnelair/mef3io/blob/main/CITATION.cff).
Licensed under [Apache 2.0](https://github.com/bnelair/mef3io/blob/main/LICENSE).
Maintained by Filip Mivalt, Bioelectronics Neurophysiology and Engineering
Laboratory (BNEL) at Mayo Clinic.

## Contributors and credits

**mef3io:** Filip Mivalt (maintainer), Vladimir Sladky, Václav Křemen.

**The format and the earlier tools**, whose authors we thank for making this
work possible: Matt Stead, who conceived MEF, and his co-authors of the 2009
paper Benjamin H. Brinkmann, Mark R. Bower, Keith A. Stengel and Gregory A.
Worrell; the meflib, pymef and mef_tools contributors, including Jan
Cimbálník, Daniel P. Crepeau, Max van den Boom, Ben Brinkmann, Eric Larson,
Václav Křemen and others; and Richard Jie Cui, who published the MATLAB
tools. The full list, with what each project did, is in
[CONTRIBUTORS.md](https://github.com/bnelair/mef3io/blob/main/CONTRIBUTORS.md).

This project follows a
[code of conduct](https://github.com/bnelair/mef3io/blob/main/CODE_OF_CONDUCT.md).
