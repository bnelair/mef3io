"""P21: every file records which mef3io version created it and which last wrote it.

The stamp lives in the universal header's 64-byte discretionary region (offset
960), which MEF 3.0 leaves to the writing application: slot [0, 32) is CREATED
BY, slot [32, 64) is LAST WRITTEN BY, each "mef3io <version>" NUL-padded.

What is pinned here:
* a fresh write stamps every file it creates (segment AND record files);
* an append / repair / recovery refreshes LAST WRITTEN BY and keeps CREATED BY;
* a file with no stamp (zeros: meflib, pymef, mef_tools, older mef3io) gains
  only LAST WRITTEN BY — mef3io did not create it, so it does not claim to;
* a region holding anything else is another application's data and is left
  byte-for-byte alone;
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
SLOT = 32
ME = f"mef3io {m.__version__}"
OLD = "mef3io 0.9.0"


def _slot(raw, i):
    s = bytes(raw[960 + i * SLOT: 960 + (i + 1) * SLOT]).split(b"\0", 1)[0].decode()
    return s if s.startswith("mef3io ") else ""


def _stamps(path):
    raw = Path(path).read_bytes()[:UH]
    return _slot(raw, 0), _slot(raw, 1)


def _set_region(path, region: bytes):
    """Overwrite the discretionary region, keeping the header CRC valid. The
    region is inside the universal header, so the body CRC is unaffected."""
    raw = bytearray(Path(path).read_bytes())
    raw[REGION] = region.ljust(64, b"\0")
    struct.pack_into("<I", raw, 0, m.crc32(bytes(raw[4:UH])))
    Path(path).write_bytes(bytes(raw))


def _old_stamp():
    return OLD.encode().ljust(SLOT, b"\0") + OLD.encode().ljust(SLOT, b"\0")


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


def test_a_fresh_write_stamps_every_file(tmp_path):
    path = tmp_path / "s.mefd"
    w = mef3io.Writer(str(path))
    w.write_int32("ch1", np.arange(N, dtype=np.int32), 0.5, START, FS)
    w.write_annotations([{"time": START, "text": "hello"}])
    w.close()

    files = [p for p in Path(path).rglob("*") if p.is_file()]
    assert {p.suffix for p in files} >= {".tmet", ".tidx", ".tdat", ".rdat", ".ridx"}
    for p in files:
        assert _stamps(p) == (ME, ME), p


def test_the_stamp_is_reported_by_the_segment_map(tmp_path):
    path = tmp_path / "s.mefd"
    _write(path)
    seg = mef3io.Reader(str(path)).segments("ch1")[0]
    assert seg["created_by"] == ME
    assert seg["last_written_by"] == ME


def test_an_append_refreshes_last_written_and_keeps_created(tmp_path):
    path = tmp_path / "s.mefd"
    _write(path)
    for f in _segment_files(path):
        _set_region(f, _old_stamp())

    _append(path)

    for f in _segment_files(path):
        assert _stamps(f) == (OLD, ME), f
    seg = mef3io.Reader(str(path)).segments("ch1")[0]
    assert (seg["created_by"], seg["last_written_by"]) == (OLD, ME)


def test_an_unstamped_file_does_not_claim_mef3io_created_it(tmp_path):
    """Zeros are what meflib, pymef, mef_tools and mef3io <= 1.1 write."""
    path = tmp_path / "s.mefd"
    _write(path)
    for f in _segment_files(path):
        _set_region(f, b"")
    assert mef3io.Reader(str(path)).segments("ch1")[0]["created_by"] == ""

    _append(path)

    for f in _segment_files(path):
        assert _stamps(f) == ("", ME), f


def test_another_applications_region_is_left_alone(tmp_path):
    path = tmp_path / "s.mefd"
    _write(path)
    foreign = bytes(range(1, 65))
    for f in _segment_files(path):
        _set_region(f, foreign)

    _append(path)

    for f in _segment_files(path):
        assert Path(f).read_bytes()[REGION] == foreign, f
    seg = mef3io.Reader(str(path)).segments("ch1")[0]
    assert (seg["created_by"], seg["last_written_by"]) == ("", "")


def test_a_repair_refreshes_last_written(tmp_path):
    path = tmp_path / "s.mefd"
    _write(path)
    tmet = _segment_files(path)[0]
    _set_region(tmet, _old_stamp())
    # Break a declaration the validator can repair (offset as in test_p13).
    raw = bytearray(tmet.read_bytes())
    s2 = 1024 + 1536
    struct.pack_into("<I", raw, s2 + 6388, 0)  # maximum_difference_bytes
    struct.pack_into("<I", raw, 4, m.crc32(bytes(raw[UH:16384])))
    struct.pack_into("<I", raw, 0, m.crc32(bytes(raw[4:UH])))
    tmet.write_bytes(bytes(raw))

    report = mef3io.repair_session(str(path), ["sizing.difference-bytes"], backup=False)
    assert report.repaired, report.summary()
    assert _stamps(tmet) == (OLD, ME)


def test_a_recovery_refreshes_last_written(tmp_path):
    path = tmp_path / "s.mefd"
    _write(path, chunks=4)
    tmet, tidx, tdat = _segment_files(path)
    for f in (tidx, tdat):
        _set_region(f, _old_stamp())
    # Index ahead of nothing: drop the last two entries so the data is ahead.
    full = (tidx.stat().st_size - UH) // 56
    raw = bytearray(tidx.read_bytes()[: UH + (full - 2) * 56])
    struct.pack_into("<q", raw, 32, full - 2)
    struct.pack_into("<I", raw, 4, m.crc32(bytes(raw[UH:])))
    struct.pack_into("<I", raw, 0, m.crc32(bytes(raw[4:UH])))
    tidx.write_bytes(bytes(raw))

    done = mef3io.recover_session(str(path), apply=True, backup=False)
    assert done.applied
    for f in (tidx, tdat):
        assert _stamps(f) == (OLD, ME), f


def test_the_legacy_stack_still_reads_a_stamped_session(tmp_path):
    pymef = pytest.importorskip("pymef")
    from pymef.mef_session import MefSession

    path = tmp_path / "s.mefd"
    x = np.arange(N, dtype=np.int32) - N // 2
    w = mef3io.Writer(str(path))
    w.write_int32("ch1", x, 0.5, START, FS)
    w.close()
    assert _stamps(_segment_files(path)[0]) == (ME, ME)

    s = MefSession(str(path), "")
    got = s.read_ts_channels_sample(["ch1"], [0, N])[0]
    np.testing.assert_array_equal(np.asarray(got).astype(np.int64), x.astype(np.int64))
