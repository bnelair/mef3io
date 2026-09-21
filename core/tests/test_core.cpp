// mef3io — standalone C++ unit tests (no Python). Covers the codec, crypto,
// header round-trips, and RED encode/decode independent of the golden fixtures.
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <vector>

#include "mef3io/byteio.hpp"
#include "mef3io/crc.hpp"
#include "mef3io/errors.hpp"
#include "mef3io/crypto.hpp"
#include "mef3io/headers.hpp"
#include "mef3io/c_api.h"
#include "mef3io/reader.hpp"
#include "mef3io/red.hpp"
#include "mef3io/session.hpp"
#include "mef3io/session_writer.hpp"
#include "mef3io/validate.hpp"

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

TEST_CASE("write_string keeps fixed-width fields null-terminated and valid UTF-8") {
  std::vector<ui1> buf(16, 0xAA);

  SECTION("an over-long value is truncated to field_len - 1") {
    byteio::write_string(buf, 0, 8, std::string(20, 'x'));
    REQUIRE(byteio::read_string(buf, 0, 8) == "xxxxxxx");
    REQUIRE(buf[7] == 0);  // terminator, so meflib's C string reads stop here
    REQUIRE(buf[8] == 0xAA);  // the next field is untouched
  }

  SECTION("an exact-fit value still reserves the terminator") {
    byteio::write_string(buf, 0, 8, "abcdefgh");
    REQUIRE(byteio::read_string(buf, 0, 8) == "abcdefg");
    REQUIRE(buf[7] == 0);
  }

  SECTION("truncation cuts on a UTF-8 boundary, never mid-character") {
    // 6 ASCII + "µ" (2 bytes): the field holds 7 bytes of content, so the
    // multi-byte character must be dropped whole rather than half-written.
    byteio::write_string(buf, 0, 8, "abcdef\xc2\xb5");
    REQUIRE(byteio::read_string(buf, 0, 8) == "abcdef");
    byteio::write_string(buf, 0, 8, "\xc2\xb5\xc2\xb5\xc2\xb5\xc2\xb5");
    REQUIRE(byteio::read_string(buf, 0, 8) == "\xc2\xb5\xc2\xb5\xc2\xb5");
  }

  SECTION("a value that fits is stored verbatim") {
    byteio::write_string(buf, 0, 8, "\xc2\xb5V");
    REQUIRE(byteio::read_string(buf, 0, 8) == "\xc2\xb5V");
  }

  SECTION("write_fixed_code fills the field without a terminator") {
    // Record type codes ("EDFA", "Note") are exactly field-width by design.
    byteio::write_fixed_code(buf, 0, 4, "EDFA");
    REQUIRE(std::memcmp(buf.data(), "EDFA", 4) == 0);
  }

  SECTION("write_fixed_code rejects any width but the exact one") {
    // Padding or trimming here would emit a valid-looking header carrying a
    // code the caller never wrote, so both directions must throw.
    REQUIRE_THROWS_AS(byteio::write_fixed_code(buf, 0, 4, "Notes"), FormatError);
    REQUIRE_THROWS_AS(byteio::write_fixed_code(buf, 0, 4, "Not"), FormatError);
    REQUIRE_THROWS_AS(byteio::write_fixed_code(buf, 0, 4, ""), FormatError);
  }
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

TEST_CASE("Section-2 sizing declarations match the blocks on disk") {
  namespace fsys = std::filesystem;
  const auto dir = fsys::temp_directory_path() / "mef3io_sizing_test.mefd";
  fsys::remove_all(dir);

  const si8 start = 1577836800000000;
  const sf8 fs = 250.0;
  std::vector<si4> a(3000);
  std::mt19937 rng(7);
  std::uniform_int_distribution<si4> dist(-30000, 30000);
  for (auto& v : a) v = dist(rng);
  {
    SessionWriter w(dir.string(), true);
    w.write_int32("ch1", a, 1.0, start, fs);
    // A gap, so the segment holds two contiguous runs rather than one.
    const si8 t2 = start + static_cast<si8>(std::llround(3000 / fs * 1e6)) + 5000000;
    w.write_int32("ch1", a, 1.0, t2, fs);
  }

  const auto seg = dir / "ch1.timd" / "ch1-000000.segd";
  auto read_file = [](const fsys::path& p) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    REQUIRE(f);
    std::vector<ui1> buf(static_cast<std::size_t>(f.tellg()));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
    return buf;
  };

  auto md = load_time_series_metadata(read_file(seg / "ch1-000000.tmet"), "");
  const auto& s2 = md.section2;

  // Recompute every declared maximum from the index and the RED block headers.
  auto tidx = read_file(seg / "ch1-000000.tidx");
  auto tdat = read_file(seg / "ch1-000000.tdat");
  const std::size_t n_blocks =
      (tidx.size() - fmt::UNIVERSAL_HEADER_BYTES) / fmt::TIME_SERIES_INDEX_BYTES;
  REQUIRE(n_blocks > 1);

  ui4 max_difference_bytes = 0, max_block_samples = 0;
  si8 max_block_bytes = 0;
  si8 run_blocks = 0, run_bytes = 0, run_samples = 0;
  si8 max_run_blocks = 0, max_run_bytes = 0, max_run_samples = 0;
  for (std::size_t i = 0; i < n_blocks; ++i) {
    auto e = fmt::TimeSeriesIndex::parse(std::span<const ui1>(tidx).subspan(
        fmt::UNIVERSAL_HEADER_BYTES + i * fmt::TIME_SERIES_INDEX_BYTES,
        fmt::TIME_SERIES_INDEX_BYTES));
    auto bh = fmt::RedBlockHeader::parse(std::span<const ui1>(tdat).subspan(
        static_cast<std::size_t>(e.file_offset), fmt::RED_BLOCK_HEADER_BYTES));

    max_difference_bytes = std::max(max_difference_bytes, bh.difference_bytes);
    max_block_samples = std::max(max_block_samples, e.number_of_samples);
    max_block_bytes = std::max<si8>(max_block_bytes, e.block_bytes);

    if (e.red_block_flags & fmt::RedBlockHeader::DISCONTINUITY_MASK)
      run_blocks = run_bytes = run_samples = 0;
    ++run_blocks;
    run_bytes += e.block_bytes;
    run_samples += e.number_of_samples;
    max_run_blocks = std::max(max_run_blocks, run_blocks);
    max_run_bytes = std::max(max_run_bytes, run_bytes);
    max_run_samples = std::max(max_run_samples, run_samples);
  }

  REQUIRE(s2.maximum_difference_bytes == max_difference_bytes);
  REQUIRE(s2.maximum_block_samples == max_block_samples);
  REQUIRE(s2.maximum_block_bytes == max_block_bytes);
  REQUIRE(s2.maximum_contiguous_blocks == max_run_blocks);
  REQUIRE(s2.maximum_contiguous_block_bytes == max_run_bytes);
  REQUIRE(s2.maximum_contiguous_samples == max_run_samples);

  // 0 is not the NO_ENTRY sentinel for any of these, so a reader cannot tell an
  // unset field from a real measurement: none of them may be left at 0.
  REQUIRE(s2.maximum_difference_bytes > 0);
  REQUIRE(s2.maximum_difference_bytes != fmt::UI4_NO_ENTRY);
  REQUIRE(s2.maximum_contiguous_block_bytes > 0);
  // The gap splits the segment, so a contiguous run is shorter than the whole.
  REQUIRE(s2.maximum_contiguous_samples < s2.number_of_samples);
  // meflib sizes its difference buffer at 5 bytes/sample worst case.
  REQUIRE(s2.maximum_difference_bytes <= 5 * s2.maximum_block_samples);

  fsys::remove_all(dir);
}

TEST_CASE("Validator reports declaration defects and repairs only what is selected") {
  namespace fsys = std::filesystem;
  const auto dir = fsys::temp_directory_path() / "mef3io_validate_test.mefd";
  fsys::remove_all(dir);

  const si8 start = 1577836800000000;
  const sf8 fs = 250.0;
  std::vector<si4> a(3000);
  std::mt19937 rng(11);
  std::uniform_int_distribution<si4> dist(-30000, 30000);
  for (auto& v : a) v = dist(rng);
  {
    SessionWriter w(dir.string(), true);
    w.write_int32("ch1", a, 1.0, start, fs);
  }

  // A session mef3io just wrote must satisfy every check.
  REQUIRE(validate_session(dir.string()).ok());
  REQUIRE(validate_session(dir.string()).findings.empty());

  const auto tmet = dir / "ch1.timd" / "ch1-000000.segd" / "ch1-000000.tmet";
  auto patch_s2_ui4 = [&](int section_offset, ui4 value) {
    std::vector<ui1> raw(fmt::METADATA_FILE_BYTES);
    {
      std::ifstream in(tmet, std::ios::binary);
      REQUIRE(in);
      in.read(reinterpret_cast<char*>(raw.data()), fmt::METADATA_FILE_BYTES);
    }
    byteio::write<ui4>(raw, fmt::METADATA_SECTION_2_OFFSET + section_offset, value);
    byteio::write<ui4>(raw, 4,
                       crc::calculate(std::span<const ui1>(raw).subspan(
                           fmt::UNIVERSAL_HEADER_BYTES,
                           fmt::METADATA_FILE_BYTES - fmt::UNIVERSAL_HEADER_BYTES)));
    byteio::write<ui4>(raw, 0,
                       crc::calculate(std::span<const ui1>(raw).subspan(
                           4, fmt::UNIVERSAL_HEADER_BYTES - 4)));
    std::ofstream out(tmet, std::ios::binary | std::ios::trunc);
    REQUIRE(out);
    out.write(reinterpret_cast<const char*>(raw.data()), fmt::METADATA_FILE_BYTES);
  };

  // Simulate a writer that never set the difference-buffer size (mef3io
  // <= 1.1.2) and also left the block interval at 0 (legacy pymef).
  patch_s2_ui4(6388, 0);  // maximum_difference_bytes
  patch_s2_ui4(6392, 0);  // block_interval (low half of an si8; 0 either way)

  auto report = validate_session(dir.string());
  REQUIRE_FALSE(report.ok());
  bool saw_difference_bytes = false;
  for (const auto& f : report.findings)
    if (f.check_id == "sizing.difference-bytes") {
      saw_difference_bytes = true;
      REQUIRE(f.severity == Severity::Error);
      REQUIRE(f.repairable);
      REQUIRE_FALSE(f.repaired);
    }
  REQUIRE(saw_difference_bytes);

  // Repairs are never implicit.
  REQUIRE_THROWS_AS(repair_session(dir.string(), RepairSelection{}), std::invalid_argument);
  RepairSelection not_repairable;
  not_repairable.check_ids = {"crc.metadata"};
  REQUIRE_THROWS_AS(repair_session(dir.string(), not_repairable), std::invalid_argument);

  // Repairing one check leaves the other finding outstanding.
  RepairSelection one;
  one.check_ids = {"sizing.difference-bytes"};
  one.backup = false;
  auto repaired = repair_session(dir.string(), one);
  REQUIRE(repaired.segments_repaired == 1);

  auto after = validate_session(dir.string());
  for (const auto& f : after.findings) REQUIRE(f.check_id != "sizing.difference-bytes");
  bool interval_still_open = false;
  for (const auto& f : after.findings)
    if (f.check_id == "times.block-interval") interval_still_open = true;
  REQUIRE(interval_still_open);

  // Opting in to the rest clears the session, and it still decodes.
  RepairSelection rest;
  rest.check_ids = after.repairable_check_ids();
  rest.backup = false;
  REQUIRE_FALSE(rest.check_ids.empty());
  repair_session(dir.string(), rest);
  REQUIRE(validate_session(dir.string()).ok());

  Session ses(dir.string());
  auto runs = ses.read_runs("ch1");
  si8 total = 0;
  for (const auto& r : runs) total += static_cast<si8>(r.samples.size());
  REQUIRE(total == 3000);
  REQUIRE(runs.front().samples.front() == a.front());

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

TEST_CASE("Corrupt inputs fail with exceptions, never crashes") {
  namespace fsys = std::filesystem;

  SECTION("RED header counter sanity") {
    std::vector<ui1> block(fmt::RED_BLOCK_HEADER_BYTES, 0);
    fmt::RedBlockHeader bh;
    bh.block_bytes = fmt::RED_BLOCK_HEADER_BYTES;
    bh.number_of_samples = 1000;
    bh.difference_bytes = 0;  // impossible: samples need difference symbols
    bh.serialize(block);
    REQUIRE_THROWS_AS(red::decode_block(block, {}, false), FormatError);

    bh.number_of_samples = 10;
    bh.difference_bytes = 0xFFFFFFFFu;  // would drive a 4-billion-symbol loop
    bh.serialize(block);
    REQUIRE_THROWS_AS(red::decode_block(block, {}, false), FormatError);
  }

  SECTION("truncated .tidx (short network read)") {
    const auto dir = fsys::temp_directory_path() / "mef3io_trunc_test.mefd";
    fsys::remove_all(dir);
    {
      SessionWriter w(dir.string(), true);
      std::vector<si4> a(1000);
      for (int i = 0; i < 1000; ++i) a[i] = i;
      w.write_int32("ch1", a, 1.0, 1577836800000000, 250.0);
    }
    const auto tidx = dir / "ch1.timd" / "ch1-000000.segd" / "ch1-000000.tidx";
    fsys::resize_file(tidx, 500);  // smaller than the universal header

    Session s(dir.string());  // open is lazy on indices and must still work
    REQUIRE_THROWS_AS(s.read_runs("ch1"), FormatError);
    REQUIRE_THROWS_AS(s.collect_blocks("ch1"), FormatError);
    REQUIRE_THROWS_AS(s.read_index("ch1"), FormatError);
    fsys::remove_all(dir);
  }
}

TEST_CASE("tmet tolerates trailing padding but not corruption inside the record") {
  namespace fsys = std::filesystem;
  const auto dir = fsys::temp_directory_path() / "mef3io_tmet_pad_test.mefd";
  fsys::remove_all(dir);
  {
    SessionWriter w(dir.string(), true);
    std::vector<si4> a(1000);
    for (int i = 0; i < 1000; ++i) a[i] = i;
    w.write_int32("ch1", a, 1.0, 1577836800000000, 250.0);
  }
  const auto tmet = dir / "ch1.timd" / "ch1-000000.segd" / "ch1-000000.tmet";
  REQUIRE(fsys::file_size(tmet) == static_cast<std::uintmax_t>(fmt::METADATA_FILE_BYTES));

  SECTION("trailing bytes past the fixed-length record are not part of the body CRC") {
    {
      std::ofstream f(tmet, std::ios::binary | std::ios::app);
      std::vector<char> pad(4096, 0);
      f.write(pad.data(), static_cast<std::streamsize>(pad.size()));
    }
    Session s(dir.string());
    REQUIRE(s.channel_info("ch1").number_of_samples == 1000);
    REQUIRE(s.channel_info("ch1").sampling_frequency == 250.0);
  }

  SECTION("corruption within the record still fails, padding or not") {
    {
      std::fstream f(tmet, std::ios::binary | std::ios::in | std::ios::out);
      f.seekp(fmt::METADATA_SECTION_2_OFFSET + 8);
      const char junk[4] = {'\xde', '\xad', '\xbe', '\xef'};
      f.write(junk, 4);
    }
    {
      std::ofstream f(tmet, std::ios::binary | std::ios::app);
      std::vector<char> pad(512, 0);
      f.write(pad.data(), static_cast<std::streamsize>(pad.size()));
    }
    REQUIRE_THROWS_AS(Session(dir.string()), CrcError);
  }
  fsys::remove_all(dir);
}

TEST_CASE("Decoding is thread-count invariant when blocks overlap the sample grid") {
  namespace fsys = std::filesystem;
  const auto dir = fsys::temp_directory_path() / "mef3io_overlap_test.mefd";
  fsys::remove_all(dir);

  const si8 start = 1577836800000000;
  const sf8 fs = 250.0;
  std::mt19937 rng(7);
  std::vector<si4> a(60000);
  for (std::size_t i = 0; i < a.size(); ++i) a[i] = static_cast<si4>(rng() % 20000) - 10000;
  {
    SessionWriter w(dir.string(), true);
    w.write_int32("ch1", a, 1.0, start, fs);
  }

  // Foreign writers store per-block timestamps carrying acquisition jitter, so
  // blocks can claim overlapping output samples. Emulate that by moving each
  // stored block start off the grid (times are stored NEGATED on disk).
  const auto tidx = dir / "ch1.timd" / "ch1-000000.segd" / "ch1-000000.tidx";
  std::vector<ui1> idx;
  {
    std::ifstream f(tidx, std::ios::binary);
    idx.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  }
  const std::size_t n_entries =
      (idx.size() - fmt::UNIVERSAL_HEADER_BYTES) / fmt::TIME_SERIES_INDEX_BYTES;
  REQUIRE(n_entries > 4);
  const int shifts[] = {-9, 4, -13, 7, -2, 11};  // samples
  for (std::size_t i = 1; i < n_entries; ++i) {
    const std::size_t off =
        fmt::UNIVERSAL_HEADER_BYTES + i * fmt::TIME_SERIES_INDEX_BYTES + 8;
    std::span<ui1> field(idx.data() + off, 8);
    const si8 stored = byteio::read<si8>(std::span<const ui1>(idx).subspan(off, 8), 0);
    const si8 shift_us =
        static_cast<si8>(std::llround(shifts[i % 6] * 1e6 / fs));
    byteio::write<si8>(field, 0, stored - shift_us);
  }
  {
    std::ofstream f(tidx, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(idx.data()), static_cast<std::streamsize>(idx.size()));
  }

  // Overlapping blocks are resolved by "last block wins", the result a serial
  // scatter produces; every thread count must reproduce it byte for byte.
  Reader r(dir.string(), "", 1);
  const RawData base = r.read_raw("ch1");
  for (int threads : {2, 3, 4, 8, 16, 0}) {
    for (int rep = 0; rep < 3; ++rep) {
      const RawData got = r.read_raw("ch1", std::nullopt, std::nullopt, threads);
      REQUIRE(got.samples == base.samples);
      REQUIRE(got.valid == base.valid);
    }
  }
  fsys::remove_all(dir);
}

TEST_CASE("Report::ok is false when nothing was examined") {
  // The C++ Report::ok has its own copy of this rule, and only the C ABI and
  // the MATLAB binding consume it — the Python layer recomputes `ok` itself,
  // so no Python test can reach this one. Deleting the guard here left both
  // suites green while a filter that matched nothing reported a clean session.
  namespace fsys = std::filesystem;
  const auto dir = fsys::temp_directory_path() / "mef3io_report_ok_test.mefd";
  fsys::remove_all(dir);

  const si8 start = 1577836800000000;
  std::vector<si4> a(2000);
  std::mt19937 rng(3);
  std::uniform_int_distribution<si4> dist(-1000, 1000);
  for (auto& v : a) v = dist(rng);
  {
    SessionWriter w(dir.string(), true);
    w.write_int32("ch1", a, 1.0, start, 250.0);
  }

  // The session itself is clean...
  REQUIRE(validate_session(dir.string()).ok());

  // ...but a channel filter that matches nothing examined no bytes at all, and
  // "no findings" from "no work" must never read as a pass.
  ValidateOptions opts;
  opts.channels = {"no-such-channel"};
  const auto report = validate_session(dir.string(), opts);
  REQUIRE(report.segments_checked == 0);
  REQUIRE(report.findings.empty());
  REQUIRE_FALSE(report.ok());

  fsys::remove_all(dir);
}
