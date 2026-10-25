// Internal (not installed): how a session tree is laid out, shared by the
// Session (session.cpp) and inspect_session (inspect.cpp) so the two can never
// disagree on which folders are channels, which are segments, what a
// segment's number is, or which file holds the session-level records.
#pragma once

#include <algorithm>
#include <exception>
#include <string>
#include <vector>

#include "mef3io/parallel.hpp"
#include "mef3io/source.hpp"

namespace mef3io::detail {

/// "<channel>-000000" -> 0. Session's historical rule (std::stoi of what
/// follows the last '-', 0 when there is none or it does not parse); kept as
/// is so segment numbers stay what Reader.segments() has always reported.
inline int layout_segment_number(const std::string& stem) {
  auto dash = stem.rfind('-');
  if (dash == std::string::npos) return 0;
  try {
    return std::stoi(stem.substr(dash + 1));
  } catch (...) {
    return 0;
  }
}

struct LayoutSegment {
  int number = 0;
  std::string tmet, tidx, tdat;  ///< source-relative paths
  /// `<seg>.segd/<seg>.rdat` — where its segment-level records would be
  /// (meflib.c:6030); not checked for existence here.
  std::string rdat() const { return tmet.substr(0, tmet.size() - 5) + ".rdat"; }
  std::string dir() const { return tmet.substr(0, tmet.rfind('/')); }
};

/// A .segd folder missing one of its three files. The Session skips it
/// silently (as it always has); inspect_session reports it.
struct IncompleteSegment {
  int number = 0;
  std::string dir;                        ///< source-relative .segd path
  std::vector<std::string> present;       ///< source-relative paths of the files that exist
  std::string missing;                    ///< e.g. ".tdat" or ".tmet, .tidx"
  bool has_records = false;               ///< its .rdat exists
};

struct LayoutChannel {
  std::string name;
  std::vector<LayoutSegment> segments;  ///< sorted by number
  std::vector<IncompleteSegment> incomplete;  ///< sorted by number
  /// `<ch>.timd/<ch>.rdat` (meflib.c:5660); not checked for existence here.
  std::string rdat() const { return name + ".timd/" + name + ".rdat"; }
};

struct SessionLayout {
  /// Channels with at least one complete segment (.tmet + .tidx + .tdat), in
  /// listing order (callers sort by name).
  std::vector<LayoutChannel> channels;
  /// Channels with records but no segment yet: a channel annotated before its
  /// data arrives (mef3io's own write_annotations(..., "chNew") makes one;
  /// pymef reads it). Listing order.
  std::vector<std::string> record_only_channels;
  /// `.vidd` folders at the root. Video is out of scope for mef3io; the
  /// Session skips them silently, inspect reports them.
  std::vector<std::string> video_channels;
  /// `.timd` folders with no complete segment (their incomplete segments in
  /// `incomplete`), whether or not they also count as record-only. The
  /// Session ignores them; inspect reports them.
  std::vector<LayoutChannel> incomplete_channels;
};

/// Walk the root and every `.timd`. One listing per channel; three existence
/// checks per segment, exactly what the Session always did.
///
/// `threads` (the object's n_threads; 0 = automatic, resolved for file
/// access) runs the per-channel listings and the per-segment existence checks
/// on a pool: on a network share each is a round trip. The RESULT and the
/// ERROR do not depend on it: every list is assembled in listing order, and a
/// failure is rethrown as a serial walk meets it (channel by channel in root
/// listing order: its listing, then its segment folders in listing order,
/// then its record-only check) — never whichever thread failed first. The
/// same requests are made either way (more only when something fails).
inline SessionLayout discover_layout(const SessionSource& src, int threads = 1) {
  SessionLayout out;
  std::vector<std::string> timd;  // channel folder names, in root listing order
  for (const auto& entry : src.list_dir("")) {
    if (!entry.is_dir) continue;
    if (entry.name.ends_with(".vidd")) {
      out.video_channels.push_back(entry.name.substr(0, entry.name.size() - 5));
      continue;
    }
    if (entry.name.ends_with(".timd")) timd.push_back(entry.name);
  }

  // 1. List every channel folder.
  std::vector<std::vector<std::string>> segd(timd.size());  // .segd names, listing order
  const auto list_err = parallel_try_each(
      timd.size(), threads,
      [&](std::size_t c) {
        for (const auto& seg_entry : src.list_dir(timd[c]))
          if (seg_entry.is_dir && seg_entry.name.ends_with(".segd"))
            segd[c].push_back(seg_entry.name);
      },
      Work::Io);

  // 2. Check every segment folder's files, all channels at once (a session of
  // one channel and thousands of segments must not run serially).
  struct SegWork {
    std::size_t channel;
    LayoutSegment seg;
    bool complete = false;
    IncompleteSegment bad;
  };
  std::vector<SegWork> work;
  std::vector<std::size_t> first(timd.size() + 1, 0);  // channel c: work[first[c], first[c+1])
  for (std::size_t c = 0; c < timd.size(); ++c) {
    first[c] = work.size();
    if (list_err[c]) continue;
    for (const auto& name : segd[c]) {
      const std::string stem = name.substr(0, name.size() - 5);
      const std::string dir = timd[c] + "/" + name;
      SegWork w;
      w.channel = c;
      w.seg.number = layout_segment_number(stem);
      w.seg.tmet = dir + "/" + stem + ".tmet";
      w.seg.tidx = dir + "/" + stem + ".tidx";
      w.seg.tdat = dir + "/" + stem + ".tdat";
      w.bad.number = w.seg.number;
      w.bad.dir = dir;
      work.push_back(std::move(w));
    }
  }
  first[timd.size()] = work.size();
  const auto seg_err = parallel_try_each(
      work.size(), threads,
      [&](std::size_t i) {
        SegWork& w = work[i];
        const bool has[3] = {src.exists(w.seg.tmet), src.exists(w.seg.tidx),
                             src.exists(w.seg.tdat)};
        if (has[0] && has[1] && has[2]) {
          w.complete = true;
          return;
        }
        const std::string* files[3] = {&w.seg.tmet, &w.seg.tidx, &w.seg.tdat};
        const char* exts[3] = {".tmet", ".tidx", ".tdat"};
        for (int k = 0; k < 3; ++k) {
          if (has[k]) {
            w.bad.present.push_back(*files[k]);
          } else {
            w.bad.missing += (w.bad.missing.empty() ? "" : ", ") + std::string(exts[k]);
          }
        }
        const std::string base = w.seg.tmet.substr(0, w.seg.tmet.size() - 5);
        for (const char* ext : {".rdat", ".ridx"}) {
          const std::string f = base + ext;
          if (src.exists(f)) {
            w.bad.present.push_back(f);
            if (std::string(ext) == ".rdat") w.bad.has_records = true;
          }
        }
      },
      Work::Io);

  // 3. A channel folder with no complete segment: does it hold records?
  std::vector<LayoutChannel> chans(timd.size());
  std::vector<std::size_t> empty;  // channels needing the record-only check
  for (std::size_t c = 0; c < timd.size(); ++c) {
    chans[c].name = timd[c].substr(0, timd[c].size() - 5);
    if (list_err[c]) continue;
    bool any_complete = false, any_err = false;
    for (std::size_t i = first[c]; i < first[c + 1]; ++i) {
      any_err = any_err || seg_err[i];
      any_complete = any_complete || work[i].complete;
    }
    if (!any_err && !any_complete) empty.push_back(c);
  }
  std::vector<char> has_records(timd.size(), 0);
  const auto rec_err = parallel_try_each(
      empty.size(), threads,
      [&](std::size_t e) { has_records[empty[e]] = src.exists(chans[empty[e]].rdat()); },
      Work::Io);

  // Assemble in listing order, failing where a serial walk would have.
  for (std::size_t c = 0; c < timd.size(); ++c) {
    if (list_err[c]) std::rethrow_exception(list_err[c]);
    LayoutChannel& ch = chans[c];
    for (std::size_t i = first[c]; i < first[c + 1]; ++i) {
      if (seg_err[i]) std::rethrow_exception(seg_err[i]);
      if (work[i].complete) {
        ch.segments.push_back(std::move(work[i].seg));
      } else {
        ch.incomplete.push_back(std::move(work[i].bad));
      }
    }
    std::sort(ch.incomplete.begin(), ch.incomplete.end(),
              [](const IncompleteSegment& a, const IncompleteSegment& b) {
                return a.number < b.number;
              });
    if (ch.segments.empty()) {
      const auto at = std::find(empty.begin(), empty.end(), c);
      const std::exception_ptr err = rec_err[static_cast<std::size_t>(at - empty.begin())];
      if (err) std::rethrow_exception(err);
      if (has_records[c]) out.record_only_channels.push_back(ch.name);
      out.incomplete_channels.push_back(std::move(ch));
      continue;
    }
    std::sort(ch.segments.begin(), ch.segments.end(),
              [](const LayoutSegment& a, const LayoutSegment& b) { return a.number < b.number; });
    out.channels.push_back(std::move(ch));
  }
  return out;
}

/// The session-level record file. meflib reads exactly
/// `<session>.mefd/<session>.rdat` (meflib.c:6335/6343), the session name
/// being the directory's (or archive's root folder's) stem. Another name is
/// accepted only when it is the ONLY .rdat at the root: with several there is
/// no telling which holds the session's records, and silently picking the
/// first in listing order is how annotations go missing.
///
/// Returns the path, or "" when there is none. When the choice is ambiguous it
/// returns "" and sets `ambiguity` to a plain-words reason.
inline std::string session_record_file(const SessionSource& src, std::string& ambiguity) {
  ambiguity.clear();
  const std::string preferred = src.session_name() + ".rdat";
  std::vector<std::string> candidates;
  for (const auto& e : src.list_dir(""))
    if (!e.is_dir && e.name.ends_with(".rdat")) candidates.push_back(e.name);
  if (std::find(candidates.begin(), candidates.end(), preferred) != candidates.end())
    return preferred;
  if (candidates.size() == 1) return candidates.front();
  if (candidates.size() > 1) {
    std::sort(candidates.begin(), candidates.end());
    std::string list;
    for (const auto& c : candidates) list += (list.empty() ? "" : ", ") + c;
    ambiguity = "ambiguous session-level records: no " + preferred + " but " +
                std::to_string(candidates.size()) + " .rdat files at the session root (" + list +
                ")";
  }
  return {};
}

}  // namespace mef3io::detail
