// mef3io — precision step (#81): detect_step / quantize_to_step and the
// SessionWriter path. The Python suite (tests/test_p43_write_step.py) pins the
// same rules end to end, plus size, speed and the legacy oracle.
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <limits>
#include <random>
#include <string>
#include <vector>

#include "mef3io/errors.hpp"
#include "mef3io/reader.hpp"
#include "mef3io/session_writer.hpp"

namespace fs = std::filesystem;
using namespace mef3io;

namespace {
struct Scratch {
  fs::path dir;
  explicit Scratch(const char* name) {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    dir = fs::temp_directory_path() /
          (std::string("mef3io_step_") + name + "_" + std::to_string(nonce) + ".mefd");
  }
  ~Scratch() {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
};

std::vector<si4> counts(std::size_t n, unsigned seed) {
  std::mt19937 rng(seed);
  std::uniform_int_distribution<int> d(-40, 40);
  std::vector<si4> c(n);
  long long v = 0;
  for (auto& x : c) {
    v += d(rng);
    if (v > 30000 || v < -30000) v /= 2;
    x = static_cast<si4>(v);
  }
  return c;
}

std::vector<sf8> times(const std::vector<si4>& c, sf8 step) {
  std::vector<sf8> x(c.size());
  for (std::size_t i = 0; i < c.size(); ++i) x[i] = static_cast<sf8>(c[i]) * step;
  return x;
}

constexpr si8 kStart = 1577836800000000;
}  // namespace

TEST_CASE("detect_step finds the step's own float64 value") {
  for (sf8 step : {0.195, 0.1, 1e-3 / 3, 10e6 / 16777216.0, 1.0, 2.5e-7}) {
    const auto x = times(counts(100000, 1), step);
    const StepEstimate e = detect_step(x);
    REQUIRE(e.step.has_value());
    REQUIRE(*e.step == step);
  }
}

TEST_CASE("detect_step: no step in filtered data, none needed for zeros") {
  auto x = times(counts(50000, 2), 0.195);
  std::vector<sf8> f(x.size());
  // A low-pass with irrational taps, like any designed filter. (Taps that
  // are short decimals or powers of two keep the data on a finer step,
  // which is then found and stored exactly — not a fallback case.)
  const sf8 a = std::sqrt(0.5) / 2, b = 1.0 - 2 * a;
  for (std::size_t i = 1; i + 1 < x.size(); ++i) f[i] = a * x[i - 1] + b * x[i] + a * x[i + 1];
  const StepEstimate e = detect_step(f);
  REQUIRE_FALSE(e.step.has_value());
  REQUIRE_FALSE(e.reason.empty());
  std::vector<sf8> z(1000, 0.0);
  z[3] = std::numeric_limits<sf8>::quiet_NaN();
  const StepEstimate ez = detect_step(z);
  REQUIRE_FALSE(ez.step.has_value());
  REQUIRE(ez.reason.empty());
}

TEST_CASE("quantize_to_step verifies every value") {
  const auto c = counts(10000, 3);
  auto x = times(c, 0.195);
  x[17] = std::numeric_limits<sf8>::quiet_NaN();
  std::vector<si4> k;
  std::vector<ui1> valid;
  REQUIRE(quantize_to_step(x, 0.195, k, valid));
  for (std::size_t i = 0; i < c.size(); ++i) {
    if (i == 17) {
      REQUIRE(valid[i] == 0);
      continue;
    }
    REQUIRE(valid[i] == 1);
    REQUIRE(k[i] == c[i]);
  }
  std::string why;
  x[9000] += 1e-6;
  REQUIRE_FALSE(quantize_to_step(x, 0.195, k, valid, &why));
  REQUIRE(why.find("index 9000") != std::string::npos);
  // On the step but beyond int32, and +-inf: clamped, counted, not a failure.
  x[9000] = 0.195 * 3e9;
  x[9001] = -std::numeric_limits<sf8>::infinity();
  ClampCount cl;
  REQUIRE(quantize_to_step(x, 0.195, k, valid, &why, &cl));
  REQUIRE(cl.finite == 1);
  REQUIRE(cl.infinite == 1);
  REQUIRE(k[9000] == 2147483647);
  REQUIRE(k[9001] == -2147483647);  // never INT32_MIN (RED_NAN)
}

TEST_CASE("SessionWriter: precision step stores the counts, falls back otherwise") {
  Scratch s("writer");
  const auto c = counts(20000, 4);
  const auto x = times(c, 0.195);
  {
    SessionWriter w(s.dir.string());
    const WriteSummary r = w.write_float("ch1", x, kStart, 512.0, Precision::detect_step());
    REQUIRE(r.quantization == Quantization::Step);
    REQUIRE(r.units_conversion_factor == 0.195);
    REQUIRE(r.step_fallback.empty());
    // Off the step on an existing channel: refused, never rounded, and the
    // default precision cannot append to a factor that is no power of ten.
    std::vector<sf8> y(1000, 0.1);
    const si8 t1 = kStart + static_cast<si8>(std::llround(20000 / 512.0 * 1e6));
    REQUIRE_THROWS_AS(w.write_float("ch1", y, t1, 512.0, Precision::detect_step()),
                      WriteConflictError);
    REQUIRE_THROWS_AS(w.write_float("ch1", y, t1, 512.0, Precision::detect_step(), true),
                      WriteConflictError);
    REQUIRE_THROWS_AS(w.write_float("ch1", y, t1, 512.0), WriteConflictError);
    // A new channel of off-step data: the default path.
    std::vector<sf8> f(x.size());
    for (std::size_t i = 0; i < x.size(); ++i) f[i] = x[i] / 3.0 + 1e-7 * std::sin(i);
    const WriteSummary d = w.write_float("ch2", f, kStart, 512.0, Precision::detect_step());
    REQUIRE(d.quantization == Quantization::Decimal);
    REQUIRE_FALSE(d.step_fallback.empty());
    // The int default is unchanged and still compiles from an int.
    const WriteSummary p = w.write_float("ch3", x, kStart, 512.0, 3);
    REQUIRE(p.quantization == Quantization::Decimal);
    REQUIRE(p.step_fallback.empty());
  }
  Reader r(s.dir.string());
  const auto raw = r.read_raw("ch1", kStart, kStart + static_cast<si8>(std::llround(20000 / 512.0 * 1e6)));
  REQUIRE(raw.units_conversion_factor == 0.195);
  REQUIRE(raw.samples.size() == c.size());
  for (std::size_t i = 0; i < c.size(); ++i) REQUIRE(raw.samples[i] == c[i]);
}

TEST_CASE("detect_step: too few distinct magnitudes is no step") {
  std::vector<sf8> sq(4096);
  for (std::size_t i = 0; i < sq.size(); ++i) sq[i] = (i / 64) % 2 ? 50.115 : -50.115;
  const StepEstimate e = detect_step(sq);
  REQUIRE_FALSE(e.step.has_value());
  REQUIRE(e.reason.find("distinct") != std::string::npos);
}

TEST_CASE("quantize_to_int32 clamps instead of wrapping") {
  std::vector<sf8> x{1.0, 3.0e6, -3.0e6, std::numeric_limits<sf8>::infinity(),
                     std::numeric_limits<sf8>::quiet_NaN()};
  ClampCount c;
  const auto k = quantize_to_int32(x, 3, &c);
  REQUIRE(k[0] == 1000);
  REQUIRE(k[1] == 2147483647);
  REQUIRE(k[2] == -2147483647);
  REQUIRE(k[3] == 2147483647);
  REQUIRE(k[4] == 0);
  REQUIRE(c.finite == 2);
  REQUIRE(c.infinite == 1);
  REQUIRE(infer_precision(std::vector<sf8>{3.0e6, 3.0e6 + 0.001}) <= 2);
}
