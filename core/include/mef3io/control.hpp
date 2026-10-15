// mef3io — cancellation and progress for reads and long operations.
//
// Two hooks, both optional, both carried by one `Control` passed to the
// operations that can take a while: a windowed or full read, archive_session,
// extract_session, inspect_session, validate_session, repair_session and
// recover_session.
//
//   * CancelToken — a flag any thread may set. The operation looks at it
//     between units of work (a decoded block, a 1 MB copy chunk, a segment)
//     and, once it is set, unwinds by throwing `Cancelled`. A cancelled call
//     returns no partial result and leaves no partial file behind.
//   * ProgressFn — `progress(done, total)` in bytes, called at a bounded rate
//     (see ProgressReporter) and never concurrently with itself.
//
// Unused hooks cost one null-pointer test per unit of work: no clock reads,
// no atomics, no allocation.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

namespace mef3io {

/// Thrown when a CancelToken passed to an operation is set.
///
/// DELIBERATELY NOT a std::exception. The core contains per-segment failures
/// in many places with `catch (const std::exception&)` — a lenient Session
/// turns one into a SegmentProblem, the validator into a skipped segment. A
/// cancellation caught there would be reported as damage and the operation
/// would carry on. Deriving from nothing means only `catch (...)` (used for
/// cleanup, always rethrowing) and an explicit `catch (const Cancelled&)` can
/// see it, so no future containment site can swallow it by accident.
class Cancelled {
 public:
  explicit Cancelled(std::string message = "operation cancelled")
      : message_(std::move(message)) {}
  const char* what() const noexcept { return message_.c_str(); }

 private:
  std::string message_;
};

/// A cancellation flag shared between the caller and a running operation.
///
/// One-shot: once cancelled it stays cancelled (there is no reset, so a token
/// can never be "un-cancelled" while an operation is still looking at it).
/// cancel() and cancelled() are safe from any thread, including a signal-free
/// UI thread while the operation runs on another.
class CancelToken {
 public:
  CancelToken() = default;
  CancelToken(const CancelToken&) = delete;
  CancelToken& operator=(const CancelToken&) = delete;

  /// Request cancellation. Idempotent.
  void cancel() noexcept { flag_.store(true, std::memory_order_relaxed); }
  /// Whether cancel() has been called.
  bool cancelled() const noexcept { return flag_.load(std::memory_order_relaxed); }

 private:
  std::atomic<bool> flag_{false};
};

/// `progress(done, total)`, both in bytes. See ProgressReporter for the
/// guarantees on how it is called.
using ProgressFn = std::function<void(std::uint64_t done, std::uint64_t total)>;

/// The optional hooks of one call. Both members may be left empty.
struct Control {
  /// Not owned; must outlive the call. nullptr = not cancellable.
  const CancelToken* cancel = nullptr;
  /// Empty = no progress reporting.
  ProgressFn progress;

  /// Throw Cancelled if the token is set. One pointer test when unused.
  void check() const {
    if (cancel && cancel->cancelled()) throw Cancelled();
  }
};

namespace detail {

/// Rate-limited, serialised, monotonic progress reporting for one operation.
///
/// Guarantees to the callback:
///   * called once with (0, total) by start(), and once with (total, total)
///     by finish() when — and only when — the cancellable work completes (for
///     total == 0 the single (0, 0) from start() is that report);
///   * the callback must not drive the same operation (no re-entry: the
///     reporter's mutex is not recursive);
///   * in between, at most once per 1% of `total` advanced, and otherwise at
///     most every 250 ms, so the call count stays bounded however fast or slow
///     the work is (about 100 + 4 per second);
///   * never concurrently: worker threads that would report while another is
///     inside the callback skip their report rather than wait (so a slow
///     callback cannot stall the decode, and a callback that needs a lock the
///     caller holds — such as the Python GIL — cannot deadlock against it);
///   * `done` never decreases and never exceeds `total` (reports at `total`
///     are left to finish()).
///
/// An exception thrown by the callback propagates out of add()/advance_to(),
/// aborting the operation exactly like a cancellation would.
class ProgressReporter {
 public:
  ProgressReporter(const Control& ctl, std::uint64_t total)
      : fn_(ctl.progress ? &ctl.progress : nullptr), total_(total) {}

  bool active() const { return fn_ != nullptr; }
  std::uint64_t total() const { return total_; }

  void start() {
    if (!fn_) return;
    std::lock_guard<std::mutex> lock(mutex_);
    last_time_ = std::chrono::steady_clock::now();
    last_done_ = 0;
    (*fn_)(0, total_);
  }

  /// Count `n` more bytes as done. Thread-safe.
  void add(std::uint64_t n) {
    if (!fn_) return;
    const std::uint64_t now_done = done_.fetch_add(n, std::memory_order_relaxed) + n;
    maybe_report(now_done);
  }

  /// Raise the done count to `v` (never lowers it). Thread-safe.
  void advance_to(std::uint64_t v) {
    if (!fn_) return;
    std::uint64_t cur = done_.load(std::memory_order_relaxed);
    while (cur < v && !done_.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
    }
    maybe_report(v > cur ? v : cur);
  }

  /// The cancellable work completed: report (total, total). Call exactly once,
  /// after the last check() and BEFORE any irreversible write.
  void finish() {
    if (!fn_ || total_ == 0) return;  // start() already said (0, 0)
    std::lock_guard<std::mutex> lock(mutex_);
    last_done_ = total_;
    (*fn_)(total_, total_);
  }

 private:
  void maybe_report(std::uint64_t now_done) {
    if (now_done >= total_) return;  // the (total, total) report is finish()'s
    const std::uint64_t step = total_ / 100;
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock()) return;  // someone is reporting right now
    if (now_done <= last_done_) return;
    const auto now = std::chrono::steady_clock::now();
    if (now_done - last_done_ < step && now - last_time_ < std::chrono::milliseconds(250))
      return;
    // Re-read under the lock: the latest value is at least `now_done`, and
    // reporting it keeps the sequence monotonic across threads.
    std::uint64_t v = done_.load(std::memory_order_relaxed);
    if (v >= total_) v = total_ - 1;
    if (v <= last_done_) return;
    last_done_ = v;
    last_time_ = now;
    (*fn_)(v, total_);
  }

  const ProgressFn* fn_;
  std::uint64_t total_;
  std::atomic<std::uint64_t> done_{0};
  std::mutex mutex_;
  std::uint64_t last_done_ = 0;
  std::chrono::steady_clock::time_point last_time_{};
};

}  // namespace detail
}  // namespace mef3io
