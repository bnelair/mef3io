// Internal (not installed): the "segment-repaired" provenance rule shared by
// repair_session (validate.cpp) and recover_session (recover.cpp).
//
// When either modifies a segment that is NOT its channel's last, it also
// stamps the LAST segment's .tmet provenance region with
// Operation::SegmentRepaired. The last segment's region is the one place a
// reader that looks at one file per channel (the warm-start snapshot check,
// for one) can see that something earlier in the channel changed. HeaderRepair
// is not reused: provenance describes what happened to THAT file, and the last
// segment was not repaired itself.
//
// Header-only: just the 64-byte region and the header CRC move. The body CRC
// is unchanged because the body is.
#pragma once

#include <array>
#include <limits>
#include <utility>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "durability.hpp"
#include "mef3io/byteio.hpp"
#include "mef3io/crc.hpp"
#include "mef3io/errors.hpp"
#include "mef3io/headers.hpp"

namespace mef3io::detail {

namespace fsys = std::filesystem;

/// "<channel>-NNNNNN" -> NNNNNN; -1 unless everything after the last '-' is
/// a digit. The SAME rule as the Python side (mef3io/cache.py _segment_key),
/// ties broken by name, so both agree on which segment is a channel's last.
inline long long segment_number_of(const std::string& stem) {
  const auto dash = stem.rfind('-');
  if (dash == std::string::npos || dash + 1 >= stem.size()) return -1;
  long long n = 0;
  for (std::size_t i = dash + 1; i < stem.size(); ++i) {
    const char c = stem[i];
    if (c < '0' || c > '9') return -1;
    if (n > (std::numeric_limits<long long>::max() - 9) / 10) return -1;
    n = n * 10 + (c - '0');
  }
  return n;
}

/// The channel's last segment folder: highest segment number, then name.
inline std::optional<fsys::path> last_segment_dir(const fsys::path& channel_dir) {
  std::optional<fsys::path> best;
  std::pair<long long, std::string> best_key{-2, {}};
  std::error_code ec;
  for (const auto& e : fsys::directory_iterator(channel_dir, ec)) {
    if (!e.is_directory() || e.path().extension() != ".segd") continue;
    std::pair<long long, std::string> key{segment_number_of(e.path().stem().string()),
                                          e.path().filename().string()};
    if (!best || key > best_key) {
      best = e.path();
      best_key = std::move(key);
    }
  }
  return best;
}

/// A writer's temp file in the channel's last segment, if any. The writer
/// publishes .tmet/.tidx through "<file>.mef3io-tmp" + rename, so one lying
/// there means a write is in progress — or crashed and left it behind. Either
/// way only the STAMP is skipped (with a note); the repair itself proceeds.
/// Nothing is ever refused on "looks active" heuristics: after `cp -r` or
/// extract_session every mtime is fresh, and a crash leaves the temp file
/// forever, so a refusal would block exactly the legacy-repair workflow.
inline std::optional<fsys::path> writer_temp_file(const fsys::path& channel_dir) {
  const auto last = last_segment_dir(channel_dir);
  if (!last) return std::nullopt;
  std::error_code ec;
  for (const auto& e : fsys::directory_iterator(*last, ec))
    if (e.path().filename().string().ends_with(".mef3io-tmp")) return e.path();
  return std::nullopt;
}

/// Appended to every note about a skipped stamp: what it means for readers.
inline constexpr const char* STAMP_SKIPPED_CONSEQUENCE =
    " Cached opens (Reader(cache=...)) will not see this repair until the channel is next "
    "written, or until the session is opened without the cache.";

/// A prepared stamp of the channel's last .tmet. `ready` false means nothing
/// is to be written, and `note` (if set) says why — never an error.
struct LastSegmentStamp {
  fsys::path tmet;
  std::string segment_stem;
  std::vector<ui1> new_header;  // 1024 bytes, ready to write in place
  std::vector<ui1> old_header;  // for the backup
  bool ready = false;
  std::string note;
};

inline LastSegmentStamp prepare_last_segment_stamp(const fsys::path& channel_dir) {
  LastSegmentStamp s;
  const auto last = last_segment_dir(channel_dir);
  if (!last) {
    s.note = "no segment found to stamp";
    return s;
  }
  s.segment_stem = last->stem().string();
  s.tmet = *last / (s.segment_stem + ".tmet");
  if (const auto tmp = writer_temp_file(channel_dir)) {
    s.note = tmp->string() +
             " is a writer's temp file, so a write may be in progress, and the last segment "
             "was not stamped. If no writer is running it is left over from an interrupted "
             "write and can be removed.";
    return s;
  }
  std::vector<ui1> file(fmt::METADATA_FILE_BYTES);
  {
    std::ifstream in(s.tmet, std::ios::binary);
    if (!in || !in.read(reinterpret_cast<char*>(file.data()),
                        static_cast<std::streamsize>(file.size()))) {
      s.note = s.tmet.string() + " could not be read in full; its provenance was not stamped";
      return s;
    }
  }
  // The same CRC gate as the repair's own: never write into a file whose
  // bytes do not verify. NO_ENTRY (never computed) is not a mismatch.
  const ui4 stored_header = byteio::read<ui4>(file, 0);
  const ui4 stored_body = byteio::read<ui4>(file, 4);
  const ui4 real_header =
      crc::calculate(std::span<const ui1>(file).subspan(4, fmt::UNIVERSAL_HEADER_BYTES - 4));
  const ui4 real_body = crc::calculate(std::span<const ui1>(file).subspan(
      fmt::UNIVERSAL_HEADER_BYTES, fmt::METADATA_FILE_BYTES - fmt::UNIVERSAL_HEADER_BYTES));
  if ((stored_header != real_header && stored_header != fmt::CRC_NO_ENTRY) ||
      (stored_body != real_body && stored_body != fmt::CRC_NO_ENTRY)) {
    s.note = s.tmet.string() + " fails its CRC; its provenance was not stamped";
    return s;
  }
  std::vector<ui1> head(file.begin(), file.begin() + fmt::UNIVERSAL_HEADER_BYTES);
  auto uh = fmt::UniversalHeader::parse(head);
  if (!fmt::provenance::stamp_modified(uh, fmt::provenance::Operation::SegmentRepaired)) {
    s.note = s.tmet.string() +
             " carries another application's data in its provenance region; not stamped";
    return s;
  }
  s.old_header = head;
  // Copy only the region back into the stored bytes: re-serializing the whole
  // header would re-terminate text fields this code has no business touching.
  std::copy(uh.discretionary_region.begin(), uh.discretionary_region.end(),
            head.begin() + fmt::provenance::REGION_OFFSET);
  if (stored_header != fmt::CRC_NO_ENTRY)
    byteio::write<ui4>(head, 0,
                       crc::calculate(std::span<const ui1>(head).subspan(
                           4, fmt::UNIVERSAL_HEADER_BYTES - 4)));
  s.new_header = std::move(head);
  s.ready = true;
  return s;
}

/// Patch the prepared header in place and flush it. Same rules as the
/// repair's own in-place header patch (validate.cpp overwrite_universal_header).
inline void write_last_segment_stamp(const LastSegmentStamp& s) {
  std::fstream out(s.tmet, std::ios::binary | std::ios::in | std::ios::out);
  if (!out) throw IoError("cannot open for header update: " + s.tmet.string());
  out.write(reinterpret_cast<const char*>(s.new_header.data()),
            static_cast<std::streamsize>(s.new_header.size()));
  out.flush();
  if (!out) throw IoError("header update failed: " + s.tmet.string());
  out.close();
  if (!out) throw IoError("close failed, header may not have reached disk: " + s.tmet.string());
  (void)fsync_file(s.tmet.string());  // best effort, as for the repair's header patches
}

}  // namespace mef3io::detail
