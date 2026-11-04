"""records="drop" on anonymize_session, transform_session and merge_sessions.

The copy holds no annotation at any level (no .rdat/.ridx at all, like a
session that never had any); signal data is as with records="keep"; verify
accepts the drop and still fails a copy that holds records; "keep" is unchanged.
"""
from __future__ import annotations

import json
import warnings
from pathlib import Path

import numpy as np
import pytest

import mef3io
from mef3io import anonymize, merge, transform

from test_p29_anonymize import START, SECOND, _all_records, _read_all, _rich_session
from test_p34_merge import _session


def _quiet(fn, *a, **kw):
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        return fn(*a, **kw)


def _no_record_files(path):
    return not (list(Path(path).rglob("*.rdat")) or list(Path(path).rglob("*.ridx")))


@pytest.fixture(scope="module")
def rich(tmp_path_factory):
    base = tmp_path_factory.mktemp("drop_rich")
    return _rich_session(base / "jane_roe.mefd")


def _same_data(src, dst):
    a, b = _read_all(src), _read_all(dst)
    assert set(a) == set(b)
    for ch in a:
        np.testing.assert_array_equal(a[ch][1], b[ch][1])


def test_source_has_records_at_every_level(rich):
    levels = {r["level"] for r in _all_records(rich)}
    assert {"session", "channel", "segment"} <= levels


@pytest.mark.parametrize("op", ["anonymize", "anonymize_shift", "transform"])
def test_drop_removes_every_record(rich, tmp_path, op):
    n = len(_all_records(rich))
    dst = tmp_path / "out.mefd"
    shift = 400 * 86400 * SECOND if op == "anonymize_shift" else None
    if op == "transform":
        rep = _quiet(mef3io.transform_session, rich, dst, records="drop")
    else:
        rep = _quiet(mef3io.anonymize_session, rich, dst, shift, records="drop")
    assert rep.verification.ok, rep.summary()
    assert rep.records == 0 and rep.records_dropped == n
    assert _no_record_files(dst)
    assert _all_records(dst) == []
    _same_data(rich, dst)
    assert mef3io.validate_session(str(dst)).ok
    if op == "transform":
        assert transform.verify(rich, dst, records="drop").ok
    else:
        assert anonymize.verify(rich, dst, shift, records="drop").ok


def test_keep_is_unchanged(rich, tmp_path):
    n = len(_all_records(rich))
    rep = _quiet(mef3io.transform_session, rich, tmp_path / "k.mefd")
    assert rep.records == n and rep.records_dropped == 0
    assert len(_all_records(tmp_path / "k.mefd")) == n
    rep = _quiet(mef3io.transform_session, rich, tmp_path / "k2.mefd", records="keep")
    assert rep.records == n


def test_verify_stays_strict(rich, tmp_path):
    kept = tmp_path / "kept.mefd"
    dropped = tmp_path / "dropped.mefd"
    _quiet(mef3io.transform_session, rich, kept)
    _quiet(mef3io.transform_session, rich, dropped, records="drop")
    # a copy that holds records fails when none were expected ...
    assert not transform.verify(rich, kept, records="drop").ok
    # ... and one that lost them fails the default
    assert not transform.verify(rich, dropped).ok
    assert not anonymize.verify(rich, dropped).ok


def test_bad_value_is_refused(rich, tmp_path):
    for fn in (mef3io.transform_session, mef3io.anonymize_session):
        with pytest.raises(ValueError):
            fn(rich, tmp_path / "x.mefd", records="none")
    assert not (tmp_path / "x.mefd").exists()
    for mod in (transform, anonymize):
        rep = mod.check(rich, tmp_path / "y.mefd", records="nope")
        assert "records-invalid" in rep.codes
        assert mod.check(rich, tmp_path / "y.mefd", records="drop").ok
    with pytest.raises(ValueError):
        merge.merge_sessions([rich, rich], tmp_path / "z.mefd", records=1)
    assert "records-invalid" in transform.REFUSAL_CODES


def test_combined_with_cut_exclusion_and_text(rich, tmp_path):
    dst = tmp_path / "c.mefd"
    rep = _quiet(mef3io.anonymize_session, rich, dst, None, records="drop", text="keep",
                 exclude_channels=["ch3"], time_range=(START, START + 30 * SECOND))
    assert rep.verification.ok, rep.summary()
    assert tuple(rep.channels_kept) == ("ch1", "ch2") and rep.records == 0
    assert rep.records_dropped > 0 and rep.records_outside_window == 0
    assert _no_record_files(dst)
    rep = _quiet(mef3io.transform_session, rich, tmp_path / "t.mefd", records="drop",
                 time_range=(START, START + 30 * SECOND))
    assert rep.verification.ok and rep.records == 0 and _no_record_files(tmp_path / "t.mefd")


def test_a_channel_of_only_records_is_left_out(tmp_path):
    src = _session(tmp_path / "s.mefd", [(0, 10)], notes=[(1, "a")])
    with mef3io.Writer(str(src)) as w:
        w.write_annotations([{"type": "Note", "time": START + SECOND, "text": "r"}], "onlyrec")
    assert (Path(src) / "onlyrec.timd").is_dir()
    rep = _quiet(mef3io.transform_session, src, tmp_path / "o.mefd", records="drop")
    assert rep.verification.ok, rep.summary()
    assert "onlyrec" in rep.channels_excluded and "onlyrec" not in rep.channels_kept
    assert _no_record_files(tmp_path / "o.mefd")
    assert transform.verify(src, tmp_path / "o.mefd", records="drop").ok


def test_merge_drop(tmp_path):
    a = _session(tmp_path / "a.mefd", [(0, 60)], notes=[(5, "a1")])
    b = _session(tmp_path / "b.mefd", [(100, 50)], notes=[(110, "b1"), (101, "b0")])
    out = tmp_path / "m.mefd"
    rep = _quiet(merge.merge_sessions, [a, b], out, records="drop")
    assert rep.verification.ok, rep.summary()
    assert rep.records == 0 and rep.records_dropped == 3
    assert _no_record_files(out) and _all_records(out) == []
    assert merge.verify([a, b], out, records="drop").ok
    assert not merge.verify([a, b], out).ok  # the default expects the annotations
    assert merge.check([a, b], tmp_path / "n.mefd", records="drop").ok
    assert mef3io.validate_session(str(out)).ok
    kept = _quiet(merge.merge_sessions, [a, b], tmp_path / "k.mefd")
    assert kept.records == 3 and kept.records_dropped == 0
    assert not merge.verify([a, b], tmp_path / "k.mefd", records="drop").ok


def test_the_cli(tmp_path, capsys):
    from mef3io.__main__ import main

    a = _session(tmp_path / "a.mefd", [(0, 20)], notes=[(1, "x")])
    b = _session(tmp_path / "b.mefd", [(40, 20)], notes=[(41, "y")])
    assert main(["transform", str(a), str(tmp_path / "t.mefd"), "--drop-records", "--json"]) == 0
    assert json.loads(capsys.readouterr().out)["records_dropped"] == 1
    assert main(["anonymize", str(a), str(tmp_path / "an.mefd"), "--drop-records", "--json"]) == 0
    assert json.loads(capsys.readouterr().out)["records"] == 0
    assert main(["merge", str(tmp_path / "m.mefd"), str(a), str(b), "--drop-records", "--json"]) == 0
    assert json.loads(capsys.readouterr().out)["records_dropped"] == 2
    assert _no_record_files(tmp_path / "m.mefd")
