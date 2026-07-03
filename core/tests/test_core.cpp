// mef3io — standalone C++ unit tests (no Python). Covers the codec, crypto,
// header round-trips, and RED encode/decode independent of the golden fixtures.
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <filesystem>
#include <random>
#include <vector>

#include "mef3io/crc.hpp"
#include "mef3io/crypto.hpp"
#include "mef3io/headers.hpp"
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
