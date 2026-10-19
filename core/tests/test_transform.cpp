// mef3io — transform_session / verify_transform / check_transform (#73),
// core level: times, samples and the regions other writers fill copied
// verbatim; channels, passwords, name and the edited metadata changed; the
// provenance stamp; the refusals; and a tampered copy failing verification.
// tests/test_p30_transform.py covers the Python API, records and the CLI.
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <random>
#include <vector>

#include "mef3io/byteio.hpp"
#include "mef3io/crc.hpp"
#include "mef3io/headers.hpp"
#include "mef3io/naming.hpp"
#include "mef3io/session.hpp"
#include "mef3io/session_writer.hpp"
#include "mef3io/transform.hpp"

using namespace mef3io;
namespace fsys = std::filesystem;

namespace {

constexpr si8 START = 1600000000000000;
constexpr std::size_t UH = fmt::UNIVERSAL_HEADER_BYTES;
constexpr std::size_t TMET = fmt::METADATA_FILE_BYTES;
constexpr std::size_t S2 = fmt::METADATA_SECTION_2_OFFSET;
constexpr std::size_t S2_DISCRETIONARY = 8592;  // meflib's writer region in section 2

fsys::path fresh(const std::string& name) {
  const auto dir = fsys::temp_directory_path() / name;
  fsys::remove_all(dir);
  fsys::create_directories(dir);
  return dir;
}

std::string write_session(const fsys::path& dir) {
  const std::string path = (dir / "subject_x.mefd").string();
  SessionWriter w(path, true);
  SessionMetadata md;
  md.subject_name_1 = "Jane";
  md.recording_location = "Ward 3";
  w.set_metadata(md);
  std::mt19937 rng(73);
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

void spit(const fsys::path& p, const std::vector<ui1>& b) {
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
}

/// Plant bytes in an unencrypted .tmet's section-2 discretionary region (as
/// another writer would) and re-seal both CRCs.
void plant_region(const fsys::path& tmet, ui1 value) {
  auto b = slurp(tmet);
  std::fill_n(b.begin() + S2 + S2_DISCRETIONARY, 16, value);
  byteio::write<ui4>(b, 4, crc::calculate(std::span<const ui1>(b).subspan(UH, TMET - UH)));
  byteio::write<ui4>(b, 0, crc::calculate(std::span<const ui1>(b).subspan(4, UH - 4)));
  spit(tmet, b);
}

const fsys::path A_TMET = fsys::path("a.timd") / "a-000000.segd" / "a-000000.tmet";

}  // namespace

TEST_CASE("transform_session: times, samples and foreign regions verbatim; edits applied") {
  const auto dir = fresh("mef3io_transform_core");
  const std::string src = write_session(dir);
  plant_region(fsys::path(src) / A_TMET, 0x5A);
  const std::string dst = (dir / "study.mefd").string();
  TransformOptions o;
  o.exclude_channels = {"b"};
  o.metadata.subject_name_1 = "";  // cleared
  o.metadata.channel_descriptions["a"] = "left hippocampus";
  const TransformReport rep = transform_session(src, dst, o);
  REQUIRE(rep.verification.ok);
  CHECK(rep.session_name == "study");
  CHECK(rep.channels_kept == std::vector<std::string>{"a"});
  CHECK(rep.channels_excluded == std::vector<std::string>{"b"});
  CHECK(rep.metadata_fields_changed == 2);
  CHECK_FALSE(fsys::exists(fsys::path(dst) / "b.timd"));

  Session s1(src), s2(dst);
  CHECK(s2.channels() == std::vector<std::string>{"a"});
  const auto& i1 = s1.channel_info("a");
  const auto& i2 = s2.channel_info("a");
  CHECK(i2.start_time == i1.start_time);  // not moved
  CHECK(i2.end_time == i1.end_time);
  CHECK(i2.subject_name_1.empty());
  CHECK(i2.recording_location == "Ward 3");  // not edited: kept
  CHECK(i2.channel_description == "left hippocampus");
  const auto r1 = s1.read_runs("a"), r2 = s2.read_runs("a");
  REQUIRE(r1.size() == r2.size());
  for (std::size_t i = 0; i < r1.size(); ++i) {
    CHECK(r2[i].samples == r1[i].samples);
    CHECK(r2[i].start_uutc == r1[i].start_uutc);
  }
  for (const auto& seg : s2.segment_map("a")) {
    REQUIRE(seg.provenance.present);
    CHECK(seg.provenance.last_operation ==
          static_cast<ui1>(fmt::provenance::Operation::Transform));
  }
  CHECK(std::string(fmt::provenance::operation_name(8)) == "transform");

  // Another writer's region is kept; the copy names its source file as its
  // provenance and gets a file UUID of its own.
  const auto sb = slurp(fsys::path(src) / A_TMET), db = slurp(fsys::path(dst) / A_TMET);
  CHECK(std::all_of(db.begin() + S2 + S2_DISCRETIONARY, db.begin() + S2 + S2_DISCRETIONARY + 16,
                    [](ui1 c) { return c == 0x5A; }));
  CHECK(!std::equal(sb.begin() + 836, sb.begin() + 852, db.begin() + 836));  // file UUID
  CHECK(byteio::read_string(db, 308, 256) == "study");

  CHECK(verify_transform(src, dst, o).ok);
  // A different edit than the one the copy was made with is reported.
  TransformOptions other = o;
  other.metadata.channel_descriptions["a"] = "right hippocampus";
  CHECK_FALSE(verify_transform(src, dst, other).ok);
}

TEST_CASE("transform_session: a verbatim copy with new passwords") {
  const auto dir = fresh("mef3io_transform_pw");
  const std::string src = write_session(dir);
  const std::string dst = (dir / "locked.mefd").string();
  TransformOptions o;
  o.output_password_1 = "pw-one";
  o.output_password_2 = "pw-two";
  const TransformReport rep = transform_session(src, dst, o);
  REQUIRE(rep.verification.ok);
  CHECK(rep.output_encrypted);
  CHECK(rep.metadata_fields_changed == 0);
  Session s2(dst, "pw-two");
  CHECK(s2.channel_info("a").subject_name_1 == "Jane");
  CHECK(s2.channel_info("b").start_time == Session(src).channel_info("b").start_time);
}

TEST_CASE("transform_session: a tampered copy fails verification") {
  const auto dir = fresh("mef3io_transform_tamper");
  const std::string src = write_session(dir);
  const std::string dst = (dir / "copy.mefd").string();
  TransformOptions o;
  REQUIRE(transform_session(src, dst, o).verification.ok);
  plant_region(fsys::path(dst) / A_TMET, 0x33);  // not in the source
  const VerifyReport v = verify_transform(src, dst, o);
  CHECK_FALSE(v.ok);
  CHECK(std::any_of(v.problems.begin(), v.problems.end(),
                    [](const VerifyIssue& i) { return i.field == "section 2"; }));
}

TEST_CASE("transform_session refuses bad edits up front, and leaves nothing behind") {
  const auto dir = fresh("mef3io_transform_refuse");
  const std::string src = write_session(dir);
  const std::string dst = (dir / "out.mefd").string();
  TransformOptions o;
  o.metadata.subject_id = std::string(200, 'x');               // field holds 127 bytes
  o.metadata.reference_descriptions["nope"] = "not a channel";  // not in the copy
  o.exclude_channels = {"b"};
  o.metadata.channel_descriptions["b"] = "left out";            // left out of the copy
  const CopyCheck c = check_transform(src, dst, o);
  CHECK_FALSE(c.ok);
  std::vector<std::string> codes;
  for (const auto& r : c.refusals) codes.push_back(r.code);
  CHECK(std::count(codes.begin(), codes.end(), "metadata-too-long") == 1);
  CHECK(std::count(codes.begin(), codes.end(), "metadata-channel-unknown") == 2);
  try {
    transform_session(src, dst, o);
    FAIL("expected a refusal");
  } catch (const std::invalid_argument& e) {
    const auto* ce = dynamic_cast<const CopyError*>(&e);
    REQUIRE(ce);
    CHECK(ce->code() == "metadata-too-long");
  }
  CHECK_FALSE(fsys::exists(dst));
  CHECK_FALSE(fsys::exists(dst + ".partial"));
}

TEST_CASE("transform_session time_range: an exact cut holds exactly the window") {
  const auto dir = fresh("mef3io_transform_cut");
  const std::string src = write_session(dir);  // 60 s at 1 kHz per channel
  const std::string dst = (dir / "cut.mefd").string();
  TransformOptions o;
  const si8 t0 = START + 1'234'567, t1 = START + 20'000'000;
  o.time_range = std::make_pair(t0, t1);
  const TransformReport rep = transform_session(src, dst, o);
  REQUIRE(rep.verification.ok);
  CHECK(rep.blocks_trimmed > 0);
  Session s1(src), s2(dst);
  for (const auto& ch : s2.channels()) {
    const auto& info = s2.channel_info(ch);
    CHECK(info.start_time >= t0);
    CHECK(info.end_time <= t1);
    const auto r1 = s1.read_runs(ch), r2 = s2.read_runs(ch);
    REQUIRE(r2.size() == 1);
    // The copy's samples are the source's from the first one at or after t0.
    const si8 skip = (r2[0].start_uutc - r1[0].start_uutc) / 1000;  // 1 kHz
    REQUIRE(skip >= 0);
    CHECK(r2[0].start_uutc == r1[0].start_uutc + skip * 1000);
    REQUIRE(r2[0].samples.size() == static_cast<std::size_t>((t1 - r2[0].start_uutc + 999) / 1000));
    CHECK(std::equal(r2[0].samples.begin(), r2[0].samples.end(),
                     r1[0].samples.begin() + skip));
    CHECK(s2.segment_map(ch).front().segment_number == 0);
  }
  CHECK(verify_transform(src, dst, o).ok);
  TransformOptions wrong = o;  // the same copy against another window
  wrong.time_range = std::make_pair(t0, t1 + 1'000'000);
  CHECK_FALSE(verify_transform(src, dst, wrong).ok);
}

TEST_CASE("time_range and name_drift: first/last metadata only; names agree") {
  const auto dir = fresh("mef3io_naming_core");
  const std::string src = write_session(dir);
  const TimeRange r = time_range(src);
  REQUIRE(r.start_uutc);
  CHECK(*r.start_uutc == START);
  CHECK(r.offset_known);
  CHECK(r.files_read == 2);  // one segment per channel: read once each
  const NameDrift d = name_drift(src);
  CHECK(d.consistent);
  CHECK(d.folder == "subject_x");
}
