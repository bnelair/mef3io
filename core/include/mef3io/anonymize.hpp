// mef3io — a de-identified copy of a session (#70).
//
// anonymize_session() writes a NEW session: every time shifted by one
// constant, the time-zone fields set to fixed values, identity removed. The
// source is never modified. The copy pass reads the source once and writes
// the copy once (verify then re-reads the source data and the copy). RED
// blocks are never decoded or re-encoded: only their start time (and, for
// encrypted blocks, the first 16 statistics bytes) is patched, the regions
// other writers may fill are zeroed, and the block CRC re-sealed, so samples
// are bit-identical.
//
// verify_anonymized() compares a copy against its source: the validator,
// block payloads and statistics, every time (exactly the shift), the records,
// and a FIELD-WISE leak scan of every time and text field. anonymize_session
// runs it on the copy before publishing it.
//
// Available in C++ and Python. There is no C ABI or MATLAB binding.
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "mef3io/control.hpp"
#include "mef3io/records.hpp"
#include "mef3io/types.hpp"

namespace mef3io {

/// Where a piece of record free text came from, for a TextFn.
struct RecordTextField {
  std::string record_type;  ///< "Note", "Seiz", ...
  std::string field;        ///< "text", "annotation", "marker_name_1", ...
  RecordLevel level = RecordLevel::Session;
  std::string channel;      ///< "" at session level
  int segment = -1;         ///< segment number at segment level, else -1
  si8 time = 0;             ///< the record's time in the COPY (shifted, uUTC)
};

/// Replace one non-empty record text field. Return the new text ("" clears).
/// Called on the writing thread, serially.
using RecordTextFn = std::function<std::string(const std::string& text, const RecordTextField&)>;

enum class RecordTextPolicy {
  Drop,      ///< every free-text field of every record is cleared
  Keep,      ///< kept, with the names removed from the copy redacted (see below)
  Callback,  ///< `text_fn` decides; the redaction below still applies after it
};

struct AnonymizeOptions {
  /// Added to every time, in microseconds. Must be non-zero, keep every time
  /// above 0, and be at least the session's time span plus one day in
  /// magnitude (so no shifted time can land inside the original range).
  si8 shift_us = 0;
  /// Written to every universal header's anonymized_name. Empty = the new
  /// session name (the stem of `dst`).
  std::string pseudonym;
  /// Section 3: clear subject_name_1, subject_name_2 and subject_id.
  bool clear_subject = true;
  /// Section 3: clear recording_location.
  bool clear_location = true;
  /// Section 2: clear channel_, session_ and reference_description. When
  /// false they are kept with the old session name replaced by the new one
  /// and every other removed name redacted.
  bool clear_descriptions = true;
  RecordTextPolicy text = RecordTextPolicy::Drop;
  RecordTextFn text_fn;  ///< required with RecordTextPolicy::Callback
  /// true: the copy takes the stem of `dst` as its session name everywhere
  /// (folder, session-level record files, every universal header,
  /// session_description). false: the source's name is kept, and the stem of
  /// `dst` must equal it.
  bool rename = true;
  std::string password;  ///< opens the source; encrypted sources need level 2
  /// Output encryption: both empty = none; otherwise BOTH set (MEF has no
  /// level-1-only files). Metadata sections 2/3 and record bodies are
  /// encrypted as on every mef3io write. RED blocks the SOURCE encrypted are
  /// re-encrypted with these passwords at the same level, or decrypted when
  /// none are given; plain blocks stay plain.
  std::string output_password_1, output_password_2;
  /// Channels to copy: empty = all. Mutually exclusive with exclude_channels.
  std::vector<std::string> channels;
  /// Channels to leave out. An excluded channel leaves nothing in the copy:
  /// no folder, no records, no per-channel entry in a Seiz record, and its
  /// name is redacted from kept text.
  std::vector<std::string> exclude_channels;
};

/// One time or text field the leak scan or the comparison flagged.
struct VerifyIssue {
  std::string location;  ///< file (source-relative) and, where useful, entry
  std::string field;     ///< e.g. "universal header start_time", "record text"
  std::string message;   ///< plain words; never echoes an identifying string
};

struct VerifyReport {
  bool ok = false;
  std::vector<VerifyIssue> problems;   ///< anything wrong, including leaks
  std::vector<VerifyIssue> leaks;      ///< the subset found by the leak scan
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

struct VerifyOptions {
  std::string password;         ///< opens the source
  std::string output_password;  ///< opens the copy (its level-2 password)
  /// What the copy should have cleared (as passed to anonymize_session).
  bool clear_subject = true;
  bool clear_location = true;
  bool clear_descriptions = true;
};

struct AnonymizeReport {
  std::string destination;
  std::string session_name;        ///< the copy's
  std::string pseudonym;
  si8 shift_us = 0;
  si8 source_recording_time_offset = 0;  ///< read from the source; the copy has 0
  std::vector<std::string> channels_kept;
  std::vector<std::string> channels_excluded;
  si8 segments = 0;
  si8 blocks = 0;
  si8 blocks_reencrypted = 0;   ///< source-encrypted blocks written encrypted
  si8 blocks_decrypted = 0;     ///< source-encrypted blocks written plain
  si8 records = 0;
  si8 record_text_fields_changed = 0;
  si8 record_bodies_cleared = 0;   ///< bodies of a type/version with no known layout
  si8 seizure_channel_entries_dropped = 0;
  std::uint64_t bytes_written = 0;
  bool output_encrypted = false;
  /// Files in the source that are not part of the MEF layout and were not
  /// copied (source-relative paths).
  std::vector<std::string> not_copied;
  std::vector<std::string> notes;
  VerifyReport verification;
};

/// Write a de-identified copy of `src` (a `.mefd` directory or `.mefd.tar`)
/// to `dst` (a new `.mefd` directory).
///
/// The copy is built in `<dst>.partial/` and moved to `dst` only after
/// verify_anonymized passes; a failure or a cancel removes `<dst>.partial`
/// and leaves nothing behind.
///
/// @throws std::invalid_argument  a bad option: zero shift, shift below the
///         floor or into times <= 0, unknown/conflicting channel selection,
///         one output password without the other, a callback policy without
///         a callback.
/// @throws IoError        `dst` (or `<dst>.partial`) exists, `dst` is inside
///         `src`, `dst` does not end in `.mefd`.
/// @throws WriteConflictError  the session cannot be copied without risking
///         a misplaced or leaked date: the conditions inspect_session marks
///         as blocking (video channels, unreadable or incomplete segments,
///         disputed or ambiguous recording-time offset, locked sections),
///         or a stored time at or before a non-zero offset.
/// @throws PasswordError  the source is encrypted and the password does not
///         grant level-2 access.
/// @throws FormatError / CrcError  damaged source data (never laundered).
/// @throws Cancelled      `ctl.cancel` was set; nothing is left behind.
AnonymizeReport anonymize_session(const std::string& src, const std::string& dst,
                                  const AnonymizeOptions& options, const Control& ctl = {});

/// Check a copy against its source. Reads both; writes nothing.
VerifyReport verify_anonymized(const std::string& src, const std::string& dst, si8 shift_us,
                               const VerifyOptions& options = {}, const Control& ctl = {});

}  // namespace mef3io
