// mef3io — the streaming block-copy engine (see segment_copy.hpp).
#include "segment_copy.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <vector>

#include "mef3io/byteio.hpp"
#include "mef3io/crc.hpp"
#include "mef3io/errors.hpp"

namespace fs = std::filesystem;

namespace mef3io::detail {
namespace {

constexpr std::size_t UH = fmt::UNIVERSAL_HEADER_BYTES;
constexpr std::size_t ENTRY = fmt::TIME_SERIES_INDEX_BYTES;
constexpr std::size_t RED = fmt::RED_BLOCK_HEADER_BYTES;
// Entries read per index chunk, and the most block bytes fetched in one read.
// Both bound the memory the copy holds whatever the segment's size.
constexpr std::size_t ENTRIES_PER_CHUNK = 8192;           // ~450 KB of index
constexpr std::size_t BATCH_BYTES = 8u * 1024u * 1024u;   // 8 MB of blocks

class OutFile {
 public:
  explicit OutFile(const std::string& path) : path_(path) {
    if (fs::exists(path)) throw IoError("output file already exists: " + path);
    f_.open(path, std::ios::binary | std::ios::trunc);
    if (!f_) throw IoError("cannot create file: " + path);
    const std::vector<ui1> placeholder(UH, 0);
    put(placeholder, false);
  }
  /// Append bytes; `body` = they count toward the body CRC.
  void put(std::span<const ui1> b, bool body = true) {
    f_.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    if (!f_) throw IoError("write failed (disk full?): " + path_);
    if (body) crc_ = crc::calculate(b, crc_);
    bytes_ += b.size();
  }
  ui4 body_crc() const { return crc_; }
  std::uint64_t bytes() const { return bytes_; }
  void finish(std::span<const ui1> header) {
    f_.seekp(0);
    f_.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(UH));
    f_.flush();
    if (!f_) throw IoError("write failed: " + path_);
    f_.close();
    if (!f_) throw IoError("close failed: " + path_);
  }

 private:
  std::string path_;
  std::ofstream f_;
  ui4 crc_ = fmt::CRC_START_VALUE;
  std::uint64_t bytes_ = 0;
};

void seal_header(std::vector<ui1>& uh, ui4 body_crc, si8 entries, si8 max_entry) {
  byteio::write<si8>(uh, 32, entries);
  byteio::write<si8>(uh, 40, max_entry);
  byteio::write<ui4>(uh, 4, body_crc);
  byteio::write<ui4>(uh, 0, crc::calculate(std::span<const ui1>(uh).subspan(4, UH - 4)));
}

}  // namespace

CopyStats copy_segment(const SegmentCopySpec& spec) {
  const SessionSource& src = *spec.source;
  const std::string where = src.describe(spec.tdat_rel);
  const std::uint64_t tidx_size = src.file_size(spec.tidx_rel);
  const std::uint64_t tdat_size = src.file_size(spec.tdat_rel);
  if (tidx_size < UH || tdat_size < UH)
    throw FormatError("segment file smaller than its universal header: " + where);
  if ((tidx_size - UH) % ENTRY != 0)
    throw FormatError("block index ends in a partial entry (an interrupted write?); run "
                      "recover_session first: " + src.describe(spec.tidx_rel));
  const std::size_t n_entries = static_cast<std::size_t>((tidx_size - UH) / ENTRY);

  std::vector<ui1> tidx_uh = src.read_range(spec.tidx_rel, 0, UH);
  std::vector<ui1> tdat_uh = src.read_range(spec.tdat_rel, 0, UH);
  CopyStats st;
  st.source_bytes = 2 * UH;

  OutFile tdat(spec.tdat_out);
  OutFile tidx(spec.tidx_out);

  si8 run_blocks = 0, run_samples = 0, run_bytes = 0;
  std::uint64_t prev_end = UH;  // source: blocks must be in file order, non-overlapping

  for (std::size_t first = 0; first < n_entries; first += ENTRIES_PER_CHUNK) {
    if (spec.control) spec.control->check();
    const std::size_t k = std::min(ENTRIES_PER_CHUNK, n_entries - first);
    std::vector<ui1> entries = src.read_range(spec.tidx_rel, UH + first * ENTRY, k * ENTRY);
    st.source_bytes += entries.size();
    if (spec.on_progress) spec.on_progress(entries.size());

    std::size_t i = 0;
    while (i < k) {
      if (spec.control) spec.control->check();
      // A batch: consecutive entries whose blocks fit one bounded read.
      const auto entry_at = [&](std::size_t j) {
        return std::span<ui1>(entries).subspan(j * ENTRY, ENTRY);
      };
      const auto offset_of = [&](std::size_t j) {
        return byteio::read<si8>(entry_at(j), 0);
      };
      const auto bytes_of = [&](std::size_t j) {
        return byteio::read<ui4>(entry_at(j), 28);
      };
      std::size_t j = i;
      std::uint64_t lo = 0, hi = 0;
      while (j < k) {
        const si8 off = offset_of(j);
        const ui4 nb = bytes_of(j);
        const std::size_t global = first + j;
        if (off < static_cast<si8>(UH) || nb < RED ||
            static_cast<std::uint64_t>(off) + nb > tdat_size)
          throw FormatError("block " + std::to_string(global) + " of " + where +
                            " lies outside the data file (offset " + std::to_string(off) +
                            ", " + std::to_string(nb) + " bytes, file " +
                            std::to_string(tdat_size) + " bytes); run recover_session first");
        if (static_cast<std::uint64_t>(off) < prev_end)
          throw FormatError("block " + std::to_string(global) + " of " + where +
                            " overlaps or precedes the block before it in the index; the "
                            "index is not in file order, so the segment cannot be copied "
                            "block by block");
        if (j > i && static_cast<std::uint64_t>(off) + nb - lo > BATCH_BYTES) break;
        if (j == i) lo = static_cast<std::uint64_t>(off);
        hi = static_cast<std::uint64_t>(off) + nb;
        prev_end = hi;
        ++j;
      }
      std::vector<ui1> buf = src.read_range(spec.tdat_rel, lo, hi - lo);
      st.source_bytes += buf.size();

      for (std::size_t b = i; b < j; ++b) {
        const std::size_t global = first + b;
        const std::size_t at = static_cast<std::size_t>(offset_of(b)) - lo;
        const ui4 nb = bytes_of(b);
        std::span<ui1> block(buf.data() + at, nb);
        const ui4 header_bytes = byteio::read<ui4>(block, 36);
        if (header_bytes != nb)
          throw FormatError("block " + std::to_string(global) + " of " + where +
                            ": the index says " + std::to_string(nb) +
                            " bytes, the block header " + std::to_string(header_bytes));
        const ui4 stored_crc = byteio::read<ui4>(block, 0);
        if (crc::calculate(std::span<const ui1>(block).subspan(4)) != stored_crc)
          throw CrcError("block " + std::to_string(global) + " of " + where +
                         " fails its CRC: the data is damaged, and copying it would hide that");

        CopyBlock cb{global, entry_at(b), block};
        if (spec.on_block && !spec.on_block(cb)) continue;

        byteio::write<ui4>(block, 0, crc::calculate(std::span<const ui1>(block).subspan(4)));
        const si8 new_offset = static_cast<si8>(tdat.bytes());
        tdat.put(block);
        byteio::write<si8>(cb.entry, 0, new_offset);
        tidx.put(cb.entry);

        const ui4 samples = byteio::read<ui4>(block, 32);
        const ui4 diff = byteio::read<ui4>(block, fmt::RedBlockHeader::DIFFERENCE_BYTES_OFFSET);
        const bool discont =
            (byteio::read<ui1>(block, 4) & fmt::RedBlockHeader::DISCONTINUITY_MASK) != 0;
        st.blocks += 1;
        st.samples += samples;
        st.max_block_bytes = std::max(st.max_block_bytes, nb);
        st.max_block_samples = std::max(st.max_block_samples, samples);
        st.max_difference_bytes = std::max(st.max_difference_bytes, diff);
        if (discont) {
          st.discontinuities += 1;
          run_blocks = run_samples = run_bytes = 0;
        }
        run_blocks += 1;
        run_samples += samples;
        run_bytes += nb;
        st.max_contiguous_blocks = std::max(st.max_contiguous_blocks, run_blocks);
        st.max_contiguous_samples = std::max(st.max_contiguous_samples, run_samples);
        st.max_contiguous_block_bytes = std::max(st.max_contiguous_block_bytes, run_bytes);
      }
      if (spec.on_progress) spec.on_progress(buf.size());
      i = j;
    }
  }
  if (spec.control) spec.control->check();

  if (spec.on_header) {
    spec.on_header(tdat_uh, true);
    spec.on_header(tidx_uh, false);
  }
  seal_header(tdat_uh, tdat.body_crc(), st.blocks, st.max_block_bytes);
  seal_header(tidx_uh, tidx.body_crc(), st.blocks, static_cast<si8>(ENTRY));
  tdat.finish(tdat_uh);
  tidx.finish(tidx_uh);
  st.output_bytes = tdat.bytes() + tidx.bytes();
  return st;
}

}  // namespace mef3io::detail
