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

/// On-disk time -> absolute uUTC. NO_ENTRY passes through; a non-negative
/// value is already absolute; a negative one has the offset removed.
constexpr si8 to_user_time(si8 stored, si8 rto) noexcept {
  if (stored == fmt::UUTC_NO_ENTRY) return stored;
  if (stored >= 0) return stored;
  return -stored + effective_rto(rto);
}

/// Absolute uUTC -> on-disk time, meflib-style (negated, offset removed).
/// Exact inverse of to_user_time for every absolute time > rto.
constexpr si8 to_disk_time(si8 absolute, si8 rto) noexcept {
  if (absolute == fmt::UUTC_NO_ENTRY) return absolute;
  return effective_rto(rto) - absolute;
}

}  // namespace mef3io
