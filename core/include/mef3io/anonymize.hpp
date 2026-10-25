// mef3io — a de-identified copy of a session (#70).
//
// anonymize_session() writes a NEW session with the identity removed. By
// default every time is KEPT exactly as the source stores it (the copy still
// shows when the recording happened); with AnonymizeOptions::shift_us set,
// every time is shifted by that one constant instead. The time-zone fields
// (GMT offset, DST) are set to fixed values in both modes. The source is
// never modified. The copy pass reads the source once and writes
// the copy once (verify then re-reads the source data and the copy). RED
// blocks are never decoded or re-encoded: only their start time (and, for
// encrypted blocks, the first 16 statistics bytes) is patched, the regions
// other writers may fill are zeroed, and the block CRC re-sealed, so samples
// are bit-identical.
//
// verify_anonymized() compares a copy against its source: the validator,
// block payloads and statistics, every time (exactly the source's when the
// times are kept, exactly the shift away otherwise), the records, and a
// FIELD-WISE leak scan of every text field (and, when shifting, of every time
// field). anonymize_session
// runs it on the copy before publishing it.
//
// check_anonymize() runs every check anonymize_session makes before it writes
// (the same code: both call one pre-flight function) and returns the
// refusals as data. It writes nothing and reads only metadata.
//
// Every refusal carries a stable `code` (docs/anonymization.md lists them);
// anonymize_session throws it as an AnonymizeError mixed into the exception
// type it always threw (std::invalid_argument, IoError, ...).
//
// Available in C++ and Python. There is no C ABI or MATLAB binding. The copy
// engine, its options, refusals and reports are shared with transform_session
// (transform.hpp, #73).
#pragma once

#include <exception>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "mef3io/control.hpp"
#include "mef3io/records.hpp"
#include "mef3io/transform.hpp"
#include "mef3io/types.hpp"

namespace mef3io {

/// Where a piece of record free text came from, for a TextFn.
struct RecordTextField {
  std::string record_type;  ///< "Note", "Seiz", ...
  std::string field;        ///< "text", "annotation", "marker_name_1", ...
  RecordLevel level = RecordLevel::Session;
  std::string channel;      ///< "" at session level
  int segment = -1;         ///< segment number at segment level, else -1
  /// The record's time in the COPY (uUTC; shifted when shifting).
  /// UUTC_NO_ENTRY when it cannot be known: a time stored against a
  /// recording-time offset the file's segments disagree on (no shift only).
  si8 time = 0;
};

/// Replace one non-empty record text field. Return the new text ("" clears).
/// Called on the writing thread, serially.
using RecordTextFn = std::function<std::string(const std::string& text, const RecordTextField&)>;

enum class RecordTextPolicy {
  Drop,      ///< every free-text field of every record is cleared
  Keep,      ///< kept, with the names removed from the copy redacted (see below)
  Callback,  ///< `text_fn` decides; the redaction below still applies after it
};

/// The copy options (CopyOptions: password, output passwords, channels,
/// rename) plus what anonymization adds. A left-out channel's name is also
/// redacted from kept text; with `rename`, session_description too.
struct AnonymizeOptions : CopyOptions {
  /// Unset (the default): every time is KEPT, stored exactly as the source
  /// stores it, and the copy keeps each segment's recording-time offset (it
  /// is needed to read those stored times back to the same instants). The
  /// copy then still shows when the recording happened.
  ///
  /// Set: added to every time, in microseconds; the copy's recording-time
  /// offset is 0. Must be non-zero, keep every time above 0, and be at least
  /// the session's time span plus one day in magnitude (so no shifted time
  /// can land inside the original range).
  std::optional<si8> shift_us;
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
};

struct VerifyOptions {
  std::string password;         ///< opens the source
  std::string output_password;  ///< opens the copy (its level-2 password)
  /// What the copy should have cleared (as passed to anonymize_session).
  bool clear_subject = true;
  bool clear_location = true;
  bool clear_descriptions = true;
  /// The cut the copy was made with (CopyOptions::time_range / exact).
  std::optional<std::pair<si8, si8>> time_range;
  bool exact = true;
};

/// The names anonymize used before the copy types were shared (#73).
using AnonymizeRefusal = CopyRefusal;
using AnonymizeError = CopyError;

/// Result of check_anonymize().
struct AnonymizeCheck : CopyCheck {
  std::string pseudonym;
};

/// When one channel's recording began and ended in the SOURCE.
struct SourceChannelTimes {
  std::string channel;
  std::optional<si8> start_us, end_us;  ///< uUTC; empty for a record-only channel
};

struct AnonymizeReport : CopyReport {
  std::string pseudonym;
  /// The shift applied; unset = the times were kept.
  std::optional<si8> shift_us;
  /// Read from the source. A shifted copy has 0; a copy that keeps the times
  /// keeps the source's (each segment its own). 0 when the source's segments
  /// disagree on it (there is no single one; a note says so).
  si8 source_recording_time_offset = 0;
  /// The SOURCE's first and last sample time (uUTC; empty without samples).
  /// THE RE-IDENTIFICATION KEY: with the shift they give the real dates back.
  /// (A copy that keeps the times shows them anyway; they are handled the
  /// same way regardless.)
  /// Returned to the caller only: never written into the copy, never logged,
  /// never put into a warning or a message.
  std::optional<si8> source_start_us, source_end_us;
  /// The same per kept channel (same caution).
  std::vector<SourceChannelTimes> source_channel_times;
  si8 record_text_fields_changed = 0;
  si8 record_bodies_cleared = 0;  ///< bodies of a type/version with no known layout
};

/// Write a de-identified copy of `src` (a `.mefd` directory or `.mefd.tar`)
/// to `dst` (a new `.mefd` directory).
///
/// The copy is built in `<dst>.partial/` and moved to `dst` only after
/// verify_anonymized passes; a failure or a cancel removes `<dst>.partial`
/// and leaves nothing behind.
///
/// Every refusal below is thrown as an exception that is ALSO an
/// AnonymizeError carrying a stable code (docs/anonymization.md).
///
/// @throws std::invalid_argument  a bad option: (when shifting) a zero
///         shift, a shift below the floor or into times <= 0, unknown/conflicting channel selection,
///         one output password without the other, a callback policy without
///         a callback.
/// @throws IoError        `dst` (or `<dst>.partial`) exists, `dst` is inside
///         `src`, `dst` does not end in `.mefd`.
/// @throws WriteConflictError  the session cannot be copied without risking
///         a misplaced or leaked date: the conditions inspect_session marks
///         as blocking (video channels, unreadable or incomplete segments,
///         locked sections; when shifting also a disputed or ambiguous
///         recording-time offset), or (when shifting) a stored time at or
///         before a non-zero offset.
/// @throws PasswordError  the source is encrypted and the password does not
///         grant level-2 access.
/// @throws FormatError / CrcError  damaged source data (never laundered).
/// @throws Cancelled      `ctl.cancel` was set; nothing is left behind.
AnonymizeReport anonymize_session(const std::string& src, const std::string& dst,
                                  const AnonymizeOptions& options, const Control& ctl = {});

/// Every check anonymize_session makes before it writes, without writing.
///
/// Runs the same pre-flight code as anonymize_session but collects the
/// refusals instead of throwing the first. Reads only metadata (what
/// inspect_session reads, plus the segment universal headers and the record
/// files of the kept channels); never a `.tdat`. Some refusals can only come
/// from the copy pass (a failing block CRC, a time in a block index or data
/// header or a block's own time that the shift rule refuses,
/// a failed verification); docs/anonymization.md lists which.
///
/// `options.shift_us` decides which copy is checked, exactly as for
/// anonymize_session: unset = the copy that keeps every time (no shift
/// checks apply), set = the shifted copy.
/// @throws Cancelled  `ctl.cancel` was set. A refusal is never thrown, it is
///         returned; only an error that is not one (a bug, an OS failure)
///         propagates.
AnonymizeCheck check_anonymize(const std::string& src, const std::string& dst,
                               const AnonymizeOptions& options, const Control& ctl = {});

/// The text anonymize_session would keep for each of `texts` (record text
/// with RecordTextPolicy::Keep, or a description it keeps): the exact
/// redaction the copy applies, with the same source, destination name and
/// options, so applications never need to copy its rules (#114). Reads only
/// metadata (the pre-flight's), writes nothing, never logs its inputs. Not
/// a check that the result is safe to keep: check_anonymize refuses text
/// the removal cannot take out ("text-unremovable").
/// @throws the first refusal anonymize_session would throw (a locked or
///         unreadable source, an unknown channel, an identity-bearing name,
///         ...), except those that do not decide the redaction: the
///         destination in use and "text-unremovable".
std::vector<std::string> redact_text(const std::string& src, const std::string& dst,
                                     const AnonymizeOptions& options,
                                     const std::vector<std::string>& texts,
                                     const Control& ctl = {});

/// Check a copy against its source. Reads both; writes nothing.
/// `shift_us` as the copy was made with: unset = the copy keeps every time,
/// and every time in it must equal the source's (as stored, so also as read);
/// set = every time must be exactly that shift away.
VerifyReport verify_anonymized(const std::string& src, const std::string& dst,
                               std::optional<si8> shift_us,
                               const VerifyOptions& options = {}, const Control& ctl = {});

}  // namespace mef3io
