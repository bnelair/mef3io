"""Metadata cache ("warm start") for mef3io sessions.

Opening a session with many segments reads one ``.tmet`` per segment, which is
cheap locally but costly on network storage. This module snapshots the
per-channel metadata plus a small per-channel signature, so a later open can
serve channel/info queries without reading the tree.

Policy:
- Caching is **off by default**. The caller must opt in.
- ``cache="auto"`` stores the snapshot in a per-user OS cache directory, keyed
  by the absolute session path — safe for read-only sessions.
- ``cache=<path>`` writes an explicit, persistent cache file the caller names.
- A snapshot is only ever read by the exact mef3io version that wrote it.
- Validity is decided ONLY here, when a cached open happens. Write paths
  (Writer, repair, recover, ...) know nothing about the cache and never touch
  a snapshot. On any mismatch the snapshot is refused, the session is opened
  normally, and a fresh snapshot is written.

A snapshot is only worth having while it is SMALL and CHEAPER than the open it
replaces (issue #64: hashing every ``.tmet`` once made a cached directory open
~2.7x slower than an uncached one). So its size and the work to check it do
not depend on the number of segments:

Validation (per channel, O(1) in segments)
------------------------------------------
1. List the ``.timd`` folder: the number of ``.segd`` entries and the name of
   the last one (catches added/removed segments).
2. Read the 1024-byte universal header of the FIRST segment's ``.tmet``
   (stored start time) and of the LAST one (stored end time, and the
   provenance region at offset 960: mef3io's last operation, last-modified-by
   version and modification count — no dates, by design). The modification
   count is the change marker: it catches a mef3io edit of the last segment
   even at equal file size. Files without the ``M3IO`` magic contribute no
   provenance.
3. ``stat`` the last segment's ``.tdat`` and ``.tidx`` sizes (appends grow
   them).

Plus, for the session root: the set of channel folders and the sizes of the
session-level ``.rdat``/``.ridx``. A ``.mefd.tar`` is one file: its size and
mtime.

Nothing here compares timestamps or inodes of files inside a directory session,
so there is no "same clock tick" race and no platform difference (Windows'
``st_ctime`` and FAT's coarse times do not matter).

**Blind spot, by design:** a FOREIGN tool's in-place change to a NON-last
segment that changes neither the channel's start/end nor the last segment's
sizes or provenance is not detected. Open without ``cache=`` for a full
re-read; that is always available. (mef3io's own ``repair_session`` /
``recover_session`` on an older segment also stamp the LAST segment with the
``segment-repaired`` provenance code, so those are seen — this module still
knows nothing about them, and they nothing about it.)

Encrypted sessions are never cached: no snapshot is written for a session
whose metadata is encrypted at any level, and a snapshot file found for one is
ignored and deleted. Such sessions are always opened normally. (Channel infos
hold decrypted metadata, which must not end up in a plaintext file.)

The snapshot stores only metadata (channel infos + signatures), never signal
data.
"""
from __future__ import annotations

import hashlib
import json
import os
import struct
import tempfile
from pathlib import Path
from typing import Optional

# 2: added "declaration_issues". A v1 snapshot has no such key, so a warm open
# from one would silently skip the open-time warning until the session changed
# on disk; bumping invalidates them instead, and the next open rebuilds.
# 3: fingerprints now include a content signature, so fixed-size/header-only
# rewrites still invalidate snapshots on coarse-mtime filesystems (superseded
# by 6, whose check is O(channels) and accepts a documented blind spot).
# 4: added "problems" (segments a strict=False open skipped). Without it a warm
# lenient open reported no problems and raised no warning, and a strict open
# was served a snapshot of a session it would have refused.
# 6: (5 was never released) the per-file fingerprint list is replaced by an
# O(1)-per-channel signature, and the exact mef3io version that wrote the
# snapshot is stored and must match (issue #64).
CACHE_FORMAT_VERSION = 6

UH_BYTES = 1024
_START_OFFSET, _END_OFFSET = 16, 24          # si8, stored (negated) uUTC
_PROVENANCE_OFFSET, _PROVENANCE_BYTES = 960, 64
_PROVENANCE_MAGIC = b"M3IO"
_PASSWORD_FIELDS_OFFSET = 868                # L1 + L2 validation fields, 16 B each
_ROOT_SUFFIXES = (".rdat", ".ridx")


def _version() -> str:
    from . import __version__
    return str(__version__)


def _os_cache_dir() -> Path:
    # Minimal platformdirs-style resolution without the dependency.
    if os.name == "nt":
        base = os.environ.get("LOCALAPPDATA") or tempfile.gettempdir()
    elif os.sys.platform == "darwin":
        base = os.path.expanduser("~/Library/Caches")
    else:
        base = os.environ.get("XDG_CACHE_HOME") or os.path.expanduser("~/.cache")
    d = Path(base) / "mef3io"
    d.mkdir(parents=True, exist_ok=True)
    return d


def auto_cache_path(session_path: str) -> Path:
    key = hashlib.sha256(os.path.abspath(session_path).encode()).hexdigest()[:16]
    stem = Path(session_path).stem
    return _os_cache_dir() / f"{stem}-{key}.mef3cache"


def resolve_cache_path(session_path: str, cache) -> Optional[Path]:
    """Map the ``cache`` argument to a concrete path, or None if disabled."""
    if cache is None or cache is False:
        return None
    if cache == "auto" or cache is True:
        return auto_cache_path(session_path)
    return Path(cache)


# --- signature -------------------------------------------------------------
# Plain strings, not Path, on this path: it runs once per channel per open.


def _fresh_stat(path: str) -> os.stat_result:
    # open + fstat, not os.stat: on NFS a bare stat can be answered from the
    # client's attribute cache (up to ~60 s stale; ~1 s on SMB), which would
    # hide an append another host just made. An open revalidates attributes
    # (close-to-open consistency). It costs a few files per channel.
    fd = os.open(path, os.O_RDONLY | getattr(os, "O_BINARY", 0))
    try:
        return os.fstat(fd)
    finally:
        os.close(fd)


def _size(path: str) -> Optional[int]:
    try:
        return _fresh_stat(path).st_size
    except FileNotFoundError:
        return None


def _header(path: str) -> bytes:
    try:
        with open(path, "rb") as f:
            return f.read(UH_BYTES)
    except FileNotFoundError:
        return b""


def _segment_key(name: str):
    # Segment folders are numbered "<channel>-NNNNNN.segd"; past 999999 the
    # number grows a digit, so order by length first, then by name.
    return (len(name), name)


def _segments(timd: str):
    """(count, first, last) of the ``.segd`` folders, in one pass and without
    holding the listing."""
    n, first, last = 0, None, None
    with os.scandir(timd) as it:
        for e in it:
            if e.name.lower().endswith(".segd") and e.is_dir():
                n += 1
                k = _segment_key(e.name)
                if first is None or k < _segment_key(first):
                    first = e.name
                if last is None or k > _segment_key(last):
                    last = e.name
    return n, first, last


def _password_protected(uh: bytes) -> bool:
    # Level-1/level-2 password validation fields, universal header 868..900;
    # all zero means the file is unencrypted.
    return any(uh[_PASSWORD_FIELDS_OFFSET:_PASSWORD_FIELDS_OFFSET + 32])


def _tar_password_protected(path: str) -> bool:
    """Whether the archive's first ``.tmet`` is password protected. Members are
    read lazily, and a session's first ``.tmet`` sits a few headers in, so
    this does not walk the archive. Unreadable -> True (fail closed)."""
    import tarfile

    try:
        with tarfile.open(path, mode="r:") as tf:
            for member in tf:
                if member.isfile() and member.name.lower().endswith(".tmet"):
                    f = tf.extractfile(member)
                    return f is None or _password_protected(f.read(UH_BYTES))
    except (OSError, tarfile.TarError):
        return True
    return False


def _channel_signature(timd: str):
    """(signature, password_protected) of one channel."""
    n, first, last = _segments(timd)
    if not n:
        return {"n_segments": 0}, False
    last_dir = os.path.join(timd, last)
    stem = last[:-5]
    last_uh = _header(os.path.join(last_dir, stem + ".tmet"))
    first_uh = last_uh if first == last else _header(
        os.path.join(timd, first, first[:-5] + ".tmet"))

    def si8(uh, off):
        return struct.unpack_from("<q", uh, off)[0] if len(uh) >= off + 8 else None

    prov = last_uh[_PROVENANCE_OFFSET:_PROVENANCE_OFFSET + _PROVENANCE_BYTES]
    return {
        "n_segments": n,
        "last_segment": last,
        "start_raw": si8(first_uh, _START_OFFSET),
        "end_raw": si8(last_uh, _END_OFFSET),
        "tdat_size": _size(os.path.join(last_dir, stem + ".tdat")),
        "tidx_size": _size(os.path.join(last_dir, stem + ".tidx")),
        # The whole 64-byte region: last op, last-modified-by version and the
        # modification count (the change marker). Files without mef3io's magic
        # (zeros, a foreign writer) contribute nothing.
        "provenance": prov.hex() if prov[:4] == _PROVENANCE_MAGIC else None,
    }, _password_protected(first_uh) or _password_protected(last_uh)


def signature(session_path: str) -> dict:
    """The snapshot's signature of ``session_path``: O(channels) in size, and
    O(channels) in I/O to compute (never per segment).

    Take it BEFORE reading the metadata the snapshot will carry: a change
    landing in between then leaves a signature older than the content, which
    the next load rejects, instead of one newer than it, which it would accept.
    """
    root = os.fspath(session_path)
    if os.path.isfile(root):
        st = _fresh_stat(root)
        return {"kind": "file", "size": st.st_size, "mtime_ns": st.st_mtime_ns,
                "encrypted": _tar_password_protected(root)}
    chans, root_files = [], {}
    with os.scandir(root) as it:
        for e in it:
            low = e.name.lower()
            if low.endswith(".timd") and e.is_dir():
                chans.append(e.name)
            elif low.endswith(_ROOT_SUFFIXES):
                root_files[e.name] = _size(e.path)
    channels = {}
    encrypted = False
    for ch in sorted(chans):
        channels[ch], enc = _channel_signature(os.path.join(root, ch))
        encrypted = encrypted or enc
    return {
        "kind": "dir",
        # Never cached (see the module docstring); recorded so a load can
        # refuse, and delete, a snapshot of a session that is encrypted now.
        "encrypted": encrypted,
        "root_files": dict(sorted(root_files.items())),
        "channels": channels,
    }


def build_snapshot(session_path: str, channel_infos: dict, declaration_issues=None,
                   problems=None, signature_=None) -> dict:
    return {
        "format": CACHE_FORMAT_VERSION,
        "mef3io_version": _version(),
        "session_path": os.path.abspath(session_path),
        "signature": signature_ if signature_ is not None else signature(session_path),
        "channel_infos": channel_infos,
        # Carried so a warm open can still warn about unset size declarations
        # without touching the session tree.
        "declaration_issues": list(declaration_issues or []),
        # Segments a strict=False open skipped. The channel infos above were
        # computed WITHOUT them, so a strict open must not be served this
        # snapshot, and a lenient one must still report them.
        "problems": list(problems or []),
    }


def save(cache_path: Path, snapshot: dict) -> None:
    """Write atomically. A unique temp file per call, so two processes saving
    the same snapshot cannot write into one shared temp name."""
    cache_path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=cache_path.parent, prefix=cache_path.name + ".",
                               suffix=".tmp")
    try:
        with os.fdopen(fd, "w") as f:
            json.dump(snapshot, f)
        os.replace(tmp, cache_path)  # atomic
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise


def discard(cache_path: Path) -> None:
    """Best-effort removal of a snapshot file that must not be used."""
    try:
        Path(cache_path).unlink()
    except OSError:
        pass


def load_valid(cache_path: Path, session_path: str) -> Optional[dict]:
    """Return the cached snapshot if present and still consistent, else None."""
    try:
        snapshot = json.loads(cache_path.read_text())
    except (OSError, ValueError):
        return None
    if snapshot.get("format") != CACHE_FORMAT_VERSION:
        return None
    if snapshot.get("mef3io_version") != _version():
        return None  # only the exact version that wrote a snapshot may read it
    if snapshot.get("session_path") != os.path.abspath(session_path):
        return None
    try:
        current = signature(session_path)
    except OSError:
        return None
    if current.get("encrypted"):
        # Encrypted sessions are never cached. A snapshot found here anyway
        # (planted, or left from before the session was encrypted) is deleted.
        discard(cache_path)
        return None
    if snapshot.get("signature") != current:
        return None
    return snapshot
