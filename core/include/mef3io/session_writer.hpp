// mef3io — high-level session writer. Owns the user-facing write semantics:
// float64 quantization with precision inference, the int32+ufact primitive
// path, NaN = discontinuity splitting, block layout, and segment management.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "mef3io/records.hpp"
#include "mef3io/types.hpp"

namespace mef3io {

// Summary returned by each write, so callers (and MATLAB/Python) can tell what
// happened without inspecting the tree — notably distinguishing an all-NaN
// no-op from a real write.
struct WriteSummary {
  si8 samples_written = 0;
  si8 blocks = 0;
  si8 gaps_skipped = 0;
  int segment = -1;
};

class SessionWriter {
 public:
  // overwrite=true removes any existing session at the path first.
  SessionWriter(const std::string& mefd_path, bool overwrite = false, std::string password_1 = "",
                std::string password_2 = "");

  // Primitive path: int32 counts + a conversion factor (e.g. amplifier V/bit).
  // Samples are stored verbatim. Optional `valid` mask (same length) marks
  // discontinuity gaps (0 = gap). The first write to a channel creates segment
  // 0; later writes append IN-SEGMENT to the channel's last segment unless
  // new_segment=true forces a fresh segment. In-segment appends must match the
  // segment's fs and conversion factor and start at/after its end time
  // (WriteConflictError otherwise).
  WriteSummary write_int32(const std::string& channel, std::span<const si4> samples,
                           sf8 units_conversion_factor, si8 start_uutc, sf8 sampling_frequency,
                           std::span<const ui1> valid = {}, bool new_segment = false);

  // Convenience path: float64 data. NaN = discontinuity. `precision` sets the
  // conversion factor as 10^-precision; if < 0 it is inferred — except when
  // appending in-segment, where the segment's existing conversion factor is
  // reused so the append cannot conflict. Data is quantized to int32 as
  // round(data * 10^precision). Same segment semantics as write_int32.
  WriteSummary write_float(const std::string& channel, std::span<const sf8> data, si8 start_uutc,
                           sf8 sampling_frequency, int precision = -1, bool new_segment = false);

  // Write records (annotations). channel == nullopt -> session-level records
  // (in the .mefd root); otherwise the given channel's records. Replaces any
  // existing records file at that level.
  void write_records(std::optional<std::string> channel, const std::vector<Record>& records);

  // Optional override for the RED block length (samples per block). <= 0 uses
  // the fs-derived heuristic.
  void set_block_length(si8 n) { block_length_override_ = n; }
  void set_units(const std::string& u) { units_ = u; }
  void set_threads(int n) { n_threads_ = n; }

 private:
  struct ChannelState {
    sf8 sampling_frequency = 0.0;
    sf8 units_conversion_factor = 1.0;
    int n_segments = 0;
    si8 total_samples = 0;   // channel-wide sample count so far
    si8 last_end_uutc = 0;   // for append-time validation
    bool hydrated = true;    // false for channels adopted from disk until their
                             // last segment's metadata has been read back
  };

  si8 block_length_for(sf8 fs) const;
  // Load fs/ufact/total_samples/last_end_uutc from the channel's last segment
  // on disk (for sessions reopened with overwrite=false). No-op once hydrated.
  void hydrate(const std::string& channel, ChannelState& st);
  WriteSummary write_blocks(const std::string& channel, const std::vector<si4>& samples,
                            std::span<const ui1> valid, sf8 ufact, si8 start_uutc, sf8 fs,
                            bool new_segment);

  std::string mefd_path_;
  std::string password_1_;
  std::string password_2_;
  std::string session_name_;
  std::string units_ = "uV";
  si8 block_length_override_ = 0;
  int n_threads_ = 0;
  std::map<std::string, ChannelState> channels_;
};

// Quantization helpers (exposed for testing / reuse).
int infer_precision(std::span<const sf8> data);
std::vector<si4> quantize_to_int32(std::span<const sf8> data, int precision);

}  // namespace mef3io
