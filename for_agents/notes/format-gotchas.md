# Format gotchas (do NOT relearn the hard way)

> Read before touching ANY on-disk field: times, encryption/passwords, RED encode, records layout, strings, .tmet/.tidx offsets; or when a test against the oracle disagrees.
> Split out of CLAUDE.md (verbatim except retargeted cross-references); "the private reader" = the private third-party reader named in CLAUDE.md.

- **Times are stored NEGATED on disk** as meflib's "recording-time-offset
  applied" marker. User uUTC = `(t>=0)?t:(-t+rto)` (read), `disk = rto-absolute`
  (write). Applies to UH start/end AND block/record times. rto=0 in fixtures.
  Missing this makes every time-range read return 0 samples.
- **Encryption is all-or-nothing pairing**: section 2 → L1 key, section 3 → L2
  key, by password presence. "Level-1 only" is not a valid file. Decrypt a
  section only when its stored level is strictly positive; unencrypted files
  carry -1/-2 (`0xFF`/`0xFE`). See docs/encryption_model.md.
- **Password scheme**: bytes = LOW BYTE OF EACH CHARACTER'S UNICODE CODE
  POINT (meflib `extract_terminal_password_bytes` via `UTF8_nextchar`,
  meflib.c:2439/8855; "é" -> 0xE9), 16, zero-pad. NOT the last UTF-8 byte —
  that was mef3io <= 1.2.0's bug (#84): equal on ASCII, wrong on everything
  else. Chars > U+00FF collide (low byte only) — matched exactly, warned on
  write (`PasswordInteropWarning`, also > 15 chars / > 15 UTF-8 bytes: pymef
  strcpy's into a 16-byte buffer; the same rule is public as
  `mef3io.password_interop_issue(pw) -> str | None`, Python only, #111). L1 field = SHA256(L1)[:16]; L2 field =
  SHA256(L2)[:16] XOR L1. L2 access derives putative L1 = SHA256(pwd)[:16]
  XOR L2field, checks its hash. READ FALLBACK (`validate_password`): meflib
  rule first; only if it fails AND the legacy bytes differ, the old
  last-UTF-8-byte rule (`extract_password_bytes_legacy`), which sets
  `AccessKeys::legacy_password_mapping` -> `LegacyPasswordWarning` /
  `mef3io:legacyPassword`. Never WRITE with the old rule; an append to an
  old segment keeps its keys (and warns), new segments use meflib's.
- **RED data blocks are UNENCRYPTED** even in encrypted sessions (meflib
  default); only metadata s2/s3 and record bodies are encrypted.
- **Index file_offset is FILE-relative** (includes the 1024 B UH); first block
  offset = 1024. Windowed reads must read only the needed byte range
  (`collect_blocks` uses `read_file_range`), not the whole `.tdat`.
- **`.tmet` is a FIXED-length record** (1024 B UH + 15360 B sections =
  `METADATA_FILE_BYTES`); the size check is `>=`, and foreign writers do append
  trailing bytes past its end. The body CRC must be bounded by
  `METADATA_FILE_BYTES`, NOT taken to EOF — hashing the padding rejects intact
  metadata as "corrupted" and, since it throws in the ctor, kills the whole
  session (reported in 1.1.1: 1094 padded files, 0 actually corrupt).

- **RED encode**: first emitted byte is junk (meflib overwrites stats[255] then
  restores) → drop emitted[0], payload = emitted[1:] at offset 304; stored
  difference_bytes = generated+1. Lossless no-detrend/no-scale, pymef-readable.
  Constants: TOP_VALUE 0x80000000, CARRY_CHECK 0x7F800000, SHIFT_BITS 23,
  EXTRA_BITS 7, BOTTOM_VALUE 0x800000, PAD_BYTE 0x7e, 8-byte alignment.
- **Records**: header 24 B (crc@0,type[4]@4,vmaj@9,vmin@10,enc@11,bytes@12,
  time@16). Body padded to 16-byte multiple with 0x7e. L2-encrypted when the
  session is encrypted. `.ridx` entry 24 B (type@0,vmaj@5,vmin@6,enc@7,
  offset@8,time@16). file_offset FILE-relative.
- **Fixed-width strings**: text fields (units_description 128 B,
  channel/session_description 2048 B, subject_* 128 B, …) are NUL-terminated,
  so max content is field_len - 1. `byteio::write_string` enforces that AND
  backs the cut off to a UTF-8 character boundary — a half-written multi-byte
  char makes the whole session throw `UnicodeDecodeError` on open in Python.
  The 4-byte record type code ("EDFA"/"Note") is the exception: it fills the
  field with no terminator, so it uses `byteio::write_fixed_code` instead —
  which REQUIRES an exact-width value. Padding/trimming a type code writes a
  header claiming a type the body was not built for (writing "Notes" stored a
  "Note" header with an empty body, silently dropping the text), so
  `write_records` rejects any type that is not 4 ASCII bytes up front, before
  opening a file. Unknown 4-char types still pass through with an empty body.
- **`reference_files/` IS PRESENT in this repo** (gitignored):
  `meflib-multiplatform/` (authoritative C), `pymef-develop/`, `mef_tools/`,
  `mef3_dump-main/`. Settle every format question against it — do not reason
  from memory, and do not trust a doc comment that cites it.
- **Sign conventions can be MIXED WITHIN ONE FILE — always compare via
  `to_user_time`.** Verified 2026-10-14 against the installed legacy stack
  (mef-tools 1.2.3 / pymef 1.4.8): it stores EVERY time meflib-style as
  −(t − rto) — `.tmet`/`.tidx`/`.tdat` universal headers, index entries, RED
  block headers and record headers alike (−t at rto 0). Older fixtures were
  seen with `.tidx`/`.tdat` universal-header times NOT negated, and files that
  passed through other writers can mix conventions, so never assume one. What
  makes `rto != 0` hazardous is that a time at or before `rto` cannot be stored
  as −(t − rto) (it turns non-negative and every reader takes it as absolute).
  So the validator's time checks still stand down entirely when `rto != 0`
  rather than risk rewriting a correct file.
- **Oracle**: use `pymef` `read_ts_channels_sample([ch],[0,nsamp])` for decoded
  int32 (no gap NaN) and `read_ts_channels_uutc` for gap-filled. `mef3_dump` is
  NOT usable (reads the encryption sentinel byte unsigned). Manifest `nsamp` !=
  stored nsamp (it's get_raw_data length incl. gaps) — compare vs pymef
  basic_info. Oracle agreement is exact for grid-aligned block times; when
  block timestamps drift off the grid the two layouts differ *by design* —
  meflib packs blocks contiguously by sample count within a run, mef3io places
  each block at its own timestamp. Not reconciled (see reader-placement-and-factors.md, "Off-grid block times").
