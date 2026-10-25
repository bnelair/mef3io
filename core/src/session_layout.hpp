// Internal (not installed): how a session tree is laid out, shared by the
// Session (session.cpp) and inspect_session (inspect.cpp) so the two can never
// disagree on which folders are channels, which are segments, what a
// segment's number is, or which file holds the session-level records.
#pragma once

#include <algorithm>
#include <string>
#include <utility>
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

/// What one listing says is in a folder, answering "does this file exist?"
/// the way the former per-file exists() calls did, without a request per
/// file: on a network share each one is a full round trip (#141). A name the
/// listing does not show (or shows as a link, which may dangle) is confirmed
/// with exists(), so a case-insensitive volume or a dangling link still gets
/// exactly the old answer — and a complete segment costs no exists() at all.
/// A folder that cannot be listed falls back to exists() for every name.
class FolderListing {
 public:
  /// Lists `dir`; a listing that fails leaves every answer to exists()
  /// (e.g. a folder that can be traversed but not read).
  FolderListing(const SessionSource& src, std::string dir) : src_(src), dir_(std::move(dir)) {
    try {
      entries_ = src.list_dir(dir_);
      listed_ = true;
    } catch (...) {
      listed_ = false;
    }
  }
  /// From a listing the caller already made (and let throw).
  FolderListing(const SessionSource& src, std::string dir, std::vector<SourceDirEntry> entries)
      : src_(src), dir_(std::move(dir)), entries_(std::move(entries)), listed_(true) {}
  /// Does `<dir>/<leaf>` exist (as anything, like SessionSource::exists)?
  bool has(const std::string& leaf) const {
    if (listed_) {
      for (const auto& e : entries_)
        if (e.name == leaf) return !e.is_symlink || src_.exists(path(leaf));
    }
    return src_.exists(path(leaf));
  }
  const std::vector<SourceDirEntry>& entries() const { return entries_; }
  std::string path(const std::string& leaf) const {
    return dir_.empty() ? leaf : dir_ + "/" + leaf;
  }

 private:
  const SessionSource& src_;
  std::string dir_;
  std::vector<SourceDirEntry> entries_;
  bool listed_ = false;
};

/// Walk the root and every `.timd` and `.segd`: one listing per folder and
/// NO per-file request on a complete session (#141; it used to be three
/// exists() per segment). The answers are the ones exists() gave — see
/// FolderListing.
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
    // A .timd that cannot be listed still fails the open, as it always has.
    const FolderListing timd(src, entry.name, src.list_dir(entry.name));
    for (const auto& seg_entry : timd.entries()) {
      if (!seg_entry.is_dir || !seg_entry.name.ends_with(".segd")) continue;
      const std::string stem = seg_entry.name.substr(0, seg_entry.name.size() - 5);
      const std::string dir = entry.name + "/" + seg_entry.name;
      const FolderListing segd(src, dir);
      LayoutSegment seg;
      seg.number = layout_segment_number(stem);
      seg.tmet = dir + "/" + stem + ".tmet";
      seg.tidx = dir + "/" + stem + ".tidx";
      seg.tdat = dir + "/" + stem + ".tdat";
      const bool has[3] = {segd.has(stem + ".tmet"), segd.has(stem + ".tidx"),
                           segd.has(stem + ".tdat")};
      if (has[0] && has[1] && has[2]) {
        ch.segments.push_back(std::move(seg));
        continue;
      }
      IncompleteSegment bad;
      bad.number = seg.number;
      bad.dir = dir;
      const std::string* files[3] = {&seg.tmet, &seg.tidx, &seg.tdat};
      const char* exts[3] = {".tmet", ".tidx", ".tdat"};
      for (int i = 0; i < 3; ++i) {
        if (has[i]) {
          bad.present.push_back(*files[i]);
        } else {
          bad.missing += (bad.missing.empty() ? "" : ", ") + std::string(exts[i]);
        }
      }
      for (const char* ext : {".rdat", ".ridx"}) {
        if (segd.has(stem + ext)) {
          bad.present.push_back(dir + "/" + stem + ext);
          if (std::string(ext) == ".rdat") bad.has_records = true;
        }
      }
      ch.incomplete.push_back(std::move(bad));
    }
    std::sort(ch.incomplete.begin(), ch.incomplete.end(),
              [](const IncompleteSegment& a, const IncompleteSegment& b) {
                return a.number < b.number;
              });
    if (ch.segments.empty()) {
      if (timd.has(ch.name + ".rdat")) out.record_only_channels.push_back(ch.name);
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
