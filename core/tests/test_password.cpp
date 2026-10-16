// mef3io — password -> key bytes (issue #84).
//
// meflib derives one key byte per character: the LOW BYTE OF ITS UNICODE CODE
// POINT (extract_terminal_password_bytes, meflib.c:2439, via UTF8_nextchar,
// meflib.c:8855). mef3io 1.2.0 and earlier kept the LAST UTF-8 BYTE instead, which
// agrees on ASCII and nothing else. Every golden value below was produced by
// the vendored meflib's own extract_terminal_password_bytes, not by mef3io.
#include <catch2/catch_test_macros.hpp>
#include <string>

#include "mef3io/c_api.h"
#include "mef3io/crypto.hpp"

using namespace mef3io;

namespace {
std::string hex(const std::array<ui1, fmt::PASSWORD_BYTES>& b) {
  static const char* d = "0123456789abcdef";
  std::string s;
  for (ui1 c : b) {
    s += d[c >> 4];
    s += d[c & 15];
  }
  return s;
}
// UTF-8 literals spelled as bytes, so the source encoding cannot matter.
const std::string kBe = "b\xC3\xA9";                                  // "bé"
const std::string kUnicode = "\xC3\x9Cn\xC3\xAF" "c\xC3\xB6" "d\xC3\xA9";  // "Ünïcödé"
const std::string kYuml = "\xC3\xBF";                                 // "ÿ" U+00FF
const std::string kAmacron = "\xC4\x80";                              // "Ā" U+0100
}  // namespace

TEST_CASE("password bytes follow meflib: low byte of each code point") {
  CHECK(hex(crypto::extract_password_bytes("abc")) == "61626300000000000000000000000000");
  CHECK(hex(crypto::extract_password_bytes(kBe)) == "62e90000000000000000000000000000");
  CHECK(hex(crypto::extract_password_bytes(kUnicode)) == "dc6eef63f664e9000000000000000000");
  CHECK(hex(crypto::extract_password_bytes(kYuml)) == "ff000000000000000000000000000000");
  // Above U+00FF only the low byte survives, so these COLLIDE — exactly as in
  // meflib. "Ā" (U+0100) gives 0x00, the same as the padding.
  CHECK(hex(crypto::extract_password_bytes(kAmacron)) == "00000000000000000000000000000000");
  CHECK(hex(crypto::extract_password_bytes(kAmacron + "a")) == "00610000000000000000000000000000");
  // 16 characters fill the key; more are ignored (meflib refuses > 15).
  CHECK(hex(crypto::extract_password_bytes(std::string(17, 'a'))) ==
        "61616161616161616161616161616161");
  // Malformed UTF-8 decodes the way meflib's UTF8_nextchar decodes it.
  CHECK(hex(crypto::extract_password_bytes("\x80" "a")) == "80610000000000000000000000000000");
  CHECK(hex(crypto::extract_password_bytes("\xC3")) == "c3000000000000000000000000000000");
  CHECK(hex(crypto::extract_password_bytes("\xC3\xA9\xA9")) == "69000000000000000000000000000000");
  CHECK(hex(crypto::extract_password_bytes("\xFE" "A")) == "fe410000000000000000000000000000");
  // Stops at the first NUL, as every C reader does.
  CHECK(hex(crypto::extract_password_bytes(std::string("ab\0cd", 5))) ==
        "61620000000000000000000000000000");
}

TEST_CASE("the legacy mapping is the last UTF-8 byte and equals meflib's on ASCII") {
  CHECK(hex(crypto::extract_password_bytes_legacy(kBe)) == "62a90000000000000000000000000000");
  CHECK(hex(crypto::extract_password_bytes_legacy(kYuml)) == "bf000000000000000000000000000000");
  for (const std::string pw : {"a", "pass1", "lvl2", "~!@#$%^&*()_+", "0123456789abcdef"})
    CHECK(crypto::extract_password_bytes(pw) == crypto::extract_password_bytes_legacy(pw));
}

TEST_CASE("validate_password falls back to the legacy mapping, and says so") {
  // A session keyed the meflib way: no fallback involved.
  auto vf = crypto::make_validation_fields(kBe, kUnicode);
  auto k2 = crypto::validate_password(kUnicode, vf.level1, vf.level2);
  CHECK(k2.access_level == fmt::LEVEL_2_ACCESS);
  CHECK_FALSE(k2.legacy_password_mapping);
  auto k1 = crypto::validate_password(kBe, vf.level1, vf.level2);
  CHECK(k1.access_level == fmt::LEVEL_1_ACCESS);
  CHECK_FALSE(k1.legacy_password_mapping);

  // A session keyed the OLD way. Building its fields with passwords whose
  // meflib bytes ARE the legacy bytes of the real passwords reproduces what an
  // older mef3io wrote: "b©" (U+00A9) maps to 62 A9, the legacy bytes of "bé".
  const std::string l1_old = "b\xC2\xA9";                          // "b©"
  const std::string l2_old = "\xC2\x9Cn\xC2\xAF" "c\xC2\xB6" "d\xC2\xA9";
  REQUIRE(crypto::extract_password_bytes(l1_old) == crypto::extract_password_bytes_legacy(kBe));
  REQUIRE(crypto::extract_password_bytes(l2_old) ==
          crypto::extract_password_bytes_legacy(kUnicode));
  auto old = crypto::make_validation_fields(l1_old, l2_old);
  auto o2 = crypto::validate_password(kUnicode, old.level1, old.level2);
  CHECK(o2.access_level == fmt::LEVEL_2_ACCESS);
  CHECK(o2.legacy_password_mapping);
  CHECK(*o2.level2_key == crypto::extract_password_bytes_legacy(kUnicode));
  CHECK(*o2.level1_key == crypto::extract_password_bytes_legacy(kBe));
  auto o1 = crypto::validate_password(kBe, old.level1, old.level2);
  CHECK(o1.access_level == fmt::LEVEL_1_ACCESS);
  CHECK(o1.legacy_password_mapping);

  // A wrong password stays wrong under both mappings.
  auto bad = crypto::validate_password("b\xC3\xA8", old.level1, old.level2);  // "bè"
  CHECK(bad.access_level == fmt::LEVEL_0_ACCESS);
  CHECK_FALSE(bad.legacy_password_mapping);
}

TEST_CASE("password_interop_issue flags only what other readers handle differently") {
  CHECK(crypto::password_interop_issue("abc").empty());
  CHECK(crypto::password_interop_issue(kBe).empty());
  CHECK(crypto::password_interop_issue(kUnicode).empty());  // 7 chars, 14 bytes
  CHECK(crypto::password_interop_issue(std::string(15, 'a')).empty());
  CHECK(crypto::password_interop_issue(std::string(16, 'a')).find("16 characters") !=
        std::string::npos);
  // 8 characters but 16 UTF-8 bytes: meflib accepts it, pymef's buffer does not.
  std::string eight_yuml;
  for (int i = 0; i < 8; ++i) eight_yuml += kYuml;
  CHECK(crypto::password_interop_issue(eight_yuml).find("16 bytes") != std::string::npos);
  CHECK(crypto::password_interop_issue(kAmacron).find("U+00FF") != std::string::npos);
}

TEST_CASE("the C ABI reports password interop issues") {
  char buf[512];
  REQUIRE(mef3io_password_interop_issue("abc", buf, sizeof buf) == 0);
  CHECK(std::string(buf).empty());
  REQUIRE(mef3io_password_interop_issue("aaaaaaaaaaaaaaaaa", buf, sizeof buf) == 0);
  CHECK(std::string(buf).find("17 characters") != std::string::npos);
  CHECK(mef3io_password_interop_issue(nullptr, buf, sizeof buf) != 0);
}
