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
  raises `PasswordError` before any file or folder is created. A channel is
  judged by its own metadata, the session level by "any channel protected"; an
  unprotected session refuses passwords for annotations; a brand-new session
  takes both passwords or none. The check is made once per Writer. Output for
  the correct pair is unchanged.
- **New validator check `records.encryption` (error, detect only).** Flags a
  record file whose protection differs from its session's (error; for example the
  plaintext file above) and a protected header over unencrypted bodies, as the
  legacy `mef_tools` writes (warning). Channel and session level files have
  `segment == -1`. It is not repairable because fixing it means
  re-encrypting record bodies; copy the annotations out and write them again
  with both passwords.
