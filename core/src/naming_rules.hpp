// Internal: rules shared by the naming operations (#72): naming.cpp
// (name_drift, rename_archive) and rename.cpp (rename_session,
// fix_name_drift). Not installed.
#pragma once

#include <cctype>
#include <functional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "mef3io/source.hpp"

namespace mef3io::detail {

/// A session name that is a portable file name: no folder, no control
/// character (a NUL would cut the name short), none of the characters or
/// device names Windows cannot use, no trailing dot or space. Throws
/// std::invalid_argument naming the problem. rename_session (#72, second
/// part) uses the same rule.
inline void check_portable_name(const std::string& name) {
  if (name.empty()) throw std::invalid_argument("the new name is empty");
  if (name == "." || name == "..")
    throw std::invalid_argument("the new name cannot be '" + name + "'");
  for (const unsigned char c : name) {
    if (c < 0x20 || c == 0x7f)
      throw std::invalid_argument("the new name contains a control character");
    if (std::string("/\\:*?\"<>|").find(static_cast<char>(c)) != std::string::npos)
      throw std::invalid_argument(std::string("the new name contains '") + static_cast<char>(c) +
                                  "', which is not allowed in file names everywhere");
  }
  if (name.back() == '.' || name.back() == ' ')
    throw std::invalid_argument("the new name ends in a dot or a space");
  std::string upper;
  for (const char c : name.substr(0, name.find('.')))
    upper += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  static const std::set<std::string> reserved = {"CON", "PRN", "AUX", "NUL"};
  if (reserved.count(upper) ||
      ((upper.rfind("COM", 0) == 0 || upper.rfind("LPT", 0) == 0) && upper.size() == 4 &&
       upper[3] >= '1' && upper[3] <= '9'))
    throw std::invalid_argument("the new name is a reserved device name on Windows");
}


/// Every file that carries a universal header (and so a stored session
/// name): .tmet, .tidx, .tdat, .rdat and .ridx at every level, as
/// source-relative paths in listing order.
inline std::vector<std::string> header_files(const SessionSource& src) {
  std::vector<std::string> out;
  std::function<void(const std::string&)> walk = [&](const std::string& rel) {
    for (const auto& e : src.list_dir(rel)) {
      const std::string p = rel.empty() ? e.name : rel + "/" + e.name;
      if (e.is_dir) {
        walk(p);
        continue;
      }
      const auto ext = e.name.size() > 5 ? e.name.substr(e.name.size() - 5) : std::string();
      if (ext == ".tmet" || ext == ".tidx" || ext == ".tdat" || ext == ".rdat" || ext == ".ridx")
        out.push_back(p);
    }
  };
  walk("");
  return out;
}

}  // namespace mef3io::detail
