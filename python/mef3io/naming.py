"""Finding and naming sessions in bulk (#72).

* :func:`time_range` — a session's first and last sample time, fast enough to
  order hundreds of sessions on a network drive: only the first and last
  segment's metadata of each channel is read, never an index or data file.
* :func:`name_drift` — where a session's name is stored (its folder, its
  session-level annotation file, every file's header) and whether those agree.
* :func:`rename_archive` — rename a ``.mefd.tar`` file, never replacing one.

Renaming a directory session in every place its name lives
(``rename_session``) follows in a later release.
"""
from __future__ import annotations

import os
from dataclasses import dataclass
from typing import Dict, Optional, Tuple

from ._cancel import CancelToken, _hooks

__all__ = ["ArchiveRename", "NameDrift", "TimeRange", "name_drift", "rename_archive",
           "time_range"]


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
