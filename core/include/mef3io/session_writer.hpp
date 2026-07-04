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
#include "mef3io/writer.hpp"  // SessionMetadata, SegmentSpec

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

/// High-level MEF 3.0 writer: float64 quantization with precision inference,
/// the int32+conversion-factor primitive path, NaN = discontinuity splitting,
/// block layout, in-segment append, and records. Times are uUTC.
class SessionWriter {
 public:
  /// Open a session for writing.
  /// @param mefd_path   `.mefd` directory to create or extend.
  /// @param overwrite   true removes any existing session first; false reopens
  ///                    it for appending (state recovered from disk).
  /// @param password_1  level-1 password (empty = no encryption).
  /// @param password_2  level-2 password (required with password_1 to encrypt).
  SessionWriter(const std::string& mefd_path, bool overwrite = false, std::string password_1 = "",
                std::string password_2 = "");

  /// Primitive path: int32 counts stored verbatim with a conversion factor
  /// (e.g. amplifier V/bit).
  /// @param channel                 channel name (created on first write).
  /// @param samples                 int32 counts, stored bit-exact.
  /// @param units_conversion_factor counts -> physical units.
  /// @param start_uutc              timestamp of the first sample.
  /// @param sampling_frequency      Hz.
  /// @param valid                   optional mask (same length); 0 marks a gap.
  /// @param new_segment             force a new segment instead of appending.
  /// @throws WriteConflictError on an in-segment append whose fs/factor differ
  ///         or that starts before the segment's end.
  WriteSummary write_int32(const std::string& channel, std::span<const si4> samples,
                           sf8 units_conversion_factor, si8 start_uutc, sf8 sampling_frequency,
                           std::span<const ui1> valid = {}, bool new_segment = false);

  /// Convenience path: float64 data. NaN marks discontinuities. @p precision
  /// sets the conversion factor to `10^-precision`; if < 0 it is inferred —
  /// except on in-segment append, where the segment's factor is reused so the
  /// append cannot conflict. Data is quantized as `round(data * 10^precision)`.
  /// Segment semantics match write_int32().
  WriteSummary write_float(const std::string& channel, std::span<const sf8> data, si8 start_uutc,
                           sf8 sampling_frequency, int precision = -1, bool new_segment = false);

  /// Write records (annotations). @p channel nullopt -> session-level (in the
  /// `.mefd` root), otherwise channel-level. Replaces the records at that level.
  void write_records(std::optional<std::string> channel, const std::vector<Record>& records);

  // Optional override for the RED block length (samples per block). <= 0 uses
  // the fs-derived heuristic.
  void set_block_length(si8 n) { block_length_override_ = n; }
  void set_units(const std::string& u) { units_ = u; }
  void set_threads(int n) { n_threads_ = n; }

  // Session-wide subject/descriptive metadata written into every channel's
  // section 2 (descriptive/acquisition) and section 3 (subject) on write.
  // Set before writing; ignored for channels already on disk.
  void set_metadata(const SessionMetadata& m) { metadata_ = m; }
  const SessionMetadata& metadata() const { return metadata_; }

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
  SessionMetadata metadata_;
  si8 block_length_override_ = 0;
  int n_threads_ = 0;
  std::map<std::string, ChannelState> channels_;
};

// Quantization helpers (exposed for testing / reuse).
int infer_precision(std::span<const sf8> data);
std::vector<si4> quantize_to_int32(std::span<const sf8> data, int precision);

}  // namespace mef3io
