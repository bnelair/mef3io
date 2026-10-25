"""anonymize_session without a shift: the de-identified copy KEEPS every time
(the default since 2.0.0).

What is pinned here, on real files:

* a copy made without ``shift_us`` reads back exactly the source's times
  (info, segments, block index, records at every level and the times inside
  record bodies) and samples, while the identity is gone exactly as in a
  shifted copy (subject, location, names, UUIDs, foreign regions,
  provenance operation 6);
* every stored time field is byte-identical to the source's, each segment
  keeps its recording-time offset (needed to read those stored times back),
  and the GMT offset / DST fields are still cleared;
* a meflib recording-time-offset session and one whose segments disagree on
  the offset are copied with their user times unchanged (only a SHIFT needs
  one agreed offset);
* a cut (time_range) and an encrypted source with output passwords, both
  without a shift;
* ``verify`` without a shift catches ONE altered time in the copy (negative
  controls: universal header, index entry, block header, record header,
  record index, record body, recording-time offset), and refuses a shifted
  copy;
* the CLI keeps the times when no shift is given.

The agreement of ``check`` and ``anonymize_session`` without a shift, case
by case, is pinned in test_p32 (``test_check_agrees_without_a_shift``).
"""
from __future__ import annotations

import shutil
import struct
import warnings
from pathlib import Path

import numpy as np
import pytest

import mef3io

from test_p29_anonymize import (
    DAY,
    FS,
    S2,
    S3,
    SECOND,
    SHIFT,
    START,
    TMET,
    UH,
    UUTC_NO_ENTRY,
    _all_records,
    _legacy,
    _plant_block_time,
    _plant_record,
    _plant_ridx,
    _plant_s3,
    _plant_uh,
    _read_all,
    _reseal,
    _rich_session,
    _set_rto,
    _signal,
    _tmets,
)


def _keep(src, dst, **kw):
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        return mef3io.anonymize_session(src, dst, **kw)


def _verify(src, dst, *a, **kw):
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        return mef3io.anonymize.verify(src, dst, *a, **kw)


def _segments_and_toc(path, password=""):
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        with mef3io.Reader(str(path), password) as r:
            return {ch: ([(s["start_time"], s["end_time"]) for s in r.segments(ch)],
                         r.toc(ch))
                    for ch in r.channels}


def _strip(records):
    """Records as compared: everything but the free text (dropped by default)."""
    out = []
    for r in records:
        d = {k: v for k, v in r.items() if k not in ("text", "annotation", "marker_name_1",
                                                         "marker_name_2", "epoch_type",
                                                         "name")}
        if "channels" in d:
            d["channels"] = [{k: v for k, v in c.items() if k != "name"} for c in d["channels"]]
        out.append(d)
    return out


@pytest.fixture(scope="module")
def kept(tmp_path_factory):
    base = tmp_path_factory.mktemp("keep")
    src = _rich_session(base / "jane_roe.mefd")
    rep = _keep(src, base / "S001.mefd")
    return src, base / "S001.mefd", rep


# --------------------------------------------------------------------------- the copy


def test_every_time_and_sample_is_the_sources(kept):
    src, dst, rep = kept
    assert rep.shift_us is None
    assert rep.verification.ok, rep.verification.summary()
    assert rep.verification.times_checked > 50 and not rep.verification.leaks
    a, b = _read_all(src), _read_all(dst)
    assert sorted(a) == sorted(b) == ["ch1", "ch2", "ch3"]
    for ch in a:
        assert b[ch][0]["start_time"] == a[ch][0]["start_time"]
        assert b[ch][0]["end_time"] == a[ch][0]["end_time"]
        np.testing.assert_array_equal(a[ch][1], b[ch][1])
    assert _segments_and_toc(src) == _segments_and_toc(dst)
    with mef3io.Reader(str(dst)) as r:
        for ch in r.channels:
            for s in r.segments(ch):
                assert s["provenance"]["last_operation"] == "anonymize"


def test_records_keep_their_times_at_every_level(kept):
    src, dst, _ = kept
    ra, rb = _all_records(src), _all_records(dst)
    assert {r["level"] for r in rb} == {"session", "channel", "segment"}
    assert _strip(ra) == _strip(rb)
    seiz = [r for r in rb if r["type"] == "Seiz"]
    assert seiz and all(c["onset"] != 0 for c in seiz[0]["channels"][:3])


def test_identity_is_gone_as_in_a_shifted_copy(kept):
    src, dst, rep = kept
    assert rep.session_name == "S001" and rep.pseudonym == "S001"
    with mef3io.Reader(str(dst)) as r:
        info = r.info("ch1")
        for k in ("subject_name_1", "subject_name_2", "subject_id", "recording_location",
                  "session_description", "channel_description"):
            assert info[k] == "", k
    blob = b"".join(p.read_bytes() for p in Path(dst).rglob("*") if p.is_file())
    for word in (b"Jane", b"Roe", b"MRN4711", b"Springfield", b"jane_roe"):
        assert word not in blob, word
    # new UUIDs: no source file UUID survives
    src_uuids = {p.read_bytes()[836:852] for p in Path(src).rglob("*") if p.is_file()}
    assert not any(p.read_bytes()[836:852] in src_uuids
                   for p in Path(dst).rglob("*") if p.is_file())


def test_stored_time_fields_are_byte_identical_and_tz_fields_cleared(kept):
    src, dst, _ = kept
    for t1, t2 in zip(_tmets(src), _tmets(dst)):
        a, b = t1.read_bytes(), t2.read_bytes()
        assert a[16:32] == b[16:32]                               # UH start/end, as stored
        rto, dst_s, dst_e, gmt = struct.unpack_from("<qqqi", b, S3)
        assert (rto, dst_s, dst_e, gmt) == (struct.unpack_from("<q", a, S3)[0],
                                            UUTC_NO_ENTRY, UUTC_NO_ENTRY, 0)
        assert b[S3 + 28:S3 + 924] == b"\0" * (924 - 28)          # subject + location cleared
        assert b[S2 + 6432:S2 + 10752] == b"\0" * (10752 - 6432)  # foreign regions cleared
    for tidx in sorted(Path(src).rglob("*.tidx")):
        rel = tidx.relative_to(src)
        x, y = tidx.read_bytes(), (Path(dst) / rel).read_bytes()
        assert x[16:32] == y[16:32]
        for off in range(UH, len(x), 56):
            assert x[off + 8:off + 16] == y[off + 8:off + 16]     # entry times


def test_gmt_and_dst_are_cleared_and_times_unchanged(tmp_path):
    src = tmp_path / "tz.mefd"
    meta = mef3io.Metadata(subject=mef3io.Subject(gmt_offset=-21600))
    with mef3io.Writer(str(src), overwrite=True, metadata=meta) as w:
        w.write("ch1", _signal(2560, 1), START, FS, precision=3)
    for tmet in _tmets(src):
        raw = bytearray(tmet.read_bytes())
        struct.pack_into("<qq", raw, S3 + 8, START - 30 * DAY, START + 30 * DAY)
        _reseal(raw, TMET)
        tmet.write_bytes(bytes(raw))
    rep = _keep(src, tmp_path / "Z9.mefd")
    assert rep.verification.ok
    for tmet in _tmets(tmp_path / "Z9.mefd"):
        assert struct.unpack_from("<qqqi", tmet.read_bytes(), S3) == \
            (0, UUTC_NO_ENTRY, UUTC_NO_ENTRY, 0)
    a, b = _read_all(src), _read_all(tmp_path / "Z9.mefd")
    assert b["ch1"][0]["start_time"] == a["ch1"][0]["start_time"] == START
    assert b["ch1"][0]["gmt_offset"] == 0


# --------------------------------------------------------------------------- offsets


def test_a_recording_time_offset_is_kept_with_the_times(tmp_path):
    rto = START - 6 * 3600 * SECOND
    src = _legacy(tmp_path / "rto.mefd", rto=rto)
    rep = _keep(src, tmp_path / "R.mefd")
    assert rep.verification.ok and rep.source_recording_time_offset == rto
    assert any("keeps it" in n for n in rep.notes)
    a, b = _read_all(src), _read_all(tmp_path / "R.mefd")
    for ch in a:
        assert b[ch][0]["start_time"] == a[ch][0]["start_time"] == START
        assert b[ch][0]["recording_time_offset"] == rto
        np.testing.assert_array_equal(a[ch][1], b[ch][1])
    assert _all_records(tmp_path / "R.mefd")[0]["time"] == _all_records(src)[0]["time"]


def test_segments_that_disagree_on_the_offset_keep_their_times(tmp_path):
    src = tmp_path / "dis.mefd"
    with mef3io.Writer(str(src), overwrite=True) as w:
        for ch in ("a", "b"):
            w.write(ch, _signal(2560, 1), START, FS, precision=3)
    _set_rto(src, START - DAY, ["a"])
    shifted = mef3io.anonymize.check(src, tmp_path / "D.mefd", SHIFT)
    assert "rto-disputed" in shifted.codes
    assert mef3io.anonymize.check(src, tmp_path / "D.mefd").ok
    rep = _keep(src, tmp_path / "D.mefd")
    assert rep.verification.ok
    assert _segments_and_toc(src) == _segments_and_toc(tmp_path / "D.mefd")
    a, b = _read_all(src), _read_all(tmp_path / "D.mefd")
    for ch in a:
        assert b[ch][0]["start_time"] == a[ch][0]["start_time"]
        assert b[ch][0]["recording_time_offset"] == a[ch][0]["recording_time_offset"]
        np.testing.assert_array_equal(a[ch][1], b[ch][1])


# --------------------------------------------------------------------------- cut, encryption


def test_a_cut_keeps_the_times(tmp_path):
    src = _rich_session(tmp_path / "jane_roe.mefd")
    t0 = START + 10 * SECOND + 3_333          # off the sample grid: edge blocks re-encoded
    t1 = START + 40 * SECOND
    rep = _keep(src, tmp_path / "C.mefd", time_range=(t0, t1))
    assert rep.verification.ok, rep.verification.summary()
    assert rep.blocks_trimmed > 0
    assert _verify(src, tmp_path / "C.mefd", time_range=(t0, t1)).ok
    with mef3io.Reader(str(src)) as r1, mef3io.Reader(str(tmp_path / "C.mefd")) as r2:
        for ch in r2.channels:
            info = r2.info(ch)
            assert t0 <= info["start_time"] < t0 + SECOND / FS
            whole = r2.read(ch)
            np.testing.assert_array_equal(
                whole, r1.read(ch, info["start_time"], info["start_time"] + len(whole) * SECOND / FS))
    times = [r["time"] for r in _all_records(tmp_path / "C.mefd")]
    want = [r["time"] for r in _all_records(src)
            if r["time"] is not None and t0 <= r["time"] < t1]
    assert sorted(t for t in times if t0 <= t < t1) == sorted(want)


def test_encrypted_source_to_reencrypted_copy_keeps_the_times(tmp_path):
    src = _rich_session(tmp_path / "enc.mefd", password=True)
    rep = _keep(src, tmp_path / "K2.mefd", password="pw-two",
                output_password=("new-one", "new-two"))
    assert rep.output_encrypted and rep.verification.ok
    a, c = _read_all(src, "pw-two"), _read_all(tmp_path / "K2.mefd", "new-two")
    for ch in a:
        assert c[ch][0]["start_time"] == a[ch][0]["start_time"]
        np.testing.assert_array_equal(a[ch][1], c[ch][1])
        assert c[ch][0]["subject_name_1"] == ""
    assert _segments_and_toc(src, "pw-two") == _segments_and_toc(tmp_path / "K2.mefd", "new-two")
    assert _strip(_all_records(src, "pw-two")) == _strip(_all_records(tmp_path / "K2.mefd",
                                                                      "new-two"))
    v = _verify(src, tmp_path / "K2.mefd", password="pw-two", output_password="new-two")
    assert v.ok, v.summary()


# --------------------------------------------------------------------------- verify


def _copy(dst, tmp_path, name):
    out = tmp_path / name / "S001.mefd"
    shutil.copytree(dst, out)
    return out


def _bump_tidx_entry(path):
    p = next(Path(path).glob("ch1.timd/ch1-000000.segd/*.tidx"))
    raw = bytearray(p.read_bytes())
    (t,) = struct.unpack_from("<q", raw, UH + 8)
    struct.pack_into("<q", raw, UH + 8, t - 1)
    _reseal(raw)
    p.write_bytes(bytes(raw))


def _bump_uh(path):
    rel = next(Path(path).glob("ch2.timd/*.segd/*.tmet")).relative_to(path)
    (t,) = struct.unpack_from("<q", (Path(path) / rel).read_bytes(), 16)
    _plant_uh(path, rel, 16, t - SECOND, TMET)


def _bump_block(path):
    tdat = next(Path(path).glob("ch1.timd/ch1-000000.segd/*.tdat"))
    (t,) = struct.unpack_from("<q", tdat.read_bytes(), UH + 40)
    _plant_block_time(path, t - 1)


def _bump_record(field):
    def f(path):
        recs = _all_records(path)
        first = recs[0]["time"]
        _plant_record(path, field, -(first - SECOND) if field == "header" else -(START + DAY))
    return f


# what to alter -> the field verify must name
ALTERED = {
    "universal header start time": (_bump_uh, "universal header start_time"),
    "index entry time": (_bump_tidx_entry, "index entry start_time"),
    "block header time": (_bump_block, "block header start_time"),
    "record header time": (_bump_record("header"), "record header time"),
    "Seiz channel onset": (_bump_record("seiz"), "Seiz channel onset"),
    "Epoc body time": (_bump_record("epoc"), "record body time at 16"),
    "record index time": (lambda p: _plant_ridx(p, -(START + 7 * SECOND)), "record index time"),
    "recording_time_offset": (lambda p: _plant_s3(p, 0, START - DAY), "recording_time_offset"),
}


@pytest.mark.parametrize("name", list(ALTERED))
def test_verify_catches_one_altered_time(tmp_path, kept, name):
    src, dst, _ = kept
    assert _verify(src, dst).ok
    bad = _copy(dst, tmp_path, name.replace(" ", "_"))
    alter, field = ALTERED[name]
    alter(bad)
    rep = _verify(src, bad)
    assert not rep.ok, f"{name}: verify passed an altered time"
    assert any(p.field == field for p in rep.problems), rep.summary()


def test_verify_tells_a_kept_copy_from_a_shifted_one(tmp_path, kept):
    src, dst, _ = kept
    assert not _verify(src, dst, SHIFT).ok
    shifted = _keep(src, tmp_path / "S002.mefd", shift_us=SHIFT)
    assert shifted.verification.ok and shifted.shift_us == SHIFT
    assert not _verify(src, tmp_path / "S002.mefd").ok
    assert _verify(src, tmp_path / "S002.mefd", SHIFT).ok


def test_zero_is_not_a_way_to_keep_the_times(tmp_path, kept):
    src, _, _ = kept
    with pytest.raises(ValueError) as exc:
        _keep(src, tmp_path / "Z.mefd", shift_us=0)
    assert exc.value.code == "shift-zero" and "leave the shift out" in str(exc.value)


# --------------------------------------------------------------------------- report, CLI


def test_the_report_says_the_times_were_kept(kept):
    src, _, rep = kept
    assert rep.to_dict()["shift_us"] is None
    assert "every time kept" in rep.summary()
    assert "source_start_us" not in rep.to_dict() and "source_start_us" not in repr(rep)
    with mef3io.Reader(str(src)) as r:
        assert rep.source_start_us == r.info("ch1")["start_time"]


def test_cli_keeps_the_times_by_default(tmp_path, kept, capsys):
    from mef3io.__main__ import main

    src, _, _ = kept
    assert main(["anonymize", str(src), str(tmp_path / "A.mefd")]) == 0
    out = capsys.readouterr().out
    assert "every time kept" in out and "verification: passed" in out
    assert _segments_and_toc(src) == _segments_and_toc(tmp_path / "A.mefd")
    assert main(["anonymize", str(src), str(tmp_path / "B.mefd"), "--shift-us", str(SHIFT)]) == 0
    a, b = _read_all(src), _read_all(tmp_path / "B.mefd")
    assert b["ch1"][0]["start_time"] == a["ch1"][0]["start_time"] + SHIFT
