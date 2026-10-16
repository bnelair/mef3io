// mef3io — anonymize_session / verify_anonymized (#70), core level. The
// Python suite (tests/test_p29_anonymize.py) covers records, encryption,
// legacy and offset sessions and the planted leaks; this pins the C++ API on
// its own: shifted times, identical samples, the provenance stamp, the
// refusals and "a cancel leaves nothing".
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <random>
#include <stdexcept>
#include <vector>

#include "mef3io/anonymize.hpp"
#include "mef3io/byteio.hpp"
#include "mef3io/reader.hpp"
#include "mef3io/session.hpp"
#include "mef3io/session_writer.hpp"

using namespace mef3io;
namespace fsys = std::filesystem;

namespace {

constexpr si8 START = 1600000000000000;
constexpr si8 DAY = 86400000000LL;

fsys::path fresh(const std::string& name) {
  const auto dir = fsys::temp_directory_path() / name;
  fsys::remove_all(dir);
  fsys::create_directories(dir);
  return dir;
}

std::string write_session(const fsys::path& dir) {
  const std::string path = (dir / "subject_x.mefd").string();
  SessionWriter w(path, true);
  std::mt19937 rng(70);
  std::uniform_int_distribution<si4> dist(-30000, 30000);
  for (const char* ch : {"a", "b"}) {
    std::vector<si4> x(60000);
    for (auto& v : x) v = dist(rng);
    w.write_int32(ch, x, 1.0, START, 1000.0, {}, false);
  }
  return path;
}

std::vector<ui1> slurp(const fsys::path& p) {
  std::ifstream f(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), {}};
}

}  // namespace

TEST_CASE("anonymize_session: times shifted, samples identical, provenance 6") {
  const auto dir = fresh("mef3io_anon_core");
  const std::string src = write_session(dir);
  const std::string dst = (dir / "P01.mefd").string();
  AnonymizeOptions o;
  o.shift_us = -5000 * DAY;
  const AnonymizeReport rep = anonymize_session(src, dst, o);
  REQUIRE(rep.verification.ok);
  CHECK(rep.session_name == "P01");
  CHECK(rep.channels_kept == std::vector<std::string>{"a", "b"});

  Session s1(src), s2(dst);
  for (const auto& ch : s1.channels()) {
    CHECK(s2.channel_info(ch).start_time == s1.channel_info(ch).start_time + o.shift_us);
    CHECK(s2.channel_info(ch).recording_time_offset == 0);
    const auto r1 = s1.read_runs(ch), r2 = s2.read_runs(ch);
    REQUIRE(r1.size() == r2.size());
    for (std::size_t i = 0; i < r1.size(); ++i) {
      CHECK(r2[i].samples == r1[i].samples);
      CHECK(r2[i].start_uutc == r1[i].start_uutc + o.shift_us);
    }
    for (const auto& seg : s2.segment_map(ch)) {
      REQUIRE(seg.provenance.present);
      CHECK(seg.provenance.last_operation == 6);
      CHECK(std::string(fmt::provenance::operation_name(6)) == "anonymize");
    }
  }
  // Every file of the copy names the new session.
  for (const auto& e : fsys::recursive_directory_iterator(dst)) {
    if (!e.is_regular_file()) continue;
    const auto b = slurp(e.path());
    CHECK(byteio::read_string(b, 308, 256) == "P01");
  }
  // verify() standalone agrees, and catches a wrong shift.
  CHECK(verify_anonymized(src, dst, o.shift_us).ok);
  CHECK_FALSE(verify_anonymized(src, dst, o.shift_us + 1).ok);
}

TEST_CASE("anonymize_session refuses, and leaves nothing behind") {
  const auto dir = fresh("mef3io_anon_refuse");
  const std::string src = write_session(dir);
  AnonymizeOptions o;
  o.shift_us = 3600000000LL;  // below span + one day
  CHECK_THROWS_AS(anonymize_session(src, (dir / "A.mefd").string(), o), std::invalid_argument);
  o.shift_us = -(START + DAY);  // into times <= 0
  CHECK_THROWS_AS(anonymize_session(src, (dir / "A.mefd").string(), o), std::invalid_argument);
  o.shift_us = 1000 * DAY;
  o.exclude_channels = {"a", "b"};  // nothing left
  CHECK_THROWS_AS(anonymize_session(src, (dir / "A.mefd").string(), o), std::invalid_argument);
  o.exclude_channels = {};
  o.output_password_1 = "only-one";
  CHECK_THROWS_AS(anonymize_session(src, (dir / "A.mefd").string(), o), std::invalid_argument);
  o.output_password_1 = "";
  CHECK_THROWS_AS(anonymize_session(src, src + "/inner.mefd", o), IoError);

  CancelToken token;
  token.cancel();
  Control ctl;
  ctl.cancel = &token;
  CHECK_THROWS_AS(anonymize_session(src, (dir / "A.mefd").string(), o, ctl), Cancelled);
  CHECK_FALSE(fsys::exists(dir / "A.mefd"));
  CHECK_FALSE(fsys::exists(dir / "A.mefd.partial"));
}
