# C++ API reference

The full C++ core reference is generated with Doxygen from the public headers
(`core/include/mef3io`), including the flat C ABI (`c_api.h`).

[Browse the full C++ / C ABI reference →](cpp/html/index.html){ .md-button .md-button--primary }

For a task-oriented walkthrough — consuming the library with CMake, reading,
writing, the error hierarchy, and the C ABI — see the [C++ guide](../cpp.md).

The key entry points:

- **`mef3io::Reader`** — high-level windowed reads, segment map, TOC, records.
- **`mef3io::SessionWriter`** — high-level writes, in-segment append, records.
- **`mef3io::Session`** — the lower-level session tree and indexed reads.
- **`c_api.h`** — the flat `extern "C"` ABI the MATLAB MEX (and any FFI) builds on.
