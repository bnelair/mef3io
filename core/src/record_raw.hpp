// Internal (not installed): a record file read raw, record by record, with
// the byte offset of each. ONE implementation shared by the copy engine
// (session_copy.cpp: anonymize / transform / cut / merge carry records with it)
// and by remove_annotations (annotation_edit.cpp: carries the records it
// keeps). A refusal is raised through the caller's own `refuse` so each user
// keeps its own codes and exception types.
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <vector>

#include "mef3io/byteio.hpp"
#include "mef3io/crc.hpp"
#include "mef3io/crypto.hpp"
#include "mef3io/headers.hpp"
#include "mef3io/records.hpp"
#include "mef3io/transform.hpp"  // RefusalKind
#include "mef3io/types.hpp"

namespace mef3io::detail {

struct RawRecord {
  std::array<ui1, 4> type_bytes{};
  std::string type;
  ui1 vmaj = 0, vmin = 0;
  si1 enc = 0;
  si8 time = 0;
  std::vector<ui1> body;  // decrypted; stored length
};

struct RawRecordFile {
  std::vector<ui1> uh;                  // 1024 bytes
  std::vector<RawRecord> records;
  std::vector<si8> record_offsets;      // file-relative
  bool has_ridx = false;
  std::vector<ui1> ridx_uh;
  std::vector<std::array<ui1, fmt::RECORD_INDEX_BYTES>> ridx;
  crypto::AccessKeys keys;
};

inline std::span<const ui1> record_key_for(const crypto::AccessKeys& keys, int level) {
  if (level == fmt::LEVEL_1_ENCRYPTION && keys.level1_key) return *keys.level1_key;
  if (level == fmt::LEVEL_2_ENCRYPTION && keys.level2_key) return *keys.level2_key;
  return {};
}

inline std::string ridx_name_of(const std::string& rdat) {
  return rdat.substr(0, rdat.size() - 5) + ".ridx";
}

inline bool all_zero_bytes(std::span<const ui1> b) {
  // A plain loop on purpose (GCC 14.2 mis-optimises std::all_of here; see
  // for_agents/notes/copy-engine.md, "all_zero and GCC 14.2").
  for (const ui1 x : b)
    if (x != 0) return false;
  return true;
}

/// Parse a record file from its bytes (`ridx` is the index file's bytes, or
/// null when there is none). `where` / `ridx_where` name the files in
/// messages. `refuse(kind, code, message)` must not return.
template <class Refuse>
RawRecordFile parse_record_file(std::span<const ui1> bytes, const std::vector<ui1>* ridx,
                                const std::string& where, const std::string& ridx_where,
                                const std::string& password, Refuse&& refuse) {
  constexpr std::size_t UH = fmt::UNIVERSAL_HEADER_BYTES;
  constexpr std::size_t RHDR = fmt::RECORD_HEADER_BYTES;
  constexpr std::size_t RIDX = fmt::RECORD_INDEX_BYTES;
  constexpr std::size_t UH_L1_FIELD = 868, UH_L2_FIELD = 884;
  using byteio::read;
  using byteio::read_string;
  RawRecordFile f;
  if (bytes.size() < UH)
    refuse(RefusalKind::Format, "record-damaged", "record file smaller than its header: " + where);
  f.uh.assign(bytes.begin(), bytes.begin() + UH);
  f.keys = crypto::validate_password(password, bytes.subspan(UH_L1_FIELD, 16),
                                     bytes.subspan(UH_L2_FIELD, 16));
  const si8 declared = read<si8>(bytes, 32);
  const bool known = declared != -1;
  std::size_t off = UH;
  while (true) {
    if (known && static_cast<si8>(f.records.size()) == declared) break;
    if (off + RHDR > bytes.size()) {
      const bool clean = !known && all_zero_bytes(bytes.subspan(off));
      if (clean) break;
      refuse(RefusalKind::Format, "record-damaged",
             "record file truncated after " + std::to_string(f.records.size()) +
                 " record(s): " + where);
    }
    RawRecord r;
    std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(off + 4), 4, r.type_bytes.begin());
    r.type = read_string(bytes, off + 4, 4);
    if (r.type.empty() && !known) break;
    if (r.type.empty())
      refuse(RefusalKind::Format, "record-damaged", "record without a type code in " + where);
    const ui4 stored_crc = read<ui4>(bytes, off);
    r.vmaj = read<ui1>(bytes, off + 9);
    r.vmin = read<ui1>(bytes, off + 10);
    r.enc = read<si1>(bytes, off + 11);
    const ui4 nb = read<ui4>(bytes, off + 12);
    r.time = read<si8>(bytes, off + 16);
    const std::size_t body_off = off + RHDR;
    if (nb > bytes.size() - body_off)
      refuse(RefusalKind::Format, "record-damaged",
             "record " + std::to_string(f.records.size()) + " runs past the end of " + where);
    if (stored_crc != fmt::CRC_NO_ENTRY &&
        crc::calculate(bytes.subspan(off + 4, RHDR - 4 + nb)) != stored_crc)
      refuse(RefusalKind::Crc, "record-crc",
             "record " + std::to_string(f.records.size()) + " (" + r.type +
                 ") fails its CRC: " + where);
    r.body.assign(bytes.begin() + static_cast<std::ptrdiff_t>(body_off),
                  bytes.begin() + static_cast<std::ptrdiff_t>(body_off + nb));
    if (r.enc > 0) {
      const auto key = record_key_for(f.keys, r.enc);
      if (key.empty())
        refuse(RefusalKind::Password, "record-locked",
               "a record body in " + where +
                   " is encrypted and the password does not open it (the level-2 password "
                   "opens every record)");
      const std::size_t whole = r.body.size() / 16 * 16;  // meflib: bytes / 16 blocks
      auto dec = crypto::aes128_ecb_decrypt(std::span<const ui1>(r.body).first(whole), key);
      std::copy(dec.begin(), dec.end(), r.body.begin());
    }
    f.record_offsets.push_back(static_cast<si8>(off));
    f.records.push_back(std::move(r));
    off = body_off + nb;
  }
  if (ridx) {
    const auto& ib = *ridx;
    if (ib.size() < UH || (ib.size() - UH) % RIDX != 0)
      refuse(RefusalKind::Format, "record-index-mismatch",
             "record index has a partial entry: " + ridx_where);
    f.has_ridx = true;
    f.ridx_uh.assign(ib.begin(), ib.begin() + UH);
    for (std::size_t o = UH; o + RIDX <= ib.size(); o += RIDX) {
      std::array<ui1, RIDX> e{};
      std::copy_n(ib.begin() + static_cast<std::ptrdiff_t>(o), RIDX, e.begin());
      f.ridx.push_back(e);
    }
    if (f.ridx.size() != f.records.size())
      refuse(RefusalKind::Format, "record-index-mismatch",
             "record index " + ridx_where + " lists " + std::to_string(f.ridx.size()) +
                 " record(s) but " + where + " holds " + std::to_string(f.records.size()) +
                 "; which one is right is unknown, so neither is copied");
  }
  return f;
}

}  // namespace mef3io::detail
