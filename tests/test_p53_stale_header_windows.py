"""A cut or a windowed merge judges a segment by its blocks, never by its .tmet
start/end time alone (#184; #146 for the reader): a stale END (an interrupted
append, a short-ending writer) or a late START header must not drop the
window's samples. The output holds exactly what Reader.read returns."""
import glob
import struct

import numpy as np
import pytest

import mef3io
from mef3io import _mef3io as m

S = 1_000_000
T = 1_700_000_000_000_000
FS = 1000.0


def _set_header_times(tmet, start=None, end=None):
    """Rewrite the universal header start/end of a .tmet (user uUTC), keeping
    the stored sign convention, and its header CRC."""
    raw = bytearray(open(tmet, "rb").read())
    for off, v in ((16, start), (24, end)):
        if v is None:
            continue
        old = struct.unpack_from("<q", raw, off)[0]
        struct.pack_into("<q", raw, off, -v if old < 0 else v)
    struct.pack_into("<I", raw, 0, m.crc32(bytes(raw[4:1024])))
    open(tmet, "wb").write(bytes(raw))


def _source(path, mode):
    """ch: segment 0 = 0..40 s, segment 1 = 60..70 s; the first segment's
    header says 'stale' (ends at 20 s), 'late' (starts at 30 s, after the
    window) or is 'ok'."""
    rng = np.random.default_rng(5)
    with mef3io.Writer(str(path), overwrite=True) as w:
        w.write("ch", rng.integers(-99, 99, 40000).astype(float), T, FS, precision=0)
        w.write("ch", rng.integers(-99, 99, 10000).astype(float), T + 60 * S, FS,
                precision=0, new_segment=True)
    tmet = sorted(glob.glob(f"{path}/ch.timd/*-000000.segd/*.tmet"))[0]
    if mode == "stale":
        _set_header_times(tmet, end=T + 20 * S)
    elif mode == "late":
        _set_header_times(tmet, start=T + 38 * S, end=T + 39 * S)
    return str(path)


def _other(path):
    with mef3io.Writer(str(path), overwrite=True) as w:
        w.write("ch", np.ones(5000), T + 100 * S, FS, precision=0)
    return str(path)


WIN = (T + 22 * S, T + 35 * S)


def _same_as_source(src, dst):
    want = mef3io.Reader(src).read("ch", *WIN)
    got = mef3io.Reader(dst).read("ch", *WIN)
    assert len(want) == 13000 and np.isfinite(want).all()
    assert got.shape == want.shape and np.array_equal(got, want)


@pytest.mark.parametrize("mode", ["stale", "late"])
@pytest.mark.parametrize("window_first", [True, False])
def test_windowed_merge_keeps_the_window_of_a_stale_header(tmp_path, mode, window_first):
    src = _source(tmp_path / "s.mefd", mode)
    oth = _other(tmp_path / "o.mefd")
    srcs, ranges = ([src, oth], [WIN, None]) if window_first else ([oth, src], [None, WIN])
    rep = mef3io.merge_sessions(srcs, str(tmp_path / "m.mefd"), time_ranges=ranges)
    assert rep.verification.ok
    _same_as_source(src, str(tmp_path / "m.mefd"))


@pytest.mark.parametrize("mode", ["stale", "late"])
def test_unwindowed_merge_keeps_everything(tmp_path, mode):
    src = _source(tmp_path / "s.mefd", mode)
    oth = _other(tmp_path / "o.mefd")
    mef3io.merge_sessions([src, oth], str(tmp_path / "m.mefd"))
    want = mef3io.Reader(src).read("ch", T, T + 71 * S)
    got = mef3io.Reader(str(tmp_path / "m.mefd")).read("ch", T, T + 71 * S)
    assert np.array_equal(got[np.isfinite(got)], want[np.isfinite(want)])


@pytest.mark.parametrize("mode", ["stale", "late"])
def test_transform_with_a_window_keeps_the_window_of_a_stale_header(tmp_path, mode):
    src = _source(tmp_path / "s.mefd", mode)
    rep = mef3io.transform_session(src, str(tmp_path / "t.mefd"), time_range=WIN)
    assert rep.verification.ok
    _same_as_source(src, str(tmp_path / "t.mefd"))


@pytest.mark.parametrize("mode", ["stale", "late"])
def test_anonymize_keeps_everything_of_a_stale_header(tmp_path, mode):
    src = _source(tmp_path / "s.mefd", mode)
    mef3io.anonymize_session(src, str(tmp_path / "a.mefd"))
    _same_as_source(src, str(tmp_path / "a.mefd"))


def test_a_window_outside_every_block_is_still_left_out(tmp_path):
    # the widened span must not keep a segment the blocks do not reach
    src = _source(tmp_path / "s.mefd", "stale")
    oth = _other(tmp_path / "o.mefd")
    with pytest.raises(Exception):
        mef3io.merge_sessions([src, oth], str(tmp_path / "m.mefd"),
                              time_ranges=[(T + 45 * S, T + 55 * S), (T, T + 1 * S)])


def test_stale_end_with_a_zero_count_last_entry_matches_the_reader(tmp_path):
    # the reader leaves a segment with a 0-count index entry open-ended (it is
    # never skipped); the copy engine must judge it the same way (#184 review
    # round 1): no refusal, no loss
    src = _source(tmp_path / "s.mefd", "stale")
    tidx = sorted(glob.glob(f"{src}/ch.timd/*-000000.segd/*.tidx"))[0]
    raw = bytearray(open(tidx, "rb").read())
    n = (len(raw) - 1024) // 56
    struct.pack_into("<I", raw, 1024 + (n - 1) * 56 + 24, 0)
    struct.pack_into("<I", raw, 4, m.crc32(bytes(raw[1024:])))
    struct.pack_into("<I", raw, 0, m.crc32(bytes(raw[4:1024])))
    open(tidx, "wb").write(bytes(raw))
    oth = _other(tmp_path / "o.mefd")
    want = mef3io.Reader(src).read("ch", *WIN)
    mef3io.merge_sessions([src, oth], str(tmp_path / "m.mefd"), time_ranges=[WIN, None])
    got = mef3io.Reader(str(tmp_path / "m.mefd")).read("ch", *WIN)
    assert got.shape == want.shape and np.array_equal(got, want, equal_nan=True)
