# Threads, GIL release, parallel open

> Read before adding a long binding call, a constructor, shared mutable state in Session/Reader, or touching discover_layout / the open path.
> Split out of CLAUDE.md verbatim; "the private reader" = the private third-party reader named in CLAUDE.md.

- **EVERY potentially long binding call RELEASES THE GIL** (#79). Opening is
  where all the metadata parsing happens (64 ch x 60 seg: a 616 ms open froze
  every other Python thread for 613 ms). Reads/writes and the session-level
  functions run through `PyHooks::run`; `toc`, `segments`, `records`,
  `write_records`, `Session.read_runs` and `infer_precision` convert their
  arguments, then wrap the core call in `nb::gil_scoped_release`; the
  `Reader`, `Session` and `SessionWriter` CONSTRUCTORS are bound as
  `__init__` lambdas that call `construct_nogil` (placement-new under
  `nb::gil_scoped_release`), NOT `nb::init<...>`, which holds the GIL.
  Building the result (dicts, lists) happens after, with the GIL. Rule while
  released: touch no Python object (arguments are already C++ values; `self`
  is invisible to other threads until __init__ returns); a callback
  re-acquires the GIL (`PyHooks::call`). Cheap accessors (`info`,
  `channels`, `problems`) keep the GIL. CONSEQUENCE: calls on ONE
  Reader/Session can now really overlap, so its mutable state must be locked
  (see (f) in large-files-and-append.md) and core globals must be immutable (the AES inverse S-box
  is `constexpr` now; it was lazily filled behind an unsynchronised flag, a
  TSan-confirmed race). A new long call or constructor must release the GIL
  too, and add a case to `tests/test_p31_gil_release.py`, which measures the
  main thread's longest stall (best of 3; skips a call under 40 ms) while a
  worker runs the call on a 2400-segment channel.
- **A LIBRARY CALL IS NOT A ROUND TRIP — measure the open at the OS level
  (#141).** On an SMB share each request is ~110 ms and the open is serial
  (209 channels: 11-24 s). Round 1 of the fix replaced the three exists() per
  segment in `discover_layout` with one listing of each `.segd`: that HALVED
  the SessionSource call count and was REVERTED, because a listing is
  opendir + getdents (x2) + fcntl + close (+ an lstat per entry where d_type
  is missing) while exists() is one stat — strace showed ~10% MORE syscalls,
  a local open 10-20% slower, a 2 ms/request FUSE mount only -12% with about
  the same kernel-request count, and per-segment TarSource::list_dir made
  tar opens quadratic. DO NOT RETRY IT. What stayed: (a) the `.tmet` is read
  with `SessionSource::read_head(rel, METADATA_FILE_BYTES)` = first
  min(size, n) bytes, one open + one read, no seek-to-end (strace, 209 ch x
  10 seg: 20,454 -> 14,184 syscalls, all 6,270 lseeks gone; FUSE without
  attribute caching: one getattr less per segment, ~-6%; with caching:
  identical). A short file comes back short, so the loader's
  ">= METADATA_FILE_BYTES" check fails with the same message; padding past
  the record is never read (rchar-pinned in test_p17). (b) TarSource builds a
  dir -> children index once (`children_`), so list_dir is O(log + children),
  not a scan of the whole archive per call. (c) `CountingSource`
  (core/tests/counting_source.hpp: counts list_dir/exists/file_size/
  read_all/read_head/read_range, optional fixed delay per call) +
  `Session`/`Reader(shared_ptr<const SessionSource>)`; test_open_calls.cpp
  pins the open at `1 + channels` listings, `3 x segments` exists and
  `segments` read_head, zero read_all/file_size/read_range (dir and tar),
  and TIMES the open at 100 vs 3200 segments (per-segment cost flat; tar
  within 2x of dir) because a count pin cannot see a call that got more
  expensive. The Python `Reader.__init__` makes no per-segment call
  (test_p17, with a positive control). The real gain for #141 is step 2: the
  metadata pass in parallel with n_threads (latency hiding), then a
  multi-channel read.
- **The open is PARALLEL but must behave SERIALLY** (#141). `discover_layout`
  (session_layout.hpp; inspect calls it with 1 thread) lists channel folders,
  then checks every segment's files, then the records-only `.rdat`, each pass
  on the object's threads (`Work::Io`); `Session::discover` then reads +
  parses every `.tmet` the same way and builds the ChannelInfos SERIALLY,
  rethrowing each segment's stored load error inside `with_segment` in
  segment order. Every pass uses `parallel_try_each` and assembles in listing
  order, so strict raises the failure a serial walk meets first (never the
  first in time) and lenient `problems()` keeps the serial order. Request
  counts are identical at any thread count (the #142 pins hold). Sources must
  be safe for concurrent calls: DirectorySource opens a stream per call,
  TarSource's tables are read-only after its ctor (its member index scan is
  serial — each header gives the next offset — and is now most of a parallel
  tar open). The "open scales linearly" timing pin runs with 1 thread on
  purpose. Measured (CountingSource, 20 ms/request): 209 ch x 10 seg 173 s ->
  10.9 s on 16 threads (docs/benchmarks.md). The `shared_ptr<const
  SessionSource>` constructors of Session/Reader are INTERNAL (tests): they
  skip the naming rule, so applications use the path constructors.
