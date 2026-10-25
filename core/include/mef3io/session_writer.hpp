// mef3io — high-level session writer. Owns the user-facing write semantics:
// float64 quantization with precision inference, the int32+ufact primitive
// path, NaN = discontinuity splitting, block layout, and segment management.
#pragma once

#include <cstdint>
#include <filesystem>
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
/// How a write turned its input into stored int32 counts.
enum class Quantization {
  Counts,   ///< write_int32: the caller's counts, stored as given.
  Decimal,  ///< float data rounded to 10^-precision (the default path).
  Step,     ///< float data that is a whole multiple of the factor: stored exactly.
  Factor,   ///< float data rounded to a channel's existing non-decimal factor
            ///< (a precision-step write the data did not fit; see write_float).
};

struct WriteSummary {
  si8 samples_written = 0;
  si8 blocks = 0;
  si8 gaps_skipped = 0;
  int segment = -1;
  /// The conversion factor the written counts carry (counts * factor = value).
  sf8 units_conversion_factor = 0.0;
  Quantization quantization = Quantization::Counts;
  /// Precision::step() only: empty when the data was stored on its step,
  /// else why it was not (the write then took the fallback path).
  std::string step_fallback;
};

/// The `precision` of a float write: a number of decimals, inferred, or the
/// data's own quantization step (#81).
struct Precision {
  /// >= 0: conversion factor 10^-digits; < 0: inferred (the default).
  int digits = -1;
  /// Detect the data's quantization step and store whole counts of it.
  bool step = false;
  /// Implicit, so every `write_float(..., 3)` call keeps compiling.
  constexpr Precision(int d = -1) noexcept : digits(d) {}
  /// Opt-in step detection (Python `precision="step"`); see write_float().
  static constexpr Precision detect_step() noexcept {
    Precision p;
    p.step = true;
    return p;
  }
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
  ///
  /// `Precision::detect_step()` (opt-in, #81) stores the data as whole counts
  /// of its own quantization step instead (e.g. an ADC's 0.195 uV), which
  /// RED compresses far better. The step is estimated from a bounded sample
  /// (detect_step) and EVERY value is verified while it is converted
  /// (quantize_to_step); nothing is stored off its value by more than float64
  /// rounding. On a NEW channel, data that is not a whole multiple of one
  /// step (filtered, resampled) or whose counts overflow int32 falls back to
  /// the default (inferred power-of-ten) path for that write. On a channel
  /// that already has segments — an append, or a new segment — the factor is
  /// the channel's: the reader scales a whole channel by one factor, so it is
  /// never changed. Data that is a whole multiple of it is stored exactly;
  /// otherwise the write falls back to rounding to it (the default path when
  /// it is a power of ten, else `round(data / factor)`, refused with
  /// std::invalid_argument before anything is written if a count leaves
  /// int32). The summary says which happened (`quantization`,
  /// `step_fallback`).
  WriteSummary write_float(const std::string& channel, std::span<const sf8> data, si8 start_uutc,
                           sf8 sampling_frequency, Precision precision = {},
                           bool new_segment = false);

  /// Write records (annotations). @p channel nullopt -> session-level (in the
  /// `.mefd` root), otherwise channel-level. Replaces the records at that level.
  /// Times are stored the way the reader resolves the level: -t at offset 0
  /// (every session mef3io creates; byte-identical to earlier releases) and
  /// rto - t under an anonymization offset, which keeps the real date hidden.
  /// Refused (WriteConflictError) when the level's segments disagree on the
  /// offset or a time is at or before it, and (PasswordError) when the
  /// offset is locked in a level-2 section 3.
  void write_records(std::optional<std::string> channel, const std::vector<Record>& records);

  /// Segment metadata this writer could not use ("path: reason"): `.tmet`
  /// files that could not be read while working out a recording-time offset,
  /// and empty segment directories it ignored on open. In a session at offset
  /// 0 an unreadable one is skipped so it does not block unrelated writes;
  /// where the offset is non-zero or nothing else is readable, the write is
  /// refused instead. Listed here either way so the damage is not silent.
  /// Grows over the writer's life; deduplicated.
  const std::vector<std::string>& unreadable_metadata() const { return unreadable_metadata_; }

  /// Segments (`.tmet` paths) this writer appended to whose password matched
  /// only through the byte mapping mef3io used up to 1.2.0 (issue #84). Their
  /// existing keys are kept (re-keying would mean re-encrypting the segment),
  /// so they stay unopenable by meflib/pymef with that password; NEW segments
  /// always use meflib's mapping. Grows over the writer's life; deduplicated.
  const std::vector<std::string>& legacy_password_segments() const {
    return legacy_password_segments_;
  }

  // Optional override for the RED block length (samples per block). <= 0 uses
  // the fs-derived heuristic. More than MAX_BLOCK_SAMPLES (writer.hpp) throws
  // std::invalid_argument: the block's 32-bit size fields would wrap.
  void set_block_length(si8 n) {
    check_block_length(n);
    block_length_override_ = n;
  }
  void set_units(const std::string& u) { units_ = u; }
  void set_threads(int n) { n_threads_ = n; }

  /// Flush appends to stable storage. **Default false.**
  ///
  /// These files are built by appending for days to months, so the append is
  /// the hot path and the barriers cost ~2.5x on it. The default therefore
  /// favours throughput, matching what every MEF writer before this one did —
  /// meflib performs no flushing at all.
  ///
  /// What that costs on an unclean shutdown: the ORDER in which the `.tdat`,
  /// `.tidx` and `.tmet` updates reach the disk is no longer enforced, so a
  /// crash can leave the index referencing data that never landed, or blocks
  /// present in `.tdat` that the index never got to mention, or a torn tail on
  /// either. Every one of those is DETECTED by `validate()` and REPAIRED by
  /// `recover_session()`, which rebuilds missing index entries from the RED
  /// block headers — so the failure mode is "run recover after an unclean
  /// shutdown", not "lose the recording".
  ///
  /// Set true for the strict contract: each append becomes all-or-nothing
  /// across the three files, at the cost of ~3 flushes per channel per append.
  void set_durable(bool d) { durable_ = d; }
  bool durable() const { return durable_; }

  // Session-wide subject/descriptive metadata written into every channel's
  // section 2 (descriptive/acquisition) and section 3 (subject) on write.
  // Set before writing; ignored for channels already on disk.
  void set_metadata(const SessionMetadata& m) { metadata_ = m; }
  const SessionMetadata& metadata() const { return metadata_; }

 private:
  struct ChannelState {
    sf8 sampling_frequency = 0.0;
    sf8 units_conversion_factor = 1.0;
    int n_segments = 0;      // real segments (with metadata) on disk
    int last_segment = -1;   // highest real segment NUMBER (-1: none)
    // Folder stem of that segment as it is ON DISK ("ch1-000005", or a
    // foreign writer's un-padded "ch1-5"), so an append extends the real
    // folder. Empty: not adopted from disk; the canonical name is used.
    std::string last_segment_stem;
    si8 total_samples = 0;   // channel-wide sample count so far
    si8 last_end_uutc = 0;   // for append-time validation
    bool hydrated = true;    // false for channels adopted from disk until their
                             // last segment's metadata has been read back
    // Exact maximum difference_bytes over the current segment's blocks, valid
    // only while every one of them was encoded by THIS writer. An append
    // cannot rediscover the old blocks' values without a seek per block, so
    // carrying the running maximum here keeps a chunked write exact; a segment
    // adopted from disk leaves the flag false and the append falls back to
    // meflib's worst-case bound instead. Reset whenever a new segment starts.
    ui4 max_difference_bytes = 0;
    bool difference_bytes_exact = false;
    // Summary of the channel's current .tidx, so an append does not re-walk it.
    // The first append after adopting a segment populates it; every later one
    // reuses it, which is what keeps a months-long recording's appends O(new
    // data) instead of O(session length). Reset whenever a new segment starts.
    AppendIndexCache index_cache;
    // Offset for this channel's next NEW segment, once worked out
    // (new_segment_rto); unset until the channel first needs one.
    std::optional<si8> new_segment_rto;
  };

  si8 block_length_for(sf8 fs) const;
  // The folder stem of `seg` for `st`: the on-disk one for the adopted last
  // segment, else the canonical zero-padded name.
  std::string segment_stem(const std::string& channel, const ChannelState& st, int seg) const;
  // Load fs/ufact/total_samples/last_end_uutc from the channel's last segment
  // on disk (for sessions reopened with overwrite=false). No-op once hydrated.
  void check_still_writable() const;
  void hydrate(const std::string& channel, ChannelState& st);
  // What the on-disk segments of a level (one channel, or the whole session)
  // say about the recording-time offset (see session_writer.cpp).
  struct LevelOffset {
    enum class State { Agreed, Disputed, Unknown } state = State::Agreed;
    si8 rto = 0;         // the agreed offset (0 when nothing readable)
    int readable = 0;    // segments whose metadata could be read
    int unreadable = 0;  // segments whose .tmet could not be read at all
    std::string detail;  // which segments disagree / which section 3 is locked
    std::string unreadable_example;
    std::string where;   // a segment carrying the agreed offset
  };
  LevelOffset scan_channel_offset(const std::filesystem::path& timd);
  // Memoized per channel; the session combines the channels' answers.
  LevelOffset level_offset(const std::optional<std::string>& channel);
  void note_new_segment(const std::string& channel, const std::string& tmet, si8 rto);
  std::map<std::string, LevelOffset> channel_offsets_;
  // The offset a write at this level stores times against, or a refusal
  // (see session_writer.cpp for the rule). Channel scope falls back to the
  // session when the channel has no segment.
  si8 resolve_offset(const std::optional<std::string>& channel, const std::string& what);
  // The offset a NEW segment of `channel` is written with.
  si8 new_segment_rto(const std::string& channel, ChannelState& st);
  void note_unreadable(const std::string& what);
  std::optional<si8> session_rto_;  // the session's resolved offset, once known
  std::vector<std::string> unreadable_metadata_;
  std::vector<std::string> legacy_password_segments_;
  std::map<std::string, std::string> empty_segment_dirs_;  // dir -> its report entry
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
  bool durable_ = false;
  std::map<std::string, ChannelState> channels_;
};

// Quantization helpers (exposed for testing / reuse).
int infer_precision(std::span<const sf8> data);
std::vector<si4> quantize_to_int32(std::span<const sf8> data, int precision);

/// The estimate of detect_step(): the step, or why there is none.
struct StepEstimate {
  std::optional<sf8> step;
  /// Why there is no step. Empty with no step: the sample holds nothing but
  /// 0 and NaN, which any factor stores exactly (no step needed).
  std::string reason;
};
/// Estimate the data's quantization step (#81) from a BOUNDED sample — the
/// first 65,536 values and 16 windows of 2,048 spread through the rest; NaN
/// ignored — so its cost does not grow with the data (only a sample of
/// nothing but 0 and NaN scans on, for the first non-zero values). The value is snapped
/// to the step's own float64 value where the sample allows (the shortest
/// decimal reproducing every sampled value exactly, e.g. 0.195). Only an
/// estimate: quantize_to_step() decides.
StepEstimate detect_step(std::span<const sf8> data);
/// Convert @p data to counts of @p step in one pass, verifying every value:
/// false (and @p why) as soon as one finite value is not a whole multiple
/// within 64 float64 roundings (`|x - double(k) * step| <= 64 eps |x|`) or
/// its count leaves int32 (INT32_MIN excluded: RED's NaN). NaN -> count 0,
/// valid 0. A negative @p step (a polarity-inverting factor) is allowed; 0
/// or non-finite returns false. The outputs are unspecified after a false.
bool quantize_to_step(std::span<const sf8> data, sf8 step, std::vector<si4>& counts,
                      std::vector<ui1>& valid, std::string* why = nullptr);

}  // namespace mef3io
