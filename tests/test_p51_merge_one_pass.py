"""Merge in ONE pass: per-source time ranges, anonymize inside the merge, threads.

Synthetic sessions only. Pinned here: each source trimmed to its own range
(overlap judged on the trimmed data), channels left out, identity of EVERY
source absent from the result, one shift for all, text drop/keep, records
dropped, byte-identical output for 1 and 4 threads (but for the 1024-byte
universal headers, which hold fresh UUIDs), cancel leaves nothing, refusals.
"""
from __future__ import annotations

import pickle
import re
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
    # whole-second blocks: every cut falls on a block edge, nothing is re-encoded
    assert (rep.blocks_trimmed, rep.blocks, rep.records) == (0, 27, 3)
    assert mef3io.merge.verify([a, b], tmp_path / "m.mefd", time_ranges=RANGES,
                               metadata={"subject_name_1": "S"}).ok
    # a's first 30 s then b's 30..90 s, exactly
    got = _read(tmp_path / "m.mefd", "A", START, START + 90 * SECOND)
    assert len(got) == int(90 * FS) and not np.isnan(got).any()
    want_a = _read(tmp_path / "a.mefd", "A", START, START + 30 * SECOND)
    want_b = _read(tmp_path / "b.mefd", "A", START + 30 * SECOND, START + 90 * SECOND)
    np.testing.assert_array_equal(got, np.concatenate([want_a, want_b]))
    # annotations outside their source's range are left out
    # a's note at 50 s lies after a's range (0..30 s); b's two (40, 80 s) are inside
    assert rep.records_outside_window == 1


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


@pytest.mark.parametrize("both_parts", [False, True])
@pytest.mark.parametrize("part_first", [True, False])
def test_parts_cut_from_sources_with_records_merge_and_verify(tmp_path, both_parts, part_first):
    """Reported against 2.1.1: a transform(time_range) part with notes, merged
    with a recording (with or without notes) must pass the merge verification
    (provenance_UUID of every file). Could not be reproduced; pinned here, and
    for the one-pass merge with per-source ranges and records kept."""
    a = _session(tmp_path / "a.mefd", [(0, 60)], channels=("A", "B"),
                 notes=[(5, "x"), (50, "y")])
    b = _session(tmp_path / "b.mefd", [(100, 60)], channels=("A", "B"))
    pa = tmp_path / "pa.mefd"
    _quiet(mef3io.transform_session, a, pa, time_range=(START, START + 30 * SECOND))
    srcs = [pa, b]
    if both_parts:
        pb = tmp_path / "pb.mefd"
        _quiet(mef3io.transform_session, b, pb,
               time_range=(START + 100 * SECOND, START + 130 * SECOND))
        srcs = [pa, pb]
    if not part_first:
        srcs.reverse()
    rep = _quiet(mef3io.merge_sessions, srcs, tmp_path / "m.mefd")
    assert rep.verification.ok, rep.summary()
    assert mef3io.merge.verify(srcs, tmp_path / "m.mefd").ok
    # the same in one pass from the uncut sources, records kept
    rep = _quiet(mef3io.merge_sessions, [a, b], tmp_path / "o.mefd",
                 time_ranges=[(START, START + 30 * SECOND), None])
    assert rep.verification.ok and rep.records >= 1, rep.summary()


# ---- review round 1 ---------------------------------------------------------


def test_one_sources_patient_in_anothers_kept_text_is_removed(tmp_path):
    """Source B's patient named in source A's kept annotation text (and the
    reverse): the identity of EVERY source is removed from every source's
    text, and the paths of the sources never reach the result."""
    chs = tuple(f"LA{i}" for i in range(1, 9))
    a = _session(tmp_path / "Alpha_Doe.mefd", [(0, 60)], subject="Jane", channels=chs,
                 notes=[(5, "Smithers called"), (20, "nurse Smithers here")])
    b = _session(tmp_path / "Beta_Roe.mefd", [(100, 60)], subject="Smithers", channels=chs,
                 notes=[(110, "visited by Smithers"), (120, "Jane arrived")])
    rep = _quiet(mef3io.merge_sessions, [a, b], tmp_path / "S1.mefd",
                 anonymize={"text": "keep", "shift_us": SHIFT}, n_threads=4)
    assert rep.verification.ok, rep.summary()
    blob = b"".join(f.read_bytes() for f in (tmp_path / "S1.mefd").rglob("*") if f.is_file())
    for needle in (b"Jane", b"Smithers", b"Alpha", b"Beta", b"Roe", b"Doe"):
        assert needle not in blob, needle
    assert b"called" in blob and b"arrived" in blob  # the rest of the text is kept


def test_overlap_is_judged_on_the_data_the_copy_keeps(tmp_path):
    """Kept blocks reach past the range end (a sample when exact, a block
    otherwise): check() must say so, never pass and fail at verify."""
    a = _session(tmp_path / "a.mefd", [(0.3, 60)], subject="S")
    b = _session(tmp_path / "b.mefd", [(29.7, 60)], subject="S")
    ranges = [(START, START + 30 * SECOND), (START + 30 * SECOND, START + 100 * SECOND)]
    for exact in (True, False):
        dst = tmp_path / f"m{exact}.mefd"
        c = mef3io.merge.check([a, b], dst, time_ranges=ranges, exact=exact)
        assert "merge-overlap" in c.codes, (exact, c.codes)
        with pytest.raises(mef3io.WriteConflictError) as e:
            mef3io.merge_sessions([a, b], dst, time_ranges=ranges, exact=exact)
        assert e.value.code == "merge-overlap"
        assert not dst.exists() and not (tmp_path / f"m{exact}.mefd.partial").exists()


def _source_times_in(text):
    """Numbers in `text` within two days of the sources' recording (a duration
    such as a span is not a time and may be named)."""
    return [x for x in re.findall(r"\d{10,}", text) if abs(int(x) - START) < 2 * 86400 * SECOND]


def test_anonymized_messages_name_no_source_time(tmp_path):
    """Every refusal, check() entry and raised error of an anonymized merge is
    free of the sources' times (the re-identification key, #111)."""
    a = _session(tmp_path / "a.mefd", [(0, 60)], subject="Jane")
    b = _session(tmp_path / "b.mefd", [(30, 60)], subject="John")
    c = mef3io.merge.check([a, b], tmp_path / "S.mefd", anonymize={"shift_us": SHIFT})
    assert "merge-overlap" in c.codes
    for r in c.refusals:
        assert not _source_times_in(r.message), r.message
    with pytest.raises(mef3io.WriteConflictError) as e:
        mef3io.merge_sessions([a, b], tmp_path / "S.mefd", anonymize={"shift_us": SHIFT})
    assert not _source_times_in(str(e.value)), str(e.value)
    # a shift into the sources' own range is refused without printing the range
    c = mef3io.merge.check([a, b], tmp_path / "S2.mefd", time_ranges=RANGES,
                           anonymize={"shift_us": 10 * SECOND})
    assert c.refusals
    for r in c.refusals:
        assert not _source_times_in(r.message), r.message


def test_anonymized_report_keeps_source_paths_in_memory_only(tmp_path):
    a, b = _pair(tmp_path)
    rep = _quiet(mef3io.merge_sessions, [a, b], tmp_path / "S.mefd", time_ranges=RANGES,
                 anonymize={"shift_us": SHIFT})
    assert tuple(rep.sources) == (str(a), str(b)) and rep.source_count == 2
    rep = mef3io.merge.MergeReport._from({**rep.to_dict(include_original_times=True)})
    rep = type(rep)(**{**rep.__dict__, "notes": (f"{a}: holds no blocks; it is left out",)})
    for text in (repr(rep), rep.summary(), str(rep.to_dict()), repr(pickle.loads(pickle.dumps(rep)).__dict__)):
        assert "a.mefd" not in text and "b.mefd" not in text, text
    assert "source 1: holds no blocks" in rep.summary()
    assert "2 recording(s)" in rep.summary()
    assert pickle.loads(pickle.dumps(rep)).source_count == 2
    kept = rep.to_dict(include_original_times=True)
    assert tuple(kept["sources"]) == (str(a), str(b)) and str(a) in kept["notes"][0]
    assert str(a) in pickle.loads(pickle.dumps(rep.with_original_times_in_pickle())).sources[0]
    # a plain (not anonymized) merge reports its sources as before
    plain = _quiet(mef3io.merge_sessions, [a, b], tmp_path / "P.mefd", time_ranges=RANGES,
                   metadata={"subject_name_1": "S"})
    assert tuple(plain.to_dict()["sources"]) == (str(a), str(b))


def test_a_2_1_1_report_still_loads_and_summarizes(tmp_path):
    a, b = _pair(tmp_path)
    rep = _quiet(mef3io.merge_sessions, [a, b], tmp_path / "P.mefd", time_ranges=RANGES,
                 metadata={"subject_name_1": "S"})
    old = rep.to_dict()
    new_keys = ("channels_without_samples", "blocks_trimmed", "records_outside_window",
                "pseudonym", "shift_us", "source_recording_time_offset",
                "record_text_fields_changed", "record_bodies_cleared", "records_dropped",
                "source_count")
    for k in new_keys:
        del old[k]
    loaded = mef3io.merge.MergeReport._from(old)
    assert loaded.channels_without_samples == () and loaded.records_dropped == 0
    assert loaded.pseudonym == "" and loaded.shift_us is None
    assert "2 recording(s)" in loaded.summary()
    assert pickle.loads(pickle.dumps(loaded)).blocks == rep.blocks
