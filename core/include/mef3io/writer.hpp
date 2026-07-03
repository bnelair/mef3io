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
};

// Write the three files for one segment into `segment_dir` (which must exist).
// Blocks must be time-ordered. Returns the number of samples written.
// Blocks are RED-encoded in parallel (n_threads: 0 -> hardware concurrency,
// 1 -> serial) then assembled into the .tdat in order, so the output is
// byte-identical regardless of thread count.
si8 write_time_series_segment(const std::string& segment_dir, const SegmentSpec& spec,
                              const std::vector<BlockSpec>& blocks, int n_threads = 0);

}  // namespace mef3io
