"""Session archiving: pack a ``.mefd`` directory into one uncompressed tar."""
from __future__ import annotations

from typing import Callable, Optional

from ._cancel import CancelToken, _hooks


def archive_session(
    session_path,
    tar_path: Optional[str] = None,
    overwrite: bool = False,
    *,
    cancel: Optional[CancelToken] = None,
    progress: Optional[Callable[[int, int], object]] = None,
) -> str:
    """Pack a session directory into a single uncompressed tar archive.

    The archive (conventionally ``name.mefd.tar``) is a plain ustar file:
    :class:`Reader` opens it directly — no extraction — and any tar tool
    (``tar -xf``) reproduces the original directory. Because it is
    uncompressed, windowed reads still fetch only the byte ranges they need.
    The source directory is left untouched; output is deterministic, so
    archiving the same session twice yields identical bytes. Tar sessions are
    read-only: :class:`Writer` refuses ``.tar`` paths.

    Parameters
    ----------
    session_path : str or path-like
        The ``.mefd`` session directory to pack (the suffix is enforced).
    tar_path : str, optional
        Target archive path; must end ``.mefd.tar``. Default derives
        ``<session_path>.tar`` (``name.mefd`` becomes ``name.mefd.tar``).
    overwrite : bool, optional
        Replace an existing target archive. Default ``False`` (an existing
        target raises).
    cancel : CancelToken, optional
        Stop the archive from another thread; checked between 1 MB chunks.
        The archive is built in a temporary file and renamed into place only
        when complete, so a cancelled call raises :class:`Cancelled` and leaves
        no archive and no temporary file (an existing target is untouched).
    progress : callable, optional
        ``progress(done, total)`` in bytes of session files copied. Called at a
        bounded rate, never concurrently; ``(total, total)`` exactly once, on
        success. If it raises, the archive is abandoned like a cancel and the
        exception propagates.

    Returns
    -------
    str
        Path of the created archive.

    Examples
    --------
    >>> tar = mef3io.archive_session("session.mefd")
    >>> with mef3io.Reader(tar) as r:
    ...     x = r.read(r.channels[0])
    """
    from . import _mef3io

    return _mef3io.archive_session(
        str(session_path), str(tar_path or ""), bool(overwrite), **_hooks(cancel, progress)
    )


def extract_session(
    tar_path,
    dest_dir: Optional[str] = None,
    overwrite: bool = False,
    *,
    cancel: Optional[CancelToken] = None,
    progress: Optional[Callable[[int, int], object]] = None,
) -> str:
    """Unpack a session archive back into a ``.mefd`` directory.

    The inverse of :func:`archive_session` — after extraction the directory is
    a normal writable session again. The session root inside the archive is
    stripped, so ``dest_dir`` becomes the session directory itself; archives
    from foreign tar tools work too. The session is unpacked into a temporary
    sibling directory and moved into place only when complete, so a failed or
    cancelled extraction never leaves a half-written directory behind — and
    with ``overwrite=True`` the existing directory survives until the new one
    is whole.

    Parameters
    ----------
    tar_path : str or path-like
        The session archive to unpack; must end ``.mefd.tar``.
    dest_dir : str, optional
        Target directory; must end ``.mefd``. Default strips the ``.tar``
        suffix (``name.mefd.tar`` becomes ``name.mefd`` next to the archive).
    overwrite : bool, optional
        Replace an existing target directory. Default ``False`` (an existing
        target raises).
    cancel : CancelToken, optional
        Stop the extraction from another thread; checked between 1 MB chunks.
        Raises :class:`Cancelled` and leaves nothing behind.
    progress : callable, optional
        ``progress(done, total)`` in bytes of member data written; as in
        :func:`archive_session`.

    Returns
    -------
    str
        Path of the extracted session directory.

    Examples
    --------
    >>> session = mef3io.extract_session("session.mefd.tar")
    >>> with mef3io.Writer(session) as w:      # writable again
    ...     w.write("ch1", more_data, t, fs=256.0)
    """
    from . import _mef3io

    return _mef3io.extract_session(
        str(tar_path), str(dest_dir or ""), bool(overwrite), **_hooks(cancel, progress)
    )
