// mef3io — time_range, name_drift, rename_archive (#72, first part). See
// naming.hpp.
#include "mef3io/naming.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <set>
#include <functional>
#include <stdexcept>

#include "no_replace.hpp"
#include "session_layout.hpp"

#include "mef3io/byteio.hpp"
#include "mef3io/errors.hpp"
#include "mef3io/headers.hpp"
#include "mef3io/metadata.hpp"
#include "mef3io/source.hpp"
#include "mef3io/time.hpp"

namespace fs = std::filesystem;

namespace mef3io {

namespace {

constexpr std::size_t UH = fmt::UNIVERSAL_HEADER_BYTES;
constexpr std::size_t UH_START = 16, UH_END = 24, UH_SESSION_NAME = 308, UH_NAME_BYTES = 256;

/// A session name that is a portable file name: no folder, no control
/// character (a NUL would cut the name short), none of the characters or
/// device names Windows cannot use, no trailing dot or space. Throws
/// std::invalid_argument naming the problem. rename_session (#72, second
/// part) uses the same rule.
void check_portable_name(const std::string& name) {
  if (name.empty() || name == "." || name == "..")
    throw std::invalid_argument("the new name is empty");
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

}  // namespace

TimeRange time_range(const std::string& path, const std::string& password, const Control& ctl) {
  ctl.check();
  const auto src = open_session_source(path);
  const auto layout = detail::discover_layout(*src);
  TimeRange r;
  for (const auto& c : layout.channels) {
    if (c.segments.empty()) continue;
    ++r.channels;
    // The first and last segment by number (time order for every writer we
    // know of). Both files' start and end count, so two segments numbered
    // against time order are still handled; a middle segment earlier than
    // both is not seen (docs: time_range trades that for its speed).
    for (const auto* seg : {&c.segments.front(), &c.segments.back()}) {
      ctl.check();
      const auto bytes = src->read_range(seg->tmet, 0, fmt::METADATA_FILE_BYTES);
      ++r.files_read;
      const TimeSeriesMetadata md = load_time_series_metadata_partial(bytes, password);
      const si8 s = md.universal_header.start_time, e = md.universal_header.end_time;
      if (!md.section3_available) r.offset_known = false;
      const si8 rto = md.section3_available ? segment_rto(md) : 0;
      // A recording-time offset with times stored non-negated cannot be told
      // apart from absolute times (CLAUDE.md, "sign conventions").
      if (rto != 0 && (s >= 0 || e >= 0)) r.offset_known = false;
      for (const si8 v : {s, e}) {
        if (v == fmt::UUTC_NO_ENTRY) continue;
        const si8 t = to_user_time(v, rto);
        r.start_uutc = std::min(r.start_uutc.value_or(t), t);
        r.end_uutc = std::max(r.end_uutc.value_or(t), t);
      }
      if (&c.segments.front() == &c.segments.back()) break;
    }
  }
  return r;
}

NameDrift name_drift(const std::string& path, const Control& ctl) {
  ctl.check();
  const auto src = open_session_source(path);
  NameDrift d;
  d.folder = src->session_name();
  std::vector<std::string> index_files;
  for (const auto& e : src->list_dir("")) {
    if (e.is_dir || e.name.size() <= 5) continue;
    const std::string ext = e.name.substr(e.name.size() - 5), stem = e.name.substr(0, e.name.size() - 5);
    if (ext == ".rdat") d.record_files.push_back(stem);
    if (ext == ".ridx") index_files.push_back(stem);
  }
  // Every file's universal header: 1024 bytes each.
  std::function<void(const std::string&)> walk = [&](const std::string& rel) {
    for (const auto& e : src->list_dir(rel)) {
      const std::string p = rel.empty() ? e.name : rel + "/" + e.name;
      if (e.is_dir) {
        walk(p);
        continue;
      }
      const auto ext = e.name.size() > 5 ? e.name.substr(e.name.size() - 5) : std::string();
      if (ext != ".tmet" && ext != ".tidx" && ext != ".tdat" && ext != ".rdat" && ext != ".ridx")
        continue;
      ctl.check();
      if (src->file_size(p) < UH) continue;
      const auto uh = src->read_range(p, 0, UH);
      ++d.files_read;
      ++d.stored_names[byteio::read_string(uh, UH_SESSION_NAME, UH_NAME_BYTES)];
    }
  };
  walk("");
  d.consistent = d.stored_names.size() <= 1 &&
                 (d.stored_names.empty() || d.stored_names.begin()->first == d.folder) &&
                 std::all_of(d.record_files.begin(), d.record_files.end(),
                             [&](const std::string& r) { return r == d.folder; }) &&
                 d.record_files.size() <= 1 && index_files == d.record_files;
  return d;
}

ArchiveRename rename_archive(const std::string& path, const std::string& new_name) {
  if (!path_has_suffix(path, ".mefd.tar"))
    throw std::invalid_argument("rename_archive renames a .mefd.tar archive: " + path);
  check_portable_name(new_name);
  // <name>.mefd.tar must fit a 255-byte file name.
  if (new_name.size() > 246)
    throw std::invalid_argument("the new name is " + std::to_string(new_name.size()) +
                                " bytes; at most 246 fit (the file name ends in .mefd.tar)");
  fs::path from(path);
  if (from.filename().empty()) from = from.parent_path();
  if (!fs::is_regular_file(from)) throw IoError("not an archive file: " + from.string());
  // Being written: archive_session writes `<archive>.tmp` and renames it;
  // a copy writes `<dst>.partial`. (A name too long for either cannot have one.)
  for (const char* ext : {".tmp", ".partial"}) {
    std::error_code ec;
    if (fs::exists(fs::path(from.string() + ext), ec))
      throw IoError("the archive is being written (a " + std::string(ext) +
                    " file is beside it): " + from.string());
  }
  const fs::path to = from.parent_path() / (new_name + ".mefd.tar");
  std::string why;
  switch (detail::rename_no_replace(from, to, why)) {
    case detail::NoReplaceResult::Renamed: break;
    case detail::NoReplaceResult::TargetExists:
      throw IoError("the target already exists: " + to.string());
    case detail::NoReplaceResult::Unsupported:
      throw IoError("this file system cannot rename without the risk of replacing a file (" +
                    why + "); nothing was renamed");
    case detail::NoReplaceResult::Failed:
      throw IoError("the rename failed (" + why + "); nothing was renamed");
  }
  return {from.string(), to.string(), true};
}

}  // namespace mef3io
