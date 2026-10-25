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
* appends: a channel keeps its factor (the reader scales a channel by one);
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


def test_infinite_values_fall_back(tmp_path):
    x = adc_counts(10_000, seed=9) * 0.195
    x[5] = np.inf
    assert _mef3io.detect_step(x)["step"] is None
    s, fb = write(tmp_path / "s.mefd", x, )
    assert s["quantization"] == "decimal" and s["step_fallback"] and len(fb) == 1


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


def test_step_append_off_the_step_is_rounded_to_the_channel_factor(tmp_path):
    p = tmp_path / "s.mefd"
    x = adc_counts(20_000, seed=11) * 0.195
    write(p, x)
    y = np.convolve(adc_counts(20_000, seed=12) * 0.195, [0.3, 0.7], "same")
    t1 = START + int(round(len(x) / FS * 1e6))
    with warnings.catch_warnings(record=True) as caught:
        warnings.simplefilter("always")
        with mef3io.Writer(str(p)) as w:
            s = w.write("ch1", y, t1, FS, precision="step")
    assert s["quantization"] == "factor" and s["units_conversion_factor"] == 0.195
    assert s["segment"] == 0 and s["step_fallback"]
    assert any(issubclass(c.category, mef3io.StepFallbackWarning) for c in caught)
    got = mef3io.Reader(str(p)).read("ch1")
    np.testing.assert_array_equal(got[: len(x)], x)
    assert np.max(np.abs(got[len(x):] - y)) <= 0.195 / 2 + 1e-12


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
    # Off that grid: exactly what the default append does.
    t2 = t1 + int(round(len(y) / FS * 1e6))
    z = np.random.default_rng(17).normal(0, 1, 1000)
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        with mef3io.Writer(str(p)) as w:
            s = w.write("ch1", z, t2, FS, precision="step")
    assert s["quantization"] == "decimal" and s["units_conversion_factor"] == 0.001
    assert s["step_fallback"]


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


def test_step_write_is_not_slower_than_the_default(tmp_path):
    """The whole step write (detection + verifying pass + encode) against the
    default write of the same data, best of 3: pinned generously at +10 %
    (measured: the step write is FASTER, its counts are smaller). The worst
    case — on the step up to the very last value, then the fallback — is a
    second quantization pass: pinned at +60 %."""
    x = adc_counts(10_000_000, seed=21) * 0.195
    y = x.copy()
    y[-1] += 0.01

    def run(data, precision):
        def go():
            with warnings.catch_warnings():
                warnings.simplefilter("ignore")
                with mef3io.Writer(str(tmp_path / "t.mefd"), overwrite=True) as w:
                    w.write("ch1", data, START, FS, precision=precision)
        return min(_timed(go) for _ in range(3))

    default, step = run(x, -1), run(x, "step")
    worst_default, worst = run(y, -1), run(y, "step")
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
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        with mef3io.Writer(str(p)) as w:
            s = w.write("ch1", np.full(100, 0.1), t1 + 30_000_000, FS, precision="step")
    assert s["quantization"] == "factor" and s["units_conversion_factor"] == -0.195
