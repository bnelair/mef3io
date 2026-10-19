// mef3io — a modified copy of a session (#73).
//
// transform_session() writes a NEW session from an existing one: a choice of
// channels, new passwords (or none), a new session name, and edits to the
// descriptive metadata. The source is never modified. It is the same
// streaming copy engine anonymize_session (#70) uses, with different rules:
//
//   * every stored time is copied VERBATIM (universal headers, index entries,
//     block headers, records, the recording-time offset and the time-zone
//     fields), so no date is moved, revealed or re-interpreted;
//   * RED blocks are never decoded or re-encoded: the samples are
//     bit-identical, and only encrypted statistics are re-keyed;
//   * the regions other writers fill (meflib's protected and discretionary
//     regions) are kept, never cleared: they are not mef3io's to drop;
//   * record bodies are kept byte for byte, except the Seiz entries of
//     channels left out of the copy;
//   * every file gets a new file UUID; level UUIDs are remapped consistently
//     (a copy is a new session); provenance_UUID keeps pointing at the
//     source; every file's provenance region says "transform" (operation 8),
//     except a region another program owns, which is never written.
//
// The copy is built in `<dst>.partial/`, verified against the source (samples,
// every time, every byte that should not change, the validator), and only
// then moved to `dst`. check_transform() runs the same pre-flight without
// writing.
//
// The types shared by every copy operation (options, refusals, reports) are
// defined here; anonymize.hpp builds on them.
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

/// What every copy operation takes.
struct CopyOptions {
  std::string password;  ///< opens the source; encrypted sources need level 2
  /// Output encryption: both empty = none; otherwise BOTH set and different
  /// (MEF has no level-1-only files). Passwords are set only on a copy, never
  /// changed in place. Metadata sections 2/3 and record bodies are encrypted
  /// as on every mef3io write; RED blocks the SOURCE encrypted are re-keyed at
  /// the same level, or decrypted when no passwords are given.
  std::string output_password_1, output_password_2;
  /// Channels to copy: empty = all. Mutually exclusive with exclude_channels.
  std::vector<std::string> channels;
  /// Channels to leave out. A left-out channel leaves nothing in the copy: no
  /// folder, no records, no per-channel entry in a Seiz record.
  std::vector<std::string> exclude_channels;
  /// true: the copy takes the stem of `dst` as its session name everywhere
  /// (folder, session-level record files, every universal header). false:
  /// the source's name is kept, and the stem of `dst` must equal it.
  bool rename = true;
  /// A cut (#103): keep only the samples whose time lies in [first, second),
  /// in uUTC as the SOURCE stores them (before any shift). Unset = all.
  /// Segments outside it are not read; blocks outside it are left out;
  /// annotations are kept when their time lies in it (a Seiz or Epoc
  /// record also when its span overlaps it).
  std::optional<std::pair<si8, si8>> time_range;
  /// With time_range: true (default) = the two edge blocks per channel and
  /// segment are decoded, trimmed to the window and re-encoded, so the copy
  /// holds exactly the window; false = whole edge blocks are kept and
  /// nothing is decoded (a block's worth of extra samples at each edge).
  bool exact = true;
};

/// Edits to the descriptive metadata of a transformed copy. Unset = copied
/// unchanged; "" = cleared. Each value must fit its field (the field size
/// minus the terminator, in UTF-8 bytes). Applied to the stored section
/// images of every segment, never by re-serializing them.
struct MetadataEdits {
  std::optional<std::string> subject_name_1;      ///< section 3, 128 B
  std::optional<std::string> subject_name_2;      ///< section 3, 128 B
  std::optional<std::string> subject_id;          ///< section 3, 128 B
  std::optional<std::string> recording_location;  ///< section 3, 512 B
  std::optional<std::string> session_description; ///< section 2, 2048 B, every channel
  /// Per channel (by name; the channel must be in the copy), section 2, 2048 B.
  std::map<std::string, std::string> channel_descriptions;
  std::map<std::string, std::string> reference_descriptions;

  bool empty() const {
    return !subject_name_1 && !subject_name_2 && !subject_id && !recording_location &&
           !session_description && channel_descriptions.empty() &&
           reference_descriptions.empty();
  }
};

struct TransformOptions : CopyOptions {
  MetadataEdits metadata;
};

/// One field the verification flagged.
struct VerifyIssue {
  std::string location;  ///< file (source-relative) and, where useful, entry
  std::string field;     ///< e.g. "universal header start_time", "record text"
  std::string message;   ///< plain words; never echoes an identifying string
};

struct VerifyReport {
  bool ok = false;
  std::vector<VerifyIssue> problems;   ///< anything wrong, including leaks
  std::vector<VerifyIssue> leaks;      ///< anonymize only: the subset the leak scan found
  /// Anonymize only. Overlaps reported but NOT refused: an identity value
  /// inside a kept channel's name as part of a longer token (or shorter than
  /// 3 characters), inside the copy's own name, or a number of fewer than 4
  /// digits inside a longer number. Never echoes an identifying value.
  std::vector<VerifyIssue> notes;
  /// Validator findings the source already had (check id, channel, segment):
  /// carried over unchanged by a copy, not introduced by it.
  std::vector<std::string> inherited_findings;
  std::vector<std::string> channels;           ///< channels in the copy
  std::vector<std::string> excluded_channels;  ///< in the source, not the copy
  si8 segments = 0;
  si8 blocks = 0;
  si8 records = 0;
  si8 times_checked = 0;
  si8 text_fields_checked = 0;
};

/// Which exception type a refusal is thrown as.
enum class RefusalKind {
  InvalidArgument,  ///< std::invalid_argument (Python ValueError)
  Io,               ///< IoError
  WriteConflict,    ///< WriteConflictError
  Password,         ///< PasswordError
  Format,           ///< FormatError
  Crc,              ///< CrcError
};

/// The Python name of the exception type: "ValueError", "IoError",
/// "WriteConflictError", "PasswordError", "FormatError" or "CrcError".
const char* refusal_kind_name(RefusalKind kind) noexcept;

/// One reason a session cannot be copied as asked.
struct CopyRefusal {
  std::string code;     ///< stable, e.g. "dst-exists" (docs/anonymization.md, docs/transform.md)
  std::string message;  ///< plain words
  RefusalKind kind = RefusalKind::InvalidArgument;
  std::string channel;  ///< the channel concerned, or ""
  std::string field;    ///< the option or field concerned (e.g. "pseudonym"), or ""
  /// For a refusal taken from inspect_session's warnings: that warning's code.
  std::string inspect_code;
};

/// Mixed into every exception a copy operation throws for a refusal, next to
/// its usual type: `catch (const CopyError& e)` reads the code, and
/// `catch (const IoError&)` still works as before.
class CopyError {
 public:
  explicit CopyError(CopyRefusal r) : refusal_(std::move(r)) {}
  virtual ~CopyError() = default;
  const CopyRefusal& refusal() const noexcept { return refusal_; }
  const std::string& code() const noexcept { return refusal_.code; }

 private:
  CopyRefusal refusal_;
};

/// Result of a pre-flight check (check_transform, check_anonymize).
struct CopyCheck {
  bool ok = false;  ///< no refusal
  /// Every refusal found, in the order the copy meets them (it throws the
  /// first). Checks that depend on an earlier one that failed (e.g.
  /// everything about the source when it cannot be opened) are skipped.
  std::vector<CopyRefusal> refusals;
  std::vector<std::string> notes;  ///< what the copy would note up front
  std::string session_name;        ///< the copy's (empty when dst is unusable)
  std::vector<std::string> channels_kept, channels_excluded;
  /// A cut: channels left out because they have no sample in the window
  /// (also in channels_excluded).
  std::vector<std::string> channels_without_samples;
};

/// What every copy operation reports.
struct CopyReport {
  std::string destination;
  std::string session_name;  ///< the copy's
  std::vector<std::string> channels_kept;
  std::vector<std::string> channels_excluded;
  /// A cut: channels left out because they have no sample in the window
  /// (also in channels_excluded).
  std::vector<std::string> channels_without_samples;
  si8 segments = 0;
  si8 blocks = 0;
  si8 blocks_reencrypted = 0;  ///< source-encrypted blocks written encrypted
  si8 blocks_decrypted = 0;    ///< source-encrypted blocks written plain
  si8 records = 0;
  si8 seizure_channel_entries_dropped = 0;
  si8 blocks_trimmed = 0;          ///< cut: edge blocks re-encoded to the window
  si8 records_outside_window = 0;  ///< cut: annotations left out
  std::uint64_t bytes_written = 0;
  bool output_encrypted = false;
  /// Files in the source that are not part of the MEF layout and were not
  /// copied (source-relative paths).
  std::vector<std::string> not_copied;
  std::vector<std::string> notes;
  VerifyReport verification;
};

struct TransformReport : CopyReport {
  /// Metadata fields whose stored value changed (one count per field per
  /// segment).
  si8 metadata_fields_changed = 0;
};

/// Write a modified copy of `src` (a `.mefd` directory or `.mefd.tar`) to
/// `dst` (a new `.mefd` directory). See the top of this file.
///
/// Every refusal is thrown as its usual exception type that is ALSO a
/// CopyError carrying a stable code (docs/transform.md).
///
/// @throws std::invalid_argument  a bad option: unknown or conflicting
///         channel selection, unpaired or equal output passwords, a metadata
///         edit that does not fit its field or names a channel not in the copy.
/// @throws IoError        `dst` (or `<dst>.partial`) exists, `dst` is inside
///         `src`, `dst` does not end in `.mefd`.
/// @throws WriteConflictError  parts of the session cannot be copied whole
///         (video channels, unreadable or incomplete segments or records).
/// @throws PasswordError  the source is encrypted and the password does not
///         grant level-2 access.
/// @throws FormatError / CrcError  damaged source data (never laundered).
/// @throws Cancelled      `ctl.cancel` was set; nothing is left behind.
TransformReport transform_session(const std::string& src, const std::string& dst,
                                  const TransformOptions& options, const Control& ctl = {});

/// Every check transform_session makes before it writes, without writing:
/// the same pre-flight code, refusals collected instead of thrown. Reads only
/// metadata, never a `.tdat`.
CopyCheck check_transform(const std::string& src, const std::string& dst,
                          const TransformOptions& options, const Control& ctl = {});

/// Check a transformed copy against its source and the options it was made
/// with (`options.output_password_2` opens the copy). Reads both; writes
/// nothing.
VerifyReport verify_transform(const std::string& src, const std::string& dst,
                              const TransformOptions& options, const Control& ctl = {});

}  // namespace mef3io
