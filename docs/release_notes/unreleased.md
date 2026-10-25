# Unreleased

Changes merged since the last release. Rewritten into the next version's
notes at release time.

## Faster

- **Opening a session makes fewer file-system requests**, which matters on a
  network share, where each request is a round trip. A segment's three
  existence checks are replaced by one listing of its folder, and its
  metadata file is read with one bounded read of the fixed record size, no
  separate size check. Padding that other writers append after the record
  is no longer read. Requests per open drop by about half (209 channels x
  10 segments: 8,570 to 4,390). Results, messages and `problems()` are
  unchanged. Part of #141; making these requests in parallel comes next.
- C++: `Session` and `Reader` can be opened from a `SessionSource`, and
  `SessionSource::read_head(path, n)` reads the first `n` bytes of a file in
  one request.
