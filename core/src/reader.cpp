// mef3io — high-level reader: gridding, gap fill, scaling, parallel decode.
#include "mef3io/reader.hpp"

#include <cmath>
#include <limits>

#include "mef3io/parallel.hpp"
#include "mef3io/red.hpp"

namespace mef3io {
namespace {
// Sample index on the [t0, fs) grid nearest to absolute uUTC `t`.
si8 grid_index(si8 t, si8 t0, sf8 fs) {
  return static_cast<si8>(std::llround(static_cast<sf8>(t - t0) * fs / 1e6));
}
}  // namespace

RawData Reader::read_raw(const std::string& channel, std::optional<si8> t0_opt,
                         std::optional<si8> t1_opt, int n_threads) {
  const ChannelInfo& ci = session_.channel_info(channel);
  const sf8 fs = ci.sampling_frequency;
  const si8 t0 = t0_opt.value_or(ci.start_time);
  const si8 t1 = t1_opt.value_or(ci.end_time);

  RawData out;
  out.start_uutc = t0;
  out.sampling_frequency = fs;
  out.units_conversion_factor = ci.units_conversion_factor;

  si8 n = grid_index(t1, t0, fs);
  if (n < 0) n = 0;
  out.samples.assign(static_cast<std::size_t>(n), 0);
  out.valid.assign(static_cast<std::size_t>(n), 0);

  BlockJobs jobs = session_.collect_blocks(channel, t0, t1);
  const int threads = (n_threads == kUseDefaultThreads) ? n_threads_ : n_threads;

  // Blocks occupy disjoint sample ranges, so each job writes a disjoint slice
  // of the output — decoding in parallel is race-free and order-independent.
  parallel_for(jobs.jobs.size(), threads, [&](std::size_t j) {
    const BlockJob& job = jobs.jobs[j];
    std::span<const ui1> block(jobs.buffers[job.buffer_index].data() + job.offset, job.block_bytes);
    auto decoded = red::decode_block(block, job.keys);
    si8 idx0 = grid_index(job.start_uutc, t0, fs);
    for (std::size_t k = 0; k < decoded.samples.size(); ++k) {
      si8 idx = idx0 + static_cast<si8>(k);
      if (idx < 0 || idx >= n) continue;  // trim to the requested window
      out.samples[static_cast<std::size_t>(idx)] = decoded.samples[k];
      out.valid[static_cast<std::size_t>(idx)] = 1;
    }
  });
  return out;
}

std::vector<sf8> Reader::read(const std::string& channel, std::optional<si8> t0,
                              std::optional<si8> t1, int n_threads) {
  RawData raw = read_raw(channel, t0, t1, n_threads);
  const sf8 uf = raw.units_conversion_factor;
  const sf8 nan = std::numeric_limits<sf8>::quiet_NaN();
  std::vector<sf8> out(raw.samples.size());
  for (std::size_t i = 0; i < out.size(); ++i)
    out[i] = raw.valid[i] ? static_cast<sf8>(raw.samples[i]) * uf : nan;
  return out;
}

}  // namespace mef3io
