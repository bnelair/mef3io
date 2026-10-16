// mef3io — MEF two-level password scheme.
#include <algorithm>
#include <string>

#include "mef3io/crypto.hpp"

namespace mef3io::crypto {

namespace {

// meflib's UTF8_nextchar (meflib.c:8855), byte for byte: accumulate the lead
// byte and every following continuation byte, then subtract the table entry
// for the sequence length. On well-formed UTF-8 this is the code point; on
// malformed input it is whatever meflib computes, which is what matters for
// matching its keys. `i` advances past the sequence. Stops at a NUL byte.
ui4 utf8_nextchar(const std::string& s, std::size_t& i) {
  static constexpr ui4 kOffsets[6] = {0x00000000UL, 0x00003080UL, 0x000E2080UL,
                                      0x03C82080UL, 0xFA082080UL, 0x82082080UL};
  const auto* p = reinterpret_cast<const ui1*>(s.data());
  ui4 ch = 0;
  std::size_t sz = 0;
  do {
    ch <<= 6;
    ch += p[i++];
    ++sz;
  } while (i < s.size() && p[i] != 0 && (p[i] & 0xC0) == 0x80);
  // meflib indexes past its 6-entry table for a run of >5 continuation bytes
  // (undefined behaviour there); clamp so the result is at least defined.
  ch -= kOffsets[std::min<std::size_t>(sz, 6) - 1];
  return ch;
}

}  // namespace

std::array<ui1, fmt::PASSWORD_BYTES> extract_password_bytes(const std::string& password) {
  // Low byte of each character's code point (meflib extract_terminal_password_
  // bytes, meflib.c:2439), up to 16, zero-padded. meflib stops at the first
  // character that decodes to 0 (its string terminator); so does this.
  std::array<ui1, fmt::PASSWORD_BYTES> out{};
  std::size_t i = 0, n = 0;
  while (i < password.size() && n < fmt::PASSWORD_BYTES) {
    const ui4 ch = utf8_nextchar(password, i);
    if (ch == 0) break;
    out[n++] = static_cast<ui1>(ch & 0xFF);
  }
  return out;
}

std::array<ui1, fmt::PASSWORD_BYTES> extract_password_bytes_legacy(const std::string& password) {
  // mef3io <= 1.2.0: terminal byte of each UTF-8 sequence, up to 16, zero-padded.
  // Kept unchanged so sessions written with it still open (issue #84).
  std::array<ui1, fmt::PASSWORD_BYTES> out{};
  const auto* p = reinterpret_cast<const ui1*>(password.data());
  std::size_t i = 0, n = 0;
  while (i < password.size() && n < fmt::PASSWORD_BYTES) {
    ui1 b = p[i];
    std::size_t len;
    if ((b & 0x80) == 0x00) len = 1;
    else if ((b & 0xE0) == 0xC0) len = 2;
    else if ((b & 0xF0) == 0xE0) len = 3;
    else if ((b & 0xF8) == 0xF0) len = 4;
    else len = 1;  // invalid start byte: treat as single, don't throw
    if (i + len > password.size()) len = password.size() - i;
    out[n++] = p[i + len - 1];  // terminal byte of the sequence
    i += len;
  }
  return out;
}

std::string password_interop_issue(const std::string& password) {
  std::size_t i = 0, chars = 0;
  bool above_latin1 = false;
  while (i < password.size()) {
    const ui4 ch = utf8_nextchar(password, i);
    if (ch == 0) break;
    ++chars;
    if (ch > 0xFF) above_latin1 = true;
  }
  // Measured up to the first NUL, which is where every C reader stops.
  const std::size_t utf8_bytes = std::min(password.find('\0'), password.size());
  constexpr std::size_t kMax = fmt::PASSWORD_BYTES - 1;  // 15, meflib's MAX_PASSWORD_CHARACTERS
  std::string out;
  if (chars > kMax)
    out = "it has " + std::to_string(chars) +
          " characters; meflib and pymef refuse passwords longer than 15 characters, so only "
          "mef3io will open this session" +
          (chars > static_cast<std::size_t>(fmt::PASSWORD_BYTES)
               ? std::string(" (and characters past the 16th are ignored)")
               : std::string());
  else if (utf8_bytes > kMax)
    out = "it is " + std::to_string(utf8_bytes) +
          " bytes long in UTF-8; pymef (and so mef_tools) copies the password into a 16-byte "
          "buffer, so it cannot safely handle more than 15 bytes, although meflib itself "
          "accepts up to 15 characters";
  if (above_latin1) {
    if (!out.empty()) out += "; ";
    out += "it contains a character above U+00FF; MEF keeps only the low byte of each "
           "character's code point (as meflib and pymef do), so a different password with the "
           "same low bytes also unlocks this session";
  }
  return out;
}

namespace {
AccessKeys validate_bytes(const std::array<ui1, fmt::PASSWORD_BYTES>& pwd,
                          std::span<const ui1> level1_validation_field,
                          std::span<const ui1> level2_validation_field) {
  AccessKeys keys;
  auto sha = sha256(pwd);

  // Level 1: SHA256(pwd)[:16] == L1 validation field.
  if (std::equal(level1_validation_field.begin(),
                 level1_validation_field.begin() + fmt::PASSWORD_VALIDATION_FIELD_BYTES,
                 sha.begin())) {
    keys.access_level = fmt::LEVEL_1_ACCESS;
    keys.level1_key = pwd;
    return keys;
  }

  // Level 2: putative L1 key = SHA256(pwd)[:16] XOR L2 field; if its hash
  // matches the L1 field, `pwd` is the L2 password and grants full access.
  std::array<ui1, fmt::PASSWORD_BYTES> putative_l1{};
  for (int i = 0; i < fmt::PASSWORD_VALIDATION_FIELD_BYTES; ++i)
    putative_l1[i] = static_cast<ui1>(sha[i] ^ level2_validation_field[i]);
  auto putative_sha = sha256(putative_l1);
  if (std::equal(level1_validation_field.begin(),
                 level1_validation_field.begin() + fmt::PASSWORD_VALIDATION_FIELD_BYTES,
                 putative_sha.begin())) {
    keys.access_level = fmt::LEVEL_2_ACCESS;
    keys.level1_key = putative_l1;
    keys.level2_key = pwd;
    return keys;
  }

  return keys;  // no match -> level 0, no keys
}
}  // namespace

AccessKeys validate_password(const std::string& password,
                             std::span<const ui1> level1_validation_field,
                             std::span<const ui1> level2_validation_field) {
  if (password.empty()) return AccessKeys{};  // level 0

  const auto pwd = extract_password_bytes(password);
  auto keys = validate_bytes(pwd, level1_validation_field, level2_validation_field);
  if (keys.access_level != fmt::LEVEL_0_ACCESS) return keys;

  // Fallback for sessions an older mef3io encrypted with a non-ASCII password
  // (issue #84). Tried only when the legacy bytes actually differ, i.e. the
  // password has a non-ASCII character; an ASCII password maps identically.
  const auto legacy = extract_password_bytes_legacy(password);
  if (legacy == pwd) return keys;
  keys = validate_bytes(legacy, level1_validation_field, level2_validation_field);
  if (keys.access_level != fmt::LEVEL_0_ACCESS) keys.legacy_password_mapping = true;
  return keys;
}

ValidationFields make_validation_fields(const std::string& password1,
                                        const std::string& password2) {
  ValidationFields vf;
  if (password1.empty()) return vf;  // all zero -> unencrypted

  auto l1 = extract_password_bytes(password1);
  auto l1_hash = sha256(l1);
  std::copy_n(l1_hash.begin(), fmt::PASSWORD_VALIDATION_FIELD_BYTES, vf.level1.begin());

  if (!password2.empty()) {
    auto l2 = extract_password_bytes(password2);
    auto l2_hash = sha256(l2);
    for (int i = 0; i < fmt::PASSWORD_VALIDATION_FIELD_BYTES; ++i)
      vf.level2[i] = static_cast<ui1>(l2_hash[i] ^ l1[i]);
  }
  return vf;
}

}  // namespace mef3io::crypto
