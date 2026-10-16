"""Warnings mef3io emits: one base class, and one way to emit them.

Every warning carries the session it is about as DATA (``w.message.path``)
and never in its text, so a caller that must keep paths out of its logs can
log ``str(w.message)`` as it is. File names inside the session can turn up in
a message (a reason taken from the core, say), but the session's own location
and name are replaced by ``<session>`` before the warning is built.

Python's default filter shows a warning once per (text, category, line). The
text used to start with the session path, which made that "once per session";
:func:`emit` keeps it so by giving each session its own warning registry.
"""
from __future__ import annotations

import os
import re
import sys
import warnings
from typing import Any, Iterable, Optional


class MefWarning(UserWarning):
    """Base class of every warning mef3io emits.

    The message never contains the session's path or name (they appear as
    ``<session>``), so it can be logged where paths must not be. What the
    message summarises is on the warning object, which Python passes as
    ``w.message`` (``warnings.catch_warnings(record=True)``,
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

    Filter every mef3io warning at once with::

        warnings.filterwarnings("ignore", category=mef3io.MefWarning)

    Examples
    --------
    >>> with warnings.catch_warnings(record=True) as caught:
    ...     warnings.simplefilter("always")
    ...     r = mef3io.Reader(path)
    >>> for w in caught:
    ...     if issubclass(w.category, mef3io.MefWarning):
    ...         log.warning("%s", w.message)        # no path in the text
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


def scrub(text: str, path: Optional[str]) -> str:
    """``text`` with the session's location and name replaced by ``<session>``."""
    if not path:
        return text
    raw = os.fspath(path).rstrip("/\\")
    if not raw:
        return text
    variants = {raw, os.path.abspath(raw), os.path.realpath(raw)}
    variants |= {v.replace("\\", "/") for v in variants} | {v.replace("/", "\\") for v in variants}
    for v in sorted(variants, key=len, reverse=True):  # longest first: no partial overlap
        text = text.replace(v, "<session>")
    stem = _SESSION_SUFFIX.sub("", os.path.basename(raw))
    if stem:
        # The session's folder inside a tar ("<stem>.mefd/...") and its own
        # record files ("<stem>.rdat"); only as a whole path component.
        text = re.sub(r"(?<![^/\\:])" + re.escape(stem) + r"(?=\.(?:mefd|rdat|ridx)\b)",
                      "<session>", text)
    return text


# One registry per session, so the default filter's "once per location"
# stays "once per location per session" now that the path is not in the text.
_REGISTRIES: dict = {}


def emit(category: type, message: str, *, path: Optional[str],
         details: Iterable = (), stacklevel: int = 1) -> None:
    """``warnings.warn(message, category, stacklevel)`` for a mef3io warning.

    ``stacklevel`` means what it means to ``warnings.warn`` called from the
    caller of ``emit``. The message is scrubbed of ``path`` here, so a call
    site cannot forget to.
    """
    w = category(scrub(message, path), path=path, details=details)
    try:
        frame = sys._getframe(stacklevel)
    except ValueError:  # stack shallower than asked: what warnings.warn does
        globals_, filename, lineno = sys.__dict__, "sys", 1
    else:
        globals_, filename, lineno = frame.f_globals, frame.f_code.co_filename, frame.f_lineno
    if path is None:
        registry = globals_.setdefault("__warningregistry__", {})
    else:
        registry = _REGISTRIES.setdefault(os.path.abspath(os.fspath(path)), {})
    warnings.warn_explicit(w, category, filename, lineno,
                           module=globals_.get("__name__", "<string>"),
                           registry=registry, module_globals=globals_)
