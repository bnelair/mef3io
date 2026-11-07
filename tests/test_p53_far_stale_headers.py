"""A header time can be stale or late by ANY amount (#146): no copy, plan or
verification may skip a part from its .tmet start/end alone. A one-part channel
holding three days whose END header is ~3 days stale (or whose START is > 2 days
late) must be copied sample for sample, and a verification must fail when a
channel is missing (2.3.1 review round 1)."""
import glob
import os
import shutil
import struct

import numpy as np
import pytest

import mef3io
from mef3io import _mef3io as m
from mef3io import merge as mg
from mef3io import transform as tr

S = 1_000_000
H = 3600 * S
D = 24 * H
T = 1_700_000_000_000_000
N = 3 * 86400  # 1 Hz for three days


def _set_header_times(tmet, start=None, end=None):
    raw = bytearray(open(tmet, "rb").read())
    for off, v in ((16, start), (24, end)):
        if v is None:
            continue
        old = struct.unpack_from("<q", raw, off)[0]
        struct.pack_into("<q", raw, off, -v if old < 0 else v)
    struct.pack_into("<I", raw, 0, m.crc32(bytes(raw[4:1024])))
    open(tmet, "wb").write(bytes(raw))


def _session(path, mode, healthy=False):
    x = (np.arange(N) % 200 - 100).astype(float)
    with mef3io.Writer(str(path), overwrite=True) as w:
        w.write("ch", x, T, 1.0, precision=0)
        if healthy:
            w.write("good", x[:5000], T + D, 1.0, precision=0)
    tmet = glob.glob(f"{path}/ch.timd/*.segd/*.tmet")[0]
    if mode == "stale":  # END days behind the data
        _set_header_times(tmet, end=T + H)
    elif mode == "late":  # START after the whole recording
        _set_header_times(tmet, start=T + 3 * D + 100 * S)
    return str(path)


CASES = [("stale", (T + 2 * D + 12 * H, T + 2 * D + 13 * H)),
         ("late", (T + 12 * H, T + 13 * H))]


def _want(src, win):
    want = mef3io.Reader(src).read("ch", *win)
    assert len(want) == 3600 and np.isfinite(want).all()
    return want


@pytest.mark.parametrize("mode,win", CASES)
def test_transform_check_and_merge_keep_every_sample(tmp_path, mode, win):
    src = _session(tmp_path / "s.mefd", mode)
    other = str(tmp_path / "o.mefd")
    with mef3io.Writer(other, overwrite=True) as w:
        w.write("ch", np.ones(100), T + 4 * D, 1.0, precision=0)
    want = _want(src, win)
    assert tr.check(src, str(tmp_path / "c.mefd"), time_range=win).ok
    rep = tr.transform_session(src, str(tmp_path / "t.mefd"), time_range=win)
    assert rep.verification.ok
    assert np.array_equal(mef3io.Reader(str(tmp_path / "t.mefd")).read("ch", *win), want)
    assert mg.check([src, other], str(tmp_path / "c2.mefd"), time_ranges=[win, None]).ok
    rep = mef3io.merge_sessions([src, other], str(tmp_path / "m.mefd"), time_ranges=[win, None])
    assert rep.verification.ok
    assert np.array_equal(mef3io.Reader(str(tmp_path / "m.mefd")).read("ch", *win), want)


@pytest.mark.parametrize("mode,win", CASES)
def test_stale_channel_beside_a_healthy_one_is_not_dropped(tmp_path, mode, win):
    src = _session(tmp_path / "s.mefd", mode, healthy=True)
    want = _want(src, win)
    both = (min(win[0], T + D), max(win[1], T + D + 5000 * S))
    dst = str(tmp_path / "t.mefd")
    rep = tr.transform_session(src, dst, time_range=both)
    assert rep.verification.ok
    r = mef3io.Reader(dst)
    assert set(r.channels) == {"ch", "good"}
    assert np.array_equal(r.read("ch", *win), want)


def _mentions_ch(rep):
    return [i for i in (*rep.problems, *rep.notes) if i.location == "ch"]


@pytest.mark.parametrize("mode,win", CASES)
def test_verify_judges_a_missing_channel_by_the_index(tmp_path, mode, win):
    # The copy lacks the stale channel. A standalone verify cannot know whether
    # that was asked for, so it reports the channel (it has samples in the
    # window by its INDEX; the header says it has none); inside a run the
    # same judgement is a problem (the pre-flight's own left-out list).
    src = _session(tmp_path / "s.mefd", mode, healthy=True)
    both = (min(win[0], T + D), max(win[1], T + D + 5000 * S))
    dst = str(tmp_path / "t.mefd")
    tr.transform_session(src, dst, time_range=both)
    shutil.rmtree(f"{dst}/ch.timd")
    rep = tr.verify(src, dst, time_range=both)
    assert any("has samples" in i.message for i in _mentions_ch(rep))
    other = str(tmp_path / "o.mefd")
    with mef3io.Writer(other, overwrite=True) as w:
        w.write("o", np.ones(100), T + 4 * D, 1.0, precision=0)
    mdst = str(tmp_path / "m.mefd")
    mef3io.merge_sessions([src, other], mdst, time_ranges=[both, None])
    shutil.rmtree(f"{mdst}/ch.timd")
    mrep = mg.verify([src, other], mdst, time_ranges=[both, None])
    assert not mrep.ok  # (a merge result must hold every channel)

