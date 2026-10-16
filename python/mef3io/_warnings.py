"""Warnings mef3io emits: one base class, and one way to emit them.

Every warning carries the session it is about as DATA (``w.message.path``),
and its text never contains the session's LOCATION (the directory or archive
path) or its NAME: both are replaced by ``<session>`` before the warning is
built. That is all that is scrubbed. Channel names and the names of files
inside the session (``<session>/ch1.timd/ch1-000001.segd/...``) can appear,
and those can identify someone too.

Python's default filter shows a warning once per (text, category, line). The
text used to start with the session path, which made that "once per session";
:func:`emit` keeps it so by giving each session (and calling file) its own
warning registry.
"""
from __future__ import annotations

import os
import re
import sys
import warnings
from typing import Any, Iterable, Optional


class MefWarning(UserWarning):
    """Base class of every warning mef3io emits.

    The message never contains the session's location (its directory or
    archive path) or its name: they appear as ``<session>``, also inside an
    archive (``<session>::<session>/...``) and in the session's own file
    names (``<session>.rdat``). Nothing else is scrubbed: **channel names and
    the names of files inside the session can appear**, and those can be
    identifying too. A caller with a stricter rule should log only the class
    name, or nothing.

    What the message summarises is on the warning object, which Python passes
    as ``w.message`` (``warnings.catch_warnings(record=True)``,
    ``pytest.warns``) or raises under ``-W error``:

    Attributes
    ----------
    path : str or None
        The session (or, for a writer, the session being written) the warning
        is about, exactly as it was passed in. ``None`` when there is none.
    details : tuple
        The structured items behind the message: the problem dicts of an
        unreadable segment, the block-copy mismatches, the unset declarations,
        and so on (each subclass says which). These MAY contain paths.

    Python's ``"default"`` and ``"module"`` filter actions show a given
    warning once per session (per calling file), not once per process: the
    path is no longer in the text, so each session keeps its own registry.

    Filter every mef3io warning at once with::

        warnings.filterwarnings("ignore", category=mef3io.MefWarning)

    Examples
    --------
    >>> with warnings.catch_warnings(record=True) as caught:
    ...     warnings.simplefilter("always")
    ...     r = mef3io.Reader(path)
    >>> for w in caught:
    ...     if issubclass(w.category, mef3io.MefWarning):
    ...         log.warning("%s", w.message)        # no session path or name
    ...         where = w.message.path              # the path, as data
    """

    def __init__(self, message: Any = "", *, path: Optional[str] = None,
                 details: Iterable = ()):
        super().__init__(message)
        self.path = None if path is None else os.fspath(path)
        self.details = tuple(details)

    def __reduce__(self):  # keyword attributes survive pickling (multiprocessing)
        return (_rebuild, (type(self), str(self), self.path, self.details))


def _rebuild(cls, message, path, details):
    return cls(message, path=path, details=details)


_SESSION_SUFFIX = re.compile(r"\.mefd(\.tar)?$", re.IGNORECASE)
# The first component after "<archive>::" is the session folder INSIDE the
# archive, which keeps its original name when the archive file is renamed.
_ARCHIVE_ROOT = re.compile(r"<session>::[^/\\\s]+")  # one path component, never the prose after it


def _stem(name: str) -> str:
    return _SESSION_SUFFIX.sub("", os.path.basename(name.rstrip("/\\")))


def identity_of(impl) -> tuple:
    """The backend's own ``[location, name]`` of the session, or ``()``.

    For a renamed archive these differ from what the path says: the folder
    inside keeps the original (identifying) name.
    """
    fn = getattr(impl, "session_identity", None)
    try:
        return tuple(fn()) if fn is not None else ()
    except Exception:  # noqa: BLE001 - a scrub aid must never fail a warning
        return ()


def scrub(text: str, path: Optional[str], also: Iterable[str] = ()) -> str:
    """``text`` with the session's location and name replaced by ``<session>``.

    ``also`` adds the backend's view of them (:func:`identity_of`). Names are
    replaced wherever they stand as a whole word (bounded by anything that is
    not a letter, digit or underscore), so a short session name also replaces
    the same word elsewhere in the text: over-scrubbing is the safe side.
    """
    locations, names = set(), set()
    if path:
        raw = os.fspath(path).rstrip("/\\")
        if raw:
            locations |= {raw, os.path.abspath(raw), os.path.realpath(raw)}
            names.add(_stem(raw))
    for item in also:
        if item:
            item = os.fspath(item)
            if "::" in item:  # "<archive>::<root folder>"
                archive, _, root = item.partition("::")
                locations.add(archive)
                names.add(_stem(root))
            elif "/" in item or "\\" in item:  # a location
                locations.add(item.rstrip("/\\"))
                names.add(_stem(item))
            else:  # a bare name: whole words only, never a substring
                names.add(_stem(item))
                continue
            names.add(_stem(item.partition("::")[0]))
    locations = {v for v in locations if v}
    locations |= {v.replace("\\", "/") for v in locations} | {v.replace("/", "\\") for v in locations}
    for v in sorted(locations, key=len, reverse=True):  # longest first: no partial overlap
        text = text.replace(v, "<session>")
    text = _ARCHIVE_ROOT.sub("<session>::<session>", text)
    pattern = "|".join(re.escape(n) for n in sorted((n for n in names if n), key=len, reverse=True))
    if pattern:
        word = re.compile(r"(?<!\w)(?:" + pattern + r")(?!\w)")
        # Pieces between the tokens already placed, so a session NAMED
        # "session" does not eat its own placeholders.
        text = "<session>".join(word.sub("<session>", piece) for piece in text.split("<session>"))
    return text


# One registry per (session, calling file), so the default filter's "once per
# location" stays "once per location per session" now that the path is not in
# the text. Keyed by file as well: a registry entry is (text, category,
# lineno), so two call sites on the same line number in different files
# would otherwise share one entry.
_REGISTRIES: dict = {}


def emit(category: type, message: str, *, path: Optional[str],
         details: Iterable = (), stacklevel: int = 1, also: Iterable[str] = ()) -> None:
    """``warnings.warn(message, category, stacklevel)`` for a mef3io warning.

    ``stacklevel`` means what it means to ``warnings.warn`` called from the
    caller of ``emit``. The message is scrubbed of ``path`` (and ``also``)
    here, so a call site cannot forget to.
    """
    w = category(scrub(message, path, also), path=path, details=details)
    try:
        frame = sys._getframe(stacklevel)
    except ValueError:  # stack shallower than asked: what warnings.warn does
        globals_, filename, lineno = sys.__dict__, "sys", 1
    else:
        globals_, filename, lineno = frame.f_globals, frame.f_code.co_filename, frame.f_lineno
    if path is None:
        registry = globals_.setdefault("__warningregistry__", {})
    else:
        key = (os.path.abspath(os.fspath(path)), filename)
        registry = _REGISTRIES.setdefault(key, {})
    warnings.warn_explicit(w, category, filename, lineno,
                           module=globals_.get("__name__", "<string>"),
                           registry=registry, module_globals=globals_)
