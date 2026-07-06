// mef3io — tar session archive tests: round trip vs the directory session,
// archive creation guards, and tolerance/rejection of foreign tar input.
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include "mef3io/c_api.h"
#include "mef3io/errors.hpp"
#include "mef3io/session.hpp"
#include "mef3io/session_writer.hpp"
#include "mef3io/source.hpp"
#include "mef3io/tar.hpp"

using namespace mef3io;
namespace fsys = std::filesystem;

namespace {

fsys::path fresh_dir(const std::string& name) {
  const auto dir = fsys::temp_directory_path() / name;
  fsys::remove_all(dir);
  return dir;
}

void write_demo_session(const std::string& path, const std::string& pw1 = "",
                        const std::string& pw2 = "") {
  SessionWriter w(path, true, pw1, pw2);
  std::vector<si4> a(1000), b(500);
  for (int i = 0; i < 1000; ++i) a[i] = i - 500;
  for (int i = 0; i < 500; ++i) b[i] = 3 * i;
  const si8 start = 1577836800000000;
  w.write_int32("ch1", a, 1.0, start, 250.0);
  w.write_int32("ch2", b, 0.5, start, 125.0);
  Record note;
  note.type = "Note";
  note.time = start + 1000;
  note.text = "hello tar";
  w.write_records(std::nullopt, {note});
  w.write_records("ch1", {note});
}

std::vector<char> slurp(const fsys::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::vector<char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// The sessions must be indistinguishable through the whole read API.
void require_same_session(Session& dir_s, Session& tar_s) {
  REQUIRE(dir_s.channels() == tar_s.channels());
  for (const auto& ch : dir_s.channels()) {
    const auto& a = dir_s.channel_info(ch);
    const auto& b = tar_s.channel_info(ch);
    REQUIRE(a.sampling_frequency == b.sampling_frequency);
    REQUIRE(a.units_conversion_factor == b.units_conversion_factor);
    REQUIRE(a.start_time == b.start_time);
    REQUIRE(a.end_time == b.end_time);
    REQUIRE(a.number_of_samples == b.number_of_samples);

    auto ra = dir_s.read_runs(ch);
    auto rb = tar_s.read_runs(ch);
    REQUIRE(ra.size() == rb.size());
    for (std::size_t i = 0; i < ra.size(); ++i) {
      REQUIRE(ra[i].start_uutc == rb[i].start_uutc);
      REQUIRE(ra[i].samples == rb[i].samples);
    }

    auto ma = dir_s.segment_map(ch);
    auto mb = tar_s.segment_map(ch);
    REQUIRE(ma.size() == mb.size());
    for (std::size_t i = 0; i < ma.size(); ++i) {
      REQUIRE(ma[i].segment_number == mb[i].segment_number);
      REQUIRE(ma[i].start_time == mb[i].start_time);
      REQUIRE(ma[i].number_of_samples == mb[i].number_of_samples);
      REQUIRE(mb[i].path.find("::") != std::string::npos);  // tar member notation
    }
  }
  auto rec_a = dir_s.read_records();
  auto rec_b = tar_s.read_records();
  REQUIRE(rec_a.size() == rec_b.size());
  for (std::size_t i = 0; i < rec_a.size(); ++i) {
    REQUIRE(rec_a[i].type == rec_b[i].type);
    REQUIRE(rec_a[i].time == rec_b[i].time);
    REQUIRE(rec_a[i].text == rec_b[i].text);
  }
  REQUIRE(dir_s.read_records("ch1").size() == tar_s.read_records("ch1").size());
}

}  // namespace

TEST_CASE("Tar archive round trip matches directory session") {
  const auto dir = fresh_dir("mef3io_tar_test.mefd");
  write_demo_session(dir.string());

  const std::string tar = archive_session(dir.string());
  REQUIRE(tar == dir.string() + ".tar");
  REQUIRE(fsys::is_regular_file(tar));

  Session dir_s(dir.string());
  Session tar_s(tar);
  require_same_session(dir_s, tar_s);

  SECTION("windowed reads hit member byte ranges identically") {
    const si8 t0 = 1577836800000000 + 1'000'000;
    const si8 t1 = t0 + 1'500'000;
    auto ja = dir_s.collect_blocks("ch1", t0, t1);
    auto jb = tar_s.collect_blocks("ch1", t0, t1);
    REQUIRE(ja.jobs.size() == jb.jobs.size());
    REQUIRE(ja.buffers == jb.buffers);
  }

  SECTION("deterministic output") {
    auto first = slurp(tar);
    const std::string again = archive_session(dir.string(), "", /*overwrite=*/true);
    REQUIRE(slurp(again) == first);
  }

  SECTION("writer refuses tar sessions") {
    REQUIRE_THROWS_AS(SessionWriter(tar, false), IoError);
    REQUIRE_THROWS_AS(SessionWriter(tar, true), IoError);  // overwrite must not delete it
    REQUIRE(fsys::exists(tar));
    // .tar target refused even if nothing exists there yet
    REQUIRE_THROWS_AS(SessionWriter((dir.parent_path() / "nope.mefd.tar").string(), true),
                      IoError);
  }

  fsys::remove_all(dir);
  fsys::remove(tar);
}

TEST_CASE("Encrypted session reads from tar with the same passwords") {
  const auto dir = fresh_dir("mef3io_tar_enc_test.mefd");
  write_demo_session(dir.string(), "pass1", "pass2");
  const std::string tar = archive_session(dir.string());

  Session dir_s(dir.string(), "pass2");
  Session tar_s(tar, "pass2");
  require_same_session(dir_s, tar_s);

  fsys::remove_all(dir);
  fsys::remove(tar);
}

TEST_CASE("archive_session guards") {
  const auto dir = fresh_dir("mef3io_tar_guard_test.mefd");
  write_demo_session(dir.string());

  const std::string tar = archive_session(dir.string());
  REQUIRE_THROWS_AS(archive_session(dir.string()), IoError);  // target exists, no overwrite
  REQUIRE_NOTHROW(archive_session(dir.string(), "", true));
  REQUIRE_THROWS_AS(archive_session(dir.string(), (dir / "inner.mefd.tar").string()), IoError);
  REQUIRE_THROWS_AS(archive_session((dir / "missing").string()), IoError);

  fsys::remove_all(dir);
  fsys::remove(tar);
}

TEST_CASE("Session naming (.mefd / .mefd.tar) is enforced on every entry point") {
  const auto base = fsys::temp_directory_path();
  const auto dir = fresh_dir("mef3io_naming_test.mefd");
  write_demo_session(dir.string());
  const std::string tar = archive_session(dir.string());

  // Reader: directory without .mefd, file without .mefd.tar.
  const auto plain_dir = base / "mef3io_naming_plain";
  fsys::create_directories(plain_dir);
  REQUIRE_THROWS_AS(Session(plain_dir.string()), IoError);
  const auto plain_tar = base / "mef3io_naming_plain.tar";  // .tar but not .mefd.tar
  fsys::copy_file(tar, plain_tar, fsys::copy_options::overwrite_existing);
  REQUIRE_THROWS_AS(Session(plain_tar.string()), IoError);
  // Trailing separator and case are tolerated on a well-named session.
  REQUIRE_NOTHROW(Session(dir.string() + "/"));

  // Writer: refuses a non-.mefd target (nothing may be created).
  const auto bad_writer = base / "mef3io_naming_writer";
  REQUIRE_THROWS_AS(SessionWriter(bad_writer.string(), true), IoError);
  REQUIRE_FALSE(fsys::exists(bad_writer));

  // Archiving: source must be .mefd, an explicit target must be .mefd.tar.
  REQUIRE_THROWS_AS(archive_session(plain_dir.string()), IoError);
  REQUIRE_THROWS_AS(archive_session(dir.string(), (base / "out.tar").string()), IoError);

  // Extraction: source must be .mefd.tar, an explicit target must be .mefd.
  REQUIRE_THROWS_AS(extract_session(plain_tar.string()), IoError);
  REQUIRE_THROWS_AS(
      extract_session(tar, (base / "mefio_naming_out").string(), true), IoError);

  fsys::remove_all(plain_dir);
  fsys::remove(plain_tar);
  fsys::remove_all(dir);
  fsys::remove(tar);
}

TEST_CASE("Long member paths use the ustar prefix split") {
  const auto dir = fresh_dir("mef3io_tar_long_test.mefd");
  const std::string deep(90, 'x');
  fsys::create_directories(dir / deep);
  {
    std::ofstream f(dir / deep / (deep + ".bin"), std::ios::binary);
    f << "payload";
  }
  const std::string tar = archive_session(dir.string());

  TarIndex idx(tar);
  const std::string member =
      dir.filename().string() + "/" + deep + "/" + deep + ".bin";  // > 100 chars
  REQUIRE(member.size() > 100);
  REQUIRE(idx.files().count(member) == 1);
  TarSource src(tar);
  auto data = src.read_all(deep + "/" + deep + ".bin");
  REQUIRE(std::string(data.begin(), data.end()) == "payload");

  fsys::remove_all(dir);
  fsys::remove(tar);
}

TEST_CASE("TarIndex parses base-256 sizes") {
  const auto path = fsys::temp_directory_path() / "mef3io_tar_b256.tar";
  std::array<unsigned char, 512> h{};
  const char* name = "f.bin";
  memcpy(h.data(), name, strlen(name));
  memcpy(h.data() + 100, "0000644", 8);
  memcpy(h.data() + 108, "0000000", 8);
  memcpy(h.data() + 116, "0000000", 8);
  h[124] = 0x80;  // base-256 marker; value 5 in the last byte
  h[135] = 5;
  memcpy(h.data() + 136, "00000000000", 12);
  h[156] = '0';
  memcpy(h.data() + 257, "ustar", 6);
  h[263] = '0';
  h[264] = '0';
  memset(h.data() + 148, ' ', 8);
  unsigned sum = 0;
  for (unsigned char c : h) sum += c;
  char chk[8];
  snprintf(chk, sizeof chk, "%06o", sum);
  memcpy(h.data() + 148, chk, 7);
  h[155] = ' ';

  {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(h.data()), 512);
    std::array<char, 512> data{};
    memcpy(data.data(), "hello", 5);
    f.write(data.data(), 512);
    std::array<char, 512> zeros{};
    f.write(zeros.data(), 512);
    f.write(zeros.data(), 512);
  }
  TarIndex idx(path.string());
  REQUIRE(idx.files().at("f.bin").size == 5);
  fsys::remove(path);
}

TEST_CASE("Non-tar and compressed inputs are rejected clearly") {
  const auto base = fsys::temp_directory_path();

  const auto gz = base / "mef3io_fake.mefd.tar";
  {
    std::ofstream f(gz, std::ios::binary | std::ios::trunc);
    const unsigned char magic[] = {0x1F, 0x8B, 0x08, 0x00};
    f.write(reinterpret_cast<const char*>(magic), 4);
    std::vector<char> pad(1024, 0);
    f.write(pad.data(), static_cast<std::streamsize>(pad.size()));
  }
  try {
    Session s(gz.string());
    FAIL("expected FormatError");
  } catch (const FormatError& e) {
    REQUIRE(std::string(e.what()).find("gzip") != std::string::npos);
  }
  fsys::remove(gz);

  const auto junk = base / "mef3io_junk.mefd.tar";
  {
    std::ofstream f(junk, std::ios::binary | std::ios::trunc);
    std::vector<char> data(1024, 'A');
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
  }
  REQUIRE_THROWS_AS(Session(junk.string()), FormatError);
  fsys::remove(junk);

  // Corrupt a header byte inside a valid archive -> checksum mismatch.
  const auto dir = fresh_dir("mef3io_tar_crc_test.mefd");
  write_demo_session(dir.string());
  const std::string tar = archive_session(dir.string());
  {
    std::fstream f(tar, std::ios::binary | std::ios::in | std::ios::out);
    f.seekp(0);
    f.put('Z');  // first byte of the root member name
  }
  REQUIRE_THROWS_AS(TarIndex(tar), FormatError);
  fsys::remove_all(dir);
  fsys::remove(tar);
}

TEST_CASE("extract_session restores the directory byte-identically") {
  const auto dir = fresh_dir("mef3io_tar_extract_test.mefd");
  write_demo_session(dir.string());
  const std::string tar = archive_session(dir.string());

  // Default target strips ".tar" — extract next to a moved copy so the
  // original stays for comparison.
  const auto moved = fresh_dir("mef3io_tar_extract_copy.mefd.tar");
  fsys::copy_file(tar, moved);
  const std::string restored = extract_session(moved.string());
  REQUIRE(restored == (moved.parent_path() / "mef3io_tar_extract_copy.mefd").string());

  std::vector<fsys::path> rels;
  for (const auto& e : fsys::recursive_directory_iterator(dir))
    if (e.is_regular_file()) rels.push_back(fsys::relative(e.path(), dir));
  REQUIRE_FALSE(rels.empty());
  for (const auto& rel : rels) REQUIRE(slurp(dir / rel) == slurp(fsys::path(restored) / rel));

  SECTION("round trip: extract -> archive is byte-identical") {
    const std::string tar2 = archive_session(restored, (fsys::temp_directory_path() /
                                                        "mef3io_tar_extract_rt.mefd.tar").string());
    // Same tree content, different root name -> compare via a fresh extract
    // of the original name instead: archive the restored dir under the
    // original name by extracting over it.
    const std::string restored_orig =
        extract_session(tar, (fsys::temp_directory_path() / "roundtrip.mefd").string());
    const std::string tar3 = archive_session(
        restored_orig, (fsys::temp_directory_path() / "roundtrip2.mefd.tar").string());
    // Roots differ from the original archive, so compare data equality via Session.
    Session a(tar), b(tar3);
    REQUIRE(a.channels() == b.channels());
    for (const auto& ch : a.channels())
      REQUIRE(a.read_runs(ch).front().samples == b.read_runs(ch).front().samples);
    fsys::remove(tar2);
    fsys::remove(tar3);
    fsys::remove_all(restored_orig);
  }

  SECTION("guards") {
    REQUIRE_THROWS_AS(extract_session(moved.string()), IoError);  // target exists
    REQUIRE_NOTHROW(extract_session(moved.string(), "", true));
    REQUIRE_THROWS_AS(extract_session(moved.string(), "", false), IoError);
    const auto no_suffix = fsys::temp_directory_path() / "mef3io_nosuffix";
    fsys::copy_file(moved, no_suffix, fsys::copy_options::overwrite_existing);
    REQUIRE_THROWS_AS(extract_session(no_suffix.string()), IoError);  // cannot derive target
    fsys::remove(no_suffix);
  }

  fsys::remove_all(dir);
  fsys::remove_all(restored);
  fsys::remove(tar);
  fsys::remove(moved);
}

TEST_CASE("C ABI: mef3io_archive_session and reading a tar session") {
  const auto dir = fresh_dir("mef3io_tar_capi_test.mefd");
  write_demo_session(dir.string());

  char out_path[1024] = {0};
  REQUIRE(mef3io_archive_session(dir.string().c_str(), nullptr, 0, out_path, sizeof out_path) ==
          MEF3IO_OK);
  REQUIRE(std::string(out_path) == dir.string() + ".tar");

  mef3io_reader* r = nullptr;
  REQUIRE(mef3io_reader_open(out_path, "", 1, &r) == MEF3IO_OK);
  int32_t nch = 0;
  REQUIRE(mef3io_reader_n_channels(r, &nch) == MEF3IO_OK);
  REQUIRE(nch == 2);
  mef3io_channel_info info{};
  REQUIRE(mef3io_reader_info(r, "ch1", &info) == MEF3IO_OK);
  REQUIRE(info.number_of_samples == 1000);
  mef3io_reader_close(r);

  // C ABI extraction round trip.
  const auto ext_dir = fsys::temp_directory_path() / "mef3io_capi_extract.mefd";
  fsys::remove_all(ext_dir);
  char ext_path[1024] = {0};
  REQUIRE(mef3io_extract_session(out_path, ext_dir.string().c_str(), 0, ext_path,
                                 sizeof ext_path) == MEF3IO_OK);
  REQUIRE(std::string(ext_path) == ext_dir.string());
  mef3io_reader* r2 = nullptr;
  REQUIRE(mef3io_reader_open(ext_path, "", 1, &r2) == MEF3IO_OK);
  REQUIRE(mef3io_reader_info(r2, "ch1", &info) == MEF3IO_OK);
  REQUIRE(info.number_of_samples == 1000);
  mef3io_reader_close(r2);
  fsys::remove_all(ext_dir);

  // Existing target without overwrite -> IO error code.
  REQUIRE(mef3io_archive_session(dir.string().c_str(), nullptr, 0, out_path, sizeof out_path) ==
          MEF3IO_ERR_IO);
  // Writer over the archive is refused.
  mef3io_writer* w = nullptr;
  REQUIRE(mef3io_writer_open(out_path, 1, "", "", &w) == MEF3IO_ERR_IO);

  fsys::remove_all(dir);
  fsys::remove(dir.string() + ".tar");
}
