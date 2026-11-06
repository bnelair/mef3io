# Unreleased

## Faster checks and copies of a short window of a long session

`merge.check`, `transform.check` and the copies themselves no longer make
thousands of small requests when only a short window of a large session is
kept (many channels and segments, e.g. on a network drive). A small block
index is now read in one request instead of about fifteen, a channel's
segments outside the window are looked at only when nothing inside it holds
samples, and each small metadata file is read once per operation. Every source
file is opened at most once per check and per copy. Results and refusals are
unchanged; the copy is byte-for-byte what it was.
