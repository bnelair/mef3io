// mef3io — crash recovery. See recover.hpp for what this is and is not.
#include "operation_marker.hpp"
#include "mef3io/recover.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>

#include "durability.hpp"
#include "last_segment.hpp"
#include "mef3io/byteio.hpp"
#include "mef3io/crc.hpp"
#include "mef3io/errors.hpp"
#include "mef3io/headers.hpp"
#include "mef3io/metadata.hpp"
#include "mef3io/source.hpp"
#include "mef3io/time.hpp"

namespace fsys = std::filesystem;

namespace mef3io {
namespace {

/// Read one byte range of a file. `.tdat` runs to tens of gigabytes, so it is
/// NEVER read whole: recovery needs its SIZE, plus a 304-byte RED header (and,
/// to verify it, the one block that header describes) at a handful of offsets.
std::vector<ui1> read_range(std::ifstream& f, std::uint64_t offset, std::size_t count) {
  std::vector<ui1> buf(count);
  f.clear();
  f.seekg(static_cast<std::streamoff>(offset));
  if (!f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(count)))
    buf.resize(static_cast<std::size_t>(f.gcount()));
  return buf;
}

std::vector<ui1> read_all(const fsys::path& p) {
  std::ifstream f(p, std::ios::binary | std::ios::ate);
  if (!f) throw IoError("cannot open file: " + p.string());
  const auto size = f.tellg();
  f.seekg(0);
  std::vector<ui1> buf(static_cast<std::size_t>(size));
  if (size && !f.read(reinterpret_cast<char*>(buf.data()), size))
    throw IoError("short read: " + p.string());
  return buf;
}

void write_all(const fsys::path& target, std::span<const ui1> bytes) {
  const fsys::path tmp = target.parent_path() / (target.filename().string() + ".mef3io-rec-tmp");
  std::error_code ignored;
  fsys::remove(tmp, ignored);
  try {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) throw IoError("cannot open for write: " + tmp.string());
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    f.flush();
    if (!f) throw IoError("write failed: " + tmp.string());
    f.close();
    detail::copy_file_identity(target, tmp);
    // Recovery runs once, on a session that is already damaged. Pay for the
    // barrier: a crash DURING recovery must not compound the problem.
    detail::fsync_file_or_throw(tmp.string());
    detail::replace_file(tmp, target);
  } catch (...) {
    fsys::remove(tmp, ignored);
    throw;
  }
  detail::fsync_directory(target.parent_path());
}

void back_up_file(const fsys::path& file, const fsys::path& root, const std::string& rel) {
  const fsys::path dest = root / rel;
  std::error_code ec;
  fsys::create_directories(dest.parent_path(), ec);
  if (fsys::exists(dest)) return;  // never overwrite a pristine backup
  fsys::copy_file(file, dest, fsys::copy_options::overwrite_existing, ec);
  if (ec) throw IoError("cannot back up " + file.string() + ": " + ec.message());
  (void)detail::fsync_file(dest.string());
}

void back_up_bytes(const fsys::path& root, const std::string& rel, std::span<const ui1> bytes) {
  const fsys::path dest = root / rel;
  std::error_code ec;
  fsys::create_directories(dest.parent_path(), ec);
  if (fsys::exists(dest)) return;  // never overwrite a pristine backup
  std::ofstream f(dest, std::ios::binary | std::ios::trunc);
  if (!f) throw IoError("cannot back up to " + dest.string());
  f.write(reinterpret_cast<const char*>(bytes.data()),
          static_cast<std::streamsize>(bytes.size()));
  f.flush();
  if (!f) throw IoError("backup write failed: " + dest.string());
  f.close();
  (void)detail::fsync_file(dest.string());
}

/// Is there a plausible, CRC-valid RED block at `offset`?
///
/// Reads the 304-byte header, then exactly the one block it describes — never
/// the whole file. A block whose CRC does not verify is a torn write, not a
/// block, and must not be indexed as if it held samples.
bool block_looks_real(std::ifstream& f, std::uint64_t size, std::uint64_t offset,
                      fmt::RedBlockHeader& out) {
  if (offset + fmt::RED_BLOCK_HEADER_BYTES > size) return false;
  const auto head = read_range(f, offset, fmt::RED_BLOCK_HEADER_BYTES);
  if (head.size() < fmt::RED_BLOCK_HEADER_BYTES) return false;
  out = fmt::RedBlockHeader::parse(head);
  if (out.block_bytes < fmt::RED_BLOCK_HEADER_BYTES) return false;
  if (out.block_bytes == fmt::UI4_NO_ENTRY) return false;
  if (offset + out.block_bytes > size) return false;   // truncated tail
  if (out.number_of_samples == 0 || out.number_of_samples == fmt::UI4_NO_ENTRY) return false;
  const auto block = read_range(f, offset + 4, static_cast<std::size_t>(out.block_bytes) - 4);
  if (block.size() + 4 != out.block_bytes) return false;
  return out.crc == crc::calculate(block);
}

/// What recovery does about a segment's END TIME (#83). Index 0/1/2 =
/// .tmet/.tidx/.tdat universal header; a set value is the new stored end.
struct EndPlan {
  bool rto_nonzero = false;  ///< the offset is known and non-zero
  std::array<std::optional<si8>, 3> new_end;
  int stale() const {
    return static_cast<int>(std::count_if(new_end.begin(), new_end.end(),
                                          [](const auto& v) { return v.has_value(); }));
  }
};

/// Decide the end times at a NON-ZERO recording-time offset. At offset 0 the
/// validator's times.segment-bounds repair rewrites the bounds after recovery
/// (the CLI runs it), so recovery keeps to the index and the data there. At a
/// non-zero offset every time check stands down (writers disagree on the
/// sign convention), which left `info()`'s end time — the .tmet universal
/// header's — describing the data from BEFORE the recovery.
///
/// Rules (review of #145):
///  * compared in USER time (to_user_time with the segment's offset);
///  * a header's end is stale only if it lies before the last kept block's
///    START plus half a sample — no end convention (exclusive, inclusive,
///    truncated, rounded) puts it there — or, when recovery REBUILT blocks,
///    before the computed end minus half a sample;
///  * an end is never moved earlier (the new value is always later);
///  * each header keeps its OWN convention: only an end stored in the
///    offset-applied (negated) form is rewritten (an unset one if its start
///    is negated); a plain one is left alone with a note, since at a
///    non-zero offset a positive value is ambiguous;
///  * the last block's stored time must be negated too, else nothing.
/// `note` is set when something stale is left alone.
EndPlan plan_end_times(const TimeSeriesMetadata& md, const std::array<const ui1*, 3>& uh,
                       si8 last_start_stored, ui4 last_samples, bool rebuilt,
                       std::string& note) {
  EndPlan out;
  const si8 rto = segment_rto(md);
  if (rto == 0) return out;  // repair_session's job (see above)
  out.rto_nonzero = true;
  const sf8 fs_hz = md.section2.sampling_frequency;
  const sf8 duration = static_cast<sf8>(last_samples) * 1e6 / fs_hz;
  if (!std::isfinite(fs_hz) || !(fs_hz > 0.0) || !std::isfinite(duration) || duration > 9e18) {
    note = "its sampling frequency is not a usable number";
    return out;
  }
  if (last_start_stored >= 0 || last_start_stored == fmt::UUTC_NO_ENTRY) {
    note = "the last block's time is not stored in the offset-applied (negated) form, so "
           "the end cannot be placed without guessing the convention";
    return out;
  }
  const si8 half = static_cast<si8>(std::llround(std::min(0.5e6 / fs_hz, 1e18)));
  const si8 start_user = to_user_time(last_start_stored, rto);
  const si8 computed = start_user + static_cast<si8>(std::llround(duration));
  const si8 limit = rebuilt ? computed - half : start_user + half;
  static const char* const names[] = {".tmet", ".tidx", ".tdat"};
  for (int i = 0; i < 3; ++i) {
    const si8 end = byteio::read<si8>(std::span<const ui1>(uh[i], fmt::UNIVERSAL_HEADER_BYTES), 24);
    const si8 start =
        byteio::read<si8>(std::span<const ui1>(uh[i], fmt::UNIVERSAL_HEADER_BYTES), 16);
    if (end != fmt::UUTC_NO_ENTRY && to_user_time(end, rto) >= limit) continue;  // fine
    const bool negated = end != fmt::UUTC_NO_ENTRY ? end < 0
                                                   : (start < 0 && start != fmt::UUTC_NO_ENTRY);
    if (!negated || !time_is_storable(computed, rto)) {
      if (!note.empty()) note += "; ";
      note += std::string("the ") + names[i] +
              " end time is not stored in the offset-applied (negated) form";
      continue;
    }
    out.new_end[i] = to_stored_time(computed, rto);
  }
  return out;
}

}  // namespace

namespace {

/// One segment's recovery, decided during the scan and carried out after it.
///
/// Recovery runs in two phases so that a cancellation can never leave a
/// session half-recovered: the SCAN (all the reading, and nearly all the time)
/// is cancellable and writes nothing; the WRITE phase is not. Only what the
/// write phase needs is kept — not the index entries that survive unchanged,
/// which for a month-long segment are megabytes per channel; those are re-read
/// after checking that the files are still the ones that were scanned.
struct PlannedRecovery {
  fsys::path channel_dir, tidx, tdat;
  std::string channel, base, description;
  int segment_number = 0;
  std::size_t report_index = 0;               ///< into RecoveryReport::segments
  std::uint64_t index_size = 0;               ///< .tidx size when scanned
  std::vector<ui1> index_header;              ///< .tidx universal header when scanned
  std::uint64_t data_size = 0;                ///< .tdat size when scanned
  std::size_t kept_from_index = 0;            ///< leading .tidx entries that stay
  std::vector<fmt::TimeSeriesIndex> rebuilt;  ///< entries recovered from .tdat
  std::uint64_t data_end = 0;                 ///< end of the last whole block
  si8 tail = 0;                               ///< trailing bytes to drop
  /// The end-time decision (plan_end_times) and the .tmet header it was
  /// decided from (re-checked before writing).
  EndPlan end;
  bool tdat_header_current = false;  ///< .tdat entry count already right
  fsys::path tmet;
  std::vector<ui1> tmet_header;
};

struct SegmentDir {
  fsys::path channel_dir, dir;
  std::string channel;
  bool is_last = false;  ///< the channel's last segment (last_segment_dir's rule)
};

}  // namespace

RecoveryReport recover_session(const std::string& path, bool apply, bool backup,
                               const std::string& password, const Control& ctl) {
  // The password is used only to read a segment's offset and rate when its
  // end time has to be decided (plan_end_times).
  if (path_has_suffix(path, ".tar"))
    throw IoError("cannot recover a tar session archive in place: " + path +
                  " (extract it first with extract_session)");
  if (!path_has_suffix(path, ".mefd"))
    throw IoError("session directory must end with .mefd: " + path);
  if (!fsys::is_directory(path)) throw IoError("no such session directory: " + path);
  if (apply) detail::refuse_if_mid_operation(path);

  const fsys::path root(path);
  RecoveryReport report;
  std::set<fsys::path> channels_to_stamp;
  const fsys::path backup_root =
      root.parent_path() / (root.filename().string() + ".recover-backup");

  std::vector<SegmentDir> all_segments;
  for (const auto& ch_entry : fsys::directory_iterator(root)) {
    if (!ch_entry.is_directory() || ch_entry.path().extension() != ".timd") continue;
    std::vector<fsys::path> segs;
    for (const auto& s : fsys::directory_iterator(ch_entry.path()))
      if (s.is_directory() && s.path().extension() == ".segd") segs.push_back(s.path());
    std::sort(segs.begin(), segs.end());
    // The last segment by the SAME rule as detail::last_segment_dir (number,
    // then folder name), from the listing already made.
    std::size_t last = 0;
    for (std::size_t i = 1; i < segs.size(); ++i) {
      const auto key = [&](std::size_t k) {
        return std::make_pair(detail::segment_number_of(segs[k].stem().string()),
                              segs[k].filename().string());
      };
      if (key(i) > key(last)) last = i;
    }
    for (std::size_t i = 0; i < segs.size(); ++i)
      all_segments.push_back(
          {ch_entry.path(), segs[i], ch_entry.path().stem().string(), i == last});
  }

  // Progress counts each segment's .tidx + .tdat bytes: the scan reads the
  // whole index and walks whatever data lies past it. Sized only for a
  // listener — it costs two stats per segment.
  std::vector<std::uint64_t> seg_bytes;
  std::uint64_t total = 0;
  if (ctl.progress) {
    seg_bytes.reserve(all_segments.size());
    for (const auto& sd : all_segments) {
      const std::string base = sd.dir.stem().string();
      std::uint64_t b = 0;
      std::error_code ec;
      const auto a = fsys::file_size(sd.dir / (base + ".tidx"), ec);
      if (!ec) b += static_cast<std::uint64_t>(a);
      const auto d = fsys::file_size(sd.dir / (base + ".tdat"), ec);
      if (!ec) b += static_cast<std::uint64_t>(d);
      seg_bytes.push_back(b);
      total += b;
    }
  }
  detail::ProgressReporter progress(ctl, total);
  progress.start();

  std::vector<PlannedRecovery> plan;
  std::uint64_t seg_base = 0;  // progress at the start of the current segment

  // ---- phase 1: scan (reads only; cancellable) ----------------------------
  for (std::size_t si = 0; si < all_segments.size(); ++si) {
    ctl.check();
    if (si > 0 && progress.active()) seg_base += seg_bytes[si - 1];
    progress.advance_to(seg_base);

    const SegmentDir& sd = all_segments[si];
    const fsys::path& seg = sd.dir;
    const std::string& channel = sd.channel;
    const std::string base = seg.stem().string();
    const fsys::path tidx = seg / (base + ".tidx");
    const fsys::path tdat = seg / (base + ".tdat");
    int segment_number = 0;
    try {
      segment_number = std::stoi(base.substr(base.rfind('-') + 1));
    } catch (...) { /* leave 0 */ }
    const std::string description = channel + "/" + base;
    ++report.segments_examined;

    if (!fsys::exists(tidx) || !fsys::exists(tdat)) {
      report.skipped.push_back(description + ": segment is incomplete (missing .tidx or .tdat)");
      continue;
    }

    // The .tidx is read whole (56 bytes per block — megabytes at most). The
    // .tdat is NOT: it is the file that runs to tens of gigabytes, and all
    // recovery needs from it is its size plus a few block headers.
    std::vector<ui1> index_bytes;
    std::uint64_t data_size = 0;
    std::ifstream data_in;
    try {
      index_bytes = read_all(tidx);
      std::error_code ec;
      data_size = static_cast<std::uint64_t>(fsys::file_size(tdat, ec));
      if (ec) throw IoError("cannot stat " + tdat.string() + ": " + ec.message());
      data_in.open(tdat, std::ios::binary);
      if (!data_in) throw IoError("cannot open file: " + tdat.string());
    } catch (const std::exception& e) {
      report.skipped.push_back(description + ": " + e.what());
      continue;
    }
    if (index_bytes.size() < fmt::UNIVERSAL_HEADER_BYTES ||
        data_size < fmt::UNIVERSAL_HEADER_BYTES) {
      report.skipped.push_back(description + ": file shorter than a universal header");
      continue;
    }
    progress.advance_to(seg_base + index_bytes.size());

    // Recovery DECIDES what to keep from these bytes, and then truncates —
    // unlike declaration repair, which never touches the index or the data.
    // So the index has to verify first. A CRC mismatch means the block table
    // itself is damaged, and "interrupted append" is then a guess: the same
    // pattern is produced by a corrupted offset, and acting on it would drop
    // real blocks or truncate real samples.
    {
      const ui4 stored_header = byteio::read<ui4>(index_bytes, 0);
      const ui4 stored_body = byteio::read<ui4>(index_bytes, 4);
      const ui4 real_header = crc::calculate(std::span<const ui1>(index_bytes)
                                                 .subspan(4, fmt::UNIVERSAL_HEADER_BYTES - 4));
      const std::size_t body_len = index_bytes.size() - fmt::UNIVERSAL_HEADER_BYTES;
      // Bound by whole entries: a torn trailing fragment is exactly what an
      // interrupted write leaves, and it must not make the whole index look
      // corrupt. The fragment is dropped further down.
      const std::size_t whole = body_len - (body_len % fmt::TIME_SERIES_INDEX_BYTES);
      const ui4 real_body = crc::calculate(
          std::span<const ui1>(index_bytes).subspan(fmt::UNIVERSAL_HEADER_BYTES, whole));
      const ui4 real_body_to_eof = crc::calculate(
          std::span<const ui1>(index_bytes).subspan(fmt::UNIVERSAL_HEADER_BYTES));
      const bool header_ok =
          stored_header == real_header || stored_header == fmt::CRC_NO_ENTRY;
      const bool body_ok = stored_body == real_body || stored_body == real_body_to_eof ||
                           stored_body == fmt::CRC_NO_ENTRY;
      if (!header_ok || !body_ok) {
        report.skipped.push_back(
            description + ": the block index does not pass its own CRC, so it cannot be used "
                          "to decide what the data file should contain; nothing was changed");
        continue;
      }
    }

    const std::size_t entry_bytes = index_bytes.size() - fmt::UNIVERSAL_HEADER_BYTES;
    const std::size_t n_entries = entry_bytes / fmt::TIME_SERIES_INDEX_BYTES;
    std::span<const ui1> entries =
        std::span<const ui1>(index_bytes).subspan(fmt::UNIVERSAL_HEADER_BYTES);

    // --- how much of the index does the data actually support? ---
    // Only the last surviving entry is needed to carry on from; the survivors
    // themselves are re-read from the .tidx if the segment is written.
    std::size_t kept = 0;
    fmt::TimeSeriesIndex last_kept;
    si8 previous_end = fmt::UNIVERSAL_HEADER_BYTES;
    bool index_ahead = false;
    for (std::size_t i = 0; i < n_entries; ++i) {
      auto e = fmt::TimeSeriesIndex::parse(
          entries.subspan(i * fmt::TIME_SERIES_INDEX_BYTES, fmt::TIME_SERIES_INDEX_BYTES));
      const bool sane = e.file_offset >= fmt::UNIVERSAL_HEADER_BYTES &&
                        e.file_offset >= previous_end - 1 &&
                        e.number_of_samples != fmt::UI4_NO_ENTRY &&
                        e.block_bytes != fmt::UI4_NO_ENTRY &&
                        static_cast<std::uint64_t>(e.file_offset) + e.block_bytes <= data_size;
      if (!sane) {                       // this entry and everything after it
        index_ahead = true;              // describes bytes that are not there
        break;
      }
      previous_end = e.file_offset + e.block_bytes;
      last_kept = e;
      ++kept;
    }
    const si8 dropped = static_cast<si8>(n_entries) - static_cast<si8>(kept);

    // --- does the data run past the index? recover those blocks ---
    std::vector<fmt::TimeSeriesIndex> rebuilt;
    si8 recovered = 0;
    std::uint64_t offset = kept == 0 ? fmt::UNIVERSAL_HEADER_BYTES
                                     : static_cast<std::uint64_t>(last_kept.file_offset +
                                                                  last_kept.block_bytes);
    si8 next_sample = kept == 0 ? 0 : last_kept.start_sample + last_kept.number_of_samples;
    fmt::RedBlockHeader header;
    while (offset < data_size) {
      // Each candidate block is read whole to verify its CRC, so this is the
      // loop that runs long on a big unindexed tail: stop between blocks.
      ctl.check();
      if (!block_looks_real(data_in, data_size, offset, header)) break;
      fmt::TimeSeriesIndex e;
      e.file_offset = static_cast<si8>(offset);
      e.start_time = header.start_time;
      e.start_sample = next_sample;
      e.number_of_samples = header.number_of_samples;
      e.block_bytes = header.block_bytes;
      // The extrema are per-block statistics a reader recomputes anyway; the
      // index copy is informational. Leaving them at the sentinel would trip
      // index.entry-counts, so use the block's own conservative bounds.
      e.maximum_sample_value = 0;
      e.minimum_sample_value = 0;
      e.red_block_flags = header.flags;
      rebuilt.push_back(e);
      next_sample += header.number_of_samples;
      offset += header.block_bytes;
      ++recovered;
      progress.advance_to(seg_base + index_bytes.size() + offset);
    }
    const si8 tail = static_cast<si8>(data_size) - static_cast<si8>(offset);
    const std::size_t n_after = kept + rebuilt.size();

    // The blocks can line up perfectly and the segment still be mid-update: a
    // crash after the .tidx header was published but before the .tdat header
    // was patched leaves the .tdat declaring the OLD count. meflib clamps the
    // block count down to that field, so the segment reads short — and
    // looking only at block alignment would report "nothing to do" and skip
    // the repair that fixes it.
    si8 stale_headers = 0;
    const si8 index_count = byteio::read<si8>(index_bytes, 32);
    std::vector<ui1> data_head;
    try {
      data_head = read_range(data_in, 0, fmt::UNIVERSAL_HEADER_BYTES);
    } catch (const std::exception&) { /* handled by the size check above */ }
    const si8 data_count = data_head.size() >= fmt::UNIVERSAL_HEADER_BYTES
                               ? byteio::read<si8>(data_head, 32)
                               : 0;
    if (index_count != static_cast<si8>(n_after)) ++stale_headers;
    if (data_count != static_cast<si8>(n_after)) ++stale_headers;

    // The end time (plan_end_times), decided in the scan so a dry run reports
    // it. The .tmet is read only where it can matter, so a healthy session
    // costs no metadata reads beyond one 1 KB header per CHANNEL:
    //  * a segment recovery writes anyway — its offset decides whether the
    //    end may be touched at all;
    //  * a channel's LAST segment, the only one an append extends, whose
    //    .tmet update a crash can lose: its header (1 KB) is read, and the
    //    whole record only if the three files' end times disagree.
    EndPlan end_plan;
    std::vector<ui1> tmet_header;
    std::string end_note;
    const fsys::path tmet = seg / (base + ".tmet");
    const bool needs_work = dropped != 0 || recovered != 0 || tail != 0 || stale_headers != 0;
    const bool is_last = sd.is_last;
    if (n_after > 0 && (needs_work || is_last) && data_head.size() >= fmt::UNIVERSAL_HEADER_BYTES) {
      try {
        std::ifstream tmet_in(tmet, std::ios::binary);
        if (!tmet_in) throw IoError("cannot open " + tmet.string());
        tmet_header = read_range(tmet_in, 0, fmt::UNIVERSAL_HEADER_BYTES);
        if (tmet_header.size() < fmt::UNIVERSAL_HEADER_BYTES)
          throw FormatError("shorter than a universal header");
        const si8 tmet_end = byteio::read<si8>(tmet_header, 24);
        const bool ends_disagree = tmet_end != byteio::read<si8>(index_bytes, 24) ||
                                   tmet_end != byteio::read<si8>(data_head, 24);
        if (needs_work || ends_disagree) {
          const std::vector<ui1> record = read_range(tmet_in, 0, fmt::METADATA_FILE_BYTES);
          const TimeSeriesMetadata md = load_time_series_metadata(record, password);
          const si8 last_start = rebuilt.empty() ? last_kept.start_time : rebuilt.back().start_time;
          const ui4 last_samples =
              rebuilt.empty() ? last_kept.number_of_samples : rebuilt.back().number_of_samples;
          if (!md.section3_available) {
            end_note = "if this session has a recording-time offset, its end time could not be "
                       "checked: section 3 is locked (pass the level-2 password); without one, "
                       "repair_session updates it";
          } else {
            end_plan = plan_end_times(md, {tmet_header.data(), index_bytes.data(), data_head.data()},
                                      last_start, last_samples, recovered != 0, end_note);
          }
        }
      } catch (const std::exception& e) {
        if (needs_work)
          end_note = std::string("the segment metadata could not be read (") + e.what() + ")";
      }
    }
    const si8 stale_end = end_plan.stale();

    if (!end_note.empty())
      report.notes.push_back(description + ": end time left as is: " + end_note);
    if (dropped == 0 && recovered == 0 && tail == 0 && stale_headers == 0 && stale_end == 0)
      continue;  // segment is fine

    RecoveredSegment out;
    out.channel = channel;
    out.segment_number = segment_number;
    out.path = description;
    out.blocks_before = static_cast<si8>(n_entries);
    out.blocks_after = static_cast<si8>(n_after);
    out.blocks_recovered = recovered;
    out.blocks_dropped = dropped;
    out.tdat_bytes_dropped = tail;
    std::string what;
    if (recovered)
      what += "recovered " + std::to_string(recovered) +
              " block(s) that reached .tdat but were never indexed";
    if (dropped) {
      if (!what.empty()) what += "; ";
      what += "dropped " + std::to_string(dropped) +
              " index entr(ies) pointing past the end of .tdat";
    }
    if (tail) {
      if (!what.empty()) what += "; ";
      what += "dropped " + std::to_string(tail) +
              " trailing byte(s) that do not form a whole block";
    }
    if (index_ahead && !dropped) what += " (index stops short of the data)";
    if (stale_headers) {
      if (!what.empty()) what += "; ";
      what += "the universal header entry count was stale in " +
              std::to_string(stale_headers) + " file(s) (index " + std::to_string(index_count) +
              ", data " + std::to_string(data_count) + ", really " + std::to_string(n_after) +
              ") — a reader clamps the block count down to it and would read the segment short";
    }
    if (stale_end) {
      if (!what.empty()) what += "; ";
      what += "the end time was stale in " + std::to_string(stale_end) +
              " universal header(s) and is set from the last block (the segment has a "
              "recording-time offset, where repair_session leaves times alone)";
    }
    out.action = what;
    report.segments.push_back(std::move(out));

    if (!apply) continue;
    PlannedRecovery p;
    p.channel_dir = sd.channel_dir;
    p.tidx = tidx;
    p.tdat = tdat;
    p.channel = channel;
    p.base = base;
    p.description = description;
    p.segment_number = segment_number;
    p.report_index = report.segments.size() - 1;
    p.index_size = index_bytes.size();
    p.index_header.assign(index_bytes.begin(), index_bytes.begin() + fmt::UNIVERSAL_HEADER_BYTES);
    p.data_size = data_size;
    p.kept_from_index = kept;
    p.rebuilt = std::move(rebuilt);
    p.data_end = offset;
    p.tail = tail;
    p.end = end_plan;
    p.tdat_header_current = data_count == static_cast<si8>(n_after);
    p.tmet = tmet;
    p.tmet_header = std::move(tmet_header);
    plan.push_back(std::move(p));
  }
  // The last point at which a cancel is honoured. Nothing has been written.
  ctl.check();
  // finish() belongs HERE, before anything is written: progress measures the
  // pass, and a callback that raises at (total, total) must stop the call
  // while there is still nothing to lose. Called after the write phase, a
  // raise there discarded the report of what had just been written.
  progress.finish();

  // ---- phase 2: write. Not cancellable: each write is a whole file replaced
  // by rename or a 1024-byte header patch, and stopping between segments would
  // leave the session half-recovered. ---------------------------------------
  std::vector<bool> withdrawn(report.segments.size(), false);
  for (const auto& p : plan) {
    const fsys::path& tidx = p.tidx;
    const fsys::path& tdat = p.tdat;
    const std::string& channel = p.channel;
    const std::string& base = p.base;

    // The scan and the write are further apart than they once were, so check
    // that the files are still the ones that were scanned. A writer appending
    // in between moves the sizes; acting on the stale plan would then drop
    // what it had just written.
    std::vector<ui1> index_bytes;
    std::ifstream data_in;
    bool unchanged = false;
    {
      std::error_code ec;
      const auto now_data = fsys::file_size(tdat, ec);
      if (!ec && static_cast<std::uint64_t>(now_data) == p.data_size) {
        index_bytes = read_all(tidx);
        unchanged = index_bytes.size() == p.index_size &&
                    std::equal(p.index_header.begin(), p.index_header.end(),
                               index_bytes.begin());
      }
    }
    if (!unchanged) {
      report.skipped.push_back(p.description +
                               ": changed while recovery was scanning (is a writer running?); "
                               "nothing was written to this segment — run recovery again");
      withdrawn[p.report_index] = true;
      continue;
    }
    data_in.open(tdat, std::ios::binary);
    if (!data_in) throw IoError("cannot open file: " + tdat.string());

    std::vector<fmt::TimeSeriesIndex> keep;
    keep.reserve(p.kept_from_index + p.rebuilt.size());
    {
      std::span<const ui1> entries =
          std::span<const ui1>(index_bytes).subspan(fmt::UNIVERSAL_HEADER_BYTES);
      for (std::size_t i = 0; i < p.kept_from_index; ++i)
        keep.push_back(fmt::TimeSeriesIndex::parse(
            entries.subspan(i * fmt::TIME_SERIES_INDEX_BYTES, fmt::TIME_SERIES_INDEX_BYTES)));
      keep.insert(keep.end(), p.rebuilt.begin(), p.rebuilt.end());
    }
    const si8 tail = p.tail;
    const std::uint64_t offset = p.data_end;

    if (backup) {
      // Back up what CHANGES, not the whole file. The .tidx is small. The
      // .tdat may be tens of gigabytes and recovery only ever rewrites its
      // 1024-byte header and drops a trailing fragment shorter than one
      // block — so those are what get saved. Copying 30 GB to undo 200 bytes
      // would make the tool unusable on exactly the files it is for.
      report.backup_root = backup_root.string();
      back_up_file(tidx, backup_root, channel + "/" + base + ".tidx");
      const auto head = read_range(data_in, 0, fmt::UNIVERSAL_HEADER_BYTES);
      back_up_bytes(backup_root, channel + "/" + base + ".tdat.header", head);
      if (tail > 0) {
        const auto frag = read_range(data_in, offset, static_cast<std::size_t>(tail));
        back_up_bytes(backup_root, channel + "/" + base + ".tdat.tail", frag);
      }
    }

    std::vector<ui1> new_index(fmt::UNIVERSAL_HEADER_BYTES);
    std::copy(index_bytes.begin(), index_bytes.begin() + fmt::UNIVERSAL_HEADER_BYTES,
              new_index.begin());
    std::vector<ui1> entry(fmt::TIME_SERIES_INDEX_BYTES);
    for (const auto& e : keep) {
      e.serialize(entry);
      new_index.insert(new_index.end(), entry.begin(), entry.end());
    }
    {
      auto uh = fmt::UniversalHeader::parse(new_index);
      uh.number_of_entries = static_cast<si8>(keep.size());
      uh.maximum_entry_size = fmt::TIME_SERIES_INDEX_BYTES;
      // At a known non-zero offset only plan_end_times moves an end (never
      // earlier, in the header's own convention). Otherwise as before: the
      // last block's start, which the follow-up repair corrects at offset 0.
      if (p.end.rto_nonzero) {
        if (p.end.new_end[1]) uh.end_time = *p.end.new_end[1];
      } else if (!keep.empty()) {
        uh.end_time = keep.back().start_time;
      }
      fmt::provenance::stamp_modified(uh, fmt::provenance::Operation::Recovery);
      uh.serialize(new_index);
      const ui4 body = crc::calculate(
          std::span<const ui1>(new_index).subspan(fmt::UNIVERSAL_HEADER_BYTES));
      byteio::write<ui4>(new_index, 4, body);
      byteio::write<ui4>(new_index, 0,
                         crc::calculate(std::span<const ui1>(new_index)
                                            .subspan(4, fmt::UNIVERSAL_HEADER_BYTES - 4)));
    }
    write_all(tidx, new_index);

    if (tail > 0) {
      // Truncate only a fragment that is not a whole, CRC-valid block. Real
      // blocks were indexed above rather than discarded.
      std::error_code ec;
      fsys::resize_file(tdat, static_cast<std::uintmax_t>(offset), ec);
      if (ec) throw IoError("cannot truncate " + tdat.string() + ": " + ec.message());
    }
    {
      // The .tdat header's entry count must match the index it is paired with.
      std::vector<ui1> head = read_range(data_in, 0, fmt::UNIVERSAL_HEADER_BYTES);
      auto uh = fmt::UniversalHeader::parse(head);
      uh.number_of_entries = static_cast<si8>(keep.size());
      if (p.end.rto_nonzero) {
        if (p.end.new_end[2]) uh.end_time = *p.end.new_end[2];
      } else if (!keep.empty()) {
        uh.end_time = keep.back().start_time;
      }
      fmt::provenance::stamp_modified(uh, fmt::provenance::Operation::Recovery);
      uh.serialize(head);
      // Truncated, blocks recovered past what the stored CRC was taken over,
      // or a header older than the last append (its entry count was stale,
      // and the writer patches count and CRC together): the stored CRC no
      // longer describes the body, so say "never computed" rather than
      // leaving a confidently wrong value. Otherwise the fix is header-only
      // (end time, dropped index entries), the body is as it was, and its
      // CRC — on a file that can be tens of GB — is kept.
      if (p.tail > 0 || !p.rebuilt.empty() || !p.tdat_header_current)
        byteio::write<ui4>(head, 4, fmt::CRC_NO_ENTRY);
      byteio::write<ui4>(
          head, 0,
          crc::calculate(std::span<const ui1>(head).subspan(4, fmt::UNIVERSAL_HEADER_BYTES - 4)));
      std::fstream f(tdat, std::ios::binary | std::ios::in | std::ios::out);
      if (!f) throw IoError("cannot open for header update: " + tdat.string());
      f.write(reinterpret_cast<const char*>(head.data()), fmt::UNIVERSAL_HEADER_BYTES);
      f.flush();
      if (!f) throw IoError("header update failed: " + tdat.string());
    }
    (void)detail::fsync_file(tdat.string());
    if (p.end.new_end[0]) {
      // The .tmet universal header: header only (the body CRC is unchanged
      // because the body is), the whole file rewritten by rename like the
      // .tidx, any foreign padding past the fixed-length record kept. Skipped,
      // with a note, if the header moved since the scan (a writer?).
      std::vector<ui1> tmet_bytes = read_all(p.tmet);
      if (tmet_bytes.size() < fmt::METADATA_FILE_BYTES ||
          !std::equal(p.tmet_header.begin(), p.tmet_header.end(), tmet_bytes.begin())) {
        report.notes.push_back(p.description + ": end time left as is: the segment metadata "
                                               "changed while recovery was running");
      } else {
        if (backup)
          back_up_bytes(backup_root, channel + "/" + base + ".tmet.header", p.tmet_header);
        auto uh = fmt::UniversalHeader::parse(tmet_bytes);
        uh.end_time = *p.end.new_end[0];
        fmt::provenance::stamp_modified(uh, fmt::provenance::Operation::Recovery);
        uh.serialize(tmet_bytes);
        byteio::write<ui4>(tmet_bytes, 0,
                           crc::calculate(std::span<const ui1>(tmet_bytes)
                                              .subspan(4, fmt::UNIVERSAL_HEADER_BYTES - 4)));
        write_all(p.tmet, tmet_bytes);
      }
    }
    if (const auto last = detail::last_segment_dir(p.channel_dir);
        last && detail::segment_number_of(last->stem().string()) != p.segment_number)
      channels_to_stamp.insert(p.channel_dir);
  }
  if (std::find(withdrawn.begin(), withdrawn.end(), true) != withdrawn.end()) {
    std::vector<RecoveredSegment> still;
    for (std::size_t i = 0; i < report.segments.size(); ++i)
      if (!withdrawn[i]) still.push_back(std::move(report.segments[i]));
    report.segments = std::move(still);
  }

  // segment-repaired: a NON-last segment changed, so the channel's last
  // segment records it (last_segment.hpp). Same backup and durability rules
  // as recovery's own header writes; a stamp that cannot be applied is a note.
  for (const auto& channel_dir : channels_to_stamp) {
    const auto stamp = detail::prepare_last_segment_stamp(channel_dir);
    const std::string channel = channel_dir.stem().string();
    if (!stamp.ready) {
      report.notes.push_back(channel + ": a segment was recovered, but the last segment could "
                                       "not record it: " + stamp.note +
                             detail::STAMP_SKIPPED_CONSEQUENCE);
      continue;
    }
    if (backup) {
      report.backup_root = backup_root.string();
      back_up_bytes(backup_root, channel + "/" + stamp.segment_stem + ".tmet.header",
                    stamp.old_header);
    }
    detail::write_last_segment_stamp(stamp);
    report.notes.push_back(channel + ": last segment stamped segment-repaired (header only): " +
                           stamp.tmet.string());
  }
  report.applied = apply;
  return report;
}

}  // namespace mef3io
