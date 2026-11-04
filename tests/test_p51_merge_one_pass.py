"""Merge in ONE pass: per-source time ranges, anonymize inside the merge, threads.

Synthetic sessions only. Pinned here: each source trimmed to its own range
(overlap judged on the trimmed data), channels left out, identity of EVERY
source absent from the result, one shift for all, text drop/keep, records
dropped, byte-identical output for 1 and 4 threads (but for the 1024-byte
universal headers, which hold fresh UUIDs), cancel leaves nothing, refusals.
"""
from __future__ import annotations

import warnings
from pathlib import Path

import numpy as np
import pytest

import mef3io

from test_p34_merge import FS, SECOND, START, UH, _session

SHIFT = 40 * 365 * 86400 * SECOND  # ~40 years


def _quiet(fn, *a, **kw):
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        return fn(*a, **kw)


def _pair(tmp_path, **kw):
    a = _session(tmp_path / "a.mefd", [(0, 60)], subject="Jane", channels=("A", "B", "C"),
                 notes=[(5, "checked electrodes"), (50, "late recording")])
    b = _session(tmp_path / "b.mefd", [(30, 60)], subject="John", channels=("A", "B", "D"),
                 notes=[(40, "arrived early"), (80, "left late")])
    return a, b


RANGES = [(START, START + 30 * SECOND), (START + 30 * SECOND, START + 100 * SECOND)]


def _read(path, ch, t0, t1):
    with mef3io.Reader(str(path)) as r:
        return r.read(ch, t0, t1)


def _body_bytes(root: Path):
    """Every file's bytes without the universal header (fresh UUIDs)."""
    out = {}
    for f in sorted(root.rglob("*")):
        if f.is_file():
            out[str(f.relative_to(root))] = f.read_bytes()[UH:]
    return out


def test_time_ranges_make_overlapping_sources_merge(tmp_path):
    a, b = _pair(tmp_path)
    assert "merge-overlap" in mef3io.merge.check([a, b], tmp_path / "m.mefd",
                                                 metadata={"subject_name_1": "S"}).codes
    rep = _quiet(mef3io.merge_sessions, [a, b], tmp_path / "m.mefd", time_ranges=RANGES,
                 metadata={"subject_name_1": "S"})
    assert rep.verification.ok, rep.summary()
    assert rep.blocks_trimmed >= 2 or rep.blocks > 0
    assert mef3io.merge.verify([a, b], tmp_path / "m.mefd", time_ranges=RANGES,
                               metadata={"subject_name_1": "S"}).ok
    # a's first 30 s then b's 30..90 s, exactly
    got = _read(tmp_path / "m.mefd", "A", START, START + 90 * SECOND)
    assert len(got) == int(90 * FS) and not np.isnan(got).any()
    want_a = _read(tmp_path / "a.mefd", "A", START, START + 30 * SECOND)
    want_b = _read(tmp_path / "b.mefd", "A", START + 30 * SECOND, START + 90 * SECOND)
    np.testing.assert_array_equal(got, np.concatenate([want_a, want_b]))
    # annotations outside their source's range are left out
    assert rep.records_outside_window == 2 + 0 or rep.records_outside_window >= 1


def test_whole_source_entry_and_channel_without_samples(tmp_path):
    a, b = _pair(tmp_path)
    # b's range lies after everything b has for D? D exists only in b: keep a whole, trim b
    rep = _quiet(mef3io.merge_sessions, [a, b], tmp_path / "m.mefd",
                 time_ranges=[None, (START + 60 * SECOND, START + 100 * SECOND)],
                 metadata={"subject_name_1": "S"}, exclude_channels=["C"])
    assert rep.verification.ok, rep.summary()
    assert "C" in rep.channels_excluded and set(rep.channels_kept) == {"A", "B", "D"}


def test_a_source_with_nothing_in_its_range_is_refused(tmp_path):
    a, b = _pair(tmp_path)
    c = mef3io.merge.check([a, b], tmp_path / "m.mefd", metadata={"subject_name_1": "S"},
                           time_ranges=[None, (START + 500 * SECOND, START + 600 * SECOND)])
    assert "time-range-empty" in c.codes


@pytest.mark.parametrize("ranges, code", [
    ([None], "merge-time-ranges-count"),
    ([None, None, None], "merge-time-ranges-count"),
])
def test_bad_range_lists_are_refused(tmp_path, ranges, code):
    a, b = _pair(tmp_path)
    assert code in mef3io.merge.check([a, b], tmp_path / "m.mefd", time_ranges=ranges,
                                      metadata={"subject_name_1": "S"}).codes
    with pytest.raises(ValueError) as e:
        mef3io.merge_sessions([a, b], tmp_path / "m.mefd", time_ranges=ranges,
                              metadata={"subject_name_1": "S"})
    assert e.value.code == code
    assert not (tmp_path / "m.mefd").exists() and not (tmp_path / "m.mefd.partial").exists()


def test_anonymize_in_the_merge_removes_every_identity(tmp_path):
    a, b = _pair(tmp_path)
    rep = _quiet(mef3io.merge_sessions, [a, b], tmp_path / "S01.mefd", time_ranges=RANGES,
                 exclude_channels=["C"], anonymize={"shift_us": SHIFT, "pseudonym": "PSEUDO"})
    assert rep.verification.ok, rep.summary()
    assert rep.shift_us == SHIFT and rep.pseudonym == "PSEUDO"
    assert rep.source_start_us == START and rep.source_end_us is not None
    assert "source_start_us" not in rep.to_dict()
    blob = b"".join(f.read_bytes() for f in (tmp_path / "S01.mefd").rglob("*") if f.is_file())
    for needle in (b"Jane", b"John", b"P-1", b"a.mefd", b"b.mefd"):
        assert needle not in blob, needle
    assert b"PSEUDO" in blob
    # one shift for every source
    got = _read(tmp_path / "S01.mefd", "A", START + SHIFT, START + SHIFT + 90 * SECOND)
    want = np.concatenate([_read(a, "A", START, START + 30 * SECOND),
                           _read(b, "A", START + 30 * SECOND, START + 90 * SECOND)])
    np.testing.assert_array_equal(got, want)
    assert mef3io.merge.verify([a, b], tmp_path / "S01.mefd", time_ranges=RANGES,
                               exclude_channels=["C"],
                               anonymize={"shift_us": SHIFT, "pseudonym": "PSEUDO"}).ok


def test_anonymize_keeping_times_and_text(tmp_path):
    a, b = _pair(tmp_path)
    rep = _quiet(mef3io.merge_sessions, [a, b], tmp_path / "S02.mefd", time_ranges=RANGES,
                 anonymize={"text": "keep"})
    assert rep.verification.ok and rep.shift_us is None, rep.summary()
    blob = b"".join(f.read_bytes() for f in (tmp_path / "S02.mefd").rglob("*") if f.is_file())
    assert b"Jane" not in blob and b"John" not in blob
    assert b"arrived early" in blob  # text kept
    rep = _quiet(mef3io.merge_sessions, [a, b], tmp_path / "S03.mefd", time_ranges=RANGES,
                 anonymize=True)
    blob = b"".join(f.read_bytes() for f in (tmp_path / "S03.mefd").rglob("*") if f.is_file())
    assert b"arrived early" not in blob and rep.verification.ok


def test_a_kept_subject_field_must_still_agree(tmp_path):
    a, b = _pair(tmp_path)
    c = mef3io.merge.check([a, b], tmp_path / "m.mefd", time_ranges=RANGES,
                           anonymize={"clear": ["location", "descriptions"]})
    assert "merge-identity-differs" in c.codes
    c = mef3io.merge.check([a, b], tmp_path / "m.mefd", time_ranges=RANGES, anonymize=True)
    assert c.ok, c.summary()


def test_anonymize_refusals(tmp_path):
    a, b = _pair(tmp_path)
    assert "merge-anonymize-metadata" in mef3io.merge.check(
        [a, b], tmp_path / "m.mefd", time_ranges=RANGES, anonymize=True,
        metadata={"subject_name_1": "S"}).codes
    # a name that holds another source's patient
    assert "name-contains-identity" in mef3io.merge.check(
        [a, b], tmp_path / "JohnDoe.mefd", time_ranges=RANGES, anonymize=True).codes
    assert "shift-below-floor" in mef3io.merge.check(
        [a, b], tmp_path / "m.mefd", time_ranges=RANGES, anonymize={"shift_us": SECOND}).codes
    assert "shift-zero" in mef3io.merge.check(
        [a, b], tmp_path / "m.mefd", time_ranges=RANGES, anonymize={"shift_us": 0}).codes
    assert "merge-threads-invalid" in mef3io.merge.check([a, b], tmp_path / "m.mefd",
                                                         n_threads=-1).codes
    with pytest.raises(ValueError) as e:
        mef3io.merge_sessions([a, b], tmp_path / "m.mefd", n_threads=-1)
    assert e.value.code == "merge-threads-invalid"


def test_records_drop_combined(tmp_path):
    a, b = _pair(tmp_path)
    rep = _quiet(mef3io.merge_sessions, [a, b], tmp_path / "S04.mefd", time_ranges=RANGES,
                 anonymize=True, records="drop", exclude_channels=["C"])
    assert rep.verification.ok and rep.records == 0 and rep.records_dropped == 4, rep.summary()
    assert not list((tmp_path / "S04.mefd").rglob("*.rdat"))


def test_output_is_identical_for_one_and_four_threads(tmp_path):
    a, b = _pair(tmp_path)
    for name, n, anon in (("t1", 1, True), ("t4", 4, True), ("p1", 1, None), ("p4", 4, None)):
        _quiet(mef3io.merge_sessions, [a, b], tmp_path / f"{name}.mefd", time_ranges=RANGES,
               n_threads=n, anonymize={"shift_us": SHIFT} if anon else None,
               metadata=None if anon else {"subject_name_1": "S"})
    # the session name is part of the metadata bodies' paths only: compare by relative path
    for x, y in (("t1", "t4"), ("p1", "p4")):
        ax, ay = _body_bytes(tmp_path / f"{x}.mefd"), _body_bytes(tmp_path / f"{y}.mefd")
        assert {k.replace(x, "N") for k in ax} == {k.replace(y, "N") for k in ay}
        ay2 = {k.replace(y, "N"): v for k, v in ay.items()}
        for k, v in ax.items():
            assert ay2[k.replace(x, "N")] == v, k


def test_cancel_with_threads_leaves_nothing(tmp_path):
    a = _session(tmp_path / "a.mefd", [(0, 300)], channels=tuple("ABCDEFGH"))
    b = _session(tmp_path / "b.mefd", [(400, 300)], channels=tuple("ABCDEFGH"))
    token = mef3io.CancelToken()
    calls = []

    def progress(done, total):
        calls.append(done)
        if len(calls) == 3:
            token.cancel()

    with pytest.raises(mef3io.Cancelled):
        _quiet(mef3io.merge_sessions, [a, b], tmp_path / "m.mefd", n_threads=4,
               progress=progress, cancel=token)
    assert not (tmp_path / "m.mefd").exists() and not (tmp_path / "m.mefd.partial").exists()
    assert calls == sorted(calls)  # never backwards


def test_progress_is_monotonic_with_threads(tmp_path):
    a, b = _pair(tmp_path)
    calls = []
    _quiet(mef3io.merge_sessions, [a, b], tmp_path / "m.mefd", time_ranges=RANGES,
           metadata={"subject_name_1": "S"}, n_threads=4,
           progress=lambda d, t: calls.append((d, t)))
    done = [d for d, _ in calls]
    assert done == sorted(done) and calls[-1][0] == calls[-1][1]
