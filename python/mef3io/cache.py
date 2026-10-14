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
- Correctness never depends on the cache: on load, every referenced file is
  re-checked (see "Validation" below); any mismatch invalidates the snapshot.
- A write to the session removes the auto cache entry; validation remains the
  guarantee for any cache a writer could not see.

Validation
----------
It has to be cheaper than the open it replaces, so it is STAT-ONLY for files
that had settled when the snapshot was taken (issue #64: hashing every
``.tmet`` made a cached directory open ~2.7x slower than an uncached one).
Each ``.tmet``/``.tidx`` (or the one ``.mefd.tar``) is fingerprinted by
``(size, mtime_ns, ctime_ns, inode)``. That catches every way mef3io changes a
file without reading it:

- an append grows the file (size);
- a repair REPLACES a ``.tmet`` through temp file + rename, so the name now
  points at a different file (inode / NTFS file ID) whatever its size and
  timestamps;
- a repair PATCHES a ``.tidx`` header in place, which moves mtime and, on
  POSIX, ctime. ctime cannot be set from user space, so even a writer that
  restores mtime afterwards is caught. On Windows ``st_ctime`` is the CREATION
  time, which an in-place write does not move, so it is not recorded there and
  an in-place rewrite that also puts mtime back by hand goes unseen — the one
  case the old content hash covered that this does not, on that platform.

A timestamp cannot catch a second write that lands in the same clock tick as
the one before it — filesystem timestamps are coarse (FAT 2 s, HFS+ 1 s, ext4
one kernel tick) — when the snapshot was taken between the two. This is git's
"racy clean" problem and gets git's answer: a file whose newest timestamp is
within ``RACY_WINDOW_NS`` of the moment the snapshot was taken ALSO gets a
content signature (a hash of the fixed-size ``.tmet``, or of the 1024-byte
universal header of a ``.tidx``), checked on load. Once the window has passed,
a load that finds the file unchanged drops the signature and re-saves the
snapshot: any later write carries a later timestamp. A session written just
before its first cached open therefore pays the hashing once or twice, then is
stat-only.

Finding ADDED files needs a listing, and listing one directory per segment is
most of what is left. On POSIX a directory's mtime/ctime move whenever an entry
is added, removed or renamed, so the snapshot also fingerprints the session
root, every ``.timd`` and every ``.segd`` (stat taken BEFORE each listing), and
when none of them moved — and none was racy — the stored file set is current
and nothing is listed. Any mismatch falls back to a full listing. On Windows
the tree is always listed: FAT/exFAT do not maintain directory timestamps.

The snapshot stores only metadata (channel infos + fingerprints), never signal
data, and does not weaken encryption (encrypted sessions still require the
password at open — the snapshot holds no decrypted content).
"""
from __future__ import annotations

import hashlib
import json
import os
import tempfile
import time
from pathlib import Path
from typing import Optional

# 2: added "declaration_issues". A v1 snapshot has no such key, so a warm open
# from one would silently skip the open-time warning until the session changed
# on disk; bumping invalidates them instead, and the next open rebuilds.
# 3: fingerprints now include a content signature, so fixed-size/header-only
# rewrites still invalidate snapshots on coarse-mtime filesystems.
# 4: added "problems" (segments a strict=False open skipped). Without it a warm
# lenient open reported no problems and raised no warning, and a strict open
# was served a snapshot of a session it would have refused.
# 5: "fingerprints" is {"files", "dirs"}; files are (size, mtime_ns, ctime_ns,
# inode) with a content signature only while racy, dirs let an unchanged tree
# skip the listing. See "Validation" above (issue #64).
CACHE_FORMAT_VERSION = 5
METADATA_FILE_BYTES = 16384

# How recently a file or directory may have changed, relative to when the
# snapshot is taken or checked, before its stat alone is no longer trusted. It
# has to cover the coarsest timestamp a session can sit on (FAT: 2 s) plus clock
# skew between this machine and a file server; it costs nothing on a settled
# session.
RACY_WINDOW_NS = 5_000_000_000

_SIGNED_SUFFIXES = {".tmet": METADATA_FILE_BYTES, ".tidx": 1024}

# Windows reports the CREATION time as st_ctime (deprecated there since 3.12),
# which an in-place write leaves alone; it adds nothing to the file-ID check.
_HAS_CHANGE_TIME = os.name != "nt"
# Whether an unchanged directory stat proves an unchanged directory listing.
_TRUST_DIR_STAT = os.name != "nt"

_ROOT = "."


def _hash_bytes(path: str, nbytes: int | None = None) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        if nbytes is None:
            for chunk in iter(lambda: f.read(65536), b""):
                h.update(chunk)
        else:
            h.update(f.read(nbytes))
    return h.hexdigest()[:16]


def _ctime(st: os.stat_result) -> int:
    return st.st_ctime_ns if _HAS_CHANGE_TIME else 0


def _file_key(st: os.stat_result) -> list:
    return [st.st_size, st.st_mtime_ns, _ctime(st), st.st_ino]


def _dir_key(st: os.stat_result) -> list:
    return [st.st_mtime_ns, _ctime(st), st.st_ino]


def _is_racy(st: os.stat_result, now_ns: int) -> bool:
    newest = max(st.st_mtime_ns, _ctime(st))
    # A timestamp in the future (clock skew) is racy too.
    return now_ns - newest < RACY_WINDOW_NS


def _signature(path: str) -> Optional[str]:
    nbytes = _SIGNED_SUFFIXES.get(os.path.splitext(path)[1].lower())
    return None if nbytes is None else _hash_bytes(path, nbytes)


def _fingerprint_file(path: str, now_ns: int) -> list:
    # os.stat, not a cached DirEntry.stat(): on Windows the latter comes from
    # the directory entry, which NTFS updates lazily, and carries no file ID.
    st = os.stat(path)
    fp = _file_key(st)
    if _is_racy(st, now_ns):
        sig = _signature(path)
        if sig is not None:
            fp.append(sig)
    return fp


def _fingerprint_dir(path: str, now_ns: int) -> list:
    st = os.stat(path)
    fp = _dir_key(st)
    if _is_racy(st, now_ns):
        fp.append("racy")  # never trusted to stand in for a listing
    return fp


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


def _walk(session_path: str, now_ns: int):
    """(files: relpath -> path, dirs: relpath -> dir fingerprint).

    Each directory is stat'd BEFORE it is listed, so an entry added while we
    walk can only make the stored directory fingerprint older than the stored
    listing — which the next load rejects — never newer.

    Tar sessions (a single archive file) are fingerprinted as that one file —
    listing inside them is impossible, and an empty dict would make a stale
    cache validate forever.
    """
    # Plain strings, not Path: building ~1000 Path objects per load cost more
    # than the stat calls themselves.
    root = os.fspath(session_path)
    if os.path.isfile(root):
        return {os.path.basename(os.path.normpath(root)): root}, {}
    files, dirs = {}, {}

    def subdirs(path, rel, suffix):
        dirs[rel] = _fingerprint_dir(path, now_ns)
        with os.scandir(path) as it:
            return [e for e in it if e.name.lower().endswith(suffix) and e.is_dir()]

    for ch in subdirs(root, _ROOT, ".timd"):
        for seg in subdirs(ch.path, ch.name, ".segd"):
            rel = f"{ch.name}/{seg.name}"
            dirs[rel] = _fingerprint_dir(seg.path, now_ns)
            with os.scandir(seg.path) as it:
                for e in it:
                    if os.path.splitext(e.name)[1].lower() in _SIGNED_SUFFIXES:
                        files[f"{rel}/{e.name}"] = e.path
    return files, dirs


def fingerprints(session_path: str) -> dict:
    """``{"files": {relpath: [size, mtime_ns, ctime_ns, inode(, sig)]},
    "dirs": {relpath: [mtime_ns, ctime_ns, inode(, "racy")]}}``.

    Take these BEFORE reading the metadata the snapshot will carry: a change
    landing in between then leaves fingerprints older than the content, which
    the next load rejects, instead of fingerprints newer than it, which it
    would accept.
    """
    now_ns = time.time_ns()
    files, dirs = _walk(session_path, now_ns)
    return {"files": {rel: _fingerprint_file(p, now_ns) for rel, p in files.items()},
            "dirs": dirs}


def build_snapshot(session_path: str, channel_infos: dict, declaration_issues=None,
                   problems=None, fingerprints_=None) -> dict:
    return {
        "format": CACHE_FORMAT_VERSION,
        "session_path": os.path.abspath(session_path),
        "fingerprints": fingerprints_ if fingerprints_ is not None
        else fingerprints(session_path),
        "channel_infos": channel_infos,
        # Carried so a warm open can still warn about unset size declarations
        # without touching the session tree. Snapshots written before this key
        # existed simply have no issues to report.
        "declaration_issues": list(declaration_issues or []),
        # Segments a strict=False open skipped. The channel infos above were
        # computed WITHOUT them, so a strict open must not be served this
        # snapshot, and a lenient one must still report them.
        "problems": list(problems or []),
    }


def save(cache_path: Path, snapshot: dict) -> None:
    cache_path.parent.mkdir(parents=True, exist_ok=True)
    tmp = cache_path.with_suffix(cache_path.suffix + ".tmp")
    tmp.write_text(json.dumps(snapshot))
    tmp.replace(cache_path)  # atomic


def _tree_unchanged(root: str, dirs: dict) -> bool:
    """True when every stored directory still has its stored stat, so no entry
    was added, removed or renamed anywhere the snapshot looked."""
    if not _TRUST_DIR_STAT or not dirs:
        return False
    for rel, fp in dirs.items():
        if len(fp) != 3:
            return False  # racy when stored
        try:
            st = os.stat(root if rel == _ROOT else os.path.join(root, rel))
        except OSError:
            return False
        if _dir_key(st) != fp:
            return False
    return True


def _still_valid(stored: dict, session_path: str) -> Optional[bool]:
    """None if the session no longer matches ``stored``; otherwise whether
    ``stored`` was updated in place (signatures dropped once their file
    settled, or directory fingerprints refreshed) and is worth re-saving."""
    files_fp, dirs_fp = stored["files"], stored["dirs"]
    root = os.fspath(session_path)
    now_ns = time.time_ns()
    changed = False
    if os.path.isdir(root) and _tree_unchanged(root, dirs_fp):
        paths = {rel: os.path.join(root, rel) for rel in files_fp}
    else:
        try:
            paths, dirs = _walk(session_path, now_ns)
        except OSError:
            return None
        if paths.keys() != files_fp.keys():
            return None  # a file was added or removed
        if dirs != dirs_fp:
            # Same files, but a directory moved (a temp file came and went) or
            # has settled: remember the stats taken before this listing.
            stored["dirs"] = dirs
            changed = True
    for rel, path in paths.items():
        fp = files_fp[rel]
        try:
            st = os.stat(path)
        except OSError:
            return None
        if _file_key(st) != fp[:4]:
            return None  # resized, rewritten, or replaced by another file
        if len(fp) > 4:
            # Racy when the snapshot was taken: the stat could be hiding a
            # second write in the same timestamp tick, so look at the bytes.
            try:
                if _signature(path) != fp[4]:
                    return None
            except OSError:
                return None
            if not _is_racy(st, now_ns):
                del fp[4:]
                changed = True
    return changed


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
    stored = snapshot.get("fingerprints")
    if (not isinstance(stored, dict) or not isinstance(stored.get("files"), dict)
            or not stored["files"] or not isinstance(stored.get("dirs"), dict)):
        return None
    changed = _still_valid(stored, session_path)
    if changed is None:
        return None
    if changed:
        # Keep what this load learned, so the next one is stat-only. Best
        # effort: a read-only cache location just keeps paying.
        try:
            save(cache_path, snapshot)
        except OSError:
            pass
    return snapshot


def invalidate(session_path: str, cache) -> None:
    """Best-effort removal of a cache entry (used after writes)."""
    path = resolve_cache_path(session_path, cache if cache is not None else "auto")
    if path is not None:
        try:
            path.unlink()
        except OSError:
            pass
