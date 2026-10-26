// Multi-channel reads (#90): Reader::read_many / read_many_f32 / read_f32 and
// block_envelope_many. Every row must be byte-identical to read() of that
// channel alone at every thread count; arguments are refused before any
// request; the error raised is the first channel's, in list order, that a
// loop of read() calls would raise; the file requests are exactly the loop's
// (concurrent, never more); n_threads = 1 starts no thread.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <typeinfo>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "counting_source.hpp"
#include "mef3io/control.hpp"
#include "mef3io/errors.hpp"
#include "mef3io/parallel.hpp"
#include "mef3io/reader.hpp"
#include "mef3io/session_writer.hpp"
#include "mef3io/source.hpp"

using namespace mef3io;
namespace fsys = std::filesystem;

namespace {
constexpr si8 START = 1577836800000000;
constexpr sf8 FS = 250.0;

// Five channels at 250 Hz, small blocks: gaps (valid mask), several segments
// with different factors, a channel starting late, plus one at 500 Hz.
std::string write_session(const std::string& name) {
  const auto dir = (fsys::temp_directory_path() / name).string();
  fsys::remove_all(dir);
  SessionWriter w(dir, true);
  std::vector<si4> x(5000);
  for (int i = 0; i < 5000; ++i) x[i] = static_cast<si4>(1000 * std::sin(i / 9.0)) + i % 7;
  std::vector<ui1> valid(5000, 1);
  for (int i = 1200; i < 1900; ++i) valid[i] = 0;
  w.write_int32("gappy", x, 0.5, START, FS, valid, false);
  w.write_int32("gappy", x, 0.5, START + 30'000'000, FS, {}, true);
  w.write_int32("mixed", x, 0.01, START, FS, {}, false);
  w.write_int32("mixed", x, -0.195, START + 20'000'000, FS, {}, true);
  w.write_int32("late", x, 1.0, START + 25'000'000, FS, {}, false);
  w.write_int32("plain", x, 2.0, START + 1'000, FS, {}, false);
  w.write_int32("fast", x, 1.0, START, 2 * FS, {}, false);
  return dir;
}

const std::vector<std::string> NAMES = {"gappy", "mixed", "late", "plain", "mixed"};

std::vector<sf8> stacked(Reader& r, const std::vector<std::string>& names, si8 t0, si8 t1) {
  std::vector<sf8> out;
  for (const auto& c : names) {
    auto row = r.read(c, t0, t1);
    out.insert(out.end(), row.begin(), row.end());
  }
  return out;
}

bool same_bytes(const std::vector<sf8>& a, const std::vector<sf8>& b) {
  return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(sf8)) == 0;
}
template <class T>
std::vector<T> flat(const ChannelRows<T>& r) {
  return std::vector<T>(r.data.get(), r.data.get() + r.size());
}

struct HookGuard {
  explicit HookGuard(void (*fn)(unsigned)) { detail::thread_start_hook.store(fn); }
  ~HookGuard() { detail::thread_start_hook.store(nullptr); }
};
std::atomic<int> g_started{0};
void count_start(unsigned) { ++g_started; }
}  // namespace

TEST_CASE("read_many rows are byte-identical to read(), at every thread count", "[read-many]") {
  const auto dir = write_session("mef3io_read_many.mefd");
  const std::vector<std::pair<si8, si8>> windows = {
      {START, START + 50'000'000},          {START - 3'000'000, START + 4'000'000},
      {START + 2'002'000, START + 27'002'000},  // half a sample off the 250 Hz grid
      {START + 19'000'000, START + 31'000'000}, {START + 70'000'000, START + 71'000'000},
      {START + 5'000'000, START + 5'000'000}};
  for (int t : {1, 2, 8, 0}) {
    Reader r(dir, "", t);
    for (const auto& [t0, t1] : windows) {
      const auto want = stacked(r, NAMES, t0, t1);
      const auto rows = r.read_many(NAMES, t0, t1);
      CHECK(rows.channels == NAMES.size());
      CHECK(rows.samples_per_channel * NAMES.size() == want.size());
      CHECK(same_bytes(flat(rows), want));
      const auto f = flat(r.read_many_f32(NAMES, t0, t1));
      REQUIRE(f.size() == want.size());
      bool f_ok = true;
      for (std::size_t i = 0; i < f.size(); ++i) {
        const sf4 e = static_cast<sf4>(want[i]);
        if (std::memcmp(&e, &f[i], sizeof e) != 0) f_ok = false;
      }
      CHECK(f_ok);
      const auto one = r.read_f32("mixed", t0, t1);
      const auto one64 = r.read("mixed", t0, t1);
      REQUIRE(one.size() == one64.size());
      for (std::size_t i = 0; i < one.size(); ++i)
        CHECK((std::isnan(one[i]) ? std::isnan(one64[i]) : one[i] == static_cast<sf4>(one64[i])));
    }
    // Unset ends: the union of the listed channels' spans.
    const auto& g = r.info("gappy");
    const auto& l = r.info("late");
    CHECK(same_bytes(flat(r.read_many({"gappy", "late"})),
                     stacked(r, {"gappy", "late"}, std::min(g.start_time, l.start_time),
                             std::max(g.end_time, l.end_time))));
    CHECK(same_bytes(flat(r.read_many({"plain"})), r.read("plain")));
  }
  fsys::remove_all(dir);
}

TEST_CASE("read_many refuses its arguments before any request", "[read-many]") {
  const auto dir = write_session("mef3io_read_many_args.mefd");
  auto src = std::make_shared<test::CountingSource>(open_session_source(dir));
  Reader r(std::shared_ptr<const SessionSource>(src), "", 4);
  src->reset();
  CHECK_THROWS_AS(r.read_many({}), std::invalid_argument);
  CHECK_THROWS_AS(r.read_many({"gappy", "fast"}), std::invalid_argument);
  CHECK_THROWS_AS(r.read_many({"gappy", "nope", "fast"}), std::out_of_range);
  try {
    r.read_many({"plain", "late", "fast"});
    FAIL("mixed rates accepted");
  } catch (const std::invalid_argument& e) {
    const std::string msg = e.what();
    CHECK(msg.find("ONE sampling rate") != std::string::npos);
    CHECK(msg.find("separate calls") != std::string::npos);
    CHECK(msg.find("plain is sampled at 250 Hz and fast at 500 Hz") != std::string::npos);
  }
  CHECK(src->calls().total() == 0);
  // block_envelope_many takes any mix of rates
  CHECK(r.block_envelope_many({"gappy", "fast"}).size() == 2);
  fsys::remove_all(dir);
}

TEST_CASE("read_many raises the first failing channel's error, in list order", "[read-many]") {
  const auto dir = write_session("mef3io_read_many_err.mefd");
  // Truncate two channels' data: their index then points past the .tdat end.
  for (const char* ch : {"late", "mixed"})
    for (const auto& e : fsys::recursive_directory_iterator(dir + "/" + ch + ".timd"))
      if (e.path().extension() == ".tdat") fsys::resize_file(e.path(), 1500);
  for (int t : {1, 2, 8}) {
    Reader r(dir, "", t);
    for (const auto& [names, bad] :
         std::vector<std::pair<std::vector<std::string>, std::string>>{
             {{"plain", "late", "mixed"}, "late"}, {{"gappy", "mixed", "late"}, "mixed"}}) {
      std::string single;
      try {
        r.read(bad);
      } catch (const FormatError& e) {
        single = e.what();
      }
      REQUIRE(!single.empty());
      try {
        r.read_many(names);
        FAIL("no error");
      } catch (const FormatError& e) {
        CHECK(std::string(e.what()) == single);
      }
    }
  }
  fsys::remove_all(dir);
}

TEST_CASE("read_many: cancel, progress over the whole call, no thread at n_threads 1",
          "[read-many]") {
  const auto dir = write_session("mef3io_read_many_ctl.mefd");
  {
    Reader r(dir, "", 4);
    CancelToken tok;
    tok.cancel();
    Control ctl;
    ctl.cancel = &tok;
    CHECK_THROWS_AS(r.read_many(NAMES, START, START + 50'000'000, ctl), Cancelled);

    std::uint64_t sum = 0;
    for (const auto& c : NAMES) {
      Control one;
      std::uint64_t total = 0;
      one.progress = [&](std::uint64_t, std::uint64_t t) { total = t; };
      r.read(c, START, START + 50'000'000, one);
      sum += total;
    }
    Control all;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> seen;
    all.progress = [&](std::uint64_t d, std::uint64_t t) { seen.emplace_back(d, t); };
    r.read_many(NAMES, START, START + 50'000'000, all);
    REQUIRE(!seen.empty());
    CHECK(seen.front().first == 0);
    CHECK(seen.back().first == sum);
    for (std::size_t i = 0; i < seen.size(); ++i) {
      CHECK(seen[i].second == sum);
      if (i) CHECK(seen[i - 1].first <= seen[i].first);
    }
  }
  {
    g_started = 0;
    HookGuard guard(&count_start);
    Reader r(dir, "", 1);
    r.read_many(NAMES);
    r.read_many_f32(NAMES);
    r.block_envelope_many(NAMES);
    CHECK(g_started == 0);
  }
  fsys::remove_all(dir);
}

TEST_CASE("read_many makes the loop's requests, concurrently", "[read-many]") {
  const auto dir = write_session("mef3io_read_many_req.mefd");
  const std::vector<std::string> names = {"gappy", "mixed", "late", "plain"};
  const auto delay = std::chrono::milliseconds(10);
  auto run = [&](bool many, test::SourceCalls& calls) {
    auto src = std::make_shared<test::CountingSource>(open_session_source(dir), delay);
    Reader r(std::shared_ptr<const SessionSource>(src), "", 8);
    src->reset();
    const auto t0 = std::chrono::steady_clock::now();
    if (many)
      r.read_many(names, START, START + 50'000'000);
    else
      for (const auto& c : names) r.read(c, START, START + 50'000'000);
    calls = src->calls();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  };
  test::SourceCalls loop_calls, many_calls;
  const double loop = run(false, loop_calls);
  const double many = run(true, many_calls);
  CHECK(many_calls.total() == loop_calls.total());
  CHECK(many_calls.read_range == loop_calls.read_range);
  INFO("loop " << loop << " s, list " << many << " s");
  CHECK(many < loop / 2);
  fsys::remove_all(dir);
}

TEST_CASE("multi-channel read time with a per-request delay", "[.read-many-bench]") {
  // Hidden; the numbers in docs/benchmarks.md come from here:
  //   build-core/mef3io_tests "[.read-many-bench]"
  // 10-minute window at 512 Hz, one segment per channel, 10-second blocks.
  const sf8 fs = 512.0;
  const si8 window = 600'000'000;
  for (int channels : {64, 209}) {
    const auto dir = (fsys::temp_directory_path() / "mef3io_read_many_bench.mefd").string();
    fsys::remove_all(dir);
    {
      SessionWriter w(dir, true);
      std::vector<si4> x(static_cast<std::size_t>(fs * 660));
      for (int c = 0; c < channels; ++c) {
        for (std::size_t i = 0; i < x.size(); ++i)
          x[i] = static_cast<si4>(800 * std::sin(i / (7.0 + c)) + (i * 2654435761u >> 27));
        w.write_int32("ch" + std::to_string(c), x, 1.0, START, fs, {}, false);
      }
    }
    std::vector<std::string> names;
    for (int c = 0; c < channels; ++c) names.push_back("ch" + std::to_string(c));
    const bool quick = std::getenv("MEF3IO_BENCH_LOCAL") != nullptr;  // local disk only
    for (int ms : {0, 5, 20}) {
      if (quick && ms) break;
      for (int t : {1, 4, 16})
        for (bool many : {false, true}) {
          auto src = std::make_shared<test::CountingSource>(open_session_source(dir),
                                                            std::chrono::milliseconds(ms));
          Reader r(std::shared_ptr<const SessionSource>(src), "", 16);
          r.set_threads(t);
          src->reset();
          const auto t0 = std::chrono::steady_clock::now();
          // The loop KEEPS its rows, as a caller that needs them all does.
          std::vector<std::vector<sf8>> kept;
          ChannelRows<sf8> rows;
          if (many)
            rows = r.read_many(names, START + 30'000'000, START + 30'000'000 + window);
          else
            for (const auto& c : names)
              kept.push_back(r.read(c, START + 30'000'000, START + 30'000'000 + window));
          const double secs =
              std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
          std::printf("%3d ch  %2d ms/request  n_threads=%2d  %-5s %8.3f s  requests=%ld\n",
                      channels, ms, t, many ? "list" : "loop", secs, src->calls().total());
          std::fflush(stdout);
        }
    }
    fsys::remove_all(dir);
  }
}

namespace {
std::string first_file(const std::string& dir, const std::string& ch, int seg,
                       const std::string& ext) {
  char buf[16];
  std::snprintf(buf, sizeof buf, "%06d", seg);
  const std::string stem = ch + "-" + buf;
  return dir + "/" + ch + ".timd/" + stem + ".segd/" + stem + ext;
}
template <class F>
std::string error_of(F&& fn) {
  try {
    fn();
  } catch (const std::exception& e) {
    return std::string(typeid(e).name()) + ": " + e.what();
  }
  return "";
}
}  // namespace

TEST_CASE("read_many: the lowest failing channel in list order wins, across stages",
          "[read-many]") {
  // `mixed` fails while DECODING (a block's payload is corrupt: CRC), `gappy`
  // while reading its INDEX (segment 1's .tidx is truncated). A loop raises
  // whichever comes first in the list; so must the list read.
  const auto dir = write_session("mef3io_read_many_stages.mefd");
  {
    std::fstream f(first_file(dir, "mixed", 0, ".tdat"),
                   std::ios::in | std::ios::out | std::ios::binary);
    f.seekp(1024 + 304 + 20);
    const char junk[4] = {'\x55', '\x2a', '\x13', '\x77'};
    f.write(junk, 4);
  }
  fsys::resize_file(first_file(dir, "gappy", 1, ".tidx"), 1024 + 56 + 10);
  for (int t : {1, 2, 8}) {
    Reader r(dir, "", t);
    const auto mixed = error_of([&] { r.read("mixed"); });
    const auto gappy = error_of([&] { r.read("gappy"); });
    REQUIRE(!mixed.empty());
    REQUIRE(!gappy.empty());
    REQUIRE(mixed != gappy);
    CHECK(error_of([&] { r.read_many({"mixed", "gappy"}); }) == mixed);
    CHECK(error_of([&] { r.read_many({"gappy", "mixed"}); }) == gappy);
    CHECK(error_of([&] { r.read_many({"plain", "late", "mixed", "gappy"}); }) == mixed);
    CHECK(error_of([&] { r.read_many({"plain", "gappy", "late", "mixed"}); }) == gappy);
  }
  fsys::remove_all(dir);
}

TEST_CASE("read_many stops early: a damaged first channel fetches no data", "[read-many]") {
  const auto dir = write_session("mef3io_read_many_early.mefd");
  fsys::resize_file(first_file(dir, "gappy", 0, ".tidx"), 1024 + 56 + 10);
  for (int t : {1, 2, 8}) {
    auto src = std::make_shared<test::CountingSource>(open_session_source(dir));
    Reader r(std::shared_ptr<const SessionSource>(src), "", t);
    src->reset();
    CHECK_THROWS_AS(r.read("gappy"), FormatError);
    const auto single = src->calls();
    src->reset();
    CHECK_THROWS_AS(r.read_many({"gappy", "mixed", "late", "plain", "mixed", "late"}),
                    FormatError);
    const auto many = src->calls();
    CHECK(single.read_range == 0);
    CHECK(many.read_range == 0);  // no channel's data was fetched
    // at most the indexes of the channels already in flight
    CHECK(many.read_all <= single.read_all + static_cast<long>(t) * 2);
  }
  fsys::remove_all(dir);
}
