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
  ([#79](https://github.com/bnelair/mef3io/issues/79)).
