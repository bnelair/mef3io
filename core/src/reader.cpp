// mef3io — high-level reader: gridding, gap fill, scaling, parallel decode.
#include "mef3io/reader.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <atomic>
#include <exception>
#include <memory>
#include <mutex>
#include <map>
#include <sstream>
#include <stdexcept>
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

// One channel's blocks for one window, with the output partitioned between
// them (see read_raw_impl for why): job j owns pieces[first[j] ..
// first[j] + count[j]), disjoint from every other job's.
struct Plan {
  BlockJobs jobs;
  std::vector<si8> idx0;                     // output slot of each job's first sample
  std::vector<std::pair<si8, si8>> pieces;   // flattened, one run per job
  std::vector<std::size_t> first, count;
};

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
Plan plan_blocks(BlockJobs&& jobs, si8 t0, sf8 fs, si8 n) {
  Plan p;
  p.jobs = std::move(jobs);
  const std::size_t n_jobs = p.jobs.jobs.size();
  p.idx0.assign(n_jobs, 0);
  p.first.assign(n_jobs, 0);
  p.count.assign(n_jobs, 0);
  IntervalMap covered;
  std::vector<std::pair<si8, si8>> owned;
  for (std::size_t r = n_jobs; r-- > 0;) {
    const BlockJob& job = p.jobs.jobs[r];
    p.idx0[r] = grid_slot(job.start_uutc, t0, fs);
    // Trim to the requested window before claiming, so samples outside it
    // never mask a block that does fall inside.
    const si8 b = std::max<si8>(p.idx0[r], 0);
    const si8 e = std::min<si8>(p.idx0[r] + static_cast<si8>(job.number_of_samples), n);
    owned.clear();
    claim_range(covered, b, e, owned);
    p.first[r] = p.pieces.size();
    p.count[r] = owned.size();
    p.pieces.insert(p.pieces.end(), owned.begin(), owned.end());
  }
  return p;
}

// A read in units refuses an invalid factor (NaN, inf) BEFORE decoding. A
// factor that is not set (0) is read as 1 instead (see read()).
void refuse_unusable_factors(const Plan& p, const std::string& channel) {
  for (std::size_t j = 0; j < p.jobs.jobs.size(); ++j)
    if (p.count[j] != 0 && !usable_units_conversion_factor(p.jobs.jobs[j].units_conversion_factor))
      throw FormatError(
          unusable_factor_message(channel, p.jobs.jobs[j].units_conversion_factor, "read_raw()"));
}

// Compressed bytes of the blocks that will actually be decoded.
std::uint64_t decode_bytes(const Plan& p) {
  std::uint64_t total = 0;
  for (std::size_t j = 0; j < p.jobs.jobs.size(); ++j)
    if (p.count[j] != 0) total += p.jobs.jobs[j].block_bytes;
  return total;
}

// Decode job j and hand every output slot it owns AND holds a sample for to
// put(slot, count). Returns how many samples the block decoded to. The
// caller skips jobs owning nothing (count == 0): fully shadowed by a later
// block or trimmed away by the window, they cannot affect the result.
template <class Put>
si8 decode_job(const Plan& p, std::size_t j, Put&& put) {
  const BlockJob& job = p.jobs.jobs[j];
  std::span<const ui1> block(p.jobs.buffers[job.buffer_index].data() + job.offset,
                             job.block_bytes);
  auto decoded = red::decode_block(block, job.keys);
  const si8 n_decoded = static_cast<si8>(decoded.samples.size());
  for (std::size_t q = p.first[j]; q < p.first[j] + p.count[j]; ++q) {
    for (si8 idx = p.pieces[q].first; idx < p.pieces[q].second; ++idx) {
      const si8 k = idx - p.idx0[j];
      if (k < 0 || k >= n_decoded) continue;  // index claims more than the block holds
      put(idx, decoded.samples[static_cast<std::size_t>(k)]);
    }
  }
  return n_decoded;
}

std::string rate_text(sf8 fs) {
  std::ostringstream os;
  os.precision(17);
  os << fs;
  return os.str();
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

  Plan plan = plan_blocks(session_.collect_blocks(channel, t0, t1, ctl), t0, fs, n);
  out.block_copy_mismatches = std::move(plan.jobs.mismatches);
  // Read once: a set_threads() from another thread applies to the next call.
  const int threads = n_threads;
  const std::size_t n_jobs = plan.jobs.jobs.size();

  if (in_units) refuse_unusable_factors(plan, channel);

  // Progress counts the compressed bytes of the blocks that will actually be
  // decoded. Summed only when someone is listening: zero cost otherwise.
  detail::ProgressReporter progress(ctl, ctl.progress ? decode_bytes(plan) : 0);
  progress.start();

  std::vector<si8> n_decoded_of(n_jobs, 0);  // one slot per job: no race
  parallel_for(n_jobs, threads, [&](std::size_t j) {
    if (plan.count[j] == 0) return;
    // Between blocks. Every remaining job checks too, so once the token is set
    // parallel_for drains in microseconds and rethrows the first Cancelled;
    // `out` is then discarded by the unwinding caller — no partial data.
    ctl.check();
    n_decoded_of[j] = decode_job(plan, j, [&](si8 idx, si4 v) {
      out.samples[static_cast<std::size_t>(idx)] = v;
      out.valid[static_cast<std::size_t>(idx)] = 1;
    });
    progress.add(plan.jobs.jobs[j].block_bytes);
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
    for (std::size_t q = plan.first[j]; q < plan.first[j] + plan.count[j]; ++q) {
      const si8 b = plan.pieces[q].first;
      const si8 e = std::min(plan.pieces[q].second, plan.idx0[j] + n_decoded_of[j]);
      if (b < e) filled.push_back({b, e, plan.jobs.jobs[j].units_conversion_factor});
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

// read() of one or many channels (#90). Values are written straight into the
// output in units: each slot holds count * the factor of the block that owns
// it, which is exactly read_raw()'s spans applied (a span groups pieces of
// EQUAL factors, same_units_conversion_factor, so every slot's span factor
// is its own block's), with no int32 + mask copy of the window in between.
template <class T, class Alloc>
void Reader::read_many_impl(const std::vector<std::string>& channels, std::optional<si8> t0_opt,
                            std::optional<si8> t1_opt, const Control& ctl, Alloc&& alloc) {
  if (channels.empty())
    throw std::invalid_argument("read: no channels given (pass at least one name)");
  const std::size_t n_ch = channels.size();
  // Arguments first, before any file is touched: every name must exist and
  // every channel must share one sampling frequency — one grid for all rows.
  // One channel is read() exactly as before: no rate rule.
  const ChannelInfo& c0 = session_.channel_info(channels.front());
  const sf8 fs = c0.sampling_frequency;
  si8 start = c0.start_time, end = c0.end_time;
  for (const auto& name : channels) {
    const ChannelInfo& ci = session_.channel_info(name);
    if (n_ch > 1) {
      if (!std::isfinite(ci.sampling_frequency))
        throw std::invalid_argument(
            "read: channel " + name + " has no usable sampling rate (" +
            rate_text(ci.sampling_frequency) +
            " Hz), so it cannot share a multi-channel read's grid; read it on its own");
      if (ci.sampling_frequency != fs)
        throw std::invalid_argument(
            "read: a multi-channel read needs ONE sampling rate for all its channels, but " +
            channels.front() + " is sampled at " + rate_text(fs) + " Hz and " + name + " at " +
            rate_text(ci.sampling_frequency) + " Hz; read channels of different rates in "
            "separate calls (one call per rate)");
    }
    start = std::min(start, ci.start_time);
    end = std::max(end, ci.end_time);
  }
  const si8 t0 = t0_opt.value_or(start);
  const si8 t1 = t1_opt.value_or(end);
  const si8 n = std::max<si8>(grid_count(t1, t0, fs), 0);
  // Read once: a set_threads() from another thread applies to the next call.
  const int threads = session_.threads();

  // ERRORS: the one raised is the lowest failing channel's in LIST order,
  // whatever stage it failed in (index, data, factor, decode) — what a loop
  // of read() raises. `limit` = the lowest channel known to fail; nothing is
  // started for a channel above it (it cannot change the outcome), so a
  // damaged first channel stops the call early. Channels below it still run
  // to the end: one of them may fail too, and its error wins.
  std::atomic<std::size_t> limit{n_ch};
  std::vector<std::exception_ptr> errors(n_ch);
  std::unique_ptr<std::atomic<bool>[]> failed(new std::atomic<bool>[n_ch]);
  for (std::size_t i = 0; i < n_ch; ++i) failed[i].store(false);
  std::mutex error_mu;
  auto lower_limit = [&](std::size_t i) {
    std::size_t cur = limit.load();
    while (i < cur && !limit.compare_exchange_weak(cur, i)) {
    }
  };
  auto fail = [&](std::size_t i) {
    {
      std::lock_guard<std::mutex> lock(error_mu);
      if (!errors[i]) errors[i] = std::current_exception();
    }
    failed[i].store(true);
    lower_limit(i);
  };
  auto skipped = [&](std::size_t i) { return i > limit.load() || failed[i].load(); };

  // A LIST first walks every channel's block index at once (file access on
  // the Reader's threads, Work::Io: a request is a round trip, whatever the
  // number of cores). That sizes the progress for the whole call, and a
  // channel whose index is damaged stops every channel after it before any
  // data is fetched. One channel skips this: collect_blocks does both halves.
  std::vector<BlockSelection> selection(n_ch);
  std::vector<std::uint64_t> selected_bytes(n_ch, 0);
  std::uint64_t total = 0;
  if (n_ch > 1) {
    parallel_for(
        n_ch, threads,
        [&](std::size_t i) {
          if (skipped(i)) return;
          try {
            selection[i] = session_.select_blocks(channels[i], t0, t1, ctl);
            selected_bytes[i] = selection[i].bytes();
            // Its stored error is raised when its data is fetched (the order
            // of a serial walk), but nothing after it matters any more.
            if (selection[i].will_fail) lower_limit(i);
          } catch (...) {
            fail(i);
          }
        },
        Work::Io);
    for (std::size_t i = 0; i < n_ch; ++i) total += selected_bytes[i];
  }
  // Progress (compressed bytes): a list's total is every selected block,
  // known now and constant; one channel's is the blocks it will decode,
  // known once they are fetched (as read() always reported it).
  std::optional<detail::ProgressReporter> progress;
  if (n_ch > 1) {
    progress.emplace(ctl, ctl.progress ? total : 0);
    progress->start();
  }

  // The output: `alloc` returns n_ch * n slots and says whether they already
  // hold NaN. If not (a multi-channel read: one fresh buffer of up to GBs),
  // the NaN fill runs on the decode threads, row by row: filling it serially
  // was most of the call's time locally (first touch of every page on one
  // thread: 0.53 s of a 0.79 s read of 209 channels x 10 min at 512 Hz).
  const std::size_t row_len = static_cast<std::size_t>(n);
  auto [base, prefilled] = alloc(n_ch * row_len);
  if (!prefilled)
    parallel_for(n_ch, threads, [&](std::size_t i) {
      std::fill(base + i * row_len, base + (i + 1) * row_len, std::numeric_limits<T>::quiet_NaN());
    });

  // Then the data, a GROUP of channels at a time (as many as the Reader's
  // file-access threads): fetch the group's byte extents at once, then ONE
  // decode pool over every block of the group. A group's compressed blocks
  // are freed when it is decoded, so the peak beside the output is one
  // group's window of compressed data, never the whole list's.
  const std::size_t group =
      n_ch == 1 ? 1 : std::max<std::size_t>(1, resolve_threads(threads, Work::Io));
  std::vector<char> unscaled(n_ch, 0);
  for (std::size_t g0 = 0; g0 < n_ch && g0 <= limit.load(); g0 += group) {
    const std::size_t g1 = std::min(n_ch, g0 + group);
    std::vector<Plan> plans(g1 - g0);
    parallel_for(
        g1 - g0, threads,
        [&](std::size_t k) {
          const std::size_t i = g0 + k;
          if (skipped(i)) return;
          try {
            BlockJobs jobs = n_ch == 1
                                 ? session_.collect_blocks(channels[i], t0, t1, ctl)
                                 : session_.fetch_blocks(channels[i], std::move(selection[i]), ctl);
            plans[k] = plan_blocks(std::move(jobs), t0, fs, n);
            // Refused BEFORE decoding, as read() refuses it (NaN, inf).
            refuse_unusable_factors(plans[k], channels[i]);
          } catch (...) {
            fail(i);
          }
        },
        Work::Io);

    // ONE decode pool over every block of the group. Blocks of different
    // channels write different rows; within a channel the plan's pieces are
    // disjoint — so no two jobs ever write the same slot.
    struct Ref {
      std::size_t k, job;
    };
    std::vector<Ref> work;
    for (std::size_t k = 0; k < plans.size(); ++k) {
      if (skipped(g0 + k)) continue;
      for (std::size_t j = 0; j < plans[k].jobs.jobs.size(); ++j)
        if (plans[k].count[j] != 0) work.push_back({k, j});
    }
    if (n_ch == 1) {
      if (failed[0].load()) break;
      // read()'s progress: the blocks that will actually be decoded.
      progress.emplace(ctl, ctl.progress ? decode_bytes(plans[0]) : 0);
      progress->start();
    }
    std::vector<char> wrote(work.size(), 0);  // one slot per job: no race
    parallel_for(work.size(), threads, [&](std::size_t w) {
      const std::size_t i = g0 + work[w].k;
      if (skipped(i)) return;
      try {
        ctl.check();  // between blocks; see read_raw_impl
        const Plan& p = plans[work[w].k];
        const BlockJob& job = p.jobs.jobs[work[w].job];
        // A factor not set (0, NO_ENTRY) gives the raw counts.
        const sf8 uf = effective_units_conversion_factor(job.units_conversion_factor);
        T* row = base + i * row_len;
        bool any = false;
        decode_job(p, work[w].job, [&](si8 idx, si4 v) {
          row[idx] = static_cast<T>(static_cast<sf8>(v) * uf);
          any = true;
        });
        if (any && job.units_conversion_factor == 0.0) wrote[w] = 1;
        progress->add(job.block_bytes);
      } catch (...) {
        fail(i);
      }
    });
    for (std::size_t w = 0; w < work.size(); ++w)
      if (wrote[w]) unscaled[g0 + work[w].k] = 1;
    // A list counts every selected block: the ones not decoded (shadowed,
    // outside the grid, a lenient segment skipped) are done now too.
    if (n_ch > 1)
      for (std::size_t k = 0; k < plans.size(); ++k) {
        if (skipped(g0 + k)) continue;
        const std::uint64_t decoded = decode_bytes(plans[k]);
        if (selected_bytes[g0 + k] > decoded) progress->add(selected_bytes[g0 + k] - decoded);
      }
  }  // the group's plans, and its compressed blocks, are freed here

  const std::size_t first_failed = limit.load();
  if (first_failed < n_ch) std::rethrow_exception(errors[first_failed]);
  if (progress) progress->finish();
  for (std::size_t i = 0; i < n_ch; ++i)
    if (unscaled[i]) session_.note_unscaled(channels[i]);
}

namespace {
// One channel: a std::vector, NaN-filled as it is made (one pass).
template <class T>
struct VectorOut {
  std::vector<T>& v;
  std::pair<T*, bool> operator()(std::size_t n) {
    v.assign(n, std::numeric_limits<T>::quiet_NaN());
    return {v.data(), true};
  }
};
// Several channels: an uninitialized buffer (`new T[]` leaves floats
// untouched), filled in parallel by read_many_impl.
template <class T>
struct RowsOut {
  ChannelRows<T>& r;
  std::size_t n_ch;
  std::pair<T*, bool> operator()(std::size_t n) {
    r.channels = n_ch;
    r.samples_per_channel = n_ch ? n / n_ch : 0;
    r.data.reset(new T[n == 0 ? 1 : n]);
    return {r.data.get(), false};
  }
};
}  // namespace

std::vector<sf8> Reader::read(const std::string& channel, std::optional<si8> t0,
                              std::optional<si8> t1, const Control& ctl) {
  std::vector<sf8> out;
  read_many_impl<sf8>({channel}, t0, t1, ctl, VectorOut<sf8>{out});
  return out;
}

std::vector<sf4> Reader::read_f32(const std::string& channel, std::optional<si8> t0,
                                  std::optional<si8> t1, const Control& ctl) {
  std::vector<sf4> out;
  read_many_impl<sf4>({channel}, t0, t1, ctl, VectorOut<sf4>{out});
  return out;
}

ChannelRows<sf8> Reader::read_many(const std::vector<std::string>& channels,
                                   std::optional<si8> t0, std::optional<si8> t1,
                                   const Control& ctl) {
  ChannelRows<sf8> out;
  read_many_impl<sf8>(channels, t0, t1, ctl, RowsOut<sf8>{out, channels.size()});
  return out;
}

ChannelRows<sf4> Reader::read_many_f32(const std::vector<std::string>& channels,
                                       std::optional<si8> t0, std::optional<si8> t1,
                                       const Control& ctl) {
  ChannelRows<sf4> out;
  read_many_impl<sf4>(channels, t0, t1, ctl, RowsOut<sf4>{out, channels.size()});
  return out;
}

std::vector<BlockEnvelope> Reader::block_envelope_many(const std::vector<std::string>& channels,
                                                       std::optional<si8> t0,
                                                       std::optional<si8> t1) {
  std::vector<BlockEnvelope> out(channels.size());
  const auto errors = parallel_try_each(
      channels.size(), session_.threads(),
      [&](std::size_t i) { out[i] = session_.block_envelope(channels[i], t0, t1); }, Work::Io);
  for (const auto& e : errors)
    if (e) std::rethrow_exception(e);
  return out;
}

}  // namespace mef3io
