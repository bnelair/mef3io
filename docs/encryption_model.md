# MEF 3.0 Encryption Model (as implemented by mef3io)

This note explains, once and for record, how encryption actually works in the
MEF 3.0 ecosystem — because the common mental model ("you can independently
choose level-1 or level-2 encryption") does **not** match what meflib/pymef
produce on disk. mef3io follows the real behavior, and this document is the
reference for why.

## TL;DR

There are effectively **two** encryption states for a MEF 3.0 time-series file,
not three:

1. **Unencrypted** — no password. Everything is plaintext.
2. **Encrypted** — both passwords set. **Metadata section 2 is encrypted with
   the level-1 key, and section 3 with the level-2 key**, as a fixed pair.

An "encrypt section 2 with level 1 but leave section 3 alone" file is not a
product this ecosystem creates, and attempting it breaks the legacy writer (see
below). Encryption is a per-file *pairing*, driven purely by which passwords
are supplied.

Time-series **data blocks** (`.tdat` RED blocks) are written **unencrypted**
even in an encrypted session — meflib's `RED_ENCRYPTION_LEVEL_DEFAULT` is
`NO_ENCRYPTION`. Only metadata sections 2/3 and record bodies are encrypted.

## Why "level-1 only" is not a thing

pymef's writer unconditionally assigns:

```
metadata->section_1->section_2_encryption = LEVEL_1_ENCRYPTION_DECRYPTED   // -1
metadata->section_1->section_3_encryption = LEVEL_2_ENCRYPTION_DECRYPTED   // -2
```

(see `pymef/mef_file/pymef3_file.c`, `_initialize_tmd2`). The negative
`*_DECRYPTED` values mean "this section *is* level-N material, currently held as
plaintext." At write time, meflib's `encrypt_metadata` flips a section's level
to positive **and** encrypts it *iff* the corresponding password is present:

- section 2 needs the **level-1** password,
- section 3 needs the **level-2** password.

So the outcomes are:

| password_1 | password_2 | section 2 on disk | section 3 on disk |
|:---:|:---:|:---:|:---:|
| —   | —   | plaintext (level `-1`) | plaintext (level `-2`) |
| set | set | encrypted (level `+1`) | encrypted (level `+2`) |
| set | —   | encrypted (level `+1`) | **left `-2`, plaintext, but marked level-2** ← broken |
| —   | set | (no level-1 pwd) — not a valid configuration | |

The third row is the trap: providing only `password_1` encrypts section 2 but
leaves section 3 marked as level-2-decrypted plaintext. When the legacy writer
later re-opens the session to append/reload, it reads section 2 back **without
the password it just used** (the writer reopens with `password_2=None`), gets
garbage for `sampling_frequency`, and dies with an `OverflowError` deep in
`get_mefblock_len`. It is not a supported state; it just happens not to be
rejected up front.

## What mef3io does

- **Read:** a section is decrypted only when its stored level is strictly
  positive. Negative (`_DECRYPTED`) or zero levels are treated as plaintext.
  This is why unencrypted files (which carry `-1`/`-2`, i.e. `0xFF`/`0xFE`
  bytes) are read correctly — a subtlety that trips up some third-party readers
  that treat the level byte as unsigned.
- **Write:** encryption is all-or-nothing per the presence of *both* passwords.
  `SessionWriter(password1=..., password2=...)` produces a fully-encrypted file
  (section 2 → L1 key, section 3 → L2 key); omitting both produces a plaintext
  file. Supplying only one password is not offered as a "partial" mode.

## Access levels are still meaningful on read

The two-level scheme still matters for *who can read what*:

- Opening with the **level-2** password grants full access (sections 2 and 3).
- Opening with the **level-1** password grants access to section 2 only;
  section 3 (subject-identifying metadata) stays encrypted. mef3io surfaces this
  as `access_level == 1` and `section3_available == False`, rather than
  throwing — the signal metadata is still readable, the identifying metadata is
  not.

This lets a data-sharing workflow hand out the level-1 password for signal
access while withholding subject identity behind the level-2 password.

## Password/key derivation (for completeness)

- Password bytes = terminal byte of each UTF-8 character, up to 16, zero-padded.
- Level-1 validation field = `SHA256(L1_bytes)[:16]`.
- Level-2 validation field = `SHA256(L2_bytes)[:16] XOR L1_bytes`.
- Verifying a password: try it as L1 (does its SHA-256 prefix match the L1
  field?); else derive a putative L1 key = `SHA256(pwd)[:16] XOR L2_field` and
  check *its* SHA-256 prefix against the L1 field — a match means the password
  is the L2 password and yields both keys.
- Encryption is AES-128-ECB over the (16-byte-aligned) section bytes.

See `core/src/{metadata,password,writer,records}.cpp` for the implementation and
`tests/test_p1_headers.py` / `tests/test_p4_write.py` for the round-trip and
access-level tests.
