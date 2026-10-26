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
  /// Validator check ids whose declarations (.tmet section 2, universal
  /// headers) disagreed with the index: in a dry run, the ones found (for a
  /// segment whose .tmet is newer or older than its index); after `apply`,
  /// the ones re-derived from the recovered index (via repair_session).
  std::vector<std::string> declarations;
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
/// Whatever is changed or dropped is ALWAYS copied to `<session>.recover-backup/`
/// first (there is no switch to turn that off; mef3io never discards data).
///
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
/// INDEX WITH A BAD CRC: its header and entries came from different writes
/// (a power cut between an append's in-place extension and its header patch).
/// It is not refused: entries past the prefix the stored body CRC covers stay
/// only if the RED block header at their offset agrees, and from the first
/// that does not the index is rebuilt from the CRC-checked blocks in .tdat.
/// TORN LAST APPEND: in each channel's last segment the blocks of the last
/// append (back to its discontinuity-flagged first block, at most 64 MiB)
/// are CRC-checked against their entries, so entries pointing at pages that
/// never landed are dropped like entries past the end of the data.
///
/// DECLARATIONS (2.0): with `apply`, every segment recovery changed, and every
/// channel's last segment whose .tmet declares other block/sample/entry counts
/// than the index holds (a .tmet published before or after its data — a power
/// cut can leave either), gets its declarations re-derived from the index
/// THROUGH `repair_session` (all repairable findings of that segment, exact
/// difference bytes; repair's backups and provenance). Recovery alone thus
/// leaves a valid session; `RecoveredSegment::declarations` lists what was
/// re-derived, and a segment the validator could not check is a note. The
/// DRY RUN reports such a .tmet as a segment needing recovery and lists the
/// exact check ids. Only reported segments are touched. LOST .tmet: a
/// non-first segment's empty/short .tmet (data and index landed) is rebuilt
/// from the previous segment's .tmet plus this segment's own files; a last
/// segment with short files and no block in its data is removed (backed up).
/// Index and data remain recovery's alone. ONE further exception (#83): at a
/// NON-ZERO recording-time offset, where every validator time check stands
/// down, recovery corrects the segment's END TIME in the three universal
/// headers (the .tmet one is what readers report) from the last block it
/// kept: an end counts as stale only before that block's start (+ half a
/// sample), or, with rebuilt blocks, before their end (- half a sample); it
/// is never moved earlier, and only an end stored in the offset-applied
/// (negated) form is rewritten (in that form), else a note. The .tmet header
/// is backed up and stamped Recovery; a stale end alone makes a channel's
/// last segment need recovery there.
RecoveryReport recover_session(const std::string& path, bool apply = false,
                               const std::string& password = "", const Control& ctl = {});

}  // namespace mef3io
