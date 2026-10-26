// mef3io — record (.rdat) parsing.
#include "mef3io/records.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
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
constexpr si8 UNKNOWN_NUMBER_OF_ENTRIES = -1;  // meflib.h:232
constexpr ui4 CRC_NO_ENTRY = 0;                // meflib.h:234

// A record type read from a damaged file can hold any bytes; messages (which
// become Python strings) must stay valid UTF-8, so non-printable-ASCII bytes
// are shown as \xNN.
std::string safe_type(const std::string& t) {
  static const char* hex = "0123456789abcdef";
  std::string o;
  for (unsigned char c : t) {
    if (c >= 0x20 && c < 0x7f) o += static_cast<char>(c);
    else { o += "\\x"; o += hex[c >> 4]; o += hex[c & 15]; }
  }
  return o;
}

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
  RecordParseInfo info;
  auto out = parse_records(rdat, rto, keys, info);
  if (offset_dependent) *offset_dependent = info.offset_dependent;
  return out;
}

std::vector<Record> parse_records(std::span<const ui1> rdat, si8 rto,
                                  const crypto::AccessKeys& keys, RecordParseInfo& info) {
  std::vector<Record> out;
  TimeConv conv{rto};
  if (rdat.size() < static_cast<std::size_t>(fmt::UNIVERSAL_HEADER_BYTES))
    throw FormatError("record file smaller than its universal header (truncated?)");
  // The universal header says how many records follow, and meflib walks
  // exactly that many when it is known (meflib.c:1869). So a file that
  // declares MORE records than it holds is damaged and must FAIL, letting the
  // caller contain it (strict raises, lenient skips and reports) instead of
  // coming back silently short. One that declares FEWER is tolerated as
  // meflib tolerates it: only the declared records are read.
  const si8 declared = read<si8>(rdat, 32);  // number_of_entries
  const bool count_known = declared != UNKNOWN_NUMBER_OF_ENTRIES;
  std::size_t off = fmt::UNIVERSAL_HEADER_BYTES;  // records follow the UH

  // Damage-tolerant walk (lenient readers only): `seen` counts every record
  // position, skipped ones included. A damaged record is listed and the walk
  // goes on at the next offset the index knows, else after its own length
  // when the header is plausible, else it stops (the rest is reported as one
  // damaged tail). Never writes anything.
  std::vector<std::uint64_t> idx;
  if (info.tolerate_damage) {
    for (si8 o : info.index_offsets)
      if (o >= static_cast<si8>(fmt::UNIVERSAL_HEADER_BYTES) && static_cast<std::uint64_t>(o) < rdat.size())
        idx.push_back(static_cast<std::uint64_t>(o));
    std::sort(idx.begin(), idx.end());
    idx.erase(std::unique(idx.begin(), idx.end()), idx.end());
  }
  std::size_t seen = 0;
  // Returns true when the walk resumed at a later record.
  auto skip_damaged = [&](std::size_t at, const std::string& reason, std::optional<std::size_t> own_end) {
    info.damaged.push_back({seen, at, reason});
    ++seen;
    auto it = std::upper_bound(idx.begin(), idx.end(), static_cast<std::uint64_t>(at));
    if (it != idx.end()) { off = static_cast<std::size_t>(*it); return true; }
    if (own_end && idx.empty()) { off = *own_end; return true; }
    return false;
  };

  while (true) {
    if (count_known && static_cast<si8>(seen) == declared) break;
    if (off + fmt::RECORD_HEADER_BYTES > rdat.size()) {
      if (info.tolerate_damage) {
        const bool zeros = std::all_of(rdat.begin() + static_cast<std::ptrdiff_t>(off), rdat.end(),
                                       [](ui1 c) { return c == 0; });
        if (!(zeros && !count_known))
          info.damaged.push_back({seen, off, "record file truncated: " + std::to_string(out.size()) +
                                  " record(s) read" +
                                  (count_known ? " of " + std::to_string(declared) + " declared" : "") +
                                  ", then " + std::to_string(rdat.size() - off) +
                                  " byte(s), less than a record header"});
        break;
      }
      // Out of bytes. Fine only when the count is unknown and nothing but
      // zero padding is left; otherwise a record was cut off.
      const bool clean_end =
          !count_known && std::all_of(rdat.begin() + static_cast<std::ptrdiff_t>(off), rdat.end(),
                                      [](ui1 c) { return c == 0; });
      if (clean_end) break;
      throw FormatError("record file truncated: " + std::to_string(out.size()) +
                        " record(s) read" +
                        (count_known ? " of " + std::to_string(declared) + " declared" : "") +
                        ", then " + std::to_string(rdat.size() - off) +
                        " byte(s), less than a record header");
    }
    // Record header (24 B).
    std::string type = read_string(rdat, off + 4, 4);
    if (type.empty() && !count_known) break;  // zero padding after the last record
    if (type.empty() && info.tolerate_damage) {
      if (skip_damaged(off, "record " + std::to_string(seen) + " has no type code (corrupt)", std::nullopt))
        continue;
      break;
    }
    if (type.empty())
      throw FormatError("record " + std::to_string(out.size()) + " of " +
                        std::to_string(declared) + " declared has no type code (corrupt)");
    const ui4 stored_crc = read<ui4>(rdat, off);
    ui1 vmaj = read<ui1>(rdat, off + 9);
    ui1 vmin = read<ui1>(rdat, off + 10);
    si1 enc = read<si1>(rdat, off + 11);
    ui4 body_bytes = read<ui4>(rdat, off + 12);
    si8 time = read<si8>(rdat, off + 16);

    std::size_t body_off = off + fmt::RECORD_HEADER_BYTES;
    if (body_bytes > rdat.size() - body_off && info.tolerate_damage) {
      if (skip_damaged(off, "record " + std::to_string(seen) + " (" + safe_type(type) + ") claims " +
                                std::to_string(body_bytes) + " body bytes but only " +
                                std::to_string(rdat.size() - body_off) + " remain (corrupt length)",
                       std::nullopt))
        continue;
      break;
    }
    if (body_bytes > rdat.size() - body_off)
      throw FormatError("record " + std::to_string(out.size()) + " claims " +
                        std::to_string(body_bytes) + " body bytes but only " +
                        std::to_string(rdat.size() - body_off) + " remain (truncated?)");
    // Per-record CRC over the stored bytes [4, header + body) — the coverage
    // meflib validates (meflib.c:1839). CRC_NO_ENTRY means "not computed".
    if (stored_crc != CRC_NO_ENTRY &&
        crc::calculate(rdat.subspan(off + 4, fmt::RECORD_HEADER_BYTES - 4 + body_bytes)) !=
            stored_crc) {
      if (info.tolerate_damage) {
        if (skip_damaged(off, "record " + std::to_string(seen) + " (" + safe_type(type) + ") fails its CRC: the record is corrupt",
                         body_off + body_bytes))
          continue;
        break;
      }
      throw CrcError("record " + std::to_string(out.size()) + " (" + safe_type(type) +
                     ") fails its CRC: the record is corrupt");
    }

    // Decrypt the body if encrypted.
    std::vector<ui1> body(rdat.begin() + body_off, rdat.begin() + body_off + body_bytes);
    bool body_locked = false;
    if (enc > 0) {
      std::span<const ui1> key;
      if (enc == fmt::LEVEL_1_ENCRYPTION && keys.level1_key)
        key = *keys.level1_key;
      else if (enc == fmt::LEVEL_2_ENCRYPTION && keys.level2_key)
        key = *keys.level2_key;
      if (key.empty() && !info.skip_locked_bodies)
        throw PasswordError("record body encrypted; insufficient access");
      if (key.empty()) {
        body_locked = true;
      } else {
        // Body is padded to a multiple of 16 for AES.
        std::size_t pad = (body.size() + 15) & ~std::size_t{15};
        body.resize(pad, 0);
        auto dec = crypto::aes128_ecb_decrypt(body, key);
        body = std::move(dec);
      }
    }
    std::span<const ui1> b(body);

    Record rec;
    rec.type = type;
    if (time != fmt::UUTC_NO_ENTRY && time >= 0) info.header_time_non_negative = true;
    rec.time = conv(time);
    rec.version_major = vmaj;
    rec.version_minor = vmin;
    if (body_locked)
      info.locked.push_back(out.size());  // header only: the body stays unread
    else
      parse_body(rec, b, conv);

    out.push_back(std::move(rec));
    ++seen;

    // Advance. Records are laid out contiguously; body may be padded to a
    // 16-byte boundary when the file uses encryption alignment.
    off = body_off + body_bytes;
  }
  info.offset_dependent = conv.offset_dependent;
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

void validate_records_for_write(const std::vector<Record>& records) {
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
}

namespace {

struct EncodedRecord {
  std::vector<ui1> bytes;                  // header + padded (maybe encrypted) body, CRC set
  std::array<ui1, fmt::RECORD_INDEX_BYTES> entry{};  // index entry; offset (8) filled by caller
  si8 time = 0;                            // absolute uUTC
  si8 disk_time = 0;
};

// Encode in the given order. `keys` is used only when `encrypt`.
std::vector<EncodedRecord> encode_records(const std::vector<const Record*>& ordered, si8 rto,
                                          bool encrypt, const crypto::AccessKeys& keys) {
  std::vector<EncodedRecord> out;
  out.reserve(ordered.size());
  for (const Record* rp : ordered) {
    const Record& r = *rp;
    std::vector<ui1> body = record_body(r);
    // Pad body to a 16-byte multiple with PAD_BYTE (also satisfies AES block).
    body.resize((body.size() + 15) & ~std::size_t{15}, PAD_BYTE);
    si1 enc = 0;
    if (encrypt) {
      enc = fmt::LEVEL_2_ENCRYPTION;  // records default to L2 (may hold PII)
      if (!keys.level2_key) throw PasswordError("level-2 key required to encrypt records");
      auto e = crypto::aes128_ecb_encrypt(body, *keys.level2_key);
      std::copy(e.begin(), e.end(), body.begin());
    }
    // mef3io's write convention (time.hpp): -(t - rto); -t at rto 0.
    require_storable_time(r.time, rto, "record time");
    EncodedRecord er;
    er.time = r.time;
    er.disk_time = to_stored_time(r.time, rto);
    er.bytes.assign(fmt::RECORD_HEADER_BYTES, 0);
    write_fixed_code(er.bytes, 4, 4, r.type);
    write<ui1>(er.bytes, 9, static_cast<ui1>(r.version_major));
    write<ui1>(er.bytes, 10, static_cast<ui1>(r.version_minor));
    write<si1>(er.bytes, 11, enc);
    write<ui4>(er.bytes, 12, static_cast<ui4>(body.size()));
    write<si8>(er.bytes, 16, er.disk_time);
    er.bytes.insert(er.bytes.end(), body.begin(), body.end());
    write<ui4>(er.bytes, 0, crc::calculate(std::span<const ui1>(er.bytes).subspan(4)));
    write_fixed_code(er.entry, 0, 4, r.type);
    write<ui1>(er.entry, 5, static_cast<ui1>(r.version_major));
    write<ui1>(er.entry, 6, static_cast<ui1>(r.version_minor));
    write<si1>(er.entry, 7, enc);
    write<si8>(er.entry, 16, er.disk_time);
    out.push_back(std::move(er));
  }
  return out;
}

std::vector<ui1> read_at(const std::string& path, std::uint64_t off, std::size_t n) {
  std::ifstream f(path, std::ios::binary);
  if (!f) throw IoError("cannot open for read: " + path);
  f.seekg(static_cast<std::streamoff>(off));
  std::vector<ui1> buf(n);
  if (!f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(n)))
    throw IoError("short read: " + path);
  return buf;
}

void write_at(const std::string& path, std::uint64_t off, std::span<const ui1> bytes) {
  std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
  if (!f) throw IoError("cannot open for append: " + path);
  f.seekp(static_cast<std::streamoff>(off));
  f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  f.flush();
  if (!f) throw IoError("append failed (disk full?): " + path);
  f.close();
  if (!f) throw IoError("close failed, data may not have reached disk: " + path);
}

constexpr const char* RECOVER_ADVICE =
    " (a write was probably interrupted; run recover_session, which repairs it, and write again)";

}  // namespace

void write_records(const std::string& dir, const std::string& base,
                   const std::string& session_name, const std::string& channel_name,
                   int segment_number, const std::vector<Record>& records, si8 rto,
                   const std::string& password_1, const std::string& password_2) {
  if (records.empty()) return;
  validate_records_for_write(records);
  const std::string rdat_path = (fs::path(dir) / (base + ".rdat")).string();
  const std::string ridx_path = (fs::path(dir) / (base + ".ridx")).string();
  const bool exists = fs::exists(rdat_path);
  // An index with no data file: never truncate it (write_file would), and do
  // not guess. recover_session lists it; the writer refuses.
  if (!exists && fs::exists(ridx_path))
    throw FormatError("record index without its record file: " + ridx_path +
                      " (the .rdat is missing; nothing was written; run recover_session, which "
                      "reports it, and restore the .rdat or move the .ridx away)");
  bool encrypt = !password_1.empty();
  crypto::ValidationFields vf =
      encrypt ? crypto::make_validation_fields(password_1, password_2) : crypto::ValidationFields{};
  crypto::AccessKeys keys;

  // Within ONE call the records go in time order (stable); across calls the
  // file keeps write order, and readers sort.
  std::vector<const Record*> ordered;
  for (const auto& r : records) ordered.push_back(&r);
  std::stable_sort(ordered.begin(), ordered.end(),
                   [](const Record* a, const Record* b) { return a->time < b->time; });

  // ---- APPEND to an existing pair: NEVER truncate, replace or rewrite -------
  if (exists) {
    std::error_code ec;
    const std::uintmax_t rdat_size = fs::file_size(rdat_path, ec);
    if (ec || rdat_size < static_cast<std::uintmax_t>(fmt::UNIVERSAL_HEADER_BYTES))
      throw FormatError("record file is shorter than its header: " + rdat_path + RECOVER_ADVICE);
    if (!fs::exists(ridx_path))
      throw FormatError("record file has no index: " + ridx_path + RECOVER_ADVICE);
    const std::uintmax_t ridx_size = fs::file_size(ridx_path, ec);
    constexpr std::uintmax_t UH = fmt::UNIVERSAL_HEADER_BYTES;
    if (ec || ridx_size < UH || (ridx_size - UH) % fmt::RECORD_INDEX_BYTES != 0)
      throw FormatError("record index is short or torn: " + ridx_path + RECOVER_ADVICE);
    auto rdat_uh_bytes = read_at(rdat_path, 0, UH);
    auto ridx_uh_bytes = read_at(ridx_path, 0, UH);
    auto rdat_uh = fmt::UniversalHeader::parse(rdat_uh_bytes);
    auto ridx_uh = fmt::UniversalHeader::parse(ridx_uh_bytes);
    if (rdat_uh.file_type_string != fmt::FILE_TYPE_RECORD_DATA ||
        ridx_uh.file_type_string != fmt::FILE_TYPE_RECORD_INDICES)
      throw FormatError("not a record file pair: " + rdat_path);
    if (!rdat_uh.header_crc_valid(rdat_uh_bytes) || !ridx_uh.header_crc_valid(ridx_uh_bytes))
      throw CrcError("record file header fails its CRC: " + rdat_path + RECOVER_ADVICE);
    const si8 n_idx = static_cast<si8>((ridx_size - UH) / fmt::RECORD_INDEX_BYTES);
    if ((rdat_uh.number_of_entries != UNKNOWN_NUMBER_OF_ENTRIES && rdat_uh.number_of_entries != n_idx) ||
        (ridx_uh.number_of_entries != UNKNOWN_NUMBER_OF_ENTRIES && ridx_uh.number_of_entries != n_idx))
      throw FormatError("record file header and index disagree on the record count: " + rdat_path +
                        RECOVER_ADVICE);
    // O(1) health check: the last index entry must point at the last record,
    // and that record must end exactly where the file does.
    si8 expect_end = static_cast<si8>(UH);
    if (n_idx > 0) {
      const auto last = read_at(ridx_path, ridx_size - fmt::RECORD_INDEX_BYTES,
                                fmt::RECORD_INDEX_BYTES);
      const si8 off = read<si8>(last, 8);
      if (off < static_cast<si8>(UH) || static_cast<std::uintmax_t>(off) + fmt::RECORD_HEADER_BYTES > rdat_size)
        throw FormatError("record index points outside its record file: " + ridx_path + RECOVER_ADVICE);
      const auto rh = read_at(rdat_path, static_cast<std::uint64_t>(off), fmt::RECORD_HEADER_BYTES);
      expect_end = off + static_cast<si8>(fmt::RECORD_HEADER_BYTES) + static_cast<si8>(read<ui4>(rh, 12));
    }
    if (static_cast<std::uintmax_t>(expect_end) != rdat_size)
      throw FormatError("record file ends in a partial record (or one the index does not list): " +
                        rdat_path + RECOVER_ADVICE);

    // Encryption must match the file (bodies carry their own level byte, but a
    // header without validation fields cannot vouch for an encrypted body).
    if (encrypt != rdat_uh.is_password_protected())
      throw WriteConflictError(
          std::string("record file ") + rdat_path +
          (rdat_uh.is_password_protected() ? " is encrypted but this writer has no password"
                                           : " is not encrypted but this writer has a password"));
    if (encrypt) {
      keys = crypto::validate_password(password_2.empty() ? password_1 : password_2,
                                       rdat_uh.level_1_password_validation_field,
                                       rdat_uh.level_2_password_validation_field);
      if (!keys.level2_key) throw PasswordError("record file: password does not open level 2");
    }

    const auto enc = encode_records(ordered, rto, encrypt, keys);
    std::vector<ui1> new_rdat, new_ridx;
    si8 min_t = fmt::UUTC_NO_ENTRY, max_t = fmt::UUTC_NO_ENTRY;
    ui4 max_entry = 0;
    for (const auto& e : enc) {
      auto entry = e.entry;
      write<si8>(entry, 8, static_cast<si8>(rdat_size) + static_cast<si8>(new_rdat.size()));
      new_rdat.insert(new_rdat.end(), e.bytes.begin(), e.bytes.end());
      new_ridx.insert(new_ridx.end(), entry.begin(), entry.end());
      max_entry = std::max(max_entry, static_cast<ui4>(e.bytes.size()));
      if (e.time != fmt::UUTC_NO_ENTRY) {
        if (min_t == fmt::UUTC_NO_ENTRY || e.time < min_t) min_t = e.time;
        if (max_t == fmt::UUTC_NO_ENTRY || e.time > max_t) max_t = e.time;
      }
    }
    // Span: old (absolute) widened by the new records, stored the way mef3io writes.
    auto widen_span = [&](fmt::UniversalHeader& uh) {
      si8 lo = min_t, hi = max_t;
      if (uh.start_time != fmt::UUTC_NO_ENTRY) {
        const si8 a = to_user_time(uh.start_time, rto);
        lo = lo == fmt::UUTC_NO_ENTRY ? a : std::min(lo, a);
      }
      if (uh.end_time != fmt::UUTC_NO_ENTRY) {
        const si8 b = to_user_time(uh.end_time, rto);
        hi = hi == fmt::UUTC_NO_ENTRY ? b : std::max(hi, b);
      }
      uh.start_time = to_stored_time(lo, rto);
      uh.end_time = to_stored_time(hi, rto);
    };
    const si8 new_n = n_idx + static_cast<si8>(enc.size());
    // Rolling CRC (no re-read). A stored CRC_NO_ENTRY (0: mef_tools never
    // computed it) is no CRC state: extending it would write a checksum the
    // file fails, so it stays "never computed".
    const ui4 rdat_body_crc = rdat_uh.body_crc == fmt::CRC_NO_ENTRY
                                  ? fmt::CRC_NO_ENTRY : crc::calculate(new_rdat, rdat_uh.body_crc);
    const ui4 ridx_body_crc = ridx_uh.body_crc == fmt::CRC_NO_ENTRY
                                  ? fmt::CRC_NO_ENTRY : crc::calculate(new_ridx, ridx_uh.body_crc);
    auto patched = [&](fmt::UniversalHeader uh, std::vector<ui1> head, ui4 body_crc, si8 max_size) {
      uh.number_of_entries = new_n;
      uh.maximum_entry_size = max_size;
      widen_span(uh);
      uh.body_crc = body_crc;
      fmt::provenance::stamp_modified(uh, fmt::provenance::Operation::Append);
      uh.serialize(head);
      uh.update_header_crc(head);
      return head;
    };
    // maximum_entry_size is an allocation contract: a stored NO_ENTRY / 0 /
    // negative is unusable, so measure the existing records (their sizes are
    // the gaps between the index offsets; the index is 24 B per record) rather
    // than describe only the appended ones.
    si8 stored_max = rdat_uh.maximum_entry_size;
    if (stored_max <= 0 && n_idx > 0) {
      const auto idx = read_at(ridx_path, UH, static_cast<std::size_t>(ridx_size - UH));
      std::vector<si8> offs;
      offs.reserve(static_cast<std::size_t>(n_idx) + 1);
      for (si8 i = 0; i < n_idx; ++i)
        offs.push_back(read<si8>(idx, static_cast<std::size_t>(i) * fmt::RECORD_INDEX_BYTES + 8));
      offs.push_back(static_cast<si8>(rdat_size));
      std::sort(offs.begin(), offs.end());
      stored_max = 0;
      for (std::size_t i = 1; i < offs.size(); ++i) stored_max = std::max(stored_max, offs[i] - offs[i - 1]);
    }
    const auto rdat_head = patched(rdat_uh, rdat_uh_bytes, rdat_body_crc,
                                   std::max<si8>(stored_max < 0 ? 0 : stored_max, max_entry));
    const auto ridx_head = patched(ridx_uh, ridx_uh_bytes, ridx_body_crc, fmt::RECORD_INDEX_BYTES);

    // Order: data, the index that points at it, then the headers that count
    // them (like the segment append). A failure puts everything back.
    bool touched_rdat = false, touched_ridx = false, patched_ridx = false;
    try {
      touched_rdat = true;
      write_at(rdat_path, rdat_size, new_rdat);
      touched_ridx = true;
      write_at(ridx_path, ridx_size, new_ridx);
      patched_ridx = true;
      write_at(ridx_path, 0, ridx_head);
      write_at(rdat_path, 0, rdat_head);
    } catch (...) {
      std::error_code ignored;
      try {
        if (patched_ridx) write_at(ridx_path, 0, ridx_uh_bytes);
      } catch (...) {}
      try {
        write_at(rdat_path, 0, rdat_uh_bytes);  // a torn header patch too
      } catch (...) {}
      if (touched_ridx) fs::resize_file(ridx_path, ridx_size, ignored);
      if (touched_rdat) fs::resize_file(rdat_path, rdat_size, ignored);
      throw;
    }
    return;
  }

  // ---- no previous file: create the pair ------------------------------------
  if (encrypt)
    keys = crypto::validate_password(password_2.empty() ? password_1 : password_2, vf.level1,
                                     vf.level2);
  const auto enc = encode_records(ordered, rto, encrypt, keys);
  std::vector<ui1> rdat_body, ridx_body;
  ui4 max_entry = 0;
  si8 min_time = fmt::UUTC_NO_ENTRY, max_time = fmt::UUTC_NO_ENTRY;  // absolute
  for (const auto& e : enc) {
    auto entry = e.entry;
    write<si8>(entry, 8, fmt::UNIVERSAL_HEADER_BYTES + static_cast<si8>(rdat_body.size()));
    rdat_body.insert(rdat_body.end(), e.bytes.begin(), e.bytes.end());
    ridx_body.insert(ridx_body.end(), entry.begin(), entry.end());
    max_entry = std::max(max_entry, static_cast<ui4>(e.bytes.size()));
    if (e.time != fmt::UUTC_NO_ENTRY) {
      if (min_time == fmt::UUTC_NO_ENTRY || e.time < min_time) min_time = e.time;
      if (max_time == fmt::UUTC_NO_ENTRY || e.time > max_time) max_time = e.time;
    }
  }
  const si8 n = static_cast<si8>(enc.size());
  const si8 start_disk = to_stored_time(min_time, rto);
  const si8 end_disk = to_stored_time(max_time, rto);
  {
    auto file = base_uh_bytes(fmt::FILE_TYPE_RECORD_DATA, session_name, channel_name,
                              segment_number, n, max_entry, start_disk, end_disk, vf);
    file.insert(file.end(), rdat_body.begin(), rdat_body.end());
    finalize_crcs(file);
    write_file(rdat_path, file);
  }
  {
    auto file = base_uh_bytes(fmt::FILE_TYPE_RECORD_INDICES, session_name, channel_name,
                              segment_number, n, fmt::RECORD_INDEX_BYTES, start_disk, end_disk, vf);
    file.insert(file.end(), ridx_body.begin(), ridx_body.end());
    finalize_crcs(file);
    write_file(ridx_path, file);
  }
}

}  // namespace mef3io
