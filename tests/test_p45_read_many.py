"""P45: multi-channel reads (#90; part of #68) and float32 output.

``Reader.read(channels, t0, t1)`` with a list of names returns ONE 2-D array
(len(channels) x n). Pinned here:

* every row is BYTE-identical to ``read()`` of that channel alone, and to the
  values derived independently from ``read_raw()`` (counts x each span's
  factor, NaN where not valid), on gappy, jittered, overlapping-block,
  multi-segment, mixed-factor and absent channels, over many windows (None
  ends, half-sample ties, outside the data, empty) and at every thread count;
* mixed sampling rates, empty lists, unknown names and bad types are refused
  before anything is read; float ``t0``/``t1`` work as for one channel;
* ``dtype="float32"`` == ``read().astype(float32)`` exactly, for one channel
  and for a list (relative error <= 2**-24 against float64);
* cancel stops the whole call, progress is ONE monotone sequence whose total
  is the sum of the single reads' totals;
* ``UnscaledSamplesWarning`` once per channel and Reader, also for a list;
* ``block_envelope(list)`` = a dict of the single-channel results, index only
  (rchar-pinned like test_p38);
* MultiReader: the same signatures and results as a Reader of the merged
  session (test_p39's contract), lists and dtype included (its thread budget
  for a list read: test_p42);
* the legacy ``MefReader.get_data(list)`` (now one multi-channel read per rate
  and window) returns what the per-channel loop returned.
"""
from __future__ import annotations

import glob
import threading
import warnings
from pathlib import Path

import numpy as np
import pytest

import mef3io
from mef3io import _mef3io as m

SECOND = 1_000_000
START = 1577836800000000
FS = 1000.0
UH, ENT = 1024, 56
UFACT_OFFSET = 1024 + 1536 + 6200
METADATA_FILE_BYTES = 16384


def _quiet(fn, *a, **kw):
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        return fn(*a, **kw)


def _reseal(dat, boff, bbytes):
    dat[boff:boff + 4] = m.crc32(bytes(dat[boff + 4:boff + bbytes])).to_bytes(4, "little")


def _jitter(path, channel, shifts):
    """Move BOTH stored copies of each block's start (index + RED header) by
    `shifts` samples: a jittered recording; negative shifts make blocks
    overlap their predecessor."""
    for tidx in sorted(glob.glob(f"{path}/{channel}.timd/*/*.tidx")):
        tdat = tidx[:-5] + ".tdat"
        raw = bytearray(Path(tidx).read_bytes())
        dat = bytearray(Path(tdat).read_bytes())
        for i in range((len(raw) - UH) // ENT):
            off = UH + i * ENT
            boff = int.from_bytes(raw[off:off + 8], "little", signed=True)
            bbytes = int.from_bytes(raw[off + 28:off + 32], "little")
            us = int(round(shifts[i % len(shifts)] * 1e6 / FS))
            stored = int.from_bytes(raw[off + 8:off + 16], "little", signed=True)
            raw[off + 8:off + 16] = (stored - us).to_bytes(8, "little", signed=True)
            h = int.from_bytes(dat[boff + 40:boff + 48], "little", signed=True)
            dat[boff + 40:boff + 48] = (h - us).to_bytes(8, "little", signed=True)
            _reseal(dat, boff, bbytes)
        Path(tidx).write_bytes(bytes(raw))
        Path(tdat).write_bytes(bytes(dat))


def _set_factor(tmet, value):
    raw = bytearray(Path(tmet).read_bytes())
    import struct
    struct.pack_into("<d", raw, UFACT_OFFSET, value)
    struct.pack_into("<I", raw, 4, m.crc32(bytes(raw[UH:METADATA_FILE_BYTES])))
    struct.pack_into("<I", raw, 0, m.crc32(bytes(raw[4:UH])))
    Path(tmet).write_bytes(bytes(raw))


def _build(path):
    """Channels of one rate, each a different shape the read must get right."""
    rng = np.random.default_rng(45)
    with mef3io.Writer(str(path), overwrite=True, block_length=700) as w:
        # gappy float data over two segments
        x = np.sin(np.arange(20000) / 13.0) * 100
        x[3000:4200] = np.nan
        x[15000:15001] = np.nan
        w.write("gappy", x, START, FS, precision=3)
        w.write("gappy", np.cos(np.arange(5000) / 7.0), START + 25 * SECOND, FS, precision=3,
                new_segment=True)
        # int32 blocks that will be jittered (some overlap their predecessor)
        w.write_int32("jitter", rng.integers(-30000, 30000, 24000).astype(np.int32), 1.0,
                      START + 1500, FS)
        # three segments with different factors (decimal, non-decimal, negative)
        for k, (t, uf) in enumerate([(0, 0.01), (9 * SECOND, 0.195), (17 * SECOND, -0.5)]):
            w.write_int32("mixed", rng.integers(-3000, 3000, 8000).astype(np.int32), uf,
                          START + t, FS, new_segment=k > 0)
        # data only late in the session (absent from early windows)
        w.write("late", rng.normal(size=4000), START + 26 * SECOND, FS, precision=4)
        # a segment whose factor is not set (0): raw counts + a warning
        w.write_int32("unset", np.arange(6000, dtype=np.int32) - 3000, 1.0, START, FS)
    _jitter(path, "jitter", [0, -9, 4, -13, 7, -2, -700, 3])
    for tmet in glob.glob(f"{path}/unset.timd/*/*.tmet"):
        _set_factor(tmet, 0.0)
    return Path(path)


CHANNELS = ["gappy", "jitter", "mixed", "late", "unset"]


@pytest.fixture(scope="module")
def session(tmp_path_factory):
    return _build(tmp_path_factory.mktemp("many") / "many.mefd")


def _windows():
    half = int(1e6 / FS / 2)  # 500 us: a tie on the 1 kHz grid
    w = [(None, None), (START, START + 31 * SECOND),
         (START - 3 * SECOND, START + 2 * SECOND),
         (START + 2 * SECOND + half, START + 9 * SECOND + half),
         (START + half, START + 20 * SECOND - half),
         (START + 40 * SECOND, START + 41 * SECOND),       # after all data
         (START + 5 * SECOND, START + 5 * SECOND),         # empty
         (START + 6 * SECOND, START + 5 * SECOND),         # reversed: empty
         (START + 3.25e6, START + 12.75e6),                # float times
         (None, START + 4 * SECOND), (START + 27 * SECOND, None)]
    rng = np.random.default_rng(3)
    for _ in range(12):
        a = int(START + rng.integers(-2, 30) * SECOND + rng.integers(0, SECOND))
        w.append((a, a + int(rng.integers(1, 12 * SECOND))))
    return w


def _from_raw(r, ch, t0, t1):
    """read()'s values derived from read_raw(): counts x the span's factor."""
    raw = _quiet(r.read_raw, ch, t0, t1)
    out = np.full(len(raw["samples"]), np.nan)
    for sp in raw["units_conversion_factors"]:
        uf = sp["units_conversion_factor"] or 1.0
        a, b = sp["start_index"], sp["end_index"]
        v = raw["valid"][a:b] != 0
        out[a:b][v] = raw["samples"][a:b][v].astype(np.float64) * uf
    return out


def _union(r, names):
    infos = [r.info(c) for c in names]
    return min(i["start_time"] for i in infos), max(i["end_time"] for i in infos)


@pytest.mark.parametrize("n_threads", [1, 2, 3, 8, 0])
def test_rows_are_byte_identical_to_single_reads(session, n_threads):
    with mef3io.Reader(str(session), n_threads=n_threads) as r:
        lists = [CHANNELS, CHANNELS[::-1], ["mixed", "gappy", "mixed"], ["late"]]
        for t0, t1 in _windows():
            for names in lists:
                got = _quiet(r.read, names, t0, t1)
                a, b = _union(r, names)
                a = a if t0 is None else t0
                b = b if t1 is None else t1
                want = np.stack([_quiet(r.read, c, a, b) for c in names])
                assert got.shape == want.shape and got.dtype == np.float64
                assert got.tobytes() == want.tobytes(), (names, t0, t1)
                derived = np.stack([_from_raw(r, c, a, b) for c in names])
                assert np.array_equal(got, derived, equal_nan=True), (names, t0, t1)


def test_the_window_really_exercises_jitter_overlap_and_factors(session):
    """The fixture is what the identity test claims it is."""
    with mef3io.Reader(str(session)) as r:
        toc = r.toc("jitter")
        starts = np.array([e["start_uutc"] for e in toc])
        ends = starts + np.array([e["number_of_samples"] for e in toc]) * 1000
        assert (starts[1:] < ends[:-1]).any()  # blocks overlap
        assert r.info("mixed")["units_conversion_factor"] is None
        x = _quiet(r.read, CHANNELS, START, START + 31 * SECOND)
        assert np.isnan(x[0]).any() and np.isfinite(x[0]).any()  # gaps and data
        assert np.isnan(x[3, :20000]).all()  # 'late' absent early


def test_default_window_is_the_union_and_one_channel_is_unchanged(session):
    with mef3io.Reader(str(session)) as r:
        one = _quiet(r.read, ["gappy"])
        assert one.tobytes() == _quiet(r.read, "gappy")[None, :].tobytes()
        a, b = _union(r, CHANNELS)
        both = _quiet(r.read, CHANNELS)
        assert both.shape[1] == int(round((b - a) * FS / 1e6))
        assert both.tobytes() == _quiet(r.read, CHANNELS, a, b).tobytes()
        assert _quiet(r.read, ("gappy", "late"), START, START + SECOND).shape == (2, 1000)


def test_arguments_are_checked_before_anything_is_read(session, tmp_path):
    other = tmp_path / "rates.mefd"
    with mef3io.Writer(str(other), overwrite=True) as w:
        w.write("a", np.zeros(1000), START, 256.0)
        w.write("b", np.zeros(1000), START, 512.0)
        w.write("c", np.zeros(1000), START, 256.0)
    with mef3io.Reader(str(other)) as r:
        with pytest.raises(ValueError, match="ONE sampling rate.*separate calls"):
            r.read(["a", "c", "b"])
        assert r.read(["a", "c"]).shape[0] == 2  # one rate: fine
        with pytest.raises(ValueError, match="no channels"):
            r.read([])
        with pytest.raises(IndexError, match="no such channel: zz"):
            r.read(["a", "zz", "b"])  # first problem in list order
        with pytest.raises(ValueError, match="ONE sampling rate"):
            r.read(["a", "b", "zz"])
        with pytest.raises(TypeError):
            r.read(["a", 3])
        with pytest.raises(TypeError):
            r.read({"a"})
        with pytest.raises(ValueError, match="float64 or float32"):
            r.read("a", dtype="int32")
        with pytest.raises(TypeError):
            r.read("a", dtype=None)
        # block_envelope takes any mix of rates
        assert set(r.block_envelope(["a", "b"])) == {"a", "b"}


def test_float32_is_the_float64_value_rounded(session):
    with mef3io.Reader(str(session)) as r:
        for t0, t1 in _windows()[:6]:
            for ch in CHANNELS:
                d = _quiet(r.read, ch, t0, t1)
                f = _quiet(r.read, ch, t0, t1, dtype="float32")
                assert f.dtype == np.float32
                assert f.tobytes() == d.astype(np.float32).tobytes()
            d = _quiet(r.read, CHANNELS, t0, t1)
            f = _quiet(r.read, CHANNELS, t0, t1, dtype=np.float32)
            assert f.dtype == np.float32 and f.shape == d.shape
            assert f.tobytes() == d.astype(np.float32).tobytes()
            ok = np.isfinite(d)
            assert np.array_equal(np.isnan(f), np.isnan(d))
            assert np.all(np.abs(f[ok] - d[ok]) <= np.abs(d[ok]) * 2.0 ** -24)


def test_progress_is_one_monotone_sequence(session):
    with mef3io.Reader(str(session), n_threads=4) as r:
        totals = []
        for ch in CHANNELS:
            calls = []
            _quiet(r.read, ch, START, START + 31 * SECOND, progress=lambda d, t: calls.append(t))
            totals.append(calls[-1])
        calls = []
        x = _quiet(r.read, CHANNELS, START, START + 31 * SECOND,
                   progress=lambda d, t: calls.append((d, t)))
        assert calls and calls[0][0] == 0 and calls[-1][0] == calls[-1][1] == sum(totals)
        assert all(t == calls[0][1] for _, t in calls)
        assert all(a[0] <= b[0] for a, b in zip(calls, calls[1:]))
        assert x.shape[0] == len(CHANNELS)


def test_cancel_stops_the_whole_call(session):
    with mef3io.Reader(str(session), n_threads=2) as r:
        tok = mef3io.CancelToken()
        tok.cancel()
        with pytest.raises(mef3io.Cancelled):
            r.read(CHANNELS, cancel=tok)
        tok = mef3io.CancelToken()
        seen = []

        def cb(d, t):
            seen.append((d, t))
            if 0 < d < t:  # demonstrably part-way through the decode
                tok.cancel()
        with pytest.raises(mef3io.Cancelled):
            _quiet(r.read, CHANNELS * 4, START, START + 31 * SECOND, cancel=tok, progress=cb)
        assert all(d < t for d, t in seen)  # never finished

        def boom(d, t):
            if 0 < d < t:
                raise KeyError("stop")
        with pytest.raises(KeyError):
            _quiet(r.read, CHANNELS, START, START + 31 * SECOND, progress=boom)
        # the Reader still works afterwards
        assert _quiet(r.read, CHANNELS, START, START + SECOND).shape == (len(CHANNELS), 1000)


def test_unscaled_warning_once_per_channel(session):
    with mef3io.Reader(str(session)) as r:
        with warnings.catch_warnings(record=True) as rec:
            warnings.simplefilter("always")
            x = r.read(["unset", "gappy", "unset"], START, START + 2 * SECOND)
            r.read(["unset"], START, START + 2 * SECOND)
            r.read("unset", START, START + 2 * SECOND)
        un = [w for w in rec if issubclass(w.category, mef3io.UnscaledSamplesWarning)]
        assert len(un) == 1 and "unset" in str(un[0].message)
        assert un[0].filename == __file__  # at the caller
        np.testing.assert_array_equal(x[0], np.arange(2000) - 3000.0)  # the raw counts
    with mef3io.Reader(str(session)) as r:  # a new Reader warns again; a window w/o it: no
        with warnings.catch_warnings(record=True) as rec:
            warnings.simplefilter("always")
            r.read(["gappy", "late"], START, START + SECOND)
            r.block_envelope(["unset", "gappy"])
        un = [w for w in rec if issubclass(w.category, mef3io.UnscaledSamplesWarning)]
        assert len(un) == 1


def test_an_invalid_factor_is_refused_like_a_single_read(tmp_path):
    path = tmp_path / "nan.mefd"
    with mef3io.Writer(str(path), overwrite=True) as w:
        w.write_int32("a", np.arange(3000, dtype=np.int32), 1.0, START, FS)
        w.write_int32("b", np.arange(3000, dtype=np.int32), 1.0, START, FS)
    for tmet in glob.glob(f"{path}/b.timd/*/*.tmet"):
        _set_factor(tmet, float("nan"))
    with mef3io.Reader(str(path)) as r:
        with pytest.raises(mef3io.FormatError) as single:
            r.read("b")
        with pytest.raises(mef3io.FormatError) as many:
            r.read(["a", "b"])
        assert str(single.value) == str(many.value)


def test_concurrent_list_reads_on_one_reader(session):
    with mef3io.Reader(str(session)) as r:
        ref = _quiet(r.read, CHANNELS, START, START + 31 * SECOND)
        errors = []

        def work():
            try:
                for _ in range(5):
                    got = _quiet(r.read, CHANNELS, START, START + 31 * SECOND)
                    assert got.tobytes() == ref.tobytes()
            except BaseException as e:  # noqa: BLE001
                errors.append(e)
        threads = [threading.Thread(target=work) for _ in range(4)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        assert not errors, errors[0]


def test_block_envelope_list_is_a_dict_of_single_results(session):
    with mef3io.Reader(str(session)) as r:
        for t0, t1 in [(None, None), (START + 2 * SECOND, START + 9 * SECOND)]:
            got = _quiet(r.block_envelope, CHANNELS + ["gappy"], t0, t1)
            assert list(got) == CHANNELS
            for ch in CHANNELS:
                one = _quiet(r.block_envelope, ch, t0, t1)
                assert set(got[ch]) == set(one)
                for k in one:
                    assert got[ch][k].dtype == one[k].dtype
                    assert np.array_equal(got[ch][k], one[k], equal_nan=True)
        with pytest.raises(IndexError):
            r.block_envelope(["gappy", "nope"])
        with pytest.raises(ValueError):
            r.block_envelope([])


@pytest.mark.skipif(not Path("/proc/self/io").exists(), reason="needs /proc/self/io")
def test_block_envelope_list_reads_no_data_byte(tmp_path):
    path = tmp_path / "big.mefd"
    with mef3io.Writer(str(path), overwrite=True) as w:
        for k in range(20):
            for ch in ("a", "b", "c"):
                w.write(ch, np.arange(int(60 * FS), dtype=float), START + k * 60 * SECOND,
                        fs=FS, precision=0, new_segment=True)

    def rchar():
        for line in Path("/proc/self/io").read_text().splitlines():
            if line.startswith("rchar:"):
                return int(line.split()[1])

    with mef3io.Reader(str(path)) as r:
        before = rchar()
        env = r.block_envelope(["a", "b", "c"])
        whole = rchar() - before
    tidx = sum(p.stat().st_size for p in path.rglob("*.tidx"))
    assert all(len(env[c]["start_uutc"]) == 20 * 6 for c in "abc")
    assert whole <= tidx + 4096, (whole, tidx)  # the indexes, never the data


# --- MultiReader --------------------------------------------------------------

def _parts(tmp_path):
    rng = np.random.default_rng(7)
    paths = []
    for k, (t, secs) in enumerate([(0, 12), (15 * SECOND, 10)]):
        p = tmp_path / f"part{k}.mefd"
        with mef3io.Writer(str(p), overwrite=True) as w:
            for ch, uf in (("A", 0.5), ("B", 1.0), ("C", 0.25 if k else 0.125)):
                x = rng.integers(-2000, 2000, int(secs * FS)).astype(np.int32)
                if ch == "B":
                    x = x.astype(np.float64)
                    x[100:300] = np.nan
                    w.write(ch, x, START + t, FS, precision=0)
                else:
                    w.write_int32(ch, x, uf, START + t, FS)
        paths.append(p)
    merged = tmp_path / "merged.mefd"
    _quiet(mef3io.merge_sessions, [str(p) for p in paths], str(merged))
    return paths, merged


def test_multireader_lists_equal_the_merged_reader(tmp_path):
    paths, merged = _parts(tmp_path)
    with mef3io.Reader(str(merged)) as r, mef3io.MultiReader([str(p) for p in paths]) as mr:
        windows = [(None, None), (START + 5 * SECOND, START + 20 * SECOND),
                   (START + 11 * SECOND + 500, START + 16 * SECOND - 500),
                   (START - SECOND, START + SECOND)]
        for names in (["A", "B", "C"], ["C", "A", "C"], ["B"]):
            for t0, t1 in windows:
                for dt in ("float64", "float32"):
                    want = _quiet(r.read, names, t0, t1, dtype=dt)
                    got = _quiet(mr.read, names, t0, t1, dtype=dt)
                    assert got.dtype == want.dtype and got.shape == want.shape
                    assert got.tobytes() == want.tobytes(), (names, t0, t1, dt)
            env_r = _quiet(r.block_envelope, names)
            env_m = _quiet(mr.block_envelope, names)
            assert list(env_r) == list(env_m)
            for ch in env_r:
                for k in env_r[ch]:
                    assert np.array_equal(env_r[ch][k], env_m[ch][k], equal_nan=True)
        assert _quiet(mr.read, "A", dtype="float32").tobytes() == \
            _quiet(r.read, "A", dtype="float32").tobytes()
        # progress: one monotone sequence over the list, ONE constant total,
        # (total, total) exactly once, at the end
        calls = []
        _quiet(mr.read, ["A", "B", "C"], progress=lambda d, t: calls.append((d, t)))
        assert calls and len({t for _, t in calls}) == 1
        assert all(a[0] < b[0] for a, b in zip(calls, calls[1:]))
        assert calls[0][0] == 0 and calls[-1][0] == calls[-1][1]
        assert sum(1 for d, t in calls if d == t) == 1


def test_multireader_refuses_like_a_reader(tmp_path):
    p = tmp_path / "x.mefd"
    with mef3io.Writer(str(p), overwrite=True) as w:
        w.write("a", np.zeros(1000), START, 256.0)
        w.write("b", np.zeros(1000), START, 1000.0 / 3)
    with mef3io.Reader(str(p)) as r, mef3io.MultiReader([str(p)]) as mr:
        for args in (["a", "b"], ["b", "a"], [], ["a", "zz"]):
            with pytest.raises(Exception) as want:
                r.read(args)
            with pytest.raises(type(want.value)) as got:
                mr.read(args)
            assert str(got.value) == str(want.value)


# --- compat -------------------------------------------------------------------

def test_get_data_list_equals_the_per_channel_loop(session, tmp_path):
    other = tmp_path / "rates.mefd"
    with mef3io.Writer(str(other), overwrite=True) as w:
        w.write("a", np.sin(np.arange(3000) / 5.0), START, 256.0, precision=3)
        w.write("b", np.cos(np.arange(3000) / 5.0), START + 2 * SECOND, 512.0, precision=3)
        w.write("c", np.arange(2000) * 0.5, START + SECOND, 256.0, precision=1)
    for path, names in ((session, CHANNELS), (other, ["a", "b", "c", "a"])):
        mr = mef3io.MefReader(str(path))
        raw = m.Reader(str(path))
        for t1, t2 in [(None, None), (START + SECOND, START + 6 * SECOND), (START + SECOND, None)]:
            got = _quiet(mr.get_data, names, t1, t2)
            picked = list(dict.fromkeys(names))
            want = [raw.read(ch, t1, t2) for ch in picked]
            assert len(got) == len(want)
            for g, w_ in zip(got, want):
                assert g.dtype == w_.dtype and g.tobytes() == w_.tobytes()
        assert _quiet(mr.get_data, names[0]).tobytes() == raw.read(names[0]).tobytes()
        assert _quiet(mr.get_data, []) == []


# --- review round 1 -------------------------------------------------------------

def _tmet_patch(tmet, offset, value):
    import struct
    raw = bytearray(Path(tmet).read_bytes())
    struct.pack_into("<d", raw, offset, value)
    struct.pack_into("<I", raw, 4, m.crc32(bytes(raw[UH:METADATA_FILE_BYTES])))
    struct.pack_into("<I", raw, 0, m.crc32(bytes(raw[4:UH])))
    Path(tmet).write_bytes(bytes(raw))


def _err(fn):
    try:
        fn()
    except Exception as e:  # noqa: BLE001 - compared below
        return type(e), str(e)
    return None


@pytest.mark.parametrize("n_threads", [1, 2, 8])
def test_the_lowest_failing_channel_wins_across_stages(tmp_path, n_threads):
    """mixed fails while DECODING (a corrupt block payload: CRC), gappy while
    reading its INDEX (a truncated .tidx): the list raises what the loop does."""
    path = _build(tmp_path / "stages.mefd")
    tdat = sorted(glob.glob(f"{path}/mixed.timd/*/*.tdat"))[0]
    raw = bytearray(Path(tdat).read_bytes())
    raw[UH + 304 + 20:UH + 304 + 24] = b"\x55\x2a\x13\x77"
    Path(tdat).write_bytes(bytes(raw))
    tidx = sorted(glob.glob(f"{path}/gappy.timd/*/*.tidx"))[1]
    with open(tidx, "r+b") as f:
        f.truncate(UH + ENT + 10)
    with mef3io.Reader(str(path), n_threads=n_threads) as r:
        mixed, gappy = _err(lambda: r.read("mixed")), _err(lambda: r.read("gappy"))
        assert mixed and gappy and mixed != gappy
        assert _err(lambda: _quiet(r.read, ["mixed", "gappy"])) == mixed
        assert _err(lambda: _quiet(r.read, ["gappy", "mixed"])) == gappy
        assert _err(lambda: _quiet(r.read, ["late", "mixed", "gappy"])) == mixed


@pytest.mark.skipif(not Path("/proc/self/io").exists(), reason="needs /proc/self/io")
def test_a_refused_first_channel_stops_the_call_early(tmp_path):
    path = tmp_path / "early.mefd"
    names = [f"c{i}" for i in range(12)]
    with mef3io.Writer(str(path), overwrite=True) as w:
        rng = np.random.default_rng(1)
        for ch in names:
            w.write_int32(ch, rng.integers(-30000, 30000, 120_000).astype(np.int32), 1.0,
                          START, FS)
    for tmet in glob.glob(f"{path}/c0.timd/*/*.tmet"):
        _tmet_patch(tmet, UFACT_OFFSET, float("nan"))

    def rchar():
        for line in Path("/proc/self/io").read_text().splitlines():
            if line.startswith("rchar:"):
                return int(line.split()[1])

    def cost(fn):
        with mef3io.Reader(str(path), n_threads=2) as r:  # cold index cache
            before = rchar()
            with pytest.raises(mef3io.FormatError):
                fn(r)
            return rchar() - before

    single = cost(lambda r: r.read("c0"))
    many = cost(lambda r: r.read(names))
    data = sum(p.stat().st_size for p in path.rglob("*.tdat"))
    idx = sum(p.stat().st_size for p in path.rglob("*.tidx"))
    # c0 and at most its group (n_threads = 2 channels) fetched data
    assert many <= 2 * single + idx + 65536, (many, single, data)
    assert many < data / 3


def test_a_single_channel_with_an_unusable_rate_reads_as_before(tmp_path):
    path = tmp_path / "rate.mefd"
    with mef3io.Writer(str(path), overwrite=True) as w:
        w.write("a", np.zeros(1000), START, FS)
        w.write("b", np.zeros(1000), START, FS)
    for tmet in glob.glob(f"{path}/b.timd/*/*.tmet"):
        _tmet_patch(tmet, 1024 + 1536 + 6160, float("nan"))
    with mef3io.Reader(str(path)) as r:
        assert _quiet(r.read, "b").size == 0  # as on main: no multi-channel text
        assert _quiet(r.read, "b", dtype="float32").size == 0
        for names in (["a", "b"], ["b", "b"], ["b", "a"]):
            with pytest.raises(ValueError, match="b has no usable sampling rate .nan Hz."):
                r.read(names)


def test_get_data_rows_are_independent_arrays(session):
    mr = mef3io.MefReader(str(session))
    got = _quiet(mr.get_data, [0, 0, "jitter", "late"], START, START + 5 * SECOND)
    assert len(got) == 4 and got[0] is not got[1]
    for i in range(4):
        assert got[i].base is None or got[i].flags.owndata
        for j in range(i + 1, 4):
            assert not np.shares_memory(got[i], got[j])
    got[0][:] = 1
    assert not np.all(got[1] == 1)


def test_get_data_errors_follow_the_requested_order(tmp_path):
    path = tmp_path / "order.mefd"
    with mef3io.Writer(str(path), overwrite=True) as w:
        w.write("a", np.zeros(3000), START, 256.0)
        w.write("b", np.zeros(3000), START, 512.0)
        w.write("c", np.zeros(3000), START, 256.0)
    for ch in ("b", "c"):
        tidx = sorted(glob.glob(f"{path}/{ch}.timd/*/*.tidx"))[0]
        with open(tidx, "r+b") as f:
            f.truncate(UH + 10)
    raw = m.Reader(str(path))
    b = _err(lambda: raw.read("b"))
    mr = mef3io.MefReader(str(path))
    assert _err(lambda: mr.get_data(["a", "b", "c"])) == b


def test_multireader_list_progress_ends_at_total_without_data_at_the_end(tmp_path):
    """Round 2 N1: the last listed channel has nothing in the window (no
    recording touched, so its read reports no progress): the call still ends
    with exactly one (total, total)."""
    paths, _ = _parts(tmp_path)
    p = tmp_path / "late.mefd"
    with mef3io.Writer(str(p), overwrite=True) as w:
        w.write("L", np.zeros(1000), START + 100 * SECOND, FS)
    with mef3io.MultiReader([str(x) for x in paths] + [str(p)]) as mr:
        for names in (["A", "L"], ["L"], ["L", "A", "L"]):
            calls = []
            x = _quiet(mr.read, names, START, START + 3 * SECOND,
                       progress=lambda d, t: calls.append((d, t)))
            assert x.shape == (len(names), 3000)
            total = (1 << 20) * len(names)
            assert calls and calls[-1] == (total, total)
            assert sum(1 for d, t in calls if d == t) == 1
            assert all(t == total for _, t in calls)
            assert all(a[0] < b[0] for a, b in zip(calls, calls[1:]))
