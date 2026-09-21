// mef3io — low-level time-series segment writer.
#include "mef3io/writer.hpp"

#include "durability.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <random>
#include <system_error>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "mef3io/byteio.hpp"
#include "mef3io/crc.hpp"
#include "mef3io/crypto.hpp"
#include "mef3io/errors.hpp"
#include "mef3io/headers.hpp"
#include "mef3io/metadata.hpp"
#include "mef3io/parallel.hpp"
#include "mef3io/red.hpp"

namespace fs = std::filesystem;

namespace mef3io {
namespace {

// Store an absolute uUTC as meflib does: negated, offset removed.
// to_user_time inverts this: -stored + rto == absolute.
si8 to_disk_time(si8 absolute, si8 rto) {
  if (absolute == fmt::UUTC_NO_ENTRY) return absolute;
  return rto - absolute;  // negative (absolute > rto)
}

si8 to_user_time(si8 stored, si8 rto) {
  if (stored == fmt::UUTC_NO_ENTRY) return stored;
  if (stored >= 0) return stored;
  return -stored + rto;
}

std::string segment_base_name(const SegmentSpec& spec) {
  char buf[8];
  std::snprintf(buf, sizeof(buf), "%06d", spec.segment_number);
  return spec.channel_name + "-" + buf;
}

std::vector<ui1> read_whole_file(const std::string& path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) throw IoError("cannot open file: " + path);
  auto size = f.tellg();
  f.seekg(0);
  std::vector<ui1> buf(static_cast<std::size_t>(size));
  if (!f.read(reinterpret_cast<char*>(buf.data()), size)) throw IoError("short read: " + path);
  return buf;
}

void finish_stream(std::ofstream& f, const std::string& path) {
  f.flush();
  if (!f) throw IoError("write failed (disk full?): " + path);
  f.close();
  if (!f) throw IoError("close failed, data may not have reached disk: " + path);
}

// File-identity and durability helpers are shared with the validator's repair
// path (durability.hpp): the append writes the SAMPLES, so it must be at
// least as careful as the path that rewrites 16 KB of declarations.
using detail::copy_file_identity;
using detail::replace_file;
using detail::fsync_directory;
using detail::fsync_file_or_throw;

void write_file_atomic(const std::string& path, std::span<const ui1> bytes,
                       bool durable = true) {
  const fs::path target(path);
  const fs::path tmp = target.parent_path() / (target.filename().string() + ".mef3io-tmp");
  std::error_code ignored;
  // Durability is only worth paying for when this write REPLACES something.
  // Creating a segment's files for the first time risks nothing: a crash
  // part-way leaves an incomplete session either way, and there is no good
  // copy underneath to lose. Replacing — which is what every append does to
  // the .tmet and .tidx — is different: the rename is ordered but not durable,
  // so without a flush a power cut can publish the new name over the old
  // bytes with the new ones still in cache, leaving a short .tmet that throws
  // from the metadata loader and takes the whole session down.
  //
  // Scoping it this way matters: a 128-channel session writes ~400 files, and
  // fsyncing every one of them on a fresh write cost roughly 3x the wall clock
  // for no guarantee anyone had.
  const bool replacing = fs::exists(target, ignored);
  fs::remove(tmp, ignored);
  try {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) throw IoError("cannot open for write: " + tmp.string());
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!f) throw IoError("write failed: " + tmp.string());
    finish_stream(f, tmp.string());
    copy_file_identity(target, tmp);
    // Flush the CONTENT before the rename, but do not flush the directory
    // entry after it. The asymmetry is deliberate. Flushing the content is
    // what stops the rename publishing a file whose bytes are still in cache —
    // a short .tmet that throws from the metadata loader and takes the whole
    // session down. Flushing the directory would only make the rename itself
    // survive a crash, and losing it is harmless: the old file is still there,
    // still complete, still consistent. Keeping a consistent previous state is
    // worth more than keeping the last append, and it halves the number of
    // fsyncs an append pays across a many-channel session.
    if (replacing && durable) fsync_file_or_throw(tmp.string());
    replace_file(tmp, target);
  } catch (...) {
    fs::remove(tmp, ignored);
    throw;
  }
}

void overwrite_file_prefix(const std::string& path, std::span<const ui1> bytes,
                           bool durable = false) {
  std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
  if (!f) throw IoError("cannot open for header update: " + path);
  f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  f.flush();
  if (!f) throw IoError("header update failed: " + path);
  f.close();
  if (!f) throw IoError("close failed, header may not have reached disk: " + path);
  // The universal header is what PUBLISHES the entries flushed just before it.
  // Leaving this best-effort meant the body could be durable while the count
  // naming it was not, so a crash could leave .tmet claiming more blocks than
  // the .tidx header admits — and meflib clamps the block count down to that
  // header, so the segment would read short. When the caller asked for
  // durability, this barrier is part of it.
  if (durable) detail::fsync_file_or_throw(path);
  else (void)detail::fsync_file(path);
}

std::array<ui1, 16> random_uuid() {
  std::array<ui1, 16> u{};
  std::random_device rd;
  for (auto& b : u) b = static_cast<ui1>(rd() & 0xFF);
  return u;
}

void write_file(const std::string& path, const std::vector<ui1>& bytes,
                bool durable = true) {
  write_file_atomic(path, bytes, durable);
}

// Same, for `.tmet` — a FIXED-length record. The body CRC must be bounded by
// METADATA_FILE_BYTES rather than taken to EOF: foreign writers append trailing
// bytes past the end of the record, and the reader (metadata.cpp) and the
// validator both bound their hash the same way. Hashing the padding writes a
// CRC that the reader then rejects, and because that throws from the metadata
// loader it kills the whole session, not just the segment.
void finalize_metadata_crcs(std::vector<ui1>& file) {
  const std::size_t body = static_cast<std::size_t>(fmt::METADATA_FILE_BYTES) -
                           static_cast<std::size_t>(fmt::UNIVERSAL_HEADER_BYTES);
  ui4 body_crc =
      crc::calculate(std::span<const ui1>(file).subspan(fmt::UNIVERSAL_HEADER_BYTES, body));
  byteio::write<ui4>(file, 4, body_crc);
  ui4 header_crc =
      crc::calculate(std::span<const ui1>(file).subspan(4, fmt::UNIVERSAL_HEADER_BYTES - 4));
  byteio::write<ui4>(file, 0, header_crc);
}

// Fill body then header CRC of a universal-header-prefixed file image in place.
void finalize_crcs(std::vector<ui1>& file) {
  ui4 body_crc = crc::calculate(std::span<const ui1>(file).subspan(fmt::UNIVERSAL_HEADER_BYTES));
  byteio::write<ui4>(file, 4, body_crc);
  ui4 header_crc =
      crc::calculate(std::span<const ui1>(file).subspan(4, fmt::UNIVERSAL_HEADER_BYTES - 4));
  byteio::write<ui4>(file, 0, header_crc);
}

// `difference_bytes` of an encoded RED block, straight out of its header.
ui4 block_difference_bytes(std::span<const ui1> encoded) {
  if (encoded.size() < fmt::RED_BLOCK_HEADER_BYTES) return 0;  // empty/degenerate block
  return byteio::read<ui4>(encoded, fmt::RedBlockHeader::DIFFERENCE_BYTES_OFFSET);
}

// meflib's worst case for the RED codec, RED_MAX_DIFFERENCE_BYTES(x): a full
// si4 plus one keysample flag byte per sample. Used only to bound blocks whose
// real difference_bytes we would otherwise have to re-read from .tdat.
//
// Saturates BELOW the sentinel, never to it: UI4_NO_ENTRY is what this field
// uses for "never set", so an overflow that returned it would write the exact
// unset value the bound exists to avoid. Mirrors the copy in validate.cpp.
ui4 red_max_difference_bytes(ui4 samples) {
  // Strictly BELOW the sentinel: UI4_NO_ENTRY / 5 * 5 is 0xFFFFFFFF exactly,
  // so saturating at the product would still hand back the sentinel.
  constexpr ui4 kMaxSamples = fmt::UI4_NO_ENTRY / 5u;
  return samples >= kMaxSamples ? fmt::UI4_NO_ENTRY - 1u : samples * 5u;
}

// Longest run of blocks uninterrupted by a discontinuity flag, which is how a
// reader delimits a contiguous run too. Each maximum is tracked independently:
// over-declaring only costs a reader some allocation, while under-declaring
// truncates its buffer.
class ContiguousRun {
 public:
  ContiguousRun() = default;
  /// Resume from a previously exported state, so an append can continue the
  /// open run instead of re-walking the index to rediscover it.
  ContiguousRun(si8 run_blocks, si8 run_samples, si8 run_bytes, si8 max_blocks, si8 max_samples,
                si8 max_bytes)
      : run_{run_blocks, run_samples, run_bytes}, max_{max_blocks, max_samples, max_bytes} {}

  si8 open_blocks() const { return run_.blocks; }
  si8 open_samples() const { return run_.samples; }
  si8 open_bytes() const { return run_.block_bytes; }

  void add(bool discontinuity, si8 samples, si8 block_bytes) {
    if (discontinuity) run_ = {};
    run_.blocks += 1;
    run_.samples += samples;
    run_.block_bytes += block_bytes;
    max_.blocks = std::max(max_.blocks, run_.blocks);
    max_.samples = std::max(max_.samples, run_.samples);
    max_.block_bytes = std::max(max_.block_bytes, run_.block_bytes);
  }

  si8 blocks() const { return max_.blocks; }
  si8 samples() const { return max_.samples; }
  si8 block_bytes() const { return max_.block_bytes; }

 private:
  struct Totals {
    si8 blocks = 0;
    si8 samples = 0;
    si8 block_bytes = 0;
  };
  Totals run_, max_;
};

fmt::UniversalHeader base_uh(const SegmentSpec& spec, const std::string& ftype, si8 start_disk,
                             si8 end_disk, si8 n_entries, si8 max_entry_size,
                             const std::array<ui1, 16>& level_uuid,
                             const crypto::ValidationFields& vf) {
  fmt::UniversalHeader uh;
  uh.file_type_string = ftype;
  uh.mef_version_major = fmt::MEF_VERSION_MAJOR;
  uh.mef_version_minor = fmt::MEF_VERSION_MINOR;
  uh.byte_order_code = fmt::MEF_LITTLE_ENDIAN;
  uh.start_time = start_disk;
  uh.end_time = end_disk;
  uh.number_of_entries = n_entries;
  uh.maximum_entry_size = max_entry_size;
  uh.segment_number = spec.segment_number;
  uh.channel_name = spec.channel_name;
  uh.session_name = spec.session_name;
  uh.level_uuid = level_uuid;
  uh.file_uuid = random_uuid();
  uh.provenance_uuid = uh.file_uuid;
  std::copy(vf.level1.begin(), vf.level1.end(), uh.level_1_password_validation_field.begin());
  std::copy(vf.level2.begin(), vf.level2.end(), uh.level_2_password_validation_field.begin());
  return uh;
}

}  // namespace

si8 write_time_series_segment(const std::string& segment_dir, const SegmentSpec& spec,
                              const std::vector<BlockSpec>& blocks, int n_threads,
                              ui4* out_max_difference_bytes) {
  if (!fs::is_directory(segment_dir)) throw IoError("segment dir does not exist: " + segment_dir);
  const sf8 fs_hz = spec.sampling_frequency;
  const si8 rto = spec.recording_time_offset;
  const bool encrypt = !spec.password_1.empty();

  crypto::ValidationFields vf =
      encrypt ? crypto::make_validation_fields(spec.password_1, spec.password_2)
              : crypto::ValidationFields{};
  crypto::AccessKeys keys;
  if (encrypt)
    keys = crypto::validate_password(spec.password_2.empty() ? spec.password_1 : spec.password_2,
                                     vf.level1, vf.level2);

  // --- encode blocks in parallel; each produces its own bytes + extrema ---
  const std::size_t nb = blocks.size();
  std::vector<std::vector<ui1>> encoded(nb);
  std::vector<si4> bmin(nb, 0), bmax(nb, 0);
  parallel_for(nb, n_threads, [&](std::size_t i) {
    // RED data blocks are written unencrypted (matches meflib default), even
    // when metadata is encrypted.
    encoded[i] = red::encode_block(blocks[i].samples, to_disk_time(blocks[i].start_uutc, rto),
                                   blocks[i].discontinuity, bmin[i], bmax[i]);
  });

  // --- assemble tdat body and index entries in order (cheap, sequential) ---
  std::vector<ui1> tdat_body;
  std::vector<fmt::TimeSeriesIndex> index;
  si8 total_samples = 0;
  si8 first_start = fmt::UUTC_NO_ENTRY, last_end = fmt::UUTC_NO_ENTRY;
  ui4 max_block_bytes = 0;
  ui4 max_difference_bytes = 0;
  ContiguousRun contiguous;
  si4 global_max = std::numeric_limits<si4>::min(), global_min = std::numeric_limits<si4>::max();
  si8 n_discont = 0;

  for (std::size_t i = 0; i < nb; ++i) {
    const auto& blk = blocks[i];
    fmt::TimeSeriesIndex e;
    // file_offset is relative to the start of the .tdat file (i.e. includes the
    // 1024-byte universal header), matching meflib.
    e.file_offset = static_cast<si8>(fmt::UNIVERSAL_HEADER_BYTES + tdat_body.size());
    e.start_time = to_disk_time(blk.start_uutc, rto);
    e.start_sample = blk.start_sample;
    e.number_of_samples = static_cast<ui4>(blk.samples.size());
    e.block_bytes = static_cast<ui4>(encoded[i].size());
    e.maximum_sample_value = bmax[i];
    e.minimum_sample_value = bmin[i];
    e.red_block_flags = blk.discontinuity ? fmt::RedBlockHeader::DISCONTINUITY_MASK : 0;
    index.push_back(e);

    tdat_body.insert(tdat_body.end(), encoded[i].begin(), encoded[i].end());
    max_block_bytes = std::max(max_block_bytes, e.block_bytes);
    max_difference_bytes = std::max(max_difference_bytes, block_difference_bytes(encoded[i]));
    contiguous.add(blk.discontinuity, static_cast<si8>(blk.samples.size()), e.block_bytes);
    if (!blk.samples.empty()) {
      global_max = std::max(global_max, bmax[i]);
      global_min = std::min(global_min, bmin[i]);
    }
    if (blk.discontinuity) ++n_discont;

    si8 blk_end = blk.start_uutc + static_cast<si8>(std::llround(blk.samples.size() * 1e6 / fs_hz));
    if (first_start == fmt::UUTC_NO_ENTRY) first_start = blk.start_uutc;
    last_end = blk_end;
    total_samples += static_cast<si8>(blk.samples.size());
  }

  const si8 start_disk = to_disk_time(first_start, rto);
  const si8 end_disk = to_disk_time(last_end, rto);
  const si8 n_blocks = static_cast<si8>(blocks.size());
  const auto level_uuid = random_uuid();

  const std::string base = segment_base_name(spec);

  // --- .tdat ---
  {
    std::vector<ui1> file(fmt::UNIVERSAL_HEADER_BYTES);
    auto uh = base_uh(spec, fmt::FILE_TYPE_TS_DATA, start_disk, end_disk, n_blocks,
                      max_block_bytes, level_uuid, vf);
    uh.serialize(file);
    file.insert(file.end(), tdat_body.begin(), tdat_body.end());
    finalize_crcs(file);
    write_file((fs::path(segment_dir) / (base + ".tdat")).string(), file);
  }

  // --- .tidx ---
  {
    std::vector<ui1> file(fmt::UNIVERSAL_HEADER_BYTES);
    auto uh = base_uh(spec, fmt::FILE_TYPE_TS_INDICES, start_disk, end_disk, n_blocks,
                      fmt::TIME_SERIES_INDEX_BYTES, level_uuid, vf);
    uh.serialize(file);
    std::vector<ui1> entry(fmt::TIME_SERIES_INDEX_BYTES);
    for (const auto& e : index) {
      e.serialize(entry);
      file.insert(file.end(), entry.begin(), entry.end());
    }
    finalize_crcs(file);
    write_file((fs::path(segment_dir) / (base + ".tidx")).string(), file);
  }

  // --- .tmet ---
  {
    std::vector<ui1> file(fmt::METADATA_FILE_BYTES, 0);

    fmt::MetadataSection1 s1;
    s1.section_2_encryption = encrypt ? fmt::LEVEL_1_ENCRYPTION : -fmt::LEVEL_1_ENCRYPTION;
    s1.section_3_encryption = encrypt ? fmt::LEVEL_2_ENCRYPTION : -fmt::LEVEL_2_ENCRYPTION;

    const SessionMetadata& meta = spec.metadata;
    fmt::TimeSeriesMetadataSection2 s2;
    // Descriptive fields: use the user value, else fall back to the name.
    s2.session_description =
        meta.session_description.empty() ? spec.session_name : meta.session_description;
    s2.channel_description =
        meta.channel_description.empty() ? spec.channel_name : meta.channel_description;
    s2.reference_description = meta.reference_description;
    s2.acquisition_channel_number = meta.acquisition_channel_number;
    s2.low_frequency_filter_setting = meta.low_frequency_filter;
    s2.high_frequency_filter_setting = meta.high_frequency_filter;
    s2.notch_filter_frequency_setting = meta.notch_filter;
    s2.ac_line_frequency = meta.line_frequency;
    s2.sampling_frequency = fs_hz;
    s2.units_conversion_factor = spec.units_conversion_factor;
    s2.units_description = spec.units_description;
    s2.number_of_samples = total_samples;
    s2.number_of_blocks = n_blocks;
    s2.start_sample = blocks.empty() ? 0 : blocks.front().start_sample;
    s2.recording_duration =
        static_cast<si8>(std::llround((last_end - first_start)));
    s2.maximum_block_bytes = max_block_bytes;
    s2.maximum_block_samples = 0;
    for (const auto& b : blocks)
      s2.maximum_block_samples =
          std::max(s2.maximum_block_samples, static_cast<ui4>(b.samples.size()));
    s2.maximum_difference_bytes = max_difference_bytes;
    s2.block_interval = static_cast<si8>(std::llround(s2.maximum_block_samples * 1e6 / fs_hz));
    s2.number_of_discontinuities = std::max<si8>(n_discont, 1);
    s2.maximum_contiguous_blocks = contiguous.blocks();
    s2.maximum_contiguous_block_bytes = contiguous.block_bytes();
    s2.maximum_contiguous_samples = contiguous.samples();
    // Native units = counts * units_conversion_factor, with the pair swapped
    // for a negative factor (pymef3_file.c:972-978). Storing raw counts here
    // would disagree with every other MEF writer by a factor of 1/ufact.
    {
      const sf8 ufact = spec.units_conversion_factor;
      const sf8 hi = static_cast<sf8>(global_max) * ufact;
      const sf8 lo = static_cast<sf8>(global_min) * ufact;
      s2.maximum_native_sample_value = ufact >= 0.0 ? hi : lo;
      s2.minimum_native_sample_value = ufact >= 0.0 ? lo : hi;
    }

    fmt::MetadataSection3 s3;
    s3.recording_time_offset = rto;
    s3.gmt_offset = spec.gmt_offset;
    s3.subject_name_1 = meta.subject_name_1;
    s3.subject_name_2 = meta.subject_name_2;
    s3.subject_id = meta.subject_id;
    s3.recording_location = meta.recording_location;

    // Serialize sections into temporary buffers, encrypt if needed.
    std::vector<ui1> s2buf(fmt::TIME_SERIES_METADATA_SECTION_2_BYTES);
    s2.serialize(s2buf);
    std::vector<ui1> s3buf(fmt::METADATA_SECTION_3_BYTES);
    s3.serialize(s3buf);
    if (encrypt) {
      if (!keys.level1_key || !keys.level2_key)
        throw PasswordError("both passwords required to encrypt metadata sections");
      auto e2 = crypto::aes128_ecb_encrypt(s2buf, *keys.level1_key);
      std::copy(e2.begin(), e2.end(), s2buf.begin());
      auto e3 = crypto::aes128_ecb_encrypt(s3buf, *keys.level2_key);
      std::copy(e3.begin(), e3.end(), s3buf.begin());
    }

    auto uh = base_uh(spec, fmt::FILE_TYPE_TS_METADATA, start_disk, end_disk, 1,
                      fmt::METADATA_FILE_BYTES, level_uuid, vf);
    uh.serialize(file);
    std::span<ui1> fspan(file);
    // section 1
    {
      std::vector<ui1> s1buf(fmt::METADATA_SECTION_1_BYTES);
      s1.serialize(s1buf);
      std::copy(s1buf.begin(), s1buf.end(), file.begin() + fmt::METADATA_SECTION_1_OFFSET);
    }
    std::copy(s2buf.begin(), s2buf.end(), file.begin() + fmt::METADATA_SECTION_2_OFFSET);
    std::copy(s3buf.begin(), s3buf.end(), file.begin() + fmt::METADATA_SECTION_3_OFFSET);
    // Equivalent here (this image is exactly METADATA_FILE_BYTES), but stated
    // explicitly so the fixed-length rule holds for every .tmet we write.
    finalize_metadata_crcs(file);
    write_file((fs::path(segment_dir) / (base + ".tmet")).string(), file);
  }

  if (out_max_difference_bytes) *out_max_difference_bytes = max_difference_bytes;
  return total_samples;
}

si8 append_time_series_segment(const std::string& segment_dir, const SegmentSpec& spec,
                               const std::vector<BlockSpec>& blocks, int n_threads,
                               ui4* out_max_difference_bytes, AppendIndexCache* cache) {
  if (blocks.empty()) return 0;
  const std::string base = segment_base_name(spec);
  const std::string tmet_path = (fs::path(segment_dir) / (base + ".tmet")).string();
  const std::string tidx_path = (fs::path(segment_dir) / (base + ".tidx")).string();
  const std::string tdat_path = (fs::path(segment_dir) / (base + ".tdat")).string();
  if (!fs::exists(tmet_path) || !fs::exists(tidx_path) || !fs::exists(tdat_path))
    throw IoError("segment to append to is incomplete or missing: " + segment_dir);

  // --- existing metadata is authoritative: load, decrypt, validate ---
  const std::string password = spec.password_2.empty() ? spec.password_1 : spec.password_2;
  std::vector<ui1> tmet = read_whole_file(tmet_path);
  TimeSeriesMetadata md = load_time_series_metadata(tmet, password);
  const bool encrypt = md.section1.section_2_encryption > 0;
  if (encrypt && md.access_level < fmt::LEVEL_1_ACCESS)
    throw PasswordError("appending to an encrypted segment requires level-1 access: " + tmet_path);

  si8 rto = md.section3_available ? md.section3.recording_time_offset : 0;
  if (rto == fmt::UUTC_NO_ENTRY) rto = 0;
  const sf8 fs_hz = md.section2.sampling_frequency;

  if (std::abs(fs_hz - spec.sampling_frequency) > 1e-9 * std::max(1.0, std::abs(fs_hz)))
    throw WriteConflictError("append fs " + std::to_string(spec.sampling_frequency) +
                             " != segment fs " + std::to_string(fs_hz));
  if (std::abs(md.section2.units_conversion_factor - spec.units_conversion_factor) >
      1e-12 * std::max(1.0, std::abs(md.section2.units_conversion_factor)))
    throw WriteConflictError("append units_conversion_factor " +
                             std::to_string(spec.units_conversion_factor) + " != segment's " +
                             std::to_string(md.section2.units_conversion_factor));
  const si8 old_end = to_user_time(md.universal_header.end_time, rto);
  // Per-block half-microsecond rounding can store an end time up to ~1 us past
  // the grid-exact end, so allow half a sample period of slack: a start within
  // it cannot land on (or before) any stored grid sample.
  const si8 slack = static_cast<si8>(std::llround(0.5e6 / fs_hz));
  if (blocks.front().start_uutc < old_end - slack)
    throw WriteConflictError("append starts at " + std::to_string(blocks.front().start_uutc) +
                             " uUTC, before segment end " + std::to_string(old_end));

  crypto::AccessKeys keys;
  if (encrypt)
    keys = crypto::validate_password(password, md.universal_header.level_1_password_validation_field,
                                     md.universal_header.level_2_password_validation_field);

  // --- encode new blocks (parallel, deterministic; RED data stays unencrypted) ---
  const std::size_t nb = blocks.size();
  std::vector<std::vector<ui1>> encoded(nb);
  std::vector<si4> bmin(nb, 0), bmax(nb, 0);
  parallel_for(nb, n_threads, [&](std::size_t i) {
    encoded[i] = red::encode_block(blocks[i].samples, to_disk_time(blocks[i].start_uutc, rto),
                                   blocks[i].discontinuity, bmin[i], bmax[i]);
  });

  // --- new index entries; file offsets continue from the current .tdat size ---
  const si8 old_tdat_size = static_cast<si8>(fs::file_size(tdat_path));
  std::vector<fmt::TimeSeriesIndex> index;
  si8 appended_samples = 0, running_offset = old_tdat_size;
  si8 last_end = old_end;
  ui4 max_block_bytes = 0;
  ui4 max_block_samples = 0;
  ui4 max_difference_bytes = 0;
  si4 new_max = std::numeric_limits<si4>::min(), new_min = std::numeric_limits<si4>::max();
  for (std::size_t i = 0; i < nb; ++i) {
    const auto& blk = blocks[i];
    fmt::TimeSeriesIndex e;
    e.file_offset = running_offset;
    e.start_time = to_disk_time(blk.start_uutc, rto);
    e.start_sample = blk.start_sample;
    e.number_of_samples = static_cast<ui4>(blk.samples.size());
    e.block_bytes = static_cast<ui4>(encoded[i].size());
    e.maximum_sample_value = bmax[i];
    e.minimum_sample_value = bmin[i];
    e.red_block_flags = blk.discontinuity ? fmt::RedBlockHeader::DISCONTINUITY_MASK : 0;
    index.push_back(e);

    running_offset += static_cast<si8>(encoded[i].size());
    max_block_bytes = std::max(max_block_bytes, e.block_bytes);
    max_block_samples = std::max(max_block_samples, e.number_of_samples);
    max_difference_bytes = std::max(max_difference_bytes, block_difference_bytes(encoded[i]));
    if (!blk.samples.empty()) {
      new_max = std::max(new_max, bmax[i]);
      new_min = std::min(new_min, bmin[i]);
    }
    last_end = blk.start_uutc + static_cast<si8>(std::llround(blk.samples.size() * 1e6 / fs_hz));
    appended_samples += static_cast<si8>(blk.samples.size());
  }
  const si8 end_disk = to_disk_time(last_end, rto);

  // --- Read the current .tdat header and advance its resumable body CRC with
  // the new bytes in memory. The Koopman CRC has no final XOR, so the stored
  // body CRC is a running state: update it with only the appended bytes —
  // appends stay O(new data) instead of re-reading the whole (potentially
  // huge) existing body.
  //
  // The universal header is patched further down, AFTER the .tidx has been
  // walked. Its entry count and maximum entry size describe the whole segment,
  // so they must come from the index rather than be folded onto whatever the
  // old header declared — see the note at the .tdat header patch. ---
  std::vector<ui1> tdat_uh(fmt::UNIVERSAL_HEADER_BYTES);
  ui4 tdat_body_crc = 0;
  bool tdat_body_crc_known = true;
  {
    std::ifstream in(tdat_path, std::ios::binary);
    if (!in) throw IoError("cannot open for read: " + tdat_path);
    if (!in.read(reinterpret_cast<char*>(tdat_uh.data()), fmt::UNIVERSAL_HEADER_BYTES))
      throw IoError("short read: " + tdat_path);
    in.close();
    // CRC_NO_ENTRY (0) means "never computed" — meflib legitimately ships it
    // for a piecemeal writer (meflib.c:9319-9321 computes the body CRC only on
    // a whole-file write). Seeding a running CRC from it would turn an honest
    // "not computed" into a confidently WRONG value, so the sentinel is
    // carried through unchanged instead: still unverifiable, but not a lie.
    tdat_body_crc = byteio::read<ui4>(tdat_uh, 4);
    tdat_body_crc_known = tdat_body_crc != fmt::CRC_NO_ENTRY;
    if (tdat_body_crc_known)
      for (std::size_t i = 0; i < nb; ++i)
        tdat_body_crc = crc::calculate(encoded[i], tdat_body_crc);
  }

  // --- Build the new .tidx in memory. The full entry list is the only place
  // the pre-existing blocks are described cheaply, so the section-2 sizing
  // statistics that span the whole segment are recomputed from it here rather
  // than folded into whatever the old .tmet happened to declare. That also
  // repairs those fields on segments written before mef3io filled them in. ---
  // --- Extend the .tidx. ---
  //
  // Two paths, producing byte-identical files (pinned by
  // test_append_index_cache_matches_the_full_walk):
  //
  //   SLOW — read the whole index, walk every entry, rewrite the file. Needed
  //   the first time this writer touches a segment, because the declarations
  //   describe ALL of its blocks and the index is the only cheap record of the
  //   ones already there. Also the fallback whenever the file is not the size
  //   the cache remembers, i.e. anything changed it behind our back.
  //
  //   FAST — append the new entries in place, extend the body CRC over just
  //   those bytes, and patch the 1024-byte header. O(new data), whatever the
  //   session length. The CRC extends because Koopman-32 here is a plain
  //   rolling state with no final inversion, so crc(A||B) == crc(B, crc(A));
  //   the .tdat path has always relied on this.
  //
  // This matters because these sessions run for days to months. Rewriting the
  // whole index on every append is O(total blocks), i.e. quadratic over the
  // recording: at one month and 256 channels it is gigabytes of pointless I/O
  // per append.
  ContiguousRun contiguous;
  ui4 index_max_block_samples = 0;
  si8 index_max_block_bytes = 0;
  si8 index_total_samples = 0;
  si8 index_n_discontinuities = 0;
  std::size_t index_entries = 0;
  std::vector<ui1> new_entries(index.size() * fmt::TIME_SERIES_INDEX_BYTES);
  for (std::size_t i = 0; i < index.size(); ++i)
    index[i].serialize(std::span<ui1>(new_entries).subspan(i * fmt::TIME_SERIES_INDEX_BYTES,
                                                           fmt::TIME_SERIES_INDEX_BYTES));

  const std::uintmax_t tidx_size_on_disk = fs::file_size(tidx_path);
  const bool use_cache = cache && cache->valid &&
                         cache->file_size == static_cast<std::size_t>(tidx_size_on_disk);

  // Rollback state: the bytes to put back if a later step fails. On the fast
  // path only the header and the file length change, so that is all we keep.
  std::vector<ui1> original_tidx;      // slow path: the file, verbatim
  std::vector<ui1> original_tidx_uh;   // fast path: just the header
  const std::size_t original_tidx_size = static_cast<std::size_t>(tidx_size_on_disk);
  std::vector<ui1> new_tidx_uh;        // the patched 1024-byte header
  // Slow path only: truncate to here (padding stripped) before appending.
  std::size_t strip_tidx_to = 0;
  ui4 tidx_body_crc = 0;

  if (use_cache) {
    index_entries = cache->entries + index.size();
    index_total_samples = cache->total_samples;
    index_n_discontinuities = cache->n_discontinuities;
    index_max_block_samples = cache->max_block_samples;
    index_max_block_bytes = cache->max_block_bytes;
    contiguous = ContiguousRun(cache->run_blocks, cache->run_samples, cache->run_bytes,
                               cache->max_blocks, cache->max_samples, cache->max_bytes);
    for (const auto& e : index) {
      const bool discontinuity =
          (e.red_block_flags & fmt::RedBlockHeader::DISCONTINUITY_MASK) != 0;
      contiguous.add(discontinuity, e.number_of_samples, e.block_bytes);
      index_max_block_samples = std::max(index_max_block_samples, e.number_of_samples);
      index_max_block_bytes = std::max<si8>(index_max_block_bytes, e.block_bytes);
      index_total_samples += e.number_of_samples;
      if (discontinuity) ++index_n_discontinuities;
    }
    tidx_body_crc = crc::calculate(new_entries, cache->body_crc);

    original_tidx_uh.assign(fmt::UNIVERSAL_HEADER_BYTES, 0);
    {
      std::ifstream in(tidx_path, std::ios::binary);
      if (!in || !in.read(reinterpret_cast<char*>(original_tidx_uh.data()),
                          fmt::UNIVERSAL_HEADER_BYTES))
        throw IoError("short read: " + tidx_path);
    }
    new_tidx_uh = original_tidx_uh;
    auto uh = fmt::UniversalHeader::parse(new_tidx_uh);
    uh.end_time = end_disk;
    uh.number_of_entries = static_cast<si8>(index_entries);
    uh.serialize(new_tidx_uh);
    byteio::write<ui4>(new_tidx_uh, 4, tidx_body_crc);
    byteio::write<ui4>(new_tidx_uh, 0,
                       crc::calculate(std::span<const ui1>(new_tidx_uh)
                                          .subspan(4, fmt::UNIVERSAL_HEADER_BYTES - 4)));
  } else {
    // The existing entries have to be READ — the totals, maxima and open
    // contiguous run can only come from them, and so can the body CRC. But
    // they do NOT have to be rewritten: the new entries are appended in place
    // exactly as on the fast path. Rewriting the whole index here cost a full
    // extra write per append for a caller that reopens its Writer per block,
    // which is a natural way to use the library and would be ~47x write
    // amplification for a 10-minute block against a month-long index.
    std::vector<ui1> existing = read_whole_file(tidx_path);
    original_tidx = existing;   // byte-exact rollback, padding included
    // Drop trailing bytes that do not form a whole entry before appending.
    // Foreign writers pad past the last entry and the validator deliberately
    // tolerates it (crc.index bounds its hash by the declared count) — but
    // appending after the padding puts every new entry at a broken stride, and
    // the reader then rejects the whole file with "index file body is not a
    // whole number of entries". Reproduced: 16 bytes of padding plus one append
    // made the session unreadable.
    if (existing.size() > fmt::UNIVERSAL_HEADER_BYTES) {
      const std::size_t body = existing.size() - fmt::UNIVERSAL_HEADER_BYTES;
      existing.resize(fmt::UNIVERSAL_HEADER_BYTES +
                      (body - (body % fmt::TIME_SERIES_INDEX_BYTES)));
    }
    // What the file will be truncated to before the new entries are appended.
    strip_tidx_to = existing.size();

    std::span<const ui1> existing_entries =
        std::span<const ui1>(existing).subspan(fmt::UNIVERSAL_HEADER_BYTES);
    // Rolling CRC over the entries already on disk, then over the new ones —
    // the same value a whole-body hash would give, computed once.
    tidx_body_crc = crc::calculate(existing_entries);
    tidx_body_crc = crc::calculate(new_entries, tidx_body_crc);

    const std::size_t n_entries =
        existing_entries.size() / fmt::TIME_SERIES_INDEX_BYTES + index.size();
    // Count what the file now holds, rather than adding to what the old header
    // claimed. A foreign or older header may carry meflib's NO_ENTRY (-1) or a
    // plain wrong number, and `stored + nb` propagates that error forever —
    // meflib clamps number_of_blocks DOWN to this field (meflib.c:5983-5984,
    // :6005-6006), so an undercount makes the segment read short, or empty.
    index_entries = n_entries;
    original_tidx_uh.assign(existing.begin(), existing.begin() + fmt::UNIVERSAL_HEADER_BYTES);
    new_tidx_uh = original_tidx_uh;
    {
      auto uh = fmt::UniversalHeader::parse(new_tidx_uh);
      uh.end_time = end_disk;
      uh.number_of_entries = static_cast<si8>(n_entries);
      uh.serialize(new_tidx_uh);
      byteio::write<ui4>(new_tidx_uh, 4, tidx_body_crc);
      byteio::write<ui4>(new_tidx_uh, 0,
                         crc::calculate(std::span<const ui1>(new_tidx_uh)
                                            .subspan(4, fmt::UNIVERSAL_HEADER_BYTES - 4)));
    }
    auto fold = [&](const fmt::TimeSeriesIndex& e) {
      const bool discontinuity = (e.red_block_flags & fmt::RedBlockHeader::DISCONTINUITY_MASK) != 0;
      // An entry that does not say how big its block is cannot be totalled.
      // Coercing it to 0 (so a sentinel cannot inflate the total) makes every
      // figure derived here too SMALL, and those figures are about to be
      // written back as the segment's declarations — under-declaring a
      // reader's buffer, the direction that truncates. Refuse the append
      // instead; `python -m mef3io validate` names the segment.
      if (e.number_of_samples == fmt::UI4_NO_ENTRY || e.block_bytes == fmt::UI4_NO_ENTRY)
        throw WriteConflictError(
            "cannot append: an existing .tidx entry leaves its sample or byte count unset "
            "(NO_ENTRY), so the segment's totals cannot be recomputed without under-declaring "
            "them: " + tidx_path);
      const ui4 samples = e.number_of_samples;
      const ui4 bytes = e.block_bytes;
      contiguous.add(discontinuity, samples, bytes);
      index_max_block_samples = std::max(index_max_block_samples, samples);
      index_max_block_bytes = std::max<si8>(index_max_block_bytes, bytes);
      index_total_samples += samples;
      if (discontinuity) ++index_n_discontinuities;
    };
    const std::size_t n_existing = existing_entries.size() / fmt::TIME_SERIES_INDEX_BYTES;
    for (std::size_t i = 0; i < n_existing; ++i)
      fold(fmt::TimeSeriesIndex::parse(existing_entries.subspan(
          i * fmt::TIME_SERIES_INDEX_BYTES, fmt::TIME_SERIES_INDEX_BYTES)));
    for (const auto& e : index) fold(e);
  }

  // --- Build the new .tdat universal header, now that the index has been
  // walked. Both fields describe the whole segment, so both come from the
  // index rather than from the old header: `number_of_entries` is what meflib
  // clamps number_of_blocks down to, and `maximum_entry_size` folded onto a
  // stored value would keep a foreign writer's number (the legacy stack stores
  // a SAMPLE COUNT there) or meflib's NO_ENTRY. ---
  std::vector<ui1> new_tdat_uh = tdat_uh;
  {
    auto uh = fmt::UniversalHeader::parse(new_tdat_uh);
    uh.end_time = end_disk;
    uh.number_of_entries = static_cast<si8>(index_entries);
    uh.maximum_entry_size = index_max_block_bytes;
    uh.serialize(new_tdat_uh);
    byteio::write<ui4>(new_tdat_uh, 4,
                       tdat_body_crc_known ? tdat_body_crc : fmt::CRC_NO_ENTRY);
    const ui4 header_crc = crc::calculate(
        std::span<const ui1>(new_tdat_uh).subspan(4, fmt::UNIVERSAL_HEADER_BYTES - 4));
    byteio::write<ui4>(new_tdat_uh, 0, header_crc);
  }

  // --- Build the new .tmet in memory. Section 1 and section 3 bytes (and the
  // password validation fields) are left verbatim. ---
  std::vector<ui1> new_tmet = tmet;
  {
    fmt::TimeSeriesMetadataSection2 s2 = md.section2;
    const si8 seg_start = to_user_time(md.universal_header.start_time, rto);
    // Totals come from the index, which describes every block old and new.
    // Folding onto the stored value carries a foreign writer's mistake forward
    // for the life of the file: the legacy stack writes 0 discontinuities while
    // writing the flags, and a NO_ENTRY (-1) total would make every subsequent
    // append off by one.
    s2.number_of_samples = index_total_samples;
    s2.number_of_blocks = static_cast<si8>(index_entries);
    s2.number_of_discontinuities = index_n_discontinuities;
    s2.recording_duration = last_end - seg_start;
    // Derived from the index rather than max()'d onto the stored value: a
    // foreign segment may carry NO_ENTRY (0xFFFFFFFF / -1) here, and a max()
    // would preserve the sentinel forever — and then block_interval below
    // would be computed from it.
    s2.maximum_block_bytes = std::max<si8>(index_max_block_bytes, max_block_bytes);
    s2.maximum_block_samples = std::max(index_max_block_samples, max_block_samples);
    s2.block_interval = static_cast<si8>(std::llround(s2.maximum_block_samples * 1e6 / fs_hz));
    s2.maximum_contiguous_blocks = contiguous.blocks();
    s2.maximum_contiguous_block_bytes = contiguous.block_bytes();
    s2.maximum_contiguous_samples = contiguous.samples();
    // The pre-existing blocks' difference_bytes live in .tdat block headers, so
    // folding them in exactly would cost one seek per old block and break the
    // O(new data) cost of an append. Trust the stored maximum when the segment
    // carries one; otherwise (mef3io <= 1.1.2 wrote 0, a foreign writer may
    // write NO_ENTRY) fall back to meflib's own worst case over the blocks the
    // index describes, which bounds them without reading .tdat.
    const ui4 stored_difference_bytes = s2.maximum_difference_bytes;
    // The old blocks' real difference_bytes live in .tdat block headers, and
    // re-reading them would cost a seek per block and break the O(new data)
    // cost of an append. A stored value is therefore UNVERIFIABLE from here: it
    // may be an honest measurement, or a defective writer's under-declaration —
    // 0, NO_ENTRY, or, just as dangerous, a plausible-looking number that is
    // simply wrong. Screening it against the blocks being appended does not
    // separate those cases: it catches a stored 1, but a stored 3000 against a
    // true 12488 passes whenever the new blocks happen to be smaller, and the
    // crash-class defect rides forward untouched.
    //
    // So take meflib's own worst case for the largest block the index describes
    // as a FLOOR. It bounds every block in the segment, old and new, whatever
    // the stored value meant, which makes the result safe without reading one
    // extra byte. Over-declaring only costs a reader some memory;
    // under-declaring truncates its buffer, which is the crash this module
    // exists to prevent. The price is modest — 5 x samples against a real
    // maximum of roughly 2.5-3 x samples — and this is the only declaration
    // that pays it; everything the index describes is still exact. A stored
    // value LARGER than the bound is kept rather than lowered: it cannot be a
    // real measurement (no RED block exceeds the worst case), but honouring it
    // costs only memory, and this path deliberately never moves a declaration
    // in the direction that truncates.
    //
    // The one exception: a caller that ENCODED the old blocks itself knows
    // their exact maximum and passes it in `known_difference_bytes`. Then no
    // bound is needed — the declaration stays exact across a chunked write,
    // which is the ordinary way a session is built.
    ui4 declared_difference_bytes = std::max(max_difference_bytes, spec.known_difference_bytes);
    if (spec.known_difference_bytes == 0)
      declared_difference_bytes =
          std::max(declared_difference_bytes, red_max_difference_bytes(index_max_block_samples));
    // NEVER move this field downwards, on either path. `known_difference_bytes`
    // means "every block in this segment was encoded by the caller", which is
    // true of a chunked write but stops being true the moment anything else
    // touches the segment — a second writer open on the same session, or
    // another process. The caller's maximum is then stale and LOWER than the
    // truth, and writing it truncates the buffer a meflib reader decodes into.
    // Folding the stored value in costs nothing when it is already correct (the
    // max() is a no-op on an ordinary chunked write, so the declaration stays
    // exact) and makes the descent unrepresentable.
    if (stored_difference_bytes != 0 && stored_difference_bytes != fmt::UI4_NO_ENTRY)
      declared_difference_bytes = std::max(declared_difference_bytes, stored_difference_bytes);
    s2.maximum_difference_bytes = declared_difference_bytes;
    // These are in NATIVE units (counts * units_conversion_factor), not raw
    // counts - see pymef3_file.c:972-978, which also swaps the pair when the
    // factor is negative. Folding a raw si4 onto them mixes units and lands the
    // value out by 1/ufact, permanently, since later appends max() against it.
    {
      const sf8 ufact = s2.units_conversion_factor;
      const sf8 new_hi = static_cast<sf8>(new_max) * ufact;
      const sf8 new_lo = static_cast<sf8>(new_min) * ufact;
      const sf8 hi = ufact >= 0.0 ? new_hi : new_lo;
      const sf8 lo = ufact >= 0.0 ? new_lo : new_hi;
      // meflib's NO_ENTRY for these two is a NaN, which "must be tested with
      // isnan()" (meflib.h:443/445) — and std::max(NaN, x) returns NaN, so an
      // unset extreme would stay unset forever even though the append knows a
      // real value. Take ours outright when the stored one is not a number.
      s2.maximum_native_sample_value = std::isnan(s2.maximum_native_sample_value)
                                           ? hi
                                           : std::max(s2.maximum_native_sample_value, hi);
      s2.minimum_native_sample_value = std::isnan(s2.minimum_native_sample_value)
                                           ? lo
                                           : std::min(s2.minimum_native_sample_value, lo);
    }

    // Edit the STORED section-2 image in place instead of re-serializing the
    // struct over a blank buffer. Section 2 is 10752 bytes; this struct models
    // the fields up to offset 6432 and nothing beyond, so a full serialize
    // zeroes meflib's protected region (6432, 2160 B) and discretionary region
    // (8592, 2160 B) — 4320 bytes of a foreign writer's metadata destroyed on
    // every append — and rewrites every text field NUL-terminated, shortening
    // one that filled its field exactly. `serialize_derived_fields` touches
    // only the eleven declarations an append actually changes. The validator's
    // repair path already does exactly this (validate.cpp); the append did not.
    std::vector<ui1> s2buf(new_tmet.begin() + fmt::METADATA_SECTION_2_OFFSET,
                           new_tmet.begin() + fmt::METADATA_SECTION_2_OFFSET +
                               fmt::TIME_SERIES_METADATA_SECTION_2_BYTES);
    if (encrypt) {
      if (!keys.level1_key) throw PasswordError("level-1 key required to re-encrypt section 2");
      auto plain = crypto::aes128_ecb_decrypt(s2buf, *keys.level1_key);
      s2.serialize_derived_fields(plain);
      auto e2 = crypto::aes128_ecb_encrypt(plain, *keys.level1_key);
      std::copy(e2.begin(), e2.end(), s2buf.begin());
    } else {
      s2.serialize_derived_fields(s2buf);
    }
    std::copy(s2buf.begin(), s2buf.end(), new_tmet.begin() + fmt::METADATA_SECTION_2_OFFSET);

    fmt::UniversalHeader uh = md.universal_header;
    uh.end_time = end_disk;
    uh.serialize(new_tmet);
    // NOT finalize_crcs: .tmet is a FIXED-length record and foreign writers
    // append trailing bytes past its end. Hashing to EOF writes a body CRC the
    // reader — which bounds its hash by METADATA_FILE_BYTES — will reject,
    // taking the whole session down from the metadata loader's constructor.
    // The same trap the reader and the validator already avoid.
    finalize_metadata_crcs(new_tmet);
  }

  bool appended_tdat = false;
  bool wrote_tidx = false;
  bool patched_tdat_header = false;
  bool wrote_tmet = false;
  try {
    {
      std::ofstream app(tdat_path, std::ios::binary | std::ios::app);
      if (!app) throw IoError("cannot open for append: " + tdat_path);
      appended_tdat = true;  // any later failure may have left partial bytes on disk
      for (std::size_t i = 0; i < nb; ++i) {
        app.write(reinterpret_cast<const char*>(encoded[i].data()),
                  static_cast<std::streamsize>(encoded[i].size()));
      }
      if (!app) throw IoError("append failed: " + tdat_path);
      finish_stream(app, tdat_path);
    }
    // Barrier. The .tidx written next POINTS AT these bytes, so they have to be
    // durable before anything references them: a power cut between the two
    // otherwise leaves an index whose entries run past the real end of .tdat —
    // a state the in-process rollback cannot repair, because the process is
    // gone. Ordering the samples before the references is the whole guarantee.
    if (spec.durable) fsync_file_or_throw(tdat_path);

    // Each flag is set BEFORE the call that might fail. These writes can throw
    // partway — overwrite_file_prefix patches bytes in place and only then
    // flushes — so a flag set afterwards would skip rollback for a file that
    // had already been modified. Rolling back a file that was never touched is
    // harmless; failing to roll back one that was is not.
    wrote_tidx = true;
    // ONE write path, cached or not: append the entries in place, make them
    // durable, and only THEN patch the header that declares them. The header is
    // what a reader counts, so publishing it before the entries are on disk is
    // the one ordering that could leave the index describing bytes that are not
    // there. The index is never rewritten whole — at month scale that was a
    // full extra write of a multi-megabyte file for every append.
    if (!use_cache && strip_tidx_to) {
      // Trailing bytes that do not form a whole entry (foreign padding, which
      // the validator tolerates) go before the new entries are appended.
      std::error_code ec;
      if (fs::file_size(tidx_path, ec) != strip_tidx_to)
        fs::resize_file(tidx_path, static_cast<std::uintmax_t>(strip_tidx_to), ec);
      if (ec) throw IoError("cannot trim index padding: " + tidx_path + ": " + ec.message());
    }
    {
      std::ofstream app(tidx_path, std::ios::binary | std::ios::app);
      if (!app) throw IoError("cannot open for append: " + tidx_path);
      app.write(reinterpret_cast<const char*>(new_entries.data()),
                static_cast<std::streamsize>(new_entries.size()));
      if (!app) throw IoError("append failed: " + tidx_path);
      finish_stream(app, tidx_path);
    }
    if (spec.durable) fsync_file_or_throw(tidx_path);
    overwrite_file_prefix(tidx_path, new_tidx_uh, spec.durable);
    patched_tdat_header = true;
    overwrite_file_prefix(tdat_path, new_tdat_uh, spec.durable);
    wrote_tmet = true;
    write_file(tmet_path, new_tmet, spec.durable);
  } catch (...) {
    if (cache) cache->valid = false;  // the file may no longer match the summary
    std::string rollback_error;
    auto note_rollback = [&](const std::string& what) {
      if (rollback_error.empty()) rollback_error = what;
    };
    // A rollback that fails on a file the failure never reached is not a
    // rollback failure. The wrote_* flags are set BEFORE each call, so on an
    // environmental failure — a read-only directory, a full disk — the flag is
    // set, the write never happened, and the rollback rewrite then fails for
    // exactly the same reason. Escalating that told the operator their
    // clinical archive may be inconsistent when it was byte-identical. So:
    // compare first, and only report a failure that left the file changed.
    auto restore = [&](const std::string& path, const std::vector<ui1>& original,
                       const char* label, auto&& write_back) {
      try {
        if (read_whole_file(path) == original) return;  // never modified
      } catch (const std::exception&) {
        // Cannot read it back to compare — fall through and try the restore.
      }
      try {
        write_back();
      } catch (const std::exception& e) {
        note_rollback(std::string(label) + " rollback failed: " + e.what());
      }
    };
    if (wrote_tmet)
      restore(tmet_path, tmet, "tmet", [&] { write_file(tmet_path, tmet); });
    if (wrote_tidx) {
      // The file was extended in place, so the original is its first
      // `original_tidx_size` bytes with the old header. On the slow path any
      // stripped padding lived in those bytes, so write them back verbatim
      // rather than truncating — padding is not necessarily zero.
      try {
        if (!original_tidx.empty()) {
          write_file(tidx_path, original_tidx, spec.durable);
        } else {
          std::error_code ec;
          fs::resize_file(tidx_path, static_cast<std::uintmax_t>(original_tidx_size), ec);
          if (ec) note_rollback("tidx truncate rollback failed: " + ec.message());
          else overwrite_file_prefix(tidx_path, original_tidx_uh);
        }
      } catch (const std::exception& e) {
        note_rollback(std::string("tidx rollback failed: ") + e.what());
      }
    }
    if (patched_tdat_header) {
      // Only the 1024-byte header was patched in place; compare just that.
      try {
        std::ifstream in(tdat_path, std::ios::binary);
        std::vector<ui1> head(fmt::UNIVERSAL_HEADER_BYTES);
        if (in && in.read(reinterpret_cast<char*>(head.data()), fmt::UNIVERSAL_HEADER_BYTES) &&
            head == tdat_uh) {
          // untouched
        } else {
          overwrite_file_prefix(tdat_path, tdat_uh);
        }
      } catch (const std::exception& e) {
        note_rollback(std::string("tdat header rollback failed: ") + e.what());
      }
    }
    if (appended_tdat) {
      std::error_code ec;
      fs::resize_file(tdat_path, static_cast<std::uintmax_t>(old_tdat_size), ec);
      if (ec) note_rollback("tdat truncate rollback failed: " + ec.message());
    }
    if (!rollback_error.empty())
      throw IoError("append failed and rollback was incomplete: " + rollback_error);
    throw;
  }

  if (out_max_difference_bytes) *out_max_difference_bytes = max_difference_bytes;
  if (cache) {
    // Everything reached disk, so the summary now describes the file. Recorded
    // unconditionally — including after a slow path — so the NEXT append can
    // take the fast one.
    cache->valid = true;
    cache->file_size = (use_cache ? original_tidx_size : strip_tidx_to) + new_entries.size();
    cache->entries = index_entries;
    cache->total_samples = index_total_samples;
    cache->n_discontinuities = index_n_discontinuities;
    cache->max_block_samples = index_max_block_samples;
    cache->max_block_bytes = index_max_block_bytes;
    cache->body_crc = tidx_body_crc;
    cache->run_blocks = contiguous.open_blocks();
    cache->run_samples = contiguous.open_samples();
    cache->run_bytes = contiguous.open_bytes();
    cache->max_blocks = contiguous.blocks();
    cache->max_samples = contiguous.samples();
    cache->max_bytes = contiguous.block_bytes();
  }
  return appended_samples;
}

}  // namespace mef3io
