"""#156 (2.0.1): annotation writes APPEND; they never replace or delete.

Before, `write_annotations` truncated `.rdat` + `.ridx` and wrote only the
batch it was given, so every call deleted the annotations stored earlier. Now
the pair is extended in place (record bytes, index entries, then the headers
that count them; body CRCs extended, never recomputed), created when absent,
and an interrupted append is repaired by `recover_session` without losing a
stored record.
"""
import os
import shutil
import sys
from pathlib import Path

import numpy as np
import pytest

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO))

import mef3io  # noqa: E402
from mef3io import _mef3io as _m  # noqa: E402

START = 1577836800000000
FS = 256.0
S = 1_000_000
UH = 1024
CRC_START = 0xFFFFFFFF


def _session(path, password=None, **kw):
    kwargs = dict(overwrite=True)
    if password:
        kwargs.update(password1=password[0], password2=password[1])
    with mef3io.Writer(str(path), **kwargs) as w:
        w.write_int32("ch1", np.arange(2560, dtype=np.int32), 1.0, START, FS)
    return Path(path)


def _notes(path, channel=None, password="", **kw):
    with mef3io.Reader(str(path), password=password) as r:
        return [(x["time"], x["text"]) for x in r.records(channel, **kw)]


def _files(path, channel=None):
    p = Path(path)
    if channel:
        return p / f"{channel}.timd" / f"{channel}.rdat", p / f"{channel}.timd" / f"{channel}.ridx"
    return p / f"{p.stem}.rdat", p / f"{p.stem}.ridx"


def _note(t, text):
    return {"time": t, "text": text}


def test_two_calls_keep_both_sets_at_session_and_channel_level(tmp_path):
    p = _session(tmp_path / "s.mefd")
    with mef3io.Writer(str(p)) as w:
        w.write_annotations([_note(START + S, "a"), _note(START + 2 * S, "b")])
        w.write_annotations([_note(START + 3 * S, "c")])
        w.write_annotations([_note(START + 4 * S, "d")], channel="ch1")
        w.write_annotations([_note(START + 5 * S, "e")], channel="ch1")
    assert [t for _, t in _notes(p, level="session")] == ["a", "b", "c"]
    assert [t for _, t in _notes(p, "ch1", level="channel")] == ["d", "e"]


def test_a_second_writer_keeps_what_the_first_stored(tmp_path):
    p = _session(tmp_path / "s.mefd")
    with mef3io.Writer(str(p)) as w:
        w.write_annotations([_note(START + S, "first")])
    with mef3io.Writer(str(p)) as w:
        w.write_annotations([_note(START + 2 * S, "second")])
    assert [t for _, t in _notes(p)] == ["first", "second"]


def test_out_of_order_append_reads_back_sorted_and_stays_in_write_order_on_disk(tmp_path):
    p = _session(tmp_path / "s.mefd")
    with mef3io.Writer(str(p)) as w:
        w.write_annotations([_note(START + 10 * S, "late")])
        w.write_annotations([_note(START + 2 * S, "early"), _note(START + 10 * S, "late2")])
    assert [t for _, t in _notes(p)] == ["early", "late", "late2"]  # stable on equal times
    _, ridx = _files(p)
    data = ridx.read_bytes()
    times = [-int.from_bytes(data[UH + i * 24 + 16: UH + i * 24 + 24], "little", signed=True)
             for i in range((len(data) - UH) // 24)]
    assert times == [START + 10 * S, START + 2 * S, START + 10 * S]  # write order
    assert mef3io.Validator(str(p)).validate().ok


def test_appended_file_equals_a_single_call_file(tmp_path):
    a = _session(tmp_path / "a.mefd")
    b = _session(tmp_path / "b.mefd")
    recs = [_note(START + i * S, f"note {i}" * (1 + i % 3)) for i in range(1, 8)]
    with mef3io.Writer(str(a)) as w:
        w.write_annotations(recs)
    with mef3io.Writer(str(b)) as w:
        w.write_annotations(recs[:3])
        w.write_annotations(recs[3:])
    for x, y in zip(_files(a), _files(b)):
        bx, by = x.read_bytes(), y.read_bytes()
        assert len(bx) == len(by)
        assert bx[UH:] == by[UH:]  # body byte for byte
        # header: count, max entry size, span and body CRC agree
        assert bx[4:8] == by[4:8] and bx[32:48] == by[32:48] and bx[8:24] == by[8:24]
    # and the header CRC / body CRC of the appended one are valid
    from mef3io import _mef3io
    for f in _files(b):
        d = f.read_bytes()
        assert _crc(d[4:UH]) == int.from_bytes(d[:4], "little")
        assert _crc(d[UH:]) == int.from_bytes(d[4:8], "little")


def _crc(data):
    return _m.crc32(bytes(data)) if hasattr(_m, "crc32") else _koopman(data)


_TABLE = None


def _koopman(data):
    # Koopman 0x741B8CD7, reflected, start 0xFFFFFFFF, no final inversion
    global _TABLE
    if _TABLE is None:
        poly = 0xEB31D82E  # reflected 0x741B8CD7
        _TABLE = []
        for i in range(256):
            c = i
            for _ in range(8):
                c = (c >> 1) ^ poly if c & 1 else c >> 1
            _TABLE.append(c)
    c = CRC_START
    for b in data:
        c = _TABLE[(c ^ b) & 0xFF] ^ (c >> 8)
    return c


def _rchar_wchar():
    d = {}
    for line in Path("/proc/self/io").read_text().splitlines():
        k, v = line.split(":")
        d[k] = int(v)
    return d["rchar"], d["wchar"]


def _append_cost(path, t):
    with mef3io.Writer(str(path)) as w:
        w.write_annotations([_note(t, "warm up")])  # whatever the first write loads once
        r0, w0 = _rchar_wchar()
        w.write_annotations([_note(t + 1, "x")])
        r1, w1 = _rchar_wchar()
    return r1 - r0, w1 - w0


@pytest.mark.skipif(not Path("/proc/self/io").exists(), reason="needs /proc/self/io")
def test_append_cost_does_not_grow_with_the_file(tmp_path):
    small = _session(tmp_path / "small.mefd")
    big = _session(tmp_path / "big.mefd")
    with mef3io.Writer(str(small)) as w:
        w.write_annotations([_note(START + i, "n" * 200) for i in range(1, 51)])
    with mef3io.Writer(str(big)) as w:
        w.write_annotations([_note(START + i, "n" * 200) for i in range(1, 20001)])
    assert _files(big)[0].stat().st_size > 4_000_000
    rs, ws = _append_cost(small, START + 50 * S)
    rb, wb = _append_cost(big, START + 50 * S)
    print("annotation append (bytes read, written): 50 records", (rs, ws),
          "20000 records", (rb, wb))
    assert rb < 32 * 1024 and wb < 16 * 1024, (rb, wb)  # the file is > 4 MB
    # (the stream's read-ahead buffer is the only difference: a deeper offset fills a full one)
    assert rb <= rs + 16 * 1024 and wb <= ws + 1024, ((rs, ws), (rb, wb))
    assert len(_notes(big)) == 20002


def test_encrypted_bodies_append_and_read_with_the_password(tmp_path):
    pw = ("p1", "p2")
    p = _session(tmp_path / "e.mefd", password=pw)
    with mef3io.Writer(str(p), password1="p1", password2="p2") as w:
        w.write_annotations([_note(START + S, "secret one")])
    with mef3io.Writer(str(p), password1="p1", password2="p2") as w:
        w.write_annotations([_note(START + 2 * S, "secret two")], channel="ch1")
        w.write_annotations([_note(START + 3 * S, "secret three")])
    assert [t for _, t in _notes(p, password="p2", level="session")] == ["secret one", "secret three"]
    raw = _files(p)[0].read_bytes()
    assert b"secret" not in raw
    assert mef3io.Validator(str(p), password="p2").validate().ok
    with mef3io.Writer(str(p)) as w:  # no password for an encrypted file: refused, nothing changed
        before = raw
        with pytest.raises(Exception):
            w.write_annotations([_note(START + 9 * S, "nope")])
    assert _files(p)[0].read_bytes() == before


# ---- crash states ------------------------------------------------------------

OLD = [_note(START + i * S, f"old {i}") for i in range(1, 6)]
NEW = [_note(START + (10 + i) * S, f"new {i}") for i in range(3)]


@pytest.fixture(scope="module")
def images(tmp_path_factory):
    root = tmp_path_factory.mktemp("rcrash")
    p = _session(root / "s.mefd")
    with mef3io.Writer(str(p)) as w:
        w.write_annotations(OLD)
    old = [f.read_bytes() for f in _files(p)]
    with mef3io.Writer(str(p)) as w:
        w.write_annotations(NEW)
    new = [f.read_bytes() for f in _files(p)]
    return p, old, new


def _variant(old, new, kind, uh_new):
    if kind == "old":
        out = old
    elif kind == "new":
        out = new
    elif kind == "torn":
        out = new[: len(old) + (len(new) - len(old)) // 2 + 3]
    else:  # zero: extended length, nothing landed
        out = old + bytes(len(new) - len(old))
    head = (new if uh_new else old)[:UH]
    return head + out[UH:]


STATES = [(r, i, rh, ih) for r in ("old", "new", "torn", "zero")
          for i in ("old", "new", "torn", "zero") for rh in (False, True) for ih in (False, True)]


@pytest.mark.parametrize("state", STATES, ids=lambda s: "-".join(map(str, s)))
def test_every_crash_state_recovers_without_losing_a_stored_record(tmp_path, images, state):
    src, old, new = images
    rdat_k, ridx_k, rdat_h, ridx_h = state
    p = tmp_path / "s.mefd"
    shutil.copytree(src, p)
    rdat, ridx = _files(p)
    rdat.write_bytes(_variant(old[0], new[0], rdat_k, rdat_h))
    ridx.write_bytes(_variant(old[1], new[1], ridx_k, ridx_h))

    rep = mef3io.recover_session(str(p), apply=False)  # dry run reports, writes nothing
    snapshot = (rdat.read_bytes(), ridx.read_bytes())
    mef3io.recover_session(str(p), apply=False)
    assert (rdat.read_bytes(), ridx.read_bytes()) == snapshot
    healthy = (rdat_k, ridx_k, rdat_h, ridx_h) in (("old", "old", False, False), ("new", "new", True, True))
    if healthy:
        assert not rep.record_files

    rep = mef3io.recover_session(str(p), apply=True)
    got = _notes(p)  # strict open
    texts = [t for _, t in got]
    for _, t in [(x["time"], x["text"]) for x in OLD]:
        assert t in texts, f"{t} lost in {state}"
    assert texts == sorted(texts, key=lambda t: dict((x["text"], x["time"]) for x in OLD + NEW)[t])
    assert mef3io.recover_session(str(p), apply=False).nothing_to_do or \
        not mef3io.recover_session(str(p), apply=False).record_files
    assert mef3io.Validator(str(p)).validate().ok
    # and the recorder carries on
    with mef3io.Writer(str(p)) as w:
        w.write_annotations([_note(START + 99 * S, "after")])
    assert _notes(p)[-1][1] == "after"
    assert len(_notes(p)) == len(texts) + 1


def test_writer_refuses_a_torn_file_and_names_recovery(tmp_path, images):
    src, old, new = images
    p = tmp_path / "s.mefd"
    shutil.copytree(src, p)
    rdat, ridx = _files(p)
    rdat.write_bytes(_variant(old[0], new[0], "torn", False))
    ridx.write_bytes(old[1])
    before = rdat.read_bytes()
    with mef3io.Writer(str(p)) as w:
        with pytest.raises(Exception, match="recover_session"):
            w.write_annotations([_note(START + 50 * S, "x")])
    assert rdat.read_bytes() == before  # nothing deleted, nothing added


def test_recovery_does_not_flag_healthy_files(tmp_path):
    p = _session(tmp_path / "s.mefd")
    with mef3io.Writer(str(p)) as w:
        w.write_annotations(OLD)
        w.write_annotations(NEW, channel="ch1")
        w.write_annotations([_note(START + S, "back")])
    rep = mef3io.recover_session(str(p))
    assert rep.record_files == () and rep.nothing_to_do


def test_recovery_does_not_flag_legacy_annotation_files(tmp_path):
    pytest.importorskip("mef_tools", reason="legacy oracle not installed")
    import pandas as pd
    from mef_tools.io import MefWriter

    p = tmp_path / "legacy.mefd"
    w = MefWriter(str(p), overwrite=True)
    w.write_data(np.arange(2560, dtype=np.int32), "ch1", START, FS, precision=0)
    w.write_annotations(pd.DataFrame({"time": [START + S, START + 2 * S], "type": ["Note"] * 2,
                                      "text": ["a", "b"]}))
    w.write_annotations(pd.DataFrame({"time": [START + 3 * S], "type": ["Note"], "text": ["c"]}),
                        channel="ch1")
    del w
    rep = mef3io.recover_session(str(p))
    assert rep.record_files == ()


def test_pymef_reads_appended_and_out_of_order_files(tmp_path):
    pytest.importorskip("pymef")
    from pymef.mef_session import MefSession

    p = _session(tmp_path / "s.mefd")
    with mef3io.Writer(str(p)) as w:
        w.write_annotations([_note(START + 5 * S, "b"), _note(START + 6 * S, "c")])
        w.write_annotations([_note(START + 1 * S, "a")])  # earlier than the stored ones
        w.write_annotations([_note(START + 2 * S, "x")], channel="ch1")
        w.write_annotations([_note(START + 3 * S, "y")], channel="ch1")
    s = MefSession(str(p), "")
    try:
        recs = s.read_records()
        got = sorted((r["text"] if "text" in r else r.get("note", ""), ) for r in recs if r.get("type") == "Note")
        assert len(recs) >= 3
        texts = sorted(str(r.get("text", r.get("note", ""))) for r in recs if r.get("type") == "Note")
        assert texts == ["a", "b", "c"], recs
        ch = s.read_records(channel="ch1")
        assert sorted(str(r.get("text", r.get("note", ""))) for r in ch if r.get("type") == "Note") == ["x", "y"]
    finally:
        s.close()


def test_pymef_reads_an_encrypted_appended_file(tmp_path):
    pytest.importorskip("pymef")
    from pymef.mef_session import MefSession

    p = _session(tmp_path / "e.mefd", password=("p1", "p2"))
    for i in range(2):
        with mef3io.Writer(str(p), password1="p1", password2="p2") as w:
            w.write_annotations([_note(START + (3 - i) * S, f"n{i}")])
    s = MefSession(str(p), "p2")
    try:
        texts = sorted(str(r.get("text", r.get("note", ""))) for r in s.read_records() if r.get("type") == "Note")
        assert texts == ["n0", "n1"]
    finally:
        s.close()


def test_compat_writer_appends(tmp_path):
    from mef3io import MefWriter, MefReader
    import pandas as pd

    p = tmp_path / "c.mefd"
    w = MefWriter(str(p), overwrite=True)
    w.write_data(np.arange(2560, dtype=np.int32), "ch1", START, FS, precision=0)
    w.write_annotations(pd.DataFrame({"time": [START + S], "type": ["Note"], "text": ["a"]}))
    w.write_annotations(pd.DataFrame({"time": [START + 2 * S], "type": ["Note"], "text": ["b"]}))
    w.close() if hasattr(w, "close") else None
    del w
    assert [t for _, t in _notes(p)] == ["a", "b"]


def test_transform_copy_of_an_out_of_order_file_verifies(tmp_path):
    p = _session(tmp_path / "s.mefd")
    with mef3io.Writer(str(p)) as w:
        w.write_annotations([_note(START + 5 * S, "b")])
        w.write_annotations([_note(START + 1 * S, "a")])
    out = tmp_path / "t.mefd"
    rep = mef3io.transform_session(str(p), str(out))
    assert mef3io.transform.verify(str(p), str(out)).ok if hasattr(mef3io, "transform") else True
    assert [t for _, t in _notes(out)] == ["a", "b"]


# ---- round-1 review fixes (#159) ----------------------------------------------


def _body_crc(path):
    return int.from_bytes(Path(path).read_bytes()[4:8], "little")


def test_appending_to_a_legacy_file_keeps_its_never_computed_crc(tmp_path):
    pytest.importorskip("mef_tools", reason="legacy oracle not installed")
    import pandas as pd
    from mef_tools.io import MefWriter

    p = tmp_path / "legacy.mefd"
    w = MefWriter(str(p), overwrite=True)
    w.write_data(np.arange(2560, dtype=np.int32), "ch1", START, FS, precision=0)
    w.write_annotations(pd.DataFrame({"time": [START + S], "type": ["Note"], "text": ["a"]}))
    w.write_annotations(pd.DataFrame({"time": [START + 2 * S], "type": ["Note"], "text": ["c"]}),
                        channel="ch1")
    del w
    stored = {lv: _body_crc(_files(p, lv)[0]) for lv in (None, "ch1")}
    assert 0 in stored.values()  # the oracle's "never computed"
    with mef3io.Writer(str(p)) as w:
        w.write_annotations([_note(START + 3 * S, "b")])
        w.write_annotations([_note(START + 4 * S, "d")], channel="ch1")
    for lv, before in stored.items():
        if before == 0:
            assert _body_crc(_files(p, lv)[0]) == 0
    assert [t for _, t in _notes(p)] == ["a", "b"]
    assert [t for _, t in _notes(p, "ch1")] == ["c", "d"]
    pytest.importorskip("pymef")
    from pymef.mef_session import MefSession
    s = MefSession(str(p), "")
    try:
        assert len([r for r in s.read_records() if r.get("type") == "Note"]) == 2
        assert len([r for r in s.read_records(channel="ch1") if r.get("type") == "Note"]) == 2
    finally:
        s.close()


def test_a_record_failing_its_crc_in_the_middle_is_reported_and_nothing_is_removed(tmp_path):
    p = _session(tmp_path / "s.mefd")
    five = [_note(START + i * S, f"n{i}") for i in range(1, 6)]
    with mef3io.Writer(str(p)) as w:
        for n in five:
            w.write_annotations([n])
    rdat, ridx = _files(p)
    raw = bytearray(rdat.read_bytes())
    ent = ridx.read_bytes()
    off2 = int.from_bytes(ent[UH + 24 + 8:UH + 24 + 16], "little")
    raw[off2 + 24 + 2] ^= 0xFF  # a byte in record 2's body
    rdat.write_bytes(bytes(raw))
    before = (rdat.read_bytes(), ridx.read_bytes())
    dry = mef3io.recover_session(str(p), apply=False)
    assert (rdat.read_bytes(), ridx.read_bytes()) == before
    assert any("fail their CRC" in n for n in dry.notes) or any(
        f.records_damaged for f in dry.record_files)
    rep = mef3io.recover_session(str(p), apply=True)
    assert len(rdat.read_bytes()) == len(before[0])  # nothing removed
    ent_after = ridx.read_bytes()
    assert len(ent_after) == len(before[1])
    # records 1,3,4,5 are byte for byte what they were
    for i in (0, 2, 3, 4):
        o = int.from_bytes(ent[UH + 24 * i + 8:UH + 24 * i + 16], "little")
        assert rdat.read_bytes()[o:o + 40] == before[0][o:o + 40]
    assert not mef3io.recover_session(str(p), apply=False).record_files
    assert not rep.skipped
    # strict raises; lenient reads the four intact records and lists the one
    with pytest.raises(Exception, match="CRC"):
        mef3io.Reader(str(p)).records()
    with mef3io.Reader(str(p), strict=False) as r:
        with pytest.warns(mef3io.UnreadableSegmentWarning):
            got = [x["text"] for x in r.records()]
        probs = [q for q in r.problems if q["kind"] == "records"]
    assert got == ["n1", "n3", "n4", "n5"]
    assert len(probs) == 1 and "fails its CRC" in probs[0]["reason"]
    assert f"byte offset {off2}" in probs[0]["reason"]


def _five_notes(tmp_path):
    p = _session(tmp_path / "s.mefd")
    with mef3io.Writer(str(p)) as w:
        for i in range(1, 6):
            w.write_annotations([_note(START + i * S, f"n{i}")])
    rdat, ridx = _files(p)
    ent = ridx.read_bytes()
    offs = [int.from_bytes(ent[UH + 24 * i + 8:UH + 24 * i + 16], "little") for i in range(5)]
    return p, rdat, ridx, offs


def test_a_corrupt_length_field_is_damage_not_a_torn_tail(tmp_path):
    p, rdat, ridx, offs = _five_notes(tmp_path)
    raw = bytearray(rdat.read_bytes())
    raw[offs[1] + 13] ^= 0x40  # a bit in record 2's length field
    rdat.write_bytes(bytes(raw))
    before = (rdat.read_bytes(), ridx.read_bytes())
    for apply in (False, True):
        rep = mef3io.recover_session(str(p), apply=apply)
        assert (rdat.read_bytes(), ridx.read_bytes()) == before
        assert not rep.record_files and not rep.skipped
        txt = " ".join(rep.notes)
        assert "damaged, not modified" in txt and "at or beyond the damage" in txt
        assert "listed by neither" not in txt
    # the lenient reader still gets every record the index can locate
    with mef3io.Reader(str(p), strict=False) as r:
        with pytest.warns(mef3io.UnreadableSegmentWarning):
            got = [x["text"] for x in r.records()]
        probs = [q for q in r.problems if q["kind"] == "records"]
    assert got == ["n1", "n3", "n4", "n5"]
    assert len(probs) == 1 and f"byte offset {offs[1]}" in probs[0]["reason"]
    with pytest.raises(Exception):
        mef3io.Reader(str(p)).records()


def test_lenient_without_an_index_reads_up_to_a_crc_damage_and_beyond(tmp_path):
    p, rdat, ridx, offs = _five_notes(tmp_path)
    raw = bytearray(rdat.read_bytes())
    raw[offs[2] + 24 + 1] ^= 0xFF  # body of record 3
    rdat.write_bytes(bytes(raw))
    ridx.unlink()
    with mef3io.Reader(str(p), strict=False) as r:
        with pytest.warns(mef3io.UnreadableSegmentWarning):
            got = [x["text"] for x in r.records()]
    assert got == ["n1", "n2", "n4", "n5"]  # the record's own length is intact: walk goes on


def test_a_torn_tail_after_a_damaged_record_drops_only_the_tail(tmp_path):
    p = _session(tmp_path / "s.mefd")
    with mef3io.Writer(str(p)) as w:
        for i in range(1, 4):
            w.write_annotations([_note(START + i * S, f"n{i}")])
    rdat, ridx = _files(p)
    raw = bytearray(rdat.read_bytes())
    ent = ridx.read_bytes()
    off2 = int.from_bytes(ent[UH + 24 + 8:UH + 24 + 16], "little")
    raw[off2 + 24 + 2] ^= 0xFF
    size = len(raw)
    rdat.write_bytes(bytes(raw) + b"\x00" * 7)  # a fragment after the last whole record
    mef3io.recover_session(str(p), apply=True)
    assert len(rdat.read_bytes()) == size  # only the fragment went
    assert len(ridx.read_bytes()) == len(ent)


def test_last_record_with_a_flipped_length_is_damage_even_with_an_index(tmp_path):
    for bit in (0x01, 0x20, 0x80):
        p, rdat, ridx, offs = _five_notes(tmp_path / str(bit))
        raw = bytearray(rdat.read_bytes())
        raw[offs[4] + 12] ^= bit  # record 5's length (the index lists it AT the stop)
        rdat.write_bytes(bytes(raw))
        before = (rdat.read_bytes(), ridx.read_bytes())
        for apply in (False, True):
            rep = mef3io.recover_session(str(p), apply=apply)
            assert (rdat.read_bytes(), ridx.read_bytes()) == before
            txt = " ".join(rep.notes)
            assert "damaged, not modified" in txt and "listed by neither" not in txt


def test_without_an_index_nothing_is_dropped_and_intact_records_still_read(tmp_path):
    for kind in ("length", "zeros"):
        p, rdat, ridx, offs = _five_notes(tmp_path / kind)
        raw = bytearray(rdat.read_bytes())
        if kind == "length":
            raw[offs[1] + 13] ^= 0x40
        else:
            raw[offs[1]:offs[2]] = b"\x00" * (offs[2] - offs[1])
        rdat.write_bytes(bytes(raw))
        ridx.unlink()
        for apply in (False, True):
            mef3io.recover_session(str(p), apply=apply)
            assert rdat.read_bytes() == bytes(raw)
            assert not ridx.exists()
        with mef3io.Reader(str(p), strict=False) as r:
            with pytest.warns(mef3io.UnreadableSegmentWarning):
                got = [x["text"] for x in r.records()]
        assert "n1" in got and got[0] == "n1"


def test_an_intact_record_after_the_damage_blocks_a_drop_even_without_entries(tmp_path):
    p, rdat, ridx, offs = _five_notes(tmp_path)
    raw = bytearray(rdat.read_bytes())
    raw[offs[1] + 13] ^= 0x40
    rdat.write_bytes(bytes(raw))
    # an index that stops before the damage: only the scan can see records 3-5
    ent = ridx.read_bytes()
    ridx.write_bytes(ent[:UH + 24])
    before = (rdat.read_bytes(), ridx.read_bytes())
    rep = mef3io.recover_session(str(p), apply=True)
    assert (rdat.read_bytes(), ridx.read_bytes()) == before
    assert "intact record follows" in " ".join(rep.notes)


def test_a_non_utf8_type_byte_never_breaks_a_lenient_read(tmp_path):
    p, rdat, ridx, offs = _five_notes(tmp_path)
    raw = bytearray(rdat.read_bytes())
    raw[offs[1] + 4] = 0xD8  # type byte that is not UTF-8
    rdat.write_bytes(bytes(raw))
    with mef3io.Reader(str(p), strict=False) as r:
        with pytest.warns(mef3io.UnreadableSegmentWarning):
            got = [x["text"] for x in r.records()]
        probs = r.problems
    assert got == ["n1", "n3", "n4", "n5"]
    assert all(isinstance(q["reason"], str) for q in probs)
    assert any("\\xd8" in q["reason"] for q in probs)
    with pytest.raises(Exception) as ei:
        mef3io.Reader(str(p)).records()
    str(ei.value)


def test_session_append_targets_the_file_the_reader_uses(tmp_path):
    p = _session(tmp_path / "orig.mefd")
    with mef3io.Writer(str(p)) as w:
        w.write_annotations([_note(START + S, "old")])
    q = tmp_path / "renamed.mefd"
    p.rename(q)
    with mef3io.Writer(str(q)) as w:
        w.write_annotations([_note(START + 2 * S, "new")])
    assert sorted(f.name for f in q.glob("*.rdat")) == ["orig.rdat"]
    assert [t for _, t in _notes(q)] == ["old", "new"]


def test_ambiguous_session_record_files_refuse_a_write(tmp_path):
    p = _session(tmp_path / "s.mefd")
    with mef3io.Writer(str(p)) as w:
        w.write_annotations([_note(START + S, "old")])
    rdat, ridx = _files(p)
    shutil.copy(rdat, p / "a.rdat")
    shutil.copy(ridx, p / "a.ridx")
    rdat.rename(p / "b.rdat")
    ridx.rename(p / "b.ridx")
    with pytest.raises(Exception, match="ambiguous"):
        with mef3io.Writer(str(p)) as w:
            w.write_annotations([_note(START + 2 * S, "new")])
    assert sorted(f.name for f in p.glob("*.rdat")) == ["a.rdat", "b.rdat"]


def test_an_orphan_index_is_never_replaced_and_recovery_reports_it(tmp_path):
    p = _session(tmp_path / "s.mefd")
    with mef3io.Writer(str(p)) as w:
        w.write_annotations([_note(START + S, "old")])
    rdat, ridx = _files(p)
    keep = ridx.read_bytes()
    rdat.unlink()
    with pytest.raises(Exception, match="recover_session"):
        with mef3io.Writer(str(p)) as w:
            w.write_annotations([_note(START + 2 * S, "new")])
    assert ridx.read_bytes() == keep and not rdat.exists()
    rep = mef3io.recover_session(str(p), apply=True)
    assert any("without its record file" in n for n in rep.notes)
    assert ridx.read_bytes() == keep


def test_unset_maximum_entry_size_is_measured_not_guessed(tmp_path):
    p = _session(tmp_path / "s.mefd")
    long_text = "x" * 500
    with mef3io.Writer(str(p)) as w:
        w.write_annotations([_note(START + S, long_text)])
    rdat, _ = _files(p)
    raw = bytearray(rdat.read_bytes())
    big = len(raw) - UH
    raw[40:48] = (-1).to_bytes(8, "little", signed=True)
    raw[0:4] = _koopman(bytes(raw[4:UH])).to_bytes(4, "little")
    rdat.write_bytes(bytes(raw))
    with mef3io.Writer(str(p)) as w:
        w.write_annotations([_note(START + 2 * S, "s")])
    assert int.from_bytes(rdat.read_bytes()[40:48], "little", signed=True) >= big
    assert [t for _, t in _notes(p)] == [long_text, "s"]


def test_multireader_lenient_reads_the_intact_records_too(tmp_path):
    p, rdat, ridx, offs = _five_notes(tmp_path)
    raw = bytearray(rdat.read_bytes())
    raw[offs[1] + 24 + 2] ^= 0xFF
    rdat.write_bytes(bytes(raw))
    with pytest.raises(Exception):
        mef3io.MultiReader([str(p)]).records()
    with mef3io.MultiReader([str(p)], strict=False) as mr:
        with pytest.warns(mef3io.UnreadableSegmentWarning):
            got = [x["text"] for x in mr.records()]
    assert got == ["n1", "n3", "n4", "n5"]
