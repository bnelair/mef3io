# Unreleased

*Changes merged since the last release. They ship with the next version.*

## Highlights

- **De-identified copies.** `mef3io.anonymize_session(src, dst, shift_us)`
  writes a copy of a session with every time moved by one constant and the
  patient's identity removed; the original is never changed. Times move
  everywhere they are stored, including the times inside seizure, epoch and
  cursor annotations. The time-zone fields are set to fixed values
  (recording-time offset 0, GMT offset 0, no daylight-saving dates). Subject
  names, ID and location, the session name, descriptions, annotation text
  (drop, keep with names removed, or your own function), file identifiers and
  other programs' private header areas are removed or replaced (keeping
  annotation text removes only the names mef3io knows, not dates or other
  names written in it). Channels can
  be left out (`exclude_channels=` or `channels=`), and nothing of a left-out
  channel stays in the copy. Samples are copied without decoding, so they are
  bit-identical. The copy is checked by `mef3io.anonymize.verify` before it is
  published; a failure or a cancel leaves nothing behind. Sessions using
  meflib's deprecated recording-time offset are accepted when their offset is
  unambiguous. Encrypted data blocks stay encrypted under the copy's new
  passwords, or are decrypted only when the copy has none. From a terminal:
  `python -m mef3io anonymize SRC DST --shift-us N` or
  `--reference-date YYYY-MM-DD [--tz ZONE]`. Python and C++ only
  ([De-identification](https://bnelair.github.io/mef3io/anonymization/),
  [#70](https://github.com/bnelair/mef3io/issues/70)).
- **Provenance operation 6, `anonymize`**, is now assigned: every file of a
  de-identified copy carries it.
