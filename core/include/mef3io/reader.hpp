// mef3io — high-level reader. Wraps Session with the user-facing semantics:
// uUTC windowed reads on a uniform sample grid, NaN-filled discontinuity gaps,
// units scaling, and a block table of contents.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "mef3io/session.hpp"
#include "mef3io/types.hpp"

namespace mef3io {

// Raw windowed read: int32 samples on a uniform grid with a validity mask
// (0 = discontinuity gap, no data). `start_uutc` is the grid origin.
struct RawData {
  std::vector<si4> samples;
  std::vector<ui1> valid;  // 1 where samples[i] is real data, 0 in gaps
  si8 start_uutc = 0;
  sf8 sampling_frequency = 0.0;
  sf8 units_conversion_factor = 1.0;
};

/// High-level MEF 3.0 reader: windowed reads on a uniform sample grid with
/// NaN-filled gaps, units scaling, and parallel RED decoding. Times are uUTC
/// (microseconds since the Unix epoch).
class Reader {
 public:
  /// Open a session.
  /// @param mefd_path  path to the `.mefd` session directory.
  /// @param password   level-1 or level-2 password; empty if unencrypted.
  /// @param n_threads  RED decode threads: 0 = hardware concurrency, 1 = serial.
  Reader(const std::string& mefd_path, std::string password = "", int n_threads = 0)
      : session_(mefd_path, std::move(password)), n_threads_(n_threads) {}

  /// Set the default decode thread count (0 = all cores, 1 = serial).
  void set_threads(int n) { n_threads_ = n; }
  /// Current default decode thread count.
  int threads() const { return n_threads_; }

  /// Channel names present in the session.
  const std::vector<std::string>& channels() const { return session_.channels(); }
  /// Metadata for @p channel (fs, conversion factor, times, sample counts,
  /// subject metadata when accessible).
  const ChannelInfo& info(const std::string& channel) const {
    return session_.channel_info(channel);
  }

  /// Read `[t0, t1)` as int32 counts plus a validity mask on the fs grid.
  /// @param channel    channel name.
  /// @param t0,t1      half-open uUTC window; defaults span the whole channel.
  ///                   Returned length is `round((t1 - t0) * fs / 1e6)`.
  /// @param n_threads  per-call thread override (kUseDefaultThreads = default).
  /// @returns samples, a validity mask (0 in gaps), and scaling metadata.
  RawData read_raw(const std::string& channel, std::optional<si8> t0 = std::nullopt,
                   std::optional<si8> t1 = std::nullopt, int n_threads = kUseDefaultThreads);

  /// Read `[t0, t1)` as float64 (`= counts * units_conversion_factor`), with
  /// discontinuity gaps filled with NaN. Parameters as in read_raw().
  std::vector<sf8> read(const std::string& channel, std::optional<si8> t0 = std::nullopt,
                        std::optional<si8> t1 = std::nullopt, int n_threads = kUseDefaultThreads);

  /// Sentinel for the per-call @c n_threads argument meaning "use the reader
  /// default".
  static constexpr int kUseDefaultThreads = -1000000;

  /// Block-level table of contents for @p channel (per-RED-block start time,
  /// sample counts, extrema, discontinuity flags) — for seeking and viewers.
  std::vector<BlockIndexEntry> toc(const std::string& channel) {
    return session_.read_index(channel);
  }

  /// Per-segment map of @p channel: each segment's time and sample ranges and
  /// block count, from metadata only (nothing decoded). Locates data across
  /// large gaps; complements toc(), which is block-level.
  std::vector<SegmentInfo> segments(const std::string& channel) {
    return session_.segment_map(channel);
  }

  /// Records (annotations): channel-level for @p channel, or session-level
  /// when @p channel is nullopt.
  std::vector<Record> records(std::optional<std::string> channel = std::nullopt) {
    return session_.read_records(std::move(channel));
  }

  Session& session() { return session_; }

 private:
  Session session_;
  int n_threads_ = 0;
};

}  // namespace mef3io
