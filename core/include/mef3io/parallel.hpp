// mef3io — minimal deterministic parallel_for. Work items are independent and
// write to disjoint outputs, so results never depend on thread count or
// scheduling. No shared mutable state, no global pool.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

namespace mef3io {

// Resolve a requested thread count: 0 or negative -> hardware concurrency.
inline unsigned resolve_threads(int requested) {
  if (requested > 0) return static_cast<unsigned>(requested);
  unsigned hc = std::thread::hardware_concurrency();
  return hc ? hc : 1u;
}

// Run body(i) for i in [0, n). Exceptions from any item are re-thrown after all
// workers join (first one wins). Deterministic: body(i) must only touch outputs
// owned by index i.
template <typename F>
void parallel_for(std::size_t n, int requested_threads, F body) {
  unsigned n_threads = resolve_threads(requested_threads);
  if (n_threads <= 1 || n <= 1) {
    for (std::size_t i = 0; i < n; ++i) body(i);
    return;
  }
  n_threads = static_cast<unsigned>(std::min<std::size_t>(n_threads, n));

  std::atomic<std::size_t> next{0};
  std::exception_ptr first_error;
  std::mutex err_mtx;
  auto worker = [&]() {
    for (;;) {
      std::size_t i = next.fetch_add(1, std::memory_order_relaxed);
      if (i >= n) break;
      try {
        body(i);
      } catch (...) {
        std::lock_guard<std::mutex> lock(err_mtx);
        if (!first_error) first_error = std::current_exception();
        // keep draining so no worker blocks on a partially-consumed range
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

}  // namespace mef3io
