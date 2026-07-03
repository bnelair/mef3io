"""P4 gate: sessions written by the mef3io C++ writer are read back identically
by the legacy pymef stack (and by mef3io itself), across data shapes,
encryption, and fractional sampling rates."""
import warnings

import numpy as np
import pytest
from pymef.mef_session import MefSession

from mef3io import _mef3io as m

START = 1577836800000000


def _write(tmp_path, data, fs=256.0, ufact=0.001, units="uV", block_len=2560, p1="", p2=""):
    path = str(tmp_path / "written.mefd")
    m.write_test_segment(
        path, "ch1", "sess", np.ascontiguousarray(data.astype(np.int32)), START, fs, ufact,
        units, block_len, p1, p2,
    )
    return path


@pytest.mark.parametrize(
    "name,data",
    [
        ("smooth", (60 * np.sin(np.arange(4000) / 22)).astype(np.int32)),
        ("random_small", np.random.default_rng(1).integers(-100, 100, 3000).astype(np.int32)),
        ("random_big", np.random.default_rng(2).integers(-(2**29), 2**29, 1500).astype(np.int32)),
        ("constant", np.full(2600, 5, np.int32)),
        ("ramp", np.arange(-2000, 2000, dtype=np.int32)),
        ("multiblock", (30 * np.sin(np.arange(9000) / 40)).astype(np.int32)),
    ],
)
def test_pymef_reads_written_data(tmp_path, name, data):
    path = _write(tmp_path, data)
    with warnings.catch_warnings():
        warnings.simplefilter("error")  # any CRC warning becomes a failure
        ms = MefSession(path, None, True)
        try:
            got = np.asarray(ms.read_ts_channels_sample(["ch1"], [0, len(data)])[0])
            assert np.array_equal(got.astype(np.int64), data.astype(np.int64))
            bi = ms.read_ts_channel_basic_info()[0]
            assert int(bi["start_time"][0]) == START
            assert int(bi["nsamp"][0]) == len(data)
        finally:
            ms.close()


def test_mef3io_reads_own_written_data(tmp_path):
    data = (50 * np.sin(np.arange(6000) / 30)).astype(np.int32)
    path = _write(tmp_path, data)
    r = m.Reader(path, "")
    raw = r.read_raw("ch1")
    assert np.array_equal(np.asarray(raw["samples"]), data)
    valid = np.asarray(raw["valid"]).astype(bool)
    assert valid.all()  # single contiguous segment, no gaps


def test_encrypted_write_read_by_pymef(tmp_path):
    data = (80 * np.sin(np.arange(3000) / 18)).astype(np.int32)
    path = _write(tmp_path, data, fs=512.5, ufact=0.01, units="mV", p1="pass1", p2="pass2")
    with warnings.catch_warnings():
        warnings.simplefilter("error")
        ms = MefSession(path, "pass2", True)
        try:
            got = np.asarray(ms.read_ts_channels_sample(["ch1"], [0, len(data)])[0])
            assert np.array_equal(got.astype(np.int64), data.astype(np.int64))
            bi = ms.read_ts_channel_basic_info()[0]
            assert float(bi["fsamp"][0]) == pytest.approx(512.5)
            assert float(bi["ufact"][0]) == pytest.approx(0.01)
        finally:
            ms.close()


def test_encrypted_requires_password(tmp_path):
    data = np.arange(500, dtype=np.int32)
    path = _write(tmp_path, data, p1="pass1", p2="pass2")
    # pymef rejects an encrypted session opened without a password at open time.
    with pytest.raises(Exception):
        MefSession(path, None, True)


def test_mef3io_roundtrip_encrypted(tmp_path):
    data = np.random.default_rng(9).integers(-500, 500, 4000).astype(np.int32)
    path = _write(tmp_path, data, p1="pass1", p2="pass2")
    r = m.Reader(path, "pass2")
    assert np.array_equal(np.asarray(r.read_raw("ch1")["samples"]), data)
