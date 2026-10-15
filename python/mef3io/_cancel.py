"""Cancellation and progress for reads and long operations (#69)."""
from __future__ import annotations

from typing import Callable, Optional

try:
    from . import _mef3io
except ImportError:  # pragma: no cover - only without the extension
    _mef3io = None

#: ``progress(done, total)``: both in bytes.
ProgressCallback = Callable[[int, int], object]

if _mef3io is not None:
    Cancelled = _mef3io.Cancelled
else:  # pragma: no cover - only without the extension

    class Cancelled(Exception):  # type: ignore[no-redef]
        """An operation was stopped through its :class:`CancelToken`."""


Cancelled.__doc__ = """Raised by an operation whose :class:`CancelToken` was cancelled.

A cancelled call returns nothing and leaves nothing behind: no partial
array, no partial archive or extracted directory, and — for
:func:`repair_session` and :func:`recover_session`, which decide everything
before they write anything — no modified file and no backup directory.

It subclasses :class:`Exception` directly, **not** :class:`MefError` or
:class:`RuntimeError`: stopping an operation on request is not a failure, and
code that catches ``MefError`` to report a damaged file should not catch it.
"""


class CancelToken:
    """A flag that stops a running read or long operation.

    Pass it as ``cancel=`` to :meth:`Reader.read`, :meth:`Reader.read_raw`,
    :func:`archive_session`, :func:`extract_session`, :func:`inspect`,
    :meth:`Validator.validate`, :func:`validate_session`,
    :func:`repair_session` or :func:`recover_session`, and call
    :meth:`cancel` from any thread. The operation notices between units of
    work (a decoded block, a 1 MB copy chunk, a segment) and raises
    :class:`Cancelled`.

    A token is one-shot: once cancelled it stays cancelled, so create a new
    one per request. One token may be shared by several calls to stop them
    together (for example every channel of one viewer request).

    Examples
    --------
    >>> token = mef3io.CancelToken()
    >>> threading.Timer(0.5, token.cancel).start()
    >>> try:
    ...     x = reader.read("ch1", cancel=token)
    ... except mef3io.Cancelled:
    ...     pass  # a newer request superseded this one
    """

    __slots__ = ("_impl", "_flag")

    def __init__(self) -> None:
        if _mef3io is not None:
            self._impl = _mef3io.CancelToken()
            self._flag = None
        else:  # pragma: no cover - only without the extension
            import threading

            self._impl = None
            self._flag = threading.Event()

    def cancel(self) -> None:
        """Request cancellation. Safe from any thread; idempotent."""
        if self._impl is not None:
            self._impl.cancel()
        else:  # pragma: no cover
            self._flag.set()

    @property
    def cancelled(self) -> bool:
        """Whether :meth:`cancel` has been called."""
        if self._impl is not None:
            return bool(self._impl.cancelled)
        return self._flag.is_set()  # pragma: no cover

    def raise_if_cancelled(self) -> None:
        """Raise :class:`Cancelled` if the token is set.

        For a caller's own loops between mef3io calls — for example reading
        many channels for one request — so they stop at the same points the
        library does.
        """
        if self.cancelled:
            raise Cancelled("operation cancelled")

    def __repr__(self) -> str:
        return f"CancelToken(cancelled={self.cancelled})"


def _hooks(cancel: Optional[CancelToken], progress: Optional[ProgressCallback]) -> dict:
    """Validate and translate the public ``cancel=`` / ``progress=`` keywords
    into the extension's arguments. Empty when neither is given, so the call
    is exactly what it was before these hooks existed."""
    out = {}
    if cancel is not None:
        if not isinstance(cancel, CancelToken):
            raise TypeError(
                f"cancel must be a mef3io.CancelToken, not {type(cancel).__name__}"
            )
        out["cancel"] = cancel._impl
    if progress is not None:
        if not callable(progress):
            raise TypeError("progress must be a callable taking (done, total)")
        out["progress"] = progress
    return out
