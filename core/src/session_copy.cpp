// mef3io — the streaming session copy, with two sets of rules:
// anonymize_session / verify_anonymized (#70, anonymize.hpp) and
// transform_session / verify_transform (#73, transform.hpp). ONE engine: the
// pre-flight, the copy pass, the verification and the publish are shared; the
// rules decide what changes. `Rules::anonymize()` = remove identity, clear
// the regions other writers fill, unlink the copy, and either keep every time
// as stored (the default, `Rules::keep_times()`: the transform's verbatim
// time path) or shift every time (`Rules::shifting()`); otherwise
// (a transform) every time, region and record body is copied verbatim and
// only the channels, passwords, name and edited metadata differ.
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
#include "operation_marker.hpp"
#include "mef3io/anonymize.hpp"
#include "mef3io/transform.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <exception>
#include <filesystem>
#include <functional>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
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
constexpr std::size_t UH_START = 16, UH_END = 24, UH_CHANNEL_NAME = 52, UH_SESSION_NAME = 308, UH_ANON_NAME = 564,
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

// ---------------------------------------------------------------------------
// Refusals. Every refusal is thrown as its usual exception type with the
// AnonymizeError mixin carrying a stable code (docs/anonymization.md).
// ---------------------------------------------------------------------------

template <class Base>
class CodedError final : public Base, public AnonymizeError {
 public:
  explicit CodedError(AnonymizeRefusal r) : Base(r.message), AnonymizeError(std::move(r)) {}
};

[[noreturn]] void raise(AnonymizeRefusal r) {
  switch (r.kind) {
    case RefusalKind::InvalidArgument: throw CodedError<std::invalid_argument>(std::move(r));
    case RefusalKind::Io: throw CodedError<IoError>(std::move(r));
    case RefusalKind::WriteConflict: throw CodedError<WriteConflictError>(std::move(r));
    case RefusalKind::Password: throw CodedError<PasswordError>(std::move(r));
    case RefusalKind::Format: throw CodedError<FormatError>(std::move(r));
    case RefusalKind::Crc: throw CodedError<CrcError>(std::move(r));
  }
  throw std::logic_error("unknown refusal kind");
}

[[noreturn]] void refuse(RefusalKind kind, std::string code, std::string message,
                         std::string channel = {}, std::string field = {}) {
  raise(AnonymizeRefusal{std::move(code), std::move(message), kind, std::move(channel),
                         std::move(field), {}});
}

/// The refusal an exception stands for: its own when it carries one, else
/// the generic code of its type (`fallback` names the context). Returns
/// nothing for an exception that is not a refusal (Cancelled is not a
/// std::exception and never reaches here; a logic_error is a bug).
std::optional<AnonymizeRefusal> refusal_of(std::exception_ptr p, const char* format_code,
                                           const char* crc_code, const char* io_code) {
  try {
    std::rethrow_exception(p);
  } catch (const AnonymizeError& e) {
    return e.refusal();
  } catch (const CrcError& e) {
    return AnonymizeRefusal{crc_code, e.what(), RefusalKind::Crc, {}, {}, {}};
  } catch (const FormatError& e) {
    return AnonymizeRefusal{format_code, e.what(), RefusalKind::Format, {}, {}, {}};
  } catch (const PasswordError& e) {
    return AnonymizeRefusal{"password-wrong", e.what(), RefusalKind::Password, {}, {}, {}};
  } catch (const IoError& e) {
    return AnonymizeRefusal{io_code, e.what(), RefusalKind::Io, {}, {}, {}};
  } catch (const WriteConflictError& e) {
    return AnonymizeRefusal{"write-conflict", e.what(), RefusalKind::WriteConflict, {}, {}, {}};
  } catch (const std::invalid_argument& e) {
    return AnonymizeRefusal{"invalid-argument", e.what(), RefusalKind::InvalidArgument, {}, {},
                            {}};
  } catch (...) {
    return std::nullopt;
  }
}

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
        refuse(RefusalKind::Format, "time-overflow",
               what + ": the stored time is out of range");
      return -s + r;
    }
    if (rto != 0 && s <= rto)
      refuse(RefusalKind::WriteConflict, "time-before-rto",
          what + " is stored non-negative at or before the session's recording-time offset. Such a value cannot be told "
          "apart from a time stored relative to the offset (the legacy convention, "
          "docs/anonymization.md), so where it lies is unknown and it is not copied.");
    return s;
  }

  si8 operator()(si8 s, const std::string& what, bool body = false) const {
    if (is_sentinel(s, body)) return s;
    const si8 a = absolute(s, what);
    if ((shift > 0 && a > INT64_MAX - shift) || (shift < 0 && a + shift <= 0))
      refuse(RefusalKind::InvalidArgument, "shift-not-positive",
             "the shift moves " + what +
                 " to 0 or below, or out of range; shifted times must stay above 0",
             {}, "shift_us");
    const si8 n = a + shift;
    return s < 0 ? -n : n;
  }
};

/// Which copy this is. `anon` set: anonymize_session's rules. Unset: a
/// transform (times, regions and record bodies verbatim; `edits` applied).
/// An anonymized copy either SHIFTS every time (`anon->shift_us` set) or
/// KEEPS every time as stored (unset, the default): the same verbatim time
/// path as a transform, with the source's recording-time offset kept so the
/// stored times read back to the same instants.
struct Rules {
  const CopyOptions* copy = nullptr;
  const AnonymizeOptions* anon = nullptr;
  const MetadataEdits* edits = nullptr;  ///< transform only (may be empty)
  bool anonymize() const { return anon != nullptr; }
  bool shifting() const { return anon != nullptr && anon->shift_us.has_value(); }
  bool keep_times() const { return anon != nullptr && !anon->shift_us.has_value(); }
};

// ---------------------------------------------------------------------------
// Text. Two matchers, deliberately unrelated:
//   * Redactor (the REWRITE): replaces the names a copy must not carry, never
//     inside a kept channel's name (which is retained verbatim by design).
//     How a value is matched depends on what it is:
//       - 4+ characters, not purely numeric: anywhere, as a substring;
//       - shorter than 4 characters: whole tokens only (a token is a maximal
//         run of letters and digits; '_' and everything else end it), so
//         "rec" does not turn "recorded" into nonsense;
//       - purely numeric: whole numbers only (a number is a maximal run of
//         digits, ':' and '.'), so ID "12" leaves "12:30" and "1200";
//       - excluded channel names: whole tokens.
//   * Scanner (the CHECK, in verify): its own code, never the Redactor's.
//       - A field the copy REWRITES: a plain case-insensitive substring
//         search. An occurrence is excused only when it lies entirely inside
//         the copy's own name or pseudonym (a NOTE), inside a kept channel's
//         name (that name's own rule decides), or, for a numeric value of
//         fewer than 4 digits, inside a longer number (a NOTE). Everything
//         else refuses the copy.
//       - A kept channel's name (and what derives from it: path components,
//         the UH channel_name, Seiz entries naming it): split into tokens;
//         REFUSED when a token equals an identity value of 3+ characters,
//         otherwise any overlap is a NOTE. Channel names are needed by every
//         study and routinely hold short codes ("LAH12", "P3", "EEG1").
// ---------------------------------------------------------------------------

bool is_alnum(char c) {
  const auto u = static_cast<unsigned char>(c);
  return u >= 0x80 || std::isalnum(u);
}
bool is_digit(char c) { return c >= '0' && c <= '9'; }
// ':' and '.' join digits into one number ("12:30", "12.5"); ',' does not, so
// "1,123" holds the number 123.
bool number_char(char c) { return is_digit(c) || c == ':' || c == '.'; }
char fold(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

/// Lower case of a Latin-1 Supplement / Latin Extended-A code point (the
/// Latin scripts of most European names); anything else unchanged. Every
/// mapping stays in U+0080..U+017F, so its UTF-8 length (2 bytes) is kept.
unsigned lower_latin(unsigned cp) {
  if (cp >= 0xC0 && cp <= 0xDE && cp != 0xD7) return cp + 0x20;
  if (cp >= 0x100 && cp <= 0x137 && cp % 2 == 0 && cp != 0x130) return cp + 1;
  if (cp >= 0x139 && cp <= 0x148 && cp % 2 == 1) return cp + 1;
  if (cp >= 0x14A && cp <= 0x177 && cp % 2 == 0) return cp + 1;
  if (cp == 0x178) return 0xFF;
  if (cp >= 0x179 && cp <= 0x17E && cp % 2 == 1) return cp + 1;
  return cp;
}

/// Case-fold `s`: ASCII, Latin-1 Supplement and Latin Extended-A. Length-
/// preserving (the Redactor replaces at the positions it finds in the folded
/// copy); bytes that are not valid 2-byte UTF-8 pass through unchanged.
std::string folded(std::string s) {
  for (std::size_t i = 0; i < s.size(); ++i) {
    const auto b = static_cast<unsigned char>(s[i]);
    if (b < 0x80) {
      s[i] = fold(s[i]);
      continue;
    }
    if (b >= 0xC2 && b <= 0xC5 && i + 1 < s.size() &&
        (static_cast<unsigned char>(s[i + 1]) & 0xC0) == 0x80) {
      const unsigned cp = ((b & 0x1Fu) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3Fu);
      const unsigned lc = lower_latin(cp);
      s[i] = static_cast<char>(0xC0 | (lc >> 6));
      s[i + 1] = static_cast<char>(0x80 | (lc & 0x3F));
      ++i;
    }
  }
  return s;
}

/// The match fold: case-folded as above, then the diacritics of those Latin
/// letters removed, so "Muller" and "Jiri" are found for "Müller" and
/// "Jiří". The scanner (the check) and the Redactor (the removal) both match
/// in this space, so the removal covers what the check looks for (#115).
/// Not length-preserving: `start` / `end`, when given, receive for each
/// output byte the input byte range of the character it came from, so a
/// match can be replaced in the original text.
std::string scan_folded(const std::string& in, std::vector<std::size_t>* start = nullptr,
                        std::vector<std::size_t>* end = nullptr) {
  // Base letters of U+0100..U+017F, in order (ligatures handled below).
  static const char* const EXT_A =
      "aaaaaaccccccccddddeeeeeeeeeegggggggghhhhiiiiiiiiiiiijjkkklllllll"
      "lllnnnnnnnnnoooooooorrrrrrssssssssttttttuuuuuuuuuuuuwwyyyzzzzzzs";
  static_assert(sizeof("aaaaaaccccccccddddeeeeeeeeeegggggggghhhhiiiiiiiiiiiijjkkklllllll"
                       "lllnnnnnnnnnoooooooorrrrrrssssssssttttttuuuuuuuuuuuuwwyyyzzzzzzs") ==
                0x80 + 1);
  static const char* const LATIN1 =  // U+00E0..U+00FF (after folding)
      "aaaaaa\x01" "ceeeeiiiidnooooo\x02ouuuuy\x03y";
  const std::string f = folded(in);  // length-preserving: f's offsets are in's
  std::string out;
  out.reserve(f.size());
  if (start) start->clear();
  if (end) end->clear();
  auto emit = [&](const char* text, std::size_t from, std::size_t to) {
    for (; *text; ++text) {
      out += *text;
      if (start) start->push_back(from);
      if (end) end->push_back(to);
    }
  };
  for (std::size_t i = 0; i < f.size(); ++i) {
    const auto b = static_cast<unsigned char>(f[i]);
    if (b >= 0xC2 && b <= 0xC5 && i + 1 < f.size() &&
        (static_cast<unsigned char>(f[i + 1]) & 0xC0) == 0x80) {
      const unsigned cp = ((b & 0x1Fu) << 6) | (static_cast<unsigned char>(f[i + 1]) & 0x3Fu);
      const char* base = nullptr;
      char one[2] = {0, 0};
      if (cp == 0xDF) base = "ss";
      else if (cp == 0xE6) base = "ae";
      else if (cp == 0xFE) base = "th";
      else if (cp == 0x132 || cp == 0x133) base = "ij";
      else if (cp == 0x152 || cp == 0x153) base = "oe";
      else if (cp >= 0xE0 && cp <= 0xFF && cp != 0xF7) base = (one[0] = LATIN1[cp - 0xE0], one);
      else if (cp >= 0x100 && cp <= 0x17F) base = (one[0] = EXT_A[cp - 0x100], one);
      if (base) {
        emit(base, i, i + 2);
        ++i;
        continue;
      }
    }
    const char c[2] = {f[i], 0};
    if (f[i] == 0) {  // emit() stops at NUL: keep it as its own byte
      out += '\0';
      if (start) start->push_back(i);
      if (end) end->push_back(i + 1);
      continue;
    }
    emit(c, i, i + 1);
  }
  return out;
}

/// A stored value as a name: trimmed, inner whitespace collapsed to one
/// space (fixed-width fields are often space-padded).
std::string normalized(const std::string& s) {
  std::string out;
  bool space = false;
  for (char c : s) {
    if (std::isspace(static_cast<unsigned char>(c))) {
      space = !out.empty();
      continue;
    }
    if (space) out += ' ';
    space = false;
    out += c;
  }
  return out;
}

/// The words of a name: letter runs of 3+ characters (non-ASCII bytes count
/// as letters; a character is counted once however many bytes it has).
std::vector<std::string> name_words(const std::string& v) {
  std::vector<std::string> out;
  auto letter = [](unsigned char c) { return c >= 0x80 || std::isalpha(c); };
  for (std::size_t i = 0; i < v.size();) {
    if (!letter(static_cast<unsigned char>(v[i]))) {
      ++i;
      continue;
    }
    std::size_t j = i, chars = 0;
    while (j < v.size() && letter(static_cast<unsigned char>(v[j]))) {
      if ((static_cast<unsigned char>(v[j]) & 0xC0) != 0x80) ++chars;
      ++j;
    }
    if (chars >= 3) out.push_back(v.substr(i, j - i));
    i = j;
  }
  return out;
}

/// What to search for, for one subject or location value: the whole value
/// (normalized) and each of its words ("Roe, Jane" -> "Roe, Jane", "Roe",
/// "Jane").
/// Digit runs of 3+ digits in a subject ID ("EEG-0042" -> "0042"), searched
/// as whole numbers: an ID's number is what identifies, its letters usually
/// an institution's prefix.
std::vector<std::string> id_digit_runs(const std::string& value) {
  std::vector<std::string> out;
  for (std::size_t i = 0; i < value.size();) {
    if (!is_digit(value[i])) {
      ++i;
      continue;
    }
    std::size_t j = i;
    while (j < value.size() && is_digit(value[j])) ++j;
    if (j - i >= 3 && j - i < value.size()) out.push_back(value.substr(i, j - i));
    i = j;
  }
  return out;
}

std::vector<std::string> identity_variants(const std::string& value, bool words = true) {
  std::vector<std::string> out;
  const std::string whole = normalized(value);
  if (whole.empty()) return out;
  out.push_back(whole);
  if (words)
    for (const auto& w : name_words(whole))
    if (std::find(out.begin(), out.end(), w) == out.end()) out.push_back(w);
  return out;
}

std::string trimmed(const std::string& s) {
  std::size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return s.substr(a, b - a);
}
bool all_digits(const std::string& s) {
  return !s.empty() && std::all_of(s.begin(), s.end(), is_digit);
}
bool all_letters(const std::string& s) {
  return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) {
    const auto u = static_cast<unsigned char>(c);
    return u >= 0x80 || std::isalpha(u);
  });
}

/// Maximal alphanumeric runs of `h` ('_', '-' and everything else split).
std::vector<std::string> alnum_tokens(const std::string& h) {
  std::vector<std::string> tokens;
  for (std::size_t i = 0; i < h.size();) {
    if (!is_alnum(h[i])) {
      ++i;
      continue;
    }
    std::size_t j = i;
    while (j < h.size() && is_alnum(h[j])) ++j;
    tokens.push_back(h.substr(i, j - i));
    i = j;
  }
  return tokens;
}

/// A token split further where letters meet digits: "roe1" -> "roe", "1".
std::vector<std::string> letter_digit_parts(const std::string& token) {
  std::vector<std::string> parts;
  std::size_t i = 0;
  while (i < token.size()) {
    const bool d = is_digit(token[i]);
    std::size_t j = i;
    while (j < token.size() && is_digit(token[j]) == d) ++j;
    parts.push_back(token.substr(i, j - i));
    i = j;
  }
  return parts;
}
/// How a channel name is compared for exclusion: trimmed, case-folded.
std::string channel_key(const std::string& s) { return folded(trimmed(s)); }

using Span = std::pair<std::size_t, std::size_t>;  // [first, last)

/// Every occurrence of `needle` (already folded) in `hay` (already folded).
std::vector<Span> occurrences(const std::string& hay, const std::string& needle) {
  std::vector<Span> out;
  if (needle.empty()) return out;
  for (std::size_t p = hay.find(needle); p != std::string::npos; p = hay.find(needle, p + 1))
    out.push_back({p, p + needle.size()});
  return out;
}

bool covered_by(const Span& s, const std::vector<Span>& by) {
  for (const auto& b : by)
    if (b.first <= s.first && s.second <= b.second) return true;
  return false;
}

/// The maximal number-like run (digits, ':' and '.') around [a, b), with
/// punctuation trimmed from both ends.
Span number_run(const std::string& h, std::size_t a, std::size_t b) {
  while (a > 0 && number_char(h[a - 1])) --a;
  while (b < h.size() && number_char(h[b])) ++b;
  while (a < b && !is_digit(h[a])) ++a;
  while (b > a && !is_digit(h[b - 1])) --b;
  return {a, b};
}

/// The maximal alphanumeric token around [a, b).
Span token_run(const std::string& h, std::size_t a, std::size_t b) {
  while (a > 0 && is_alnum(h[a - 1])) --a;
  while (b < h.size() && is_alnum(h[b])) ++b;
  return {a, b};
}

struct Redactor {
  enum class Mode { Substring, Token, Number };
  struct Rule {
    std::string needle;  // scan_folded: case and Latin diacritics folded
    std::string replacement;
    Mode mode = Mode::Substring;
  };
  std::vector<Rule> rules;
  /// Kept channel names, case-folded (accents KEPT), longest first. A kept
  /// name is protected only where the text spells it exactly (but for
  /// case): an accent-free look-alike ("PER" in "Pérez") is not a channel.
  std::vector<std::string> protect;

  /// `name_like`: an excluded channel name, matched as a whole token.
  void add(const std::string& value, const std::string& replacement, bool name_like) {
    if (value.size() < 2) return;  // one byte would match nearly any text
    const std::string n = scan_folded(value);
    if (n.empty() || n == scan_folded(replacement)) return;
    for (const auto& r : rules)
      if (r.needle == n) return;
    // The mode by the value as stored (bytes, case-folded): "Zoë" (4 bytes)
    // is searched anywhere, as before #115; only shorter values are words.
    Mode mode = Mode::Substring;
    if (all_digits(n)) mode = Mode::Number;
    else if (name_like || folded(value).size() < 4) mode = Mode::Token;
    rules.push_back({n, replacement, mode});
    std::stable_sort(rules.begin(), rules.end(),
                     [](const Rule& a, const Rule& b) { return a.needle.size() > b.needle.size(); });
  }

  /// Every rule applied in turn, each in ONE pass: matched in the scanner's
  /// folded space (so "Muller" goes for "Müller") and replaced in the
  /// original text, whole characters at a time.
  std::string apply(std::string text) const {
    for (const auto& r : rules) {
      std::vector<std::size_t> start, end;
      const std::string h = scan_folded(text, &start, &end);
      if (h.find(r.needle) == std::string::npos) continue;
      // Kept names, located in the ORIGINAL text (case-folded only).
      std::vector<Span> keep;
      const std::string f = folded(text);
      for (const auto& k : protect)
        for (const auto& sp : occurrences(f, k)) keep.push_back(sp);
      std::sort(keep.begin(), keep.end());
      std::size_t ki = 0;  // kept spans before it end before every later match
      std::string out;
      std::size_t copied = 0;  // bytes of `text` already in `out`
      for (std::size_t p = h.find(r.needle); p != std::string::npos;) {
        const std::size_t a = start[p], b = end[p + r.needle.size() - 1];
        bool ok = a >= copied;
        // A match overlapping a kept name (inside it or partly) stays.
        // Matches only move forward, so spans ending before one are passed.
        while (ki < keep.size() && keep[ki].second <= a) ++ki;
        for (std::size_t j = ki; j < keep.size() && keep[j].first < b && ok; ++j)
          if (a < keep[j].second) ok = false;
        const Span s{p, p + r.needle.size()};
        if (ok && r.mode == Mode::Token) ok = token_run(h, s.first, s.second) == s;
        if (ok && r.mode == Mode::Number) ok = number_run(h, s.first, s.second) == s;
        if (!ok) {
          p = h.find(r.needle, p + 1);
          continue;
        }
        out.append(text, copied, a - copied);
        out += r.replacement;
        copied = b;
        p = h.find(r.needle, s.second);
      }
      out.append(text, copied, std::string::npos);
      text = std::move(out);
    }
    return text;
  }
};

/// verify's text check. Never shares code with the Redactor's matching.
struct Scanner {
  struct Needle {
    std::string value;  // case-folded
    std::string label;  // what it is (never the value)
    bool session_name = false;  // the source's session name: a looser channel rule
    /// A word of the recording location ("EEG Lab" -> "eeg", "lab"): searched
    /// in rewritten fields only, never in channel names, where such words are
    /// ordinary electrode and montage labels.
    bool location_word = false;
  };
  struct Result {
    std::string refuse;  // label of a forbidden string; "" = none
    std::string note;    // an overlap that is reported, not refused
  };
  std::vector<Needle> identity;            // must not occur anywhere
  std::vector<Needle> excluded;            // excluded channel names
  std::vector<std::string> allowed;        // the copy's own names (folded)
  std::vector<std::string> kept_channels;  // folded, longest first

  /// The rule for a kept channel's name and anything derived from it.
  Result channel(const std::string& name) const {
    const std::string h = scan_folded(name);
    const std::vector<std::string> tokens = alnum_tokens(h);
    Result r;
    for (const auto& n : identity) {
      if (n.location_word) continue;
      const std::string& v = n.value;
      const bool token_equal = std::find(tokens.begin(), tokens.end(), v) != tokens.end();
      if (token_equal && v.size() >= 3) return {n.label, ""};
      if (!n.session_name) {
        // A patient's name glued to digits or another word: "Roe1",
        // "JaneRoe". (The source session name keeps the looser rule above:
        // recorders name sessions after the montage, "EEG" / "EEG1".)
        for (const auto& t : tokens) {
          if (v.size() >= 4 && t.find(v) != std::string::npos) return {n.label, ""};
          if (v.size() >= 3 && all_letters(v))
            for (const auto& part : letter_digit_parts(t))
              if (part == v) return {n.label, ""};
        }
      }
      if (r.note.empty() && h.find(n.value) != std::string::npos)
        r.note = "contains " + n.label + " as part of a longer name, or one shorter than 3 "
                 "characters; kept, as channel names are";
    }
    for (const auto& n : excluded)
      if (r.note.empty() && h.find(n.value) != std::string::npos)
        r.note = "contains " + n.label + "; kept, as channel names are";
    return r;
  }

  /// The rule for a path component of the copy: the channel rule when it is
  /// derived from a kept channel's name (its folder, its segments, its record
  /// files), the rewritten-field rule otherwise (the session's own record
  /// file `session_name`.rdat, anything else).
  Result path(const std::string& name, const std::string& session_name,
              const std::set<std::string>& kept) const {
    const std::string base = name.substr(0, name.rfind('.'));
    if (base == session_name) return text(base);
    // "<ch>.timd", "<ch>.rdat": the base itself; "<ch>-NNNNNN.segd" and the
    // segment's files: the base without its segment number. The base is
    // tried first, so a channel "LAH-12" owns "LAH-12.timd".
    if (kept.count(base)) return channel(name);
    const auto dash = base.rfind('-');
    if (dash != std::string::npos && all_digits(base.substr(dash + 1)) &&
        kept.count(base.substr(0, dash)))
      return channel(name);
    return text(name);
  }

  /// The rule for a field the copy rewrites.
  Result text(const std::string& value) const {
    const std::string h = scan_folded(value);
    std::vector<Span> own, kept;
    std::vector<std::string> kept_of;  // the channel name each kept span is
    for (const auto& a : allowed)
      for (const auto& s : occurrences(h, a)) own.push_back(s);
    for (const auto& k : kept_channels)
      for (const auto& s : occurrences(h, k)) {
        kept.push_back(s);
        kept_of.push_back(k);
      }
    Result r;
    auto note = [&](const std::string& n) {
      if (r.note.empty()) r.note = n;
    };
    for (const auto& n : identity) {
      for (const auto& s : occurrences(h, n.value)) {
        if (n.value.size() < 3 && covered_by(s, own)) {
          note("contains " + n.label + " (shorter than 3 characters) inside the copy's own name");
          continue;
        }
        bool decided = false;
        for (std::size_t k = 0; k < kept.size() && !decided; ++k) {
          if (!(kept[k].first <= s.first && s.second <= kept[k].second)) continue;
          const Result c = channel(kept_of[k]);
          if (!c.refuse.empty()) return {c.refuse, ""};
          note("mentions a kept channel whose name contains " + n.label);
          decided = true;
        }
        if (decided) continue;
        if (all_digits(n.value) && n.value.size() < 4 &&
            number_run(h, s.first, s.second) != s) {
          note("contains " + n.label + " (a number of fewer than 4 digits) inside a longer "
               "number");
          continue;
        }
        if (all_letters(n.value) && n.value.size() < 4 &&
            token_run(h, s.first, s.second) != s) {
          note("contains " + n.label + " (fewer than 4 letters) inside a longer word");
          continue;
        }
        return {n.label, ""};
      }
    }
    for (const auto& n : excluded)
      for (const auto& s : occurrences(h, n.value))
        if (!covered_by(s, kept) && !covered_by(s, own)) return {n.label, ""};
    return r;
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
  if (bytes.size() < UH)
    refuse(RefusalKind::Format, "record-damaged", "record file smaller than its header: " + where);
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
      refuse(RefusalKind::Format, "record-damaged",
             "record file truncated after " + std::to_string(f.records.size()) +
                 " record(s): " + where);
    }
    RawRecord r;
    std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(off + 4), 4, r.type_bytes.begin());
    r.type = read_string(bytes, off + 4, 4);
    if (r.type.empty() && !known) break;
    if (r.type.empty())
      refuse(RefusalKind::Format, "record-damaged", "record without a type code in " + where);
    const ui4 stored_crc = read<ui4>(bytes, off);
    r.vmaj = read<ui1>(bytes, off + 9);
    r.vmin = read<ui1>(bytes, off + 10);
    r.enc = read<si1>(bytes, off + 11);
    const ui4 nb = read<ui4>(bytes, off + 12);
    r.time = read<si8>(bytes, off + 16);
    const std::size_t body_off = off + RHDR;
    if (nb > bytes.size() - body_off)
      refuse(RefusalKind::Format, "record-damaged",
             "record " + std::to_string(f.records.size()) + " runs past the end of " + where);
    if (stored_crc != fmt::CRC_NO_ENTRY &&
        crc::calculate(std::span<const ui1>(bytes).subspan(off + 4, RHDR - 4 + nb)) != stored_crc)
      refuse(RefusalKind::Crc, "record-crc",
             "record " + std::to_string(f.records.size()) + " (" + r.type +
                 ") fails its CRC: " + where);
    r.body.assign(bytes.begin() + static_cast<std::ptrdiff_t>(body_off),
                  bytes.begin() + static_cast<std::ptrdiff_t>(body_off + nb));
    if (r.enc > 0) {
      const auto key = key_for(f.keys, r.enc);
      if (key.empty())
        refuse(RefusalKind::Password, "record-locked",
               "a record body in " + where +
                   " is encrypted and the password does not open it (the level-2 password "
                   "opens every record)");
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
      refuse(RefusalKind::Format, "record-index-mismatch",
             "record index has a partial entry: " + src.describe(ridx_rel));
    f.has_ridx = true;
    f.ridx_uh.assign(ib.begin(), ib.begin() + UH);
    for (std::size_t o = UH; o + RIDX <= ib.size(); o += RIDX) {
      std::array<ui1, RIDX> e{};
      std::copy_n(ib.begin() + static_cast<std::ptrdiff_t>(o), RIDX, e.begin());
      f.ridx.push_back(e);
    }
    if (f.ridx.size() != f.records.size())
      refuse(RefusalKind::Format, "record-index-mismatch",
             "record index " + src.describe(ridx_rel) + " lists " +
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

/// Bytes of an LNTP body that carry data: the length field and the template.
/// mefrec.h says si4 elements; pymef writes si8 (pymef3_file.c:4159). Same
/// detection as records.cpp: si8 only when every high word sign-extends its
/// low word. Bounded by the body.
std::size_t lntp_bytes(const std::vector<ui1>& b) {
  if (b.size() < 8) return b.size();
  const si8 declared = read<si8>(b, 0);
  const std::size_t room = b.size() - 8;
  const std::size_t n = declared > 0 ? static_cast<std::size_t>(declared) : 0;
  bool wide = n > 0 && n <= room / 8;
  for (std::size_t i = 0; wide && i < n; ++i) {
    const si4 lo = read<si4>(b, 8 + i * 8), hi = read<si4>(b, 8 + i * 8 + 4);
    wide = hi == (lo < 0 ? -1 : 0);
  }
  const std::size_t width = wide ? 8 : 4;
  return 8 + std::min(n, room / width) * width;
}

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

/// How many channel entries a Seiz body holds: its declared count, bounded
/// by what the body has room for (mefrec.h:110-131). The one place every
/// reader of Seiz entries (copy, pre-flight, verification) takes it from.
std::size_t seiz_entry_count(const std::vector<ui1>& b) {
  const si4 declared = read<si4>(b, SEIZ_NCH);
  const std::size_t room = (b.size() - SEIZ_FIXED) / SEIZ_CHANNEL;
  return std::min(room, static_cast<std::size_t>(std::max<si4>(declared, 0)));
}

std::string seiz_entry_name(const std::vector<ui1>& b, std::size_t c) {
  return read_string(b, SEIZ_FIXED + c * SEIZ_CHANNEL, SEIZ_CHANNEL_NAME);
}

/// Every free-text field of a record that anonymize's text policy applies
/// to, as the copy rewrites them (rewrite_body): the variable text of
/// Note/SyLg/EDFA, the fixed text slots, and the names in a Seiz record's
/// channel list (`seiz_name`). Bodies with no known layout are not kept, so
/// they have none.
void record_text_fields(
    const RawRecord& r,
    const std::function<void(const char* field, const std::string& value, bool seiz_name)>& fn) {
  const BodyLayout* lay = layout_of(r);
  if (!lay || lay->keep_whole) return;
  if (lay->variable_text_from != SIZE_MAX) {
    const std::size_t from = lay->variable_text_from;
    fn("text", read_string(r.body, from, r.body.size() - from), false);
    return;
  }
  for (const auto& slot : lay->text)
    fn(slot.name, read_string(r.body, slot.offset, slot.bytes), false);
  if (r.type == "Seiz")
    for (std::size_t c = 0; c < seiz_entry_count(r.body); ++c)
      fn("channel name", seiz_entry_name(r.body, c), true);
}

/// A Seiz body without the entries of `excluded_keys` (channel_key()s); the
/// body as stored when it names none of them. `dropped` counts what went.
/// The copy (kept_body) and a merge's verification both take it from here.
std::vector<ui1> seiz_without(const RawRecord& r, const std::set<std::string>& excluded_keys,
                              si8* dropped = nullptr) {
  if (r.type != "Seiz" || excluded_keys.empty() || !layout_of(r)) return r.body;
  const std::size_t n = seiz_entry_count(r.body);
  std::vector<ui1> out(r.body.begin(), r.body.begin() + SEIZ_FIXED);
  si4 kept = 0;
  for (std::size_t c = 0; c < n; ++c) {
    if (excluded_keys.count(channel_key(seiz_entry_name(r.body, c)))) {
      if (dropped) ++*dropped;
      continue;
    }
    const auto first = r.body.begin() + static_cast<std::ptrdiff_t>(SEIZ_FIXED + c * SEIZ_CHANNEL);
    out.insert(out.end(), first, first + SEIZ_CHANNEL);
    ++kept;
  }
  if (kept == static_cast<si4>(n)) return r.body;  // nothing left out: as stored
  write<si4>(out, SEIZ_NCH, kept);
  return out;
}

/// One metadata text field a transform sets in a segment's sections.
struct EditedField {
  std::size_t section;  // S2 or S3 (offset of the section in the .tmet)
  std::size_t offset, bytes;
  std::string value;
};

/// The MetadataEdits that apply to `channel`'s segments. The copy writes
/// them and the verification expects them from this one table.
std::vector<EditedField> edited_fields(const MetadataEdits& m, const std::string& channel) {
  std::vector<EditedField> v;
  auto add = [&](std::size_t sec, std::size_t off, std::size_t n,
                 const std::optional<std::string>& x) {
    if (x) v.push_back({sec, off, n, *x});
  };
  add(S3, S3_NAME1, S3_SUBJECT_BYTES, m.subject_name_1);
  add(S3, S3_NAME2, S3_SUBJECT_BYTES, m.subject_name_2);
  add(S3, S3_ID, S3_SUBJECT_BYTES, m.subject_id);
  add(S3, S3_LOCATION, S3_LOCATION_BYTES, m.recording_location);
  add(S2, S2_SESSION_DESC, S2_DESC_BYTES, m.session_description);
  if (const auto it = m.channel_descriptions.find(channel); it != m.channel_descriptions.end())
    v.push_back({S2, S2_CHANNEL_DESC, S2_DESC_BYTES, it->second});
  if (const auto it = m.reference_descriptions.find(channel);
      it != m.reference_descriptions.end())
    v.push_back({S2, S2_REFERENCE_DESC, S2_DESC_BYTES, it->second});
  return v;
}

/// A record's encryption byte in the copy: level 2 with output passwords
/// (records may hold identifying text); otherwise 0, except that a transform
/// keeps a source's own "not encrypted" value (it changes no byte it has no
/// reason to).
si1 record_encryption(si1 source, bool encrypt, bool anonymize) {
  if (encrypt) return fmt::LEVEL_2_ENCRYPTION;
  return (!anonymize && source <= 0) ? source : si1{0};
}

// ---------------------------------------------------------------------------
// Cuts (#103): which samples and annotations a time window keeps. The copy
// and the verification both decide with these, and the verification also
// checks the result independently (every sample inside the window, the
// source's samples just outside it absent).
// ---------------------------------------------------------------------------

/// Sample i of a block that starts at `t` (uUTC) lies at t + i * 1e6 / fs.
si8 sample_time(si8 t, std::size_t i, sf8 fs) {
  return t + static_cast<si8>(std::llround(static_cast<long double>(i) * 1e6L / fs));
}

/// The samples [i0, i1) of an n-sample block starting at `t` whose times lie
/// in [t0, t1).
std::pair<std::size_t, std::size_t> window_slice(si8 t, std::size_t n, sf8 fs, si8 t0, si8 t1) {
  auto first_at_or_after = [&](si8 x) -> std::size_t {
    if (x <= t) return 0;
    const long double i = std::ceil(static_cast<long double>(x - t) * fs / 1e6L - 1e-9L);
    return i >= static_cast<long double>(n) ? n : static_cast<std::size_t>(i);
  };
  return {first_at_or_after(t0), first_at_or_after(t1)};
}

/// Whether a cut [t0, t1) keeps an annotation: its time lies in the window,
/// or (a Seiz or Epoc record) its span overlaps it. One without a time is
/// kept: it cannot be placed, and dropping it would lose it silently.
bool record_in_window(const RawRecord& r, si8 rto, si8 t0, si8 t1) {
  if (is_sentinel(r.time, false)) return true;
  const si8 t = to_user_time(r.time, rto);
  if (t >= t0 && t < t1) return true;
  if (layout_of(r) && (r.type == "Seiz" || r.type == "Epoc")) {
    const std::size_t a = r.type == "Seiz" ? 0 : 8;
    const si8 s = read<si8>(r.body, a), e = read<si8>(r.body, a + 8);
    if (is_sentinel(s, true) || is_sentinel(e, true)) return false;
    return to_user_time(s, rto) < t1 && to_user_time(e, rto) >= t0;
  }
  return false;
}

/// A record whose placement in time depends on the offset: a time stored
/// negated (offset applied). Non-negative times are absolute whatever it is.
bool record_offset_dependent(const RawRecord& r) {
  if (!is_sentinel(r.time, false) && r.time < 0) return true;
  if (layout_of(r) && (r.type == "Seiz" || r.type == "Epoc")) {
    const std::size_t a = r.type == "Seiz" ? 0 : 8;
    for (std::size_t off : {a, a + 8}) {
      const si8 v = read<si8>(r.body, off);
      if (!is_sentinel(v, true) && v < 0) return true;
    }
  }
  return false;
}

/// How far before t0 a block of this segment can start and still reach it:
/// its longest declared block plus a second; an hour when the declaration
/// is unset (a foreign writer's), so nothing is missed, only more is read.
si8 longest_block_us(const fmt::TimeSeriesMetadataSection2& m) {
  const ui4 n = m.maximum_block_samples;
  if (n == 0 || n == 0xFFFFFFFFu || !(m.sampling_frequency > 0)) return 3'600'000'000LL;
  return static_cast<si8>(std::llround(n * 1e6 / m.sampling_frequency)) + 1'000'000;
}

/// The index entries [first, last) of a segment a cut [t0, t1) must look at,
/// found by binary search on the index's start times (O(log n) small reads:
/// never the whole index, never the data): from two blocks before the first
/// one starting after t0 - `reach_us` (the longest a block can be:
/// longest_block_us, so a long block overlapped by later short ones is not
/// missed) to one after the first starting at or after t1. The two-block
/// margin covers an index time that disagrees a little with its block header
/// (the header decides, #11/#13); the verification checks the block just
/// before the range holds nothing of the window.
std::pair<std::size_t, std::size_t> window_entries(const SessionSource& src,
                                                   const std::string& tidx_rel, si8 rto, si8 t0,
                                                   si8 t1, si8 reach_us) {
  const std::uint64_t size = src.file_size(tidx_rel);
  if (size < fmt::UNIVERSAL_HEADER_BYTES) return {0, 0};
  const std::size_t n =
      static_cast<std::size_t>((size - fmt::UNIVERSAL_HEADER_BYTES) / fmt::TIME_SERIES_INDEX_BYTES);
  auto time_at = [&](std::size_t i) {
    const auto b = src.read_range(
        tidx_rel, fmt::UNIVERSAL_HEADER_BYTES + i * fmt::TIME_SERIES_INDEX_BYTES + 8, 8);
    return to_user_time(read<si8>(b, 0), rto);
  };
  auto first_after = [&](si8 t, bool or_at) {  // first i with time > t (or >= t)
    std::size_t lo = 0, hi = n;
    while (lo < hi) {
      const std::size_t mid = lo + (hi - lo) / 2;
      const si8 v = time_at(mid);
      if (or_at ? v >= t : v > t) hi = mid;
      else lo = mid + 1;
    }
    return lo;
  };
  const std::size_t a = first_after(saturating_add(t0, -reach_us), false);
  const std::size_t b = first_after(t1, true);
  return {a >= 2 ? a - 2 : 0, std::min(n, b + 1)};
}

/// A cut's user time stored the way `like` was stored (meflib's negated
/// form, or absolute); an unset `like` gets the negated form.
si8 stored_like(si8 like, si8 user, si8 rto) {
  return (like > 0 && like != fmt::UUTC_NO_ENTRY) ? user : to_stored_time(user, rto);
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
    refuse(RefusalKind::Password, "password-level1-only",
           "metadata section 3 of " + src.describe(rel) +
               " is locked: it holds the recording-time offset and the subject fields, so the "
               "copy needs the level-2 password");
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
    if (key.empty())
      refuse(RefusalKind::Password, "password-level1-only",
             "cannot decrypt metadata of " + src.describe(rel));
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

/// "<channel>-NNNNNN": how mef3io's writer names a segment (writer.cpp).
std::string canonical_segment_stem(const std::string& channel, int number) {
  char buf[16];
  std::snprintf(buf, sizeof buf, "%06d", number);
  return channel + "-" + buf;
}

/// What one copy writes, under its Rules. Every file of the copy is written
/// by one of copy_tmet / copy_segment_data / copy_records.
class Copier {
 public:
  Copier(const SessionSource& src, Rules rules, const Shifter& sh, const Redactor* red,
         std::string new_name, std::string pseudonym, std::set<std::string> excluded,
         std::set<std::string> kept, OutputKeys out, const Control& ctl,
         std::function<void(std::uint64_t)> progress, CopyReport& report,
         AnonymizeReport* anon_report, TransformReport* transform_report)
      : src_(src), rules_(rules), sh_(sh), red_(red), name_(std::move(new_name)),
        pseudonym_(std::move(pseudonym)), kept_(std::move(kept)), out_(std::move(out)),
        ctl_(ctl), progress_(std::move(progress)), rep_(report), arep_(anon_report),
        trep_(transform_report) {
    for (const auto& c : excluded) excluded_.insert(channel_key(c));
    if (rules_.copy->time_range) window_ = rules_.copy->time_range;
  }

  std::vector<std::string> written;  // every output file, for the flush

  /// A stored time as the copy writes it: shifted (anonymize with a shift)
  /// or verbatim (a transform, or anonymize keeping the times).
  si8 time(si8 s, const std::string& what, bool body = false) const {
    return rules_.shifting() ? sh_(s, what, body) : s;
  }

  /// Patch a universal header in place (any file type). Counts, maximum
  /// sizes and the CRCs are the caller's.
  void patch_uh(std::span<ui1> uh, const std::string& what, const std::string& channel) {
    const bool anon = rules_.anonymize();
    si8 start = read<si8>(uh, UH_START), end = read<si8>(uh, UH_END);
    if (seg_span_) {  // a cut segment: the times of what it now holds
      start = stored_like(start, seg_span_->first, seg_rto_);
      end = stored_like(end, seg_span_->second, seg_rto_);
    }
    write<si8>(uh, UH_START, time(start, what + " start time"));
    write<si8>(uh, UH_END, time(end, what + " end time"));
    if (seg_number_) write<si4>(uh, 48, *seg_number_);  // a cut renumbers segments
    if (anon) {
      // Every text field is rewritten whole: write_string zero-fills the
      // field, so nothing hidden after a terminator survives.
      write_string(uh, UH_CHANNEL_NAME, UH_NAME_BYTES, channel);
      write_string(uh, UH_SESSION_NAME, UH_NAME_BYTES, name_);
      write_string(uh, UH_ANON_NAME, UH_NAME_BYTES, pseudonym_);
    } else if (read_string(uh, UH_SESSION_NAME, UH_NAME_BYTES) != name_) {
      // A transform keeps every byte it has no reason to change: the name is
      // written only when the copy is renamed.
      write_string(uh, UH_SESSION_NAME, UH_NAME_BYTES, name_);
    }
    const Uuid old_level = uuid_at(uh, UH_LEVEL_UUID);
    const Uuid old_file = uuid_at(uh, UH_FILE_UUID);
    Uuid level{};
    if (level_override_) {
      level = *level_override_;
    } else if (!all_zero(old_level)) {
      auto it = level_map_.find(old_level);
      if (it == level_map_.end()) it = level_map_.emplace(old_level, random_uuid()).first;
      level = it->second;
    }
    const Uuid file = random_uuid();
    std::copy(level.begin(), level.end(), uh.begin() + UH_LEVEL_UUID);
    std::copy(file.begin(), file.end(), uh.begin() + UH_FILE_UUID);
    if (anon) {
      std::copy(file.begin(), file.end(), uh.begin() + UH_PROV_UUID);  // its own origin
    } else if (all_zero(uuid_at(uh, UH_PROV_UUID))) {
      // The copy's origin is the source file (meflib: provenance_UUID names
      // the file a file was derived from); a set one is kept.
      std::copy(old_file.begin(), old_file.end(), uh.begin() + UH_PROV_UUID);
    }
    std::copy(out_.vf.level1.begin(), out_.vf.level1.end(), uh.begin() + UH_L1_FIELD);
    std::copy(out_.vf.level2.begin(), out_.vf.level2.end(), uh.begin() + UH_L2_FIELD);
    if (anon) std::fill_n(uh.begin() + UH_PROTECTED, UH_PROTECTED_BYTES, ui1{0});
    // Provenance: anonymize keeps a mef3io region's history and clears anyone
    // else's; a transform stamps a mef3io (or empty) region and never writes
    // another program's (stamp_modified declines it).
    fmt::UniversalHeader h;
    std::copy_n(uh.begin() + fmt::provenance::REGION_OFFSET, fmt::provenance::REGION_BYTES,
                h.discretionary_region.begin());
    if (anon && !fmt::provenance::read(h).present) h.discretionary_region.fill(0);
    fmt::provenance::stamp_modified(
        h, anon ? fmt::provenance::Operation::Anonymize : fmt::provenance::Operation::Transform);
    std::copy(h.discretionary_region.begin(), h.discretionary_region.end(),
              uh.begin() + fmt::provenance::REGION_OFFSET);
  }

  std::string text_field(const std::string& value, const RecordTextField& where) {
    if (value.empty()) return value;
    std::string out;
    switch (rules_.anon->text) {
      case RecordTextPolicy::Drop: out = ""; break;
      case RecordTextPolicy::Keep: out = red_->apply(value); break;
      case RecordTextPolicy::Callback: out = red_->apply(rules_.anon->text_fn(value, where)); break;
    }
    if (out != value) ++arep_->record_text_fields_changed;
    return out;
  }

  void write_out(const fs::path& p, const std::vector<ui1>& bytes) {
    if (fs::exists(p))
      refuse(RefusalKind::Io, "write-failed", "output file already exists: " + p.string());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) refuse(RefusalKind::Io, "write-failed", "cannot create file: " + p.string());
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    f.flush();
    if (!f) refuse(RefusalKind::Io, "write-failed", "write failed (disk full?): " + p.string());
    f.close();
    written.push_back(p.string());
    rep_.bytes_written += bytes.size();
  }

  /// Write a segment's metadata, read by copy_segment. `st` (a cut): the
  /// section-2 quantities of what the segment now holds.
  void copy_tmet(TmetImage t, const std::string& rel, const fs::path& out,
                 const std::string& channel, const detail::CopyStats* st, si8 start_sample) {
    // (out is named canonically by the caller)
    std::span<ui1> b(t.bytes);
    auto s2 = b.subspan(S2, S2_BYTES);
    auto s3 = b.subspan(S3, S3_BYTES);
    auto s1 = b.subspan(S1, S1_BYTES);
    if (rules_.anonymize()) {
      // Shifting reads every time against ONE offset; keeping them copies
      // each segment's times and offset as stored, so a disagreement does
      // not concern it.
      if (rules_.shifting() && t.rto != sh_.rto)
        refuse(RefusalKind::WriteConflict, "rto-disputed",
               "segments disagree on the recording-time offset (" + src_.describe(rel) +
                   " differs from the rest of the session), so there is no single right way to read their times",
               channel);
      const AnonymizeOptions& o = *rules_.anon;
      // Section 2: descriptions, and the regions meflib leaves to writers.
      // Every text field is REWRITTEN, never left as is: write_string
      // zero-fills the whole field, so bytes hidden after a terminator go.
      for (std::size_t off : {S2_CHANNEL_DESC, S2_SESSION_DESC, S2_REFERENCE_DESC}) {
        const std::string old = read_string(s2, off, S2_DESC_BYTES);
        write_string(s2, off, S2_DESC_BYTES,
                     o.clear_descriptions ? std::string() : red_->apply(old));
      }
      write_string(s2, S2_UNITS, S2_UNITS_BYTES,
                   red_->apply(read_string(s2, S2_UNITS, S2_UNITS_BYTES)));
      std::fill(s2.begin() + S2_REGIONS, s2.end(), ui1{0});
      // Section 3: time-zone fields SET, identity cleared. The offset is 0
      // in a shifted copy (its times are absolute); a copy that keeps the
      // times keeps it, since its stored times are read against it. GMT
      // offset and DST can say where the recording was made and change no
      // time a reader returns (uUTC), so they are cleared in both.
      if (rules_.shifting()) write<si8>(s3, S3_RTO, 0);
      write<si8>(s3, S3_DST_START, fmt::UUTC_NO_ENTRY);
      write<si8>(s3, S3_DST_END, fmt::UUTC_NO_ENTRY);
      write<si4>(s3, S3_GMT, 0);
      for (std::size_t off : {S3_NAME1, S3_NAME2, S3_ID})
        write_string(s3, off, S3_SUBJECT_BYTES,
                     o.clear_subject ? std::string() : read_string(s3, off, S3_SUBJECT_BYTES));
      write_string(s3, S3_LOCATION, S3_LOCATION_BYTES,
                   o.clear_location ? std::string()
                                    : read_string(s3, S3_LOCATION, S3_LOCATION_BYTES));
      std::fill(s3.begin() + S3_REGIONS, s3.end(), ui1{0});
      // Section 1: the copy's encryption levels; regions cleared.
      std::fill(s1.begin(), s1.end(), ui1{0});
    } else {
      // A transform: the sections as stored, except the edited fields.
      for (const auto& f : edited_fields(*rules_.edits, channel)) {
        auto sec = f.section == S2 ? s2 : s3;
        if (read_string(sec, f.offset, f.bytes) != f.value || !terminated_clean(sec, f)) {
          write_string(sec, f.offset, f.bytes, f.value);
          ++trep_->metadata_fields_changed;
        }
      }
    }
    if (st) {
      // A cut: every derived declaration describes what the segment now
      // holds (edited in the stored image, never re-serialized).
      auto m = fmt::TimeSeriesMetadataSection2::parse(s2);
      m.number_of_samples = st->samples;
      m.number_of_blocks = st->blocks;
      m.start_sample = start_sample;
      m.recording_duration = seg_span_->second - seg_span_->first;
      m.maximum_block_bytes = st->max_block_bytes;
      m.maximum_block_samples = st->max_block_samples;
      m.block_interval = static_cast<si8>(std::llround(st->max_block_samples * 1e6 / seg_fs_));
      m.maximum_difference_bytes = st->max_difference_bytes;
      m.number_of_discontinuities = st->discontinuities;
      m.maximum_contiguous_blocks = st->max_contiguous_blocks;
      m.maximum_contiguous_block_bytes = st->max_contiguous_block_bytes;
      m.maximum_contiguous_samples = st->max_contiguous_samples;
      m.maximum_native_sample_value = native_max_ * m.units_conversion_factor;
      m.minimum_native_sample_value = native_min_ * m.units_conversion_factor;
      m.serialize_derived_fields(s2);
    }
    write<si1>(s1, 0, out_.encrypt ? fmt::LEVEL_1_ENCRYPTION : -fmt::LEVEL_1_ENCRYPTION);
    write<si1>(s1, 1, out_.encrypt ? fmt::LEVEL_2_ENCRYPTION : -fmt::LEVEL_2_ENCRYPTION);
    if (out_.encrypt) {
      auto e2 = crypto::aes128_ecb_encrypt(s2, *out_.keys.level1_key);
      std::copy(e2.begin(), e2.end(), s2.begin());
      auto e3 = crypto::aes128_ecb_encrypt(s3, *out_.keys.level2_key);
      std::copy(e3.begin(), e3.end(), s3.begin());
    }
    patch_uh(b.first(UH), channel + " metadata", channel);
    // Bounded CRCs: the record is exactly TMET bytes.
    write<ui4>(b, 4, crc::calculate(std::span<const ui1>(b).subspan(UH, TMET - UH)));
    write<ui4>(b, 0, crc::calculate(std::span<const ui1>(b).subspan(4, UH - 4)));
    write_out(out, t.bytes);
    if (progress_) progress_(TMET);
  }

  /// One segment: its data, then its metadata (which, in a cut, describes
  /// what was kept). Returns false when a cut keeps nothing of it (nothing
  /// is left on disk then). `number`: the segment's number in the copy;
  /// `next_start`: the channel-wide sample its first sample gets (a cut
  /// renumbers), advanced by what it holds.
  bool copy_segment(const detail::LayoutSegment& seg, const fs::path& cdir,
                    const std::string& channel, int number, si8& next_start,
                    std::optional<std::pair<std::size_t, std::size_t>> piece = std::nullopt) {
    ctl_.check();
    piece_ = piece;
    const bool reshape = window_ || piece_;  // the copy's segments differ from the source's
    TmetImage t = open_tmet(src_, seg.tmet, rules_.copy->password);
    seg_keys_ = t.keys;
    seg_rto_ = t.rto;
    {
      const auto m = fmt::TimeSeriesMetadataSection2::parse(
          std::span<const ui1>(t.bytes).subspan(S2, S2_BYTES));
      seg_fs_ = m.sampling_frequency;
      seg_reach_us_ = longest_block_us(m);
    }
    if (window_) {
      // A segment outside the window is not read at all.
      const si8 a = read<si8>(t.bytes, UH_START), b = read<si8>(t.bytes, UH_END);
      if (!is_sentinel(a, false) && !is_sentinel(b, false) &&
          !(to_user_time(a, t.rto) < window_->second && to_user_time(b, t.rto) > window_->first))
        return false;
    }
    if (reshape) {
      if (!(seg_fs_ > 0))
        refuse(RefusalKind::Format, "data-damaged",
               src_.describe(seg.tmet) + " has no valid sampling frequency, so its samples "
               "cannot be placed in time",
               channel);
      seg_number_ = number;
    }
    const std::string stem = canonical_segment_stem(channel, number);
    const fs::path sdir = cdir / (stem + ".segd");
    fs::create_directory(sdir);
    const si8 start = next_start;
    const detail::CopyStats st =
        copy_segment_data(seg, sdir, channel, stem, reshape ? std::optional<si8>(start)
                                                             : std::nullopt);
    if (reshape && st.blocks == 0) {  // only gaps of it lay in the window
      written.resize(written.size() - 2);
      rep_.blocks -= st.blocks;
      rep_.bytes_written -= st.output_bytes;
      std::error_code ec;
      fs::remove_all(sdir, ec);
      end_segment();
      return false;
    }
    copy_tmet(std::move(t), seg.tmet, sdir / (stem + ".tmet"), channel, reshape ? &st : nullptr,
              start);
    next_start += st.samples;
    return true;
  }

  /// After a segment's own annotations: the next files are not the cut
  /// segment's.
  void end_segment() {
    seg_span_.reset();
    seg_number_.reset();
    piece_.reset();
    level_override_.reset();
  }

  /// A merge (#103) names the level each file belongs to itself: one UUID
  /// per channel and session across all sources, a new one per piece.
  void set_level_uuid(std::optional<Uuid> u) { level_override_ = u; }

  /// A merged record file: its universal header spans [first, last] (user
  /// uUTC, stored against `rto`), the records it holds.
  void set_record_span(si8 first, si8 last, si8 rto) {
    seg_span_ = std::make_pair(first, last);
    seg_rto_ = rto;
  }

  detail::CopyStats copy_segment_data(const detail::LayoutSegment& seg, const fs::path& dir,
                                      const std::string& channel, const std::string& stem,
                                      std::optional<si8> start_sample) {
    detail::SegmentCopySpec spec;
    spec.start_sample = start_sample;
    if (window_)  // only the blocks around the window are read
      spec.entries =
          window_entries(src_, seg.tidx, seg_rto_, window_->first, window_->second, seg_reach_us_);
    else if (piece_)  // a merge: one piece of the segment
      spec.entries = piece_;
    std::optional<si8> first_t, end_t;  // a cut: the kept samples' span (user uUTC)
    native_max_ = std::numeric_limits<si4>::min();
    native_min_ = std::numeric_limits<si4>::max();
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
      if (window_) {
        // The block header's time and count are the authority (#11/#13).
        const si8 stored = read<si8>(cb.block, 40);
        const si8 t = to_user_time(stored, seg_rto_);
        const std::size_t n = read<ui4>(cb.block, 32);
        const auto [i0, i1] = window_slice(t, n, seg_fs_, window_->first, window_->second);
        if (i0 >= i1) return false;
        const bool trim = rules_.copy->exact && (i0 > 0 || i1 < n);
        const std::size_t k0 = trim ? i0 : 0, k1 = trim ? i1 : n;
        if (!first_t) first_t = sample_time(t, k0, seg_fs_);
        end_t = sample_time(t, k1, seg_fs_);
        if (trim) {
          trim_block(cb, stored, t, k0, k1, what, channel);
          return true;
        }
      } else if (piece_) {  // a merge piece: its span, from the headers
        const si8 t = to_user_time(read<si8>(cb.block, 40), seg_rto_);
        if (!first_t) first_t = t;
        end_t = sample_time(t, read<ui4>(cb.block, 32), seg_fs_);
      }
      if (rules_.anonymize()) {
        write<si8>(cb.block, 40, time(read<si8>(cb.block, 40), what + " start time"));
        write<si8>(cb.entry, 8, time(read<si8>(cb.entry, 8), "index entry " + what));
        // Regions meflib leaves to writers, zero in everything it writes:
        // index entry protected (40, 4 B), RED-protected (45, 3 B) and
        // discretionary (48, 8 B); block header protected (5, 3 B) and
        // discretionary (8, 8 B). Inside the block CRC, which is re-sealed.
        std::fill_n(cb.entry.begin() + 40, 4, ui1{0});
        std::fill_n(cb.entry.begin() + 45, 11, ui1{0});
        std::fill_n(cb.block.begin() + 5, 11, ui1{0});
      }
      const ui1 flags = read<ui1>(cb.block, 4);
      const ui1 level_mask = flags & ENC_MASK;
      if (level_mask) {
        const int level =
            (flags & fmt::RedBlockHeader::LEVEL_1_ENCRYPTION_MASK) ? fmt::LEVEL_1_ENCRYPTION
                                                                   : fmt::LEVEL_2_ENCRYPTION;
        const auto key = key_for(seg_keys_, level);
        if (key.empty())
          refuse(RefusalKind::Password, "data-locked",
                 what + " is level-" + std::to_string(level) +
                     " encrypted and the password does not open it",
                 channel);
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
      track_extrema(cb.entry);
      return true;
    };
    spec.on_header = [&](std::span<ui1> uh, bool is_tdat) {
      if ((window_ || piece_) && first_t) seg_span_ = std::make_pair(*first_t, *end_t);
      patch_uh(uh, channel + (is_tdat ? " data" : " index"), channel);
    };
    detail::CopyStats st;
    try {
      st = detail::copy_segment(spec);
    } catch (const CopyError&) {
      throw;
    } catch (const CrcError& e) {  // a block's CRC: never re-sealed
      refuse(RefusalKind::Crc, "block-crc", e.what(), channel);
    } catch (const FormatError& e) {
      refuse(RefusalKind::Format, "data-damaged", e.what(), channel);
    } catch (const IoError& e) {
      refuse(RefusalKind::Io, "write-failed", e.what(), channel);
    }
    written.push_back(spec.tidx_out);
    written.push_back(spec.tdat_out);
    rep_.blocks += st.blocks;
    rep_.bytes_written += st.output_bytes;
    return st;
  }

  void track_extrema(std::span<const ui1> entry) {
    native_max_ = std::max(native_max_, read<si4>(entry, 32));
    native_min_ = std::min(native_min_, read<si4>(entry, 36));
  }

  /// A cut's edge block: samples [k0, k1) decoded and re-encoded with the
  /// existing RED encoder, at the source's encryption level with the copy's
  /// keys (or plain when the copy has none), its time in the source's
  /// storage convention (shifted when anonymizing with a shift).
  void trim_block(detail::CopyBlock& cb, si8 stored, si8 t, std::size_t k0, std::size_t k1,
                  const std::string& what, const std::string& channel) {
    const ui1 flags = read<ui1>(cb.block, 4);
    const int level = (flags & fmt::RedBlockHeader::LEVEL_1_ENCRYPTION_MASK)   ? 1
                      : (flags & fmt::RedBlockHeader::LEVEL_2_ENCRYPTION_MASK) ? 2
                                                                               : 0;
    if (level && key_for(seg_keys_, level).empty())
      refuse(RefusalKind::Password, "data-locked",
             what + " is level-" + std::to_string(level) +
                 " encrypted and the password does not open it",
             channel);
    red::DecodedBlock d = red::decode_block(cb.block, seg_keys_, /*validate_crc=*/true);
    const std::span<const si4> kept(d.samples.data() + k0, k1 - k0);
    const si8 new_stored = time(stored_like(stored, sample_time(t, k0, seg_fs_), seg_rto_),
                                what + " start time");
    const bool discontinuity = (flags & fmt::RedBlockHeader::DISCONTINUITY_MASK) || k0 > 0;
    const int out_level = out_.encrypt ? level : 0;
    si4 mn = 0, mx = 0;
    cb.replacement =
        red::encode_block(kept, new_stored, discontinuity, mn, mx, out_level, out_.keys);
    if (level) ++(out_level ? rep_.blocks_reencrypted : rep_.blocks_decrypted);
    ++rep_.blocks_trimmed;
    const ui1 masks = ENC_MASK | fmt::RedBlockHeader::DISCONTINUITY_MASK;
    write<si8>(cb.entry, 8, new_stored);
    write<ui4>(cb.entry, 24, static_cast<ui4>(kept.size()));
    write<si4>(cb.entry, 32, mx);
    write<si4>(cb.entry, 36, mn);
    write<ui1>(cb.entry, 44, static_cast<ui1>((read<ui1>(cb.entry, 44) & ~masks) |
                                              (cb.replacement[4] & masks)));
    if (rules_.anonymize()) {  // the regions other writers fill, as for any block
      std::fill_n(cb.entry.begin() + 40, 4, ui1{0});
      std::fill_n(cb.entry.begin() + 45, 11, ui1{0});
    }
    track_extrema(cb.entry);
  }

  /// One record as the copy writes it: header + body, and its index entry
  /// without the file offset (set when the file is written).
  struct OutRecord {
    si8 sort_time = 0;  ///< user uUTC, for a merge's order
    std::vector<ui1> bytes;
    std::array<ui1, RIDX> entry{};
  };

  /// A source record file's records as the copy writes them (rules applied,
  /// re-encrypted, a cut's window and `keep` honoured), and its headers.
  std::vector<OutRecord> prepare_records(const RawRecordFile& f, const std::string& rdat_rel,
                                         RecordLevel level, const std::string& channel,
                                         int segment,
                                         const std::function<bool(si8)>& keep = {}) {
    const std::string where = src_.describe(rdat_rel);
    // The offset this file's times are read with, as the Reader reads them.
    const std::optional<si8> lrto = level == RecordLevel::Segment ? std::optional<si8>(seg_rto_)
                                    : level_rto_ ? level_rto_(channel)
                                                 : std::optional<si8>(sh_.rto);
    const si8 rto = lrto.value_or(sh_.rto);  // only absolute times use it when unset
    std::vector<OutRecord> out;
    for (std::size_t i = 0; i < f.records.size(); ++i) {
      const RawRecord& r = f.records[i];
      if (!lrto && (window_ || keep) && record_offset_dependent(r))
        refuse(RefusalKind::WriteConflict, "time-range-offset-disputed",
               "record " + std::to_string(i) + " of " + where +
                   " stores its time against a recording-time offset its segments disagree on "
                   "(or that cannot be read), so whether it lies in time_range is unknown",
               channel, "time_range");
      if (window_ && !record_in_window(r, rto, window_->first, window_->second)) {
        ++rep_.records_outside_window;
        continue;
      }
      if (keep && !keep(is_sentinel(r.time, false) ? r.time : to_user_time(r.time, rto)))
        continue;
      const std::string what = "record " + std::to_string(i) + " (" + r.type + ") of " + where;
      const si8 t = time(r.time, what + " time");
      std::vector<ui1> body;
      if (rules_.anonymize()) {
        // Its time in the copy; unknown (NO_ENTRY) for a time stored
        // against an offset its segments disagree on.
        const bool unknown = !lrto && !rules_.shifting() && record_offset_dependent(r);
        RecordTextField tf{r.type, "", level, channel, segment,
                           unknown              ? fmt::UUTC_NO_ENTRY
                           : is_sentinel(t, false) ? t
                                                   : to_user_time(t, rules_.shifting() ? 0 : rto)};
        body = rewrite_body(r, tf, what);
      } else {
        body = kept_body(r);
      }
      if (body.size() % 16) body.resize((body.size() + 15) & ~std::size_t{15}, PAD_BYTE);
      const si1 enc = record_encryption(r.enc, out_.encrypt, rules_.anonymize());
      if (out_.encrypt) {
        auto e = crypto::aes128_ecb_encrypt(body, *out_.keys.level2_key);
        std::copy(e.begin(), e.end(), body.begin());
      }
      OutRecord o;
      o.sort_time = is_sentinel(r.time, false) ? r.time : to_user_time(r.time, rto);
      o.bytes.assign(RHDR, 0);
      std::copy(r.type_bytes.begin(), r.type_bytes.end(), o.bytes.begin() + 4);
      write<ui1>(o.bytes, 9, r.vmaj);
      write<ui1>(o.bytes, 10, r.vmin);
      write<si1>(o.bytes, 11, enc);
      write<ui4>(o.bytes, 12, static_cast<ui4>(body.size()));
      write<si8>(o.bytes, 16, t);
      o.bytes.insert(o.bytes.end(), body.begin(), body.end());
      write<ui4>(o.bytes, 0, crc::calculate(std::span<const ui1>(o.bytes).subspan(4)));
      std::copy(r.type_bytes.begin(), r.type_bytes.end(), o.entry.begin());
      o.entry[5] = r.vmaj;
      o.entry[6] = r.vmin;
      o.entry[7] = static_cast<ui1>(enc);
      // The index entry is matched to its record by file offset, never by
      // position; an index that names no such offset is refused.
      si8 idx_time = r.time;
      if (f.has_ridx) {
        const auto it = std::find_if(f.ridx.begin(), f.ridx.end(), [&](const auto& x) {
          return read<si8>(x, 8) == f.record_offsets[i];
        });
        if (it == f.ridx.end())
          refuse(RefusalKind::Format, "record-index-mismatch",
                 "record index of " + where + " has no entry for record " + std::to_string(i) +
                     "; which time is right is unknown",
                 channel);
        idx_time = read<si8>(*it, 16);
      }
      write<si8>(o.entry, 16, time(idx_time, "record index entry " + std::to_string(i) + " of " + where));
      out.push_back(std::move(o));
    }
    return out;
  }

  /// Write records (in the given order) to `out_rdat` (+ .ridx), with the
  /// universal headers of `f` (a source record file of the same level).
  void write_records(const std::vector<OutRecord>& recs, const RawRecordFile& f,
                     const std::string& what, const fs::path& out_rdat,
                     const std::string& channel) {
    std::vector<ui1> rdat(f.uh);
    std::vector<ui1> ridx(f.has_ridx ? f.ridx_uh : f.uh);
    if (!f.has_ridx) write_string(ridx, 8, 5, fmt::FILE_TYPE_RECORD_INDICES);
    ui4 max_entry = 0;
    for (const auto& o : recs) {
      std::array<ui1, RIDX> e = o.entry;
      write<si8>(e, 8, static_cast<si8>(rdat.size()));
      rdat.insert(rdat.end(), o.bytes.begin(), o.bytes.end());
      max_entry = std::max(max_entry, static_cast<ui4>(o.bytes.size()));
      ridx.insert(ridx.end(), e.begin(), e.end());
      ++rep_.records;
    }
    const si8 n = static_cast<si8>(recs.size());
    auto seal = [&](std::vector<ui1>& file, si8 max, const std::string& label) {
      std::span<ui1> uh(file.data(), UH);
      patch_uh(uh, label, channel);
      write<si8>(uh, 32, n);
      write<si8>(uh, 40, max);
      write<ui4>(file, 4, crc::calculate(std::span<const ui1>(file).subspan(UH)));
      write<ui4>(file, 0, crc::calculate(std::span<const ui1>(file).subspan(4, UH - 4)));
    };
    seal(rdat, max_entry, "record file " + what);
    seal(ridx, static_cast<si8>(RIDX), "record index of " + what);
    write_out(out_rdat, rdat);
    write_out(fs::path(ridx_of(out_rdat.string())), ridx);
    if (progress_) progress_(rdat.size() + ridx.size());
  }

  /// One record file pair -> `out_rdat` (+ .ridx).
  void copy_records(const std::string& rdat_rel, const fs::path& out_rdat, RecordLevel level,
                    const std::string& channel, int segment,
                    const std::function<bool(si8)>& keep = {}) {
    ctl_.check();
    const RawRecordFile f = read_record_file(src_, rdat_rel, rules_.copy->password);
    write_records(prepare_records(f, rdat_rel, level, channel, segment, keep), f,
                  src_.describe(rdat_rel), out_rdat, channel);
  }

  /// The source this copier reads.
  const SessionSource& source() const { return src_; }

  /// How a channel's (`channel` non-empty) or the session's record file is
  /// read: the offset its segments agree on, nullopt when they do not (see
  /// level_rto_of). Unset: the session's offset (`sh_.rto`). A segment's own
  /// record file always uses that segment's offset.
  void set_level_rto(std::function<std::optional<si8>(const std::string&)> f) {
    level_rto_ = std::move(f);
  }

 private:
  /// The field holds nothing after its terminator (an edit to the same value
  /// leaves such a field alone; anything hidden there is rewritten away).
  static bool terminated_clean(std::span<const ui1> sec, const EditedField& f) {
    const std::size_t n = read_string(sec, f.offset, f.bytes).size();
    return all_zero(sec.subspan(f.offset + n, f.bytes - n));
  }

  /// A transform keeps a record body as stored, except the Seiz entries of
  /// left-out channels.
  std::vector<ui1> kept_body(const RawRecord& r) {
    return seiz_without(r, excluded_, &rep_.seizure_channel_entries_dropped);
  }

  std::vector<ui1> rewrite_body(const RawRecord& r, RecordTextField tf, const std::string& what) {
    const BodyLayout* lay = layout_of(r);
    if (!lay) {
      // No known layout (UnRc, an unknown type or version, a body shorter
      // than its type): it may hold text or dates nothing here can find, so
      // it is not carried over. The record itself (type, time) is kept.
      if (!r.body.empty()) ++arep_->record_bodies_cleared;
      return {};
    }
    if (lay->keep_whole) {  // LNTP: the template, re-padded (nothing after it is data)
      std::vector<ui1> b(r.body.begin(),
                         r.body.begin() + static_cast<std::ptrdiff_t>(lntp_bytes(r.body)));
      return b;
    }
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
      write<si8>(b, off, time(read<si8>(b, off), what + " body time", true));
    for (const auto& slot : lay->text) {
      tf.field = slot.name;
      write_string(b, slot.offset, slot.bytes,
                   text_field(read_string(b, slot.offset, slot.bytes), tf));
    }
    if (r.type != "Seiz") {
      b.resize(lay->fixed);  // what follows the fixed part is padding
      return b;
    }
    // Seiz channel entries: excluded channels leave, the rest are shifted.
    const std::size_t n = seiz_entry_count(b);
    std::vector<ui1> out(b.begin(), b.begin() + SEIZ_FIXED);
    si4 kept = 0;
    for (std::size_t c = 0; c < n; ++c) {
      std::vector<ui1> e(b.begin() + static_cast<std::ptrdiff_t>(SEIZ_FIXED + c * SEIZ_CHANNEL),
                         b.begin() + static_cast<std::ptrdiff_t>(SEIZ_FIXED + (c + 1) * SEIZ_CHANNEL));
      const std::string name = read_string(e, 0, SEIZ_CHANNEL_NAME);
      if (excluded_.count(channel_key(name))) {  // trimmed, case-insensitive
        ++rep_.seizure_channel_entries_dropped;
        continue;
      }
      // A kept channel's exact name stays. Anything else is free text: with
      // text="drop" it is cleared, otherwise redacted. Rewritten whole.
      std::string now = name;
      if (!kept_.count(name))
        now = rules_.anon->text == RecordTextPolicy::Drop ? std::string() : red_->apply(name);
      if (now != name) ++arep_->record_text_fields_changed;
      write_string(e, 0, SEIZ_CHANNEL_NAME, now);
      for (std::size_t off : {SEIZ_CH_ONSET, SEIZ_CH_OFFSET})
        write<si8>(e, off, time(read<si8>(e, off), what + " channel " + std::to_string(c) + " time",
                                true));
      out.insert(out.end(), e.begin(), e.end());
      ++kept;
    }
    write<si4>(out, SEIZ_NCH, kept);
    return out;
  }

  const SessionSource& src_;
  Rules rules_;
  const Shifter& sh_;
  std::function<std::optional<si8>(const std::string&)> level_rto_;
  const Redactor* red_;  // anonymize only
  std::string name_, pseudonym_;
  std::set<std::string> excluded_;  // channel_key()s
  std::set<std::string> kept_;
  OutputKeys out_;
  const Control& ctl_;
  std::function<void(std::uint64_t)> progress_;
  CopyReport& rep_;
  AnonymizeReport* arep_;    // anonymize only
  TransformReport* trep_;    // transform only
  std::map<Uuid, Uuid> level_map_;
  crypto::AccessKeys seg_keys_;
  // A cut (#103).
  std::optional<std::pair<si8, si8>> window_;     // [t0, t1), user uUTC
  si8 seg_rto_ = 0;                               // the segment being copied
  sf8 seg_fs_ = 0;
  si8 seg_reach_us_ = 0;                          // longest_block_us
  std::optional<std::pair<si8, si8>> seg_span_;   // its kept samples' span
  std::optional<si4> seg_number_;                 // its number in the copy
  std::optional<std::pair<std::size_t, std::size_t>> piece_;  // a merge: the entries copied
  std::optional<Uuid> level_override_;            // a merge: the file's level UUID
  si4 native_max_ = 0, native_min_ = 0;           // its kept samples' extrema
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
  /// The offset status inspect reports ("none", "applied", "disputed", ...).
  std::string rto_status = "none";
  /// Each segment's own offset (by source .tmet path), read only when the
  /// segments disagree ("disputed"); otherwise every segment has `rto`.
  std::map<std::string, si8> segment_rtos;
  si8 first = 0, last = 0;
  bool has_times = false;
  std::set<std::string> session_names;    // folder stem + every stored session_name
  std::set<std::string> subject_values, location_values, anonymized_values;
  /// The subject IDs among subject_values: searched whole, never word by word
  /// (an ID's letters are usually an institution's prefix: "EEG-0042").
  std::set<std::string> id_values;
  std::set<Uuid> uuids;
};

/// The offset a segment's times are stored against (its own section 3).
si8 segment_rto_of(const SourcePlan& p, const std::string& tmet) {
  const auto it = p.segment_rtos.find(tmet);
  return it == p.segment_rtos.end() ? p.rto : it->second;
}

/// The offset a channel's (`channel` set) or the session's record file is
/// read with: the one all of its segments agree on, as the Reader resolves
/// it (Session::agreed_rto; a channel without segments takes the session's).
/// nullopt = there is none: they disagree, or the offset is not known.
std::optional<si8> level_rto_of(const SourcePlan& p, const std::string* channel) {
  if (p.rto_status == "none" || p.rto_status == "applied") return p.rto;
  if (p.rto_status != "disputed" || !channel) return std::nullopt;
  std::optional<si8> agreed;
  for (const auto& c : p.layout.channels) {
    if (c.name != *channel) continue;
    for (const auto& seg : c.segments) {
      const si8 r = segment_rto_of(p, seg.tmet);
      if (agreed && *agreed != r) return std::nullopt;
      agreed = r;
    }
  }
  return agreed ? agreed : level_rto_of(p, nullptr);
}

/// The source's recording range +- (1 day - 1 us), plain or negated. For a
/// field that is not a stored time (an ID), only this window applies: an
/// offset-relative one reaches down to small numbers. Shared by the
/// pre-flight (which refuses an Epoc/Curs id_number inside it before writing)
/// and the verification (which re-checks the copy).
bool in_absolute_window(const SourcePlan& p, si8 v) {
  if (!p.has_times || v == fmt::UUTC_NO_ENTRY) return false;
  const si8 a = v < 0 ? -v : v;
  return a >= p.first - DAY_US + 1 && a <= p.last + DAY_US - 1;
}

/// `text` with every number that could be one of the source's times (10+
/// digits, inside the recording range +- a day, absolute or relative to the
/// offset, or the offset itself) replaced by "<time>". The source's times
/// are the re-identification key: they never reach a message (#111). The
/// messages built here name no time in the first place; this also covers
/// text that comes from shared code (inspect, the validator).
std::string scrub_times(const SourcePlan& p, const std::string& text) {
  if (!p.has_times && p.rto == 0) return text;
  std::string out;
  out.reserve(text.size());
  std::size_t i = 0;
  while (i < text.size()) {
    if (!std::isdigit(static_cast<unsigned char>(text[i]))) {
      out += text[i++];
      continue;
    }
    std::size_t j = i;
    while (j < text.size() && std::isdigit(static_cast<unsigned char>(text[j]))) ++j;
    const std::string run = text.substr(i, j - i);
    bool hit = false;
    if (run.size() >= 10 && run.size() <= 18) {
      const si8 a = std::stoll(run);
      hit = in_absolute_window(p, a) || (p.rto != 0 && a == (p.rto < 0 ? -p.rto : p.rto));
      if (!hit && p.rto != 0 && p.has_times) {
        const si8 lo = p.first - DAY_US + 1 - p.rto, hi = p.last + DAY_US - 1 - p.rto;
        hit = a >= lo && a <= hi;
      }
    } else if (run.size() > 18) {
      hit = true;  // out of range for a time; never worth printing
    }
    out += hit ? std::string("<time>") : run;
    i = j;
  }
  return out;
}

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
  p.rto_status = to.status;
  if (to.status == "disputed")  // each segment's own offset (metadata reads only)
    for (const auto& c : p.layout.channels)
      for (const auto& seg : c.segments) {
        ctl.check();
        p.segment_rtos[seg.tmet] =
            segment_rto(load_time_series_metadata(src.read_all(seg.tmet), password));
      }
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
    if (f.name == "subject_id")
      for (const auto& v : f.values)
        if (!v.empty()) p.id_values.insert(v);
  }
  return p;
}

Redactor build_redactor(const SourcePlan& p, const std::string& new_name, bool clear_subject,
                        bool clear_location, const std::set<std::string>& excluded,
                        const std::set<std::string>& kept, const std::string& pseudonym) {
  Redactor r;
  for (const auto& k : kept) r.protect.push_back(folded(k));
  std::sort(r.protect.begin(), r.protect.end(),
            [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
  // Every value normalized; subject and location values also word by word.
  for (const auto& n : p.session_names)
    if (n != new_name) r.add(normalized(n), new_name, false);
  if (clear_subject)
    for (const auto& v : p.subject_values) {
      for (const auto& w : identity_variants(v, !p.id_values.count(v))) r.add(w, "", false);
      if (p.id_values.count(v))
        for (const auto& d : id_digit_runs(normalized(v))) r.add(d, "", false);  // Number mode
    }
  if (clear_location)
    for (const auto& v : p.location_values)
      for (const auto& w : identity_variants(v)) r.add(w, "", false);
  for (const auto& v : p.anonymized_values)
    if (v != pseudonym && v != new_name) r.add(normalized(v), "", false);
  for (const auto& c : excluded) r.add(normalized(c), "", true);
  return r;
}

Scanner build_scanner(const SourcePlan& p, const std::string& new_name, bool clear_subject,
                      bool clear_location, const std::set<std::string>& excluded,
                      const std::set<std::string>& kept, const std::string& pseudonym) {
  Scanner s;
  auto add = [](std::vector<Scanner::Needle>& v, const std::string& value, const char* label,
                bool session = false) {
    const std::string n = normalized(value);
    if (n.size() < 2) return;  // as the rewrite: too short to search for
    v.push_back({scan_folded(n), label, session});
  };
  for (const auto& n : p.session_names)
    if (n != new_name) add(s.identity, n, "the source's session name", true);
  if (clear_subject)
    for (const auto& v : p.subject_values) {
      for (const auto& w : identity_variants(v, !p.id_values.count(v)))
        add(s.identity, w, "a subject name or ID");
      if (p.id_values.count(v))
        for (const auto& d : id_digit_runs(normalized(v))) add(s.identity, d, "a subject ID");
    }
  if (clear_location)
    for (const auto& v : p.location_values) {
      const auto vs = identity_variants(v);
      for (std::size_t i = 0; i < vs.size(); ++i) {
        add(s.identity, vs[i], "the recording location");
        if (i > 0) s.identity.back().location_word = true;  // [0] is the whole value
      }
    }
  for (const auto& v : p.anonymized_values)
    if (v != pseudonym && v != new_name) add(s.identity, v, "the source's anonymized name");
  for (const auto& c : excluded) add(s.excluded, c, "an excluded channel's name");
  s.allowed = {scan_folded(new_name)};
  if (!pseudonym.empty()) s.allowed.push_back(scan_folded(pseudonym));
  for (const auto& k : kept) s.kept_channels.push_back(scan_folded(k));
  // Longest first, so a kept name containing another is removed whole.
  auto longest = [](const std::string& a, const std::string& b) { return a.size() > b.size(); };
  std::sort(s.allowed.begin(), s.allowed.end(), longest);
  std::sort(s.kept_channels.begin(), s.kept_channels.end(), longest);
  return s;
}

// ---------------------------------------------------------------------------
// Verification.
// ---------------------------------------------------------------------------

class Verifier {
 public:
  /// `edits` set = a transform's verification (times, regions and bodies
  /// must equal the source's; only the edits may differ); unset = anonymize,
  /// with every time `shift` away, or (`shift` unset) every time kept.
  Verifier(const SessionSource& src, const SessionSource& dst, const SourcePlan& plan,
           std::optional<si8> shift, const VerifyOptions& o, const MetadataEdits* edits,
           const Control& ctl, std::function<void(std::uint64_t)> progress, VerifyReport& rep)
      : src_(src), dst_(dst), plan_(plan), o_(o), edits_(edits), ctl_(ctl),
        progress_(std::move(progress)), rep_(rep) {
    keep_times_ = !edits && !shift;
    sh_.rto = plan.rto;
    sh_.shift = shift.value_or(0);
    // The leak window: the original range, widened by just under a day on
    // each side. A shift that passed the floor puts every copied time outside
    // it, so a value inside can only be one the rewrite missed.
    lo_ = plan.first - DAY_US + 1;
    hi_ = plan.last + DAY_US - 1;
  }

  void problem(const std::string& loc, const std::string& field, const std::string& msg) {
    rep_.problems.push_back({loc, scrub_times(plan_, field), scrub_times(plan_, msg)});
  }

  void leak(const std::string& loc, const std::string& field, const std::string& msg) {
    VerifyIssue i{loc, scrub_times(plan_, field), scrub_times(plan_, msg)};
    rep_.leaks.push_back(i);
    rep_.problems.push_back(i);
  }

  bool in_absolute_window(si8 v) const { return mef3io::in_absolute_window(plan_, v); }

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
    if (transform()) {  // copied verbatim
      if (dst_v != src_v) problem(loc, field, "differs from the source's (a transform copies every time as stored)");
      return;
    }
    if (keep_times_) {  // kept: as stored, so also as read (the offset is kept too)
      if (dst_v != src_v)
        problem(loc, field, "differs from the source's (the copy keeps every time as stored)");
      return;
    }
    if (!(body && is_sentinel(dst_v, true)) && in_window(dst_v))
      leak(loc, field, "holds a time inside the source's recording range (plain or negated)");
    si8 expected;
    try {
      expected = sh_(src_v, field, body);
    } catch (const std::exception& e) {
      problem(loc, field, std::string("the source's value cannot be shifted: ") + e.what());
      return;
    }
    if (dst_v != expected)
      problem(loc, field,
              "differs from the source's value shifted by the shift in the same sign "
              "convention");
  }

  /// A fixed-width text field of the copy, `len` bytes at `off`: no
  /// forbidden string in it, and nothing hidden after its terminator (every
  /// byte zero, or the 0x7e record padding when `pad_ok`). Returns the text.
  std::string text(std::span<const ui1> b, std::size_t off, std::size_t len,
                   const std::string& loc, const std::string& field, bool pad_ok = false) {
    ++rep_.text_fields_checked;
    const std::string value = read_string(b, off, len);
    report(scan_.text(value), loc, field);
    for (std::size_t i = off + value.size(); i < off + len; ++i)
      if (b[i] != 0 && !(pad_ok && b[i] == PAD_BYTE)) {
        leak(loc, field, "holds bytes after its terminator (hidden text)");
        break;
      }
    return value;
  }

  void report(const Scanner::Result& r, const std::string& loc, const std::string& field) {
    if (!r.refuse.empty()) leak(loc, field, "contains " + r.refuse);
    else if (!r.note.empty()) note(loc, field, r.note);
  }

  /// A note: reported, never a refusal. One per distinct field + message.
  void note(const std::string& loc, const std::string& field, const std::string& msg) {
    for (const auto& n : rep_.notes)
      if (n.field == field && n.message == msg) return;
    rep_.notes.push_back({loc, field, msg});
  }

  /// A kept channel's name, or a text field holding exactly one: the
  /// channel rule. Anything else: the rewritten-field rule.
  void name_field(const std::string& value, const std::string& loc, const std::string& field) {
    ++rep_.text_fields_checked;
    if (value.empty() || kept_.count(value))
      report(scan_.channel(value), loc, field);
    else
      report(scan_.text(value), loc, field);
  }

  /// A path component of the copy: derived from a channel's name (its
  /// folder, its segments, its record files) unless it is the session's own
  /// record file.
  void path_component(const std::string& name, const std::string& loc) {
    ++rep_.text_fields_checked;
    report(scan_.path(name, dst_.session_name(), kept_), loc, "path");
  }

  void set_kept(std::set<std::string> k) { kept_ = std::move(k); }

  bool transform() const { return edits_ != nullptr; }

  /// While checking a segment of a cut copy: its times, segment number and
  /// derived declarations describe what it holds, not the source segment.
  void set_cut_segment(bool on) { cut_segment_ = on; }

  /// Checking a merge (#103): level UUIDs are the merge's own.
  void set_merge(bool on) { merge_ = on; }

  /// The sampling rate of the source segment check_tmet last read.
  sf8 segment_fs() const { return seg_fs_; }

  void set_scanner(Scanner s) { scan_ = std::move(s); }

  /// `counts_from_source`: the entry count and maximum entry size (32..48)
  /// are the source's (a .tmet, which the copy does not resize); otherwise
  /// they are checked against what the copy holds (check_counts).
  /// `rebuilt_index`: a record index the copy built because the source had
  /// none, from the record file's header (file type "ridx").
  void check_uh(std::span<const ui1> s, std::span<const ui1> d, const std::string& loc,
                bool counts_from_source = false, bool rebuilt_index = false) {
    if (transform()) {
      check_uh_transform(s, d, loc, counts_from_source, rebuilt_index);
      return;
    }
    if (cut_segment_) {  // a cut segment's span: only the leak scan applies
      // (kept times: the span of what the segment holds, which the
      // validator checks against its blocks, as for a transform)
      for (std::size_t off : {UH_START, UH_END}) {
        if (keep_times_) break;
        ++rep_.times_checked;
        if (in_window(read<si8>(d, off)))
          leak(loc, "universal header time", "holds a time inside the source's recording range");
      }
    } else {
      time_field(read<si8>(s, UH_START), read<si8>(d, UH_START), loc,
                 "universal header start_time");
      time_field(read<si8>(s, UH_END), read<si8>(d, UH_END), loc, "universal header end_time");
    }
    const std::string sn = text(d, UH_SESSION_NAME, UH_NAME_BYTES, loc, "universal header session_name");
    if (sn != dst_.session_name())
      problem(loc, "universal header session_name",
              "does not match the copy's folder name");
    text(d, UH_ANON_NAME, UH_NAME_BYTES, loc, "universal header anonymized_name");
    {
      const std::string cn = read_string(d, UH_CHANNEL_NAME, UH_NAME_BYTES);
      name_field(cn, loc, "universal header channel_name");
      if (!all_zero(d.subspan(UH_CHANNEL_NAME + cn.size(), UH_NAME_BYTES - cn.size())))
        leak(loc, "universal header channel_name", "holds bytes after its terminator (hidden text)");
    }
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

  /// A transform's universal header: the source's, byte for byte, except
  /// the CRCs, the session name (when renamed), the UUIDs (new file UUID,
  /// level UUIDs remapped one to one, provenance_UUID naming the source), the
  /// password fields, the provenance region (exactly the source's stamped as
  /// a transform, or untouched when another program owns it) and, unless
  /// `counts_from_source`, the entry count and maximum entry size, which the
  /// copy derives from what it wrote and check_counts verifies against it.
  void check_uh_transform(std::span<const ui1> s, std::span<const ui1> d, const std::string& loc,
                          bool counts_from_source, bool rebuilt_index) {
    if (crc::calculate(d.subspan(4, UH - 4)) != read<ui4>(d, 0))
      problem(loc, "universal header CRC", "does not verify");
    if (read_string(d, UH_SESSION_NAME, UH_NAME_BYTES) != dst_.session_name())
      problem(loc, "universal header session_name", "does not match the copy's folder name");
    if (!cut_segment_) {  // a cut segment's span is checked against its blocks
      time_field(read<si8>(s, UH_START), read<si8>(d, UH_START), loc,
                 "universal header start_time");
      time_field(read<si8>(s, UH_END), read<si8>(d, UH_END), loc, "universal header end_time");
    }
    struct Span { std::size_t from, to; const char* what; };
    std::vector<Span> same = {
        {rebuilt_index ? std::size_t{13} : std::size_t{8}, 16, "file type and version"}};
    if (!cut_segment_) same.push_back({48, 52, "segment number"});
    for (const Span& x : std::vector<Span>{
             {UH_CHANNEL_NAME, UH_SESSION_NAME, "channel_name"},
             {UH_ANON_NAME, UH_LEVEL_UUID, "anonymized_name"},
             {UH_PROTECTED, fmt::provenance::REGION_OFFSET, "protected region"}})
      same.push_back(x);
    if (counts_from_source) same.push_back({32, 48, "entry count and maximum entry size"});
    for (const auto& x : same)
      if (!std::equal(s.begin() + x.from, s.begin() + x.to, d.begin() + x.from))
        problem(loc, std::string("universal header ") + x.what, "differs from the source's");
    if (rebuilt_index && read_string(d, 8, 5) != fmt::FILE_TYPE_RECORD_INDICES)
      problem(loc, "universal header file type", "is not a record index");
    if (uuid_at(d, UH_FILE_UUID) == uuid_at(s, UH_FILE_UUID))
      problem(loc, "universal header file_UUID", "is the source's; a copy is a new file");
    const Uuid sp = uuid_at(s, UH_PROV_UUID);
    if (uuid_at(d, UH_PROV_UUID) != (all_zero(sp) ? uuid_at(s, UH_FILE_UUID) : sp))
      problem(loc, "universal header provenance_UUID", "does not name the source file");
    // Level UUIDs: a new one per source level, the same for every file of it.
    const Uuid sl = uuid_at(s, UH_LEVEL_UUID), dl = uuid_at(d, UH_LEVEL_UUID);
    bool level_ok = all_zero(sl) == all_zero(dl);
    if (merge_) {  // a merge names every level itself: never the source's, that is all
      level_ok = all_zero(sl) || sl != dl;
    } else if (!all_zero(sl)) {
      const auto it = level_map_.emplace(sl, dl).first;  // the first file of that level sets it
      level_ok = level_ok && sl != dl && it->second == dl;
    }
    if (!level_ok) problem(loc, "universal header level_UUID", "is not the source level's new UUID");
    // Provenance: the source's region as stamp_modified leaves it (which
    // declines another program's region, so that one must be unchanged).
    fmt::UniversalHeader want = fmt::UniversalHeader::parse(s);
    fmt::provenance::stamp_modified(want, fmt::provenance::Operation::Transform);
    if (!std::equal(want.discretionary_region.begin(), want.discretionary_region.end(),
                    d.begin() + fmt::provenance::REGION_OFFSET))
      problem(loc, "provenance region",
              "is not the source's stamped as a transform (operation 8), or another program's "
              "region was changed");
  }

  /// The entry count and maximum entry size of a file the copy wrote, against
  /// what it holds (any copy: the engine derives them, and the validator
  /// reports a wrong maximum only as Info and never reads record files).
  void check_counts(std::span<const ui1> d, si8 entries, si8 max_entry, const std::string& loc) {
    if (read<si8>(d, 32) != entries)
      problem(loc, "universal header number_of_entries",
              "is not the number of entries the file holds");
    if (read<si8>(d, 40) != max_entry)
      problem(loc, "universal header maximum_entry_size", "is not the largest entry's size");
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
    seg_rto_ = s.rto;
    dst_seg_rto_ = d.rto;
    dst_tmet_uh_.assign(d.bytes.begin(), d.bytes.begin() + UH);
    {
      const auto m = fmt::TimeSeriesMetadataSection2::parse(sb.subspan(S2, S2_BYTES));
      seg_fs_ = m.sampling_frequency;
      seg_reach_us_ = longest_block_us(m);
    }
    check_uh(sb.first(UH), db.first(UH), drel, /*counts_from_source=*/true);
    if (transform()) {
      check_sections_transform(sb, db, drel, d);
      dst_encrypted_ = d.s2_level > 0;
      src_keys_ = s.keys;
      dst_keys_ = d.keys;
      if (progress_) progress_(2 * TMET);
      return;
    }
    const auto ds2 = db.subspan(S2, S2_BYTES), ss2 = sb.subspan(S2, S2_BYTES);
    const auto ds3 = db.subspan(S3, S3_BYTES);
    // Time-zone fields: SET values, not shifted. The offset: 0 in a shifted
    // copy, the source segment's own in one that keeps the times.
    if (keep_times_) {
      if (read<si8>(ds3, S3_RTO) != s.rto)
        problem(drel, "recording_time_offset",
                "is not the source's (a copy that keeps the times keeps it)");
    } else if (read<si8>(ds3, S3_RTO) != 0) {
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
      const std::string v = text(ds3, off, S3_SUBJECT_BYTES, drel, name);
      if (o_.clear_subject && !v.empty()) leak(drel, name, "is not cleared");
    }
    {
      const std::string v =
          text(ds3, S3_LOCATION, S3_LOCATION_BYTES, drel, "recording_location");
      if (o_.clear_location && !v.empty()) leak(drel, "recording_location", "is not cleared");
    }
    const std::pair<std::size_t, const char*> s2_text[] = {
        {S2_CHANNEL_DESC, "channel_description"},
        {S2_SESSION_DESC, "session_description"},
        {S2_REFERENCE_DESC, "reference_description"}};
    for (const auto& [off, name] : s2_text) {
      const std::string v = text(ds2, off, S2_DESC_BYTES, drel, name);
      if (o_.clear_descriptions && !v.empty()) leak(drel, name, "is not cleared");
    }
    text(ds2, S2_UNITS, S2_UNITS_BYTES, drel, "units_description");
    if (!all_zero(ds2.subspan(S2_REGIONS)))
      problem(drel, "section 2 protected/discretionary regions", "are not cleared");
    if (!all_zero(ds3.subspan(S3_REGIONS)))
      problem(drel, "section 3 protected/discretionary regions", "are not cleared");
    if (!all_zero(db.subspan(S1 + 2, S1_BYTES - 2)))
      problem(drel, "section 1 regions", "are not cleared");
    // The signal's declarations are carried over unchanged (a cut's derived
    // counts describe what it holds; the validator checks those).
    if (!std::equal(ss2.begin() + 6152, ss2.begin() + 6208, ds2.begin() + 6152) ||
        (!cut_segment_ && !std::equal(ss2.begin() + 6336, ss2.begin() + 6432, ds2.begin() + 6336)))
      problem(drel, "section 2", "signal declarations (sampling frequency, units factor, "
                                 "filters, counts, maxima) differ from the source's");
    dst_encrypted_ = d.s2_level > 0;
    src_keys_ = s.keys;
    dst_keys_ = d.keys;
    if (progress_) progress_(2 * TMET);
  }

  /// A transform's sections 1-3 (decrypted): the source's, byte for byte,
  /// except the encryption levels and the edited fields, which must hold
  /// exactly the edit.
  void check_sections_transform(std::span<const ui1> sb, std::span<const ui1> db,
                                const std::string& drel, const TmetImage& d) {
    if (!std::equal(sb.begin() + S1 + 2, sb.begin() + S1 + S1_BYTES, db.begin() + S1 + 2))
      problem(drel, "section 1", "differs from the source's beyond the encryption levels");
    const bool want_enc = !o_.output_password.empty();
    if ((d.s2_level > 0) != want_enc || (d.s3_level > 0) != want_enc)
      problem(drel, "section 1 encryption levels", "do not match the output passwords");
    // The channel this segment belongs to: "<ch>.timd/<ch>-NNNNNN.segd/..".
    const std::string channel = drel.substr(0, drel.find(".timd/"));
    std::vector<ui1> expect(sb.begin(), sb.end());
    std::span<ui1> e(expect);
    for (const auto& f : edited_fields(*edits_, channel))
      write_string(e.subspan(f.section), f.offset, f.bytes, f.value);
    if (cut_segment_)  // the derived declarations describe the cut (the validator checks them)
      for (const auto& [from, to] : {std::pair<std::size_t, std::size_t>{4096, 4104}, {6336, 6432}})
        std::copy(db.begin() + S2 + from, db.begin() + S2 + to, e.begin() + S2 + from);
    rep_.text_fields_checked += 7;
    rep_.times_checked += 4;  // rto, DST start/end, GMT: inside section 3
    if (!std::equal(e.begin() + S2, e.begin() + S2 + S2_BYTES, db.begin() + S2))
      problem(drel, "section 2", "differs from the source's beyond the requested edits");
    if (!std::equal(e.begin() + S3, e.begin() + S3 + S3_BYTES, db.begin() + S3))
      problem(drel, "section 3", "differs from the source's beyond the requested edits");
  }

  void check_segment_data(const detail::LayoutSegment& s, const detail::LayoutSegment& d) {
    if (cut_segment_) {
      check_segment_data_cut(s, d);
      return;
    }
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
    ui4 dmax = 0;  // the largest block the copy holds
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
          dmax = std::max(dmax, dn);
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
          if (!std::equal(sent.begin() + 16, sent.begin() + 40, dent.begin() + 16) ||
              (sent[44] & ~ENC_MASK) != (dent[44] & ~ENC_MASK))
            problem(loc, "index entry", "differs from the source's beyond its time");
          // meflib's per-entry protected (40, 4 B), RED-protected (45, 3 B)
          // and discretionary (48, 8 B) regions: another writer's bytes.
          if (transform()) {
            if (!std::equal(sent.begin() + 40, sent.begin() + 44, dent.begin() + 40) ||
                !std::equal(sent.begin() + 45, sent.end(), dent.begin() + 45))
              problem(loc, "index entry regions", "differ from the source's");
          } else if (!all_zero(dent.subspan(40, 4)) || !all_zero(dent.subspan(45, 11))) {
            leak(loc, "index entry regions", "are not cleared");
          }
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
    check_counts(duh_x, static_cast<si8>(n), static_cast<si8>(ENTRY), d.tidx);
    check_counts(duh_d, static_cast<si8>(n), static_cast<si8>(dmax), d.tdat);
    if (read<ui4>(duh_x, 4) != dcrc_x) problem(d.tidx, "body CRC", "does not verify");
    if (read<ui4>(duh_d, 4) != dcrc_d) problem(d.tdat, "body CRC", "does not verify");
  }

  /// A segment of a cut copy against its source segment, in LOCKSTEP: the
  /// source's blocks around the window are walked in order (placed by their
  /// own headers, the authority), and each one holding samples of the window
  /// must be matched by the copy's NEXT block: byte-identical when kept
  /// whole (check_block), or holding exactly its samples in the window when
  /// trimmed (decoded and compared). A copy block left over, missing,
  /// repeated or out of order fails. The source blocks just outside the
  /// range searched must hold nothing of the window (an index out of order
  /// there would hide blocks). Both files are read in bounded batches, only
  /// around the window.
  /// `piece`: a merge's piece, the source entries [first, last) copied whole
  /// (no window: every block of the range is kept).
  void check_segment_data_cut(const detail::LayoutSegment& s, const detail::LayoutSegment& d,
                              std::optional<std::pair<std::size_t, std::size_t>> piece = {}) {
    const si8 t0 = piece ? std::numeric_limits<si8>::min() + 1 : o_.time_range->first;
    const si8 t1 = piece ? std::numeric_limits<si8>::max() : o_.time_range->second;
    const bool exact = o_.exact;
    const auto dtidx = dst_.file_size(d.tidx), dtdat = dst_.file_size(d.tdat);
    const auto stdat = src_.file_size(s.tdat);
    const auto suh_x = src_.read_range(s.tidx, 0, UH), duh_x = dst_.read_range(d.tidx, 0, UH);
    const auto suh_d = src_.read_range(s.tdat, 0, UH), duh_d = dst_.read_range(d.tdat, 0, UH);
    collect_source_uuids(suh_x);
    collect_source_uuids(suh_d);
    check_uh(suh_x, duh_x, d.tidx);
    check_uh(suh_d, duh_d, d.tdat);
    if (dtidx < UH || (dtidx - UH) % ENTRY) {
      problem(d.tidx, "block index", "ends in a partial entry");
      return;
    }
    const std::size_t nd = static_cast<std::size_t>((dtidx - UH) / ENTRY);
    const auto de = dst_.read_range(d.tidx, UH, nd * ENTRY);  // the copy: O(window)
    const ui4 dcrc_x = crc::calculate(de, fmt::CRC_START_VALUE);
    const std::size_t ns = static_cast<std::size_t>((src_.file_size(s.tidx) - UH) / ENTRY);
    const auto [f, l] =
        piece ? *piece : window_entries(src_, s.tidx, seg_rto_, t0, t1, seg_reach_us_);

    // A source block's header (time, samples), read at the entry's offset.
    auto header_of = [&](std::span<const ui1> block) {
      return std::make_pair(to_user_time(read<si8>(block, 40), seg_rto_),
                            static_cast<std::size_t>(read<ui4>(block, 32)));
    };
    for (const std::size_t i : {f > 0 ? f - 1 : ns, l})
      if (i < ns && !piece) {  // a piece's neighbours belong to other pieces
        const auto e = src_.read_range(s.tidx, UH + i * ENTRY, ENTRY);
        const auto off = static_cast<std::uint64_t>(read<si8>(e, 0));
        if (off + fmt::RED_BLOCK_HEADER_BYTES > stdat) continue;  // the validator's
        const auto [t, n] = header_of(src_.read_range(s.tdat, off, fmt::RED_BLOCK_HEADER_BYTES));
        const auto [a, b] = window_slice(t, n, seg_fs_, t0, t1);
        if (a < b)
          problem(s.tidx, "block index",
                  "is out of time order around the time range, so the cut cannot be checked");
      }

    // The copy's blocks, read forward in bounded batches.
    constexpr std::uint64_t BATCH = 8u << 20;
    std::vector<ui1> dbuf;
    std::uint64_t dlo = 0, dexpect = UH;
    std::size_t dloaded = 0;  // copy blocks [0, dloaded) have been read
    ui4 dcrc_d = fmt::CRC_START_VALUE;
    bool broken = false;
    auto load_until = [&](std::size_t j) {  // make copy block j available
      while (!broken && dloaded <= j && dloaded < nd) {
        std::size_t k = dloaded;
        std::uint64_t lo = dexpect, hi = dexpect;
        while (k < nd) {
          const auto off = static_cast<std::uint64_t>(read<si8>(de, k * ENTRY));
          const auto nb = read<ui4>(de, k * ENTRY + 28);
          if (off != hi || off + nb > dtdat || nb < fmt::RED_BLOCK_HEADER_BYTES) {
            problem(d.tidx, "entry " + std::to_string(k),
                    "does not describe the next block of the data file (offset/size)");
            broken = true;
            return;
          }
          if (k > dloaded && off + nb - lo > BATCH) break;
          hi = off + nb;
          ++k;
        }
        dbuf = dst_.read_range(d.tdat, lo, hi - lo);
        dcrc_d = crc::calculate(dbuf, dcrc_d);
        if (progress_) progress_(dbuf.size());
        dlo = lo;
        dexpect = hi;
        dloaded = k;
      }
    };
    auto dst_block = [&](std::size_t j) {
      const auto off = static_cast<std::uint64_t>(read<si8>(de, j * ENTRY));
      return std::span<const ui1>(dbuf.data() + (off - dlo), read<ui4>(de, j * ENTRY + 28));
    };

    // The source's blocks around the window, in order, in bounded batches.
    const auto se = src_.read_range(s.tidx, UH + f * ENTRY, (l - f) * ENTRY);
    std::optional<si8> span_first, span_end;  // user uUTC, as the source places them
    std::size_t j = 0;  // the copy's next block
    ui4 dmax = 0;
    std::size_t i = 0;
    while (i < l - f && !broken) {
      ctl_.check();
      std::size_t k = i;
      std::uint64_t lo = 0, hi = 0;
      while (k < l - f) {
        const auto off = static_cast<std::uint64_t>(read<si8>(se, k * ENTRY));
        const auto nb = read<ui4>(se, k * ENTRY + 28);
        if (off + nb > stdat || nb < fmt::RED_BLOCK_HEADER_BYTES) {
          problem(s.tidx, "entry " + std::to_string(f + k), "lies outside the data file");
          return;
        }
        if (k > i && (off < hi || off + nb - lo > BATCH)) break;
        if (k == i) lo = off;
        hi = std::max(hi, off + nb);
        ++k;
      }
      const auto sbuf = src_.read_range(s.tdat, lo, hi - lo);
      for (std::size_t b = i; b < k && !broken; ++b) {
        std::span<const ui1> sblk(sbuf.data() + (read<si8>(se, b * ENTRY) - lo),
                                  read<ui4>(se, b * ENTRY + 28));
        const auto [ts, ns_b] = header_of(sblk);
        const auto [a, e] = window_slice(ts, ns_b, seg_fs_, t0, t1);
        if (a >= e) continue;  // nothing of this block is in the window
        const bool whole = !exact || (a == 0 && e == ns_b);
        // The span the copy's segment holds, from the source block's header
        // (the same rule the copy uses), checked against its headers below.
        if (!span_first) span_first = sample_time(ts, whole ? 0 : a, seg_fs_);
        span_end = sample_time(ts, whole ? ns_b : e, seg_fs_);
        load_until(j);
        if (broken) break;
        const std::string loc = d.tdat + " block " + std::to_string(j);
        if (j >= nd) {
          problem(d.tdat, "blocks", "the copy lacks samples the source has in the time range");
          broken = true;
          break;
        }
        const auto dblk = dst_block(j);
        const auto dent = std::span<const ui1>(de).subspan(j * ENTRY, ENTRY);
        const auto sent = std::span<const ui1>(se).subspan(b * ENTRY, ENTRY);
        if (whole) {
          // Its entry as the source's (verbatim, or shifted): even an index
          // time that disagrees with the header stays what it was.
          time_field(read<si8>(sent, 8), read<si8>(dent, 8), loc, "index entry start_time");
          if (read<ui4>(dent, 24) != read<ui4>(sent, 24))
            problem(loc, "index entry", "sample count differs from the source's");
        } else if (read<si8>(dent, 8) != read<si8>(dblk, 40) ||
                   read<ui4>(dent, 24) != read<ui4>(dblk, 32)) {
          problem(loc, "index entry", "time or sample count differs from its re-encoded block's");
        }
        if (whole) {
          if (dblk.size() != sblk.size())
            problem(loc, "block", "is not the source's block kept whole");
          else
            check_block(sblk, dblk, loc);  // byte-identical
        } else {
          if (read<ui4>(dblk, 32) != e - a)
            problem(loc, "block", "does not hold the source block's samples in the time range");
          else
            check_trimmed_block(sblk, dblk, a, loc);
          time_field(stored_like(read<si8>(sblk, 40), sample_time(ts, a, seg_fs_), seg_rto_),
                     read<si8>(dblk, 40), loc, "block header start_time");
        }
        dmax = std::max(dmax, static_cast<ui4>(dblk.size()));
        ++rep_.blocks;
        ++j;
      }
      i = k;
    }
    if (!broken) {
      if (j < nd)
        problem(d.tdat, "blocks", "the copy holds blocks the source does not have in the time range");
      load_until(nd);  // the rest, for the CRC
    }
    if (broken) return;
    // Every universal header of the segment (.tmet, .tidx, .tdat) spans
    // exactly what it holds: its first kept sample to just past its last,
    // in the copy's own offset and (when shifting) moved by the shift. The
    // validator cannot be left this: its time checks stand down at rto != 0.
    if (span_first) {
      const si8 moved = (transform() || keep_times_) ? 0 : sh_.shift;
      const std::pair<std::span<const ui1>, const std::string*> uhs[] = {
          {dst_tmet_uh_, &d.tmet}, {duh_x, &d.tidx}, {duh_d, &d.tdat}};
      for (const auto& [uh, where] : uhs) {
        if (uh.size() < UH) continue;
        rep_.times_checked += 2;
        const si8 a = read<si8>(uh, UH_START), b = read<si8>(uh, UH_END);
        if (is_sentinel(a, false) || to_user_time(a, dst_seg_rto_) != *span_first + moved)
          problem(*where, "universal header start_time", "is not the first sample the segment holds");
        if (is_sentinel(b, false) || to_user_time(b, dst_seg_rto_) != *span_end + moved)
          problem(*where, "universal header end_time", "is not the end of what the segment holds");
      }
    }
    if (dexpect != dtdat) problem(d.tdat, "data file", "holds bytes no index entry describes");
    if (read<ui4>(duh_x, 4) != dcrc_x) problem(d.tidx, "body CRC", "does not verify");
    if (read<ui4>(duh_d, 4) != dcrc_d) problem(d.tdat, "body CRC", "does not verify");
    check_counts(duh_x, static_cast<si8>(nd), static_cast<si8>(ENTRY), d.tidx);
    check_counts(duh_d, static_cast<si8>(nd), static_cast<si8>(dmax), d.tdat);
  }

  /// A re-encoded edge block: a valid block whose samples are exactly the
  /// source block's from sample `i0`, at the encryption the copy asks for.
  void check_trimmed_block(std::span<const ui1> s, std::span<const ui1> d, std::size_t i0,
                           const std::string& loc) {
    if (crc::calculate(d.subspan(4)) != read<ui4>(d, 0)) {
      problem(loc, "block CRC", "does not verify");
      return;
    }
    const ui1 sf = s[4], df = d[4];
    if ((sf & ENC_MASK) ? (dst_encrypted_ ? (df & ENC_MASK) != (sf & ENC_MASK) : (df & ENC_MASK) != 0)
                        : (df & ENC_MASK) != 0)
      problem(loc, "block encryption", "does not match the source's level and the output passwords");
    try {
      const red::DecodedBlock a = red::decode_block(s, src_keys_);
      const red::DecodedBlock b = red::decode_block(d, dst_keys_);
      if (i0 + b.samples.size() > a.samples.size() ||
          !std::equal(b.samples.begin(), b.samples.end(), a.samples.begin() + static_cast<std::ptrdiff_t>(i0)))
        problem(loc, "samples", "differ from the source's at the same times");
    } catch (const std::exception& e) {
      problem(loc, "block", std::string("cannot be decoded: ") + e.what());
    }
  }

  void check_block(std::span<const ui1> s, std::span<const ui1> d, const std::string& loc) {
    if (crc::calculate(d.subspan(4)) != read<ui4>(d, 0))
      problem(loc, "block CRC", "does not verify");
    const ui1 sf = s[4], df = d[4];
    // A cut flags the first block it keeps after dropped ones.
    const ui1 free = cut_segment_ ? ENC_MASK | fmt::RedBlockHeader::DISCONTINUITY_MASK : ENC_MASK;
    if ((sf & ~free) != (df & ~free) ||
        !std::equal(s.begin() + 16, s.begin() + 40, d.begin() + 16))
      problem(loc, "block header", "differs from the source's beyond its time and encryption");
    // Protected (5, 3 B) and discretionary (8, 8 B) regions of the header.
    if (transform()) {
      if (!std::equal(s.begin() + 5, s.begin() + 16, d.begin() + 5))
        problem(loc, "block header regions", "differ from the source's");
    } else if (!all_zero(d.subspan(5, 11))) {
      leak(loc, "block header regions", "are not cleared");
    }
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

  /// `rto`: the offset the source file's times are read with (null = the
  /// session's); pointing at nullopt = its segments disagree, so a time
  /// stored against it cannot be placed in a cut.
  void check_records(const std::string& srel, const std::string& drel,
                     const std::set<std::string>& excluded,
                     const std::function<bool(si8)>& keep_time = {},
                     const std::optional<si8>* rto = nullptr) {
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
    if (d.has_ridx)
      check_uh(s.has_ridx ? s.ridx_uh : s.uh, d.ridx_uh, ridx_of(drel), false, !s.has_ridx);
    else problem(drel, "record index", "is missing");
    {
      si8 largest = 0;
      for (const auto& r : d.records)
        largest = std::max(largest, static_cast<si8>(RHDR + r.body.size()));
      const si8 n = static_cast<si8>(d.records.size());
      check_counts(d.uh, n, largest, drel);
      if (d.has_ridx) check_counts(d.ridx_uh, n, static_cast<si8>(RIDX), ridx_of(drel));
    }
    // A cut keeps the records in its window (the same rule as the copy),
    // each placed with the offset ITS file is read with.
    const std::optional<si8> lrto = rto ? *rto : std::optional<si8>(sh_.rto);
    const si8 r_rto = lrto.value_or(sh_.rto);
    std::vector<std::size_t> keep;
    for (std::size_t i = 0; i < s.records.size(); ++i) {
      const RawRecord& r = s.records[i];
      if ((o_.time_range || keep_time) && !lrto && record_offset_dependent(r)) {
        problem(drel, "records",
                "a source record's time is stored against a recording-time offset its "
                "segments disagree on (or that cannot be read), so whether a cut keeps it is "
                "unknown");
        return;
      }
      if (o_.time_range && !record_in_window(r, r_rto, o_.time_range->first, o_.time_range->second))
        continue;
      if (keep_time && !keep_time(is_sentinel(r.time, false) ? r.time : to_user_time(r.time, r_rto)))
        continue;
      keep.push_back(i);
    }
    if (keep.size() != d.records.size()) {
      problem(drel, "records", "the copy holds " + std::to_string(d.records.size()) +
                                   " record(s), the source " + std::to_string(keep.size()));
      return;
    }
    for (std::size_t k = 0; k < keep.size(); ++k) {
      const std::size_t i = keep[k];
      const RawRecord& a = s.records[i];
      const RawRecord& b = d.records[k];
      const std::string loc = drel + " record " + std::to_string(k);
      ++rep_.records;
      if (a.type_bytes != b.type_bytes || a.vmaj != b.vmaj || a.vmin != b.vmin) {
        problem(loc, "record header", "type or version differs from the source's (order?)");
        continue;
      }
      time_field(a.time, b.time, loc, "record header time");
      if (b.enc != record_encryption(a.enc, !o_.output_password.empty(), !transform()))
        problem(loc, "record header encryption", "does not match the output passwords");
      if (d.has_ridx) {
        // Index entries are matched to records by file offset, not position.
        const auto* de = index_entry(d, k);
        const auto* se = s.has_ridx ? index_entry(s, i) : nullptr;
        if (!de) {
          problem(loc, "record index", "has no entry pointing at the record");
        } else {
          time_field(se ? read<si8>(*se, 16) : a.time, read<si8>(*de, 16), loc,
                     "record index time");
          // Type, version and encryption as the record's own header says.
          if (!std::equal(b.type_bytes.begin(), b.type_bytes.end(), de->begin()) ||
              (*de)[5] != b.vmaj || (*de)[6] != b.vmin || static_cast<si1>((*de)[7]) != b.enc)
            problem(loc, "record index entry", "type, version or encryption differs from the record's");
        }
      }
      check_body(a, b, loc, excluded);
    }
  }

  /// A transform keeps a body as stored (padded to 16 bytes), except the
  /// Seiz entries of left-out channels. Checked entry by entry rather than by
  /// rebuilding the body the way the copy does: the fixed part equal but for
  /// the count, and the copy's entries, in order, exactly the source's
  /// entries that name a kept channel.
  void check_body_transform(const RawRecord& a, const RawRecord& b, const std::string& loc,
                            const std::set<std::string>& excluded) {
    auto padding_only = [](const std::vector<ui1>& v, std::size_t from) {
      return v.size() >= from && v.size() - from < 16 &&
             std::all_of(v.begin() + static_cast<std::ptrdiff_t>(from), v.end(),
                         [](ui1 c) { return c == PAD_BYTE; });
    };
    if (a.type != "Seiz" || !layout_of(a)) {
      if (!(b.body.size() >= a.body.size() &&
            std::equal(a.body.begin(), a.body.end(), b.body.begin()) &&
            padding_only(b.body, a.body.size())))
        problem(loc, "record body", "differs from the source's");
      return;
    }
    if (b.body.size() < SEIZ_FIXED ||
        !std::equal(a.body.begin(), a.body.begin() + SEIZ_NCH, b.body.begin()) ||
        !std::equal(a.body.begin() + SEIZ_NCH + 4, a.body.begin() + SEIZ_FIXED,
                    b.body.begin() + SEIZ_NCH + 4)) {
      problem(loc, "record body", "differs from the source's");
      return;
    }
    std::size_t k = 0;  // the copy's next entry
    const std::size_t have = seiz_entry_count(b.body);
    for (std::size_t c = 0; c < seiz_entry_count(a.body); ++c) {
      if (excluded.count(channel_key(seiz_entry_name(a.body, c)))) continue;
      const auto src = a.body.begin() + static_cast<std::ptrdiff_t>(SEIZ_FIXED + c * SEIZ_CHANNEL);
      if (k >= have || !std::equal(src, src + SEIZ_CHANNEL,
                                   b.body.begin() + static_cast<std::ptrdiff_t>(
                                                        SEIZ_FIXED + k * SEIZ_CHANNEL))) {
        problem(loc, "Seiz channels", "are not the source's entries for the kept channels");
        return;
      }
      ++k;
    }
    const bool whole = k == seiz_entry_count(a.body);  // nothing left out: stored as is
    if (k != have || read<si4>(b.body, SEIZ_NCH) != static_cast<si4>(k) ||
        !padding_only(b.body, whole ? a.body.size() : SEIZ_FIXED + k * SEIZ_CHANNEL))
      problem(loc, "Seiz channels", "the copy lists other entries than the source's kept ones");
  }

  static const std::array<ui1, RIDX>* index_entry(const RawRecordFile& f, std::size_t i) {
    for (const auto& e : f.ridx)
      if (read<si8>(e, 8) == f.record_offsets[i]) return &e;
    return nullptr;
  }

  void check_body(const RawRecord& a, const RawRecord& b, const std::string& loc,
                  const std::set<std::string>& excluded) {
    if (transform()) {
      check_body_transform(a, b, loc, excluded);
      return;
    }
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
    if (la->keep_whole) {  // LNTP: the template only, then padding
      const std::size_t n = lntp_bytes(a.body);
      if (b.body.size() < n || !std::equal(a.body.begin(), a.body.begin() + n, b.body.begin()))
        problem(loc, "record body", "differs from the source's");
      else if (!std::all_of(b.body.begin() + n, b.body.end(),
                            [](ui1 c) { return c == PAD_BYTE || c == 0; }))
        leak(loc, "record body", "holds bytes after the template");
      return;
    }
    if (la->variable_text_from != SIZE_MAX) {
      const std::size_t from = la->variable_text_from;
      if (from && !std::equal(a.body.begin(), a.body.begin() + from, b.body.begin()))
        problem(loc, "record body", "duration differs from the source's");
      text(b.body, from, b.body.size() - from, loc, "record text", true);
      return;
    }
    for (std::size_t off : la->times)
      time_field(read<si8>(a.body, off), read<si8>(b.body, off), loc,
                 "record body time at " + std::to_string(off), true);
    // Epoc / Curs id_number (+0) is an si8 a recorder may fill with a time.
    // It is copied as is; a value in the recording range is reported (when
    // shifting: a copy that keeps the times shows the dates anyway).
    if (!keep_times_ && (a.type == "Epoc" || a.type == "Curs")) {
      ++rep_.times_checked;
      if (in_absolute_window(read<si8>(b.body, 0)))
        leak(loc, "record id_number", "holds a value inside the source's recording range");
    }
    for (const auto& slot : la->text)
      text(b.body, slot.offset, slot.bytes, loc, std::string("record ") + slot.name);
    if (b.body.size() > lb->fixed && a.type != "Seiz")
      leak(loc, "record body", "holds bytes after its fixed part");
    if (a.type != "Seiz") return;
    // Channel entries: the source's minus the excluded, in order, shifted.
    std::vector<std::size_t> want;
    const auto entries = [](const RawRecord& r) { return seiz_entry_count(r.body); };
    for (std::size_t c = 0; c < entries(a); ++c)
      if (!excluded.count(
              channel_key(read_string(a.body, SEIZ_FIXED + c * SEIZ_CHANNEL, SEIZ_CHANNEL_NAME))))
        want.push_back(c);
    const std::size_t have = entries(b);
    if (!std::all_of(b.body.begin() + static_cast<std::ptrdiff_t>(
                                          std::min(b.body.size(), SEIZ_FIXED + have * SEIZ_CHANNEL)),
                     b.body.end(), [](ui1 c) { return c == PAD_BYTE || c == 0; }))
      leak(loc, "record body", "holds bytes after its channel entries");
    if (have != want.size() || read<si4>(b.body, SEIZ_NCH) != static_cast<si4>(have)) {
      problem(loc, "Seiz channels", "the copy lists " + std::to_string(have) + ", expected " +
                                        std::to_string(want.size()));
      return;
    }
    for (std::size_t k = 0; k < have; ++k) {
      const std::size_t so = SEIZ_FIXED + want[k] * SEIZ_CHANNEL, dof = SEIZ_FIXED + k * SEIZ_CHANNEL;
      // The name may be kept, redacted or cleared; it must not leak.
      {
        const std::string en = read_string(b.body, dof, SEIZ_CHANNEL_NAME);
        name_field(en, loc, "Seiz channel name");
        if (!all_zero(std::span<const ui1>(b.body).subspan(dof + en.size(),
                                                            SEIZ_CHANNEL_NAME - en.size())))
          leak(loc, "Seiz channel name", "holds bytes after its terminator (hidden text)");
      }
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
  const MetadataEdits* edits_;  // set = a transform
  bool keep_times_ = false;     // anonymize without a shift: every time kept
  std::map<Uuid, Uuid> level_map_;  // transform: source level UUID -> the copy's
  // A cut (#103): set while a segment of the copy is checked.
  bool cut_segment_ = false;
  bool merge_ = false;
  si8 seg_rto_ = 0;  // the source segment's
  si8 dst_seg_rto_ = 0;              // the copy segment's (check_tmet)
  std::vector<ui1> dst_tmet_uh_;     // the copy segment's .tmet header (check_tmet)
  sf8 seg_fs_ = 0;
  si8 seg_reach_us_ = 0;
  const Control& ctl_;
  std::function<void(std::uint64_t)> progress_;
  VerifyReport& rep_;
  Shifter sh_;
  Scanner scan_;
  std::set<std::string> kept_;
  si8 lo_ = 0, hi_ = 0;
  std::set<Uuid> src_uuids_;
  bool dst_encrypted_ = false;
  crypto::AccessKeys src_keys_, dst_keys_;
};

std::string finding_key(const Finding& f) {
  return f.check_id + " | " + f.channel + " | segment " + std::to_string(f.segment_number) +
         " | " + f.field + " | " + f.message;
}

/// `edits` set = verify a transform (MetadataEdits it was made with); unset =
/// verify an anonymized copy (shift, VerifyOptions' clear flags).
VerifyReport verify_impl(const std::string& src_path, const SessionSource& src,
                         const SourcePlan& plan, const std::string& dst_path,
                         std::optional<si8> shift,
                         const VerifyOptions& o, const MetadataEdits* edits, const Control& ctl,
                         std::function<void(std::uint64_t)> progress,
                         const std::vector<std::string>* left_out_without_samples = nullptr) {
  VerifyReport rep;
  const auto dsrc = open_session_source(dst_path);
  const SessionSource& dst = *dsrc;
  const auto dlayout = detail::discover_layout(dst);

  // Channels: the copy's must be a subset of the source's.
  std::set<std::string> dch;
  for (const auto& c : dlayout.channels) dch.insert(c.name);
  for (const auto& c : dlayout.record_only_channels) dch.insert(c);
  std::set<std::string> excluded, excluded_keys;
  for (const auto& c : plan.all_channels)
    if (!dch.count(c)) {
      excluded.insert(c);
      excluded_keys.insert(channel_key(c));
    }
  rep.channels.assign(dch.begin(), dch.end());
  rep.excluded_channels.assign(excluded.begin(), excluded.end());

  Verifier v(src, dst, plan, shift, o, edits, ctl, std::move(progress), rep);
  for (const auto& c : dch)
    if (!std::binary_search(plan.all_channels.begin(), plan.all_channels.end(), c))
      v.problem(c, "channel", "is not a channel of the source");
  // A channel with records and no segment is listed as incomplete too (the
  // layout's rule); the copy writes one for a record-only source channel.
  // Accepted only where the SOURCE's channel is record-only too: a data
  // channel whose segments are gone from the copy is a loss, not a layout.
  std::map<std::string, std::size_t> source_segments;  // data channels
  for (const auto& c : plan.layout.channels) source_segments[c.name] = c.segments.size();
  bool bad_layout = !dlayout.video_channels.empty();
  for (const auto& c : dlayout.incomplete_channels) {
    const bool record_only =
        c.incomplete.empty() &&
        std::find(dlayout.record_only_channels.begin(), dlayout.record_only_channels.end(),
                  c.name) != dlayout.record_only_channels.end();
    if (!record_only) bad_layout = true;
  }
  if (bad_layout)
    v.problem(dst_path, "layout", "the copy holds video or incomplete channel folders");
  // Every source data channel the copy keeps must be a data channel there
  // (segment counts are compared below).
  {
    std::set<std::string> ddata;
    for (const auto& c : dlayout.channels) ddata.insert(c.name);
    for (const auto& [name, n] : source_segments)
      if (dch.count(name) && !ddata.count(name))
        v.problem(name, "segments",
                  "has no segments in the copy; the source's has " + std::to_string(n));
  }

  // The shift floor, re-checked: below it the leak window cannot tell a
  // leak from a legitimately shifted time. (No shift: the times are kept,
  // and every one is compared with the source's instead.)
  if (!edits && shift && plan.has_times) {
    const si8 span = saturating_add(plan.last, -plan.first);
    const si8 mag = *shift == INT64_MIN ? INT64_MAX : (*shift < 0 ? -*shift : *shift);
    if (mag < saturating_add(span, DAY_US))
      v.problem(dst_path, "shift", "|shift| is below the session's time span plus one day");
  }

  const std::string dname = dst.session_name();
  if (edits) {
    v.set_kept(dch);
  } else {
    // The anonymized_name the copy carries is its pseudonym, not a leak.
    std::string pseudonym;
    if (!dlayout.channels.empty() && !dlayout.channels.front().segments.empty()) {
      const auto uh = dst.read_range(dlayout.channels.front().segments.front().tmet, 0, UH);
      pseudonym = read_string(uh, UH_ANON_NAME, UH_NAME_BYTES);
    }
    v.set_scanner(build_scanner(plan, dname, o.clear_subject, o.clear_location, excluded, dch,
                                pseudonym));
    v.set_kept(dch);
  }
  // Every name the copy carries outside its files: each path component
  // (anonymize: none may carry what was removed).
  {
    std::function<void(const std::string&)> walk = [&](const std::string& rel) {
      for (const auto& e : dst.list_dir(rel)) {
        const std::string p = rel.empty() ? e.name : rel + "/" + e.name;
        v.path_component(e.name, p);
        if (e.is_dir) walk(p);
      }
    };
    if (!edits) walk("");
    for (const auto& dc : dlayout.channels)
      for (const auto& ds : dc.segments)
        if (leaf(ds.dir()) != canonical_segment_stem(dc.name, ds.number) + ".segd")
          v.problem(ds.dir(), "segment folder", "is not named <channel>-NNNNNN.segd");
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
    // Compared as a multiset: each source finding excuses one copy finding.
    // A finding is excused only when the source had exactly the same one
    // (check, channel, segment, field, message). A cut recomputes the very
    // declarations several checks look at, so it never matches more loosely:
    // a cut of a session that already fails such a check is refused (repair
    // the source first) rather than risk excusing the cut's own mistake.
    std::map<std::string, int> known;
    for (const auto& f : sr.findings) ++known[finding_key(f)];
    for (const auto& f : dr.findings) {
      if (f.severity == Severity::Info) continue;
      const std::string key = finding_key(f);
      auto it = known.find(key);
      if (it != known.end() && it->second > 0) {
        --it->second;
        rep.inherited_findings.push_back(scrub_times(plan, key));
      } else {
        v.problem(f.path, f.check_id, "the validator reports on the copy: " + f.message);
      }
    }
    for (const auto& s : dr.skipped)
      v.problem(s.path, "validator", "could not check a segment of the copy: " + s.reason);
  }

  // A cut: the offsets the SOURCE's files are read with, from its own
  // metadata (not the plan's), and which source channels hold samples in
  // the window (each segment by its own offset, each block by its header).
  std::map<std::string, si8> src_seg_rto;
  auto seg_rto = [&](const std::string& tmet) {
    auto it = src_seg_rto.find(tmet);
    if (it == src_seg_rto.end())
      it = src_seg_rto
               .emplace(tmet, segment_rto(load_time_series_metadata(src.read_all(tmet),
                                                                    o.password)))
               .first;
    return it->second;
  };
  std::function<std::optional<si8>(const std::string*)> level_rto =
      [&](const std::string* channel) -> std::optional<si8> {
    std::optional<si8> agreed;
    for (const auto& c : plan.layout.channels) {
      if (channel && c.name != *channel) continue;
      for (const auto& x : c.segments) {
        const si8 r = seg_rto(x.tmet);
        if (agreed && *agreed != r) return std::nullopt;
        agreed = r;
      }
    }
    if (channel && !agreed) return level_rto(nullptr);  // no segments: the session's
    return agreed.value_or(0);
  };
  auto has_samples_in_window = [&](const detail::LayoutChannel& c) {
    const auto [t0, t1] = *o.time_range;
    for (const auto& x : c.segments) {
      ctl.check();
      const TmetImage t = open_tmet(src, x.tmet, o.password);
      const auto m = fmt::TimeSeriesMetadataSection2::parse(
          std::span<const ui1>(t.bytes).subspan(S2, S2_BYTES));
      if (!(m.sampling_frequency > 0)) continue;
      const auto [f, l] = window_entries(src, x.tidx, t.rto, t0, t1, longest_block_us(m));
      if (f >= l) continue;
      const auto es = src.read_range(x.tidx, UH + f * ENTRY, (l - f) * ENTRY);
      const auto tdat_bytes = src.file_size(x.tdat);
      for (std::size_t i = 0; i < l - f; ++i) {
        const auto off = static_cast<std::uint64_t>(read<si8>(es, i * ENTRY));
        if (off + fmt::RED_BLOCK_HEADER_BYTES > tdat_bytes) continue;
        const auto h = src.read_range(x.tdat, off, fmt::RED_BLOCK_HEADER_BYTES);
        const auto [a, b] = window_slice(to_user_time(read<si8>(h, 40), t.rto),
                                         read<ui4>(h, 32), m.sampling_frequency, t0, t1);
        if (a < b) return true;
      }
    }
    return false;
  };
  if (o.time_range) {
    std::set<std::string> claimed;
    if (left_out_without_samples)
      claimed.insert(left_out_without_samples->begin(), left_out_without_samples->end());
    for (const auto& c : plan.layout.channels) {
      if (dch.count(c.name)) continue;
      if (left_out_without_samples && !claimed.count(c.name)) continue;  // excluded on request
      if (!has_samples_in_window(c)) continue;
      if (left_out_without_samples)
        v.problem(c.name, "channel",
                  "was left out as having no samples in time_range, but the source has samples "
                  "there");
      else
        v.note(c.name, "channel", "has samples in time_range but is not in the copy (excluded?)");
    }
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
    const bool cut = o.time_range.has_value();
    if (!cut && sc.segments.size() != dc.segments.size())
      v.problem(dc.name, "segments", "the copy holds " + std::to_string(dc.segments.size()) +
                                         ", the source " + std::to_string(sc.segments.size()));
    // A cut renumbers what it keeps: a segment of the copy is matched to the
    // source segment whose span holds its first time, in order.
    std::vector<std::pair<si8, si8>> spans;  // source segments, user uUTC
    if (cut)
      for (const auto& x : sc.segments) {
        const auto uh = src.read_range(x.tmet, 0, UH);
        const si8 rto = segment_rto(load_time_series_metadata(src.read_all(x.tmet), o.password));
        spans.push_back({to_user_time(read<si8>(uh, UH_START), rto),
                         to_user_time(read<si8>(uh, UH_END), rto)});
      }
    std::size_t next = 0;
    int expected_number = 0;  // a cut numbers each channel's segments from 0
    for (const auto& ds : dc.segments) {
      auto ss = sc.segments.end();
      if (!cut) {
        // Matched by segment NUMBER: the copy names its folders canonically.
        ss = std::find_if(sc.segments.begin(), sc.segments.end(),
                          [&](const detail::LayoutSegment& x) { return x.number == ds.number; });
      } else {
        const auto uh = dst.read_range(ds.tmet, 0, UH);
        const si8 stored = read<si8>(uh, UH_START);
        for (; next < sc.segments.size(); ++next) {
          const si8 rto = segment_rto(
              load_time_series_metadata(src.read_all(sc.segments[next].tmet), o.password));
          const si8 t = (edits || !shift) ? to_user_time(stored, rto)
                                          : to_user_time(stored, 0) - *shift;
          if (t >= spans[next].first && t < spans[next].second) {
            ss = sc.segments.begin() + static_cast<std::ptrdiff_t>(next++);
            break;
          }
        }
        if (ds.number != expected_number++)
          v.problem(ds.dir(), "segment", "is not numbered in order from 0");
      }
      if (ss == sc.segments.end()) {
        v.problem(ds.dir(), "segment", "has no counterpart in the source");
        continue;
      }
      v.set_cut_segment(cut);
      v.check_tmet(ss->tmet, ds.tmet);
      v.check_segment_data(*ss, ds);
      ++rep.segments;
      const bool s_rec = src.exists(ss->rdat()), d_rec = dst.exists(ds.rdat());
      if (s_rec && d_rec) {
        const std::optional<si8> r = cut ? std::optional<si8>(seg_rto(ss->tmet)) : std::nullopt;
        v.check_records(ss->rdat(), ds.rdat(), excluded_keys, {}, cut ? &r : nullptr);
      } else if (s_rec != d_rec && !(cut && s_rec))  // a cut may keep none of its records
        v.problem(ds.dir(), "segment records", "present in only one of source and copy");
      v.set_cut_segment(false);
    }
  }
  // Channel-level and session-level records.
  for (const auto& c : dch) {
    const std::string rel = c + ".timd/" + c + ".rdat";
    const bool s_rec = src.exists(rel), d_rec = dst.exists(rel);
    if (s_rec && d_rec) {
      const std::optional<si8> r = o.time_range ? level_rto(&c) : std::nullopt;
      v.check_records(rel, rel, excluded_keys, {}, o.time_range ? &r : nullptr);
    } else if (s_rec != d_rec) {
      v.problem(rel, "channel records", "present in only one of source and copy");
    }
  }
  {
    std::string amb_s, amb_d;
    const std::string s_rel = detail::session_record_file(src, amb_s);
    const std::string d_rel = detail::session_record_file(dst, amb_d);
    if (!s_rel.empty() && !d_rel.empty()) {
      if (d_rel != dname + ".rdat")
        v.problem(d_rel, "session records", "are not named after the copy's session");
      const std::optional<si8> r = o.time_range ? level_rto(nullptr) : std::nullopt;
      v.check_records(s_rel, d_rel, excluded_keys, {}, o.time_range ? &r : nullptr);
    } else if (s_rel.empty() != d_rel.empty() || !amb_d.empty()) {
      v.problem(dst_path, "session records", "present in only one of source and copy");
    }
  }
  rep.ok = rep.problems.empty();
  return rep;
}

// ---------------------------------------------------------------------------
// The pre-flight: every check made before anything is written. ONE function
// for anonymize_session (which throws the first refusal) and check_anonymize
// (which collects them all), so the two cannot drift apart.
// ---------------------------------------------------------------------------

/// The refusal code for an inspect warning that blocks a copy.
std::string blocker_code(const std::string& inspect_code) {
  static const std::map<std::string, std::string> codes = {
      {"video-channels", "video-channels"},
      {"unreadable-segments", "unreadable-segments"},
      {"incomplete-segments", "incomplete-segments"},
      {"unread-record-files", "unread-record-files"},
      {"unreadable-records", "unreadable-records"},
      {"locked", "parts-locked"},
      {"time-offset-disputed", "rto-disputed"},
      {"ambiguous-time-convention", "time-before-rto"},
      {"offset-assumed-zero", "rto-unreadable"},
      {"times-unresolved", "rto-unreadable"},
  };
  const auto it = codes.find(inspect_code);
  return it == codes.end() ? "inspect-blocker" : it->second;
}

struct Preflight {
  std::vector<CopyRefusal> refusals;
  std::vector<std::string> notes;
  fs::path dst, partial;
  std::shared_ptr<const SessionSource> source;
  std::optional<SourcePlan> plan;
  std::set<std::string> kept, excluded;
  std::string new_name, pseudonym;
  std::map<std::string, int> segment_numbers;  // by source .tmet path
  std::string session_records;                 // source-relative, or ""
  /// A cut: kept channels with no sample in the window. They are left out
  /// of the copy (and counted with the excluded ones from then on).
  std::vector<std::string> without_samples;
};

/// One piece of source text an anonymized copy keeps (after redaction).
struct KeptText {
  std::string value, where, field, channel;
};

/// The Redactor a copy applies, from the pre-flight's results. `rename`
/// false: the source's session name is kept on purpose, so it is not removed.
Redactor copy_redactor(const SourcePlan& plan, const AnonymizeOptions& an, const Preflight& pf) {
  SourcePlan p = plan;
  if (!an.rename) p.session_names.clear();
  return build_redactor(p, pf.new_name, an.clear_subject, an.clear_location, pf.excluded,
                        pf.kept, pf.pseudonym);
}

/// `collect`: false = throw the first refusal (anonymize_session /
/// transform_session), true = record every refusal and go on with the checks
/// that do not depend on a failed one (check_anonymize / check_transform).
/// Reads metadata only, never a `.tdat`. The anonymization checks (identity
/// in the new names) run only under anonymize's rules; the time checks (the
/// shift, the offset convention, times hidden in record ids) only when it
/// SHIFTS. A transform, and an anonymized copy that keeps the times, copy
/// every time as stored, so none of those applies to them.
/// `check_shift` false (redact_text): the shift's own size and sign are not
/// checked; the shifting rules still apply when a shift is given.
Preflight preflight(const std::string& src_path, const std::string& dst_path, const Rules& rules,
                    const Control& ctl, bool collect, bool check_shift) {
  const CopyOptions& o = *rules.copy;
  const AnonymizeOptions* an = rules.anon;
  const bool shifting = rules.shifting();
  Preflight pf;
  auto add = [&](RefusalKind kind, std::string code, std::string message,
                 std::string channel = {}, std::string field = {}) {
    CopyRefusal r{std::move(code), std::move(message), kind, std::move(channel),
                  std::move(field), {}};
    if (pf.plan) r.message = scrub_times(*pf.plan, r.message);  // never a source time
    if (!collect) raise(std::move(r));
    pf.refusals.push_back(std::move(r));
  };
  // An exception from code shared with other entry points (the metadata
  // loader, inspect, the source): its own refusal, or the code given here.
  auto add_exception = [&](std::exception_ptr e, const char* format_code, const char* crc_code,
                           const char* io_code, const std::string& channel = {}) {
    auto r = refusal_of(e, format_code, crc_code, io_code);
    if (!r) std::rethrow_exception(e);  // Cancelled, or a bug: not a refusal
    if (r->channel.empty()) r->channel = channel;
    if (pf.plan) r->message = scrub_times(*pf.plan, r->message);
    if (!collect) raise(std::move(*r));
    pf.refusals.push_back(std::move(*r));
  };
  ctl.check();

  // ---- options -----------------------------------------------------------
  if (shifting && check_shift && *an->shift_us == 0)
    add(RefusalKind::InvalidArgument, "shift-zero",
        "shift_us must not be 0 (leave the shift out to keep every time as it is)", {},
        "shift_us");
  const bool selection_conflict = !o.channels.empty() && !o.exclude_channels.empty();
  if (selection_conflict)
    add(RefusalKind::InvalidArgument, "channel-selection-conflict",
        "pass channels or exclude_channels, not both", {}, "channels");
  if (an && an->text == RecordTextPolicy::Callback && !an->text_fn)
    add(RefusalKind::InvalidArgument, "text-callback-missing",
        "the callback text policy needs a text function", {}, "text");
  if (o.output_password_1.empty() != o.output_password_2.empty())
    add(RefusalKind::InvalidArgument, "output-password-unpaired",
        "output passwords come in pairs: give both the level-1 and the level-2 password, or "
        "neither (MEF has no level-1-only files)",
        {}, "output_password");
  if (!o.output_password_1.empty() && o.output_password_1 == o.output_password_2)
    add(RefusalKind::InvalidArgument, "output-passwords-equal",
        "the level-1 and level-2 output passwords must differ", {}, "output_password");

  // ---- paths -------------------------------------------------------------
  const bool dst_named = path_has_suffix(dst_path, ".mefd");
  if (!dst_named)
    add(RefusalKind::Io, "dst-not-mefd", "the copy must be a directory ending in .mefd: " + dst_path,
        {}, "dst");
  fs::path dst(dst_path);
  if (dst.filename().empty()) dst = dst.parent_path();
  pf.dst = dst;
  pf.partial = dst.parent_path() / (dst.filename().string() + ".partial");
  if (fs::exists(dst))
    add(RefusalKind::Io, "dst-exists", "the destination already exists: " + dst.string(), {},
        "dst");
  if (fs::exists(pf.partial))
    add(RefusalKind::Io, "partial-exists",
        "a partial copy is in the way (an earlier run that did not finish, or one still "
        "running): " + pf.partial.string() + ". Remove it if no copy is running.",
        {}, "dst");
  if (!fs::exists(src_path)) {
    add(RefusalKind::Io, "source-missing", "the source does not exist: " + src_path, {}, "src");
    return pf;  // nothing below can be decided
  }
  if (const auto m = operation_marker(src_path))
    add(RefusalKind::WriteConflict, "source-mid-operation", detail::mid_operation_message(*m), {},
        "src");
  if (path_inside(dst, src_path) || path_inside(src_path, dst))
    add(RefusalKind::Io, "dst-inside-src",
        "the destination must not be inside the source: " + dst.string(), {}, "dst");

  // ---- the source --------------------------------------------------------
  try {
    pf.source = open_session_source(src_path);
  } catch (...) {
    add_exception(std::current_exception(), "source-damaged", "source-crc", "source-unreadable");
    return pf;
  }
  const SessionSource& src = *pf.source;
  {
    std::string ambiguity;
    try {
      pf.session_records = detail::session_record_file(src, ambiguity);
    } catch (...) {
      add_exception(std::current_exception(), "source-damaged", "source-crc",
                    "source-unreadable");
      return pf;
    }
    if (!ambiguity.empty()) {
      add(RefusalKind::Format, "records-ambiguous", ambiguity);
      return pf;  // inspect would only fail on it again
    }
  }
  // Inner calls get the cancel token but not the progress callback: their
  // own (0..total) reports would interleave with the caller's.
  Control inner;
  inner.cancel = ctl.cancel;
  try {
    pf.plan = plan_source(src, src_path, o.password, inner);
  } catch (...) {
    add_exception(std::current_exception(), "source-damaged", "source-crc", "source-unreadable");
    return pf;
  }
  SourcePlan& plan = *pf.plan;
  ctl.check();
  if (plan.summary.encryption.access != "full")
    add(RefusalKind::Password,
        plan.summary.encryption.access == "level-1" ? "password-level1-only" : "password-required",
        std::string("the session is encrypted and the password does not open all of it: metadata "
                    "section 3 holds the recording-time offset and the subject fields, so ") +
            (an ? "anonymizing" : "copying") + " needs the level-2 password",
        {}, "password");
  {
    // A transform (and an anonymized copy that keeps the times) copies every
    // time as stored, with the offset it is read against, so how the offset
    // convention reads them does not concern it; everything else (parts it
    // could not copy whole) does.
    static const std::set<std::string> time_only = {
        "time-offset-disputed", "ambiguous-time-convention", "offset-assumed-zero",
        "times-unresolved"};
    std::vector<CopyRefusal> blockers;
    for (const auto& w : plan.summary.warnings)
      if (w.blocks_anonymize && (shifting || !time_only.count(w.code)))
        blockers.push_back(CopyRefusal{blocker_code(w.code), scrub_times(plan, w.message),
                                       RefusalKind::WriteConflict, {}, {}, w.code});
    if (!blockers.empty() && !collect) {
      // One exception for all of them, as every release so far has thrown.
      CopyRefusal r = blockers.front();
      r.message = an ? "this session cannot be anonymized safely:"
                     : "this session cannot be copied whole:";
      for (const auto& b : blockers) r.message += "\n  - " + b.inspect_code + ": " + b.message;
      raise(std::move(r));
    }
    for (auto& b : blockers) pf.refusals.push_back(std::move(b));
  }
  if (shifting && plan.rto != 0)
    pf.notes.push_back("the source used meflib's recording-time offset; its times were read as "
                       "true UTC and the copy has recording_time_offset = 0");
  else if (an && plan.rto_status == "disputed")
    pf.notes.push_back("the source's segments disagree on meflib's recording-time offset; the "
                       "copy keeps each segment's own with every time as stored, so it reads the "
                       "same times (the report's source_recording_time_offset is 0: there is no "
                       "single one)");
  else if (an && plan.rto != 0)
    pf.notes.push_back("the source used meflib's recording-time offset; the copy keeps it with "
                       "every time as stored, so it reads the same times");

  // ---- channel selection -------------------------------------------------
  {
    std::set<std::string> known(plan.all_channels.begin(), plan.all_channels.end());
    for (const auto& c : o.channels)
      if (!known.count(c))
        add(RefusalKind::InvalidArgument, "channel-unknown", "unknown channel: " + c, c,
            "channels");
    for (const auto& c : o.exclude_channels)
      if (!known.count(c))
        add(RefusalKind::InvalidArgument, "channel-unknown", "unknown channel: " + c, c,
            "exclude_channels");
    for (const auto& c : plan.all_channels) {
      // With both lists given (refused above) the rest is checked as if every
      // channel were kept.
      bool in = true;
      if (!selection_conflict)
        in = o.channels.empty()
                 ? std::find(o.exclude_channels.begin(), o.exclude_channels.end(), c) ==
                       o.exclude_channels.end()
                 : std::find(o.channels.begin(), o.channels.end(), c) != o.channels.end();
      (in ? pf.kept : pf.excluded).insert(c);
    }
    if (pf.kept.empty())
      add(RefusalKind::InvalidArgument, "no-channels-left",
          "the selection leaves no channel to copy", {},
          o.channels.empty() ? "exclude_channels" : "channels");
  }

  // ---- a cut (#103) --------------------------------------------------------
  // Which kept channels have samples in the window, from each overlapping
  // segment's index around it (window_entries: a few small reads, never the
  // data). A channel without any is left out, so the copy never holds an
  // empty channel folder.
  if (o.time_range) {
    const auto [t0, t1] = *o.time_range;
    if (t0 >= t1) {
      add(RefusalKind::InvalidArgument, "time-range-invalid",
          "time_range must start before it ends", {}, "time_range");
    } else {
      bool any = false;
      for (const auto& c : plan.layout.channels) {
        if (!pf.kept.count(c.name)) continue;
        bool has = false;
        for (const auto& seg : c.segments) {
          if (has) break;
          ctl.check();
          try {
            const auto uh = src.read_range(seg.tmet, 0, UH);
            const si8 a = read<si8>(uh, UH_START), b = read<si8>(uh, UH_END);
            // A first look with the segment's own offset (a day's margin;
            // the session's may differ from it when segments disagree).
            const si8 r = segment_rto_of(plan, seg.tmet);
            if (!is_sentinel(a, false) && !is_sentinel(b, false) &&
                !(to_user_time(a, r) < saturating_add(t1, DAY_US) &&
                  to_user_time(b, r) > saturating_add(t0, -DAY_US)))
              continue;
            const TmetImage t = open_tmet(src, seg.tmet, o.password);
            const auto m = fmt::TimeSeriesMetadataSection2::parse(
                std::span<const ui1>(t.bytes).subspan(S2, S2_BYTES));
            const sf8 fs = m.sampling_frequency;
            if (!(fs > 0)) continue;
            const auto [f, l] = window_entries(src, seg.tidx, t.rto, t0, t1, longest_block_us(m));
            if (f >= l) continue;
            const auto es = src.read_range(seg.tidx, UH + f * ENTRY, (l - f) * ENTRY);
            for (std::size_t i = 0; i < l - f && !has; ++i) {
              const auto [i0, i1] =
                  window_slice(to_user_time(read<si8>(es, i * ENTRY + 8), t.rto),
                               read<ui4>(es, i * ENTRY + 24), fs, t0, t1);
              has = i0 < i1;
            }
          } catch (...) {
            add_exception(std::current_exception(), "source-damaged", "source-crc",
                          "source-unreadable", c.name);
          }
        }
        if (has) any = true;
        else pf.without_samples.push_back(c.name);
      }
      for (const auto& c : pf.without_samples) {
        pf.kept.erase(c);
        pf.excluded.insert(c);
        pf.notes.push_back("channel " + c + " has no samples in time_range; it is left out");
      }
      if (!any)
        add(RefusalKind::InvalidArgument, "time-range-empty",
            "no channel of the copy has samples inside time_range", {}, "time_range");
    }
  }

  // ---- metadata edits (a transform) -------------------------------------
  if (rules.edits && !rules.edits->empty()) {
    const MetadataEdits& m = *rules.edits;
    auto fits = [&](const std::string& v, std::size_t field_bytes, const char* name,
                    const std::string& channel = {}) {
      if (v.size() > field_bytes - 1)
        add(RefusalKind::InvalidArgument, "metadata-too-long",
            std::string(name) + " is " + std::to_string(v.size()) + " bytes; the field holds " +
                std::to_string(field_bytes - 1),
            channel, name);
      else if (v.find('\0') != std::string::npos)
        add(RefusalKind::InvalidArgument, "metadata-invalid",
            std::string(name) + " contains a NUL character", channel, name);
    };
    if (m.subject_name_1) fits(*m.subject_name_1, S3_SUBJECT_BYTES, "subject_name_1");
    if (m.subject_name_2) fits(*m.subject_name_2, S3_SUBJECT_BYTES, "subject_name_2");
    if (m.subject_id) fits(*m.subject_id, S3_SUBJECT_BYTES, "subject_id");
    if (m.recording_location) fits(*m.recording_location, S3_LOCATION_BYTES, "recording_location");
    if (m.session_description) fits(*m.session_description, S2_DESC_BYTES, "session_description");
    for (const auto* per : {&m.channel_descriptions, &m.reference_descriptions}) {
      const char* name =
          per == &m.channel_descriptions ? "channel_description" : "reference_description";
      for (const auto& [ch, v] : *per) {
        fits(v, S2_DESC_BYTES, name, ch);
        if (!pf.kept.count(ch))
          add(RefusalKind::InvalidArgument, "metadata-channel-unknown",
              std::string(name) + " is given for " + ch + ", which is not a channel of the copy",
              ch, name);
      }
    }
  }

  // ---- the shift ---------------------------------------------------------
  bool first_time_refused = false;
  if (shifting && check_shift && *an->shift_us != 0 && plan.has_times) {
    const si8 shift_us = *an->shift_us;
    const si8 span = saturating_add(plan.last, -plan.first);
    const si8 mag =
        shift_us == INT64_MIN ? INT64_MAX : (shift_us < 0 ? -shift_us : shift_us);
    const si8 floor = saturating_add(span, DAY_US);
    if (mag < floor)
      add(RefusalKind::InvalidArgument, "shift-below-floor",
          "|shift_us| (" + std::to_string(mag) + ") must be at least the session's time span "
          "plus one day (" + std::to_string(floor) + " us), so that no shifted time can fall "
          "inside the original range",
          {}, "shift_us");
    if (shift_us < 0 && plan.first + shift_us <= 0) {
      first_time_refused = true;
      add(RefusalKind::InvalidArgument, "shift-not-positive",
          "the shift moves the session's first time to 0 or below; shifted times must stay "
          "above 0",
          {}, "shift_us");
    }
  }

  // ---- names -------------------------------------------------------------
  if (dst_named) {
    pf.new_name = session_name_from_path(dst.string());
    if (!o.rename && pf.new_name != src.session_name())
      add(RefusalKind::InvalidArgument, "rename-name-mismatch",
          "rename=False keeps the source's session name, so the destination folder must be "
          "named after it",
          {}, "rename");
    pf.pseudonym = !an ? std::string() : an->pseudonym.empty() ? pf.new_name : an->pseudonym;
    // The copy's own names are written everywhere, so they must not carry
    // what the copy removes: a value of 3+ characters anywhere in them
    // ("Roe1", "XRoe"), a shorter one as a whole word. Subject and location
    // values are also checked word by word; everything in the scanner's
    // folded space (case and Latin diacritics).
    struct Value {
      std::string value;
      const char* what;
    };
    std::vector<Value> values;
    for (const auto& f : plan.summary.identity) {
      const char* what = nullptr;
      if (f.name == "subject_name_1" || f.name == "subject_name_2") what = "the patient's name";
      else if (f.name == "subject_id") what = "the patient's ID";
      else if (f.name == "recording_location") what = "the recording location";
      if (what)
        for (const auto& v : f.values)
          for (const auto& w : identity_variants(v, f.name != "subject_id"))
            values.push_back({w, what});
      if (f.name == "subject_id")
        for (const auto& v : f.values)
          for (const auto& d : id_digit_runs(normalized(v)))
            values.push_back({d, "the patient's ID"});
    }
    if (o.rename)
      for (const auto& n : plan.session_names) values.push_back({n, "the original session name"});
    auto check = [&](const std::string& name, const char* which, const char* code,
                     const char* field) {
      const std::string h = scan_folded(name);
      const auto tokens = alnum_tokens(h);
      for (const auto& v : values) {
        const std::string f = scan_folded(normalized(v.value));
        if (f.size() < 2) continue;
        const bool hit = f.size() >= 3 ? h.find(f) != std::string::npos
                                       : std::find(tokens.begin(), tokens.end(), f) != tokens.end();
        if (hit) {
          add(RefusalKind::InvalidArgument, code,
              std::string(which) + " contains " + v.what + "; choose another", {}, field);
          return;  // one refusal per name
        }
      }
    };
    if (an) {
      check(pf.new_name, "the new session name (the destination folder's name)",
            "name-contains-identity", "dst");
      if (pf.pseudonym != pf.new_name)
        check(pf.pseudonym, "the pseudonym", "pseudonym-contains-identity", "pseudonym");
    }
    if (pf.pseudonym.size() > UH_NAME_BYTES - 1 || pf.new_name.size() > UH_NAME_BYTES - 1)
      add(RefusalKind::InvalidArgument, "name-too-long",
          "the session name and pseudonym must fit 255 bytes", {},
          pf.new_name.size() > UH_NAME_BYTES - 1 ? "dst" : "pseudonym");
  }

  // ---- what the copy pass would meet, read from metadata -----------------
  // Every time the metadata and record files hold goes through the copy's
  // own Shifter, so a time the copy would refuse (at or before a non-zero
  // offset, moved to 0 or below, out of range) is refused here first. Only
  // the .tidx/.tdat headers and block times are left to the copy pass. After
  // the first such refusal the rest are skipped (one per cause is enough).
  // A transform, and a copy that keeps the times, write times as stored:
  // nothing to check.
  const Shifter sh{plan.rto, shifting && check_shift ? *an->shift_us : 0};
  bool time_refused = first_time_refused;  // that refusal covers the rest
  auto shift_time = [&](si8 v, const std::string& what, bool body, const std::string& channel) {
    if (!shifting || time_refused) return;
    try {
      (void)sh(v, what, body);
    } catch (...) {
      time_refused = true;
      add_exception(std::current_exception(), "time-overflow", "source-crc", "io-error", channel);
    }
  };
  // Segment numbers: named canonically, as the writer names them. The
  // number is the .tmet header's (@48); the folder name must agree. A folder
  // whose suffix is not a number, or names another segment, is refused:
  // which one is right is unknown. (1024 bytes per segment.)
  struct RecordFileRef {
    std::string rel, channel;
    bool segment_level = false;
  };
  std::vector<RecordFileRef> record_files;
  std::vector<KeptText> kept_texts;  // anonymize, text kept: checked at the end
  for (const auto& c : plan.layout.channels) {
    if (!pf.kept.count(c.name)) continue;
    std::set<int> numbers;
    for (const auto& seg : c.segments) {
      ctl.check();
      if (src.exists(seg.rdat())) record_files.push_back({seg.rdat(), c.name, true});
      const std::string folder = leaf(seg.dir());
      const std::string fstem = folder.substr(0, folder.size() - 5);
      const auto dash = fstem.rfind('-');
      const std::string suffix = dash == std::string::npos ? "" : fstem.substr(dash + 1);
      if (!all_digits(suffix) || suffix.size() > 9) {
        add(RefusalKind::WriteConflict, "segment-folder-unnumbered",
            "segment folder " + src.describe(seg.dir()) + " does not end in a segment number",
            c.name);
        continue;
      }
      const int folder_number = std::stoi(suffix);
      si4 uh_number = fmt::SI4_NO_ENTRY;
      try {
        const auto uh = src.read_range(seg.tmet, 0, UH);
        uh_number = read<si4>(uh, 48);
        shift_time(read<si8>(uh, UH_START), c.name + " metadata start time", false, c.name);
        shift_time(read<si8>(uh, UH_END), c.name + " metadata end time", false, c.name);
      } catch (...) {
        add_exception(std::current_exception(), "source-damaged", "source-crc",
                      "source-unreadable", c.name);
        continue;
      }
      if (uh_number != fmt::SI4_NO_ENTRY && uh_number != folder_number) {
        add(RefusalKind::WriteConflict, "segment-number-mismatch",
            "segment folder " + src.describe(seg.dir()) + " says segment " +
                std::to_string(folder_number) + " but its metadata says " +
                std::to_string(uh_number),
            c.name);
        continue;
      }
      if (!numbers.insert(folder_number).second) {
        add(RefusalKind::WriteConflict, "segment-number-duplicate",
            "two segment folders of channel " + c.name + " have the same segment number " +
                std::to_string(folder_number),
            c.name);
        continue;
      }
      pf.segment_numbers[seg.tmet] = folder_number;
    }
  }
  // ---- kept channel names (#143) ------------------------------------------
  // A kept channel's name stays verbatim: in its folder and file names, its
  // universal headers' channel_name and the Seiz entries naming it. The
  // copy's verification applies the channel-name rule (Scanner::channel) to
  // all of them; whatever it would refuse is refused HERE, before anything
  // is written, on the names the copy will produce (segments numbered as
  // above; a cut renumbers from 0, so 0..n-1 covers what it can keep). The
  // verifier still makes its own call on what the copy actually holds.
  if (an && dst_named) {
    const Scanner scan = build_scanner(plan, pf.new_name, an->clear_subject, an->clear_location,
                                       pf.excluded, pf.kept, pf.pseudonym);
    std::map<std::string, std::vector<int>> numbers;  // per kept data channel
    for (const auto& c : plan.layout.channels) {
      if (!pf.kept.count(c.name)) continue;
      auto& n = numbers[c.name];
      for (std::size_t i = 0; i < c.segments.size(); ++i) {
        if (o.time_range) {
          n.push_back(static_cast<int>(i));
        } else if (const auto it = pf.segment_numbers.find(c.segments[i].tmet);
                   it != pf.segment_numbers.end()) {
          n.push_back(it->second);
        }
      }
    }
    for (const auto& ch : pf.kept) {
      ctl.check();
      std::vector<std::string> names = {ch, ch + ".timd", ch + ".rdat", ch + ".ridx"};
      for (int number : numbers[ch]) {
        const std::string stem = canonical_segment_stem(ch, number);
        for (const char* ext : {".segd", ".tmet", ".tidx", ".tdat", ".rdat", ".ridx"})
          names.push_back(stem + ext);
      }
      for (const auto& name : names) {
        // The bare name: its channel_name fields and Seiz entries (verify's
        // name rule for a kept name); the rest: verify's path rule.
        const Scanner::Result r =
            name == ch ? scan.channel(name) : scan.path(name, pf.new_name, pf.kept);
        if (r.refuse.empty()) continue;
        add(RefusalKind::WriteConflict, "channel-name-identity",
            "kept channel " + ch + ": " +
                (name == ch ? std::string("its name, which the copy keeps,")
                            : "the folder or file name " + name + " the copy makes for it") +
                " contains " + r.refuse +
                "; leave the channel out (exclude_channels) for this copy",
            ch, "channels");
        break;  // one refusal per channel
      }
    }
  }
  for (const auto& ch : pf.kept) {
    const std::string crec = ch + ".timd/" + ch + ".rdat";
    if (src.exists(crec)) record_files.push_back({crec, ch, false});
  }
  if (!pf.session_records.empty()) record_files.push_back({pf.session_records, "", false});
  // Record files: read (and CRC-checked) as the copy pass reads them, each
  // index entry matched to its record by offset, and an Epoc/Curs id_number
  // holding a time inside the recording range refused: it is copied as is,
  // so the copy would carry the date. Small files; inspect reads them too.
  for (const auto& [rel, channel, segment_level] : record_files) {
    ctl.check();
    RawRecordFile f;
    try {
      f = read_record_file(src, rel, o.password);
    } catch (...) {
      add_exception(std::current_exception(), "record-damaged", "record-crc", "source-unreadable",
                    channel);
      continue;
    }
    const std::string where = src.describe(rel);
    // A cut decides per record whether it lies in the window, which needs
    // the offset the file's times are stored against: the segment's own, or
    // the one all segments of its channel (or session) agree on. Without
    // one, a time stored against it (negated) cannot be placed: refused, as
    // the Reader refuses to read such a file.
    if (o.time_range && !segment_level &&
        !level_rto_of(plan, channel.empty() ? nullptr : &channel))
      for (std::size_t i = 0; i < f.records.size(); ++i)
        if (record_offset_dependent(f.records[i])) {
          add(RefusalKind::WriteConflict, "time-range-offset-disputed",
              "record " + std::to_string(i) + " of " + where +
                  " stores its time against a recording-time offset " +
                  (plan.rto_status == "disputed"
                       ? std::string("its segments disagree on")
                       : std::string("that cannot be read")) +
                  ", so whether it lies in time_range is unknown; copy it without time_range",
              channel, "time_range");
          break;
        }
    for (const auto* uh : {&f.uh, &f.ridx_uh}) {
      if (uh->size() < UH) continue;  // no index file
      shift_time(read<si8>(*uh, UH_START), "record file " + where + " start time", false, channel);
      shift_time(read<si8>(*uh, UH_END), "record file " + where + " end time", false, channel);
    }
    for (const auto& e : f.ridx)
      shift_time(read<si8>(e, 16), "record index entry of " + where, false, channel);
    std::set<std::string> excluded_keys;
    for (const auto& c : pf.excluded) excluded_keys.insert(channel_key(c));
    for (std::size_t i = 0; i < f.records.size(); ++i) {
      const RawRecord& r = f.records[i];
      const std::string what = "record " + std::to_string(i) + " (" + r.type + ") of " + where;
      shift_time(r.time, what + " time", false, channel);
      if (an && an->text == RecordTextPolicy::Keep)
        record_text_fields(r, [&](const char* field, const std::string& v, bool seiz_name) {
          // A Seiz entry of a left-out channel goes; a kept channel's exact
          // name stays as it is (rewrite_body's rules).
          if (v.empty() || (seiz_name && (excluded_keys.count(channel_key(v)) || pf.kept.count(v))))
            return;
          kept_texts.push_back({v, what, std::string("record ") + field, channel});
        });
      if (const BodyLayout* lay = layout_of(r); lay && !lay->keep_whole &&
                                                 lay->variable_text_from == SIZE_MAX) {
        for (std::size_t off : lay->times)
          shift_time(read<si8>(r.body, off), what + " body time", true, channel);
        if (r.type == "Seiz") {  // the entries the copy keeps, as rewrite_body does
          const std::size_t n = seiz_entry_count(r.body);
          for (std::size_t c = 0; c < n; ++c) {
            const std::size_t e = SEIZ_FIXED + c * SEIZ_CHANNEL;
            if (excluded_keys.count(channel_key(read_string(r.body, e, SEIZ_CHANNEL_NAME))))
              continue;
            for (std::size_t off : {SEIZ_CH_ONSET, SEIZ_CH_OFFSET})
              shift_time(read<si8>(r.body, e + off), what + " channel time", true, channel);
          }
        }
      }
      if (f.has_ridx &&
          std::none_of(f.ridx.begin(), f.ridx.end(),
                       [&](const auto& x) { return read<si8>(x, 8) == f.record_offsets[i]; }))
        add(RefusalKind::Format, "record-index-mismatch",
            "record index of " + where + " has no entry for record " + std::to_string(i) +
                "; which time is right is unknown",
            channel);
      if (shifting && (r.type == "Epoc" || r.type == "Curs") && layout_of(r) &&
          in_absolute_window(plan, read<si8>(r.body, 0)))
        add(RefusalKind::WriteConflict, "id-number-time",
            "record " + std::to_string(i) + " (" + r.type + ") of " + where +
                ": its id_number holds a value inside the source's recording range (a recorder "
                "may store a time there); it is copied unchanged, so the copy would carry the "
                "date",
            channel, "id_number");
    }
  }

  // ---- kept text: the removal, then the check, as the copy will run them --
  // (#115) Text the copy keeps is redacted, and the copy's verification then
  // scans it. Whatever the scan would still refuse is refused HERE, before
  // anything is written: a value the removal leaves inside a longer word
  // ("a3a3" for a left-out channel "a3"). The message names the field, never
  // the text. A text callback's output cannot be known in advance; verify
  // still checks it.
  if (an && dst_named) {
    // The units the copy always rewrites, and (when kept) the descriptions:
    // the kept channels' own, never a left-out channel's.
    for (const auto& c : plan.summary.channels)
      if (pf.kept.count(c.name) && c.units_description && !c.units_description->empty())
        kept_texts.push_back({*c.units_description, c.name + " metadata", "units_description",
                              c.name});
    if (!an->clear_descriptions)
      for (const auto& c : plan.layout.channels) {
        if (!pf.kept.count(c.name)) continue;
        std::set<std::pair<std::string, std::string>> seen;  // (field, value)
        for (const auto& seg : c.segments) {
          ctl.check();
          try {
            const TmetImage t = open_tmet(src, seg.tmet, o.password);
            const auto s2 = std::span<const ui1>(t.bytes).subspan(S2, S2_BYTES);
            for (const auto& [off, n, field] :
                 {std::tuple<std::size_t, std::size_t, const char*>{S2_CHANNEL_DESC, S2_DESC_BYTES,
                                                                    "channel_description"},
                  {S2_SESSION_DESC, S2_DESC_BYTES, "session_description"},
                  {S2_REFERENCE_DESC, S2_DESC_BYTES, "reference_description"},
                  {S2_UNITS, S2_UNITS_BYTES, "units_description"}}) {
              const std::string v = read_string(s2, off, n);
              if (!v.empty() && seen.insert({field, v}).second)
                kept_texts.push_back({v, c.name + " metadata", field, c.name});
            }
          } catch (...) {
            add_exception(std::current_exception(), "source-damaged", "source-crc",
                          "source-unreadable", c.name);
          }
        }
      }
    if (!kept_texts.empty()) {
      const Redactor red = copy_redactor(plan, *an, pf);
      const Scanner scan = build_scanner(plan, pf.new_name, an->clear_subject,
                                         an->clear_location, pf.excluded, pf.kept, pf.pseudonym);
      std::set<std::pair<std::string, std::string>> reported;  // (where, field)
      for (const auto& t : kept_texts) {
        const Scanner::Result r = scan.text(red.apply(t.value));
        if (r.refuse.empty() || !reported.insert({t.where, t.field}).second) continue;
        add(RefusalKind::WriteConflict, "text-unremovable",
            t.where + ": its " + t.field + " holds " + r.refuse +
                " in a form the removal does not take out (inside a longer word or number); "
                "drop the text, or clear the descriptions, for this copy",
            t.channel, t.field);
      }
    }
  }
  return pf;
}

/// verify_anonymized / verify_transform: plan the source, then verify_impl
/// with its own progress over both sessions.
VerifyReport verify_standalone(const std::string& src, const std::string& dst,
                               std::optional<si8> shift_us,
                               const VerifyOptions& options, const MetadataEdits* edits,
                               const Control& ctl) {
  const auto source = open_session_source(src);
  Control inner;
  inner.cancel = ctl.cancel;
  const SourcePlan plan = plan_source(*source, src, options.password, inner);
  std::uint64_t total = std::max<std::uint64_t>(1, 2 * plan.summary.total_bytes);
  detail::ProgressReporter progress(ctl, total);
  progress.start();
  auto rep = verify_impl(src, *source, plan, dst, shift_us, options, edits, ctl,
                         progress.active() ? std::function<void(std::uint64_t)>(
                                                 [&](std::uint64_t n) { progress.add(n); })
                                           : std::function<void(std::uint64_t)>());
  ctl.check();
  progress.finish();
  return rep;
}

}  // namespace

// ---------------------------------------------------------------------------

VerifyReport verify_anonymized(const std::string& src, const std::string& dst,
                               std::optional<si8> shift_us, const VerifyOptions& options,
                               const Control& ctl) {
  return verify_standalone(src, dst, shift_us, options, nullptr, ctl);
}

const char* refusal_kind_name(RefusalKind kind) noexcept {
  switch (kind) {
    case RefusalKind::InvalidArgument: return "ValueError";
    case RefusalKind::Io: return "IoError";
    case RefusalKind::WriteConflict: return "WriteConflictError";
    case RefusalKind::Password: return "PasswordError";
    case RefusalKind::Format: return "FormatError";
    case RefusalKind::Crc: return "CrcError";
  }
  return "MefError";
}

namespace {

CopyCheck check_of(Preflight& pf) {
  CopyCheck c;
  c.ok = pf.refusals.empty();
  c.refusals = std::move(pf.refusals);
  c.notes = std::move(pf.notes);
  c.session_name = pf.new_name;
  if (pf.plan) {
    c.channels_kept.assign(pf.kept.begin(), pf.kept.end());
    c.channels_excluded.assign(pf.excluded.begin(), pf.excluded.end());
    c.channels_without_samples = pf.without_samples;
  }
  return c;
}

}  // namespace

AnonymizeCheck check_anonymize(const std::string& src, const std::string& dst,
                               const AnonymizeOptions& options, const Control& ctl) {
  Preflight pf = preflight(src, dst, Rules{&options, &options, nullptr}, ctl, /*collect=*/true,
                           /*check_shift=*/true);
  AnonymizeCheck c;
  c.pseudonym = pf.pseudonym;
  static_cast<CopyCheck&>(c) = check_of(pf);
  return c;
}

std::vector<std::string> redact_text(const std::string& src, const std::string& dst,
                                     const AnonymizeOptions& options,
                                     const std::vector<std::string>& texts, const Control& ctl) {
  Preflight pf = preflight(src, dst, Rules{&options, &options, nullptr}, ctl, /*collect=*/true,
                           /*check_shift=*/false);
  // What the copy itself would refuse, it refuses here too: a locked section
  // 3 leaves the subject names out of the rules, an unknown channel or a name
  // carrying the identity changes them. Only what does not decide the
  // redaction is let through: the destination being in use, and the text
  // the redaction cannot take out (which is what the caller asks about).
  static const std::set<std::string> irrelevant = {"dst-exists", "partial-exists",
                                                   "text-unremovable"};
  for (auto& r : pf.refusals)
    if (!irrelevant.count(r.code)) raise(std::move(r));
  if (!pf.plan || pf.new_name.empty())
    refuse(RefusalKind::InvalidArgument, "invalid-argument",
           "the redaction needs a readable source and a destination ending in .mefd");
  const Redactor red = copy_redactor(*pf.plan, options, pf);
  std::vector<std::string> out;
  out.reserve(texts.size());
  for (const auto& t : texts) out.push_back(red.apply(t));
  return out;
}

CopyCheck check_transform(const std::string& src, const std::string& dst,
                          const TransformOptions& options, const Control& ctl) {
  Preflight pf = preflight(src, dst, Rules{&options, nullptr, &options.metadata}, ctl,
                           /*collect=*/true, /*check_shift=*/false);
  return check_of(pf);
}

namespace {

OutputKeys output_keys(const CopyOptions& o) {
  OutputKeys out;
  if (!o.output_password_1.empty()) {
    out.encrypt = true;
    out.vf = crypto::make_validation_fields(o.output_password_1, o.output_password_2);
    out.keys = crypto::validate_password(o.output_password_2, out.vf.level1, out.vf.level2);
    if (!out.keys.level1_key || !out.keys.level2_key)
      throw std::logic_error("output keys could not be derived");
  }
  return out;
}

/// The copy: pre-flight (throws the first refusal), the copy pass into
/// `<dst>.partial/<name>`, the flush, the verification, the publish. ONE
/// driver for anonymize_session and transform_session; `rules` decides what
/// changes, `arep` / `trep` receive what only that operation reports.
void run_copy(const std::string& src_path, const std::string& dst_path, const Rules& rules,
              const Control& ctl, CopyReport& rep, AnonymizeReport* arep,
              TransformReport* trep) {
  const CopyOptions& o = *rules.copy;
  const AnonymizeOptions* an = rules.anon;
  // Every refusal that can be decided before writing, thrown at the first.
  Preflight pf = preflight(src_path, dst_path, rules, ctl, /*collect=*/false, /*check_shift=*/true);
  const fs::path& dst = pf.dst;
  const fs::path& partial = pf.partial;
  const SessionSource& src = *pf.source;
  SourcePlan& plan = *pf.plan;
  const std::set<std::string>& kept = pf.kept;
  const std::set<std::string>& excluded = pf.excluded;
  const std::string& new_name = pf.new_name;
  const std::string& pseudonym = pf.pseudonym;

  Shifter sh{plan.rto, rules.shifting() ? *an->shift_us : 0};
  std::optional<Redactor> red;
  if (an) {
    red = copy_redactor(plan, *an, pf);
    if (!o.rename) plan.session_names.clear();  // the name is kept on purpose (verify, too)
  }
  const OutputKeys out = output_keys(o);

  rep.destination = dst.string();
  rep.session_name = new_name;
  rep.channels_kept.assign(kept.begin(), kept.end());
  rep.channels_excluded.assign(excluded.begin(), excluded.end());
  rep.channels_without_samples = pf.without_samples;
  rep.output_encrypted = out.encrypt;
  rep.notes = pf.notes;
  if (arep) {
    arep->pseudonym = pseudonym;
    arep->shift_us = an->shift_us;
    arep->source_recording_time_offset = plan.rto;
    // The re-identification key: returned to the caller, never written.
    arep->source_start_us = plan.summary.start_time;
    arep->source_end_us = plan.summary.end_time;
    for (const auto& ic : plan.summary.channels)
      if (kept.count(ic.name))
        arep->source_channel_times.push_back({ic.name, ic.start_time, ic.end_time});
    for (const auto& ch : kept)  // record-only channels: no samples, no times
      if (std::none_of(arep->source_channel_times.begin(), arep->source_channel_times.end(),
                       [&](const SourceChannelTimes& t) { return t.channel == ch; }))
        arep->source_channel_times.push_back({ch, std::nullopt, std::nullopt});
    std::sort(arep->source_channel_times.begin(), arep->source_channel_times.end(),
              [](const SourceChannelTimes& x, const SourceChannelTimes& y) {
                return x.channel < y.channel;
              });
  }

  const std::uint64_t total = std::max<std::uint64_t>(1, 3 * plan.summary.total_bytes);
  detail::ProgressReporter progress(ctl, total);
  std::function<void(std::uint64_t)> on_bytes;
  if (progress.active()) on_bytes = [&](std::uint64_t n) { progress.add(n); };

  const fs::path root = partial / dst.filename();
  try {
    progress.start();
    fs::create_directories(root);
    Copier cp(src, rules, sh, red ? &*red : nullptr, new_name, pseudonym, excluded, kept, out,
              ctl, on_bytes, rep, arep, trep);
    cp.set_level_rto([&plan](const std::string& ch) {
      return level_rto_of(plan, ch.empty() ? nullptr : &ch);
    });

    std::set<std::string> handled;  // source-relative paths copied
    std::map<std::string, const detail::LayoutChannel*> data;
    for (const auto& c : plan.layout.channels) data[c.name] = &c;
    for (const auto& ch : kept) {
      const fs::path cdir = root / (ch + ".timd");
      fs::create_directory(cdir);
      const std::string crec = ch + ".timd/" + ch + ".rdat";
      if (src.exists(crec)) {
        cp.copy_records(crec, cdir / (ch + ".rdat"), RecordLevel::Channel, ch, -1);
        handled.insert(crec);
        handled.insert(ridx_of(crec));
      }
      const auto it = data.find(ch);
      if (it == data.end()) continue;  // record-only channel
      int next_number = 0;  // a cut renumbers the segments it keeps from 0
      si8 next_start = 0;
      for (const auto& seg : it->second->segments) {
        // Numbered by the pre-flight (folder name and .tmet header agree).
        const int number = o.time_range ? next_number : pf.segment_numbers.at(seg.tmet);
        handled.insert(seg.tmet);
        handled.insert(seg.tidx);
        handled.insert(seg.tdat);
        if (src.exists(seg.rdat())) {
          handled.insert(seg.rdat());
          handled.insert(ridx_of(seg.rdat()));
        }
        if (!cp.copy_segment(seg, cdir, ch, number, next_start)) continue;  // outside the cut
        const std::string stem = canonical_segment_stem(ch, number);
        if (src.exists(seg.rdat()))
          cp.copy_records(seg.rdat(), cdir / (stem + ".segd") / (stem + ".rdat"),
                          RecordLevel::Segment, ch, number);
        cp.end_segment();
        ++next_number;
        ++rep.segments;
      }
    }
    {
      const std::string& srec = pf.session_records;
      if (!srec.empty()) {
        cp.copy_records(srec, root / (new_name + ".rdat"), RecordLevel::Session, "", -1);
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
    for (const auto& f : cp.written) detail::fsync_file_or_throw(f);

    VerifyOptions vo;
    vo.password = o.password;
    vo.output_password = o.output_password_2;
    vo.time_range = o.time_range;
    vo.exact = o.exact;
    if (an) {
      vo.clear_subject = an->clear_subject;
      vo.clear_location = an->clear_location;
      vo.clear_descriptions = an->clear_descriptions;
    }
    rep.verification = verify_impl(src_path, src, plan, root.string(),
                                   an ? an->shift_us : std::optional<si8>(0), vo,
                                   an ? nullptr : rules.edits, ctl, on_bytes,
                                   &pf.without_samples);
    // verify alone cannot tell a dropped channel from an excluded one; here
    // the selection is known.
    if (rep.verification.channels != rep.channels_kept) {
      rep.verification.problems.push_back(
          {root.string(), "channels", "the copy does not hold exactly the kept channels"});
      rep.verification.ok = false;
    }
    if (!rep.verification.ok) {
      std::string msg = "the copy failed verification, so it was not published (" +
                        std::to_string(rep.verification.problems.size()) + " problem(s)):";
      const std::size_t show = std::min<std::size_t>(rep.verification.problems.size(), 10);
      for (std::size_t i = 0; i < show; ++i) {
        const auto& p = rep.verification.problems[i];
        msg += "\n  - " + p.location + ": " + p.field + " " + p.message;
      }
      refuse(RefusalKind::WriteConflict, "verify-failed", msg);
    }
    ctl.check();  // the last point a cancel can stop it
    progress.finish();
    if (fs::exists(dst))
      refuse(RefusalKind::Io, "dst-exists",
             "the destination appeared while copying: " + dst.string(), {}, "dst");
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
}

/// Whatever shared code threw without a code gets the generic one of its
/// type; Cancelled and internal errors pass through unchanged.
template <class F>
auto coded(F&& f) -> decltype(f()) {
  try {
    return f();
  } catch (const CopyError&) {
    throw;
  } catch (...) {
    auto r = refusal_of(std::current_exception(), "source-damaged", "source-crc", "io-error");
    if (!r) throw;
    raise(std::move(*r));
  }
}

}  // namespace

AnonymizeReport anonymize_session(const std::string& src_path, const std::string& dst_path,
                                  const AnonymizeOptions& o, const Control& ctl) {
  return coded([&] {
    AnonymizeReport rep;
    run_copy(src_path, dst_path, Rules{&o, &o, nullptr}, ctl, rep, &rep, nullptr);
    return rep;
  });
}

TransformReport transform_session(const std::string& src_path, const std::string& dst_path,
                                  const TransformOptions& o, const Control& ctl) {
  return coded([&] {
    TransformReport rep;
    run_copy(src_path, dst_path, Rules{&o, nullptr, &o.metadata}, ctl, rep, nullptr, &rep);
    return rep;
  });
}

VerifyReport verify_transform(const std::string& src, const std::string& dst,
                              const TransformOptions& options, const Control& ctl) {
  VerifyOptions vo;
  vo.password = options.password;
  vo.output_password = options.output_password_2;
  vo.time_range = options.time_range;
  vo.exact = options.exact;
  return verify_standalone(src, dst, 0, vo, &options.metadata, ctl);
}

// ---------------------------------------------------------------------------
// Merge (#103): several sessions into one, on the same engine. Each source is
// copied with its own Copier under the transform's rules; the merge decides
// which piece of which source segment becomes which segment of the result.
// ---------------------------------------------------------------------------

namespace {

/// One segment of the result: a run of a source segment's blocks.
struct MergePiece {
  std::size_t source = 0;
  const detail::LayoutSegment* segment = nullptr;
  std::pair<std::size_t, std::size_t> entries;  ///< [first, last) of its index
  si8 start = 0;   ///< its first block's time (index, user uUTC)
  si8 lo = 0, hi = 0;  ///< the record times its segment-level annotations take
};

struct MergeSourcePlan {
  std::string path;
  TransformOptions options;  ///< this source's password and channel selection
  Preflight pf;
  bool used = true;          ///< holds a channel of the result
};

struct MergePlan {
  std::vector<std::unique_ptr<MergeSourcePlan>> sources;  // stable addresses (Rules point in)
  std::vector<CopyRefusal> refusals;
  std::vector<std::string> notes;
  std::set<std::string> kept, excluded;  // over all sources
  std::map<std::string, std::vector<MergePiece>> pieces;  // per kept data channel, by time
  fs::path dst, partial;
  std::string new_name;
  si8 segments_split = 0;
};

/// A segment's blocks as [start, end) (user uUTC) from its INDEX copy of
/// each block's time and count (the header is the authority, #11/#13; a
/// disagreement shows as a refused verification, never a silent result).
struct IndexBlock {
  si8 start = 0, end = 0;
};
std::vector<IndexBlock> index_blocks(const SessionSource& src, const detail::LayoutSegment& seg,
                                     si8 rto, sf8 fs) {
  std::vector<IndexBlock> out;
  const std::uint64_t size = src.file_size(seg.tidx);
  if (size < UH) return out;
  const std::size_t n = static_cast<std::size_t>((size - UH) / ENTRY);
  const auto es = src.read_range(seg.tidx, UH, n * ENTRY);
  out.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    const si8 t = to_user_time(read<si8>(es, i * ENTRY + 8), rto);
    out.push_back({t, sample_time(t, read<ui4>(es, i * ENTRY + 24), fs)});
  }
  return out;
}

MergePlan preflight_merge(const std::vector<std::string>& sources, const std::string& dst_path,
                          const MergeOptions& o, const Control& ctl, bool collect) {
  MergePlan m;
  auto add = [&](RefusalKind kind, std::string code, std::string message, std::string channel = {},
                 std::string field = {}) {
    CopyRefusal r{std::move(code), std::move(message), kind, std::move(channel), std::move(field),
                  {}};
    if (!collect) raise(std::move(r));
    m.refusals.push_back(std::move(r));
  };
  ctl.check();
  if (sources.size() < 2)
    add(RefusalKind::InvalidArgument, "merge-too-few", "a merge needs at least two sources", {},
        "sources");
  if (!o.passwords.empty() && o.passwords.size() != sources.size())
    add(RefusalKind::InvalidArgument, "merge-passwords-count",
        "passwords must hold one password per source (or be empty)", {}, "passwords");
  if (o.time_range)
    add(RefusalKind::InvalidArgument, "merge-time-range",
        "a merge cannot be combined with time_range yet: cut the result afterwards", {},
        "time_range");
  if (!o.rename)
    add(RefusalKind::InvalidArgument, "merge-rename",
        "a merge is named after the destination; rename=False does not apply", {}, "rename");

  // Each source's channels (a directory listing), to give each its share of
  // the selection: a channel only some sources hold is not "unknown" to the
  // others.
  std::set<std::string> all_channels;
  std::vector<std::set<std::string>> holds(sources.size());
  for (std::size_t k = 0; k < sources.size(); ++k) {
    try {
      const auto src = open_session_source(sources[k]);
      const auto layout = detail::discover_layout(*src);
      for (const auto& c : layout.channels) holds[k].insert(c.name);
      for (const auto& c : layout.record_only_channels) holds[k].insert(c);
    } catch (...) {
      // the source's own pre-flight below reports it
    }
    all_channels.insert(holds[k].begin(), holds[k].end());
  }
  for (const auto* list : {&o.channels, &o.exclude_channels})
    for (const auto& c : *list)
      if (!all_channels.count(c))
        add(RefusalKind::InvalidArgument, "channel-unknown", "unknown channel: " + c, c,
            list == &o.channels ? "channels" : "exclude_channels");

  for (std::size_t k = 0; k < sources.size(); ++k) {
    auto sp = std::make_unique<MergeSourcePlan>();
    sp->path = sources[k];
    TransformOptions& so = sp->options;
    static_cast<CopyOptions&>(so) = o;
    // (A count mismatch is refused above; never index past the list.)
    so.password = k < o.passwords.size() ? o.passwords[k] : o.password;
    so.rename = true;
    so.time_range.reset();
    auto mine = [&](const std::vector<std::string>& v) {
      std::vector<std::string> out;
      for (const auto& c : v)
        if (holds[k].count(c)) out.push_back(c);
      return out;
    };
    so.channels = mine(o.channels);
    so.exclude_channels = mine(o.exclude_channels);
    if (!o.channels.empty() && so.channels.empty()) sp->used = false;
    so.metadata = o.metadata;
    for (auto* per : {&so.metadata.channel_descriptions, &so.metadata.reference_descriptions})
      for (auto it = per->begin(); it != per->end();)
        it = holds[k].count(it->first) ? std::next(it) : per->erase(it);
    if (sp->used) {
      sp->pf = preflight(sources[k], dst_path, Rules{&so, nullptr, &so.metadata}, ctl,
                         /*collect=*/true, /*check_shift=*/false);
      for (auto& r : sp->pf.refusals) {
        // The destination's refusals come once, not once per source.
        const bool dup = std::any_of(m.refusals.begin(), m.refusals.end(), [&](const CopyRefusal& x) {
          return x.code == r.code && x.field == r.field && x.channel == r.channel &&
                 x.message == r.message;
        });
        if (dup) continue;
        if (r.channel.empty() && r.field != "dst")
          r.message = sources[k] + ": " + r.message;  // which source
        if (!collect) raise(std::move(r));
        m.refusals.push_back(std::move(r));
      }
      for (const auto& n : sp->pf.notes) m.notes.push_back(sources[k] + ": " + n);
      if (sp->pf.plan) {
        m.kept.insert(sp->pf.kept.begin(), sp->pf.kept.end());
        m.excluded.insert(sp->pf.excluded.begin(), sp->pf.excluded.end());
      }
      m.dst = sp->pf.dst;
      m.partial = sp->pf.partial;
      m.new_name = sp->pf.new_name;
    }
    m.sources.push_back(std::move(sp));
  }
  for (const auto& c : m.kept) m.excluded.erase(c);  // kept by any source = kept
  // Everything below needs every used source readable.
  for (const auto& sp : m.sources)
    if (sp->used && !sp->pf.plan) return m;

  // ---- the sources must agree where their data meet ----------------------
  // Recording-time offset: times are copied verbatim.
  std::set<si8> rtos;
  for (const auto& sp : m.sources)
    if (sp->used) rtos.insert(sp->pf.plan->rto);
  if (rtos.size() > 1)
    add(RefusalKind::WriteConflict, "merge-rto-differs",
        "the sources use different recording-time offsets, so their times cannot be copied "
        "into one session as they are",
        {}, "sources");
  // One sampling rate per channel.
  std::map<std::string, std::set<sf8>> rates;
  for (const auto& sp : m.sources)
    if (sp->used)
      for (const auto& c : sp->pf.plan->summary.channels)
        if (m.kept.count(c.name) && c.sampling_frequency) rates[c.name].insert(*c.sampling_frequency);
  for (const auto& [ch, r] : rates)
    if (r.size() > 1)
      add(RefusalKind::WriteConflict, "merge-rate-differs",
          "channel " + ch + " has different sampling rates in different sources", ch,
          "sampling_frequency");
  // The patient: the same in every source, or set for the result.
  const MetadataEdits& me = o.metadata;
  for (const auto& [field, edit] :
       {std::pair<const char*, bool>{"subject_name_1", me.subject_name_1.has_value()},
        {"subject_name_2", me.subject_name_2.has_value()},
        {"subject_id", me.subject_id.has_value()},
        {"recording_location", me.recording_location.has_value()}}) {
    std::set<std::string> values;
    for (const auto& sp : m.sources)
      if (sp->used)
        for (const auto& f : sp->pf.plan->summary.identity)
          if (f.name == field)
            for (const auto& v : f.values)
              if (!v.empty()) values.insert(v);
    if (values.size() > 1 && !edit)
      add(RefusalKind::WriteConflict, "merge-identity-differs",
          std::string("the sources hold different values of ") + field +
              " (different patients?); set it for the result with metadata, or check the "
              "sources",
          {}, field);
  }

  // ---- overlap, on the recorded data, and the pieces ----------------------
  for (const auto& ch : m.kept) {
    struct Seg {
      std::size_t source;
      const detail::LayoutSegment* seg;
      si8 rto = 0;
      sf8 fs = 0;
      si8 start = 0, end = 0;  // UH span
      std::vector<IndexBlock> blocks;
      bool read = false;
    };
    std::vector<Seg> segs;
    for (std::size_t k = 0; k < m.sources.size(); ++k) {
      const auto& sp = *m.sources[k];
      if (!sp.used || !sp.pf.kept.count(ch)) continue;
      const SessionSource& src = *sp.pf.source;
      for (const auto& c : sp.pf.plan->layout.channels) {
        if (c.name != ch) continue;
        for (const auto& seg : c.segments) {
          ctl.check();
          try {
            const TmetImage t = open_tmet(src, seg.tmet, sp.options.password);
            const sf8 fs = fmt::TimeSeriesMetadataSection2::parse(
                               std::span<const ui1>(t.bytes).subspan(S2, S2_BYTES))
                               .sampling_frequency;
            // The span from the header, widened to the index's first and
            // last block: a foreign writer's header time may be unset or
            // stale, and two segments are compared only where spans meet.
            si8 a = std::numeric_limits<si8>::max(), b = std::numeric_limits<si8>::min();
            const si8 ua = read<si8>(t.bytes, UH_START), ub = read<si8>(t.bytes, UH_END);
            if (!is_sentinel(ua, false)) a = to_user_time(ua, t.rto);
            if (!is_sentinel(ub, false)) b = to_user_time(ub, t.rto);
            const std::uint64_t isize = src.file_size(seg.tidx);
            if (isize >= UH + ENTRY) {
              const std::size_t n = static_cast<std::size_t>((isize - UH) / ENTRY);
              const auto e0 = src.read_range(seg.tidx, UH, ENTRY);
              const auto el = src.read_range(seg.tidx, UH + (n - 1) * ENTRY, ENTRY);
              a = std::min(a, to_user_time(read<si8>(e0, 8), t.rto));
              b = std::max(b, sample_time(to_user_time(read<si8>(el, 8), t.rto),
                                          read<ui4>(el, 24), fs));
            }
            segs.push_back({k, &seg, t.rto, fs, a, b, {}, false});
          } catch (...) {
            // the source's own pre-flight already reported it
          }
        }
      }
    }
    std::sort(segs.begin(), segs.end(), [](const Seg& a, const Seg& b) { return a.start < b.start; });
    auto blocks_of = [&](Seg& sg) -> const std::vector<IndexBlock>& {
      if (!sg.read) {
        sg.blocks = index_blocks(*m.sources[sg.source]->pf.source, *sg.seg, sg.rto, sg.fs);
        sg.read = true;
      }
      return sg.blocks;
    };
    // Segments of different sources whose spans overlap: compare their data.
    bool overlap = false;
    std::vector<std::vector<IndexBlock>> foreign(segs.size());  // other sources' data in each span
    for (std::size_t i = 0; i < segs.size() && !overlap; ++i)
      for (std::size_t j = i + 1; j < segs.size() && segs[j].start < segs[i].end && !overlap; ++j) {
        if (segs[i].source == segs[j].source) continue;
        const auto& a = blocks_of(segs[i]);
        const auto& b = blocks_of(segs[j]);
        const si8 tol = static_cast<si8>(std::llround(0.5e6 / std::max(segs[i].fs, 1e-9)));
        std::size_t p = 0;
        for (const auto& x : a) {
          while (p < b.size() && b[p].end <= x.start + tol) ++p;
          for (std::size_t q = p; q < b.size() && b[q].start < x.end - tol; ++q) {
            overlap = true;
            add(RefusalKind::WriteConflict, "merge-overlap",
                "channel " + ch + ": " + m.sources[segs[i].source]->path + " and " +
                    m.sources[segs[j].source]->path + " both hold samples from " +
                    std::to_string(std::max(x.start, b[q].start)) + " to " +
                    std::to_string(std::min(x.end, b[q].end)) + " (uUTC)",
                ch, "sources");
            break;
          }
          if (overlap) break;
        }
        foreign[i].insert(foreign[i].end(), b.begin(), b.end());
        foreign[j].insert(foreign[j].end(), a.begin(), a.end());
      }
    if (overlap) continue;
    // Each segment: split where another source's data lies between two of
    // its blocks, so that the result's segments never overlap in time.
    std::vector<MergePiece> pieces;
    for (std::size_t i = 0; i < segs.size(); ++i) {
      Seg& sg = segs[i];
      const std::size_t n = static_cast<std::size_t>(
          (m.sources[sg.source]->pf.source->file_size(sg.seg->tidx) - UH) / ENTRY);
      if (n == 0) {  // a segment without blocks has nothing to merge
        m.notes.push_back(m.sources[sg.source]->path + ": " + sg.seg->dir() +
                          " holds no blocks; it is left out");
        continue;
      }
      if (foreign[i].empty()) {
        pieces.push_back({sg.source, sg.seg, {0, n}, sg.start, 0, 0});
        continue;
      }
      std::vector<si8> starts;
      for (const auto& b : foreign[i]) starts.push_back(b.start);
      std::sort(starts.begin(), starts.end());
      const auto& bl = blocks_of(sg);
      // The same half-sample tolerance as the overlap check: foreign data
      // starting up to half a sample before a block's end lies in the gap
      // after it (recorders round times to whole microseconds).
      const si8 tol = static_cast<si8>(std::llround(0.5e6 / std::max(sg.fs, 1e-9)));
      std::size_t first = 0, cut = 0;
      for (std::size_t b = 1; b < bl.size(); ++b) {
        while (cut < starts.size() && starts[cut] < bl[b - 1].end - tol) ++cut;
        if (cut < starts.size() && starts[cut] < bl[b].start) {  // foreign data in this gap
          pieces.push_back({sg.source, sg.seg, {first, b}, bl[first].start, 0, 0});
          first = b;
        }
      }
      pieces.push_back({sg.source, sg.seg, {first, bl.size()}, bl.empty() ? sg.start : bl[first].start, 0, 0});
      if (first > 0) ++m.segments_split;
    }
    std::stable_sort(pieces.begin(), pieces.end(),
                     [](const MergePiece& a, const MergePiece& b) { return a.start < b.start; });
    // A split segment's own annotations go to the piece their time falls in.
    for (std::size_t i = 0; i < pieces.size(); ++i) {
      auto& pc = pieces[i];
      pc.lo = std::numeric_limits<si8>::min();
      pc.hi = std::numeric_limits<si8>::max();
      for (const auto& other : pieces) {
        if (other.segment != pc.segment || &other == &pc) continue;
        if (other.start < pc.start) pc.lo = std::max(pc.lo, pc.start);
        if (other.start > pc.start) pc.hi = std::min(pc.hi, other.start);
      }
    }
    m.pieces[ch] = std::move(pieces);
  }
  return m;
}

/// The merged result against its sources: every segment against its piece
/// (lockstep, blocks byte-identical), annotations at every level against the
/// sources' merged by time, no two segments of a channel overlapping, and the
/// validator.
VerifyReport verify_merge_impl(MergePlan& m, const std::string& dst_path, const Control& ctl,
                               std::function<void(std::uint64_t)> progress) {
  VerifyReport rep;
  const auto dsrc = open_session_source(dst_path);
  const SessionSource& dst = *dsrc;
  const auto dlayout = detail::discover_layout(dst);
  std::set<std::string> dch;
  for (const auto& c : dlayout.channels) dch.insert(c.name);
  for (const auto& c : dlayout.record_only_channels) dch.insert(c);
  rep.channels.assign(dch.begin(), dch.end());
  rep.excluded_channels.assign(m.excluded.begin(), m.excluded.end());
  auto problem = [&](const std::string& loc, const std::string& field, const std::string& msg) {
    rep.problems.push_back({loc, field, msg});
  };
  if (dch != m.kept) problem(dst_path, "channels", "the result does not hold exactly the kept channels");

  // One Verifier per source (its plan, password and options).
  std::vector<std::unique_ptr<Verifier>> v;
  std::vector<VerifyOptions> vos(m.sources.size());
  std::vector<VerifyReport> parts(m.sources.size());
  for (std::size_t k = 0; k < m.sources.size(); ++k) {
    auto& sp = *m.sources[k];
    vos[k].password = sp.options.password;
    vos[k].output_password = sp.options.output_password_2;
    if (!sp.used) {
      v.push_back(nullptr);
      continue;
    }
    v.push_back(std::make_unique<Verifier>(*sp.pf.source, dst, *sp.pf.plan, 0, vos[k],
                                           &sp.options.metadata, ctl, progress, parts[k]));
    v.back()->set_kept(dch);
    v.back()->set_merge(true);
  }

  std::set<std::string> excluded_keys;
  for (const auto& c : m.excluded) excluded_keys.insert(channel_key(c));
  for (const auto& dc : dlayout.channels) {
    const auto it = m.pieces.find(dc.name);
    if (it == m.pieces.end()) {
      problem(dc.name, "channel", "has segments no source piece accounts for");
      continue;
    }
    const auto& pieces = it->second;
    if (dc.segments.size() != pieces.size()) {
      problem(dc.name, "segments", "the result holds " + std::to_string(dc.segments.size()) +
                                       ", the merge planned " + std::to_string(pieces.size()));
      continue;
    }
    std::optional<si8> prev_end;  // the previous segment's end, once there is one
    for (std::size_t k = 0; k < pieces.size(); ++k) {
      const auto& pc = pieces[k];
      const auto& ds = dc.segments[k];
      if (ds.number != static_cast<int>(k))
        problem(ds.dir(), "segment", "is not numbered in order from 0");
      Verifier& vv = *v[pc.source];
      vv.set_cut_segment(true);
      vv.check_tmet(pc.segment->tmet, ds.tmet);
      vv.check_segment_data_cut(*pc.segment, ds, pc.entries);
      const auto& src = *m.sources[pc.source]->pf.source;
      const bool s_rec = src.exists(pc.segment->rdat()), d_rec = dst.exists(ds.rdat());
      if (s_rec && d_rec)
        vv.check_records(pc.segment->rdat(), ds.rdat(), excluded_keys,
                         [&](si8 t) { return t >= pc.lo && t < pc.hi; });
      else if (!s_rec && d_rec)
        problem(ds.dir(), "segment records", "present in the result, not in its source");
      vv.set_cut_segment(false);
      // Segments in time order, never overlapping.
      const auto uh = dst.read_range(ds.tmet, 0, UH);
      const si8 rto = m.sources[pc.source]->pf.plan->rto;
      const si8 a = to_user_time(read<si8>(uh, UH_START), rto), b = to_user_time(read<si8>(uh, UH_END), rto);
      // The overlap check's half-sample tolerance (recorder rounding).
      const si8 tol = static_cast<si8>(std::llround(0.5e6 / std::max(vv.segment_fs(), 1e-9)));
      if (prev_end && a < *prev_end - tol)
        problem(ds.dir(), "segment", "overlaps the segment before it in time");
      prev_end = std::max(prev_end.value_or(b), b);
      ++rep.segments;
    }
  }

  // The plan itself, checked rather than trusted: the pieces of each source
  // segment tile its blocks exactly once.
  {
    std::map<const detail::LayoutSegment*, std::vector<std::pair<std::size_t, std::size_t>>> tiles;
    std::map<const detail::LayoutSegment*, std::size_t> owner;
    for (const auto& [ch, pieces] : m.pieces)
      for (const auto& pc : pieces) {
        tiles[pc.segment].push_back(pc.entries);
        owner[pc.segment] = pc.source;
      }
    for (auto& [seg, t] : tiles) {
      std::sort(t.begin(), t.end());
      const std::size_t n = static_cast<std::size_t>(
          (m.sources[owner[seg]]->pf.source->file_size(seg->tidx) - UH) / ENTRY);
      std::size_t next = 0;
      for (const auto& [a, b] : t) {
        if (a != next || b <= a) problem(seg->dir(), "merge plan", "its pieces do not tile its blocks");
        next = b;
      }
      if (next != n) problem(seg->dir(), "merge plan", "its pieces do not cover all its blocks");
    }
  }
  // Level UUIDs: one per segment of the result (all its files), one per
  // channel, one for the session, all different.
  {
    std::set<Uuid> seen;
    auto level_of = [&](const std::string& rel) {
      return uuid_at(dst.read_range(rel, 0, UH), UH_LEVEL_UUID);
    };
    auto one_level = [&](const std::vector<std::string>& files, const std::string& where) {
      std::optional<Uuid> u;
      for (const auto& f : files) {
        if (!dst.exists(f)) continue;
        const Uuid x = level_of(f);
        if (u && x != *u) problem(where, "level_UUID", "differs between files of one level");
        u = x;
      }
      if (u && (all_zero(*u) || !seen.insert(*u).second))
        problem(where, "level_UUID", "is unset or shared with another level");
    };
    for (const auto& dc : dlayout.channels)
      for (const auto& ds : dc.segments)
        one_level({ds.tmet, ds.tidx, ds.tdat, ds.rdat(), ridx_of(ds.rdat())}, ds.dir());
    for (const auto& ch : dch) {
      const std::string r = ch + ".timd/" + ch + ".rdat";
      one_level({r, ridx_of(r)}, ch + ".timd");
    }
    one_level({m.new_name + ".rdat", m.new_name + ".ridx"}, m.new_name + ".rdat");
  }

  // Channel- and session-level annotations: the sources', merged by time.
  auto merged_records = [&](const std::string& drel,
                            const std::vector<std::pair<std::size_t, std::string>>& from) {
    RawRecordFile d;
    try {
      d = read_record_file(dst, drel, vos.empty() ? "" : vos[0].output_password);
    } catch (const std::exception& e) {
      problem(drel, "records", std::string("cannot be read: ") + e.what());
      return;
    }
    struct Want { si8 t; const RawRecord* r; std::vector<ui1> body; si8 idx_time; };
    std::vector<RawRecordFile> files;
    files.reserve(from.size());
    std::vector<Want> want;
    for (const auto& [k, rel] : from) {
      files.push_back(read_record_file(*m.sources[k]->pf.source, rel, m.sources[k]->options.password));
      const RawRecordFile& f = files.back();
      const si8 rto = m.sources[k]->pf.plan->rto;
      for (std::size_t i = 0; i < f.records.size(); ++i) {
        const RawRecord& r = f.records[i];
        si8 idx_time = r.time;  // the source index's time for it, else its own
        for (const auto& e : f.ridx)
          if (read<si8>(e, 8) == f.record_offsets[i]) idx_time = read<si8>(e, 16);
        want.push_back({is_sentinel(r.time, false) ? r.time : to_user_time(r.time, rto), &r,
                        seiz_without(r, excluded_keys), idx_time});
      }
    }
    std::stable_sort(want.begin(), want.end(), [](const Want& a, const Want& b) { return a.t < b.t; });
    if (want.size() != d.records.size()) {
      problem(drel, "records", "the result holds " + std::to_string(d.records.size()) +
                                   ", the sources " + std::to_string(want.size()));
      return;
    }
    for (std::size_t i = 0; i < want.size(); ++i) {
      const RawRecord& a = *want[i].r;
      const RawRecord& b = d.records[i];
      const std::string loc = drel + " record " + std::to_string(i);
      ++rep.records;
      const auto& wb = want[i].body;
      const bool same = a.type_bytes == b.type_bytes && a.vmaj == b.vmaj && a.vmin == b.vmin &&
                        a.time == b.time && b.body.size() >= wb.size() &&
                        b.body.size() - wb.size() < 16 &&
                        std::equal(wb.begin(), wb.end(), b.body.begin()) &&
                        std::all_of(b.body.begin() + static_cast<std::ptrdiff_t>(wb.size()),
                                    b.body.end(), [](ui1 c) { return c == PAD_BYTE; });
      if (!same) problem(loc, "record", "is not the sources' record at this place in time order");
      if (b.enc != record_encryption(a.enc, !vos[0].output_password.empty(), false))
        problem(loc, "record header encryption", "does not match the output passwords");
      // Its index entry: found by offset, the record's type, version and
      // encryption, and the source index's time.
      const std::array<ui1, RIDX>* e = nullptr;
      for (const auto& x : d.ridx)
        if (read<si8>(x, 8) == d.record_offsets[i]) e = &x;
      if (!e) {
        problem(loc, "record index", "has no entry pointing at the record");
      } else if (!std::equal(b.type_bytes.begin(), b.type_bytes.end(), e->begin()) ||
                 (*e)[5] != b.vmaj || (*e)[6] != b.vmin || static_cast<si1>((*e)[7]) != b.enc ||
                 read<si8>(*e, 16) != want[i].idx_time) {
        problem(loc, "record index entry", "is not the source's for this record");
      }
    }
    // The file's headers: the first source's, as a copy's (times aside),
    // counting what it holds and spanning its records.
    const RawRecordFile& first = files.front();
    Verifier& vv = *v[from.front().first];
    vv.set_cut_segment(true);
    vv.check_uh(first.uh, d.uh, drel);
    if (d.has_ridx) vv.check_uh(first.has_ridx ? first.ridx_uh : first.uh, d.ridx_uh,
                                ridx_of(drel), false, !first.has_ridx);
    else problem(drel, "record index", "is missing");
    vv.set_cut_segment(false);
    si8 largest = 0;
    for (const auto& r : d.records) largest = std::max(largest, static_cast<si8>(RHDR + r.body.size()));
    vv.check_counts(d.uh, static_cast<si8>(d.records.size()), largest, drel);
    if (d.has_ridx)
      vv.check_counts(d.ridx_uh, static_cast<si8>(d.records.size()), static_cast<si8>(RIDX),
                      ridx_of(drel));
    std::optional<si8> lo, hi;
    for (const auto& w : want)
      if (!is_sentinel(w.t, false)) {
        lo = std::min(lo.value_or(w.t), w.t);
        hi = std::max(hi.value_or(w.t), w.t);
      }
    const si8 rto = m.sources[from.front().first]->pf.plan->rto;
    if (lo && (to_user_time(read<si8>(d.uh, UH_START), rto) != *lo ||
               to_user_time(read<si8>(d.uh, UH_END), rto) != *hi))
      problem(drel, "universal header times", "do not span the records the file holds");
  };
  for (const auto& ch : dch) {
    std::vector<std::pair<std::size_t, std::string>> from;
    const std::string rel = ch + ".timd/" + ch + ".rdat";
    for (std::size_t k = 0; k < m.sources.size(); ++k)
      if (m.sources[k]->used && m.sources[k]->pf.source->exists(rel)) from.push_back({k, rel});
    if (!from.empty()) merged_records(rel, from);
    else if (dst.exists(rel)) problem(rel, "channel records", "present in the result, in no source");
  }
  {
    std::vector<std::pair<std::size_t, std::string>> from;
    for (std::size_t k = 0; k < m.sources.size(); ++k)
      if (m.sources[k]->used && !m.sources[k]->pf.session_records.empty())
        from.push_back({k, m.sources[k]->pf.session_records});
    if (!from.empty()) merged_records(m.new_name + ".rdat", from);
  }

  // The validator: nothing on the result (sources' own findings are not
  // excused: their segments are renumbered; repair the sources first).
  {
    ValidateOptions vo;
    vo.exact_difference_bytes = false;
    vo.password = vos.empty() ? "" : vos[0].output_password;
    Control inner;
    inner.cancel = ctl.cancel;
    const Report dr = validate_session(dst_path, vo, inner);
    for (const auto& f : dr.findings)
      if (f.severity != Severity::Info)
        problem(f.path, f.check_id, "the validator reports on the result: " + f.message);
    for (const auto& sk : dr.skipped)
      problem(sk.path, "validator", "could not check a segment of the result: " + sk.reason);
  }
  for (const auto& part : parts) {
    rep.problems.insert(rep.problems.end(), part.problems.begin(), part.problems.end());
    rep.blocks += part.blocks;
    rep.records += part.records;
    rep.times_checked += part.times_checked;
    rep.text_fields_checked += part.text_fields_checked;
  }
  rep.ok = rep.problems.empty();
  return rep;
}

void run_merge(const std::vector<std::string>& sources, const std::string& dst_path,
               const MergeOptions& o, const Control& ctl, MergeReport& rep) {
  MergePlan m = preflight_merge(sources, dst_path, o, ctl, /*collect=*/false);
  if (!m.refusals.empty()) raise(m.refusals.front());
  const OutputKeys out = output_keys(o);
  rep.sources = sources;
  rep.destination = m.dst.string();
  rep.session_name = m.new_name;
  rep.channels_kept.assign(m.kept.begin(), m.kept.end());
  rep.channels_excluded.assign(m.excluded.begin(), m.excluded.end());
  rep.output_encrypted = out.encrypt;
  rep.notes = m.notes;
  rep.segments_split = m.segments_split;

  std::uint64_t total_bytes = 0;
  for (const auto& sp : m.sources)
    if (sp->used) total_bytes += sp->pf.plan->summary.total_bytes;
  detail::ProgressReporter progress(ctl, std::max<std::uint64_t>(1, 3 * total_bytes));
  std::function<void(std::uint64_t)> on_bytes;
  if (progress.active()) on_bytes = [&](std::uint64_t n) { progress.add(n); };

  const fs::path root = m.partial / m.dst.filename();
  try {
    progress.start();
    fs::create_directories(root);
    std::vector<Shifter> shifters;
    shifters.reserve(m.sources.size());
    std::vector<std::unique_ptr<Copier>> cps;
    for (auto& sp : m.sources) {
      shifters.push_back(Shifter{sp->used ? sp->pf.plan->rto : 0, 0});
      if (!sp->used) {
        cps.push_back(nullptr);
        continue;
      }
      cps.push_back(std::make_unique<Copier>(*sp->pf.source,
                                             Rules{&sp->options, nullptr, &sp->options.metadata},
                                             shifters.back(), nullptr, m.new_name, "", m.excluded,
                                             m.kept, out, ctl, on_bytes, rep, nullptr, &rep));
    }
    const Uuid session_uuid = random_uuid();
    // Annotations of one level from several sources: merged by time.
    auto merge_records = [&](const std::vector<std::pair<std::size_t, std::string>>& from,
                             const fs::path& out_rdat, RecordLevel level, const std::string& ch,
                             const Uuid& level_uuid) {
      std::vector<Copier::OutRecord> all;
      std::optional<RawRecordFile> headers;
      std::size_t first = from.front().first;
      for (const auto& [k, rel] : from) {
        ctl.check();
        RawRecordFile f = read_record_file(*m.sources[k]->pf.source, rel,
                                           m.sources[k]->options.password);
        auto recs = cps[k]->prepare_records(f, rel, level, ch, -1);
        all.insert(all.end(), std::make_move_iterator(recs.begin()),
                   std::make_move_iterator(recs.end()));
        if (!headers) headers = std::move(f);
      }
      std::stable_sort(all.begin(), all.end(),
                       [](const Copier::OutRecord& a, const Copier::OutRecord& b) {
                         return a.sort_time < b.sort_time;
                       });
      cps[first]->set_level_uuid(level_uuid);
      // The file's header spans the records it now holds.
      std::optional<si8> lo, hi;
      for (const auto& r : all)
        if (!is_sentinel(r.sort_time, false)) {
          lo = std::min(lo.value_or(r.sort_time), r.sort_time);
          hi = std::max(hi.value_or(r.sort_time), r.sort_time);
        }
      if (lo) cps[first]->set_record_span(*lo, *hi, m.sources[first]->pf.plan->rto);
      cps[first]->write_records(all, *headers, "merged " + out_rdat.filename().string(), out_rdat,
                                ch);
      cps[first]->end_segment();
    };
    for (const auto& ch : m.kept) {
      const fs::path cdir = root / (ch + ".timd");
      fs::create_directory(cdir);
      const Uuid channel_uuid = random_uuid();
      std::vector<std::pair<std::size_t, std::string>> from;
      const std::string crec = ch + ".timd/" + ch + ".rdat";
      for (std::size_t k = 0; k < m.sources.size(); ++k)
        if (cps[k] && m.sources[k]->pf.source->exists(crec)) from.push_back({k, crec});
      if (!from.empty()) merge_records(from, cdir / (ch + ".rdat"), RecordLevel::Channel, ch, channel_uuid);
      const auto it = m.pieces.find(ch);
      if (it == m.pieces.end()) continue;  // record-only channel
      si8 next_start = 0;
      int number = 0;
      for (const auto& pc : it->second) {
        Copier& cp = *cps[pc.source];
        cp.set_level_uuid(random_uuid());
        if (!cp.copy_segment(*pc.segment, cdir, ch, number, next_start, pc.entries)) continue;
        const std::string stem = canonical_segment_stem(ch, number);
        if (m.sources[pc.source]->pf.source->exists(pc.segment->rdat())) {
          const si8 lo = pc.lo, hi = pc.hi;
          cp.copy_records(pc.segment->rdat(), cdir / (stem + ".segd") / (stem + ".rdat"),
                          RecordLevel::Segment, ch, number,
                          [lo, hi](si8 t) { return t >= lo && t < hi; });
        }
        cp.end_segment();
        ++number;
        ++rep.segments;
      }
    }
    {
      std::vector<std::pair<std::size_t, std::string>> from;
      for (std::size_t k = 0; k < m.sources.size(); ++k)
        if (cps[k] && !m.sources[k]->pf.session_records.empty())
          from.push_back({k, m.sources[k]->pf.session_records});
      if (!from.empty())
        merge_records(from, root / (m.new_name + ".rdat"), RecordLevel::Session, "", session_uuid);
    }
    for (const auto& cp : cps)
      if (cp)
        for (const auto& f : cp->written) detail::fsync_file_or_throw(f);

    rep.verification = verify_merge_impl(m, root.string(), ctl, on_bytes);
    if (!rep.verification.ok) {
      std::string msg = "the merged session failed verification, so it was not published (" +
                        std::to_string(rep.verification.problems.size()) + " problem(s)):";
      const std::size_t show = std::min<std::size_t>(rep.verification.problems.size(), 10);
      for (std::size_t i = 0; i < show; ++i) {
        const auto& p = rep.verification.problems[i];
        msg += "\n  - " + p.location + ": " + p.field + " " + p.message;
      }
      refuse(RefusalKind::WriteConflict, "verify-failed", msg);
    }
    ctl.check();
    progress.finish();
    if (fs::exists(m.dst))
      refuse(RefusalKind::Io, "dst-exists",
             "the destination appeared while merging: " + m.dst.string(), {}, "dst");
    fs::rename(root, m.dst);
    detail::fsync_directory(m.dst.parent_path().empty() ? fs::path(".") : m.dst.parent_path());
  } catch (...) {
    std::error_code ec;
    fs::remove_all(m.partial, ec);
    throw;
  }
  std::error_code ec;
  fs::remove_all(m.partial, ec);
}

}  // namespace

MergeReport merge_sessions(const std::vector<std::string>& sources, const std::string& dst,
                           const MergeOptions& o, const Control& ctl) {
  return coded([&] {
    MergeReport rep;
    run_merge(sources, dst, o, ctl, rep);
    return rep;
  });
}

CopyCheck check_merge(const std::vector<std::string>& sources, const std::string& dst,
                      const MergeOptions& o, const Control& ctl) {
  MergePlan m = preflight_merge(sources, dst, o, ctl, /*collect=*/true);
  CopyCheck c;
  c.ok = m.refusals.empty();
  c.refusals = std::move(m.refusals);
  c.notes = std::move(m.notes);
  c.session_name = m.new_name;
  c.channels_kept.assign(m.kept.begin(), m.kept.end());
  c.channels_excluded.assign(m.excluded.begin(), m.excluded.end());
  return c;
}

VerifyReport verify_merge(const std::vector<std::string>& sources, const std::string& dst,
                          const MergeOptions& o, const Control& ctl) {
  // The plan is the merge's own (pieces from the sources' indexes), made as
  // if the result did not exist yet.
  MergePlan m = preflight_merge(sources, dst + ".verify-plan.mefd", o, ctl, /*collect=*/true);
  for (const auto& r : m.refusals)
    if (r.code != "dst-exists" && r.code != "partial-exists") raise(r);
  m.new_name = session_name_from_path(dst);
  return verify_merge_impl(m, dst, ctl, {});
}

}  // namespace mef3io
