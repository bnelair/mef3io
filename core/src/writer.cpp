// mef3io — low-level time-series segment writer.
#include "mef3io/writer.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <random>

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

std::array<ui1, 16> random_uuid() {
  std::array<ui1, 16> u{};
  std::random_device rd;
  for (auto& b : u) b = static_cast<ui1>(rd() & 0xFF);
  return u;
}

void write_file(const std::string& path, const std::vector<ui1>& bytes) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) throw IoError("cannot open for write: " + path);
  f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (!f) throw IoError("write failed: " + path);
}

// Fill body then header CRC of a universal-header-prefixed file image in place.
void finalize_crcs(std::vector<ui1>& file) {
  ui4 body_crc = crc::calculate(std::span<const ui1>(file).subspan(fmt::UNIVERSAL_HEADER_BYTES));
  byteio::write<ui4>(file, 4, body_crc);
  ui4 header_crc =
      crc::calculate(std::span<const ui1>(file).subspan(4, fmt::UNIVERSAL_HEADER_BYTES - 4));
  byteio::write<ui4>(file, 0, header_crc);
}

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
                              const std::vector<BlockSpec>& blocks, int n_threads) {
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

    fmt::TimeSeriesMetadataSection2 s2;
    s2.session_description = spec.session_name;
    s2.channel_description = spec.channel_name;
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
    s2.block_interval = static_cast<si8>(std::llround(s2.maximum_block_samples * 1e6 / fs_hz));
    s2.number_of_discontinuities = std::max<si8>(n_discont, 1);
    s2.maximum_contiguous_blocks = n_blocks;
    s2.maximum_contiguous_block_bytes = 0;
    s2.maximum_contiguous_samples = total_samples;
    s2.maximum_native_sample_value = static_cast<sf8>(global_max);
    s2.minimum_native_sample_value = static_cast<sf8>(global_min);
    s2.acquisition_channel_number = 1;

    fmt::MetadataSection3 s3;
    s3.recording_time_offset = rto;
    s3.gmt_offset = spec.gmt_offset;

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
    finalize_crcs(file);
    write_file((fs::path(segment_dir) / (base + ".tmet")).string(), file);
  }

  return total_samples;
}

si8 append_time_series_segment(const std::string& segment_dir, const SegmentSpec& spec,
                               const std::vector<BlockSpec>& blocks, int n_threads) {
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
  si8 last_end = old_end, n_discont = 0;
  ui4 max_block_bytes = 0;
  ui4 max_block_samples = 0;
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
    if (!blk.samples.empty()) {
      new_max = std::max(new_max, bmax[i]);
      new_min = std::min(new_min, bmin[i]);
    }
    if (blk.discontinuity) ++n_discont;
    last_end = blk.start_uutc + static_cast<si8>(std::llround(blk.samples.size() * 1e6 / fs_hz));
    appended_samples += static_cast<si8>(blk.samples.size());
  }
  const si8 end_disk = to_disk_time(last_end, rto);

  // --- .tdat: append the new blocks and patch the UH. The Koopman CRC has no
  // final XOR, so the stored body CRC is a resumable running state: seed from
  // it and update with only the appended bytes — appends stay O(new data)
  // instead of re-reading the whole (potentially huge) existing body. ---
  {
    std::vector<ui1> old_uh(fmt::UNIVERSAL_HEADER_BYTES);
    std::ifstream in(tdat_path, std::ios::binary);
    if (!in) throw IoError("cannot open for read: " + tdat_path);
    if (!in.read(reinterpret_cast<char*>(old_uh.data()), fmt::UNIVERSAL_HEADER_BYTES))
      throw IoError("short read: " + tdat_path);
    in.close();
    ui4 body_crc = byteio::read<ui4>(old_uh, 4);

    {
      std::ofstream app(tdat_path, std::ios::binary | std::ios::app);
      if (!app) throw IoError("cannot open for append: " + tdat_path);
      for (std::size_t i = 0; i < nb; ++i) {
        app.write(reinterpret_cast<const char*>(encoded[i].data()),
                  static_cast<std::streamsize>(encoded[i].size()));
        body_crc = crc::calculate(encoded[i], body_crc);
      }
      if (!app) throw IoError("append failed: " + tdat_path);
    }

    auto uh = fmt::UniversalHeader::parse(old_uh);
    uh.end_time = end_disk;
    uh.number_of_entries += static_cast<si8>(nb);
    uh.maximum_entry_size = std::max<si8>(uh.maximum_entry_size, max_block_bytes);
    uh.serialize(old_uh);
    byteio::write<ui4>(old_uh, 4, body_crc);
    ui4 header_crc =
        crc::calculate(std::span<const ui1>(old_uh).subspan(4, fmt::UNIVERSAL_HEADER_BYTES - 4));
    byteio::write<ui4>(old_uh, 0, header_crc);
    std::fstream hdr(tdat_path, std::ios::binary | std::ios::in | std::ios::out);
    if (!hdr) throw IoError("cannot open for header update: " + tdat_path);
    hdr.write(reinterpret_cast<const char*>(old_uh.data()), fmt::UNIVERSAL_HEADER_BYTES);
    if (!hdr) throw IoError("header update failed: " + tdat_path);
  }

  // --- .tidx: small; extend in memory and rewrite ---
  {
    std::vector<ui1> file = read_whole_file(tidx_path);
    auto uh = fmt::UniversalHeader::parse(file);
    uh.end_time = end_disk;
    uh.number_of_entries += static_cast<si8>(nb);
    std::vector<ui1> entry(fmt::TIME_SERIES_INDEX_BYTES);
    for (const auto& e : index) {
      e.serialize(entry);
      file.insert(file.end(), entry.begin(), entry.end());
    }
    uh.serialize(file);
    finalize_crcs(file);
    write_file(tidx_path, file);
  }

  // --- .tmet: update section-2 statistics, re-encrypt, rewrite. Section 1 and
  // section 3 bytes (and the password validation fields) are left verbatim. ---
  {
    fmt::TimeSeriesMetadataSection2 s2 = md.section2;
    const si8 seg_start = to_user_time(md.universal_header.start_time, rto);
    s2.number_of_samples += appended_samples;
    s2.number_of_blocks += static_cast<si8>(nb);
    s2.recording_duration = last_end - seg_start;
    s2.maximum_block_bytes = std::max<si8>(s2.maximum_block_bytes, max_block_bytes);
    s2.maximum_block_samples = std::max(s2.maximum_block_samples, max_block_samples);
    s2.block_interval = static_cast<si8>(std::llround(s2.maximum_block_samples * 1e6 / fs_hz));
    s2.number_of_discontinuities += n_discont;
    s2.maximum_contiguous_blocks = s2.number_of_blocks;
    s2.maximum_contiguous_samples = s2.number_of_samples;
    s2.maximum_native_sample_value =
        std::max(s2.maximum_native_sample_value, static_cast<sf8>(new_max));
    s2.minimum_native_sample_value =
        std::min(s2.minimum_native_sample_value, static_cast<sf8>(new_min));

    std::vector<ui1> s2buf(fmt::TIME_SERIES_METADATA_SECTION_2_BYTES);
    s2.serialize(s2buf);
    if (encrypt) {
      if (!keys.level1_key) throw PasswordError("level-1 key required to re-encrypt section 2");
      auto e2 = crypto::aes128_ecb_encrypt(s2buf, *keys.level1_key);
      std::copy(e2.begin(), e2.end(), s2buf.begin());
    }
    std::copy(s2buf.begin(), s2buf.end(), tmet.begin() + fmt::METADATA_SECTION_2_OFFSET);

    fmt::UniversalHeader uh = md.universal_header;
    uh.end_time = end_disk;
    uh.serialize(tmet);
    finalize_crcs(tmet);
    write_file(tmet_path, tmet);
  }

  return appended_samples;
}

}  // namespace mef3io
