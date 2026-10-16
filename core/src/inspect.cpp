// mef3io — inspect_session: everything about a session without its samples.
//
// File access, per segment: one .tmet read (the same 16 KB the Reader reads
// on open), the sizes of .tmet/.tidx/.tdat, and an existence check for the
// segment's record file. The .tidx is read only with read_index. The .tdat is
// NEVER opened for reading — tests/test_p26_inspect.py pins that with
// /proc/self/io.
#include "mef3io/inspect.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <map>
#include <set>

#include "mef3io/byteio.hpp"
#include "mef3io/crypto.hpp"
#include "mef3io/errors.hpp"
#include "mef3io/metadata.hpp"
#include "mef3io/source.hpp"
#include "mef3io/time.hpp"
#include "mef3io/validate.hpp"
#include "session_layout.hpp"

namespace mef3io {
namespace {

/// A stored time as absolute uUTC, or nullopt when it cannot be known: the
/// NO_ENTRY sentinel, or an offset-applied (negative) time whose offset is
/// unknown. A non-negative time is absolute whatever the offset is.
std::optional<si8> resolve(si8 stored, std::optional<si8> rto) {
  if (stored == fmt::UUTC_NO_ENTRY) return std::nullopt;
  if (stored >= 0) return stored;
  if (!rto) return std::nullopt;
  return to_user_time(stored, *rto);
}

void widen(std::optional<si8>& lo, std::optional<si8>& hi, std::optional<si8> t) {
  if (!t) return;
  lo = lo ? std::min(*lo, *t) : *t;
  hi = hi ? std::max(*hi, *t) : *t;
}

/// Collects identifying fields by name, in first-seen order.
class IdentityCollector {
 public:
  explicit IdentityCollector(bool keep_values) : keep_values_(keep_values) {}

  void add(const std::string& name, const std::string& location, const std::string& value) {
    Entry& e = entry(name, location);
    if (value.empty()) return;
    ++e.field.occurrences;
    e.field.max_length = std::max(e.field.max_length, value.size());
    e.distinct.insert(value);
  }
  void locked(const std::string& name, const std::string& location) {
    ++entry(name, location).field.locked;
  }
  std::vector<IdentityField> finish() {
    std::vector<IdentityField> out;
    for (auto& name : order_) {
      Entry& e = entries_.at(name);
      e.field.distinct = e.distinct.size();
      if (keep_values_) e.field.values.assign(e.distinct.begin(), e.distinct.end());
      out.push_back(std::move(e.field));
    }
    return out;
  }

 private:
  struct Entry {
    IdentityField field;
    std::set<std::string> distinct;
  };
  Entry& entry(const std::string& name, const std::string& location) {
    auto it = entries_.find(name);
    if (it == entries_.end()) {
      order_.push_back(name);
      it = entries_.emplace(name, Entry{}).first;
      it->second.field.name = name;
      it->second.field.location = location;
    }
    return it->second;
  }
  bool keep_values_;
  std::vector<std::string> order_;
  std::map<std::string, Entry> entries_;
};

/// Everything inspect learns about one segment before the summary is built.
struct SegState {
  const detail::LayoutSegment* layout = nullptr;
  std::optional<TimeSeriesMetadata> md;  ///< nullopt: unreadable (lenient)
  /// The offset the segment's times are resolved with (0 = none); nullopt
  /// only when the segment could not be read.
  std::optional<si8> rto;
  /// Section 3 is locked, so `rto` is an ASSUMED 0 — what the Reader uses
  /// too (segment_rto). Right for every file mef3io or pymef wrote; for
  /// meflib's deprecated date masking it gives the masked times. Never
  /// called verified.
  bool rto_assumed = false;
  bool has_records = false;  ///< its .rdat exists (checked once, in the segment pass)
};

/// Gaps in a set of [start, end) spans: anywhere the next span starts more
/// than `tolerance_us` after everything before it has ended.
std::vector<InspectGap> find_gaps(std::vector<std::pair<si8, si8>> spans, double tolerance_us) {
  std::vector<InspectGap> out;
  std::sort(spans.begin(), spans.end());
  std::optional<si8> covered;  // end of everything seen so far
  for (const auto& [s, e] : spans) {
    // In double: two file-derived si8 times can differ by more than si8 holds.
    if (covered && static_cast<double>(s) - static_cast<double>(*covered) > tolerance_us)
      out.push_back(InspectGap{*covered, s});
    covered = covered ? std::max(*covered, e) : e;
  }
  return out;
}

}  // namespace

SessionSummary inspect_session(const std::string& path, const InspectOptions& opt,
                               const Control& ctl) {
  SessionSummary out;
  const auto src = open_session_source(path);  // enforces the naming rules
  const auto layout = detail::discover_layout(*src);

  // Progress (#69): bytes of the files the segment pass reads. Sized only for
  // a listener, so a plain inspect pays nothing for it.
  std::uint64_t progress_total = 0;
  if (ctl.progress) {
    for (const auto& lc : layout.channels)
      for (const auto& ls : lc.segments) {
        try {
          progress_total += src->file_size(ls.tmet);
          if (opt.read_index) progress_total += src->file_size(ls.tidx);
        } catch (const std::exception&) { /* reported by the pass itself */ }
      }
  }
  detail::ProgressReporter progress(ctl, progress_total);
  progress.start();
  std::uint64_t progress_done = 0;

  out.name = src->session_name();
  out.path = path;
  out.layout = path_has_suffix(path, ".tar") ? "tar" : "directory";
  if (out.layout == "tar") out.archive_bytes = std::filesystem::file_size(path);
  out.index_read = opt.read_index;
  out.video_channels = layout.video_channels;
  std::sort(out.video_channels.begin(), out.video_channels.end());
  out.record_only_channels = layout.record_only_channels;
  std::sort(out.record_only_channels.begin(), out.record_only_channels.end());

  IdentityCollector identity(opt.include_values);
  // Fixed order of the identity table, so a field that is never set still
  // appears (with zero occurrences) and the shape never depends on the file.
  for (const char* n : {"subject_name_1", "subject_name_2", "subject_id", "recording_location"})
    identity.add(n, "metadata section 3", "");
  for (const char* n : {"session_description", "channel_description", "reference_description"})
    identity.add(n, "metadata section 2", "");
  for (const char* n : {"session_name", "anonymized_name"})
    identity.add(n, "universal header", "");
  identity.add("record_text", "record bodies", "");

  // Run `fn`, containing a failure when lenient. A wrong password is never
  // contained: it is the caller's mistake, and reporting every segment of the
  // session as "unreadable" would hide that.
  auto contain = [&](const std::string& channel, int segment, const std::string& where,
                     const std::string& kind, auto&& fn) -> std::string {
    if (opt.strict) {
      fn();
      return {};
    }
    try {
      fn();
      return {};
    } catch (const PasswordError&) {
      throw;
    } catch (const std::exception& e) {
      const std::string reason = kind == "records" ? std::string("records: ") + e.what()
                                                   : std::string(e.what());
      out.problems.push_back(SegmentProblem{channel, segment, where, reason, kind});
      return e.what();
    }
  };

  // A .segd missing one of its three files. A Reader skips it silently; here
  // it is listed, unreadable, its files counted, and everything it might hold
  // (identity, an offset) is "unknown" — never "absent".
  std::size_t incomplete_segments = 0;
  auto describe_incomplete = [&](const std::string& channel, const detail::IncompleteSegment& bad,
                                 std::uint64_t& bytes) {
    ++incomplete_segments;
    InspectSegment is;
    is.segment_number = bad.number;
    is.path = src->describe(bad.dir);
    is.readable = false;
    is.problem = "incomplete segment folder: missing " + bad.missing;
    is.time_offset = "unknown";
    is.has_records = bad.has_records;
    for (const auto& f : bad.present) {
      // A half-written folder is where odd entries turn up: a failure to
      // measure one is contained like any other (strict still raises).
      std::uint64_t n = 0;
      contain(channel, bad.number, src->describe(f), "segment", [&] { n = src->file_size(f); });
      if (f.ends_with(".tmet")) is.tmet_bytes = n;
      else if (f.ends_with(".tidx")) is.tidx_bytes = n;
      else if (f.ends_with(".tdat")) is.tdat_bytes = n;
      else is.record_bytes += n;
    }
    bytes += is.tmet_bytes + is.tidx_bytes + is.tdat_bytes + is.record_bytes;
    for (const char* n : {"subject_name_1", "subject_name_2", "subject_id", "recording_location"})
      identity.locked(n, "metadata section 3");
    for (const char* n : {"session_description", "channel_description", "reference_description"})
      identity.locked(n, "metadata section 2");
    for (const char* n : {"session_name", "anonymized_name"})
      identity.locked(n, "universal header");
    if (bad.has_records) identity.locked("record_text", "record bodies");
    return is;
  };

  bool any_ambiguous = false;
  std::string ambiguous_where;
  auto note_ambiguous = [&](std::optional<si8> rto, si8 stored, const std::string& where) {
    if (!rto || *rto == 0 || stored == fmt::UUTC_NO_ENTRY || stored < 0) return;
    if (!any_ambiguous) ambiguous_where = where;
    any_ambiguous = true;
  };

  int worst_access = fmt::LEVEL_2_ACCESS;
  std::set<si8> offsets_seen;           // non-zero offsets of readable segments
  bool zero_offset_seen = false;
  bool offset_plain = false;            // some offset segment keeps section 3 in plain
  std::optional<fmt::MetadataSection3> tz;   // first readable section 3
  bool tz_differs = false;
  std::map<std::string, std::vector<SegState>> states;  // by channel

  // ------------------------------------------------------------------ segments
  std::vector<const detail::LayoutChannel*> chans;
  for (const auto& lc : layout.channels) chans.push_back(&lc);
  std::sort(chans.begin(), chans.end(),
            [](auto* a, auto* b) { return a->name < b->name; });

  for (const auto* lc : chans) {
    InspectChannel ic;
    ic.name = lc->name;
    auto& seg_states = states[lc->name];
    std::vector<std::pair<si8, si8>> spans;
    bool spans_complete = true;
    bool spans_certain = true;  // false: a listed gap may hold an unreadable segment's data
    bool ch_assumed = false, ch_verified_offset = false;  // see the gap computation
    bool n_samples_known = true;
    si8 n_samples = 0;

    for (const auto& ls : lc->segments) {
      ctl.check();  // Cancelled is not a std::exception: contain() never swallows it
      InspectSegment is;
      is.segment_number = ls.number;
      is.path = src->describe(ls.dir());
      SegState st;
      st.layout = &ls;
      std::vector<std::pair<si8, si8>> seg_spans;

      const std::string failure = contain(lc->name, ls.number, src->describe(ls.tmet), "segment", [&] {
        is.tidx_bytes = src->file_size(ls.tidx);
        is.tdat_bytes = src->file_size(ls.tdat);  // size only: never read
        const std::string rdat = ls.rdat();
        if (src->exists(rdat)) {
          is.has_records = true;
          is.record_bytes = src->file_size(rdat);
          const std::string ridx = rdat.substr(0, rdat.size() - 5) + ".ridx";
          if (src->exists(ridx)) is.record_bytes += src->file_size(ridx);
        }

        const auto tmet_bytes = src->read_all(ls.tmet);  // whole file: its size is free
        is.tmet_bytes = tmet_bytes.size();
        auto md = load_time_series_metadata_partial(tmet_bytes, opt.password);
        const auto& uh = md.universal_header;
        // Everything that can still throw comes FIRST, before this segment
        // touches any session-wide tally: in lenient mode a segment that fails
        // part-way must contribute nothing at all.
        // Section 3 locked: assume offset 0, as the Reader does, and flag it.
        const std::optional<si8> seg_rto =
            md.section3_available ? effective_rto(md.section3.recording_time_offset) : 0;
        std::string index_ambiguous;  // first .tidx time stored non-negative under an offset
        bool index_complete = true;
        bool index_offset_dependent = false;  // some .tidx entry time stored negated
        // Block index (opt-in): spans of the blocks, for gaps inside segments.
        if (opt.read_index) {
          const auto idx = src->read_all(ls.tidx);
          if (idx.size() < static_cast<std::size_t>(fmt::UNIVERSAL_HEADER_BYTES) ||
              (idx.size() - fmt::UNIVERSAL_HEADER_BYTES) % fmt::TIME_SERIES_INDEX_BYTES != 0)
            throw FormatError("index file is not a whole number of entries (truncated?): " +
                              src->describe(ls.tidx));
          const std::size_t n =
              (idx.size() - fmt::UNIVERSAL_HEADER_BYTES) / fmt::TIME_SERIES_INDEX_BYTES;
          const sf8 fs = md.section2_available ? md.section2.sampling_frequency : 0.0;
          const std::span<const ui1> all(idx);
          // Legacy writers left .tidx (and .tdat) universal-header times
          // non-negated (CLAUDE.md); under an offset that is ambiguous too.
          // The .tdat header is never checked: inspect never opens a .tdat.
          const auto idx_uh = fmt::UniversalHeader::parse(all);
          for (si8 t : {idx_uh.start_time, idx_uh.end_time})
            if (seg_rto && *seg_rto != 0 && t != fmt::UUTC_NO_ENTRY && t >= 0 &&
                index_ambiguous.empty())
              index_ambiguous = src->describe(ls.tidx);
          for (std::size_t i = 0; i < n; ++i) {
            const auto e = fmt::TimeSeriesIndex::parse(
                all.subspan(fmt::UNIVERSAL_HEADER_BYTES + i * fmt::TIME_SERIES_INDEX_BYTES,
                            fmt::TIME_SERIES_INDEX_BYTES));
            if (e.file_offset < 0 || e.number_of_samples == 0) continue;  // padding, as read_index
            if (seg_rto && *seg_rto != 0 && e.start_time != fmt::UUTC_NO_ENTRY &&
                e.start_time >= 0 && index_ambiguous.empty())
              index_ambiguous = src->describe(ls.tidx);
            if (e.start_time != fmt::UUTC_NO_ENTRY && e.start_time < 0)
              index_offset_dependent = true;
            const auto t = resolve(e.start_time, seg_rto);
            // fs and the count come from the file: a zero, NaN, denormal or
            // absurd value must not reach llround (UB outside si8's range).
            const double dur_us = std::isfinite(fs) && fs > 0 ? e.number_of_samples * 1e6 / fs : -1.0;
            if (!t || !(dur_us >= 0 && dur_us < 9e18)) {
              index_complete = false;
              continue;
            }
            seg_spans.emplace_back(*t, saturating_add(*t, static_cast<si8>(std::llround(dur_us))));
          }
        }
        if (!index_complete) spans_complete = spans_certain = false;
        if (!index_ambiguous.empty()) note_ambiguous(seg_rto, 0, index_ambiguous);

        const bool encrypted =
            md.section1.section_2_encryption > 0 || md.section1.section_3_encryption > 0;
        if (encrypted || uh.is_password_protected()) out.encryption.encrypted = true;
        out.encryption.section2_level =
            std::max<int>(out.encryption.section2_level, md.section1.section_2_encryption);
        out.encryption.section3_level =
            std::max<int>(out.encryption.section3_level, md.section1.section_3_encryption);
        worst_access = std::min(worst_access, encrypted ? md.access_level : fmt::LEVEL_2_ACCESS);
        is.section2_available = md.section2_available;
        is.section3_available = md.section3_available;
        if (!md.section2_available) ++out.encryption.section2_locked;
        if (!md.section3_available) ++out.encryption.section3_locked;

        // The offset: section 3's, when it can be read. Locked: ASSUMED 0, as
        // the Reader does (segment_rto) — flagged wherever a time depended on
        // it, never called verified.
        if (md.section3_available) {
          st.rto = effective_rto(md.section3.recording_time_offset);
          is.time_offset = *st.rto != 0 ? "applied" : "none";
          if (*st.rto != 0) {
            offsets_seen.insert(*st.rto);
            if (md.section1.section_3_encryption <= 0) offset_plain = true;
            if (opt.include_values) is.recording_time_offset = *st.rto;
          } else {
            zero_offset_seen = true;
          }
          if (!tz) {
            tz = md.section3;
          } else if (tz->gmt_offset != md.section3.gmt_offset ||
                     tz->dst_start_time != md.section3.dst_start_time ||
                     tz->dst_end_time != md.section3.dst_end_time) {
            tz_differs = true;
          }
        } else {
          is.time_offset = "locked";
          st.rto = 0;
          st.rto_assumed = true;
          // Flag only what the assumption touched: a time stored negated
          // (offset applied). Non-negative times are absolute regardless.
          auto dep = [](si8 t) { return t != fmt::UUTC_NO_ENTRY && t < 0; };
          is.offset_assumed_zero =
              dep(uh.start_time) || dep(uh.end_time) || index_offset_dependent;
        }

        is.start_time = resolve(uh.start_time, st.rto);
        is.end_time = resolve(uh.end_time, st.rto);
        note_ambiguous(st.rto, uh.start_time, src->describe(ls.tmet));
        note_ambiguous(st.rto, uh.end_time, src->describe(ls.tmet));

        if (md.section2_available) {
          const auto& s2 = md.section2;
          is.start_sample = s2.start_sample;
          is.number_of_samples = s2.number_of_samples;
          is.number_of_blocks = s2.number_of_blocks;
          is.number_of_discontinuities = s2.number_of_discontinuities;
          is.unset_declarations = unset_declarations(s2);
        }
        is.provenance = fmt::provenance::read(uh);

        // Identity: universal header (always plain), sections 2 and 3 when open.
        identity.add("session_name", "universal header", uh.session_name);
        identity.add("anonymized_name", "universal header", uh.anonymized_name);
        if (md.section2_available) {
          identity.add("session_description", "metadata section 2", md.section2.session_description);
          identity.add("channel_description", "metadata section 2", md.section2.channel_description);
          identity.add("reference_description", "metadata section 2",
                       md.section2.reference_description);
        } else {
          for (const char* n : {"session_description", "channel_description", "reference_description"})
            identity.locked(n, "metadata section 2");
        }
        if (md.section3_available) {
          identity.add("subject_name_1", "metadata section 3", md.section3.subject_name_1);
          identity.add("subject_name_2", "metadata section 3", md.section3.subject_name_2);
          identity.add("subject_id", "metadata section 3", md.section3.subject_id);
          identity.add("recording_location", "metadata section 3", md.section3.recording_location);
        } else {
          for (const char* n : {"subject_name_1", "subject_name_2", "subject_id", "recording_location"})
            identity.locked(n, "metadata section 3");
        }
        st.md = std::move(md);
      });

      if (!failure.empty()) {
        // Lenient: the segment is listed, unreadable, and contributes nothing
        // else — a half-described segment is worse than a skipped one.
        InspectSegment bad;
        bad.segment_number = is.segment_number;
        bad.path = is.path;
        bad.readable = false;
        bad.problem = failure;
        bad.time_offset = "unknown";
        // Sizes measured before the failure are facts about the disk, so they
        // still count; nothing READ from the segment does.
        bad.tmet_bytes = is.tmet_bytes;  // set once read, before the parse can throw
        if (bad.tmet_bytes == 0) {         // the read itself failed: measure it now
          try {
            bad.tmet_bytes = src->file_size(ls.tmet);
          } catch (const std::exception&) {
            // unmeasurable too; the problem is already reported
          }
        }
        bad.tidx_bytes = is.tidx_bytes;
        bad.tdat_bytes = is.tdat_bytes;
        bad.record_bytes = is.record_bytes;
        bad.has_records = is.has_records;
        ic.bytes += bad.tmet_bytes + bad.tidx_bytes + bad.tdat_bytes + bad.record_bytes;
        // Its identity fields were never read: "unknown", not "absent".
        for (const char* n : {"subject_name_1", "subject_name_2", "subject_id", "recording_location"})
          identity.locked(n, "metadata section 3");
        for (const char* n : {"session_description", "channel_description", "reference_description"})
          identity.locked(n, "metadata section 2");
        for (const char* n : {"session_name", "anonymized_name"})
          identity.locked(n, "universal header");
        if (is.has_records) identity.locked("record_text", "record bodies");
        ic.segments.push_back(std::move(bad));
        seg_states.push_back(SegState{&ls, std::nullopt, std::nullopt});
        spans_complete = false;
        spans_certain = false;
        n_samples_known = false;
        continue;
      }

      progress_done += is.tmet_bytes + (opt.read_index ? is.tidx_bytes : 0);
      progress.advance_to(progress_done);
      const auto& md = *st.md;
      if (md.section2_available) {
        if (!ic.sampling_frequency) {
          ic.sampling_frequency = md.section2.sampling_frequency;
          ic.units_conversion_factor = md.section2.units_conversion_factor;
          ic.units_description = md.section2.units_description;
        }
        // File-derived: a negative count is nonsense, and the sum must not overflow.
        if (md.section2.number_of_samples < 0)
          n_samples_known = false;
        else
          n_samples = saturating_add(n_samples, md.section2.number_of_samples);
        for (const auto& f : is.unset_declarations)
          out.declaration_issues.push_back({lc->name, ls.number, f});
      } else {
        n_samples_known = false;
      }
      widen(ic.start_time, ic.end_time, is.start_time);
      widen(ic.start_time, ic.end_time, is.end_time);
      if (is.offset_assumed_zero) ic.offset_assumed_zero = out.offset_assumed_zero = true;
      if (is.offset_assumed_zero) ch_assumed = true;  // a time actually assumed 0
      else if (st.rto && *st.rto != 0) ch_verified_offset = true;
      if (opt.read_index) {
        spans.insert(spans.end(), seg_spans.begin(), seg_spans.end());
      } else if (is.start_time && is.end_time) {
        spans.emplace_back(*is.start_time, *is.end_time);
      } else {
        spans_complete = false;
        spans_certain = false;  // a segment we cannot place may fill a "gap"
      }
      ic.bytes += is.tmet_bytes + is.tidx_bytes + is.tdat_bytes + is.record_bytes;
      st.has_records = is.has_records;
      seg_states.push_back(std::move(st));
      ic.segments.push_back(std::move(is));
    }

    for (const auto& bad : lc->incomplete) {
      ic.segments.push_back(describe_incomplete(lc->name, bad, ic.bytes));
      seg_states.push_back(SegState{nullptr, std::nullopt, std::nullopt});  // offset unknown
      spans_complete = spans_certain = false;  // its index was never read
      n_samples_known = false;
    }
    std::stable_sort(ic.segments.begin(), ic.segments.end(),
                     [](const InspectSegment& a, const InspectSegment& b) {
                       return a.segment_number < b.segment_number;
                     });

    // A locked segment placed with an assumed 0 next to one with a READ
    // non-zero offset: the two sit up to that offset apart on the time axis,
    // so a "gap" between them may be the assumption, not the recording.
    if (ch_assumed && ch_verified_offset) spans_complete = spans_certain = false;

    if (n_samples_known) ic.number_of_samples = n_samples;
    if (ic.sampling_frequency && *ic.sampling_frequency > 0) {
      const double period = 1e6 / *ic.sampling_frequency;
      // From the index, block ends are exact (start + n/fs), so anything past
      // half a sample period is a gap. Between segments, writers disagree on
      // whether the universal-header end is the last sample or just past it,
      // so a gap there must exceed one and a half periods to count.
      ic.gaps = find_gaps(std::move(spans), opt.read_index ? 0.5 * period : 1.5 * period);
      ic.gaps_complete = opt.read_index && spans_complete;
      ic.gaps_certain = spans_certain;
    }
    out.total_bytes += ic.bytes;
    widen(out.start_time, out.end_time, ic.start_time);
    widen(out.start_time, out.end_time, ic.end_time);
    out.channels.push_back(std::move(ic));
  }

  // Channel folders without one complete segment: no channel to describe,
  // but their files are counted and their segments' offsets are unknown.
  for (const auto& lc : layout.incomplete_channels) {
    if (lc.incomplete.empty()) continue;  // just records, or an empty folder
    out.incomplete_channels.push_back(lc.name);
    for (const auto& bad : lc.incomplete) {
      describe_incomplete(lc.name, bad, out.total_bytes);
      states[lc.name].push_back(SegState{nullptr, std::nullopt, std::nullopt});
    }
  }
  std::sort(out.incomplete_channels.begin(), out.incomplete_channels.end());

  // ------------------------------------------------------------------ offsets
  // The offset a channel- or session-level record file is read with: the one
  // every readable segment of the scope agrees on (Session::agreed_rto); a
  // locked one takes part with its assumed 0. A disagreeing or unreadable
  // segment makes it unknown; `why` says which.
  auto agreed = [&](const std::string* channel, std::string& why,
                    bool& assumed) -> std::optional<si8> {
    std::optional<si8> value;
    bool any = false, unreadable = false;
    assumed = false;
    for (const auto& [name, segs] : states) {
      if (channel && name != *channel) continue;
      for (const auto& st : segs) {
        if (!st.md) {  // unreadable: its offset is unknown, so the scope's is too
          unreadable = true;
          continue;
        }
        // A locked section 3 counts as its assumed 0, exactly as the Reader's
        // segment_rto does: against a readable non-zero offset that is a
        // dispute, never a silent choice.
        if (st.rto_assumed) assumed = true;
        if (any && *value != *st.rto) {
          why = "segments disagree on the recording-time offset";
          return std::nullopt;
        }
        value = st.rto;
        any = true;
      }
    }
    // A segment that could not be read may carry a different offset, so the
    // readable ones cannot speak for the scope: unknown, never guessed. Only a
    // scope with no segments at all has nothing to disagree with.
    if (unreadable) {
      why = "a segment of this scope could not be read, so its recording-time offset is unknown";
      return std::nullopt;
    }
    return any ? value : std::optional<si8>(0);
  };

  // ------------------------------------------------------------------ records
  struct Unplaced {
    std::string file, why;
  };
  std::vector<Unplaced> unplaced;
  std::vector<std::string> unread_root_records;  // .rdat at the root that are not the session's
  std::size_t records_failed = 0;

  auto read_file = [&](const std::string& rdat, std::optional<si8> rto, bool rto_assumed,
                       const std::string& why, RecordLevel level, const std::string& channel,
                       int segment) {
    ctl.check();
    std::vector<InspectRecord> got;
    std::vector<std::string> texts;  // each record's free text, for the identity table
    std::uint64_t file_bytes = 0;
    fmt::UniversalHeader uh;
    bool placeable = true, ambiguous = false, offset_dependent = false;
    const std::string failure =
        contain(channel, segment, src->describe(rdat), "records", [&] {
          file_bytes = src->file_size(rdat);
          const std::string ridx = rdat.substr(0, rdat.size() - 5) + ".ridx";
          if (src->exists(ridx)) file_bytes += src->file_size(ridx);

          const auto raw = src->read_all(rdat);
          uh = fmt::UniversalHeader::parse(raw);
          const auto keys = crypto::validate_password(opt.password,
                                                      uh.level_1_password_validation_field,
                                                      uh.level_2_password_validation_field);
          if (!opt.password.empty() && uh.is_password_protected() &&
              keys.access_level == fmt::LEVEL_0_ACCESS)
            throw PasswordError("record file is encrypted and the password is incorrect: " +
                                src->describe(rdat));
          RecordParseInfo info;
          info.skip_locked_bodies = true;
          auto recs = parse_records(raw, rto.value_or(0), keys, info);
          // NOTHING above may touch a session-wide tally: it can all throw,
          // and a file that failed must contribute nothing but its size.
          // Placed only if the times do not depend on an offset we lack.
          placeable = rto.has_value() || !info.offset_dependent;
          offset_dependent = info.offset_dependent;
          ambiguous = rto && *rto != 0 && info.header_time_non_negative;
          std::set<std::size_t> locked(info.locked.begin(), info.locked.end());
          for (std::size_t i = 0; i < recs.size(); ++i) {
            const Record& r = recs[i];
            InspectRecord ir;
            ir.level = level;
            ir.channel = channel;
            ir.segment = segment;
            ir.type = r.type;
            ir.body_locked = locked.count(i) != 0;
            ir.duration = r.duration;

            std::size_t text = 0;
            std::string joined;
            for (const auto& f : record_fields(r)) {
              if (!f.value) continue;
              if (const auto* s = std::get_if<std::string>(&*f.value)) {
                text += s->size();
                if (!s->empty()) joined += (joined.empty() ? "" : "\n") + *s;
              }
            }
            ir.text_bytes = text;
            texts.push_back(std::move(joined));
            if (placeable) {
              ir.time = resolve(r.time, 0);  // already absolute; drops NO_ENTRY
              std::optional<si8> lo, hi;
              // Body times at or below 0 are sentinels (-1, NO_ENTRY), not times.
              auto body = [&](const std::optional<si8>& t) {
                if (t && *t > 0) widen(lo, hi, *t);
              };
              body(ir.time);
              body(r.earliest_onset);
              body(r.latest_offset);
              for (const auto& c : r.seizure_channels) {
                body(c.onset);
                body(c.offset);
              }
              body(r.timestamp);
              body(r.end_timestamp);
              body(r.trace_timestamp);
              if (ir.time && r.duration && *r.duration > 0)
                body(saturating_add(*ir.time, *r.duration));
              ir.first_time = lo;
              ir.last_time = hi;
            }
            ir.times_known = placeable;
            if (opt.include_values) {
              ir.full = r;
              ir.full->level = level;
              ir.full->channel = channel;
              ir.full->segment = segment;
              if (!placeable) {
                // The parse resolved these with offset 0 as a placeholder.
                // Never hand that out: the full record must not carry a date
                // the summary itself refuses to state.
                ir.full->time = fmt::UUTC_NO_ENTRY;  // shown as None
                ir.full->earliest_onset.reset();
                ir.full->latest_offset.reset();
                ir.full->timestamp.reset();
                ir.full->end_timestamp.reset();
                ir.full->trace_timestamp.reset();
                for (auto& c : ir.full->seizure_channels) c.onset = c.offset = -1;  // NO_ENTRY
              }
            }
            got.push_back(std::move(ir));
          }
        });
    // Its size is a fact about the disk, whether or not it could be read.
    if (level != RecordLevel::Segment) out.total_bytes += file_bytes;  // segments counted above
    if (level == RecordLevel::Channel)
      for (auto& ic : out.channels)
        if (ic.name == channel) ic.bytes += file_bytes;
    if (!failure.empty()) {
      ++records_failed;
      // Never read: its identity fields are unknown, not absent.
      identity.locked("session_name", "universal header");
      identity.locked("anonymized_name", "universal header");
      identity.locked("record_text", "record bodies");
      return;
    }
    identity.add("session_name", "universal header", uh.session_name);
    identity.add("anonymized_name", "universal header", uh.anonymized_name);
    if (!placeable) unplaced.push_back({src->describe(rdat), why});
    if (ambiguous) {
      if (!any_ambiguous) ambiguous_where = src->describe(rdat);
      any_ambiguous = true;
    }
    for (std::size_t i = 0; i < got.size(); ++i) {
      if (got[i].body_locked) {
        ++out.encryption.record_bodies_locked;
        out.encryption.encrypted = true;
        identity.locked("record_text", "record bodies");
      } else if (!texts[i].empty()) {
        identity.add("record_text", "record bodies", texts[i]);
      }
    }
    // Placed with an assumed offset: flag what actually depended on it.
    const bool assumed = rto_assumed && offset_dependent && placeable;
    if (assumed) out.offset_assumed_zero = true;
    for (auto& ir : got) {
      ir.offset_assumed_zero = assumed;
      if (assumed && !ir.channel.empty())
        for (auto& ic : out.channels)
          if (ic.name == ir.channel) ic.offset_assumed_zero = true;
      widen(out.records_start_time, out.records_end_time, ir.first_time);
      widen(out.records_start_time, out.records_end_time, ir.last_time);
      if (!ir.channel.empty())
        for (auto& ic : out.channels)
          if (ic.name == ir.channel) ++ic.records;
      out.records.push_back(std::move(ir));
    }
  };

  {  // session level
    std::string ambiguity;
    const std::string rdat = detail::session_record_file(*src, ambiguity);
    if (!ambiguity.empty()) {
      contain("", -1, src->describe(""), "records", [&] { throw FormatError(ambiguity); });
      ++records_failed;
    }
    // Root record files other than the session's own: not read (meflib and
    // the Reader ignore them too), but on disk and possibly holding identity,
    // so they are counted, their fields reported unknown, and warned about.
    const std::string chosen_ridx = rdat.empty() ? "" : rdat.substr(0, rdat.size() - 5) + ".ridx";
    for (const auto& e : src->list_dir("")) {
      if (e.is_dir || !(e.name.ends_with(".rdat") || e.name.ends_with(".ridx"))) continue;
      if (e.name == rdat || e.name == chosen_ridx) continue;
      contain("", -1, src->describe(e.name), "records",
              [&] { out.total_bytes += src->file_size(e.name); });
      if (e.name.ends_with(".rdat")) {
        unread_root_records.push_back(e.name);
        identity.locked("session_name", "universal header");
        identity.locked("anonymized_name", "universal header");
        identity.locked("record_text", "record bodies");
      }
    }
    if (!rdat.empty()) {
      std::string why;
      bool assumed = false;
      const auto rto = agreed(nullptr, why, assumed);
      read_file(rdat, rto, assumed, why, RecordLevel::Session, "", -1);
    }
  }
  std::vector<std::string> record_channels;
  for (const auto* lc : chans) record_channels.push_back(lc->name);
  record_channels.insert(record_channels.end(), out.record_only_channels.begin(),
                         out.record_only_channels.end());
  std::sort(record_channels.begin(), record_channels.end());
  for (const auto& name : record_channels) {
    const std::string cand = name + ".timd/" + name + ".rdat";
    if (src->exists(cand)) {
      std::string why;
      // A channel with NO segments uses the session's offset, as the writer
      // did (SessionWriter::resolve_offset). One whose segments exist but
      // failed does not: agreed() says unknown for it.
      const auto st_it = states.find(name);
      const bool has_segments = st_it != states.end() && !st_it->second.empty();
      bool assumed = false;
      const auto rto = has_segments ? agreed(&name, why, assumed) : agreed(nullptr, why, assumed);
      read_file(cand, rto, assumed, why, RecordLevel::Channel, name, -1);
    }
    auto it = states.find(name);
    if (it == states.end()) continue;
    for (const auto& st : it->second) {
      if (!st.md) continue;
      const std::string rdat = st.layout->rdat();
      if (!st.has_records) continue;  // checked once, in the segment pass
      read_file(rdat, st.rto, st.rto_assumed, "", RecordLevel::Segment, name,
                st.layout->number);
    }
  }
  ctl.check();
  progress.finish();  // the reading is done; what follows only summarises
  widen(out.first_time, out.last_time, out.start_time);
  widen(out.first_time, out.last_time, out.end_time);
  widen(out.first_time, out.last_time, out.records_start_time);
  widen(out.first_time, out.last_time, out.records_end_time);

  // ------------------------------------------------------------- summaries
  out.encryption.access = worst_access >= fmt::LEVEL_2_ACCESS
                              ? "full"
                              : (worst_access == fmt::LEVEL_1_ACCESS ? "level-1" : "none");
  if (out.encryption.record_bodies_locked && out.encryption.access == "full")
    out.encryption.access = "level-1";
  if (tz) {
    if (tz->gmt_offset != fmt::GMT_OFFSET_NO_ENTRY) out.gmt_offset = tz->gmt_offset;
    if (tz->dst_start_time != fmt::UUTC_NO_ENTRY) out.dst_start_time = tz->dst_start_time;
    if (tz->dst_end_time != fmt::UUTC_NO_ENTRY) out.dst_end_time = tz->dst_end_time;
  }

  auto& to = out.time_offset;
  to.segments_locked = out.encryption.section3_locked;
  for (const auto& [name, segs] : states)
    for (const auto& st : segs)
      if (!st.md) ++to.segments_unknown;
  for (const auto& [name, segs] : states)
    for (const auto& st : segs)
      if (st.rto && *st.rto != 0) ++to.segments_with_offset;
  // A locked segment counts as its assumed 0 — the rule agreed() and the
  // Reader follow — so next to a read non-zero offset that is a dispute too.
  if (offsets_seen.size() > 1 ||
      (!offsets_seen.empty() && (zero_offset_seen || to.segments_locked)))
    to.status = "disputed";
  else if (!offsets_seen.empty())
    to.status = "applied";
  else if (to.segments_locked)
    to.status = "locked";
  else if (to.segments_unknown && !zero_offset_seen)
    to.status = "unknown";
  to.ambiguous = any_ambiguous;
  if (!offsets_seen.empty()) {
    to.date_recoverable = true;
    to.recoverable_by = offset_plain ? "anyone" : "level-2 password";
    if (opt.include_values && to.status == "applied") to.value = *offsets_seen.begin();
  }
  if (to.status == "none") {
    to.interpretation =
        "No recording-time offset. Stored times are absolute UTC (negated on disk as meflib's "
        "marker) and are reported as they are.";
  } else if (to.status == "applied") {
    to.interpretation =
        "meflib's recording-time offset is set (deprecated as anonymization). Stored times "
        "count from the offset and were converted back to true UTC. The real recording date "
        "is stored in metadata section 3 " +
        std::string(offset_plain ? "in plain text: anyone can recover it."
                                 : "encrypted at level 2: anyone with the level-2 password "
                                   "can recover it.");
  } else if (to.status == "disputed") {
    to.interpretation =
        "Segments disagree on the recording-time offset (a segment with section 3 locked counts "
        "as its assumed 0). Each segment's times were resolved with its own offset; channel- and session-level records that depend on it cannot be "
        "placed.";
  } else if (to.status == "unknown") {
    to.interpretation =
        "No segment could be read, so any recording-time offset is unknown. Times stored "
        "relative to it are reported as unknown rather than guessed.";
  } else {
    to.interpretation =
        std::string(
            "Metadata section 3 is encrypted and the password does not open it, so the "
            "recording-time offset cannot be read.") +
        (out.offset_assumed_zero
             ? " Times stored relative to it were resolved ASSUMING offset 0, as the Reader does "
               "(offset_assumed_zero): right for every session mef3io or pymef wrote, which "
               "always use 0; under meflib's deprecated date masking they are the masked times, "
               "which is what meflib meant a level-1 user to see. They are not verified."
             : " No time reported here depends on it: the stored times are absolute.");
  }
  if (to.segments_unknown)
    to.interpretation += " " + std::to_string(to.segments_unknown) +
                         " segment(s) could not be read; whatever offset they carry is unknown, "
                         "so this description covers the readable ones only.";
  if (to.segments_locked && to.status != "locked")
    to.interpretation += " " + std::to_string(to.segments_locked) +
                         " segment(s) keep section 3 locked" +
                         (out.offset_assumed_zero
                              ? std::string("; times that depend on their offset assume 0 and "
                                            "are not verified.")
                              : std::string("."));

  out.identity = identity.finish();

  // ------------------------------------------------------------- warnings
  auto warn = [&](std::string code, std::string message, bool blocks) {
    out.warnings.push_back(InspectWarning{std::move(code), std::move(message), blocks});
  };
  if (!out.video_channels.empty()) {
    std::string list;
    for (const auto& v : out.video_channels) list += (list.empty() ? "" : ", ") + v;
    warn("video-channels",
         std::to_string(out.video_channels.size()) +
             " video channel(s) present (" + list +
             "). mef3io does not read video; they are listed but not described.",
         true);
  }
  std::size_t bad_segments = 0;
  for (const auto& p : out.problems)
    if (p.kind == "segment") ++bad_segments;
  if (bad_segments)
    warn("unreadable-segments",
         std::to_string(bad_segments) +
             " segment(s) could not be read; see problems. Their spans are not described.",
         true);
  if (incomplete_segments)
    warn("incomplete-segments",
         std::to_string(incomplete_segments) +
             " segment folder(s) lack one of .tmet / .tidx / .tdat" +
             (out.incomplete_channels.empty()
                  ? std::string()
                  : " (channel folder(s) with no complete segment: " +
                        std::to_string(out.incomplete_channels.size()) + ")") +
             ". A Reader skips them silently; they are listed as unreadable and their files "
             "counted, and what they may hold is reported as unknown.",
         true);
  if (!unread_root_records.empty()) {
    std::string list;
    for (const auto& n : unread_root_records) list += (list.empty() ? "" : ", ") + n;
    warn("unread-record-files",
         std::to_string(unread_root_records.size()) + " record file(s) at the session root are "
             "not the session's own and were not read (" + list + "). meflib and the Reader "
             "ignore them too, but they may hold annotation text.",
         true);
  }
  if (records_failed)
    warn("unreadable-records",
         std::to_string(records_failed) + " record file(s) could not be read; see problems.",
         true);
  if (out.encryption.section2_locked || out.encryption.section3_locked ||
      out.encryption.record_bodies_locked)
    warn("locked",
         "Part of the session is encrypted and the password does not open it (section 2 locked "
         "in " + std::to_string(out.encryption.section2_locked) + " segment(s), section 3 in " +
             std::to_string(out.encryption.section3_locked) + ", " +
             std::to_string(out.encryption.record_bodies_locked) +
             " record bod(ies)). Those values are reported as locked, not guessed" +
             (out.offset_assumed_zero ? std::string(" (times: see offset-assumed-zero)")
                                      : std::string()) +
             "; the level-2 password opens everything.",
         true);
  if (to.status == "applied" || to.status == "disputed")
    warn("legacy-time-offset",
         "This session uses meflib's recording-time offset, which is deprecated as "
         "anonymization: the real recording date is stored in metadata section 3 and can be "
         "recovered by " + std::string(offset_plain ? "anyone" : "anyone with the level-2 password") +
             ". Times read with that offset are the true UTC times" +
             (out.offset_assumed_zero ? std::string(" (not those that assume 0; see "
                                                    "offset-assumed-zero)")
                                      : std::string()) +
             ". Treat the session as identifiable.",
         false);
  if (to.status == "disputed")
    warn("time-offset-disputed",
         "Segments disagree on the recording-time offset; each segment was read with its own.",
         true);
  if (to.ambiguous)
    warn("ambiguous-time-convention",
         "A time is stored non-negative although the recording-time offset is set (first: " +
             ambiguous_where +
             "). Such a value cannot be told apart from an absolute time, so where it really "
             "lies is uncertain.",
         true);
  if (out.offset_assumed_zero) {
    std::size_t assumed_segments = 0, assumed_records = 0;
    for (const auto& ic : out.channels)
      for (const auto& is : ic.segments) assumed_segments += is.offset_assumed_zero;
    for (const auto& r : out.records) assumed_records += r.offset_assumed_zero;
    warn("offset-assumed-zero",
         "Metadata section 3 is locked, so the recording-time offset cannot be read. The times "
         "of " + std::to_string(assumed_segments) + " segment(s) and " +
             std::to_string(assumed_records) +
             " record(s) depend on it and assume offset 0, as the Reader does: right for "
             "sessions mef3io and pymef wrote, the masked times under meflib's deprecated date "
             "masking. Not verified; the level-2 password resolves them.",
         true);
  }
  bool times_unknown = false;
  for (const auto& ic : out.channels)
    for (const auto& is : ic.segments)
      if (is.readable && (!is.start_time || !is.end_time)) times_unknown = true;
  if (times_unknown || !unplaced.empty()) {
    std::string msg = "Some times could not be resolved because the recording-time offset they "
                      "depend on is unknown";
    if (!unplaced.empty())
      msg += "; " + std::to_string(unplaced.size()) + " record file(s) have no times (first: " +
             unplaced.front().file + ": " + unplaced.front().why + ")";
    warn("times-unresolved", msg + ". They are reported as unknown rather than guessed.", true);
  }
  if (!out.declaration_issues.empty())
    warn("unset-declarations",
         std::to_string(out.declaration_issues.size()) +
             " section-2 size declaration(s) are unset. mef3io reads the data regardless; "
             "meflib-based readers allocate from them. See declaration_issues.",
         false);
  if (tz_differs)
    warn("time-zone-fields-differ",
         "Segments disagree on GMT_offset or the DST fields; the first segment's are reported.",
         false);
  return out;
}

}  // namespace mef3io
