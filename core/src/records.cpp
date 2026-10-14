// mef3io — record (.rdat) parsing.
#include "mef3io/records.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <vector>

#include "mef3io/byteio.hpp"
#include "mef3io/crc.hpp"
#include "mef3io/errors.hpp"
#include "mef3io/headers.hpp"
#include "mef3io/time.hpp"

namespace fs = std::filesystem;

namespace mef3io {
namespace {
using byteio::read;
using byteio::read_string;
using byteio::write;
using byteio::write_fixed_code;
using byteio::write_string;

constexpr ui1 PAD_BYTE = 0x7e;

// ---- body layouts (mefrec.h), offsets relative to the start of the BODY ----
// Seiz 1.0 (mefrec.h:110-131)
constexpr std::size_t SEIZ_BYTES = 1312, SEIZ_MARKER_BYTES = 128, SEIZ_ANNOTATION_BYTES = 1024,
                      SEIZ_CHANNEL_BYTES = 272,
                      SEIZ_CHANNEL_NAME_BYTES = 256;  // MEF_BASE_FILE_NAME_BYTES, meflib.h:220
constexpr si8 SEIZ_CHANNEL_TIME_NO_ENTRY = -1;  // mefrec.h:129 / 131
// Epoc 1.0 (mefrec.h:314-323)
constexpr std::size_t EPOC_BYTES = 192, EPOC_TYPE_BYTES = 32, EPOC_TEXT_BYTES = 128;
// Curs 1.0 (mefrec.h:280-287)
constexpr std::size_t CURS_BYTES = 160, CURS_NAME_BYTES = 128;
// CSti 1.0 (mefrec.h:194-202)
constexpr std::size_t CSTI_BYTES = 200, CSTI_STRING_BYTES = 64;
// ESti 1.0 (mefrec.h:227-239)
constexpr std::size_t ESTI_BYTES = 416, ESTI_STRING_BYTES = 128;
// LNTP 1.0 (mefrec.h:64-67)
constexpr std::size_t LNTP_BYTES = 8;

// BODY TIMES. meflib applies the recording-time offset to the record HEADER
// time only (decrypt_records, meflib.c:1846/1880); mefrec.c never rewrites a
// body time. But it DISPLAYS every body time through local_date_time_string
// (mefrec.c:377, 383, 438, 444, 801, 895, 901), which calls
// remove_recording_time_offset (meflib.c:4649 -> 7660): a negative body time
// is read as "offset applied", a non-negative one as absolute. That is
// exactly to_user_time, so body times are converted with the same rule and
// the same offset as the header time:
//   * a non-negative body time — the normal case, even in an offset session,
//     since nothing in meflib negates one — is returned unchanged;
//   * a negative one is resolved with the offset, as meflib shows it.
// So every time in a Record is absolute uUTC whichever convention a writer
// used. Sentinels are not times and pass through (UUTC_NO_ENTRY inside
// to_user_time; Seiz channel onset/offset -1 in TimeConv::seiz_channel).
// Converts on-disk times with one offset and remembers whether any of them
// DEPENDED on it: only a negative (offset-applied) time does, so a file with
// none can be placed even when the offset itself is unknown or disputed.
struct TimeConv {
  si8 rto;
  bool offset_dependent = false;
  si8 operator()(si8 t) {
    if (t != fmt::UUTC_NO_ENTRY && t < 0) offset_dependent = true;
    return to_user_time(t, rto);
  }
  si8 seiz_channel(si8 t) {  // -1 is a sentinel here, not a time (mefrec.h:129/131)
    return t == SEIZ_CHANNEL_TIME_NO_ENTRY ? t : (*this)(t);
  }
};

bool is_v1_0(int vmaj, int vmin) { return vmaj == 1 && vmin == 0; }

// The LNTP template's element type. mefrec.h says si4 (MEFREC_LNTP_1_0_
// TEMPLATE_OFFSET, "// si4"), but pymef writes si8 (pymef3_file.c:4159-4160;
// its reader admits the discrepancy at :3729), and the legacy mef_tools stack
// writes through pymef. The two cannot be told apart by the header, so take
// si8 only when the body is big enough for `n` si8 values AND reading it that
// way is self-consistent: every high word is the sign extension of its low
// word, which a si4 template followed by meflib's 0x7e padding never is.
bool lntp_is_pymef_int64(std::span<const ui1> tmpl, std::size_t n) {
  if (n == 0 || tmpl.size() < n * sizeof(si8)) return false;
  for (std::size_t i = 0; i < n; ++i) {
    const si4 lo = read<si4>(tmpl, i * 8);
    const si4 hi = read<si4>(tmpl, i * 8 + 4);
    if (hi != (lo < 0 ? -1 : 0)) return false;
  }
  return true;
}

// Decode the body of `rec` (already decrypted). meflib decodes EVERY body
// type only at version 1.0 (each show_mefrec_*_type, mefrec.c:201, 232, 304,
// 375, 540, 569, 658, 793, 886), so another version leaves the body fields
// unset; so does a body shorter than its type's fixed part.
void parse_body(Record& rec, std::span<const ui1> b, TimeConv& conv) {
  if (!is_v1_0(rec.version_major, rec.version_minor)) return;
  const std::string& type = rec.type;
  if (type == "Note" || type == "SyLg") {
    rec.text = read_string(b, 0, b.size());
  } else if (type == "EDFA") {
    if (b.size() < 8) return;  // no complete duration: neither field is there
    rec.duration = read<si8>(b, 0);
    rec.text = read_string(b, 8, b.size() - 8);
  } else if (type == "Seiz") {
    if (b.size() < SEIZ_BYTES) return;
    rec.earliest_onset = conv(read<si8>(b, 0));
    rec.latest_offset = conv(read<si8>(b, 8));
    rec.duration = read<si8>(b, 16);
    rec.number_of_channels = read<si4>(b, 24);
    rec.onset_code = read<si4>(b, 28);
    rec.marker_name_1 = read_string(b, 32, SEIZ_MARKER_BYTES);
    rec.marker_name_2 = read_string(b, 160, SEIZ_MARKER_BYTES);
    rec.annotation = read_string(b, 288, SEIZ_ANNOTATION_BYTES);
    // number_of_channels is a declaration: never read past the body for it.
    const std::size_t room = (b.size() - SEIZ_BYTES) / SEIZ_CHANNEL_BYTES;
    const std::size_t n =
        std::min(room, static_cast<std::size_t>(std::max<si4>(*rec.number_of_channels, 0)));
    for (std::size_t i = 0; i < n; ++i) {
      const std::size_t c = SEIZ_BYTES + i * SEIZ_CHANNEL_BYTES;
      rec.seizure_channels.push_back(SeizureChannel{
          read_string(b, c, SEIZ_CHANNEL_NAME_BYTES), conv.seiz_channel(read<si8>(b, c + 256)),
          conv.seiz_channel(read<si8>(b, c + 264))});
    }
  } else if (type == "Epoc") {
    if (b.size() < EPOC_BYTES) return;
    rec.id_number = read<si8>(b, 0);
    rec.timestamp = conv(read<si8>(b, 8));
    rec.end_timestamp = conv(read<si8>(b, 16));
    rec.duration = read<si8>(b, 24);
    rec.epoch_type = read_string(b, 32, EPOC_TYPE_BYTES);
    rec.text = read_string(b, 64, EPOC_TEXT_BYTES);
  } else if (type == "Curs") {
    if (b.size() < CURS_BYTES) return;
    rec.id_number = read<si8>(b, 0);
    rec.trace_timestamp = conv(read<si8>(b, 8));
    rec.latency = read<si8>(b, 16);
    rec.value = read<sf8>(b, 24);
    rec.name = read_string(b, 32, CURS_NAME_BYTES);
  } else if (type == "CSti") {
    if (b.size() < CSTI_BYTES) return;
    rec.task_type = read_string(b, 0, CSTI_STRING_BYTES);
    rec.stimulus_duration = read<si8>(b, 64);
    rec.stimulus_type = read_string(b, 72, CSTI_STRING_BYTES);
    rec.patient_response = read_string(b, 136, CSTI_STRING_BYTES);
  } else if (type == "ESti") {
    if (b.size() < ESTI_BYTES) return;
    rec.amplitude = read<sf8>(b, 0);
    rec.frequency = read<sf8>(b, 8);
    rec.pulse_width = read<si8>(b, 16);
    rec.ampunit_code = read<si4>(b, 24);
    rec.mode_code = read<si4>(b, 28);
    rec.waveform = read_string(b, 32, ESTI_STRING_BYTES);
    rec.anode = read_string(b, 160, ESTI_STRING_BYTES);
    rec.catode = read_string(b, 288, ESTI_STRING_BYTES);
  } else if (type == "LNTP") {
    // The template starts at body offset 8, per the MEFREC_LNTP_1_0_TEMPLATE_
    // OFFSET define. (meflib's own display, mefrec.c:306, computes it with
    // si4-pointer arithmetic and so looks 4x too far; the define is the
    // layout, the display is the bug.) Element width: see lntp_is_pymef_int64.
    if (b.size() < LNTP_BYTES) return;
    rec.template_length = read<si8>(b, 0);
    const std::span<const ui1> tmpl = b.subspan(LNTP_BYTES);
    const std::size_t declared = static_cast<std::size_t>(std::max<si8>(*rec.template_length, 0));
    const bool wide = lntp_is_pymef_int64(tmpl, declared);
    const std::size_t width = wide ? sizeof(si8) : sizeof(si4);
    const std::size_t n = std::min(tmpl.size() / width, declared);  // bounded by the body
    rec.line_noise_template.reserve(n);
    for (std::size_t i = 0; i < n; ++i)
      rec.line_noise_template.push_back(read<si4>(tmpl, i * width));  // low word when wide
  }
  // UnRc and unknown types: no defined body.
}
}  // namespace

const char* record_level_name(RecordLevel level) {
  switch (level) {
    case RecordLevel::Session: return "session";
    case RecordLevel::Channel: return "channel";
    case RecordLevel::Segment: return "segment";
  }
  return "session";
}

std::vector<RecordField> record_fields(const Record& r) {
  std::vector<RecordField> f;
  // si4 fields are reported as integers like every other count/code.
  auto widen = [](const std::optional<si4>& v) {
    return v ? std::optional<si8>(*v) : std::optional<si8>();
  };
  auto add = [&](const char* name, const auto& opt) {
    RecordField field{name, std::nullopt};
    if (opt) field.value = RecordField::Value(*opt);
    f.push_back(std::move(field));
  };
  // A list is only meaningful when the struct it belongs to was decoded;
  // `decoded` says whether it was, so an absent body reports None, not [].
  auto add_list = [&](const char* name, bool decoded, const auto& list) {
    RecordField field{name, std::nullopt};
    if (decoded) field.value = RecordField::Value(list);
    f.push_back(std::move(field));
  };
  const std::string& t = r.type;
  if (t == "Note" || t == "SyLg") {
    add("text", r.text);
  } else if (t == "EDFA") {
    add("duration", r.duration);
    add("text", r.text);
  } else if (t == "Seiz") {
    add("earliest_onset", r.earliest_onset);
    add("latest_offset", r.latest_offset);
    add("duration", r.duration);
    add("number_of_channels", widen(r.number_of_channels));
    add("onset_code", widen(r.onset_code));
    add("marker_name_1", r.marker_name_1);
    add("marker_name_2", r.marker_name_2);
    add("annotation", r.annotation);
    add_list("channels", r.number_of_channels.has_value(), r.seizure_channels);
  } else if (t == "Epoc") {
    add("id_number", r.id_number);
    add("timestamp", r.timestamp);
    add("end_timestamp", r.end_timestamp);
    add("duration", r.duration);
    add("epoch_type", r.epoch_type);
    add("text", r.text);
  } else if (t == "Curs") {
    add("id_number", r.id_number);
    add("trace_timestamp", r.trace_timestamp);
    add("latency", r.latency);
    add("value", r.value);
    add("name", r.name);
  } else if (t == "CSti") {
    add("task_type", r.task_type);
    add("stimulus_duration", r.stimulus_duration);
    add("stimulus_type", r.stimulus_type);
    add("patient_response", r.patient_response);
  } else if (t == "ESti") {
    add("amplitude", r.amplitude);
    add("frequency", r.frequency);
    add("pulse_width", r.pulse_width);
    add("ampunit_code", widen(r.ampunit_code));
    add("mode_code", widen(r.mode_code));
    add("waveform", r.waveform);
    add("anode", r.anode);
    add("catode", r.catode);
  } else if (t == "LNTP") {
    add("length", r.template_length);
    add_list("template", r.template_length.has_value(), r.line_noise_template);
  }
  return f;
}

std::vector<Record> parse_records(std::span<const ui1> rdat, si8 rto,
                                  const crypto::AccessKeys& keys, bool* offset_dependent) {
  std::vector<Record> out;
  TimeConv conv{rto};
  std::size_t off = fmt::UNIVERSAL_HEADER_BYTES;  // records follow the UH

  while (off + fmt::RECORD_HEADER_BYTES <= rdat.size()) {
    // Record header (24 B).
    std::string type = read_string(rdat, off + 4, 4);
    if (type.empty()) break;  // padding / end
    ui1 vmaj = read<ui1>(rdat, off + 9);
    ui1 vmin = read<ui1>(rdat, off + 10);
    si1 enc = read<si1>(rdat, off + 11);
    ui4 body_bytes = read<ui4>(rdat, off + 12);
    si8 time = read<si8>(rdat, off + 16);

    std::size_t body_off = off + fmt::RECORD_HEADER_BYTES;
    if (body_off + body_bytes > rdat.size()) break;  // truncated

    // Decrypt the body if encrypted.
    std::vector<ui1> body(rdat.begin() + body_off, rdat.begin() + body_off + body_bytes);
    if (enc > 0) {
      std::span<const ui1> key;
      if (enc == fmt::LEVEL_1_ENCRYPTION && keys.level1_key)
        key = *keys.level1_key;
      else if (enc == fmt::LEVEL_2_ENCRYPTION && keys.level2_key)
        key = *keys.level2_key;
      if (key.empty()) throw PasswordError("record body encrypted; insufficient access");
      // Body is padded to a multiple of 16 for AES.
      std::size_t pad = (body.size() + 15) & ~std::size_t{15};
      body.resize(pad, 0);
      auto dec = crypto::aes128_ecb_decrypt(body, key);
      body = std::move(dec);
    }
    std::span<const ui1> b(body);

    Record rec;
    rec.type = type;
    rec.time = conv(time);
    rec.version_major = vmaj;
    rec.version_minor = vmin;
    parse_body(rec, b, conv);

    out.push_back(std::move(rec));

    // Advance. Records are laid out contiguously; body may be padded to a
    // 16-byte boundary when the file uses encryption alignment.
    std::size_t advance = fmt::RECORD_HEADER_BYTES + body_bytes;
    off += advance;
  }
  if (offset_dependent) *offset_dependent = conv.offset_dependent;
  return out;
}

namespace {

// Build the body bytes for a record (before padding/encryption).
std::vector<ui1> record_body(const Record& r) {
  std::vector<ui1> body;
  auto put_str = [&](const std::string& s) {
    body.insert(body.end(), s.begin(), s.end());
    body.push_back(0);  // null terminator
  };
  if (r.type == "Note" || r.type == "SyLg") {
    put_str(r.text.value_or(""));
  } else if (r.type == "EDFA") {
    body.resize(8);
    write<si8>(body, 0, r.duration.value_or(0));
    std::string ann = r.text.value_or("");
    body.insert(body.end(), ann.begin(), ann.end());
    body.push_back(0);
  } else {
    // Unknown/unsupported type: empty body.
  }
  return body;
}

void finalize_crcs(std::vector<ui1>& file) {
  ui4 body_crc = crc::calculate(std::span<const ui1>(file).subspan(fmt::UNIVERSAL_HEADER_BYTES));
  write<ui4>(file, 4, body_crc);
  ui4 header_crc =
      crc::calculate(std::span<const ui1>(file).subspan(4, fmt::UNIVERSAL_HEADER_BYTES - 4));
  write<ui4>(file, 0, header_crc);
}

std::vector<ui1> base_uh_bytes(const std::string& ftype, const std::string& session_name,
                               const std::string& channel_name, int segment_number, si8 n_entries,
                               si8 max_entry, si8 start_disk, si8 end_disk,
                               const crypto::ValidationFields& vf) {
  fmt::UniversalHeader uh;
  uh.file_type_string = ftype;
  uh.start_time = start_disk;
  uh.end_time = end_disk;
  uh.number_of_entries = n_entries;
  uh.maximum_entry_size = max_entry;
  uh.segment_number = segment_number;
  uh.session_name = session_name;
  uh.channel_name = channel_name;
  std::copy(vf.level1.begin(), vf.level1.end(), uh.level_1_password_validation_field.begin());
  std::copy(vf.level2.begin(), vf.level2.end(), uh.level_2_password_validation_field.begin());
  fmt::provenance::stamp_created(uh);
  std::vector<ui1> buf(fmt::UNIVERSAL_HEADER_BYTES);
  uh.serialize(buf);
  return buf;
}

void write_file(const std::string& path, const std::vector<ui1>& bytes) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) throw IoError("cannot open for write: " + path);
  f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

}  // namespace

void write_records(const std::string& dir, const std::string& base,
                   const std::string& session_name, const std::string& channel_name,
                   int segment_number, const std::vector<Record>& records, si8 rto,
                   const std::string& password_1, const std::string& password_2) {
  if (records.empty()) return;
  // Reject bad type codes before touching the filesystem, so a rejected batch
  // never leaves a half-written .rdat/.ridx pair behind. Any 4-character code
  // is allowed through: types we build no body for still round-trip as an
  // empty-bodied record, which is how unknown MEF record types pass through.
  for (const auto& r : records) {
    // The field is 4 raw bytes, so the width is measured in bytes, not in
    // characters: a 4-character non-ASCII code ("Nöte") is 5 bytes and would
    // not fit, and a code that happens to be 4 bytes of UTF-8 ("Nöt") is not a
    // MEF type tag either. Say bytes and require ASCII so the message and the
    // check agree.
    const bool ascii = std::all_of(r.type.begin(), r.type.end(),
                                   [](unsigned char c) { return c >= 0x20 && c <= 0x7E; });
    if (r.type.size() != RECORD_TYPE_BYTES || !ascii)
      throw FormatError("record type must be exactly " + std::to_string(RECORD_TYPE_BYTES) +
                        " ASCII bytes (e.g. \"Note\", \"EDFA\", \"SyLg\", \"Seiz\"); got \"" +
                        r.type + "\" (" + std::to_string(r.type.size()) + " bytes)");
    // A 4-character code is well-formed but still says nothing about which
    // payload the body layout can hold, and record_body drops whatever it has
    // no slot for. That is the same silent loss as a mis-width type, just
    // reached by a misspelling ("note") or a type whose body we do not build
    // (Seiz), so refuse the write rather than store a record missing its
    // payload. Empty text is not a payload -- the Python helper defaults it.
    const bool stores_text = r.type == "Note" || r.type == "SyLg" || r.type == "EDFA";
    const bool stores_duration = r.type == "EDFA";
    if (!stores_text && r.text && !r.text->empty())
      throw FormatError("record type \"" + r.type +
                        "\" stores no text (only \"Note\", \"SyLg\" and \"EDFA\" do), but text "
                        "was given; it would be silently dropped");
    if (!stores_duration && r.duration)
      throw FormatError("record type \"" + r.type +
                        "\" stores no duration (only \"EDFA\" does), but duration was given; "
                        "it would be silently dropped");
  }
  const bool encrypt = !password_1.empty();
  crypto::ValidationFields vf =
      encrypt ? crypto::make_validation_fields(password_1, password_2) : crypto::ValidationFields{};
  crypto::AccessKeys keys;
  if (encrypt)
    keys = crypto::validate_password(password_2.empty() ? password_1 : password_2, vf.level1,
                                     vf.level2);

  // Records are time-ordered on disk.
  std::vector<const Record*> ordered;
  for (const auto& r : records) ordered.push_back(&r);
  std::sort(ordered.begin(), ordered.end(),
            [](const Record* a, const Record* b) { return a->time < b->time; });

  std::vector<ui1> rdat_body;
  std::vector<ui1> ridx_body;
  ui4 max_entry = 0;
  si8 start_disk = fmt::UUTC_NO_ENTRY, end_disk = fmt::UUTC_NO_ENTRY;

  for (const Record* rp : ordered) {
    const Record& r = *rp;
    std::vector<ui1> body = record_body(r);
    // Pad body to a 16-byte multiple with PAD_BYTE (also satisfies AES block).
    std::size_t padded = (body.size() + 15) & ~std::size_t{15};
    body.resize(padded, PAD_BYTE);

    si1 enc = 0;
    if (encrypt) {
      enc = fmt::LEVEL_2_ENCRYPTION;  // records default to L2 (may hold PII)
      if (!keys.level2_key) throw PasswordError("level-2 key required to encrypt records");
      auto e = crypto::aes128_ecb_encrypt(body, *keys.level2_key);
      std::copy(e.begin(), e.end(), body.begin());
    }

    si8 disk_time = to_disk_time(r.time, rto);
    si8 file_offset = fmt::UNIVERSAL_HEADER_BYTES + static_cast<si8>(rdat_body.size());

    // Record header (24 B).
    std::vector<ui1> hdr(fmt::RECORD_HEADER_BYTES, 0);
    write_fixed_code(hdr, 4, 4, r.type);
    write<ui1>(hdr, 9, static_cast<ui1>(r.version_major));
    write<ui1>(hdr, 10, static_cast<ui1>(r.version_minor));
    write<si1>(hdr, 11, enc);
    write<ui4>(hdr, 12, static_cast<ui4>(body.size()));
    write<si8>(hdr, 16, disk_time);

    // Assemble the record, then CRC over [4, end).
    std::vector<ui1> rec;
    rec.insert(rec.end(), hdr.begin(), hdr.end());
    rec.insert(rec.end(), body.begin(), body.end());
    ui4 rec_crc = crc::calculate(std::span<const ui1>(rec).subspan(4));
    write<ui4>(rec, 0, rec_crc);

    max_entry = std::max(max_entry, static_cast<ui4>(rec.size()));
    rdat_body.insert(rdat_body.end(), rec.begin(), rec.end());

    // Record index entry (24 B): type[4], vmaj@5, vmin@6, enc@7, offset@8, time@16.
    std::vector<ui1> ie(fmt::RECORD_INDEX_BYTES, 0);
    write_fixed_code(ie, 0, 4, r.type);
    write<ui1>(ie, 5, static_cast<ui1>(r.version_major));
    write<ui1>(ie, 6, static_cast<ui1>(r.version_minor));
    write<si1>(ie, 7, enc);
    write<si8>(ie, 8, file_offset);
    write<si8>(ie, 16, disk_time);
    ridx_body.insert(ridx_body.end(), ie.begin(), ie.end());

    if (start_disk == fmt::UUTC_NO_ENTRY || disk_time > start_disk) start_disk = disk_time;
    if (end_disk == fmt::UUTC_NO_ENTRY || disk_time < end_disk) end_disk = disk_time;
  }

  const si8 n = static_cast<si8>(ordered.size());
  // .rdat
  {
    auto file = base_uh_bytes(fmt::FILE_TYPE_RECORD_DATA, session_name, channel_name,
                              segment_number, n, max_entry, start_disk, end_disk, vf);
    file.insert(file.end(), rdat_body.begin(), rdat_body.end());
    finalize_crcs(file);
    write_file((fs::path(dir) / (base + ".rdat")).string(), file);
  }
  // .ridx
  {
    auto file = base_uh_bytes(fmt::FILE_TYPE_RECORD_INDICES, session_name, channel_name,
                              segment_number, n, fmt::RECORD_INDEX_BYTES, start_disk, end_disk, vf);
    file.insert(file.end(), ridx_body.begin(), ridx_body.end());
    finalize_crcs(file);
    write_file((fs::path(dir) / (base + ".ridx")).string(), file);
  }
}

}  // namespace mef3io
