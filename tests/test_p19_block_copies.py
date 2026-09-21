"""P19: a block's start time and sample count are each stored TWICE.

Once in the `.tidx` entry, once in the RED block header at the head of the
block. Nothing in the format keeps them in step, and the two copies are not
equally trustworthy:

  * the header copy is covered by the per-block CRC, verified on every decode;
    the index copy is covered only by the `.tidx` body CRC, which the read path
    never checks;
  * the header copy is the one meflib and pymef place data by.

So mef3io places by the header and uses the index only to SELECT blocks. These
tests pin both halves of that: that a file whose copies AGREE (everything the
legacy stack or mef3io has ever written) is unaffected, and that a file whose
copies DISAGREE reads the way the legacy stack reads it and says so, instead of
silently losing samples.

Issues #11 (start time) and #13 (sample count).
"""
from __future__ import annotations

import glob
import sys
import warnings
from pathlib import Path

import numpy as np
import pytest

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO))

import mef3io  # noqa: E402
from mef3io import _mef3io as m  # noqa: E402

FS = 1000.0
START = 1577836800000000
UH, ENT = 1024, 56


def _write(path, n=60000):
    data = np.arange(n, dtype=np.int32)
    w = m.SessionWriter(str(path), True)
    w.write_int32("ch1", data, 1.0, START, FS)
    del w
    return data


def _files(path):
    tidx = glob.glob(str(path) + "/ch1.timd/*/*.tidx")[0]
    return Path(tidx), Path(tidx[:-5] + ".tdat")


def _n_blocks(tidx: Path) -> int:
    return (tidx.stat().st_size - UH) // ENT


def _edit(path, fn):
    """Hand `fn(raw, dat, i, off, boff, bbytes)` each block, then write back."""
    tidx, tdat = _files(path)
    raw = bytearray(tidx.read_bytes())
    dat = bytearray(tdat.read_bytes())
    for i in range((len(raw) - UH) // ENT):
        off = UH + i * ENT
        boff = int.from_bytes(raw[off:off + 8], "little", signed=True)
        bbytes = int.from_bytes(raw[off + 28:off + 32], "little")
        fn(raw, dat, i, off, boff, bbytes)
    tidx.write_bytes(bytes(raw))
    tdat.write_bytes(bytes(dat))


def _reseal(dat, boff, bbytes):
    """Re-seal a block's CRC after editing its header; the decoder checks it."""
    dat[boff:boff + 4] = m.crc32(bytes(dat[boff + 4:boff + bbytes])).to_bytes(4, "little")


def _shift_both(path, shifts):
    """A genuinely jittered file: move BOTH copies of each block's start time."""
    def fn(raw, dat, i, off, boff, bbytes):
        us = int(round(shifts[i % len(shifts)] * 1e6 / FS))
        stored = int.from_bytes(raw[off + 8:off + 16], "little", signed=True)
        raw[off + 8:off + 16] = (stored - us).to_bytes(8, "little", signed=True)
        h = int.from_bytes(dat[boff + 40:boff + 48], "little", signed=True)
        dat[boff + 40:boff + 48] = (h - us).to_bytes(8, "little", signed=True)
        _reseal(dat, boff, bbytes)
    _edit(path, fn)


# --- the copies agree: nothing changes ---------------------------------------

def test_a_healthy_session_reports_no_mismatch(tmp_path):
    path = tmp_path / "s.mefd"
    data = _write(path)
    with warnings.catch_warnings():
        warnings.simplefilter("error")  # a spurious warning here is a failure
        out = mef3io.Reader(str(path)).read_raw("ch1")
    assert out["block_copy_mismatches"] == []
    assert np.array_equal(np.asarray(out["samples"]), data)


def test_jitter_applied_to_both_copies_is_not_a_mismatch(tmp_path):
    """A real off-grid recording moves both copies together. That is a
    well-formed file, however jittered, and must not be reported."""
    path = tmp_path / "s.mefd"
    _write(path)
    _shift_both(path, [0, -9, 4, -13, 7, -2])
    with warnings.catch_warnings():
        warnings.simplefilter("error")
        out = mef3io.Reader(str(path)).read_raw("ch1")
    assert out["block_copy_mismatches"] == []


# --- #13: the sample counts disagree -----------------------------------------

@pytest.mark.parametrize("factor, label", [(0.5, "understates"), (2.0, "overstates")])
def test_an_index_that_misstates_the_sample_count_loses_nothing(tmp_path, factor, label):
    """The index count used to drive the output partition, so an entry that
    understated its block turned real samples into NaN gaps, and one that
    overstated it masked the preceding block. The header count is used now, so
    neither happens."""
    path = tmp_path / "s.mefd"
    data = _write(path)
    target = 2

    def fn(raw, dat, i, off, boff, bbytes):
        if i != target:
            return
        n = int.from_bytes(raw[off + 24:off + 28], "little")
        raw[off + 24:off + 28] = int(n * factor).to_bytes(4, "little")
    _edit(path, fn)

    with pytest.warns(mef3io.BlockCopyWarning, match="sample count"):
        out = mef3io.Reader(str(path)).read_raw("ch1")

    samples = np.asarray(out["samples"])
    valid = np.asarray(out["valid"]).astype(bool)
    assert valid.all(), f"{label}: {(~valid).sum()} samples were lost to NaN"
    assert np.array_equal(samples, data), f"{label}: wrong values"

    mm = out["block_copy_mismatches"]
    assert len(mm) == 1 and mm[0]["block_index"] == target
    assert mm[0]["index_number_of_samples"] != mm[0]["header_number_of_samples"]
    assert mm[0]["index_start_uutc"] == mm[0]["header_start_uutc"]


# --- #11: the start times disagree -------------------------------------------

def test_an_index_whose_times_drifted_is_reported_and_the_header_wins(tmp_path):
    """Moving the index copy alone does not make a jittered file — it makes an
    inconsistent one. mef3io follows the header, so the data stays where the
    block itself says it is, and the divergence is reported."""
    path = tmp_path / "s.mefd"
    data = _write(path)

    def fn(raw, dat, i, off, boff, bbytes):
        if i == 0:
            return
        us = int(round(-17 * 1e6 / FS))
        stored = int.from_bytes(raw[off + 8:off + 16], "little", signed=True)
        raw[off + 8:off + 16] = (stored - us).to_bytes(8, "little", signed=True)
    _edit(path, fn)

    with pytest.warns(mef3io.BlockCopyWarning, match="start time"):
        out = mef3io.Reader(str(path)).read_raw("ch1")

    # Placement followed the untouched headers, so the data is unmoved.
    assert np.array_equal(np.asarray(out["samples"]), data)
    assert np.asarray(out["valid"]).astype(bool).all()

    mm = out["block_copy_mismatches"]
    tidx, _ = _files(path)
    assert len(mm) == _n_blocks(tidx) - 1
    assert all(x["index_start_uutc"] != x["header_start_uutc"] for x in mm)


def test_the_warning_names_the_segment_and_block(tmp_path):
    """A report that does not say WHERE cannot be acted on."""
    path = tmp_path / "s.mefd"
    _write(path)

    def fn(raw, dat, i, off, boff, bbytes):
        if i == 1:
            raw[off + 24:off + 28] = (123).to_bytes(4, "little")
    _edit(path, fn)

    with pytest.warns(mef3io.BlockCopyWarning) as rec:
        mef3io.Reader(str(path)).read_raw("ch1")
    text = str(rec[0].message)
    assert "block 1" in text and ".tdat" in text
    assert "123" in text  # the index value it disagreed with
