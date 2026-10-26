// mef3io — describe a session without reading any sample data (#67).
//
// inspect_session() reads the metadata (.tmet), the record files (.rdat) and,
// only when asked, the block indices (.tidx). It NEVER reads a .tdat: one
// channel of a long recording is tens of gigabytes, and an inspector that
// touched the data would be unusable on exactly the sessions it exists for.
// The .tdat appears only as a file size.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "mef3io/control.hpp"
#include "mef3io/headers.hpp"
#include "mef3io/records.hpp"
#include "mef3io/session.hpp"
#include "mef3io/types.hpp"

namespace mef3io {

struct InspectOptions {
  /// Opens encrypted sections. Empty is allowed for an encrypted session:
  /// what it locks is REPORTED (EncryptionSummary, None times) rather than
  /// thrown. A password that is given and wrong still throws PasswordError.
  std::string password;
  /// true (the default, as everywhere in mef3io): a segment or record file
  /// that cannot be read throws. false: it is reported in
  /// SessionSummary::problems and marked unreadable, and the rest is described.
  bool strict = true;
  /// Return identity VALUES (subject names, descriptions, record text, the
  /// recording-time offset) and the full decoded records. Off by default:
  /// identity fields then report presence and length only.
  bool include_values = false;
  /// Also read every segment's block index (.tidx, ~2% of the data) to find
  /// the gaps INSIDE segments. Off by default: gaps are then the ones between
  /// segments only, and ChannelSummary::gaps_complete is false. Gaps come from
  /// the index's copy of each block's start time and count; the RED block
  /// header (the authoritative copy) is inside the .tdat and is never read.
  bool read_index = false;
};

struct InspectSegment {
  int segment_number = 0;
  std::string path;             ///< the .segd ("<archive>::<member>" in a tar)
  bool readable = true;         ///< false: see `problem` (lenient mode only)
  std::string problem;
  bool section2_available = false;  ///< section 2 could be decrypted (or was plain)
  bool section3_available = false;  ///< section 3 could be decrypted (or was plain)
  /// Absolute uUTC of the first sample / just past the last (as the
  /// universal header states them). With section 3 locked they assume offset
  /// 0 (offset_assumed_zero); nullopt only for a segment that could not be read.
  std::optional<si8> start_time, end_time;
  std::optional<si8> start_sample, number_of_samples, number_of_blocks;
  /// As DECLARED in section 2 (legacy writers leave 0 here even with gaps).
  std::optional<si8> number_of_discontinuities;
  /// Section 2's units_conversion_factor, as stored (0 = MEF's "no entry");
  /// nullopt when section 2 is locked or the segment could not be read.
  std::optional<sf8> units_conversion_factor;
  /// "none" (no offset), "applied" (meflib's recording-time offset is set),
  /// "locked" (section 3 is encrypted and the password does not open it) or
  /// "unknown" (the segment could not be read: lenient mode, or a segment
  /// folder missing one of its files).
  std::string time_offset = "none";
  std::optional<si8> recording_time_offset;  ///< include_values only
  /// Section 3 is locked and a time above was stored offset-applied
  /// (negated), so it was resolved ASSUMING offset 0, as the Reader does. Not
  /// verified (see TimeOffsetSummary::interpretation). time_offset is then
  /// "locked"; a locked segment whose times are all absolute is not flagged.
  bool offset_assumed_zero = false;
  std::uint64_t tmet_bytes = 0, tidx_bytes = 0, tdat_bytes = 0;
  std::uint64_t record_bytes = 0;  ///< this segment's .rdat + .ridx
  bool has_records = false;
  fmt::provenance::Provenance provenance;
  std::vector<std::string> unset_declarations;
};

/// A span of the channel with no samples: [start, end), absolute uUTC.
struct InspectGap {
  si8 start = 0;
  si8 end = 0;
};

struct InspectChannel {
  std::string name;
  /// From the first segment whose section 2 is readable. The conversion
  /// factor is nullopt also when the readable segments store different ones
  /// (#150; each segment's is in InspectSegment::units_conversion_factor).
  std::optional<sf8> sampling_frequency, units_conversion_factor;
  std::optional<std::string> units_description;
  /// Section 2's free-text channel description, as the first segment whose
  /// section 2 is readable states it (segments can differ; the identity
  /// table lists every distinct value). An IDENTITY value, so it is returned
  /// only with InspectOptions::include_values (like the identity table's
  /// values); nullopt otherwise, and when every section 2 is locked.
  std::optional<std::string> channel_description;
  std::optional<si8> start_time, end_time;
  /// Stored samples (gaps excluded); nullopt when a section 2 is locked.
  std::optional<si8> number_of_samples;
  std::vector<InspectSegment> segments;
  std::vector<InspectGap> gaps;
  /// true only when the block indices were read (InspectOptions::read_index)
  /// and every segment could be placed; otherwise `gaps` holds the gaps
  /// BETWEEN segments and may miss ones inside a segment.
  bool gaps_complete = false;
  /// false when a listed gap may not be one: a segment that could not be
  /// read or placed (lenient mode) may hold data there, or a segment placed
  /// with an assumed offset 0 sits next to one with a read non-zero offset.
  bool gaps_certain = true;
  std::uint64_t bytes = 0;  ///< every file of the channel, .tdat included (sizes only)
  /// Some of its times (segments or records) assume offset 0: not verified.
  bool offset_assumed_zero = false;
  std::size_t records = 0;  ///< channel- and segment-level records
};

struct InspectRecord {
  RecordLevel level = RecordLevel::Session;
  std::string channel;
  int segment = -1;
  std::string type;
  /// Header time, absolute uUTC. nullopt when it was stored relative to an
  /// offset that is disputed or belongs to an unreadable segment.
  std::optional<si8> time;
  /// false when the file's times depend on an offset that is disputed or
  /// unknown (an unreadable segment in scope): then every time here (and in
  /// `full`) is unset, never guessed.
  bool times_known = true;
  /// The times were resolved with an ASSUMED offset 0 (a section 3 in their
  /// scope is locked), as the Reader does. Not verified.
  bool offset_assumed_zero = false;
  std::optional<si8> duration;
  /// Earliest and latest time the record mentions: the header time, the body
  /// times (Seiz onsets/offsets, Epoc timestamp/end, Curs trace timestamp) and
  /// time + duration. What an anonymizer's time range must cover.
  std::optional<si8> first_time, last_time;
  /// Bytes of free text in the body (every string field). 0 for none.
  std::size_t text_bytes = 0;
  /// The body is encrypted and the password does not open it: only the
  /// header (type, time) was read.
  bool body_locked = false;
  std::optional<Record> full;  ///< include_values only
};

/// One identifying field: where it is, and whether it holds anything.
struct IdentityField {
  std::string name;       ///< e.g. "subject_name_1", "session_name", "record_text"
  std::string location;   ///< e.g. "metadata section 3", "universal header"
  std::size_t occurrences = 0;  ///< places (files or records) where it is non-empty
  /// places it could not be read: encrypted, or (lenient mode) a file that
  /// failed. Either way it MAY be set there, so it is not "absent".
  std::size_t locked = 0;
  std::size_t max_length = 0;   ///< longest value, in bytes
  std::size_t distinct = 0;     ///< distinct non-empty values
  std::vector<std::string> values;  ///< distinct values; include_values only
};

struct InspectWarning {
  std::string code;
  std::string message;
  /// anonymize_session (#70) refuses a session with this condition.
  bool blocks_anonymize = false;
};

struct EncryptionSummary {
  bool encrypted = false;  ///< any metadata section or record body is encrypted
  /// What the password opened, for the WORST segment: "full" (everything, or
  /// nothing was encrypted), "level-1" (signal metadata, not section 3 or
  /// level-2 record bodies), "none" (no password; only the plain parts).
  std::string access = "full";
  int section2_level = 0;  ///< highest level stored across segments (0 = plain)
  int section3_level = 0;
  std::size_t section2_locked = 0;  ///< segments whose section 2 stayed locked
  std::size_t section3_locked = 0;
  std::size_t record_bodies_locked = 0;
};

struct TimeOffsetSummary {
  /// "none": no readable segment sets an offset. "applied": meflib's
  /// recording-time offset is set, and the readable segments agree on it.
  /// "disputed": they disagree. "locked": no offset seen, but some section 3
  /// could not be read, so there may be one. "unknown": no segment could be
  /// read at all. Every status describes the READABLE segments only; see
  /// segments_unknown for the rest.
  std::string status = "none";
  std::size_t segments_with_offset = 0;
  std::size_t segments_locked = 0;
  /// Segments that could not be read at all (lenient mode, incomplete
  /// folders): whatever offset they carry is unknown.
  std::size_t segments_unknown = 0;
  /// The offset is set, so the real recording date is stored in section 3
  /// and can be recovered (docs/anonymization.md).
  bool date_recoverable = false;
  /// "anyone" (section 3 stored in plain) or "level-2 password" (encrypted);
  /// "" when date_recoverable is false.
  std::string recoverable_by;
  /// A time is stored non-negative under a non-zero offset, so it cannot be
  /// told apart from an absolute time (docs/anonymization.md, "The sign
  /// convention is not reliable either").
  bool ambiguous = false;
  std::optional<si8> value;  ///< include_values and status "applied" only
  std::string interpretation;  ///< plain words: how times were read
};

struct SessionSummary {
  std::string name;    ///< the session's name (folder stem)
  std::string path;
  std::string layout;  ///< "directory" or "tar"
  /// Every MEF file inspect accounts for (metadata, indices, data, records),
  /// by size. Video channels and unknown files are not counted.
  std::uint64_t total_bytes = 0;
  std::optional<std::uint64_t> archive_bytes;  ///< tar only: the archive file
  EncryptionSummary encryption;
  TimeOffsetSummary time_offset;
  /// Section 3 time-zone fields of the first segment that has them; nullopt
  /// when locked or NO_ENTRY.
  std::optional<si4> gmt_offset;
  std::optional<si8> dst_start_time, dst_end_time;
  std::optional<si8> start_time, end_time;                   ///< samples, all channels
  std::optional<si8> records_start_time, records_end_time;   ///< records, all levels
  std::optional<si8> first_time, last_time;                  ///< samples AND records
  std::vector<InspectChannel> channels;            ///< by name
  std::vector<std::string> record_only_channels;   ///< records but no segments yet
  std::vector<std::string> video_channels;         ///< .vidd: out of scope, reported
  /// .timd folders without one complete segment (no .tmet/.tidx/.tdat set).
  /// A Reader does not list them; their files are counted and reported.
  std::vector<std::string> incomplete_channels;
  std::vector<InspectRecord> records;              ///< session -> channel -> segment
  std::vector<IdentityField> identity;
  std::vector<InspectWarning> warnings;
  std::vector<SegmentProblem> problems;            ///< lenient mode only
  std::vector<DeclarationIssue> declaration_issues;
  bool index_read = false;
  /// The password opened some metadata or record file only through the
  /// password-to-key mapping of mef3io 1.2.0 and earlier (issue #84,
  /// crypto::extract_password_bytes_legacy). The data is read correctly, but
  /// meflib and pymef cannot open those files with this password. A
  /// "legacy-password" warning says how many files.
  bool legacy_password_mapping = false;
  /// Some time in this summary assumes offset 0 because a section 3 is
  /// locked (level-1 password or none). Not verified.
  bool offset_assumed_zero = false;
};

/// Describe the session at `path` (.mefd directory or .mefd.tar archive).
/// Reads metadata, record files and, with read_index, block indices — never a
/// .tdat.
/// @throws IoError        not a session path
/// @throws PasswordError  a password was given and is wrong
/// @throws MefError       (strict only) a segment or record file is unreadable
/// @throws Cancelled      `control.cancel` was set (checked per segment and per
///                        record file); nothing is returned.
/// @param control  optional cancel token and progress callback (#69).
///                 Progress is in bytes of the files the segment pass reads:
///                 every .tmet, plus every .tidx with read_index. Record files
///                 are not counted. Sized (one stat per file) only when a
///                 progress callback is attached.
SessionSummary inspect_session(const std::string& path, const InspectOptions& options = {},
                               const Control& control = {});

}  // namespace mef3io
