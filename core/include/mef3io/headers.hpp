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
// Provenance: which mef3io version created a file, which last modified it,
// and WHAT was done to it. NO TIMES — deliberately: the universal header is
// never encrypted, and a write time would date a recording that
// recording_time_offset exists to hide.
//
// Kept in the universal header's 64-byte DISCRETIONARY region (file offset
// 960), which MEF 3.0 leaves to the writing application: meflib never
// interprets it, pymef only reports it as raw bytes, and it sits under the
// header CRC every writer already recomputes. Every file carries its own.
//
// FROZEN FORMAT (docs/mef3_format.md, "Provenance region"). Offsets, widths
// and encodings below never change; operation codes are append-only and never
// reused; later layout versions may only ASSIGN the reserved bytes. Pinned by
// a golden-bytes test — moving anything here fails it.
//
//   off  len  field
//     0    4  magic "M3IO"
//     4    1  layout version (1)
//     5    1  last operation (code)
//     6    2  reserved, zero
//     8   20  created-by version     ASCII, NUL-padded, <= 19 chars + NUL
//    28   20  last-modified-by       ASCII, NUL-padded, <= 19 chars + NUL
//    48    4  operations-ever mask   ui4 LE, bit n = code n ever applied
//    52    4  modification count     ui4 LE, operations after creation;
//                                    SATURATES at 0xFFFFFFFF, never wraps
//    56    8  reserved, zero
//
// All-zero region = never stamped (meflib, pymef, mef_tools, mef3io <= 1.1):
// a modification initialises it, leaving created-by empty, so mef3io never
// claims a file it did not create. Anything else without the magic is another
// application's data and is NEVER written. A writer preserves every byte it
// does not own (reserved bytes, a newer layout's fields).
// ---------------------------------------------------------------------------
namespace provenance {
inline constexpr std::size_t REGION_OFFSET = 960;  // within the universal header
inline constexpr std::size_t REGION_BYTES = 64;
inline constexpr std::size_t MAGIC_OFFSET = 0, MAGIC_BYTES = 4;
inline constexpr std::size_t LAYOUT_OFFSET = 4;
inline constexpr std::size_t LAST_OP_OFFSET = 5;
inline constexpr std::size_t CREATED_BY_OFFSET = 8, VERSION_BYTES = 20;
inline constexpr std::size_t MODIFIED_BY_OFFSET = 28;
inline constexpr std::size_t MASK_OFFSET = 48;
inline constexpr std::size_t COUNT_OFFSET = 52;
inline constexpr ui1 MAGIC[MAGIC_BYTES] = {'M', '3', 'I', 'O'};
inline constexpr ui1 LAYOUT_VERSION = 1;
inline constexpr ui4 COUNT_MAX = 0xFFFFFFFFu;

// Every field must lie inside the region, and the region inside the header —
// checked by the compiler, so no edit can make a stamp write out of range.
static_assert(REGION_OFFSET + REGION_BYTES == static_cast<std::size_t>(UNIVERSAL_HEADER_BYTES));
static_assert(MAGIC_OFFSET + MAGIC_BYTES <= LAYOUT_OFFSET);
static_assert(LAYOUT_OFFSET < LAST_OP_OFFSET && LAST_OP_OFFSET < CREATED_BY_OFFSET);
static_assert(CREATED_BY_OFFSET + VERSION_BYTES <= MODIFIED_BY_OFFSET);
static_assert(MODIFIED_BY_OFFSET + VERSION_BYTES <= MASK_OFFSET);
static_assert(MASK_OFFSET + sizeof(ui4) <= COUNT_OFFSET);
static_assert(COUNT_OFFSET + sizeof(ui4) <= REGION_BYTES);

/// Operation codes. APPEND-ONLY: a code is never renumbered, reused or
/// redefined. Codes must stay below 32 to have a bit in the operations mask.
enum class Operation : ui1 {
  Unset = 0,
  Create = 1,          ///< file written fresh
  Append = 2,          ///< samples added to an existing segment
  HeaderRepair = 3,    ///< repair_session: declarations only, samples untouched
  Recovery = 4,        ///< recover_session: index and data reconciled
  MetadataUpdate = 5,  ///< reserved: descriptive metadata rewritten in place
};
inline constexpr ui1 MAX_CODE = 5;
static_assert(MAX_CODE < 32, "every operation code needs a bit in the 32-bit mask");

/// Stable lowercase name of a code ("create", "append", ...); nullptr if unknown.
const char* operation_name(ui1 code);

struct Provenance {
  bool present = false;        ///< region carries the magic
  ui1 layout_version = 0;
  ui1 last_operation = 0;      ///< raw code; may be newer than this library knows
  std::string created_by;      ///< "" when unknown
  std::string last_modified_by;
  ui4 operations_mask = 0;
  ui4 modification_count = 0;

  bool ever(Operation op) const {
    return (operations_mask >> static_cast<unsigned>(op)) & 1u;
  }
};

Provenance read(const UniversalHeader& uh);
/// A NEW file: the region is rewritten whole (created-by = last-modified-by =
/// this version, last operation create, count 0). `version` defaults to this
/// library's; it is a parameter only so tests can prove that ANY string —
/// over-long, non-ASCII — stays inside its 20-byte field.
void stamp_created(UniversalHeader& uh, std::string_view version = {});
/// A MODIFIED file: last operation, last-modified-by, mask and count updated;
/// every other byte preserved. Returns false, writing nothing, when the region
/// is another application's.
bool stamp_modified(UniversalHeader& uh, Operation op, std::string_view version = {});
}  // namespace provenance

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
