---
description: "How MEF 3.0 encryption works and what it protects: the two password levels, metadata and data-block encryption, compatibility with meflib and pymef."
---

# MEF 3.0 Encryption Model (as implemented by mef3io)

This note explains, once and for record, how encryption actually works in the
MEF 3.0 ecosystem — because the common mental model ("you can independently
choose level-1 or level-2 encryption") does **not** match what meflib/pymef
produce on disk. mef3io follows the real behavior, and this document is the
reference for why.

## What encryption protects

*In plain words, for anyone deciding whether MEF passwords are enough to
protect a recording.*

**MEF 3.0 encryption protects the subject fields (name, ID, recording
location) and the text of annotations. It does not protect the signal, or
when it was recorded (unless the session uses a recording-time offset), or
the names of the session and its channels.**
Someone who has the files but no password can still decode every stored
sample and see when each piece of the recording starts. Do not put
identifying information (a patient's name or record number) in session,
channel or folder names: they are never encrypted.

What is encrypted in an encrypted session, and what is not:

| What | Where it is stored | Encrypted? |
|---|---|---|
| Subject name, subject ID, recording location | metadata section 3 (`.tmet`) | **yes**, level-2 password |
| Recording-time offset, GMT offset, daylight-saving start/end | metadata section 3 | **yes**, level-2 password |
| Sampling rate, conversion factor to physical units (e.g. µV), units, filter settings, channel/session/reference descriptions, sample and block totals (this copy only: the index and data blocks below keep their own, in plain text) | metadata section 2 (`.tmet`) | **yes**, level-1 password |
| Annotation contents: note text, seizure and event details | record bodies (`.rdat`) | **yes**, level-2 password, when the writer encrypts them: mef3io always does (and refuses to add annotations to a password-protected session unless both passwords are given, or to an unprotected one when passwords are given, so a record file always matches its session; the legacy stack's unencrypted bodies under a protected header are reported by the validator as a `records.encryption` warning), the legacy Python stack (`mef_tools`/`pymef`) does not |
| Annotation type, time and size (the size of its contents, to within 16 bytes) | record headers (`.rdat`) and record index (`.ridx`) | no |
| Recording start and end time, session and channel names, anonymized name (if the writer filled it), number of blocks or records, file IDs, mef3io's [provenance stamp](mef3_format.md#provenance-region-mef3io-12-frozen-format) | the 1024-byte header at the start of **every** file | no |
| Every data block's start time, sample count, position in the file, smallest and largest sample, and gap flag | block index (`.tidx`) | no |
| Every data block's start time, sample count, scale factor and compression table | data block header (`.tdat`) | no ([with one exception](#encrypted-data-blocks)) |
| **The samples themselves** (compressed) | data block (`.tdat`) | **no**: meflib writes data blocks unencrypted by default, and so does mef3io. That is not the design of MEF 3.0 but an artifact of past implementations: the format defines encrypted data blocks |
| File and folder names, file sizes | the file system | no |

So, **without any password**, anyone with the files can recover:

- **The stored samples of every channel, exactly.** Decoding a data block
  needs nothing from the encrypted sections. What is missing is the
  conversion factor, so the values come out in the recorder's integer
  units rather than microvolts: the waveform, its spectrum and every event
  in it are intact; only the scale (and its sign) and the units are
  unknown.
- **The timing.** The start time and sample count of every block, and so
  the gaps and the sampling rate (samples per block divided by the time
  between blocks). In a session without a recording-time offset, which
  includes every session mef3io creates, these are real dates and times.
  Only a session that uses meflib's recording-time offset shows
  times of day instead, with the date in encrypted section 3; mef3io never
  creates such sessions (see [De-identification](anonymization.md)).
- **When and what kind of annotation** was made, but not its text.

**mef3io refusing to open an encrypted session without a password is a
choice of this library, not protection.** The bytes it would need to decode
the signal are not encrypted, and other code can read them directly.

**If the signal itself must stay confidential, protect the storage**:
encrypted disks or volumes, file permissions, access-controlled servers and
encrypted transfer. Use MEF passwords on top of that, for what they do
protect (subject identity and annotation text), not instead of it. To share
a recording without its identity and dates, see
[De-identification](anonymization.md).

Also worth knowing:

- **The level-2 password also unlocks level 1.** Whoever has it can read
  everything. To let someone see the technical metadata (conversion factor
  and units, filter settings, descriptions) but not the subject fields or
  annotation text, give them only the level-1 password.
- **Passwords are short and can be guessed offline.** At most 16
  characters count, and only one byte of each. Every file stores check
  values derived from an unsalted SHA-256 hash of each password in plain
  text, so anyone with the files can test guesses as fast as their computer
  allows. A short or common password protects little. For compatibility,
  use at most 15 characters, and at most 15 bytes once written as UTF-8
  (meflib refuses longer passwords, pymef copies the password into a
  16-byte buffer, and mef3io ignores everything past the 16th character).
  Letters such as é, ü or ÿ (up to U+00FF) work the same in mef3io, meflib
  and pymef, but take two of those 15 bytes each. Avoid characters beyond
  U+00FF (Ł, €, any non-Latin script): every MEF reader keeps only the low
  byte of each one, so a different password can unlock the same session.
  mef3io's writer warns (`PasswordInteropWarning`) about either case;
  `mef3io.password_interop_issue(password)` checks a password before
  anything is written.
- **In an unencrypted session nothing is hidden**, including section 3. A
  recording-time offset there hides no date: it is stored right next to the
  times it shifts ([details](anonymization.md)).

### Encrypted data blocks

MEF 3.0 does let a writer mark individual data blocks as encrypted, but this
is not a way to protect the signal. meflib then encrypts only the first 16
bytes of the block's 256-byte compression table; the samples' compressed
bytes, the block's start time and sample count, and its index entry
(including its smallest and largest sample) all stay in plain text (see the
[data block section of the format reference](mef3_format.md#data-file-tdat-1024-b-uh-concatenated-red-blocks)).
How much of the signal that hides has not been established, and the legacy
stack barely supports it: pymef crashes on an encrypted block, and meflib
needs the level-2 password even for blocks encrypted at level 1. mef3io
reads such blocks, but never writes them.

A format extension that would also hide the signal and its timing without the
password is being explored in
[#92](https://github.com/bnelair/mef3io/issues/92). It is an exploration, not a
plan: MEF 3.0 as described here is what mef3io reads and writes.

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
`NO_ENCRYPTION`. That is not the design of MEF 3.0 (which defines encrypted
data blocks) but an artifact of past implementations. Only metadata sections 2/3 and record bodies are encrypted,
so the samples and their timing decode without a password (see
[What encryption protects](#what-encryption-protects)).
A block that another writer DID encrypt has only the first 16 bytes of its
statistics table enciphered; see the RED block section of
[mef3_format.md](mef3_format.md).

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

This lets a data-sharing workflow hand out the level-1 password for the
technical metadata (conversion factor, units, filter settings) while withholding
subject identity behind the level-2 password. The level-1 password does not
gate the signal itself: the samples are readable without any password (see
[What encryption protects](#what-encryption-protects)).

!!! note "Section 3 is plain text in an unencrypted session"
    Without a level-2 password, section 3 is stored unencrypted (level `-2`).
    It holds the subject fields **and** `recording_time_offset`, which in a
    session "anonymized" by meflib's recording-time offset is the recording
    date. Anyone can read it from the bytes, and with the level-2 password
    anyone can read it from an encrypted session too. See
    [De-identification and the recording-time offset](anonymization.md).

## Compatibility with other MEF 3.0 tools

- **Encrypted metadata** (sections 2 and 3, record bodies) interoperates in
  both directions: pymef and mef_tools read encrypted sessions mef3io writes,
  and mef3io reads theirs at either password level. (With only the level-1
  password the legacy reader itself returns no samples, whoever wrote the
  file; see the [legacy comparison](legacy_comparison.md).)
- **Encrypted data blocks.** Some recordings have encrypted data blocks
  (written by some third-party tools; meflib can write them, though not by
  default). mef3io reads them: a block encrypted at level 1 with either
  password, one at level 2 with the level-2 password. pymef and meflib
  handle them poorly (see [Encrypted data blocks](#encrypted-data-blocks)).
  Recordings mef3io writes leave data blocks unencrypted and stay fully
  compatible. mef3io 1.2 and earlier decoded such blocks wrongly (they
  deciphered the whole 256-byte table instead of its first 16 bytes).
- **Unencrypted data blocks are not the design of MEF 3.0 but an artifact
  of past implementations.** The format
  defines data-block encryption; blocks are left unencrypted because of past
  implementations' defaults (meflib's `RED_ENCRYPTION_LEVEL_DEFAULT` is no
  encryption) and because the legacy readers mishandle encrypted blocks.
  mef3io intends to align with the specification later, which may break
  compatibility with older tools for encrypted data. That is not decided yet:
  see [#94](https://github.com/bnelair/mef3io/issues/94) and, for a
  different direction, [#92](https://github.com/bnelair/mef3io/issues/92).

## Password/key derivation (for completeness)

- Password bytes, zero-padded to 16: **the low byte of each character's
  Unicode code point** (meflib `extract_terminal_password_bytes`,
  meflib.c:2439, decoding with `UTF8_nextchar`, meflib.c:8855). So `é`
  (U+00E9, UTF-8 `C3 A9`) is `0xE9`. meflib accepts at most 15 characters
  (`check_password`, meflib.c:1056; meflib.h:260); pymef passes the
  password to meflib as UTF-8 through a 16-byte buffer (`strcpy` into
  `password_arr[PASSWORD_BYTES]`, pymef3_file.c), so for pymef and
  `mef_tools` the limit is 15 UTF-8 **bytes**. mef3io derives exactly
  meflib's bytes (it reproduces `UTF8_nextchar` byte for byte, malformed
  input included), keeps reading up to 16 characters, ignores the rest, and
  stops at the first NUL.
- **Characters above U+00FF collide.** Only the low byte survives, so `Ł`
  (U+0141) and `A` (U+0041) give the same byte, and `Ā` (U+0100) gives
  `0x00`, the same as the padding. mef3io matches this exactly, because a
  different rule would make such passwords unusable in meflib and pymef, and
  warns when writing with one.
- **Sessions written by mef3io 1.2.0 and earlier.** Those versions kept the
  *last UTF-8 byte* of each character instead (`é` became `0xA9`),
  which agrees with meflib on ASCII and on nothing else
  ([#84](https://github.com/bnelair/mef3io/issues/84)). To keep those
  sessions readable, mef3io verifies a password with meflib's rule first
  and, only if that fails and the password has a non-ASCII character (so
  the old rule gives different bytes), with the old rule. A match through
  the old rule opens the session and raises `LegacyPasswordWarning`
  (a `MefWarning`; MATLAB: `mef3io:legacyPassword`; C:
  `mef3io_reader_n_legacy_password_channels`, and from `records()` for
  record files; `inspect()` reports it as
  `SessionSummary.legacy_password_mapping` and a `legacy-password` entry in
  its `warnings`): the data is intact, but
  meflib and pymef cannot open that session with that password. The
  fallback cannot tell who wrote a file, so it also accepts a second
  password, the one whose old-rule bytes equal the real key (a session
  keyed with `b©` also opens with `bé`); that is inherent to any such
  fallback, and the warning announces every use of it. A re-keyed copy makes it
  readable everywhere: `mef3io.anonymize_session(...,
  output_password=(level1, level2))` re-encrypts every file with meflib's
  mapping. It is not a pure re-key: the copy is de-identified (and, with a
  shift, every time is moved by at least the recording's span + 1 day)
  ([De-identification](anonymization.md)). A pure re-key is `transform_session`
  ([Modified copies](transform.md)). New files are
  never written with the old rule. Appending to an old segment keeps that
  segment's keys (changing them would mean re-encrypting it) and warns; a
  new segment uses meflib's rule.
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
