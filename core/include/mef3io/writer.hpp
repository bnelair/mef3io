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
};

// Write the three files for one segment into `segment_dir` (which must exist).
// Blocks must be time-ordered. Returns the number of samples written.
// Blocks are RED-encoded in parallel (n_threads: 0 -> hardware concurrency,
// 1 -> serial) then assembled into the .tdat in order, so the output is
// byte-identical regardless of thread count.
si8 write_time_series_segment(const std::string& segment_dir, const SegmentSpec& spec,
                              const std::vector<BlockSpec>& blocks, int n_threads = 0);

// Append blocks to an EXISTING segment (in-segment append): extends the .tdat
// and .tidx in place and rewrites the .tmet statistics plus the universal
// headers' end times / entry counts / CRCs. The segment's fs and conversion
// factor are authoritative: a mismatch with `spec`, or a first block starting
// before the segment's stored end time, throws WriteConflictError. Encrypted
// segments need a password granting at least level-1 access (to re-encrypt
// section 2; section 3 bytes are preserved verbatim). File UUIDs and password
// validation fields are preserved. Returns the number of samples appended.
si8 append_time_series_segment(const std::string& segment_dir, const SegmentSpec& spec,
                               const std::vector<BlockSpec>& blocks, int n_threads = 0);

}  // namespace mef3io
