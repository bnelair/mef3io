// mef3io — cryptography for MEF: SHA-256, AES-128-ECB, and the two-level
// password scheme. Self-contained (no external crypto dependency); validated
// against NIST vectors and Python's `cryptography` in the test suite.
//
// For how MEF encryption actually behaves (it is an all-or-nothing pairing —
// section 2 with the L1 key, section 3 with the L2 key — and "level-1 only" is
// not a valid file), see docs/encryption_model.md.
#pragma once

#include <array>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "mef3io/types.hpp"

namespace mef3io::crypto {

// --- SHA-256 ---
std::array<ui1, 32> sha256(std::span<const ui1> data);

// --- AES-128-ECB ---
// key must be 16 bytes; buffers must be a multiple of 16 bytes. Operates on a
// copy and returns it (metadata sections are small; clarity over in-place).
std::vector<ui1> aes128_ecb_encrypt(std::span<const ui1> data, std::span<const ui1> key);
std::vector<ui1> aes128_ecb_decrypt(std::span<const ui1> data, std::span<const ui1> key);

// --- Password scheme ---

/// The 16 password bytes meflib derives from a UTF-8 password
/// (`extract_terminal_password_bytes`, meflib.c:2439): each character is
/// decoded to its Unicode code point (`UTF8_nextchar`, meflib.c:8855) and its
/// LOW BYTE is kept, so "é" (U+00E9, UTF-8 C3 A9) gives 0xE9. Zero-padded to 16.
/// Characters above U+00FF keep only their low byte and therefore COLLIDE
/// ("Ā" U+0100 gives 0x00, the same as the padding); this is what meflib and
/// pymef do, so it is kept exactly for interoperability. meflib refuses more
/// than 15 characters (`check_password`); mef3io keeps reading up to 16 and
/// ignores the rest, as earlier releases did. Decoding stops at the first NUL.
std::array<ui1, fmt::PASSWORD_BYTES> extract_password_bytes(const std::string& password);

/// The mapping mef3io used up to 1.2.0 (issue #84): the LAST byte of each
/// character's UTF-8 sequence ("é" gave 0xA9). Identical to
/// extract_password_bytes for ASCII. Used only as a read-side fallback for
/// sessions an older mef3io encrypted with a non-ASCII password; never for
/// writing.
std::array<ui1, fmt::PASSWORD_BYTES> extract_password_bytes_legacy(const std::string& password);

/// Why `password` will not behave the way a user expects in other MEF
/// readers, or "" when it is fine: more than 15 characters (meflib refuses
/// it, so only mef3io can open the session), or more than 15 UTF-8 bytes
/// (pymef copies the password into a 16-byte buffer), and/or a character
/// above U+00FF (only its low byte is used, so different passwords unlock the
/// same session). A writer surfaces this as a warning; it never refuses.
std::string password_interop_issue(const std::string& password);

// Result of validating a user password against a universal header's fields.
struct AccessKeys {
  int access_level = fmt::LEVEL_0_ACCESS;
  std::optional<std::array<ui1, fmt::PASSWORD_BYTES>> level1_key;  // decrypts L1 sections
  std::optional<std::array<ui1, fmt::PASSWORD_BYTES>> level2_key;  // decrypts L2 sections
  /// The password matched only through extract_password_bytes_legacy: the
  /// file was encrypted by an older mef3io with a non-ASCII password, and
  /// meflib/pymef cannot open it with that password.
  bool legacy_password_mapping = false;
};

// Validate `password` against the two 16-byte validation fields from a
// universal header. Empty/no password with all-zero fields -> level 0 (file
// unencrypted). Wrong password -> level 0 with no keys. Tries meflib's mapping
// first; only if that fails AND the legacy mapping gives different bytes (the
// password has a non-ASCII character) is the legacy mapping tried, and a match
// sets `legacy_password_mapping`.
AccessKeys validate_password(const std::string& password,
                             std::span<const ui1> level1_validation_field,
                             std::span<const ui1> level2_validation_field);

// Build the validation fields for writing. l2 requires l1.
struct ValidationFields {
  std::array<ui1, fmt::PASSWORD_VALIDATION_FIELD_BYTES> level1{};
  std::array<ui1, fmt::PASSWORD_VALIDATION_FIELD_BYTES> level2{};
};
ValidationFields make_validation_fields(const std::string& password1, const std::string& password2);

}  // namespace mef3io::crypto
