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
#include "mef3io/crc.hpp"
#include "mef3io/reader.hpp"
#include "mef3io/source.hpp"

#include "../src/segment_copy.hpp"
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

TEST_CASE("copy_segment: dropping blocks flags the next kept one and renumbers samples") {
  const auto dir = fresh("mef3io_copy_drop");
  const std::string path = (dir / "c.mefd").string();
  {
    SessionWriter w(path, true);
    std::vector<si4> x(50000);
    for (std::size_t i = 0; i < x.size(); ++i) x[i] = static_cast<si4>(i % 97);
    w.write_int32("a", x, 1.0, START, 1000.0, {}, false);
  }
  auto src = open_session_source(path);
  const std::string seg = "a.timd/a-000000.segd/a-000000";
  const auto out = dir / "out";
  fsys::create_directories(out);
  detail::SegmentCopySpec spec;
  spec.source = src.get();
  spec.tidx_rel = seg + ".tidx";
  spec.tdat_rel = seg + ".tdat";
  spec.tidx_out = (out / "x.tidx").string();
  spec.tdat_out = (out / "x.tdat").string();
  std::size_t total = 0;
  spec.on_block = [&](detail::CopyBlock& b) {
    ++total;
    return b.index != 1;  // drop the second block
  };
  const auto st = detail::copy_segment(spec);
  REQUIRE(total >= 3);
  CHECK(st.blocks == static_cast<si8>(total - 1));

  const auto idx = slurp(out / "x.tidx");
  const auto dat = slurp(out / "x.tdat");
  si8 expect_sample = 0, off = 1024;
  for (si8 i = 0; i < st.blocks; ++i) {
    std::span<const ui1> e(idx.data() + 1024 + i * 56, 56);
    const si8 fo = byteio::read<si8>(e, 0);
    CHECK(fo == off);
    const ui4 nb = byteio::read<ui4>(e, 28), ns = byteio::read<ui4>(e, 24);
    CHECK(byteio::read<si8>(e, 16) == expect_sample);  // dense after the drop
    const bool discont = (e[44] & 1) != 0, hdr_discont = (dat[fo + 4] & 1) != 0;
    CHECK(discont == hdr_discont);
    if (i == 1) CHECK(discont);  // the block after the dropped one
    std::span<const ui1> blk(dat.data() + fo, nb);
    CHECK(crc::calculate(blk.subspan(4)) == byteio::read<ui4>(blk, 0));
    expect_sample += ns;
    off += nb;
  }
  CHECK(st.discontinuities == 2);
  CHECK(st.max_contiguous_blocks == st.blocks - 1);
  CHECK(crc::calculate(std::span<const ui1>(idx).subspan(1024)) == byteio::read<ui4>(idx, 4));
  CHECK(crc::calculate(std::span<const ui1>(dat).subspan(1024)) == byteio::read<ui4>(dat, 4));

  SECTION("a LEADING drop: the first written block is a discontinuity, renumbered") {
    const auto out2 = dir / "out2";
    fsys::create_directories(out2);
    // Clear the source's own flag on block 1 so only the drop can set it.
    spec.tidx_out = (out2 / "y.tidx").string();
    spec.tdat_out = (out2 / "y.tdat").string();
    spec.start_sample = 0;
    spec.on_block = [&](detail::CopyBlock& b) {
      b.block[4] = static_cast<ui1>(b.block[4] & ~1u);
      b.entry[44] = static_cast<ui1>(b.entry[44] & ~1u);
      return b.index != 0;  // drop the FIRST block
    };
    const auto st2 = detail::copy_segment(spec);
    REQUIRE(st2.blocks >= 1);
    const auto idx2 = slurp(out2 / "y.tidx");
    const auto dat2 = slurp(out2 / "y.tdat");
    std::span<const ui1> e0(idx2.data() + 1024, 56);
    CHECK((e0[44] & 1) != 0);                      // index flag
    CHECK((dat2[1024 + 4] & 1) != 0);              // RED header flag
    CHECK(byteio::read<si8>(e0, 16) == 0);         // renumbered from start_sample
    std::span<const ui1> blk(dat2.data() + 1024, byteio::read<ui4>(e0, 28));
    CHECK(crc::calculate(blk.subspan(4)) == byteio::read<ui4>(blk, 0));
    if (st2.blocks > 1) {
      std::span<const ui1> e1(idx2.data() + 1024 + 56, 56);
      CHECK((e1[44] & 1) == 0);                    // only the first is flagged
    }
    CHECK(st2.discontinuities == 1);
  }

  SECTION("a LEADING drop without start_sample is refused (precondition for #103)") {
    const auto out3 = dir / "out3";
    fsys::create_directories(out3);
    spec.tidx_out = (out3 / "z.tidx").string();
    spec.tdat_out = (out3 / "z.tdat").string();
    spec.start_sample.reset();
    spec.on_block = [](detail::CopyBlock& b) { return b.index != 0; };
    CHECK_THROWS_AS(detail::copy_segment(spec), std::invalid_argument);
  }
}
