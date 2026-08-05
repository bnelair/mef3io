// mef3io — high-level reader: gridding, gap fill, scaling, parallel decode.
#include "mef3io/reader.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <utility>

#include "mef3io/parallel.hpp"
#include "mef3io/red.hpp"

namespace mef3io {
namespace {
// Sample index on the [t0, fs) grid nearest to absolute uUTC `t`.
si8 grid_index(si8 t, si8 t0, sf8 fs) {
  return static_cast<si8>(std::llround(static_cast<sf8>(t - t0) * fs / 1e6));
}

// Half-open output ranges claimed by one block, and the set already claimed by
// blocks that win over it. `covered` holds disjoint, merged intervals keyed by
// begin; for the usual contiguous channel it stays a single entry.
using IntervalMap = std::map<si8, si8>;

// Append the parts of [b, e) that no earlier caller has claimed, then mark the
// whole of [b, e) claimed. Calling this for blocks in DESCENDING job order
// gives each output sample to the LAST block covering it, which is what a
// serial front-to-back scatter produces.
void claim_range(IntervalMap& covered, si8 b, si8 e,
                 std::vector<std::pair<si8, si8>>& pieces) {
  if (b >= e) return;

  si8 cur = b;
  auto it = covered.lower_bound(b);
  if (it != covered.begin() && std::prev(it)->second > cur) cur = std::prev(it)->second;
  for (; it != covered.end() && it->first < e && cur < e; ++it) {
    if (it->first > cur) pieces.emplace_back(cur, std::min(it->first, e));
    cur = std::max(cur, it->second);
  }
  if (cur < e) pieces.emplace_back(cur, e);

  // Merge [b, e) into `covered`, absorbing every interval it meets.
  auto lo = covered.lower_bound(b);
  if (lo != covered.begin() && std::prev(lo)->second >= b) --lo;
  si8 nb = b, ne = e;
  auto hi = lo;
  while (hi != covered.end() && hi->first <= ne) {
    nb = std::min(nb, hi->first);
    ne = std::max(ne, hi->second);
    ++hi;
  }
  covered.erase(lo, hi);
  covered.emplace(nb, ne);
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
  const std::size_t n_jobs = jobs.jobs.size();

  // Blocks do NOT always occupy disjoint sample ranges. Only writers that put
  // every block start exactly on the sampling grid tile the output cleanly;
  // foreign recorders store per-block timestamps carrying acquisition jitter
  // and microsecond rounding, so a block can begin a few samples before the
  // previous one ends. Two blocks then claim the same output samples, and
  // letting workers write them concurrently is a data race: results vary with
  // thread count and scheduling, silently, in the sample values themselves.
  //
  // So partition the output first. Each block's range is known from the index
  // without decoding anything, and resolving the overlaps in descending job
  // order gives every sample to the last block covering it — the result a
  // serial front-to-back scatter produces, which is what meflib produces.
  // Workers then own disjoint pieces and any thread count is byte-identical.
  std::vector<si8> idx0(n_jobs);
  std::vector<std::pair<si8, si8>> pieces;   // flattened, one run per job
  std::vector<std::size_t> first(n_jobs, 0), count(n_jobs, 0);
  {
    IntervalMap covered;
    std::vector<std::pair<si8, si8>> owned;
    for (std::size_t r = n_jobs; r-- > 0;) {
      const BlockJob& job = jobs.jobs[r];
      idx0[r] = grid_index(job.start_uutc, t0, fs);
      // Trim to the requested window before claiming, so samples outside it
      // never mask a block that does fall inside.
      const si8 b = std::max<si8>(idx0[r], 0);
      const si8 e = std::min<si8>(idx0[r] + static_cast<si8>(job.number_of_samples), n);
      owned.clear();
      claim_range(covered, b, e, owned);
      first[r] = pieces.size();
      count[r] = owned.size();
      pieces.insert(pieces.end(), owned.begin(), owned.end());
    }
  }

  parallel_for(n_jobs, threads, [&](std::size_t j) {
    const BlockJob& job = jobs.jobs[j];
    std::span<const ui1> block(jobs.buffers[job.buffer_index].data() + job.offset, job.block_bytes);
    auto decoded = red::decode_block(block, job.keys);
    const si8 n_decoded = static_cast<si8>(decoded.samples.size());
    for (std::size_t p = first[j]; p < first[j] + count[j]; ++p) {
      for (si8 idx = pieces[p].first; idx < pieces[p].second; ++idx) {
        const si8 k = idx - idx0[j];
        if (k < 0 || k >= n_decoded) continue;  // index claims more than the block holds
        out.samples[static_cast<std::size_t>(idx)] = decoded.samples[static_cast<std::size_t>(k)];
        out.valid[static_cast<std::size_t>(idx)] = 1;
      }
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
