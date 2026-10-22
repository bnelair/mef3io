"""MultiReader: several recordings read as one, without merging them on disk.

Pinned here:

* THE acceptance test: MultiReader.read over the parts equals Reader.read
  over the same parts physically merged with merge_sessions — same length,
  same values, NaN in the same places — for many windows (inside one file,
  across 2 and 3 boundaries, in gaps, before/after, half-sample ties) at
  256 Hz, 500 Hz and 1000/3 Hz, for recordings that follow each other, take
  turns, touch within half a sample, and miss a channel;
* the drop-in guarantee: the same battery of calls on a Reader over the
  merged recording and on a MultiReader over its parts gives the same types,
  keys and values;
* refusals (rates, overlapping data), read_raw and units, combined info /
  metadata with the difference warning, paths handling, tar parts, threads,
  cancel/progress, sub-reader warnings, records.
"""
from __future__ import annotations

import os
import shutil
import threading
import warnings
from pathlib import Path

import numpy as np
import pytest

import mef3io

SECOND = 1_000_000
START = 1_577_880_000_000_000  # 2020-01-01T12:00:00Z


def _quiet(fn, *a, **kw):
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        return fn(*a, **kw)


def _write(path, pieces, *, rate, channels=("A", "B"), subject="Jane", precision=0,
           description="stay", notes=()):
    """`pieces`: (start in us from START, seconds, new_segment) per write, every channel."""
    meta = mef3io.Metadata(subject=mef3io.Subject(name_1=subject, id="P-1"),
                           acquisition=mef3io.Acquisition(session_description=description))
    rng = np.random.default_rng(abs(hash((str(path), rate))) % 2**32)
    with mef3io.Writer(str(path), overwrite=True, metadata=meta) as w:
        for t, secs, new in pieces:
            for c, ch in enumerate(channels):
                x = rng.integers(-2000, 2000, int(round(rate * secs))).astype(np.float64)
                w.write(ch, x, START + t, rate, precision=precision, new_segment=new)
        if notes:
            w.write_annotations([{"type": "Note", "time": START + t, "text": s} for t, s in notes])
    return Path(path)


def _merge(parts, dst):
    _quiet(mef3io.merge_sessions, [str(p) for p in parts], str(dst))
    return dst


def _windows(rate, lo, hi, n_random=40, seed=0):
    """(t0, t1) pairs over [lo, hi): None ends, edges, half-sample ties, random."""
    period = 1e6 / rate
    w = [(None, None), (lo, hi), (lo - 5 * SECOND, lo + 3 * SECOND), (hi - 2 * SECOND, hi + 4 * SECOND),
         (lo - 10 * SECOND, lo - 2 * SECOND), (hi + 1 * SECOND, hi + 3 * SECOND),
         (None, lo + 7 * SECOND), (hi - 7 * SECOND, None)]
    for k in (1, 3, 1001):  # half a sample off the grid, where that is a whole us
        half = period * (k + 0.5)
        if abs(half - round(half)) < 1e-9:
            w.append((lo + int(round(half)), hi - int(round(half))))
            w.append((lo + int(round(half)), lo + int(round(half)) + 13 * SECOND))
        # A window an even number and a half samples long: C's llround goes
        # up, Python's round() would go down.
        span = period * (2 * k + 0.5) + period * 600
        if abs(span - round(span)) < 1e-6:
            w.append((lo + 5 * SECOND, lo + 5 * SECOND + int(round(span))))
    rng = np.random.default_rng(seed)
    for _ in range(n_random):
        a, b = sorted(rng.integers(lo - 3 * SECOND, hi + 3 * SECOND, 2).tolist())
        w.append((a, b + 1))
    return w


def _assert_same_reads(parts, merged, channels, windows, **kw):
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        with mef3io.Reader(str(merged)) as m, mef3io.MultiReader(parts, **kw) as r:
            for ch in channels:
                for t0, t1 in windows:
                    a = r.read(ch, t0, t1)
                    b = m.read(ch, t0, t1)
                    assert a.dtype == b.dtype == np.float64
                    assert a.shape == b.shape, (ch, t0, t1)
                    np.testing.assert_array_equal(a, b, err_msg=f"{ch} {t0} {t1}")
                    ra, rb = r.read_raw(ch, t0, t1), m.read_raw(ch, t0, t1)
                    assert list(ra) == list(rb)
                    np.testing.assert_array_equal(ra["valid"], rb["valid"])
                    np.testing.assert_array_equal(ra["samples"][rb["valid"] == 1],
                                                  rb["samples"][rb["valid"] == 1])
                    for k in ("start_uutc", "sampling_frequency", "units_conversion_factor"):
                        assert ra[k] == rb[k] and type(ra[k]) is type(rb[k])


# ---------------------------------------------------------------------------
# the acceptance test
# ---------------------------------------------------------------------------

RATES = [256.0, 500.0, 1000.0 / 3.0]


@pytest.mark.parametrize("rate", RATES)
def test_consecutive_recordings_read_as_their_merge(tmp_path, rate):
    # Three days' worth in miniature: each file starts off the previous one's
    # grid, has a gap of its own and a second segment.
    a = _write(tmp_path / "a.mefd", [(0, 20, False), (25 * SECOND + 1234, 10, False)], rate=rate)
    b = _write(tmp_path / "b.mefd", [(40 * SECOND + 777, 15, False), (60 * SECOND, 8, True)],
               rate=rate)
    c = _write(tmp_path / "c.mefd", [(70 * SECOND + 3, 12, False)], rate=rate)
    m = _merge([a, b, c], tmp_path / "m.mefd")
    _assert_same_reads([c, a, b], m, ["A", "B"], _windows(rate, START, START + 82 * SECOND))


@pytest.mark.parametrize("rate", RATES)
def test_recordings_that_take_turns(tmp_path, rate):
    # A records, stops, B records, A continues (in A's same segment).
    a = _write(tmp_path / "a.mefd", [(0, 10, False), (30 * SECOND, 10, False)], rate=rate)
    b = _write(tmp_path / "b.mefd", [(15 * SECOND + 99, 10, False)], rate=rate)
    m = _merge([a, b], tmp_path / "m.mefd")
    _assert_same_reads([a, b], m, ["A", "B"], _windows(rate, START, START + 40 * SECOND, seed=1))


@pytest.mark.parametrize("rate", [256.0, 1000.0 / 3.0])
def test_recordings_touching_within_half_a_sample(tmp_path, rate):
    # B starts a little before A's last sample period ends: recorder rounding,
    # allowed by merge_sessions and here; the later block wins a shared slot.
    n = int(rate * 10)
    end = int(n * 1e6 / rate)
    tol = int(round(0.5e6 / rate))
    for shift in (1, tol - 1, tol // 2):
        a = _write(tmp_path / "a.mefd", [(0, n / rate, False)], rate=rate)
        b = _write(tmp_path / "b.mefd", [(end - shift, 10, False)], rate=rate)
        dst = tmp_path / "m.mefd"
        shutil.rmtree(dst, ignore_errors=True)
        m = _merge([a, b], dst)
        _assert_same_reads([a, b], m, ["A"], _windows(rate, START, START + 20 * SECOND,
                                                      n_random=60, seed=shift))


@pytest.mark.parametrize("rate", [500.0, 1000.0 / 3.0])
def test_turns_touching_within_half_a_sample_the_later_block_wins(tmp_path, rate):
    # A, then B starting just before A's period ends, then A again starting
    # just before B's ends: at the second boundary the LATER recording in the
    # list (B) must lose the shared slot to A's later block.
    n = int(rate * 10)
    span = int(n * 1e6 / rate)
    shift = int(round(0.5e6 / rate)) - 1
    a = _write(tmp_path / "a.mefd", [(0, n / rate, False), (2 * span - 2 * shift, n / rate, False)],
               rate=rate, channels=("A",))
    b = _write(tmp_path / "b.mefd", [(span - shift, n / rate, False)], rate=rate, channels=("A",))
    m = _merge([a, b], tmp_path / "m.mefd")
    _assert_same_reads([a, b], m, ["A"], _windows(rate, START, START + 3 * span, n_random=80))


def test_a_channel_missing_from_one_recording_reads_as_a_gap_there(tmp_path):
    a = _write(tmp_path / "a.mefd", [(0, 10, False)], rate=256.0, channels=("A", "B"))
    b = _write(tmp_path / "b.mefd", [(12 * SECOND, 10, False)], rate=256.0, channels=("A",))
    c = _write(tmp_path / "c.mefd", [(24 * SECOND, 10, False)], rate=256.0, channels=("A", "B"))
    m = _merge([a, b, c], tmp_path / "m.mefd")
    _assert_same_reads([a, b, c], m, ["A", "B"], _windows(256.0, START, START + 34 * SECOND))
    with _quiet(mef3io.MultiReader, [a, b, c]) as r:
        x = r.read("B", START + 12 * SECOND, START + 22 * SECOND)
        assert np.isnan(x).all()
        assert r.info("B")["number_of_samples"] == 2 * 2560


# ---------------------------------------------------------------------------
# the drop-in guarantee
# ---------------------------------------------------------------------------

def _battery(r):
    """Every public member of Reader, called the same way."""
    out = {"channels": r.channels, "metadata": r.metadata, "problems": r.problems,
           "declaration_issues": r.declaration_issues}
    for ch in r.channels:
        out[("info", ch)] = r.info(ch)
        out[("segments", ch)] = [{k: v for k, v in s.items() if k not in ("path", "provenance")}
                                 for s in r.segments(ch)]
        out[("seg-keys", ch)] = [list(s) for s in r.segments(ch)]
        out[("toc", ch)] = r.toc(ch)
        out[("records", ch)] = r.records(ch)
        out[("records-ch", ch)] = r.records(ch, level="channel")
        for k, s in enumerate(r.segments(ch)):
            out[("records-seg", ch, k)] = r.records(ch, segment=s["segment"])
        i = r.info(ch)
        for t0, t1 in [(None, None), (i["start_time"] + 3 * SECOND, i["end_time"] - 3 * SECOND)]:
            out[("read", ch, t0, t1)] = r.read(ch, t0, t1)
            out[("read1", ch, t0, t1)] = r.read(ch, t0, t1, 1)
            out[("raw", ch, t0, t1)] = r.read_raw(ch, t0, t1)
            out[("env", ch, t0, t1)] = r.block_envelope(ch, t0, t1)
    out["records"] = r.records()
    out["records-all"] = r.records(level="all")
    return out


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


def test_drop_in_same_battery_as_a_reader_over_the_merged_recording(tmp_path):
    a = _write(tmp_path / "a.mefd", [(0, 20, False), (25 * SECOND, 10, True)], rate=500.0,
               notes=[(5 * SECOND, "lights off"), (2 * SECOND, "start")])
    b = _write(tmp_path / "b.mefd", [(40 * SECOND, 15, False)], rate=500.0,
               notes=[(41 * SECOND, "seizure?")])
    c = _write(tmp_path / "c.mefd", [(60 * SECOND, 12, False)], rate=500.0)
    with mef3io.Writer(str(b)) as w:  # channel-level records too
        w.write_annotations([{"type": "Note", "time": START + 42 * SECOND, "text": "B ch"}],
                            channel="A")
    m = _merge([a, b, c], tmp_path / "m.mefd")
    with warnings.catch_warnings():
        warnings.simplefilter("error")  # the parts agree: no warning either
        with mef3io.Reader(str(m)) as ref, mef3io.MultiReader([c, b, a]) as multi:
            want, got = _battery(ref), _battery(multi)
    assert list(want) == list(got)
    for key in want:
        _same(want[key], got[key], str(key))


def test_an_existing_reader_function_works_unchanged(tmp_path):
    def mean_per_channel(reader, t0=None, t1=None):  # written against Reader
        with reader as r:
            return {ch: float(np.nanmean(r.read(ch, t0, t1))) for ch in r.channels}

    a = _write(tmp_path / "a.mefd", [(0, 10, False)], rate=256.0)
    b = _write(tmp_path / "b.mefd", [(20 * SECOND, 10, False)], rate=256.0)
    m = _merge([a, b], tmp_path / "m.mefd")
    assert mean_per_channel(mef3io.MultiReader([a, b])) == mean_per_channel(mef3io.Reader(str(m)))


def test_every_public_member_of_reader_is_on_multireader():
    import inspect

    public = {n for n in dir(mef3io.Reader) if not n.startswith("_")}
    assert public <= set(dir(mef3io.MultiReader))
    for name in public:
        a, b = getattr(mef3io.Reader, name), getattr(mef3io.MultiReader, name)
        assert isinstance(a, property) == isinstance(b, property), name
        if callable(a) and not isinstance(a, property):
            pa = list(inspect.signature(a).parameters.values())[1:]
            pb = list(inspect.signature(b).parameters.values())[1:]
            assert [(p.name, p.kind, p.default) for p in pa] == \
                   [(p.name, p.kind, p.default) for p in pb], name
    pa = list(inspect.signature(mef3io.Reader).parameters)[1:]
    pb = list(inspect.signature(mef3io.MultiReader).parameters)[1:]
    assert pb == [p for p in pa if not p.startswith("_")]


# ---------------------------------------------------------------------------
# refusals
# ---------------------------------------------------------------------------

def test_different_rates_are_refused(tmp_path):
    a = _write(tmp_path / "a.mefd", [(0, 10, False)], rate=256.0)
    b = _write(tmp_path / "b.mefd", [(20 * SECOND, 10, False)], rate=512.0)
    with pytest.raises(ValueError, match="different sampling rates"):
        _quiet(mef3io.MultiReader, [a, b])


def test_overlapping_data_is_refused_but_turns_are_not(tmp_path):
    a = _write(tmp_path / "a.mefd", [(0, 10, False), (30 * SECOND, 10, False)], rate=256.0)
    b = _write(tmp_path / "b.mefd", [(5 * SECOND, 10, False)], rate=256.0)
    with pytest.raises(ValueError, match="both hold samples"):
        _quiet(mef3io.MultiReader, [a, b])
    # Inside A's gap: fine, although the spans overlap.
    b = _write(tmp_path / "b.mefd", [(12 * SECOND, 10, False)], rate=256.0)
    _quiet(mef3io.MultiReader, [a, b]).close()
    # One sample of overlap is more than rounding.
    a = _write(tmp_path / "a.mefd", [(0, 10, False)], rate=256.0)
    b = _write(tmp_path / "b.mefd", [(10 * SECOND - 3907, 10, False)], rate=256.0)
    with pytest.raises(ValueError, match="both hold samples"):
        _quiet(mef3io.MultiReader, [a, b])


def test_a_failing_open_closes_the_others_and_raises(tmp_path):
    a = _write(tmp_path / "a.mefd", [(0, 10, False)], rate=256.0)
    with pytest.raises(mef3io.IoError):
        mef3io.MultiReader([a, tmp_path / "missing.mefd"])


# ---------------------------------------------------------------------------
# read_raw and units
# ---------------------------------------------------------------------------

def test_read_raw_refuses_different_units_and_read_scales_each(tmp_path):
    a = _write(tmp_path / "a.mefd", [(0, 10, False)], rate=256.0, precision=0)
    b = _write(tmp_path / "b.mefd", [(20 * SECOND, 10, False)], rate=256.0, precision=2)
    with warnings.catch_warnings(record=True) as caught:
        warnings.simplefilter("always")
        r = mef3io.MultiReader([a, b])
    assert any(issubclass(w.category, mef3io.MultiReaderDifferenceWarning)
               and "units_conversion_factor" in w.message.details for w in caught)
    with r, _quiet(mef3io.Reader, str(a)) as ra, _quiet(mef3io.Reader, str(b)) as rb:
        assert r.info("A")["units_conversion_factor"] is None
        with pytest.raises(ValueError, match="units-conversion factors"):
            r.read_raw("A")
        # Inside one recording, read_raw is that recording's.
        one = r.read_raw("A", START, START + 10 * SECOND)
        _same(one, ra.read_raw("A", START, START + 10 * SECOND))
        x = r.read("A", START, START + 30 * SECOND)
        np.testing.assert_array_equal(x[:2560], ra.read("A"))
        np.testing.assert_array_equal(x[5120:], rb.read("A"))
        assert np.isnan(x[2560:5120]).all()


# ---------------------------------------------------------------------------
# combined values and the difference warning
# ---------------------------------------------------------------------------

def test_metadata_agreeing_fields_kept_differing_fields_none_with_a_warning(tmp_path):
    a = _write(tmp_path / "jane_a.mefd", [(0, 10, False)], rate=256.0, subject="Jane")
    b = _write(tmp_path / "jane_b.mefd", [(20 * SECOND, 10, False)], rate=256.0, subject="Janet")
    with warnings.catch_warnings(record=True) as caught:
        warnings.simplefilter("always")
        r = mef3io.MultiReader([b, a])
    diff = [w for w in caught if issubclass(w.category, mef3io.MultiReaderDifferenceWarning)]
    assert len(diff) == 1
    w = diff[0].message
    assert w.details == ("subject_name_1",)
    text = str(w)
    assert "subject_name_1" in text and "Jane" not in text and str(tmp_path) not in text
    assert "jane_a" not in text and "jane_b" not in text
    assert w.path == str(a)  # the first in time order
    assert diff[0].filename == __file__
    with r:
        md = r.metadata
        assert isinstance(md, mef3io.Metadata)
        assert md.subject.name_1 is None
        assert md.subject.id == "P-1" and md.acquisition.session_description == "stay"
        assert r.info("A")["subject_name_1"] is None
        assert r.info("A")["subject_id"] == "P-1"
        by = r.metadata_by_source
        assert list(by) == [str(a), str(b)]
        assert by[str(a)].subject.name_1 == "Jane" and by[str(b)].subject.name_1 == "Janet"
        assert r.paths == (str(a), str(b))
        info = r.info("A")
        assert info["start_time"] == START and info["n_segments"] == 2
        assert info["number_of_samples"] == 5120
        assert info["end_time"] == START + 30 * SECOND


# ---------------------------------------------------------------------------
# paths
# ---------------------------------------------------------------------------

def test_paths_argument(tmp_path):
    a = _write(tmp_path / "a.mefd", [(0, 10, False)], rate=256.0)
    b = _write(tmp_path / "b.mefd", [(20 * SECOND, 10, False)], rate=256.0)
    ref = _quiet(mef3io.MultiReader, [str(a), str(b)]).read("A")
    for given in ([a, b], (str(b), a), (p for p in [a, str(b)]), [os.path.relpath(a), b]):
        with _quiet(mef3io.MultiReader, given) as r:
            np.testing.assert_array_equal(r.read("A"), ref)
            assert r.paths[0].endswith("a.mefd")
    for single in (str(a), a, b"a.mefd"):
        with pytest.raises(TypeError, match="sequence"):
            mef3io.MultiReader(single)
    with pytest.raises(TypeError):
        mef3io.MultiReader([a, 3])
    with pytest.raises(ValueError, match="at least one"):
        mef3io.MultiReader([])
    for dup in ([a, str(a)], [a, b, str(a) + "/"], [a, Path(os.path.relpath(a))]):
        with pytest.raises(ValueError, match="same recording"):
            mef3io.MultiReader(dup)
    with pytest.raises(ValueError, match="cache"):
        mef3io.MultiReader([a, b], cache=str(tmp_path / "one.json"))


def test_tar_parts_and_per_recording_cache(tmp_path):
    a = _write(tmp_path / "a.mefd", [(0, 10, False)], rate=256.0)
    b = _write(tmp_path / "b.mefd", [(20 * SECOND, 10, False)], rate=256.0)
    ta = mef3io.archive_session(str(a), str(tmp_path / "a.mefd.tar"))
    ref = _quiet(mef3io.MultiReader, [a, b]).read("A")
    with _quiet(mef3io.MultiReader, [ta, b]) as r:
        np.testing.assert_array_equal(r.read("A"), ref)
        assert r.segments("A")[0]["path"].startswith(str(ta))
    caches = [tmp_path / "ca.json", tmp_path / "cb.json"]
    for _ in range(2):  # cold, then warm
        with _quiet(mef3io.MultiReader, [a, b], cache=caches) as r:
            np.testing.assert_array_equal(r.read("A"), ref)


# ---------------------------------------------------------------------------
# threads, cancel, progress
# ---------------------------------------------------------------------------

def test_thread_budget_and_concurrent_calls_give_identical_results(tmp_path):
    parts = [_write(tmp_path / f"{i}.mefd", [(i * 12 * SECOND, 10, False)], rate=1000.0 / 3.0)
             for i in range(3)]
    with _quiet(mef3io.MultiReader, parts, n_threads=1) as serial:
        want = serial.read("A")
    with _quiet(mef3io.MultiReader, parts) as r:
        for n in (None, 0, 1, 2, 7):
            np.testing.assert_array_equal(r.read("A", n_threads=n), want)
        out, errors = [], []

        def worker():
            try:
                for _ in range(5):
                    out.append(r.read("A"))
                    r.toc("B")
                    r.records(level="all")
            except Exception as exc:  # noqa: BLE001
                errors.append(exc)

        threads = [threading.Thread(target=worker) for _ in range(6)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
    assert not errors
    for x in out:
        np.testing.assert_array_equal(x, want)


def test_cancel_and_progress(tmp_path):
    parts = [_write(tmp_path / f"{i}.mefd", [(i * 12 * SECOND, 10, False)], rate=256.0)
             for i in range(3)]
    with _quiet(mef3io.MultiReader, parts) as r:
        token = mef3io.CancelToken()
        token.cancel()
        with pytest.raises(mef3io.Cancelled):
            r.read("A", cancel=token)
        calls = []
        r.read("A", progress=lambda d, t: calls.append((d, t)))
        assert calls and calls[-1][0] == calls[-1][1] > 0

        def boom(d, t):
            raise KeyError("stop")
        with pytest.raises(KeyError):
            r.read("A", progress=boom)
        with pytest.raises(TypeError):
            r.read("A", cancel=object())


def test_closed_multireader_refuses_reads(tmp_path):
    a = _write(tmp_path / "a.mefd", [(0, 10, False)], rate=256.0)
    b = _write(tmp_path / "b.mefd", [(20 * SECOND, 10, False)], rate=256.0)
    r = _quiet(mef3io.MultiReader, [a, b])
    r.close()
    r.close()
    assert r.channels == ["A", "B"]  # metadata stays, as on a Reader
    for call in (lambda: r.read("A"), lambda: r.read_raw("A"), lambda: r.toc("A"),
                 lambda: r.records(), lambda: r.block_envelope("A")):
        with pytest.raises(ValueError, match="closed"):
            call()


def test_unknown_channel_raises_what_reader_raises(tmp_path):
    a = _write(tmp_path / "a.mefd", [(0, 10, False)], rate=256.0)
    b = _write(tmp_path / "b.mefd", [(20 * SECOND, 10, False)], rate=256.0)
    with _quiet(mef3io.MultiReader, [a, b]) as r:
        for call in (lambda: r.info("X"), lambda: r.read("X"), lambda: r.segments("X"),
                     lambda: r.toc("X"), lambda: r.records("X")):
            with pytest.raises(IndexError):
                call()
        with pytest.raises(IndexError):
            r.records("A", segment=7)


# ---------------------------------------------------------------------------
# the recordings' own warnings
# ---------------------------------------------------------------------------

def test_sub_reader_warnings_are_emitted_once_at_the_caller(tmp_path):
    import glob

    a = _write(tmp_path / "a.mefd", [(0, 10, False), (12 * SECOND, 10, True)], rate=256.0)
    b = _write(tmp_path / "b.mefd", [(30 * SECOND, 10, False)], rate=256.0)
    tmet = glob.glob(str(a) + "/A.timd/A-000001.segd/*.tmet")[0]
    raw = bytearray(open(tmet, "rb").read())
    raw[2000] ^= 0xFF
    open(tmet, "wb").write(bytes(raw))
    with pytest.raises(Exception):
        mef3io.MultiReader([a, b])
    with warnings.catch_warnings(record=True) as caught:
        warnings.simplefilter("always")
        r = mef3io.MultiReader([a, b], strict=False)
    bad = [w for w in caught if issubclass(w.category, mef3io.UnreadableSegmentWarning)]
    assert len(bad) == 1 and bad[0].message.path == str(a) and bad[0].filename == __file__
    with r:
        assert [p["channel"] for p in r.problems] == ["A"]
        x = r.read("A", START, START + 40 * SECOND)
        assert np.isnan(x[12 * 256:22 * 256]).all() and not np.isnan(x[:2560]).any()


def test_legacy_mef_tools_parts(tmp_path):
    pytest.importorskip("mef_tools", reason="legacy oracle not installed")
    from mef_tools.io import MefWriter

    paths = []
    for i in range(2):
        p = tmp_path / f"legacy{i}.mefd"
        w = MefWriter(str(p), overwrite=True)
        w.write_data(np.arange(5000, dtype=np.int32) * 1.0 + i, "ch1",
                     START + i * 30 * SECOND, 256.0, precision=0)
        del w
        paths.append(p)
    with _quiet(mef3io.MultiReader, paths) as r:
        x = r.read("ch1")
        for p in paths:
            with _quiet(mef3io.Reader, str(p)) as one:
                i = one.info("ch1")
                np.testing.assert_array_equal(r.read("ch1", i["start_time"], i["end_time"]),
                                              one.read("ch1"))
        assert len(x) == round((r.info("ch1")["end_time"] - START) * 256 / 1e6)
        assert len(r.declaration_issues) == len(_quiet(mef3io.Reader, str(paths[0]))
                                                .declaration_issues) * 2
