"""Metadata cache ("warm start") for mef3io sessions.

Opening a session with many segments touches one small ``.tmet`` per segment,
which is cheap locally but costly on network storage. This module snapshots the
per-channel metadata plus per-file fingerprints so a later open can serve
channel/info queries without walking the tree.

Policy (as specified):
- Caching is **off by default**. The caller must opt in.
- ``cache="auto"`` stores the snapshot in a per-user OS cache directory, keyed
  by the absolute session path — safe for read-only sessions.
- ``cache=<path>`` writes an explicit, persistent cache file the caller names.
- Correctness never depends on the cache: on load, every referenced file's
  (size, mtime) is re-checked; any mismatch invalidates the snapshot.
- A write to the session removes the auto cache entry; stat validation remains
  the guarantee for any cache a writer could not see.

The snapshot stores only metadata (channel infos + fingerprints), never signal
data, and does not weaken encryption (encrypted sessions still require the
password at open — the snapshot holds no decrypted content).
"""
from __future__ import annotations

import hashlib
import json
import os
import tempfile
from pathlib import Path
from typing import Optional

CACHE_FORMAT_VERSION = 1


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


def _fingerprints(session_path: str) -> dict:
    """(relpath -> [size, mtime_ns]) for every metadata/index file in the tree.

    Tar sessions (a single archive file) are fingerprinted as that one file —
    globbing inside them is impossible, and an empty dict would make a stale
    cache validate forever.
    """
    root = Path(session_path)
    if root.is_file():
        st = root.stat()
        return {root.name: [st.st_size, st.st_mtime_ns]}
    fp = {}
    for pattern in ("*.timd/*.segd/*.tmet", "*.timd/*.segd/*.tidx"):
        for f in root.glob(pattern):
            st = f.stat()
            fp[str(f.relative_to(root))] = [st.st_size, st.st_mtime_ns]
    return fp


def build_snapshot(session_path: str, channel_infos: dict) -> dict:
    return {
        "format": CACHE_FORMAT_VERSION,
        "session_path": os.path.abspath(session_path),
        "fingerprints": _fingerprints(session_path),
        "channel_infos": channel_infos,
    }


def save(cache_path: Path, snapshot: dict) -> None:
    cache_path.parent.mkdir(parents=True, exist_ok=True)
    tmp = cache_path.with_suffix(cache_path.suffix + ".tmp")
    tmp.write_text(json.dumps(snapshot))
    tmp.replace(cache_path)  # atomic


def load_valid(cache_path: Path, session_path: str) -> Optional[dict]:
    """Return the cached snapshot if present and still consistent, else None."""
    try:
        snapshot = json.loads(cache_path.read_text())
    except (OSError, ValueError):
        return None
    if snapshot.get("format") != CACHE_FORMAT_VERSION:
        return None
    if snapshot.get("session_path") != os.path.abspath(session_path):
        return None
    if snapshot.get("fingerprints") != _fingerprints(session_path):
        return None  # any file added/removed/resized/modified -> stale
    return snapshot


def invalidate(session_path: str, cache) -> None:
    """Best-effort removal of a cache entry (used after writes)."""
    path = resolve_cache_path(session_path, cache if cache is not None else "auto")
    if path is not None:
        try:
            path.unlink()
        except OSError:
            pass
