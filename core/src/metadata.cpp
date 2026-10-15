// mef3io — .tmet loader with CRC validation, password check, decryption.
#include "mef3io/metadata.hpp"

#include <optional>
#include <vector>
#include <algorithm>

#include "mef3io/crc.hpp"
#include "mef3io/crypto.hpp"
#include "mef3io/errors.hpp"

namespace mef3io {
namespace {
// The universal-header CRC rule shared by the full loader and the offset
// probe (see load_time_series_metadata for why CRC_NO_ENTRY is accepted only
// on an otherwise self-consistent header).
void check_tmet_universal_header(const fmt::UniversalHeader& uh, std::span<const ui1> bytes) {
  using namespace fmt;
  const bool header_unverifiable = uh.header_crc == CRC_NO_ENTRY &&
                                   uh.file_type_string == FILE_TYPE_TS_METADATA &&
                                   uh.byte_order_code == MEF_LITTLE_ENDIAN;
  if (!header_unverifiable && !uh.header_crc_valid(bytes))
    throw CrcError("tmet universal header CRC mismatch");
}
}  // namespace

std::optional<si8> probe_recording_time_offset(std::span<const ui1> head,
                                               std::span<const ui1> section3,
                                               const std::string& password) {
  using namespace fmt;
  if (head.size() < static_cast<std::size_t>(METADATA_SECTION_2_OFFSET) ||
      section3.size() < static_cast<std::size_t>(METADATA_SECTION_3_BYTES))
    throw FormatError("tmet too small to hold its universal header, section 1 and section 3");
  const auto uh = UniversalHeader::parse(head);
  check_tmet_universal_header(uh, head);
  const auto s1 = MetadataSection1::parse(
      head.subspan(METADATA_SECTION_1_OFFSET, METADATA_SECTION_1_BYTES));
  const int s2_enc = s1.section_2_encryption > 0 ? s1.section_2_encryption : 0;
  const int s3_enc = s1.section_3_encryption > 0 ? s1.section_3_encryption : 0;
  std::vector<ui1> s3(section3.begin(), section3.begin() + METADATA_SECTION_3_BYTES);
  if (s2_enc > 0 || s3_enc > 0) {
    const auto keys = crypto::validate_password(password, uh.level_1_password_validation_field,
                                                uh.level_2_password_validation_field);
    if (keys.access_level == LEVEL_0_ACCESS)
      throw PasswordError("tmet is encrypted and the password is missing or incorrect");
    if (s3_enc > 0) {
      std::span<const ui1> key;
      if (s3_enc == LEVEL_1_ENCRYPTION && keys.level1_key) key = *keys.level1_key;
      if (s3_enc == LEVEL_2_ENCRYPTION && keys.level2_key) key = *keys.level2_key;
      if (key.empty()) return std::nullopt;  // locked: the offset is unknown, not 0
      auto dec = crypto::aes128_ecb_decrypt(s3, key);
      std::copy(dec.begin(), dec.end(), s3.begin());
    }
  }
  const si8 rto = MetadataSection3::parse(s3).recording_time_offset;
  return rto == UUTC_NO_ENTRY ? 0 : rto;
}

TimeSeriesMetadata load_time_series_metadata(std::span<const ui1> tmet_bytes,
                                             const std::string& password) {
  using namespace fmt;
  if (tmet_bytes.size() < METADATA_FILE_BYTES)
    throw FormatError("tmet file too small: expected " + std::to_string(METADATA_FILE_BYTES) +
                      " bytes, got " + std::to_string(tmet_bytes.size()));

  TimeSeriesMetadata md;
  md.universal_header = UniversalHeader::parse(tmet_bytes);
  // A stored CRC of CRC_NO_ENTRY (0) means the writer never computed one —
  // meflib's own marker, and legitimate: it computes a body CRC only when
  // writing a whole file at once. Rejecting it here would throw from this
  // constructor and take the WHOLE SESSION down over a file that reads
  // perfectly. A real mismatch still throws.
  //
  // But `0` is ALSO what the commonest corruption produces — a torn write or a
  // sparse hole zeroes the CRC along with everything around it — so the
  // exemption cannot be granted on the CRC field alone: the damage would
  // switch off the only check that would have caught it. Require the rest of
  // the universal header to be self-consistent first. A zeroed or garbled
  // header fails this and is rejected as it was before; a genuine meflib file
  // carries a correct type string and byte-order code and passes.
  check_tmet_universal_header(md.universal_header, tmet_bytes);
  // The body CRC covers sections 1-3 (encryption flags, fs/ufact/counts,
  // subject metadata) — 15/16 of the file. Without this check, body
  // corruption silently yields garbage scaling.
  //
  // Bound the CRC by the record's declared size, NOT by EOF: .tmet is a
  // fixed-length record (1024 B universal header + 15360 B of sections) and
  // some writers leave trailing bytes past its end. Those bytes are not part
  // of the record the stored CRC was computed over, so hashing to EOF rejects
  // intact metadata as corrupt — and every read of such a session fails.
  if (md.universal_header.body_crc != CRC_NO_ENTRY) {
    ui4 body = crc::calculate(
        tmet_bytes.subspan(UNIVERSAL_HEADER_BYTES, METADATA_FILE_BYTES - UNIVERSAL_HEADER_BYTES));
    if (body != md.universal_header.body_crc)
      throw CrcError("tmet body CRC mismatch (metadata sections corrupted)");
  }

  // Section 1 is never encrypted.
  auto s1_bytes = tmet_bytes.subspan(METADATA_SECTION_1_OFFSET, METADATA_SECTION_1_BYTES);
  md.section1 = MetadataSection1::parse(s1_bytes);

  // Only a strictly positive encryption level means the section is encrypted
  // on disk. A negative "_DECRYPTED" sentinel (e.g. -1) marks a section that is
  // conceptually encryptable but currently stored as plaintext (unencrypted
  // files), and 0 means never encrypted.
  int s2_enc = md.section1.section_2_encryption > 0 ? md.section1.section_2_encryption : 0;
  int s3_enc = md.section1.section_3_encryption > 0 ? md.section1.section_3_encryption : 0;
  const bool encrypted = s2_enc > 0 || s3_enc > 0;

  crypto::AccessKeys keys;
  if (encrypted) {
    keys = crypto::validate_password(
        password, md.universal_header.level_1_password_validation_field,
        md.universal_header.level_2_password_validation_field);
    if (keys.access_level == LEVEL_0_ACCESS)
      throw PasswordError("tmet is encrypted and the password is missing or incorrect");
  }
  md.access_level = keys.access_level;

  // Copy the two section images so we can decrypt without mutating the input.
  std::vector<ui1> s2(tmet_bytes.begin() + METADATA_SECTION_2_OFFSET,
                      tmet_bytes.begin() + METADATA_SECTION_2_OFFSET +
                          TIME_SERIES_METADATA_SECTION_2_BYTES);
  std::vector<ui1> s3(tmet_bytes.begin() + METADATA_SECTION_3_OFFSET,
                      tmet_bytes.begin() + METADATA_SECTION_3_OFFSET + METADATA_SECTION_3_BYTES);

  auto key_for = [&](int level) -> std::span<const ui1> {
    if (level == LEVEL_1_ENCRYPTION && keys.level1_key) return *keys.level1_key;
    if (level == LEVEL_2_ENCRYPTION && keys.level2_key) return *keys.level2_key;
    return {};
  };

  if (s2_enc > 0) {
    auto key = key_for(s2_enc);
    if (key.empty()) throw PasswordError("insufficient access to decrypt metadata section 2");
    auto dec = crypto::aes128_ecb_decrypt(s2, key);
    std::copy(dec.begin(), dec.end(), s2.begin());
  }
  md.section2 = TimeSeriesMetadataSection2::parse(s2);

  if (s3_enc > 0) {
    auto key = key_for(s3_enc);
    if (key.empty()) {
      // L2 section not accessible with an L1 password: leave section3 default.
      md.section3_available = false;
    } else {
      auto dec = crypto::aes128_ecb_decrypt(s3, key);
      std::copy(dec.begin(), dec.end(), s3.begin());
      md.section3 = MetadataSection3::parse(s3);
    }
  } else {
    md.section3 = MetadataSection3::parse(s3);
  }

  return md;
}

}  // namespace mef3io
