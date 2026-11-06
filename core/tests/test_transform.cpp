// mef3io — transform_session / verify_transform / check_transform (#73),
// core level: times, samples and the regions other writers fill copied
// verbatim; channels, passwords, name and the edited metadata changed; the
// provenance stamp; the refusals; and a tampered copy failing verification.
// tests/test_p30_transform.py covers the Python API, records and the CLI.
#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#include "mef3io/byteio.hpp"
#include "mef3io/crc.hpp"
#include "mef3io/headers.hpp"
#include "mef3io/naming.hpp"
#include "mef3io/session.hpp"
#include "mef3io/session_writer.hpp"
#include "mef3io/merge.hpp"
#include "mef3io/source.hpp"
#include "mef3io/transform.hpp"
#include "../src/durability.hpp"
#include "../src/out_file.hpp"

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

TEST_CASE("transform_session drop_records: no record file anywhere; verify expects none") {
  const auto dir = fresh("mef3io_transform_drop");
  const std::string src = write_session(dir);
  {
    SessionWriter w(src, false);
    w.write_records(std::nullopt, {{.type = "Note", .time = START, .text = "s"}});
    w.write_records(std::string("a"), {{.type = "Note", .time = START + 1, .text = "c"}});
  }
  const std::string dst = (dir / "dropped.mefd").string();
  TransformOptions o;
  o.drop_records = true;
  const TransformReport rep = transform_session(src, dst, o);
  REQUIRE(rep.verification.ok);
  CHECK(rep.records == 0);
  CHECK(rep.records_dropped == 2);
  for (const auto& e : fsys::recursive_directory_iterator(dst)) {
    CHECK(e.path().extension() != ".rdat");
    CHECK(e.path().extension() != ".ridx");
  }
  CHECK(verify_transform(src, dst, o).ok);
  TransformOptions keep;  // the strict default expects the annotations
  CHECK_FALSE(verify_transform(src, dst, keep).ok);
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

TEST_CASE("rename_session: all four places, a crash rolled back, writes refused meanwhile") {
  const auto dir = fresh("mef3io_rename_core");
  const std::string src = write_session(dir);
  const auto target = (dir / "pt1.mefd").string();
  RenameOptions crash;
  crash.crash_at = 3;  // the marker is in, two headers changed
  REQUIRE_THROWS_AS(rename_session(src, "pt1", crash), IoError);
  REQUIRE(operation_marker(src));
  CHECK(operation_marker(src)->operation == "rename");
  CHECK_THROWS_AS(SessionWriter(src), WriteConflictError);
  CHECK_THROWS_AS(clear_operation_marker(src), WriteConflictError);  // half-changed

  const RenameReport rep = rename_session(src, "pt1");  // rolls back, then renames
  CHECK(!rep.rolled_back.empty());
  CHECK(rep.new_path == fsys::absolute(target).string());
  CHECK(rep.folder_renamed);
  const NameDrift d = name_drift(target);
  CHECK(d.consistent);
  CHECK(d.folder == "pt1");
  REQUIRE(d.stored_names.size() == 1);
  CHECK(d.stored_names.begin()->first == "pt1");
  CHECK(!operation_marker(target));
  CHECK(!fsys::exists(src));
  CHECK(!fsys::exists(src + ".repair-backup"));

  // Undo: back to the old name, exactly the descriptions changed.
  RenameOptions undo;
  undo.descriptions = rep.descriptions_changed;
  const RenameReport back = rename_session(target, "subject_x", undo);
  CHECK(back.descriptions_changed == rep.descriptions_changed);
  CHECK(name_drift(src).consistent);
}

TEST_CASE("merge_sessions: recordings that take turns merge; overlapping data is refused") {
  const auto dir = fresh("mef3io_merge_core");
  auto write = [&](const std::string& name, std::vector<std::pair<si8, std::size_t>> chunks) {
    const std::string path = (dir / name).string();
    SessionWriter w(path, true);
    for (const auto& [t, n] : chunks) {
      std::vector<si4> x(n);
      for (std::size_t i = 0; i < n; ++i) x[i] = static_cast<si4>(i % 1000);
      w.write_int32("a", x, 1.0, START + t, 1000.0, {}, false);  // one segment, gaps between
    }
    return path;
  };
  // A: 0-10 s and 30-40 s in ONE segment; B: 15-25 s, inside A's gap.
  const std::string a = write("a.mefd", {{0, 10'000}, {30'000'000, 10'000}});
  const std::string b = write("b.mefd", {{15'000'000, 10'000}});
  MergeOptions o;
  const MergeReport rep = merge_sessions({a, b}, (dir / "m.mefd").string(), o);
  REQUIRE(rep.verification.ok);
  CHECK(rep.segments == 3);
  CHECK(rep.segments_split == 1);
  Session m((dir / "m.mefd").string());
  const auto segs = m.segment_map("a");
  REQUIRE(segs.size() == 3);
  CHECK(segs[0].start_time == START);
  CHECK(segs[1].start_time == START + 15'000'000);
  CHECK(segs[2].start_time == START + 30'000'000);
  CHECK(verify_merge({a, b}, (dir / "m.mefd").string(), o).ok);

  const std::string c = write("c.mefd", {{5'000'000, 10'000}});  // 5-15 s: overlaps A
  const CopyCheck chk = check_merge({a, c}, (dir / "n.mefd").string(), o);
  CHECK_FALSE(chk.ok);
  CHECK(std::any_of(chk.refusals.begin(), chk.refusals.end(),
                    [](const CopyRefusal& r) { return r.code == "merge-overlap"; }));
}

TEST_CASE("check_merge: options a merge does not take are refused with their codes") {
  const auto dir = fresh("mef3io_merge_options");
  const std::string a = write_session(dir);
  const auto other = fresh("mef3io_merge_options_b");
  const std::string b = write_session(other);
  MergeOptions o;
  o.time_range = std::make_pair(START, START + 1'000'000);
  o.rename = false;
  const CopyCheck c = check_merge({a, b}, (dir / "m.mefd").string(), o);
  std::vector<std::string> codes;
  for (const auto& r : c.refusals) codes.push_back(r.code);
  CHECK(std::count(codes.begin(), codes.end(), "merge-time-range") == 1);
  CHECK(std::count(codes.begin(), codes.end(), "merge-rename") == 1);
}

TEST_CASE("merge_sessions: a derived record file names the source file it came from") {
  // A copy of a file whose provenance_UUID is unset names the source FILE as
  // its origin (a store the 2.1.1 manylinux build dropped from the record
  // files of a merge, so the merge failed its own check; ctest in a Release
  // build is what catches it). The writer leaves both UUIDs zero, so the
  // transformed part is the first file that carries a file UUID.
  const auto dir = fresh("mef3io_merge_prov");
  auto write = [&](const std::string& name, si8 start) {
    const std::string path = (dir / name).string();
    SessionWriter w(path, true);
    std::vector<si4> x(10'000);
    for (std::size_t i = 0; i < x.size(); ++i) x[i] = static_cast<si4>(i % 1000);
    w.write_int32("a", x, 1.0, start, 1000.0, {}, false);
    Record r;
    r.type = "Note";
    r.time = start + 1'000'000;
    r.text = "note";
    w.write_records(std::nullopt, {r});
    return path;
  };
  const std::string a = write("a.mefd", START), b = write("b.mefd", START + 100'000'000);
  const std::string part = (dir / "part.mefd").string();
  TransformOptions to;
  to.time_range = std::make_pair(START, START + 5'000'000);
  REQUIRE(transform_session(a, part, to).verification.ok);
  const std::string out = (dir / "m.mefd").string();
  REQUIRE(merge_sessions({part, b}, out, MergeOptions{}).verification.ok);
  auto uuid = [&](const std::string& session, const char* stem, const char* ext, std::size_t off) {
    const auto f = slurp(fsys::path(session) / (std::string(stem) + ext));
    REQUIRE(f.size() >= UH);
    return std::vector<ui1>(f.begin() + static_cast<std::ptrdiff_t>(off),
                            f.begin() + static_cast<std::ptrdiff_t>(off) + 16);
  };
  constexpr std::size_t FILE_UUID = 836, PROV_UUID = 852;
  for (const char* ext : {".rdat", ".ridx"}) {
    const auto want = uuid(part, "part", ext, FILE_UUID);
    CHECK(want != std::vector<ui1>(16, 0));
    CHECK(uuid(out, "m", ext, PROV_UUID) == want);
  }
}

TEST_CASE("merge_sessions: per-source ranges, anonymize and threads give the same bytes") {
  const auto dir = fresh("mef3io_merge_onepass");
  auto write = [&](const std::string& name, si8 t, std::size_t n) {
    const std::string path = (dir / name).string();
    SessionWriter w(path, true);
    for (const char* ch : {"a", "b"}) {
      std::vector<si4> x(n);
      for (std::size_t i = 0; i < n; ++i) x[i] = static_cast<si4>((i * 7) % 1000);
      w.write_int32(ch, x, 1.0, START + t, 1000.0, {}, false);
    }
    return path;
  };
  const std::string a = write("a.mefd", 0, 20'000);           // 0-20 s
  const std::string b = write("b.mefd", 10'000'000, 20'000);  // 10-30 s: overlaps a
  MergeOptions o;
  CHECK_FALSE(check_merge({a, b}, (dir / "m.mefd").string(), o).ok);
  o.time_ranges = {std::make_pair(START, START + 10'000'000),
                   std::make_pair(START + 10'000'000, START + 30'000'000)};
  o.anonymize = AnonymizeOptions{};
  o.anonymize->shift_us = 3'000'000'000'000'000;  // ~95 years
  std::vector<std::vector<ui1>> tdat;
  for (int threads : {1, 4}) {
    o.n_threads = threads;
    const std::string dst = (dir / ("m" + std::to_string(threads) + ".mefd")).string();
    const MergeReport rep = merge_sessions({a, b}, dst, o);
    REQUIRE(rep.verification.ok);
    CHECK(rep.shift_us == o.anonymize->shift_us);
    CHECK(verify_merge({a, b}, dst, o).ok);
    Session m(dst);
    CHECK(m.segment_map("a").size() == 2);
    // The data file of segment 1 of channel a, past its universal header.
    std::ifstream f(dst + "/a.timd/a-000001.segd/a-000001.tdat", std::ios::binary);
    std::vector<ui1> bytes((std::istreambuf_iterator<char>(f)), {});
    REQUIRE(bytes.size() > 1024);
    tdat.emplace_back(bytes.begin() + 1024, bytes.end());
  }
  CHECK(tdat[0] == tdat[1]);
  o.n_threads = -1;
  CHECK_FALSE(check_merge({a, b}, (dir / "z.mefd").string(), o).ok);
}

// ---- review round 1 of the one-pass merge ------------------------------------
namespace {

std::string write_patient(const fsys::path& dir, const std::string& name, const std::string& patient,
                          si8 t, std::size_t n) {
  const std::string path = (dir / name).string();
  SessionWriter w(path, true);
  SessionMetadata md;
  md.subject_name_1 = patient;
  w.set_metadata(md);
  for (const char* ch : {"a", "b"}) {
    std::vector<si4> x(n);
    for (std::size_t i = 0; i < n; ++i) x[i] = static_cast<si4>((i * 7) % 1000);
    w.write_int32(ch, x, 1.0, START + t, 1000.0, {}, false);
  }
  return path;
}

std::string all_bytes(const fsys::path& root) {
  std::string out;
  for (const auto& e : fsys::recursive_directory_iterator(root))
    if (e.is_regular_file()) {
      const auto b = slurp(e.path());
      out.append(b.begin(), b.end());
      out += e.path().string();  // the names count too
    }
  return out;
}

std::atomic<int> g_fail_source{-1};
std::atomic<int> g_fail_attempts{0};  // attempts (0 = first, 1 = retry) < this fail
void fail_preopen(std::size_t k, int attempt) {
  if (static_cast<int>(k) == g_fail_source.load() && attempt < g_fail_attempts.load())
    throw std::runtime_error("injected: transient open failure");
}
struct PreopenGuard {
  PreopenGuard(int source, int attempts) {
    g_fail_source = source;
    g_fail_attempts = attempts;
    detail::merge_preopen_hook.store(&fail_preopen);
  }
  ~PreopenGuard() { detail::merge_preopen_hook.store(nullptr); }
};
std::atomic<int> g_threads_started{0};
void count_thread(unsigned) { ++g_threads_started; }

MergeOptions anon_ranges_options() {
  MergeOptions o;
  o.time_ranges = {std::make_pair(START, START + 10'000'000),
                   std::make_pair(START + 10'000'000, START + 30'000'000)};
  o.anonymize = AnonymizeOptions{};
  o.anonymize->shift_us = 3'000'000'000'000'000;
  o.n_threads = 1;
  return o;
}

}  // namespace

TEST_CASE("merge_sessions: anonymize with time ranges removes every source's patient and name") {
  const auto dir = fresh("mef3io_merge_anon_ranges");
  const std::string a = write_patient(dir, "Alpha_Jane.mefd", "Jane", 0, 20'000);
  const std::string b = write_patient(dir, "Beta_Smithers.mefd", "Smithers", 10'000'000, 20'000);
  const MergeOptions o = anon_ranges_options();
  const std::string dst = (dir / "S.mefd").string();
  const MergeReport rep = merge_sessions({a, b}, dst, o);
  REQUIRE(rep.verification.ok);
  CHECK(rep.shift_us == o.anonymize->shift_us);
  CHECK(rep.segments == 4);  // 2 channels x one segment per source
  const std::string blob = all_bytes(dst);
  for (const char* needle : {"Jane", "Smithers", "Alpha", "Beta"})
    CHECK(blob.find(needle) == std::string::npos);
  CHECK(verify_merge({a, b}, dst, o).ok);
}

TEST_CASE("merge_sessions: a source whose first open failed is still pooled and scanned") {
  const auto dir = fresh("mef3io_merge_preopen_fault");
  const std::string a = write_patient(dir, "Alpha_Jane.mefd", "Jane", 0, 20'000);
  const std::string b = write_patient(dir, "Beta_Smithers.mefd", "Smithers", 10'000'000, 20'000);
  const MergeOptions o = anon_ranges_options();
  const std::string clean = (dir / "clean.mefd").string();
  REQUIRE(merge_sessions({a, b}, clean, o).verification.ok);
  for (int k : {0, 1}) {
    const std::string dst = (dir / ("f" + std::to_string(k) + ".mefd")).string();
    PreopenGuard fault(k, 1);  // the parallel open of source k throws once
    const MergeReport rep = merge_sessions({a, b}, dst, o);
    REQUIRE(rep.verification.ok);
    const std::string blob = all_bytes(dst);
    for (const char* needle : {"Jane", "Smithers", "Alpha", "Beta"})
      CHECK(blob.find(needle) == std::string::npos);
    CHECK(slurp(dst + "/a.timd/a-000000.segd/a-000000.tdat").size() ==
          slurp(clean + "/a.timd/a-000000.segd/a-000000.tdat").size());
    CHECK(verify_merge({a, b}, dst, o).ok);
  }
  // identity-differs still compares the source whose first open failed
  MergeOptions keep = o;
  keep.anonymize->clear_subject = false;
  for (int k : {0, 1}) {
    PreopenGuard fault(k, 1);
    const CopyCheck chk = check_merge({a, b}, (dir / "d.mefd").string(), keep);
    CHECK(std::any_of(chk.refusals.begin(), chk.refusals.end(),
                      [](const CopyRefusal& r) { return r.code == "merge-identity-differs"; }));
  }
  // both pre-opens of a source fail (its own pre-flight then plans it): the
  // pool is built again from the plans the sources ended with
  PreopenGuard both(0, 2);
  const std::string dst = (dir / "g.mefd").string();
  REQUIRE(merge_sessions({a, b}, dst, o).verification.ok);
  const std::string blob = all_bytes(dst);
  for (const char* needle : {"Jane", "Smithers", "Alpha", "Beta"})
    CHECK(blob.find(needle) == std::string::npos);
}

TEST_CASE("merge_sessions: n_threads = 1 starts no thread") {
  const auto dir = fresh("mef3io_merge_one_thread");
  const std::string a = write_patient(dir, "a.mefd", "Jane", 0, 20'000);
  const std::string b = write_patient(dir, "b.mefd", "Smithers", 10'000'000, 20'000);
  MergeOptions o = anon_ranges_options();
  g_threads_started = 0;
  detail::thread_start_hook.store(&count_thread);
  struct Reset {
    ~Reset() { detail::thread_start_hook.store(nullptr); }
  } reset;
  REQUIRE(merge_sessions({a, b}, (dir / "m1.mefd").string(), o).verification.ok);
  CHECK(g_threads_started == 0);
  o.n_threads = 4;
  REQUIRE(merge_sessions({a, b}, (dir / "m4.mefd").string(), o).verification.ok);
  CHECK(g_threads_started > 0);
}

// ---- file opens (network drives: every open is a round trip) ----------------
namespace {

std::mutex g_open_mu;
std::map<std::string, int> g_opens;   // DirectorySource opens per path
std::map<std::string, int> g_syncs;   // fsyncs per path
void note_open(const std::string& p) {
  std::lock_guard<std::mutex> lk(g_open_mu);
  ++g_opens[p];
}
void note_sync(const std::string& p) {
  std::lock_guard<std::mutex> lk(g_open_mu);
  ++g_syncs[p];
}
struct OpenWatch {
  OpenWatch() {
    g_opens.clear();
    g_syncs.clear();
    detail::source_open_observer = &note_open;
    detail::fsync_observer = &note_sync;
  }
  ~OpenWatch() {
    detail::source_open_observer = nullptr;
    detail::fsync_observer = nullptr;
  }
};

// Every .tmet/.tidx/.tdat of `under` (a source, or the destination while it
// is verified) is opened at most `most` times in the whole operation.
void check_opens(const std::string& under, int most, int min_files) {
  int files = 0;
  for (const auto& [path, n] : g_opens) {
    if (path.find(under) != 0) continue;
    const auto ext = fsys::path(path).extension().string();
    if (ext != ".tmet" && ext != ".tidx" && ext != ".tdat") continue;
    ++files;
    INFO(path << " opened " << n << " times");
    CHECK(n <= most);
  }
  CHECK(files >= min_files);
}

}  // namespace

TEST_CASE("merge_sessions: each source file is opened once, whatever the number of passes") {
  const auto dir = fresh("mef3io_merge_open_counts");
  const std::string a = write_patient(dir, "a.mefd", "Jane", 0, 20'000);
  const std::string b = write_patient(dir, "b.mefd", "Smithers", 10'000'000, 20'000);
  for (int threads : {1, 4}) {
    MergeOptions o = anon_ranges_options();
    o.n_threads = threads;
    const std::string dst = (dir / ("m" + std::to_string(threads) + ".mefd")).string();
    OpenWatch watch;
    REQUIRE(merge_sessions({a, b}, dst, o).verification.ok);
    check_opens(a, 1, 6);
    check_opens(b, 1, 6);
    // The result is read through handles opened after it was written (the
    // verifier reads what is on disk), once per file.
    check_opens((dir / ("m" + std::to_string(threads) + ".mefd.partial")).string(), 1, 12);
    // Every output file is flushed exactly once, through its own handle.
    int flushed = 0;
    for (const auto& [path, n] : g_syncs)
      if (path.find(".partial") != std::string::npos) {
        ++flushed;
        CHECK(n == 1);
      }
    CHECK(flushed >= 12);
  }
}

TEST_CASE("anonymize_session and transform_session: each source file is opened once") {
  const auto dir = fresh("mef3io_copy_open_counts");
  const std::string a = write_patient(dir, "a.mefd", "Jane", 0, 20'000);
  {
    OpenWatch watch;
    AnonymizeOptions ao;
    ao.shift_us = 3'000'000'000'000'000;
    REQUIRE(anonymize_session(a, (dir / "an.mefd").string(), ao).verification.ok);
    check_opens(a, 1, 6);
    check_opens((dir / "an.mefd.partial").string(), 1, 6);
  }
  {
    OpenWatch watch;
    REQUIRE(transform_session(a, (dir / "tr.mefd").string(), TransformOptions{}).verification.ok);
    check_opens(a, 1, 6);
    check_opens((dir / "tr.mefd.partial").string(), 1, 6);
  }
}

// ---- the handle cache belongs to one copy (review round 1) ------------------
namespace {

std::string write_bytes(const fsys::path& dir, const std::string& rel, std::size_t n) {
  fsys::create_directories((dir / rel).parent_path());
  std::vector<ui1> bytes(n);
  for (std::size_t i = 0; i < n; ++i) bytes[i] = static_cast<ui1>(i * 7 + 3);
  std::ofstream(dir / rel, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), n);
  return (dir / rel).string();
}

std::mutex g_fault_mu;
std::string g_fault_path;
int g_fault_errno = 0;
int g_fault_left = 0;  // -1 = forever
int fault(const std::string& p) {
  std::lock_guard<std::mutex> lk(g_fault_mu);
  if (p != g_fault_path || g_fault_left == 0) return 0;
  if (g_fault_left > 0) --g_fault_left;
  return g_fault_errno;
}
struct FaultGuard {
  FaultGuard(const std::string& p, int e, int times) {
    std::lock_guard<std::mutex> lk(g_fault_mu);
    g_fault_path = p;
    g_fault_errno = e;
    g_fault_left = times;
    detail::read_fault_hook = &fault;
  }
  ~FaultGuard() { detail::read_fault_hook = nullptr; }
};

}  // namespace

TEST_CASE("merge_sessions: other readers see fresh state while a copy caches its sources") {
  const auto dir = fresh("mef3io_merge_isolation");
  const std::string a = write_patient(dir, "a.mefd", "Jane", 0, 20'000);
  const std::string b = write_patient(dir, "b.mefd", "Smithers", 10'000'000, 20'000);
  MergeOptions o = anon_ranges_options();
  bool checked = false;
  Control ctl;
  ctl.progress = [&](std::uint64_t, std::uint64_t) {
    if (checked) return;
    checked = true;  // (the copy has listed and sized its sources by now)
    std::thread other([&] {
      const fsys::path note = fsys::path(a) / "note.txt";
      std::ofstream(note) << "hello";
      DirectorySource plain(a);  // an unrelated source on the same files
      bool listed = false;
      for (const auto& e : plain.list_dir("")) listed |= e.name == "note.txt";
      CHECK(listed);
      CHECK(plain.exists("note.txt"));
      CHECK(plain.file_size("note.txt") == 5);
      CHECK(open_session_source(a)->exists("note.txt"));
      CHECK_NOTHROW(Session(a));
      fsys::remove(note);
      CHECK_FALSE(plain.exists("note.txt"));
    });
    other.join();
  };
  const std::string dst = (dir / "m.mefd").string();
  OpenWatch watch;
  REQUIRE(merge_sessions({a, b}, dst, o, ctl).verification.ok);
  CHECK(checked);
  check_opens(a, 2, 6);  // each file once for the copy, once more for the other reader's Session
  check_opens(b, 1, 6);  // and the copy itself still opens each file once
}

TEST_CASE("a stale cached handle is evicted and the read retried once") {
#ifndef _WIN32
  const auto dir = fresh("mef3io_stale_handle") / "s.mefd";
  const std::string file = write_bytes(dir, "f.bin", 4096);
  const detail::ReadHandleScope scope;
  const auto src = scope.open(dir.string());
  const auto want = DirectorySource(dir.string()).read_range("f.bin", 100, 200);
  OpenWatch watch;
  REQUIRE(src->read_range("f.bin", 100, 200) == want);  // warm: opened once
  CHECK(g_opens[file] == 1);
  for (int code : {ESTALE, EIO, EBADF}) {
    g_opens.clear();
    FaultGuard guard(file, code, 1);
    REQUIRE(src->read_range("f.bin", 100, 200) == want);
    CHECK(g_opens[file] == 1);  // exactly the retry's fresh open
    g_opens.clear();
    REQUIRE(src->read_range("f.bin", 100, 200) == want);
    CHECK(g_opens.empty());  // and that handle is cached again
  }
  {
    FaultGuard guard(file, ESTALE, -1);  // fails again after the reopen
    try {
      src->read_range("f.bin", 0, 10);
      FAIL("expected IoError");
    } catch (const IoError& e) {
      CHECK(std::string(e.what()).find(file) != std::string::npos);
    }
  }
  {
    FaultGuard guard(file, ENOSPC, -1);  // not a stale-handle error: no retry
    g_opens.clear();
    try {
      src->read_range("f.bin", 0, 10);
      FAIL("expected IoError");
    } catch (const IoError& e) {
      CHECK(std::string(e.what()).find(file) != std::string::npos);
    }
    CHECK(g_opens[file] <= 1);
  }
#endif
}

TEST_CASE("threads asking for the same file open it exactly once") {
  const auto dir = fresh("mef3io_same_file_threads") / "s.mefd";
  const std::string file = write_bytes(dir, "f.bin", 1 << 16);
  for (int round = 0; round < 20; ++round) {
    const detail::ReadHandleScope scope;
    const auto src = scope.open(dir.string());
    OpenWatch watch;
    std::atomic<int> ready{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < 8; ++t)
      ts.emplace_back([&] {
        ++ready;
        while (ready < 8) std::this_thread::yield();
        CHECK(src->read_range("f.bin", 10, 100).size() == 100);
      });
    for (auto& t : ts) t.join();
    CHECK(g_opens[file] == 1);
  }
}

TEST_CASE("read_head returns what a shrunk file still has") {
  const auto dir = fresh("mef3io_shrunk_head") / "s.mefd";
  const std::string file = write_bytes(dir, "f.bin", 100);
  const detail::ReadHandleScope scope;
  const auto src = scope.open(dir.string());
  REQUIRE(src->exists("f.bin"));  // remembers size 100
  fsys::resize_file(file, 40);
  CHECK(src->read_head("f.bin", 100).size() == 40);
  CHECK(DirectorySource(dir.string()).read_head("f.bin", 100).size() == 40);
}

TEST_CASE("non-ASCII names work through the output file and the cached source") {
  const auto root = fresh("mef3io_unicode_names");
  const auto dir = root / detail::utf8_path("テスト.mefd");
  fsys::create_directories(dir);
  const auto file = dir / detail::utf8_path("ファイル.bin");
  const std::vector<ui1> bytes{1, 2, 3, 4, 5};
  {
    detail::OutputFile f(file, 0);
    f.write(bytes);
    f.finish(true);
  }
  const std::string text = detail::path_utf8(dir);
  const detail::ReadHandleScope scope;
  for (const auto& src : {scope.open(text), open_session_source(text)}) {
    CHECK(src->exists("ファイル.bin"));
    CHECK(src->file_size("ファイル.bin") == 5);
    CHECK(src->read_all("ファイル.bin") == bytes);
    CHECK(src->read_range("ファイル.bin", 1, 3) == std::vector<ui1>{2, 3, 4});
    bool listed = false;
    for (const auto& e : src->list_dir("")) listed |= e.name == "ファイル.bin";
    CHECK(listed);
  }
}

// ---- a windowed check/run on many segments (network drives: requests) -------
namespace {
std::mutex g_rd_mu;
std::map<std::string, int> g_reads;
void note_read(const std::string& p) {
  std::lock_guard<std::mutex> lk(g_rd_mu);
  ++g_reads[p];
}

// CH channels x SEG segments of 2 s at 1 kHz, 10 s apart.
std::string write_many_segments(const fsys::path& dir, int ch, int seg) {
  const std::string path = (dir / "many.mefd").string();
  SessionWriter w(path, true);
  SessionMetadata md;
  md.subject_name_1 = "Jane";
  w.set_metadata(md);
  for (int s = 0; s < seg; ++s)
    for (int c = 0; c < ch; ++c) {
      std::vector<si4> x(2000);
      for (std::size_t i = 0; i < x.size(); ++i) x[i] = static_cast<si4>((i * 7 + c) % 1000);
      w.write_int32("c" + std::to_string(c), x, 1.0, START + s * 10'000'000LL, 1000.0, {}, s > 0);
    }
  return path;
}

struct ReadWatch {
  ReadWatch() {
    g_reads.clear();
    detail::source_read_observer = &note_read;
  }
  ~ReadWatch() { detail::source_read_observer = nullptr; }
};
}  // namespace

TEST_CASE("a windowed check and run on many segments: every source file opened once, few reads") {
  const auto dir = fresh("mef3io_window_requests");
  constexpr int CH = 4, SEG = 12;
  const std::string src = write_many_segments(dir, CH, SEG);
  TransformOptions o;
  o.time_range = std::make_pair(START + 8 * 10'000'000LL + 500'000, START + 8 * 10'000'000LL + 1'000'000);
  const int files = CH * SEG;
  for (const bool run : {false, true}) {
    const std::string dst = (dir / (run ? "run.mefd" : "chk.mefd")).string();
    OpenWatch watch;
    ReadWatch rw;
    if (run) {
      REQUIRE(transform_session(src, dst, o).verification.ok);
    } else {
      REQUIRE(check_transform(src, dst, o).ok);
    }
    // No source path is opened twice (the result's own files are: written, then verified).
    int opened = 0;
    for (const auto& [path, n] : g_opens) {
      if (path.find(src) != 0) continue;
      ++opened;
      INFO(path << " opened " << n << " times");
      CHECK(n == 1);
    }
    CHECK(opened > 0);
    // Reads of the source: the old per-segment binary searches cost ~15 reads
    // per segment index; the bound is a couple per segment file (measured: check ~1, run ~3).
    int reads = 0;
    for (const auto& [path, n] : g_reads)
      if (path.find(src) == 0) reads += n;
    INFO((run ? "run" : "check") << ": " << reads << " source reads");
    CHECK(reads <= (run ? 4 : 2) * files);
  }
}

TEST_CASE("a windowed check still finds what the full check finds") {
  const auto dir = fresh("mef3io_window_refusals");
  const std::string src = write_many_segments(dir, 3, 6);
  const si8 mid = START + 3 * 10'000'000LL;
  {  // a window in a gap holds no samples
    TransformOptions o;
    o.time_range = std::make_pair(mid + 4'000'000, mid + 6'000'000);
    const CopyCheck c = check_transform(src, (dir / "g.mefd").string(), o);
    CHECK_FALSE(c.ok);
    std::vector<std::string> codes;
    for (const auto& r : c.refusals) codes.push_back(r.code);
    CHECK(std::count(codes.begin(), codes.end(), "time-range-empty") == 1);
  }
  {  // a window beyond the margin of every segment
    TransformOptions o;
    o.time_range = std::make_pair(START + 400LL * 86400 * 1'000'000, START + 401LL * 86400 * 1'000'000);
    CHECK_FALSE(check_transform(src, (dir / "far.mefd").string(), o).ok);
  }
  {  // a metadata refusal is still reported next to a window
    TransformOptions o;
    o.time_range = std::make_pair(mid, mid + 1'000'000);
    o.metadata.subject_id = std::string(200, 'x');
    const CopyCheck c = check_transform(src, (dir / "m.mefd").string(), o);
    CHECK_FALSE(c.ok);
    bool found = false;
    for (const auto& r : c.refusals) found = found || r.code == "metadata-too-long";
    CHECK(found);
  }
  {  // a good window passes
    TransformOptions o;
    o.time_range = std::make_pair(mid, mid + 1'000'000);
    CHECK(check_transform(src, (dir / "ok.mefd").string(), o).ok);
  }
}
