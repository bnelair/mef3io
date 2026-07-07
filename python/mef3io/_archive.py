"""Session archiving: pack a ``.mefd`` directory into one uncompressed tar."""
from __future__ import annotations

from typing import Optional


def archive_session(session_path, tar_path: Optional[str] = None, overwrite: bool = False) -> str:
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

    return _mef3io.archive_session(str(session_path), str(tar_path or ""), bool(overwrite))


def extract_session(tar_path, dest_dir: Optional[str] = None, overwrite: bool = False) -> str:
    """Unpack a session archive back into a ``.mefd`` directory.

    The inverse of :func:`archive_session` — after extraction the directory is
    a normal writable session again. The session root inside the archive is
    stripped, so ``dest_dir`` becomes the session directory itself; archives
    from foreign tar tools work too. A failed extraction never leaves a
    half-written directory behind.

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

    return _mef3io.extract_session(str(tar_path), str(dest_dir or ""), bool(overwrite))
