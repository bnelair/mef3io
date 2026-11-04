"""Joining a trimmed (and password-protected) part of a recording with another
session, with session- and channel-level notes, passes the merge's own check."""
import itertools

import numpy as np
import pytest

import mef3io

S = 1_000_000
H = 3600 * S
T0 = 946_684_800_000_000
PW = ("bbbbbbbbbbbb", "ffffffffffff")


def _write(path, start, session_note, channel_note):
    md = mef3io.Metadata(subject=mef3io.Subject(name_1="Q", name_2="Z", id="MRN-1"))
    with mef3io.Writer(str(path), overwrite=True, metadata=md) as w:
        for c in ("a1", "a2", "a3"):
            w.write(c, np.arange(14400, dtype=float), start, 2.0)
        if session_note:
            w.write_annotations([{"time": start + 10 * S, "text": "note"}])
        if channel_note:
            w.write_annotations([{"time": start + 20 * S, "text": "cn"}], channel="a1")


@pytest.mark.parametrize(
    "session_note,channel_note,encrypted,part_first",
    list(itertools.product((False, True), (False, True), (False, True), (False, True))),
)
def test_merge_of_trimmed_part_with_notes_verifies(tmp_path, session_note, channel_note,
                                                   encrypted, part_first):
    a, b, part = tmp_path / "A.mefd", tmp_path / "B.mefd", tmp_path / "part.mefd"
    _write(a, T0, session_note, channel_note)
    _write(b, T0 + 3 * H, session_note, channel_note)
    mef3io.transform_session(str(a), str(part), channels=None, exclude_channels=None,
                             metadata=None, time_range=(T0, T0 + H), exact=True, password="",
                             output_password=PW if encrypted else None)
    pw_part = PW[1] if encrypted else ""
    srcs, pws = ([str(part), str(b)], [pw_part, ""]) if part_first else (
        [str(b), str(part)], ["", pw_part])
    out = tmp_path / "out.mefd"
    mef3io.merge_sessions(srcs, str(out), passwords=pws, exclude_channels=None,
                          output_password=None)
    assert out.exists()
