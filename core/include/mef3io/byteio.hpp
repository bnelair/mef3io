// mef3io — little-endian field read/write over byte buffers.
//
// The MEF spec fixes little-endian on disk. We never reinterpret_cast packed
// structs (alignment/aliasing UB); every field goes through these helpers with
// an explicit offset, so the code is portable regardless of host endianness.
#pragma once

#include <cstring>
#include <span>
#include <string>
#include <type_traits>

#include "mef3io/errors.hpp"
#include "mef3io/types.hpp"

namespace mef3io::byteio {

constexpr bool host_is_little_endian() {
  // Compile-time on every compiler we target; avoids a runtime union hack.
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__)
  return __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__;
#else
  return true;  // all supported targets (x86_64, arm64, Win) are little-endian
#endif
}

template <typename T>
T byteswap(T v) {
  static_assert(std::is_trivially_copyable_v<T>);
  ui1 b[sizeof(T)];
  std::memcpy(b, &v, sizeof(T));
  for (std::size_t i = 0; i < sizeof(T) / 2; ++i) std::swap(b[i], b[sizeof(T) - 1 - i]);
  T out;
  std::memcpy(&out, b, sizeof(T));
  return out;
}

// Read a little-endian scalar of type T at `offset` in `buf`.
template <typename T>
T read(std::span<const ui1> buf, std::size_t offset) {
  static_assert(std::is_trivially_copyable_v<T>);
  if (offset + sizeof(T) > buf.size())
    throw FormatError("byteio::read out of range at offset " + std::to_string(offset));
  T v;
  std::memcpy(&v, buf.data() + offset, sizeof(T));
  if constexpr (sizeof(T) > 1)
    if (!host_is_little_endian()) v = byteswap(v);
  return v;
}

// Write a little-endian scalar of type T at `offset` in `buf`.
template <typename T>
void write(std::span<ui1> buf, std::size_t offset, T v) {
  static_assert(std::is_trivially_copyable_v<T>);
  if (offset + sizeof(T) > buf.size())
    throw FormatError("byteio::write out of range at offset " + std::to_string(offset));
  if constexpr (sizeof(T) > 1)
    if (!host_is_little_endian()) v = byteswap(v);
  std::memcpy(buf.data() + offset, &v, sizeof(T));
}

// Read a fixed-width, null-terminated UTF-8/ASCII string field.
inline std::string read_string(std::span<const ui1> buf, std::size_t offset, std::size_t max_len) {
  if (offset + max_len > buf.size())
    throw FormatError("byteio::read_string out of range at offset " + std::to_string(offset));
  const ui1* p = buf.data() + offset;
  std::size_t n = 0;
  while (n < max_len && p[n] != 0) ++n;
  return std::string(reinterpret_cast<const char*>(p), n);
}

// Write a string into a fixed-width field: null-padded, truncated to
// field_len - 1 bytes so the field is always null-terminated (MEF convention;
// meflib reads these with C string semantics and would run past an exact-fit
// field into the next one). Truncation backs off to a UTF-8 character
// boundary, so a cut never leaves a half-written multi-byte character that
// would make the field undecodable.
inline void write_string(std::span<ui1> buf, std::size_t offset, std::size_t field_len,
                         const std::string& s) {
  if (offset + field_len > buf.size())
    throw FormatError("byteio::write_string out of range at offset " + std::to_string(offset));
  ui1* p = buf.data() + offset;
  std::memset(p, 0, field_len);
  if (field_len == 0) return;
  std::size_t n = std::min(s.size(), field_len - 1);
  // If we cut mid-string, walk the cut point back over any UTF-8 continuation
  // bytes (0b10xxxxxx) so it lands on a character boundary and the kept prefix
  // holds only whole characters.
  if (n < s.size())
    while (n > 0 && (static_cast<ui1>(s[n]) & 0xC0) == 0x80) --n;
  std::memcpy(p, s.data(), n);
}

// Write a fixed-width tag that is NOT null-terminated: MEF stores some fields
// as exact-width codes that fill the field (e.g. the 4-byte record type
// "EDFA"/"Note"). Use write_string for human-readable text fields instead.
inline void write_fixed_code(std::span<ui1> buf, std::size_t offset, std::size_t field_len,
                             const std::string& s) {
  if (offset + field_len > buf.size())
    throw FormatError("byteio::write_fixed_code out of range at offset " + std::to_string(offset));
  ui1* p = buf.data() + offset;
  std::memset(p, 0, field_len);
  std::memcpy(p, s.data(), std::min(s.size(), field_len));
}

}  // namespace mef3io::byteio
