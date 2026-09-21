// mef3io — low-level time-series segment writer. Produces byte-valid .tmet /
// .tidx / .tdat that meflib/pymef read. The high-level Writer (P5) builds the
// block layout (discontinuity splitting, scaling) and calls this.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "mef3io/types.hpp"

namespace mef3io {

// One RED block's worth of already-int32 samples with its absolute start time.
struct BlockSpec {
  std::vector<si4> samples;
  si8 start_uutc = 0;      // absolute (offset not yet applied)
  si8 start_sample = 0;    // channel-wide sample index of first sample
  bool discontinuity = false;
};

// User-settable session/subject/acquisition metadata (MEF section 2 & 3
// descriptive fields). All optional with sane defaults; the writer fills the
// computed fields (counts, extrema, durations) itself. Empty string / NO_ENTRY
// sentinels mean "unset". Subject fields live in the level-2-encrypted
// section 3; the descriptive/acquisition fields in the level-1 section 2.
struct SessionMetadata {
  // --- section 2: descriptive / acquisition ---
  std::string session_description;      // free text; defaults to session name
  std::string channel_description;      // free text; defaults to channel name
  std::string reference_description;    // e.g. montage/reference note
  si8 acquisition_channel_number = 1;   // amplifier channel index
  sf8 low_frequency_filter = -1.0;      // Hz; -1 = not recorded
  sf8 high_frequency_filter = -1.0;     // Hz
  sf8 notch_filter = -1.0;              // Hz
  sf8 line_frequency = -1.0;            // Hz (mains, e.g. 50/60)
  // --- section 3: subject / time zone ---
  std::string subject_name_1;
  std::string subject_name_2;
  std::string subject_id;
  std::string recording_location;
  si4 gmt_offset = 0;                   // seconds
};

/// Everything an append derives by walking the whole `.tidx`, carried across
/// calls so it does not have to walk it again.
///
/// A segment's declarations describe all of its blocks, so the append
/// recomputed them from the full index every time — reading, CRC-ing, walking
/// and rewriting the entire file. That is O(total blocks) per append, which is
/// quadratic over a session, and these sessions run for days to months.
///
/// Holding the summary makes an append O(NEW data): entries are appended in
/// place, the body CRC is extended over just the new bytes (the Koopman CRC is
/// a rolling state with no final inversion, exactly as the `.tdat` path has
/// always done), and the totals are folded in. `file_size` is checked against
/// the file before the fast path is taken, so anything that changed the index
/// behind our back falls back to the full walk.
struct AppendIndexCache {
  bool valid = false;         ///< false => walk the index and populate this
  std::size_t file_size = 0;  ///< .tidx size this summary describes
  std::size_t entries = 0;
  si8 total_samples = 0;
  si8 n_discontinuities = 0;
  ui4 max_block_samples = 0;
  si8 max_block_bytes = 0;
  ui4 body_crc = 0;           ///< rolling CRC over every entry in the file
  si8 run_blocks = 0, run_samples = 0, run_bytes = 0;      ///< open run
  si8 max_blocks = 0, max_samples = 0, max_bytes = 0;      ///< longest run seen
};

struct SegmentSpec {
  std::string session_name;
  std::string channel_name;
  int segment_number = 0;
  sf8 sampling_frequency = 0.0;
  sf8 units_conversion_factor = 1.0;
  std::string units_description = "uV";
  si8 recording_time_offset = 0;
  si4 gmt_offset = 0;
  std::string password_1;  // empty -> unencrypted
  std::string password_2;  // empty -> section 3 not L2-encrypted
  SessionMetadata metadata;  // descriptive/subject fields (see above)
  /// Exact maximum `difference_bytes` over the segment's PRE-EXISTING blocks,
  /// or 0 when unknown. Append only; ignored when writing a fresh segment.
  ///
  /// The old blocks' real values live in .tdat block headers, so an append that
  /// had to discover them would pay a seek per block and lose its O(new data)
  /// cost. With 0 it therefore declares meflib's worst case instead — safe, but
  /// 1.0-1.4x the truth. A caller that ENCODED those blocks itself already
  /// knows the exact value and can pass it here to keep the declaration exact;
  /// it must be a true maximum, since a reader allocates its difference buffer
  /// from the result and an under-declaration truncates that buffer.
  ui4 known_difference_bytes = 0;
  /// Flush to stable storage at the points that keep a segment self-consistent.
  ///
  /// true (default) — the `.tdat` body is flushed BEFORE the `.tidx` that
  /// references it, and the `.tidx`/`.tmet` replacements are flushed before the
  /// rename. A power cut then leaves the segment consistent, with the last
  /// append either fully present or fully absent.
  ///
  /// false — no flushes. Writes are still ATOMIC (temp file + rename), so no
  /// file is ever torn and the session is never half-written; what is lost is
  /// the ORDERING guarantee between files, so a crash can leave the index
  /// referencing `.tdat` bytes that never landed. That is detectable
  /// (`index.block-offsets`, `index.data-coverage`) and repairable
  /// (`recover_session`), which is what makes the trade a reasonable one to
  /// offer. It is the durability meflib gives — which is none.
  bool durable = true;
};

// Write the three files for one segment into `segment_dir` (which must exist).
// Blocks must be time-ordered. Returns the number of samples written.
// Blocks are RED-encoded in parallel (n_threads: 0 -> hardware concurrency,
// 1 -> serial) then assembled into the .tdat in order, so the output is
// byte-identical regardless of thread count.
/// `out_max_difference_bytes` (optional) receives the exact maximum
/// `difference_bytes` measured over the blocks written here, for a caller that
/// wants to carry it into a later append as `SegmentSpec::known_difference_bytes`.
si8 write_time_series_segment(const std::string& segment_dir, const SegmentSpec& spec,
                              const std::vector<BlockSpec>& blocks, int n_threads = 0,
                              ui4* out_max_difference_bytes = nullptr);

// Append blocks to an EXISTING segment (in-segment append): extends the .tdat
// and .tidx in place and rewrites the .tmet statistics plus the universal
// headers' end times / entry counts / CRCs. The segment's fs and conversion
// factor are authoritative: a mismatch with `spec`, or a first block starting
// before the segment's stored end time, throws WriteConflictError. Encrypted
// segments need a password granting at least level-1 access (to re-encrypt
// section 2; section 3 bytes are preserved verbatim). File UUIDs and password
// validation fields are preserved. Returns the number of samples appended.
/// `out_max_difference_bytes` (optional) receives the exact maximum
/// `difference_bytes` over the blocks appended here — the NEW blocks only, not
/// the segment's declared maximum.
/// `cache` (optional) carries the index summary across appends. Pass the same
/// object back on every append to the same segment and the index is never
/// walked twice; pass nullptr, or a default-constructed one, for the full walk.
si8 append_time_series_segment(const std::string& segment_dir, const SegmentSpec& spec,
                               const std::vector<BlockSpec>& blocks, int n_threads = 0,
                               ui4* out_max_difference_bytes = nullptr,
                               AppendIndexCache* cache = nullptr);

}  // namespace mef3io
