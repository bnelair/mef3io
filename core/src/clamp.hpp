// mef3io — internal: clamping a float write's count into what MEF stores.
//
// Maintainer decision (#149): a sample whose count would leave int32 at the
// conversion factor, or that is +-inf, is stored as the nearest storable
// count and counted; the write succeeds and the caller reports how many.
// The bounds are meflib's own RED_POSITIVE_INFINITY (0x7FFFFFFF) and
// RED_NEGATIVE_INFINITY (0x80000001), meflib.h:1060-1061 — exactly what
// meflib's RED_round clamps to (meflib.c:7322). INT32_MIN itself is RED_NAN
// (meflib.h:1059) and is never written as a sample.
#pragma once

#include <limits>

#include "mef3io/session_writer.hpp"

namespace mef3io {

inline constexpr sf8 kMaxStorableCount = static_cast<sf8>(std::numeric_limits<si4>::max());

namespace detail {
/// The storable count nearest `rounded` (|rounded| > kMaxStorableCount or
/// infinite), counted in `c` as finite or infinite by `finite_input`.
inline si4 clamp_count(sf8 rounded, bool finite_input, ClampCount& c) {
  if (finite_input)
    ++c.finite;
  else
    ++c.infinite;
  return rounded > 0 ? std::numeric_limits<si4>::max() : -std::numeric_limits<si4>::max();
}
}  // namespace detail
}  // namespace mef3io
