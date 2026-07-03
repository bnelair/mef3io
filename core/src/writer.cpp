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

  const std::string base =
      spec.channel_name + "-" + [](int n) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%06d", n);
        return std::string(buf);
      }(spec.segment_number);

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

}  // namespace mef3io
