// mef3io — cancellation and progress (#69): the reporter's guarantees, the
// Cancelled type, and "a cancelled call leaves nothing behind" for the core
// operations and the C ABI.
#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <set>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "mef3io/c_api.h"
#include "mef3io/control.hpp"
#include "mef3io/parallel.hpp"
#include "mef3io/reader.hpp"
#include "mef3io/recover.hpp"
#include "mef3io/session_writer.hpp"
#include "mef3io/tar.hpp"
#include "mef3io/validate.hpp"

using namespace mef3io;
namespace fsys = std::filesystem;

namespace {

fsys::path fresh_dir(const std::string& name) {
  const auto dir = fsys::temp_directory_path() / name;
  fsys::remove_all(dir);
  fsys::create_directories(dir);
  return dir;
}

// Two channels x two segments, enough blocks and bytes for several 1 MB chunks.
std::string write_session(const fsys::path& dir) {
  const std::string path = (dir / "s.mefd").string();
  SessionWriter w(path, true);
  std::mt19937 rng(69);
  std::uniform_int_distribution<si4> dist(-30000, 30000);
  const si8 start = 1600000000000000;
  for (const char* ch : {"a", "b"}) {
    for (int s = 0; s < 2; ++s) {
      std::vector<si4> x(300000);
      for (auto& v : x) v = dist(rng);
      w.write_int32(ch, x, 1.0, start + s * 400000000LL, 1000.0, {}, s > 0);
    }
  }
  return path;
}

std::set<std::string> listing(const fsys::path& dir) {
  std::set<std::string> out;
  for (const auto& e : fsys::directory_iterator(dir)) out.insert(e.path().filename().string());
  return out;
}

struct Calls {
  std::vector<std::pair<std::uint64_t, std::uint64_t>> v;
  ProgressFn fn() {
    return [this](std::uint64_t d, std::uint64_t t) { v.emplace_back(d, t); };
  }
  void check_complete() const {
    REQUIRE(!v.empty());
    const auto total = v.front().second;
    CHECK(v.front().first == 0);
    CHECK(v.back() == std::make_pair(total, total));
    std::uint64_t prev = 0, at_total = 0;
    for (const auto& [d, t] : v) {
      CHECK(t == total);
      CHECK(d >= prev);
      CHECK(d <= total);
      prev = d;
      if (d == total) ++at_total;
    }
    CHECK((at_total == 1 || total == 0));
  }
};

}  // namespace

TEST_CASE("Cancelled cannot be swallowed by catch (std::exception&)") {
  STATIC_REQUIRE(!std::is_base_of_v<std::exception, Cancelled>);
  bool caught_as_std = false, caught = false;
  try {
    try {
      Control ctl;
      CancelToken t;
      ctl.cancel = &t;
      t.cancel();
      ctl.check();
    } catch (const std::exception&) {
      caught_as_std = true;
    }
  } catch (const Cancelled&) {
    caught = true;
  }
  CHECK(!caught_as_std);
  CHECK(caught);
}

TEST_CASE("ProgressReporter: start, bounded, monotonic, finish once") {
  Calls calls;
  Control ctl;
  ctl.progress = calls.fn();
  const std::uint64_t total = 1000000;
  detail::ProgressReporter p(ctl, total);
  p.start();
  for (int i = 0; i < 100000; ++i) p.add(10);  // reaches exactly total
  p.finish();
  calls.check_complete();
  // ~1% steps: at most ~100 intermediate reports however many adds there were.
  CHECK(calls.v.size() <= 110);
  CHECK(calls.v.size() >= 50);

  SECTION("an unused reporter never calls anything and costs nothing") {
    Control none;
    detail::ProgressReporter q(none, total);
    CHECK(!q.active());
    q.start();
    q.add(5);
    q.advance_to(10);
    q.finish();
  }
}

TEST_CASE("ProgressReporter is safe from many threads and never re-enters") {
  std::atomic<int> inside{0}, max_inside{0};
  std::vector<std::uint64_t> seen;
  Control ctl;
  ctl.progress = [&](std::uint64_t d, std::uint64_t) {
    const int now = ++inside;
    int m = max_inside.load();
    while (now > m && !max_inside.compare_exchange_weak(m, now)) {
    }
    seen.push_back(d);
    std::this_thread::sleep_for(std::chrono::microseconds(50));
    --inside;
  };
  const std::uint64_t total = 8 * 20000;
  detail::ProgressReporter p(ctl, total);
  p.start();
  parallel_for(8, 8, [&](std::size_t) {
    for (int i = 0; i < 20000; ++i) p.add(1);
  });
  p.finish();
  CHECK(max_inside.load() == 1);
  CHECK(std::is_sorted(seen.begin(), seen.end()));
  CHECK(seen.back() == total);
}

TEST_CASE("parallel_for stops scheduling work after the first exception") {
  std::atomic<int> ran{0};
  CHECK_THROWS_AS(parallel_for(10000, 4,
                               [&](std::size_t i) {
                                 ++ran;
                                 if (i == 0) throw Cancelled();
                               }),
                  Cancelled);
  CHECK(ran.load() < 10000);
}

TEST_CASE("A cancelled read throws and an uncancelled one is unchanged") {
  const auto dir = fresh_dir("mef3io_cancel_read");
  const auto path = write_session(dir);
  Reader r(path, "", 4);
  const auto plain = r.read_raw("a");

  Calls calls;
  CancelToken unused;
  Control ctl;
  ctl.cancel = &unused;
  ctl.progress = calls.fn();
  const auto hooked = r.read_raw("a", std::nullopt, std::nullopt, Reader::kUseDefaultThreads, ctl);
  CHECK(hooked.samples == plain.samples);
  CHECK(hooked.valid == plain.valid);
  calls.check_complete();

  CancelToken token;
  Control stop;
  stop.cancel = &token;
  stop.progress = [&](std::uint64_t d, std::uint64_t t) {
    if (d > t / 4) token.cancel();
  };
  CHECK_THROWS_AS(r.read("a", std::nullopt, std::nullopt, 1, stop), Cancelled);
  // Strict or not, the session is untouched and reads in full afterwards.
  CHECK(r.read_raw("a").samples == plain.samples);
  fsys::remove_all(dir);
}

TEST_CASE("Cancelled archive / extract / validate / repair / recover leave nothing") {
  const auto dir = fresh_dir("mef3io_cancel_ops");
  const auto path = write_session(dir);
  const auto before = listing(dir);

  CancelToken token;
  Control ctl;
  ctl.cancel = &token;

  SECTION("archive, cancelled part-way through the copy") {
    ctl.progress = [&](std::uint64_t d, std::uint64_t t) {
      if (d > 0 && d < t) token.cancel();
    };
    CHECK_THROWS_AS(archive_session(path, "", false, ctl), Cancelled);
    CHECK(listing(dir) == before);
  }
  SECTION("extract, cancelled part-way, with overwrite of an existing tree") {
    const auto tar = archive_session(path);
    const auto target = (dir / "x.mefd").string();
    fsys::create_directories(target);
    {
      std::ofstream(fsys::path(target) / "keep.txt") << "mine";
    }
    const auto with_target = listing(dir);
    ctl.progress = [&](std::uint64_t d, std::uint64_t t) {
      if (d > 0 && d < t) token.cancel();
    };
    CHECK_THROWS_AS(extract_session(tar, target, true, ctl), Cancelled);
    CHECK(listing(dir) == with_target);
    CHECK(fsys::exists(fsys::path(target) / "keep.txt"));
  }
  SECTION("validate, pre-cancelled") {
    token.cancel();
    CHECK_THROWS_AS(validate_session(path, {}, ctl), Cancelled);
  }
  SECTION("repair and recover, pre-cancelled, write nothing and back up nothing") {
    token.cancel();
    RepairSelection sel;
    sel.check_ids = {"sizing.difference-bytes"};
    CHECK_THROWS_AS(repair_session(path, sel, {}, ctl), Cancelled);
    CHECK_THROWS_AS(recover_session(path, true, true, "", ctl), Cancelled);
    CHECK(listing(dir) == before);
  }
  fsys::remove_all(dir);
}

TEST_CASE("C ABI: cancel token, progress callback, MEF3IO_ERR_CANCELLED") {
  const auto dir = fresh_dir("mef3io_cancel_capi");
  const auto path = write_session(dir);
  const auto before = listing(dir);

  CHECK(mef3io_cancel_token_is_cancelled(nullptr) == 0);
  mef3io_cancel_token_cancel(nullptr);  // ignored
  mef3io_cancel_token* token = mef3io_cancel_token_new();
  REQUIRE(token);
  CHECK(mef3io_cancel_token_is_cancelled(token) == 0);

  struct State {
    std::uint64_t last = 0, total = 0;
    int calls = 0;
    mef3io_cancel_token* cancel_at_half = nullptr;
  } st;
  auto cb = [](std::uint64_t done, std::uint64_t total, void* user) {
    auto* s = static_cast<State*>(user);
    ++s->calls;
    s->last = done;
    s->total = total;
    if (s->cancel_at_half && done > total / 2) mef3io_cancel_token_cancel(s->cancel_at_half);
  };

  char out[1024];
  REQUIRE(mef3io_archive_session_ex(path.c_str(), nullptr, 0, out, sizeof out, token, cb, &st) ==
          MEF3IO_OK);
  CHECK(st.calls >= 2);
  CHECK(st.last == st.total);
  fsys::remove(out);

  st = State{};
  st.cancel_at_half = token;
  CHECK(mef3io_archive_session_ex(path.c_str(), nullptr, 0, out, sizeof out, token, cb, &st) ==
        MEF3IO_ERR_CANCELLED);
  CHECK(mef3io_cancel_token_is_cancelled(token) == 1);
  CHECK(listing(dir) == before);

  mef3io_reader* r = nullptr;
  REQUIRE(mef3io_reader_open(path.c_str(), "", 2, &r) == MEF3IO_OK);
  int64_t n = 0;
  REQUIRE(mef3io_reader_read_size(r, "a", MEF3IO_TIME_UNSET, MEF3IO_TIME_UNSET, &n) == MEF3IO_OK);
  std::vector<double> buf(static_cast<std::size_t>(n));
  int64_t got = 0;
  CHECK(mef3io_reader_read_ex(r, "a", MEF3IO_TIME_UNSET, MEF3IO_TIME_UNSET, buf.data(), n, &got,
                              token, nullptr, nullptr) == MEF3IO_ERR_CANCELLED);
  st = State{};
  CHECK(mef3io_reader_read_ex(r, "a", MEF3IO_TIME_UNSET, MEF3IO_TIME_UNSET, buf.data(), n, &got,
                              nullptr, cb, &st) == MEF3IO_OK);
  CHECK(got == n);
  CHECK(st.last == st.total);
  std::vector<int32_t> s(static_cast<std::size_t>(n));
  std::vector<uint8_t> v(static_cast<std::size_t>(n));
  CHECK(mef3io_reader_read_raw_ex(r, "a", MEF3IO_TIME_UNSET, MEF3IO_TIME_UNSET, s.data(),
                                  v.data(), n, &got, nullptr, token, nullptr, nullptr) ==
        MEF3IO_ERR_CANCELLED);
  mef3io_reader_close(r);

  int64_t segs = -1;
  CHECK(mef3io_recover_session_ex(path.c_str(), 1, 1, "", nullptr, 0, &segs, token, nullptr,
                                  nullptr) == MEF3IO_ERR_CANCELLED);
  CHECK(listing(dir) == before);

  const auto tar = archive_session(path);
  const auto target = (dir / "y.mefd").string();
  CHECK(mef3io_extract_session_ex(tar.c_str(), target.c_str(), 0, out, sizeof out, token, nullptr,
                                  nullptr) == MEF3IO_ERR_CANCELLED);
  CHECK(!fsys::exists(target));

  mef3io_cancel_token_free(token);
  mef3io_cancel_token_free(nullptr);
  fsys::remove_all(dir);
}
