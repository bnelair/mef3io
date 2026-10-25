"""#81: ``Writer.write(..., precision="step")`` stores float data as whole
counts of its own quantization step (an ADC's 0.195 uV, ...).

What is pinned here:

* the step is DETECTED exactly (the stored factor is the step's own float64
  value) for decimal, repeating and binary steps, negative data and NaN gaps;
* EXACTNESS CRITERION: a value stored on its step reads back as
  ``double(count) * step`` (what ``Reader.read`` computes). When the data was
  produced as ``count * step`` in float64 that is the same product, so the read
  is BITWISE equal (NaN where NaN was written). For any other data that passes,
  ``|read - written| <= 64 * eps * |written|``; data that does not pass is not
  stored on a step at all;
* data off any step (filtered, resampled, an outlier outside the sample)
  FALLS BACK to the default path, with a StepFallbackWarning, and stores what
  a default write stores; int32 overflow falls back too;
* too little evidence (a DC level, a square wave) adopts no step;
* appends and new segments: a channel keeps its factor (the reader scales a
  channel by one, #150); data off it is REFUSED, never rounded;
* no path wraps a count beyond int32: it is refused, nothing written;
* it is fast (detection on a bounded sample; one verifying pass) and the
  files are much smaller; the legacy stack reads them identically.
"""
from __future__ import annotations

import os
import time
import warnings
from pathlib import Path

import numpy as np
import pytest

import mef3io
from mef3io import _mef3io

START = 1_577_836_800_000_000
FS = 512.0
EPS = np.finfo(np.float64).eps


def adc_counts(n, seed=0, bits=16, walk=40):
    """int counts like an EEG amplifier's: a bounded random walk plus noise."""
    rng = np.random.default_rng(seed)
    lo, hi = -(2 ** (bits - 1)), 2 ** (bits - 1) - 1
    c = np.cumsum(rng.integers(-walk, walk + 1, n))
    c = c - np.round(np.linspace(0, c[-1], n)).astype(np.int64)  # keep it bounded
    c = c + rng.integers(-3, 4, n)
    return np.clip(c, lo, hi).astype(np.int64)


def write(path, data, precision="step", channel="ch1", **kw):
    with warnings.catch_warnings(record=True) as caught:
        warnings.simplefilter("always")
        with mef3io.Writer(str(path), overwrite=kw.pop("overwrite", True)) as w:
            s = w.write(channel, data, kw.pop("start", START), FS, precision=precision, **kw)
    fallbacks = [x for x in caught if issubclass(x.category, mef3io.StepFallbackWarning)]
    return s, fallbacks


def size_of(path):
    return sum(f.stat().st_size for f in Path(path).rglob("*") if f.is_file())


STEPS = {
    "0.195": 0.195,
    "0.1": 0.1,
    "1/3e-3": 1e-3 / 3,
    "binary 10e6/2**24": 10e6 / 2**24,
    "1.0": 1.0,
    "2.5e-7": 2.5e-7,
}


@pytest.mark.parametrize("step", list(STEPS.values()), ids=list(STEPS))
def test_step_detected_exactly_and_read_back_bitwise(tmp_path, step):
    c = adc_counts(200_000, seed=1)
    x = c * step
    assert _mef3io.detect_step(x)["step"] == step
    s, fb = write(tmp_path / "s.mefd", x)
    assert not fb
    assert s["quantization"] == "step" and s["step_fallback"] is None
    assert s["units_conversion_factor"] == step  # bitwise the step itself
    r = mef3io.Reader(str(tmp_path / "s.mefd"))
    raw = r.read_raw("ch1")
    np.testing.assert_array_equal(raw["samples"], c)  # the original counts
    assert raw["units_conversion_factor"] == step
    assert np.array_equal(r.read("ch1"), x)  # bitwise


def test_negative_data_and_nan_gaps(tmp_path):
    c = -np.abs(adc_counts(100_000, seed=2)) - 1  # all negative
    x = c * 0.195
    x[1000:3000] = np.nan  # a gap
    x[50_000:50_010] = np.nan
    s, fb = write(tmp_path / "s.mefd", x)
    assert not fb and s["quantization"] == "step" and s["units_conversion_factor"] == 0.195
    assert s["gaps_skipped"] == 2
    y = mef3io.Reader(str(tmp_path / "s.mefd")).read("ch1")
    assert np.array_equal(y, x, equal_nan=True)


def test_nan_heavy_start_and_all_zero_start(tmp_path):
    """A sample of nothing but NaN/0 looks on through the data."""
    c = adc_counts(300_000, seed=3)
    x = c * 0.195
    x[:200_000] = 0.0
    x[:1000] = np.nan
    assert _mef3io.detect_step(x)["step"] == 0.195
    s, fb = write(tmp_path / "s.mefd", x)
    assert not fb and s["quantization"] == "step"
    t = START + int(round(1000 / FS * 1e6))  # the channel starts at its first sample
    got = mef3io.Reader(str(tmp_path / "s.mefd")).read("ch1", t)
    assert np.array_equal(got, x[1000:])


def test_all_zero_and_all_nan_are_no_fallback(tmp_path):
    s, fb = write(tmp_path / "z.mefd", np.zeros(1000))
    assert not fb and s["step_fallback"] is None and s["quantization"] == "decimal"
    s, fb = write(tmp_path / "n.mefd", np.full(1000, np.nan))
    assert not fb and s["samples_written"] == 0


def test_inexact_products_read_back_within_the_tolerance(tmp_path):
    """Data on a step but not made as count*step in float64 (here: divided by
    the reciprocal) is stored within 64 eps relative, never further."""
    c = adc_counts(100_000, seed=4)
    x = c / (1 / 0.195)
    s, _ = write(tmp_path / "s.mefd", x)
    assert s["quantization"] == "step"
    y = mef3io.Reader(str(tmp_path / "s.mefd")).read("ch1")
    assert np.all(np.abs(y - x) <= 64 * EPS * np.abs(x))


def _default(path, x):
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        with mef3io.Writer(str(path), overwrite=True) as w:
            return w.write("ch1", x, START, FS)


@pytest.mark.parametrize("kind", ["filtered", "resampled", "noise", "tiny jitter"])
def test_off_step_data_falls_back_to_the_default(tmp_path, kind):
    rng = np.random.default_rng(5)
    x = adc_counts(100_000, seed=5) * 0.195
    if kind == "filtered":
        # A real low-pass FIR (Hann-windowed sinc). Taps that are powers of
        # two could keep the data on a finer step, stored exactly then.
        k = np.arange(-8, 9)
        taps = np.sinc(k / 4) * np.hanning(19)[1:-1]
        x = np.convolve(x, taps / taps.sum(), "same")
    elif kind == "resampled":
        t = np.arange(len(x))
        x = np.interp(t * 0.73, t, x)
    elif kind == "noise":
        x = rng.normal(0, 50, len(x))
    else:
        x = x + rng.normal(0, 1e-9, len(x))
    s, fb = write(tmp_path / "s.mefd", x)
    assert s["quantization"] == "decimal" and s["step_fallback"]
    assert len(fb) == 1 and fb[0].message.details[0] == "ch1"
    # It stores exactly what a default write stores.
    d = _default(tmp_path / "d.mefd", x)
    assert s["units_conversion_factor"] == d["units_conversion_factor"]
    a = mef3io.Reader(str(tmp_path / "s.mefd")).read_raw("ch1")["samples"]
    b = mef3io.Reader(str(tmp_path / "d.mefd")).read_raw("ch1")["samples"]
    np.testing.assert_array_equal(a, b)


def _outside_sample(n):
    """An index the bounded sample of detect_step does not look at."""
    head, windows, win = 65536, 16, 2048
    starts = [head + (n - head) * j // (windows + 1) for j in range(1, windows + 1)]
    i = starts[3] + win + 100
    assert all(not (b <= i < b + win) for b in starts) and i >= head
    return i


def test_a_value_off_the_step_outside_the_sample_is_caught(tmp_path):
    """The estimate looks at a sample; the write checks EVERY value."""
    x = adc_counts(1_000_000, seed=6) * 0.195
    i = _outside_sample(len(x))
    x[i] += 0.01
    assert _mef3io.detect_step(x)["step"] == 0.195  # the sample cannot see it
    s, fb = write(tmp_path / "s.mefd", x)
    assert s["quantization"] == "decimal" and f"index {i}" in s["step_fallback"]
    assert len(fb) == 1


def test_int32_overflow_falls_back(tmp_path):
    # A step whose counts need more than int32 over the sample...
    x = adc_counts(10_000, seed=7) * 0.195
    x[0] = 0.195 * 3_000_000_000
    s, _ = write(tmp_path / "a.mefd", x)
    assert s["quantization"] == "decimal" and s["step_fallback"]
    # ...and one outside the sample: the verifying pass catches it.
    x = adc_counts(1_000_000, seed=8) * 0.195
    x[_outside_sample(len(x))] = 0.195 * 3_000_000_000
    assert _mef3io.detect_step(x)["step"] == 0.195
    s, fb = write(tmp_path / "b.mefd", x)
    assert s["quantization"] == "decimal" and "int32" in s["step_fallback"]
    assert len(fb) == 1


def test_infinite_values_are_refused(tmp_path):
    """No step for inf; the default path then refuses it (it used to store
    whatever llround(inf) gave)."""
    x = adc_counts(10_000, seed=9) * 0.195
    x[5] = np.inf
    assert _mef3io.detect_step(x)["step"] is None
    p = tmp_path / "s.mefd"
    with mef3io.Writer(str(p), overwrite=True) as w:
        with warnings.catch_warnings():
            warnings.simplefilter("ignore")
            with pytest.raises(ValueError, match="int32"):
                w.write("ch1", x, START, FS, precision="step")
            with pytest.raises(ValueError, match="int32"):
                w.write("ch1", x, START, FS)
    assert not (p / "ch1.timd").exists()


def test_only_step_is_accepted_as_a_string(tmp_path):
    with mef3io.Writer(str(tmp_path / "s.mefd"), overwrite=True) as w:
        with pytest.raises(ValueError, match="'step'"):
            w.write("ch1", np.zeros(10), START, FS, precision="Step")
    assert not (tmp_path / "s.mefd" / "ch1.timd").exists()


# --- appends: a channel keeps one factor --------------------------------------


def test_step_append_on_the_step_stays_exact(tmp_path):
    p = tmp_path / "s.mefd"
    c = adc_counts(60_000, seed=10)
    x = c * 0.195
    half = 30_000
    s1, _ = write(p, x[:half])
    t1 = START + int(round(half / FS * 1e6))
    with mef3io.Writer(str(p)) as w:  # reopened: the factor comes from disk
        s2 = w.write("ch1", x[half:], t1, FS, precision="step")
    assert s2["quantization"] == "step" and s2["segment"] == s1["segment"] == 0
    assert s2["units_conversion_factor"] == 0.195
    assert np.array_equal(mef3io.Reader(str(p)).read("ch1"), x)


def _tree(path):
    return {str(f): f.stat().st_size for f in Path(path).rglob("*") if f.is_file()}


def test_step_append_off_the_step_is_refused(tmp_path):
    p = tmp_path / "s.mefd"
    x = adc_counts(20_000, seed=11) * 0.195
    write(p, x)
    before = _tree(p)
    y = np.convolve(adc_counts(20_000, seed=12) * 0.195, [0.3, 0.7], "same")
    t1 = START + int(round(len(x) / FS * 1e6))
    for new_segment in (False, True):
        with mef3io.Writer(str(p)) as w:
            with pytest.raises(mef3io.WriteConflictError, match="write_int32"):
                w.write("ch1", y, t1, FS, precision="step", new_segment=new_segment)
        assert _tree(p) == before  # nothing written
    assert np.array_equal(mef3io.Reader(str(p)).read("ch1"), x)


@pytest.mark.parametrize("step", [10.0, 1e6])
def test_factor_of_ten_or_more_is_kept_on_new_segments(tmp_path, step):
    """Factors >= 10 have a negative 'precision': never confused with 'infer'."""
    p = tmp_path / "s.mefd"
    x = adc_counts(20_000, seed=30) * step
    s, _ = write(p, x)
    assert s["units_conversion_factor"] == step
    t1 = START + int(round(len(x) / FS * 1e6)) + 10_000_000
    y = adc_counts(20_000, seed=31) * step
    with mef3io.Writer(str(p)) as w:
        s = w.write("ch1", y, t1, FS, precision="step", new_segment=True)
        assert s["segment"] == 1 and s["units_conversion_factor"] == step
        with pytest.raises(mef3io.WriteConflictError):
            w.write("ch1", np.random.default_rng(0).normal(0, 30, 5120),
                    t1 + 60_000_000, FS, precision="step", new_segment=True)
    r = mef3io.Reader(str(p))
    got = r.read("ch1", t1, t1 + int(round(len(y) / FS * 1e6)))
    assert np.array_equal(got, y)


def test_step_new_segment_keeps_the_channel_factor(tmp_path):
    """Even new_segment=True: one factor per channel, or the reader misreads."""
    p = tmp_path / "s.mefd"
    x = adc_counts(20_000, seed=13) * 0.195
    write(p, x)
    z = adc_counts(20_000, seed=14) * 0.39  # on a coarser step: still fits 0.195
    t1 = START + int(round(len(x) / FS * 1e6)) + 10_000_000
    with mef3io.Writer(str(p)) as w:
        s = w.write("ch1", z, t1, FS, precision="step", new_segment=True)
    assert s["segment"] == 1 and s["quantization"] == "step"
    assert s["units_conversion_factor"] == 0.195
    r = mef3io.Reader(str(p))
    got = r.read("ch1", t1, t1 + int(round(len(z) / FS * 1e6)))
    np.testing.assert_allclose(got, z, rtol=64 * EPS, atol=0)


def test_step_on_a_decimal_channel(tmp_path):
    """A channel written with the default path keeps its decimal factor."""
    p = tmp_path / "s.mefd"
    x = np.round(adc_counts(20_000, seed=15) * 0.001, 3)
    with mef3io.Writer(str(p), overwrite=True) as w:
        w.write("ch1", x, START, FS, precision=3)
    t1 = START + int(round(len(x) / FS * 1e6))
    y = adc_counts(20_000, seed=16) * 0.002  # on the 0.001 grid: exact
    with mef3io.Writer(str(p)) as w:
        s = w.write("ch1", y, t1, FS, precision="step")
    assert s["quantization"] == "step" and s["units_conversion_factor"] == 0.001
    got = mef3io.Reader(str(p)).read("ch1")[len(x):]
    np.testing.assert_allclose(got, y, rtol=64 * EPS, atol=0)
    # Off that grid: refused, not rounded.
    t2 = t1 + int(round(len(y) / FS * 1e6))
    z = np.random.default_rng(17).normal(0, 1, 1000)
    with mef3io.Writer(str(p)) as w:
        with pytest.raises(mef3io.WriteConflictError):
            w.write("ch1", z, t2, FS, precision="step")


def test_compat_writer_accepts_step(tmp_path):
    p = tmp_path / "s.mefd"
    c = adc_counts(10_000, seed=18)
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        w = mef3io.MefWriter(str(p), overwrite=True)
        w.write_data(c * 0.195, "ch1", START, FS, precision="step")
        w.close()
    raw = mef3io.Reader(str(p)).read_raw("ch1")
    assert raw["units_conversion_factor"] == 0.195
    np.testing.assert_array_equal(raw["samples"], c)
    with pytest.raises(ValueError, match="float data"):
        mef3io.MefWriter(str(tmp_path / "t.mefd"), overwrite=True).write_data(
            c.astype(np.int32), "ch1", START, FS, precision="step")


# --- size, speed, oracle --------------------------------------------------------


def test_files_are_much_smaller_and_match_write_int32(tmp_path):
    c = adc_counts(1_843_200, seed=19)  # 1 h at 512 Hz
    x = c * 0.195
    write(tmp_path / "step.mefd", x)
    _default(tmp_path / "default.mefd", x)
    with mef3io.Writer(str(tmp_path / "int.mefd"), overwrite=True) as w:
        w.write_int32("ch1", c.astype(np.int32), 0.195, START, FS)
    step, default, ints = (size_of(tmp_path / f"{n}.mefd") for n in ("step", "default", "int"))
    print(f"\n1 h @ 512 Hz, 0.195 step: default {default / 1e6:.2f} MB, "
          f"step {step / 1e6:.2f} MB, write_int32 {ints / 1e6:.2f} MB")
    assert step * 2 < default
    assert abs(step - ints) <= 0.05 * ints


def test_detection_is_bounded_on_a_large_array():
    """Detection reads a fixed-size sample: 50 M samples cost what 1 M do."""
    x = adc_counts(50_000_000, seed=20) * 0.195
    best = min(_timed(lambda: _mef3io.detect_step(x)) for _ in range(3))
    assert _mef3io.detect_step(x)["step"] == 0.195
    print(f"\ndetect_step on 50 M samples: {best * 1e3:.1f} ms")
    assert best < 0.25


def _timed(fn):
    t = time.perf_counter()
    fn()
    return time.perf_counter() - t


def _write_times(tmp_path, n):
    x = adc_counts(n, seed=21) * 0.195
    y = x.copy()
    y[-1] += 0.01

    def run(data, precision):
        def go():
            with warnings.catch_warnings():
                warnings.simplefilter("ignore")
                with mef3io.Writer(str(tmp_path / "t.mefd"), overwrite=True) as w:
                    w.write("ch1", data, START, FS, precision=precision)
        return min(_timed(go) for _ in range(3))

    return run(x, -1), run(x, "step"), run(y, -1), run(y, "step")


def test_step_write_time_sanity(tmp_path):
    """Default suite: only a loose bound (shared runners are noisy); the
    tight ratios are the opt-in benchmark below."""
    default, step, worst_default, worst = _write_times(tmp_path, 2_000_000)
    assert step <= 2.5 * default
    assert worst <= 2.5 * worst_default


@pytest.mark.benchmark
def test_step_write_is_not_slower_than_the_default(tmp_path):
    """The whole step write (detection + verifying pass + encode) against the
    default write of the same data, best of 3: pinned generously at +10 %
    (measured: the step write is FASTER, its counts are smaller). The worst
    case — on the step up to the very last value, then the fallback — is a
    second quantization pass: pinned at +60 %. Opt-in (--run-benchmark):
    wall-clock ratios flake on shared runners."""
    default, step, worst_default, worst = _write_times(tmp_path, 10_000_000)
    print(f"\n10 M samples: default {default:.3f} s, step {step:.3f} s; "
          f"fallback at the last value {worst:.3f} s vs {worst_default:.3f} s")
    assert step <= 1.10 * default
    assert worst <= 1.60 * worst_default


def test_legacy_stack_reads_the_step_files_identically(tmp_path):
    pymef = pytest.importorskip("pymef.mef_session", reason="legacy oracle not installed")
    pytest.importorskip("mef_tools", reason="legacy oracle not installed")
    from mef_tools.io import MefReader

    p = tmp_path / "s.mefd"
    c = adc_counts(50_000, seed=22)
    x = c * 0.195
    write(p, x)
    session = pymef.MefSession(str(p), "")
    try:
        # Copied before close(): pymef's arrays can point into its own memory.
        got = np.array(session.read_ts_channels_sample("ch1", [0, len(c)])).ravel()
        info = session.read_ts_channel_basic_info()
        ufact = float([ch for ch in info if ch["name"] == "ch1"][0]["ufact"][0])
    finally:
        session.close()
    np.testing.assert_array_equal(got.astype(np.int64), c)
    assert ufact == 0.195
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        legacy = np.asarray(MefReader(str(p)).get_data("ch1")).ravel()[: len(x)]
    np.testing.assert_allclose(legacy, x, rtol=64 * EPS, atol=0)


def test_step_append_on_a_negative_factor(tmp_path):
    """A polarity-inverting factor (allowed, see write_int32) is kept too."""
    p = tmp_path / "s.mefd"
    c = adc_counts(10_000, seed=23)
    with mef3io.Writer(str(p), overwrite=True) as w:
        w.write_int32("ch1", c.astype(np.int32), -0.195, START, FS)
    t1 = START + int(round(len(c) / FS * 1e6))
    y = adc_counts(10_000, seed=24) * -0.195
    with mef3io.Writer(str(p)) as w:
        s = w.write("ch1", y, t1, FS, precision="step")
    assert s["quantization"] == "step" and s["units_conversion_factor"] == -0.195
    assert np.array_equal(mef3io.Reader(str(p)).read("ch1")[len(c):], y)
    with mef3io.Writer(str(p)) as w:
        with pytest.raises(mef3io.WriteConflictError):
            w.write("ch1", np.full(100, 0.1), t1 + 30_000_000, FS, precision="step")



# --- evidence before a step is adopted (#149 review) ----------------------------


@pytest.mark.parametrize("kind", ["square wave", "dc", "few levels"])
def test_too_little_evidence_adopts_no_step(tmp_path, kind):
    n = 20_000
    if kind == "square wave":
        x = np.where((np.arange(n) // 256) % 2 == 0, 50.115, -50.115)
    elif kind == "dc":
        x = np.full(n, 0.585)
    else:
        x = np.random.default_rng(32).integers(-7, 8, n) * 0.195  # 7 magnitudes
    assert _mef3io.detect_step(x)["step"] is None
    p = tmp_path / "s.mefd"
    s, fb = write(p, x)
    assert s["quantization"] == "decimal" and "distinct" in s["step_fallback"] and len(fb) == 1
    # The channel did not lock the level as its factor. The default's decimal
    # factor holds real step data appended later within float64 rounding —
    # except a DC level, where the default itself picks factor 1: then the
    # later data is refused, never rounded.
    y = adc_counts(20_000, seed=33) * 0.195
    t1 = START + int(round(n / FS * 1e6))
    with mef3io.Writer(str(p)) as w:
        if kind == "dc":
            assert s["units_conversion_factor"] == 1.0
            with pytest.raises(mef3io.WriteConflictError):
                w.write("ch1", y, t1, FS, precision="step")
            return
        s = w.write("ch1", y, t1, FS, precision="step")
    assert s["quantization"] == "step"
    got = mef3io.Reader(str(p)).read("ch1")[n:]
    assert np.all(np.abs(got - y) <= 64 * EPS * np.abs(y))


def test_even_counts_first_then_odd_counts_are_refused_not_rounded(tmp_path):
    p = tmp_path / "s.mefd"
    x = adc_counts(20_000, seed=34) * 2 * 0.195  # only even counts: step 0.39
    s, _ = write(p, x)
    assert s["units_conversion_factor"] == 0.39
    y = (adc_counts(20_000, seed=35) * 2 + 1) * 0.195  # odd counts
    t1 = START + int(round(len(x) / FS * 1e6))
    with mef3io.Writer(str(p)) as w:
        with pytest.raises(mef3io.WriteConflictError, match="0.39"):
            w.write("ch1", y, t1, FS, precision="step")


# --- no silent int32 wrap, anywhere (#149 review) --------------------------------


def test_no_path_wraps_a_count_beyond_int32(tmp_path):
    p = tmp_path / "s.mefd"
    x = np.round(adc_counts(5_000, seed=36) * 0.001, 3)
    with mef3io.Writer(str(p), overwrite=True) as w:
        w.write("ch1", x, START, FS, precision=3)
    before = _tree(p)
    t1 = START + int(round(len(x) / FS * 1e6))
    big = np.full(512, 3.0e6 + 0.0004)  # 3e9 counts at 0.001
    with mef3io.Writer(str(p)) as w:
        with pytest.raises(mef3io.WriteConflictError, match="int32"):
            w.write("ch1", big, t1, FS, precision="step")
        with pytest.raises(ValueError, match="int32"):
            w.write("ch1", big, t1, FS)  # the default append: used to wrap
        with pytest.raises(ValueError, match="int32"):
            w.write("ch1", big, t1, FS, precision=3)
    assert _tree(p) == before
    # A fresh default write of one-sided large data picks a precision that
    # fits instead of wrapping (the inference checked only "both ends").
    q = tmp_path / "q.mefd"
    z = 3.0e6 + np.round(np.random.default_rng(37).normal(0, 1, 5000), 3)
    with mef3io.Writer(str(q), overwrite=True) as w:
        s = w.write("ch1", z, START, FS)
    got = mef3io.Reader(str(q)).read("ch1")
    assert np.max(np.abs(got - z)) <= s["units_conversion_factor"] / 2 + 1e-9


def test_default_append_to_a_step_channel_is_refused_clearly(tmp_path):
    p = tmp_path / "s.mefd"
    x = adc_counts(20_000, seed=38) * 0.195
    write(p, x)
    t1 = START + int(round(len(x) / FS * 1e6))
    with mef3io.Writer(str(p)) as w:
        with pytest.raises(mef3io.WriteConflictError, match="not a power of ten"):
            w.write("ch1", x, t1, FS)
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        mw = mef3io.MefWriter(str(p), overwrite=False)
        with pytest.raises(mef3io.WriteConflictError, match="precision='step'"):
            mw.write_data(x, "ch1", t1, FS)


# --- estimator edge cases: the reasons say what happened ------------------------


def test_huge_steps_are_found_and_tiny_ones_say_why(tmp_path):
    c = adc_counts(20_000, seed=39, walk=3)
    assert _mef3io.detect_step(c * 1e300)["step"] == 1e300  # no overflow in the refit
    s, _ = write(tmp_path / "a.mefd", c * 1e300)
    assert s["quantization"] == "step"
    x = adc_counts(20_000, seed=40) * 0.195
    x[_outside_sample(len(x)) if len(x) > 70_000 else -1] = -(2.0 ** 31) * 0.195
    s, _ = write(tmp_path / "b.mefd", x)
    assert s["quantization"] == "decimal" and "int32" in s["step_fallback"]
