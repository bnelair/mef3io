"""P6 gate: parallel decode/encode is deterministic (results independent of
thread count) and still cross-compatible with pymef."""
import glob
import warnings

import numpy as np
import pytest

from mef3io import _mef3io as m

warnings.filterwarnings("ignore")
START = 1577836800000000
FS = 1000.0


def _signal(n=200000):
    return 200 * np.sin(np.arange(n) / 50) + np.random.default_rng(0).normal(0, 5, n)


def _write(path, threads, data):
    w = m.SessionWriter(path, True)
    w.set_threads(threads)
    w.write_float("ch1", np.ascontiguousarray(data), START, FS, 2)
    del w


def _tdat_blocks(path):
    return open(glob.glob(path + "/ch1.timd/*/*.tdat")[0], "rb").read()[1024:]


def _tidx_body(path):
    return open(glob.glob(path + "/ch1.timd/*/*.tidx")[0], "rb").read()[1024:]


@pytest.mark.parametrize("threads", [1, 2, 4, 8])
def test_encode_byte_identical_across_threads(tmp_path, threads):
    data = _signal()
    ref = str(tmp_path / "ref.mefd")
    _write(ref, 1, data)
    other = str(tmp_path / f"t{threads}.mefd")
    _write(other, threads, data)
    # RED block payloads and index entries must be identical regardless of
    # thread count (only the random per-file UUIDs in the header differ).
    assert _tdat_blocks(other) == _tdat_blocks(ref)
    assert _tidx_body(other) == _tidx_body(ref)


def test_decode_deterministic_across_threads(tmp_path):
    data = _signal()
    path = str(tmp_path / "s.mefd")
    _write(path, 0, data)
    r = m.Reader(path, "")
    base = np.asarray(r.read_raw("ch1", n_threads=1)["samples"])
    for threads in (2, 4, 8, 0):
        got = np.asarray(r.read_raw("ch1", n_threads=threads)["samples"])
        assert np.array_equal(got, base)


def test_threaded_write_readable_by_pymef(tmp_path):
    pytest.importorskip("pymef")
    from pymef.mef_session import MefSession

    data = _signal(50000)
    path = str(tmp_path / "s.mefd")
    _write(path, 8, data)
    ms = MefSession(path, None, True)
    try:
        got = np.asarray(ms.read_ts_channels_sample(["ch1"], [0, len(data)])[0]).astype(np.int64)
        assert np.array_equal(got, np.round(data * 100).astype(np.int64))
    finally:
        ms.close()


def _block_lengths(path):
    raw = open(glob.glob(path + "/ch1.timd/*/*.tidx")[0], "rb").read()
    uh, ent = 1024, 56
    n = (len(raw) - uh) // ent
    return [int.from_bytes(raw[uh + i * ent + 24: uh + i * ent + 28], "little") for i in range(n)]


def _shift_block_times(path, shifts_samples):
    """Move block i's stored start time by shifts_samples[i] on the time axis.

    Blocks written by mef3io start exactly on the sampling grid, so they tile
    the output without overlap. Foreign writers carry acquisition jitter and
    per-block microsecond rounding, so a block can begin a few samples before
    the previous one ends and the two claim the same output samples. Times are
    stored NEGATED on disk, so moving a block later subtracts from the stored
    value.

    BOTH COPIES MOVE. A block's start time is stored twice — in the `.tidx`
    entry and in the RED block header at the head of the block — and the reader
    places by the header (see BlockCopyMismatch in session.hpp). This helper
    used to edit the index alone, which did not produce a jittered file at all:
    it produced an INCONSISTENT one, where the copy mef3io read had moved and
    the copy pymef read had not. That is what made the two libraries look like
    they disagreed about layout (issue #11) when they do not. Editing the
    header means re-sealing the per-block CRC over [4, block_bytes), which the
    decoder verifies.
    """
    tidx = glob.glob(path + "/ch1.timd/*/*.tidx")[0]
    tdat = tidx[:-5] + ".tdat"
    raw = bytearray(open(tidx, "rb").read())
    dat = bytearray(open(tdat, "rb").read())
    uh, ent = 1024, 56
    n = (len(raw) - uh) // ent
    assert len(shifts_samples) == n, (len(shifts_samples), n)
    for i in range(n):
        off = uh + i * ent
        shift_us = int(round(shifts_samples[i] * 1e6 / FS))

        stored = int.from_bytes(raw[off + 8: off + 16], "little", signed=True)
        raw[off + 8: off + 16] = (stored - shift_us).to_bytes(8, "little", signed=True)

        # The header's own copy, at block_offset + 40, then re-seal the block.
        boff = int.from_bytes(raw[off: off + 8], "little", signed=True)
        bbytes = int.from_bytes(raw[off + 28: off + 32], "little")
        hstored = int.from_bytes(dat[boff + 40: boff + 48], "little", signed=True)
        dat[boff + 40: boff + 48] = (hstored - shift_us).to_bytes(8, "little", signed=True)
        crc = m.crc32(bytes(dat[boff + 4: boff + bbytes]))
        dat[boff: boff + 4] = crc.to_bytes(4, "little")

    open(tidx, "wb").write(bytes(raw))
    open(tdat, "wb").write(bytes(dat))


def _assert_thread_invariant(path, threads=(2, 3, 4, 8, 16, 0), repeats=3):
    r = m.Reader(path, "")
    base = r.read_raw("ch1", n_threads=1)
    samples, valid = np.asarray(base["samples"]), np.asarray(base["valid"])
    for t in threads:
        for _ in range(repeats):  # a race need not show on the first read
            got = r.read_raw("ch1", n_threads=t)
            assert np.array_equal(np.asarray(got["samples"]), samples), t
            assert np.array_equal(np.asarray(got["valid"]), valid), t


def test_decode_deterministic_when_blocks_overlap_the_grid(tmp_path):
    # Regression: blocks whose start times sit off the sampling grid claim
    # overlapping output samples. Workers used to write those concurrently, so
    # the decoded VALUES varied with thread count and scheduling — silently.
    path = str(tmp_path / "s.mefd")
    _write(path, 1, _signal(120000))
    lens = _block_lengths(path)
    assert len(lens) > 4, "need several blocks for overlaps to arise"
    pattern = (-9, 4, -13, 7, -2, 11)
    _shift_block_times(path, [0] + [pattern[i % len(pattern)] for i in range(len(lens) - 1)])
    _assert_thread_invariant(path)


def test_decode_deterministic_when_a_block_is_shadowed(tmp_path):
    # The overlap resolution must survive a short block landing entirely inside
    # its predecessor's span: the earlier block then owns output samples on BOTH
    # sides of it, so clipping each block at the next block's start is wrong.
    rng = np.random.default_rng(5)
    path = str(tmp_path / "s.mefd")
    w = m.SessionWriter(path, True)
    t, us, gap = START, 1e6 / FS, 100
    for _ in range(12):
        for npt in (9000, 700):  # a small gap keeps each piece its own block
            w.write_float("ch1", np.ascontiguousarray(rng.normal(0, 50, npt)), t, FS, 2)
            t += int(round((npt + gap) * us))
    del w

    lens = _block_lengths(path)
    assert lens == [9000, 700] * 12, lens
    # Pull every short block back so it starts 800 samples before the preceding
    # long block ends and finishes 100 samples before it: fully shadowed.
    _shift_block_times(path, [0 if i % 2 == 0 else -(gap + 800) for i in range(len(lens))])
    _assert_thread_invariant(path)


def test_decode_when_a_later_block_swallows_an_earlier_one(tmp_path):
    # The mirror of the case above: a LATER block pulled back far enough to
    # cover an earlier one outright. The earlier block then owns no output at
    # all and read_raw skips decoding it, so the samples in its range must come
    # from the later block — what a serial front-to-back scatter leaves there.
    data = _signal(120000)
    path = str(tmp_path / "s.mefd")
    _write(path, 1, data)
    lens = _block_lengths(path)
    assert len(lens) > 4 and len(set(lens)) == 1, lens
    span = lens[0]
    assert len(lens) * span == len(data), (len(lens), span)
    # Pull every odd block back one whole block: it then spans exactly its
    # predecessor's range, leaving the predecessor with nothing to write.
    _shift_block_times(path, [0 if i % 2 == 0 else -span for i in range(len(lens))])

    quant = np.round(data * 100).astype(np.int64)
    got = np.asarray(m.Reader(path, "").read_raw("ch1", n_threads=1)["samples"]).astype(np.int64)
    for i in range(1, len(lens), 2):  # each swallowed block's range
        lo = (i - 1) * span
        assert np.array_equal(got[lo:lo + span], quant[i * span:(i + 1) * span]), i
    _assert_thread_invariant(path)
