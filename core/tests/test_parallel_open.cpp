// The open's metadata pass runs on the object's thread budget (#141): channel
// listings, segment file checks and every .tmet read + parse on a pool. What
// the open RETURNS must not depend on that: the same channels and infos, the
// same segment maps and declaration issues, the same problems() in the same
// order (lenient), and the same exception (strict) — the one a serial walk
// meets first, never whichever thread failed first. Pinned here for 1, 2, 8
// and 32 threads, on a directory and on a tar, with damaged and incomplete
// segments, and through a source that scrambles completion order.
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <typeinfo>
#include <atomic>
#include <cmath>
#include <stdexcept>
#include <system_error>
#include <vector>

#include "counting_source.hpp"
#include "mef3io/errors.hpp"
#include "mef3io/parallel.hpp"
#include "mef3io/reader.hpp"
#include "mef3io/session.hpp"
#include "mef3io/session_writer.hpp"
#include "mef3io/tar.hpp"

using namespace mef3io;
namespace fsys = std::filesystem;

namespace {
constexpr si8 START = 1577836800000000;
const int THREADS[] = {1, 2, 8, 32};

std::string stem(const std::string& ch, int n) {
  char buf[16];
  std::snprintf(buf, sizeof buf, "%06d", n);
  return ch + "-" + buf;
}
std::string seg_file(const std::string& session, const std::string& ch, int n,
                     const std::string& ext) {
  return session + "/" + ch + ".timd/" + stem(ch, n) + ".segd/" + stem(ch, n) + ext;
}

// Six channels x six segments, then damage:
//   a: segment 2's .tmet truncated, segment 4's .tmet body flipped (CRC)
//   b: segment 1 has no .tdat (incomplete: skipped silently)
//   c: segment 0's .tmet truncated
//   d: every segment incomplete, plus a channel .rdat (records-only)
//   e, f: intact
std::string write_damaged(const std::string& name) {
  const auto dir = (fsys::temp_directory_path() / name).string();
  fsys::remove_all(dir);
  {
    SessionWriter w(dir);
    std::vector<si4> x(500);
    for (int i = 0; i < 500; ++i) x[i] = i * 3 - 700;
    for (const char* ch : {"a", "b", "c", "d", "e", "f"})
      for (int s = 0; s < 6; ++s)
        w.write_int32(ch, x, 1.0, START + s * 4'000'000LL, 250.0, {}, s > 0);
  }
  fsys::resize_file(seg_file(dir, "a", 2, ".tmet"), 3000);
  {
    std::fstream f(seg_file(dir, "a", 4, ".tmet"), std::ios::in | std::ios::out | std::ios::binary);
    f.seekp(5000);
    f.put('\x55');
  }
  fsys::remove(seg_file(dir, "b", 1, ".tdat"));
  fsys::resize_file(seg_file(dir, "c", 0, ".tmet"), 100);
  for (int s = 0; s < 6; ++s) fsys::remove(seg_file(dir, "d", s, ".tidx"));
  { std::ofstream(dir + "/d.timd/d.rdat") << "not a record file"; }
  return dir;
}

std::string describe(const Session& s) {
  std::ostringstream o;
  o.precision(17);
  for (const auto& name : s.channels()) {
    const auto& i = s.channel_info(name);
    o << name << " fs=" << i.sampling_frequency << " uf=" << i.units_conversion_factor.value_or(-12345.0)
      << " u=" << i.units_description << " t=" << i.start_time << ".." << i.end_time
      << " n=" << i.number_of_samples << " rto=" << i.recording_time_offset
      << " segs=" << i.n_segments << " gmt=" << i.gmt_offset << " sd=" << i.session_description
      << " cd=" << i.channel_description << " s3=" << i.section3_available
      << " pw=" << i.password_protected << " leg=" << i.legacy_password_mapping
      << " subj=" << i.subject_name_1 << "|" << i.subject_name_2 << "|" << i.subject_id << "|"
      << i.recording_location << "\n";
  }
  for (const auto& p : s.problems())
    o << "problem " << p.channel << " " << p.segment_number << " " << p.segment << " " << p.kind
      << " " << p.reason << "\n";
  for (const auto& d : s.declaration_issues())
    o << "decl " << d.channel << " " << d.segment_number << " " << d.field << "\n";
  return o.str();
}

std::string describe_maps(Session& s) {
  std::ostringstream o;
  for (const auto& name : s.channels())
    for (const auto& m : s.segment_map(name))
      o << name << " " << m.segment_number << " " << m.path << " " << m.start_time << " "
        << m.end_time << " " << m.start_sample << " " << m.number_of_samples << "\n";
  return o.str();
}

// Delays every request by a pseudo-random 0-1.5 ms keyed on its path, so the
// pool's completion order differs from the submission order.
class ScramblingSource : public SessionSource {
 public:
  explicit ScramblingSource(std::shared_ptr<const SessionSource> inner) : inner_(std::move(inner)) {}
  std::vector<SourceDirEntry> list_dir(const std::string& rel) const override {
    wait(rel);
    return inner_->list_dir(rel);
  }
  bool exists(const std::string& rel) const override {
    wait(rel);
    return inner_->exists(rel);
  }
  std::uint64_t file_size(const std::string& rel) const override { return inner_->file_size(rel); }
  std::vector<ui1> read_all(const std::string& rel) const override {
    wait(rel);
    return inner_->read_all(rel);
  }
  std::vector<ui1> read_head(const std::string& rel, std::size_t n) const override {
    wait(rel);
    return inner_->read_head(rel, n);
  }
  std::vector<ui1> read_range(const std::string& rel, std::size_t o, std::size_t n) const override {
    return inner_->read_range(rel, o, n);
  }
  std::string describe(const std::string& rel) const override { return inner_->describe(rel); }
  std::string session_name() const override { return inner_->session_name(); }

 private:
  static void wait(const std::string& rel) {
    const auto h = std::hash<std::string>{}(rel);
    std::this_thread::sleep_for(std::chrono::microseconds(h % 1500));
  }
  std::shared_ptr<const SessionSource> inner_;
};

// Throws IoError for chosen requests: to pin WHICH failure a strict open
// raises when several requests fail.
class FailingSource : public SessionSource {
 public:
  FailingSource(std::shared_ptr<const SessionSource> inner, std::string fail_list,
                std::string fail_exists, std::chrono::milliseconds slow_others)
      : inner_(std::move(inner)), fail_list_(std::move(fail_list)),
        fail_exists_(std::move(fail_exists)), slow_(slow_others) {}
  std::vector<SourceDirEntry> list_dir(const std::string& rel) const override {
    if (rel == fail_list_) throw IoError("cannot list " + rel);
    if (!rel.empty()) std::this_thread::sleep_for(slow_);
    return inner_->list_dir(rel);
  }
  bool exists(const std::string& rel) const override {
    if (rel == fail_exists_) throw IoError("cannot stat " + rel);
    return inner_->exists(rel);
  }
  std::uint64_t file_size(const std::string& rel) const override { return inner_->file_size(rel); }
  std::vector<ui1> read_all(const std::string& rel) const override { return inner_->read_all(rel); }
  std::vector<ui1> read_head(const std::string& rel, std::size_t n) const override {
    return inner_->read_head(rel, n);
  }
  std::vector<ui1> read_range(const std::string& rel, std::size_t o, std::size_t n) const override {
    return inner_->read_range(rel, o, n);
  }
  std::string describe(const std::string& rel) const override { return inner_->describe(rel); }
  std::string session_name() const override { return inner_->session_name(); }

 private:
  std::shared_ptr<const SessionSource> inner_;
  std::string fail_list_, fail_exists_;
  std::chrono::milliseconds slow_;
};

struct Outcome {
  std::string type, message;
};
Outcome strict_failure(const std::function<void()>& open) {
  try {
    open();
  } catch (const std::exception& e) {
    return {typeid(e).name(), e.what()};
  }
  return {"", "opened"};
}
}  // namespace

TEST_CASE("resolve_threads: the one place 'automatic' is decided", "[parallel-open]") {
  unsigned hc = std::thread::hardware_concurrency();
  if (hc == 0) hc = 1;
  CHECK(resolve_threads(0) == hc);
  CHECK(resolve_threads(-3) == hc);
  CHECK(resolve_threads(0, Work::Io) == std::max(hc, kMinAutomaticIoThreads));
  CHECK(resolve_threads(5) == 5u);
  CHECK(resolve_threads(5, Work::Io) == 5u);
  CHECK(resolve_threads(1, Work::Io) == 1u);
}

TEST_CASE("parallel_try_each runs every item and reports each one's error", "[parallel-open]") {
  for (int t : THREADS) {
    std::vector<int> ran(50, 0);
    const auto errs = parallel_try_each(50, t, [&](std::size_t i) {
      ran[i] = 1;
      if (i % 7 == 3) throw std::runtime_error(std::to_string(i));
    });
    for (std::size_t i = 0; i < 50; ++i) {
      CHECK(ran[i] == 1);
      CHECK(static_cast<bool>(errs[i]) == (i % 7 == 3));
    }
  }
}

TEST_CASE("a lenient open is identical at every thread count, directory and tar",
          "[parallel-open]") {
  const auto dir = write_damaged("mef3io_par_open_lenient.mefd");
  const auto tar = mef3io::test::archive_fresh(dir);
  for (const std::string& path : {dir, tar}) {
    CAPTURE(path);
    Session base(path, "", false, 1);
    const std::string want = describe(base);
    const std::string want_maps = describe_maps(base);
    // The damage is all there: three .tmet failures, records-only d, b short by one.
    REQUIRE(base.problems().size() == 3);
    REQUIRE(base.channel_info("b").n_segments == 5);
    REQUIRE(base.channel_info("a").n_segments == 4);
    for (int t : THREADS) {
      CAPTURE(t);
      Session s(path, "", false, t);
      CHECK(describe(s) == want);
      CHECK(describe_maps(s) == want_maps);
      Session scrambled(std::make_shared<ScramblingSource>(open_session_source(path)), "", false,
                        t);
      CHECK(describe(scrambled) == want);
    }
  }
  fsys::remove(tar);
  fsys::remove_all(dir);
}

TEST_CASE("a strict open raises the serial walk's error at every thread count",
          "[parallel-open]") {
  const auto dir = write_damaged("mef3io_par_open_strict.mefd");
  const auto tar = mef3io::test::archive_fresh(dir);
  for (const std::string& path : {dir, tar}) {
    CAPTURE(path);
    const Outcome want = strict_failure([&] { Session s(path, "", true, 1); });
    REQUIRE(want.message != "opened");
    for (int t : THREADS) {
      CAPTURE(t);
      const Outcome got = strict_failure([&] { Session s(path, "", true, t); });
      CHECK(got.type == want.type);
      CHECK(got.message == want.message);
      const Outcome scrambled = strict_failure([&] {
        Session s(std::make_shared<ScramblingSource>(open_session_source(path)), "", true, t);
      });
      CHECK(scrambled.type == want.type);
      CHECK(scrambled.message == want.message);
    }
  }
  fsys::remove(tar);
  fsys::remove_all(dir);
}

TEST_CASE("a layout failure is the first in listing order, not the first in time",
          "[parallel-open]") {
  // Two requests fail: the LAST channel folder's listing, which fails at
  // once, and an existence check in the FIRST channel, which comes late (the
  // other listings are slowed down). A serial walk meets the first channel's
  // failure first, so that is what every thread count must raise.
  const auto dir = write_damaged("mef3io_par_open_layout.mefd");
  const auto inner = open_session_source(dir);
  std::vector<std::string> timd;
  for (const auto& e : inner->list_dir(""))
    if (e.is_dir && e.name.ends_with(".timd")) timd.push_back(e.name);
  REQUIRE(timd.size() >= 2);
  const std::string first_ch = timd.front().substr(0, timd.front().size() - 5);
  const std::string late = timd.front() + "/" + stem(first_ch, 3) + ".segd/" + stem(first_ch, 3) +
                           ".tidx";
  for (int t : THREADS) {
    CAPTURE(t);
    const Outcome got = strict_failure([&] {
      Session s(std::make_shared<FailingSource>(inner, timd.back(), late,
                                                std::chrono::milliseconds(20)),
                "", true, t);
    });
    CHECK(got.message == "cannot stat " + late);
  }
  fsys::remove_all(dir);
}

TEST_CASE("the open makes the same requests at every thread count", "[parallel-open][open-calls]") {
  const auto dir = write_damaged("mef3io_par_open_calls.mefd");
  test::SourceCalls want{};
  for (int t : THREADS) {
    auto src = std::make_shared<test::CountingSource>(open_session_source(dir));
    Session s(std::shared_ptr<const SessionSource>(src), "", false, t);
    const auto c = src->calls();
    if (t == 1) want = c;
    CAPTURE(t);
    CHECK(c.list_dir == want.list_dir);
    CHECK(c.exists == want.exists);
    CHECK(c.read_head == want.read_head);
    CHECK(c.total() == want.total());
  }
  fsys::remove_all(dir);
}

TEST_CASE("Reader thread count: set at open, changeable later", "[parallel-open]") {
  const auto dir = write_damaged("mef3io_par_open_reader.mefd");
  Reader r(dir, "", 3, false);
  CHECK(r.threads() == 3);
  const auto want = r.read_raw("e");
  for (int t : {0, 1, 2, 16}) {
    r.set_threads(t);
    CHECK(r.threads() == t);
    const auto got = r.read_raw("e");
    CHECK(got.samples == want.samples);
    CHECK(got.valid == want.valid);
  }
  fsys::remove_all(dir);
}

TEST_CASE("open time with a per-request delay, by thread count", "[.open-bench-threads]") {
  // Hidden; the numbers in docs/benchmarks.md come from here:
  //   build-core/mef3io_tests "[.open-bench-threads]"
  for (int ms : {5, 20})
    for (int channels : {64, 209})
      for (int segments : {1, 10}) {
        const auto dir = (fsys::temp_directory_path() / "mef3io_par_open_bench.mefd").string();
        fsys::remove_all(dir);
        {
          SessionWriter w(dir);
          std::vector<si4> a(256);
          for (int i = 0; i < 256; ++i) a[i] = i;
          for (int c = 0; c < channels; ++c)
            for (int s = 0; s < segments; ++s)
              w.write_int32("ch" + std::to_string(c), a, 1.0, START + s * 2'000'000LL, 256.0, {},
                            s > 0);
        }
        for (int t : {1, 4, 16}) {
          auto src = std::make_shared<test::CountingSource>(open_session_source(dir),
                                                            std::chrono::milliseconds(ms));
          const auto t0 = std::chrono::steady_clock::now();
          Session s(std::shared_ptr<const SessionSource>(src), "", true, t);
          const double secs =
              std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
          std::printf("%2d ms/request  %3d ch x %2d seg  n_threads=%2d  %8.2f s  requests=%ld\n",
                      ms, channels, segments, t, secs, src->calls().total());
          std::fflush(stdout);
        }
        fsys::remove_all(dir);
      }
}

namespace {
// Installs a thread_start_hook for the scope of a test.
struct HookGuard {
  explicit HookGuard(void (*fn)(unsigned)) { detail::thread_start_hook.store(fn); }
  ~HookGuard() { detail::thread_start_hook.store(nullptr); }
};
std::atomic<int> g_started{0};
void count_start(unsigned) { ++g_started; }
void fail_after_two(unsigned running) {
  if (running >= 2) throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
  ++g_started;
}
void fail_always(unsigned) {
  throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
}
}  // namespace

TEST_CASE("a thread that cannot be started degrades, never aborts", "[parallel-open]") {
  // std::thread throws EAGAIN under a pids / ulimit -u cap; an unjoined
  // std::thread then used to std::terminate the process (#146 review).
  const auto dir = write_damaged("mef3io_par_open_spawnfail.mefd");
  Session serial(dir, "", false, 1);
  const std::string want = describe(serial);
  Reader rs(dir, "", 1, false);
  const auto want_read = rs.read_raw("e");
  for (auto hook : {&fail_after_two, &fail_always}) {
    g_started = 0;
    HookGuard guard(hook);
    Session s(dir, "", false, 16);
    CHECK(describe(s) == want);
    Reader r(dir, "", 16, false);
    const auto got = r.read_raw("e");
    CHECK(got.samples == want_read.samples);
    CHECK(got.valid == want_read.valid);
    CHECK(g_started <= 2 * 8);  // never more than the hook let through per pass
    std::vector<int> hits(100, 0);
    parallel_for(100, 16, [&](std::size_t i) { ++hits[i]; });
    for (int h : hits) CHECK(h == 1);
  }
  fsys::remove_all(dir);
}

TEST_CASE("n_threads = 1 starts no extra thread: open, read, write", "[parallel-open]") {
  const auto dir = write_damaged("mef3io_par_open_one.mefd");
  {
    g_started = 0;
    HookGuard guard(&count_start);
    Session s(dir, "", false, 1);
    Reader r(dir, "", 1, false);
    r.read_raw("e");
    r.read("f");
    r.toc("e");
    const auto wdir = (fsys::temp_directory_path() / "mef3io_par_open_one_w.mefd").string();
    SessionWriter w(wdir);
    w.set_threads(1);
    std::vector<si4> x(50000, 3);
    w.write_int32("z", x, 1.0, START, 250.0, {}, false);
    CHECK(g_started == 0);
    fsys::remove_all(wdir);
  }
  {
    g_started = 0;
    HookGuard guard(&count_start);
    Session s(dir, "", false, 4);
    CHECK(g_started > 0);  // the hook does see a parallel open
  }
  fsys::remove_all(dir);
}

TEST_CASE("a negative thread count is an error everywhere", "[parallel-open]") {
  const auto dir = write_damaged("mef3io_par_open_neg.mefd");
  CHECK_THROWS_AS(Session(dir, "", false, -1), std::invalid_argument);
  CHECK_THROWS_AS(Reader(dir, "", -4, false), std::invalid_argument);
  Reader r(dir, "", 2, false);
  CHECK_THROWS_AS(r.set_threads(-1), std::invalid_argument);
  CHECK(r.threads() == 2);
  SessionWriter w((fsys::temp_directory_path() / "mef3io_par_open_neg_w.mefd").string());
  CHECK_THROWS_AS(w.set_threads(-1), std::invalid_argument);
  CHECK(check_threads(0) == 0);
  fsys::remove_all(dir);
  fsys::remove_all(fsys::temp_directory_path() / "mef3io_par_open_neg_w.mefd");
}

TEST_CASE("a windowed read skips walking cached indexes that miss the window",
          "[parallel-open]") {
  // #110 item 3, without trusting any .tmet time (#146 review rounds 1-2):
  // a walk learns the span of the index it walked; while that index stays
  // cached, a window it misses skips it. No I/O either way.
  const auto dir = (fsys::temp_directory_path() / "mef3io_par_open_span.mefd").string();
  fsys::remove_all(dir);
  {
    SessionWriter w(dir);
    std::vector<si4> a(2560, 1);
    for (int s = 0; s < 40; ++s)
      w.write_int32("ch", a, 1.0, START + s * 20'000'000LL, 256.0, {}, s > 0);
  }
  auto src = std::make_shared<test::CountingSource>(open_session_source(dir));
  Reader r(std::shared_ptr<const SessionSource>(src), "", 1);
  const auto full = r.read("ch");
  src->reset();
  const si8 t = START + 20 * 20'000'000LL + 2'000'000LL;  // inside segment 20 only
  const auto win = r.read("ch", t, t + 1'000'000);
  REQUIRE(win.size() == 256);
  for (double v : win) CHECK(v == 1.0);
  CHECK(src->calls().read_all == 0);    // every index cached
  CHECK(src->calls().read_range == 1);  // segment 20's blocks, no other segment's
  // Evicted: the spans go with the indexes, and the next window walks again.
  r.session().set_index_cache_bytes(1);
  r.read("ch", t, t + 1'000'000);  // trims at its end
  src->reset();
  CHECK(r.read("ch", t, t + 1'000'000) == win);
  CHECK(src->calls().read_all == 40);  // re-read, as on main
  fsys::remove_all(dir);
}

TEST_CASE("a segment that grew after its span was learnt is read once evicted",
          "[parallel-open]") {
  // #146 round-2 N1: a span outliving its index hid an append forever.
  const auto dir = (fsys::temp_directory_path() / "mef3io_par_open_grow.mefd").string();
  fsys::remove_all(dir);
  std::vector<si4> a(256 * 60);
  for (std::size_t i = 0; i < a.size(); ++i) a[i] = static_cast<si4>(i % 1000);
  {
    SessionWriter w(dir);
    w.write_int32("ch", a, 1.0, START, 256.0, {}, false);  // [0, 60 s)
  }
  Reader r(dir, "", 1);
  REQUIRE(r.read("ch", START + 10'000'000, START + 20'000'000).size() == 2560);
  {
    SessionWriter w(dir);  // another writer appends [60, 120 s) in place
    w.write_int32("ch", a, 1.0, START + 60'000'000, 256.0, {}, false);
  }
  r.session().set_index_cache_bytes(1);
  r.read("ch", START + 10'000'000, START + 20'000'000);  // evicts at its end
  const auto got = r.read("ch", START + 70'000'000, START + 80'000'000);
  Reader fresh(dir, "", 1);
  const auto want = fresh.read("ch", START + 70'000'000, START + 80'000'000);
  REQUIRE(got.size() == want.size());
  std::size_t finite = 0;
  for (std::size_t i = 0; i < got.size(); ++i) {
    CHECK((std::isnan(got[i]) == std::isnan(want[i])));
    if (!std::isnan(want[i])) {
      ++finite;
      CHECK(got[i] == want[i]);
    }
  }
  CHECK(finite == 2560);
  fsys::remove_all(dir);
}
