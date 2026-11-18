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
#include "mef3io/c_api.h"
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

TEST_CASE("remove_annotations refuses a file with bytes after the last counted record") {
  const auto p = make_session("mef3io_remove_torn");
  const auto recs = stored(p);
  // a whole, valid record appended to the .rdat only (data landed, header and index did not)
  const auto enc = encode_records_for_file({note(9, "TAILREC")}, 0, false, {});
  const auto rdat = p / "s.rdat";
  {
    std::ofstream f(rdat, std::ios::binary | std::ios::app);
    f.write(reinterpret_cast<const char*>(enc[0].bytes.data()),
            static_cast<std::streamsize>(enc[0].bytes.size()));
  }
  const auto size = fsys::file_size(rdat);
  REQUIRE(code_of([&] { remove_annotations(p.string(), {selector_for(recs[0])}); }) ==
          "record-torn");
  REQUIRE(fsys::file_size(rdat) == size);
  // recovery keeps the record, then the removal works and the record survives
  recover_session(p.string(), true);
  REQUIRE(remove_annotations(p.string(), {selector_for(recs[0])}).removed == 1);
  REQUIRE(texts(p) == std::vector<std::string>{"b", "c", "TAILREC"});
  fsys::remove_all(p.parent_path());
}

namespace {

using Events = std::vector<std::pair<std::string, std::string>>;

Events run_edit_recording(const char* name) {
  const auto p = make_session(name);
  const auto sess = stored(p), chan = stored(p, std::string("ch1"));
  Events events;
  testing::set_edit_observer(
      [&](const std::string& k, const std::string& v) { events.emplace_back(k, v); });
  remove_annotations(p.string(), {selector_for(sess[1]), selector_for(chan[0])}, {note(2, "x")});
  testing::set_edit_observer({});
  fsys::remove_all(p.parent_path());
  return events;
}

/// The durable order of one edit: every new file fsynced, then a folder flush, then the
/// marker (its part file fsynced, renamed into place, folder flushed) BEFORE the first
/// target -> .old rename.
bool order_holds(const Events& e) {
  auto index = [&](const std::string& kind, const std::string& ends, std::size_t from = 0) {
    for (std::size_t i = from; i < e.size(); ++i)
      if (e[i].first == kind && e[i].second.size() >= ends.size() &&
          e[i].second.compare(e[i].second.size() - ends.size(), ends.size(), ends) == 0)
        return i;
    return e.size();
  };
  std::size_t last_new_fsync = e.size();
  for (std::size_t i = 0; i < e.size(); ++i)
    if (e[i].first == "fsync-file" && e[i].second.size() > 4 &&
        e[i].second.compare(e[i].second.size() - 4, 4, ".new") == 0)
      last_new_fsync = i;
  const std::size_t marker = index("marker-durable", "");
  const std::size_t part_fsync = index("fsync-file", ".mef3io-operation.part");
  if (last_new_fsync == e.size() || marker == e.size() || part_fsync == e.size()) return false;
  bool flush_between = false;
  for (std::size_t i = last_new_fsync; i < marker; ++i) flush_between = flush_between || e[i].first == "flush-dir";
  bool flush_after_part = false;
  for (std::size_t i = part_fsync; i < marker; ++i) flush_after_part = flush_after_part || e[i].first == "flush-dir";
  std::size_t to_old = e.size();
  for (std::size_t i = 0; i < e.size(); ++i)
    if (e[i].first == "rename") {
      const auto bar = e[i].second.find('|');
      const std::string dst = e[i].second.substr(bar + 1);
      if (dst.size() > 4 && dst.compare(dst.size() - 4, 4, ".old") == 0) {
        to_old = i;
        break;
      }
    }
  return flush_between && flush_after_part && part_fsync < marker && marker < to_old;
}

}  // namespace

TEST_CASE("the durable steps of an edit come in the required order, and the check can fail") {
  REQUIRE(order_holds(run_edit_recording("mef3io_remove_order")));
#ifdef MEF3IO_TEST_SEAMS
  // negative control: an edit that skips its flushes must NOT satisfy the same check
  testing::set_edit_skip_flush(true);
  const auto skipped = run_edit_recording("mef3io_remove_order_skip");
  testing::set_edit_skip_flush(false);
  REQUIRE_FALSE(order_holds(skipped));
#endif
}

TEST_CASE("every reader of records refuses while an edit is interrupted (C ABI included)") {
  for (int k = 0;; ++k) {
    const auto p = make_session("mef3io_remove_capi");
    const auto sess = stored(p);
    RemoveAnnotationsOptions o;
    o.crash_at = k;
    try {
      remove_annotations(p.string(), {selector_for(sess[1])}, {}, o);
      fsys::remove_all(p.parent_path());
      break;
    } catch (const IoError&) {
    }
    if (has_interrupted_edit(p.string())) {
      REQUIRE(code_of([&] { Session(p.string(), "").read_records(); }) == "edit-interrupted");
      mef3io_reader* r = nullptr;
      REQUIRE(mef3io_reader_open(p.string().c_str(), "", 1, &r) == MEF3IO_OK);
      mef3io_record_list* list = nullptr;
      REQUIRE(mef3io_reader_read_records(r, nullptr, -1, MEF3IO_RECORD_LEVEL_ALL, &list) !=
              MEF3IO_OK);
      REQUIRE(std::string(mef3io_last_error()).find("annotation edit") != std::string::npos);
      mef3io_reader_close(r);
    }
    fsys::remove_all(p.parent_path());
  }
}

namespace {

/// A crashed edit of the session (marker present, swap not yet started: every .new in place).
fsys::path crashed_edit(const char* name) {
  for (int k = 0;; ++k) {
    const auto p = make_session(name);
    const auto sess = stored(p);
    RemoveAnnotationsOptions o;
    o.crash_at = k;
    try {
      remove_annotations(p.string(), {selector_for(sess[1])}, {}, o);
    } catch (const IoError&) {
    }
    if (has_interrupted_edit(p.string())) {
      bool all_new = true;
      for (const char* f : {"s.rdat", "s.ridx"}) {
        bool any = false;
        for (const auto& e : fsys::directory_iterator(p))
          any = any || e.path().filename().string().rfind(std::string(f) + ".", 0) == 0;
        all_new = all_new && any;
      }
      if (all_new) return p;
    }
    fsys::remove_all(p.parent_path());
  }
}

std::string c_recover(const fsys::path& p, int* rc) {
  std::vector<char> buf(8192);
  int64_t n = 0;
  *rc = mef3io_recover_session(p.string().c_str(), 1, 1, "", buf.data(), buf.size(), &n);
  return std::string(buf.data());
}

}  // namespace

TEST_CASE("the C API recovery summary says what became of an interrupted annotation edit") {
  int rc = 0;
  auto p = crashed_edit("mef3io_capi_done");
  auto text = c_recover(p, &rc);
  REQUIRE(rc == MEF3IO_OK);
  REQUIRE(text.find("interrupted annotation edit") != std::string::npos);
  REQUIRE(text.find("finished") != std::string::npos);
  REQUIRE(text.find("NOT done") == std::string::npos);
  fsys::remove_all(p.parent_path());

  // the new files are gone: rolled back, and the summary says the removal was NOT done
  p = crashed_edit("mef3io_capi_back");
  for (const auto& e : fsys::directory_iterator(p))
    if (e.path().extension() == ".new") fsys::remove(e.path());
  for (const auto& e : fsys::directory_iterator(p)) {
    const auto n = e.path().filename().string();
    if (n.size() > 4 && n.compare(n.size() - 4, 4, ".new") == 0) fsys::remove(e.path());
  }
  text = c_recover(p, &rc);
  REQUIRE(rc == MEF3IO_OK);
  REQUIRE(text.find("rolled back") != std::string::npos);
  REQUIRE(text.find("the removal was NOT done") != std::string::npos);
  fsys::remove_all(p.parent_path());

  // a marker of a newer layout: refused through the error channel, nothing changed
  p = crashed_edit("mef3io_capi_refused");
  {
    std::ifstream in(p / ".mef3io-operation", std::ios::binary);
    std::string body((std::istreambuf_iterator<char>(in)), {});
    in.close();
    const auto at = body.find("edit-format 1");
    REQUIRE(at != std::string::npos);
    body.replace(at, 13, "edit-format 2");
    std::ofstream(p / ".mef3io-operation", std::ios::binary | std::ios::trunc) << body;
  }
  text = c_recover(p, &rc);
  REQUIRE(rc != MEF3IO_OK);
  REQUIRE(std::string(mef3io_last_error()).find("newer mef3io") != std::string::npos);
  REQUIRE(has_interrupted_edit(p.string()));
  fsys::remove_all(p.parent_path());
}
