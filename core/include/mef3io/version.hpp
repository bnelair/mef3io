// mef3io — library version, baked from the repo-root VERSION file at build
// time (the single source of truth shared by the C++ core, the Python wheel,
// and, later, the MATLAB MEX).
#pragma once

namespace mef3io {

inline const char* version() {
#ifdef MEF3IO_VERSION_STRING
  return MEF3IO_VERSION_STRING;
#else
  return "0.0.0";
#endif
}

}  // namespace mef3io
