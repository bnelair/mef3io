// mef3io — session validation and targeted repair.
//
// A MEF 3.0 session declares, in metadata section 2 and the universal headers,
// a number of quantities that are really re-derivable from the data itself:
// how many blocks there are, how many samples, how large a buffer a reader
// must allocate. Readers trust those declarations — meflib-based ones allocate
// from them before decoding anything — so a wrong declaration is a defect in
// the file even though every sample on disk is intact.
//
// This module checks those declarations against the data and, when asked,
// rewrites them. Two rules shape the API:
//
//   * Nothing is ever repaired implicitly. `validate_session` only reads.
//     `repair_session` takes an explicit, non-empty list of check ids and
//     touches nothing else.
//   * Every call returns a full report. A repair still runs every check, so
//     the caller sees the whole picture and not just the part they fixed.
//
// Adding a check later means adding one entry to the registry in validate.cpp;
// checks run in registry order, so later checks may assume earlier ones have
// already reported (never that they have repaired).
#pragma once

#include <string>
#include <vector>

#include "mef3io/headers.hpp"
#include "mef3io/types.hpp"

namespace mef3io {

/// Names of the allocation-relevant section-2 declarations this section 2
/// leaves unset — 0 or NO_ENTRY where a reader expects a size.
///
/// Pure: it reads nothing from disk, only an already-parsed section 2. That is
/// what makes it cheap enough to run on every segment at open time, where a
/// full validate_session (which walks the block index) would not be. It sees
/// only what is *missing*, never what is merely wrong; use validate_session
/// for that.
std::vector<std::string> unset_declarations(const fmt::TimeSeriesMetadataSection2& s2);

/// How badly a finding breaks a reader.
enum class Severity {
  Info = 0,     ///< worth knowing; no reader is harmed
  Warning = 1,  ///< wasteful or wrong, but readers cope
  Error = 2,    ///< can truncate a buffer, mislead, or crash a reader
};

/// "info" / "warning" / "error".
std::string severity_name(Severity s);

/// One check in the registry.
struct CheckInfo {
  std::string id;           ///< stable dotted id, e.g. "sizing.difference-bytes"
  std::string title;        ///< one line, for a report header
  std::string description;  ///< what it compares and why it matters
  Severity severity = Severity::Warning;
  bool repairable = false;  ///< can the truth be written back from the data?
};

/// Every check, in the order they run. Stable across releases: ids are API.
const std::vector<CheckInfo>& checks();

/// Look up one check by id; nullptr when unknown.
const CheckInfo* find_check(const std::string& id);

/// One problem, found by one check, in one segment.
struct Finding {
  std::string check_id;
  Severity severity = Severity::Warning;
  std::string channel;
  int segment_number = 0;
  std::string path;     ///< human-readable segment location
  std::string field;    ///< the field at fault, when the check names one
  std::string stored;   ///< what the file declares
  std::string expected; ///< what the data says it should be
  std::string message;  ///< one-line explanation
  bool repairable = false;
  bool repaired = false;  ///< set by repair_session when it wrote the fix
};

/// A segment that could not be checked, and why (bad CRC, missing password,
/// unreadable file). Never silently dropped.
struct SkippedSegment {
  std::string channel;
  int segment_number = 0;
  std::string path;
  std::string reason;
};

struct ValidateOptions {
  std::string password;                ///< needed for encrypted sessions
  std::vector<std::string> channels;   ///< empty = every channel
  std::vector<int> segments;           ///< empty = every segment
  std::vector<std::string> check_ids;  ///< empty = run every check
  /// Read every RED block header to learn the real maximum_difference_bytes.
  /// It is the one declaration not derivable from the index, so the exact
  /// answer costs one small read per block. With `false`, the check instead
  /// bounds it by meflib's RED_MAX_DIFFERENCE_BYTES (5 bytes/sample) and only
  /// reports a value that is clearly unset — no .tdat reads at all.
  bool exact_difference_bytes = true;
};

/// Which repairs to apply. `check_ids` is required and must be non-empty:
/// there is deliberately no "repair everything" shorthand, so a caller cannot
/// fix something it never looked at.
struct RepairSelection {
  std::vector<std::string> check_ids;
  std::vector<std::string> channels;  ///< empty = every channel
  std::vector<int> segments;          ///< empty = every segment
  /// Copy each file before rewriting it, into `<session>.repair-backup/`
  /// (outside the session tree, so no reader mistakes a backup for data).
  bool backup = true;
};

struct Report {
  std::vector<Finding> findings;
  std::vector<SkippedSegment> skipped;
  si8 segments_checked = 0;
  si8 segments_repaired = 0;
  std::vector<std::string> checks_run;      ///< ids, in the order they ran
  std::vector<std::string> checks_repaired; ///< ids the caller selected

  /// True when nothing worse than a warning is left outstanding and nothing
  /// was skipped. Findings this pass repaired no longer count against it.
  bool ok() const;
  si8 count(Severity s) const;
  /// Distinct check ids that reported something a repair could fix. Pass these
  /// to RepairSelection to opt in deliberately.
  std::vector<std::string> repairable_check_ids() const;
};

/// Check a session. Reads only — never writes, never throws because a segment
/// is damaged (those land in `Report::skipped`). Accepts a .mefd directory or
/// a .mefd.tar archive.
Report validate_session(const std::string& path, const ValidateOptions& opts = {});

/// Validate, then write back the selected repairs. Returns the full report,
/// with `repaired` set on the findings that were actually written.
///
/// Only declarations are rewritten: metadata section 2 and the universal
/// headers of .tmet/.tidx/.tdat. Sample data and index entries are never
/// touched. A segment whose CRCs do not verify is reported and left alone —
/// if the bytes cannot be trusted, neither can anything derived from them.
/// Tar sessions are rejected (an archive cannot be rewritten in place).
///
/// Throws std::invalid_argument when the selection is empty or names an
/// unknown/unrepairable check.
Report repair_session(const std::string& path, const RepairSelection& selection,
                      const ValidateOptions& opts = {});

}  // namespace mef3io
