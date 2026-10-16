// mef3io — nanobind bindings. Fleshed out per phase; P0 exposes just enough
// (crc + version) to prove the toolchain end to end.
#include <nanobind/nanobind.h>
#include <nanobind/ndarray.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include <span>
#include <optional>
#include <utility>
#include <vector>

#include "mef3io/control.hpp"
#include "mef3io/crc.hpp"
#include "mef3io/errors.hpp"
#include "mef3io/crypto.hpp"
#include "mef3io/inspect.hpp"
#include "mef3io/metadata.hpp"
#include "mef3io/reader.hpp"
#include "mef3io/recover.hpp"
#include "mef3io/red.hpp"
#include "mef3io/session.hpp"
#include "mef3io/session_writer.hpp"
#include "mef3io/tar.hpp"
#include "mef3io/validate.hpp"
#include "mef3io/types.hpp"
#include "mef3io/version.hpp"
#include "mef3io/writer.hpp"

#include <cmath>
#include <algorithm>
#include <filesystem>

namespace nb = nanobind;

namespace {
// Move a vector<T> onto the heap and hand numpy ownership via a capsule.
template <typename T>
nb::ndarray<nb::numpy, T> vec_to_numpy(std::vector<T>&& v) {
  auto* heap = new std::vector<T>(std::move(v));
  nb::capsule owner(heap, [](void* p) noexcept { delete static_cast<std::vector<T>*>(p); });
  size_t n = heap->size();
  return nb::ndarray<nb::numpy, T>(heap->data(), {n}, owner);
}
}  // namespace

namespace {
std::span<const mef3io::ui1> as_span(nb::bytes b) {
  return {reinterpret_cast<const mef3io::ui1*>(b.c_str()), b.size()};
}
nb::bytes to_bytes(std::span<const mef3io::ui1> s) {
  return nb::bytes(reinterpret_cast<const char*>(s.data()), s.size());
}
}  // namespace

namespace {
nb::dict report_to_dict(const mef3io::Report& r) {
  nb::list findings;
  for (const auto& f : r.findings) {
    nb::dict d;
    d["check_id"] = f.check_id;
    d["severity"] = mef3io::severity_name(f.severity);
    d["channel"] = f.channel;
    d["segment"] = f.segment_number;
    d["path"] = f.path;
    d["field"] = f.field;
    d["stored"] = f.stored;
    d["expected"] = f.expected;
    d["message"] = f.message;
    d["repairable"] = f.repairable;
    d["repaired"] = f.repaired;
    findings.append(d);
  }
  nb::list skipped;
  for (const auto& s : r.skipped) {
    nb::dict d;
    d["channel"] = s.channel;
    d["segment"] = s.segment_number;
    d["path"] = s.path;
    d["reason"] = s.reason;
    skipped.append(d);
  }
  nb::dict out;
  out["findings"] = findings;
  out["skipped"] = skipped;
  out["segments_checked"] = r.segments_checked;
  out["segments_repaired"] = r.segments_repaired;
  out["checks_run"] = r.checks_run;
  out["checks_repaired"] = r.checks_repaired;
  return out;
}
}  // namespace

namespace {
// Flexible int64 from Python: ints and numpy ints exactly, floats and numpy
// floats via llround (uUTC values are exact in float64). Clear TypeError
// otherwise. MUST be called while holding the GIL.
mef3io::si8 to_si8(nb::object o, const char* what) {
  mef3io::si8 v;
  if (nb::try_cast<mef3io::si8>(o, v)) return v;
  double d;
  if (nb::try_cast<double>(o, d)) return static_cast<mef3io::si8>(std::llround(d));
  throw nb::type_error((std::string(what) + " must be a number (int or float)").c_str());
}

std::optional<mef3io::si8> opt_si8(nb::object o, const char* what) {
  if (o.is_none()) return std::nullopt;
  return to_si8(o, what);
}
}  // namespace

namespace {
// None when the file carries no provenance; otherwise a dict. A code this
// build does not know (written by a newer mef3io) is reported as
// "unknown(<code>)" rather than dropped.
nb::object provenance_dict(const mef3io::fmt::provenance::Provenance& p) {
  namespace pv = mef3io::fmt::provenance;
  if (!p.present) return nb::none();
  auto name = [](unsigned code) {
    const char* n = pv::operation_name(static_cast<mef3io::ui1>(code));
    return n ? std::string(n) : "unknown(" + std::to_string(code) + ")";
  };
  nb::dict d;
  d["layout_version"] = p.layout_version;
  d["created_by"] = p.created_by;
  d["last_modified_by"] = p.last_modified_by;
  d["last_operation"] = name(p.last_operation);
  nb::list ops;
  for (unsigned b = 0; b < 32; ++b)
    if ((p.operations_mask >> b) & 1u) ops.append(name(b));
  d["operations"] = ops;
  d["modification_count"] = p.modification_count;
  return d;
}
}  // namespace

namespace {
// Record text comes from foreign writers and is not guaranteed UTF-8; one
// bad byte must not make the whole records() call raise, so decode leniently.
nb::object record_str(const std::string& s) {
  return nb::steal(PyUnicode_DecodeUTF8(s.data(), static_cast<Py_ssize_t>(s.size()), "replace"));
}

mef3io::Session::RecordLevels record_levels(nb::object level) {
  using L = mef3io::Session::RecordLevels;
  if (level.is_none()) return L::Default;
  const std::string s = nb::cast<std::string>(level);
  if (s == "session") return L::Session;
  if (s == "channel") return L::Channel;
  if (s == "segment") return L::Segment;
  if (s == "all") return L::All;
  throw std::invalid_argument(
      "level must be None, 'session', 'channel', 'segment' or 'all'; got '" + s + "'");
}

// One record as a dict: common keys, then the body fields of its type in
// record_fields' stable order (None where the body does not carry one).
nb::dict record_to_dict(const mef3io::Record& rec) {
  nb::dict d;
  d["type"] = record_str(rec.type);  // 4 raw bytes from the file: may not be UTF-8
  d["time"] = rec.time;
  d["level"] = mef3io::record_level_name(rec.level);
  d["channel"] = rec.level == mef3io::RecordLevel::Session
                     ? nb::object(nb::none())
                     : nb::object(nb::str(rec.channel.c_str()));
  d["segment"] =
      rec.level == mef3io::RecordLevel::Segment ? nb::object(nb::int_(rec.segment)) : nb::none();
  d["version_major"] = rec.version_major;
  d["version_minor"] = rec.version_minor;
  for (const auto& f : mef3io::record_fields(rec)) {
    nb::object v = nb::none();
    if (f.value) {
      v = std::visit(
          [](const auto& x) -> nb::object {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, std::string>) {
              return record_str(x);
            } else if constexpr (std::is_same_v<T, std::vector<mef3io::si4>>) {
              nb::list l;
              for (auto s : x) l.append(s);
              return l;
            } else if constexpr (std::is_same_v<T, std::vector<mef3io::SeizureChannel>>) {
              nb::list l;
              for (const auto& c : x) {
                nb::dict cd;
                cd["name"] = record_str(c.name);
                cd["onset"] = c.onset;
                cd["offset"] = c.offset;
                l.append(cd);
              }
              return l;
            } else {
              return nb::cast(x);
            }
          },
          *f.value);
    }
    d[f.name.c_str()] = v;
  }
  return d;
}
}  // namespace

namespace {
template <class T>
nb::object opt_obj(const std::optional<T>& v) {
  return v ? nb::cast(*v) : nb::none();
}

nb::object opt_str(const std::string& s) {
  return s.empty() ? nb::object(nb::none()) : record_str(s);
}

// inspect_session's result as plain nested dicts/lists: the Python layer
// (mef3io/_inspect.py) turns them into dataclasses, and a helper process can
// ship them as JSON unchanged.
nb::dict summary_to_dict(const mef3io::SessionSummary& s) {
  nb::dict d;
  d["name"] = record_str(s.name);
  d["path"] = s.path;
  d["layout"] = s.layout;
  d["total_bytes"] = s.total_bytes;
  d["archive_bytes"] = opt_obj(s.archive_bytes);
  d["index_read"] = s.index_read;
  d["offset_assumed_zero"] = s.offset_assumed_zero;
  d["legacy_password_mapping"] = s.legacy_password_mapping;

  nb::dict enc;
  enc["encrypted"] = s.encryption.encrypted;
  enc["access"] = s.encryption.access;
  enc["section2_level"] = s.encryption.section2_level;
  enc["section3_level"] = s.encryption.section3_level;
  enc["section2_locked"] = s.encryption.section2_locked;
  enc["section3_locked"] = s.encryption.section3_locked;
  enc["record_bodies_locked"] = s.encryption.record_bodies_locked;
  d["encryption"] = enc;

  nb::dict to;
  to["status"] = s.time_offset.status;
  to["segments_with_offset"] = s.time_offset.segments_with_offset;
  to["segments_locked"] = s.time_offset.segments_locked;
  to["segments_unknown"] = s.time_offset.segments_unknown;
  to["date_recoverable"] = s.time_offset.date_recoverable;
  to["recoverable_by"] = opt_str(s.time_offset.recoverable_by);
  to["ambiguous"] = s.time_offset.ambiguous;
  to["value"] = opt_obj(s.time_offset.value);
  to["interpretation"] = s.time_offset.interpretation;
  d["time_offset"] = to;

  d["gmt_offset"] = opt_obj(s.gmt_offset);
  d["dst_start_time"] = opt_obj(s.dst_start_time);
  d["dst_end_time"] = opt_obj(s.dst_end_time);
  d["start_time"] = opt_obj(s.start_time);
  d["end_time"] = opt_obj(s.end_time);
  d["records_start_time"] = opt_obj(s.records_start_time);
  d["records_end_time"] = opt_obj(s.records_end_time);
  d["first_time"] = opt_obj(s.first_time);
  d["last_time"] = opt_obj(s.last_time);

  nb::list chans;
  for (const auto& c : s.channels) {
    nb::dict cd;
    cd["name"] = record_str(c.name);
    cd["sampling_frequency"] = opt_obj(c.sampling_frequency);
    cd["units_conversion_factor"] = opt_obj(c.units_conversion_factor);
    cd["units_description"] =
        c.units_description ? record_str(*c.units_description) : nb::object(nb::none());
    cd["channel_description"] =
        c.channel_description ? record_str(*c.channel_description) : nb::object(nb::none());
    cd["start_time"] = opt_obj(c.start_time);
    cd["end_time"] = opt_obj(c.end_time);
    cd["number_of_samples"] = opt_obj(c.number_of_samples);
    cd["gaps_complete"] = c.gaps_complete;
    cd["gaps_certain"] = c.gaps_certain;
    cd["offset_assumed_zero"] = c.offset_assumed_zero;
    cd["bytes"] = c.bytes;
    cd["records"] = c.records;
    nb::list gaps;
    for (const auto& g : c.gaps) gaps.append(nb::make_tuple(g.start, g.end));
    cd["gaps"] = gaps;
    nb::list segs;
    for (const auto& g : c.segments) {
      nb::dict sd;
      sd["segment"] = g.segment_number;
      sd["path"] = record_str(g.path);
      sd["readable"] = g.readable;
      sd["problem"] = opt_str(g.problem);
      sd["section2_available"] = g.section2_available;
      sd["section3_available"] = g.section3_available;
      sd["start_time"] = opt_obj(g.start_time);
      sd["end_time"] = opt_obj(g.end_time);
      sd["start_sample"] = opt_obj(g.start_sample);
      sd["number_of_samples"] = opt_obj(g.number_of_samples);
      sd["number_of_blocks"] = opt_obj(g.number_of_blocks);
      sd["number_of_discontinuities"] = opt_obj(g.number_of_discontinuities);
      sd["time_offset"] = g.time_offset;
      sd["recording_time_offset"] = opt_obj(g.recording_time_offset);
      sd["offset_assumed_zero"] = g.offset_assumed_zero;
      sd["tmet_bytes"] = g.tmet_bytes;
      sd["tidx_bytes"] = g.tidx_bytes;
      sd["tdat_bytes"] = g.tdat_bytes;
      sd["record_bytes"] = g.record_bytes;
      sd["has_records"] = g.has_records;
      sd["provenance"] = provenance_dict(g.provenance);
      sd["unset_declarations"] = g.unset_declarations;
      segs.append(sd);
    }
    cd["segments"] = segs;
    chans.append(cd);
  }
  d["channels"] = chans;
  nb::list rec_only, video;
  for (const auto& n : s.record_only_channels) rec_only.append(record_str(n));
  for (const auto& n : s.video_channels) video.append(record_str(n));
  d["record_only_channels"] = rec_only;
  d["video_channels"] = video;
  nb::list incomplete;
  for (const auto& n : s.incomplete_channels) incomplete.append(record_str(n));
  d["incomplete_channels"] = incomplete;

  nb::list recs;
  for (const auto& r : s.records) {
    nb::dict rd;
    rd["type"] = record_str(r.type);
    rd["level"] = mef3io::record_level_name(r.level);
    rd["channel"] = r.level == mef3io::RecordLevel::Session ? nb::object(nb::none())
                                                            : record_str(r.channel);
    rd["segment"] = r.level == mef3io::RecordLevel::Segment ? nb::object(nb::int_(r.segment))
                                                            : nb::object(nb::none());
    rd["time"] = opt_obj(r.time);
    rd["times_known"] = r.times_known;
    rd["offset_assumed_zero"] = r.offset_assumed_zero;
    rd["duration"] = opt_obj(r.duration);
    rd["first_time"] = opt_obj(r.first_time);
    rd["last_time"] = opt_obj(r.last_time);
    rd["text_bytes"] = r.text_bytes;
    rd["body_locked"] = r.body_locked;
    if (r.full) {
      nb::dict fd = record_to_dict(*r.full);
      // A time inspect could not resolve is NO_ENTRY in the record: None here.
      if (r.full->time == mef3io::fmt::UUTC_NO_ENTRY) fd["time"] = nb::none();
      // Seiz per-channel onset/offset: -1 is meflib's NO_ENTRY there; when the
      // times are unknown, say None like every other time.
      if (!r.times_known && fd.contains("channels") && !fd["channels"].is_none())
        for (auto c : nb::borrow<nb::list>(fd["channels"])) {
          nb::dict cd = nb::borrow<nb::dict>(c);
          cd["onset"] = nb::none();
          cd["offset"] = nb::none();
        }
      rd["fields"] = fd;
    } else {
      rd["fields"] = nb::none();
    }
    recs.append(rd);
  }
  d["records"] = recs;

  nb::list ids;
  for (const auto& f : s.identity) {
    nb::dict fd;
    fd["name"] = f.name;
    fd["location"] = f.location;
    fd["occurrences"] = f.occurrences;
    fd["locked"] = f.locked;
    fd["max_length"] = f.max_length;
    fd["distinct"] = f.distinct;
    nb::list vals;
    for (const auto& v : f.values) vals.append(record_str(v));
    fd["values"] = vals;
    ids.append(fd);
  }
  d["identity"] = ids;

  nb::list warns;
  for (const auto& w : s.warnings) {
    nb::dict wd;
    wd["code"] = w.code;
    wd["message"] = record_str(w.message);
    wd["blocks_anonymize"] = w.blocks_anonymize;
    warns.append(wd);
  }
  d["warnings"] = warns;

  nb::list probs;
  for (const auto& p : s.problems) {
    nb::dict e;
    e["channel"] = record_str(p.channel);
    e["segment_number"] = p.segment_number;
    e["segment"] = record_str(p.segment);
    e["reason"] = record_str(p.reason);
    e["kind"] = p.kind;
    probs.append(e);
  }
  d["problems"] = probs;

  nb::list decl;
  for (const auto& i : s.declaration_issues) {
    nb::dict e;
    e["channel"] = record_str(i.channel);
    e["segment"] = i.segment_number;
    e["field"] = i.field;
    decl.append(e);
  }
  d["declaration_issues"] = decl;
  return d;
}
}  // namespace

namespace {
// The cancel/progress hooks of ONE call (#69), bridged to Python.
//
// Built and destroyed with the GIL held, on the binding's stack; the work runs
// with the GIL released. The std::function handed to the core captures only
// `this`, so the core may copy it freely without touching a Python refcount.
//
// The callback re-acquires the GIL just for the call. It may be invoked from a
// decode worker thread — the core serialises the calls and never waits on one
// (see detail::ProgressReporter), so a callback blocked on the GIL cannot
// deadlock the work. If it raises, the Python exception is kept, the operation
// is aborted like a cancellation (Cancelled is not a std::exception, so no
// per-segment containment in the core can swallow it), and the ORIGINAL
// exception is re-raised to the caller once the GIL is back.
struct PyHooks {
  nb::object progress;
  std::optional<nb::python_error> error;
  mef3io::Control control;

  PyHooks(const mef3io::CancelToken* token, nb::object progress_fn)
      : progress(std::move(progress_fn)) {
    control.cancel = token;
    if (!progress.is_none()) {
      if (!PyCallable_Check(progress.ptr()))
        throw nb::type_error("progress must be a callable taking (done, total)");
      control.progress = [this](std::uint64_t done, std::uint64_t total) { call(done, total); };
    }
  }
  PyHooks(const PyHooks&) = delete;
  PyHooks& operator=(const PyHooks&) = delete;

  void call(std::uint64_t done, std::uint64_t total) {
    nb::gil_scoped_acquire gil;
    if (error) throw mef3io::Cancelled("progress callback raised");
    try {
      progress(done, total);
    } catch (nb::python_error& e) {
      error.emplace(std::move(e));
      throw mef3io::Cancelled("progress callback raised");
    }
  }

  /// Run `fn(control)` without the GIL; translate a callback's exception back.
  template <class F>
  auto run(F&& fn) {
    try {
      nb::gil_scoped_release release;
      return fn(control);
    } catch (const mef3io::Cancelled&) {
      if (error) {
        nb::python_error e(std::move(*error));
        error.reset();
        throw e;
      }
      throw;
    }
  }
};
}  // namespace

NB_MODULE(_mef3io, m) {
  m.attr("__version__") = mef3io::version();
#ifdef MEF3IO_BUILD_ID
  m.attr("__build_id__") = MEF3IO_BUILD_ID;  // see the top-level CMakeLists.txt
#else
  m.attr("__build_id__") = "";
#endif
  m.doc() = "mef3io C++ backend (nanobind extension)";
  m.attr("__mef_version_major__") = mef3io::fmt::MEF_VERSION_MAJOR;
  m.attr("__mef_version_minor__") = mef3io::fmt::MEF_VERSION_MINOR;

  // Distinct Python types for the core exceptions. Every one of these derives
  // from MefError : std::runtime_error, so without this they all reached
  // Python as a bare RuntimeError and a caller could only tell "wrong
  // password" from "corrupt file" by matching on the message text. Registered
  // most-derived FIRST: nanobind tries translators in reverse registration
  // order, so the base must be registered before its subclasses for the
  // subclasses to win.
  // Rooted at RuntimeError, NOT at Exception: every release so far surfaced
  // these as a bare RuntimeError, so code in the field catches that. Making
  // MefError a subclass of it means existing `except RuntimeError` keeps
  // working unchanged while new code can catch the specific type.
  static nb::exception<mef3io::MefError> mef_error(m, "MefError", PyExc_RuntimeError);
  static nb::exception<mef3io::FormatError> format_error(m, "FormatError", mef_error.ptr());
  static nb::exception<mef3io::CrcError> crc_error(m, "CrcError", mef_error.ptr());
  static nb::exception<mef3io::PasswordError> password_error(m, "PasswordError", mef_error.ptr());
  static nb::exception<mef3io::IoError> io_error(m, "IoError", mef_error.ptr());
  static nb::exception<mef3io::WriteConflictError> write_conflict_error(
      m, "WriteConflictError", mef_error.ptr());
  // NOT under MefError/RuntimeError: a cancellation is not a failure, and code
  // that catches MefError to report a damaged file must not catch it.
  static nb::exception<mef3io::Cancelled> cancelled_error(m, "Cancelled", PyExc_Exception);

  nb::class_<mef3io::CancelToken>(m, "CancelToken")
      .def(nb::init<>())
      .def("cancel", &mef3io::CancelToken::cancel, "Request cancellation (any thread).")
      .def_prop_ro("cancelled", &mef3io::CancelToken::cancelled);

  // Both move the whole session through the filesystem — seconds on a large
  // one — and touch no Python object while doing it, so they release the GIL
  // rather than freezing every other thread in the process.
  m.def(
      "archive_session",
      [](const std::string& session_dir, const std::string& tar_path, bool overwrite,
         const mef3io::CancelToken* cancel, nb::object progress) {
        PyHooks hooks(cancel, std::move(progress));
        return hooks.run([&](const mef3io::Control& ctl) {
          return mef3io::archive_session(session_dir, tar_path, overwrite, ctl);
        });
      },
      nb::arg("session_dir"), nb::arg("tar_path") = "", nb::arg("overwrite") = false,
      nb::arg("cancel").none() = nb::none(), nb::arg("progress") = nb::none(),
      "Pack a session directory into a single uncompressed tar archive and "
      "return the archive path.");
  m.def(
      "extract_session",
      [](const std::string& tar_path, const std::string& dest_dir, bool overwrite,
         const mef3io::CancelToken* cancel, nb::object progress) {
        PyHooks hooks(cancel, std::move(progress));
        return hooks.run([&](const mef3io::Control& ctl) {
          return mef3io::extract_session(tar_path, dest_dir, overwrite, ctl);
        });
      },
      nb::arg("tar_path"), nb::arg("dest_dir") = "", nb::arg("overwrite") = false,
      nb::arg("cancel").none() = nb::none(), nb::arg("progress") = nb::none(),
      "Unpack a session archive back into a directory and return its path.");

  m.def(
      "recover_session",
      [](const std::string& path, bool apply, bool backup, const std::string& password,
         const mef3io::CancelToken* cancel, nb::object progress) {
        PyHooks hooks(cancel, std::move(progress));
        mef3io::RecoveryReport r = hooks.run([&](const mef3io::Control& ctl) {
          return mef3io::recover_session(path, apply, backup, password, ctl);
        });
        nb::list segs;
        for (const auto& s : r.segments) {
          nb::dict d;
          d["channel"] = s.channel;
          d["segment"] = s.segment_number;
          d["path"] = s.path;
          d["blocks_before"] = s.blocks_before;
          d["blocks_after"] = s.blocks_after;
          d["blocks_recovered"] = s.blocks_recovered;
          d["blocks_dropped"] = s.blocks_dropped;
          d["tdat_bytes_dropped"] = s.tdat_bytes_dropped;
          d["action"] = s.action;
          segs.append(d);
        }
        nb::list skipped;
        for (const auto& s : r.skipped) skipped.append(s);
        nb::dict out;
        out["segments"] = segs;
        out["skipped"] = skipped;
        out["segments_examined"] = r.segments_examined;
        out["applied"] = r.applied;
        out["backup_root"] = r.backup_root;
        nb::list notes;
        for (const auto& n : r.notes) notes.append(n);
        out["notes"] = notes;
        return out;
      },
      nb::arg("path"), nb::arg("apply") = false, nb::arg("backup") = true,
      nb::arg("password") = "", nb::arg("cancel").none() = nb::none(),
      nb::arg("progress") = nb::none(),
      "Make each segment's block index and data agree after an interrupted "
      "write. Dry run unless apply=True.");

  // --- inspect (#67): everything about a session without its samples ---
  m.def(
      "inspect_session",
      [](const std::string& path, const std::string& password, bool strict,
         bool include_values, bool read_index, const mef3io::CancelToken* cancel,
         nb::object progress) {
        mef3io::InspectOptions opts;
        opts.password = password;
        opts.strict = strict;
        opts.include_values = include_values;
        opts.read_index = read_index;
        // File I/O over every segment: runs without the GIL (PyHooks::run).
        PyHooks hooks(cancel, std::move(progress));
        mef3io::SessionSummary s = hooks.run([&](const mef3io::Control& ctl) {
          return mef3io::inspect_session(path, opts, ctl);
        });
        return summary_to_dict(s);
      },
      nb::arg("path"), nb::arg("password") = "", nb::arg("strict") = true,
      nb::arg("include_values") = false, nb::arg("read_index") = false,
      nb::arg("cancel").none() = nb::none(), nb::arg("progress") = nb::none(),
      "Describe a session without reading sample data. See mef3io.inspect.");

  // --- validation / repair (dicts in, dicts out; mef3io.validate wraps them) ---
  m.def(
      "validation_checks",
      [] {
        nb::list out;
        for (const auto& c : mef3io::checks()) {
          nb::dict d;
          d["id"] = c.id;
          d["title"] = c.title;
          d["description"] = c.description;
          d["severity"] = mef3io::severity_name(c.severity);
          d["repairable"] = c.repairable;
          out.append(d);
        }
        return out;
      },
      "The validation check registry, in the order checks run.");

  m.def(
      "validate_session",
      [](const std::string& path, const std::string& password,
         std::vector<std::string> channels, std::vector<int> segments,
         std::vector<std::string> check_ids, bool exact_difference_bytes,
         const mef3io::CancelToken* cancel, nb::object progress) {
        mef3io::ValidateOptions opts;
        opts.password = password;
        opts.channels = std::move(channels);
        opts.segments = std::move(segments);
        opts.check_ids = std::move(check_ids);
        opts.exact_difference_bytes = exact_difference_bytes;
        // Unbounded blocking I/O: with exact_difference_bytes it reads a header
        // per RED block. Holding the GIL would freeze the whole interpreter.
        PyHooks hooks(cancel, std::move(progress));
        mef3io::Report r = hooks.run([&](const mef3io::Control& ctl) {
          return mef3io::validate_session(path, opts, ctl);
        });
        return report_to_dict(r);
      },
      nb::arg("path"), nb::arg("password") = "",
      nb::arg("channels") = std::vector<std::string>{},
      nb::arg("segments") = std::vector<int>{},
      nb::arg("check_ids") = std::vector<std::string>{},
      nb::arg("exact_difference_bytes") = true, nb::arg("cancel").none() = nb::none(),
      nb::arg("progress") = nb::none(),
      "Check a session's declarations against its data. Reads only.");

  m.def(
      "repair_session",
      [](const std::string& path, std::vector<std::string> repair_check_ids,
         const std::string& password, std::vector<std::string> channels,
         std::vector<int> segments, std::vector<std::string> check_ids,
         bool exact_difference_bytes, bool backup, const mef3io::CancelToken* cancel,
         nb::object progress) {
        mef3io::ValidateOptions opts;
        opts.password = password;
        opts.channels = channels;
        opts.segments = segments;
        opts.check_ids = std::move(check_ids);
        opts.exact_difference_bytes = exact_difference_bytes;
        mef3io::RepairSelection sel;
        sel.check_ids = std::move(repair_check_ids);
        sel.channels = std::move(channels);
        sel.segments = std::move(segments);
        sel.backup = backup;
        PyHooks hooks(cancel, std::move(progress));
        mef3io::Report r = hooks.run([&](const mef3io::Control& ctl) {
          return mef3io::repair_session(path, sel, opts, ctl);
        });
        return report_to_dict(r);
      },
      nb::arg("path"), nb::arg("repair_check_ids"), nb::arg("password") = "",
      nb::arg("channels") = std::vector<std::string>{},
      nb::arg("segments") = std::vector<int>{},
      nb::arg("check_ids") = std::vector<std::string>{},
      nb::arg("exact_difference_bytes") = true, nb::arg("backup") = true,
      nb::arg("cancel").none() = nb::none(), nb::arg("progress") = nb::none(),
      "Validate, then write back only the selected repairs. Never implicit.");

  // Exposed for parity tests against the Python oracles.
  m.def(
      "crc32", [](nb::bytes data) { return mef3io::crc::calculate(as_span(data)); },
      nb::arg("data"), "CRC-32 (Koopman) over the given bytes.");

  m.def(
      "sha256",
      [](nb::bytes data) {
        auto h = mef3io::crypto::sha256(as_span(data));
        return to_bytes(h);
      },
      nb::arg("data"));

  m.def(
      "aes128_ecb_encrypt",
      [](nb::bytes data, nb::bytes key) {
        return to_bytes(mef3io::crypto::aes128_ecb_encrypt(as_span(data), as_span(key)));
      },
      nb::arg("data"), nb::arg("key"));

  m.def(
      "aes128_ecb_decrypt",
      [](nb::bytes data, nb::bytes key) {
        return to_bytes(mef3io::crypto::aes128_ecb_decrypt(as_span(data), as_span(key)));
      },
      nb::arg("data"), nb::arg("key"));

  m.def("extract_password_bytes", [](const std::string& p) {
    auto b = mef3io::crypto::extract_password_bytes(p);
    return to_bytes(b);
  });
  m.def("extract_password_bytes_legacy", [](const std::string& p) {
    auto b = mef3io::crypto::extract_password_bytes_legacy(p);
    return to_bytes(b);
  });
  m.def("password_interop_issue", &mef3io::crypto::password_interop_issue, nb::arg("password"),
        "Why a password will not behave as expected in other MEF readers, or \"\".");

  // validate_password reads 16 bytes from each validation field; a shorter
  // Python bytes object would be an out-of-bounds read, so these test helpers
  // require exactly 16 bytes per field ALWAYS (even for an empty password or
  // level 0, where nothing would be read) and raise ValueError otherwise.
  auto checked_keys = [](const std::string& password, nb::bytes l1, nb::bytes l2) {
    constexpr std::size_t n = mef3io::fmt::PASSWORD_VALIDATION_FIELD_BYTES;
    if (l1.size() != n || l2.size() != n)
      throw nb::value_error("password validation fields must be 16 bytes each");
    return mef3io::crypto::validate_password(password, as_span(l1), as_span(l2));
  };

  m.def("validate_password", [checked_keys](const std::string& password, nb::bytes l1, nb::bytes l2) {
    auto keys = checked_keys(password, l1, l2);
    return keys.access_level;
  });

  // Parse a .tmet image -> dict of the fields the reader cares about. Used by
  // the P1 test to validate struct layout + decryption against golden files.
  m.def(
      "parse_tmet",
      [](nb::bytes tmet, const std::string& password) {
        auto md = mef3io::load_time_series_metadata(as_span(tmet), password);
        nb::dict d;
        d["channel_name"] = md.universal_header.channel_name;
        d["session_name"] = md.universal_header.session_name;
        d["segment_number"] = md.universal_header.segment_number;
        d["start_time"] = md.universal_header.start_time;
        d["end_time"] = md.universal_header.end_time;
        d["access_level"] = md.access_level;
        d["section3_available"] = md.section3_available;
        d["sampling_frequency"] = md.section2.sampling_frequency;
        d["units_conversion_factor"] = md.section2.units_conversion_factor;
        d["units_description"] = md.section2.units_description;
        d["number_of_samples"] = md.section2.number_of_samples;
        d["number_of_blocks"] = md.section2.number_of_blocks;
        d["start_sample"] = md.section2.start_sample;
        d["recording_time_offset"] = md.section3.recording_time_offset;
        d["gmt_offset"] = md.section3.gmt_offset;
        return d;
      },
      nb::arg("tmet"), nb::arg("password") = "");

  // Round-trip the universal header (parse -> serialize) and return the 1024
  // re-serialized bytes; the P1 test checks they equal the original, which
  // validates the serialize-side field offsets ahead of the write phase.
  m.def("roundtrip_universal_header", [](nb::bytes file_bytes) {
    auto uh = mef3io::fmt::UniversalHeader::parse(as_span(file_bytes));
    std::vector<mef3io::ui1> out(mef3io::fmt::UNIVERSAL_HEADER_BYTES);
    uh.serialize(out);
    return to_bytes(out);
  });

  // RED encode/decode round-trip helper (P4 validation). Takes int32 samples,
  // returns the decoded int32 samples after a full encode->decode cycle.
  m.def("red_roundtrip", [](nb::ndarray<const mef3io::si4, nb::ndim<1>, nb::c_contig> samples,
                            bool discontinuity) {
    std::span<const mef3io::si4> s(samples.data(), samples.size());
    mef3io::si4 mn = 0, mx = 0;
    auto block = mef3io::red::encode_block(s, 12345, discontinuity, mn, mx);
    auto dec = mef3io::red::decode_block(block);
    nb::dict d;
    d["samples"] = vec_to_numpy(std::move(dec.samples));
    d["min"] = mn;
    d["max"] = mx;
    d["block_bytes"] = static_cast<mef3io::si8>(block.size());
    d["discontinuity"] = dec.discontinuity;
    return d;
  });

  // Single-block RED encode/decode with block encryption (#80 interop tests).
  // Keys come from `password` checked against the two universal-header
  // validation fields (16 B each), exactly as the reader derives them.
  m.def(
      "red_encode_block",
      [checked_keys](nb::ndarray<const mef3io::si4, nb::ndim<1>, nb::c_contig> samples,
                     mef3io::si8 start_time, bool discontinuity, int encryption_level,
                     const std::string& password, nb::bytes l1_field, nb::bytes l2_field) {
        std::span<const mef3io::si4> s(samples.data(), samples.size());
        auto keys = checked_keys(password, l1_field, l2_field);
        mef3io::si4 mn = 0, mx = 0;
        return to_bytes(mef3io::red::encode_block(s, start_time, discontinuity, mn, mx,
                                                  encryption_level, keys));
      },
      nb::arg("samples"), nb::arg("start_time"), nb::arg("discontinuity"),
      nb::arg("encryption_level"), nb::arg("password"), nb::arg("l1_field"), nb::arg("l2_field"));

  m.def(
      "red_decode_block",
      [checked_keys](nb::bytes block, const std::string& password, nb::bytes l1_field,
                     nb::bytes l2_field) {
        auto keys = checked_keys(password, l1_field, l2_field);
        auto dec = mef3io::red::decode_block(as_span(block), keys);
        nb::dict d;
        d["samples"] = vec_to_numpy(std::move(dec.samples));
        d["start_time"] = dec.start_time;
        d["discontinuity"] = dec.discontinuity;
        return d;
      },
      nb::arg("block"), nb::arg("password"), nb::arg("l1_field"), nb::arg("l2_field"));

  // Write one single-segment time-series channel (P4 validation helper).
  // Splits `samples` into RED blocks of `block_len`; block 0 is marked as a
  // discontinuity (meflib convention for a segment's first block).
  m.def(
      "write_test_segment",
      [](const std::string& mefd_path, const std::string& channel, const std::string& session_name,
         nb::ndarray<const mef3io::si4, nb::ndim<1>, nb::c_contig> samples, mef3io::si8 start_uutc,
         double fs, double ufact, const std::string& units, mef3io::si8 block_len,
         const std::string& password1, const std::string& password2) {
        namespace fs_ns = std::filesystem;
        std::string seg = channel + "-000000";
        auto seg_dir = fs_ns::path(mefd_path) / (channel + ".timd") / (seg + ".segd");
        fs_ns::create_directories(seg_dir);

        std::span<const mef3io::si4> all(samples.data(), samples.size());
        std::vector<mef3io::BlockSpec> blocks;
        mef3io::si8 n = static_cast<mef3io::si8>(all.size());
        for (mef3io::si8 s = 0; s < n; s += block_len) {
          mef3io::si8 len = std::min(block_len, n - s);
          mef3io::BlockSpec b;
          b.samples.assign(all.begin() + s, all.begin() + s + len);
          b.start_sample = s;
          b.start_uutc = start_uutc + static_cast<mef3io::si8>(std::llround(s / fs * 1e6));
          b.discontinuity = (s == 0);
          blocks.push_back(std::move(b));
        }

        mef3io::SegmentSpec spec;
        spec.session_name = session_name;
        spec.channel_name = channel;
        spec.segment_number = 0;
        spec.sampling_frequency = fs;
        spec.units_conversion_factor = ufact;
        spec.units_description = units;
        spec.gmt_offset = -21600;
        spec.password_1 = password1;
        spec.password_2 = password2;
        return mef3io::write_time_series_segment(seg_dir.string(), spec, blocks);
      },
      nb::arg("mefd_path"), nb::arg("channel"), nb::arg("session_name"), nb::arg("samples"),
      nb::arg("start_uutc"), nb::arg("fs"), nb::arg("ufact"), nb::arg("units") = "uV",
      nb::arg("block_len") = 2560, nb::arg("password1") = "", nb::arg("password2") = "");

  // --- Session (P2 low-level reads) ---
  nb::class_<mef3io::Session>(m, "Session")
      .def(nb::init<const std::string&, std::string>(), nb::arg("path"), nb::arg("password") = "")
      .def_prop_ro("channels", &mef3io::Session::channels)
      .def("channel_info",
           [](mef3io::Session& s, const std::string& name) {
             const auto& ci = s.channel_info(name);
             nb::dict d;
             d["name"] = ci.name;
             d["sampling_frequency"] = ci.sampling_frequency;
             d["units_conversion_factor"] = ci.units_conversion_factor;
             d["units_description"] = ci.units_description;
             d["start_time"] = ci.start_time;
             d["end_time"] = ci.end_time;
             d["number_of_samples"] = ci.number_of_samples;
             d["recording_time_offset"] = ci.recording_time_offset;
             d["n_segments"] = ci.n_segments;
             d["session_description"] = ci.session_description;
             d["channel_description"] = ci.channel_description;
             d["reference_description"] = ci.reference_description;
             d["acquisition_channel_number"] = ci.acquisition_channel_number;
             d["low_frequency_filter"] = ci.low_frequency_filter;
             d["high_frequency_filter"] = ci.high_frequency_filter;
             d["notch_filter"] = ci.notch_filter;
             d["line_frequency"] = ci.line_frequency;
             d["section3_available"] = ci.section3_available;
             if (ci.section3_available) {
               d["gmt_offset"] = ci.gmt_offset;
               d["subject_name_1"] = ci.subject_name_1;
               d["subject_name_2"] = ci.subject_name_2;
               d["subject_id"] = ci.subject_id;
               d["recording_location"] = ci.recording_location;
             } else {
               d["gmt_offset"] = nb::none();
               d["subject_name_1"] = nb::none();
               d["subject_name_2"] = nb::none();
               d["subject_id"] = nb::none();
               d["recording_location"] = nb::none();
             }
             return d;
           })
      .def(
          "set_index_cache_bytes", &mef3io::Session::set_index_cache_bytes, nb::arg("n"))
      .def("index_cache_bytes", &mef3io::Session::index_cache_bytes)
      .def(
          "read_runs",
          [](mef3io::Session& s, const std::string& channel, nb::object t0, nb::object t1) {
            // Convert the Python timestamps BEFORE releasing, then decode
            // without the GIL: this is a full parallel decode and was holding
            // the interpreter for its entire duration.
            const auto a = opt_si8(t0, "t0");
            const auto b = opt_si8(t1, "t1");
            std::vector<mef3io::DataRun> runs;
            {
              nb::gil_scoped_release rel;
              runs = s.read_runs(channel, a, b);
            }
            nb::list out;
            for (auto& r : runs) {
              nb::dict d;
              d["start_uutc"] = r.start_uutc;
              d["start_sample"] = r.start_sample;
              d["samples"] = vec_to_numpy(std::move(r.samples));
              out.append(d);
            }
            return out;
          },
          nb::arg("channel"), nb::arg("t0") = nb::none(), nb::arg("t1") = nb::none());

  // --- SessionWriter (P5 high-level write) ---
  auto summary_dict = [](const mef3io::WriteSummary& s) {
    nb::dict d;
    d["samples_written"] = s.samples_written;
    d["blocks"] = s.blocks;
    d["gaps_skipped"] = s.gaps_skipped;
    d["segment"] = s.segment;
    return d;
  };
  nb::class_<mef3io::SessionWriter>(m, "SessionWriter")
      .def(nb::init<const std::string&, bool, std::string, std::string>(), nb::arg("path"),
           nb::arg("overwrite") = false, nb::arg("password1") = "", nb::arg("password2") = "")
      .def("set_block_length", &mef3io::SessionWriter::set_block_length)
      .def("set_units", &mef3io::SessionWriter::set_units)
      .def_prop_ro("unreadable_metadata",
                   [](const mef3io::SessionWriter& w) { return w.unreadable_metadata(); })
      .def_prop_ro("legacy_password_segments",
                   [](const mef3io::SessionWriter& w) { return w.legacy_password_segments(); })
      .def("set_threads", &mef3io::SessionWriter::set_threads)
          .def("set_durable", &mef3io::SessionWriter::set_durable, nb::arg("durable"))
      .def(
          "set_metadata",
          [](mef3io::SessionWriter& w, nb::dict d) {
            mef3io::SessionMetadata m;
            auto get_s = [&](const char* k, std::string& dst) {
              if (d.contains(k) && !d[k].is_none()) dst = nb::cast<std::string>(d[k]);
            };
            auto get_f = [&](const char* k, mef3io::sf8& dst) {
              if (d.contains(k) && !d[k].is_none()) dst = nb::cast<mef3io::sf8>(d[k]);
            };
            get_s("session_description", m.session_description);
            get_s("channel_description", m.channel_description);
            get_s("reference_description", m.reference_description);
            if (d.contains("acquisition_channel_number") &&
                !d["acquisition_channel_number"].is_none())
              m.acquisition_channel_number = to_si8(d["acquisition_channel_number"],
                                                    "acquisition_channel_number");
            get_f("low_frequency_filter", m.low_frequency_filter);
            get_f("high_frequency_filter", m.high_frequency_filter);
            get_f("notch_filter", m.notch_filter);
            get_f("line_frequency", m.line_frequency);
            get_s("subject_name_1", m.subject_name_1);
            get_s("subject_name_2", m.subject_name_2);
            get_s("subject_id", m.subject_id);
            get_s("recording_location", m.recording_location);
            if (d.contains("gmt_offset") && !d["gmt_offset"].is_none())
              m.gmt_offset = static_cast<mef3io::si4>(to_si8(d["gmt_offset"], "gmt_offset"));
            w.set_metadata(m);
          },
          nb::arg("metadata"))
      .def(
          "write_float",
          [summary_dict](mef3io::SessionWriter& w, const std::string& ch,
                         nb::ndarray<const mef3io::sf8, nb::ndim<1>, nb::c_contig> data,
                         nb::object start, double fs, int precision, bool new_segment) {
            std::span<const mef3io::sf8> s(data.data(), data.size());
            const mef3io::si8 start_us = to_si8(start, "start_uutc");
            mef3io::WriteSummary r;
            {
              nb::gil_scoped_release rel;
              r = w.write_float(ch, s, start_us, fs, precision, new_segment);
            }
            return summary_dict(r);
          },
          nb::arg("channel"), nb::arg("data"), nb::arg("start_uutc"), nb::arg("fs"),
          nb::arg("precision") = -1, nb::arg("new_segment") = false)
      .def(
          "write_int32",
          [summary_dict](mef3io::SessionWriter& w, const std::string& ch,
                         nb::ndarray<const mef3io::si4, nb::ndim<1>, nb::c_contig> data,
                         double ufact, nb::object start, double fs, nb::object valid,
                         bool new_segment) {
            std::span<const mef3io::si4> s(data.data(), data.size());
            // Copy the mask rather than pointing into the cast result. nb::cast
            // converts (convert=true), so a mask that is not already contiguous
            // uint8 — a bool array, float64, int64, a stride-2 slice — is
            // materialised into a NEW array owned only by the local `v`. `v`
            // dies at the end of this scope, so a span into it dangles, and the
            // writer then reads freed memory and lays the gaps out wrong with
            // no error at all. The copy is one byte per sample and happens once
            // per call, against an encode that is orders of magnitude dearer.
            std::vector<mef3io::ui1> vbuf;
            std::span<const mef3io::ui1> vspan;
            if (!valid.is_none()) {
              auto v = nb::cast<nb::ndarray<const mef3io::ui1, nb::ndim<1>, nb::c_contig>>(valid);
              vbuf.assign(v.data(), v.data() + v.size());
              vspan = std::span<const mef3io::ui1>(vbuf);
            }
            const mef3io::si8 start_us = to_si8(start, "start_uutc");
            mef3io::WriteSummary r;
            {
              // Match write_float: the encode is long and touches no Python.
              nb::gil_scoped_release rel;
              r = w.write_int32(ch, s, ufact, start_us, fs, vspan, new_segment);
            }
            return summary_dict(r);
          },
          nb::arg("channel"), nb::arg("data"), nb::arg("ufact"), nb::arg("start_uutc"),
          nb::arg("fs"), nb::arg("valid") = nb::none(), nb::arg("new_segment") = false)
      .def(
          "write_records",
          [](mef3io::SessionWriter& w, nb::object channel, nb::list records) {
            std::optional<std::string> ch;
            if (!channel.is_none()) ch = nb::cast<std::string>(channel);
            std::vector<mef3io::Record> recs;
            for (auto item : records) {
              nb::dict d = nb::cast<nb::dict>(item);
              mef3io::Record r;
              r.type = nb::cast<std::string>(d["type"]);
              r.time = to_si8(d["time"], "record time");
              if (d.contains("text")) {
                nb::object t = d["text"];
                if (!t.is_none()) r.text = nb::cast<std::string>(t);
              }
              if (d.contains("duration")) {
                nb::object dur = d["duration"];
                if (!dur.is_none()) r.duration = to_si8(dur, "record duration");
              }
              recs.push_back(std::move(r));
            }
            w.write_records(ch, recs);
          },
          nb::arg("channel").none(), nb::arg("records"));

  m.def("infer_precision", [](nb::ndarray<const mef3io::sf8, nb::ndim<1>, nb::c_contig> d) {
    return mef3io::infer_precision(std::span<const mef3io::sf8>(d.data(), d.size()));
  });

  // --- Reader (P3 high-level API) ---
  auto opt_time = [](nb::object o) { return opt_si8(o, "t0/t1"); };

  nb::class_<mef3io::Reader>(m, "Reader")
      .def(nb::init<const std::string&, std::string, int, bool>(), nb::arg("path"),
           nb::arg("password") = "", nb::arg("n_threads") = 0, nb::arg("strict") = true)
      .def("set_threads", &mef3io::Reader::set_threads)
      .def("problems",
           [](mef3io::Reader& r) {
             nb::list out;
             for (const auto& p : r.problems()) {
               nb::dict e;
               e["channel"] = p.channel;
               e["segment_number"] = p.segment_number;
               e["segment"] = p.segment;
               e["reason"] = p.reason;
               e["kind"] = p.kind;
               out.append(e);
             }
             return out;
           })
      .def_prop_ro("channels", &mef3io::Reader::channels)
      .def(
          "password_protected",
          [](mef3io::Reader& r) {
            for (const auto& ch : r.channels())
              if (r.info(ch).password_protected) return true;
            return false;
          },
          "Whether any segment's metadata is encrypted. The warm-start cache "
          "never snapshots such a session.")
      .def(
          "legacy_password_channels",
          [](mef3io::Reader& r) {
            std::vector<std::string> out;
            for (const auto& ch : r.channels())
              if (r.info(ch).legacy_password_mapping) out.push_back(ch);
            return out;
          },
          "Channels whose metadata the password opened only through the byte "
          "mapping mef3io used up to 1.2.0 (issue #84).")
      .def(
          "legacy_password_record_files",
          [](mef3io::Reader& r) { return r.legacy_password_record_files(); },
          "Record files read so far that the password opened only through the "
          "byte mapping mef3io used up to 1.2.0 (issue #84).")
      .def(
          "session_identity",
          [](mef3io::Reader& r) {
            return std::vector<std::string>{r.session().location(), r.session().session_name()};
          },
          "[location, name] of the session as the backend sees it: what a warning "
          "message must not contain (mef3io._warnings.scrub).")
      .def(
          "declaration_issues",
          [](mef3io::Reader& r) {
            nb::list out;
            for (const auto& i : r.declaration_issues()) {
              nb::dict d;
              d["channel"] = i.channel;
              d["segment"] = i.segment_number;
              d["field"] = i.field;
              out.append(d);
            }
            return out;
          },
          "Section-2 size declarations this session leaves unset. Free to call.")
      .def("info",
           [](mef3io::Reader& r, const std::string& ch) {
             const auto& ci = r.info(ch);
             nb::dict d;
             d["name"] = ci.name;
             d["sampling_frequency"] = ci.sampling_frequency;
             d["units_conversion_factor"] = ci.units_conversion_factor;
             d["units_description"] = ci.units_description;
             d["start_time"] = ci.start_time;
             d["end_time"] = ci.end_time;
             d["number_of_samples"] = ci.number_of_samples;
             d["recording_time_offset"] = ci.recording_time_offset;
             d["n_segments"] = ci.n_segments;
             d["session_description"] = ci.session_description;
             d["channel_description"] = ci.channel_description;
             d["reference_description"] = ci.reference_description;
             d["acquisition_channel_number"] = ci.acquisition_channel_number;
             d["low_frequency_filter"] = ci.low_frequency_filter;
             d["high_frequency_filter"] = ci.high_frequency_filter;
             d["notch_filter"] = ci.notch_filter;
             d["line_frequency"] = ci.line_frequency;
             d["section3_available"] = ci.section3_available;
             if (ci.section3_available) {
               d["gmt_offset"] = ci.gmt_offset;
               d["subject_name_1"] = ci.subject_name_1;
               d["subject_name_2"] = ci.subject_name_2;
               d["subject_id"] = ci.subject_id;
               d["recording_location"] = ci.recording_location;
             } else {
               d["gmt_offset"] = nb::none();
               d["subject_name_1"] = nb::none();
               d["subject_name_2"] = nb::none();
               d["subject_id"] = nb::none();
               d["recording_location"] = nb::none();
             }
             return d;
           })
      .def(
          "read",
          [opt_time](mef3io::Reader& r, const std::string& ch, nb::object t0, nb::object t1,
                     int n_threads, const mef3io::CancelToken* cancel, nb::object progress) {
            // Resolve the Python arguments BEFORE releasing the GIL —
            // nb::cast is Python C API and crashes without a thread state
            // (bit us with float timestamps, e.g. t0 + 10*1e6).
            auto a = opt_time(t0), b = opt_time(t1);
            PyHooks hooks(cancel, std::move(progress));
            std::vector<mef3io::sf8> v = hooks.run(
                [&](const mef3io::Control& ctl) { return r.read(ch, a, b, n_threads, ctl); });
            return vec_to_numpy(std::move(v));
          },
          nb::arg("channel"), nb::arg("t0") = nb::none(), nb::arg("t1") = nb::none(),
          nb::arg("n_threads") = mef3io::Reader::kUseDefaultThreads,
          nb::arg("cancel").none() = nb::none(), nb::arg("progress") = nb::none())
      .def(
          "read_raw",
          [opt_time](mef3io::Reader& r, const std::string& ch, nb::object t0, nb::object t1,
                     int n_threads, const mef3io::CancelToken* cancel, nb::object progress) {
            auto a = opt_time(t0), b = opt_time(t1);  // see read: cast needs the GIL
            PyHooks hooks(cancel, std::move(progress));
            mef3io::RawData d = hooks.run(
                [&](const mef3io::Control& ctl) { return r.read_raw(ch, a, b, n_threads, ctl); });
            nb::dict out;
            out["start_uutc"] = d.start_uutc;
            out["sampling_frequency"] = d.sampling_frequency;
            out["units_conversion_factor"] = d.units_conversion_factor;
            out["samples"] = vec_to_numpy(std::move(d.samples));
            out["valid"] = vec_to_numpy(std::move(d.valid));
            nb::list mm;
            for (const auto& x : d.block_copy_mismatches) {
              nb::dict e;
              e["segment"] = x.segment;
              e["block_index"] = x.block_index;
              e["index_start_uutc"] = x.index_start_uutc;
              e["header_start_uutc"] = x.header_start_uutc;
              e["index_number_of_samples"] = x.index_number_of_samples;
              e["header_number_of_samples"] = x.header_number_of_samples;
              mm.append(e);
            }
            out["block_copy_mismatches"] = mm;
            return out;
          },
          nb::arg("channel"), nb::arg("t0") = nb::none(), nb::arg("t1") = nb::none(),
          nb::arg("n_threads") = mef3io::Reader::kUseDefaultThreads,
          nb::arg("cancel").none() = nb::none(), nb::arg("progress") = nb::none())
      .def("segments",
           [](mef3io::Reader& r, const std::string& ch) {
             auto segs = r.segments(ch);
             nb::list out;
             for (const auto& s : segs) {
               nb::dict d;
               d["segment"] = s.segment_number;
               d["path"] = s.path;
               d["start_time"] = s.start_time;
               d["end_time"] = s.end_time;
               d["start_sample"] = s.start_sample;
               d["number_of_samples"] = s.number_of_samples;
               d["number_of_blocks"] = s.number_of_blocks;
               d["provenance"] = provenance_dict(s.provenance);
               out.append(d);
             }
             return out;
           })
      .def("toc",
           [](mef3io::Reader& r, const std::string& ch) {
             auto entries = r.toc(ch);
             nb::list out;
             for (const auto& e : entries) {
               nb::dict d;
               d["start_uutc"] = e.start_uutc;
               d["start_sample"] = e.start_sample;
               d["number_of_samples"] = e.number_of_samples;
               d["maximum_sample_value"] = e.maximum_sample_value;
               d["minimum_sample_value"] = e.minimum_sample_value;
               d["discontinuity"] = e.discontinuity;
               out.append(d);
             }
             return out;
           })
      .def(
          "records",
          [](mef3io::Reader& r, nb::object channel, nb::object segment, nb::object level) {
            std::optional<std::string> ch;
            if (!channel.is_none()) ch = nb::cast<std::string>(channel);
            std::optional<int> seg;
            if (!segment.is_none()) seg = nb::cast<int>(segment);
            auto recs = r.records(ch, seg, record_levels(level));
            nb::list out;
            for (const auto& rec : recs) out.append(record_to_dict(rec));
            return out;
          },
          nb::arg("channel") = nb::none(), nb::arg("segment") = nb::none(),
          nb::arg("level") = nb::none());
}
