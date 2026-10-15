// mef3io — load and decrypt a time-series metadata (.tmet) file.
#pragma once

#include <optional>
#include <span>
#include <string>

#include "mef3io/headers.hpp"

namespace mef3io {

struct TimeSeriesMetadata {
  fmt::UniversalHeader universal_header;
  fmt::MetadataSection1 section1;
  fmt::TimeSeriesMetadataSection2 section2;
  fmt::MetadataSection3 section3;
  int access_level = fmt::LEVEL_0_ACCESS;  // level attained with the password
  bool section3_available = true;          // false if L2-encrypted and only L1 access
};

/// Just the recording-time offset of a .tmet record (the first
/// METADATA_FILE_BYTES of the file), for a writer that must learn every
/// segment's offset. Verifies exactly what the full loader verifies — the
/// universal-header CRC, the body CRC (bounded by the record size) and the
/// password — and throws on any failure, but decrypts and parses only section
/// 3. Returns nullopt when section 3 is locked (level-1 access only), and maps
/// NO_ENTRY to 0. Section 3 has no CRC of its own, so without the body check a
/// damaged (e.g. zeroed) offset would be trusted — and 0 writes the real date.
std::optional<si8> probe_recording_time_offset(std::span<const ui1> tmet_bytes,
                                               const std::string& password);

/// The recording-time offset this segment's times are resolved with: section
/// 3's value, or 0 when section 3 is unreadable (level-1 access) or NO_ENTRY.
/// Pass it to to_user_time / to_disk_time (mef3io/time.hpp).
inline si8 segment_rto(const TimeSeriesMetadata& md) {
  if (!md.section3_available || md.section3.recording_time_offset == fmt::UUTC_NO_ENTRY)
    return 0;
  return md.section3.recording_time_offset;
}

// Parse a 16 KB .tmet image. Validates the universal-header CRC (throws
// CrcError on mismatch), then, if encrypted, validates `password` and decrypts
// sections 2/3 in place before parsing. Section 3 requires L2 access when
// L2-encrypted; if the password only grants L1, section3_available is false
// and section3 holds parsed-but-still-encrypted garbage (callers should not
// use it in that case).
TimeSeriesMetadata load_time_series_metadata(std::span<const ui1> tmet_bytes,
                                             const std::string& password = "");

}  // namespace mef3io
