// What opening a session costs in file-system requests (#141). On a network
// share every request is a round trip (~110 ms reported on SMB), so the
// per-segment request count of the open is pinned here through a counting
// SessionSource decorator: a new per-segment request cannot slip in unnoticed.
//
// The count is the LIBRARY's, not the operating system's: a listing is one
// list_dir() here but opendir + getdents + close (+ a stat per entry on some
// file systems) below. Replacing three exists() per segment by one listing
// per .segd halved this count and ADDED syscalls (strace), so it was reverted;
// see for_agents/notes/threads-gil-concurrency.md. A count pin also cannot see a call that got more expensive
// (a tar list_dir that scanned the whole archive made the opens quadratic),
// hence the scaling cases at the end.
//
// The "[.open-bench]" case is hidden: it adds a fixed delay per request and
// prints open times.
//   build-core/mef3io_tests "[.open-bench]"
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <fstream>
#include <string>
#include <vector>

#include "counting_source.hpp"
#include "mef3io/errors.hpp"
#include "mef3io/reader.hpp"
#include "mef3io/session.hpp"
#include "mef3io/session_writer.hpp"
#include "mef3io/tar.hpp"

using namespace mef3io;
using mef3io::test::CountingSource;
namespace fsys = std::filesystem;

namespace {
constexpr si8 START = 1577836800000000;

std::string write_session(const std::string& name, int channels, int segments) {
  const auto dir = fsys::temp_directory_path() / name;
  fsys::remove_all(dir);
  SessionWriter w(dir.string(), true);
  std::vector<si4> a(256);
  for (int i = 0; i < 256; ++i) a[i] = i;
  for (int c = 0; c < channels; ++c)
    for (int s = 0; s < segments; ++s)
      w.write_int32("ch" + std::to_string(c), a, 1.0, START + s * 2'000'000LL, 256.0, {}, s > 0);
  return dir.string();
}

std::shared_ptr<CountingSource> counted(const std::string& path,
                                        std::chrono::microseconds delay = {}) {
  return std::make_shared<CountingSource>(open_session_source(path), delay);
}

std::string segd(const std::string& session, const std::string& ch, int n) {
  char buf[16];
  std::snprintf(buf, sizeof buf, "%06d", n);
  return session + "/" + ch + ".timd/" + ch + "-" + buf + ".segd";
}
std::string stem(const std::string& ch, int n) {
  char buf[16];
  std::snprintf(buf, sizeof buf, "%06d", n);
  return ch + "-" + buf;
}
}  // namespace

TEST_CASE("opening a session: requests per channel and per segment", "[open-calls]") {
  const int C = 3, S = 4;
  const auto path = write_session("mef3io_open_calls.mefd", C, S);
  // root + every .timd listed; three existence checks and ONE bounded .tmet
  // read per segment; the index stays lazy; no size probes.
  auto check = [&](const mef3io::test::SourceCalls& c) {
    CHECK(c.list_dir == 1 + C);
    CHECK(c.exists == 3 * C * S);
    CHECK(c.read_head == C * S);
    CHECK(c.read_all == 0);
    CHECK(c.read_range == 0);
    CHECK(c.file_size == 0);
  };
  SECTION("Session, directory") {
    auto src = counted(path);
    Session s{std::shared_ptr<const SessionSource>(src)};
    REQUIRE(s.channels().size() == C);
    check(src->calls());
  }
  SECTION("Reader, directory") {
    auto src = counted(path);
    Reader r{std::shared_ptr<const SessionSource>(src)};
    check(src->calls());
  }
  SECTION("Session, tar archive") {
    const auto tar = archive_session(path, "", true);
    auto src = std::make_shared<CountingSource>(std::make_shared<TarSource>(tar));
    Session s{std::shared_ptr<const SessionSource>(src)};
    REQUIRE(s.channels().size() == C);
    check(src->calls());
    fsys::remove(tar);
  }
}

TEST_CASE("the .tmet read is bounded by the fixed record size", "[open-calls]") {
  const auto path = write_session("mef3io_open_tmet.mefd", 1, 2);
  const auto tmet = segd(path, "ch0", 0) + "/" + stem("ch0", 0) + ".tmet";

  SECTION("foreign padding past the record is neither read nor hashed") {
    { std::ofstream(tmet, std::ios::binary | std::ios::app) << std::string(5000, '\x7e'); }
    REQUIRE(fsys::file_size(tmet) == fmt::METADATA_FILE_BYTES + 5000);
    const auto src = open_session_source(path);
    const std::string rel = "ch0.timd/" + stem("ch0", 0) + ".segd/" + stem("ch0", 0) + ".tmet";
    REQUIRE(src->read_head(rel, fmt::METADATA_FILE_BYTES).size() == fmt::METADATA_FILE_BYTES);
    Session s(path);
    REQUIRE(s.channel_info("ch0").number_of_samples == 512);
  }
  SECTION("a short file fails exactly as before") {
    fsys::resize_file(tmet, 1000);
    try {
      Session s(path);
      FAIL("a truncated .tmet opened");
    } catch (const FormatError& e) {
      REQUIRE(std::string(e.what()).find("expected 16384 bytes, got 1000") != std::string::npos);
    }
    Session lenient(path, "", false);
    REQUIRE(lenient.problems().size() == 1);
    REQUIRE(lenient.problems()[0].reason.find("got 1000") != std::string::npos);
  }
  SECTION("the tar source returns the same bounded bytes") {
    const auto tar = archive_session(path, "", true);
    TarSource t(tar);
    const std::string rel = "ch0.timd/" + stem("ch0", 0) + ".segd/" + stem("ch0", 0) + ".tmet";
    REQUIRE(t.read_head(rel, 100) == t.read_range(rel, 0, 100));
    REQUIRE(t.read_head(rel, 1 << 20).size() == fmt::METADATA_FILE_BYTES);
    fsys::remove(tar);
  }
}

TEST_CASE("open cost with a per-request delay", "[.open-bench]") {
  for (int channels : {64, 209})
    for (int segments : {1, 10}) {
      const auto path = write_session("mef3io_open_bench.mefd", channels, segments);
      for (int ms : {0, 20}) {
        auto src = counted(path, std::chrono::milliseconds(ms));
        const auto t0 = std::chrono::steady_clock::now();
        Session s{std::shared_ptr<const SessionSource>(src)};
        const double secs =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const auto c = src->calls();
        std::printf("open %3d ch x %2d seg, %2d ms/request: %8.3f s  requests=%ld "
                    "(list_dir=%ld exists=%ld file_size=%ld read_all=%ld read_head=%ld "
                    "read_range=%ld)\n",
                    channels, segments, ms, secs, c.total(), c.list_dir, c.exists, c.file_size,
                    c.read_all, c.read_head, c.read_range);
      }
      fsys::remove_all(path);
    }
}

namespace {
double best_of_3(const std::function<void()>& fn) {
  double best = 1e30;
  for (int i = 0; i < 3; ++i) {
    const auto t0 = std::chrono::steady_clock::now();
    fn();
    best = std::min(best,
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
  }
  return best;
}

// The median of `n` runs: one run is a wall-clock ratio of single measurements and
// a noisy neighbour (a CI VM, a busy laptop) can spoil any one of them; the median
// of several is stable while a real slowdown of every run still shows.
double median_of(int n, const std::function<void()>& fn) {
  std::vector<double> t;
  for (int i = 0; i < n; ++i) {
    const auto t0 = std::chrono::steady_clock::now();
    fn();
    t.push_back(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
  }
  std::sort(t.begin(), t.end());
  return t[t.size() / 2];
}

// Every folder of a tar session lists exactly as the same folder on disk.
void require_same_listings(const SessionSource& dir, const SessionSource& tar,
                           const std::string& rel = "") {
  auto a = dir.list_dir(rel), b = tar.list_dir(rel);
  auto by_name = [](const SourceDirEntry& x, const SourceDirEntry& y) { return x.name < y.name; };
  std::sort(a.begin(), a.end(), by_name);
  REQUIRE(a.size() == b.size());
  for (std::size_t i = 0; i < a.size(); ++i) {
    CAPTURE(rel, a[i].name);
    REQUIRE(a[i].name == b[i].name);
    REQUIRE(a[i].is_dir == b[i].is_dir);
    if (a[i].is_dir) require_same_listings(dir, tar, rel.empty() ? a[i].name : rel + "/" + a[i].name);
  }
}
}  // namespace

TEST_CASE("TarSource::list_dir lists the children only, in sorted order", "[open-calls][tar]") {
  const auto path = write_session("mef3io_tar_listing.mefd", 3, 3);
  { std::ofstream(path + "/ch1.timd/ch1.rdat") << "x"; }
  const auto tar = archive_session(path, "", true);
  DirectorySource dir(path);
  TarSource t(tar);
  require_same_listings(dir, t);
  REQUIRE_THROWS_AS(t.list_dir("nope.timd"), IoError);
#ifndef _WIN32
  // A foreign archive: "./" prefixes and NO directory members, so every
  // folder is implied by the file paths.
  const auto foreign = fsys::temp_directory_path() / "mef3io_tar_foreign.mefd.tar";
  const auto parent = fsys::path(path).parent_path().string();
  const auto name = fsys::path(path).filename().string();
  const std::string cmd = "cd '" + parent + "' && find './" + name +
                          "' -type f | tar --no-recursion -cf '" + foreign.string() +
                          "' -T - 2>/dev/null";
  if (std::system(cmd.c_str()) == 0) {
    TarSource f(foreign.string());
    require_same_listings(dir, f);
    fsys::remove(foreign);
  }
#endif
  fsys::remove(tar);
}

TEST_CASE("TarSource::list_dir does not scale with the archive", "[open-calls][tar]") {
  // It once scanned every member of the archive per call, so an open that
  // lists each channel folder cost O(channels x members).
  auto per_call = [](int segments) {
    const auto path = write_session("mef3io_tar_scale.mefd", 1, segments);
    const auto tar = archive_session(path, "", true);
    TarSource t(tar);
    const double secs = best_of_3([&] {
      for (int i = 0; i < 2000; ++i) t.list_dir("ch0.timd/" + stem("ch0", i % segments) + ".segd");
    });
    fsys::remove(tar);
    fsys::remove_all(path);
    return secs / 2000;
  };
  const double small = per_call(100), big = per_call(800);  // 8x the members
  CAPTURE(small, big);
  CHECK(big < 3 * small + 20e-6);  // a scan per call would be ~8x; slack for noisy CI VMs
}

TEST_CASE("an open scales linearly in segments, directory and tar", "[open-calls]") {
  // A count pin cannot see a request that got MORE EXPENSIVE: listing every
  // .segd through a TarSource that scanned the whole archive per call made a
  // tar open O(segments x members) with the same request count. Timed, so
  // the bounds are loose: the per-segment cost must not grow much from S to
  // 32S, and a tar must open about as fast as the same session on disk (that
  // bug made it ~3-6x slower at 3200 segments; it is ~1.0-1.2x). ONE thread:
  // this pins the cost per request, and a parallel open would hide it behind
  // the tar index scan, which is serial (each header gives the next offset).
  // Noise (a busy CI VM, frequency changes) comes in phases longer than one
  // measurement, so single numbers, even their best or median of several, can
  // be off by 2-3x. Every round therefore times all four opens back to back,
  // the RATIOS are formed inside the round (a phase hits all four alike), and
  // the bounds apply to the median of the rounds' ratios. A real slowdown of
  // the tar open or of the per-segment cost shows in every round.
  struct Sessions { std::string dir, tar; int segments; };
  auto make = [](const char* name, int segments) {
    const auto path = write_session(name, 1, segments);
    return Sessions{path, archive_session(path, "", true), segments};
  };
  auto once = [](const std::function<void()>& fn) {
    const auto t0 = std::chrono::steady_clock::now();
    fn();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  };
  const Sessions small = make("mef3io_open_scale_small.mefd", 100);
  const Sessions big = make("mef3io_open_scale_big.mefd", 3200);
  std::vector<double> dir_growth, tar_growth, tar_vs_dir;
  for (int round = 0; round < 7; ++round) {
    const double sd = once([&] { Session s(small.dir, "", true, 1); }) / small.segments;
    const double st = once([&] { Session s(small.tar, "", true, 1); }) / small.segments;
    const double bd = once([&] { Session s(big.dir, "", true, 1); }) / big.segments;
    const double bt = once([&] { Session s(big.tar, "", true, 1); }) / big.segments;
    dir_growth.push_back(bd / sd);
    tar_growth.push_back(bt / st);
    tar_vs_dir.push_back(bt / bd);
  }
  auto median = [](std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
  };
  const double dg = median(dir_growth), tg = median(tar_growth), tv = median(tar_vs_dir);
  CAPTURE(dg, tg, tv);
  if (std::getenv("MEF3IO_PRINT_TIMINGS"))
    std::printf("RATIOS dir-growth %.3g tar-growth %.3g tar-vs-dir %.3g\n", dg, tg, tv);
  fsys::remove(small.tar);
  fsys::remove_all(small.dir);
  fsys::remove(big.tar);
  fsys::remove_all(big.dir);
  CHECK(dg < 3);   // per-segment cost of a directory open, 32x the segments
  CHECK(tg < 3);   // the same through a tar
  CHECK(tv < 2);   // a tar opens about as fast as the same session on disk
}
