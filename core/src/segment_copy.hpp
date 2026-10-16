// Internal (not installed): the streaming block-copy engine.
//
// Copies one segment's `.tidx` + `.tdat` from a SessionSource to two fresh
// files, block by block, never holding more than a bounded batch of blocks in
// memory (CLAUDE.md: "NEVER READ OR COPY A .tdat WHOLE"). Each block is handed
// to a caller hook that may edit its bytes in place (a header field, the
// statistics encryption) or drop it; the engine then
//
//   * re-seals the block CRC (bytes 4..block_bytes, meflib.c:7045),
//   * packs the kept blocks contiguously and rewrites each index entry's
//     file_offset to where its block now lies,
//   * extends the rolling body CRCs of both files over just the new bytes
//     (the Koopman CRC has no final inversion: crc(A||B) == crc(B, crc(A))),
//   * fills in the universal headers' entry counts and maximum entry sizes,
//   * and returns the section-2 quantities of what it wrote (block and sample
//     counts, the maxima, the contiguous-run trio), so a caller that drops
//     blocks can write declarations that match.
//
// anonymize_session (#70) keeps every block and only patches its time. A
// later cut/merge (#103) selects blocks through the same hook — which is why
// the engine is generic rather than anonymize-specific.
//
// Integrity first: a block whose stored CRC does not verify, whose index
// entry disagrees with its header about its size, or that lies outside the
// data file is REFUSED (FormatError/CrcError) — re-sealing it would launder
// corruption into a copy that then verifies clean.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>

#include "mef3io/control.hpp"
#include "mef3io/headers.hpp"
#include "mef3io/source.hpp"
#include "mef3io/types.hpp"

namespace mef3io::detail {

/// One block as the hooks see it.
struct CopyBlock {
  std::size_t index = 0;          ///< position in the SOURCE index
  std::span<ui1> entry;           ///< the source index entry, 56 bytes, editable
  std::span<ui1> block;           ///< header + payload, block_bytes long, editable
};

/// What the engine wrote, in section-2 terms.
struct CopyStats {
  si8 blocks = 0;
  si8 samples = 0;
  ui4 max_block_bytes = 0;
  ui4 max_block_samples = 0;
  ui4 max_difference_bytes = 0;
  si8 discontinuities = 0;
  si8 max_contiguous_blocks = 0;
  si8 max_contiguous_samples = 0;
  si8 max_contiguous_block_bytes = 0;
  std::uint64_t source_bytes = 0;  ///< bytes read (.tidx + .tdat)
  std::uint64_t output_bytes = 0;  ///< bytes written (.tidx + .tdat)
};

struct SegmentCopySpec {
  const SessionSource* source = nullptr;
  std::string tidx_rel, tdat_rel;     ///< source-relative paths
  std::string tidx_out, tdat_out;     ///< output paths (created; must not exist)
  /// Edit the block in place. Return false to drop it. The CRC is re-sealed
  /// after the hook; `entry` is written after the hook with its file_offset
  /// (bytes 0..8) set to the block's new position.
  std::function<bool(CopyBlock&)> on_block;
  /// Patch a 1024-byte universal header before its CRCs are filled.
  /// `is_tdat` tells the two files apart. Counts and maximum sizes are set by
  /// the engine afterwards.
  std::function<void(std::span<ui1> uh, bool is_tdat)> on_header;
  /// Renumber every kept block's channel-wide start_sample (.tidx +16)
  /// contiguously from this value. Unset: entries keep their own numbers
  /// until a block is dropped; from then on each kept block continues from
  /// the previous kept one (start + samples), so the numbering stays dense.
  std::optional<si8> start_sample;
  const Control* control = nullptr;
  /// Called with the number of source bytes consumed since the last call.
  std::function<void(std::uint64_t)> on_progress;
};

/// Dropping blocks: the next KEPT block after a dropped one is flagged as a
/// discontinuity (RED header flag and index flag, inside the re-sealed CRC),
/// since the samples between them are gone; the contiguous-run statistics are
/// computed after that, so they describe what was written.
///
/// Copy one segment's index and data. Throws on any integrity failure, on I/O
/// failure and (via `control`) on cancellation; the caller removes the
/// partial output.
CopyStats copy_segment(const SegmentCopySpec& spec);

}  // namespace mef3io::detail
