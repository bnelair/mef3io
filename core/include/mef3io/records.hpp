// mef3io — MEF record (annotation) reading. Records live in <name>.rdat with a
// parallel <name>.ridx index, at session, channel or segment level.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "mef3io/crypto.hpp"
#include "mef3io/types.hpp"

namespace mef3io {

/// Where a record file lives. MEF 3.0 allows record files at three levels
/// (meflib.c:6343 session, :5660 channel, :6030 segment), each named after
/// its directory: `<session>.mefd/<session>.rdat`, `<ch>.timd/<ch>.rdat`,
/// `<ch>.timd/<ch>-NNNNNN.segd/<ch>-NNNNNN.rdat`.
enum class RecordLevel : int { Session = 0, Channel = 1, Segment = 2 };

/// "session" / "channel" / "segment".
const char* record_level_name(RecordLevel level);

/// One channel entry of a Seiz body (MEFREC_Seiz_1_0_CHANNEL, mefrec.h:152).
/// onset/offset are absolute uUTC; -1 is meflib's NO_ENTRY for both
/// (mefrec.h:129/131) and is returned as -1.
struct SeizureChannel {
  std::string name;
  si8 onset = -1;
  si8 offset = -1;
};

/// A decoded record: header fields, where it was found, and the body fields
/// of its type. Body fields are nullopt when the body is too short to hold
/// them or its version is not one meflib decodes (it decodes only 1.0 for the
/// structured types, mefrec.c). Every TIME below — `time` and the body fields
/// marked "uUTC" — is absolute, resolved with the same sign-aware rule
/// (mef3io/time.hpp); see parse_records for why body times are converted too.
struct Record {
  std::string type;   // e.g. "Note", "EDFA", "Seiz", "SyLg"
  si8 time = 0;       // absolute uUTC (record header)
  int version_major = 1;
  int version_minor = 0;

  /// Where the record was read from. Ignored on write.
  RecordLevel level = RecordLevel::Session;
  std::string channel;   ///< empty for session-level records
  int segment = -1;      ///< segment number for segment-level records, else -1

  // Note / SyLg (whole body), EDFA (after duration), Epoc (text field).
  std::optional<std::string> text;
  // EDFA / Seiz / Epoc, microseconds.
  std::optional<si8> duration;

  // Seiz (MEFREC_Seiz_1_0, mefrec.h:141).
  std::optional<si8> earliest_onset;            // uUTC
  std::optional<si8> latest_offset;             // uUTC
  std::optional<si4> number_of_channels;        // as declared; see seizure_channels
  std::optional<si4> onset_code;
  std::optional<std::string> marker_name_1, marker_name_2, annotation;
  /// Channel entries actually present in the body: at most
  /// number_of_channels, fewer when the body is shorter than it declares.
  std::vector<SeizureChannel> seizure_channels;

  // Epoc (MEFREC_Epoc_1_0, mefrec.h:326) and Curs (MEFREC_Curs_1_0, :290).
  std::optional<si8> id_number;                 // Epoc, Curs
  std::optional<si8> timestamp;                 // Epoc, uUTC
  std::optional<si8> end_timestamp;             // Epoc, uUTC
  std::optional<std::string> epoch_type;        // Epoc
  std::optional<si8> trace_timestamp;           // Curs, uUTC
  std::optional<si8> latency;                   // Curs, microseconds
  std::optional<sf8> value;                     // Curs
  std::optional<std::string> name;              // Curs

  // CSti (MEFREC_CSti_1_0, mefrec.h:205).
  std::optional<std::string> task_type, stimulus_type, patient_response;
  std::optional<si8> stimulus_duration;

  // ESti (MEFREC_ESti_1_0, mefrec.h:253).
  std::optional<sf8> amplitude, frequency;
  std::optional<si8> pulse_width;
  std::optional<si4> ampunit_code, mode_code;
  std::optional<std::string> waveform, anode, catode;

  // LNTP (MEFREC_LNTP_1_0, mefrec.h:70): `length` si4 template values.
  std::optional<si8> template_length;
  std::vector<si4> line_noise_template;
};

/// One named body field of a record, in a stable per-type order. `value` is
/// empty when the record does not carry it (short body, unknown version).
struct RecordField {
  using Value =
      std::variant<si8, sf8, std::string, std::vector<si4>, std::vector<SeizureChannel>>;
  std::string name;
  std::optional<Value> value;
};

/// The body fields of `r`'s type: always the same names in the same order for
/// a given type, and none for a type with no defined body (UnRc, unknown
/// codes). Every binding builds its per-record dict / struct from this, so all
/// of them show the same shape. Names follow mefrec.h; LNTP's are "length" and
/// "template", Seiz's channel list is "channels".
std::vector<RecordField> record_fields(const Record& r);

namespace fmt {
inline constexpr int RECORD_HEADER_BYTES = 24;
inline constexpr int RECORD_INDEX_BYTES = 24;
}  // namespace fmt

/// Width of a record type code ("Note", "EDFA", ...): exact, not null-terminated.
inline constexpr std::size_t RECORD_TYPE_BYTES = 4;

// Parse a .rdat image (universal header + concatenated records). `rto` is the
// recording-time-offset for resolving absolute record times — header AND body
// times (see records.cpp for the body-time rule); `keys` decrypts encrypted
// record bodies when present. Level / channel / segment are left at their
// defaults; the Session fills them in.
std::vector<Record> parse_records(std::span<const ui1> rdat_bytes, si8 rto,
                                   const crypto::AccessKeys& keys = {});

// Write records to `<dir>/<base>.rdat` + `<base>.ridx`. `session_name`/
// `channel_name` populate the universal headers; `rto` sets the on-disk time
// convention. When `password_1`/`password_2` are set, record bodies are
// level-2 encrypted (meflib convention: records may hold identifying data).
void write_records(const std::string& dir, const std::string& base,
                   const std::string& session_name, const std::string& channel_name,
                   int segment_number, const std::vector<Record>& records, si8 rto,
                   const std::string& password_1 = "", const std::string& password_2 = "");

}  // namespace mef3io
