// mef3io — the ONE definition of MEF's on-disk time convention.
//
// meflib marks a time whose recording-time offset (RTO) has been applied by
// storing it NEGATED: stored = -(absolute - rto) (apply_recording_time_offset,
// meflib.c:744-759). A non-negative stored value means the offset was NOT
// applied and the value is already absolute (remove_recording_time_offset,
// meflib.c:7660-7675). UUTC_NO_ENTRY is a sentinel, never a time.
//
// This rule is the single most error-prone one in the format (for_agents/notes/format-gotchas.md:
// "Times are stored NEGATED on disk", "Sign conventions are MIXED WITHIN ONE
// FILE"), so it lives here once and every reader/writer/validator uses it. Do
// not re-derive it locally.
#pragma once

#include <string>

#include "mef3io/errors.hpp"
#include "mef3io/types.hpp"

namespace mef3io {

/// The offset meflib actually uses for an `rto` field: NO_ENTRY means "no
/// offset", i.e. plain negation (meflib.c:753 / 7669).
constexpr si8 effective_rto(si8 rto) noexcept { return rto == fmt::UUTC_NO_ENTRY ? 0 : rto; }

/// a + b, saturated instead of overflowing (signed overflow is UB, and these
/// inputs come straight from file bytes — a record body can hold anything).
/// The low end stops at INT64_MIN + 1 so a result never turns into the
/// NO_ENTRY sentinel.
constexpr si8 saturating_add(si8 a, si8 b) noexcept {
  constexpr si8 MAX = INT64_MAX;
  constexpr si8 MIN = fmt::UUTC_NO_ENTRY + 1;
  if (b > 0 && a > MAX - b) return MAX;
  if (b < 0 && a < MIN - b) return MIN;
  const si8 r = a + b;
  return r < MIN ? MIN : r;
}

/// On-disk time -> absolute uUTC. NO_ENTRY passes through; a non-negative
/// value is already absolute; a negative one has the offset removed.
/// Saturates rather than overflowing on nonsense input.
constexpr si8 to_user_time(si8 stored, si8 rto) noexcept {
  if (stored == fmt::UUTC_NO_ENTRY) return stored;
  if (stored >= 0) return stored;
  return saturating_add(-stored, effective_rto(rto));  // -stored fits: stored > INT64_MIN
}

/// Absolute uUTC -> on-disk time, meflib-style (negated, offset removed).
/// Exact inverse of to_user_time for every absolute time > rto. Saturates
/// rather than overflowing on nonsense input.
constexpr si8 to_disk_time(si8 absolute, si8 rto) noexcept {
  if (absolute == fmt::UUTC_NO_ENTRY) return absolute;
  return saturating_add(effective_rto(rto), -absolute);  // -absolute fits likewise
}

/// Whether mef3io can WRITE `absolute` under `rto` (see to_stored_time).
///
/// With a non-zero offset only times AFTER it can be stored: for t <= rto the
/// offset-applied form rto - t is non-negative, every reader takes a
/// non-negative time as absolute (meflib.c:7665), and the only way to store
/// it correctly would be the real date in plain — which the offset exists to
/// hide (meflib's anonymization). Such a write is refused, never degraded.
constexpr bool time_is_storable(si8 absolute, si8 rto) noexcept {
  const si8 r = effective_rto(rto);
  return absolute == fmt::UUTC_NO_ENTRY || r == 0 || absolute > r;
}

/// How mef3io WRITES a time — data blocks, index, universal headers and
/// records alike: meflib's offset-applied form, -(t - rto).
///   * rto == 0 (every session mef3io creates): -t, byte-identical to every
///     earlier release.
///   * rto != 0 (a foreign session that used meflib's anonymization): rto - t,
///     which keeps the real date hidden and reads back right in every reader.
///     Requires time_is_storable(); write paths check it and refuse first.
constexpr si8 to_stored_time(si8 absolute, si8 rto) noexcept {
  return to_disk_time(absolute, rto);
}

/// Throw WriteConflictError, in plain words, unless time_is_storable().
/// `what` names the time ("record time", "block start", ...).
inline void require_storable_time(si8 absolute, si8 rto, const std::string& what) {
  if (time_is_storable(absolute, rto)) return;
  throw WriteConflictError(
      "cannot write " + what + " " + std::to_string(absolute) +
      " (uUTC): it is at or before this session's anonymization offset (recording_time_offset "
      "= " + std::to_string(effective_rto(rto)) +
      "). MEF can only store such a time as the real date in plain, which the offset exists "
      "to hide, so the write is refused.");
}

}  // namespace mef3io
