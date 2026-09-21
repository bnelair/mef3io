// mef3io — the durability barriers themselves.
//
// `fsync_file` is the one place the library's crash guarantee actually lives,
// and most of its callers IGNORE the result: `durability="fast"` and the
// recovery backups flush best-effort, so a platform where the flush never
// works reports nothing at all. That is exactly how it shipped broken on
// Windows — FlushFileBuffers requires a GENERIC_WRITE handle, the code opened
// with GENERIC_READ, and every flush failed. `durability="full"` threw on the
// first append; the best-effort paths silently had no durability whatsoever.
//
// These tests are deliberately about the PRIMITIVE, not about a session. A
// round trip cannot see this: the bytes land either way on an orderly shutdown.
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "../src/durability.hpp"

namespace fs = std::filesystem;
using namespace mef3io;

namespace {

fs::path scratch(const char* name) {
  const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path p = fs::temp_directory_path() /
                     (std::string("mef3io_dur_") + name + "_" + std::to_string(nonce));
  fs::create_directories(p);
  return p;
}

void write_file(const fs::path& p, const std::string& body) {
  std::ofstream f(p, std::ios::binary);
  f.write(body.data(), static_cast<std::streamsize>(body.size()));
  f.close();
  REQUIRE(static_cast<bool>(f));
}

}  // namespace

TEST_CASE("fsync_file flushes an ordinary file", "[durability]") {
  const auto dir = scratch("flush");
  const auto p = dir / "data.bin";
  write_file(p, std::string(4096, 'x'));

  // The whole point: on a working platform this SUCCEEDS. It returned false on
  // every Windows call for a release, which no session-level test could see.
  std::string why;
  REQUIRE(detail::fsync_file(p.string(), &why));
  REQUIRE(why.empty());

  fs::remove_all(dir);
}

TEST_CASE("fsync_file flushes a file that is still open for append", "[durability]") {
  // The writer's real shape: the .tdat stream is closed, then flushed, while
  // other handles may still be around. Opening the file for flushing must not
  // be refused by sharing rules (the Windows path passes FILE_SHARE_*).
  const auto dir = scratch("append");
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
  fs::remove_all(dir);
}

TEST_CASE("fsync_file_or_throw succeeds quietly on a real file", "[durability]") {
  const auto dir = scratch("orthrow");
  const auto p = dir / "data.bin";
  write_file(p, "payload");
  REQUIRE_NOTHROW(detail::fsync_file_or_throw(p.string()));
  fs::remove_all(dir);
}

TEST_CASE("a missing file is a reported failure, not a silent one", "[durability]") {
  const auto dir = scratch("missing");
  const auto p = (dir / "absent.bin").string();

  std::string why;
  REQUIRE_FALSE(detail::fsync_file(p, &why));
  // The reason must come back. "the write is not durable" with no cause is a
  // bug report nobody can act on — diagnosing the Windows failure needed the
  // OS error, and it was not there.
  REQUIRE_FALSE(why.empty());

  REQUIRE_THROWS_AS(detail::fsync_file_or_throw(p), IoError);
  fs::remove_all(dir);
}

TEST_CASE("replace_file publishes over an existing target", "[durability]") {
  const auto dir = scratch("replace");
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

  fs::remove_all(dir);
}
