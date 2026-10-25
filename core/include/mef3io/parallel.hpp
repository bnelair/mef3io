// mef3io — the ONE thread rule, and a minimal deterministic parallel_for.
//
// THREAD RULE (docs/threads.md): an object's `n_threads` is how many threads
// that object may use for everything it does — opening, file access,
// decoding, decrypting, encoding. 0 = automatic (the default), 1 = no extra
// threads. resolve_threads() below is the ONLY place "automatic" is decided;
// bindings ask it (Python `mef3io._mef3io.resolve_threads`) instead of
// re-deriving it.
//
// parallel_for: work items are independent and write to disjoint outputs, so
// results never depend on thread count or scheduling. No shared mutable
// state, no global pool.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

namespace mef3io {

/// What the threads will mostly do. Only matters for an AUTOMATIC count.
enum class Work {
  Cpu,  ///< decoding, decrypting, encoding: one thread per hardware thread
  Io,   ///< file access (the open's metadata pass): waits on round trips, so
        ///< more threads than cores pay off on a network share
};

/// Smallest automatic thread count for file access: a network share answers
/// each request in milliseconds whatever the core count, and the open issues
/// several requests per segment.
inline constexpr unsigned kMinAutomaticIoThreads = 8;

/// Resolve a requested thread count. A positive request is taken as is; 0 (or
/// a negative value) is AUTOMATIC: the hardware concurrency for CPU work, and
/// at least kMinAutomaticIoThreads for file access.
inline unsigned resolve_threads(int requested, Work kind = Work::Cpu) {
  if (requested > 0) return static_cast<unsigned>(requested);
  unsigned hc = std::thread::hardware_concurrency();
  if (hc == 0) hc = 1;
  return kind == Work::Io ? std::max(hc, kMinAutomaticIoThreads) : hc;
}

// Run body(i) for i in [0, n). Exceptions from any item are re-thrown after all
// workers join (first one wins). Deterministic: body(i) must only touch outputs
// owned by index i.
template <typename F>
void parallel_for(std::size_t n, int requested_threads, F body, Work kind = Work::Cpu) {
  unsigned n_threads = resolve_threads(requested_threads, kind);
  if (n_threads <= 1 || n <= 1) {
    for (std::size_t i = 0; i < n; ++i) body(i);
    return;
  }
  n_threads = static_cast<unsigned>(std::min<std::size_t>(n_threads, n));

  std::atomic<std::size_t> next{0};
  std::atomic<bool> failed{false};
  std::exception_ptr first_error;
  std::mutex err_mtx;
  auto worker = [&]() {
    for (;;) {
      std::size_t i = next.fetch_add(1, std::memory_order_relaxed);
      if (i >= n) break;
      // Once any item has thrown the result is discarded, so the rest are
      // skipped rather than run: a cancelled read stops decoding promptly
      // instead of finishing every remaining block first. Still drained, so
      // no worker blocks on a partially-consumed range.
      if (failed.load(std::memory_order_relaxed)) continue;
      try {
        body(i);
      } catch (...) {
        std::lock_guard<std::mutex> lock(err_mtx);
        if (!first_error) first_error = std::current_exception();
        failed.store(true, std::memory_order_relaxed);
      }
    }
  };

  std::vector<std::thread> pool;
  pool.reserve(n_threads - 1);
  for (unsigned t = 1; t < n_threads; ++t) pool.emplace_back(worker);
  worker();
  for (auto& th : pool) th.join();

  if (first_error) std::rethrow_exception(first_error);
}

/// Run body(i) for EVERY i in [0, n) — none is skipped after a failure — and
/// return each item's exception (null = it succeeded), indexed like the items.
///
/// For work whose error must not depend on scheduling: the caller picks the
/// failure a SERIAL loop would have met first (the lowest index, or its own
/// order), never whichever thread happened to fail first. The open uses it so
/// a strict Session raises exactly what a one-thread open raises.
template <typename F>
std::vector<std::exception_ptr> parallel_try_each(std::size_t n, int requested_threads, F body,
                                                  Work kind = Work::Cpu) {
  std::vector<std::exception_ptr> errors(n);
  parallel_for(
      n, requested_threads,
      [&](std::size_t i) {
        try {
          body(i);
        } catch (...) {
          errors[i] = std::current_exception();
        }
      },
      kind);
  return errors;
}

}  // namespace mef3io
