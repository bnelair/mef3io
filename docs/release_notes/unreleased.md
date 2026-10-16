# Unreleased

*Changes merged since the last release. They ship with the next version.*

## Fixes

- **Opening a session no longer freezes other Python threads.** `Reader(...)`
  (and the low-level `Session` and `SessionWriter` constructors, which the
  `Writer` uses to reopen a session) now scan the session and parse its
  metadata with the GIL released, as reads and writes already did. Before,
  every other thread, including a GUI's event loop, stopped for the whole
  open: about 0.6 s on 64 channels x 60 segments, seconds on a long
  recording. Opening on a worker thread now leaves the rest of the program
  responsive, and several sessions can be opened at once
  ([#79](https://github.com/bnelair/mef3io/issues/79)). `toc`, `segments`,
  `records` and `Writer.write_annotations` release it too.
- **Reading one session from several threads at once no longer crashes.**
  Sharing one `Reader` across threads (a data loader, a viewer) could free a
  block index that another thread was still reading when the index cache
  evicted it, and the process aborted. Each call now holds its own reference
  to the index it reads, and the reader's shared state is locked.
- **A data race in decrypting encrypted metadata** when two threads did it for
  the first time at once (the AES inverse S-box was filled lazily) is gone:
  the table is now fixed at compile time.
