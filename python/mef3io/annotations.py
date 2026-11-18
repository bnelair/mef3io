"""Remove (and so change) stored annotations.

:meth:`Writer.write_annotations` only ever **appends**. :func:`remove_annotations`
is the one other way annotations change: it takes records out of a session's
annotation files by their *content* and, in the same call, may add others, so
"change a note" is "remove the wrong one and add the corrected one" in a
single crash-safe rewrite.

A removed annotation is **gone**. There is no undo and no history: the old
files are kept only until the new ones are in place.
"""
from __future__ import annotations

import os
from dataclasses import dataclass
from typing import Iterable, Mapping, Optional, Sequence, Tuple

from ._cancel import CancelToken, ProgressCallback, _hooks

__all__ = ["RecordFileEdit", "RemoveAnnotationsReport", "remove_annotations"]


def _backend():
    from . import _mef3io

    return _mef3io


@dataclass(frozen=True)
class RecordFileEdit:
    """One annotation file pair (``.rdat`` + ``.ridx``) the call rewrote.

    Attributes
    ----------
    level : {"session", "channel", "segment"}
    channel : str or None
    segment : int or None
    file : str
        Session-relative path of the ``.rdat``.
    records_before, removed, added, records_after : int
    pair_removed : bool
        The last record went, so the pair was deleted (a later
        :meth:`Writer.write_annotations` creates a fresh one).
    """

    level: str
    channel: Optional[str]
    segment: Optional[int]
    file: str
    records_before: int
    removed: int
    added: int
    records_after: int
    pair_removed: bool


@dataclass(frozen=True)
class RemoveAnnotationsReport:
    """What :func:`remove_annotations` did (or, with ``dry_run``, would do).

    Attributes
    ----------
    files : tuple of RecordFileEdit
    removed, added : int
        Totals over all files.
    dry_run : bool
        Nothing was written.
    resumed : str
        The id of an earlier edit this session was interrupted in, which was
        finished (or rolled back) before this call's own work; ``""`` if none.
    notes : tuple of str
    """

    files: Tuple[RecordFileEdit, ...] = ()
    removed: int = 0
    added: int = 0
    dry_run: bool = False
    resumed: str = ""
    notes: Tuple[str, ...] = ()


def remove_annotations(
    path,
    remove: Iterable[Mapping] = (),
    add: Iterable[Mapping] = (),
    *,
    password1: str = "",
    password2: str = "",
    all_copies: bool = False,
    dry_run: bool = False,
    cancel: Optional[CancelToken] = None,
    progress: Optional[ProgressCallback] = None,
    _crash_at: int = -1,
    _fail_at: int = -1,
) -> RemoveAnnotationsReport:
    """Remove stored annotations by content, and optionally add others.

    This is **not** :meth:`Writer.write_annotations`, which only appends and
    never replaces or deletes. Removal is a separate, explicit call that
    **rewrites the annotation files** (``.rdat`` + ``.ridx``) **of the levels
    involved** and nothing else: signal files and metadata are never opened
    for writing. Every record that stays is carried over byte for byte; the
    files' headers are made right (count, span, CRCs, provenance
    ``records-edit``). **A removed annotation cannot be recovered.**

    Parameters
    ----------
    path : str or path-like
        A ``.mefd`` session folder. A ``.mefd.tar`` archive is refused
        (``tar-read-only``).
    remove : iterable of dict
        The records to remove, named by content exactly as
        :meth:`Reader.records` returns them: ``level``, ``channel`` /
        ``segment`` where they apply, ``time``, ``type`` and **every body
        field** of the type (e.g. ``text`` for a ``Note``; ``duration`` and
        ``text`` for ``EDFA``). A record dict read back from ``records()``
        works as is. A selector that leaves out a field the stored record has
        is refused (``selector-incomplete``), so it cannot hit a record it
        was not meant for. Two optional keys control the count:

        * ``expect`` (default ``1``): exactly this many stored records must
          match. Identical duplicates (the same note stored twice) are the
          ordinary reason for more than one: say ``expect=2`` or
          ``all_copies=True``.
        * ``all_copies`` (default: the call's ``all_copies``): remove every
          copy, however many (at least one must exist).

        A selector that matches nothing, fewer or more than expected is a
        refusal and **nothing is removed**: the call is all or nothing.
    add : iterable of dict, optional
        Records to add in the same rewrite, like
        :meth:`Writer.write_annotations` takes them (``time``; ``type``
        defaulting to ``"Note"``; ``text``; ``duration``), plus optional
        ``channel`` / ``segment`` for the level (default: session level).
        The level's annotation file must already exist (use
        :meth:`Writer.write_annotations` to create it). Added records are
        stored after the kept ones, in time order, encrypted like every
        record of a protected session.
    password1, password2 : str, optional
        The session's passwords, both or neither, as for :class:`Writer`.
        The session decides whether they are needed.
    all_copies : bool, default False
        Default for selectors that give no ``all_copies`` of their own.
    dry_run : bool, default False
        Select, build and verify everything in memory and report; write
        nothing.
    cancel : CancelToken, optional
        Stops the call **before** the new files are in place, leaving the
        session as it was (:class:`Cancelled`). After that point a cancel is
        ignored: the swap is a few renames and is always completed.
    progress : callable, optional
        ``progress(done, total)`` in bytes of annotation files read, written
        and verified.

    Returns
    -------
    RemoveAnnotationsReport

    Raises
    ------
    ValueError
        A selector that cannot be used or does not match: ``code``
        ``bad-selector``, ``selector-unknown-field``, ``selector-incomplete``,
        ``no-match``, ``ambiguous`` (more copies than ``expect``),
        ``count-mismatch`` (fewer), ``selectors-overlap``, ``nothing-to-do``,
        ``unknown-channel``, ``unknown-segment``, ``add-level-missing``.
    PasswordError
        Wrong or missing passwords, or a record that stays locked
        (``password``, ``record-locked``).
    FormatError, CrcError
        An annotation file that cannot be read in full (``record-damaged``,
        ``record-crc``, ``record-index-mismatch``, ``record-index-missing``,
        ``ambiguous-session-records``): nothing is touched; run
        :func:`recover_session` or :func:`validate_session` first.
    WriteConflictError
        The session is being written or mid-operation (``being-written``,
        ``mid-operation``; ``edit-interrupted`` for a ``dry_run`` on a session an edit was
        interrupted in), the annotations changed while the call worked
        (``records-changed``), a time cannot be stored (``add-time``), an
        unknown or disputed recording-time offset (``offset-unknown``), or
        records that do not match the session's encryption.
    IoError
        A tar archive (``tar-read-only``), a path that is not a ``.mefd``
        folder, or a failure while swapping the files (``edit-failed``; the
        old files are put back). Every refusal exception has a stable
        ``.code``.
    Cancelled
        ``cancel`` was set in time.

    Notes
    -----
    **Crash safety.** The call checks and selects everything in memory, writes
    the new files beside the old ones, flushes and reads them back, then swaps
    them in under the session's operation marker. After a crash or power cut
    at any point the session is either as it was, or carries that marker: the
    open then warns (:class:`SessionOperationWarning`), writes are refused,
    :func:`validate_session` reports ``records.interrupted-edit``, and
    ``recover_session(path, apply=True)`` finishes the edit (or rolls it back
    when a new file is not intact). The next ``remove_annotations`` does the
    same first (``report.resumed``).

    **Concurrency.** There is no cross-process write lock: stop other
    writers of the session first. A session whose files show a writer at work
    is refused, and the annotation files are re-checked after the marker is
    down (``records-changed``). On Windows a record file another program holds
    open prevents the swap; the call then puts the old files back and raises
    ``edit-failed``.

    Python and C++ only (no MATLAB).

    Examples
    --------
    >>> notes = mef3io.Reader(path).records()
    >>> wrong = [n for n in notes if n["type"] == "Note" and n["text"] == "typo"]
    >>> mef3io.remove_annotations(path, remove=wrong,
    ...                           add=[{"time": wrong[0]["time"], "text": "fixed"}])
    """
    for name, value in (("remove", remove), ("add", add)):
        if isinstance(value, (Mapping, str, bytes)):
            raise TypeError(f"{name} must be a list of dicts (one per record), not a single "
                            f"{type(value).__name__}: wrap it in a list")
    remove = [dict(r) for r in remove]
    for r in remove:
        if all_copies and "all_copies" not in r:
            r["all_copies"] = True
    add = [dict(a) for a in add]
    raw = _backend().remove_annotations(
        os.fspath(path), remove, add, password1 or "", password2 or "", bool(dry_run),
        _crash_at, _fail_at, **_hooks(cancel, progress)
    )
    return RemoveAnnotationsReport(
        files=tuple(RecordFileEdit(**f) for f in raw["files"]),
        removed=raw["removed"],
        added=raw["added"],
        dry_run=raw["dry_run"],
        resumed=raw["resumed"],
        notes=tuple(raw["notes"]),
    )
