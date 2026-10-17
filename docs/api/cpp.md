# C++ API reference

The full C++ core reference is generated with Doxygen from the public headers
(`core/include/mef3io`), including the flat C ABI (`c_api.h`).

[Browse the full C++ / C ABI reference →](cpp/html/index.html){ .md-button .md-button--primary }

For a task-oriented walkthrough — consuming the library with CMake, reading,
writing, the error hierarchy, and the C ABI — see the [C++ guide](../cpp.md).

The key entry points:

- **`mef3io::Reader`** — high-level windowed reads, segment map, TOC, records.
  Reads on one `Reader` may run from several threads at once (see the
  [C++ guide](../cpp.md#reading) for the limits).
- **`mef3io::SessionWriter`** — high-level writes, in-segment append, records.
- **`mef3io::Session`** — the lower-level session tree and indexed reads.
- **`mef3io::inspect_session`** (`inspect.hpp`) — everything about a session
  without reading its samples. Not in the C ABI.
- **`mef3io::validate_session`** / **`mef3io::repair_session`**
  (`validate.hpp`) and **`mef3io::recover_session`** (`recover.hpp`) —
  checking, repairing and recovering sessions ([Validation](../validation.md)).
- **`mef3io::anonymize_session`** / **`mef3io::check_anonymize`** /
  **`mef3io::verify_anonymized`** (`anonymize.hpp`) — a de-identified copy
  with every time shifted, every check it makes before writing (without
  writing), and its check against the source
  ([De-identification](../anonymization.md)). Refusals are thrown with the
  `mef3io::AnonymizeError` mixin carrying a stable code
  ([Refusal codes](../anonymization.md#refusal-codes)). Not in the C ABI.
- **`c_api.h`** — the flat `extern "C"` ABI the MATLAB MEX (and any FFI) builds on.
