// Kept-open data files of a Reader/Session (source.cpp, `detail::kept_pread`):
// a bounded per-Session LRU of read-only handles with positional reads, so
// paging through a recording does not reopen the same .tdat for every read.
// Rules pinned here: results are byte-identical with the cache off / cap 1 /
// default, at every thread count; the cap is never exceeded (counted, and as
// process descriptors on Linux); eviction is least-recently-used; a file
// appended in place is seen, a file REPLACED, renamed or deleted under a kept
// handle is never served from the old handle; release and destruction close
// everything; a tar keeps its one archive file; a read in segment k opens only
// segment k's data files.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "mef3io/errors.hpp"
#include "mef3io/reader.hpp"
#include "mef3io/session_writer.hpp"
#include "mef3io/source.hpp"
#include "mef3io/tar.hpp"

using namespace mef3io;
namespace fsys = std::filesystem;

namespace {
constexpr si8 START = 1577836800000000;
constexpr sf8 FS = 250.0;
constexpr si8 SEG_STEP = 30'000'000;  // segment k starts at START + k * SEG_STEP
constexpr int SEGMENTS = 3;
const std::vector<std::string> CHANNELS = {"a", "b", "c", "d"};

std::vector<si4> wave(int n, double phase) {
  std::vector<si4> x(n);
  for (int i = 0; i < n; ++i) x[i] = static_cast<si4>(900 * std::sin(i / 11.0 + phase)) + i % 5;
  return x;
}

// 4 channels x 3 segments x 5000 samples (20 s at 250 Hz; 10 s gap between segments).
std::string write_session(const std::string& name, double phase = 0.0, int offset = 0) {
  const auto dir = (fsys::temp_directory_path() / name).string();
  fsys::remove_all(dir);
  SessionWriter w(dir, true);
  for (int s = 0; s < SEGMENTS; ++s)
    for (std::size_t c = 0; c < CHANNELS.size(); ++c)
      {
        auto x = wave(5000, phase + 0.3 * static_cast<double>(c) + s);
        for (auto& v : x) v += offset;
        w.write_int32(CHANNELS[c], x, 0.5, START + s * SEG_STEP, FS, {}, s > 0);
      }
  return dir;
}

std::string tdat_of(const std::string& dir, const std::string& ch, int seg) {
  char seg_name[16];
  std::snprintf(seg_name, sizeof seg_name, "%06d", seg);
  const fsys::path p = fsys::path(dir) / (ch + ".timd") / (ch + "-" + seg_name + ".segd");
  return (p / (ch + "-" + seg_name + ".tdat")).string();
}

// Paths compared with '/' separators: Windows mixes them in one path.
std::string slashed(std::string p) {
  std::replace(p.begin(), p.end(), '\\', '/');
  return p;
}

std::mutex g_mu;
std::map<std::string, int> g_opens;
void note_open(const std::string& p) {
  std::lock_guard<std::mutex> lk(g_mu);
  ++g_opens[slashed(p)];
}
struct OpenWatch {
  OpenWatch() {
    g_opens.clear();
    detail::source_open_observer = &note_open;
  }
  ~OpenWatch() { detail::source_open_observer = nullptr; }
  static int opens_of(const std::string& path) {
    std::lock_guard<std::mutex> lk(g_mu);
    const auto it = g_opens.find(slashed(path));
    return it == g_opens.end() ? 0 : it->second;
  }
  static int tdat_opens() {
    std::lock_guard<std::mutex> lk(g_mu);
    int n = 0;
    for (const auto& [p, k] : g_opens)
      if (p.size() > 5 && p.compare(p.size() - 5, 5, ".tdat") == 0) n += k;
    return n;
  }
};

bool same(const std::vector<sf8>& a, const std::vector<sf8>& b) {
  return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(sf8)) == 0;
}

// Every channel over [t0, t1): one flat vector.
std::vector<sf8> all(Reader& r, si8 t0, si8 t1) {
  std::vector<sf8> out;
  for (const auto& c : CHANNELS) {
    auto row = r.read(c, t0, t1);
    out.insert(out.end(), row.begin(), row.end());
  }
  return out;
}

// A battery of windows: inside one segment, across two, the whole session.
std::vector<std::vector<sf8>> battery(Reader& r) {
  std::vector<std::vector<sf8>> out;
  const si8 s1 = START + SEG_STEP;
  out.push_back(all(r, START + 2'000'000, START + 12'000'000));
  out.push_back(all(r, s1 + 2'000'000, s1 + 9'000'000));
  out.push_back(all(r, START + 15'000'000, s1 + 5'000'000));
  out.push_back(all(r, START, START + 3 * SEG_STEP));
  out.push_back(all(r, START + 2'000'000, START + 12'000'000));  // again: same file, kept handle
  return out;
}

// Open descriptors of this process (Linux only; 0 where /proc is absent).
std::size_t process_fds() {
  std::error_code ec;
  if (!fsys::exists("/proc/self/fd", ec)) return 0;
  std::size_t n = 0;
  for (auto it = fsys::directory_iterator("/proc/self/fd", ec); !ec && it != fsys::directory_iterator();
       it.increment(ec))
    ++n;
  return n;
}
}  // namespace

TEST_CASE("kept files: results are identical with the cache off, tiny and default, at every thread count",
          "[open_files]") {
  const auto dir = write_session("mef3io_of_equal.mefd");
  Reader plain(dir, "", 1, true);
  plain.set_open_files(0);
  const auto want = battery(plain);
  for (const int threads : {1, 2, 8}) {
    for (const std::size_t cap : {std::size_t(0), std::size_t(1), std::size_t(2), std::size_t(5),
                                  std::size_t(500), detail::suggested_open_files()}) {
      INFO("threads " << threads << " cap " << cap);
      Reader r(dir, "", threads, true);
      r.set_open_files(detail::suggested_open_files());
      r.set_open_files(cap);
      const auto got = battery(r);
      REQUIRE(got.size() == want.size());
      for (std::size_t i = 0; i < want.size(); ++i) REQUIRE(same(got[i], want[i]));
      REQUIRE(r.open_file_count() <= cap);
    }
  }
}

TEST_CASE("kept files: paging again and again opens each data file once", "[open_files]") {
  const auto dir = write_session("mef3io_of_once.mefd");
  Reader r(dir, "", 1, true);
  r.set_open_files(detail::suggested_open_files());
  REQUIRE(r.open_files() == detail::suggested_open_files());
  OpenWatch watch;
  for (int i = 0; i < 20; ++i) all(r, START + 2'000'000, START + 12'000'000);
  // Segment 0 of 4 channels: four files, one open each, however many reads.
  REQUIRE(OpenWatch::tdat_opens() == 4);
  for (const auto& c : CHANNELS) REQUIRE(OpenWatch::opens_of(tdat_of(dir, c, 0)) == 1);
  REQUIRE(r.open_file_count() == 4);

  // Off: today's behaviour, one open per read.
  r.set_open_files(0);
  REQUIRE(r.open_file_count() == 0);
  OpenWatch again;
  for (int i = 0; i < 5; ++i) all(r, START + 2'000'000, START + 12'000'000);
  REQUIRE(OpenWatch::tdat_opens() == 20);
  REQUIRE(r.open_file_count() == 0);
}

TEST_CASE("kept files: a read in one segment opens only that segment's data files", "[open_files]") {
  const auto dir = write_session("mef3io_of_seg.mefd");
  Reader r(dir, "", 1, true);
  r.set_open_files(detail::suggested_open_files());
  r.read("a", START + SEG_STEP + 1'000'000, START + SEG_STEP + 5'000'000);  // learns every span
  r.release_open_files();
  OpenWatch watch;
  r.read("a", START + SEG_STEP + 1'000'000, START + SEG_STEP + 5'000'000);
  REQUIRE(OpenWatch::tdat_opens() == 1);
  REQUIRE(OpenWatch::opens_of(tdat_of(dir, "a", 1)) == 1);
}

TEST_CASE("kept files: the cap is never exceeded, eviction is least recently used", "[open_files]") {
  const auto dir = write_session("mef3io_of_cap.mefd");
  const std::size_t base_fds = process_fds();
  {
    Reader r(dir, "", 1, true);
    r.set_open_files(detail::suggested_open_files());
    r.set_open_files(3);
    // 12 data files, one read each: never more than 3 kept, and (Linux) never
    // more than 3 descriptors beyond what the process held before.
    for (int s = 0; s < SEGMENTS; ++s)
      for (const auto& c : CHANNELS) {
        r.read(c, START + s * SEG_STEP + 1'000'000, START + s * SEG_STEP + 4'000'000);
        REQUIRE(r.open_file_count() <= 3);
        if (base_fds) REQUIRE(process_fds() <= base_fds + 3);
      }
    REQUIRE(r.open_file_count() == 3);

    // LRU: a, b, c kept (cap 3); touch a, then open d: b (least recently used) goes.
    r.release_open_files();
    const auto win = [&](const std::string& c) {
      r.read(c, START + 1'000'000, START + 4'000'000);
    };
    win("a");
    win("b");
    win("c");
    win("a");
    win("d");
    OpenWatch watch;
    win("a");  // kept
    win("c");  // kept
    win("d");  // kept
    REQUIRE(OpenWatch::tdat_opens() == 0);
    win("b");  // was evicted: opened again
    REQUIRE(OpenWatch::opens_of(tdat_of(dir, "b", 0)) == 1);

    // Lowering the cap closes the extra files at once.
    r.set_open_files(1);
    REQUIRE(r.open_file_count() == 1);
    if (base_fds) REQUIRE(process_fds() <= base_fds + 1);
  }
  // Destruction closes everything.
  if (base_fds) REQUIRE(process_fds() == base_fds);
}

TEST_CASE("kept files: release lets go of everything and the Reader still works", "[open_files]") {
  const auto dir = write_session("mef3io_of_release.mefd");
  const std::size_t base_fds = process_fds();
  Reader r(dir, "", 1, true);
  r.set_open_files(detail::suggested_open_files());
  const auto before = all(r, START, START + 3 * SEG_STEP);
  REQUIRE(r.open_file_count() == 12);
  r.release_open_files();
  REQUIRE(r.open_file_count() == 0);
  if (base_fds) REQUIRE(process_fds() == base_fds);
  REQUIRE(same(all(r, START, START + 3 * SEG_STEP), before));
  REQUIRE(r.open_file_count() == 12);
}

TEST_CASE("kept files: many threads on one Reader with a tiny cap read the same bytes", "[open_files]") {
  const auto dir = write_session("mef3io_of_threads.mefd");
  Reader off(dir, "", 1, true);
  off.set_open_files(0);
  const auto want = battery(off);
  Reader r(dir, "", 4, true);
  r.set_open_files(detail::suggested_open_files());
  r.set_open_files(2);  // 12 files over 2 slots: constant eviction while others read
  std::atomic<int> bad{0};
  std::vector<std::thread> pool;
  for (int t = 0; t < 8; ++t)
    pool.emplace_back([&] {
      for (int rep = 0; rep < 4; ++rep) {
        const auto got = battery(r);
        for (std::size_t i = 0; i < want.size(); ++i)
          if (!same(got[i], want[i])) ++bad;
      }
    });
  for (auto& t : pool) t.join();
  REQUIRE(bad == 0);
  REQUIRE(r.open_file_count() <= 2);
}

TEST_CASE("kept files: a file appended in place is read to its new end", "[open_files]") {
  const auto dir = write_session("mef3io_of_append.mefd");
  Reader r(dir, "", 1, true);
  r.set_open_files(detail::suggested_open_files());
  r.session().set_index_cache_bytes(1);  // the index is re-read per call, so appended blocks show
  const si8 last = START + 2 * SEG_STEP;  // segment 2 of "a" ends 20 s later
  REQUIRE(r.read("a", last + 1'000'000, last + 3'000'000).size() > 0);
  REQUIRE(r.open_file_count() == 1);  // the handle of a's segment 2 is kept
  {
    SessionWriter w(dir, false);  // append in place: the same .tdat grows
    w.write_int32("a", wave(2500, 7.0), 0.5, last + 20'000'000, FS);
  }
  const si8 t0 = last + 18'000'000, t1 = last + 28'000'000;
  const auto kept = r.read("a", t0, t1);
  Reader fresh(dir, "", 1, true);
  fresh.set_open_files(0);
  const auto want = fresh.read("a", t0, t1);
  REQUIRE(same(kept, want));
  REQUIRE(std::count_if(kept.begin(), kept.end(), [](sf8 v) { return !std::isnan(v); }) > 2000);
  REQUIRE(r.open_file_count() == 1);  // still the one handle (same file, grown)
}

#ifndef _WIN32  // Windows refuses to replace a file that anyone holds open (see docs/long_recordings.md)
TEST_CASE("kept files: a file replaced under a kept handle is never read through the old handle",
          "[open_files]") {
  const auto dir = write_session("mef3io_of_replace.mefd");
  const auto other = write_session("mef3io_of_replace_other.mefd", 5.0);
  Reader r(dir, "", 1, true);
  r.set_open_files(detail::suggested_open_files());
  r.session().set_index_cache_bytes(1);
  const si8 t0 = START + 1'000'000, t1 = START + 6'000'000;
  const auto before = r.read("a", t0, t1);
  REQUIRE(r.open_file_count() == 1);
  // Replace a's segment-0 index and data with another recording's (new files
  // renamed into place, as a copy tool or a rewriting session would do).
  for (const char* ext : {".tdat", ".tidx"}) {
    std::string from = tdat_of(other, "a", 0), to = tdat_of(dir, "a", 0);
    from.replace(from.size() - 5, 5, ext);
    to.replace(to.size() - 5, 5, ext);
    fsys::copy_file(from, to + ".new", fsys::copy_options::overwrite_existing);
    fsys::rename(to + ".new", to);
  }
  const auto after = r.read("a", t0, t1);
  Reader fresh(dir, "", 1, true);
  fresh.set_open_files(0);
  REQUIRE(same(after, fresh.read("a", t0, t1)));
  REQUIRE_FALSE(same(after, before));  // the test can tell old from new
  REQUIRE(r.open_file_count() == 1);   // the old handle was replaced, not added
}
#endif

#ifndef _WIN32  // Windows cannot rename or remove a folder while any file under it is open
TEST_CASE("kept files: a renamed or deleted session fails like a Reader that keeps nothing open",
          "[open_files]") {
  const auto dir = write_session("mef3io_of_gone.mefd");
  const auto moved = (fsys::temp_directory_path() / "mef3io_of_gone_moved.mefd").string();
  fsys::remove_all(moved);
  const auto message_of = [](Reader& r) {
    try {
      r.read("a", START + 1'000'000, START + 4'000'000);
    } catch (const std::exception& e) {
      return std::string(e.what());
    }
    return std::string("no error");
  };
  Reader kept(dir, "", 1, true);
  kept.set_open_files(detail::suggested_open_files());
  Reader none(dir, "", 1, true);
  none.set_open_files(0);
  none.read("a", START + 1'000'000, START + 4'000'000);  // its index is cached, as kept's
  const auto good = kept.read("a", START + 1'000'000, START + 4'000'000);
  REQUIRE(kept.open_file_count() == 1);
  fsys::rename(dir, moved);
  const std::string a = message_of(kept), b = message_of(none);
  REQUIRE(a != "no error");  // not served from the handle that still points at the moved file
  REQUIRE(a == b);
  REQUIRE(kept.open_file_count() == 0);  // the handle of a path that is gone is dropped
  fsys::rename(moved, dir);
  REQUIRE(same(kept.read("a", START + 1'000'000, START + 4'000'000), good));  // back: works again
  fsys::remove_all(dir);
  const std::string c = message_of(kept), d = message_of(none);
  REQUIRE(c != "no error");
  REQUIRE(c == d);
}
#endif

TEST_CASE("kept files: a file that is deleted under a kept handle is not served from it", "[open_files]") {
  // The one file operation Windows allows under an open handle that shares delete
  // (a replacing rename and a folder rename it refuses): remove the FILE.
  const auto dir = write_session("mef3io_of_remove.mefd");
  Reader r(dir, "", 1, true);
  r.set_open_files(detail::suggested_open_files());
  r.read("b", START + 1'000'000, START + 4'000'000);
  REQUIRE(r.open_file_count() == 1);
  fsys::remove(tdat_of(dir, "b", 0));  // not blocked by the kept handle
  REQUIRE_THROWS(r.read("b", START + 1'000'000, START + 4'000'000));
  REQUIRE(r.open_file_count() == 0);
  REQUIRE(r.read("a", START + 1'000'000, START + 4'000'000).size() > 0);  // the others go on
}

TEST_CASE("kept files: a tar archive is read through its one kept file", "[open_files][tar]") {
  const auto dir = write_session("mef3io_of_tar.mefd");
  const auto tar_path = (fsys::temp_directory_path() / "mef3io_of_tar.mefd.tar").string();
  fsys::remove(tar_path);
  archive_session(dir, tar_path, true);
  Reader from_dir(dir, "", 1, true);
  from_dir.set_open_files(0);
  const auto want = battery(from_dir);
  for (const int threads : {1, 8}) {
    OpenWatch watch;
    Reader r(tar_path, "", threads, true);  // the open's own requests share the one handle too
    r.set_open_files(detail::suggested_open_files());
    const auto got = battery(r);
    for (std::size_t i = 0; i < want.size(); ++i) REQUIRE(same(got[i], want[i]));
    REQUIRE(r.open_file_count() == 1);
    REQUIRE(OpenWatch::opens_of(tar_path) == 1);  // one open for the open AND every request of the battery
    r.release_open_files();
    REQUIRE(r.open_file_count() == 0);
    r.set_open_files(0);
    const auto off = battery(r);
    for (std::size_t i = 0; i < want.size(); ++i) REQUIRE(same(off[i], want[i]));
    REQUIRE(r.open_file_count() == 0);
  }
}

TEST_CASE("kept files: off by default, nothing is held after a read", "[open_files]") {
  const auto dir = write_session("mef3io_of_default.mefd");
  REQUIRE(detail::default_open_files() == 0);
  const std::size_t base = process_fds();
  Reader r(dir, "", 1, true);
  const std::size_t after_open = process_fds();
  REQUIRE(r.open_files() == 0);
  all(r, START, START + 3 * SEG_STEP);
  REQUIRE(r.open_file_count() == 0);
  if (base) REQUIRE(process_fds() == after_open);
}

TEST_CASE("kept files: the suggested cap respects the descriptor limit", "[open_files]") {
  const auto cap = detail::suggested_open_files();
  REQUIRE(cap <= 128);
#ifndef _WIN32
  REQUIRE(cap > 0);
#endif
}

TEST_CASE("kept files: deleted and recreated under the same name (same size, other content) is a new file",
          "[open_files]") {
  const auto dir = write_session("mef3io_of_recreate.mefd");
  const auto other = write_session("mef3io_of_recreate_o.mefd", 0.0, 3);
  const auto old_path = tdat_of(dir, "a", 0), new_path = tdat_of(other, "a", 0);
  REQUIRE(fsys::file_size(old_path) == fsys::file_size(new_path));
  Reader r(dir, "", 1, true);
  r.set_open_files(detail::suggested_open_files());
  const si8 t0 = START + 1'000'000, t1 = START + 6'000'000;
  const auto before = r.read("a", t0, t1);
  REQUIRE(r.open_file_count() == 1);
  try {
    fsys::remove(old_path);
    fsys::copy_file(new_path, old_path);
  } catch (const fsys::filesystem_error&) {
    return;  // the OS keeps the name of a deleted file that is still held: no recreate to check
  }
  const auto after = r.read("a", t0, t1);
  Reader fresh(dir, "", 1, true);
  REQUIRE(same(after, fresh.read("a", t0, t1)));
  REQUIRE_FALSE(same(after, before));
}
