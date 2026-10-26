// mef3io — high-level reader: gridding, gap fill, scaling, parallel decode.
#include "mef3io/reader.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <utility>

#include "mef3io/errors.hpp"
#include "mef3io/parallel.hpp"
#include "mef3io/red.hpp"

namespace mef3io {
namespace {
using detail::grid_count;
using detail::grid_slot;

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

RawData Reader::read_raw(const std::string& channel, std::optional<si8> t0,
                         std::optional<si8> t1, const Control& ctl) {
  return read_raw_impl(channel, t0, t1, session_.threads(), ctl, false);
}

RawData Reader::read_raw_impl(const std::string& channel, std::optional<si8> t0_opt,
                              std::optional<si8> t1_opt, int n_threads, const Control& ctl,
                              bool in_units) {
  const ChannelInfo& ci = session_.channel_info(channel);
  const sf8 fs = ci.sampling_frequency;
  const si8 t0 = t0_opt.value_or(ci.start_time);
  const si8 t1 = t1_opt.value_or(ci.end_time);

  RawData out;
  out.start_uutc = t0;
  out.sampling_frequency = fs;
  out.units_conversion_factor = ci.units_conversion_factor;

  si8 n = grid_count(t1, t0, fs);
  if (n < 0) n = 0;
  out.samples.assign(static_cast<std::size_t>(n), 0);
  out.valid.assign(static_cast<std::size_t>(n), 0);

  BlockJobs jobs = session_.collect_blocks(channel, t0, t1, ctl);
  out.block_copy_mismatches = std::move(jobs.mismatches);
  // Read once: a set_threads() from another thread applies to the next call.
  const int threads = n_threads;
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
      idx0[r] = grid_slot(job.start_uutc, t0, fs);
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

  // A read in units refuses an invalid factor (NaN, inf) BEFORE decoding. A
  // factor that is not set (0) is read as 1 instead (see read()).
  if (in_units)
    for (std::size_t j = 0; j < n_jobs; ++j)
      if (count[j] != 0 && !usable_units_conversion_factor(jobs.jobs[j].units_conversion_factor))
        throw FormatError(
            unusable_factor_message(channel, jobs.jobs[j].units_conversion_factor, "read_raw()"));

  // Progress counts the compressed bytes of the blocks that will actually be
  // decoded. Summed only when someone is listening: zero cost otherwise.
  std::uint64_t decode_bytes = 0;
  if (ctl.progress)
    for (std::size_t j = 0; j < n_jobs; ++j)
      if (count[j] != 0) decode_bytes += jobs.jobs[j].block_bytes;
  detail::ProgressReporter progress(ctl, decode_bytes);
  progress.start();

  std::vector<si8> n_decoded_of(n_jobs, 0);  // one slot per job: no race
  parallel_for(n_jobs, threads, [&](std::size_t j) {
    // A block that owns no output — fully shadowed by a later one, or trimmed
    // away by the window — cannot affect the result, so don't decode it.
    if (count[j] == 0) return;
    // Between blocks. Every remaining job checks too, so once the token is set
    // parallel_for drains in microseconds and rethrows the first Cancelled;
    // `out` is then discarded by the unwinding caller — no partial data.
    ctl.check();
    const BlockJob& job = jobs.jobs[j];
    std::span<const ui1> block(jobs.buffers[job.buffer_index].data() + job.offset, job.block_bytes);
    auto decoded = red::decode_block(block, job.keys);
    const si8 n_decoded = static_cast<si8>(decoded.samples.size());
    n_decoded_of[j] = n_decoded;
    for (std::size_t p = first[j]; p < first[j] + count[j]; ++p) {
      for (si8 idx = pieces[p].first; idx < pieces[p].second; ++idx) {
        const si8 k = idx - idx0[j];
        if (k < 0 || k >= n_decoded) continue;  // index claims more than the block holds
        out.samples[static_cast<std::size_t>(idx)] = decoded.samples[static_cast<std::size_t>(k)];
        out.valid[static_cast<std::size_t>(idx)] = 1;
      }
    }
    progress.add(job.block_bytes);
  });
  progress.finish();

  // Factor spans (#150), from the pieces each block actually filled: the
  // slots it owns, cut to the samples it decoded. Pieces are disjoint, so in
  // order of start they are the valid slots in order.
  struct Filled {
    si8 b, e;
    sf8 f;
  };
  std::vector<Filled> filled;
  for (std::size_t j = 0; j < n_jobs; ++j)
    for (std::size_t p = first[j]; p < first[j] + count[j]; ++p) {
      const si8 b = pieces[p].first;
      const si8 e = std::min(pieces[p].second, idx0[j] + n_decoded_of[j]);
      if (b < e) filled.push_back({b, e, jobs.jobs[j].units_conversion_factor});
    }
  std::sort(filled.begin(), filled.end(),
            [](const Filled& a, const Filled& b) { return a.b < b.b; });
  for (const auto& f : filled) {
    if (!out.factor_spans.empty() &&
        same_units_conversion_factor(out.factor_spans.back().units_conversion_factor, f.f))
      out.factor_spans.back().end_index = f.e;
    else
      out.factor_spans.push_back(FactorSpan{f.b, f.e, f.f});
  }
  if (out.factor_spans.size() == 1)
    out.units_conversion_factor = out.factor_spans.front().units_conversion_factor;
  else if (out.factor_spans.size() > 1)
    out.units_conversion_factor.reset();
  return out;
}

std::vector<sf8> Reader::read(const std::string& channel, std::optional<si8> t0,
                              std::optional<si8> t1, const Control& ctl) {
  RawData raw = read_raw_impl(channel, t0, t1, session_.threads(), ctl, true);
  const sf8 nan = std::numeric_limits<sf8>::quiet_NaN();
  std::vector<sf8> out(raw.samples.size(), nan);
  // Each sample by its own segment's factor (#150); every valid slot lies in
  // exactly one span. A factor not set (0, NO_ENTRY) gives the raw counts.
  for (const auto& sp : raw.factor_spans) {
    if (sp.units_conversion_factor == 0.0) session_.note_unscaled(channel);
    const sf8 uf = effective_units_conversion_factor(sp.units_conversion_factor);
    for (si8 i = sp.start_index; i < sp.end_index; ++i) {
      const auto k = static_cast<std::size_t>(i);
      if (raw.valid[k]) out[k] = static_cast<sf8>(raw.samples[k]) * uf;
    }
  }
  return out;
}

}  // namespace mef3io
