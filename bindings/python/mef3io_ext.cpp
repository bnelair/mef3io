// mef3io — nanobind bindings. Fleshed out per phase; P0 exposes just enough
// (crc + version) to prove the toolchain end to end.
#include <limits>
#include <nanobind/nanobind.h>
#include <nanobind/ndarray.h>
#include <nanobind/stl/map.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/pair.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include <span>
#include <optional>
#include <utility>
#include <vector>

#include "mef3io/anonymize.hpp"
#include "mef3io/control.hpp"
#include "mef3io/crc.hpp"
#include "mef3io/errors.hpp"
#include "mef3io/crypto.hpp"
#include "mef3io/inspect.hpp"
#include "mef3io/metadata.hpp"
#include "mef3io/naming.hpp"
#include "mef3io/parallel.hpp"
#include "mef3io/reader.hpp"
#include "mef3io/recover.hpp"
#include "mef3io/red.hpp"
#include "mef3io/session.hpp"
#include "mef3io/session_writer.hpp"
#include "mef3io/tar.hpp"
#include "mef3io/transform.hpp"
#include "mef3io/validate.hpp"
#include "mef3io/types.hpp"
#include "mef3io/version.hpp"
#include "mef3io/writer.hpp"

#include <cmath>
#include <cstring>
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
// A multi-channel read's rows as a 2-D array (channels x n), no copy: numpy
// owns the buffer through a capsule.
template <typename T>
nb::ndarray<nb::numpy, T> rows_to_numpy(mef3io::ChannelRows<T>&& r) {
  T* data = r.data.release();
  nb::capsule owner(data, [](void* p) noexcept { delete[] static_cast<T*>(p); });
  return nb::ndarray<nb::numpy, T>(data, {r.channels, r.samples_per_channel}, owner);
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
      sd["units_conversion_factor"] = opt_obj(g.units_conversion_factor);
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
template <class T>
nb::object opt_or_none(const std::optional<T>& v) {
  return v ? nb::cast(*v) : nb::none();
}

nb::object str_or_none(const std::string& v) {
  if (v.empty()) return nb::none();
  return nb::str(v.c_str());
}

/// details of a refusal, as on the raised exception and in CheckReport.
nb::dict refusal_details(const mef3io::CopyRefusal& r) {
  nb::dict d;
  d["channel"] = str_or_none(r.channel);
  d["field"] = str_or_none(r.field);
  d["inspect_code"] = str_or_none(r.inspect_code);
  return d;
}

// The Python exception type of each RefusalKind (set once in NB_MODULE).
PyObject* g_refusal_types[6] = {};

nb::dict verify_report_dict(const mef3io::VerifyReport& r) {
  auto issues = [](const std::vector<mef3io::VerifyIssue>& v) {
    nb::list l;
    for (const auto& i : v) {
      nb::dict d;
      d["location"] = i.location;
      d["field"] = i.field;
      d["message"] = i.message;
      l.append(d);
    }
    return l;
  };
  nb::dict d;
  d["ok"] = r.ok;
  d["problems"] = issues(r.problems);
  d["leaks"] = issues(r.leaks);
  d["notes"] = issues(r.notes);
  d["inherited_findings"] = r.inherited_findings;
  d["channels"] = r.channels;
  d["excluded_channels"] = r.excluded_channels;
  d["segments"] = r.segments;
  d["blocks"] = r.blocks;
  d["records"] = r.records;
  d["times_checked"] = r.times_checked;
  d["text_fields_checked"] = r.text_fields_checked;
  return d;
}

/// The options every copy operation shares (CopyOptions).
void set_copy_options(mef3io::CopyOptions& o, bool rename, const std::string& password,
                      const std::string& output_password_1, const std::string& output_password_2,
                      std::vector<std::string> channels, std::vector<std::string> exclude_channels,
                      std::optional<std::pair<std::int64_t, std::int64_t>> time_range = std::nullopt,
                      bool exact = true) {
  o.time_range = time_range;
  o.exact = exact;
  o.rename = rename;
  o.password = password;
  o.output_password_1 = output_password_1;
  o.output_password_2 = output_password_2;
  o.channels = std::move(channels);
  o.exclude_channels = std::move(exclude_channels);
}

/// A pre-flight result (CopyCheck) as the dict mef3io's CheckReport reads.
nb::dict check_dict(const mef3io::CopyCheck& c) {
  nb::list refusals;
  for (const auto& r : c.refusals) {
    nb::dict d;
    d["code"] = r.code;
    // Messages may name source paths, which need not be UTF-8.
    d["message"] = nb::steal(PyUnicode_DecodeUTF8(
        r.message.data(), static_cast<Py_ssize_t>(r.message.size()), "replace"));
    d["error"] = mef3io::refusal_kind_name(r.kind);
    d["channel"] = str_or_none(r.channel);
    d["field"] = str_or_none(r.field);
    d["inspect_code"] = str_or_none(r.inspect_code);
    refusals.append(d);
  }
  nb::dict out;
  out["ok"] = c.ok;
  out["refusals"] = refusals;
  out["notes"] = c.notes;
  out["session_name"] = c.session_name;
  out["channels_kept"] = c.channels_kept;
  out["channels_excluded"] = c.channels_excluded;
  out["channels_without_samples"] = c.channels_without_samples;
  return out;
}

/// The fields every copy report has (CopyReport).
nb::dict copy_report_dict(const mef3io::CopyReport& r) {
  nb::dict out;
  out["destination"] = r.destination;
  out["session_name"] = r.session_name;
  out["channels_kept"] = r.channels_kept;
  out["channels_excluded"] = r.channels_excluded;
  out["channels_without_samples"] = r.channels_without_samples;
  out["segments"] = r.segments;
  out["blocks"] = r.blocks;
  out["blocks_reencrypted"] = r.blocks_reencrypted;
  out["blocks_decrypted"] = r.blocks_decrypted;
  out["records"] = r.records;
  out["seizure_channel_entries_dropped"] = r.seizure_channel_entries_dropped;
  out["blocks_trimmed"] = r.blocks_trimmed;
  out["records_outside_window"] = r.records_outside_window;
  out["bytes_written"] = r.bytes_written;
  out["output_encrypted"] = r.output_encrypted;
  out["not_copied"] = r.not_copied;
  out["notes"] = r.notes;
  out["verification"] = verify_report_dict(r.verification);
  return out;
}

/// MetadataEdits from the binding's arguments (None = unchanged).
mef3io::MetadataEdits metadata_edits(std::optional<std::string> subject_name_1,
                                     std::optional<std::string> subject_name_2,
                                     std::optional<std::string> subject_id,
                                     std::optional<std::string> recording_location,
                                     std::optional<std::string> session_description,
                                     std::map<std::string, std::string> channel_descriptions,
                                     std::map<std::string, std::string> reference_descriptions) {
  mef3io::MetadataEdits m;
  m.subject_name_1 = std::move(subject_name_1);
  m.subject_name_2 = std::move(subject_name_2);
  m.subject_id = std::move(subject_id);
  m.recording_location = std::move(recording_location);
  m.session_description = std::move(session_description);
  m.channel_descriptions = std::move(channel_descriptions);
  m.reference_descriptions = std::move(reference_descriptions);
  return m;
}
}  // namespace

namespace {
// Construct a bound C++ object in place with the GIL RELEASED (#79): opening a
// session reads and parses metadata for every segment, and holding the GIL
// for that froze every other Python thread (a GUI's event loop) for seconds.
// Safe because nothing Python is touched while it is released: every argument
// is already a C++ value (nanobind converted them before this runs), and
// `self` is not yet visible to any other thread (nanobind marks the instance
// ready only after __init__ returns). No callback runs during a constructor;
// one that is added must re-acquire the GIL, as PyHooks::call does. An
// exception thrown here re-acquires the GIL while unwinding (the release guard
// is destroyed first) and leaves the instance unconstructed, as nb::init would.
// Unsupported: two threads calling __init__ on the SAME not-yet-ready instance
// (reachable only through an explicit __new__); nanobind does not guard that.
template <class T, class... Args>
void construct_nogil(T* self, Args&&... args) {
  nb::gil_scoped_release release;
  new (self) T(std::forward<Args>(args)...);
}

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
  // Multi-channel write failures (SessionWriter::write_int32_many):
  // PartialWriteError.written_channels = on disk; both carry
  // .incomplete_channels = channels whose rollback did not complete.
  static nb::exception<mef3io::PartialWriteError> partial_write_error(
      m, "PartialWriteError", io_error.ptr());
  static nb::exception<mef3io::RollbackIncompleteError> rollback_incomplete_error(
      m, "RollbackIncompleteError", io_error.ptr());
  nb::register_exception_translator([](const std::exception_ptr& p, void*) {
    auto raise = [](PyObject* type, const char* what, const std::vector<std::string>* written,
                    const std::vector<std::string>& incomplete) {
      PyObject* text = PyUnicode_DecodeUTF8(what, static_cast<Py_ssize_t>(std::strlen(what)),
                                            "replace");
      if (!text) return;
      PyObject* exc = PyObject_CallFunctionObjArgs(type, text, nullptr);
      Py_DECREF(text);
      if (!exc) return;
      if (written) {
        nb::list w;
        for (const auto& c : *written) w.append(c);
        PyObject_SetAttrString(exc, "written_channels", w.ptr());
      }
      nb::list bad;
      for (const auto& c : incomplete) bad.append(c);
      PyObject_SetAttrString(exc, "incomplete_channels", bad.ptr());
      PyErr_SetObject(type, exc);
      Py_DECREF(exc);
    };
    try {
      std::rethrow_exception(p);
    } catch (const mef3io::PartialWriteError& e) {
      raise(partial_write_error.ptr(), e.what(), &e.written(), e.incomplete());
    } catch (const mef3io::RollbackIncompleteError& e) {
      raise(rollback_incomplete_error.ptr(), e.what(), nullptr, e.incomplete());
    }
  });
  // NOT under MefError/RuntimeError: a cancellation is not a failure, and code
  // that catches MefError to report a damaged file must not catch it.
  static nb::exception<mef3io::Cancelled> cancelled_error(m, "Cancelled", PyExc_Exception);

  // copy refusals (anonymize #111, transform #73): the exception type they always had, plus
  // `.code` (stable) and `.details` (channel, field, inspect_code). Registered
  // LAST so it is tried FIRST (nanobind tries translators in reverse order).
  g_refusal_types[static_cast<int>(mef3io::RefusalKind::InvalidArgument)] = PyExc_ValueError;
  g_refusal_types[static_cast<int>(mef3io::RefusalKind::Io)] = io_error.ptr();
  g_refusal_types[static_cast<int>(mef3io::RefusalKind::WriteConflict)] = write_conflict_error.ptr();
  g_refusal_types[static_cast<int>(mef3io::RefusalKind::Password)] = password_error.ptr();
  g_refusal_types[static_cast<int>(mef3io::RefusalKind::Format)] = format_error.ptr();
  g_refusal_types[static_cast<int>(mef3io::RefusalKind::Crc)] = crc_error.ptr();
  nb::register_exception_translator([](const std::exception_ptr& p, void*) {
    try {
      std::rethrow_exception(p);
    } catch (const mef3io::CopyError& e) {
      const auto& r = e.refusal();
      PyObject* type = g_refusal_types[static_cast<int>(r.kind)];
      const auto* se = dynamic_cast<const std::exception*>(&e);
      const char* msg = se ? se->what() : r.message.c_str();
      // "replace": a message holding a non-UTF-8 path must still raise the
      // refusal (with its code), not a UnicodeDecodeError.
      PyObject* text = PyUnicode_DecodeUTF8(msg, static_cast<Py_ssize_t>(std::strlen(msg)),
                                            "replace");
      if (!text) return;
      // PyObject_CallFunctionObjArgs, not PyObject_CallOneArg: the latter is
      // missing from nanobind's macOS symbol list (darwin-ld-cpython.sym), so
      // the extension failed to link on macOS.
      PyObject* exc = PyObject_CallFunctionObjArgs(type, text, nullptr);
      Py_DECREF(text);
      if (!exc) return;  // the constructor's error is set
      PyObject_SetAttrString(exc, "code", nb::str(r.code.c_str()).ptr());
      PyObject_SetAttrString(exc, "details", refusal_details(r).ptr());
      PyErr_SetObject(type, exc);
      Py_DECREF(exc);
    }
  });

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
          nb::list decl;
          for (const auto& c : s.declarations) decl.append(c);
          d["declarations"] = decl;
          segs.append(d);
        }
        nb::list rfiles;
        for (const auto& f : r.record_files) {
          nb::dict d;
          d["path"] = f.path;
          d["records_before"] = f.records_before;
          d["records_after"] = f.records_after;
          d["bytes_dropped"] = f.bytes_dropped;
          d["records_damaged"] = f.records_damaged;
          d["index_rebuilt"] = f.index_rebuilt;
          d["action"] = f.action;
          rfiles.append(d);
        }
        nb::list skipped;
        for (const auto& s : r.skipped) skipped.append(s);
        nb::dict out;
        out["segments"] = segs;
        out["record_files"] = rfiles;
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

  // --- anonymize (#70): a de-identified copy; times kept, or shifted ---
  m.def(
      "check_anonymize",
      [](const std::string& src, const std::string& dst, std::optional<std::int64_t> shift_us,
         const std::string& pseudonym, bool clear_subject, bool clear_location,
         bool clear_descriptions, const std::string& text, bool has_text_fn, bool rename,
         const std::string& password, const std::string& output_password_1,
         const std::string& output_password_2, std::vector<std::string> channels,
         std::vector<std::string> exclude_channels, std::optional<std::pair<std::int64_t, std::int64_t>> time_range, bool exact,
         const mef3io::CancelToken* cancel) {
        PyHooks hooks(cancel, nb::none());
        mef3io::AnonymizeOptions o;
        o.shift_us = shift_us;  // none = the copy keeps every time
        o.pseudonym = pseudonym;
        o.clear_subject = clear_subject;
        o.clear_location = clear_location;
        o.clear_descriptions = clear_descriptions;
        if (text == "drop") o.text = mef3io::RecordTextPolicy::Drop;
        else if (text == "keep") o.text = mef3io::RecordTextPolicy::Keep;
        else if (text == "callback") o.text = mef3io::RecordTextPolicy::Callback;
        else throw nb::value_error("text must be 'drop', 'keep' or a callable");
        // The check never calls it; it only needs to know there is one.
        if (has_text_fn)
          o.text_fn = [](const std::string& v, const mef3io::RecordTextField&) { return v; };
        set_copy_options(o, rename, password, output_password_1, output_password_2,
                         std::move(channels), std::move(exclude_channels), time_range, exact);
        mef3io::AnonymizeCheck c = hooks.run([&](const mef3io::Control& ctl) {
          return mef3io::check_anonymize(src, dst, o, ctl);
        });
        nb::dict out = check_dict(c);
        out["pseudonym"] = c.pseudonym;
        return out;
      },
      nb::arg("src"), nb::arg("dst"), nb::arg("shift_us").none() = nb::none(),
      nb::arg("pseudonym") = "", nb::arg("clear_subject") = true,
      nb::arg("clear_location") = true, nb::arg("clear_descriptions") = true,
      nb::arg("text") = "drop", nb::arg("has_text_fn") = false,
      nb::arg("rename") = true, nb::arg("password") = "", nb::arg("output_password_1") = "",
      nb::arg("output_password_2") = "", nb::arg("channels") = std::vector<std::string>{},
      nb::arg("exclude_channels") = std::vector<std::string>{},
      nb::arg("time_range").none() = nb::none(), nb::arg("exact") = true,
      nb::arg("cancel").none() = nb::none(),
      "Every check anonymize_session makes before writing; writes nothing "
      "(see mef3io.anonymize.check).");
  m.def(
      "anonymize_session",
      [](const std::string& src, const std::string& dst, std::optional<std::int64_t> shift_us,
         const std::string& pseudonym, bool clear_subject, bool clear_location,
         bool clear_descriptions, const std::string& text, nb::object text_fn, bool rename,
         const std::string& password, const std::string& output_password_1,
         const std::string& output_password_2, std::vector<std::string> channels,
         std::vector<std::string> exclude_channels, std::optional<std::pair<std::int64_t, std::int64_t>> time_range, bool exact,
         const mef3io::CancelToken* cancel, nb::object progress) {
        PyHooks hooks(cancel, std::move(progress));
        mef3io::AnonymizeOptions o;
        o.shift_us = shift_us;
        o.pseudonym = pseudonym;
        o.clear_subject = clear_subject;
        o.clear_location = clear_location;
        o.clear_descriptions = clear_descriptions;
        if (text == "drop") o.text = mef3io::RecordTextPolicy::Drop;
        else if (text == "keep") o.text = mef3io::RecordTextPolicy::Keep;
        else if (text == "callback") o.text = mef3io::RecordTextPolicy::Callback;
        else throw nb::value_error("text must be 'drop', 'keep' or a callable");
        if (o.text == mef3io::RecordTextPolicy::Callback) {
          if (!PyCallable_Check(text_fn.ptr())) throw nb::type_error("text must be callable");
          // Called on the copying thread with the GIL released; take it for
          // the call. A raised exception aborts the copy (nothing is left
          // behind) and is re-raised unchanged, like a progress callback's.
          o.text_fn = [&hooks, &text_fn](const std::string& value,
                                         const mef3io::RecordTextField& f) -> std::string {
            nb::gil_scoped_acquire gil;
            if (hooks.error) throw mef3io::Cancelled("text callback raised");
            try {
              nb::dict info;
              info["record_type"] = f.record_type;
              info["field"] = f.field;
              info["level"] = mef3io::record_level_name(f.level);
              info["channel"] = f.channel;
              info["segment"] = f.segment;
              // NO_ENTRY: not known (an offset its segments disagree on).
              if (f.time == std::numeric_limits<std::int64_t>::min()) info["time"] = nb::none();
              else info["time"] = f.time;
              // Record text is bytes from a file and need not be valid UTF-8:
              // decode with surrogateescape so every byte survives, and
              // encode the result back the same way.
              PyObject* raw = PyUnicode_DecodeUTF8(value.data(),
                                                   static_cast<Py_ssize_t>(value.size()),
                                                   "surrogateescape");
              if (!raw) throw nb::python_error();
              nb::object arg = nb::steal(raw);
              nb::object r = text_fn(arg, info);
              if (r.is_none()) return std::string();
              if (!nb::isinstance<nb::str>(r)) {
                PyErr_SetString(PyExc_TypeError, "the text callable must return a str or None");
                throw nb::python_error();
              }
              PyObject* enc = PyUnicode_AsEncodedString(r.ptr(), "utf-8", "surrogateescape");
              if (!enc) throw nb::python_error();
              nb::object b = nb::steal(enc);
              return std::string(PyBytes_AS_STRING(enc),
                                 static_cast<std::size_t>(PyBytes_GET_SIZE(enc)));
            } catch (nb::python_error& e) {
              hooks.error.emplace(std::move(e));
              throw mef3io::Cancelled("text callback raised");
            }
          };
        }
        set_copy_options(o, rename, password, output_password_1, output_password_2,
                         std::move(channels), std::move(exclude_channels), time_range, exact);
        mef3io::AnonymizeReport r = hooks.run([&](const mef3io::Control& ctl) {
          return mef3io::anonymize_session(src, dst, o, ctl);
        });
        nb::dict out = copy_report_dict(r);
        out["pseudonym"] = r.pseudonym;
        out["shift_us"] = opt_or_none(r.shift_us);
        out["source_recording_time_offset"] = r.source_recording_time_offset;
        // The re-identification key (see AnonymizeReport): to the caller only.
        out["source_start_us"] = opt_or_none(r.source_start_us);
        out["source_end_us"] = opt_or_none(r.source_end_us);
        nb::dict ctimes;
        for (const auto& t : r.source_channel_times)
          ctimes[nb::str(t.channel.c_str())] =
              nb::make_tuple(opt_or_none(t.start_us), opt_or_none(t.end_us));
        out["source_channel_times"] = ctimes;
        out["record_text_fields_changed"] = r.record_text_fields_changed;
        out["record_bodies_cleared"] = r.record_bodies_cleared;
        return out;
      },
      nb::arg("src"), nb::arg("dst"), nb::arg("shift_us").none() = nb::none(),
      nb::arg("pseudonym") = "",
      nb::arg("clear_subject") = true, nb::arg("clear_location") = true,
      nb::arg("clear_descriptions") = true, nb::arg("text") = "drop",
      nb::arg("text_fn") = nb::none(), nb::arg("rename") = true, nb::arg("password") = "",
      nb::arg("output_password_1") = "", nb::arg("output_password_2") = "",
      nb::arg("channels") = std::vector<std::string>{},
      nb::arg("exclude_channels") = std::vector<std::string>{},
      nb::arg("time_range").none() = nb::none(), nb::arg("exact") = true,
      nb::arg("cancel").none() = nb::none(), nb::arg("progress") = nb::none(),
      "Write a de-identified copy of a session (see mef3io.anonymize_session).");
  m.def(
      "redact_text",
      [](const std::string& src, const std::string& dst, nb::list raw_texts,
         const std::string& pseudonym, bool clear_subject, bool clear_location,
         bool clear_descriptions, bool rename, const std::string& password,
         std::vector<std::string> channels, std::vector<std::string> exclude_channels,
         const mef3io::CancelToken* cancel) {
        std::vector<std::string> texts;
        for (nb::handle h : raw_texts) {
          const nb::bytes b = nb::cast<nb::bytes>(h);
          texts.emplace_back(b.c_str(), b.size());
        }
        PyHooks hooks(cancel, nb::none());
        mef3io::AnonymizeOptions o;
        o.pseudonym = pseudonym;
        o.clear_subject = clear_subject;
        o.clear_location = clear_location;
        o.clear_descriptions = clear_descriptions;
        o.text = mef3io::RecordTextPolicy::Keep;
        set_copy_options(o, rename, password, "", "", std::move(channels),
                         std::move(exclude_channels));
        // Bytes in, bytes out (text from a file need not be UTF-8).
        const std::vector<std::string> out = hooks.run([&](const mef3io::Control& ctl) {
          return mef3io::redact_text(src, dst, o, texts, ctl);
        });
        nb::list l;
        for (const auto& t : out) l.append(nb::bytes(t.data(), t.size()));
        return l;
      },
      nb::arg("src"), nb::arg("dst"), nb::arg("texts"), nb::arg("pseudonym") = "",
      nb::arg("clear_subject") = true, nb::arg("clear_location") = true,
      nb::arg("clear_descriptions") = true, nb::arg("rename") = true, nb::arg("password") = "",
      nb::arg("channels") = std::vector<std::string>{},
      nb::arg("exclude_channels") = std::vector<std::string>{},
      nb::arg("cancel").none() = nb::none(),
      "The text anonymize_session would keep (see mef3io.anonymize.redact_text).");
  m.def(
      "verify_anonymized",
      [](const std::string& src, const std::string& dst, std::optional<std::int64_t> shift_us,
         const std::string& password, const std::string& output_password, bool clear_subject,
         bool clear_location, bool clear_descriptions, std::optional<std::pair<std::int64_t, std::int64_t>> time_range, bool exact,
         const mef3io::CancelToken* cancel, nb::object progress) {
        PyHooks hooks(cancel, std::move(progress));
        mef3io::VerifyOptions o;
        o.time_range = time_range;
        o.exact = exact;
        o.password = password;
        o.output_password = output_password;
        o.clear_subject = clear_subject;
        o.clear_location = clear_location;
        o.clear_descriptions = clear_descriptions;
        mef3io::VerifyReport r = hooks.run([&](const mef3io::Control& ctl) {
          return mef3io::verify_anonymized(src, dst, shift_us, o, ctl);
        });
        return verify_report_dict(r);
      },
      nb::arg("src"), nb::arg("dst"), nb::arg("shift_us").none() = nb::none(),
      nb::arg("password") = "",
      nb::arg("output_password") = "", nb::arg("clear_subject") = true,
      nb::arg("clear_location") = true, nb::arg("clear_descriptions") = true,
      nb::arg("time_range").none() = nb::none(), nb::arg("exact") = true,
      nb::arg("cancel").none() = nb::none(), nb::arg("progress") = nb::none(),
      "Check a de-identified copy against its source (see mef3io.anonymize.verify).");

  // --- transform (#73): a copy with a choice of channels, passwords, name
  // and descriptive metadata; times and samples verbatim ---
#define MEF3IO_TRANSFORM_ARGS                                                                    \
  const std::string &src, const std::string &dst, bool rename, const std::string &password,     \
      const std::string &output_password_1, const std::string &output_password_2,                \
      std::vector<std::string> channels, std::vector<std::string> exclude_channels,              \
      std::optional<std::string> subject_name_1, std::optional<std::string> subject_name_2,      \
      std::optional<std::string> subject_id, std::optional<std::string> recording_location,      \
      std::optional<std::string> session_description,                                            \
      std::map<std::string, std::string> channel_descriptions,                                   \
      std::map<std::string, std::string> reference_descriptions,                                 \
      std::optional<std::pair<std::int64_t, std::int64_t>> time_range, bool exact,               \
      const mef3io::CancelToken *cancel
#define MEF3IO_TRANSFORM_OPTIONS(o)                                                              \
  mef3io::TransformOptions o;                                                                    \
  set_copy_options(o, rename, password, output_password_1, output_password_2,                    \
                   std::move(channels), std::move(exclude_channels), time_range, exact);         \
  o.metadata = metadata_edits(std::move(subject_name_1), std::move(subject_name_2),              \
                              std::move(subject_id), std::move(recording_location),              \
                              std::move(session_description), std::move(channel_descriptions),   \
                              std::move(reference_descriptions))
#define MEF3IO_TRANSFORM_NB_ARGS                                                                 \
  nb::arg("src"), nb::arg("dst"), nb::arg("rename") = true, nb::arg("password") = "",            \
      nb::arg("output_password_1") = "", nb::arg("output_password_2") = "",                      \
      nb::arg("channels") = std::vector<std::string>{},                                          \
      nb::arg("exclude_channels") = std::vector<std::string>{},                                  \
      nb::arg("subject_name_1").none() = nb::none(), nb::arg("subject_name_2").none() = nb::none(), \
      nb::arg("subject_id").none() = nb::none(), nb::arg("recording_location").none() = nb::none(), \
      nb::arg("session_description").none() = nb::none(),                                        \
      nb::arg("channel_descriptions") = std::map<std::string, std::string>{},                    \
      nb::arg("reference_descriptions") = std::map<std::string, std::string>{},                  \
      nb::arg("time_range").none() = nb::none(), nb::arg("exact") = true,                        \
      nb::arg("cancel").none() = nb::none()
  m.def(
      "check_transform",
      [](MEF3IO_TRANSFORM_ARGS) {
        PyHooks hooks(cancel, nb::none());
        MEF3IO_TRANSFORM_OPTIONS(o);
        const mef3io::CopyCheck c = hooks.run(
            [&](const mef3io::Control& ctl) { return mef3io::check_transform(src, dst, o, ctl); });
        return check_dict(c);
      },
      MEF3IO_TRANSFORM_NB_ARGS,
      "Every check transform_session makes before writing; writes nothing "
      "(see mef3io.transform.check).");
  m.def(
      "transform_session",
      [](MEF3IO_TRANSFORM_ARGS, nb::object progress) {
        PyHooks hooks(cancel, std::move(progress));
        MEF3IO_TRANSFORM_OPTIONS(o);
        const mef3io::TransformReport r = hooks.run([&](const mef3io::Control& ctl) {
          return mef3io::transform_session(src, dst, o, ctl);
        });
        nb::dict out = copy_report_dict(r);
        out["metadata_fields_changed"] = r.metadata_fields_changed;
        return out;
      },
      MEF3IO_TRANSFORM_NB_ARGS, nb::arg("progress") = nb::none(),
      "Write a modified copy of a session (see mef3io.transform_session).");
  m.def(
      "verify_transform",
      [](MEF3IO_TRANSFORM_ARGS, nb::object progress) {
        PyHooks hooks(cancel, std::move(progress));
        MEF3IO_TRANSFORM_OPTIONS(o);
        const mef3io::VerifyReport r = hooks.run([&](const mef3io::Control& ctl) {
          return mef3io::verify_transform(src, dst, o, ctl);
        });
        return verify_report_dict(r);
      },
      MEF3IO_TRANSFORM_NB_ARGS, nb::arg("progress") = nb::none(),
      "Check a transformed copy against its source (see mef3io.transform.verify).");
  // --- merge (#103): several sessions into one ---
#define MEF3IO_MERGE_ARGS                                                                        \
  const std::vector<std::string> &sources, const std::string &dst,                               \
      std::vector<std::string> passwords, const std::string &output_password_1,                  \
      const std::string &output_password_2, std::vector<std::string> channels,                   \
      std::vector<std::string> exclude_channels, std::optional<std::string> subject_name_1,      \
      std::optional<std::string> subject_name_2, std::optional<std::string> subject_id,          \
      std::optional<std::string> recording_location,                                             \
      std::optional<std::string> session_description,                                            \
      std::map<std::string, std::string> channel_descriptions,                                   \
      std::map<std::string, std::string> reference_descriptions, const mef3io::CancelToken *cancel
#define MEF3IO_MERGE_OPTIONS(o)                                                                  \
  mef3io::MergeOptions o;                                                                        \
  set_copy_options(o, true, "", output_password_1, output_password_2, std::move(channels),       \
                   std::move(exclude_channels));                                                 \
  o.passwords = std::move(passwords);                                                            \
  o.metadata = metadata_edits(std::move(subject_name_1), std::move(subject_name_2),              \
                              std::move(subject_id), std::move(recording_location),              \
                              std::move(session_description), std::move(channel_descriptions),   \
                              std::move(reference_descriptions))
#define MEF3IO_MERGE_NB_ARGS                                                                     \
  nb::arg("sources"), nb::arg("dst"), nb::arg("passwords") = std::vector<std::string>{},         \
      nb::arg("output_password_1") = "", nb::arg("output_password_2") = "",                      \
      nb::arg("channels") = std::vector<std::string>{},                                          \
      nb::arg("exclude_channels") = std::vector<std::string>{},                                  \
      nb::arg("subject_name_1").none() = nb::none(), nb::arg("subject_name_2").none() = nb::none(), \
      nb::arg("subject_id").none() = nb::none(), nb::arg("recording_location").none() = nb::none(), \
      nb::arg("session_description").none() = nb::none(),                                        \
      nb::arg("channel_descriptions") = std::map<std::string, std::string>{},                    \
      nb::arg("reference_descriptions") = std::map<std::string, std::string>{},                  \
      nb::arg("cancel").none() = nb::none()
  m.def(
      "check_merge",
      [](MEF3IO_MERGE_ARGS) {
        PyHooks hooks(cancel, nb::none());
        MEF3IO_MERGE_OPTIONS(o);
        const mef3io::CopyCheck c = hooks.run(
            [&](const mef3io::Control& ctl) { return mef3io::check_merge(sources, dst, o, ctl); });
        return check_dict(c);
      },
      MEF3IO_MERGE_NB_ARGS, "Every check merge_sessions makes before writing (see mef3io.merge).");
  m.def(
      "merge_sessions",
      [](MEF3IO_MERGE_ARGS, nb::object progress) {
        PyHooks hooks(cancel, std::move(progress));
        MEF3IO_MERGE_OPTIONS(o);
        const mef3io::MergeReport r = hooks.run([&](const mef3io::Control& ctl) {
          return mef3io::merge_sessions(sources, dst, o, ctl);
        });
        nb::dict out = copy_report_dict(r);
        out["metadata_fields_changed"] = r.metadata_fields_changed;
        out["sources"] = r.sources;
        out["segments_split"] = r.segments_split;
        return out;
      },
      MEF3IO_MERGE_NB_ARGS, nb::arg("progress") = nb::none(),
      "Merge several sessions into one (see mef3io.merge_sessions).");
  m.def(
      "verify_merge",
      [](MEF3IO_MERGE_ARGS, nb::object progress) {
        PyHooks hooks(cancel, std::move(progress));
        MEF3IO_MERGE_OPTIONS(o);
        const mef3io::VerifyReport r = hooks.run(
            [&](const mef3io::Control& ctl) { return mef3io::verify_merge(sources, dst, o, ctl); });
        return verify_report_dict(r);
      },
      MEF3IO_MERGE_NB_ARGS, nb::arg("progress") = nb::none(),
      "Check a merged session against its sources (see mef3io.merge.verify).");
#undef MEF3IO_MERGE_ARGS
#undef MEF3IO_MERGE_OPTIONS
#undef MEF3IO_MERGE_NB_ARGS
#undef MEF3IO_TRANSFORM_ARGS
#undef MEF3IO_TRANSFORM_OPTIONS
#undef MEF3IO_TRANSFORM_NB_ARGS

  // --- naming (#72, first part) ---
  m.def(
      "time_range",
      [](const std::string& path, const std::string& password, const mef3io::CancelToken* cancel) {
        PyHooks hooks(cancel, nb::none());
        const mef3io::TimeRange r = hooks.run(
            [&](const mef3io::Control& ctl) { return mef3io::time_range(path, password, ctl); });
        nb::dict d;
        d["start_uutc"] = opt_or_none(r.start_uutc);
        d["end_uutc"] = opt_or_none(r.end_uutc);
        d["offset_known"] = r.offset_known;
        d["channels"] = r.channels;
        d["files_read"] = r.files_read;
        return d;
      },
      nb::arg("path"), nb::arg("password") = "", nb::arg("cancel").none() = nb::none(),
      "A session's first and last sample time, fast (see mef3io.time_range).");
  m.def(
      "name_drift",
      [](const std::string& path, const mef3io::CancelToken* cancel) {
        PyHooks hooks(cancel, nb::none());
        const mef3io::NameDrift r =
            hooks.run([&](const mef3io::Control& ctl) { return mef3io::name_drift(path, ctl); });
        nb::dict d;
        d["folder"] = r.folder;
        d["record_files"] = r.record_files;
        nb::dict names;
        for (const auto& [k, v] : r.stored_names) names[nb::str(k.c_str())] = v;
        d["stored_names"] = names;
        d["files_read"] = r.files_read;
        d["consistent"] = r.consistent;
        return d;
      },
      nb::arg("path"), nb::arg("cancel").none() = nb::none(),
      "Where a session's name is stored and whether the places agree (see mef3io.name_drift).");
  m.def(
      "rename_archive",
      [](const std::string& path, const std::string& new_name) {
        const mef3io::ArchiveRename r = mef3io::rename_archive(path, new_name);
        nb::dict d;
        d["old_path"] = r.old_path;
        d["new_path"] = r.new_path;
        d["inside_keeps_old_name"] = r.inside_keeps_old_name;
        return d;
      },
      nb::arg("path"), nb::arg("new_name"),
      "Rename a .mefd.tar archive without ever replacing a file (see mef3io.rename_archive).");

  // --- rename_session / fix_name_drift (#72, second part) ---
  auto rename_dict = [](const mef3io::RenameReport& r) {
    nb::dict d;
    d["old_path"] = r.old_path;
    d["new_path"] = r.new_path;
    d["old_name"] = r.old_name;
    d["new_name"] = r.new_name;
    d["folder_renamed"] = r.folder_renamed;
    d["files_changed"] = r.files_changed;
    d["descriptions_changed"] = r.descriptions_changed;
    nb::list recs;
    for (const auto& [a, b] : r.records_renamed) recs.append(nb::make_tuple(a, b));
    d["records_renamed"] = recs;
    d["rolled_back"] = r.rolled_back;
    d["journals_discarded"] = r.journals_discarded;
    d["notes"] = r.notes;
    return d;
  };
  auto rename_opts = [](std::string password, std::string backup_dir,
                        std::optional<std::vector<std::string>> descriptions, int crash_at,
                        int fail_at) {
    mef3io::RenameOptions o;
    o.crash_at = crash_at;
    o.fail_at = fail_at;
    o.password = std::move(password);
    o.backup_dir = std::move(backup_dir);
    o.descriptions = std::move(descriptions);
    return o;
  };
  m.def(
      "rename_session",
      [=](const std::string& path, const std::string& new_name, std::string password,
          std::string backup_dir, std::optional<std::vector<std::string>> descriptions,
          int crash_at, int fail_at) {
        const auto o = rename_opts(std::move(password), std::move(backup_dir), std::move(descriptions), crash_at, fail_at);
        mef3io::RenameReport r;
        {
          nb::gil_scoped_release release;
          r = mef3io::rename_session(path, new_name, o);
        }
        return rename_dict(r);
      },
      nb::arg("path"), nb::arg("new_name"), nb::arg("password") = "", nb::arg("backup_dir") = "",
      nb::arg("descriptions").none() = nb::none(), nb::arg("_crash_at") = -1, nb::arg("_fail_at") = -1,
      "Rename a directory session in every place its name lives (see mef3io.rename_session).");
  m.def(
      "fix_name_drift",
      [=](const std::string& path, const std::string& target, std::string password,
          std::string backup_dir, std::optional<std::vector<std::string>> descriptions,
          int crash_at, int fail_at) {
        const auto o = rename_opts(std::move(password), std::move(backup_dir), std::move(descriptions), crash_at, fail_at);
        mef3io::RenameReport r;
        {
          nb::gil_scoped_release release;
          r = mef3io::fix_name_drift(path, target, o);
        }
        return rename_dict(r);
      },
      nb::arg("path"), nb::arg("target") = "", nb::arg("password") = "", nb::arg("backup_dir") = "",
      nb::arg("descriptions").none() = nb::none(), nb::arg("_crash_at") = -1, nb::arg("_fail_at") = -1,
      "Set the stored names to the folder's name (see mef3io.fix_name_drift).");
  m.def(
      "operation_marker",
      [](const std::string& path) -> nb::object {
        const auto mk = mef3io::operation_marker(path);
        if (!mk) return nb::none();
        nb::dict d;
        d["tool"] = mk->tool;
        d["operation"] = mk->operation;
        d["id"] = mk->id;
        return d;
      },
      nb::arg("path"), "The session's operation marker, or None.");
  m.def("clear_operation_marker", &mef3io::clear_operation_marker, nb::arg("path"),
        "Remove an operation marker whose journal is lost (see mef3io.clear_operation_marker).");

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

  // The writer's argument rules (#83), exposed so mef3io.Writer can refuse a
  // bad block length BEFORE its constructor creates the session folder.
  m.attr("MAX_BLOCK_SAMPLES") = mef3io::MAX_BLOCK_SAMPLES;
  m.def("check_block_length", &mef3io::check_block_length, nb::arg("samples_per_block"));

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
      // Opening scans the tree and parses every segment's metadata: without
      // the GIL (#79), like every other long call. See construct_nogil.
      .def("__init__",
           [](mef3io::Session* self, const std::string& path, std::string password, bool strict,
              int n_threads) {
             construct_nogil(self, path, std::move(password), strict, n_threads);
           },
           nb::arg("path"), nb::arg("password") = "", nb::arg("strict") = true,
           nb::arg("n_threads") = 0)
      .def("set_threads", &mef3io::Session::set_threads, nb::arg("n"))
      .def("threads", &mef3io::Session::threads)
      .def_prop_ro("channels", &mef3io::Session::channels)
      .def("channel_info",
           [](mef3io::Session& s, const std::string& name) {
             const auto& ci = s.channel_info(name);
             nb::dict d;
             d["name"] = ci.name;
             d["sampling_frequency"] = ci.sampling_frequency;
             d["units_conversion_factor"] = opt_obj(ci.units_conversion_factor);
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
              d["units_conversion_factor"] = r.units_conversion_factor;
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
    d["units_conversion_factor"] = s.units_conversion_factor;
    const char* q = "int32";
    switch (s.quantization) {
      case mef3io::Quantization::Counts: q = "int32"; break;
      case mef3io::Quantization::Decimal: q = "decimal"; break;
      case mef3io::Quantization::Step: q = "step"; break;
    }
    d["quantization"] = q;
    if (s.step_fallback.empty())
      d["step_fallback"] = nb::none();
    else
      d["step_fallback"] = s.step_fallback;
    d["clamped"] = s.clamped;
    d["clamped_infinite"] = s.clamped_infinite;
    return d;
  };
  nb::class_<mef3io::SessionWriter>(m, "SessionWriter")
      // Removes the old tree (overwrite=True) or hydrates every channel's
      // state from disk: without the GIL (#79). See construct_nogil.
      .def("__init__",
           [](mef3io::SessionWriter* self, const std::string& path, bool overwrite,
              std::string password1, std::string password2) {
             construct_nogil(self, path, overwrite, std::move(password1), std::move(password2));
           },
           nb::arg("path"), nb::arg("overwrite") = false, nb::arg("password1") = "",
           nb::arg("password2") = "")
      .def("set_block_length", &mef3io::SessionWriter::set_block_length)
      .def("set_units", &mef3io::SessionWriter::set_units)
      .def_prop_ro("unreadable_metadata",
                   [](const mef3io::SessionWriter& w) { return w.unreadable_metadata(); })
      .def_prop_ro("legacy_password_segments",
                   [](const mef3io::SessionWriter& w) { return w.legacy_password_segments(); })
      .def("set_threads", &mef3io::SessionWriter::set_threads)
      .def(
          "flush",
          [](mef3io::SessionWriter& w) {
            nb::gil_scoped_release release;  // a disk wait: other threads run
            w.flush();
          },
          "Return only when everything this writer wrote so far is on disk "
          "(one combined wait).")
      .def("set_wait_for_disk", &mef3io::SessionWriter::set_wait_for_disk, nb::arg("on"))
      .def("wait_for_disk", &mef3io::SessionWriter::wait_for_disk)
      .def(
          "set_flush_mode",
          [](mef3io::SessionWriter& w, const std::string& mode) {
            if (mode == "each")
              w.set_flush_mode(mef3io::FlushMode::Each);
            else if (mode == "close")
              w.set_flush_mode(mef3io::FlushMode::Close);
            else
              throw std::invalid_argument("flush must be \"each\" or \"close\", not \"" + mode +
                                          "\"");
          },
          nb::arg("mode"))
      .def("flush_mode",
           [](const mef3io::SessionWriter& w) {
             return w.flush_mode() == mef3io::FlushMode::Close ? "close" : "each";
           })
      .def("threads", &mef3io::SessionWriter::threads)
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
                         nb::object start, double fs, int precision, bool new_segment,
                         bool step) {
            std::span<const mef3io::sf8> s(data.data(), data.size());
            const mef3io::si8 start_us = to_si8(start, "start_uutc");
            // step=True is precision="step" (#81); `precision` is then unused.
            const mef3io::Precision p =
                step ? mef3io::Precision::detect_step() : mef3io::Precision(precision);
            mef3io::WriteSummary r;
            {
              nb::gil_scoped_release rel;
              r = w.write_float(ch, s, start_us, fs, p, new_segment);
            }
            return summary_dict(r);
          },
          nb::arg("channel"), nb::arg("data"), nb::arg("start_uutc"), nb::arg("fs"),
          nb::arg("precision") = -1, nb::arg("new_segment") = false, nb::arg("step") = false)
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
          "write_int32_many",
          [summary_dict](mef3io::SessionWriter& w, std::vector<std::string> channels,
                         nb::ndarray<const mef3io::si4, nb::ndim<2>, nb::c_contig> data,
                         std::vector<double> ufacts, nb::object start, double fs,
                         bool new_segment) {
            if (static_cast<std::size_t>(data.shape(0)) != channels.size())
              throw std::invalid_argument("data has " + std::to_string(data.shape(0)) +
                                          " rows for " + std::to_string(channels.size()) +
                                          " channels");
            std::span<const mef3io::si4> s(data.data(), data.size());
            const mef3io::si8 start_us = to_si8(start, "start_uutc");
            std::vector<mef3io::WriteSummary> r;
            {
              // The whole call (encode, files, the combined flush) touches no Python.
              nb::gil_scoped_release rel;
              r = w.write_int32_many(channels, s, ufacts, start_us, fs, new_segment);
            }
            nb::list out;
            for (const auto& x : r) out.append(summary_dict(x));
            return out;
          },
          nb::arg("channels"), nb::arg("data"), nb::arg("ufact"), nb::arg("start_uutc"),
          nb::arg("fs"), nb::arg("new_segment") = false)
      .def(
          "write_float_many",
          [summary_dict](mef3io::SessionWriter& w, std::vector<std::string> channels,
                         nb::ndarray<const mef3io::sf8, nb::ndim<2>, nb::c_contig> data,
                         nb::object start, double fs, int precision, bool new_segment,
                         bool step) {
            if (static_cast<std::size_t>(data.shape(0)) != channels.size())
              throw std::invalid_argument("data has " + std::to_string(data.shape(0)) +
                                          " rows for " + std::to_string(channels.size()) +
                                          " channels");
            std::span<const mef3io::sf8> s(data.data(), data.size());
            const mef3io::si8 start_us = to_si8(start, "start_uutc");
            const mef3io::Precision p =
                step ? mef3io::Precision::detect_step() : mef3io::Precision(precision);
            std::vector<mef3io::WriteSummary> r;
            {
              nb::gil_scoped_release rel;
              r = w.write_float_many(channels, s, start_us, fs, p, new_segment);
            }
            nb::list out;
            for (const auto& x : r) out.append(summary_dict(x));
            return out;
          },
          nb::arg("channels"), nb::arg("data"), nb::arg("start_uutc"), nb::arg("fs"),
          nb::arg("precision") = -1, nb::arg("new_segment") = false, nb::arg("step") = false)
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
            // Converted above, WITH the GIL; the file work runs without it.
            nb::gil_scoped_release rel;
            w.write_records(ch, recs);
          },
          nb::arg("channel").none(), nb::arg("records"));

  // #81: the step estimate alone (tests, and callers that want to know the
  // step before writing). The write verifies every value; this does not.
  m.def("detect_step", [](nb::ndarray<const mef3io::sf8, nb::ndim<1>, nb::c_contig> d) {
    mef3io::StepEstimate e;
    {
      nb::gil_scoped_release rel;  // bounded sample; the argument keeps it alive
      e = mef3io::detect_step(std::span<const mef3io::sf8>(d.data(), d.size()));
    }
    nb::dict out;
    if (e.step)
      out["step"] = *e.step;
    else
      out["step"] = nb::none();
    out["reason"] = e.reason;
    return out;
  });

  // The ONE thread rule's resolver (parallel.hpp): what "automatic" means.
  // Python asks here instead of re-deriving it (MultiReader's budget).
  m.def(
      "resolve_threads",
      [](int n, bool io) {
        return mef3io::resolve_threads(n, io ? mef3io::Work::Io : mef3io::Work::Cpu);
      },
      nb::arg("n"), nb::arg("io") = false);

  m.def("infer_precision", [](nb::ndarray<const mef3io::sf8, nb::ndim<1>, nb::c_contig> d) {
    nb::gil_scoped_release rel;  // one pass over the data; the argument keeps it alive
    return mef3io::infer_precision(std::span<const mef3io::sf8>(d.data(), d.size()));
  });

  // --- Reader (P3 high-level API) ---
  auto opt_time = [](nb::object o) { return opt_si8(o, "t0/t1"); };

  nb::class_<mef3io::Reader>(m, "Reader")
      // The open (tree scan, every .tmet parsed and decrypted) is the long
      // part of a Reader's life: without the GIL (#79). See construct_nogil.
      .def("__init__",
           [](mef3io::Reader* self, const std::string& path, std::string password, int n_threads,
              bool strict) {
             construct_nogil(self, path, std::move(password), n_threads, strict);
           },
           nb::arg("path"), nb::arg("password") = "", nb::arg("n_threads") = 0,
           nb::arg("strict") = true)
      .def("set_threads", &mef3io::Reader::set_threads, nb::arg("n"))
      .def("threads", &mef3io::Reader::threads)
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
          "unscaled_channels",
          [](mef3io::Reader& r) { return r.session().unscaled_channels(); },
          "Channels read() or block_envelope() returned raw counts for, from a "
          "segment whose conversion factor is not set (0).")
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
             d["units_conversion_factor"] = opt_obj(ci.units_conversion_factor);
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
                     bool float32, const mef3io::CancelToken* cancel, nb::object progress) {
            // Resolve the Python arguments BEFORE releasing the GIL —
            // nb::cast is Python C API and crashes without a thread state
            // (bit us with float timestamps, e.g. t0 + 10*1e6).
            auto a = opt_time(t0), b = opt_time(t1);
            PyHooks hooks(cancel, std::move(progress));
            if (float32) {
              std::vector<mef3io::sf4> v = hooks.run(
                  [&](const mef3io::Control& ctl) { return r.read_f32(ch, a, b, ctl); });
              return nb::cast(vec_to_numpy(std::move(v)));
            }
            std::vector<mef3io::sf8> v = hooks.run(
                [&](const mef3io::Control& ctl) { return r.read(ch, a, b, ctl); });
            return nb::cast(vec_to_numpy(std::move(v)));
          },
          nb::arg("channel"), nb::arg("t0") = nb::none(), nb::arg("t1") = nb::none(),
          nb::kw_only(), nb::arg("float32") = false, nb::arg("cancel").none() = nb::none(),
          nb::arg("progress") = nb::none())
      .def(
          "read_many",
          [opt_time](mef3io::Reader& r, const std::vector<std::string>& chs, nb::object t0,
                     nb::object t1, bool float32, const mef3io::CancelToken* cancel,
                     nb::object progress) {
            auto a = opt_time(t0), b = opt_time(t1);  // see read: cast needs the GIL
            PyHooks hooks(cancel, std::move(progress));
            // One call for the whole list (#90): concurrent file access, one
            // decode pool, all without the GIL. Row-major (channels x n).
            if (float32) {
              auto v = hooks.run(
                  [&](const mef3io::Control& ctl) { return r.read_many_f32(chs, a, b, ctl); });
              return nb::cast(rows_to_numpy(std::move(v)));
            }
            auto v = hooks.run(
                [&](const mef3io::Control& ctl) { return r.read_many(chs, a, b, ctl); });
            return nb::cast(rows_to_numpy(std::move(v)));
          },
          nb::arg("channels"), nb::arg("t0") = nb::none(), nb::arg("t1") = nb::none(),
          nb::kw_only(), nb::arg("float32") = false, nb::arg("cancel").none() = nb::none(),
          nb::arg("progress") = nb::none())
      .def(
          "read_raw",
          [opt_time](mef3io::Reader& r, const std::string& ch, nb::object t0, nb::object t1,
                     const mef3io::CancelToken* cancel, nb::object progress,
                     std::optional<int> split) {
            auto a = opt_time(t0), b = opt_time(t1);  // see read: cast needs the GIL
            PyHooks hooks(cancel, std::move(progress));
            // `_n_threads` is PRIVATE (MultiReader's per-call share of its one
            // budget, Reader::read_raw_split). Not public API: the public rule
            // is one thread count per object (docs/threads.md).
            mef3io::RawData d = hooks.run([&](const mef3io::Control& ctl) {
              return split ? r.read_raw_split(ch, a, b, *split, ctl) : r.read_raw(ch, a, b, ctl);
            });
            nb::dict out;
            out["start_uutc"] = d.start_uutc;
            out["sampling_frequency"] = d.sampling_frequency;
            out["units_conversion_factor"] = opt_obj(d.units_conversion_factor);
            nb::list spans;
            for (const auto& sp : d.factor_spans) {
              nb::dict e;
              e["start_index"] = sp.start_index;
              e["end_index"] = sp.end_index;
              e["units_conversion_factor"] = sp.units_conversion_factor;
              spans.append(e);
            }
            out["units_conversion_factors"] = spans;
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
          nb::kw_only(), nb::arg("cancel").none() = nb::none(),
          nb::arg("progress") = nb::none(), nb::arg("_n_threads") = nb::none())
      .def("segments",
           [](mef3io::Reader& r, const std::string& ch) {
             std::vector<mef3io::SegmentInfo> segs;
             {
               nb::gil_scoped_release rel;  // metadata only, but every segment of a channel
               segs = r.segments(ch);
             }
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
               d["units_conversion_factor"] = s.units_conversion_factor;
               d["provenance"] = provenance_dict(s.provenance);
               out.append(d);
             }
             return out;
           })
      .def(
          "block_envelope",
          [](mef3io::Reader& r, const std::string& ch, std::optional<std::int64_t> t0,
             std::optional<std::int64_t> t1) {
            mef3io::BlockEnvelope e;
            {
              nb::gil_scoped_release rel;  // reads the .tidx of every segment in the window
              e = r.block_envelope(ch, t0, t1);
            }
            nb::dict d;
            d["start_uutc"] = vec_to_numpy(std::move(e.start_uutc));
            d["end_uutc"] = vec_to_numpy(std::move(e.end_uutc));
            d["min"] = vec_to_numpy(std::move(e.minimum));
            d["max"] = vec_to_numpy(std::move(e.maximum));
            return d;
          },
          nb::arg("channel"), nb::arg("t0").none() = nb::none(), nb::arg("t1").none() = nb::none())
      .def(
          "block_envelope_many",
          [](mef3io::Reader& r, const std::vector<std::string>& chs,
             std::optional<std::int64_t> t0, std::optional<std::int64_t> t1) {
            std::vector<mef3io::BlockEnvelope> es;
            {
              nb::gil_scoped_release rel;  // every listed channel's .tidx, concurrently
              es = r.block_envelope_many(chs, t0, t1);
            }
            nb::list out;
            for (auto& e : es) {
              nb::dict d;
              d["start_uutc"] = vec_to_numpy(std::move(e.start_uutc));
              d["end_uutc"] = vec_to_numpy(std::move(e.end_uutc));
              d["min"] = vec_to_numpy(std::move(e.minimum));
              d["max"] = vec_to_numpy(std::move(e.maximum));
              out.append(d);
            }
            return out;
          },
          nb::arg("channels"), nb::arg("t0").none() = nb::none(),
          nb::arg("t1").none() = nb::none())
      .def("toc",
           [](mef3io::Reader& r, const std::string& ch) {
             std::vector<mef3io::BlockIndexEntry> entries;
             {
               nb::gil_scoped_release rel;  // reads every segment's .tidx
               entries = r.toc(ch);
             }
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
            const auto levels = record_levels(level);  // Python objects: converted with the GIL
            std::vector<mef3io::Record> recs;
            {
              nb::gil_scoped_release rel;  // every record file in scope
              recs = r.records(ch, seg, levels);
            }
            nb::list out;
            for (const auto& rec : recs) out.append(record_to_dict(rec));
            return out;
          },
          nb::arg("channel") = nb::none(), nb::arg("segment") = nb::none(),
          nb::arg("level") = nb::none());
}
