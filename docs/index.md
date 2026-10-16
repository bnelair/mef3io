---
description: "Read and write MEF 3.0 (Multiscale Electrophysiology Format) recordings such as EEG, iEEG and ECG from Python, MATLAB and C++, compatible with meflib, pymef and mef_tools."
---

# mef3io

**MEF 3.0 read and write — one C++ core, identical behavior in Python, MATLAB,
and C++.**

**MEF 3.0 (Multiscale Electrophysiology Format)** is a compressed,
encryptable file format for long electrophysiology recordings: mainly EEG,
intracranial EEG (iEEG) and ECG, but it suits any regularly sampled signal,
such as EMG, other biosignals or sensor data. mef3io implements it once, in
C++17/20, and wraps that core thinly for Python, MATLAB and C++, so float
scaling, NaN-gap handling, precision inference, appends, and encryption
behave exactly the same everywhere, and every binding carries the same
version number.

!!! tip "Long recordings"
    These files run for days to months and reach tens of gigabytes **per
    channel**. [Long recordings](long_recordings.md) covers the append
    workload, the `durability` knob, and what is guaranteed at that scale;
    [Validation & repair](validation.md#recovery-after-an-interrupted-write)
    covers recovering a session after an interrupted write.

## Why mef3io

- **Fast** — RED blocks are encoded and decoded in parallel. Full reads run
  **~9× faster** than the legacy pymef/mef_tools stack and whole-session
  writes **~2× faster**, at identical file sizes. That was measured on a
  6-core 2013 Xeon; an Apple-silicon Mac measured ~7–8× on both. Each core
  decodes at the same speed as meflib: the gain comes from using all of
  them. MATLAB and Python perform identically. See the
  [benchmarks](benchmarks.md) and the
  [legacy comparison](legacy_comparison.md).
- **Correct** — cross-validated in both directions against pymef/mef_tools
  (values, NaN gaps, times, encryption, fractional sampling rates, records),
  with committed golden fixtures, Python and C++ test suites, and a MATLAB
  round-trip suite.
- **Drop-in** — `from mef3io import MefReader, MefWriter` runs existing
  mef_tools code with only the import changed.
- **Encryption that works** — the two-level password model is implemented as
  specified: a level-1 password reads the signal and technical metadata while
  subject identity stays locked. (The legacy reader returns zero samples and
  garbage metadata at level 1 — [details](legacy_comparison.md).) Encryption
  hides identity and annotation text, not the samples themselves — see
  [what it protects](encryption_model.md#what-encryption-protects).
- **Deterministic** — output is byte-identical regardless of thread count.
- **Made for tools** — `mef3io.inspect()` describes a session without
  reading its samples, and reads and long operations can report progress and
  be cancelled from another thread (Python and C++).
- **Encrypted data blocks** written by some third-party tools are read too;
  see [compatibility](encryption_model.md#compatibility-with-other-mef-30-tools).

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
  [matmef](https://github.com/MultimodalNeuroimagingLab/matmef) (a MATLAB
  wrapper over meflib),
  [mef_reader_fieldtrip](https://github.com/MultimodalNeuroimagingLab/mef_reader_fieldtrip)
  (FieldTrip reader for MEF 2.1, MEF 3.0 and MED 1.0) and
  [MEF_import](https://github.com/jiecui/MEF_import) (EEGLAB plugin). mef3io
  has its own MATLAB interface.
- The full list of earlier tools and their contributors is on
  [Contributors and credits](contributors.md).
- The older **MEF 2.1** format has its own library and specification,
  [mef_lib_2_1](https://github.com/benbrinkmann/mef_lib_2_1); mef3io reads
  and writes MEF 3.0 only.

mef3io is an independent implementation of the specification, tested against
meflib, pymef and mef_tools. New to the format? See
[MEF 3.0 for students](students.md).

## Quick taste (Python)

```python
import mef3io

with mef3io.Writer("session.mefd", overwrite=True, units="uV") as w:
    w.write("ch1", data, start_uutc, fs=256.0)          # NaN = gap
    w.write_annotations([{"time": start_uutc, "text": "start"}], "ch1")

with mef3io.Reader("session.mefd") as r:
    x = r.read("ch1", t0, t1)     # float64 on the fs grid, gaps = NaN
    r.segments("ch1")             # what data is where, across any gaps
```

The same session opens in MATLAB (`mef3io.Reader('session.mefd')`) and C++
(`mef3io::Reader`), and in the legacy pymef/mef_tools stack.

## Where to go

| | |
|---|---|
| [Install](install.md) | pip wheels, MATLAB MEX, building from source |
| [Python guide](python.md) | `Reader`, `Writer`, legacy compat, caching |
| [MATLAB guide](matlab.md) | `+mef3io` classes and the MEX |
| [C++ guide](cpp.md) | using the core library and the flat C ABI |
| [How-to](howto/read_python.md) | short answers: read in [Python](howto/read_python.md) or [MATLAB](howto/read_matlab.md), [convert EDF to MEF 3.0](howto/edf_to_mef.md), [long recordings with gaps](howto/long_recordings.md) |
| [Examples](examples.md) | twelve runnable scripts |
| [Release notes](release_notes/index.md) | what changed in each version, and what an upgrade asks of you |
| [Format reference](mef3_format.md) | the MEF 3.0 on-disk structure, field by field |
| [De-identification](anonymization.md) | why meflib's recording-time offset is not anonymization, and what mef3io does instead |
| [Performance & legacy comparison](legacy_comparison.md) | measured speed, encryption, and quantization differences |

## Scope and limitations

- MEF video files are out of scope; a session's video folders are skipped.
- Encryption is all or nothing per session, and protects identity and
  annotation text, not the signal — see
  [what encryption protects](encryption_model.md#what-encryption-protects).
- Single-file `.mefd.tar` sessions are a mef3io addition, not part of
  MEF 3.0: legacy tools need them unpacked first (`extract_session`,
  `tar -xf`, or 7-Zip on Windows); see the [Python guide](python.md#tar-archives-single-file-sessions).
- Data blocks are written unencrypted, as meflib does by default, although
  the format defines their encryption; aligning with the specification is
  under discussion
  ([compatibility](encryption_model.md#compatibility-with-other-mef-30-tools)).
- Every record type MEF 3.0 defines is read; `Note`, `SyLg` and `EDFA` can be
  written with their text.
- The pure-Python fallback backend is not implemented.
- Sessions written by mef3io **≤ 1.1.2** declare some buffer sizes as `0`,
  which can crash meflib-based readers (mef3io and pymef read them fine);
  [Validating and repairing](validation.md) shows how to fix them.

What changed in each version is in the
[release notes](release_notes/index.md).

## Related projects

- [mef3io-studio](https://github.com/bnelair/mef3io-studio): a desktop app for
  exploring and anonymizing MEF 3.0 recordings; installers are on its
  Releases page.
- [mef3io-server](https://github.com/bnelair/mef3io-server): a gRPC server
  that streams MEF 3.0 data to viewers on a local network.

Maintained by Filip Mivalt, Bioelectronics Neurophysiology and Engineering
Laboratory (BNEL) at Mayo Clinic.
