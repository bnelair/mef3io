---
description: "How mef3io uses threads: one n_threads count per Reader, MultiReader or Writer covers opening, file access, decoding, decrypting and encoding; 0 = automatic, 1 = no extra threads; changeable at runtime."
---

# Threads

mef3io has **one thread rule**, the same in Python, C++, the C ABI and
MATLAB:

> An object's `n_threads` is how many threads **that object** may use for
> **everything it does**: opening, file access, decoding, decrypting and
> encoding. `0` (the default) = automatic. `1` = no extra threads.

Results never depend on the count. A read returns the same samples, an open
the same channels, metadata, problems and errors, and a write the same bytes,
whether it ran on 1 thread or 32. The test suites check this.

```python
import mef3io

r = mef3io.Reader("session.mefd")             # automatic
r = mef3io.Reader("session.mefd", n_threads=4)
r.n_threads = 1                               # from the next call on
w = mef3io.Writer("out.mefd", n_threads=2)
```

## What "automatic" means

`0` is resolved in one place in the core (`resolve_threads` in
`parallel.hpp`; Python can ask it through `mef3io._mef3io.resolve_threads`):

- **CPU work** (decoding, decrypting, encoding): one thread per hardware
  thread.
- **File access** (the open's pass over the metadata files): the same, but
  **at least 8**. On a network share each request is a round trip of several
  milliseconds whatever the number of cores, so more requests in flight pay
  off even on a small machine.

A positive count is used as given for both kinds of work. A **negative**
count is an error everywhere: `ValueError` in Python, `std::invalid_argument`
in C++, `MEF3IO_ERR_ARGUMENT` from the C ABI, and a validation error in
MATLAB.

`1` really means no extra thread: the open, reads and writes then run on the
calling thread only, and so does a `MultiReader` with `n_threads=1`.

If the system refuses to start a thread (a process or container limit), the
work continues on the threads already running, and at worst on the calling
thread alone. Results stay the same; nothing aborts.

## What uses the threads

| Object | Uses `n_threads` for |
|---|---|
| `Reader` | the open: listing the channel folders, checking each segment's files, reading and parsing every `.tmet`. Reads: decoding (and decrypting) the blocks of the window. A multi-channel read (`read([...])`, `block_envelope([...])`): the channels' file reads at once (file access: automatic = at least 8), then ONE decode pool over every block of every channel, never one pool per channel |
| `MultiReader` | the same, as a **total** shared by its recordings (below) |
| `Writer` | encoding the RED blocks of each write |

Opening a session makes several requests per segment. On a local disk that is
cheap. On a network share it is most of the open time, and spreading the
requests over threads is what speeds it up. Measured with an artificial delay
per request, the open gets faster almost linearly with the thread count
([benchmarks](benchmarks.md#opening-over-a-slow-file-system)).

## Changing it later

`n_threads` is a settable property on `Reader`, `MultiReader` and `Writer`
(C++ `set_threads`; C ABI `mef3io_reader_set_threads` /
`mef3io_writer_set_threads`; MATLAB `setThreads`). A change takes effect on
the **next call**. Calls already running keep the count they started with,
and setting it while other threads read is safe. The open always uses the
value given to the constructor.

There is **no per-call thread argument**. `Reader.read(..., n_threads=)` and
`read_raw(..., n_threads=)` were removed in 2.0. Set the object's count
instead.

## MultiReader: a total, never multiplied

A `MultiReader` holds a Reader per recording. Its `n_threads` is the total for
all of them, never a count per recording:

- **Open:** recordings are opened at once, and the budget is split between
  them (for example, 16 threads for 4 recordings = 4 opens at a time with 4
  threads each).
- **Read:** a read that touches several recordings reads them at once, and
  each gets its share of the budget. A read inside one recording gets all of
  it.
- **Setting `n_threads`** re-splits from the next call.

- **Multi-channel read** (`read([...])`): the channels are read one after
  the other, each as above, so the budget still holds.

So `MultiReader(paths, n_threads=8)` never runs more than 8 threads, however
many recordings it combines.

## Sharing an object between threads

Reads, `toc`, `segments`, `block_envelope` and `records` on one `Reader` or
`MultiReader` may run from several threads at once. The bindings release the
GIL while the C++ works, opening included. A `Reader` opened with `cache=`
builds its backend once, whichever thread asks first. `close()` waits for a
backend still being built and then stays closed. A skipped record file is
warned about once per Reader, not once per call. A `Writer` is not meant to
be shared between threads.

The thread count and a sharing pattern multiply. Eight Python threads reading
one `Reader` with `n_threads=8` can run 64 decode threads at once. To cap the
total, give the Reader a smaller count, or use one `MultiReader`, which shares
its budget.
