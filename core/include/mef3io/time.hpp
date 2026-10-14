// mef3io — the ONE definition of MEF's on-disk time convention.
//
// meflib marks a time whose recording-time offset (RTO) has been applied by
// storing it NEGATED: stored = -(absolute - rto) (apply_recording_time_offset,
// meflib.c:744-759). A non-negative stored value means the offset was NOT
// applied and the value is already absolute (remove_recording_time_offset,
// meflib.c:7660-7675). UUTC_NO_ENTRY is a sentinel, never a time.
//
// This rule is the single most error-prone one in the format (CLAUDE.md:
// "Times are stored NEGATED on disk", "Sign conventions are MIXED WITHIN ONE
// FILE"), so it lives here once and every reader/writer/validator uses it. Do
// not re-derive it locally.
#pragma once

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

/// How mef3io WRITES a time (header, index, block and record times alike).
///
///   * rto == 0 (every session mef3io creates): meflib's marker, -t. Byte-
///     identical to every earlier release.
///   * rto != 0 (only in a foreign session that used meflib's anonymization,
///     which mef3io extends): plain absolute UTC, t. Every reader takes a
///     non-negative stored time as absolute (remove_recording_time_offset,
///     meflib.c:7665), so no offset arithmetic is involved and nothing can go
///     wrong for any t. The negated form `rto - t` would turn NON-NEGATIVE for
///     t <= rto and be read back as a time near 1970.
constexpr si8 to_stored_time(si8 absolute, si8 rto) noexcept {
  if (absolute == fmt::UUTC_NO_ENTRY) return absolute;
  return effective_rto(rto) == 0 ? to_disk_time(absolute, 0) : absolute;
}

}  // namespace mef3io
