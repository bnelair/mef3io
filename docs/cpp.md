---
description: "C++ and C ABI guide to mef3io, a MEF 3.0 (Multiscale Electrophysiology Format) read/write library."
---

# C++ and C ABI guide

The core (`core/`) is a self-contained C++20 static library — no external
dependencies (SHA-256, AES-128, and CRC-32 are built in). Both the Python
extension and the MATLAB MEX are thin layers over it, and it can be used
directly.

## Consuming with CMake

```cmake
add_subdirectory(mef3io/core)              # or FetchContent on the repo
target_link_libraries(your_app PRIVATE mef3io_core)
```

The target exports its include directory; the library version (from the
repo-root `VERSION` file) is available as `mef3io::version()`
(`mef3io/version.hpp`).

## Reading

```cpp
#include "mef3io/reader.hpp"

mef3io::Reader r("session.mefd", /*password=*/"", /*n_threads=*/0);

for (const auto& ch : r.channels()) { ... }
const mef3io::ChannelInfo& ci = r.info("ch1");     // fs, ufact, times, counts,
                                                   // section-3 subject metadata

// Float64 on the fs grid over [t0, t1) (defaults: whole channel);
// gaps = NaN, each value scaled by its own SEGMENT's conversion factor
// (segments may store different ones; FormatError for a factor of 0 /
// NaN / inf, which cannot scale).
std::vector<double> x = r.read("ch1", t0, t1);

// Or the raw form: int32 counts + validity mask (0 in gaps), and
// raw.factor_spans: which factor applies to which samples. ci.units_conversion_factor
// and raw.units_conversion_factor are std::optional: nullopt when the
// segments (in the window) store different factors; segments() has each one's.
mef3io::RawData raw = r.read_raw("ch1", t0, t1);

auto segs = r.segments("ch1");    // per-segment map: what data is where
auto toc  = r.toc("ch1");         // per-RED-block index entries
auto env  = r.block_envelope("ch1", t0, t1);  // per-block min/max, index only
auto recs = r.records("ch1");     // ch1's channel- + segment-level annotations
// r.records() = session level; r.records("ch1", 0) = segment 0 only;
// r.records(std::nullopt, std::nullopt, mef3io::Session::RecordLevels::All) = all.
// mef3io::record_fields(rec) lists a record's body fields in a stable order.
```

**Threads.** Reads, `toc`, `segments` and `records` on one `Reader` (and
the `Session` under it) may run from several threads at once. Each call
holds its own reference to the block index it reads, and the shared mutable
state is locked (`core/tests/test_concurrency.cpp`). Configure
`set_threads` and `set_index_cache_bytes` are atomic and may be called while
other threads read; a change applies from the next call. The metadata
parsed at open is immutable afterwards, so calls read it without a lock.
A `SessionWriter` is not for sharing.

The `n_threads` given to `Reader` (or `Session`) is how many threads the
object may use for everything: the open (channel listings, segment file
checks, every `.tmet` read and parse) and decoding. `0` = automatic
(`mef3io::resolve_threads` in `parallel.hpp`: hardware concurrency for CPU
work, at least 8 for file access), `1` = no extra threads. The open's result,
its strict-mode error and `problems()` are the same at every count. There is
no per-call count: `read`/`read_raw` take `(channel, t0, t1, ctl)`. See
[Threads](threads.md).

`Reader` wraps `mef3io::Session` (`session.hpp`), which exposes the lower
level: lazy `.mefd/.timd/.segd` traversal, `read_runs` (decoded contiguous
runs), `collect_blocks` (undecoded block gathering for custom parallel
pipelines).

The session path may also be an **uncompressed tar archive** of a session
(`name.mefd.tar`, from `mef3io::archive_session` in `tar.hpp`) — read in
place, without extraction; windowed reads fetch member byte ranges directly.
All file access goes through the `SessionSource` interface (`source.hpp`:
`DirectorySource` / `TarSource`), so both layouts behave identically:

```cpp
#include "mef3io/tar.hpp"

std::string tar = mef3io::archive_session("session.mefd");  // session.mefd.tar
mef3io::Reader rt(tar);                                     // same API
std::string dir = mef3io::extract_session(tar);             // session.mefd back
```

Tar sessions are read-only — `SessionWriter` throws `IoError` on `.tar`
paths; `extract_session` restores a writable directory (exact inverse:
archive → extract → archive is byte-identical). Archive creation is
deterministic (sorted members, zeroed mtimes) and plain ustar, so `tar -xf`
restores the directory exactly as well.

Session naming is enforced by the core at every entry point (so all bindings
behave identically): directories must end `.mefd`, archives `.mefd.tar`;
`Session`/`Reader`, `SessionWriter`, `archive_session` and `extract_session`
all throw `IoError` on anything else.

## Writing

```cpp
#include "mef3io/session_writer.hpp"

mef3io::SessionWriter w("session.mefd", /*overwrite=*/true,
                        /*password_1=*/"", /*password_2=*/"");
w.set_units("uV");

// Float path: NaN = discontinuity gap; precision < 0 -> inferred
// (reused on append). Non-first writes append IN-SEGMENT;
// new_segment=true forces a fresh segment.
mef3io::WriteSummary s = w.write_float("ch1", data, start_uutc, 256.0);

// Primitive path: int32 counts stored verbatim + conversion factor;
// optional validity mask marks gaps.
w.write_int32("ch2", counts, /*ufact=*/0.01, start_uutc, 256.0, valid_mask);

// Records (annotations); std::nullopt -> session level.
w.write_records("ch1", {{.type = "Note", .time = t, .text = "marker"}});
```

A `SessionWriter` is non-durable by default (no flushes, like the Python
`durability="fast"`); call `w.set_durable(true)` for the flush barriers
described in [Long recordings](long_recordings.md#the-durability-knob).

Appends are validated against the on-disk segment (fs, conversion factor,
start after the segment end) and throw `WriteConflictError` on violation.

## De-identified copies

`mef3io::anonymize_session` (`anonymize.hpp`) writes a de-identified copy of
a session: identity removed, samples copied byte for byte, every time kept
(the default) or moved by `shift_us` when it is set, verified against the
source before it is published. What it
does, and every refusal, is described in
[De-identification](anonymization.md).

```cpp
#include "mef3io/anonymize.hpp"

mef3io::AnonymizeOptions opt;
opt.shift_us = shift;              // std::optional: leave unset to keep every time
opt.exclude_channels = {"ECG"};

mef3io::AnonymizeCheck chk = mef3io::check_anonymize("patient.mefd", "S001.mefd", opt);
for (const auto& r : chk.refusals) std::cerr << r.code << ": " << r.message << "\n";

if (chk.ok) {
  try {
    mef3io::AnonymizeReport rep = mef3io::anonymize_session("patient.mefd", "S001.mefd", opt);
    // rep.verification.ok, rep.verification.notes
  } catch (const mef3io::AnonymizeError& e) {
    std::cerr << e.code() << "\n";          // e.g. "block-crc": only the copy can find it
  }
}
```

`check_anonymize` runs the same pre-flight code as `anonymize_session` and
returns every refusal instead of throwing the first; it reads only
metadata. `verify_anonymized` re-checks a copy at any time. Not in the C
ABI, so not in MATLAB.

## Modified copies

`mef3io::transform_session` (`transform.hpp`) is the same copy engine with
other rules:
- you choose the channels, new passwords (or none), the session name and
  edits to the descriptive metadata;
- every time and every sample is copied as stored;
- the copy is verified before it is published.

See [Modified copies](transform.md).

```cpp
#include "mef3io/transform.hpp"

mef3io::TransformOptions opt;
opt.exclude_channels = {"ECG"};
opt.metadata.subject_id = "S-017";
opt.metadata.channel_descriptions["Fp1"] = "frontal pole, left";
mef3io::TransformReport rep = mef3io::transform_session("patient.mefd", "eeg.mefd", opt);
```

`check_transform` and `verify_transform` mirror their anonymize
counterparts. `merge_sessions(sources, dst, MergeOptions)` (with
`check_merge` / `verify_merge`) merges several sessions on the same engine;
see [Merging recordings](merge.md). The options, refusals and reports are shared:
- `AnonymizeOptions` and `TransformOptions` both extend `CopyOptions`;
- `AnonymizeError` is an alias of `CopyError`.

## Errors

The format and I/O errors come from the `mef3io::MefError` hierarchy
(`errors.hpp`): `IoError`, `FormatError`, `CrcError`, `PasswordError`,
`WriteConflictError`. A bad argument throws `std::invalid_argument`, an
unknown channel or segment `std::out_of_range`. Every refusal of
`anonymize_session` and `transform_session` is also a `mef3io::CopyError`
(alias `AnonymizeError`) carrying a stable `code()`, next to its usual type. `mef3io::Cancelled` (below) is thrown only
when a `CancelToken` you passed is cancelled.

## Cancelling and progress

Reads and the long operations take an optional `const mef3io::Control&` as
their last argument (`control.hpp`):

```cpp
#include "mef3io/control.hpp"

mef3io::CancelToken token;               // cancel() from any thread
mef3io::Control ctl;
ctl.cancel = &token;                     // not owned; must outlive the call
ctl.progress = [](std::uint64_t done, std::uint64_t total) { /* bytes */ };

try {
  auto x = reader.read("ch1", t0, t1, mef3io::Reader::kUseDefaultThreads, ctl);
} catch (const mef3io::Cancelled&) {
  // superseded
}
```

Taking it: `Reader::read` / `read_raw`, `Session::collect_blocks`,
`archive_session`, `extract_session`, `inspect_session`, `validate_session`,
`repair_session`, `recover_session`, `anonymize_session`,
`check_anonymize` and `verify_anonymized`. The guarantees are the ones in the
[Python guide](python.md#cancelling-and-progress) — nothing is left behind,
repair/recover write only after a cancellable read-only pass, progress is
`(0, total)` … monotonic … `(total, total)` once when the cancellable work completes, at a bounded
rate, never concurrently, possibly from a worker thread.

`mef3io::Cancelled` is deliberately **not** a `std::exception`. The core
contains per-segment failures with `catch (const std::exception&)` (a lenient
`Session`, the validator's skipped segments); a cancellation caught there
would be reported as damage and the operation would carry on. Catch it by
name. A progress callback that throws aborts the operation the same way and
its exception propagates.

C ABI: `mef3io_cancel_token_new` / `_cancel` / `_is_cancelled` / `_free`,
a `mef3io_progress_fn(done, total, user_data)` callback, and the `*_ex`
variants `mef3io_reader_read_ex`, `mef3io_reader_read_raw_ex`,
`mef3io_archive_session_ex`, `mef3io_extract_session_ex` and
`mef3io_recover_session_ex` (the existing functions are unchanged). A
cancelled call returns `MEF3IO_ERR_CANCELLED`.

Conversion factors are stored per segment, and a channel's segments may
differ: `mef3io_channel_info.units_conversion_factor` is then NaN,
`mef3io_reader_segment_units_conversion_factor` gives each segment's,
`mef3io_reader_read` scales each sample by its own segment's, and
`mef3io_reader_read_raw_factors` returns the counts with the spans
(`mef3io_factor_span`) saying which factor applies where. Validation, repair and
`inspect_session` and de-identification have no C ABI functions, so MATLAB
(built on the C ABI) has none of them either.


## Module map

| Header | What it is |
|---|---|
| `types.hpp` | meflib-style type aliases + all format constants/offsets |
| `byteio.hpp` | little-endian field IO (no packed-struct casts) |
| `crc.hpp` | CRC-32 Koopman |
| `crypto.hpp` | SHA-256, AES-128-ECB, the two-level password scheme |
| `headers.hpp` | universal header, metadata sections, index entries, RED block header — parse/serialize by explicit offset |
| `metadata.hpp` | `.tmet` loader: CRC → password → decrypt |
| `red.hpp` | RED block decode + encode (lossless) |
| `session.hpp` | session tree, indexed reads, block gathering |
| `inspect.hpp` | `inspect_session`: everything about a session without reading sample data (Python: `mef3io.inspect`) |
| `source.hpp` | `SessionSource` abstraction: directory vs tar-backed sessions |
| `tar.hpp` | uncompressed tar session archives: `archive_session`, `TarSource` |
| `reader.hpp` | gridded reads, NaN fill, scaling, parallel decode |
| `writer.hpp` | low-level segment write + in-segment append |
| `session_writer.hpp` | precision inference, quantization, NaN splitting, segments, records |
| `records.hpp` | record (.rdat/.ridx) read + write |
| `validate.hpp` | validator (check registry) and `repair_session` |
| `recover.hpp` | `recover_session`: rebuild index/data agreement after an interrupted write |
| `anonymize.hpp` | `anonymize_session`, `check_anonymize`, `verify_anonymized` |
| `errors.hpp` | the `MefError` hierarchy |
| `time.hpp` | stored ↔ user time (the recording-time offset and sign conventions) |
| `parallel.hpp` | deterministic `parallel_for` |
| `control.hpp` | `CancelToken`, `Cancelled`, progress callback type, `Control` |
| `version.hpp` | `mef3io::version()` |
| `c_api.h` | the flat C ABI (below) |

## The flat C ABI

`core/include/mef3io/c_api.h` + `core/src/c_api.cpp` expose the full
reader/writer surface as `extern "C"` functions for FFI consumers (the
MATLAB MEX is built on it). Conventions:

- every fallible call returns `MEF3IO_OK` (0) or an error code
  (`MEF3IO_ERR_IO/FORMAT/CRC/PASSWORD/CONFLICT/ARGUMENT/CANCELLED`); the message is in
  `mef3io_last_error()` (thread-local);
- handles are opaque (`mef3io_reader*`, `mef3io_writer*`); close functions
  accept NULL;
- `mef3io_reader_open_ex` adds the `strict` flag (lenient reading;
  `mef3io_reader_n_problems` / `mef3io_reader_problem` list what was
  skipped); `mef3io_password_interop_issue` says why a password will not
  work as expected in other MEF readers;
- `mef3io_reader_open` also accepts a tar session archive;
  `mef3io_archive_session(dir, tar_path_or_NULL, overwrite, out, n)` creates
  one, `mef3io_extract_session` unpacks it back, and `mef3io_writer_open`
  rejects `.tar` paths (`MEF3IO_ERR_IO`);
- reads use a deterministic size query + caller-allocated buffer:

```c
mef3io_reader* r = NULL;
mef3io_reader_open("session.mefd", "", 0, &r);
int64_t n;
mef3io_reader_read_size(r, "ch1", MEF3IO_TIME_UNSET, MEF3IO_TIME_UNSET, &n);
double* buf = malloc(n * sizeof(double));
mef3io_reader_read(r, "ch1", MEF3IO_TIME_UNSET, MEF3IO_TIME_UNSET, buf, n, &n);
mef3io_reader_close(r);
```

The header is self-documenting; the Catch2 suite covers a full round trip
through it.
