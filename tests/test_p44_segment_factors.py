"""Per-segment units-conversion factors (#150).

MEF 3.0 stores the conversion factor per SEGMENT (section 2), and segments of
one channel may differ (meflib then sets the channel-level factor to NO_ENTRY,
meflib.c:5494/6176). The reader used to scale a whole channel by its FIRST
segment's factor, so a later segment with another factor read back wrong with
no warning. Pinned here:

* read() scales each sample by its own segment's factor (decimal, non-decimal
  and negative factors), block_envelope() each block (swapped when negative),
  read_runs() splits a run where the factor changes;
* read_raw() lists where each factor applies (``units_conversion_factors``,
  canonical spans) and gives one ``units_conversion_factor`` only when the
  window's samples share it; info() is None for a mixed channel, segments()
  gives each segment's; inspect() the same;
* a factor that cannot scale (0 = MEF's "no entry", NaN, inf) is REFUSED by
  read() and block_envelope() for the windows that touch it, never turned into
  zeros, and never contained as a "damaged segment" by a lenient open;
* copies (transform cut across the boundary, anonymize, merge) keep every
  segment's factor, and a MultiReader over mixed parts equals their merge;
* the pymef oracle agrees: its channel-level factor is NO_ENTRY (0.0) and its
  counts times each segment's own factor are mef3io's values;
* mef_tools' drop-in (compat) get_data.
"""
from __future__ import annotations

import struct
import warnings
from pathlib import Path

import numpy as np
import pytest

import mef3io
from mef3io import _mef3io as m

SECOND = 1_000_000
DAY = 86_400 * SECOND
START = 1_577_880_000_000_000  # 2020-01-01T12:00:00Z
FS = 256.0
N = 2560  # 10 s per segment
UH_BYTES = 1024
METADATA_FILE_BYTES = 16384
UFACT_OFFSET = 1024 + 1536 + 6200  # section 2 + units_conversion_factor (meflib.h:438)

# (start offset in us, factor, new segment): a decimal, a non-decimal and a
# negative factor; the last segment starts right where the second ends (no
# gap) so only the factor separates them.
PLAN = [(0, 0.01, False), (20 * SECOND, 0.195, True), (30 * SECOND, -0.5, True)]


def _quiet(fn, *a, **kw):
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        return fn(*a, **kw)


def _counts(k):
    rng = np.random.default_rng(100 + k)
    return rng.integers(-3000, 3000, N).astype(np.int32)


def _write_mixed(path, plan=PLAN, channels=("A",), start=START):
    with mef3io.Writer(str(path), overwrite=True) as w:
        for k, (t, uf, new) in enumerate(plan):
            for ch in channels:
                w.write_int32(ch, _counts(k), uf, start + t, FS, new_segment=new)
    return Path(path)


def _expected(plan=PLAN, start=START):
    """The values a correct reader returns over the whole channel, NaN in gaps."""
    end = max(t + int(N / FS * SECOND) for t, _, _ in plan)
    out = np.full(int(round(end * FS / SECOND)), np.nan)
    for k, (t, uf, _) in enumerate(plan):
        i = int(round(t * FS / SECOND))
        out[i:i + N] = _counts(k).astype(np.float64) * uf
    return out


def _tmets(path, channel="A"):
    return sorted(Path(path).rglob(f"{channel}-*.tmet"))


def _set_factor(tmet, value):
    """Store `value` as a segment's factor, resealing both .tmet CRCs."""
    raw = bytearray(Path(tmet).read_bytes())
    struct.pack_into("<d", raw, UFACT_OFFSET, value)
    struct.pack_into("<I", raw, 4, m.crc32(bytes(raw[UH_BYTES:METADATA_FILE_BYTES])))
    struct.pack_into("<I", raw, 0, m.crc32(bytes(raw[4:UH_BYTES])))
    Path(tmet).write_bytes(bytes(raw))


@pytest.fixture(scope="module")
def mixed(tmp_path_factory):
    return _write_mixed(tmp_path_factory.mktemp("mixed") / "mixed.mefd")


# ---------------------------------------------------------------------------
# reads
# ---------------------------------------------------------------------------

def test_the_issue_repro_decimal_precisions(tmp_path):
    p = tmp_path / "s.mefd"
    with mef3io.Writer(str(p)) as w:
        w.write("ch", np.full(1000, 9.99), START, 100.0, precision=2)
        w.write("ch", np.full(1000, 9.9999), START + 20 * SECOND, 100.0, precision=4,
                new_segment=True)
    with mef3io.Reader(str(p)) as r:
        x = r.read("ch")
        np.testing.assert_allclose(x[:1000], 9.99)
        np.testing.assert_allclose(x[2000:], 9.9999)
        assert [s["units_conversion_factor"] for s in r.segments("ch")] == [0.01, 0.0001]


def test_read_scales_each_segment_by_its_own_factor(mixed):
    with mef3io.Reader(str(mixed)) as r:
        x = r.read("A")
        want = _expected()
        assert x.shape == want.shape
        # Bit-exact: the same product of the same doubles.
        np.testing.assert_array_equal(x, want)
        # Windows inside one segment, across a gap, across the gapless boundary.
        for t0, t1 in [(START + 2 * SECOND, START + 4 * SECOND),
                       (START + 5 * SECOND, START + 25 * SECOND),
                       (START + 28 * SECOND, START + 33 * SECOND)]:
            i0 = int(round((t0 - START) * FS / SECOND))
            np.testing.assert_array_equal(r.read("A", t0, t1), want[i0:i0 + len(r.read("A", t0, t1))])


def test_info_segments_and_inspect_say_the_factors_differ(mixed):
    with mef3io.Reader(str(mixed)) as r:
        assert r.info("A")["units_conversion_factor"] is None
        assert [s["units_conversion_factor"] for s in r.segments("A")] == [0.01, 0.195, -0.5]
    summary = mef3io.inspect(str(mixed))
    ch = summary.channels[0]
    assert ch.units_conversion_factor is None
    assert [s.units_conversion_factor for s in ch.segments] == [0.01, 0.195, -0.5]


def test_uniform_channel_keeps_its_one_factor(tmp_path):
    p = _write_mixed(tmp_path / "u.mefd", [(0, 0.195, False), (20 * SECOND, 0.195, True)])
    with mef3io.Reader(str(p)) as r:
        assert r.info("A")["units_conversion_factor"] == 0.195
        raw = r.read_raw("A")
        assert raw["units_conversion_factor"] == 0.195
        # Two segments, one factor, a gap between them: ONE span, the gap inside.
        assert raw["units_conversion_factors"] == [
            {"start_index": 0, "end_index": 2 * N + int(10 * FS), "units_conversion_factor": 0.195}]
    assert mef3io.inspect(str(p)).channels[0].units_conversion_factor == 0.195


def test_read_raw_lists_where_each_factor_applies(mixed):
    with mef3io.Reader(str(mixed)) as r:
        raw = r.read_raw("A")
        assert list(raw) == ["start_uutc", "sampling_frequency", "units_conversion_factor",
                             "units_conversion_factors", "samples", "valid",
                             "block_copy_mismatches"]
        assert raw["units_conversion_factor"] is None
        g = int(20 * FS)  # where the second segment starts
        assert raw["units_conversion_factors"] == [
            {"start_index": 0, "end_index": N, "units_conversion_factor": 0.01},
            {"start_index": g, "end_index": g + N, "units_conversion_factor": 0.195},
            {"start_index": g + N, "end_index": g + 2 * N, "units_conversion_factor": -0.5},
        ]
        # The spans turn the counts into read()'s values.
        x = np.full(len(raw["samples"]), np.nan)
        for sp in raw["units_conversion_factors"]:
            sl = slice(sp["start_index"], sp["end_index"])
            x[sl] = raw["samples"][sl] * sp["units_conversion_factor"]
        x[raw["valid"] == 0] = np.nan
        np.testing.assert_array_equal(x, r.read("A"))
        # A window inside one segment: that segment's factor, one span.
        one = r.read_raw("A", START + 21 * SECOND, START + 22 * SECOND)
        assert one["units_conversion_factor"] == 0.195
        assert one["units_conversion_factors"] == [
            {"start_index": 0, "end_index": int(FS), "units_conversion_factor": 0.195}]
        # Nothing valid: no span, the channel's factor (None here).
        gap = r.read_raw("A", START + 12 * SECOND, START + 13 * SECOND)
        assert gap["units_conversion_factors"] == [] and gap["units_conversion_factor"] is None


def test_block_envelope_scales_each_block_by_its_segment(mixed):
    with mef3io.Reader(str(mixed)) as r:
        env = r.block_envelope("A")
        want = _expected()
        for t0, t1, lo, hi in zip(env["start_uutc"], env["end_uutc"], env["min"], env["max"]):
            i0 = int(round((t0 - START) * FS / SECOND))
            i1 = int(round((t1 - START) * FS / SECOND))
            assert lo == np.nanmin(want[i0:i1]) and hi == np.nanmax(want[i0:i1])
        assert (env["min"] <= env["max"]).all()


def test_read_runs_split_where_the_factor_changes(mixed):
    s = m.Session(str(mixed))
    runs = s.read_runs("A")
    assert [r["units_conversion_factor"] for r in runs] == [0.01, 0.195, -0.5]
    # The last two segments touch: only the factor separates them.
    assert runs[2]["start_sample"] == runs[1]["start_sample"] + N
    for k, r in enumerate(runs):
        np.testing.assert_array_equal(r["samples"], _counts(k))


def test_compat_get_data_is_per_segment(mixed):
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        mr = mef3io.MefReader(str(mixed))
    assert mr.get_channel_info("A")["ufact"] == [None]
    np.testing.assert_array_equal(mr.get_data("A"), _expected())
    np.testing.assert_array_equal(mr.get_data(["A"])[0], _expected())


# ---------------------------------------------------------------------------
# a factor that cannot scale
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("bad, shown", [(0.0, "0 (MEF's \"no entry\")"), (float("nan"), "nan"),
                                        (float("inf"), "inf")])
@pytest.mark.parametrize("strict", [True, False])
def test_unusable_factor_is_refused_not_zeroed(tmp_path, bad, shown, strict):
    p = _write_mixed(tmp_path / "z.mefd")
    _set_factor(_tmets(p)[1], bad)
    with mef3io.Reader(str(p), strict=strict) as r:
        msg = (f"channel A: samples in this window come from a segment that stores "
               f"units_conversion_factor {shown}, so they have no value in units; ")
        with pytest.raises(mef3io.FormatError) as e:
            r.read("A")
        assert str(e.value).startswith(msg) and "read_raw() returns the stored counts" in str(e.value)
        with pytest.raises(mef3io.FormatError, match="toc\\(\\) returns the stored counts"):
            r.block_envelope("A")
        # Not a damaged segment: nothing contained, the counts still read.
        assert r.problems == []
        raw = r.read_raw("A")
        f = raw["units_conversion_factors"][1]["units_conversion_factor"]
        assert f == bad or (f != f and bad != bad)
        np.testing.assert_array_equal(raw["samples"][int(20 * FS):int(20 * FS) + N], _counts(1))
        # Windows that do not touch it read as before.
        want = _expected()
        np.testing.assert_array_equal(r.read("A", START, START + 10 * SECOND), want[:N])
        env = r.block_envelope("A", START, START + 10 * SECOND)
        assert len(env["min"]) >= 1
        assert r.problems == []


def test_multireader_refuses_with_readers_words(tmp_path):
    a = _write_mixed(tmp_path / "a.mefd", [(0, 0.01, False)])
    b = _write_mixed(tmp_path / "b.mefd", [(0, 0.2, False)], start=START + 20 * SECOND)
    _set_factor(_tmets(b)[0], 0.0)
    merged = _quiet(mef3io.merge_sessions, [str(a), str(b)], str(tmp_path / "ab.mefd"))
    del merged
    with _quiet(mef3io.Reader, str(tmp_path / "ab.mefd")) as r, \
            _quiet(mef3io.MultiReader, [a, b]) as mr:
        with pytest.raises(mef3io.FormatError) as e1:
            r.read("A")
        with pytest.raises(mef3io.FormatError) as e2:
            mr.read("A")
        assert str(e1.value) == str(e2.value)
        _same(mr.read_raw("A"), r.read_raw("A"))


# ---------------------------------------------------------------------------
# the writer, copies and the MultiReader
# ---------------------------------------------------------------------------

def test_append_with_another_factor_is_still_refused_new_segment_is_not(tmp_path):
    p = tmp_path / "w.mefd"
    with mef3io.Writer(str(p)) as w:
        w.write_int32("A", _counts(0), 0.01, START, FS)
        with pytest.raises(RuntimeError):
            w.write_int32("A", _counts(1), 0.02, START + 10 * SECOND, FS)
        w.write_int32("A", _counts(1), 0.02, START + 10 * SECOND, FS, new_segment=True)
    with mef3io.Reader(str(p)) as r:
        np.testing.assert_array_equal(
            r.read("A"), np.concatenate([_counts(0) * 0.01, _counts(1) * 0.02]))


def _read_window(path, t0, t1):
    with _quiet(mef3io.Reader, str(path)) as r:
        return r.read("A", t0, t1), [s["units_conversion_factor"] for s in r.segments("A")]


def test_cut_across_the_factor_boundary_keeps_each_factor(mixed, tmp_path):
    t0, t1 = START + 25 * SECOND + 3906, START + 34 * SECOND  # inside seg 1 .. inside seg 2
    for exact in (True, False):
        dst = tmp_path / f"cut{int(exact)}.mefd"
        _quiet(mef3io.transform_session, str(mixed), str(dst), time_range=(t0, t1), exact=exact)
        with _quiet(mef3io.Reader, str(dst)) as c:
            s0 = c.info("A")["start_time"]
            got = c.read("A", s0, t1)
            assert [s["units_conversion_factor"] for s in c.segments("A")] == [0.195, -0.5]
            assert c.info("A")["units_conversion_factor"] is None
        want, _ = _read_window(mixed, s0, t1)
        np.testing.assert_array_equal(got, want)
        # The cut segment's declared extremes stay ordered with a negative factor.
        tmet = _tmets(dst)[-1]
        raw = Path(tmet).read_bytes()
        hi, lo = struct.unpack_from("<dd", raw, 1024 + 1536 + 6208)
        assert hi >= lo


def test_anonymized_copy_keeps_each_factor(mixed, tmp_path):
    dst = tmp_path / "anon.mefd"
    _quiet(mef3io.anonymize_session, str(mixed), str(dst), -7_000 * DAY)
    with _quiet(mef3io.Reader, str(dst)) as c:
        np.testing.assert_array_equal(c.read("A"), _expected())
        assert [s["units_conversion_factor"] for s in c.segments("A")] == [0.01, 0.195, -0.5]


def test_merge_of_parts_with_different_factors_reads_right(tmp_path):
    a = _write_mixed(tmp_path / "a.mefd", PLAN[:2])
    b = _write_mixed(tmp_path / "b.mefd", [(0, -0.5, False), (15 * SECOND, 0.01, True)],
                     start=START + 40 * SECOND)
    dst = tmp_path / "ab.mefd"
    _quiet(mef3io.merge_sessions, [str(a), str(b)], str(dst))
    with _quiet(mef3io.Reader, str(dst)) as r:
        assert [s["units_conversion_factor"] for s in r.segments("A")] == [0.01, 0.195, -0.5, 0.01]
        x = r.read("A")
    xa, _ = _read_window(a, START, START + 40 * SECOND)
    xb, _ = _read_window(b, START + 40 * SECOND, START + 65 * SECOND)
    np.testing.assert_array_equal(x, np.concatenate([xa, xb]))
    # A MultiReader over the same parts equals the merge, read_raw included.
    with _quiet(mef3io.MultiReader, [a, b]) as mr, _quiet(mef3io.Reader, str(dst)) as r:
        for t0, t1 in [(None, None), (START + 5 * SECOND, START + 50 * SECOND),
                       (START + 21 * SECOND, START + 22 * SECOND),
                       (START + 41 * SECOND, START + 60 * SECOND),
                       (START + 12 * SECOND, START + 13 * SECOND)]:
            np.testing.assert_array_equal(mr.read("A", t0, t1), r.read("A", t0, t1))
            _same(mr.read_raw("A", t0, t1), r.read_raw("A", t0, t1))
            _same(mr.block_envelope("A", t0, t1), r.block_envelope("A", t0, t1))
        assert mr.info("A")["units_conversion_factor"] is r.info("A")["units_conversion_factor"]
        assert ([s["units_conversion_factor"] for s in mr.segments("A")]
                == [s["units_conversion_factor"] for s in r.segments("A")])


def _same(a, b, where=""):
    assert type(a) is type(b), (where, type(a), type(b))
    if isinstance(a, dict):
        assert list(a) == list(b), where
        for k in a:
            _same(a[k], b[k], f"{where}.{k}")
    elif isinstance(a, (list, tuple)):
        assert len(a) == len(b), where
        for i, (x, y) in enumerate(zip(a, b)):
            _same(x, y, f"{where}[{i}]")
    elif isinstance(a, np.ndarray):
        assert a.dtype == b.dtype and a.shape == b.shape, where
        np.testing.assert_array_equal(a, b, err_msg=where)
    else:
        assert a == b or (a != a and b != b), (where, a, b)


# ---------------------------------------------------------------------------
# the oracle
# ---------------------------------------------------------------------------

def test_pymef_agrees_on_a_mixed_factor_session(mixed):
    pymef = pytest.importorskip("pymef", reason="legacy oracle not installed")
    from pymef.mef_session import MefSession
    del pymef
    s = MefSession(str(mixed), "")
    md = s.session_md["time_series_channels"]["A"]
    # meflib's channel aggregate: NO_ENTRY (0.0) when the segments differ.
    # (float(): pymef's metadata arrays point into memory close() frees.)
    assert float(md["section_2"]["units_conversion_factor"][0]) == 0.0
    segs = md["segments"]
    factors = [float(segs[k]["section_2"]["units_conversion_factor"][0]) for k in sorted(segs)]
    assert factors == [0.01, 0.195, -0.5]
    counts = np.asarray(s.read_ts_channels_sample("A", [0, 3 * N])).astype(np.int64)
    s.close()
    oracle = np.concatenate([counts[k * N:(k + 1) * N] * f for k, f in enumerate(factors)])
    with mef3io.Reader(str(mixed)) as r:
        x = r.read("A")
    np.testing.assert_array_equal(x[~np.isnan(x)], oracle)
