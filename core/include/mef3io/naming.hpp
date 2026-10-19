// mef3io — finding and naming sessions in bulk (#72, first part).
//
//   * time_range():   a session's first and last sample time, fast: only the
//     first and last segment's .tmet of each channel are read (never an
//     index or data file), so hundreds of sessions on a network drive can
//     be ordered by time quickly.
//   * name_drift():   where a session's name lives (folder, session-level
//     record files, every universal header's session_name, and section 2
//     session_description), and whether those places agree. Read-only.
//   * rename_archive(): renames a .mefd.tar file with a primitive that never
//     replaces an existing file. The archive's inside keeps its old name.
//
// rename_session (renaming a directory session in all four places, as one
// journaled operation) is the second part of #72.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "mef3io/control.hpp"
#include "mef3io/types.hpp"

namespace mef3io {

struct TimeRange {
  /// The first and last sample time, uUTC (the end as the universal headers
  /// state it: the end of the last block). Empty when no channel has data.
  std::optional<si8> start_uutc, end_uutc;
  /// false when the times cannot be presented as absolute: the session is
  /// encrypted and the password does not open section 3 (where the
  /// recording-time offset is kept), or a segment uses a recording-time
  /// offset with times stored in the ambiguous (non-negated) form.
  bool offset_known = true;
  std::size_t channels = 0;    ///< channels with data that were looked at
  std::size_t files_read = 0;  ///< .tmet files read (two per channel at most)
};

/// The first and last sample time of a session (`.mefd` or `.mefd.tar`),
/// reading only the first and last segment's .tmet of each channel (the
/// bytes read are O(channels); listing the session still checks each
/// segment's files exist). A middle segment earlier than both is not seen:
/// segments are numbered in time order by every writer we know of. A wrong
/// password throws PasswordError; no password on an encrypted session is
/// allowed (offset_known is then false).
TimeRange time_range(const std::string& path, const std::string& password = "",
                     const Control& ctl = {});

/// Where a session's name is stored, and whether the places agree.
struct NameDrift {
  std::string folder;                     ///< the stem of the .mefd / .mefd.tar
  std::vector<std::string> record_files;  ///< session-level *.rdat stems
  /// The distinct session_name values in the universal headers, with how
  /// many files hold each.
  std::map<std::string, std::size_t> stored_names;
  std::size_t files_read = 0;
  /// Folder, the session-level record file (if any) and every stored name
  /// are the same.
  bool consistent = false;
};

/// Read every file's universal header (1024 bytes each) and list the names.
/// Writes nothing. Works on tar archives too.
NameDrift name_drift(const std::string& path, const Control& ctl = {});

struct ArchiveRename {
  std::string old_path, new_path;
  /// Always true: the archive's internal folder, member names and stored
  /// names still carry the old name (extract, rename_session and archive
  /// again to change them).
  bool inside_keeps_old_name = true;
};

/// Rename a `.mefd.tar` file to `<new_name>.mefd.tar` in the same folder.
/// Refused (std::invalid_argument / IoError), never truncated or replaced:
/// a name over 246 bytes of UTF-8 (so the file name fits 255 bytes), a name
/// that is not a portable file name (a folder, a control character, a
/// character or device name Windows cannot use, a trailing dot or space), a
/// target that exists, a source that is not a `.mefd.tar`, an archive being
/// written (`<path>.tmp` or `.partial` beside it), and a file system without
/// a no-replace rename (never falls back to a plain one). A symlink is
/// renamed itself, not its target.
ArchiveRename rename_archive(const std::string& path, const std::string& new_name);

}  // namespace mef3io
