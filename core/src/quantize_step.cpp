// mef3io — the data's own quantization step (#81, `precision="step"`).
//
// Float data that came from an ADC is `count * step` (e.g. 0.195 uV per
// count). Stored with that step as the conversion factor, the samples are the
// original small integers, which RED compresses far better than the
// power-of-ten counts of the default path (`count * 195` at precision 3).
//
// Two parts, and only the second decides:
//   detect_step       ESTIMATES the step from a bounded sample of the data
//                     (the first 65,536 values + 16 windows of 2,048 spread
//                     through the rest), so it costs the same on 1 k or 1 G
//                     samples;
//   quantize_to_step  converts EVERY value and verifies it on the way (one
//                     pass, the pass that produces the counts anyway): a value
//                     that is not a whole multiple of the step within float64
//                     precision, or whose count does not fit int32, fails the
//                     whole conversion and the caller falls back.
//
// The guarantee for a value stored this way is the reader's own arithmetic:
// `double(count) * step` (Reader::read) differs from the value written by at
// most kStepRelTol * |value| — 64 units of float64 rounding, ~1.4e-14
// relative. When the data was produced as `count * step` in float64 with the
// step detected here (the usual case: the snapping below finds the step's
// own float64 value), it is the same multiplication and reads back BITWISE
// equal (-0.0 reads back as 0.0).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

#include "mef3io/session_writer.hpp"
#include "number_text.hpp"

namespace mef3io {
namespace {

constexpr sf8 kEps = std::numeric_limits<sf8>::epsilon();
// A value is a whole multiple of the step when the reader's reconstruction
// lands within this many float64 roundings of it (relative).
constexpr sf8 kStepRelTol = 64.0 * kEps;
// int32 counts, keeping INT32_MIN free: it is RED's NaN marker.
constexpr sf8 kMaxCount = 2147483647.0;
// The estimate's own fit test, relative to the step. Generous on purpose: it
// only steers the estimate; the strict test is kStepRelTol, applied to the
// snapped step on the sample and then to every value.
constexpr sf8 kCoarseTol = 1e-6;

// Distinct non-zero magnitudes a sample must show before a step is adopted.
constexpr std::size_t kMinDistinct = 16;
constexpr const char* kNoStep =
    "the values are not whole multiples of one step whose counts fit int32";

constexpr std::size_t kHead = 65536;
constexpr std::size_t kWindows = 16;
constexpr std::size_t kWindow = 2048;

// |x - round(x / step) * step|, the distance to the nearest multiple.
inline sf8 remainder_to(sf8 x, sf8 step) { return std::abs(x - std::round(x / step) * step); }

// Euclid on reals: the largest s with a and b (both > 0) whole multiples of s
// within kCoarseTol * s. 0 when it would drop below min_step (no step whose
// counts fit int32). Each round at least halves b, so this ends.
sf8 real_gcd(sf8 a, sf8 b, sf8 min_step) {
  if (a < b) std::swap(a, b);
  while (b >= min_step) {
    const sf8 r = remainder_to(a, b);
    if (r <= kCoarseTol * b + kStepRelTol * a) return b;
    a = b;
    b = r;
  }
  return 0.0;
}

// The reader's reconstruction of |u| with step c, and whether it is exact /
// within tolerance. u >= 0 (sign is symmetric under rounding).
struct Fit {
  bool valid = true;
  std::size_t mismatches = 0;
};
Fit evaluate(const std::vector<sf8>& mags, sf8 c) {
  Fit f;
  if (!(c > 0.0) || !std::isfinite(c)) {
    f.valid = false;
    return f;
  }
  for (sf8 u : mags) {
    const sf8 q = std::round(u / c);
    if (!(q <= kMaxCount)) {
      f.valid = false;
      return f;
    }
    const sf8 r = q * c;  // == double(si4(q)) * c, what the reader computes
    if (r != u) {
      if (std::abs(u - r) > kStepRelTol * u) {
        f.valid = false;
        return f;
      }
      ++f.mismatches;
    }
  }
  return f;
}

sf8 round_significant(sf8 x, int digits) {
  char buf[40];
  std::snprintf(buf, sizeof(buf), "%.*e", digits - 1, x);
  return std::strtod(buf, nullptr);
}

}  // namespace

StepEstimate detect_step(std::span<const sf8> data) {
  StepEstimate out;
  // The bounded sample: never more than kHead + kWindows * kWindow values,
  // whatever the length of the data.
  std::vector<sf8> mags;
  mags.reserve(kHead + kWindows * kWindow);
  bool non_finite = false;
  auto take = [&](std::size_t b, std::size_t e) {
    for (std::size_t i = b; i < e; ++i) {
      const sf8 v = data[i];
      if (std::isnan(v)) continue;
      if (!std::isfinite(v)) non_finite = true;
      mags.push_back(std::abs(v));
    }
  };
  const std::size_t n = data.size();
  const std::size_t head = std::min(n, kHead);
  take(0, head);
  if (n > head)
    for (std::size_t j = 1; j <= kWindows; ++j) {
      const std::size_t b = head + (n - head) * j / (kWindows + 1);
      take(b, std::min(n, b + kWindow));
    }
  auto nonzero = [&] {
    std::sort(mags.begin(), mags.end());
    mags.erase(std::unique(mags.begin(), mags.end()), mags.end());
    if (!mags.empty() && mags.front() == 0.0) mags.erase(mags.begin());  // 0 fits any step
  };
  nonzero();
  if (mags.empty() && !non_finite) {
    // A sample of nothing but 0 and NaN (a flat or unconnected start): look
    // on through the data for the first non-zero values. Only this
    // degenerate input pays a scan, and it stops once it has a sample.
    for (std::size_t i = 0; i < n && mags.size() < kHead; ++i) {
      const sf8 v = data[i];
      if (v != v || v == 0.0) continue;
      if (!std::isfinite(v)) non_finite = true;
      mags.push_back(std::abs(v));
    }
    nonzero();
  }
  if (non_finite) {
    out.reason = "the data holds an infinite value";
    return out;
  }
  if (mags.empty()) return out;  // nothing but 0 and NaN: no step, and none needed
  // Minimum evidence (#149 review): a DC level, a square wave or a handful
  // of levels "has" a step (the level itself), and since a channel keeps its
  // first factor, adopting it would make every later write refuse. Too few
  // distinct magnitudes to establish a step: the default path.
  if (mags.size() < kMinDistinct) {
    out.reason = "the sample holds only " + std::to_string(mags.size()) +
                 " distinct non-zero magnitude(s), too few to establish a step (" +
                 std::to_string(kMinDistinct) + " needed)";
    return out;
  }
  const sf8 maxabs = mags.back();
  const sf8 min_step = maxabs / kMaxCount;  // finer and the counts overflow int32

  // Ascending magnitudes: the smallest values fix the step's scale exactly
  // (Euclid on small multiples is precise), and a least-squares refit at
  // every power-of-two index keeps its relative error far below what the
  // larger values that follow can amplify.
  sf8 step = mags.front();
  sf8 sum_ku = 0.0, sum_kk = 0.0;
  std::size_t next_refit = 64;
  for (std::size_t i = 0; i < mags.size(); ++i) {
    const sf8 u = mags[i];
    const sf8 r = remainder_to(u, step);
    if (r > kCoarseTol * step + kStepRelTol * u) {
      step = real_gcd(step, r, min_step);
      if (step == 0.0) {
        out.reason = kNoStep;
        return out;
      }
      sum_ku = sum_kk = 0.0;
      next_refit = i + 1;  // refit over everything seen, with the new step
    }
    if (i + 1 == next_refit) {
      // In units of the step (k and u/step are both <= 2^31), so the sums
      // cannot overflow however large the magnitudes are (1e300 steps).
      sum_ku = sum_kk = 0.0;
      for (std::size_t j = 0; j <= i; ++j) {
        const sf8 ratio = mags[j] / step;
        const sf8 k = std::round(ratio);
        sum_ku += k * ratio;
        sum_kk += k * k;
      }
      if (sum_kk > 0.0) step *= sum_ku / sum_kk;
      next_refit = std::max<std::size_t>(64, 2 * (i + 1));
    }
  }

  // Snap the estimate to the step's own float64 value: the shortest decimal
  // that reproduces every sampled value exactly wins (0.195, 0.1, 1e-3/3 at
  // 16 digits), then the value implied by the largest count and its float64
  // neighbours (binary steps such as 10e6 / 2**24). With no exact candidate
  // the one with the fewest inexact (but within-tolerance) values is taken.
  std::vector<sf8> candidates;
  for (int d = 1; d <= 17; ++d) {
    const sf8 c = round_significant(step, d);
    if (std::abs(c - step) <= kCoarseTol * step) candidates.push_back(c);
  }
  const sf8 k_max = std::round(maxabs / step);
  if (k_max >= 1.0) {
    sf8 c = maxabs / k_max;
    candidates.push_back(c);
    sf8 up = c, down = c;
    for (int s = 0; s < 2; ++s) {
      up = std::nextafter(up, std::numeric_limits<sf8>::infinity());
      down = std::nextafter(down, 0.0);
      candidates.push_back(up);
      candidates.push_back(down);
    }
  }
  candidates.push_back(step);

  bool found = false;
  std::size_t best_mismatches = 0;
  sf8 best = 0.0;
  for (sf8 c : candidates) {
    const Fit f = evaluate(mags, c);
    if (!f.valid) continue;
    if (!found || f.mismatches < best_mismatches) {
      found = true;
      best = c;
      best_mismatches = f.mismatches;
      if (best_mismatches == 0) break;
    }
  }
  if (!found) {
    out.reason = kNoStep;
    return out;
  }
  out.step = best;
  return out;
}

bool quantize_to_step(std::span<const sf8> data, sf8 step, std::vector<si4>& counts,
                      std::vector<ui1>& valid, std::string* why, StepMismatch* kind) {
  if (kind) *kind = StepMismatch::None;
  auto fail = [&](std::size_t i, const char* what, StepMismatch k) {
    if (kind) *kind = k;
    if (why) *why = "the value at index " + std::to_string(i) + " " + what + " " + detail::shortest_number(step);
    return false;
  };
  // A negative step is a polarity inversion (a foreign channel's factor):
  // the counts change sign, the check below is the same.
  if (step == 0.0 || !std::isfinite(step)) {
    if (why) *why = "the step is not a finite non-zero number";
    if (kind) *kind = StepMismatch::BadStep;
    return false;
  }
  if (!std::isfinite(1.0 / step)) {  // subnormal: no reciprocal to convert by
    if (why) *why = "the step " + detail::shortest_number(step) + " is too small (subnormal)";
    if (kind) *kind = StepMismatch::BadStep;
    return false;
  }
  const std::size_t n = data.size();
  counts.resize(n);
  valid.resize(n);
  si4* const out = counts.data();
  ui1* const ok = valid.data();
  // The hot loop of a step write (one pass over the data, the pass that makes
  // the counts anyway). Kept free of library calls: the multiply by the
  // reciprocal lands within a few roundings of the count for any value that
  // passes (they are whole multiples), and adding and removing 1.5 * 2^52
  // rounds a double of magnitude < 2^51 to the nearest integer in two adds.
  // Neither is a shortcut of the CHECK, which is the reader's own product.
  const sf8 inv = 1.0 / step;
  constexpr sf8 kRound = 6755399441055744.0;  // 1.5 * 2^52
  for (std::size_t i = 0; i < n; ++i) {
    const sf8 x = data[i];
    if (x != x) {  // NaN: gap position, not stored
      out[i] = 0;
      ok[i] = 0;
      continue;
    }
    const sf8 y = x * inv;
    if (!(std::abs(y) <= kMaxCount)) {  // also an infinite x
      if (!std::isfinite(x)) return fail(i, "is not finite, at the step", StepMismatch::NotFinite);
      return fail(i, "needs a count beyond int32 at the step", StepMismatch::Overflow);
    }
    const sf8 q = (y + kRound) - kRound;
    if (std::abs(x - q * step) > kStepRelTol * std::abs(x))
      return fail(i, "is not a whole multiple of the step", StepMismatch::NotMultiple);
    out[i] = static_cast<si4>(q);
    ok[i] = 1;
  }
  return true;
}

namespace detail {
std::string shortest_number(sf8 x) {
  char buf[40];
  for (int d = 1; d <= 17; ++d) {
    std::snprintf(buf, sizeof(buf), "%.*g", d, x);
    if (std::strtod(buf, nullptr) == x) break;
  }
  return buf;
}
}  // namespace detail

}  // namespace mef3io
