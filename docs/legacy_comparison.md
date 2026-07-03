# mef3io vs the legacy stack (pymef / mef_tools)

Both implementations read each other's files bit-identically at full access,
and encrypting a session never changes the decoded samples. The differences
below are measured, and each is reproducible by running
[`examples/09_encryption_replicability.py`](../examples/09_encryption_replicability.py)
(needs `pip install "mef3io[test]"`).

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
