// mef3io — RED (Range Encoded Differences) block decoding.
#pragma once

#include <span>
#include <vector>

#include "mef3io/crypto.hpp"
#include "mef3io/headers.hpp"
#include "mef3io/types.hpp"

namespace mef3io::red {

// Range-coder parameters (meflib.h).
inline constexpr int EXTRA_BITS = 7;
inline constexpr ui4 BOTTOM_VALUE = 0x800000u;  // 1 << 23
inline constexpr int STATISTICS_BYTES = 256;

// Round half away from zero, clamped to RED's si4 infinity sentinels
// (matches meflib RED_round).
si4 round_si4(sf8 val);

// RED statistics-table encryption. meflib encrypts ONLY THE FIRST 16 BYTES
// (one AES-128 block) of the 256-byte statistics table: RED_encode calls
// AES_encrypt(block_header->statistics, ...) (meflib.c:7027) and RED_decode
// AES_decrypt (meflib.c:6676), and both run AES_cipher / AES_inv_cipher
// (meflib.c:292 / :423) over a single 4x4 state. Bytes 16..255 are stored in
// the clear. The block CRC is computed AFTER encryption (meflib.c:7045), so it
// covers the ciphertext. These helpers transform bytes 0..15 in place and leave
// 16..255 untouched; `key` is the raw 16-byte key (expanded internally).
inline constexpr int ENCRYPTED_STATISTICS_BYTES = fmt::ENCRYPTION_BLOCK_BYTES;
void encrypt_statistics(std::span<ui1, STATISTICS_BYTES> statistics, std::span<const ui1> key);
void decrypt_statistics(std::span<ui1, STATISTICS_BYTES> statistics, std::span<const ui1> key);

struct DecodedBlock {
  std::vector<si4> samples;
  si8 start_time = 0;      // block header start_time (offset-relative, as stored)
  bool discontinuity = false;
};

// Decode one RED block. `block` must span the whole block (header + payload,
// i.e. block_bytes long). If the block statistics are encrypted, `keys` must
// grant sufficient access or a PasswordError is thrown.
DecodedBlock decode_block(std::span<const ui1> block,
                          const crypto::AccessKeys& keys = {},
                          bool validate_crc = true);

// Encode one block of int32 samples as lossless RED (no detrend/scale), 8-byte
// aligned, with header/CRC filled in. `start_time` is stored as given (caller
// applies the recording-time-offset convention). If `encryption_level` > 0 the
// first 16 bytes of the statistics table are AES-encrypted with the matching
// key from `keys` (meflib's rule; see encrypt_statistics).
// Also fills `min_out`/`max_out` with the block sample extrema for the index.
std::vector<ui1> encode_block(std::span<const si4> samples, si8 start_time, bool discontinuity,
                              si4& min_out, si4& max_out, int encryption_level = 0,
                              const crypto::AccessKeys& keys = {});

}  // namespace mef3io::red
