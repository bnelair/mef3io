// mef3io — time_range, name_drift, rename_archive (#72, first part). See
// naming.hpp.
#include "mef3io/naming.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <set>
#include <stdexcept>

#include "naming_rules.hpp"
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

using detail::check_portable_name;
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
  for (const auto& p : detail::header_files(*src)) {
    ctl.check();
    if (src->file_size(p) < UH) continue;
    const auto uh = src->read_range(p, 0, UH);
    ++d.files_read;
    ++d.stored_names[byteio::read_string(uh, UH_SESSION_NAME, UH_NAME_BYTES)];
  }
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
