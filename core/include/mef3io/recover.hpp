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
// Both are reachable after a power cut during an append: the writer flushes
// only the `.tmet` before its rename, never the `.tdat`/`.tidx` it extends in
// place (2.0 removed the `durability` option). This function is what makes
// that trade safe; only the last append of a segment can be affected.
#pragma once

#include <string>
#include <vector>

#include "mef3io/control.hpp"
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
  /// Informational, never an error: a channel's last segment was stamped
  /// "segment-repaired" (naming the file), or could not be (foreign region,
  /// bad CRC, a writer's temp file present).
  std::vector<std::string> notes;

  /// True when every segment is self-consistent and nothing needs doing.
  bool nothing_to_do() const { return segments.empty() && skipped.empty(); }
};

/// Make every segment's index and data agree.
///
/// @param path      `.mefd` session directory. Tar archives are refused.
/// @param apply     false (default) reports what it WOULD do and writes nothing.
/// @param backup    copy each file to `<session>.recover-backup/` before writing.
/// @param password  needed only to read encrypted metadata (the offset and
///                  rate, at a non-zero recording-time offset).
///
/// @param ctl       optional hooks. Recovery SCANS every segment first and
///                  writes only afterwards. `ctl.cancel` is honoured
///                  throughout the scan (per segment, and per block examined
///                  past the end of the index); a cancelled call throws
///                  Cancelled having written NOTHING — no file, no backup. The
///                  write phase that follows is not interruptible (whole-file
///                  renames and 1024-byte header patches): a cancel requested
///                  during it is ignored and the call returns its report.
///                  `ctl.progress` reports bytes of .tidx + .tdat covered.
///
/// Declarations are NOT updated here — run `repair_session` afterwards, which
/// is what the CLI does. Keeping the two apart means the dangerous operation
/// stays small and the safe one stays reusable. ONE exception (#83): at a
/// NON-ZERO recording-time offset, where every validator time check stands
/// down, recovery corrects the segment's END TIME in the three universal
/// headers (the .tmet one is what readers report) from the last block it
/// kept: an end counts as stale only before that block's start (+ half a
/// sample), or, with rebuilt blocks, before their end (- half a sample); it
/// is never moved earlier, and only an end stored in the offset-applied
/// (negated) form is rewritten (in that form), else a note. The .tmet header
/// is backed up and stamped Recovery; a stale end alone makes a channel's
/// last segment need recovery there.
RecoveryReport recover_session(const std::string& path, bool apply = false, bool backup = true,
                               const std::string& password = "", const Control& ctl = {});

}  // namespace mef3io
