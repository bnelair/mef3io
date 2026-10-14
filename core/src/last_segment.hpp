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
#include <chrono>
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

/// A segment whose last file changed this recently, or that holds a writer's
/// temp file, is taken to be under an active writer. Stamping it then would
/// race that writer's own header rewrite.
inline constexpr auto ACTIVE_WRITE_WINDOW = std::chrono::minutes(2);

/// "<channel>-NNNNNN" -> NNNNNN (-1 when there is no number).
inline long long segment_number_of(const std::string& stem) {
  const auto dash = stem.rfind('-');
  if (dash == std::string::npos || dash + 1 >= stem.size()) return -1;
  try {
    return std::stoll(stem.substr(dash + 1));
  } catch (...) {
    return -1;
  }
}

/// The channel's last segment folder (highest segment number), or nullopt.
inline std::optional<fsys::path> last_segment_dir(const fsys::path& channel_dir) {
  std::optional<fsys::path> best;
  long long best_n = -1;
  std::error_code ec;
  for (const auto& e : fsys::directory_iterator(channel_dir, ec)) {
    if (!e.is_directory() || e.path().extension() != ".segd") continue;
    const long long n = segment_number_of(e.path().stem().string());
    if (!best || n > best_n) {
      best = e.path();
      best_n = n;
    }
  }
  return best;
}

/// Whether `channel_dir` looks like it is being written right now; `why` says
/// what gave it away.
inline bool channel_looks_active(const fsys::path& channel_dir, std::string& why) {
  const auto last = last_segment_dir(channel_dir);
  if (!last) return false;
  const auto now = fsys::file_time_type::clock::now();
  std::error_code ec;
  for (const auto& e : fsys::directory_iterator(*last, ec)) {
    const std::string name = e.path().filename().string();
    // The writer publishes .tmet/.tidx through "<file>.mef3io-tmp" + rename;
    // one left lying there means a write is in flight (or was interrupted).
    if (name.ends_with(".mef3io-tmp") || name.ends_with(".partial")) {
      why = e.path().string() + " is a writer's in-flight file";
      return true;
    }
    std::error_code tec;
    const auto t = fsys::last_write_time(e.path(), tec);
    if (!tec && now - t < ACTIVE_WRITE_WINDOW) {
      why = e.path().string() + " was modified less than " +
            std::to_string(std::chrono::duration_cast<std::chrono::seconds>(ACTIVE_WRITE_WINDOW)
                               .count()) +
            " s ago";
      return true;
    }
  }
  return false;
}

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
