# mef3io examples

Small, self-contained scripts, each runnable directly:

```bash
python examples/01_write_and_read.py
```

Each script writes its session under a temporary directory and prints what it
does. Times in MEF 3.0 are **uUTC** — microseconds since the Unix epoch.

| Script | Shows |
| --- | --- |
| `01_write_and_read.py` | Writing float data (NaN = gap, precision inference) and reading it back |
| `02_int32_primitive.py` | The primitive path: int32 counts + conversion factor, validity mask |
| `03_appending.py` | In-segment append, forced new segments, reopening a session, conflicts |
| `04_segment_map.py` | `Reader.segments()` / `toc()` — finding what data is where across gaps |
| `05_annotations.py` | Writing and reading records (annotations), session- and channel-level |
| `06_encryption.py` | Two-level passwords, what each level can access |
| `07_legacy_mef_tools_style.py` | `from mef3io import MefReader, MefWriter` as a mef_tools drop-in |
| `08_legacy_compatibility.py` | Legacy-vs-new write/read timing and value comparison on larger data (needs `mef3io[test]` + scipy/tqdm; writes under a temporary directory, or `$MEF3IO_BENCH_DIR`) |
| `09_encryption_replicability.py` | Encryption replicability matrix: both writers × plain/encrypted × both readers, plus what L1/L2/wrong passwords actually unlock (needs `mef3io[test]`) |
| `10_tar_archive.py` | Single-file `.mefd.tar` sessions: archive, read in place, extract |
| `11_legacy_rto_sessions.py` | Legacy recording-time-offset ("anonymized") sessions: the date is recoverable from plain bytes; mef3io and pymef read true UTC; mef3io appends, adds a channel and a note consistently; refusals (needs `mef3io[test]`; see `docs/anonymization.md`) |
| `12_edf_to_mef.py` | Converting EDF, EDF+ or BDF to MEF 3.0: per-channel rates, units, time zone, annotations, EDF+D gaps, verification (needs `mef3io[edf]`; with no arguments it converts a demo file; see `docs/howto/edf_to_mef.md`) |
