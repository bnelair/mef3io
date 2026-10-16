// mef3io — anonymize_session and verify_anonymized (#70). See anonymize.hpp.
//
// Format facts this file relies on (settled against meflib and mef3io):
//   * every time is stored meflib-style: negative = offset applied,
//     -(t - rto); non-negative = absolute (time.hpp, meflib.c:744 / 7660).
//     A shifted time keeps the SIGN it came in with, and the copy's rto is 0;
//   * a RED block's CRC covers bytes 4..block_bytes, start time at +40
//     (meflib.c:7045, meflib.h:1049); only the first 16 statistics bytes are
//     ciphertext (meflib.c:292 / 6676 / 7027, red.hpp);
//   * metadata sections 2 and 3 are AES-ECB as a whole, section 2 with the
//     level-1 key and section 3 with the level-2 key (docs/encryption_model.md);
//   * `.tmet` is a fixed 16384-byte record: its body CRC is bounded by that
//     size, never taken to EOF, and section 2 is edited as a STORED IMAGE —
//     never re-serialized, which would rewrite fields this code does not own;
//   * record bodies are AES-ECB over `bytes / 16` blocks (meflib.c:2246); the
//     record CRC covers bytes 4..24+bytes (meflib.c:1839); body layouts per
//     mefrec.h (offsets below);
//   * session-level records are `<session>.mefd/<session>.rdat`
//     (meflib.c:6335), so a renamed copy renames them too.
#include "mef3io/anonymize.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <random>
#include <set>
#include <stdexcept>

#include "durability.hpp"
#include "segment_copy.hpp"
#include "session_layout.hpp"

#include "mef3io/byteio.hpp"
#include "mef3io/crc.hpp"
#include "mef3io/crypto.hpp"
#include "mef3io/errors.hpp"
#include "mef3io/headers.hpp"
#include "mef3io/inspect.hpp"
#include "mef3io/metadata.hpp"
#include "mef3io/red.hpp"
#include "mef3io/source.hpp"
#include "mef3io/time.hpp"
#include "mef3io/validate.hpp"

namespace fs = std::filesystem;

namespace mef3io {
namespace {

using byteio::read;
using byteio::read_string;
using byteio::write;
using byteio::write_string;

constexpr std::size_t UH = fmt::UNIVERSAL_HEADER_BYTES;
constexpr std::size_t S1 = fmt::METADATA_SECTION_1_OFFSET;
constexpr std::size_t S2 = fmt::METADATA_SECTION_2_OFFSET;
constexpr std::size_t S3 = fmt::METADATA_SECTION_3_OFFSET;
constexpr std::size_t S1_BYTES = fmt::METADATA_SECTION_1_BYTES;
constexpr std::size_t S2_BYTES = fmt::TIME_SERIES_METADATA_SECTION_2_BYTES;
constexpr std::size_t S3_BYTES = fmt::METADATA_SECTION_3_BYTES;
constexpr std::size_t TMET = fmt::METADATA_FILE_BYTES;
constexpr std::size_t ENTRY = fmt::TIME_SERIES_INDEX_BYTES;
constexpr std::size_t RHDR = fmt::RECORD_HEADER_BYTES;
constexpr std::size_t RIDX = fmt::RECORD_INDEX_BYTES;
constexpr si8 DAY_US = 86'400'000'000LL;
constexpr ui1 PAD_BYTE = 0x7e;
constexpr ui1 ENC_MASK =
    fmt::RedBlockHeader::LEVEL_1_ENCRYPTION_MASK | fmt::RedBlockHeader::LEVEL_2_ENCRYPTION_MASK;

// Universal-header field offsets (meflib.h:330-362).
constexpr std::size_t UH_START = 16, UH_END = 24, UH_SESSION_NAME = 308, UH_ANON_NAME = 564,
                      UH_NAME_BYTES = 256, UH_LEVEL_UUID = 820, UH_FILE_UUID = 836,
                      UH_PROV_UUID = 852, UH_L1_FIELD = 868, UH_L2_FIELD = 884,
                      UH_PROTECTED = 900, UH_PROTECTED_BYTES = 60;
// Section 2 / 3 (section-relative): text fields and the regions meflib
// reserves (protected 6432 + discretionary 8592 in s2; 924.. in s3).
constexpr std::size_t S2_CHANNEL_DESC = 0, S2_SESSION_DESC = 2048, S2_REFERENCE_DESC = 4104,
                      S2_DESC_BYTES = 2048, S2_UNITS = 6208, S2_UNITS_BYTES = 128,
                      S2_REGIONS = 6432;
constexpr std::size_t S3_RTO = 0, S3_DST_START = 8, S3_DST_END = 16, S3_GMT = 24,
                      S3_NAME1 = 28, S3_NAME2 = 156, S3_ID = 284, S3_SUBJECT_BYTES = 128,
                      S3_LOCATION = 412, S3_LOCATION_BYTES = 512, S3_REGIONS = 924;

using Uuid = std::array<ui1, 16>;

Uuid random_uuid() {
  static thread_local std::random_device rd;
  Uuid u{};
  for (std::size_t i = 0; i < u.size(); i += 4) {
    const unsigned v = rd();
    for (std::size_t k = 0; k < 4; ++k) u[i + k] = static_cast<ui1>((v >> (8 * k)) & 0xFF);
  }
  u[6] = static_cast<ui1>((u[6] & 0x0F) | 0x40);  // RFC 4122 version 4
  u[8] = static_cast<ui1>((u[8] & 0x3F) | 0x80);
  return u;
}

bool all_zero(std::span<const ui1> b) {
  return std::all_of(b.begin(), b.end(), [](ui1 x) { return x == 0; });
}

Uuid uuid_at(std::span<const ui1> b, std::size_t off) {
  Uuid u{};
  std::copy_n(b.begin() + static_cast<std::ptrdiff_t>(off), 16, u.begin());
  return u;
}

// ---------------------------------------------------------------------------
// Times.
// ---------------------------------------------------------------------------

/// Not a time: nothing to shift, and nothing that can leak. UUTC_NO_ENTRY is
/// meflib's sentinel; 0 is "unset" in practice (1970-01-01 dates nothing);
/// -1 is the documented NO_ENTRY of record body times (mefrec.h:129/131).
bool is_sentinel(si8 s, bool body) {
  return s == fmt::UUTC_NO_ENTRY || s == 0 || (body && s == -1);
}

/// The one shifting rule. `s` is a stored value; the result is stored the
/// same way (sign kept) against offset 0.
struct Shifter {
  si8 rto = 0;    // the SOURCE's recording-time offset
  si8 shift = 0;

  /// Absolute uUTC of a stored value (not a sentinel); throws when it cannot
  /// be told apart from a relative one.
  si8 absolute(si8 s, const std::string& what) const {
    if (s < 0) {
      if (s == fmt::UUTC_NO_ENTRY) throw std::logic_error("sentinel");
      const si8 r = effective_rto(rto);
      if (r > 0 && -s > INT64_MAX - r)
        throw FormatError(what + ": stored time " + std::to_string(s) + " overflows");
      return -s + r;
    }
    if (rto != 0 && s <= rto)
      throw WriteConflictError(
          what + " is stored non-negative (" + std::to_string(s) +
          ") at or before the session's recording-time offset. Such a value cannot be told "
          "apart from a time stored relative to the offset (the legacy convention, "
          "docs/anonymization.md), so where it lies is unknown and it is not copied.");
    return s;
  }

  si8 operator()(si8 s, const std::string& what, bool body = false) const {
    if (is_sentinel(s, body)) return s;
    const si8 a = absolute(s, what);
    if ((shift > 0 && a > INT64_MAX - shift) || (shift < 0 && a + shift <= 0))
      throw std::invalid_argument("the shift moves " + what + " (" + std::to_string(a) +
                                  " uUTC) to 0 or below, or out of range; shifted times must "
                                  "stay above 0");
    const si8 n = a + shift;
    return s < 0 ? -n : n;
  }
};

// ---------------------------------------------------------------------------
// Text: word-bounded, ASCII-case-insensitive search and replace of the names
// a copy must not carry.
// ---------------------------------------------------------------------------

bool is_word(ui1 c) { return c >= 0x80 || std::isalnum(c) || c == '_'; }
char fold(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

struct Redactor {
  struct Rule {
    std::string needle;       // lower-cased
    std::string replacement;
    std::string label;        // what it is, for reports (never the value)
  };
  std::vector<Rule> rules;

  void add(const std::string& value, const std::string& replacement, const std::string& label) {
    // One- and zero-byte values would match nearly any text.
    if (value.size() < 2) return;
    std::string n(value);
    std::transform(n.begin(), n.end(), n.begin(), fold);
    if (n == [&] {
          std::string r(replacement);
          std::transform(r.begin(), r.end(), r.begin(), fold);
          return r;
        }())
      return;
    for (const auto& r : rules)
      if (r.needle == n) return;
    rules.push_back({n, replacement, label});
    // Longest first, so "Jane Doe" is replaced before "Jane".
    std::stable_sort(rules.begin(), rules.end(),
                     [](const Rule& a, const Rule& b) { return a.needle.size() > b.needle.size(); });
  }

  /// Position of the first word-bounded match of `rule` in `text` at or after
  /// `from`, or npos.
  static std::size_t find(const std::string& text, const std::string& needle, std::size_t from) {
    if (needle.empty() || text.size() < needle.size()) return std::string::npos;
    for (std::size_t i = from; i + needle.size() <= text.size(); ++i) {
      bool eq = true;
      for (std::size_t k = 0; k < needle.size() && eq; ++k) eq = fold(text[i + k]) == needle[k];
      if (!eq) continue;
      const bool left = i == 0 || !is_word(static_cast<ui1>(text[i - 1]));
      const bool right =
          i + needle.size() == text.size() || !is_word(static_cast<ui1>(text[i + needle.size()]));
      if (left && right) return i;
    }
    return std::string::npos;
  }

  std::string apply(std::string text) const {
    for (const auto& r : rules) {
      std::size_t pos = 0;
      while ((pos = find(text, r.needle, pos)) != std::string::npos) {
        text.replace(pos, r.needle.size(), r.replacement);
        pos += r.replacement.size();
      }
    }
    return text;
  }

  /// The label of the first rule that matches, or "".
  std::string match(const std::string& text) const {
    for (const auto& r : rules)
      if (find(text, r.needle, 0) != std::string::npos) return r.label;
    return {};
  }
};

// ---------------------------------------------------------------------------
// Record files, raw.
// ---------------------------------------------------------------------------

struct RawRecord {
  std::array<ui1, 4> type_bytes{};
  std::string type;
  ui1 vmaj = 0, vmin = 0;
  si1 enc = 0;
  si8 time = 0;
  std::vector<ui1> body;  // decrypted; stored length
};

struct RawRecordFile {
  std::vector<ui1> uh;                  // 1024 bytes
  std::vector<RawRecord> records;
  std::vector<si8> record_offsets;      // file-relative
  bool has_ridx = false;
  std::vector<ui1> ridx_uh;
  std::vector<std::array<ui1, RIDX>> ridx;
  crypto::AccessKeys keys;
};

std::span<const ui1> key_for(const crypto::AccessKeys& keys, int level) {
  if (level == fmt::LEVEL_1_ENCRYPTION && keys.level1_key) return *keys.level1_key;
  if (level == fmt::LEVEL_2_ENCRYPTION && keys.level2_key) return *keys.level2_key;
  return {};
}

std::string ridx_of(const std::string& rdat) { return rdat.substr(0, rdat.size() - 5) + ".ridx"; }

RawRecordFile read_record_file(const SessionSource& src, const std::string& rdat_rel,
                               const std::string& password) {
  RawRecordFile f;
  const auto bytes = src.read_all(rdat_rel);
  const std::string where = src.describe(rdat_rel);
  if (bytes.size() < UH) throw FormatError("record file smaller than its header: " + where);
  f.uh.assign(bytes.begin(), bytes.begin() + UH);
  f.keys = crypto::validate_password(password, std::span<const ui1>(bytes).subspan(UH_L1_FIELD, 16),
                                     std::span<const ui1>(bytes).subspan(UH_L2_FIELD, 16));
  const si8 declared = read<si8>(bytes, 32);
  const bool known = declared != -1;
  std::size_t off = UH;
  while (true) {
    if (known && static_cast<si8>(f.records.size()) == declared) break;
    if (off + RHDR > bytes.size()) {
      const bool clean = !known && all_zero(std::span<const ui1>(bytes).subspan(off));
      if (clean) break;
      throw FormatError("record file truncated after " + std::to_string(f.records.size()) +
                        " record(s): " + where);
    }
    RawRecord r;
    std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(off + 4), 4, r.type_bytes.begin());
    r.type = read_string(bytes, off + 4, 4);
    if (r.type.empty() && !known) break;
    if (r.type.empty()) throw FormatError("record without a type code in " + where);
    const ui4 stored_crc = read<ui4>(bytes, off);
    r.vmaj = read<ui1>(bytes, off + 9);
    r.vmin = read<ui1>(bytes, off + 10);
    r.enc = read<si1>(bytes, off + 11);
    const ui4 nb = read<ui4>(bytes, off + 12);
    r.time = read<si8>(bytes, off + 16);
    const std::size_t body_off = off + RHDR;
    if (nb > bytes.size() - body_off)
      throw FormatError("record " + std::to_string(f.records.size()) +
                        " runs past the end of " + where);
    if (stored_crc != fmt::CRC_NO_ENTRY &&
        crc::calculate(std::span<const ui1>(bytes).subspan(off + 4, RHDR - 4 + nb)) != stored_crc)
      throw CrcError("record " + std::to_string(f.records.size()) + " (" + r.type +
                     ") fails its CRC: " + where);
    r.body.assign(bytes.begin() + static_cast<std::ptrdiff_t>(body_off),
                  bytes.begin() + static_cast<std::ptrdiff_t>(body_off + nb));
    if (r.enc > 0) {
      const auto key = key_for(f.keys, r.enc);
      if (key.empty())
        throw PasswordError("a record body in " + where +
                            " is encrypted and the password does not open it (the level-2 "
                            "password opens every record)");
      const std::size_t whole = r.body.size() / 16 * 16;  // meflib: bytes / 16 blocks
      auto dec = crypto::aes128_ecb_decrypt(std::span<const ui1>(r.body).first(whole), key);
      std::copy(dec.begin(), dec.end(), r.body.begin());
    }
    f.record_offsets.push_back(static_cast<si8>(off));
    f.records.push_back(std::move(r));
    off = body_off + nb;
  }
  const std::string ridx_rel = ridx_of(rdat_rel);
  if (src.exists(ridx_rel)) {
    const auto ib = src.read_all(ridx_rel);
    if (ib.size() < UH || (ib.size() - UH) % RIDX != 0)
      throw FormatError("record index has a partial entry: " + src.describe(ridx_rel));
    f.has_ridx = true;
    f.ridx_uh.assign(ib.begin(), ib.begin() + UH);
    for (std::size_t o = UH; o + RIDX <= ib.size(); o += RIDX) {
      std::array<ui1, RIDX> e{};
      std::copy_n(ib.begin() + static_cast<std::ptrdiff_t>(o), RIDX, e.begin());
      f.ridx.push_back(e);
    }
    if (f.ridx.size() != f.records.size())
      throw FormatError("record index " + src.describe(ridx_rel) + " lists " +
                        std::to_string(f.ridx.size()) + " record(s) but " + where + " holds " +
                        std::to_string(f.records.size()) +
                        "; which one is right is unknown, so neither is copied");
  }
  return f;
}

// Record body layouts at version 1.0 (mefrec.h). Offsets body-relative.
struct TextSlot {
  std::size_t offset, bytes;
  const char* name;
};
struct BodyLayout {
  std::size_t fixed = 0;               // fixed part; 0 for variable (Note/SyLg/EDFA)
  std::vector<TextSlot> text;          // fixed-width text fields
  std::vector<std::size_t> times;      // si8 absolute/negated times
  std::size_t variable_text_from = SIZE_MAX;  // Note/SyLg (0), EDFA (8): text to the end
  bool keep_whole = false;             // LNTP: no text, no times
};

// Seiz (mefrec.h:110-131)
constexpr std::size_t SEIZ_FIXED = 1312, SEIZ_CHANNEL = 272, SEIZ_CHANNEL_NAME = 256,
                      SEIZ_NCH = 24, SEIZ_CH_ONSET = 256, SEIZ_CH_OFFSET = 264;

const BodyLayout* layout_of(const RawRecord& r) {
  static const std::map<std::string, BodyLayout> table = {
      {"Note", {0, {}, {}, 0, false}},
      {"SyLg", {0, {}, {}, 0, false}},
      {"EDFA", {8, {}, {}, 8, false}},
      {"Seiz",
       {SEIZ_FIXED,
        {{32, 128, "marker_name_1"}, {160, 128, "marker_name_2"}, {288, 1024, "annotation"}},
        {0, 8},
        SIZE_MAX,
        false}},
      {"Epoc", {192, {{32, 32, "epoch_type"}, {64, 128, "text"}}, {8, 16}, SIZE_MAX, false}},
      {"Curs", {160, {{32, 128, "name"}}, {8}, SIZE_MAX, false}},
      {"CSti",
       {200,
        {{0, 64, "task_type"}, {72, 64, "stimulus_type"}, {136, 64, "patient_response"}},
        {},
        SIZE_MAX,
        false}},
      {"ESti",
       {416, {{32, 128, "waveform"}, {160, 128, "anode"}, {288, 128, "catode"}}, {}, SIZE_MAX,
        false}},
      {"LNTP", {8, {}, {}, SIZE_MAX, true}},
  };
  if (!(r.vmaj == 1 && r.vmin == 0)) return nullptr;
  const auto it = table.find(r.type);
  if (it == table.end()) return nullptr;
  if (r.body.size() < it->second.fixed) return nullptr;
  return &it->second;
}

// ---------------------------------------------------------------------------
// Metadata images.
// ---------------------------------------------------------------------------

struct TmetImage {
  std::vector<ui1> bytes;  // exactly TMET; sections 2/3 DECRYPTED
  int s2_level = 0, s3_level = 0;  // as stored (> 0 = was encrypted)
  crypto::AccessKeys keys;
  si8 rto = 0;
};

TmetImage open_tmet(const SessionSource& src, const std::string& rel, const std::string& password) {
  auto all = src.read_all(rel);
  // Full integrity + password check by the one loader every reader uses.
  const TimeSeriesMetadata md = load_time_series_metadata(all, password);
  if (!md.section3_available)
    throw PasswordError("metadata section 3 of " + src.describe(rel) +
                        " is locked: it holds the recording-time offset and the subject fields, "
                        "so the copy needs the level-2 password");
  TmetImage t;
  t.bytes.assign(all.begin(), all.begin() + TMET);  // trailing padding is not part of the record
  t.rto = segment_rto(md);
  const auto s1 = fmt::MetadataSection1::parse(std::span<const ui1>(t.bytes).subspan(S1, S1_BYTES));
  t.s2_level = s1.section_2_encryption > 0 ? s1.section_2_encryption : 0;
  t.s3_level = s1.section_3_encryption > 0 ? s1.section_3_encryption : 0;
  t.keys = crypto::validate_password(password,
                                     std::span<const ui1>(t.bytes).subspan(UH_L1_FIELD, 16),
                                     std::span<const ui1>(t.bytes).subspan(UH_L2_FIELD, 16));
  auto decrypt = [&](std::size_t off, std::size_t n, int level) {
    if (level <= 0) return;
    const auto key = key_for(t.keys, level);
    if (key.empty()) throw PasswordError("cannot decrypt metadata of " + src.describe(rel));
    auto dec = crypto::aes128_ecb_decrypt(std::span<const ui1>(t.bytes).subspan(off, n), key);
    std::copy(dec.begin(), dec.end(), t.bytes.begin() + static_cast<std::ptrdiff_t>(off));
  };
  decrypt(S2, S2_BYTES, t.s2_level);
  decrypt(S3, S3_BYTES, t.s3_level);
  return t;
}

// ---------------------------------------------------------------------------
// The copy.
// ---------------------------------------------------------------------------

struct OutputKeys {
  bool encrypt = false;
  crypto::ValidationFields vf;
  crypto::AccessKeys keys;
};

class Anonymizer {
 public:
  Anonymizer(const SessionSource& src, const AnonymizeOptions& o, const Shifter& sh,
             const Redactor& red, std::string new_name, std::string pseudonym,
             std::set<std::string> excluded, OutputKeys out, const Control& ctl,
             std::function<void(std::uint64_t)> progress, AnonymizeReport& report)
      : src_(src), o_(o), sh_(sh), red_(red), name_(std::move(new_name)),
        pseudonym_(std::move(pseudonym)), excluded_(std::move(excluded)), out_(std::move(out)),
        ctl_(ctl), progress_(std::move(progress)), rep_(report) {}

  std::vector<std::string> written;  // every output file, for the flush

  /// Patch a universal header in place (any file type). Counts, maximum
  /// sizes and the CRCs are the caller's.
  void patch_uh(std::span<ui1> uh, const std::string& what) {
    write<si8>(uh, UH_START, sh_(read<si8>(uh, UH_START), what + " start time"));
    write<si8>(uh, UH_END, sh_(read<si8>(uh, UH_END), what + " end time"));
    write_string(uh, UH_SESSION_NAME, UH_NAME_BYTES, name_);
    write_string(uh, UH_ANON_NAME, UH_NAME_BYTES, pseudonym_);
    const Uuid old_level = uuid_at(uh, UH_LEVEL_UUID);
    Uuid level{};
    if (!all_zero(old_level)) {
      auto it = level_map_.find(old_level);
      if (it == level_map_.end()) it = level_map_.emplace(old_level, random_uuid()).first;
      level = it->second;
    }
    const Uuid file = random_uuid();
    std::copy(level.begin(), level.end(), uh.begin() + UH_LEVEL_UUID);
    std::copy(file.begin(), file.end(), uh.begin() + UH_FILE_UUID);
    std::copy(file.begin(), file.end(), uh.begin() + UH_PROV_UUID);  // the copy is its own origin
    std::copy(out_.vf.level1.begin(), out_.vf.level1.end(), uh.begin() + UH_L1_FIELD);
    std::copy(out_.vf.level2.begin(), out_.vf.level2.end(), uh.begin() + UH_L2_FIELD);
    std::fill_n(uh.begin() + UH_PROTECTED, UH_PROTECTED_BYTES, ui1{0});
    // Provenance: keep a mef3io region's history, clear anyone else's, and
    // record the anonymization (operation 6).
    fmt::UniversalHeader h;
    std::copy_n(uh.begin() + fmt::provenance::REGION_OFFSET, fmt::provenance::REGION_BYTES,
                h.discretionary_region.begin());
    if (!fmt::provenance::read(h).present) h.discretionary_region.fill(0);
    fmt::provenance::stamp_modified(h, fmt::provenance::Operation::Anonymize);
    std::copy(h.discretionary_region.begin(), h.discretionary_region.end(),
              uh.begin() + fmt::provenance::REGION_OFFSET);
  }

  std::string text_field(const std::string& value, const RecordTextField& where) {
    if (value.empty()) return value;
    std::string out;
    switch (o_.text) {
      case RecordTextPolicy::Drop: out = ""; break;
      case RecordTextPolicy::Keep: out = red_.apply(value); break;
      case RecordTextPolicy::Callback: out = red_.apply(o_.text_fn(value, where)); break;
    }
    if (out != value) ++rep_.record_text_fields_changed;
    return out;
  }

  void write_out(const fs::path& p, const std::vector<ui1>& bytes) {
    if (fs::exists(p)) throw IoError("output file already exists: " + p.string());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) throw IoError("cannot create file: " + p.string());
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    f.flush();
    if (!f) throw IoError("write failed (disk full?): " + p.string());
    f.close();
    written.push_back(p.string());
    rep_.bytes_written += bytes.size();
  }

  void copy_tmet(const std::string& rel, const fs::path& out, const std::string& channel) {
    ctl_.check();
    TmetImage t = open_tmet(src_, rel, o_.password);
    if (t.rto != sh_.rto)
      throw WriteConflictError("segments disagree on the recording-time offset (" +
                               std::to_string(sh_.rto) + " for the session, " +
                               std::to_string(t.rto) + " in " + src_.describe(rel) +
                               "), so there is no single right way to read their times");
    std::span<ui1> b(t.bytes);
    auto s2 = b.subspan(S2, S2_BYTES);
    auto s3 = b.subspan(S3, S3_BYTES);
    // Section 2: descriptions, and the regions meflib leaves to writers.
    for (std::size_t off : {S2_CHANNEL_DESC, S2_SESSION_DESC, S2_REFERENCE_DESC}) {
      const std::string old = read_string(s2, off, S2_DESC_BYTES);
      const std::string now = o_.clear_descriptions ? std::string() : red_.apply(old);
      if (now != old) write_string(s2, off, S2_DESC_BYTES, now);
    }
    std::fill(s2.begin() + S2_REGIONS, s2.end(), ui1{0});
    // Section 3: time-zone fields SET, identity cleared.
    write<si8>(s3, S3_RTO, 0);
    write<si8>(s3, S3_DST_START, fmt::UUTC_NO_ENTRY);
    write<si8>(s3, S3_DST_END, fmt::UUTC_NO_ENTRY);
    write<si4>(s3, S3_GMT, 0);
    if (o_.clear_subject)
      for (std::size_t off : {S3_NAME1, S3_NAME2, S3_ID})
        std::fill_n(s3.begin() + static_cast<std::ptrdiff_t>(off), S3_SUBJECT_BYTES, ui1{0});
    if (o_.clear_location)
      std::fill_n(s3.begin() + S3_LOCATION, S3_LOCATION_BYTES, ui1{0});
    std::fill(s3.begin() + S3_REGIONS, s3.end(), ui1{0});
    // Section 1: encryption levels of the COPY; regions cleared.
    auto s1 = b.subspan(S1, S1_BYTES);
    std::fill(s1.begin(), s1.end(), ui1{0});
    write<si1>(s1, 0, out_.encrypt ? fmt::LEVEL_1_ENCRYPTION : -fmt::LEVEL_1_ENCRYPTION);
    write<si1>(s1, 1, out_.encrypt ? fmt::LEVEL_2_ENCRYPTION : -fmt::LEVEL_2_ENCRYPTION);
    if (out_.encrypt) {
      auto e2 = crypto::aes128_ecb_encrypt(s2, *out_.keys.level1_key);
      std::copy(e2.begin(), e2.end(), s2.begin());
      auto e3 = crypto::aes128_ecb_encrypt(s3, *out_.keys.level2_key);
      std::copy(e3.begin(), e3.end(), s3.begin());
    }
    patch_uh(b.first(UH), channel + " metadata");
    // Bounded CRCs: the record is exactly TMET bytes.
    write<ui4>(b, 4, crc::calculate(std::span<const ui1>(b).subspan(UH, TMET - UH)));
    write<ui4>(b, 0, crc::calculate(std::span<const ui1>(b).subspan(4, UH - 4)));
    write_out(out, t.bytes);
    if (progress_) progress_(TMET);
    seg_keys_ = t.keys;
  }

  void copy_segment_data(const detail::LayoutSegment& seg, const fs::path& dir,
                         const std::string& channel) {
    const std::string stem = fs::path(seg.tmet).stem().string();
    detail::SegmentCopySpec spec;
    spec.source = &src_;
    spec.tidx_rel = seg.tidx;
    spec.tdat_rel = seg.tdat;
    spec.tidx_out = (dir / (stem + ".tidx")).string();
    spec.tdat_out = (dir / (stem + ".tdat")).string();
    spec.control = &ctl_;
    spec.on_progress = progress_;
    const std::string where = src_.describe(seg.tdat);
    spec.on_block = [&](detail::CopyBlock& cb) {
      const std::string what = "block " + std::to_string(cb.index) + " of " + where;
      write<si8>(cb.block, 40, sh_(read<si8>(cb.block, 40), what + " start time"));
      write<si8>(cb.entry, 8, sh_(read<si8>(cb.entry, 8), "index entry " + what));
      const ui1 flags = read<ui1>(cb.block, 4);
      const ui1 level_mask = flags & ENC_MASK;
      if (level_mask) {
        const int level =
            (flags & fmt::RedBlockHeader::LEVEL_1_ENCRYPTION_MASK) ? fmt::LEVEL_1_ENCRYPTION
                                                                   : fmt::LEVEL_2_ENCRYPTION;
        const auto key = key_for(seg_keys_, level);
        if (key.empty())
          throw PasswordError(what + " is level-" + std::to_string(level) +
                              " encrypted and the password does not open it");
        std::span<ui1, red::STATISTICS_BYTES> stats(cb.block.data() + 48, red::STATISTICS_BYTES);
        red::decrypt_statistics(stats, key);
        ui1 new_flags = static_cast<ui1>(flags & ~ENC_MASK);
        if (out_.encrypt) {
          // Same level, the copy's key: protection is kept, never downgraded.
          red::encrypt_statistics(stats, key_for(out_.keys, level));
          new_flags = flags;
          ++rep_.blocks_reencrypted;
        } else {
          ++rep_.blocks_decrypted;
        }
        write<ui1>(cb.block, 4, new_flags);
        const ui1 eflags = read<ui1>(cb.entry, 44);
        write<ui1>(cb.entry, 44,
                   static_cast<ui1>((eflags & ~ENC_MASK) | (new_flags & ENC_MASK)));
      }
      return true;
    };
    spec.on_header = [&](std::span<ui1> uh, bool is_tdat) {
      patch_uh(uh, channel + (is_tdat ? " data" : " index"));
    };
    const auto st = detail::copy_segment(spec);
    written.push_back(spec.tidx_out);
    written.push_back(spec.tdat_out);
    rep_.blocks += st.blocks;
    rep_.bytes_written += st.output_bytes;
  }

  /// One record file pair -> `out_rdat` (+ .ridx).
  void copy_records(const std::string& rdat_rel, const fs::path& out_rdat, RecordLevel level,
                    const std::string& channel, int segment) {
    ctl_.check();
    RawRecordFile f = read_record_file(src_, rdat_rel, o_.password);
    const std::string where = src_.describe(rdat_rel);
    std::vector<ui1> rdat(f.uh);
    std::vector<ui1> ridx(f.has_ridx ? f.ridx_uh : f.uh);
    if (!f.has_ridx) write_string(ridx, 8, 5, fmt::FILE_TYPE_RECORD_INDICES);
    ui4 max_entry = 0;
    for (std::size_t i = 0; i < f.records.size(); ++i) {
      RawRecord& r = f.records[i];
      const std::string what = "record " + std::to_string(i) + " (" + r.type + ") of " + where;
      const si8 time = sh_(r.time, what + " time");
      RecordTextField tf{r.type, "", level, channel, segment, to_user_time(time, 0)};
      std::vector<ui1> body = rewrite_body(r, tf, what);
      if (body.size() % 16) body.resize((body.size() + 15) & ~std::size_t{15}, PAD_BYTE);
      si1 enc = 0;
      if (out_.encrypt) {
        enc = fmt::LEVEL_2_ENCRYPTION;  // records may hold identifying text
        auto e = crypto::aes128_ecb_encrypt(body, *out_.keys.level2_key);
        std::copy(e.begin(), e.end(), body.begin());
      }
      std::vector<ui1> rec(RHDR, 0);
      std::copy(r.type_bytes.begin(), r.type_bytes.end(), rec.begin() + 4);
      write<ui1>(rec, 9, r.vmaj);
      write<ui1>(rec, 10, r.vmin);
      write<si1>(rec, 11, enc);
      write<ui4>(rec, 12, static_cast<ui4>(body.size()));
      write<si8>(rec, 16, time);
      rec.insert(rec.end(), body.begin(), body.end());
      write<ui4>(rec, 0, crc::calculate(std::span<const ui1>(rec).subspan(4)));
      const si8 offset = static_cast<si8>(rdat.size());
      rdat.insert(rdat.end(), rec.begin(), rec.end());
      max_entry = std::max(max_entry, static_cast<ui4>(rec.size()));

      std::array<ui1, RIDX> e{};
      std::copy(r.type_bytes.begin(), r.type_bytes.end(), e.begin());
      e[5] = r.vmaj;
      e[6] = r.vmin;
      e[7] = static_cast<ui1>(enc);
      write<si8>(e, 8, offset);
      const si8 idx_time = f.has_ridx ? read<si8>(f.ridx[i], 16) : r.time;
      write<si8>(e, 16, sh_(idx_time, "record index entry " + std::to_string(i) + " of " + where));
      ridx.insert(ridx.end(), e.begin(), e.end());
      ++rep_.records;
    }
    const si8 n = static_cast<si8>(f.records.size());
    auto seal = [&](std::vector<ui1>& file, si8 max, const std::string& what) {
      std::span<ui1> uh(file.data(), UH);
      patch_uh(uh, what);
      write<si8>(uh, 32, n);
      write<si8>(uh, 40, max);
      write<ui4>(file, 4, crc::calculate(std::span<const ui1>(file).subspan(UH)));
      write<ui4>(file, 0, crc::calculate(std::span<const ui1>(file).subspan(4, UH - 4)));
    };
    seal(rdat, max_entry, "record file " + where);
    seal(ridx, static_cast<si8>(RIDX), "record index of " + where);
    write_out(out_rdat, rdat);
    write_out(fs::path(ridx_of(out_rdat.string())), ridx);
    if (progress_) progress_(rdat.size() + ridx.size());
  }

 private:
  std::vector<ui1> rewrite_body(const RawRecord& r, RecordTextField tf, const std::string& what) {
    const BodyLayout* lay = layout_of(r);
    if (!lay) {
      // No known layout (UnRc, an unknown type or version, a body shorter
      // than its type): it may hold text or dates nothing here can find, so
      // it is not carried over. The record itself (type, time) is kept.
      if (!r.body.empty()) ++rep_.record_bodies_cleared;
      return {};
    }
    if (lay->keep_whole) return r.body;
    std::vector<ui1> b(r.body);
    if (lay->variable_text_from != SIZE_MAX) {
      const std::size_t from = lay->variable_text_from;
      tf.field = "text";
      const std::string text = text_field(read_string(b, from, b.size() - from), tf);
      b.resize(from);
      b.insert(b.end(), text.begin(), text.end());
      b.push_back(0);
      return b;
    }
    for (std::size_t off : lay->times)
      write<si8>(b, off, sh_(read<si8>(b, off), what + " body time", true));
    for (const auto& slot : lay->text) {
      tf.field = slot.name;
      const std::string old = read_string(b, slot.offset, slot.bytes);
      const std::string now = text_field(old, tf);
      if (now != old) write_string(b, slot.offset, slot.bytes, now);
    }
    if (r.type != "Seiz") {
      b.resize(lay->fixed);  // what follows the fixed part is padding
      return b;
    }
    // Seiz channel entries: excluded channels leave, the rest are shifted.
    const si4 declared = read<si4>(b, SEIZ_NCH);
    const std::size_t room = (b.size() - SEIZ_FIXED) / SEIZ_CHANNEL;
    const std::size_t n = std::min(room, static_cast<std::size_t>(std::max<si4>(declared, 0)));
    std::vector<ui1> out(b.begin(), b.begin() + SEIZ_FIXED);
    si4 kept = 0;
    for (std::size_t c = 0; c < n; ++c) {
      std::vector<ui1> e(b.begin() + static_cast<std::ptrdiff_t>(SEIZ_FIXED + c * SEIZ_CHANNEL),
                         b.begin() + static_cast<std::ptrdiff_t>(SEIZ_FIXED + (c + 1) * SEIZ_CHANNEL));
      const std::string name = read_string(e, 0, SEIZ_CHANNEL_NAME);
      if (excluded_.count(name)) {
        ++rep_.seizure_channel_entries_dropped;
        continue;
      }
      for (std::size_t off : {SEIZ_CH_ONSET, SEIZ_CH_OFFSET})
        write<si8>(e, off, sh_(read<si8>(e, off), what + " channel " + std::to_string(c) + " time",
                               true));
      out.insert(out.end(), e.begin(), e.end());
      ++kept;
    }
    write<si4>(out, SEIZ_NCH, kept);
    return out;
  }

  const SessionSource& src_;
  const AnonymizeOptions& o_;
  const Shifter& sh_;
  const Redactor& red_;
  std::string name_, pseudonym_;
  std::set<std::string> excluded_;
  OutputKeys out_;
  const Control& ctl_;
  std::function<void(std::uint64_t)> progress_;
  AnonymizeReport& rep_;
  std::map<Uuid, Uuid> level_map_;
  crypto::AccessKeys seg_keys_;
};

std::string leaf(const std::string& rel) {
  const auto p = rel.rfind('/');
  return p == std::string::npos ? rel : rel.substr(p + 1);
}

bool path_inside(const fs::path& inner, const fs::path& outer) {
  std::error_code ec;
  const fs::path a = fs::weakly_canonical(inner, ec);
  const fs::path b = fs::weakly_canonical(outer, ec);
  auto ai = a.begin();
  for (auto bi = b.begin(); bi != b.end(); ++bi, ++ai) {
    if (bi->empty()) continue;  // trailing separator
    if (ai == a.end() || *ai != *bi) return false;
  }
  return true;
}

/// Everything about the source the copy and the verification both need.
struct SourcePlan {
  SessionSummary summary;
  detail::SessionLayout layout;
  std::vector<std::string> all_channels;  // data + record-only, sorted
  si8 rto = 0;
  si8 first = 0, last = 0;
  bool has_times = false;
  std::set<std::string> session_names;    // folder stem + every stored session_name
  std::set<std::string> subject_values, location_values, anonymized_values;
  std::set<Uuid> uuids;
};

SourcePlan plan_source(const SessionSource& src, const std::string& path,
                       const std::string& password, const Control& ctl) {
  SourcePlan p;
  InspectOptions io;
  io.password = password;
  io.strict = true;
  io.include_values = true;
  p.summary = inspect_session(path, io, ctl);
  p.layout = detail::discover_layout(src);
  for (const auto& c : p.layout.channels) p.all_channels.push_back(c.name);
  for (const auto& c : p.layout.record_only_channels) p.all_channels.push_back(c);
  std::sort(p.all_channels.begin(), p.all_channels.end());
  p.all_channels.erase(std::unique(p.all_channels.begin(), p.all_channels.end()),
                       p.all_channels.end());
  const auto& to = p.summary.time_offset;
  p.rto = (to.status == "applied" && to.value) ? *to.value : 0;
  if (p.summary.first_time && p.summary.last_time) {
    p.has_times = true;
    p.first = *p.summary.first_time;
    p.last = *p.summary.last_time;
  }
  p.session_names.insert(src.session_name());
  for (const auto& f : p.summary.identity) {
    std::set<std::string>* dst = nullptr;
    if (f.name == "session_name") dst = &p.session_names;
    else if (f.name == "subject_name_1" || f.name == "subject_name_2" || f.name == "subject_id")
      dst = &p.subject_values;
    else if (f.name == "recording_location") dst = &p.location_values;
    else if (f.name == "anonymized_name") dst = &p.anonymized_values;
    if (dst)
      for (const auto& v : f.values)
        if (!v.empty()) dst->insert(v);
  }
  return p;
}

Redactor build_redactor(const SourcePlan& p, const std::string& new_name, bool clear_subject,
                        bool clear_location, const std::set<std::string>& excluded,
                        const std::string& pseudonym) {
  Redactor r;
  for (const auto& n : p.session_names)
    if (n != new_name) r.add(n, new_name, "the source's session name");
  if (clear_subject)
    for (const auto& v : p.subject_values) r.add(v, "", "a subject name or ID");
  if (clear_location)
    for (const auto& v : p.location_values) r.add(v, "", "the recording location");
  for (const auto& v : p.anonymized_values)
    if (v != pseudonym && v != new_name) r.add(v, "", "the source's anonymized name");
  for (const auto& c : excluded) r.add(c, "", "an excluded channel's name");
  return r;
}

// ---------------------------------------------------------------------------
// Verification.
// ---------------------------------------------------------------------------

class Verifier {
 public:
  Verifier(const SessionSource& src, const SessionSource& dst, const SourcePlan& plan, si8 shift,
           const VerifyOptions& o, const Control& ctl, std::function<void(std::uint64_t)> progress,
           VerifyReport& rep)
      : src_(src), dst_(dst), plan_(plan), o_(o), ctl_(ctl), progress_(std::move(progress)),
        rep_(rep) {
    sh_.rto = plan.rto;
    sh_.shift = shift;
    // The leak window: the original range, widened by just under a day on
    // each side. A shift that passed the floor puts every copied time outside
    // it, so a value inside can only be one the rewrite missed.
    lo_ = plan.first - DAY_US + 1;
    hi_ = plan.last + DAY_US - 1;
  }

  void problem(const std::string& loc, const std::string& field, const std::string& msg) {
    rep_.problems.push_back({loc, field, msg});
  }

  void leak(const std::string& loc, const std::string& field, const std::string& msg) {
    VerifyIssue i{loc, field, msg};
    rep_.leaks.push_back(i);
    rep_.problems.push_back(i);
  }

  bool in_window(si8 v) const {
    if (!plan_.has_times || v == fmt::UUTC_NO_ENTRY) return false;
    const si8 a = v < 0 ? -v : v;  // plain or negated (v > INT64_MIN here)
    if (a >= lo_ && a <= hi_) return true;
    // A source with an offset stores -(t - rto): the time of day. Its
    // magnitudes are a window of their own.
    if (plan_.rto != 0) {
      const si8 rlo = lo_ - plan_.rto, rhi = hi_ - plan_.rto;
      if (a >= rlo && a <= rhi) return true;
    }
    return false;
  }

  /// A time field of the copy: in the leak window? and, given the source's
  /// value, exactly the shift away in the same convention?
  void time_field(si8 src_v, si8 dst_v, const std::string& loc, const std::string& field,
                  bool body = false) {
    ++rep_.times_checked;
    if (!(body && is_sentinel(dst_v, true)) && in_window(dst_v))
      leak(loc, field, "holds " + std::to_string(dst_v) +
                           ", a time inside the source's recording range (plain or negated)");
    si8 expected;
    try {
      expected = sh_(src_v, field, body);
    } catch (const std::exception& e) {
      problem(loc, field, std::string("the source's value cannot be shifted: ") + e.what());
      return;
    }
    if (dst_v != expected)
      problem(loc, field,
              "is " + std::to_string(dst_v) + ", expected " + std::to_string(expected) +
                  " (the source's " + std::to_string(src_v) + " shifted by " +
                  std::to_string(sh_.shift) + " us in the same sign convention)");
  }

  void text(const std::string& value, const std::string& loc, const std::string& field) {
    ++rep_.text_fields_checked;
    const std::string hit = red_.match(value);
    if (!hit.empty()) leak(loc, field, "contains " + hit);
  }

  void set_redactor(Redactor r) { red_ = std::move(r); }

  void check_uh(std::span<const ui1> s, std::span<const ui1> d, const std::string& loc) {
    time_field(read<si8>(s, UH_START), read<si8>(d, UH_START), loc, "universal header start_time");
    time_field(read<si8>(s, UH_END), read<si8>(d, UH_END), loc, "universal header end_time");
    const std::string sn = read_string(d, UH_SESSION_NAME, UH_NAME_BYTES);
    if (sn != dst_.session_name())
      problem(loc, "universal header session_name",
              "does not match the copy's folder name");
    text(sn, loc, "universal header session_name");
    text(read_string(d, UH_ANON_NAME, UH_NAME_BYTES), loc, "universal header anonymized_name");
    for (std::size_t off : {UH_LEVEL_UUID, UH_FILE_UUID, UH_PROV_UUID}) {
      const Uuid u = uuid_at(d, off);
      if (!all_zero(u) && src_uuids_.count(u))
        leak(loc, "universal header UUID at " + std::to_string(off),
             "is the source's UUID, which links the copy to the original");
    }
    if (!all_zero(d.subspan(UH_PROTECTED, UH_PROTECTED_BYTES)))
      problem(loc, "universal header protected region", "is not cleared");
    fmt::UniversalHeader h = fmt::UniversalHeader::parse(d);
    const auto pv = fmt::provenance::read(h);
    if (!pv.present ||
        pv.last_operation != static_cast<ui1>(fmt::provenance::Operation::Anonymize))
      problem(loc, "provenance", "is not stamped as anonymized (operation 6)");
    if (crc::calculate(d.subspan(4, UH - 4)) != read<ui4>(d, 0))
      problem(loc, "universal header CRC", "does not verify");
  }

  void collect_source_uuids(std::span<const ui1> uh) {
    for (std::size_t off : {UH_LEVEL_UUID, UH_FILE_UUID, UH_PROV_UUID}) {
      const Uuid u = uuid_at(uh, off);
      if (!all_zero(u)) src_uuids_.insert(u);
    }
  }

  void check_tmet(const std::string& srel, const std::string& drel) {
    ctl_.check();
    const TmetImage s = open_tmet(src_, srel, o_.password);
    TmetImage d;
    try {
      d = open_tmet(dst_, drel, o_.output_password);
    } catch (const std::exception& e) {
      problem(drel, "metadata", std::string("cannot be read: ") + e.what());
      return;
    }
    collect_source_uuids(std::span<const ui1>(s.bytes).first(UH));
    std::span<const ui1> sb(s.bytes), db(d.bytes);
    check_uh(sb.first(UH), db.first(UH), drel);
    const auto ds2 = db.subspan(S2, S2_BYTES), ss2 = sb.subspan(S2, S2_BYTES);
    const auto ds3 = db.subspan(S3, S3_BYTES);
    // Time-zone fields: SET values, not shifted.
    if (read<si8>(ds3, S3_RTO) != 0) {
      if (in_window(read<si8>(ds3, S3_RTO)))
        leak(drel, "recording_time_offset", "holds a date from the source's range");
      problem(drel, "recording_time_offset", "is not 0");
    }
    for (std::size_t off : {S3_DST_START, S3_DST_END}) {
      const si8 v = read<si8>(ds3, off);
      if (v != fmt::UUTC_NO_ENTRY) {
        if (in_window(v)) leak(drel, off == S3_DST_START ? "DST_start_time" : "DST_end_time",
                               "holds a date from the source's range");
        problem(drel, off == S3_DST_START ? "DST_start_time" : "DST_end_time", "is not NO_ENTRY");
      }
    }
    rep_.times_checked += 3;
    if (read<si4>(ds3, S3_GMT) != 0) problem(drel, "GMT_offset", "is not 0");
    // Text fields, field by field.
    const std::pair<std::size_t, const char*> s3_text[] = {
        {S3_NAME1, "subject_name_1"}, {S3_NAME2, "subject_name_2"}, {S3_ID, "subject_id"}};
    for (const auto& [off, name] : s3_text) {
      const std::string v = read_string(ds3, off, S3_SUBJECT_BYTES);
      if (o_.clear_subject && !v.empty()) leak(drel, name, "is not cleared");
      text(v, drel, name);
    }
    {
      const std::string v = read_string(ds3, S3_LOCATION, S3_LOCATION_BYTES);
      if (o_.clear_location && !v.empty()) leak(drel, "recording_location", "is not cleared");
      text(v, drel, "recording_location");
    }
    const std::pair<std::size_t, const char*> s2_text[] = {
        {S2_CHANNEL_DESC, "channel_description"},
        {S2_SESSION_DESC, "session_description"},
        {S2_REFERENCE_DESC, "reference_description"}};
    for (const auto& [off, name] : s2_text) {
      const std::string v = read_string(ds2, off, S2_DESC_BYTES);
      if (o_.clear_descriptions && !v.empty()) leak(drel, name, "is not cleared");
      text(v, drel, name);
    }
    text(read_string(ds2, S2_UNITS, S2_UNITS_BYTES), drel, "units_description");
    if (!all_zero(ds2.subspan(S2_REGIONS)))
      problem(drel, "section 2 protected/discretionary regions", "are not cleared");
    if (!all_zero(ds3.subspan(S3_REGIONS)))
      problem(drel, "section 3 protected/discretionary regions", "are not cleared");
    if (!all_zero(db.subspan(S1 + 2, S1_BYTES - 2)))
      problem(drel, "section 1 regions", "are not cleared");
    // The signal's declarations are carried over unchanged.
    if (!std::equal(ss2.begin() + 6152, ss2.begin() + 6208, ds2.begin() + 6152) ||
        !std::equal(ss2.begin() + 6336, ss2.begin() + 6432, ds2.begin() + 6336))
      problem(drel, "section 2", "signal declarations (sampling frequency, units factor, "
                                 "filters, counts, maxima) differ from the source's");
    dst_encrypted_ = d.s2_level > 0;
    src_keys_ = s.keys;
    dst_keys_ = d.keys;
    if (progress_) progress_(2 * TMET);
  }

  void check_segment_data(const detail::LayoutSegment& s, const detail::LayoutSegment& d) {
    const auto stidx = src_.file_size(s.tidx), dtidx = dst_.file_size(d.tidx);
    const auto stdat = src_.file_size(s.tdat), dtdat = dst_.file_size(d.tdat);
    const auto suh_x = src_.read_range(s.tidx, 0, UH), duh_x = dst_.read_range(d.tidx, 0, UH);
    const auto suh_d = src_.read_range(s.tdat, 0, UH), duh_d = dst_.read_range(d.tdat, 0, UH);
    collect_source_uuids(suh_x);
    collect_source_uuids(suh_d);
    check_uh(suh_x, duh_x, d.tidx);
    check_uh(suh_d, duh_d, d.tdat);
    const std::size_t n = static_cast<std::size_t>((stidx - UH) / ENTRY);
    if (dtidx < UH || (dtidx - UH) / ENTRY != n || (dtidx - UH) % ENTRY) {
      problem(d.tidx, "block index", "does not hold the source's " + std::to_string(n) + " entries");
      return;
    }
    ui4 dcrc_x = fmt::CRC_START_VALUE, dcrc_d = fmt::CRC_START_VALUE;
    std::uint64_t dexpect = UH;
    constexpr std::size_t CHUNK = 8192, BATCH = 8u << 20;
    for (std::size_t first = 0; first < n; first += CHUNK) {
      ctl_.check();
      const std::size_t k = std::min(CHUNK, n - first);
      const auto se = src_.read_range(s.tidx, UH + first * ENTRY, k * ENTRY);
      const auto de = dst_.read_range(d.tidx, UH + first * ENTRY, k * ENTRY);
      dcrc_x = crc::calculate(de, dcrc_x);
      std::size_t i = 0;
      while (i < k) {
        ctl_.check();
        // A batch of blocks, bounded in bytes on both sides.
        std::size_t j = i;
        std::uint64_t slo = 0, shi = 0, dlo = 0, dhi = 0;
        while (j < k) {
          const auto so = static_cast<std::uint64_t>(read<si8>(se, j * ENTRY));
          const auto sn = read<ui4>(se, j * ENTRY + 28);
          const auto dof = static_cast<std::uint64_t>(read<si8>(de, j * ENTRY));
          const auto dn = read<ui4>(de, j * ENTRY + 28);
          if (so < UH || so + sn > stdat || dof != dexpect || dof + dn > dtdat || sn != dn) {
            problem(d.tidx, "entry " + std::to_string(first + j),
                    "does not describe the source block's copy (offset/size)");
            return;
          }
          if (j > i && (so + sn - slo > BATCH || dof + dn - dlo > BATCH || so < shi)) break;
          if (j == i) { slo = so; dlo = dof; }
          shi = std::max(shi, so + sn);
          dhi = dof + dn;
          dexpect = dhi;
          ++j;
        }
        const auto sbuf = src_.read_range(s.tdat, slo, shi - slo);
        const auto dbuf = dst_.read_range(d.tdat, dlo, dhi - dlo);
        dcrc_d = crc::calculate(dbuf, dcrc_d);
        for (std::size_t b = i; b < j; ++b) {
          const std::string loc = d.tdat + " block " + std::to_string(first + b);
          const auto sent = std::span<const ui1>(se).subspan(b * ENTRY, ENTRY);
          const auto dent = std::span<const ui1>(de).subspan(b * ENTRY, ENTRY);
          time_field(read<si8>(sent, 8), read<si8>(dent, 8), loc, "index entry start_time");
          if (!std::equal(sent.begin() + 16, sent.begin() + 44, dent.begin() + 16) ||
              !std::equal(sent.begin() + 45, sent.end(), dent.begin() + 45) ||
              (sent[44] & ~ENC_MASK) != (dent[44] & ~ENC_MASK))
            problem(loc, "index entry", "differs from the source's beyond its time");
          const auto nb = read<ui4>(sent, 28);
          std::span<const ui1> sblk(sbuf.data() + (read<si8>(sent, 0) - slo), nb);
          std::span<const ui1> dblk(dbuf.data() + (read<si8>(dent, 0) - dlo), nb);
          check_block(sblk, dblk, loc);
          ++rep_.blocks;
        }
        if (progress_) progress_(sbuf.size() + dbuf.size());
        i = j;
      }
    }
    if (dexpect != dtdat) problem(d.tdat, "data file", "holds bytes no index entry describes");
    if (read<ui4>(duh_x, 4) != dcrc_x) problem(d.tidx, "body CRC", "does not verify");
    if (read<ui4>(duh_d, 4) != dcrc_d) problem(d.tdat, "body CRC", "does not verify");
  }

  void check_block(std::span<const ui1> s, std::span<const ui1> d, const std::string& loc) {
    if (crc::calculate(d.subspan(4)) != read<ui4>(d, 0))
      problem(loc, "block CRC", "does not verify");
    const ui1 sf = s[4], df = d[4];
    if ((sf & ~ENC_MASK) != (df & ~ENC_MASK) || !std::equal(s.begin() + 5, s.begin() + 40, d.begin() + 5))
      problem(loc, "block header", "differs from the source's beyond its time and encryption");
    time_field(read<si8>(s, 40), read<si8>(d, 40), loc, "block header start_time");
    // The statistics table, compared in the clear.
    std::array<ui1, red::STATISTICS_BYTES> ss{}, ds{};
    std::copy_n(s.begin() + 48, ss.size(), ss.begin());
    std::copy_n(d.begin() + 48, ds.size(), ds.begin());
    auto clear = [&](std::array<ui1, red::STATISTICS_BYTES>& st, ui1 flags,
                     const crypto::AccessKeys& keys, const char* side) {
      if (!(flags & ENC_MASK)) return true;
      const int level =
          (flags & fmt::RedBlockHeader::LEVEL_1_ENCRYPTION_MASK) ? 1 : 2;
      const auto key = key_for(keys, level);
      if (key.empty()) {
        problem(loc, "statistics", std::string("cannot be decrypted in the ") + side);
        return false;
      }
      red::decrypt_statistics(st, key);
      return true;
    };
    if (clear(ss, sf, src_keys_, "source") && clear(ds, df, dst_keys_, "copy") && ss != ds)
      problem(loc, "statistics table", "differs from the source's (after decryption)");
    if (sf & ENC_MASK) {
      if (dst_encrypted_ ? (df & ENC_MASK) != (sf & ENC_MASK) : (df & ENC_MASK) != 0)
        problem(loc, "block encryption",
                dst_encrypted_ ? "is not re-encrypted at the source's level"
                               : "is still marked encrypted in an unencrypted copy");
    } else if (df & ENC_MASK) {
      problem(loc, "block encryption", "is encrypted although the source's block is not");
    }
    if (!std::equal(s.begin() + fmt::RED_BLOCK_HEADER_BYTES, s.end(),
                    d.begin() + fmt::RED_BLOCK_HEADER_BYTES))
      problem(loc, "RED payload", "is not byte-identical to the source's");
  }

  void check_records(const std::string& srel, const std::string& drel,
                     const std::set<std::string>& excluded) {
    ctl_.check();
    RawRecordFile s = read_record_file(src_, srel, o_.password);
    RawRecordFile d;
    try {
      d = read_record_file(dst_, drel, o_.output_password);
    } catch (const std::exception& e) {
      problem(drel, "records", std::string("cannot be read: ") + e.what());
      return;
    }
    collect_source_uuids(s.uh);
    if (s.has_ridx) collect_source_uuids(s.ridx_uh);
    check_uh(s.uh, d.uh, drel);
    if (d.has_ridx) check_uh(s.has_ridx ? s.ridx_uh : s.uh, d.ridx_uh, ridx_of(drel));
    else problem(drel, "record index", "is missing");
    if (s.records.size() != d.records.size()) {
      problem(drel, "records", "the copy holds " + std::to_string(d.records.size()) +
                                   " record(s), the source " + std::to_string(s.records.size()));
      return;
    }
    for (std::size_t i = 0; i < s.records.size(); ++i) {
      const RawRecord& a = s.records[i];
      const RawRecord& b = d.records[i];
      const std::string loc = drel + " record " + std::to_string(i);
      ++rep_.records;
      if (a.type_bytes != b.type_bytes || a.vmaj != b.vmaj || a.vmin != b.vmin) {
        problem(loc, "record header", "type or version differs from the source's (order?)");
        continue;
      }
      time_field(a.time, b.time, loc, "record header time");
      if (d.has_ridx) {
        const auto& e = d.ridx[i];
        time_field(s.has_ridx ? read<si8>(s.ridx[i], 16) : a.time, read<si8>(e, 16), loc,
                   "record index time");
        if (read<si8>(e, 8) != d.record_offsets[i])
          problem(loc, "record index offset", "does not point at the record");
      }
      check_body(a, b, loc, excluded);
    }
  }

  void check_body(const RawRecord& a, const RawRecord& b, const std::string& loc,
                  const std::set<std::string>& excluded) {
    const BodyLayout* la = layout_of(a);
    if (!la) {
      if (!b.body.empty() && !all_zero(b.body) &&
          !std::all_of(b.body.begin(), b.body.end(), [](ui1 c) { return c == PAD_BYTE; }))
        leak(loc, "record body", "a body with no known layout was carried over");
      return;
    }
    const BodyLayout* lb = layout_of(b);
    if (!lb) {
      problem(loc, "record body", "is not a valid " + a.type + " body");
      return;
    }
    if (la->keep_whole) {
      if (!std::equal(a.body.begin(), a.body.end(), b.body.begin(), b.body.end()) &&
          !(b.body.size() >= a.body.size() &&
            std::equal(a.body.begin(), a.body.end(), b.body.begin())))
        problem(loc, "record body", "differs from the source's");
      return;
    }
    if (la->variable_text_from != SIZE_MAX) {
      const std::size_t from = la->variable_text_from;
      if (from && !std::equal(a.body.begin(), a.body.begin() + from, b.body.begin()))
        problem(loc, "record body", "duration differs from the source's");
      text(read_string(b.body, from, b.body.size() - from), loc, "record text");
      return;
    }
    for (std::size_t off : la->times)
      time_field(read<si8>(a.body, off), read<si8>(b.body, off), loc,
                 "record body time at " + std::to_string(off), true);
    for (const auto& slot : la->text) text(read_string(b.body, slot.offset, slot.bytes), loc,
                                           std::string("record ") + slot.name);
    if (a.type != "Seiz") return;
    // Channel entries: the source's minus the excluded, in order, shifted.
    std::vector<std::size_t> want;
    const auto entries = [](const RawRecord& r) {
      const si4 declared = read<si4>(r.body, SEIZ_NCH);
      const std::size_t room = (r.body.size() - SEIZ_FIXED) / SEIZ_CHANNEL;
      return std::min(room, static_cast<std::size_t>(std::max<si4>(declared, 0)));
    };
    for (std::size_t c = 0; c < entries(a); ++c)
      if (!excluded.count(read_string(a.body, SEIZ_FIXED + c * SEIZ_CHANNEL, SEIZ_CHANNEL_NAME)))
        want.push_back(c);
    const std::size_t have = entries(b);
    if (have != want.size() || read<si4>(b.body, SEIZ_NCH) != static_cast<si4>(have)) {
      problem(loc, "Seiz channels", "the copy lists " + std::to_string(have) + ", expected " +
                                        std::to_string(want.size()));
      return;
    }
    for (std::size_t k = 0; k < have; ++k) {
      const std::size_t so = SEIZ_FIXED + want[k] * SEIZ_CHANNEL, dof = SEIZ_FIXED + k * SEIZ_CHANNEL;
      const std::string name = read_string(b.body, dof, SEIZ_CHANNEL_NAME);
      if (excluded.count(name)) leak(loc, "Seiz channel name", "names an excluded channel");
      if (name != read_string(a.body, so, SEIZ_CHANNEL_NAME))
        problem(loc, "Seiz channel " + std::to_string(k), "name differs from the source's");
      time_field(read<si8>(a.body, so + SEIZ_CH_ONSET), read<si8>(b.body, dof + SEIZ_CH_ONSET), loc,
                 "Seiz channel onset", true);
      time_field(read<si8>(a.body, so + SEIZ_CH_OFFSET), read<si8>(b.body, dof + SEIZ_CH_OFFSET),
                 loc, "Seiz channel offset", true);
    }
  }

 private:
  const SessionSource& src_;
  const SessionSource& dst_;
  const SourcePlan& plan_;
  const VerifyOptions& o_;
  const Control& ctl_;
  std::function<void(std::uint64_t)> progress_;
  VerifyReport& rep_;
  Shifter sh_;
  Redactor red_;
  si8 lo_ = 0, hi_ = 0;
  std::set<Uuid> src_uuids_;
  bool dst_encrypted_ = false;
  crypto::AccessKeys src_keys_, dst_keys_;
};

std::string finding_key(const Finding& f) {
  return f.check_id + " | " + f.channel + " | segment " + std::to_string(f.segment_number) +
         " | " + f.field;
}

VerifyReport verify_impl(const std::string& src_path, const SessionSource& src,
                         const SourcePlan& plan, const std::string& dst_path, si8 shift,
                         const VerifyOptions& o, const Control& ctl,
                         std::function<void(std::uint64_t)> progress) {
  VerifyReport rep;
  const auto dsrc = open_session_source(dst_path);
  const SessionSource& dst = *dsrc;
  const auto dlayout = detail::discover_layout(dst);

  // Channels: the copy's must be a subset of the source's.
  std::set<std::string> dch;
  for (const auto& c : dlayout.channels) dch.insert(c.name);
  for (const auto& c : dlayout.record_only_channels) dch.insert(c);
  std::set<std::string> excluded;
  for (const auto& c : plan.all_channels)
    if (!dch.count(c)) excluded.insert(c);
  rep.channels.assign(dch.begin(), dch.end());
  rep.excluded_channels.assign(excluded.begin(), excluded.end());

  Verifier v(src, dst, plan, shift, o, ctl, std::move(progress), rep);
  for (const auto& c : dch)
    if (!std::binary_search(plan.all_channels.begin(), plan.all_channels.end(), c))
      v.problem(c, "channel", "is not a channel of the source");
  if (!dlayout.video_channels.empty() || !dlayout.incomplete_channels.empty())
    v.problem(dst_path, "layout", "the copy holds video or incomplete channel folders");

  // The shift floor, re-checked: below it the leak window cannot tell a
  // leak from a legitimately shifted time.
  if (plan.has_times) {
    const si8 span = plan.last - plan.first;
    const si8 mag = shift == INT64_MIN ? INT64_MAX : (shift < 0 ? -shift : shift);
    if (mag < span + DAY_US)
      v.problem(dst_path, "shift", "|shift| is below the session's time span plus one day");
  }

  const std::string dname = dst.session_name();
  {
    // The anonymized_name the copy carries is its pseudonym, not a leak.
    std::string pseudonym;
    if (!dlayout.channels.empty() && !dlayout.channels.front().segments.empty()) {
      const auto uh = dst.read_range(dlayout.channels.front().segments.front().tmet, 0, UH);
      pseudonym = read_string(uh, UH_ANON_NAME, UH_NAME_BYTES);
    }
    v.set_redactor(
        build_redactor(plan, dname, o.clear_subject, o.clear_location, excluded, pseudonym));
  }

  // Validator: nothing new on the copy.
  {
    ValidateOptions vo;
    vo.password = o.password;
    vo.exact_difference_bytes = false;
    Control inner;
    inner.cancel = ctl.cancel;
    const Report sr = validate_session(src_path, vo, inner);
    vo.password = o.output_password;
    const Report dr = validate_session(dst_path, vo, inner);
    std::set<std::string> known;
    for (const auto& f : sr.findings) known.insert(finding_key(f));
    for (const auto& f : dr.findings) {
      if (f.severity == Severity::Info) continue;
      const std::string key = finding_key(f);
      if (known.count(key))
        rep.inherited_findings.push_back(key);
      else
        v.problem(f.path, f.check_id, "the validator reports on the copy: " + f.message);
    }
    for (const auto& s : dr.skipped)
      v.problem(s.path, "validator", "could not check a segment of the copy: " + s.reason);
  }

  // Segments.
  std::map<std::string, const detail::LayoutChannel*> schan;
  for (const auto& c : plan.layout.channels) schan[c.name] = &c;
  for (const auto& dc : dlayout.channels) {
    const auto it = schan.find(dc.name);
    if (it == schan.end()) {
      v.problem(dc.name, "channel", "has segments the source does not");
      continue;
    }
    const auto& sc = *it->second;
    if (sc.segments.size() != dc.segments.size())
      v.problem(dc.name, "segments", "the copy holds " + std::to_string(dc.segments.size()) +
                                         ", the source " + std::to_string(sc.segments.size()));
    for (const auto& ds : dc.segments) {
      const auto ss = std::find_if(sc.segments.begin(), sc.segments.end(),
                                   [&](const detail::LayoutSegment& x) {
                                     return leaf(x.dir()) == leaf(ds.dir());
                                   });
      if (ss == sc.segments.end()) {
        v.problem(ds.dir(), "segment", "has no counterpart in the source");
        continue;
      }
      v.check_tmet(ss->tmet, ds.tmet);
      v.check_segment_data(*ss, ds);
      ++rep.segments;
      const bool s_rec = src.exists(ss->rdat()), d_rec = dst.exists(ds.rdat());
      if (s_rec && d_rec) v.check_records(ss->rdat(), ds.rdat(), excluded);
      else if (s_rec != d_rec)
        v.problem(ds.dir(), "segment records", "present in only one of source and copy");
    }
  }
  // Channel-level and session-level records.
  for (const auto& c : dch) {
    const std::string rel = c + ".timd/" + c + ".rdat";
    const bool s_rec = src.exists(rel), d_rec = dst.exists(rel);
    if (s_rec && d_rec) v.check_records(rel, rel, excluded);
    else if (s_rec != d_rec) v.problem(rel, "channel records", "present in only one of source and copy");
  }
  {
    std::string amb_s, amb_d;
    const std::string s_rel = detail::session_record_file(src, amb_s);
    const std::string d_rel = detail::session_record_file(dst, amb_d);
    if (!s_rel.empty() && !d_rel.empty()) {
      if (d_rel != dname + ".rdat")
        v.problem(d_rel, "session records", "are not named after the copy's session");
      v.check_records(s_rel, d_rel, excluded);
    } else if (s_rel.empty() != d_rel.empty() || !amb_d.empty()) {
      v.problem(dst_path, "session records", "present in only one of source and copy");
    }
  }
  rep.ok = rep.problems.empty();
  return rep;
}

}  // namespace

// ---------------------------------------------------------------------------

VerifyReport verify_anonymized(const std::string& src, const std::string& dst, si8 shift_us,
                               const VerifyOptions& options, const Control& ctl) {
  const auto source = open_session_source(src);
  Control inner;
  inner.cancel = ctl.cancel;
  const SourcePlan plan = plan_source(*source, src, options.password, inner);
  std::uint64_t total = std::max<std::uint64_t>(1, 2 * plan.summary.total_bytes);
  detail::ProgressReporter progress(ctl, total);
  progress.start();
  auto rep = verify_impl(src, *source, plan, dst, shift_us, options, ctl,
                         progress.active() ? std::function<void(std::uint64_t)>(
                                                 [&](std::uint64_t n) { progress.add(n); })
                                           : std::function<void(std::uint64_t)>());
  ctl.check();
  progress.finish();
  return rep;
}

AnonymizeReport anonymize_session(const std::string& src_path, const std::string& dst_path,
                                  const AnonymizeOptions& o, const Control& ctl) {
  // ---- options -----------------------------------------------------------
  if (o.shift_us == 0) throw std::invalid_argument("shift_us must not be 0");
  if (!o.channels.empty() && !o.exclude_channels.empty())
    throw std::invalid_argument("pass channels or exclude_channels, not both");
  if (o.text == RecordTextPolicy::Callback && !o.text_fn)
    throw std::invalid_argument("the callback text policy needs a text function");
  if (o.output_password_1.empty() != o.output_password_2.empty())
    throw std::invalid_argument(
        "output passwords come in pairs: give both the level-1 and the level-2 password, or "
        "neither (MEF has no level-1-only files)");
  if (!o.output_password_1.empty() && o.output_password_1 == o.output_password_2)
    throw std::invalid_argument("the level-1 and level-2 output passwords must differ");

  // ---- paths -------------------------------------------------------------
  if (!path_has_suffix(dst_path, ".mefd"))
    throw IoError("the copy must be a directory ending in .mefd: " + dst_path);
  fs::path dst(dst_path);
  if (dst.filename().empty()) dst = dst.parent_path();
  if (fs::exists(dst)) throw IoError("the destination already exists: " + dst.string());
  const fs::path partial = dst.parent_path() / (dst.filename().string() + ".partial");
  if (fs::exists(partial))
    throw IoError("a partial copy is in the way (an earlier run that did not finish, or one "
                  "still running): " + partial.string() + ". Remove it if no copy is running.");
  if (!fs::exists(src_path)) throw IoError("the source does not exist: " + src_path);
  if (path_inside(dst, src_path) || path_inside(src_path, dst))
    throw IoError("the destination must not be inside the source: " + dst.string());

  const auto source = open_session_source(src_path);
  const SessionSource& src = *source;

  // ---- what the source is ------------------------------------------------
  // Inner calls get the cancel token but not the progress callback: their
  // own (0..total) reports would interleave with this call's.
  Control inner;
  inner.cancel = ctl.cancel;
  SourcePlan plan = plan_source(src, src_path, o.password, inner);
  {
    if (plan.summary.encryption.access != "full")
      throw PasswordError("the session is encrypted and the password does not open all of it: "
                          "metadata section 3 holds the recording-time offset and the subject "
                          "fields, so anonymizing needs the level-2 password");
    std::string why;
    for (const auto& w : plan.summary.warnings)
      if (w.blocks_anonymize) why += "\n  - " + w.code + ": " + w.message;
    if (!why.empty())
      throw WriteConflictError("this session cannot be anonymized safely:" + why);
  }

  // ---- channel selection -------------------------------------------------
  std::set<std::string> known(plan.all_channels.begin(), plan.all_channels.end());
  std::set<std::string> kept, excluded;
  for (const auto& c : o.channels)
    if (!known.count(c)) throw std::invalid_argument("unknown channel: " + c);
  for (const auto& c : o.exclude_channels)
    if (!known.count(c)) throw std::invalid_argument("unknown channel: " + c);
  for (const auto& c : plan.all_channels) {
    const bool in = o.channels.empty()
                        ? std::find(o.exclude_channels.begin(), o.exclude_channels.end(), c) ==
                              o.exclude_channels.end()
                        : std::find(o.channels.begin(), o.channels.end(), c) != o.channels.end();
    (in ? kept : excluded).insert(c);
  }
  if (kept.empty()) throw std::invalid_argument("the selection leaves no channel to copy");

  // ---- the shift ---------------------------------------------------------
  Shifter sh{plan.rto, o.shift_us};
  if (plan.has_times) {
    const si8 span = plan.last - plan.first;
    const si8 mag = o.shift_us == INT64_MIN ? INT64_MAX : (o.shift_us < 0 ? -o.shift_us : o.shift_us);
    if (mag < span + DAY_US)
      throw std::invalid_argument(
          "|shift_us| (" + std::to_string(mag) + ") must be at least the session's time span "
          "plus one day (" + std::to_string(span + DAY_US) + " us), so that no shifted time can "
          "fall inside the original range");
    if (o.shift_us < 0 && plan.first + o.shift_us <= 0)
      throw std::invalid_argument("the shift moves the first time (" + std::to_string(plan.first) +
                                  " uUTC) to 0 or below; shifted times must stay above 0");
  }

  // ---- names -------------------------------------------------------------
  const std::string new_name = session_name_from_path(dst.string());
  if (!o.rename && new_name != src.session_name())
    throw std::invalid_argument("rename=False keeps the source's session name, so the "
                                "destination folder must be named after it");
  const std::string pseudonym = o.pseudonym.empty() ? new_name : o.pseudonym;
  if (pseudonym.size() > UH_NAME_BYTES - 1 || new_name.size() > UH_NAME_BYTES - 1)
    throw std::invalid_argument("the session name and pseudonym must fit 255 bytes");
  if (!o.rename) plan.session_names.clear();  // the name is kept on purpose
  const Redactor red =
      build_redactor(plan, new_name, o.clear_subject, o.clear_location, excluded, pseudonym);

  OutputKeys out;
  if (!o.output_password_1.empty()) {
    out.encrypt = true;
    out.vf = crypto::make_validation_fields(o.output_password_1, o.output_password_2);
    out.keys = crypto::validate_password(o.output_password_2, out.vf.level1, out.vf.level2);
    if (!out.keys.level1_key || !out.keys.level2_key)
      throw std::logic_error("output keys could not be derived");
  }

  AnonymizeReport rep;
  rep.destination = dst.string();
  rep.session_name = new_name;
  rep.pseudonym = pseudonym;
  rep.shift_us = o.shift_us;
  rep.source_recording_time_offset = plan.rto;
  rep.channels_kept.assign(kept.begin(), kept.end());
  rep.channels_excluded.assign(excluded.begin(), excluded.end());
  rep.output_encrypted = out.encrypt;
  if (plan.rto != 0)
    rep.notes.push_back("the source used meflib's recording-time offset; its times were read as "
                        "true UTC and the copy has recording_time_offset = 0");

  const std::uint64_t total = std::max<std::uint64_t>(1, 3 * plan.summary.total_bytes);
  detail::ProgressReporter progress(ctl, total);
  std::function<void(std::uint64_t)> on_bytes;
  if (progress.active()) on_bytes = [&](std::uint64_t n) { progress.add(n); };

  const fs::path root = partial / dst.filename();
  try {
    progress.start();
    fs::create_directories(root);
    Anonymizer an(src, o, sh, red, new_name, pseudonym, excluded, out, ctl, on_bytes, rep);

    std::set<std::string> handled;  // source-relative paths copied
    std::map<std::string, const detail::LayoutChannel*> data;
    for (const auto& c : plan.layout.channels) data[c.name] = &c;
    for (const auto& ch : kept) {
      const fs::path cdir = root / (ch + ".timd");
      fs::create_directory(cdir);
      const std::string crec = ch + ".timd/" + ch + ".rdat";
      if (src.exists(crec)) {
        an.copy_records(crec, cdir / (ch + ".rdat"), RecordLevel::Channel, ch, -1);
        handled.insert(crec);
        handled.insert(ridx_of(crec));
      }
      const auto it = data.find(ch);
      if (it == data.end()) continue;  // record-only channel
      for (const auto& seg : it->second->segments) {
        const fs::path sdir = cdir / leaf(seg.dir());
        fs::create_directory(sdir);
        an.copy_tmet(seg.tmet, sdir / leaf(seg.tmet), ch);
        an.copy_segment_data(seg, sdir, ch);
        handled.insert(seg.tmet);
        handled.insert(seg.tidx);
        handled.insert(seg.tdat);
        if (src.exists(seg.rdat())) {
          an.copy_records(seg.rdat(), sdir / leaf(seg.rdat()), RecordLevel::Segment, ch,
                          seg.number);
          handled.insert(seg.rdat());
          handled.insert(ridx_of(seg.rdat()));
        }
        ++rep.segments;
      }
    }
    {
      std::string ambiguity;
      const std::string srec = detail::session_record_file(src, ambiguity);
      if (!ambiguity.empty()) throw WriteConflictError(ambiguity);
      if (!srec.empty()) {
        an.copy_records(srec, root / (new_name + ".rdat"), RecordLevel::Session, "", -1);
        handled.insert(srec);
        handled.insert(ridx_of(srec));
      }
    }
    // What the copy leaves out: anything outside the MEF layout (and the
    // excluded channels, which are reported separately).
    std::function<void(const std::string&)> walk = [&](const std::string& rel) {
      for (const auto& e : src.list_dir(rel)) {
        const std::string p = rel.empty() ? e.name : rel + "/" + e.name;
        if (e.is_dir) {
          if (rel.empty() && e.name.ends_with(".timd") &&
              excluded.count(e.name.substr(0, e.name.size() - 5)))
            continue;
          walk(p);
        } else if (!handled.count(p)) {
          rep.not_copied.push_back(p);
        }
      }
    };
    walk("");

    // Durable before it is published: the rename below makes it a session.
    for (const auto& f : an.written) detail::fsync_file_or_throw(f);

    VerifyOptions vo;
    vo.password = o.password;
    vo.output_password = o.output_password_2;
    vo.clear_subject = o.clear_subject;
    vo.clear_location = o.clear_location;
    vo.clear_descriptions = o.clear_descriptions;
    rep.verification =
        verify_impl(src_path, src, plan, root.string(), o.shift_us, vo, ctl, on_bytes);
    if (!rep.verification.ok) {
      std::string msg = "the copy failed verification, so it was not published (" +
                        std::to_string(rep.verification.problems.size()) + " problem(s)):";
      const std::size_t show = std::min<std::size_t>(rep.verification.problems.size(), 10);
      for (std::size_t i = 0; i < show; ++i) {
        const auto& p = rep.verification.problems[i];
        msg += "\n  - " + p.location + ": " + p.field + " " + p.message;
      }
      throw WriteConflictError(msg);
    }
    ctl.check();  // the last point a cancel can stop it
    progress.finish();
    if (fs::exists(dst)) throw IoError("the destination appeared while copying: " + dst.string());
    fs::rename(root, dst);
    detail::fsync_directory(dst.parent_path().empty() ? fs::path(".") : dst.parent_path());
  } catch (...) {
    std::error_code ec;
    fs::remove_all(partial, ec);
    throw;
  }
  std::error_code ec;
  fs::remove_all(partial, ec);
  rep.destination = dst.string();
  return rep;
}

}  // namespace mef3io
