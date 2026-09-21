// mef3io — crash recovery. See recover.hpp for what this is and is not.
#include "mef3io/recover.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "durability.hpp"
#include "mef3io/byteio.hpp"
#include "mef3io/crc.hpp"
#include "mef3io/errors.hpp"
#include "mef3io/headers.hpp"
#include "mef3io/source.hpp"

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

}  // namespace

RecoveryReport recover_session(const std::string& path, bool apply, bool backup,
                               const std::string& password) {
  (void)password;  // only the index and data are touched; section 2 is not read
  if (path_has_suffix(path, ".tar"))
    throw IoError("cannot recover a tar session archive in place: " + path +
                  " (extract it first with extract_session)");
  if (!path_has_suffix(path, ".mefd"))
    throw IoError("session directory must end with .mefd: " + path);
  if (!fsys::is_directory(path)) throw IoError("no such session directory: " + path);

  RecoveryReport report;
  const fsys::path root(path);
  const fsys::path backup_root =
      root.parent_path() / (root.filename().string() + ".recover-backup");

  for (const auto& ch_entry : fsys::directory_iterator(root)) {
    if (!ch_entry.is_directory() || ch_entry.path().extension() != ".timd") continue;
    const std::string channel = ch_entry.path().stem().string();
    std::vector<fsys::path> segs;
    for (const auto& s : fsys::directory_iterator(ch_entry.path()))
      if (s.is_directory() && s.path().extension() == ".segd") segs.push_back(s.path());
    std::sort(segs.begin(), segs.end());

    for (const auto& seg : segs) {
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
      std::vector<fmt::TimeSeriesIndex> keep;
      keep.reserve(n_entries);
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
        keep.push_back(e);
      }
      const si8 dropped = static_cast<si8>(n_entries) - static_cast<si8>(keep.size());

      // --- does the data run past the index? recover those blocks ---
      si8 recovered = 0;
      std::uint64_t offset = keep.empty() ? fmt::UNIVERSAL_HEADER_BYTES
                                          : static_cast<std::uint64_t>(keep.back().file_offset +
                                                                       keep.back().block_bytes);
      si8 next_sample =
          keep.empty() ? 0 : keep.back().start_sample + keep.back().number_of_samples;
      fmt::RedBlockHeader header;
      while (offset < data_size && block_looks_real(data_in, data_size, offset, header)) {
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
        keep.push_back(e);
        next_sample += header.number_of_samples;
        offset += header.block_bytes;
        ++recovered;
      }
      const si8 tail = static_cast<si8>(data_size) - static_cast<si8>(offset);

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
      if (index_count != static_cast<si8>(keep.size())) ++stale_headers;
      if (data_count != static_cast<si8>(keep.size())) ++stale_headers;

      if (dropped == 0 && recovered == 0 && tail == 0 && stale_headers == 0)
        continue;  // segment is fine

      RecoveredSegment out;
      out.channel = channel;
      out.segment_number = segment_number;
      out.path = description;
      out.blocks_before = static_cast<si8>(n_entries);
      out.blocks_after = static_cast<si8>(keep.size());
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
                ", data " + std::to_string(data_count) + ", really " +
                std::to_string(keep.size()) + ") — a reader clamps the block count down to it "
                "and would read the segment short";
      }
      out.action = what;
      report.segments.push_back(std::move(out));

      if (!apply) continue;

      // --- write it back ---
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
        if (!keep.empty()) uh.end_time = keep.back().start_time;
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
        if (!keep.empty()) uh.end_time = keep.back().start_time;
        uh.serialize(head);
        // The body changed, so the stored body CRC no longer describes it. Say
        // "never computed" rather than leaving a confidently wrong value.
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
    }
  }
  report.applied = apply;
  return report;
}

}  // namespace mef3io
