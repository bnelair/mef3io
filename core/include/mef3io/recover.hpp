// mef3io — crash recovery: make a segment's block index and its data agree
// again.
//
// This is NOT the validator's repair. `repair_session` only ever rewrites
// DECLARATIONS — section 2 and the universal headers — and never touches the
// index or the samples, which is what makes it safe to run on anything.
// Recovery is the other half: it may TRUNCATE the index, REBUILD index entries
// from the data file, and DROP an unusable trailing fragment. It exists for one
// situation — a write interrupted part-way — and it is deliberately separate,
// dry-run by default, and backed up before it writes.
//
// The two shapes an interrupted append leaves behind:
//
//   INDEX AHEAD OF DATA — entries reference `.tdat` bytes that never landed.
//   The samples for those blocks do not exist; the entries are dropped.
//
//   DATA AHEAD OF INDEX — blocks reached `.tdat` but the index was not
//   extended. The samples DO exist, so they are recovered: the RED block
//   headers carry the sample count, byte count, start time and discontinuity
//   flag, which is everything an index entry needs.
//
// With `durability="full"` (the default) an append cannot leave either state:
// the `.tdat` is flushed before the `.tidx` that references it. They are
// reachable with `durability="fast"`, which is what makes that trade a
// reasonable one to offer.
#pragma once

#include <string>
#include <vector>

#include "mef3io/types.hpp"

namespace mef3io {

/// What recovery found, and did or would do, for one segment.
struct RecoveredSegment {
  std::string channel;
  int segment_number = 0;
  std::string path;          ///< human-readable segment location
  si8 blocks_before = 0;     ///< index entries found
  si8 blocks_after = 0;      ///< index entries after recovery
  si8 blocks_recovered = 0;  ///< rebuilt from unreferenced .tdat blocks
  si8 blocks_dropped = 0;    ///< entries pointing past the end of .tdat
  si8 tdat_bytes_dropped = 0;///< trailing bytes that are not a whole block
  std::string action;        ///< what happened, in words
};

/// Result of a recovery pass. `applied` is false for a dry run.
struct RecoveryReport {
  std::vector<RecoveredSegment> segments;   ///< only segments needing work
  std::vector<std::string> skipped;         ///< segment + reason
  si8 segments_examined = 0;
  bool applied = false;
  std::string backup_root;                  ///< empty when nothing was backed up

  /// True when every segment is self-consistent and nothing needs doing.
  bool nothing_to_do() const { return segments.empty() && skipped.empty(); }
};

/// Make every segment's index and data agree.
///
/// @param path      `.mefd` session directory. Tar archives are refused.
/// @param apply     false (default) reports what it WOULD do and writes nothing.
/// @param backup    copy each file to `<session>.recover-backup/` before writing.
/// @param password  needed only to read encrypted metadata.
///
/// Declarations are NOT updated here — run `repair_session` afterwards, which
/// is what the CLI does. Keeping the two apart means the dangerous operation
/// stays small and the safe one stays reusable.
RecoveryReport recover_session(const std::string& path, bool apply = false, bool backup = true,
                               const std::string& password = "");

}  // namespace mef3io
