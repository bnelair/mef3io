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

class Reader {
 public:
  // n_threads: 0 -> hardware concurrency; 1 -> serial. Per-call override on
  // read_raw/read.
  Reader(const std::string& mefd_path, std::string password = "", int n_threads = 0)
      : session_(mefd_path, std::move(password)), n_threads_(n_threads) {}

  void set_threads(int n) { n_threads_ = n; }
  int threads() const { return n_threads_; }

  const std::vector<std::string>& channels() const { return session_.channels(); }
  const ChannelInfo& info(const std::string& channel) const {
    return session_.channel_info(channel);
  }

  // Read [t0, t1) as int32 + validity mask on the fs grid. Defaults span the
  // whole channel. N = round((t1 - t0) * fs / 1e6). `n_threads` overrides the
  // reader default for this call (INT_MIN = use default).
  RawData read_raw(const std::string& channel, std::optional<si8> t0 = std::nullopt,
                   std::optional<si8> t1 = std::nullopt, int n_threads = kUseDefaultThreads);

  // Read [t0, t1) as float64 = int32 * units_conversion_factor, gaps = NaN.
  std::vector<sf8> read(const std::string& channel, std::optional<si8> t0 = std::nullopt,
                        std::optional<si8> t1 = std::nullopt, int n_threads = kUseDefaultThreads);

  static constexpr int kUseDefaultThreads = -1000000;

  std::vector<BlockIndexEntry> toc(const std::string& channel) {
    return session_.read_index(channel);
  }

  std::vector<Record> records(std::optional<std::string> channel = std::nullopt) {
    return session_.read_records(std::move(channel));
  }

  Session& session() { return session_; }

 private:
  Session session_;
  int n_threads_ = 0;
};

}  // namespace mef3io
