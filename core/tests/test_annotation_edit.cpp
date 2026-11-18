// mef3io — remove_annotations: selection by content, the rewrite, and a crash at
// every step (the Python suite, tests/test_p54_remove_annotations.py, covers the
// levels, encryption, the oracle and the validator at length).
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "mef3io/annotation_edit.hpp"
#include "mef3io/errors.hpp"
#include "mef3io/naming.hpp"
#include "mef3io/reader.hpp"
#include "mef3io/recover.hpp"
#include "mef3io/records.hpp"
#include "mef3io/session.hpp"
#include "mef3io/session_writer.hpp"
#include "mef3io/validate.hpp"

using namespace mef3io;
namespace fsys = std::filesystem;

namespace {

constexpr si8 T0 = 1577836800000000;
constexpr si8 SEC = 1000000;

Record note(si8 t, const char* text) {
  Record r;
  r.type = "Note";
  r.time = T0 + t * SEC;
  r.text = text;
  return r;
}

fsys::path make_session(const char* name) {
  const auto dir = fsys::temp_directory_path() / name;
  fsys::remove_all(dir);
  fsys::create_directories(dir);
  const auto p = dir / "s.mefd";
  {
    SessionWriter w(p.string(), true);
    std::vector<si4> x(2560);
    for (std::size_t i = 0; i < x.size(); ++i) x[i] = static_cast<si4>(i);
    w.write_int32("ch1", x, 1.0, T0, 256.0);
    w.write_records(std::nullopt, {note(1, "a"), note(2, "b"), note(3, "c")});
    w.write_records(std::string("ch1"), {note(1, "k1"), note(2, "k2")});
  }
  return p;
}

std::vector<std::string> texts(const fsys::path& p, std::optional<std::string> ch = std::nullopt) {
  Session s(p.string(), "");
  std::vector<std::string> out;
  for (const auto& r : s.read_records(ch, std::nullopt,
                                      ch ? Session::RecordLevels::Channel : Session::RecordLevels::Session))
    out.push_back(r.text.value_or(""));
  return out;
}

std::vector<Record> stored(const fsys::path& p, std::optional<std::string> ch = std::nullopt) {
  Session s(p.string(), "");
  return s.read_records(ch, std::nullopt,
                        ch ? Session::RecordLevels::Channel : Session::RecordLevels::Session);
}

std::string code_of(const std::function<void()>& f) {
  try {
    f();
  } catch (const CopyError& e) {
    return e.code();
  } catch (...) {
    return "uncoded";
  }
  return "";
}

}  // namespace

TEST_CASE("remove_annotations removes by content, all or nothing, and keeps the rest") {
  const auto p = make_session("mef3io_remove_basic");
  const auto recs = stored(p);
  REQUIRE(recs.size() == 3);

  // a selector that matches nothing is refused with a code, and nothing changes
  auto ghost = selector_for(recs[1]);
  ghost.fields[0].value = std::string("not there");
  REQUIRE(code_of([&] { remove_annotations(p.string(), {selector_for(recs[0]), ghost}); }) ==
          "no-match");
  REQUIRE(texts(p) == std::vector<std::string>{"a", "b", "c"});

  // a selector that leaves a field out
  auto incomplete = selector_for(recs[1]);
  incomplete.fields.clear();
  REQUIRE(code_of([&] { remove_annotations(p.string(), {incomplete}); }) == "selector-incomplete");

  const auto rep = remove_annotations(p.string(), {selector_for(recs[1])}, {note(2, "b fixed")});
  REQUIRE(rep.removed == 1);
  REQUIRE(rep.added == 1);
  REQUIRE(texts(p) == std::vector<std::string>{"a", "b fixed", "c"});
  REQUIRE(validate_session(p.string()).ok());
  REQUIRE_FALSE(operation_marker(p.string()).has_value());
  fsys::remove_all(p.parent_path());
}

TEST_CASE("remove_annotations: identical copies need expect or all_copies") {
  const auto p = make_session("mef3io_remove_dup");
  {
    SessionWriter w(p.string(), false);
    w.write_records(std::nullopt, {note(5, "dup"), note(5, "dup")});
  }
  Record d = note(5, "dup");
  d.level = RecordLevel::Session;
  auto sel = selector_for(stored(p)[3]);
  REQUIRE(code_of([&] { remove_annotations(p.string(), {sel}); }) == "ambiguous");
  sel.expect = 3;
  REQUIRE(code_of([&] { remove_annotations(p.string(), {sel}); }) == "count-mismatch");
  sel.expect = 2;
  REQUIRE(remove_annotations(p.string(), {sel}).removed == 2);
  REQUIRE(texts(p) == std::vector<std::string>{"a", "b", "c"});
  fsys::remove_all(p.parent_path());
}

TEST_CASE("remove_annotations: a crash at every step ends old or new and recovery converges") {
  int steps = 0;
  for (int k = 0;; ++k) {
    const auto p = make_session("mef3io_remove_crash");
    const auto sess = stored(p), chan = stored(p, std::string("ch1"));
    RemoveAnnotationsOptions o;
    o.crash_at = k;
    bool crashed = false;
    try {
      remove_annotations(p.string(), {selector_for(sess[1]), selector_for(chan[0])},
                         {note(2, "b fixed")}, o);
    } catch (const IoError& e) {
      REQUIRE(std::string(e.what()).find("simulated crash") != std::string::npos);
      crashed = true;
    }
    if (!crashed) {
      REQUIRE(texts(p) == std::vector<std::string>{"a", "b fixed", "c"});
      fsys::remove_all(p.parent_path());
      break;
    }
    ++steps;
    if (const auto edit = interrupted_record_edit(p.string())) {
      // detected: recovery (dry run) says what it would do and writes nothing
      REQUIRE(operation_marker(p.string()).has_value());
      const auto dry = recover_session(p.string());
      REQUIRE(dry.record_edits.size() == 1);
      REQUIRE_FALSE(dry.record_edits[0].applied);
      REQUIRE(edit->action == "finish");
      recover_session(p.string(), true);
      REQUIRE_FALSE(operation_marker(p.string()).has_value());
      REQUIRE(texts(p) == std::vector<std::string>{"a", "b fixed", "c"});
      REQUIRE(texts(p, std::string("ch1")) == std::vector<std::string>{"k2"});
    } else {
      const auto s = texts(p);
      REQUIRE((s == std::vector<std::string>{"a", "b", "c"} ||
               s == std::vector<std::string>{"a", "b fixed", "c"}));
    }
    recover_session(p.string(), true);  // sweeps leftovers
    REQUIRE(validate_session(p.string()).ok());
    fsys::remove_all(p.parent_path());
  }
  REQUIRE(steps >= 14);
}

TEST_CASE("remove_annotations refuses a tar archive and leaves a cancelled call untouched") {
  const auto p = make_session("mef3io_remove_refuse");
  CancelToken token;
  token.cancel();
  Control ctl;
  ctl.cancel = &token;
  const auto recs = stored(p);
  REQUIRE_THROWS_AS(remove_annotations(p.string(), {selector_for(recs[0])}, {}, {}, ctl), Cancelled);
  REQUIRE(texts(p).size() == 3);
  REQUIRE(code_of([&] { remove_annotations(p.string() + ".tar", {selector_for(recs[0])}); }) ==
          "tar-read-only");
  fsys::remove_all(p.parent_path());
}
