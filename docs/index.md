# mef3io

**MEF 3.0 read and write — one C++ core, identical behavior in Python, MATLAB,
and C++.**

[MEF 3.0](https://msel.mayo.edu/codes.html) (Multiscale Electrophysiology
Format) is a compressed, encryptable format for long-term electrophysiology
recordings. mef3io implements it once, in C++17/20, and wraps that core
thinly for each language — so float scaling, NaN-gap handling, precision
inference, appends, and encryption behave exactly the same everywhere, and
every binding carries the same version number.

!!! tip "Long recordings"
    These files run for days to months and reach tens of gigabytes **per
    channel**. [Long recordings](long_recordings.md) covers the append
    workload, the `durability` knob, and what is guaranteed at that scale;
    [Validation & repair](validation.md#recovery-after-an-interrupted-write)
    covers recovering a session after an interrupted write.

## Why mef3io

- **Fast** — parallel RED encode/decode: **~7–8× faster** than the legacy
  pymef/mef_tools stack on both write and read, at identical file sizes;
  MATLAB and Python perform identically (~135 M samples/s reads). See the
  [measured comparison](legacy_comparison.md).
- **Correct** — cross-validated in both directions against pymef/mef_tools
  (values, NaN gaps, times, encryption, fractional sampling rates, records),
  with committed golden fixtures, ~100 Python tests, C++ Catch2 tests, and a
  MATLAB round-trip suite.
- **Drop-in** — `from mef3io import MefReader, MefWriter` runs existing
  mef_tools code with only the import changed.
- **Encryption that works** — the two-level password model is implemented as
  specified: a level-1 password reads the signal and technical metadata while
  subject identity stays locked. (The legacy reader returns zero samples and
  garbage metadata at level 1 — [details](legacy_comparison.md).)
- **Deterministic** — output is byte-identical regardless of thread count.

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
| [Examples](examples.md) | nine runnable scripts |
| [Format reference](mef3_format.md) | the MEF 3.0 on-disk structure, field by field |
| [Performance & legacy comparison](legacy_comparison.md) | measured speed, encryption, and quantization differences |

## Status and scope

Read + write are complete and released; in-segment append matches legacy
semantics. Out of scope: MEF video files. Records cover Note, EDFA, SyLg,
Seiz. The pure-Python fallback backend is not yet implemented.

Sessions written by mef3io **≤ 1.1.2** under-declare the section-2 buffer sizes
that meflib-based readers (CyberPSG and similar) allocate from; they read
correctly in mef3io and pymef but can crash such a reader. Since 1.1.3 those
fields are measured from the blocks written — appending to an older segment
repairs it in place, and rewriting the session fixes it outright. Opening such
a session warns once, saying plainly that reading is unaffected; see
[Validating and repairing](validation.md) to inspect or fix a file, and
[the format reference](mef3_format.md#the-buffer-sizing-declarations) for what
the declarations mean.

mef3io is developed at the Mayo Clinic BNEL (Bioelectronics Neurophysiology
and Engineering Lab).
