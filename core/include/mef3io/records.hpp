// mef3io — MEF record (annotation) reading. Records live in <name>.rdat with a
// parallel <name>.ridx index, at session or channel level.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "mef3io/crypto.hpp"
#include "mef3io/types.hpp"

namespace mef3io {

// A decoded record. Common fields plus type-specific ones (unset -> nullopt).
struct Record {
  std::string type;   // e.g. "Note", "EDFA", "Seiz", "SyLg"
  si8 time = 0;       // absolute uUTC
  int version_major = 1;
  int version_minor = 0;

  std::optional<std::string> text;       // Note / SyLg / EDFA annotation
  std::optional<si8> duration;           // EDFA / Seiz
  // Seiz specifics (kept minimal; extend as needed).
  std::optional<si8> earliest_onset;
  std::optional<si8> latest_offset;
};

namespace fmt {
inline constexpr int RECORD_HEADER_BYTES = 24;
inline constexpr int RECORD_INDEX_BYTES = 24;
}  // namespace fmt

// Parse a .rdat image (universal header + concatenated records). `rto` is the
// recording-time-offset for resolving absolute record times; `keys` decrypts
// encrypted record bodies when present.
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
