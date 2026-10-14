// mef3io — record handling: the one time conversion (#63), every mefrec.h body
// (#62), segment-level records and their C ABI (#61).
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "mef3io/byteio.hpp"
#include "mef3io/c_api.h"
#include "mef3io/crc.hpp"
#include "mef3io/errors.hpp"
#include "mef3io/reader.hpp"
#include "mef3io/records.hpp"
#include "mef3io/session_writer.hpp"
#include "mef3io/time.hpp"

using namespace mef3io;
namespace fsys = std::filesystem;

namespace {

constexpr si8 T0 = 1577836800000000;  // 2020-01-01
constexpr si8 SEC = 1000000;

// One record laid out as meflib does (meflib.h:517-530): 24-byte header, body
// padded to 16 with 0x7e, CRC over [4, end).
std::vector<ui1> make_record(const std::string& type, std::vector<ui1> body, si8 disk_time,
                             ui1 vmaj = 1, ui1 vmin = 0) {
  body.resize((body.size() + 15) & ~std::size_t{15}, 0x7e);
  std::vector<ui1> rec(fmt::RECORD_HEADER_BYTES, 0);
  std::memcpy(rec.data() + 4, type.data(), 4);
  rec[9] = vmaj;
  rec[10] = vmin;
  byteio::write<si1>(rec, 11, -1);
  byteio::write<ui4>(rec, 12, static_cast<ui4>(body.size()));
  byteio::write<si8>(rec, 16, disk_time);
  rec.insert(rec.end(), body.begin(), body.end());
  byteio::write<ui4>(rec, 0, crc::calculate(std::span<const ui1>(rec).subspan(4)));
  return rec;
}

struct Body {
  std::vector<ui1> b;
  template <class T>
  Body& put(T v) {
    const std::size_t off = b.size();
    b.resize(off + sizeof(T));
    byteio::write<T>(b, off, v);
    return *this;
  }
  Body& str(const std::string& s, std::size_t field) {
    const std::size_t off = b.size();
    b.resize(off + field, 0);
    std::memcpy(b.data() + off, s.data(), s.size());
    return *this;
  }
};

std::vector<ui1> seiz_body(si8 onset, si8 offset, si8 ch_onset, si8 ch_offset) {
  Body b;
  b.put<si8>(onset).put<si8>(offset).put<si8>(offset - onset).put<si4>(2).put<si4>(1);
  b.str("m1", 128).str("m2", 128).str("seizure", 1024);
  REQUIRE(b.b.size() == 1312);
  b.str("LA1", 256).put<si8>(ch_onset).put<si8>(ch_offset);
  b.str("LA2", 256).put<si8>(-1).put<si8>(-1);
  return b.b;
}

std::vector<ui1> rdat_image(const std::vector<std::vector<ui1>>& recs) {
  std::vector<ui1> f(fmt::UNIVERSAL_HEADER_BYTES, 0);
  byteio::write<si8>(f, 32, static_cast<si8>(recs.size()));  // number_of_entries
  for (const auto& r : recs) f.insert(f.end(), r.begin(), r.end());
  return f;
}

const RecordField& field(const std::vector<RecordField>& fs, const std::string& name) {
  for (const auto& f : fs)
    if (f.name == name) return f;
  FAIL("no field " << name);
  return fs.front();
}

}  // namespace

TEST_CASE("time conversion: one definition that handles NO_ENTRY itself") {
  constexpr si8 NE = fmt::UUTC_NO_ENTRY;
  constexpr si8 RTO = 3 * 86400 * SEC;

  SECTION("a NO_ENTRY time is a sentinel, never converted") {
    REQUIRE(to_user_time(NE, 0) == NE);
    REQUIRE(to_user_time(NE, RTO) == NE);
    REQUIRE(to_disk_time(NE, RTO) == NE);
  }
  SECTION("a NO_ENTRY offset means no offset (meflib.c:753 / 7669), not overflow") {
    REQUIRE(effective_rto(NE) == 0);
    REQUIRE(to_user_time(-T0, NE) == T0);
    REQUIRE(to_disk_time(T0, NE) == -T0);
  }
  SECTION("non-negative stored times are already absolute") {
    REQUIRE(to_user_time(T0, RTO) == T0);
    REQUIRE(to_user_time(0, RTO) == 0);
  }
  SECTION("negated stored times have the offset removed") {
    REQUIRE(to_user_time(-T0, 0) == T0);
    REQUIRE(to_user_time(-(T0 - RTO), RTO) == T0);
  }
  SECTION("round trip") {
    for (si8 rto : {si8{0}, RTO, NE})
      for (si8 t : {T0, T0 + 1, T0 + 86400 * SEC}) {
        const si8 disk = to_disk_time(t, rto);
        REQUIRE(disk < 0);  // meflib's "offset applied" marker
        REQUIRE(to_user_time(disk, rto) == t);
      }
  }
  SECTION("nonsense input saturates instead of overflowing (signed overflow is UB)") {
    // Body times come straight from file bytes, so these are reachable.
    constexpr si8 MAX = INT64_MAX;
    REQUIRE(to_user_time(NE + 1, RTO) == MAX);          // -stored == MAX, + rto overflows
    REQUIRE(to_user_time(NE + 1, 0) == MAX);
    REQUIRE(to_user_time(-1, MAX) == MAX);
    REQUIRE(to_disk_time(NE + 1, RTO) == MAX);          // rto - (MIN+1) overflows
    REQUIRE(to_disk_time(MAX, -RTO) == NE + 1);         // never becomes the sentinel
    REQUIRE(to_disk_time(MAX, 0) == -MAX);
    static_assert(to_user_time(fmt::UUTC_NO_ENTRY + 1, INT64_MAX) == INT64_MAX);
  }
}

TEST_CASE("parse_records decodes every mefrec.h body type") {
  const si8 RTO = 3 * 86400 * SEC;
  const si8 t = T0 + 5 * SEC;
  const si8 disk = to_disk_time(t, RTO);

  Body epoc;
  // timestamp negated (offset applied), end_timestamp absolute: both must be t-based.
  epoc.put<si8>(42).put<si8>(to_disk_time(t, RTO)).put<si8>(t + 60 * SEC).put<si8>(60 * SEC);
  epoc.str("sleep", 32).str("N2", 128);
  Body curs;
  curs.put<si8>(7).put<si8>(to_disk_time(t, RTO)).put<si8>(1500).put<sf8>(-12.5).str("c1", 128);
  Body csti;
  csti.str("naming", 64).put<si8>(2500).str("picture", 64).str("ok", 64);
  Body esti;
  esti.put<sf8>(2.5).put<sf8>(50.0).put<si8>(300).put<si4>(1).put<si4>(2);
  esti.str("biphasic", 128).str("LA1", 128).str("LA2", 128);
  Body lntp;
  lntp.put<si8>(3).put<si4>(1).put<si4>(-2).put<si4>(3);

  const auto img = rdat_image({
      make_record("Seiz", seiz_body(t, t + 30 * SEC, to_disk_time(t + SEC, RTO), t + 20 * SEC),
                  disk),
      make_record("Epoc", epoc.b, disk),
      make_record("Curs", curs.b, disk),
      make_record("CSti", csti.b, disk),
      make_record("ESti", esti.b, disk),
      make_record("LNTP", lntp.b, disk),
      make_record("UnRc", {}, disk),
      make_record("Epoc", epoc.b, disk, 2, 0),  // a version meflib does not decode
  });
  const auto recs = parse_records(img, RTO);
  REQUIRE(recs.size() == 8);
  for (const auto& r : recs) REQUIRE(r.time == t);

  const Record& s = recs[0];
  REQUIRE(*s.earliest_onset == t);                 // positive body time: unchanged
  REQUIRE(*s.latest_offset == t + 30 * SEC);
  REQUIRE(*s.duration == 30 * SEC);
  REQUIRE(*s.number_of_channels == 2);
  REQUIRE(*s.onset_code == 1);
  REQUIRE(*s.marker_name_1 == "m1");
  REQUIRE(*s.annotation == "seizure");
  REQUIRE(s.seizure_channels.size() == 2);
  REQUIRE(s.seizure_channels[0].name == "LA1");
  REQUIRE(s.seizure_channels[0].onset == t + SEC);  // negated body time: offset removed
  REQUIRE(s.seizure_channels[0].offset == t + 20 * SEC);
  REQUIRE(s.seizure_channels[1].onset == -1);       // mefrec.h:129 NO_ENTRY kept
  REQUIRE(s.seizure_channels[1].offset == -1);

  REQUIRE(*recs[1].id_number == 42);
  REQUIRE(*recs[1].timestamp == t);
  REQUIRE(*recs[1].end_timestamp == t + 60 * SEC);
  REQUIRE(*recs[1].epoch_type == "sleep");
  REQUIRE(*recs[1].text == "N2");

  REQUIRE(*recs[2].trace_timestamp == t);
  REQUIRE(*recs[2].latency == 1500);
  REQUIRE(*recs[2].value == -12.5);
  REQUIRE(*recs[2].name == "c1");

  REQUIRE(*recs[3].task_type == "naming");
  REQUIRE(*recs[3].stimulus_duration == 2500);
  REQUIRE(*recs[3].patient_response == "ok");

  REQUIRE(*recs[4].amplitude == 2.5);
  REQUIRE(*recs[4].mode_code == 2);
  REQUIRE(*recs[4].catode == "LA2");

  REQUIRE(*recs[5].template_length == 3);
  REQUIRE(recs[5].line_noise_template == std::vector<si4>{1, -2, 3});

  REQUIRE(record_fields(recs[6]).empty());          // UnRc: no defined body

  SECTION("record_fields: same names in the same order, absent values reported") {
    const auto good = record_fields(recs[1]);
    const auto future = record_fields(recs[7]);
    REQUIRE(good.size() == future.size());
    for (std::size_t i = 0; i < good.size(); ++i) {
      REQUIRE(good[i].name == future[i].name);
      REQUIRE(good[i].value.has_value());
      REQUIRE_FALSE(future[i].value.has_value());
    }
    const auto seiz = record_fields(s);
    const auto& chans =
        std::get<std::vector<SeizureChannel>>(*field(seiz, "channels").value);
    REQUIRE(chans.size() == 2);
  }
}

TEST_CASE("C ABI reads records at every level with every body field") {
  const auto dir = fsys::temp_directory_path() / "mef3io_capi_records.mefd";
  fsys::remove_all(dir);
  const std::string path = dir.string();

  std::vector<double> data(1000, 0.0);
  mef3io_writer* w = nullptr;
  REQUIRE(mef3io_writer_open(path.c_str(), 1, "", "", &w) == MEF3IO_OK);
  mef3io_write_summary sum{};
  REQUIRE(mef3io_writer_write_float(w, "ch1", data.data(), 1000, T0, 250.0, 3, 0, &sum) ==
          MEF3IO_OK);
  mef3io_records* recs = mef3io_records_create();
  REQUIRE(mef3io_records_add(recs, "Note", T0 + SEC, "segment note", -1) == MEF3IO_OK);
  REQUIRE(mef3io_writer_write_records(w, "ch1", recs) == MEF3IO_OK);
  REQUIRE(mef3io_writer_write_records(w, nullptr, recs) == MEF3IO_OK);
  mef3io_records_free(recs);
  mef3io_writer_close(w);

  // Move the channel-level files to where meflib looks for SEGMENT records
  // (meflib.c:6026/6030), and replace the session file with a Seiz record.
  const auto timd = dir / "ch1.timd";
  for (const char* ext : {".rdat", ".ridx"})
    fsys::rename(timd / (std::string("ch1") + ext),
                 timd / "ch1-000000.segd" / (std::string("ch1-000000") + ext));
  fsys::path session_rdat;
  for (const auto& e : fsys::directory_iterator(dir))
    if (e.path().extension() == ".rdat") session_rdat = e.path();
  REQUIRE(!session_rdat.empty());
  {
    std::ifstream in(session_rdat, std::ios::binary);
    std::vector<ui1> img(fmt::UNIVERSAL_HEADER_BYTES);
    in.read(reinterpret_cast<char*>(img.data()), fmt::UNIVERSAL_HEADER_BYTES);
    in.close();
    const auto rec = make_record("Seiz", seiz_body(T0, T0 + SEC, T0, T0 + SEC), -T0);
    img.insert(img.end(), rec.begin(), rec.end());
    std::ofstream(session_rdat, std::ios::binary | std::ios::trunc)
        .write(reinterpret_cast<const char*>(img.data()), static_cast<std::streamsize>(img.size()));
  }

  mef3io_reader* r = nullptr;
  REQUIRE(mef3io_reader_open(path.c_str(), "", 1, &r) == MEF3IO_OK);

  mef3io_record_list* list = nullptr;
  REQUIRE(mef3io_reader_read_records(r, nullptr, -1, MEF3IO_RECORD_LEVEL_ALL, &list) ==
          MEF3IO_OK);
  int32_t n = 0;
  REQUIRE(mef3io_record_list_size(list, &n) == MEF3IO_OK);
  REQUIRE(n == 2);

  mef3io_record_header h{};
  REQUIRE(mef3io_record_list_header(list, 0, &h) == MEF3IO_OK);  // session first
  REQUIRE(std::string(h.type) == "Seiz");
  REQUIRE(h.level == MEF3IO_RECORD_LEVEL_SESSION);
  REQUIRE(h.segment == -1);
  REQUIRE(std::string(h.channel).empty());
  REQUIRE(h.time == T0);
  REQUIRE(h.n_fields == 9);
  mef3io_record_field f{};
  REQUIRE(mef3io_record_list_field(list, 0, 0, &f) == MEF3IO_OK);
  REQUIRE(std::string(f.name) == "earliest_onset");
  REQUIRE(f.kind == MEF3IO_FIELD_INT64);
  REQUIRE(f.int_value == T0);
  REQUIRE(mef3io_record_list_field(list, 0, 7, &f) == MEF3IO_OK);
  REQUIRE(std::string(f.name) == "annotation");
  REQUIRE(f.kind == MEF3IO_FIELD_STRING);
  REQUIRE(std::string(f.string_value) == "seizure");
  REQUIRE(mef3io_record_list_field(list, 0, 8, &f) == MEF3IO_OK);
  REQUIRE(f.kind == MEF3IO_FIELD_SEIZURE_CHANNELS);
  REQUIRE(f.count == 2);
  mef3io_seizure_channel sc{};
  REQUIRE(mef3io_record_list_seizure_channel(list, 0, 8, 1, &sc) == MEF3IO_OK);
  REQUIRE(std::string(sc.name) == "LA2");
  REQUIRE(sc.onset == -1);
  REQUIRE(mef3io_record_list_seizure_channel(list, 0, 7, 0, &sc) == MEF3IO_ERR_ARGUMENT);

  REQUIRE(mef3io_record_list_header(list, 1, &h) == MEF3IO_OK);
  REQUIRE(std::string(h.type) == "Note");
  REQUIRE(h.level == MEF3IO_RECORD_LEVEL_SEGMENT);
  REQUIRE(h.segment == 0);
  REQUIRE(std::string(h.channel) == "ch1");
  REQUIRE(mef3io_record_list_field(list, 1, 0, &f) == MEF3IO_OK);
  REQUIRE(std::string(f.string_value) == "segment note");
  mef3io_record_list_free(list);

  // The compatibility entry points see segment records under the channel.
  int32_t nrec = 0;
  REQUIRE(mef3io_reader_n_records(r, "ch1", &nrec) == MEF3IO_OK);
  REQUIRE(nrec == 1);

  // Query errors are argument errors.
  REQUIRE(mef3io_reader_read_records(r, nullptr, 0, MEF3IO_RECORD_LEVEL_DEFAULT, &list) ==
          MEF3IO_ERR_ARGUMENT);
  REQUIRE(list == nullptr);
  REQUIRE(mef3io_reader_read_records(r, "ch1", -1, MEF3IO_RECORD_LEVEL_SESSION, &list) ==
          MEF3IO_ERR_ARGUMENT);
  REQUIRE(mef3io_reader_read_records(r, "ch1", 9, MEF3IO_RECORD_LEVEL_DEFAULT, &list) ==
          MEF3IO_ERR_ARGUMENT);
  REQUIRE(mef3io_reader_read_records(r, "ch1", -1, 17, &list) == MEF3IO_ERR_ARGUMENT);

  mef3io_reader_close(r);
  fsys::remove_all(dir);
}

namespace {
// Give one .tmet's section 3 this recording-time offset and reseal both CRCs
// (the body CRC bounded by the fixed record length, never to EOF).
void set_tmet_rto(const fsys::path& tmet, si8 rto) {
  std::vector<ui1> raw(fsys::file_size(tmet));
  std::ifstream(tmet, std::ios::binary)
      .read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(raw.size()));
  constexpr std::size_t S3_RTO = 13312, FILE_BYTES = 16384;
  byteio::write<si8>(raw, S3_RTO, rto);
  const std::span<const ui1> all(raw);
  byteio::write<ui4>(raw, 4, crc::calculate(all.subspan(fmt::UNIVERSAL_HEADER_BYTES,
                                                        FILE_BYTES - fmt::UNIVERSAL_HEADER_BYTES)));
  byteio::write<ui4>(raw, 0, crc::calculate(all.subspan(4, fmt::UNIVERSAL_HEADER_BYTES - 4)));
  std::ofstream(tmet, std::ios::binary | std::ios::trunc)
      .write(reinterpret_cast<const char*>(raw.data()), static_cast<std::streamsize>(raw.size()));
}
}  // namespace

TEST_CASE("Record times follow the offset of the level they belong to") {
  const auto dir = fsys::temp_directory_path() / "mef3io_records_rto.mefd";
  fsys::remove_all(dir);
  const si8 RTO = 3 * 86400 * SEC;
  {
    SessionWriter w(dir.string(), true);
    const std::vector<si4> x(2560, 0);
    w.write_int32("ch1", x, 1.0, T0, 256.0);
    w.write_int32("ch2", x, 1.0, T0, 256.0);
    Record r;
    r.type = "Note";
    r.time = T0 + SEC;
    r.text = "note";
    w.write_records(std::nullopt, {r});
    w.write_records("ch1", {r});
  }
  // The same file at the segment level too (meflib.c:6030 naming).
  const auto segd = dir / "ch1.timd" / "ch1-000000.segd";
  fsys::copy_file(dir / "ch1.timd" / "ch1.rdat", segd / "ch1-000000.rdat");

  SECTION("segment-level records use their segment's own offset") {
    set_tmet_rto(segd / "ch1-000000.tmet", RTO);
    set_tmet_rto(dir / "ch2.timd" / "ch2-000000.segd" / "ch2-000000.tmet", RTO);
    Reader r(dir.string());
    const auto recs = r.records("ch1", 0);
    REQUIRE(recs.size() == 1);
    REQUIRE(recs[0].level == RecordLevel::Segment);
    REQUIRE(recs[0].time == T0 + SEC + RTO);  // written with rto 0, now read with RTO
    REQUIRE(recs[0].time - r.info("ch1").start_time == SEC);
    REQUIRE(r.records()[0].time - r.info("ch1").start_time == SEC);  // session level too
  }

  SECTION("a disputed offset refuses offset-dependent records, and only those") {
    set_tmet_rto(dir / "ch2.timd" / "ch2-000000.segd" / "ch2-000000.tmet", RTO);
    Reader strict(dir.string());
    REQUIRE_THROWS_AS(strict.records(), FormatError);         // session level is disputed
    REQUIRE(strict.records("ch1").size() == 2);               // ch1 agrees with itself

    Reader lenient(dir.string(), "", 0, /*strict=*/false);
    REQUIRE(lenient.records().empty());
    REQUIRE(lenient.problems().size() == 1);
    REQUIRE(lenient.problems()[0].segment_number == -1);
    REQUIRE(lenient.records(std::nullopt, std::nullopt, Session::RecordLevels::All).size() == 2);
    REQUIRE(lenient.problems().size() == 1);                  // reported once
    REQUIRE(lenient.problems()[0].kind == "records");

    // The same through the C ABI, which reports the kind separately so
    // mef3io_segment_problem keeps its layout.
    mef3io_reader* r = nullptr;
    REQUIRE(mef3io_reader_open_ex(dir.string().c_str(), "", 1, 0, &r) == MEF3IO_OK);
    mef3io_record_list* list = nullptr;
    REQUIRE(mef3io_reader_read_records(r, nullptr, -1, MEF3IO_RECORD_LEVEL_SESSION, &list) ==
            MEF3IO_OK);
    mef3io_record_list_free(list);
    int32_t n = 0, kind = -1;
    REQUIRE(mef3io_reader_n_problems(r, &n) == MEF3IO_OK);
    REQUIRE(n == 1);
    mef3io_segment_problem p{};
    REQUIRE(mef3io_reader_problem(r, 0, &p) == MEF3IO_OK);
    REQUIRE(p.segment_number == -1);
    REQUIRE(mef3io_reader_problem_kind(r, 0, &kind) == MEF3IO_OK);
    REQUIRE(kind == MEF3IO_PROBLEM_RECORDS);
    REQUIRE(mef3io_reader_problem_kind(r, 1, &kind) == MEF3IO_ERR_ARGUMENT);
    mef3io_reader_close(r);
  }
  fsys::remove_all(dir);
}

TEST_CASE("mef3io stores times offset-applied and refuses what would reveal the date") {
  constexpr si8 RTO = 3 * 86400 * SEC;
  const si8 R = T0 + RTO;
  REQUIRE(to_stored_time(T0, 0) == -T0);                        // byte-identical to before
  REQUIRE(to_stored_time(T0, fmt::UUTC_NO_ENTRY) == -T0);
  // After the offset: rto - t, negative, never the absolute time.
  const si8 after = R + SEC;
  REQUIRE(time_is_storable(after, R));
  REQUIRE(to_stored_time(after, R) == -SEC);
  REQUIRE(to_user_time(to_stored_time(after, R), R) == after);
  REQUIRE_NOTHROW(require_storable_time(after, R, "t"));
  // At or before it: not representable without writing the real date.
  for (si8 t : {R, R - SEC}) {
    REQUIRE_FALSE(time_is_storable(t, R));
    REQUIRE_THROWS_AS(require_storable_time(t, R, "t"), WriteConflictError);
  }
  REQUIRE(time_is_storable(T0, 0));                             // rto 0: anything goes
  REQUIRE(time_is_storable(fmt::UUTC_NO_ENTRY, R));
  REQUIRE(to_stored_time(fmt::UUTC_NO_ENTRY, RTO) == fmt::UUTC_NO_ENTRY);
}

static std::vector<ui1> text_body(const std::string& s) {
  std::vector<ui1> b(s.begin(), s.end());
  b.push_back(0);
  return b;
}

TEST_CASE("parse_records fails on damage instead of returning less") {
  const auto good = rdat_image({make_record("Note", text_body("first"), -T0),
                                make_record("Note", text_body("second"), -T0)});
  REQUIRE(parse_records(good, 0).size() == 2);

  SECTION("cut off mid-record") {
    std::vector<ui1> img(good.begin(), good.end() - 10);
    REQUIRE_THROWS_AS(parse_records(img, 0), FormatError);
  }
  SECTION("a body larger than the file") {
    auto img = good;
    byteio::write<ui4>(img, fmt::UNIVERSAL_HEADER_BYTES + 12, 0x7FFFFFF0u);
    REQUIRE_THROWS_AS(parse_records(img, 0), FormatError);
  }
  SECTION("a flipped body byte fails the record CRC (meflib.c:1839)") {
    auto img = good;
    img[fmt::UNIVERSAL_HEADER_BYTES + fmt::RECORD_HEADER_BYTES] ^= 0x01;
    REQUIRE_THROWS_AS(parse_records(img, 0), CrcError);
  }
  SECTION("fewer records than the universal header declares") {
    auto img = good;
    byteio::write<si8>(img, 32, 3);
    REQUIRE_THROWS_AS(parse_records(img, 0), FormatError);
  }
  SECTION("an unknown count still stops cleanly at zero padding") {
    auto img = good;
    byteio::write<si8>(img, 32, -1);
    img.resize(img.size() + 32, 0);
    REQUIRE(parse_records(img, 0).size() == 2);
  }
  SECTION("a CRC of NO_ENTRY (0) is not checked") {
    auto img = good;
    byteio::write<ui4>(img, fmt::UNIVERSAL_HEADER_BYTES, 0u);
    img[fmt::UNIVERSAL_HEADER_BYTES + fmt::RECORD_HEADER_BYTES] ^= 0x01;
    REQUIRE(parse_records(img, 0).size() == 2);
  }
}

TEST_CASE("C ABI reports segment metadata the writer could not use") {
  const auto dir = fsys::temp_directory_path() / "mef3io_unreadable_md.mefd";
  fsys::remove_all(dir);
  {
    SessionWriter w(dir.string(), true);
    const std::vector<si4> x(512, 0);
    w.write_int32("ch1", x, 1.0, T0, 256.0);
    w.write_int32("ch2", x, 1.0, T0, 256.0);
  }
  // Break ch2's metadata body (the CRC check rejects it).
  const auto tmet = dir / "ch2.timd" / "ch2-000000.segd" / "ch2-000000.tmet";
  {
    std::fstream f(tmet, std::ios::in | std::ios::out | std::ios::binary);
    f.seekp(2000);
    const char junk = 0x5a;
    f.write(&junk, 1);
  }
  mef3io_writer* w = nullptr;
  REQUIRE(mef3io_writer_open(dir.string().c_str(), 0, "", "", &w) == MEF3IO_OK);
  int32_t n = -1;
  REQUIRE(mef3io_writer_n_unreadable_metadata(w, &n) == MEF3IO_OK);
  REQUIRE(n == 0);
  mef3io_records* recs = mef3io_records_create();
  REQUIRE(mef3io_records_add(recs, "Note", T0, "ok", -1) == MEF3IO_OK);
  // Offset 0 everywhere readable: skipped and reported, the write goes ahead.
  REQUIRE(mef3io_writer_write_records(w, nullptr, recs) == MEF3IO_OK);
  mef3io_records_free(recs);
  REQUIRE(mef3io_writer_n_unreadable_metadata(w, &n) == MEF3IO_OK);
  REQUIRE(n == 1);
  char buf[1024];
  REQUIRE(mef3io_writer_unreadable_metadata(w, 0, buf, sizeof buf) == MEF3IO_OK);
  REQUIRE(std::string(buf).find("ch2-000000.tmet") != std::string::npos);
  REQUIRE(mef3io_writer_unreadable_metadata(w, 1, buf, sizeof buf) == MEF3IO_ERR_ARGUMENT);
  mef3io_writer_close(w);
  fsys::remove_all(dir);
}
