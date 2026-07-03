# mef3io vs the legacy stack (pymef / mef_tools)

Both implementations read each other's files bit-identically at full access,
and encrypting a session never changes the decoded samples. The differences
below are measured, and each is reproducible by running
[`examples/08_legacy_compatibility.py`](https://github.com/bnelair/mef3io/blob/main/examples/08_legacy_compatibility.py)
(performance) and
[`examples/09_encryption_replicability.py`](https://github.com/bnelair/mef3io/blob/main/examples/09_encryption_replicability.py)
(encryption/access levels); both need `pip install "mef3io[test]"`.

## Performance

Measured with `examples/08`: 5 channels × 5 h at 512 Hz (9.2 M samples per
channel, ~88 MB session, band-pass-filtered noise, precision 3), encrypted,
Apple-silicon macOS, Python 3.13:

| Operation | legacy | mef3io | speedup |
|---|---|---|---|
| Write (whole session) | 4.67 s | 0.66 s | **7.1×** |
| Open / read headers (legacy-written) | 0.011 s | 0.004 s | 2.5× |
| Read all data (legacy-written file) | 2.78 s | 0.36 s | **7.8×** |
| Read all data (mef3io-written file) | 2.80 s | 0.35 s | **8.0×** |
| File size | 87.79 MB | 87.79 MB | identical |

Same run confirms cross-compatibility on unencrypted content at full access:
both readers return equal arrays with matching NaN positions on both writers'
files (`Data equality: True`, `NaN positions match: True`, all four
writer×reader combinations).

### MATLAB vs Python binding (same C++ core)

Both bindings sit on the same core, so the language layer is essentially
free. Measured with `matlab/benchmark_mef3io.m` and
`benchmarks/bindings_benchmark.py` on the identical workload (5 ch × 5 h @
512 Hz = 46.1 M samples, smoothed noise + NaN gap, precision 3; same machine
as above; MATLAB R2026a / Python 3.13):

| | write | read | file size |
|---|---|---|---|
| Python, plain | 0.62 s (74 MS/s) | 0.34 s (134 MS/s) | 80.2 MB |
| Python, encrypted | 0.62 s (75 MS/s) | 0.34 s (137 MS/s) | 80.2 MB |
| MATLAB, plain | 0.74 s (63 MS/s) | 0.34 s (135 MS/s) | 80.1 MB |
| MATLAB, encrypted | 0.61 s (75 MS/s) | 0.34 s (137 MS/s) | 80.1 MB |

Takeaways: MATLAB and Python are within measurement noise of each other
(reads identical at ~135 M samples/s; the one slower MATLAB write is
first-run warmup), **encryption costs nothing** on either binding (it only
wraps metadata — the signal codec path is unchanged), and both are the same
~7–8× ahead of the legacy pymef stack shown in the table above. Sessions
written by either binding read back bit-identically in the other.

## Level-1 password behavior — the main difference

MEF 3.0 encrypts **section 2** (technical metadata: fs, sample counts,
conversion factor) with the level-1 key and **section 3** (subject identity,
recording-time offset) with the level-2 key; signal blocks themselves are not
encrypted. The intended contract: an L1 holder reads the signal and technical
metadata but cannot see who the subject is; L2 unlocks everything
(see [encryption_model.md](encryption_model.md)).

With an **L1 password on an encrypted session**:

| | legacy (pymef / mef_tools 1.2.3) | mef3io |
|---|---|---|
| Signal | **0 samples returned** | bit-identical, complete |
| Technical metadata (s2) | **ciphertext read as numbers** (e.g. fs = −1.5·10¹⁹⁹) | correct |
| Subject metadata (s3) | no API; garbage used internally | cleanly locked, fields `None` |
| Annotations | see the writer gap below | refused (`UNAVAILABLE`) |
| Wrong / missing password | rejected | rejected |

pymef *validates* an L1 password (the session opens) but never uses the L1 key
to decrypt section 2 — so fs and sample counts are ciphertext reinterpreted as
float64/int64, and the windowed read matches zero blocks. This happens on
legacy-written files as well as mef3io-written ones: it is a reader defect,
not a file incompatibility. In practice the legacy L1 password was unusable.

mef3io validates the password, derives the access level, decrypts exactly the
sections that level allows, and reports the rest as unavailable
(`Reader.info()` → `section3_available`, subject fields `None` under L1).
Two-level access therefore works as designed: the L1 password can be given to
signal-processing staff without exposing subject identity.

## Annotation encryption — a legacy writer gap

The legacy writer stores annotation record bodies **unencrypted** even in an
encrypted session: they are readable with an L1 password — or straight off
disk with no password. mef3io encrypts record bodies with the level-2 key
(meflib semantics). Treat annotations in legacy-written encrypted sessions as
unprotected; rewriting the session with mef3io fixes it.

## Quantization — not bit-identical across writers, by design

For `precision=3` the legacy writer computes `np.round(x, 3)` followed by a
truncating int32 cast of `1000 * rounded`; mef3io stores `round(x * 1000)`
directly. Boundary samples can therefore differ by up to ~2 quantization
counts between the two writers. Both are valid MEF, each round-trips its own
quantization exactly, and both readers return bit-identical arrays for any
given file.
