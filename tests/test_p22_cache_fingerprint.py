"""P22: validating a warm-start snapshot must be cheaper than a plain open.

Issue #64: the snapshot cache made opening a DIRECTORY session ~2.7x slower
than not using it, because validation SHA-256-hashed every `.tmet` and the head
of every `.tidx` — more I/O than the open it was meant to skip. Validation is
now stat-only (size, mtime, ctime, inode) for files that had settled when the
snapshot was taken, with a content signature kept only for files modified
within `RACY_WINDOW_NS` of it (git's "racy clean" rule). On POSIX an unchanged
set of directory stats also stands in for listing the tree.

What must NOT be lost is the reason the content signature existed: a repair can
rewrite a fixed-size `.tmet` (temp file + rename) or patch a `.tidx` header in
place without changing the size, and on a coarse filesystem without changing
mtime. Each of those shapes is pinned here against a SETTLED snapshot — the one
that no longer reads content. Oracle-free on purpose, so it runs on every CI
platform including the wheel smoke tests' environments.
"""
from __future__ import annotations

import json
import os
import sys
import time
from pathlib import Path

import numpy as np
import pytest

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO))

import mef3io  # noqa: E402
from mef3io import cache as C  # noqa: E402

START = 1577836800000000
FS = 256.0
HOUR_NS = 3600 * 10**9


def _write(path: Path, new_segment: bool = False, start=START):
    x = 40 * np.sin(np.arange(2000) / 20)
    with mef3io.Writer(str(path), overwrite=not path.exists()) as w:
        w.write("ch1", x, start, FS, 3, new_segment=new_segment)


def _files(path: Path, suffix: str):
    return sorted(path.glob(f"*.timd/*.segd/*{suffix}"))


def _age(path: Path):
    """Move every fingerprinted file's mtime an hour into the past, so a
    later in-place write is bound to change it."""
    for f in _files(path, ".tmet") + _files(path, ".tidx"):
        st = f.stat()
        os.utime(f, ns=(st.st_atime_ns, st.st_mtime_ns - HOUR_NS))


@pytest.fixture
def settled(monkeypatch):
    """Treat every file as settled: snapshots carry no content signatures, so
    whatever is detected below is detected from the stat alone."""
    monkeypatch.setattr(C, "RACY_WINDOW_NS", 0)


def _no_hashing(monkeypatch):
    def boom(*a, **k):
        raise AssertionError("validation read file content")
    monkeypatch.setattr(C, "_hash_bytes", boom)


def _snapshot(tmp_path, path):
    cache = tmp_path / "snap.mef3cache"
    mef3io.Reader(str(path), cache=str(cache))
    assert C.load_valid(cache, str(path)) is not None
    return cache


def test_settled_snapshot_is_validated_without_reading_content(tmp_path, monkeypatch, settled):
    path = tmp_path / "s.mefd"
    _write(path)
    cache = _snapshot(tmp_path, path)
    stored = json.loads(cache.read_text())["fingerprints"]["files"]
    assert stored and all(len(fp) == 4 for fp in stored.values()), stored

    _no_hashing(monkeypatch)
    warm = mef3io.Reader(str(path), cache=str(cache))
    assert warm._impl is None, "the open was not served from the snapshot"


def test_fresh_files_carry_a_signature_until_they_settle(tmp_path, monkeypatch):
    """Written just now, the session is racy: a second write in the same
    timestamp tick would be invisible to stat, so the snapshot signs content.
    Once the window has passed, a load that finds nothing changed drops the
    signatures, and later loads are stat-only."""
    path = tmp_path / "s.mefd"
    _write(path)
    cache = _snapshot(tmp_path, path)
    stored = json.loads(cache.read_text())["fingerprints"]["files"]
    assert all(len(fp) == 5 for fp in stored.values()), stored

    monkeypatch.setattr(C, "RACY_WINDOW_NS", 0)  # time has passed
    assert C.load_valid(cache, str(path)) is not None
    stored = json.loads(cache.read_text())["fingerprints"]["files"]
    assert all(len(fp) == 4 for fp in stored.values()), "the settled snapshot was not re-saved"

    _no_hashing(monkeypatch)
    assert C.load_valid(cache, str(path)) is not None


def test_racy_signature_is_actually_checked(tmp_path):
    """The racy entries are the ONLY guard against a same-tick rewrite, so a
    signature that no longer matches must reject the snapshot even though
    every stat still does."""
    path = tmp_path / "s.mefd"
    _write(path)
    cache = _snapshot(tmp_path, path)
    snap = json.loads(cache.read_text())
    rel = next(k for k in snap["fingerprints"]["files"] if k.endswith(".tmet"))
    assert len(snap["fingerprints"]["files"][rel]) == 5
    snap["fingerprints"]["files"][rel][4] = "0" * 16
    cache.write_text(json.dumps(snap))
    assert C.load_valid(cache, str(path)) is None


def test_in_place_header_patch_invalidates_a_settled_snapshot(tmp_path, settled):
    """The shape of a `.tidx` universal-header repair: same file, same size,
    bytes patched in place. mtime moves, on every platform."""
    path = tmp_path / "s.mefd"
    _write(path)
    _age(path)
    cache = _snapshot(tmp_path, path)

    tidx = _files(path, ".tidx")[0]
    size = tidx.stat().st_size
    with open(tidx, "r+b") as f:
        f.seek(100)
        b = f.read(1)
        f.seek(100)
        f.write(bytes([b[0] ^ 0x01]))
    assert tidx.stat().st_size == size
    assert C.load_valid(cache, str(path)) is None


def test_replaced_tmet_with_restored_mtime_invalidates_a_settled_snapshot(tmp_path, settled):
    """The shape of a `.tmet` repair: a same-size temp file renamed over the
    original. Even with size AND mtime made identical, the name now points at
    a different file (inode / NTFS file ID)."""
    path = tmp_path / "s.mefd"
    _write(path)
    _age(path)
    cache = _snapshot(tmp_path, path)

    tmet = _files(path, ".tmet")[0]
    st = tmet.stat()
    raw = bytearray(tmet.read_bytes())
    raw[-1] ^= 0x01
    tmp = tmet.with_name(tmet.name + ".tmp")
    tmp.write_bytes(raw)
    os.replace(tmp, tmet)
    os.utime(tmet, ns=(st.st_atime_ns, st.st_mtime_ns))
    assert tmet.stat().st_size == st.st_size
    assert tmet.stat().st_mtime_ns == st.st_mtime_ns
    assert C.load_valid(cache, str(path)) is None


@pytest.mark.skipif(os.name == "nt", reason="Windows st_ctime is the creation time, "
                    "which an in-place write does not move; there a settled snapshot "
                    "relies on mtime (see cache.py)")
def test_in_place_rewrite_with_restored_mtime_is_caught_by_ctime(tmp_path, settled):
    """Same file, same size, mtime put back by hand: only ctime moved, and
    ctime cannot be set from user space."""
    path = tmp_path / "s.mefd"
    _write(path)
    _age(path)
    cache = _snapshot(tmp_path, path)

    time.sleep(1.1)  # past any filesystem's ctime granularity
    tmet = _files(path, ".tmet")[0]
    st = tmet.stat()
    raw = bytearray(tmet.read_bytes())
    raw[-1] ^= 0x01
    with open(tmet, "r+b") as f:
        f.write(raw)
    os.utime(tmet, ns=(st.st_atime_ns, st.st_mtime_ns))
    assert tmet.stat().st_ino == st.st_ino
    assert tmet.stat().st_mtime_ns == st.st_mtime_ns
    assert C.load_valid(cache, str(path)) is None


def test_new_segment_invalidates_an_explicit_snapshot(tmp_path, settled):
    """A Writer only removes the AUTO cache; a snapshot at a path it cannot
    know about has to notice the new files itself."""
    path = tmp_path / "s.mefd"
    _write(path)
    cache = _snapshot(tmp_path, path)
    _write(path, new_segment=True, start=START + 60 * 10**6)
    assert len(_files(path, ".tmet")) == 2
    assert C.load_valid(cache, str(path)) is None


def _no_listing(monkeypatch):
    def boom(*a, **k):
        raise AssertionError("validation listed a directory")
    monkeypatch.setattr(C.os, "scandir", boom)


@pytest.mark.skipif(not C._TRUST_DIR_STAT, reason="the tree is always listed on Windows")
def test_settled_unchanged_tree_is_not_listed(tmp_path, monkeypatch, settled):
    path = tmp_path / "s.mefd"
    _write(path)
    _write(path, new_segment=True, start=START + 60 * 10**6)
    cache = _snapshot(tmp_path, path)
    dirs = json.loads(cache.read_text())["fingerprints"]["dirs"]
    # the root, the channel and its two segments
    assert len(dirs) == 4 and all(len(fp) == 3 for fp in dirs.values()), dirs

    _no_listing(monkeypatch)
    _no_hashing(monkeypatch)
    warm = mef3io.Reader(str(path), cache=str(cache))
    assert warm._impl is None


def test_racy_directory_is_listed_until_it_settles(tmp_path, monkeypatch):
    path = tmp_path / "s.mefd"
    _write(path)
    cache = _snapshot(tmp_path, path)
    dirs = json.loads(cache.read_text())["fingerprints"]["dirs"]
    assert all(fp[-1] == "racy" for fp in dirs.values()), dirs

    monkeypatch.setattr(C, "RACY_WINDOW_NS", 0)  # time has passed
    assert C.load_valid(cache, str(path)) is not None
    dirs = json.loads(cache.read_text())["fingerprints"]["dirs"]
    assert all(len(fp) == 3 for fp in dirs.values()), dirs


def test_a_temp_file_that_came_and_went_keeps_the_snapshot(tmp_path, monkeypatch, settled):
    """A directory stat that moved is a reason to LIST, not to invalidate: if
    the listing still matches, the snapshot stands and remembers the new stat."""
    path = tmp_path / "s.mefd"
    _write(path)
    cache = _snapshot(tmp_path, path)
    seg = _files(path, ".tmet")[0].parent
    time.sleep(1.1)  # past any filesystem's directory timestamp granularity
    (seg / "scratch.tmp").write_bytes(b"x")
    (seg / "scratch.tmp").unlink()
    assert C.load_valid(cache, str(path)) is not None
    if C._TRUST_DIR_STAT:
        _no_listing(monkeypatch)
        assert C.load_valid(cache, str(path)) is not None


def test_new_channel_invalidates_an_explicit_snapshot(tmp_path, settled):
    path = tmp_path / "s.mefd"
    _write(path)
    cache = _snapshot(tmp_path, path)
    x = 40 * np.sin(np.arange(2000) / 20)
    with mef3io.Writer(str(path)) as w:
        w.write("ch2", x, START, FS, 3)
    assert C.load_valid(cache, str(path)) is None


def test_removed_segment_invalidates_a_snapshot(tmp_path, settled):
    path = tmp_path / "s.mefd"
    _write(path)
    _write(path, new_segment=True, start=START + 60 * 10**6)
    cache = _snapshot(tmp_path, path)
    for f in _files(path, ".tidx")[-1:]:
        f.unlink()
    assert C.load_valid(cache, str(path)) is None


def test_missing_session_with_cache_raises_like_without(tmp_path):
    path = tmp_path / "nope.mefd"
    cache = tmp_path / "snap.mef3cache"
    with pytest.raises(Exception) as plain:
        mef3io.Reader(str(path))
    with pytest.raises(type(plain.value)):
        mef3io.Reader(str(path), cache=str(cache))
    assert not cache.exists()


def test_old_format_snapshot_is_rebuilt(tmp_path):
    path = tmp_path / "s.mefd"
    _write(path)
    cache = _snapshot(tmp_path, path)
    snap = json.loads(cache.read_text())
    snap["format"] = C.CACHE_FORMAT_VERSION - 1
    cache.write_text(json.dumps(snap))
    assert C.load_valid(cache, str(path)) is None
    r = mef3io.Reader(str(path), cache=str(cache))
    assert r._impl is not None
    assert json.loads(cache.read_text())["format"] == C.CACHE_FORMAT_VERSION
