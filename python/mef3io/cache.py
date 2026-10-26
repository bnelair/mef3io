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
- A snapshot is only ever read by the exact mef3io BUILD that wrote it: the
  version string plus the compiled extension's size and mtime.
- Validity is decided ONLY here, when a cached open happens. Write paths
  (Writer, repair, recover, ...) know nothing about the cache and never touch
  a snapshot. On any mismatch the snapshot is refused, the session is opened
  normally, and a fresh snapshot is written.

A snapshot is only worth having while it is SMALL and CHEAPER than the open it
replaces (issue #64: hashing every ``.tmet`` once made a cached directory open
~2.7x slower than an uncached one). So its size and the work to check it do
not depend on the number of segments, and none is saved for a session with no
more segments than channels, where it cannot beat a plain open
(:func:`worth_caching`) — only a metadata-free marker (:func:`build_marker`)
that sends later cached opens straight to a plain open.

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
   provenance. Both headers' random per-file UUIDs (offset 836) are compared
   too, which catches a session deleted and rewritten at the same path from the same
   samples but with new units, conversion factor or subject.
3. The last segment's ``.tdat`` and ``.tidx`` sizes (appends grow them), via
   ``open`` + ``fstat`` so an NFS attribute cache cannot hide them.

Plus, for the session root: the set of channel folders and the sizes of the
session-level ``.rdat``/``.ridx``. A ``.mefd.tar`` is one file: its size and
mtime.

Nothing here compares timestamps or inodes of files inside a directory session,
so there is no "same clock tick" race and no platform difference (Windows'
``st_ctime`` and FAT's coarse times do not matter).

**Blind spot, by design:** an in-place change to a NON-last segment that
changes neither the channel's start/end nor anything checked in its last
segment is not detected. Open without ``cache=`` for a full re-read; that is
always available. In practice that covers:

- a FOREIGN tool editing an older segment;
- mef3io's own ``repair_session`` / ``recover_session`` on an older segment
  WHEN its ``segment-repaired`` stamp on the last segment was skipped: the
  last segment's provenance region belongs to another application, its CRC
  fails, or a writer's ``.mef3io-tmp`` file sits next to it. The repair report
  says so in that case. Otherwise the stamp moves the last segment's
  provenance and the change IS seen. (This module still knows nothing about
  repairs, and they nothing about it.)

Encrypted sessions are never cached: no snapshot is written for a session
whose metadata is encrypted at any level, and ANY snapshot file found for one
— whatever its format or version, including the format-4 snapshots of mef3io
1.1.x — is deleted before anything else is looked at. Such sessions are always
opened normally. (Channel infos hold decrypted metadata, which must not end up
in a plaintext file.)

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
# 7: a channel info's "units_conversion_factor" is None when the channel's
# segments store different factors (#150; it used to be the first segment's).
# The build check alone already rejects older snapshots; the bump says so.
CACHE_FORMAT_VERSION = 7

UH_BYTES = 1024
_START_OFFSET, _END_OFFSET = 16, 24          # si8, stored (negated) uUTC
_PROVENANCE_OFFSET, _PROVENANCE_BYTES = 960, 64
_PROVENANCE_MAGIC = b"M3IO"
_PASSWORD_FIELDS_OFFSET = 868                # L1 + L2 validation fields, 16 B each
_FILE_UUID_OFFSET = 836                      # random per file, drawn at creation
_ROOT_SUFFIXES = (".rdat", ".ridx")


def _version() -> str:
    """The mef3io version PLUS the identity of the compiled extension: the
    build ID CMake bakes in (git commit, else configure time —
    ``_mef3io.__build_id__``) and the binary's size and mtime. A version
    string alone is shared by every dev build at any commit and by a
    reinstalled wheel; the build ID separates same-size binaries in stores
    that pin every mtime (Nix), and size + mtime catch an incremental rebuild
    that kept the configure-time ID. A snapshot is only ever read by the exact
    build that wrote it.

    TODO: a PyInstaller onefile app extracts the extension afresh on every run,
    so its mtime — and with it this identity — changes per run and snapshots
    are never reused there (slower, never wrong). Dropping size+mtime when the
    build ID is a clean git commit would fix that, but needs a build-time (not
    configure-time) ID to stay safe for incremental dev builds."""
    from . import __version__

    build_id, binary = "", "?"
    try:
        from . import _mef3io

        build_id = str(getattr(_mef3io, "__build_id__", "") or "")
        st = os.stat(_mef3io.__file__)
        binary = f"{st.st_size}-{st.st_mtime_ns}"
    except (ImportError, OSError, AttributeError):
        pass
    return f"{__version__}+build.{build_id or 'unknown'}.{binary}"


def _session_key(session_path: str) -> str:
    """What a snapshot stores to say which session it describes: a SHA-256 of
    the absolute path, not the path. Only ever compared for equality, and
    paths can carry a subject's name."""
    return hashlib.sha256(
        os.path.abspath(session_path).encode("utf-8", "surrogatepass")).hexdigest()


def _looks_like_ours(data) -> bool:
    """A parsed cache file that is a snapshot or marker of ANY mef3io version
    (a JSON object with a ``format`` key). An explicit ``cache=<path>`` can
    name any file; nothing that is not one of ours is ever deleted."""
    return isinstance(data, dict) and "format" in data


def _read_json(cache_path):
    try:
        return json.loads(Path(cache_path).read_text())
    except (OSError, ValueError, UnicodeDecodeError):
        return None


def discard_if_ours(cache_path) -> None:
    """Delete ``cache_path`` only if it is a mef3io snapshot or marker."""
    if _looks_like_ours(_read_json(cache_path)):
        discard(cache_path)


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
    """``<hash16>.mef3cache`` in the OS cache directory. The hash ONLY: a
    session is often named after its subject, and the file name is visible
    to anyone who can list that directory (#82). A snapshot saved by an
    earlier version under ``<session stem>-<hash16>.mef3cache`` is deleted
    by the next ``cache="auto"`` open of that session, which builds a new
    one under this name."""
    key = hashlib.sha256(os.path.abspath(session_path).encode()).hexdigest()[:16]
    return _os_cache_dir() / f"{key}.mef3cache"


def _legacy_auto_cache_path(session_path: str) -> Path:
    """Where mef3io before 2.0.0 put the auto snapshot: the session stem in
    the name. Only ever looked at to delete it (#82)."""
    key = hashlib.sha256(os.path.abspath(session_path).encode()).hexdigest()[:16]
    return _os_cache_dir() / f"{Path(session_path).stem}-{key}.mef3cache"


def _discard_legacy_auto_cache(session_path: str) -> None:
    """Delete this session's pre-2.0 auto snapshot, which carries the session
    name in its file name. That exact name only — no listing — and only if it
    is one of ours (discard_if_ours). One failed open when there is none."""
    try:
        discard_if_ours(_legacy_auto_cache_path(session_path))
    except OSError:
        pass


def resolve_cache_path(session_path: str, cache) -> Optional[Path]:
    """Map the ``cache`` argument to a concrete path, or None if disabled."""
    if cache is None or cache is False:
        return None
    if cache == "auto" or cache is True:
        _discard_legacy_auto_cache(session_path)
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


_LL_MAX = 2**63 - 1


def _segment_number(stem: str) -> int:
    """``<channel>-NNNNNN`` -> NNNNNN; -1 unless everything after the last
    '-' is an ASCII digit. A line-for-line port of C++ ``segment_number_of``
    (core/src/last_segment.hpp), overflow cut-off included, so the snapshot
    check and the segment-repaired stamp always agree on which segment is a
    channel's last. ``str.isdigit`` alone would accept '²' or Arabic-Indic
    digits, which C++ rejects and ``int()`` can raise on."""
    dash = stem.rfind("-")
    if dash < 0 or dash + 1 >= len(stem):
        return -1
    n = 0
    for c in stem[dash + 1:]:
        if not ("0" <= c <= "9"):
            return -1
        if n > (_LL_MAX - 9) // 10:
            return -1
        n = n * 10 + (ord(c) - 48)
    return n


def _segment_key(name: str):
    # Order by segment number, ties broken by the full folder name — the
    # C++ rule (a pair of number and filename). Case-sensitive ".segd", as
    # the C++ reader and stamp use.
    stem = name[:-5] if name.endswith(".segd") else name
    return (_segment_number(stem), name)


def _segments(timd: str):
    """(count, first, last) of the ``.segd`` folders, in one pass and without
    holding the listing."""
    n, first, last = 0, None, None
    with os.scandir(timd) as it:
        for e in it:
            # Case-sensitive, like the C++ reader (session.cpp): a folder the
            # reader ignores must not count here either.
            if e.name.endswith(".segd") and e.is_dir():
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


def _channel_signature(timd: str, n: int, first: Optional[str], last: Optional[str]):
    """(signature, password_protected) of one channel, from its listing."""
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
        # The writer draws a fresh random file UUID for every file it creates,
        # so a session RECREATED at the same path (folder removed, then rewritten) with the same
        # samples but new units, conversion factor or subject — same counts,
        # times, sizes, and provenance back to create/0 — still differs here.
        "file_uuids": first_uh[_FILE_UUID_OFFSET:_FILE_UUID_OFFSET + 16].hex()
        + last_uh[_FILE_UUID_OFFSET:_FILE_UUID_OFFSET + 16].hex(),
    }, _password_protected(first_uh) or _password_protected(last_uh)


def layout(session_path: str) -> dict:
    """The listing a signature is built from: the channel folders, each
    channel's segment count and first/last segment, and the session-level
    record files. O(channels) directory reads; no file is opened."""
    root = os.fspath(session_path)
    if os.path.isfile(root):
        return {"kind": "file"}
    chans, root_files = [], []
    with os.scandir(root) as it:
        for e in it:
            low = e.name.lower()
            if e.name.endswith(".timd") and e.is_dir():  # case-sensitive, as the reader
                chans.append(e.name)
            elif low.endswith(_ROOT_SUFFIXES):
                root_files.append(e.name)
    return {
        "kind": "dir",
        "channels": {ch: _segments(os.path.join(root, ch)) for ch in sorted(chans)},
        "root_files": sorted(root_files),
    }


def worth_caching(lay: dict) -> bool:
    """Whether a snapshot can make an open of this layout cheaper.

    A snapshot replaces reading one ``.tmet`` per segment with a few small
    reads per channel. With one segment per channel there is nothing to
    replace — measured on 128 x 1 and 256 x 1, a cached open was no faster than
    a plain one, and building the snapshot made the first open ~2x slower — so
    none is saved when the session has no more segments than channels. A tar
    is always worth it: its cold open has to read the archive's member index.
    """
    if lay.get("kind") == "file":
        return True
    chans = lay.get("channels", {})
    return sum(n for n, _, _ in chans.values()) > len(chans)


def worth_caching_infos(channel_infos: dict) -> bool:
    """:func:`worth_caching` from an open backend's channel infos (free)."""
    return sum(int(i.get("n_segments") or 0) for i in channel_infos.values()) > len(channel_infos)


# A session not worth a snapshot gets a MARKER instead: no metadata at all,
# just "a plain open is the fast path here". It lets later cached opens skip
# even the O(channels) listing that decides this (~40 us per channel, ~17% of
# a one-segment-per-channel open) and pay one tiny JSON read instead. The
# Reader drops the marker as soon as its plain open shows the session has
# grown past the threshold; the next cached open then builds a snapshot.
def build_marker(session_path: str) -> dict:
    return {
        "format": CACHE_FORMAT_VERSION,
        "mef3io_version": _version(),
        "session_key": _session_key(session_path),
        "not_worth_caching": True,
    }


def is_marker(snapshot: Optional[dict]) -> bool:
    return bool(snapshot) and snapshot.get("not_worth_caching") is True


def signature(session_path: str, lay: Optional[dict] = None) -> dict:
    """The snapshot's signature of ``session_path``: O(channels) in size, and
    O(channels) in I/O to compute (never per segment).

    Take it BEFORE reading the metadata the snapshot will carry: a change
    landing in between then leaves a signature older than the content, which
    the next load rejects, instead of one newer than it, which it would accept.
    """
    root = os.fspath(session_path)
    lay = lay if lay is not None else layout(root)
    if lay["kind"] == "file":
        st = _fresh_stat(root)
        return {"kind": "file", "size": st.st_size, "mtime_ns": st.st_mtime_ns,
                "encrypted": _tar_password_protected(root)}
    channels = {}
    encrypted = False
    for ch, (n, first, last) in lay["channels"].items():
        channels[ch], enc = _channel_signature(os.path.join(root, ch), n, first, last)
        encrypted = encrypted or enc
    return {
        "kind": "dir",
        # Never cached (see the module docstring); recorded so a load can
        # refuse, and delete, a snapshot of a session that is encrypted now.
        "encrypted": encrypted,
        "root_files": {name: _size(os.path.join(root, name)) for name in lay["root_files"]},
        "channels": channels,
    }


def build_snapshot(session_path: str, channel_infos: dict, declaration_issues=None,
                   problems=None, signature_=None) -> dict:
    return {
        "format": CACHE_FORMAT_VERSION,
        "mef3io_version": _version(),
        "session_key": _session_key(session_path),
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
    """Return the cached snapshot if present and still consistent, else None.

    A current marker (:func:`build_marker`) is returned as is, without any
    check of the session: it carries no metadata, so there is nothing stale or
    secret to protect, and the caller opens the session normally anyway.
    """
    if not Path(cache_path).exists():
        return None
    early = _read_json(cache_path)
    if (isinstance(early, dict) and is_marker(early)
            and early.get("format") == CACHE_FORMAT_VERSION
            and early.get("mef3io_version") == _version()
            and early.get("session_key") == _session_key(session_path)
            and set(early) == {"format", "mef3io_version", "session_key",
                               "not_worth_caching"}):
        return early
    try:
        current = signature(session_path)
    except OSError:
        return None
    if current.get("encrypted"):
        # Checked FIRST, before format or version: encrypted sessions are
        # never cached, and a snapshot found for one is deleted whatever wrote
        # it — including the format-4 snapshots of mef3io 1.1.x, which held
        # decrypted metadata in plaintext.
        if _looks_like_ours(early):
            discard(cache_path)
        return None
    snapshot = early
    if not isinstance(snapshot, dict) or is_marker(snapshot):
        return None  # unreadable, or a marker of another build/path
    if snapshot.get("format") != CACHE_FORMAT_VERSION:
        return None
    if snapshot.get("mef3io_version") != _version():
        return None  # only the exact build that wrote a snapshot may read it
    if snapshot.get("session_key") != _session_key(session_path):
        return None
    if snapshot.get("signature") != current:
        return None
    return snapshot
