# Unreleased

Changes merged since the last release. Rewritten into the next version's
notes at release time.

## Changed

- `recover_session` and `repair_session` now ALWAYS keep their backup
  (`<session>.recover-backup/` / `<session>.repair-backup/`): mef3io never
  changes or deletes stored data without keeping a copy. `backup=False`,
  `--no-backup`, the C++ `backup` flags, the C ABI `backup == 0` and MATLAB
  `Backup=false` are still accepted (no signature break) but ignored; Python
  emits one `BackupAlwaysKeptWarning` and MATLAB warns
  `mef3io:backupAlwaysKept`. Recovery's dataless segments (and a channel
  folder left empty) are moved into the backup, never deleted.
  C++ and C ABI callers get no warning (the flag is ignored silently, as
  documented in the headers). A backup is never replaced by a later one of
  the same name: recovery picks a free name (`<name>.1`, `.2`, ...).
