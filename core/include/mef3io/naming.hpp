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
//   * rename_session(): renames a directory session in all four places its
//     name lives, as one journaled operation (#72, second part).
//   * fix_name_drift(): brings the stored names back in line with the folder
//     after another tool renamed only the folder.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
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

/// The "operation in progress" marker a multi-step in-place operation leaves
/// in the session folder while it runs. It names no session, path or person.
struct OperationMarker {
  std::string tool;       ///< e.g. "mef3io 1.5.0"
  std::string operation;  ///< "rename", "fix-name-drift"
  std::string id;         ///< 32 hex characters; names the journal
};

struct RenameOptions {
  /// Needed when section 2 is encrypted: it holds session_description.
  std::string password;
  /// Where the journal and the old headers are kept while the operation runs
  /// (deleted when it completes). Empty: `<session>.repair-backup` beside the
  /// session. Old names are identifying, so an application passes a folder
  /// of its own.
  std::string backup_dir;
  /// Change session_description only in these .tmet files (session-relative
  /// paths, as RenameReport::descriptions_changed lists them). Empty: every
  /// .tmet whose description equals the old name. Used by an undo, so it
  /// reverts exactly the descriptions the rename changed.
  std::optional<std::vector<std::string>> descriptions;
  /// For tests only: stop at this point as a crash would (no rollback,
  /// marker and journal left on disk) by throwing IoError. 0 = journal
  /// written, marker not yet; 1 = marker written; 1 + k = after the k-th
  /// change (header, record file, folder); the last point = marker removed,
  /// journal not yet. -1 (default): never.
  int crash_at = -1;
};

struct RenameReport {
  std::string old_path, new_path;  ///< the session folder before and after
  std::string old_name, new_name;  ///< the stored name before and after
  bool folder_renamed = false;
  /// Session-relative paths (after the operation) of the files whose
  /// universal header changed.
  std::vector<std::string> files_changed;
  /// Session-relative paths of the .tmet files whose session_description
  /// changed (it was equal to the old name).
  std::vector<std::string> descriptions_changed;
  /// Session-level record files renamed, as (old file name, new file name).
  std::vector<std::pair<std::string, std::string>> records_renamed;
  /// An earlier operation interrupted by a crash, rolled back first (its id).
  std::string rolled_back;
  /// Journals of finished or never-started operations on this session that
  /// were found and discarded.
  std::size_t journals_discarded = 0;
};

/// Rename a directory session (`<parent>/<old>.mefd` -> `<parent>/<new_name>.mefd`)
/// in all four places its name lives: the folder, the session-level
/// `<name>.rdat`/`.ridx`, `session_name` in every file's universal header,
/// and `session_description` in every `.tmet` where it equals the old name.
/// No samples, index entries or times change. Journaled and marked
/// (OperationMarker) so a crash leaves a session the next call rolls back;
/// renames never replace an existing file. Refused: tar archives, a name
/// that is not portable or over 250 bytes, a target that exists, a session
/// being written or mid-operation, a name already out of step (fix_name_drift
/// first), a damaged header, and an encrypted section 2 without the level-1
/// password. Undo: rename_session back, with `descriptions` set to the
/// report's descriptions_changed.
RenameReport rename_session(const std::string& path, const std::string& new_name,
                            const RenameOptions& opts = {});

/// After another tool renamed only the folder: set the stored names and the
/// session-level record files to `target` (default: the folder's name). The
/// folder is never renamed. Refused when the stored names disagree with each
/// other or there are several session-level record files. Undo:
/// fix_name_drift(path, <old stored name>) with `descriptions`.
RenameReport fix_name_drift(const std::string& path, const std::string& target = "",
                            const RenameOptions& opts = {});

/// The session's operation marker, if it carries one (directory sessions).
std::optional<OperationMarker> operation_marker(const std::string& path);

/// Remove an operation marker whose journal is lost. Allowed only when
/// name_drift shows the name consistent; otherwise WriteConflictError.
void clear_operation_marker(const std::string& path);

}  // namespace mef3io
