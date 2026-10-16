// inspect_session (#67): the C++ entry point. The full behaviour is pinned in
// tests/test_p26_inspect.py; this covers the core contract without Python.
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <vector>

#include "mef3io/control.hpp"
#include "mef3io/errors.hpp"
#include "mef3io/inspect.hpp"
#include "mef3io/session.hpp"
#include "mef3io/session_writer.hpp"
#include "mef3io/tar.hpp"

using namespace mef3io;
namespace fsys = std::filesystem;

namespace {
constexpr si8 START = 1577836800000000;

std::string write_session(const std::string& name, const std::string& pw1 = "",
                          const std::string& pw2 = "") {
  const auto dir = fsys::temp_directory_path() / name;
  fsys::remove_all(dir);
  SessionWriter w(dir.string(), true, pw1, pw2);
  SessionMetadata md;
  md.subject_id = "MRN-1";
  w.set_metadata(md);
  std::vector<si4> a(1000);
  for (int i = 0; i < 1000; ++i) a[i] = i;
  w.write_int32("ch1", a, 1.0, START, 250.0);
  w.write_int32("ch1", a, 1.0, START + 10'000'000, 250.0, {}, true);  // second segment
  Record note;
  note.type = "Note";
  note.time = START + 1000;
  note.text = "hello";
  w.write_records(std::nullopt, {note});
  return dir.string();
}

const IdentityField& field(const SessionSummary& s, const std::string& name) {
  for (const auto& f : s.identity)
    if (f.name == name) return f;
  FAIL("no identity field " << name);
  throw;  // unreachable
}
}  // namespace

TEST_CASE("inspect_session agrees with the Session and hides identity values") {
  const auto path = write_session("mef3io_inspect.mefd");
  const auto s = inspect_session(path);
  Session sess(path);
  REQUIRE(s.layout == "directory");
  REQUIRE(s.channels.size() == 1);
  const auto& c = s.channels[0];
  const auto& info = sess.channel_info("ch1");
  REQUIRE(c.sampling_frequency == info.sampling_frequency);
  REQUIRE(c.start_time == info.start_time);
  REQUIRE(c.end_time == info.end_time);
  REQUIRE(c.number_of_samples == info.number_of_samples);
  REQUIRE(c.segments.size() == 2);
  REQUIRE(c.gaps.size() == 1);  // 4 s of data, then 6 s nothing, then the next segment
  REQUIRE(c.gaps[0].start == START + 4'000'000);
  REQUIRE(c.gaps[0].end == START + 10'000'000);
  REQUIRE(s.records.size() == 1);
  REQUIRE(s.records[0].time == START + 1000);
  REQUIRE(s.records[0].text_bytes == 5);
  REQUIRE_FALSE(s.records[0].full.has_value());
  REQUIRE(field(s, "subject_id").occurrences == 2);
  REQUIRE(field(s, "subject_id").values.empty());
  REQUIRE(s.time_offset.status == "none");
  REQUIRE(s.warnings.empty());

  InspectOptions opt;
  opt.include_values = true;
  const auto v = inspect_session(path, opt);
  REQUIRE(field(v, "subject_id").values == std::vector<std::string>{"MRN-1"});
  REQUIRE(v.records[0].full->text == "hello");
}

TEST_CASE("inspect_session reads a tar archive in place") {
  const auto path = write_session("mef3io_inspect_tar.mefd");
  fsys::remove(path + ".tar");  // a previous run's archive
  const auto tar = archive_session(path);
  const auto d = inspect_session(path);
  const auto t = inspect_session(tar);
  REQUIRE(t.layout == "tar");
  REQUIRE(t.archive_bytes.has_value());
  REQUIRE(t.total_bytes == d.total_bytes);
  REQUIRE(t.channels[0].start_time == d.channels[0].start_time);
  REQUIRE(t.records.size() == d.records.size());
}

TEST_CASE("inspect_session describes an encrypted session it cannot open") {
  const auto path = write_session("mef3io_inspect_enc.mefd", "pw1", "pw2");
  const auto s = inspect_session(path);  // no password: described, not refused
  REQUIRE(s.encryption.encrypted);
  REQUIRE(s.encryption.access == "none");
  REQUIRE(s.encryption.section2_locked == 2);
  REQUIRE(s.encryption.section3_locked == 2);
  REQUIRE_FALSE(s.channels[0].sampling_frequency.has_value());
  // Offset locked: times assume 0, as the Reader does, and are flagged.
  REQUIRE(s.channels[0].start_time == START);
  REQUIRE(s.offset_assumed_zero);
  REQUIRE(s.channels[0].segments[0].offset_assumed_zero);
  REQUIRE(s.records[0].body_locked);
  REQUIRE(s.time_offset.status == "locked");

  InspectOptions wrong;
  wrong.password = "nope";
  REQUIRE_THROWS_AS(inspect_session(path, wrong), PasswordError);

  InspectOptions full;
  full.password = "pw2";
  const auto f = inspect_session(path, full);
  REQUIRE(f.encryption.access == "full");
  REQUIRE(f.channels[0].start_time == START);
  REQUIRE(f.records[0].text_bytes == 5);
  REQUIRE_FALSE(f.offset_assumed_zero);
}

TEST_CASE("inspect_session honours a cancel token, lenient or not") {
  const auto path = write_session("mef3io_inspect_cancel.mefd");
  CancelToken token;
  token.cancel();
  Control ctl;
  ctl.cancel = &token;
  for (bool strict : {true, false}) {
    InspectOptions opt;
    opt.strict = strict;
    // Lenient or not, a set token stops the call with Cancelled (which is not a
    // std::exception, so no containment site could swallow it either).
    REQUIRE_THROWS_AS(inspect_session(path, opt, ctl), Cancelled);
  }
  std::vector<std::pair<std::uint64_t, std::uint64_t>> calls;
  Control prog;
  prog.progress = [&](std::uint64_t d, std::uint64_t t) { calls.emplace_back(d, t); };
  inspect_session(path, {}, prog);
  REQUIRE(calls.size() >= 2);
  REQUIRE(calls.front().first == 0);
  REQUIRE(calls.back().first == calls.back().second);
}
