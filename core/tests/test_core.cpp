// mef3io — standalone C++ unit tests (no Python). Covers the codec, crypto,
// header round-trips, and RED encode/decode independent of the golden fixtures.
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <random>
#include <vector>

#include "mef3io/crc.hpp"
#include "mef3io/crypto.hpp"
#include "mef3io/headers.hpp"
#include "mef3io/c_api.h"
#include "mef3io/red.hpp"
#include "mef3io/session.hpp"
#include "mef3io/session_writer.hpp"

using namespace mef3io;

static std::vector<ui1> bytes(const std::string& s) {
  return std::vector<ui1>(s.begin(), s.end());
}

TEST_CASE("CRC-32 Koopman known value") {
  auto b = bytes("hello world");
  // Cross-checked against the Python oracle (reimplementation.py).
  REQUIRE(crc::calculate(b) == 0x20c8c2c3u);
}

TEST_CASE("SHA-256 NIST 'abc'") {
  auto h = crypto::sha256(bytes("abc"));
  const std::array<ui1, 4> prefix = {0xba, 0x78, 0x16, 0xbf};
  REQUIRE(std::equal(prefix.begin(), prefix.end(), h.begin()));
}

TEST_CASE("AES-128 FIPS-197 vector round trip") {
  std::vector<ui1> key = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                          0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
  std::vector<ui1> pt = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                         0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
  std::vector<ui1> ct = {0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30,
                         0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a};
  REQUIRE(crypto::aes128_ecb_encrypt(pt, key) == ct);
  REQUIRE(crypto::aes128_ecb_decrypt(ct, key) == pt);
}

TEST_CASE("Two-level password derivation") {
  auto vf = crypto::make_validation_fields("pass1", "pass2");
  auto l2 = crypto::validate_password("pass2", vf.level1, vf.level2);
  REQUIRE(l2.access_level == fmt::LEVEL_2_ACCESS);
  REQUIRE(l2.level1_key.has_value());
  REQUIRE(l2.level2_key.has_value());
  auto l1 = crypto::validate_password("pass1", vf.level1, vf.level2);
  REQUIRE(l1.access_level == fmt::LEVEL_1_ACCESS);
  REQUIRE(l1.level1_key.has_value());
  REQUIRE_FALSE(l1.level2_key.has_value());
  auto bad = crypto::validate_password("nope", vf.level1, vf.level2);
  REQUIRE(bad.access_level == fmt::LEVEL_0_ACCESS);
}

TEST_CASE("Universal header serialize/parse round trip") {
  fmt::UniversalHeader uh;
  uh.file_type_string = "tmet";
  uh.start_time = -1577836800000000LL;
  uh.number_of_entries = 1;
  uh.channel_name = "ch1";
  std::vector<ui1> buf(fmt::UNIVERSAL_HEADER_BYTES);
  uh.serialize(buf);
  auto p = fmt::UniversalHeader::parse(buf);
  REQUIRE(p.file_type_string == "tmet");
  REQUIRE(p.start_time == -1577836800000000LL);
  REQUIRE(p.number_of_entries == 1);
  REQUIRE(p.channel_name == "ch1");
}

TEST_CASE("RED encode -> decode reproduces samples") {
  std::mt19937 rng(42);
  std::uniform_int_distribution<si4> dist(-1000, 1000);
  for (int n : {1, 5, 100, 2560}) {
    std::vector<si4> data(n);
    si4 acc = 0;
    for (auto& x : data) {
      acc += dist(rng) % 50;  // smooth-ish
      x = acc;
    }
    si4 mn = 0, mx = 0;
    auto block = red::encode_block(data, 12345, false, mn, mx);
    auto dec = red::decode_block(block);
    REQUIRE(dec.samples == data);
    REQUIRE(mn == *std::min_element(data.begin(), data.end()));
    REQUIRE(mx == *std::max_element(data.begin(), data.end()));
  }
}

TEST_CASE("RED handles int32 extremes") {
  std::vector<si4> data = {std::numeric_limits<si4>::min() + 1, 0,
                           std::numeric_limits<si4>::max() - 1, -5, 5};
  si4 mn = 0, mx = 0;
  auto block = red::encode_block(data, 1, true, mn, mx);
  auto dec = red::decode_block(block);
  REQUIRE(dec.samples == data);
  REQUIRE(dec.discontinuity);
}

TEST_CASE("In-segment append extends the last segment") {
  namespace fsys = std::filesystem;
  const auto dir = fsys::temp_directory_path() / "mef3io_append_test.mefd";
  fsys::remove_all(dir);

  const si8 start = 1577836800000000;
  const sf8 fs = 250.0;
  std::vector<si4> a(1000), b(1000);
  for (int i = 0; i < 1000; ++i) {
    a[i] = i;
    b[i] = 2 * i;
  }
  {
    SessionWriter w(dir.string(), true);
    auto s1 = w.write_int32("ch1", a, 1.0, start, fs);
    REQUIRE(s1.segment == 0);
    const si8 t2 = start + static_cast<si8>(std::llround(1000 / fs * 1e6));
    auto s2 = w.write_int32("ch1", b, 1.0, t2, fs);
    REQUIRE(s2.segment == 0);  // appended, not a new segment
  }
  int n_seg_dirs = 0;
  for (const auto& e : fsys::directory_iterator(dir / "ch1.timd"))
    if (e.is_directory()) ++n_seg_dirs;
  REQUIRE(n_seg_dirs == 1);

  Session ses(dir.string());
  REQUIRE(ses.channel_info("ch1").number_of_samples == 2000);
  auto runs = ses.read_runs("ch1");
  si8 total = 0;
  for (const auto& r : runs) total += static_cast<si8>(r.samples.size());
  REQUIRE(total == 2000);
  REQUIRE(runs.front().samples.front() == 0);
  REQUIRE(runs.back().samples.back() == 2 * 999);

  auto map = ses.segment_map("ch1");
  REQUIRE(map.size() == 1);
  REQUIRE(map[0].segment_number == 0);
  REQUIRE(map[0].start_sample == 0);
  REQUIRE(map[0].number_of_samples == 2000);
  REQUIRE(map[0].number_of_blocks == 2);
  fsys::remove_all(dir);
}

TEST_CASE("C ABI round trip (write, read, records, segments)") {
  namespace fsys = std::filesystem;
  const auto dir = fsys::temp_directory_path() / "mef3io_capi_test.mefd";
  fsys::remove_all(dir);
  const std::string path = dir.string();
  const si8 start = 1577836800000000;
  const sf8 fs = 250.0;

  std::vector<double> data(1000);
  for (int i = 0; i < 1000; ++i) data[i] = std::sin(i / 20.0);
  data[100] = std::nan("");  // one-sample gap

  // --- write ---
  mef3io_writer* w = nullptr;
  REQUIRE(mef3io_writer_open(path.c_str(), 1, "", "", &w) == MEF3IO_OK);
  mef3io_write_summary sum{};
  REQUIRE(mef3io_writer_write_float(w, "ch1", data.data(), 1000, start, fs, 3, 0, &sum) ==
          MEF3IO_OK);
  REQUIRE(sum.samples_written == 999);
  REQUIRE(sum.segment == 0);
  mef3io_records* recs = mef3io_records_create();
  REQUIRE(mef3io_records_add(recs, "Note", start + 1000, "hello", -1) == MEF3IO_OK);
  REQUIRE(mef3io_writer_write_records(w, "ch1", recs) == MEF3IO_OK);
  mef3io_records_free(recs);
  mef3io_writer_close(w);

  // --- read back ---
  mef3io_reader* r = nullptr;
  REQUIRE(mef3io_reader_open(path.c_str(), "", 1, &r) == MEF3IO_OK);
  int32_t nch = 0;
  REQUIRE(mef3io_reader_n_channels(r, &nch) == MEF3IO_OK);
  REQUIRE(nch == 1);
  char name[64];
  REQUIRE(mef3io_reader_channel_name(r, 0, name, sizeof name) == MEF3IO_OK);
  REQUIRE(std::string(name) == "ch1");

  mef3io_channel_info info{};
  REQUIRE(mef3io_reader_info(r, "ch1", &info) == MEF3IO_OK);
  REQUIRE(info.sampling_frequency == fs);
  REQUIRE(info.number_of_samples == 999);
  REQUIRE(info.start_time == start);
  REQUIRE(info.section3_available == 1);

  int64_t n = 0;
  REQUIRE(mef3io_reader_read_size(r, "ch1", MEF3IO_TIME_UNSET, MEF3IO_TIME_UNSET, &n) ==
          MEF3IO_OK);
  REQUIRE(n == 1000);
  std::vector<double> got(static_cast<size_t>(n));
  int64_t n_read = 0;
  REQUIRE(mef3io_reader_read(r, "ch1", MEF3IO_TIME_UNSET, MEF3IO_TIME_UNSET, got.data(), n,
                             &n_read) == MEF3IO_OK);
  REQUIRE(n_read == 1000);
  REQUIRE(std::isnan(got[100]));
  REQUIRE(got[0] == 0.0);
  REQUIRE(std::abs(got[500] - std::round(std::sin(500 / 20.0) * 1000) / 1000) < 1e-9);

  int32_t nseg = 0;
  REQUIRE(mef3io_reader_n_segments(r, "ch1", &nseg) == MEF3IO_OK);
  REQUIRE(nseg == 1);
  mef3io_segment_info seg{};
  REQUIRE(mef3io_reader_segment(r, "ch1", 0, &seg) == MEF3IO_OK);
  REQUIRE(seg.number_of_samples == 999);

  int32_t nrec = 0;
  REQUIRE(mef3io_reader_n_records(r, "ch1", &nrec) == MEF3IO_OK);
  REQUIRE(nrec == 1);
  mef3io_record_info rec{};
  REQUIRE(mef3io_reader_record(r, "ch1", 0, &rec) == MEF3IO_OK);
  REQUIRE(std::string(rec.type) == "Note");
  REQUIRE(std::string(rec.text) == "hello");
  REQUIRE(rec.duration == -1);

  // errors surface as codes + message
  REQUIRE(mef3io_reader_info(r, "nope", &info) == MEF3IO_ERR_ARGUMENT);
  REQUIRE(std::string(mef3io_last_error()).find("nope") != std::string::npos);
  mef3io_reader_close(r);

  REQUIRE(std::string(mef3io_version()).find('.') != std::string::npos);
  fsys::remove_all(dir);
}
