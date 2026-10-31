# Unreleased

Changes merged since the last release. Rewritten into the next version's
notes at release time.

## Fixed

- **Annotations in a password-protected session were stored unencrypted when
  the Writer had no level-1 password (#167).** A `Writer` opened on an
  encrypted session with `password1` empty and `password2` given created a new
  record file (`.rdat`) at a level that had none yet, with the note text in
  plaintext. The session now decides: every annotation write to a
  password-protected session needs BOTH correct passwords, otherwise it
  raises `PasswordError` before any file or folder is created. Output for the
  correct pair is byte-identical to before.
- **New validator check `records.encryption` (error, detect only).** Flags a
  record file whose protection differs from the session's (for example the
  plaintext file above). It is not repairable because fixing it means
  re-encrypting record bodies; copy the annotations out and write them again
  with both passwords.
