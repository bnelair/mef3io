"""records="drop" on anonymize_session, transform_session and merge_sessions.

The copy holds no annotation at any level (no .rdat/.ridx at all, like a
session that never had any); signal data is as with records="keep"; verify
accepts the drop and still fails a copy that holds records; "keep" is unchanged.
"""
from __future__ import annotations

import json
import pickle
import shutil
import warnings
from pathlib import Path

import numpy as np
import pytest

import mef3io
from mef3io import anonymize, merge, transform

from test_p29_anonymize import _reseal, START, SECOND, _all_records, _read_all, _rich_session
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


def _rich_pair(tmp_path):
    a = _rich_session(tmp_path / "ra.mefd")
    b = _rich_session(tmp_path / "rb.mefd", channels=("d1", "d2"))
    return a, b


def test_merge_drop_rich_sources(tmp_path):
    a, b = _rich_pair(tmp_path)
    levels = {r["level"] for r in _all_records(a)} | {r["level"] for r in _all_records(b)}
    assert {"session", "channel", "segment"} <= levels
    out = tmp_path / "m.mefd"
    rep = _quiet(merge.merge_sessions, [a, b], out, records="drop")
    assert rep.verification.ok, rep.summary()
    assert rep.records == 0 and rep.records_dropped > 0
    assert _no_record_files(out) and _all_records(out) == []
    assert merge.verify([a, b], out, records="drop").ok
    assert mef3io.validate_session(str(out)).ok


def _plant(kept, dropped, rel_dir, stem):
    """Copy one record file pair from the kept copy into the dropped copy.

    A segment's files must share their level UUID, so the planted pair takes
    its siblings' (else the check that fails would be the UUID one, not the
    no-records one)."""
    sib = next((Path(dropped) / rel_dir).glob("*.tmet"), None)
    for ext in ("rdat", "ridx"):
        dst = Path(dropped) / rel_dir / f"{stem}.{ext}"
        shutil.copy(Path(kept) / rel_dir / f"{stem}.{ext}", dst)
        if sib is not None:
            raw = bytearray(dst.read_bytes())
            raw[820:836] = sib.read_bytes()[820:836]
            _reseal(raw)
            dst.write_bytes(bytes(raw))


def _levels_of(kept):
    """(relative dir, stem) of one session / channel / segment record file."""
    kept = Path(kept)
    out = {}
    for f in sorted(kept.rglob("*.rdat")):
        level = ("segment" if f.parent.suffix == ".segd" else
                 "channel" if f.parent.suffix == ".timd" else "session")
        out.setdefault(level, (f.parent.relative_to(kept), f.stem))
    assert set(out) == {"segment", "channel", "session"}
    return out


@pytest.mark.parametrize("level", ["segment", "channel", "session"])
@pytest.mark.parametrize("op", ["transform", "anonymize", "merge"])
def test_one_planted_record_file_fails_drop_verify(rich, tmp_path, op, level):
    if op == "merge":
        srcs = _rich_pair(tmp_path)
        run = lambda dst, **kw: _quiet(merge.merge_sessions, list(srcs), dst, **kw)
        verify = lambda dst: merge.verify(list(srcs), dst, records="drop")
    elif op == "transform":
        run = lambda dst, **kw: _quiet(mef3io.transform_session, rich, dst, **kw)
        verify = lambda dst: transform.verify(rich, dst, records="drop")
    else:
        run = lambda dst, **kw: _quiet(mef3io.anonymize_session, rich, dst, None, **kw)
        verify = lambda dst: anonymize.verify(rich, dst, None, records="drop")
    kept, dropped = tmp_path / "kept.mefd", tmp_path / "dropped.mefd"
    run(kept)
    run(dropped, records="drop")
    assert verify(dropped).ok
    rel, stem = _levels_of(kept)[level]
    _plant(kept, dropped, rel, stem)
    assert not verify(dropped).ok


def test_summary_and_old_reports(rich, tmp_path):
    for rep in (
        _quiet(mef3io.transform_session, rich, tmp_path / "t.mefd", records="drop"),
        _quiet(mef3io.anonymize_session, rich, tmp_path / "a.mefd", None, records="drop"),
        _quiet(merge.merge_sessions, list(_rich_pair(tmp_path)), tmp_path / "m.mefd",
               records="drop"),
    ):
        assert f"{rep.records_dropped} annotation(s) left out" in rep.summary()
        # a report from 2.1.1 has no records_dropped: it still loads, as 0
        d = rep.to_dict()
        del d["records_dropped"]
        old = type(rep)._from(d) if type(rep) is not mef3io.anonymize.AnonymizeReport else None
        if old is not None:
            assert old.records_dropped == 0
            assert "annotation(s) left out" not in old.summary()
    kept = _quiet(mef3io.transform_session, rich, tmp_path / "k.mefd")
    assert "left out" not in kept.summary().replace("channels left out", "")


def test_anonymize_pickle_without_records_dropped(rich, tmp_path):
    from mef3io.anonymize import _unpickle_report

    rep = _quiet(mef3io.anonymize_session, rich, tmp_path / "a.mefd", None, records="drop")
    values = {f: getattr(rep, f) for f in rep.__dataclass_fields__}
    del values["records_dropped"]
    assert _unpickle_report(values, False).records_dropped == 0
    assert pickle.loads(pickle.dumps(rep)).records_dropped == rep.records_dropped
