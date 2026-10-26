# Unreleased

Changes merged since the last release. Rewritten into the next version's
notes at release time.

### Changed — check before upgrading

- **mef3io never deletes a session** (maintainer rule: write paths append, or
  create what is absent). Removed, in every language:
  - `overwrite` from `Writer`, `MefWriter`, C++ `SessionWriter`, MATLAB
    `Writer(Overwrite=)`, `archive_session` and `extract_session` (C++, Python,
    MATLAB). Opening a `Writer` on an existing session always appends to it
    (as `overwrite=False` did); an existing archive or extraction target is
    refused. Passing `overwrite` raises `TypeError` in Python and an error in
    MATLAB: "mef3io never deletes a session; remove or rename the folder
    yourself, or write to a new path". In Python `MefWriter(path, True)` (the
    old second positional argument) is refused too instead of shifting the
    passwords.
  - `backup` from `recover_session` (and `--no-backup` of
    `python -m mef3io recover`), and `backup` / `--no-backup` from
    `repair_session` / `python -m mef3io repair`. Both always back up what they
    change or drop. MATLAB `recoverSession(Backup=)` is removed too.
  - The C ABI keeps its signatures: `mef3io_writer_open`,
    `mef3io_archive_session[_ex]` and `mef3io_extract_session[_ex]` return
    `MEF3IO_ERR_ARGUMENT` for a non-zero `overwrite`, and
    `mef3io_recover_session[_ex]` for `backup == 0`.
  - **Migration:** delete or rename the folder yourself (`shutil.rmtree`,
    `rmdir(p, 's')`) before writing a session you want to start from scratch,
    or write to a new path. Recovery and repair always keep backups.
