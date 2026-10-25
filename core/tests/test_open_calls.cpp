// How many file-system requests opening a session costs (#141). On a network
// share every request is a full round trip (~110 ms measured on SMB), so the
// open must stay at one listing per folder plus one bounded read per segment's
// .tmet — no per-file exists(), no size probe. Pinned here through a counting
// SessionSource decorator so a new per-segment request cannot slip in.
//
// The "[.open-bench]" case is hidden: it adds a fixed delay per request and
// prints open times for the PR / docs/benchmarks.md numbers.
//   build-core/mef3io_tests "[.open-bench]"
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../src/session_layout.hpp"
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

// The layout walk as it was before #141 (three exists() per segment, plus two
// for an incomplete one and one for a channel without segments), kept here as
// the reference the listing-based walk must agree with.
detail::SessionLayout reference_layout(const SessionSource& src) {
  using namespace detail;
  SessionLayout out;
  for (const auto& entry : src.list_dir("")) {
    if (!entry.is_dir) continue;
    if (entry.name.ends_with(".vidd")) {
      out.video_channels.push_back(entry.name.substr(0, entry.name.size() - 5));
      continue;
    }
    if (!entry.name.ends_with(".timd")) continue;
    LayoutChannel ch;
    ch.name = entry.name.substr(0, entry.name.size() - 5);
    for (const auto& seg_entry : src.list_dir(entry.name)) {
      if (!seg_entry.is_dir || !seg_entry.name.ends_with(".segd")) continue;
      const std::string stem = seg_entry.name.substr(0, seg_entry.name.size() - 5);
      const std::string dir = entry.name + "/" + seg_entry.name;
      LayoutSegment seg;
      seg.number = layout_segment_number(stem);
      seg.tmet = dir + "/" + stem + ".tmet";
      seg.tidx = dir + "/" + stem + ".tidx";
      seg.tdat = dir + "/" + stem + ".tdat";
      const bool has[3] = {src.exists(seg.tmet), src.exists(seg.tidx), src.exists(seg.tdat)};
      if (has[0] && has[1] && has[2]) {
        ch.segments.push_back(std::move(seg));
        continue;
      }
      IncompleteSegment bad;
      bad.number = seg.number;
      bad.dir = dir;
      const std::string* files[3] = {&seg.tmet, &seg.tidx, &seg.tdat};
      const char* exts[3] = {".tmet", ".tidx", ".tdat"};
      for (int i = 0; i < 3; ++i) {
        if (has[i])
          bad.present.push_back(*files[i]);
        else
          bad.missing += (bad.missing.empty() ? "" : ", ") + std::string(exts[i]);
      }
      for (const char* ext : {".rdat", ".ridx"}) {
        const std::string f = dir + "/" + stem + ext;
        if (src.exists(f)) {
          bad.present.push_back(f);
          if (std::string(ext) == ".rdat") bad.has_records = true;
        }
      }
      ch.incomplete.push_back(std::move(bad));
    }
    std::sort(ch.incomplete.begin(), ch.incomplete.end(),
              [](const auto& a, const auto& b) { return a.number < b.number; });
    if (ch.segments.empty()) {
      if (src.exists(ch.rdat())) out.record_only_channels.push_back(ch.name);
      out.incomplete_channels.push_back(std::move(ch));
      continue;
    }
    std::sort(ch.segments.begin(), ch.segments.end(),
              [](const auto& a, const auto& b) { return a.number < b.number; });
    out.channels.push_back(std::move(ch));
  }
  return out;
}

void require_same_channels(const std::vector<detail::LayoutChannel>& a,
                           const std::vector<detail::LayoutChannel>& b) {
  REQUIRE(a.size() == b.size());
  for (std::size_t i = 0; i < a.size(); ++i) {
    CAPTURE(a[i].name);
    REQUIRE(a[i].name == b[i].name);
    REQUIRE(a[i].segments.size() == b[i].segments.size());
    for (std::size_t j = 0; j < a[i].segments.size(); ++j) {
      REQUIRE(a[i].segments[j].number == b[i].segments[j].number);
      REQUIRE(a[i].segments[j].tmet == b[i].segments[j].tmet);
      REQUIRE(a[i].segments[j].tidx == b[i].segments[j].tidx);
      REQUIRE(a[i].segments[j].tdat == b[i].segments[j].tdat);
    }
    REQUIRE(a[i].incomplete.size() == b[i].incomplete.size());
    for (std::size_t j = 0; j < a[i].incomplete.size(); ++j) {
      const auto &x = a[i].incomplete[j], &y = b[i].incomplete[j];
      REQUIRE(x.number == y.number);
      REQUIRE(x.dir == y.dir);
      REQUIRE(x.present == y.present);
      REQUIRE(x.missing == y.missing);
      REQUIRE(x.has_records == y.has_records);
    }
  }
}

void require_same_layout(const detail::SessionLayout& a, const detail::SessionLayout& b) {
  require_same_channels(a.channels, b.channels);
  require_same_channels(a.incomplete_channels, b.incomplete_channels);
  REQUIRE(a.record_only_channels == b.record_only_channels);
  REQUIRE(a.video_channels == b.video_channels);
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

TEST_CASE("opening a session: one listing per folder, one read per segment",
          "[open-calls]") {
  const int C = 3, S = 4;
  const auto path = write_session("mef3io_open_calls.mefd", C, S);

  SECTION("Session, directory") {
    auto src = counted(path);
    Session s{std::shared_ptr<const SessionSource>(src)};
    const auto c = src->calls();
    REQUIRE(s.channels().size() == C);
    CHECK(c.list_dir == 1 + C + C * S);  // root + every .timd + every .segd
    CHECK(c.read_head == C * S);         // one bounded .tmet read per segment
    CHECK(c.read_all == 0);              // the index stays lazy
    CHECK(c.read_range == 0);
    CHECK(c.exists == 0);
    CHECK(c.file_size == 0);
  }
  SECTION("Reader, directory") {
    auto src = counted(path);
    Reader r{std::shared_ptr<const SessionSource>(src)};
    CHECK(src->calls().total() == 1 + C + C * S + C * S);
  }
  SECTION("Session, tar archive") {
    const auto tar = archive_session(path, "", true);
    auto src = std::make_shared<CountingSource>(std::make_shared<TarSource>(tar));
    Session s{std::shared_ptr<const SessionSource>(src)};
    const auto c = src->calls();
    REQUIRE(s.channels().size() == C);
    CHECK(c.list_dir == 1 + C + C * S);
    CHECK(c.read_head == C * S);
    CHECK(c.exists + c.read_all + c.read_range + c.file_size == 0);
    fsys::remove(tar);
  }
}

TEST_CASE("the listing-based layout agrees with the exists()-based one", "[open-calls]") {
  const auto path = write_session("mef3io_open_layout.mefd", 4, 3);
  // ch0: complete. ch1 segment 1 lost its .tdat but kept records next to it.
  fsys::remove(segd(path, "ch1", 1) + "/" + stem("ch1", 1) + ".tdat");
  { std::ofstream(segd(path, "ch1", 1) + "/" + stem("ch1", 1) + ".rdat") << "x"; }
  // ch2 segment 0: .tmet and .tidx gone; segment 2: its .tmet is a DANGLING link.
  fsys::remove(segd(path, "ch2", 0) + "/" + stem("ch2", 0) + ".tmet");
  fsys::remove(segd(path, "ch2", 0) + "/" + stem("ch2", 0) + ".tidx");
  { std::ofstream(segd(path, "ch2", 0) + "/" + stem("ch2", 0) + ".ridx") << "x"; }
  const auto tmet2 = segd(path, "ch2", 2) + "/" + stem("ch2", 2) + ".tmet";
  fsys::remove(tmet2);
  bool linked = true;
  try {
    fsys::create_symlink(segd(path, "ch2", 2) + "/nowhere", tmet2);
  } catch (...) {
    linked = false;  // no symlinks here (e.g. unprivileged Windows)
  }
  // ch3: no complete segment left, but channel-level records -> record-only.
  for (int n = 0; n < 3; ++n) fsys::remove(segd(path, "ch3", n) + "/" + stem("ch3", n) + ".tdat");
  { std::ofstream(path + "/ch3.timd/ch3.rdat") << "x"; }

  const auto src = open_session_source(path);
  const auto now = detail::discover_layout(*src);
  require_same_layout(now, reference_layout(*src));
  REQUIRE(now.record_only_channels == std::vector<std::string>{"ch3"});
  if (linked) {
    // The dangling link reads as missing, as exists() always said.
    bool found = false;
    for (const auto& ch : now.channels)
      if (ch.name == "ch2")
        for (const auto& bad : ch.incomplete)
          if (bad.number == 2) found = bad.missing == ".tmet";
    REQUIRE(found);
  }
  // Only the names the listings do not vouch for are asked about one by one.
  CountingSource c(src);
  detail::discover_layout(c);
  // ch1/1: .tdat, .ridx; ch2/0: .tmet, .tidx, .rdat; ch2/2: the link (or the
  // missing .tmet), .rdat, .ridx; ch3: .tdat, .rdat, .ridx in each of 3.
  CHECK(c.calls().exists == 2 + 3 + 3 + 3 * 3);

  // The lenient open reports exactly what it did before: incomplete segments
  // are skipped silently, a dangling .tmet is not even attempted.
  Session s(path, "", false);
  REQUIRE(s.problems().empty());
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
