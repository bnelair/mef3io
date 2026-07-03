"""P9: malformed sessions must raise Python exceptions — never crash the
interpreter. Models what flaky network mounts produce: truncated index files,
corrupted block headers."""
import os
import sys
import warnings
from pathlib import Path

import numpy as np
import pytest

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO))
warnings.filterwarnings("ignore")

import mef3io  # noqa: E402
from mef3io import _mef3io as m  # noqa: E402

START = 1577836800000000
FS = 256.0


def _make_session(tmp_path):
    path = str(tmp_path / "s.mefd")
    w = m.SessionWriter(path, True)
    w.write_float("ch1", np.ascontiguousarray(np.sin(np.arange(5000) / 20)), START, FS, 3)
    del w
    return path


def _seg_file(path, ext):
    return Path(path) / "ch1.timd" / "ch1-000000.segd" / f"ch1-000000.{ext}"


def test_truncated_tidx_raises(tmp_path):
    path = _make_session(tmp_path)
    with open(_seg_file(path, "tidx"), "r+b") as f:
        f.truncate(500)  # smaller than the universal header

    r = mef3io.Reader(path)  # open stays lazy and must succeed
    with pytest.raises(RuntimeError, match="truncated"):
        r.read("ch1", START, START + int(1e6))  # the windowed (compat) path
    with pytest.raises(RuntimeError, match="truncated"):
        r.toc("ch1")


def test_tidx_not_multiple_of_entry_raises(tmp_path):
    path = _make_session(tmp_path)
    size = os.path.getsize(_seg_file(path, "tidx"))
    with open(_seg_file(path, "tidx"), "r+b") as f:
        f.truncate(size - 13)  # mid-entry cut

    with pytest.raises(RuntimeError, match="truncated"):
        mef3io.Reader(path).read("ch1")


def test_corrupt_red_counters_raise(tmp_path):
    path = _make_session(tmp_path)
    tdat = _seg_file(path, "tdat")
    with open(tdat, "r+b") as f:
        f.seek(1024 + 28)  # first block's difference_bytes field
        f.write((0xFFFFFFFF).to_bytes(4, "little"))

    # Either the block CRC or the counter sanity check must reject it.
    with pytest.raises(RuntimeError):
        mef3io.Reader(path).read("ch1")


def test_truncated_tdat_raises(tmp_path):
    path = _make_session(tmp_path)
    with open(_seg_file(path, "tdat"), "r+b") as f:
        f.truncate(1500)  # index now points past the end

    with pytest.raises(RuntimeError, match="tdat"):
        mef3io.Reader(path).read("ch1")
