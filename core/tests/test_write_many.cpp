// mef3io — multi-channel write (#165) and flush="close" (#166).
//
// write_int32_many / write_float_many are a PERFORMANCE API: byte-identical
// to a loop of per-channel calls (fresh segments differ only in their random
// UUIDs, and the CRCs that cover them), with ONE combined .tmet barrier per
// call and all-or-nothing behaviour on a failure before publication.
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "../src/durability.hpp"
#include "mef3io/errors.hpp"
#include "mef3io/session_writer.hpp"

namespace fs = std::filesystem;
using namespace mef3io;

namespace {

struct Scratch {
  fs::path dir;
  explicit Scratch(const char* name) {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    dir = fs::temp_directory_path() / (std::string("mef3io_many_") + name + "_" + std::to_string(nonce));
    fs::create_directories(dir);
  }
  ~Scratch() {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
  Scratch(const Scratch&) = delete;
  Scratch& operator=(const Scratch&) = delete;
};

bool ends_with(const std::string& s, const std::string& suf) {
  return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

std::vector<unsigned char> slurp(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

// relative path -> bytes, for every file under a session
std::map<std::string, std::vector<unsigned char>> snapshot(const fs::path& root) {
  std::map<std::string, std::vector<unsigned char>> out;
  for (const auto& e : fs::recursive_directory_iterator(root))
    if (e.is_regular_file()) out[fs::relative(e.path(), root).string()] = slurp(e.path());
  return out;
}

// Universal-header bytes that legitimately differ between two writes of the
// same data: the CRCs over the random UUIDs (0..8) and the three UUIDs.
bool random_header_byte(const std::string& rel, std::size_t i) {
  const bool uh = ends_with(rel, ".tdat") || ends_with(rel, ".tidx") || ends_with(rel, ".tmet");
  return uh && (i < 8 || (i >= 820 && i < 868));
}

void require_same_but_uuids(const fs::path& a, const fs::path& b) {
  const auto sa = snapshot(a), sb = snapshot(b);
  REQUIRE(sa.size() == sb.size());
  for (const auto& [rel, bytes] : sa) {
    REQUIRE(sb.count(rel) == 1);
    const auto& other = sb.at(rel);
    REQUIRE(bytes.size() == other.size());
    for (std::size_t i = 0; i < bytes.size(); ++i)
      if (bytes[i] != other[i] && !random_header_byte(rel, i)) FAIL(rel << " differs at byte " << i);
  }
}

std::vector<si4> walk(std::size_t n, unsigned seed) {
  std::vector<si4> v(n);
  si4 x = 0;
  unsigned s = seed;
  for (auto& e : v) {
    s = s * 1664525u + 1013904223u;
    x += static_cast<si4>((s >> 16) % 101) - 50;
    e = x;
  }
  return v;
}

std::mutex g_mu;
std::vector<std::vector<std::string>> g_batches;
std::atomic<int> g_fsyncs{0};

struct Observers {
  Observers() {
    g_batches.clear();
    g_fsyncs = 0;
    detail::flush_batch_observer = [](const std::vector<fs::path>& files) {
      std::vector<std::string> names;
      for (const auto& f : files) names.push_back(f.string());
      std::lock_guard<std::mutex> lk(g_mu);
      g_batches.push_back(std::move(names));
    };
    detail::fsync_observer = [](const std::string&) { ++g_fsyncs; };
  }
  ~Observers() {
    detail::flush_batch_observer = nullptr;
    detail::fsync_observer = nullptr;
  }
  Observers(const Observers&) = delete;
  Observers& operator=(const Observers&) = delete;
  static std::vector<std::vector<std::string>> take() {
    std::lock_guard<std::mutex> lk(g_mu);
    auto out = std::move(g_batches);
    g_batches.clear();
    return out;
  }
};

std::size_t count_suffix(const std::vector<std::string>& v, const char* suf) {
  return static_cast<std::size_t>(
      std::count_if(v.begin(), v.end(), [&](const std::string& p) { return ends_with(p, suf); }));
}

const si8 kStart = 1577836800000000;
const sf8 kRate = 512.0;
const std::size_t kN = 512 * 30;  // 30 s per round
si8 round_start(int r) { return kStart + static_cast<si8>(r) * 30'000'000; }

}  // namespace

TEST_CASE("write_int32_many == a loop of write_int32 (any thread count)", "[write_many]") {
  const std::vector<std::string> chs = {"ch1", "ch2", "ch3", "ch4", "ch5"};
  const std::size_t nc = chs.size();
  for (int threads : {1, 4}) {
    const Scratch dir("identity");
    fs::create_directories(dir.dir / "loop");
    fs::create_directories(dir.dir / "many");
    const auto p_loop = (dir.dir / "loop" / "s.mefd").string();
    const auto p_many = (dir.dir / "many" / "s.mefd").string();
    {
      SessionWriter a(p_loop, true), b(p_many, true);
      a.set_threads(threads);
      b.set_threads(threads);
      for (int r = 0; r < 3; ++r) {  // a fresh segment, then two in-segment appends
        std::vector<si4> block(nc * kN);
        for (std::size_t c = 0; c < nc; ++c) {
          const auto row = walk(kN, static_cast<unsigned>(c * 10 + r));
          std::copy(row.begin(), row.end(), block.begin() + static_cast<std::ptrdiff_t>(c * kN));
        }
        for (std::size_t c = 0; c < nc; ++c)
          a.write_int32(chs[c], std::span<const si4>(block).subspan(c * kN, kN),
                        0.25 * static_cast<sf8>(c + 1), round_start(r), kRate);
        std::vector<sf8> ufacts;
        for (std::size_t c = 0; c < nc; ++c) ufacts.push_back(0.25 * static_cast<sf8>(c + 1));
        const auto sums = b.write_int32_many(chs, block, ufacts, round_start(r), kRate);
        REQUIRE(sums.size() == nc);
        for (const auto& s : sums) {
          REQUIRE(s.samples_written == static_cast<si8>(kN));
          REQUIRE(s.segment == 0);
        }
      }
    }
    require_same_but_uuids(p_loop, p_many);
  }
}

TEST_CASE("write_float_many == a loop of write_float (NaN gaps, per-channel precision)",
          "[write_many]") {
  const std::vector<std::string> chs = {"a", "b", "c"};
  const Scratch dir("float");
  fs::create_directories(dir.dir / "loop");
  fs::create_directories(dir.dir / "many");
  const auto p_loop = (dir.dir / "loop" / "s.mefd").string();
  const auto p_many = (dir.dir / "many" / "s.mefd").string();
  std::vector<sf8> block(chs.size() * kN);
  for (std::size_t c = 0; c < chs.size(); ++c)
    for (std::size_t i = 0; i < kN; ++i) {
      // different dynamic range per channel -> a different inferred precision
      block[c * kN + i] = std::sin(0.01 * static_cast<sf8>(i)) * std::pow(10.0, static_cast<sf8>(c) - 1);
      if (i % 3000 > 2900) block[c * kN + i] = std::nan("");  // gaps
    }
  {
    SessionWriter a(p_loop, true), b(p_many, true);
    b.set_threads(3);
    for (int r = 0; r < 2; ++r) {
      for (std::size_t c = 0; c < chs.size(); ++c)
        a.write_float(chs[c], std::span<const sf8>(block).subspan(c * kN, kN), round_start(r),
                      kRate);
      const auto sums = b.write_float_many(chs, block, round_start(r), kRate);
      REQUIRE(sums.size() == chs.size());
      REQUIRE(sums[0].units_conversion_factor != sums[2].units_conversion_factor);
      REQUIRE(sums[0].gaps_skipped > 0);
    }
  }
  require_same_but_uuids(p_loop, p_many);
}

TEST_CASE("a multi-channel call pays ONE combined wait (default mode)", "[write_many]") {
  const Scratch dir("barrier");
  const auto session = (dir.dir / "s.mefd").string();
  const std::vector<std::string> chs = {"ch1", "ch2", "ch3", "ch4"};
  std::vector<si4> block(chs.size() * kN, 7);
  const std::vector<sf8> uf = {0.5};
  SessionWriter w(session, true);
  w.set_threads(3);
  const Observers obs;
  w.write_int32_many(chs, block, uf, round_start(0), kRate);  // first segments
  auto got = Observers::take();
  REQUIRE(got.size() == 1);
  REQUIRE(count_suffix(got[0], ".tmet.mef3io-tmp") == 4);
  w.write_int32_many(chs, block, uf, round_start(1), kRate);  // appends
  got = Observers::take();
  REQUIRE(got.size() == 1);
  REQUIRE(count_suffix(got[0], ".tmet.mef3io-tmp") == 4);
  REQUIRE(g_fsyncs == 0);  // no per-channel fsync at all
  // the loop pays one wait per channel
  for (const auto& c : chs)
    w.write_int32(c, std::span<const si4>(block).subspan(0, kN), 0.5, round_start(2), kRate);
  REQUIRE(g_fsyncs == 4);
}

TEST_CASE("wait_for_disk: a multi-channel call is two waits, files included", "[write_many]") {
  const Scratch dir("wait");
  const auto session = (dir.dir / "s.mefd").string();
  const std::vector<std::string> chs = {"ch1", "ch2", "ch3"};
  std::vector<si4> block(chs.size() * kN, 3);
  const std::vector<sf8> uf = {0.5};
  SessionWriter w(session, true);
  w.set_wait_for_disk(true);
  const Observers obs;
  for (int r = 0; r < 2; ++r) {
    w.write_int32_many(chs, block, uf, round_start(r), kRate);
    const auto got = Observers::take();
    REQUIRE(got.size() == 2);  // before the renames (data + temps), then the published names
    REQUIRE(count_suffix(got[0], ".tdat") == 3);
    REQUIRE(count_suffix(got[0], ".tidx") == 3);
    REQUIRE(count_suffix(got[0], ".tmet.mef3io-tmp") == 3);
    REQUIRE(count_suffix(got[1], ".tmet") == 3);
  }
  w.flush();
  REQUIRE(Observers::take().empty());  // nothing owed
}

TEST_CASE("a failure before publication rolls EVERY channel back", "[write_many]") {
  const Scratch dir("rollback");
  const auto session = (dir.dir / "s.mefd").string();
  const std::vector<std::string> chs = {"ch1", "ch2", "ch3"};
  std::vector<si4> block(chs.size() * kN, 11);
  const std::vector<sf8> uf = {0.5};
  SessionWriter w(session, true);
  w.set_threads(3);
  w.write_int32_many(chs, block, uf, round_start(0), kRate);
  w.flush();

  SECTION("an append conflict found while writing (ch3 already ahead)") {
    w.write_int32("ch3", std::span<const si4>(block).subspan(0, kN), 0.5, round_start(1), kRate);
    w.flush();
    const auto before = snapshot(session);
    REQUIRE_THROWS_AS(w.write_int32_many(chs, block, uf, round_start(1), kRate), WriteConflictError);
    REQUIRE(snapshot(session) == before);  // ch1, ch2 appended in memory of the call: undone
    // and the writer is still usable
    w.write_int32("ch1", std::span<const si4>(block).subspan(0, kN), 0.5, round_start(1), kRate);
  }

  SECTION("the combined flush fails") {
    const auto before = snapshot(session);
    detail::flush_batch_observer = [](const std::vector<fs::path>&) {
      throw IoError("could not flush to disk, the write is not durable: injected");
    };
    REQUIRE_THROWS_AS(w.write_int32_many(chs, block, uf, round_start(1), kRate), IoError);
    detail::flush_batch_observer = nullptr;
    REQUIRE(snapshot(session) == before);  // no temp file left either
  }

  SECTION("new channels: the failed call leaves no folder behind") {
    const std::vector<std::string> fresh = {"n1", "n2"};
    std::vector<si4> b2(fresh.size() * kN, 5);
    const auto before = snapshot(session);
    detail::flush_batch_observer = [](const std::vector<fs::path>&) {
      throw IoError("could not flush to disk, the write is not durable: injected");
    };
    REQUIRE_THROWS_AS(w.write_int32_many(fresh, b2, uf, round_start(0), kRate), IoError);
    detail::flush_batch_observer = nullptr;
    REQUIRE(snapshot(session) == before);
    REQUIRE_FALSE(fs::exists(fs::path(session) / "n1.timd"));
  }
}

TEST_CASE("a refused multi-channel call writes nothing", "[write_many]") {
  const Scratch dir("refuse");
  const auto session = (dir.dir / "s.mefd").string();
  std::vector<si4> block(2 * kN, 1);
  const std::vector<sf8> uf = {0.5};
  SessionWriter w(session, true);
  REQUIRE_THROWS_AS(w.write_int32_many({"x", "x"}, block, uf, round_start(0), kRate),
                    std::invalid_argument);
  REQUIRE_THROWS_AS(w.write_int32_many({"x", "y"}, block, uf, round_start(0), 0.0),
                    std::invalid_argument);
  REQUIRE_THROWS_AS(w.write_int32_many({"x", "y"}, block, std::vector<sf8>{0.5, 0.5, 0.5},
                                       round_start(0), kRate),
                    std::invalid_argument);
  block.push_back(1);
  REQUIRE_THROWS_AS(w.write_int32_many({"x", "y", "z"}, block, uf, round_start(0), kRate),
                    std::invalid_argument);  // not a whole number of rows
  REQUIRE_FALSE(fs::exists(fs::path(session) / "x.timd"));
  REQUIRE(w.write_int32_many({}, {}, uf, round_start(0), kRate).empty());
}

TEST_CASE("flush=close: no flush while writing, ONE combined batch at close", "[write_many]") {
  const Scratch dir("close");
  const auto session = (dir.dir / "s.mefd").string();
  const std::vector<std::string> chs = {"ch1", "ch2", "ch3"};
  std::vector<si4> block(chs.size() * kN, 9);
  const std::vector<sf8> uf = {0.5};
  const Observers obs;
  {
    SessionWriter w(session, true);
    w.set_flush_mode(FlushMode::Close);
    REQUIRE_THROWS_AS(w.set_wait_for_disk(true), std::invalid_argument);
    w.write_int32_many(chs, block, uf, round_start(0), kRate);
    w.write_int32_many(chs, block, uf, round_start(1), kRate);
    for (const auto& c : chs)  // per-channel appends too
      w.write_int32(c, std::span<const si4>(block).subspan(0, kN), 0.5, round_start(2), kRate);
    w.write_float("fl", std::vector<sf8>(kN, 1.5), round_start(0), kRate);
    w.write_float("fl", std::vector<sf8>(kN, 2.5), round_start(1), kRate);
    REQUIRE(Observers::take().empty());
    REQUIRE(g_fsyncs == 0);
  }  // close
  const auto got = Observers::take();
  REQUIRE(got.size() == 1);
  REQUIRE(count_suffix(got[0], ".tmet") == 4);
  REQUIRE(count_suffix(got[0], ".tdat") == 4);
  REQUIRE(g_fsyncs == 0);
  // wait_for_disk first, then close mode: refused the other way round too
  SessionWriter w2((dir.dir / "t.mefd").string(), true);
  w2.set_wait_for_disk(true);
  REQUIRE_THROWS_AS(w2.set_flush_mode(FlushMode::Close), std::invalid_argument);
  // the default is unchanged: a channel's first segment and an append fsync their .tmet
  SessionWriter w3((dir.dir / "u.mefd").string(), true);
  w3.write_int32("a", std::span<const si4>(block).subspan(0, kN), 0.5, round_start(0), kRate);
  w3.write_int32("a", std::span<const si4>(block).subspan(0, kN), 0.5, round_start(1), kRate);
  REQUIRE(g_fsyncs == 2);
}
