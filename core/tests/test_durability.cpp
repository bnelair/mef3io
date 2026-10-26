// mef3io — the durability barriers themselves.
//
// `fsync_file` is the one place the library's crash guarantee actually lives,
// and some callers IGNORE the result (the recovery backups flush best-effort),
// so a platform where the flush never works reports nothing at all. That is
// exactly how it shipped broken on Windows in 1.x — FlushFileBuffers requires
// a GENERIC_WRITE handle, the code opened with GENERIC_READ, and every flush
// failed: `durability="full"` threw on the first append, the best-effort
// paths silently had no durability whatsoever. Since 2.0 the writer's one
// barrier — the replaced `.tmet` — goes through the same primitive.
//
// Most of these tests are about the PRIMITIVE, not a session: a round trip
// cannot see this, the bytes land either way on an orderly shutdown. The last
// ones pin WHICH files a write flushes, through the `fsync_observer` hook.
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <iterator>
#include <mutex>
#include <vector>
#include <string>
#include <system_error>

#include "../src/durability.hpp"
#include "mef3io/session_writer.hpp"

namespace fs = std::filesystem;
using namespace mef3io;

namespace {

/// A temp directory that cleans itself up, even when an assertion throws.
///
/// The nonce keeps concurrent runs from sharing a path — CI builds three
/// platforms and ctest may repeat. The DESTRUCTOR matters just as much: a
/// REQUIRE that fires unwinds past the rest of the test body, so a manual
/// remove_all at the end leaks a directory on exactly the runs being debugged.
struct Scratch {
  fs::path dir;

  explicit Scratch(const char* name) {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    dir = fs::temp_directory_path() /
          (std::string("mef3io_dur_") + name + "_" + std::to_string(nonce));
    fs::create_directories(dir);
  }
  ~Scratch() {
    std::error_code ec;
    fs::remove_all(dir, ec);  // best effort: a test must not fail in cleanup
  }
  Scratch(const Scratch&) = delete;
  Scratch& operator=(const Scratch&) = delete;

  fs::path operator/(const char* leaf) const { return dir / leaf; }
};

void write_file(const fs::path& p, const std::string& body) {
  std::ofstream f(p, std::ios::binary);
  f.write(body.data(), static_cast<std::streamsize>(body.size()));
  f.close();
  REQUIRE(static_cast<bool>(f));
}

}  // namespace

TEST_CASE("fsync_file flushes an ordinary file", "[durability]") {
  const Scratch dir("flush");
  const auto p = dir / "data.bin";
  write_file(p, std::string(4096, 'x'));

  // The whole point: on a working platform this SUCCEEDS. It returned false on
  // every Windows call for a release, which no session-level test could see.
  std::string why;
  REQUIRE(detail::fsync_file(p.string(), &why));
  REQUIRE(why.empty());

}

TEST_CASE("fsync_file flushes a file that is still open for append", "[durability]") {
  // The writer's real shape: the .tdat stream is closed, then flushed, while
  // other handles may still be around. Opening the file for flushing must not
  // be refused by sharing rules (the Windows path passes FILE_SHARE_*).
  const Scratch dir("append");
  const auto p = dir / "data.bin";
  write_file(p, "header");
  {
    std::ofstream app(p, std::ios::binary | std::ios::app);
    app << "more";
    app.flush();
    std::string why;
    REQUIRE(detail::fsync_file(p.string(), &why));
    REQUIRE(why.empty());
  }
  REQUIRE(fs::file_size(p) == 10);
}

TEST_CASE("fsync_file_or_throw succeeds quietly on a real file", "[durability]") {
  const Scratch dir("orthrow");
  const auto p = dir / "data.bin";
  write_file(p, "payload");
  REQUIRE_NOTHROW(detail::fsync_file_or_throw(p.string()));
}

TEST_CASE("a missing file is a reported failure, not a silent one", "[durability]") {
  const Scratch dir("missing");
  const auto p = (dir / "absent.bin").string();

  std::string why;
  REQUIRE_FALSE(detail::fsync_file(p, &why));
  // The reason must come back. "the write is not durable" with no cause is a
  // bug report nobody can act on — diagnosing the Windows failure needed the
  // OS error, and it was not there.
  REQUIRE_FALSE(why.empty());

  REQUIRE_THROWS_AS(detail::fsync_file_or_throw(p), IoError);
}

TEST_CASE("replace_file publishes over an existing target", "[durability]") {
  const Scratch dir("replace");
  const auto target = dir / "live.bin";
  const auto tmp = dir / "live.bin.tmp";
  write_file(target, "old");
  write_file(tmp, "new");

  REQUIRE_NOTHROW(detail::replace_file(tmp, target));
  REQUIRE_FALSE(fs::exists(tmp));
  std::string got;
  {
    // Scoped: Windows refuses to delete a file that is still open, so an
    // ifstream left alive here makes the cleanup below throw.
    std::ifstream f(target, std::ios::binary);
    got.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  }
  REQUIRE(got == "new");

}

namespace {

// Every flush fsync_file attempts while a FlushLog is alive. Several channels
// can be written at once, hence the mutex.
std::mutex g_flush_mu;
std::vector<std::string> g_flushed;

struct FlushLog {
  FlushLog() {
    std::lock_guard<std::mutex> lk(g_flush_mu);
    g_flushed.clear();
    detail::fsync_observer = [](const std::string& p) {
      std::lock_guard<std::mutex> lk2(g_flush_mu);
      g_flushed.push_back(p);
    };
  }
  ~FlushLog() { detail::fsync_observer = nullptr; }
  FlushLog(const FlushLog&) = delete;
  FlushLog& operator=(const FlushLog&) = delete;
  std::vector<std::string> take() {
    std::lock_guard<std::mutex> lk(g_flush_mu);
    auto out = g_flushed;
    g_flushed.clear();
    return out;
  }
};

bool ends_with(const std::string& s, const std::string& suffix) {
  return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

/// A NEW segment's files are flushed in ONE batch (flush_files) before its
/// .tmet is renamed into place; the writer marks each batch "batch:<tmp>".
/// Inside it, Linux issues one syncfs (no per-file event) and other systems
/// flush each file (an event per file). Returns the number of batches and
/// requires every other event to be one of those segments' files.
std::size_t new_segment_batches(const std::vector<std::string>& events) {
  std::size_t batches = 0;
  for (const auto& p : events) {
    if (p.rfind("batch:", 0) == 0) {
      ++batches;
      REQUIRE(ends_with(p, ".tmet.mef3io-tmp"));
    } else {
      INFO(p);
      REQUIRE((ends_with(p, ".tdat") || ends_with(p, ".tidx") || ends_with(p, ".tmet.mef3io-tmp")));
    }
  }
  return batches;
}

}  // namespace

// 2.0 removed the `durability` option. What stays is ONE barrier: the .tmet
// temp file is flushed before it is renamed over the original, because a short
// .tmet published by the rename makes the whole session fail to open. Nothing
// else a write touches is flushed — torn .tdat/.tidx tails are recover_session's
// job. Pinned here per file, since no round trip can tell the difference.
TEST_CASE("a write flushes only the .tmet it replaces, before the rename", "[durability]") {
  const Scratch dir("writer_flush");
  const auto session = (dir.dir / "s.mefd").string();
  const si8 start = 1577836800000000;
  const sf8 rate = 256.0;
  std::vector<si4> a(2560);
  for (std::size_t i = 0; i < a.size(); ++i) a[i] = static_cast<si4>(i % 997) - 400;
  const si8 step = static_cast<si8>(a.size() / rate * 1e6);
  const char* channels[] = {"ch1", "ch2", "ch3"};

  FlushLog log;
  {
    SessionWriter w(session, true);
    for (const char* ch : channels) w.write_int32(ch, a, 0.5, start, rate);
    // A FRESH session: each channel's FIRST segment flushes its three files
    // in ONE batch before its .tmet is renamed into place (once per channel:
    // no previous segment a lost .tmet could be rebuilt from). Nothing else.
    {
      REQUIRE(new_segment_batches(log.take()) == 3);  // one batch per channel
    }

    // An append through the SAME writer (cached index path): exactly one flush
    // per channel, of the .tmet temp file, i.e. before the rename publishes it.
    for (const char* ch : channels) w.write_int32(ch, a, 0.5, start + step, rate);
    auto got = log.take();
    REQUIRE(got.size() == 3);
    for (const auto& p : got) {
      INFO(p);
      REQUIRE(ends_with(p, ".tmet.mef3io-tmp"));
    }
    // A forced new, NON-FIRST segment in a session THIS writer created: not
    // flushed (a conversion writing thousands of segments would pay 3 flushes
    // each); recovery rebuilds a lost .tmet there from the previous segment's.
    w.write_int32("ch1", a, 0.5, start + 2 * step, rate, {}, /*new_segment=*/true);
    REQUIRE(log.take().empty());
  }
  {
    // A new segment in a session that held data when the writer opened it:
    // its three fresh files ARE flushed, in one batch, before its .tmet is
    // renamed into place — a short one would fail the strict open of the
    // whole session (one MISSING is only skipped).
    SessionWriter w(session, false);
    w.write_int32("ch3", a, 0.5, start + 2 * step, rate, {}, /*new_segment=*/true);
    REQUIRE(new_segment_batches(log.take()) == 1);
  }
  {
    // A REOPENED writer appends through the slow path (walks the index once):
    // the same one barrier, nothing on the .tdat or .tidx.
    SessionWriter w(session, false);
    w.write_int32("ch2", a, 0.5, start + 2 * step, rate);
    auto got = log.take();
    INFO(got.size());
    REQUIRE(got.size() == 1);
    REQUIRE(ends_with(got[0], ".tmet.mef3io-tmp"));
    REQUIRE(got[0].find("ch2.timd") != std::string::npos);
  }
  {
    // A NEW CHANNEL in a session that already held data when the writer
    // opened it: flushed the same way (3 files).
    SessionWriter w(session, false);
    w.write_int32("ch4", a, 0.5, start, rate);
    REQUIRE(new_segment_batches(log.take()) == 1);
  }
  // The temp files are gone: each was renamed into place.
  for (const auto& e : fs::recursive_directory_iterator(dir.dir))
    REQUIRE_FALSE(ends_with(e.path().string(), ".mef3io-tmp"));
}

TEST_CASE("a failed append's rollback publishes the restored .tidx flushed", "[durability]") {
  const Scratch dir("rollback_flush");
  const auto session = (dir.dir / "s.mefd").string();
  const si8 start = 1577836800000000;
  const sf8 rate = 256.0;
  std::vector<si4> a(2560);
  for (std::size_t i = 0; i < a.size(); ++i) a[i] = static_cast<si4>(i % 991) - 300;
  const si8 step = static_cast<si8>(a.size() / rate * 1e6);
  { SessionWriter w(session, true); w.write_int32("ch1", a, 0.5, start, rate); }

  fs::path seg;
  for (const auto& e : fs::directory_iterator(dir.dir / "s.mefd" / "ch1.timd")) seg = e.path();
  const std::string base = seg.stem().string();
  const auto read = [](const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  };
  const std::string tidx_before = read(seg / (base + ".tidx"));
  // Make the .tmet publish fail AFTER the data and index were extended: a
  // non-empty directory where its temp file must go.
  const fs::path blocker = seg / (base + ".tmet.mef3io-tmp");
  fs::create_directories(blocker / "x");

  FlushLog log;
  {
    // A REOPENED writer: the slow path, whose rollback writes the original
    // .tidx back whole (padding included) through write_file_atomic.
    SessionWriter w(session, false);
    REQUIRE_THROWS(w.write_int32("ch1", a, 0.5, start + step, rate));
  }
  const auto got = log.take();
  REQUIRE(got.size() == 1);
  REQUIRE(ends_with(got[0], ".tidx.mef3io-tmp"));  // flushed BEFORE its rename
  REQUIRE(read(seg / (base + ".tidx")) == tidx_before);
  REQUIRE_FALSE(fs::exists(seg / (base + ".tidx.mef3io-tmp")));
  fs::remove_all(blocker);
}
