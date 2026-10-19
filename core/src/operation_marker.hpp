// Internal: the "operation in progress" marker (#72).
//
// A multi-step in-place operation (rename_session, fix_name_drift) places
// `<session>/.mef3io-operation` before its first change and removes it after
// its last. While it is there, every mef3io WRITE to the session refuses
// (refuse_if_mid_operation), and the Reader reports the session as
// mid-operation. The file name is fixed and carries nothing identifying; the
// content is only the tool that started the operation, its kind and a random
// operation id (which names the journal in the caller's backup folder).
// Not installed.
#pragma once

#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <system_error>

#include "durability.hpp"
#include "no_replace.hpp"
#include "mef3io/errors.hpp"
#include "mef3io/naming.hpp"
#include "mef3io/source.hpp"

namespace mef3io::detail {

inline constexpr const char* OPERATION_MARKER = ".mef3io-operation";
inline constexpr const char* OPERATION_MARKER_MAGIC = "mef3io-operation 1";

/// 32 random hex characters: operation ids and temporary names.
inline std::string random_token() {
  static thread_local std::random_device rd;
  static const char* hex = "0123456789abcdef";
  std::string t;
  for (int i = 0; i < 32; ++i) t += hex[rd() & 0xF];
  return t;
}

/// The marker of a session directory, if there is one. A marker this
/// version cannot parse still counts (tool/operation "unknown"): refusing on
/// it is safe, ignoring it is not.
inline std::optional<OperationMarker> read_operation_marker(const std::filesystem::path& dir) {
  std::error_code ec;
  const auto file = dir / OPERATION_MARKER;
  if (!std::filesystem::exists(file, ec)) return std::nullopt;
  OperationMarker m{"unknown", "unknown", ""};
  std::ifstream in(file, std::ios::binary);
  std::string line;
  if (in && std::getline(in, line) && line == OPERATION_MARKER_MAGIC) {
    while (std::getline(in, line)) {
      const auto sp = line.find(' ');
      if (sp == std::string::npos) continue;
      const std::string key = line.substr(0, sp), value = line.substr(sp + 1);
      if (key == "tool") m.tool = value;
      if (key == "operation") m.operation = value;
      if (key == "id") m.id = value;
    }
  }
  return m;
}

/// Write the marker, refusing to replace one, and flush it AND its directory
/// entry: nothing may change before the marker is durable.
inline void write_operation_marker(const std::filesystem::path& dir, const OperationMarker& m) {
  // Written whole under a temporary name and renamed into place (never
  // replacing one), so a failure part-way never leaves a marker without its
  // id: such a marker would refuse every write with no journal to match.
  const auto file = dir / OPERATION_MARKER;
  const auto part = dir / (std::string(OPERATION_MARKER) + ".part");
  try {
    {
      std::ofstream out(part, std::ios::binary | std::ios::trunc);
      if (!out) throw IoError("cannot write the operation marker: " + part.string());
      out << OPERATION_MARKER_MAGIC << "\ntool " << m.tool << "\noperation " << m.operation
          << "\nid " << m.id << "\n";
      finish_stream(out, part.string());
    }
    fsync_file_or_throw(part.string());
    std::string why;
    switch (rename_no_replace(part, file, why)) {
      case NoReplaceResult::Renamed: break;
      case NoReplaceResult::TargetExists:
        throw WriteConflictError("the session already carries an operation marker: " +
                                 dir.string());
      default: throw IoError("cannot place the operation marker (" + why + ")");
    }
  } catch (...) {
    std::error_code ec;
    std::filesystem::remove(part, ec);
    throw;
  }
  fsync_directory(dir);
}

inline void remove_operation_marker(const std::filesystem::path& dir) {
  std::error_code ec;
  std::filesystem::remove(dir / OPERATION_MARKER, ec);
  if (ec) throw IoError("cannot remove the operation marker in " + dir.string() + ": " + ec.message());
  fsync_directory(dir);
}

inline std::string mid_operation_message(const OperationMarker& m) {
  return "the session is in the middle of an operation (" + m.operation + ", started by " +
         m.tool + (m.id.empty() ? "" : ", operation " + m.id) +
         "). Nothing will be written to it until that operation is finished or rolled back: "
         "run the same operation again with the same backup folder (the operation's journal "
         "is kept there) to roll it back. If the journal is lost and name_drift shows the "
         "name consistent, clear_operation_marker() removes the marker.";
}

/// Every write path calls this before changing anything. A tar archive never
/// carries a marker (archive_session refuses a marked session).
inline void refuse_if_mid_operation(const std::string& session_path) {
  if (path_has_suffix(session_path, ".tar")) return;
  if (const auto m = read_operation_marker(std::filesystem::path(session_path)))
    throw WriteConflictError(mid_operation_message(*m));
}

}  // namespace mef3io::detail
