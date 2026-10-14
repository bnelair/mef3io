// mef3io — which segment is a channel's LAST (core/src/last_segment.hpp).
//
// repair/recover stamp "segment-repaired" on the last segment, and the Python
// warm-start snapshot check (mef3io/cache.py `_segment_number`/`_segment_key`)
// reads the last segment's provenance. The two must agree EXACTLY on which
// folder that is, or a stamp lands where the cache never looks. The rule:
// segment number = the ASCII digits after the last '-' (else -1, also on
// overflow), compared numerically, ties broken by folder name. Pinned against
// the Python port by tests/test_p22_cache_snapshot.py with the same cases.
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <string>

#include "../src/last_segment.hpp"

namespace fs = std::filesystem;
using mef3io::detail::last_segment_dir;
using mef3io::detail::segment_number_of;

namespace {
struct Scratch {
  fs::path dir;
  explicit Scratch(const char* name) {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    dir = fs::temp_directory_path() /
          (std::string("mef3io_last_") + name + "_" + std::to_string(nonce));
    fs::create_directories(dir);
  }
  ~Scratch() {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
};
}  // namespace

TEST_CASE("segment_number_of: ASCII digits after the last dash, else -1") {
  REQUIRE(segment_number_of("ch1-000000") == 0);
  REQUIRE(segment_number_of("ch1-000123") == 123);
  REQUIRE(segment_number_of("a-b-10") == 10);
  REQUIRE(segment_number_of("ch-9") == 9);
  REQUIRE(segment_number_of("nodash") == -1);
  REQUIRE(segment_number_of("ch-") == -1);
  REQUIRE(segment_number_of("ch-12x") == -1);   // stoll would have said 12
  REQUIRE(segment_number_of("ch-+5") == -1);
  REQUIRE(segment_number_of("ch- 5") == -1);
  REQUIRE(segment_number_of("ch-99999999999999999999") == -1);  // overflow
}

TEST_CASE("last_segment_dir: numeric order, not lexicographic; name breaks ties") {
  Scratch s("order");
  for (const char* d : {"ch-9.segd", "ch-10.segd", "ch-x.segd", "notes"})
    fs::create_directories(s.dir / d);
  auto last = last_segment_dir(s.dir);
  REQUIRE(last);
  REQUIRE(last->filename().string() == "ch-10.segd");

  // Equal numbers: the larger folder name wins.
  fs::create_directories(s.dir / "zz-10.segd");
  last = last_segment_dir(s.dir);
  REQUIRE(last->filename().string() == "zz-10.segd");

  // The suffix is case-sensitive, like the reader: an upper-case folder the
  // reader ignores is not a segment here either.
  fs::create_directories(s.dir / "ch-99.SEGD");
  last = last_segment_dir(s.dir);
  REQUIRE(last->filename().string() == "zz-10.segd");
}
