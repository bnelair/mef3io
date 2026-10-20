"""Reader.block_envelope: per-block min/max from the block index alone.

* rows match a decode of each block (min/max, start/end times), scaled like
  read();
* a window returns exactly the blocks that overlap it, gaps are absent rows;
* no .tdat byte is read (rchar), and segments outside the window are skipped;
* unset extremes (RED_NAN, as some writers leave them) come back as NaN.
"""
from __future__ import annotations

from pathlib import Path

import numpy as np
import pytest

import mef3io

START = 1_700_000_000_000_000
SECOND = 1_000_000
FS = 256.0


@pytest.fixture(scope="module")
def session(tmp_path_factory):
    """Two segments with a 30 s gap; 10-s blocks; values in mV (ufact 1e-3)."""
    path = tmp_path_factory.mktemp("env") / "env.mefd"
    rng = np.random.default_rng(3)
    x = rng.normal(0, 1, int(60 * FS)).round(3)  # 60 s, precision 3
    y = rng.normal(5, 2, int(40 * FS)).round(3)
    with mef3io.Writer(str(path), overwrite=True) as w:
        w.write("ch", x, START, fs=FS, precision=3)
        w.write("ch", y, START + 90 * SECOND, fs=FS, precision=3, new_segment=True)
    return path, x, y


def test_rows_match_the_decoded_blocks(session):
    path, x, y = session
    with mef3io.Reader(str(path)) as r:
        env = r.block_envelope("ch")
        assert env["start_uutc"].dtype == np.int64 and env["min"].dtype == np.float64
        assert len(env["start_uutc"]) == 6 + 4  # 10-s blocks: 60 s + 40 s
        block = int(10 * FS)
        expect_lo = [b.min() for b in np.split(x, 6)] + [b.min() for b in np.split(y, 4)]
        expect_hi = [b.max() for b in np.split(x, 6)] + [b.max() for b in np.split(y, 4)]
        np.testing.assert_allclose(env["min"], expect_lo, atol=1e-9)
        np.testing.assert_allclose(env["max"], expect_hi, atol=1e-9)
        starts = [START + k * 10 * SECOND for k in range(6)] + \
                 [START + 90 * SECOND + k * 10 * SECOND for k in range(4)]
        np.testing.assert_array_equal(env["start_uutc"], starts)
        np.testing.assert_array_equal(env["end_uutc"] - env["start_uutc"],
                                      round(block * 1e6 / FS))
        # The same numbers a full read gives, block by block.
        for s, e, lo, hi in zip(env["start_uutc"], env["end_uutc"], env["min"], env["max"]):
            v = r.read("ch", int(s), int(e))
            assert np.nanmin(v) == pytest.approx(lo) and np.nanmax(v) == pytest.approx(hi)


def test_a_window_returns_the_overlapping_blocks(session):
    path, _, _ = session
    with mef3io.Reader(str(path)) as r:
        env = r.block_envelope("ch", START + 15 * SECOND, START + 95 * SECOND)
        # blocks [10,20) .. [50,60) of the first segment, [90,100) of the second
        assert list((env["start_uutc"] - START) // SECOND) == [10, 20, 30, 40, 50, 90]
        empty = r.block_envelope("ch", START + 65 * SECOND, START + 85 * SECOND)  # in the gap
        assert len(empty["start_uutc"]) == 0
        with pytest.raises(Exception):
            r.block_envelope("nope")


@pytest.mark.skipif(not Path("/proc/self/io").exists(), reason="needs /proc/self/io")
def test_no_data_byte_is_read(tmp_path):
    path = tmp_path / "big.mefd"
    with mef3io.Writer(str(path), overwrite=True) as w:
        for k in range(40):
            w.write("ch", np.arange(int(60 * FS), dtype=float), START + k * 60 * SECOND, fs=FS,
                    precision=0, new_segment=True)
    tdat = sum(p.stat().st_size for p in path.rglob("*.tdat"))

    def rchar():
        for line in Path("/proc/self/io").read_text().splitlines():
            if line.startswith("rchar:"):
                return int(line.split()[1])

    with mef3io.Reader(str(path)) as r:
        before = rchar()
        env = r.block_envelope("ch")
        whole = rchar() - before
        before = rchar()
        r.block_envelope("ch", START + 5 * 60 * SECOND, START + 6 * 60 * SECOND)
        window = rchar() - before
    assert len(env["start_uutc"]) == 40 * 6
    tidx = sum(p.stat().st_size for p in path.rglob("*.tidx"))
    assert whole <= tidx + 4096, (whole, tidx, tdat)  # the indexes, never the data
    assert window < whole / 10  # only the segments in the window


def test_unset_extremes_are_nan(tmp_path):
    path = tmp_path / "unset.mefd"
    with mef3io.Writer(str(path), overwrite=True) as w:
        w.write("ch", np.arange(int(20 * FS), dtype=float), START, fs=FS, precision=0)
    tidx = next(path.rglob("*.tidx"))
    raw = bytearray(tidx.read_bytes())
    # first entry's max/min (offsets 32 and 36 of the 56-byte entry) -> RED_NAN
    raw[1024 + 32:1024 + 40] = (0x80000000).to_bytes(4, "little") * 2
    tidx.write_bytes(bytes(raw))  # index body CRC is not checked on read
    with mef3io.Reader(str(path)) as r:
        env = r.block_envelope("ch")
    assert np.isnan(env["min"][0]) and np.isnan(env["max"][0])
    assert env["min"][1] == 2560 and env["max"][1] == 5119
