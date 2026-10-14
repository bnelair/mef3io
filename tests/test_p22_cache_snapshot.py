"""P22: a warm-start snapshot must be small and cheaper than the open it skips.

Issue #64: the snapshot cache made opening a DIRECTORY session ~2.7x slower
than not using it, because validation SHA-256-hashed every `.tmet` and the head
of every `.tidx` — more I/O than the plain open. A snapshot only makes sense if
it is small and cheaper than reading the original, so both are pinned here:

- its size does not grow with the number of segments;
- checking it reads fewer bytes AND opens fewer files than a plain open;
- a freshly written session is not slower to open with `cache=` than without.

Validation is O(1) per channel: the segment count, start/end from the first and
last segment's `.tmet` universal header, the last segment's `.tdat`/`.tidx`
sizes and its provenance region; plus the channel folders and session-level
record sizes. Each of those is pinned below, and so is the documented BLIND
SPOT (an in-place edit of a middle segment, by any tool including mef3io's own
`repair_session`), on purpose: it is a design decision, not an accident.

Writers are cache-unaware; validity is decided only when a cached open happens.
Oracle-free, so it runs on every CI platform.
"""
from __future__ import annotations

import builtins
import io
import json
import os
import struct
import sys
import time
from pathlib import Path

import numpy as np
import pytest

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO))

import mef3io  # noqa: E402
from mef3io import _mef3io as m  # noqa: E402
from mef3io import cache as C  # noqa: E402

START = 1577836800000000
FS = 256.0
SEG_SAMPLES = 256
SEG_US = int(SEG_SAMPLES / FS * 1e6)

UH_BYTES = 1024
METADATA_FILE_BYTES = 16384
S2 = 1024 + 1536
MAX_DIFF_BYTES = 6388  # section-2 offset of maximum_difference_bytes (ui4)
PROV_COUNT = 960 + 52  # provenance modification count (ui4 LE)


def _write(path: Path, channels=("ch1",), segments=3, first=0):
    """``segments`` one-second segments per channel, appended to ``path``."""
    x = np.arange(SEG_SAMPLES, dtype=np.int32) % 50
    with mef3io.Writer(str(path), overwrite=not path.exists()) as w:
        for ch in channels:
            for s in range(first, first + segments):
                w.write_int32(ch, x, 0.5, START + s * SEG_US, FS, new_segment=True)


def _tmets(path: Path, ch="ch1"):
    return sorted((path / f"{ch}.timd").glob("*.segd/*.tmet"))


def _snapshot(tmp_path, path, name="snap.mef3cache"):
    cache = tmp_path / name
    mef3io.Reader(str(path), cache=str(cache))
    assert C.load_valid(cache, str(path)) is not None
    return cache


def _patch(file: Path, offset: int, fmt: str, value):
    """Foreign-style in-place edit, CRCs left as they were."""
    with open(file, "r+b") as f:
        f.seek(offset)
        f.write(struct.pack(fmt, value))


def _session(tmp_path, **kw):
    path = tmp_path / "s.mefd"
    _write(path, **kw)
    return path


# --- each check catches its case ---------------------------------------------


def test_warm_open_is_served_and_survives_an_unrelated_touch(tmp_path):
    path = _session(tmp_path)
    cache = _snapshot(tmp_path, path)
    for f in _tmets(path):
        os.utime(f, None)  # timestamps are not part of the check
    warm = mef3io.Reader(str(path), cache=str(cache))
    assert warm._impl is None, "the open was not served from the snapshot"


def test_append_to_the_last_segment_is_detected(tmp_path):
    path = _session(tmp_path)
    cache = _snapshot(tmp_path, path)
    x = np.arange(SEG_SAMPLES, dtype=np.int32)
    with mef3io.Writer(str(path)) as w:
        w.write_int32("ch1", x, 0.5, START + 3 * SEG_US, FS)
    assert len(_tmets(path)) == 3, "expected an in-segment append"
    assert C.load_valid(cache, str(path)) is None


def test_new_segment_is_detected(tmp_path):
    path = _session(tmp_path)
    cache = _snapshot(tmp_path, path)
    _write(path, segments=1, first=3)
    assert C.load_valid(cache, str(path)) is None


def test_new_channel_is_detected(tmp_path):
    path = _session(tmp_path)
    cache = _snapshot(tmp_path, path)
    _write(path, channels=("ch2",), segments=1)
    assert C.load_valid(cache, str(path)) is None


def test_removed_channel_is_detected(tmp_path):
    import shutil

    path = _session(tmp_path, channels=("ch1", "ch2"))
    cache = _snapshot(tmp_path, path)
    shutil.rmtree(path / "ch2.timd")
    assert C.load_valid(cache, str(path)) is None


def test_removed_segment_is_detected(tmp_path):
    import shutil

    path = _session(tmp_path)
    cache = _snapshot(tmp_path, path)
    shutil.rmtree(_tmets(path)[1].parent)
    assert C.load_valid(cache, str(path)) is None


def test_end_time_change_is_detected(tmp_path):
    path = _session(tmp_path)
    cache = _snapshot(tmp_path, path)
    last = _tmets(path)[-1]
    end = struct.unpack_from("<q", last.read_bytes(), 24)[0]
    _patch(last, 24, "<q", end - 1)
    assert C.load_valid(cache, str(path)) is None


def test_start_time_change_is_detected(tmp_path):
    path = _session(tmp_path)
    cache = _snapshot(tmp_path, path)
    first = _tmets(path)[0]
    start = struct.unpack_from("<q", first.read_bytes(), 16)[0]
    _patch(first, 16, "<q", start - 1)
    assert C.load_valid(cache, str(path)) is None


def test_provenance_count_change_at_equal_size_is_detected(tmp_path):
    path = _session(tmp_path)
    cache = _snapshot(tmp_path, path)
    last = _tmets(path)[-1]
    raw = last.read_bytes()
    assert raw[960:964] == b"M3IO", "fixture lacks a provenance region"
    sizes = {f: f.stat().st_size for f in last.parent.iterdir()}
    _patch(last, PROV_COUNT, "<I", struct.unpack_from("<I", raw, PROV_COUNT)[0] + 1)
    assert {f: f.stat().st_size for f in last.parent.iterdir()} == sizes
    assert C.load_valid(cache, str(path)) is None


def test_session_level_records_change_is_detected(tmp_path):
    path = _session(tmp_path)
    cache = _snapshot(tmp_path, path)
    with mef3io.Writer(str(path)) as w:
        w.write_annotations([{"time": START, "text": "event"}])
    assert list(path.glob("*.rdat")), "expected session-level records"
    assert C.load_valid(cache, str(path)) is None


def test_version_mismatch_is_rebuilt(tmp_path):
    path = _session(tmp_path)
    cache = _snapshot(tmp_path, path)
    snap = json.loads(cache.read_text())
    assert snap["mef3io_version"] == mef3io.__version__
    snap["mef3io_version"] = "0.0.0-other"
    cache.write_text(json.dumps(snap))
    assert C.load_valid(cache, str(path)) is None
    r = mef3io.Reader(str(path), cache=str(cache))
    assert r._impl is not None, "a foreign-version snapshot was used"
    assert json.loads(cache.read_text())["mef3io_version"] == mef3io.__version__


def test_refused_snapshot_is_replaced_by_a_fresh_one(tmp_path):
    path = _session(tmp_path)
    cache = _snapshot(tmp_path, path)
    _write(path, segments=1, first=3)
    cold = mef3io.Reader(str(path), cache=str(cache))
    assert cold._impl is not None
    assert cold.info("ch1")["n_segments"] == 4
    warm = mef3io.Reader(str(path), cache=str(cache))
    assert warm._impl is None and warm.info("ch1")["n_segments"] == 4


def test_missing_session_with_cache_raises_like_without(tmp_path):
    path = tmp_path / "nope.mefd"
    cache = tmp_path / "snap.mef3cache"
    with pytest.raises(Exception) as plain:
        mef3io.Reader(str(path))
    with pytest.raises(type(plain.value)):
        mef3io.Reader(str(path), cache=str(cache))
    assert not cache.exists()


# --- encrypted sessions are never cached -------------------------------------


def _encrypted_session(tmp_path, segments=2):
    path = tmp_path / "enc.mefd"
    x = np.arange(SEG_SAMPLES, dtype=np.int32) % 50
    md = mef3io.Metadata(subject=mef3io.Subject(name_1="Jane", id="MRN123"))
    with mef3io.Writer(str(path), overwrite=True, password1="pw1", password2="pw2",
                       metadata=md) as w:
        for s in range(segments):
            w.write_int32("ch1", x, 0.5, START + s * SEG_US, FS, new_segment=True)
    return path


def test_encrypted_session_writes_no_snapshot(tmp_path):
    path = _encrypted_session(tmp_path)
    cache = tmp_path / "snap.mef3cache"
    r = mef3io.Reader(str(path), password="pw2", cache=str(cache))
    assert r.info("ch1")["subject_id"] == "MRN123"
    assert not cache.exists()
    assert not list(tmp_path.glob("snap.mef3cache*")), "a temp file was left behind"


def test_planted_snapshot_for_an_encrypted_session_is_ignored_and_deleted(tmp_path):
    path = _encrypted_session(tmp_path)
    with pytest.raises(Exception) as no_pw:
        mef3io.Reader(str(path))
    infos = mef3io.Reader(str(path), password="pw2")._impl
    infos = {c: infos.info(c) for c in infos.channels}
    cache = tmp_path / "snap.mef3cache"
    C.save(cache, C.build_snapshot(str(path), infos))  # as an older version would have
    assert "MRN123" in cache.read_text()

    with pytest.raises(type(no_pw.value)):
        mef3io.Reader(str(path), cache=str(cache))  # no password: must not be served
    assert not cache.exists(), "the planted snapshot was left in place"


def test_planted_snapshot_for_an_encrypted_tar_is_not_served(tmp_path):
    path = _encrypted_session(tmp_path)
    tar = mef3io.archive_session(str(path))
    infos = mef3io.Reader(tar, password="pw2")._impl
    infos = {c: infos.info(c) for c in infos.channels}
    cache = tmp_path / "snap.mef3cache"
    C.save(cache, C.build_snapshot(tar, infos))
    with pytest.raises(Exception):
        mef3io.Reader(tar, cache=str(cache))
    assert not cache.exists()


# --- robustness of the snapshot itself --------------------------------------


def test_signature_is_taken_before_the_metadata_is_read(tmp_path, monkeypatch):
    """A write that lands DURING the cold open (after the backend read the
    metadata) must leave a snapshot that the next load rejects. Taking the
    signature after the read would vouch for content the snapshot never saw."""
    path = _session(tmp_path)
    cache = tmp_path / "snap.mef3cache"
    original = mef3io.Reader._collect_declaration_issues

    def write_during_open(self):
        _write(path, segments=1, first=3)  # lands after the backend's read
        return original(self)

    monkeypatch.setattr(mef3io.Reader, "_collect_declaration_issues", write_during_open)
    r = mef3io.Reader(str(path), cache=str(cache))
    monkeypatch.undo()
    assert r.info("ch1")["n_segments"] == 3  # what the open saw
    assert cache.exists()
    assert C.load_valid(cache, str(path)) is None, "a snapshot vouched for unseen content"


def test_a_failed_save_never_fails_an_open(tmp_path, monkeypatch):
    path = _session(tmp_path)

    def boom(*a, **k):
        raise OSError("disk full")

    monkeypatch.setattr(C, "save", boom)
    r = mef3io.Reader(str(path), cache=str(tmp_path / "snap.mef3cache"))
    assert r.info("ch1")["n_segments"] == 3


def test_concurrent_saves_do_not_share_a_temp_file(tmp_path):
    import threading

    path = _session(tmp_path)
    cache = tmp_path / "snap.mef3cache"
    snap = C.build_snapshot(str(path), {"ch1": {}})
    errors = []

    def worker():
        try:
            for _ in range(50):
                C.save(cache, snap)
        except Exception as e:  # noqa: BLE001
            errors.append(e)

    threads = [threading.Thread(target=worker) for _ in range(4)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    assert not errors, errors
    assert json.loads(cache.read_text())["format"] == C.CACHE_FORMAT_VERSION
    assert not list(tmp_path.glob("snap.mef3cache.*")), "temp files left behind"


# --- the documented blind spot -----------------------------------------------


def test_blind_spot_foreign_edit_of_a_middle_segment(tmp_path):
    """NOT detected, by design: a middle segment changed in place without
    touching the channel's start/end or the last segment. Open without
    ``cache=`` for a full re-read."""
    path = _session(tmp_path)
    cache = _snapshot(tmp_path, path)
    middle = _tmets(path)[1]
    _patch(middle, S2 + MAX_DIFF_BYTES, "<I", 0)
    assert C.load_valid(cache, str(path)) is not None


def test_blind_spot_repair_of_a_middle_segment(tmp_path):
    """Also NOT detected, by design: mef3io's own repair_session on a middle
    segment. Writers do not know about the cache."""
    path = _session(tmp_path)
    middle = _tmets(path)[1]
    raw = bytearray(middle.read_bytes())
    struct.pack_into("<I", raw, S2 + MAX_DIFF_BYTES, 0)
    struct.pack_into("<I", raw, 4, m.crc32(bytes(raw[UH_BYTES:METADATA_FILE_BYTES])))
    struct.pack_into("<I", raw, 0, m.crc32(bytes(raw[4:UH_BYTES])))
    middle.write_bytes(bytes(raw))

    cache = _snapshot(tmp_path, path)
    report = mef3io.repair_session(str(path), ["sizing.difference-bytes"], backup=False)
    assert report.segments_repaired >= 1
    assert C.load_valid(cache, str(path)) is not None


# --- small, and cheaper than the original ------------------------------------


def test_snapshot_size_does_not_grow_with_segments(tmp_path):
    sizes = {}
    for n in (10, 500):
        path = tmp_path / f"s{n}.mefd"
        _write(path, channels=("ch1", "ch2"), segments=n)
        sizes[n] = _snapshot(tmp_path, path, f"snap{n}").stat().st_size
    # Only digit counts in the stored numbers (sample counts, sizes) may differ.
    assert abs(sizes[500] - sizes[10]) < 256, sizes


def _rchar() -> int:
    with open("/proc/self/io") as f:
        for line in f:
            if line.startswith("rchar"):
                return int(line.split()[1])
    raise RuntimeError("rchar not present in /proc/self/io")


class _OpenCounter:
    """Counts every file and directory the Python side opens."""

    def __init__(self, monkeypatch):
        self.n = 0
        for mod, name in ((builtins, "open"), (io, "open"), (os, "open"), (os, "scandir")):
            orig = getattr(mod, name)

            def wrapped(*a, _orig=orig, **k):
                self.n += 1
                return _orig(*a, **k)

            monkeypatch.setattr(mod, name, wrapped)


@pytest.mark.skipif(not os.path.exists("/proc/self/io"), reason="needs /proc/self/io (Linux)")
@pytest.mark.parametrize("segments", [10, 150, 500])
def test_cached_open_does_less_io_than_a_plain_open(tmp_path, monkeypatch, segments):
    path = tmp_path / "s.mefd"
    channels = ("ch1", "ch2")
    _write(path, channels=channels, segments=segments)
    cache = tmp_path / "snap.mef3cache"
    mef3io.Reader(str(path), cache=str(cache))  # build the snapshot

    before = _rchar()
    plain = mef3io.Reader(str(path))
    [plain.info(c) for c in plain.channels]
    plain_bytes = _rchar() - before
    n_tmet = len(channels) * segments
    # A plain open reads every .tmet in full: that is the floor it pays.
    assert plain_bytes >= n_tmet * METADATA_FILE_BYTES, plain_bytes

    counter = _OpenCounter(monkeypatch)
    before = _rchar()
    warm = mef3io.Reader(str(path), cache=str(cache))
    [warm.info(c) for c in warm.channels]
    cached_bytes = _rchar() - before
    monkeypatch.undo()
    assert warm._impl is None, "the cached open built the backend"
    assert cached_bytes < plain_bytes, (cached_bytes, plain_bytes)
    # The plain open opens at least every .tmet; the cached one opens the
    # snapshot, the root and each channel folder, and two headers per channel.
    assert counter.n < n_tmet, (counter.n, n_tmet)


def test_fresh_session_is_not_slower_with_a_cache(tmp_path):
    """Written moments ago and opened at once: the cached path (first open
    builds the snapshot, later ones use it) must not cost more than a plain
    open beyond a small constant."""
    path = tmp_path / "s.mefd"
    _write(path, channels=tuple(f"ch{i}" for i in range(8)), segments=40)
    cache = tmp_path / "snap.mef3cache"

    def timed(**kw):
        t = time.perf_counter()
        r = mef3io.Reader(str(path), **kw)
        [r.info(c) for c in r.channels]
        return time.perf_counter() - t

    timed()  # warm the page cache and lazy imports
    plain = sorted(timed() for _ in range(5))[2]
    first = timed(cache=str(cache))
    warm = sorted(timed(cache=str(cache)) for _ in range(5))[2]
    slack = 0.05  # one JSON read/write, plus CI noise
    assert first <= 1.5 * plain + slack, (first, plain)
    assert warm <= 1.5 * plain + slack, (warm, plain)
