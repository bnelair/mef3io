# Examples

Thirteen runnable Python scripts live in
[`examples/`](https://github.com/bnelair/mef3io/tree/main/examples); each is
self-contained, writes its session under a temporary directory, and prints
what it does. MATLAB equivalents of the core operations are shown in the
[MATLAB guide](matlab.md) and exercised by
[`matlab/test_mef3io.m`](https://github.com/bnelair/mef3io/blob/main/matlab/test_mef3io.m).

| Script | Shows |
|---|---|
| [01_write_and_read.py](https://github.com/bnelair/mef3io/blob/main/examples/01_write_and_read.py) | Writing float data (NaN = gap, precision inference), whole-channel and windowed reads, `read_raw` |
| [02_int32_primitive.py](https://github.com/bnelair/mef3io/blob/main/examples/02_int32_primitive.py) | The primitive path: int32 counts + conversion factor, validity mask, bit-exact round trip |
| [03_appending.py](https://github.com/bnelair/mef3io/blob/main/examples/03_appending.py) | In-segment append, reopening a session, gaps, forced new segments, rejected conflicts |
| [04_segment_map.py](https://github.com/bnelair/mef3io/blob/main/examples/04_segment_map.py) | `Reader.segments()` / `toc()`: locating data across day-long gaps, targeted windowed reads |
| [05_annotations.py](https://github.com/bnelair/mef3io/blob/main/examples/05_annotations.py) | Records at session and channel level; Note/SyLg/EDFA with durations |
| [06_encryption.py](https://github.com/bnelair/mef3io/blob/main/examples/06_encryption.py) | Two-level passwords and what each level unlocks |
| [07_legacy_mef_tools_style.py](https://github.com/bnelair/mef3io/blob/main/examples/07_legacy_mef_tools_style.py) | `from mef3io import MefReader, MefWriter` as a mef_tools drop-in |
| [08_legacy_compatibility.py](https://github.com/bnelair/mef3io/blob/main/examples/08_legacy_compatibility.py) | Legacy-vs-new write/read timing and value comparison (needs `mef3io[test]` + scipy/tqdm) |
| [09_encryption_replicability.py](https://github.com/bnelair/mef3io/blob/main/examples/09_encryption_replicability.py) | The full encryption matrix: both writers × plain/encrypted × both readers, plus L1/L2/wrong-password probes (needs `mef3io[test]`) |
| [10_tar_archive.py](https://github.com/bnelair/mef3io/blob/main/examples/10_tar_archive.py) | Single-file sessions: `archive_session`, reading the tar in place, writer rejection, standard-tar interop |
| [11_legacy_rto_sessions.py](https://github.com/bnelair/mef3io/blob/main/examples/11_legacy_rto_sessions.py) | Legacy recording-time-offset ("anonymized") sessions: the date recovered from plain bytes, true-UTC reads in mef3io and pymef, consistent appends/channels/notes, the refusals (needs `mef3io[test]`; see [De-identification](anonymization.md)) |
| [12_edf_to_mef.py](https://github.com/bnelair/mef3io/blob/main/examples/12_edf_to_mef.py) | Converting EDF, EDF+ or BDF to MEF 3.0, with a check of the result (needs pyedflib: `pip install "mef3io[edf]"`; see [the how-to](howto/edf_to_mef.md)) |
| [13_anonymize.py](https://github.com/bnelair/mef3io/blob/main/examples/13_anonymize.py) | De-identified copies: `anonymize.check` before copying (a refused copy name and its code), `anonymize_session` with one channel left out and the dates moved to start on 1 January 2000, a copy that keeps the real dates (the default), the verification and its notes, the copy read back (Python and C++ only; see [De-identification](anonymization.md)) |

The numbers produced by 08/09 (and the MATLAB/Python twins
`benchmarks/bindings_benchmark.py` + `matlab/benchmark_mef3io.m`) are
aggregated in [Performance & legacy comparison](legacy_comparison.md).
