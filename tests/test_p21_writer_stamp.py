"""P21: provenance — which mef3io version created each file, which last
modified it, and what was done to it. Versions and operations only: NO TIMES.

Lives in the universal header's 64-byte discretionary region (file offset 960).
The layout is a FROZEN format (docs/mef3_format.md, "Provenance region"):

    off len field
      0   4 magic "M3IO"
      4   1 layout version (1)
      5   1 last operation code
      6   2 reserved
      8  20 created-by version (ASCII, NUL-padded, <= 19 chars + NUL)
     28  20 last-modified-by version
     48   4 operations-ever mask, ui4 LE (bit n = code n)
     52   4 modification count, ui4 LE, SATURATES at 0xFFFFFFFF
     56   8 reserved

Codes: 0 unset, 1 create, 2 append, 3 header-repair, 4 recovery,
5 metadata-update. Append-only.

What is pinned here, on real files (the byte layout itself, saturation and
bounds are pinned in the Catch2 suite):
* a fresh write stamps every file it creates (segment AND record files);
* append / repair / recovery each record their own operation, and the mask
  keeps the history a later operation would hide;
* a long acquisition (thousands of appends) counts exactly, and a count at its
  ceiling stays there with the header CRC — and the session — still valid;
* an unstamped file (zeros: every other writer, mef3io <= 1.1) never gains a
  created-by;
* another application's region is left byte-for-byte alone;
* the legacy stack still reads a stamped session.
"""
from __future__ import annotations

import struct
import sys
from pathlib import Path

import numpy as np
import pytest

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO))

import mef3io  # noqa: E402
from mef3io import _mef3io as m  # noqa: E402

START = 1577836800000000
FS = 256.0
N = 2000
UH = 1024
REGION = slice(960, 1024)
ME = m.__version__
CREATE, APPEND, REPAIR, RECOVERY = 1, 2, 3, 4
COUNT_MAX = 0xFFFFFFFF


def _region(path) -> dict | None:
    r = Path(path).read_bytes()[REGION]
    if r[:4] != b"M3IO":
        return None
    ver = lambda a: r[a:a + 20].split(b"\0", 1)[0].decode("ascii")
    mask, count = struct.unpack_from("<II", r, 48)
    return {"layout": r[4], "last_op": r[5], "created_by": ver(8),
            "modified_by": ver(28), "mask": mask, "count": count}


def _set_region(path, region: bytes):
    """Overwrite the region, keeping the header CRC valid. It lies inside the
    universal header, so the body CRC is unaffected."""
    raw = bytearray(Path(path).read_bytes())
    raw[REGION] = region.ljust(64, b"\0")
    struct.pack_into("<I", raw, 0, m.crc32(bytes(raw[4:UH])))
    Path(path).write_bytes(bytes(raw))


def _patch_region(path, offset, fmt, value):
    raw = bytearray(Path(path).read_bytes())
    struct.pack_into(fmt, raw, 960 + offset, value)
    struct.pack_into("<I", raw, 0, m.crc32(bytes(raw[4:UH])))
    Path(path).write_bytes(bytes(raw))


def _segment_files(path):
    seg = next(Path(path).glob("*.timd/*.segd"))
    return [next(seg.glob(f"*.{ext}")) for ext in ("tmet", "tidx", "tdat")]


def _write(path, chunks=1):
    rng = np.random.default_rng(0)
    w = mef3io.Writer(str(path))
    for i in range(chunks):
        w.write_int32("ch1", rng.integers(-9000, 9000, N, dtype=np.int32), 0.5,
                      START + int(i * N / FS * 1e6), FS)
    w.close()


def _append(path, i=1):
    rng = np.random.default_rng(i)
    w = mef3io.Writer(str(path))
    w.write_int32("ch1", rng.integers(-9000, 9000, N, dtype=np.int32), 0.5,
                  START + int(i * N / FS * 1e6), FS)
    w.close()


def _bit(*codes):
    return sum(1 << c for c in codes)


def test_a_fresh_write_stamps_every_file(tmp_path):
    path = tmp_path / "s.mefd"
    w = mef3io.Writer(str(path))
    w.write_int32("ch1", np.arange(N, dtype=np.int32), 0.5, START, FS)
    w.write_annotations([{"time": START, "text": "hello"}])
    w.close()

    files = [p for p in Path(path).rglob("*") if p.is_file()]
    assert {p.suffix for p in files} >= {".tmet", ".tidx", ".tdat", ".rdat", ".ridx"}
    for p in files:
        assert _region(p) == {"layout": 1, "last_op": CREATE, "created_by": ME,
                              "modified_by": ME, "mask": _bit(CREATE), "count": 0}, p


def test_the_segment_map_reports_provenance(tmp_path):
    path = tmp_path / "s.mefd"
    _write(path)
    _append(path)
    pv = mef3io.Reader(str(path)).segments("ch1")[0]["provenance"]
    assert pv == {"layout_version": 1, "created_by": ME, "last_modified_by": ME,
                  "last_operation": "append", "operations": ["create", "append"],
                  "modification_count": 1}


def test_an_append_records_itself_and_keeps_created(tmp_path):
    path = tmp_path / "s.mefd"
    _write(path)
    for f in _segment_files(path):
        _patch_region(f, 8, "20s", b"0.9.0")  # pretend an older mef3io made it
    _append(path)
    for f in _segment_files(path):
        r = _region(f)
        assert (r["created_by"], r["modified_by"]) == ("0.9.0", ME), f
        assert r["last_op"] == APPEND and r["mask"] == _bit(CREATE, APPEND) and r["count"] == 1


def test_a_long_acquisition_counts_every_append(tmp_path):
    """A month of 10-minute appends is ~4300 of them (the counter logic itself is
    driven through 10,000 and to its ceiling in Catch2). The count must track every
    one, on every file the append touches, without disturbing anything else."""
    path = tmp_path / "s.mefd"
    n_appends = 1000
    block = 256
    w = mef3io.Writer(str(path))  # ONE writer, as an acquisition would keep
    for i in range(n_appends + 1):
        w.write_int32("ch1", np.full(block, i % 100, dtype=np.int32), 1.0,
                      START + int(i * block / FS * 1e6), FS)
    w.close()

    for f in _segment_files(path):
        r = _region(f)
        assert r["count"] == n_appends, (f, r)
        assert r["created_by"] == ME and r["last_op"] == APPEND
        assert r["mask"] == _bit(CREATE, APPEND)
    assert mef3io.Validator(str(path)).validate().ok
    assert int(np.sum(np.isfinite(mef3io.Reader(str(path)).read("ch1")))) == (n_appends + 1) * block


def test_a_count_at_its_ceiling_stays_there_and_the_file_stays_valid(tmp_path):
    path = tmp_path / "s.mefd"
    _write(path)
    for f in _segment_files(path):
        _patch_region(f, 52, "<I", COUNT_MAX - 1)
    _append(path, 1)
    _append(path, 2)
    _append(path, 3)
    for f in _segment_files(path):
        r = _region(f)
        assert r["count"] == COUNT_MAX, f           # saturated, not wrapped to 0
        assert r["mask"] == _bit(CREATE, APPEND)    # neighbours intact
        assert Path(f).read_bytes()[1016:1024] == b"\0" * 8  # reserved tail untouched
    assert mef3io.Validator(str(path)).validate().ok  # header CRCs valid
    assert int(np.sum(np.isfinite(mef3io.Reader(str(path)).read("ch1")))) == 4 * N


def test_an_unstamped_file_does_not_claim_mef3io_created_it(tmp_path):
    """Zeros are what meflib, pymef, mef_tools and mef3io <= 1.1 write."""
    path = tmp_path / "s.mefd"
    _write(path)
    for f in _segment_files(path):
        _set_region(f, b"")
    assert mef3io.Reader(str(path)).segments("ch1")[0]["provenance"] is None

    _append(path)

    for f in _segment_files(path):
        r = _region(f)
        assert r["created_by"] == "" and r["modified_by"] == ME, f
        assert r["mask"] == _bit(APPEND) and r["count"] == 1


def test_another_applications_region_is_left_alone(tmp_path):
    path = tmp_path / "s.mefd"
    _write(path)
    foreign = bytes(range(1, 65))
    for f in _segment_files(path):
        _set_region(f, foreign)

    _append(path)

    for f in _segment_files(path):
        assert Path(f).read_bytes()[REGION] == foreign, f
    assert mef3io.Reader(str(path)).segments("ch1")[0]["provenance"] is None


def test_a_repair_is_recorded_and_survives_a_later_append(tmp_path):
    path = tmp_path / "s.mefd"
    _write(path)
    tmet = _segment_files(path)[0]
    raw = bytearray(tmet.read_bytes())
    struct.pack_into("<I", raw, 1024 + 1536 + 6388, 0)  # maximum_difference_bytes (as test_p13)
    struct.pack_into("<I", raw, 4, m.crc32(bytes(raw[UH:16384])))
    struct.pack_into("<I", raw, 0, m.crc32(bytes(raw[4:UH])))
    tmet.write_bytes(bytes(raw))

    report = mef3io.repair_session(str(path), ["sizing.difference-bytes"], backup=False)
    assert report.repaired, report.summary()
    r = _region(tmet)
    assert r["last_op"] == REPAIR and r["mask"] == _bit(CREATE, REPAIR)

    _append(path)
    pv = mef3io.Reader(str(path)).segments("ch1")[0]["provenance"]
    assert pv["last_operation"] == "append"
    assert pv["operations"] == ["create", "append", "header-repair"]  # history kept
    assert pv["modification_count"] == 2


def test_a_recovery_is_recorded(tmp_path):
    path = tmp_path / "s.mefd"
    _write(path, chunks=4)
    tmet, tidx, tdat = _segment_files(path)
    full = (tidx.stat().st_size - UH) // 56
    raw = bytearray(tidx.read_bytes()[: UH + (full - 2) * 56])  # data ahead of index
    struct.pack_into("<q", raw, 32, full - 2)
    struct.pack_into("<I", raw, 4, m.crc32(bytes(raw[UH:])))
    struct.pack_into("<I", raw, 0, m.crc32(bytes(raw[4:UH])))
    tidx.write_bytes(bytes(raw))

    done = mef3io.recover_session(str(path), apply=True, backup=False)
    assert done.applied
    for f in (tidx, tdat):
        r = _region(f)
        assert r["last_op"] == RECOVERY and r["mask"] & _bit(RECOVERY), f


def test_the_legacy_stack_still_reads_a_stamped_session(tmp_path):
    pytest.importorskip("pymef")
    from pymef.mef_session import MefSession

    path = tmp_path / "s.mefd"
    x = np.arange(N, dtype=np.int32) - N // 2
    w = mef3io.Writer(str(path))
    w.write_int32("ch1", x, 0.5, START, FS)
    w.close()
    _append(path)
    assert _region(_segment_files(path)[0])["count"] == 1

    s = MefSession(str(path), "")
    got = s.read_ts_channels_sample(["ch1"], [0, N])[0]
    np.testing.assert_array_equal(np.asarray(got).astype(np.int64), x.astype(np.int64))
