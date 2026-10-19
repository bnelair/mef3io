// mef3io — time_range, name_drift, rename_archive (#72, first part). See
// naming.hpp.
#include "mef3io/naming.hpp"

#include <algorithm>
#include <filesystem>
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

}  // namespace

TimeRange time_range(const std::string& path, const std::string& password, const Control& ctl) {
  ctl.check();
  const auto src = open_session_source(path);
  const auto layout = detail::discover_layout(*src);
  TimeRange r;
  for (const auto& c : layout.channels) {
    if (c.segments.empty()) continue;
    ++r.channels;
    // The first segment's start and the last segment's end (segments are in
    // number order, which is time order for every writer we know of; the two
    // ends are also compared with each other below).
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
        if (seg == &c.segments.front() || &c.segments.front() == &c.segments.back())
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
  for (const auto& e : src->list_dir(""))
    if (!e.is_dir && e.name.size() > 5 && e.name.substr(e.name.size() - 5) == ".rdat")
      d.record_files.push_back(e.name.substr(0, e.name.size() - 5));
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
                 d.record_files.size() <= 1;
  return d;
}

ArchiveRename rename_archive(const std::string& path, const std::string& new_name) {
  if (!path_has_suffix(path, ".mefd.tar"))
    throw std::invalid_argument("rename_archive renames a .mefd.tar archive: " + path);
  if (new_name.empty() || new_name.find('/') != std::string::npos ||
      new_name.find('\\') != std::string::npos || new_name == "." || new_name == "..")
    throw std::invalid_argument("the new name must be a plain name, without a folder");
  // <name>.mefd.tar must fit a 255-byte file name.
  if (new_name.size() > 246)
    throw std::invalid_argument("the new name is " + std::to_string(new_name.size()) +
                                " bytes; at most 246 fit (the file name ends in .mefd.tar)");
  fs::path from(path);
  if (from.filename().empty()) from = from.parent_path();
  if (!fs::is_regular_file(from)) throw IoError("not an archive file: " + from.string());
  std::error_code ec;  // a name too long for a ".partial" beside it cannot have one
  if (fs::exists(fs::path(from.string() + ".partial"), ec))
    throw IoError("the archive is being written (a .partial file is beside it): " + from.string());
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
