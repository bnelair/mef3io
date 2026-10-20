"""Finding and naming sessions in bulk (#72).

* :func:`time_range` — a session's first and last sample time, fast enough to
  order hundreds of sessions on a network drive: only the first and last
  segment's metadata of each channel is read, never an index or data file.
* :func:`name_drift` — where a session's name is stored (its folder, its
  session-level annotation file, every file's header) and whether those agree.
* :func:`rename_archive` — rename a ``.mefd.tar`` file, never replacing one.
* :func:`rename_session` — rename a ``.mefd`` folder session in every place
  its name lives, as one operation that a crash cannot leave half-done.
* :func:`fix_name_drift` — after another tool renamed only the folder, bring
  the stored names back in line with it.
"""
from __future__ import annotations

import os
from dataclasses import dataclass
from typing import Dict, List, Optional, Sequence, Tuple

from ._cancel import CancelToken, _hooks

__all__ = ["ArchiveRename", "NameDrift", "RenameReport", "TimeRange", "clear_operation_marker",
           "fix_name_drift", "name_drift", "operation_marker", "rename_archive",
           "rename_session", "time_range"]


def _backend():
    from . import _mef3io

    return _mef3io


@dataclass(frozen=True)
class TimeRange:
    """A session's first and last sample time.

    Attributes
    ----------
    start_uutc, end_uutc : int or None
        uUTC; ``None`` when no channel holds samples. The end is the end of
        the last block, as the files' headers state it.
    offset_known : bool
        ``False`` when the times cannot be shown as absolute: the session is
        encrypted and the password does not open the part holding the
        recording-time offset, or it uses that offset with times stored in
        the ambiguous form. Order such sessions with care.
    channels, files_read : int
        Channels with data looked at, and metadata files read (two per
        channel at most).
    """

    start_uutc: Optional[int]
    end_uutc: Optional[int]
    offset_known: bool
    channels: int
    files_read: int


def time_range(path, password: str = "", *, cancel: Optional[CancelToken] = None) -> TimeRange:
    """The first and last sample time of a session, reading as little as possible.

    Only the first and last segment's ``.tmet`` of each channel is read
    (about 16 KB each), never a block index or data file, so it suits
    ordering many sessions by time. Listing the session still checks that
    each segment's files exist. Segments are assumed numbered in time order,
    as every known writer numbers them: a middle segment earlier than the
    first and last is not seen. Works on ``.mefd`` folders and
    ``.mefd.tar`` archives. An encrypted session can be ordered without its
    password (``offset_known`` is then ``False``); a wrong password raises
    :class:`PasswordError`.
    """
    d = _backend().time_range(os.fspath(path), password or "", **_hooks(cancel, None))
    return TimeRange(**d)


@dataclass(frozen=True)
class NameDrift:
    """Where a session's name is stored, and whether the places agree.

    Attributes
    ----------
    folder : str
        The folder's (or archive's) name without ``.mefd`` / ``.mefd.tar``.
    record_files : tuple of str
        The names of the session-level annotation files (``<name>.rdat``);
        ``consistent`` also requires their ``.ridx`` to carry the same name.
    stored_names : dict of str to int
        Each distinct name stored in the files' headers, with how many
        files hold it.
    files_read : int
    consistent : bool
        All the places name the session the same. A folder renamed by
        another tool leaves this ``False``, and other MEF software then
        silently misses the session's annotations.
    """

    folder: str
    record_files: Tuple[str, ...]
    stored_names: Dict[str, int]
    files_read: int
    consistent: bool


def name_drift(path, *, cancel: Optional[CancelToken] = None) -> NameDrift:
    """Where a session's name is stored and whether those places agree. Writes nothing.

    Reads the 1 KB header of every file (and lists the folders).
    """
    d = _backend().name_drift(os.fspath(path), **_hooks(cancel, None))
    d["record_files"] = tuple(d["record_files"])
    return NameDrift(**d)


@dataclass(frozen=True)
class ArchiveRename:
    """Result of :func:`rename_archive`. ``inside_keeps_old_name`` is always
    ``True``: the archive's internal folder, member names and stored names
    still carry the old name (extract, rename and archive again to change
    them)."""

    old_path: str
    new_path: str
    inside_keeps_old_name: bool


def rename_archive(path, new_name: str) -> ArchiveRename:
    """Rename a ``.mefd.tar`` archive to ``<new_name>.mefd.tar`` in the same folder.

    The rename never replaces an existing file: where the file system cannot
    guarantee that, it is refused rather than done unsafely. Also refused: a
    name over 246 bytes of UTF-8 (never truncated); a name that is not a
    portable file name (a folder, a control character, ``:*?"<>|``, a
    Windows device name such as ``CON``, a trailing dot or space); an archive
    being written (``.tmp`` or ``.partial`` beside it). A symlink is renamed
    itself, not the file it points to.
    Undo it with ``rename_archive(new_path, old_name)``.
    """
    return ArchiveRename(**_backend().rename_archive(os.fspath(path), new_name))


@dataclass(frozen=True)
class RenameReport:
    """Result of :func:`rename_session` or :func:`fix_name_drift`.

    Attributes
    ----------
    old_path, new_path : str
        The session folder before and after (equal for ``fix_name_drift``).
    old_name, new_name : str
        The stored session name before and after.
    folder_renamed : bool
    files_changed : tuple of str
        Session-relative paths of the files whose header changed.
    descriptions_changed : tuple of str
        Session-relative paths of the ``.tmet`` files whose
        ``session_description`` changed (it was equal to the old name).
        :meth:`undo` reverts exactly these.
    records_renamed : tuple of (str, str)
        Session-level annotation files renamed, as (old, new) file names.
    files : dict of str to (int, int)
        Each changed file's size and modification time (ns) right after the
        operation, so a caller can tell whether the session was written to
        since (an undo is still exact after an append).
    rolled_back : str
        The id of an earlier, interrupted operation that was rolled back
        first, or ``""``.
    journals_discarded : int
        Journals of operations that never started or had finished, removed.
    notes : tuple of str
        Things to know, e.g. a marker that could not be removed after the
        operation completed.
    operation : str
        ``"rename"`` or ``"fix-name-drift"``.
    """

    old_path: str
    new_path: str
    old_name: str
    new_name: str
    folder_renamed: bool
    files_changed: Tuple[str, ...]
    descriptions_changed: Tuple[str, ...]
    records_renamed: Tuple[Tuple[str, str], ...]
    files: Dict[str, Tuple[int, int]]
    rolled_back: str
    journals_discarded: int
    operation: str
    notes: Tuple[str, ...] = ()

    @property
    def changed(self) -> bool:
        """Whether anything was changed (a session already named so is left alone)."""
        return bool(self.files_changed or self.records_renamed or self.folder_renamed)

    def undo(self, *, password: str = "", backup_dir=None) -> "RenameReport":
        """Reverse this operation, logically: rename back to the old name and
        revert exactly the descriptions this operation changed.

        Never a byte restore, so samples, times and annotations written since
        (an append, say) are kept. It is a full operation with every check of
        the original (marker, journal, no-replace renames, the password of an
        encrypted session). For ``fix_name_drift`` the folder is left as it is.
        """
        if self.operation == "rename":
            return rename_session(self.new_path, self.old_name, password=password,
                                  backup_dir=backup_dir,
                                  descriptions=self.descriptions_changed)
        return fix_name_drift(self.new_path, self.old_name, password=password,
                              backup_dir=backup_dir, descriptions=self.descriptions_changed)


def _report(d: dict, operation: str) -> RenameReport:
    files = {}
    for rel in d["files_changed"]:
        st = os.stat(os.path.join(d["new_path"], rel))
        files[rel] = (st.st_size, st.st_mtime_ns)
    return RenameReport(
        old_path=d["old_path"], new_path=d["new_path"], old_name=d["old_name"],
        new_name=d["new_name"], folder_renamed=d["folder_renamed"],
        files_changed=tuple(d["files_changed"]),
        descriptions_changed=tuple(d["descriptions_changed"]),
        records_renamed=tuple(tuple(r) for r in d["records_renamed"]),
        files=files, rolled_back=d["rolled_back"],
        journals_discarded=d["journals_discarded"], operation=operation,
        notes=tuple(d["notes"]))


def _descriptions(descriptions: Optional[Sequence[str]]) -> Optional[List[str]]:
    if descriptions is None:
        return None
    if isinstance(descriptions, (str, bytes)):
        raise TypeError("descriptions must be a list of paths, not a single string")
    return [str(d) for d in descriptions]


def rename_session(path, new_name: str, *, password: str = "", backup_dir=None,
                   descriptions: Optional[Sequence[str]] = None,
                   _crash_at: int = -1, _fail_at: int = -1) -> RenameReport:
    """Rename a ``.mefd`` session in every place its name lives.

    A session's name is stored in four places, and other MEF software reads
    all of them: renaming only the folder makes it silently miss the
    session's annotations. This changes, together:

    1. the folder, ``<old>.mefd`` -> ``<new_name>.mefd``;
    2. the session's annotation files, ``<old>.rdat`` / ``.ridx``;
    3. the name stored in every file's header;
    4. ``session_description`` in each channel's metadata, only where it is
       exactly the old name (a description that merely mentions it is kept).

    No sample, block index or time changes, and every renamed file is
    stamped ``rename`` in its provenance region.

    **A crash cannot leave it half-done.** The old headers and a journal of
    the planned steps go to ``backup_dir`` first, then a marker into the
    session; mef3io refuses to write to a marked session. If the operation is
    interrupted, the next ``rename_session`` / ``fix_name_drift`` call on the
    session (with the same ``backup_dir``) rolls it back first. The journal
    and the backups are deleted when the operation completes.

    Parameters
    ----------
    path : str or PathLike
        The ``.mefd`` folder.
    new_name : str
        Without ``.mefd``. At most 250 bytes of UTF-8 and a portable file
        name (no ``/\\:*?"<>|``, control characters, Windows device names,
        or trailing dot or space). Never truncated.
    password : str
        Needed when the session is encrypted: the descriptions are in the
        level-1 encrypted part.
    backup_dir : str or PathLike, optional
        Where the journal and old headers are kept while it runs. Default:
        ``<session>.repair-backup`` beside the session. The old name is
        identifying, so an application passes a folder of its own.
    descriptions : list of str, optional
        Only change the descriptions in these ``.tmet`` files (session
        relative paths). Used by :meth:`RenameReport.undo`.

    Raises
    ------
    ValueError
        A name that is not allowed, the same name, or a ``.mefd.tar`` (use
        :func:`rename_archive`).
    IoError
        The target exists, a header is damaged (validate and repair first),
        or the file system cannot rename without the risk of replacing a file.
    WriteConflictError
        The session is being written, carries the marker of an operation
        whose journal is not in ``backup_dir``, or its name is already out of
        step (run :func:`fix_name_drift` first).
    PasswordError
        The session is encrypted and ``password`` does not open its metadata.

    Notes
    -----
    Undo with ``report.undo()``, which renames back and reverts only the
    descriptions this call changed. On a macOS HFS+ volume (which stores
    names decomposed) a long accented name can be refused by the file
    system at the last step; the operation is then rolled back.
    """
    d = _backend().rename_session(os.fspath(path), new_name, password or "",
                                  os.fspath(backup_dir) if backup_dir else "",
                                  _descriptions(descriptions), _crash_at, _fail_at)
    return _report(d, "rename")


def fix_name_drift(path, target: Optional[str] = None, *, password: str = "", backup_dir=None,
                   descriptions: Optional[Sequence[str]] = None,
                   _crash_at: int = -1, _fail_at: int = -1) -> RenameReport:
    """Bring a session's stored names in line with its folder.

    For a session whose folder another tool renamed (:func:`name_drift`
    shows it). Sets the name in every file's header, the session's
    annotation files and the descriptions equal to the old name to
    ``target`` (default: the folder's name). The folder itself is never
    renamed. Same safety as :func:`rename_session` (journal, marker,
    rollback, no-replace renames, provenance ``rename``).

    Refused when the old name is ambiguous: the files store different names,
    or there are several session-level annotation files. Undo with
    ``report.undo()``.
    """
    d = _backend().fix_name_drift(os.fspath(path), target or "", password or "",
                                  os.fspath(backup_dir) if backup_dir else "",
                                  _descriptions(descriptions), _crash_at, _fail_at)
    return _report(d, "fix-name-drift")


def operation_marker(path) -> Optional[Dict[str, str]]:
    """The marker of an unfinished in-place operation, or ``None``.

    A dict with ``tool`` (e.g. ``"mef3io 1.5.0"``), ``operation``
    (``"rename"``, ``"fix-name-drift"``) and ``id``. It names no session,
    path or person.
    """
    return _backend().operation_marker(os.fspath(path))


def clear_operation_marker(path) -> None:
    """Remove the marker of an operation whose journal is lost.

    Only for a session whose rename was interrupted and whose backup folder
    is gone (another machine, a deleted folder). Refused unless
    :func:`name_drift` shows the name consistent, so a session the operation
    left half-changed keeps its marker. When the journal exists, run the
    operation again instead: it rolls back.
    """
    _backend().clear_operation_marker(os.fspath(path))
