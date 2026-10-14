// mef3io — parse/serialize for MEF fixed-layout structures.
#include "mef3io/headers.hpp"

#include <algorithm>
#include <iterator>
#include <string_view>
#include <tuple>

#include "mef3io/byteio.hpp"
#include "mef3io/crc.hpp"
#include "mef3io/version.hpp"

namespace mef3io::fmt {
namespace {
using byteio::read;
using byteio::read_string;
using byteio::write;
using byteio::write_string;

template <std::size_t N>
std::array<ui1, N> read_bytes(std::span<const ui1> buf, std::size_t off) {
  std::array<ui1, N> a{};
  if (off + N > buf.size()) throw FormatError("read_bytes out of range");
  std::copy_n(buf.data() + off, N, a.begin());
  return a;
}

template <std::size_t N>
void write_bytes(std::span<ui1> buf, std::size_t off, const std::array<ui1, N>& a) {
  if (off + N > buf.size()) throw FormatError("write_bytes out of range");
  std::copy_n(a.begin(), N, buf.data() + off);
}
}  // namespace

// --- UniversalHeader ---------------------------------------------------------
UniversalHeader UniversalHeader::parse(std::span<const ui1> b) {
  if (b.size() < UNIVERSAL_HEADER_BYTES) throw FormatError("universal header: buffer too small");
  UniversalHeader h;
  h.header_crc = read<ui4>(b, 0);
  h.body_crc = read<ui4>(b, 4);
  h.file_type_string = read_string(b, 8, 5);
  h.mef_version_major = read<ui1>(b, 13);
  h.mef_version_minor = read<ui1>(b, 14);
  h.byte_order_code = read<ui1>(b, 15);
  h.start_time = read<si8>(b, 16);
  h.end_time = read<si8>(b, 24);
  h.number_of_entries = read<si8>(b, 32);
  h.maximum_entry_size = read<si8>(b, 40);
  h.segment_number = read<si4>(b, 48);
  h.channel_name = read_string(b, 52, 256);
  h.session_name = read_string(b, 308, 256);
  h.anonymized_name = read_string(b, 564, 256);
  h.level_uuid = read_bytes<16>(b, 820);
  h.file_uuid = read_bytes<16>(b, 836);
  h.provenance_uuid = read_bytes<16>(b, 852);
  h.level_1_password_validation_field = read_bytes<16>(b, 868);
  h.level_2_password_validation_field = read_bytes<16>(b, 884);
  h.protected_region = read_bytes<60>(b, 900);
  h.discretionary_region = read_bytes<64>(b, 960);
  return h;
}

void UniversalHeader::serialize(std::span<ui1> b) const {
  if (b.size() < UNIVERSAL_HEADER_BYTES) throw FormatError("universal header: buffer too small");
  std::fill_n(b.data(), UNIVERSAL_HEADER_BYTES, ui1{0});
  write<ui4>(b, 0, header_crc);
  write<ui4>(b, 4, body_crc);
  write_string(b, 8, 5, file_type_string);
  write<ui1>(b, 13, mef_version_major);
  write<ui1>(b, 14, mef_version_minor);
  write<ui1>(b, 15, byte_order_code);
  write<si8>(b, 16, start_time);
  write<si8>(b, 24, end_time);
  write<si8>(b, 32, number_of_entries);
  write<si8>(b, 40, maximum_entry_size);
  write<si4>(b, 48, segment_number);
  write_string(b, 52, 256, channel_name);
  write_string(b, 308, 256, session_name);
  write_string(b, 564, 256, anonymized_name);
  write_bytes<16>(b, 820, level_uuid);
  write_bytes<16>(b, 836, file_uuid);
  write_bytes<16>(b, 852, provenance_uuid);
  write_bytes<16>(b, 868, level_1_password_validation_field);
  write_bytes<16>(b, 884, level_2_password_validation_field);
  write_bytes<60>(b, 900, protected_region);
  write_bytes<64>(b, 960, discretionary_region);
}

bool UniversalHeader::header_crc_valid(std::span<const ui1> file_bytes) const {
  if (file_bytes.size() < UNIVERSAL_HEADER_BYTES) return false;
  ui4 calc = crc::calculate(file_bytes.subspan(4, UNIVERSAL_HEADER_BYTES - 4));
  return calc == header_crc;
}

void UniversalHeader::update_header_crc(std::span<ui1> file_bytes) {
  ui4 calc = crc::calculate(file_bytes.subspan(4, UNIVERSAL_HEADER_BYTES - 4));
  header_crc = calc;
  write<ui4>(file_bytes, 0, header_crc);
}

bool UniversalHeader::is_password_protected() const {
  auto nonzero = [](const std::array<ui1, 16>& a) {
    return std::any_of(a.begin(), a.end(), [](ui1 x) { return x != 0; });
  };
  return nonzero(level_1_password_validation_field) ||
         nonzero(level_2_password_validation_field);
}

// --- MetadataSection1 --------------------------------------------------------
MetadataSection1 MetadataSection1::parse(std::span<const ui1> b) {
  if (b.size() < METADATA_SECTION_1_BYTES) throw FormatError("section 1: buffer too small");
  MetadataSection1 s;
  s.section_2_encryption = read<si1>(b, 0);
  s.section_3_encryption = read<si1>(b, 1);
  return s;
}

void MetadataSection1::serialize(std::span<ui1> b) const {
  if (b.size() < METADATA_SECTION_1_BYTES) throw FormatError("section 1: buffer too small");
  std::fill_n(b.data(), METADATA_SECTION_1_BYTES, ui1{0});
  write<si1>(b, 0, section_2_encryption);
  write<si1>(b, 1, section_3_encryption);
}

// --- TimeSeriesMetadataSection2 ---------------------------------------------
TimeSeriesMetadataSection2 TimeSeriesMetadataSection2::parse(std::span<const ui1> b) {
  if (b.size() < TIME_SERIES_METADATA_SECTION_2_BYTES)
    throw FormatError("TS section 2: buffer too small");
  TimeSeriesMetadataSection2 s;
  s.channel_description = read_string(b, 0, 2048);
  s.session_description = read_string(b, 2048, 2048);
  s.recording_duration = read<si8>(b, 4096);
  s.reference_description = read_string(b, 4104, 2048);
  s.acquisition_channel_number = read<si8>(b, 6152);
  s.sampling_frequency = read<sf8>(b, 6160);
  s.low_frequency_filter_setting = read<sf8>(b, 6168);
  s.high_frequency_filter_setting = read<sf8>(b, 6176);
  s.notch_filter_frequency_setting = read<sf8>(b, 6184);
  s.ac_line_frequency = read<sf8>(b, 6192);
  s.units_conversion_factor = read<sf8>(b, 6200);
  s.units_description = read_string(b, 6208, 128);
  s.maximum_native_sample_value = read<sf8>(b, 6336);
  s.minimum_native_sample_value = read<sf8>(b, 6344);
  s.start_sample = read<si8>(b, 6352);
  s.number_of_samples = read<si8>(b, 6360);
  s.number_of_blocks = read<si8>(b, 6368);
  s.maximum_block_bytes = read<si8>(b, 6376);
  s.maximum_block_samples = read<ui4>(b, 6384);
  s.maximum_difference_bytes = read<ui4>(b, 6388);
  s.block_interval = read<si8>(b, 6392);
  s.number_of_discontinuities = read<si8>(b, 6400);
  s.maximum_contiguous_blocks = read<si8>(b, 6408);
  s.maximum_contiguous_block_bytes = read<si8>(b, 6416);
  s.maximum_contiguous_samples = read<si8>(b, 6424);
  return s;
}

void TimeSeriesMetadataSection2::serialize(std::span<ui1> b) const {
  if (b.size() < TIME_SERIES_METADATA_SECTION_2_BYTES)
    throw FormatError("TS section 2: buffer too small");
  std::fill_n(b.data(), TIME_SERIES_METADATA_SECTION_2_BYTES, ui1{0});
  write_string(b, 0, 2048, channel_description);
  write_string(b, 2048, 2048, session_description);
  write<si8>(b, 4096, recording_duration);
  write_string(b, 4104, 2048, reference_description);
  write<si8>(b, 6152, acquisition_channel_number);
  write<sf8>(b, 6160, sampling_frequency);
  write<sf8>(b, 6168, low_frequency_filter_setting);
  write<sf8>(b, 6176, high_frequency_filter_setting);
  write<sf8>(b, 6184, notch_filter_frequency_setting);
  write<sf8>(b, 6192, ac_line_frequency);
  write<sf8>(b, 6200, units_conversion_factor);
  write_string(b, 6208, 128, units_description);
  write<sf8>(b, 6336, maximum_native_sample_value);
  write<sf8>(b, 6344, minimum_native_sample_value);
  write<si8>(b, 6352, start_sample);
  write<si8>(b, 6360, number_of_samples);
  write<si8>(b, 6368, number_of_blocks);
  write<si8>(b, 6376, maximum_block_bytes);
  write<ui4>(b, 6384, maximum_block_samples);
  write<ui4>(b, 6388, maximum_difference_bytes);
  write<si8>(b, 6392, block_interval);
  write<si8>(b, 6400, number_of_discontinuities);
  write<si8>(b, 6408, maximum_contiguous_blocks);
  write<si8>(b, 6416, maximum_contiguous_block_bytes);
  write<si8>(b, 6424, maximum_contiguous_samples);
}

void TimeSeriesMetadataSection2::serialize_derived_fields(std::span<ui1> b) const {
  if (b.size() < TIME_SERIES_METADATA_SECTION_2_BYTES)
    throw FormatError("TS section 2: buffer too small");
  // No fill: every byte not listed here keeps its existing value.
  write<si8>(b, 4096, recording_duration);
  write<sf8>(b, 6336, maximum_native_sample_value);
  write<sf8>(b, 6344, minimum_native_sample_value);
  write<si8>(b, 6352, start_sample);
  write<si8>(b, 6360, number_of_samples);
  write<si8>(b, 6368, number_of_blocks);
  write<si8>(b, 6376, maximum_block_bytes);
  write<ui4>(b, 6384, maximum_block_samples);
  write<ui4>(b, 6388, maximum_difference_bytes);
  write<si8>(b, 6392, block_interval);
  write<si8>(b, 6400, number_of_discontinuities);
  write<si8>(b, 6408, maximum_contiguous_blocks);
  write<si8>(b, 6416, maximum_contiguous_block_bytes);
  write<si8>(b, 6424, maximum_contiguous_samples);
}

// --- MetadataSection3 --------------------------------------------------------
MetadataSection3 MetadataSection3::parse(std::span<const ui1> b) {
  if (b.size() < METADATA_SECTION_3_BYTES) throw FormatError("section 3: buffer too small");
  MetadataSection3 s;
  s.recording_time_offset = read<si8>(b, 0);
  s.dst_start_time = read<si8>(b, 8);
  s.dst_end_time = read<si8>(b, 16);
  s.gmt_offset = read<si4>(b, 24);
  s.subject_name_1 = read_string(b, 28, 128);
  s.subject_name_2 = read_string(b, 156, 128);
  s.subject_id = read_string(b, 284, 128);
  s.recording_location = read_string(b, 412, 512);
  return s;
}

void MetadataSection3::serialize(std::span<ui1> b) const {
  if (b.size() < METADATA_SECTION_3_BYTES) throw FormatError("section 3: buffer too small");
  std::fill_n(b.data(), METADATA_SECTION_3_BYTES, ui1{0});
  write<si8>(b, 0, recording_time_offset);
  write<si8>(b, 8, dst_start_time);
  write<si8>(b, 16, dst_end_time);
  write<si4>(b, 24, gmt_offset);
  write_string(b, 28, 128, subject_name_1);
  write_string(b, 156, 128, subject_name_2);
  write_string(b, 284, 128, subject_id);
  write_string(b, 412, 512, recording_location);
}

// --- TimeSeriesIndex ---------------------------------------------------------
TimeSeriesIndex TimeSeriesIndex::parse(std::span<const ui1> b) {
  if (b.size() < TIME_SERIES_INDEX_BYTES) throw FormatError("TS index entry: buffer too small");
  TimeSeriesIndex e;
  e.file_offset = read<si8>(b, 0);
  e.start_time = read<si8>(b, 8);
  e.start_sample = read<si8>(b, 16);
  e.number_of_samples = read<ui4>(b, 24);
  e.block_bytes = read<ui4>(b, 28);
  e.maximum_sample_value = read<si4>(b, 32);
  e.minimum_sample_value = read<si4>(b, 36);
  e.red_block_flags = read<ui1>(b, 44);
  return e;
}

void TimeSeriesIndex::serialize(std::span<ui1> b) const {
  if (b.size() < TIME_SERIES_INDEX_BYTES) throw FormatError("TS index entry: buffer too small");
  std::fill_n(b.data(), TIME_SERIES_INDEX_BYTES, ui1{0});
  write<si8>(b, 0, file_offset);
  write<si8>(b, 8, start_time);
  write<si8>(b, 16, start_sample);
  write<ui4>(b, 24, number_of_samples);
  write<ui4>(b, 28, block_bytes);
  write<si4>(b, 32, maximum_sample_value);
  write<si4>(b, 36, minimum_sample_value);
  write<ui1>(b, 44, red_block_flags);
}

// --- RedBlockHeader ----------------------------------------------------------
RedBlockHeader RedBlockHeader::parse(std::span<const ui1> b) {
  if (b.size() < RED_BLOCK_HEADER_BYTES) throw FormatError("RED block header: buffer too small");
  RedBlockHeader h;
  h.crc = read<ui4>(b, 0);
  h.flags = read<ui1>(b, 4);
  h.detrend_slope = read<sf4>(b, 16);
  h.detrend_intercept = read<sf4>(b, 20);
  h.scale_factor = read<sf4>(b, 24);
  h.difference_bytes = read<ui4>(b, RedBlockHeader::DIFFERENCE_BYTES_OFFSET);
  h.number_of_samples = read<ui4>(b, 32);
  h.block_bytes = read<ui4>(b, 36);
  h.start_time = read<si8>(b, 40);
  h.statistics = read_bytes<256>(b, 48);
  return h;
}

void RedBlockHeader::serialize(std::span<ui1> b) const {
  if (b.size() < RED_BLOCK_HEADER_BYTES) throw FormatError("RED block header: buffer too small");
  std::fill_n(b.data(), RED_BLOCK_HEADER_BYTES, ui1{0});
  write<ui4>(b, 0, crc);
  write<ui1>(b, 4, flags);
  write<sf4>(b, 16, detrend_slope);
  write<sf4>(b, 20, detrend_intercept);
  write<sf4>(b, 24, scale_factor);
  write<ui4>(b, DIFFERENCE_BYTES_OFFSET, difference_bytes);
  write<ui4>(b, 32, number_of_samples);
  write<ui4>(b, 36, block_bytes);
  write<si8>(b, 40, start_time);
  write_bytes<256>(b, 48, statistics);
}

// --- Provenance region ---

namespace provenance {
namespace {

using Region = std::array<ui1, REGION_BYTES>;
static_assert(std::tuple_size_v<decltype(UniversalHeader::discretionary_region)> == REGION_BYTES);

bool has_magic(const Region& r) {
  return std::equal(std::begin(MAGIC), std::end(MAGIC), r.begin() + MAGIC_OFFSET);
}

bool all_zero(const Region& r) {
  return std::all_of(r.begin(), r.end(), [](ui1 b) { return b == 0; });
}

ui4 read_u32(const Region& r, std::size_t off) {
  return byteio::read<ui4>(std::span<const ui1>(r), off);  // range-checked
}

void write_u32(Region& r, std::size_t off, ui4 v) {
  byteio::write<ui4>(std::span<ui1>(r), off, v);  // range-checked
}

std::string read_version(const Region& r, std::size_t off) {
  std::string s;
  for (std::size_t i = 0; i < VERSION_BYTES && r[off + i] != 0; ++i)
    s.push_back(static_cast<char>(r[off + i]));
  return s;
}

// ASCII only, at most VERSION_BYTES - 1 characters, always NUL-terminated:
// the field is fully cleared first, then filled, so nothing outside it is
// touched whatever the version string is.
void write_version(Region& r, std::size_t off, std::string_view v) {
  if (v.empty()) v = mef3io::version();
  std::fill_n(r.begin() + off, VERSION_BYTES, ui1{0});
  for (std::size_t i = 0; i + 1 < VERSION_BYTES && i < v.size() && v[i] != '\0'; ++i) {
    const auto c = static_cast<unsigned char>(v[i]);
    r[off + i] = (c >= 0x20 && c < 0x7F) ? c : static_cast<ui1>('?');
  }
}

ui4 bit(Operation op) { return ui4{1} << static_cast<unsigned>(op); }

}  // namespace

const char* operation_name(ui1 code) {
  switch (code) {
    case 0: return "unset";
    case 1: return "create";
    case 2: return "append";
    case 3: return "header-repair";
    case 4: return "recovery";
    case 5: return "metadata-update";
    default: return nullptr;
  }
}

Provenance read(const UniversalHeader& uh) {
  const Region& r = uh.discretionary_region;
  Provenance p;
  if (!has_magic(r)) return p;
  p.present = true;
  p.layout_version = r[LAYOUT_OFFSET];
  p.last_operation = r[LAST_OP_OFFSET];
  p.created_by = read_version(r, CREATED_BY_OFFSET);
  p.last_modified_by = read_version(r, MODIFIED_BY_OFFSET);
  p.operations_mask = read_u32(r, MASK_OFFSET);
  p.modification_count = read_u32(r, COUNT_OFFSET);
  return p;
}

void stamp_created(UniversalHeader& uh, std::string_view version) {
  Region& r = uh.discretionary_region;
  r.fill(0);
  std::copy(std::begin(MAGIC), std::end(MAGIC), r.begin() + MAGIC_OFFSET);
  r[LAYOUT_OFFSET] = LAYOUT_VERSION;
  r[LAST_OP_OFFSET] = static_cast<ui1>(Operation::Create);
  write_version(r, CREATED_BY_OFFSET, version);
  write_version(r, MODIFIED_BY_OFFSET, version);
  write_u32(r, MASK_OFFSET, bit(Operation::Create));
  write_u32(r, COUNT_OFFSET, 0);
}

bool stamp_modified(UniversalHeader& uh, Operation op, std::string_view version) {
  Region& r = uh.discretionary_region;
  if (!has_magic(r)) {
    if (!all_zero(r)) return false;  // another application's region
    std::copy(std::begin(MAGIC), std::end(MAGIC), r.begin() + MAGIC_OFFSET);
    r[LAYOUT_OFFSET] = LAYOUT_VERSION;  // created-by stays empty: not ours
  }
  // A newer layout's version byte is kept: its extra fields live in bytes this
  // code does not touch, and downgrading the byte would hide them.
  r[LAST_OP_OFFSET] = static_cast<ui1>(op);
  write_version(r, MODIFIED_BY_OFFSET, version);
  write_u32(r, MASK_OFFSET, read_u32(r, MASK_OFFSET) | bit(op));
  const ui4 n = read_u32(r, COUNT_OFFSET);
  write_u32(r, COUNT_OFFSET, n == COUNT_MAX ? COUNT_MAX : n + 1);  // saturate
  return true;
}

}  // namespace provenance

}  // namespace mef3io::fmt
