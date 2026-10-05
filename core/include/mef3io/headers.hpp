// mef3io — MEF 3.0 fixed-layout structures: parse/serialize against the
// on-disk little-endian byte layout. Offsets transcribed from the MEF 3.0
// spec (and cross-checked against meflib / pymef / reimplementation.py).
#pragma once

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "mef3io/types.hpp"

namespace mef3io::fmt {

// ---------------------------------------------------------------------------
// Universal header (1024 B). Prefixes every .tmet/.tidx/.tdat/.rdat/.ridx file.
// ---------------------------------------------------------------------------
struct UniversalHeader {
  ui4 header_crc = 0;
  ui4 body_crc = 0;
  std::string file_type_string;           // 4 chars (+ null) in a 5-byte field
  ui1 mef_version_major = MEF_VERSION_MAJOR;
  ui1 mef_version_minor = MEF_VERSION_MINOR;
  ui1 byte_order_code = MEF_LITTLE_ENDIAN;
  si8 start_time = UUTC_NO_ENTRY;
  si8 end_time = UUTC_NO_ENTRY;
  si8 number_of_entries = SI8_NO_ENTRY;
  si8 maximum_entry_size = SI8_NO_ENTRY;
  si4 segment_number = SI4_NO_ENTRY;
  std::string channel_name;               // utf8, 256-byte field
  std::string session_name;               // utf8, 256-byte field
  std::string anonymized_name;            // utf8, 256-byte field
  std::array<ui1, 16> level_uuid{};
  std::array<ui1, 16> file_uuid{};
  std::array<ui1, 16> provenance_uuid{};
  std::array<ui1, 16> level_1_password_validation_field{};
  std::array<ui1, 16> level_2_password_validation_field{};
  std::array<ui1, 60> protected_region{};
  std::array<ui1, 64> discretionary_region{};

  static UniversalHeader parse(std::span<const ui1> buf);   // buf.size() >= 1024
  void serialize(std::span<ui1> buf) const;                 // buf.size() >= 1024

  // CRC over bytes [4, 1024) matches the stored header_crc.
  bool header_crc_valid(std::span<const ui1> file_bytes) const;
  // Recompute and set header_crc from the current field values.
  void update_header_crc(std::span<ui1> file_bytes);

  bool is_password_protected() const;
};

// ---------------------------------------------------------------------------
// Writer stamp: which mef3io version created a file, and which last wrote it.
//
// Kept in the universal header's 64-byte DISCRETIONARY region (offset 960),
// which MEF 3.0 leaves to the writing application: meflib never interprets
// it, pymef only reports it as raw bytes, and it sits under the header CRC
// every writer already recomputes. Every file carries a universal header, so
// every file carries its own stamp.
//
// Two 32-byte slots, each "mef3io <version>" NUL-padded: [0, 32) = CREATED BY,
// written once when the file is made; [32, 64) = LAST WRITTEN BY, refreshed by
// an append, a repair or a recovery. A slot counts only if it begins with the
// prefix, so zeros (meflib, pymef, mef_tools, mef3io <= 1.1.x) read as
// "unknown" and nothing else can be mistaken for a stamp.
//
// A region holding anything else belongs to another application: it is LEFT
// ALONE, and that file simply carries no stamp from mef3io.
// ---------------------------------------------------------------------------
inline constexpr std::size_t WRITER_STAMP_SLOT_BYTES = 32;
inline constexpr std::string_view WRITER_STAMP_PREFIX = "mef3io ";

/// "mef3io <version>" of the writer that created the file; empty if unknown.
std::string created_by(const UniversalHeader& uh);
/// "mef3io <version>" of the writer that last modified the file; empty if unknown.
std::string last_written_by(const UniversalHeader& uh);
/// Stamp a NEW file: both slots set to this library's version.
void stamp_created(UniversalHeader& uh);
/// Stamp a MODIFIED file: refresh the last-written slot. The created slot is
/// kept as is — left empty for a file mef3io did not create. Returns false,
/// writing nothing, when the region holds another application's bytes.
bool stamp_modified(UniversalHeader& uh);

// ---------------------------------------------------------------------------
// Metadata section 1 (1536 B): encryption levels for sections 2 and 3.
// ---------------------------------------------------------------------------
struct MetadataSection1 {
  si1 section_2_encryption = NO_ENCRYPTION;   // may be negative (_DECRYPTED)
  si1 section_3_encryption = NO_ENCRYPTION;
  // protected/discretionary regions omitted (zeros on write).

  static MetadataSection1 parse(std::span<const ui1> buf);  // section-relative
  void serialize(std::span<ui1> buf) const;
};

// ---------------------------------------------------------------------------
// Time-series metadata section 2 (10752 B).
// ---------------------------------------------------------------------------
struct TimeSeriesMetadataSection2 {
  std::string channel_description;
  std::string session_description;
  si8 recording_duration = SI8_NO_ENTRY;
  std::string reference_description;
  si8 acquisition_channel_number = SI8_NO_ENTRY;
  sf8 sampling_frequency = -1.0;
  sf8 low_frequency_filter_setting = -1.0;
  sf8 high_frequency_filter_setting = -1.0;
  sf8 notch_filter_frequency_setting = -1.0;
  sf8 ac_line_frequency = -1.0;
  sf8 units_conversion_factor = 0.0;
  std::string units_description;
  sf8 maximum_native_sample_value = 0.0;
  sf8 minimum_native_sample_value = 0.0;
  si8 start_sample = 0;
  si8 number_of_samples = 0;
  si8 number_of_blocks = 0;
  si8 maximum_block_bytes = 0;
  ui4 maximum_block_samples = 0;
  ui4 maximum_difference_bytes = 0;
  si8 block_interval = 0;
  si8 number_of_discontinuities = 0;
  si8 maximum_contiguous_blocks = 0;
  si8 maximum_contiguous_block_bytes = 0;
  si8 maximum_contiguous_samples = 0;

  static TimeSeriesMetadataSection2 parse(std::span<const ui1> buf);  // section-relative
  void serialize(std::span<ui1> buf) const;

  /// Write back ONLY the derived numeric fields (counts, maxima, times) into
  /// an existing section-2 image, leaving every other byte exactly as it was.
  ///
  /// `serialize` zero-fills the whole 10752-byte section and then writes the
  /// fields this struct models — which ends at offset 6432. meflib puts a
  /// 2160-byte protected region at 6432 and a 2160-byte discretionary region
  /// at 8592, so a repair that re-serialized would silently destroy 4320 bytes
  /// it does not understand. Text fields are left alone too: round-tripping a
  /// description through std::string can shorten one that was stored without a
  /// NUL terminator.
  void serialize_derived_fields(std::span<ui1> buf) const;
};

// ---------------------------------------------------------------------------
// Metadata section 3 (3072 B).
// ---------------------------------------------------------------------------
struct MetadataSection3 {
  si8 recording_time_offset = UUTC_NO_ENTRY;
  si8 dst_start_time = UUTC_NO_ENTRY;
  si8 dst_end_time = UUTC_NO_ENTRY;
  si4 gmt_offset = GMT_OFFSET_NO_ENTRY;
  std::string subject_name_1;
  std::string subject_name_2;
  std::string subject_id;
  std::string recording_location;

  static MetadataSection3 parse(std::span<const ui1> buf);  // section-relative
  void serialize(std::span<ui1> buf) const;
};

// ---------------------------------------------------------------------------
// Time-series index entry (56 B). One per RED block in a .tidx file.
// ---------------------------------------------------------------------------
struct TimeSeriesIndex {
  si8 file_offset = SI8_NO_ENTRY;        // byte offset of the block in .tdat
  si8 start_time = UUTC_NO_ENTRY;        // uUTC of first sample (offset-relative)
  si8 start_sample = SI8_NO_ENTRY;       // sample index within the channel
  ui4 number_of_samples = UI4_NO_ENTRY;
  ui4 block_bytes = UI4_NO_ENTRY;
  si4 maximum_sample_value = RED_NAN;
  si4 minimum_sample_value = RED_NAN;
  ui1 red_block_flags = 0;

  static TimeSeriesIndex parse(std::span<const ui1> buf);  // 56-byte entry
  void serialize(std::span<ui1> buf) const;
};

// ---------------------------------------------------------------------------
// RED block header (304 B) at the start of each compressed block in .tdat.
// ---------------------------------------------------------------------------
struct RedBlockHeader {
  ui4 crc = 0;
  ui1 flags = 0;
  sf4 detrend_slope = 0.0f;
  sf4 detrend_intercept = 0.0f;
  sf4 scale_factor = 0.0f;
  ui4 difference_bytes = 0;
  ui4 number_of_samples = 0;
  ui4 block_bytes = 0;
  si8 start_time = UUTC_NO_ENTRY;
  std::array<ui1, 256> statistics{};  // symbol frequency table

  static RedBlockHeader parse(std::span<const ui1> buf);  // >= 304 bytes
  void serialize(std::span<ui1> buf) const;

  // Offset of `difference_bytes` within the header. Exposed so a writer can
  // read it back from an encoded block without parsing (and copying) the
  // 256-byte statistics table for every block.
  static constexpr int DIFFERENCE_BYTES_OFFSET = 28;

  // RED block header flag bits (from meflib.h).
  static constexpr ui1 DISCONTINUITY_MASK = 0x01;
  static constexpr ui1 LEVEL_1_ENCRYPTION_MASK = 0x02;
  static constexpr ui1 LEVEL_2_ENCRYPTION_MASK = 0x04;
};

}  // namespace mef3io::fmt
