// mef3io — record (.rdat) parsing.
#include "mef3io/records.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <vector>

#include "mef3io/byteio.hpp"
#include "mef3io/crc.hpp"
#include "mef3io/errors.hpp"
#include "mef3io/headers.hpp"

namespace fs = std::filesystem;

namespace mef3io {
namespace {
using byteio::read;
using byteio::read_string;
using byteio::write;
using byteio::write_string;

// Same negated-on-disk convention as time-series times.
si8 to_user_time(si8 t, si8 rto) {
  if (t == fmt::UUTC_NO_ENTRY) return t;
  if (t >= 0) return t;
  return -t + rto;
}
si8 to_disk_time(si8 absolute, si8 rto) {
  if (absolute == fmt::UUTC_NO_ENTRY) return absolute;
  return rto - absolute;
}
constexpr ui1 PAD_BYTE = 0x7e;
}  // namespace

std::vector<Record> parse_records(std::span<const ui1> rdat, si8 rto,
                                  const crypto::AccessKeys& keys) {
  std::vector<Record> out;
  std::size_t off = fmt::UNIVERSAL_HEADER_BYTES;  // records follow the UH

  while (off + fmt::RECORD_HEADER_BYTES <= rdat.size()) {
    // Record header (24 B).
    std::string type = read_string(rdat, off + 4, 4);
    if (type.empty()) break;  // padding / end
    ui1 vmaj = read<ui1>(rdat, off + 9);
    ui1 vmin = read<ui1>(rdat, off + 10);
    si1 enc = read<si1>(rdat, off + 11);
    ui4 body_bytes = read<ui4>(rdat, off + 12);
    si8 time = read<si8>(rdat, off + 16);

    std::size_t body_off = off + fmt::RECORD_HEADER_BYTES;
    if (body_off + body_bytes > rdat.size()) break;  // truncated

    // Decrypt the body if encrypted.
    std::vector<ui1> body(rdat.begin() + body_off, rdat.begin() + body_off + body_bytes);
    if (enc > 0) {
      std::span<const ui1> key;
      if (enc == fmt::LEVEL_1_ENCRYPTION && keys.level1_key)
        key = *keys.level1_key;
      else if (enc == fmt::LEVEL_2_ENCRYPTION && keys.level2_key)
        key = *keys.level2_key;
      if (key.empty()) throw PasswordError("record body encrypted; insufficient access");
      // Body is padded to a multiple of 16 for AES.
      std::size_t pad = (body.size() + 15) & ~std::size_t{15};
      body.resize(pad, 0);
      auto dec = crypto::aes128_ecb_decrypt(body, key);
      body = std::move(dec);
    }
    std::span<const ui1> b(body);

    Record rec;
    rec.type = type;
    rec.time = to_user_time(time, rto);
    rec.version_major = vmaj;
    rec.version_minor = vmin;

    if (type == "Note" || type == "SyLg") {
      rec.text = read_string(b, 0, b.size());
    } else if (type == "EDFA") {
      if (b.size() >= 8) rec.duration = read<si8>(b, 0);
      rec.text = read_string(b, 8, b.size() > 8 ? b.size() - 8 : 0);
    } else if (type == "Seiz") {
      if (b.size() >= 24) {
        rec.earliest_onset = read<si8>(b, 0);
        rec.latest_offset = read<si8>(b, 8);
        rec.duration = read<si8>(b, 16);
      }
    }
    // Other types: header fields only (type/time) for now.

    out.push_back(std::move(rec));

    // Advance. Records are laid out contiguously; body may be padded to a
    // 16-byte boundary when the file uses encryption alignment.
    std::size_t advance = fmt::RECORD_HEADER_BYTES + body_bytes;
    off += advance;
  }
  return out;
}

namespace {

// Build the body bytes for a record (before padding/encryption).
std::vector<ui1> record_body(const Record& r) {
  std::vector<ui1> body;
  auto put_str = [&](const std::string& s) {
    body.insert(body.end(), s.begin(), s.end());
    body.push_back(0);  // null terminator
  };
  if (r.type == "Note" || r.type == "SyLg") {
    put_str(r.text.value_or(""));
  } else if (r.type == "EDFA") {
    body.resize(8);
    write<si8>(body, 0, r.duration.value_or(0));
    std::string ann = r.text.value_or("");
    body.insert(body.end(), ann.begin(), ann.end());
    body.push_back(0);
  } else {
    // Unknown/unsupported type: empty body.
  }
  return body;
}

void finalize_crcs(std::vector<ui1>& file) {
  ui4 body_crc = crc::calculate(std::span<const ui1>(file).subspan(fmt::UNIVERSAL_HEADER_BYTES));
  write<ui4>(file, 4, body_crc);
  ui4 header_crc =
      crc::calculate(std::span<const ui1>(file).subspan(4, fmt::UNIVERSAL_HEADER_BYTES - 4));
  write<ui4>(file, 0, header_crc);
}

std::vector<ui1> base_uh_bytes(const std::string& ftype, const std::string& session_name,
                               const std::string& channel_name, int segment_number, si8 n_entries,
                               si8 max_entry, si8 start_disk, si8 end_disk,
                               const crypto::ValidationFields& vf) {
  fmt::UniversalHeader uh;
  uh.file_type_string = ftype;
  uh.start_time = start_disk;
  uh.end_time = end_disk;
  uh.number_of_entries = n_entries;
  uh.maximum_entry_size = max_entry;
  uh.segment_number = segment_number;
  uh.session_name = session_name;
  uh.channel_name = channel_name;
  std::copy(vf.level1.begin(), vf.level1.end(), uh.level_1_password_validation_field.begin());
  std::copy(vf.level2.begin(), vf.level2.end(), uh.level_2_password_validation_field.begin());
  std::vector<ui1> buf(fmt::UNIVERSAL_HEADER_BYTES);
  uh.serialize(buf);
  return buf;
}

void write_file(const std::string& path, const std::vector<ui1>& bytes) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) throw IoError("cannot open for write: " + path);
  f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

}  // namespace

void write_records(const std::string& dir, const std::string& base,
                   const std::string& session_name, const std::string& channel_name,
                   int segment_number, const std::vector<Record>& records, si8 rto,
                   const std::string& password_1, const std::string& password_2) {
  if (records.empty()) return;
  const bool encrypt = !password_1.empty();
  crypto::ValidationFields vf =
      encrypt ? crypto::make_validation_fields(password_1, password_2) : crypto::ValidationFields{};
  crypto::AccessKeys keys;
  if (encrypt)
    keys = crypto::validate_password(password_2.empty() ? password_1 : password_2, vf.level1,
                                     vf.level2);

  // Records are time-ordered on disk.
  std::vector<const Record*> ordered;
  for (const auto& r : records) ordered.push_back(&r);
  std::sort(ordered.begin(), ordered.end(),
            [](const Record* a, const Record* b) { return a->time < b->time; });

  std::vector<ui1> rdat_body;
  std::vector<ui1> ridx_body;
  ui4 max_entry = 0;
  si8 start_disk = fmt::UUTC_NO_ENTRY, end_disk = fmt::UUTC_NO_ENTRY;

  for (const Record* rp : ordered) {
    const Record& r = *rp;
    std::vector<ui1> body = record_body(r);
    // Pad body to a 16-byte multiple with PAD_BYTE (also satisfies AES block).
    std::size_t padded = (body.size() + 15) & ~std::size_t{15};
    body.resize(padded, PAD_BYTE);

    si1 enc = 0;
    if (encrypt) {
      enc = fmt::LEVEL_2_ENCRYPTION;  // records default to L2 (may hold PII)
      if (!keys.level2_key) throw PasswordError("level-2 key required to encrypt records");
      auto e = crypto::aes128_ecb_encrypt(body, *keys.level2_key);
      std::copy(e.begin(), e.end(), body.begin());
    }

    si8 disk_time = to_disk_time(r.time, rto);
    si8 file_offset = fmt::UNIVERSAL_HEADER_BYTES + static_cast<si8>(rdat_body.size());

    // Record header (24 B).
    std::vector<ui1> hdr(fmt::RECORD_HEADER_BYTES, 0);
    write_string(hdr, 4, 4, r.type);
    write<ui1>(hdr, 9, static_cast<ui1>(r.version_major));
    write<ui1>(hdr, 10, static_cast<ui1>(r.version_minor));
    write<si1>(hdr, 11, enc);
    write<ui4>(hdr, 12, static_cast<ui4>(body.size()));
    write<si8>(hdr, 16, disk_time);

    // Assemble the record, then CRC over [4, end).
    std::vector<ui1> rec;
    rec.insert(rec.end(), hdr.begin(), hdr.end());
    rec.insert(rec.end(), body.begin(), body.end());
    ui4 rec_crc = crc::calculate(std::span<const ui1>(rec).subspan(4));
    write<ui4>(rec, 0, rec_crc);

    max_entry = std::max(max_entry, static_cast<ui4>(rec.size()));
    rdat_body.insert(rdat_body.end(), rec.begin(), rec.end());

    // Record index entry (24 B): type[4], vmaj@5, vmin@6, enc@7, offset@8, time@16.
    std::vector<ui1> ie(fmt::RECORD_INDEX_BYTES, 0);
    write_string(ie, 0, 4, r.type);
    write<ui1>(ie, 5, static_cast<ui1>(r.version_major));
    write<ui1>(ie, 6, static_cast<ui1>(r.version_minor));
    write<si1>(ie, 7, enc);
    write<si8>(ie, 8, file_offset);
    write<si8>(ie, 16, disk_time);
    ridx_body.insert(ridx_body.end(), ie.begin(), ie.end());

    if (start_disk == fmt::UUTC_NO_ENTRY || disk_time > start_disk) start_disk = disk_time;
    if (end_disk == fmt::UUTC_NO_ENTRY || disk_time < end_disk) end_disk = disk_time;
  }

  const si8 n = static_cast<si8>(ordered.size());
  // .rdat
  {
    auto file = base_uh_bytes(fmt::FILE_TYPE_RECORD_DATA, session_name, channel_name,
                              segment_number, n, max_entry, start_disk, end_disk, vf);
    file.insert(file.end(), rdat_body.begin(), rdat_body.end());
    finalize_crcs(file);
    write_file((fs::path(dir) / (base + ".rdat")).string(), file);
  }
  // .ridx
  {
    auto file = base_uh_bytes(fmt::FILE_TYPE_RECORD_INDICES, session_name, channel_name,
                              segment_number, n, fmt::RECORD_INDEX_BYTES, start_disk, end_disk, vf);
    file.insert(file.end(), ridx_body.begin(), ridx_body.end());
    finalize_crcs(file);
    write_file((fs::path(dir) / (base + ".ridx")).string(), file);
  }
}

}  // namespace mef3io
