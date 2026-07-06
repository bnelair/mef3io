# C++ and C ABI guide

The core (`core/`) is a self-contained C++17/20 static library — no external
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
// gaps = NaN, values scaled by the conversion factor.
std::vector<double> x = r.read("ch1", t0, t1);

// Or the raw form: int32 counts + validity mask (0 in gaps).
mef3io::RawData raw = r.read_raw("ch1", t0, t1);

auto segs = r.segments("ch1");    // per-segment map: what data is where
auto toc  = r.toc("ch1");         // per-RED-block index entries
auto recs = r.records("ch1");     // annotations (std::nullopt -> session level)
```

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

Appends are validated against the on-disk segment (fs, conversion factor,
start after the segment end) and throw `WriteConflictError` on violation.

## Errors

Everything throws from the `mef3io::MefError` hierarchy (`errors.hpp`):
`IoError`, `FormatError`, `CrcError`, `PasswordError`, `WriteConflictError`.

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
| `source.hpp` | `SessionSource` abstraction: directory vs tar-backed sessions |
| `tar.hpp` | uncompressed tar session archives: `archive_session`, `TarSource` |
| `reader.hpp` | gridded reads, NaN fill, scaling, parallel decode |
| `writer.hpp` | low-level segment write + in-segment append |
| `session_writer.hpp` | precision inference, quantization, NaN splitting, segments, records |
| `records.hpp` | record (.rdat/.ridx) read + write |
| `parallel.hpp` | deterministic `parallel_for` |
| `version.hpp` | `mef3io::version()` |
| `c_api.h` | the flat C ABI (below) |

## The flat C ABI

`core/include/mef3io/c_api.h` + `core/src/c_api.cpp` expose the full
reader/writer surface as `extern "C"` functions for FFI consumers (the
MATLAB MEX is built on it). Conventions:

- every fallible call returns `MEF3IO_OK` (0) or an error code
  (`MEF3IO_ERR_IO/FORMAT/CRC/PASSWORD/CONFLICT/ARGUMENT`); the message is in
  `mef3io_last_error()` (thread-local);
- handles are opaque (`mef3io_reader*`, `mef3io_writer*`); close functions
  accept NULL;
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
