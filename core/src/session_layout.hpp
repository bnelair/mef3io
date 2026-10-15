// Internal (not installed): how a session tree is laid out, shared by the
// Session (session.cpp) and inspect_session (inspect.cpp) so the two can never
// disagree on which folders are channels, which are segments, what a
// segment's number is, or which file holds the session-level records.
#pragma once

#include <algorithm>
#include <string>
#include <vector>

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

struct LayoutChannel {
  std::string name;
  std::vector<LayoutSegment> segments;  ///< sorted by number
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
};

/// Walk the root and every `.timd`. One listing per channel; three existence
/// checks per segment, exactly what the Session always did.
inline SessionLayout discover_layout(const SessionSource& src) {
  SessionLayout out;
  for (const auto& entry : src.list_dir("")) {
    if (!entry.is_dir) continue;
    if (entry.name.ends_with(".vidd")) {
      out.video_channels.push_back(entry.name.substr(0, entry.name.size() - 5));
      continue;
    }
    if (!entry.name.ends_with(".timd")) continue;

    LayoutChannel ch;
    ch.name = entry.name.substr(0, entry.name.size() - 5);
    for (const auto& seg_entry : src.list_dir(entry.name)) {
      if (!seg_entry.is_dir || !seg_entry.name.ends_with(".segd")) continue;
      const std::string stem = seg_entry.name.substr(0, seg_entry.name.size() - 5);
      const std::string dir = entry.name + "/" + seg_entry.name;
      LayoutSegment seg;
      seg.number = layout_segment_number(stem);
      seg.tmet = dir + "/" + stem + ".tmet";
      seg.tidx = dir + "/" + stem + ".tidx";
      seg.tdat = dir + "/" + stem + ".tdat";
      if (src.exists(seg.tmet) && src.exists(seg.tidx) && src.exists(seg.tdat))
        ch.segments.push_back(std::move(seg));
    }
    if (ch.segments.empty()) {
      if (src.exists(ch.rdat())) out.record_only_channels.push_back(ch.name);
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
