# Unreleased

Changes merged since the last release. Rewritten into the next version's
notes at release time. The next release is **2.0.0**; it has breaking changes
(below).

## Breaking: one thread count per object (migration)

- **The per-call `n_threads` argument of `Reader.read` and `Reader.read_raw`
  is removed** (also on `MultiReader`), with no deprecation period. Set the
  object's count instead, which is now a settable property:

  ```python
  # before
  x = r.read("ch1", t0, t1, n_threads=1)
  # now
  r.n_threads = 1          # from the next call on
  x = r.read("ch1", t0, t1)
  ```

  `cancel=` and `progress=` are keyword-only on the C++ binding's
  `read`/`read_raw` (the Python `Reader` already required that).
- **`n_threads` must be an int >= 0** (`0` = automatic). A negative value now
  raises `ValueError`, and a bool or float raises `TypeError`. Before, a
  negative value silently meant "all cores".
- **`MultiReader(n_threads=)` is a total**, shared by its recordings and never
  multiplied by them: the open splits it between the recordings it opens at
  once, a read between the recordings it touches, and a read inside one
  recording gets all of it. Before, every recording was opened with the full
  count, so a MultiReader of 10 recordings could run 10 times its budget.
  Setting `MultiReader.n_threads` re-splits from the next call.
- C++: `Reader::read` / `read_raw` take `(channel, t0, t1, ctl)`, and
  `Reader::kUseDefaultThreads` is gone. `Session`'s constructors take an
  optional `n_threads` after `strict`.

See [Threads](../threads.md) for the rule.

## New

- **One thread rule, everywhere.** An object's `n_threads` is how many threads
  it may use for everything it does: opening, file access, decoding,
  decrypting, encoding. `0` = automatic, `1` = no extra threads. "Automatic"
  is decided in one place in the core: hardware concurrency for CPU work, at
  least 8 for file access. It is changeable at runtime as `Reader.n_threads`,
  `MultiReader.n_threads` and `Writer.n_threads` (Python), `set_threads` (C++,
  now atomic), `mef3io_reader_set_threads` / `mef3io_reader_threads` /
  `mef3io_writer_threads` (C ABI; `mef3io_writer_set_threads` existed), and
  `setThreads` / `threads` on the MATLAB `Reader` and `Writer`. A change
  applies from the next call; calls in flight keep their count.
- **Sharing a Reader is safe in every case now** (#110). A `Reader` opened
  with `cache=` builds its backend once, whichever thread asks first (before,
  racing threads each built one). `close()` racing that first build stays
  closed (before, the session could silently reopen). A skipped record file
  or one opened through the old password mapping is warned about once per
  Reader, not once per concurrent call. C++ `set_threads` and
  `set_index_cache_bytes` are atomic, and the metadata loaded at open is read
  without a lock afterwards.

## Faster

- **Opening a session is parallel** (#141). The channel listings, the
  segment file checks and every metadata file's read and parse run on the
  Reader's threads. With 20 ms per request (a slow network share, simulated),
  209 channels x 10 segments opens in 10.9 s on 16 threads instead of 173 s on
  one, and 64 x 1 in 0.46 s instead of 6.5 s
  ([benchmarks](../benchmarks.md#the-open-on-several-threads)). The result
  does not depend on the thread count. The channels, metadata, the lenient
  `problems()` (in the same order) and the strict error (the first in listing
  order, as a serial open meets it) are all the same.
- **Windowed reads skip segments outside the window** before reading their
  block index (#110). A one-second read of a 2,400-segment channel went from
  3.9 ms to 0.24 ms. The last segment of a channel is never skipped by its end
  time, which an interrupted append can leave stale. `block_envelope` uses
  the same rule.

- **Opening a session reads each segment's metadata file with one bounded
  read** of its fixed record size. There is no longer a separate seek to find
  the file's size, and padding that other writers append after the record is
  not read. Locally this removes three system calls per segment. On a network
  file system that re-checks file attributes, it saves one request per
  segment: about 6% on a simulated slow mount. Results and messages are
  unchanged. Part of #141.
- **Opening a `.mefd.tar` archive no longer scans the whole archive for every
  folder it lists** (the open lists the root and each channel folder once).
  Folder listings are now indexed once when the archive is opened.
- C++: `Session` and `Reader` can be opened from a `SessionSource`, and
  `SessionSource::read_head(path, n)` reads the first `n` bytes of a file in
  one request.
