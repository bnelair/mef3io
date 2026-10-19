"""#103 (second half) — merge_sessions: several recordings into one.

Pinned here, on real files:

* consecutive recordings, recordings that take turns (a segment split around
  another recording's data) and different channels of one period merge, with
  every sample and time as stored, annotations merged in time order, and a
  split segment's own annotations with the right piece;
* the result reads back in mef3io and pymef, validates, and verify() passes;
  a tampered result fails it;
* refusals before anything is written (overlapping data, rates, offsets,
  patients, arguments), encrypted and tar sources, the CLI, cancel, and that
  docs/merge.md documents every code only a merge raises.
"""
from __future__ import annotations

import json
import re
import shutil
import struct
import warnings
from pathlib import Path

import numpy as np
import pytest

import mef3io
from mef3io import _mef3io as m
from mef3io.merge import REFUSAL_CODES

from test_p29_anonymize import START, SECOND, UH, _legacy, _reseal

FS = 256.0
DOCS = Path(__file__).resolve().parents[1] / "docs" / "merge.md"


def _quiet(fn, *a, **kw):
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        return fn(*a, **kw)


def _session(path, chunks, *, channels=("A", "B"), subject="Jane", rate=FS, new_segment=False,
             notes=(), password=False):
    """`chunks`: (start offset in s, seconds); each written to every channel."""
    kw = dict(password1="pw-one", password2="pw-two") if password else {}
    meta = mef3io.Metadata(subject=mef3io.Subject(name_1=subject, id="P-1"))
    with mef3io.Writer(str(path), overwrite=True, metadata=meta, **kw) as w:
        for i, (t, secs) in enumerate(chunks):
            for c, ch in enumerate(channels):
                x = np.arange(int(rate * secs)) * 1.0 + 1000 * i + 100 * c
                w.write(ch, x, START + int(t * SECOND), rate, precision=0,
                        new_segment=new_segment and i > 0)
        if notes:
            w.write_annotations([{"type": "Note", "time": START + int(t * SECOND), "text": x}
                                 for t, x in notes])
    return Path(path)


def _samples(path, ch, password=""):
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        with mef3io.Reader(str(path), password) as r:
            return {s["segment"]: (s["start_time"], s["number_of_samples"]) for s in r.segments(ch)}


def _same_data(sources, merged, passwords=None):
    """Each source's samples, read over its own span, equal the result's."""
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        with mef3io.Reader(str(merged), "") as mr:
            for k, src in enumerate(sources):
                pw = passwords[k] if passwords else ""
                with mef3io.Reader(str(src), pw) as r:
                    for ch in r.channels:
                        for seg in r.segments(ch):
                            t0 = seg["start_time"]
                            t1 = seg["end_time"]
                            a = r.read(ch, t0, t1)
                            b = mr.read(ch, t0, t1)
                            ok = ~np.isnan(a)
                            np.testing.assert_array_equal(a[ok], b[ok])


def test_consecutive_recordings_merge(tmp_path):
    a = _session(tmp_path / "a.mefd", [(0, 60)], notes=[(5, "a1")])
    b = _session(tmp_path / "b.mefd", [(100, 50)], notes=[(110, "b1"), (101, "b0")])
    rep = _quiet(mef3io.merge_sessions, [a, b], tmp_path / "m.mefd")
    assert rep.verification.ok, rep.summary()
    assert rep.segments == 4 and rep.segments_split == 0  # 2 channels x 2
    assert sorted(_samples(tmp_path / "m.mefd", "A")) == [0, 1]
    _same_data([a, b], tmp_path / "m.mefd")
    with mef3io.Reader(str(tmp_path / "m.mefd")) as r:
        assert [x["text"] for x in r.records(level="all")] == ["a1", "b0", "b1"]
    assert mef3io.validate_session(str(tmp_path / "m.mefd")).ok
    assert mef3io.merge.verify([a, b], tmp_path / "m.mefd").ok


def test_recordings_that_take_turns_merge(tmp_path):
    # A: ONE segment holding 0-60 s and 200-260 s; B: 100-150 s inside A's gap.
    a = _session(tmp_path / "a.mefd", [(0, 60), (200, 60)])
    b = _session(tmp_path / "b.mefd", [(100, 50)])
    seg = next((a / "A.timd").glob("*.segd"))  # A's own annotations, one per part
    with mef3io.Writer(str(a)) as w:
        w.write_annotations([{"type": "Note", "time": START + 10 * SECOND, "text": "early"},
                             {"type": "Note", "time": START + 210 * SECOND, "text": "late"}], "A")
    for ext in ("rdat", "ridx"):
        shutil.move(a / "A.timd" / f"A.{ext}", seg / f"{seg.stem}.{ext}")
    rep = _quiet(mef3io.merge_sessions, [a, b], tmp_path / "m.mefd")
    assert rep.verification.ok, rep.summary()
    assert rep.segments_split == 2  # A's segment, in both channels
    spans = _samples(tmp_path / "m.mefd", "A")
    assert [(t - START) // SECOND for t, _ in spans.values()] == [0, 100, 200]
    _same_data([a, b], tmp_path / "m.mefd")
    with mef3io.Reader(str(tmp_path / "m.mefd")) as r:
        by_seg = {x["segment"]: x["text"] for x in r.records("A", level="segment")}
    assert by_seg == {0: "early", 2: "late"}


def test_two_amplifiers_of_one_period_merge(tmp_path):
    a = _session(tmp_path / "a.mefd", [(0, 60)], channels=("Fp1", "Fp2"))
    b = _session(tmp_path / "b.mefd", [(0, 60)], channels=("ECG",))
    rep = _quiet(mef3io.merge_sessions, [a, b], tmp_path / "m.mefd")
    assert rep.verification.ok and rep.channels_kept == ("ECG", "Fp1", "Fp2")
    _same_data([a, b], tmp_path / "m.mefd")


def test_pymef_reads_the_result(tmp_path):
    pytest.importorskip("pymef")
    from pymef.mef_session import MefSession

    a = _session(tmp_path / "a.mefd", [(0, 10)], channels=("A",))
    b = _session(tmp_path / "b.mefd", [(20, 10)], channels=("A",))
    _quiet(mef3io.merge_sessions, [a, b], tmp_path / "m.mefd")
    s = MefSession(str(tmp_path / "m.mefd"), "")
    try:
        y = np.asarray(s.read_ts_channels_sample("A", [0, int(20 * FS)])).ravel()
        want = np.concatenate([np.arange(int(10 * FS)), np.arange(int(10 * FS))])
        np.testing.assert_array_equal(y.astype(np.int64), want)
    finally:
        s.close()


def test_encrypted_and_tar_sources(tmp_path):
    a = _session(tmp_path / "a.mefd", [(0, 30)], password=True)
    b = _session(tmp_path / "b.mefd", [(40, 30)])
    tar = mef3io.archive_session(str(b), str(tmp_path / "b.mefd.tar"))
    rep = _quiet(mef3io.merge_sessions, [a, tar], tmp_path / "m.mefd",
                 passwords=["pw-two", ""], output_password=("n1", "n2"))
    assert rep.verification.ok and rep.output_encrypted, rep.summary()
    with mef3io.Reader(str(tmp_path / "m.mefd"), "n2") as r:
        assert r.info("A")["subject_name_1"] == "Jane"


@pytest.mark.parametrize("case, code", [
    ("overlap", "merge-overlap"),
    ("rate", "merge-rate-differs"),
    ("patient", "merge-identity-differs"),
    ("one", "merge-too-few"),
    ("passwords", "merge-passwords-count"),
])
def test_refusals_before_anything_is_written(tmp_path, case, code):
    a = _session(tmp_path / "a.mefd", [(0, 60)])
    other = {
        "overlap": lambda: _session(tmp_path / "b.mefd", [(50, 30)]),
        "rate": lambda: _session(tmp_path / "b.mefd", [(100, 10)], rate=512.0),
        "patient": lambda: _session(tmp_path / "b.mefd", [(100, 10)], subject="John"),
    }.get(case, lambda: _session(tmp_path / "b.mefd", [(100, 10)]))()
    sources = [a] if case == "one" else [a, other]
    kw = {"passwords": ["x"]} if case == "passwords" else {}
    rep = mef3io.merge.check(sources, tmp_path / "m.mefd", **kw)
    assert code in rep.codes, rep.summary()
    with pytest.raises((ValueError, RuntimeError)) as exc:
        _quiet(mef3io.merge_sessions, sources, tmp_path / "m.mefd", **kw)
    assert exc.value.code == code
    assert not (tmp_path / "m.mefd").exists() and not (tmp_path / "m.mefd.partial").exists()


def test_a_patient_set_for_the_result_merges(tmp_path):
    a = _session(tmp_path / "a.mefd", [(0, 30)])
    b = _session(tmp_path / "b.mefd", [(40, 30)], subject="John")
    rep = _quiet(mef3io.merge_sessions, [a, b], tmp_path / "m.mefd",
                 metadata={"subject_name_1": "S-01"})
    assert rep.verification.ok
    with mef3io.Reader(str(tmp_path / "m.mefd")) as r:
        assert r.info("A")["subject_name_1"] == "S-01"


def test_different_offsets_are_refused(tmp_path):
    a = _legacy(tmp_path / "a.mefd", rto=START - 3600 * SECOND)
    b = _legacy(tmp_path / "b.mefd", rto=0)
    assert "merge-rto-differs" in mef3io.merge.check([a, b], tmp_path / "m.mefd").codes


def test_a_tampered_result_fails_verification(tmp_path):
    a = _session(tmp_path / "a.mefd", [(0, 60)], channels=("A",))
    b = _session(tmp_path / "b.mefd", [(100, 50)], channels=("A",))
    _quiet(mef3io.merge_sessions, [a, b], tmp_path / "m.mefd")
    tdat = next((tmp_path / "m.mefd" / "A.timd").glob("A-000001.segd/*.tdat"))
    raw = bytearray(tdat.read_bytes())
    raw[UH + 304 + 10] ^= 0x01
    nb = struct.unpack_from("<I", raw, UH + 36)[0]
    struct.pack_into("<I", raw, UH, m.crc32(bytes(raw[UH + 4:UH + nb])))
    _reseal(raw)
    tdat.write_bytes(bytes(raw))
    v = mef3io.merge.verify([a, b], tmp_path / "m.mefd")
    assert not v.ok, v.summary()


def test_cancel_leaves_nothing_behind(tmp_path):
    a = _session(tmp_path / "a.mefd", [(0, 600)])
    b = _session(tmp_path / "b.mefd", [(700, 600)])
    token = mef3io.CancelToken()
    calls = []

    def progress(done, total):
        calls.append(done)
        if len(calls) == 3:
            token.cancel()

    with pytest.raises(mef3io.Cancelled):
        mef3io.merge_sessions([a, b], tmp_path / "m.mefd", progress=progress, cancel=token)
    assert not (tmp_path / "m.mefd").exists() and not (tmp_path / "m.mefd.partial").exists()


def test_the_cli_merges(tmp_path, capsys):
    from mef3io.__main__ import main

    a = _session(tmp_path / "a.mefd", [(0, 20)])
    b = _session(tmp_path / "b.mefd", [(40, 20)])
    assert main(["merge", str(tmp_path / "m.mefd"), str(a), str(b), "--json"]) == 0
    assert json.loads(capsys.readouterr().out)["verification"]["ok"]
    assert main(["merge", str(tmp_path / "m.mefd"), str(a), str(b)]) == 2
    assert "error (dst-exists)" in capsys.readouterr().err


def test_every_code_only_a_merge_raises_is_documented():
    from mef3io.transform import REFUSAL_CODES as TRANSFORM

    rows = {code: (error, where) for code, error, where in re.findall(
        r"^\| `([a-z0-9-]+)` \| `(\w+)` \| (\w+) \|", DOCS.read_text(encoding="utf-8"),
        re.MULTILINE)}
    assert rows == {k: v for k, v in REFUSAL_CODES.items() if k not in TRANSFORM}


# --------------------------------------------------------------------------- review round 1


def _one(path, t_us, n, rate=FS, channels=("A",)):
    with mef3io.Writer(str(path), overwrite=True) as w:
        for ch in channels:
            w.write(ch, np.arange(n) * 1.0, START + t_us, rate, precision=0)
    return Path(path)


@pytest.mark.parametrize("rate", [499.7, 1024.0])
def test_a_recording_starting_a_microsecond_early_merges(tmp_path, rate):
    """B starts at A's end truncated to whole microseconds (recorder rounding):
    within half a sample, so accepted by the check AND by the copy."""
    n = int(rate * 10)
    a = _one(tmp_path / "a.mefd", 0, n, rate)
    end = int(n * 1e6 / rate)  # truncated
    b = _one(tmp_path / "b.mefd", end - 1, n, rate)
    assert mef3io.merge.check([a, b], tmp_path / "m.mefd").ok
    rep = _quiet(mef3io.merge_sessions, [a, b], tmp_path / "m.mefd")
    assert rep.verification.ok, rep.summary()


def test_data_starting_just_inside_a_blocks_end_splits_there(tmp_path):
    # A: one segment, 0-60 s and 120-180 s; B starts 0.4 sample before A's
    # 60-s block end, inside A's gap: A is split, nothing refused late.
    a = _session(tmp_path / "a.mefd", [(0, 60), (120, 60)], channels=("A",))
    b = _one(tmp_path / "b.mefd", 60 * SECOND - int(0.4e6 / FS), int(FS * 30))
    assert mef3io.merge.check([a, b], tmp_path / "m.mefd").ok
    rep = _quiet(mef3io.merge_sessions, [a, b], tmp_path / "m.mefd")
    assert rep.verification.ok and rep.segments_split == 1, rep.summary()


def _merged_with_notes(tmp_path):
    a = _session(tmp_path / "a.mefd", [(0, 20)], channels=("A",), notes=[(5, "a")])
    b = _session(tmp_path / "b.mefd", [(40, 20)], channels=("A",), notes=[(45, "b")])
    _quiet(mef3io.merge_sessions, [a, b], tmp_path / "m.mefd")
    return a, b, tmp_path / "m.mefd"


def test_merged_record_files_span_their_records(tmp_path):
    a, b, mm = _merged_with_notes(tmp_path)
    uh = (mm / "m.rdat").read_bytes()[:UH]
    start, end = struct.unpack_from("<qq", uh, 16)
    assert (-start, -end) == (START + 5 * SECOND, START + 45 * SECOND)


@pytest.mark.parametrize("rel, offset, fmt, value, field", [
    ("m.ridx", UH + 16, "<q", -(START + 50 * SECOND), "record index entry"),  # a record's time
    ("m.rdat", 24, "<q", -(START + 99 * SECOND), "universal header times"),
    ("A.timd/A-000000.segd/A-000000.tidx", 820, "16s", b"\x01" * 16, "level_UUID"),
])
def test_verify_catches_tampered_merge_headers(tmp_path, rel, offset, fmt, value, field):
    a, b, mm = _merged_with_notes(tmp_path)
    f = mm / rel
    raw = bytearray(f.read_bytes())
    struct.pack_into(fmt, raw, offset, value)
    _reseal(raw)
    f.write_bytes(bytes(raw))
    v = mef3io.merge.verify([a, b], mm)
    assert not v.ok and any(field in p.field for p in v.problems), v.summary()
