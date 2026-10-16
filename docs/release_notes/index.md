# Release notes

What changed in each version of mef3io, and what an upgrade asks of you.
Newest first.

| Version | Released | Main changes |
|---|---|---|
| [Unreleased](unreleased.md) | — | opening a session releases the GIL |
| [1.3.0](1.3.0.md) | 2026-10-16 | `inspect()`, cancelling and progress, encrypted data blocks from other writers decoded correctly, safer `extract_session(overwrite=True)` |
| [1.2.0](1.2.0.md) | 2026-10-15 | lenient reading (`strict=False`), records at every level, provenance stamp, faster snapshot cache, recording-time-offset fixes |
| [1.1.4](1.1.4.md) | 2026-09-21 | Windows: `durability="full"` appends and non-ASCII paths |
| [1.1.3](1.1.3.md) | 2026-09-21 | validator, repair and recovery; appends that cost only the new data; buffer-size declarations fixed |
| [1.1.2](1.1.2.md) | 2026-08-05 | overlapping blocks decoded correctly in parallel; padded metadata files accepted |
| [1.1.1](1.1.1.md) | 2026-07-07 | single-file `.mefd.tar` sessions |
| [1.1.0](1.1.0.md) | 2026-07-04 | subject and acquisition metadata |
| [1.0.1](1.0.1.md) | 2026-07-04 | documentation only |
| [1.0.0](1.0.0.md) | 2026-07-04 | one MATLAB bundle for all platforms |
| [0.3.0](0.3.0.md) | 2026-07-03 | first release on PyPI |
| [0.2.0](0.2.0.md) | 2026-07-03 | first feature set (not published to PyPI) |
