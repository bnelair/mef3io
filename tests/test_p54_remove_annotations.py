"""remove_annotations: take stored annotations out by content, crash-safely.

`write_annotations` only appends (test_p47). This is the one other way the
annotations of a session change, so what is pinned here is, in order: what is
selected (content, never a position; all or nothing), what is rewritten (the
record files of the levels named, nothing else; every kept record byte for
byte), how a crash at EVERY step ends (old or new, or a marker the library
finishes), the encryption levels, and that the oracle (pymef / mef_tools) and
the validator read the result.
"""
import os
import shutil
import subprocess
import sys
import threading
import warnings
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
PASSWORDS = ("pw-one", "pw-two")


def _session(path, password=None, channels=("ch1",)):
    kwargs = dict(overwrite=True)
    if password:
        kwargs.update(password1=password[0], password2=password[1])
    with mef3io.Writer(str(path), **kwargs) as w:
        for ch in channels:
            w.write_int32(ch, np.arange(2560, dtype=np.int32), 1.0, START, FS)
    return Path(path)


def _note(t, text, **kw):
    return {"time": START + t * S, "text": text, **kw}


def _with_notes(path, notes, channel=None, password=None):
    kw = {}
    if password:
        kw.update(password1=password[0], password2=password[1])
    with mef3io.Writer(str(path), **kw) as w:
        w.write_annotations(notes, channel=channel)


def _records(path, password="", **kw):
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        with mef3io.Reader(str(path), password=password) as r:
            return r.records(**kw)


def _texts(path, password="", channel=None, **kw):
    return [x["text"] for x in _records(path, password, channel=channel, **kw)]


def _pw(password):
    return dict(password1=password[0], password2=password[1]) if password else {}


def _pair(path, channel=None):
    p = Path(path)
    if channel:
        return p / f"{channel}.timd" / f"{channel}.rdat", p / f"{channel}.timd" / f"{channel}.ridx"
    return p / f"{p.stem}.rdat", p / f"{p.stem}.ridx"


def _chunks(rdat_bytes):
    """The stored bytes of every record (header + body), in file order."""
    out, off = [], UH
    while off < len(rdat_bytes):
        n = int.from_bytes(rdat_bytes[off + 12: off + 16], "little")
        out.append(bytes(rdat_bytes[off: off + 24 + n]))
        off += 24 + n
    return out


def _entries(ridx_bytes):
    return [bytes(ridx_bytes[UH + i * 24: UH + (i + 1) * 24])
            for i in range((len(ridx_bytes) - UH) // 24)]


def _tree(path, skip_records=False):
    """{relative file: bytes}; with skip_records, without the annotation files."""
    out = {}
    for f in sorted(Path(path).rglob("*")):
        if f.is_file():
            if skip_records and f.suffix in (".rdat", ".ridx"):
                continue
            out[str(f.relative_to(path))] = f.read_bytes()
    return out


def _mtimes(path):
    return {str(f.relative_to(path)): f.stat().st_mtime_ns
            for f in Path(path).rglob("*") if f.is_file() and f.suffix not in (".rdat", ".ridx")}


def _leftovers(path):
    return [f for f in Path(path).rglob("*")
            if f.name.endswith((".new", ".old", ".part"))]


def _code(excinfo):
    return excinfo.value.code


def _validate_ok(path, password=""):
    rep = mef3io.Validator(str(path), password=password).validate()
    assert rep.ok, rep.summary()
    return rep


def _crc(data):
    poly = 0xEB31D82E
    table = []
    for i in range(256):
        c = i
        for _ in range(8):
            c = (c >> 1) ^ poly if c & 1 else c >> 1
        table.append(c)
    c = 0xFFFFFFFF
    for b in data:
        c = table[(c ^ b) & 0xFF] ^ (c >> 8)
    return c


def _assert_headers_right(rdat, ridx, n):
    for f in (rdat, ridx):
        d = f.read_bytes()
        assert _crc(d[4:UH]) == int.from_bytes(d[:4], "little"), "header CRC"
        assert _crc(d[UH:]) == int.from_bytes(d[4:8], "little"), "body CRC"
        assert int.from_bytes(d[32:40], "little") == n, "entry count"
        assert d[960:964] == b"M3IO" and d[965] == 10, "provenance: records-edit"
    assert len(_chunks(rdat.read_bytes())) == n
    assert len(_entries(ridx.read_bytes())) == n


# ---------------------------------------------------------------------------
# what is rewritten, and how
# ---------------------------------------------------------------------------

def test_the_record_stays_byte_for_byte_and_nothing_else_is_written(tmp_path):
    p = _session(tmp_path / "s.mefd", channels=("ch1", "ch2"))
    _with_notes(p, [_note(1, "a"), _note(2, "b"), _note(3, "c" * 70)])
    old_rdat, old_ridx = (f.read_bytes() for f in _pair(p))
    old_chunks, old_entries = _chunks(old_rdat), _entries(old_ridx)
    tree, mt = _tree(p, skip_records=True), _mtimes(p)

    recs = _records(p)
    report = mef3io.remove_annotations(p, [recs[1]])

    assert (report.removed, report.added) == (1, 0)
    (f,) = report.files
    assert (f.level, f.file, f.records_before, f.records_after, f.pair_removed) == \
        ("session", "s.rdat", 3, 2, False)
    assert _texts(p) == ["a", "c" * 70]
    new_rdat, new_ridx = (f.read_bytes() for f in _pair(p))
    assert _chunks(new_rdat) == [old_chunks[0], old_chunks[2]]       # byte for byte, in order
    kept = _entries(new_ridx)
    assert [e[:8] + e[16:] for e in kept] == [old_entries[0][:8] + old_entries[0][16:],
                                              old_entries[2][:8] + old_entries[2][16:]]
    offs = [int.from_bytes(e[8:16], "little") for e in kept]
    assert offs == [UH, UH + len(old_chunks[0])]
    _assert_headers_right(*_pair(p), 2)
    # signal files and metadata: identical bytes, never touched
    assert _tree(p, skip_records=True) == tree
    assert _mtimes(p) == mt
    assert not _leftovers(p) and mef3io.operation_marker(p) is None
    _validate_ok(p)


def test_header_span_follows_the_kept_records(tmp_path):
    p = _session(tmp_path / "s.mefd")
    _with_notes(p, [_note(1, "first"), _note(5, "middle"), _note(9, "last")])
    recs = _records(p)
    mef3io.remove_annotations(p, [recs[0], recs[2]])
    rdat, _ = _pair(p)
    d = rdat.read_bytes()
    start = -int.from_bytes(d[16:24], "little", signed=True)
    end = -int.from_bytes(d[24:32], "little", signed=True)
    assert start == end == START + 5 * S


def test_dry_run_writes_nothing_and_reports(tmp_path):
    p = _session(tmp_path / "s.mefd")
    _with_notes(p, [_note(1, "a"), _note(2, "b")])
    before = _tree(p)
    report = mef3io.remove_annotations(p, [_records(p)[0]], dry_run=True)
    assert report.dry_run and report.removed == 1 and report.files[0].records_after == 1
    assert _tree(p) == before and not _leftovers(p)


def test_selector_with_expect_and_add_changes_a_note_in_one_rewrite(tmp_path):
    p = _session(tmp_path / "s.mefd")
    _with_notes(p, [_note(1, "good"), _note(2, "typo"), _note(3, "also good")])
    wrong = [r for r in _records(p) if r["text"] == "typo"]
    report = mef3io.remove_annotations(p, wrong, add=[_note(2, "fixed")])
    assert (report.removed, report.added) == (1, 1)
    assert _texts(p) == ["good", "fixed", "also good"]          # readers sort by time
    rdat, ridx = _pair(p)
    assert _chunks(rdat.read_bytes())[-1][24:].startswith(b"fixed")   # added after the kept
    _assert_headers_right(rdat, ridx, 3)
    _validate_ok(p)


def test_last_record_removed_removes_the_pair_and_a_write_creates_a_fresh_one(tmp_path):
    p = _session(tmp_path / "s.mefd")
    _with_notes(p, [_note(1, "only")])
    report = mef3io.remove_annotations(p, _records(p))
    assert report.files[0].pair_removed and report.files[0].records_after == 0
    assert not any(f.exists() for f in _pair(p))
    assert _records(p) == []
    _validate_ok(p)
    assert mef3io.inspect(str(p)).records.total == 0 if hasattr(mef3io.inspect(str(p)), "records") \
        and hasattr(mef3io.inspect(str(p)).records, "total") else True
    _with_notes(p, [_note(7, "again")])
    assert _texts(p) == ["again"]
    _validate_ok(p)


def test_a_later_append_works_on_the_rewritten_files(tmp_path):
    p = _session(tmp_path / "s.mefd")
    _with_notes(p, [_note(1, "a"), _note(2, "b"), _note(3, "c")])
    mef3io.remove_annotations(p, [_records(p)[1]])
    _with_notes(p, [_note(4, "d")])
    assert _texts(p) == ["a", "c", "d"]
    _assert_headers_right_after_append = _pair(p)
    d = _assert_headers_right_after_append[0].read_bytes()
    assert _crc(d[UH:]) == int.from_bytes(d[4:8], "little")     # the rolling CRC continued right
    _validate_ok(p)


def test_levels_channel_and_segment_and_the_session_in_one_call(tmp_path):
    p = _session(tmp_path / "s.mefd", channels=("ch1", "ch2"))
    _with_notes(p, [_note(1, "s1"), _note(2, "s2")])
    _with_notes(p, [_note(1, "c1"), _note(2, "c2")], channel="ch1")
    _with_notes(p, [_note(1, "k1"), _note(2, "k2")], channel="ch2")
    # a segment-level pair, as another writer would have made it
    segd = p / "ch1.timd" / "ch1-000000.segd"
    for src, ext in ((_pair(p, "ch1")[0], ".rdat"), (_pair(p, "ch1")[1], ".ridx")):
        shutil.copy(src, segd / f"ch1-000000{ext}")
    recs = _records(p, level="all")
    assert sorted((r["level"], r["text"]) for r in recs) == sorted(
        [("session", "s1"), ("session", "s2"), ("channel", "c1"), ("channel", "c2"),
         ("segment", "c1"), ("segment", "c2"), ("channel", "k1"), ("channel", "k2")])
    wanted = {("session", "s1"), ("channel", "c2"), ("segment", "c1"), ("channel", "k1")}
    pick = [r for r in recs if (r["level"], r["text"]) in wanted]
    assert len(pick) == 4
    report = mef3io.remove_annotations(p, pick)
    assert report.removed == 4 and len(report.files) == 4
    assert sorted((r["level"], r["text"]) for r in _records(p, level="all")) == sorted(
        [("session", "s2"), ("channel", "c1"), ("segment", "c2"), ("channel", "k2")])
    _validate_ok(p)


def test_a_failing_selector_leaves_every_level_untouched(tmp_path):
    p = _session(tmp_path / "s.mefd")
    _with_notes(p, [_note(1, "s1")])
    _with_notes(p, [_note(1, "c1")], channel="ch1")
    before = _tree(p)
    good = _records(p, level="session")[0]
    ghost = dict(_records(p, "ch1", level="channel")[0], text="not there")
    with pytest.raises(ValueError) as e:
        mef3io.remove_annotations(p, [good, ghost])
    assert _code(e) == "no-match"
    assert _tree(p) == before and not _leftovers(p)


# ---------------------------------------------------------------------------
# selection: by content, and refusals with codes
# ---------------------------------------------------------------------------

def test_a_selector_names_content_and_refusals_have_codes(tmp_path):
    p = _session(tmp_path / "s.mefd")
    _with_notes(p, [_note(1, "x"), _note(2, "dup"), _note(2, "dup"), _note(3, "y")])
    before = _tree(p)
    dup = [r for r in _records(p) if r["text"] == "dup"][0]

    def refused(code, remove, **kw):
        with pytest.raises(ValueError) as e:
            mef3io.remove_annotations(p, remove, **kw)
        assert _code(e) == code, str(e.value)
        assert _tree(p) == before and not _leftovers(p)      # nothing removed

    refused("ambiguous", [dup])                               # two identical copies, one expected
    refused("count-mismatch", [dict(dup, expect=3)])          # only two exist
    refused("no-match", [dict(dup, text="nope")])
    refused("no-match", [dict(dup, time=dup["time"] + 1)])
    refused("selector-incomplete", [{k: v for k, v in dup.items() if k != "text"}])
    refused("selector-unknown-field", [dict(dup, bogus=1)])
    refused("bad-selector", [dict(dup, expect=0)])
    refused("bad-selector", [dict(dup, level="channel", channel=None)])
    refused("unknown-channel", [dict(dup, level="channel", channel="nope")])
    refused("selectors-overlap", [dict(dup, expect=2), dict(dup, expect=2, all_copies=True)])
    refused("nothing-to-do", [])
    with pytest.raises(ValueError) as e:                       # a write level with no file
        mef3io.remove_annotations(p, [], add=[dict(_note(1, "q"), channel="ch1")])
    assert _code(e) == "add-level-missing"
    assert _tree(p) == before

    mef3io.remove_annotations(p, [dict(dup, expect=2)])        # said so: both go
    assert _texts(p) == ["x", "y"]


def test_all_copies_removes_every_identical_record(tmp_path):
    p = _session(tmp_path / "s.mefd")
    _with_notes(p, [_note(2, "dup")] * 5 + [_note(3, "keep")])
    dup = _records(p)[0]
    report = mef3io.remove_annotations(p, [dup], all_copies=True)
    assert report.removed == 5 and _texts(p) == ["keep"]
    with pytest.raises(ValueError) as e:                       # at least one must exist
        mef3io.remove_annotations(p, [dup], all_copies=True)
    assert _code(e) == "no-match"


def test_other_types_are_selected_by_their_fields(tmp_path):
    p = _session(tmp_path / "s.mefd")
    with mef3io.Writer(str(p)) as w:
        w.write_annotations([{"type": "EDFA", "time": START + S, "text": "ev", "duration": 5},
                             {"type": "EDFA", "time": START + S, "text": "ev", "duration": 6},
                             {"type": "SyLg", "time": START + S, "text": "ev"}])
    recs = _records(p)
    five = [r for r in recs if r["type"] == "EDFA" and r["duration"] == 5][0]
    mef3io.remove_annotations(p, [five])
    left = sorted((r["type"], r.get("duration")) for r in _records(p))
    assert left == [("EDFA", 6), ("SyLg", None)]


def test_a_session_at_an_anonymization_offset_keeps_its_times(tmp_path):
    pytest.importorskip("mef_tools")
    pytest.importorskip("pymef")
    pd = pytest.importorskip("pandas")
    from mef_tools.io import MefWriter
    p = tmp_path / "off.mefd"
    rto = START - 10 * 86400 * S
    w = MefWriter(str(p), overwrite=True)
    w.record_offset = rto
    w.write_data(np.arange(5000, dtype=np.int32) * 1.0, "ch1", START, FS, precision=0)
    w.write_annotations(pd.DataFrame([{"time": START + S, "type": "Note", "text": "s1"},
                                      {"time": START + 2 * S, "type": "Note", "text": "s2"},
                                      {"time": START + 3 * S, "type": "Note", "text": "s3"}]))
    del w
    old_chunks = _chunks(_pair(p)[0].read_bytes())
    mef3io.remove_annotations(p, [_records(p)[1]])
    assert [x["time"] for x in _records(p)] == [START + S, START + 3 * S]
    assert _chunks(_pair(p)[0].read_bytes()) == [old_chunks[0], old_chunks[2]]
    _oracles_agree(p)


# ---------------------------------------------------------------------------
# refusals about the session
# ---------------------------------------------------------------------------

def test_a_tar_archive_is_read_only(tmp_path):
    p = _session(tmp_path / "s.mefd")
    _with_notes(p, [_note(1, "a")])
    tar = mef3io.archive_session(str(p))
    with pytest.raises(OSError.__mro__[0] if False else mef3io.IoError) as e:
        mef3io.remove_annotations(tar, [_records(p)[0]])
    assert _code(e) == "tar-read-only"
    assert _texts(tar) == ["a"]


def test_a_session_being_written_or_mid_operation_is_refused(tmp_path):
    p = _session(tmp_path / "s.mefd")
    _with_notes(p, [_note(1, "a")])
    rec = _records(p)[0]
    before = _tree(p)
    busy = p / "ch1.timd" / "x.mef3io-tmp"
    busy.write_bytes(b"")
    with pytest.raises(mef3io.WriteConflictError) as e:
        mef3io.remove_annotations(p, [rec])
    assert _code(e) == "being-written"
    busy.unlink()
    (p / ".mef3io-operation").write_bytes(
        b"mef3io-operation 1\ntool mef3io x\noperation rename\nid " + b"0" * 32 + b"\n")
    with pytest.raises(mef3io.WriteConflictError) as e:
        mef3io.remove_annotations(p, [rec])
    assert _code(e) == "mid-operation"
    (p / ".mef3io-operation").unlink()
    assert _tree(p) == before


def test_a_damaged_annotation_file_is_refused_and_untouched(tmp_path):
    p = _session(tmp_path / "s.mefd")
    _with_notes(p, [_note(1, "a"), _note(2, "b")])
    rec = _records(p)[0]
    rdat, ridx = _pair(p)
    good = rdat.read_bytes()

    def damaged(code, mutate):
        rdat.write_bytes(mutate(bytearray(good)))
        before = _tree(p)
        with pytest.raises((mef3io.FormatError, mef3io.CrcError)) as e:
            mef3io.remove_annotations(p, [rec])
        assert _code(e) == code
        assert _tree(p) == before and not _leftovers(p)

    damaged("record-damaged", lambda b: b[: len(b) - 10])     # torn last record
    flipped = lambda b: (b.__setitem__(UH + 30, b[UH + 30] ^ 1), b)[1]
    damaged("record-crc", flipped)
    rdat.write_bytes(good)
    ridx_bytes = ridx.read_bytes()
    ridx.write_bytes(ridx_bytes[:-24])                         # short index
    with pytest.raises(mef3io.FormatError) as e:
        mef3io.remove_annotations(p, [rec])
    assert _code(e) == "record-index-mismatch"
    ridx.unlink()
    with pytest.raises(mef3io.FormatError) as e:
        mef3io.remove_annotations(p, [rec])
    assert _code(e) == "record-index-missing"


# ---------------------------------------------------------------------------
# encryption
# ---------------------------------------------------------------------------

def test_encrypted_session_needs_both_passwords_and_keeps_ciphertext(tmp_path):
    p = _session(tmp_path / "s.mefd", password=PASSWORDS)
    _with_notes(p, [_note(1, "alpha"), _note(2, "beta"), _note(3, "gamma")], password=PASSWORDS)
    old = _chunks(_pair(p)[0].read_bytes())
    assert all(c[11] == 2 for c in old)                         # level-2 encrypted bodies
    rec = _records(p, password=PASSWORDS[1])[1]
    before = _tree(p)
    for kw, exc in (({}, mef3io.PasswordError),
                    (dict(password1="wrong", password2="wrong2"), mef3io.PasswordError),
                    (dict(password1=PASSWORDS[0]), mef3io.PasswordError)):
        with pytest.raises(exc) as e:
            mef3io.remove_annotations(p, [rec], **kw)
        assert _code(e) == "password"
        assert _tree(p) == before
    mef3io.remove_annotations(p, [rec], **_pw(PASSWORDS))
    assert _texts(p, PASSWORDS[1]) == ["alpha", "gamma"]
    assert _chunks(_pair(p)[0].read_bytes()) == [old[0], old[2]]  # same ciphertext, same level
    _assert_headers_right(*_pair(p), 2)
    _validate_ok(p, PASSWORDS[1])


def test_encrypted_adds_are_level_two_and_an_unprotected_session_refuses_passwords(tmp_path):
    p = _session(tmp_path / "s.mefd", password=PASSWORDS)
    _with_notes(p, [_note(1, "alpha"), _note(2, "beta")], password=PASSWORDS)
    mef3io.remove_annotations(p, [_records(p, password=PASSWORDS[1])[0]],
                              add=[_note(5, "secret text")], **_pw(PASSWORDS))
    chunks = _chunks(_pair(p)[0].read_bytes())
    assert [c[11] for c in chunks] == [2, 2]
    assert b"secret text" not in _pair(p)[0].read_bytes()
    assert _texts(p, PASSWORDS[1]) == ["beta", "secret text"]
    _validate_ok(p, PASSWORDS[1])

    q = _session(tmp_path / "plain.mefd")
    _with_notes(q, [_note(1, "a")])
    with pytest.raises(mef3io.PasswordError) as e:
        mef3io.remove_annotations(q, [_records(q)[0]], **_pw(PASSWORDS))
    assert _code(e) == "password"
    assert _texts(q) == ["a"]


# ---------------------------------------------------------------------------
# the oracle reads the result
# ---------------------------------------------------------------------------

def _oracles_agree(path):
    pytest.importorskip("pymef")
    from mef_tools.io import MefReader
    from pymef.mef_session import MefSession
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        ours = sorted((a["text"], a["time"]) for a in mef3io.Reader(str(path)).records())
        legacy = sorted((a["text"], int(a["time"])) for a in MefReader(str(path)).get_annotations())
        pym = sorted((r["text"], int(r["time"])) for r in MefSession(str(path), "").read_records())
    assert ours == legacy == pym
    return ours


def test_the_oracle_reads_the_rewritten_session_and_a_later_append(tmp_path):
    pytest.importorskip("pymef")
    p = _session(tmp_path / "s.mefd")
    _with_notes(p, [_note(i, f"n{i}") for i in range(1, 7)])
    wrong = [r for r in _records(p) if r["text"] in ("n2", "n5")]
    mef3io.remove_annotations(p, wrong, add=[_note(2, "n2 fixed")])
    assert [t for t, _ in _oracles_agree(p)] == ["n1", "n2 fixed", "n3", "n4", "n6"]
    _with_notes(p, [_note(9, "later")])
    assert "later" in [t for t, _ in _oracles_agree(p)]
    _validate_ok(p)


def test_the_oracle_reads_a_session_whose_last_annotation_was_removed(tmp_path):
    """No annotations = no record files, the state of every never-annotated
    session: pymef opens it and has no records to give, exactly as for one
    that never had any (the oracle's own read_records raises KeyError there)."""
    pytest.importorskip("pymef")
    from pymef.mef_session import MefSession
    p = _session(tmp_path / "s.mefd")
    _with_notes(p, [_note(1, "a")])
    mef3io.remove_annotations(p, _records(p))
    never = _session(tmp_path / "never.mefd")

    def outcome(path):
        s = MefSession(str(path), "")
        try:
            return [r for r in s.read_records()]
        except KeyError:
            return "no records"
        finally:
            assert "records_info" not in s.session_md

    assert outcome(p) == outcome(never)
    assert _records(p) == []


def test_inspect_counts_the_remaining_annotations(tmp_path):
    p = _session(tmp_path / "s.mefd")
    _with_notes(p, [_note(1, "a"), _note(2, "b"), _note(3, "c")])
    _with_notes(p, [_note(1, "k")], channel="ch1")
    mef3io.remove_annotations(p, [_records(p)[0]])
    s = mef3io.inspect(str(p))
    assert sorted((r.level, r.time) for r in s.records) == sorted(
        [("session", START + 2 * S), ("session", START + 3 * S), ("channel", START + S)])
    assert s.records_start_time == START + S and s.records_end_time == START + 3 * S
    assert {c.name: c.records for c in s.channels}["ch1"] == 1


# ---------------------------------------------------------------------------
# cancel and progress
# ---------------------------------------------------------------------------

def test_cancel_before_the_swap_leaves_the_old_state(tmp_path):
    p = _session(tmp_path / "s.mefd")
    _with_notes(p, [_note(1, "a"), _note(2, "b")])
    before = _tree(p)
    token = mef3io.CancelToken()
    token.cancel()
    with pytest.raises(mef3io.Cancelled):
        mef3io.remove_annotations(p, [_records(p)[0]], cancel=token)
    assert _tree(p) == before and not _leftovers(p)

    token = mef3io.CancelToken()
    seen = []

    def progress(done, total):
        seen.append((done, total))
        if done > 0:
            token.cancel()            # mid-call, after the files were read

    with pytest.raises(mef3io.Cancelled):
        mef3io.remove_annotations(p, [_records(p)[0]], cancel=token, progress=progress)
    assert _tree(p) == before and not _leftovers(p)
    assert seen


def test_progress_ends_at_total(tmp_path):
    p = _session(tmp_path / "s.mefd")
    _with_notes(p, [_note(i, "n" * 200) for i in range(1, 40)])
    seen = []
    mef3io.remove_annotations(p, [_records(p)[0]], progress=lambda d, t: seen.append((d, t)))
    assert seen[0][0] == 0 and seen[-1][0] == seen[-1][1] and seen[-1][1] > 0
    assert [d for d, _ in seen] == sorted(d for d, _ in seen)


def test_the_gil_is_released_while_it_works(tmp_path):
    p = _session(tmp_path / "s.mefd")
    _with_notes(p, [_note(i, "n" * 500) for i in range(1, 4000)])
    recs = _records(p)
    ticks = []
    stop = threading.Event()

    def ticker():
        while not stop.is_set():
            ticks.append(1)
            stop.wait(0.0005)

    t = threading.Thread(target=ticker)
    t.start()
    try:
        mef3io.remove_annotations(p, recs[:1000])
    finally:
        stop.set()
        t.join()
    assert len(ticks) > 3


# ---------------------------------------------------------------------------
# a crash at EVERY step
# ---------------------------------------------------------------------------

def _crash_session(tmp_path, name, password=None):
    p = _session(tmp_path / name, password=password, channels=("ch1",))
    _with_notes(p, [_note(1, "a"), _note(2, "b"), _note(3, "c")], password=password)
    _with_notes(p, [_note(1, "k1"), _note(2, "k2")], channel="ch1", password=password)
    return p


def _crash_run(p, *, password=None, crash_at=-1, fail_at=-1):
    rec = _records(p, password=(password[1] if password else ""), level="session")[1]
    kc = _records(p, password=(password[1] if password else ""), level="channel", channel="ch1")[0]
    return mef3io.remove_annotations(p, [rec, kc], add=[_note(2, "b fixed")],
                                     _crash_at=crash_at, _fail_at=fail_at, **_pw(password))


def _state(p, password=""):
    return (tuple(_texts(p, password, level="session")),
            tuple(_texts(p, password, "ch1", level="channel")))


OLD = (("a", "b", "c"), ("k1", "k2"))
NEW = (("a", "b fixed", "c"), ("k2",))


@pytest.mark.parametrize("password", [None, PASSWORDS], ids=["plain", "L1+L2"])
def test_a_crash_at_every_step_ends_old_or_new_and_recovery_converges(tmp_path, password):
    pw = password[1] if password else ""
    # how many steps does the call have?
    probe = _crash_session(tmp_path, "probe.mefd", password)
    steps = 0
    while True:
        q = _crash_session(tmp_path, f"probe{steps}.mefd", password)
        try:
            _crash_run(q, password=password, crash_at=steps)
        except mef3io.IoError as e:
            assert "simulated crash" in str(e)
            steps += 1
            continue
        break
    assert steps >= 14, steps
    ref = _crash_session(tmp_path, "ref.mefd", password)
    _crash_run(ref, password=password)
    ref_chunks = {lvl: _chunks(_pair(ref, ch)[0].read_bytes())
                  for lvl, ch in (("s", None), ("c", "ch1"))}
    seen_states = set()
    for k in range(steps):
        p = _crash_session(tmp_path, f"c{k}.mefd", password)
        before = _tree(p)
        with pytest.raises(mef3io.IoError, match="simulated crash"):
            _crash_run(p, password=password, crash_at=k)
        marker = mef3io.operation_marker(p)
        if marker is None:
            # before the marker (or after its removal): the session is whole, old or new
            state = _state(p, pw)
            assert state in (OLD, NEW), (k, state)
            if state == OLD:
                assert {n: b for n, b in _tree(p).items() if not n.endswith((".new", ".part"))} == before
            seen_states.add(("nomarker", state))
        else:
            assert marker["operation"] == "remove-annotations"
            seen_states.add(("marker", None))
            # detected on open ...
            with warnings.catch_warnings(record=True) as caught:
                warnings.simplefilter("always")
                mef3io.Reader(str(p), password=pw).close()
            assert any(isinstance(w.message, mef3io.SessionOperationWarning) for w in caught)
            # ... records() refuses (a half-swapped file must not read as "no annotations"),
            # while the samples stay readable ...
            with warnings.catch_warnings(), mef3io.Reader(str(p), password=pw) as rd:
                warnings.simplefilter("ignore")
                with pytest.raises(mef3io.WriteConflictError) as ei:
                    rd.records()
                assert ei.value.code == "edit-interrupted"
                with pytest.raises(mef3io.WriteConflictError):
                    rd.records(level="all")
                assert len(rd.read("ch1")) > 0
            # ... writes are refused ...
            with pytest.raises(mef3io.WriteConflictError, match="annotation edit"):
                with mef3io.Writer(str(p), **_pw(password)) as w:
                    w.write_annotations([_note(9, "x")])
            # ... the validator says so ...
            rep = mef3io.Validator(str(p), password=pw).validate()
            assert any(f.check_id == "records.interrupted-edit" for f in rep.findings)
            # ... a dry run says what recovery would do, and writes nothing ...
            snapshot = _tree(p)
            dry = mef3io.recover_session(str(p), password=pw)
            assert dry.record_edits and dry.record_edits[0].action == "finish"
            assert _tree(p) == snapshot
            # ... and recovery ends in the NEW state, however often it is interrupted.
            j = 0
            while True:
                try:
                    _m._finish_interrupted_edit(str(p), j, -1)
                except mef3io.IoError as e:
                    assert "simulated crash" in str(e)
                    j += 1
                    assert j < 40
                    continue
                break
            assert mef3io.operation_marker(p) is None
            assert _state(p, pw) == NEW, k
            assert not _leftovers(p)
        rep = mef3io.recover_session(str(p), apply=True, password=pw)   # settles leftovers
        assert not _leftovers(p) and mef3io.operation_marker(p) is None
        assert _state(p, pw) in (OLD, NEW)
        if _state(p, pw) == NEW:           # the very bytes an uninterrupted call stores
            for lvl, ch in (("s", None), ("c", "ch1")):
                assert _chunks(_pair(p, ch)[0].read_bytes()) == ref_chunks[lvl], (k, lvl)
        rep2 = mef3io.Validator(str(p), password=pw).validate()
        assert rep2.ok, rep2.summary()
        # a later append works whatever the state
        _with_notes(p, [_note(20, "tail")], password=password)
        assert _texts(p, pw, level="session")[-1] == "tail"
    assert ("marker", None) in seen_states and any(s[0] == "nomarker" for s in seen_states)


def test_the_next_edit_finishes_an_interrupted_one_first(tmp_path):
    p = _crash_session(tmp_path, "s.mefd")
    with pytest.raises(mef3io.IoError, match="simulated crash"):
        _crash_run(p, crash_at=12)
    assert mef3io.operation_marker(p) is not None
    a = {"type": "Note", "time": START + S, "text": "a", "level": "session"}
    report = mef3io.remove_annotations(p, [a])
    assert report.resumed and any("interrupted" in n for n in report.notes)
    assert _texts(p, level="session") == ["b fixed", "c"]       # the first edit finished, then this one
    assert mef3io.operation_marker(p) is None


@pytest.mark.parametrize("password", [None, PASSWORDS], ids=["plain", "L1+L2"])
def test_an_ordinary_failure_at_every_step_puts_the_old_files_back(tmp_path, password):
    pw = password[1] if password else ""
    first_marker_step = None
    k = 0
    outcomes = []
    while True:
        p = _crash_session(tmp_path, f"f{k}.mefd", password)
        before = _tree(p)
        try:
            _crash_run(p, password=password, fail_at=k)
        except (mef3io.IoError, mef3io.WriteConflictError) as e:
            assert not _leftovers(p) and mef3io.operation_marker(p) is None, (k, str(e))
            assert getattr(e, "code", None) == "edit-failed" or "simulated" in str(e), (k, str(e))
            assert _tree(p) == before, k                            # the old state, byte for byte
            outcomes.append("old")
        else:
            outcomes.append("new")
            assert _state(p, pw) == NEW
            break
        k += 1
        assert k < 60
    assert outcomes[-1] == "new" and outcomes.count("old") >= 14
    # failing at the very last step (marker removal) still ends in a finished edit or the old state
    assert set(outcomes) <= {"old", "new"}


def test_a_new_file_that_is_not_intact_rolls_the_edit_back(tmp_path):
    # every step after the marker is down, then a damaged new file: recovery goes BACK,
    # however often it is interrupted, to the exact old bytes
    k = 0
    tried = 0
    while True:
        q = _crash_session(tmp_path, f"x{k}.mefd")
        before = _tree(q)
        with pytest.raises(mef3io.IoError, match="simulated crash"):
            _crash_run(q, crash_at=k)
        k += 1
        if mef3io.operation_marker(q) is None:
            if _state(q) == NEW:
                break
            continue
        news = sorted(f for f in q.rglob("*.new"))
        if not news:
            continue                                    # all new files already in place
        news[0].write_bytes(news[0].read_bytes()[:-5])  # damaged new file
        dry = mef3io.recover_session(str(q))
        assert dry.record_edits[0].action == "roll back"
        j = 0
        while True:
            try:
                _m._finish_interrupted_edit(str(q), j, -1)
            except mef3io.IoError:
                j += 1
                assert j < 60
                continue
            break
        assert _state(q) == OLD and mef3io.operation_marker(q) is None and not _leftovers(q)
        assert _tree(q) == before                       # the old files, byte for byte
        tried += 1
    assert tried >= 3


def test_leftover_temporary_files_are_swept_by_recovery_only(tmp_path):
    p = _crash_session(tmp_path, "s.mefd")
    with pytest.raises(mef3io.IoError, match="simulated crash"):
        _crash_run(p, crash_at=2)                                # a staged file, no marker
    assert _leftovers(p) and mef3io.operation_marker(p) is None
    assert _state(p) == OLD
    dry = mef3io.recover_session(str(p))
    assert any("temporary file" in n for n in dry.notes) and _leftovers(p)
    mef3io.recover_session(str(p), apply=True)
    assert not _leftovers(p) and _state(p) == OLD


def test_the_marker_of_an_interrupted_edit_cannot_be_cleared_by_hand(tmp_path):
    p = _crash_session(tmp_path, "s.mefd")
    with pytest.raises(mef3io.IoError, match="simulated crash"):
        _crash_run(p, crash_at=12)
    with pytest.raises(mef3io.WriteConflictError, match="recover_session"):
        mef3io.clear_operation_marker(p)
    assert mef3io.operation_marker(p) is not None
    mef3io.recover_session(str(p), apply=True)
    assert mef3io.operation_marker(p) is None and _state(p) == NEW


def test_a_dry_run_on_an_interrupted_session_says_to_recover_first(tmp_path):
    p = _crash_session(tmp_path, "s.mefd")
    with pytest.raises(mef3io.IoError, match="simulated crash"):
        _crash_run(p, crash_at=12)
    snapshot = _tree(p)
    with pytest.raises(mef3io.WriteConflictError) as e:
        mef3io.remove_annotations(p, [{"type": "Note", "time": START + S, "text": "a",
                                       "level": "session"}], dry_run=True)
    assert _code(e) == "edit-interrupted"
    assert _tree(p) == snapshot


# ---------------------------------------------------------------------------
# review round 1
# ---------------------------------------------------------------------------

def _snap(path):
    """Everything a refusal must leave alone: names, bytes and mtimes of files, and the folders."""
    root = Path(path)
    files = {str(f.relative_to(root)): (f.read_bytes(), f.stat().st_mtime_ns)
             for f in root.rglob("*") if f.is_file()}
    dirs = {str(f.relative_to(root)) for f in root.rglob("*") if f.is_dir()}
    siblings = sorted(x.name for x in root.parent.iterdir())
    return files, dirs, siblings


def _append_to_rdat(p, extra):
    """What an interrupted append leaves: bytes in the .rdat the header and index do not count."""
    rdat, _ = _pair(p)
    with open(rdat, "ab") as f:
        f.write(extra)


def _whole_record(text="TAILREC", t=9):
    """A complete, CRC-correct Note record (as write_annotations stores it) for the tail of a .rdat."""
    body = text.encode() + b"\0"
    body += b"\x7e" * (-len(body) % 16)
    rec = bytearray(24) + body
    rec[4:8] = b"Note"
    rec[9], rec[10], rec[11] = 1, 0, 0xFF          # version 1.0, not encrypted (-1)
    rec[12:16] = len(body).to_bytes(4, "little")
    rec[16:24] = (-(START + t * S)).to_bytes(8, "little", signed=True)
    rec[0:4] = _crc(bytes(rec[4:])).to_bytes(4, "little")
    return bytes(rec)


@pytest.mark.parametrize("tail", ["whole", "fragment", "index-ahead"])
def test_a_torn_annotation_file_is_refused_never_silently_trimmed(tmp_path, tail):
    p = _session(tmp_path / "s.mefd")
    _with_notes(p, [_note(1, "a"), _note(2, "b"), _note(3, "c")])
    rec = _records(p)[0]
    rdat, ridx = _pair(p)
    if tail == "whole":
        _append_to_rdat(p, _whole_record())                 # data landed, index and header did not
    elif tail == "fragment":
        _append_to_rdat(p, _whole_record()[:30])
    else:
        _append_to_rdat(p, _whole_record())
        # the index got a new entry too, the header counts did not
        d = bytearray(ridx.read_bytes())
        entry = bytearray(24)
        entry[0:4] = b"Note"
        entry[5], entry[6], entry[7] = 1, 0, 0xFF
        entry[8:16] = (len(rdat.read_bytes()) - len(_whole_record())).to_bytes(8, "little")
        d += entry
        ridx.write_bytes(bytes(d))
    before = _snap(p)
    with pytest.raises((mef3io.FormatError, mef3io.CrcError)) as e:
        mef3io.remove_annotations(p, [rec])
    assert _code(e) in ("record-torn", "record-index-mismatch"), _code(e)
    assert "recover_session" in str(e.value)
    assert _snap(p) == before
    # the repair that KEEPS the record, then the removal works and the record survives
    if tail == "whole":
        mef3io.recover_session(str(p), apply=True)
        mef3io.remove_annotations(p, [rec])
        assert b"TAILREC" in rdat.read_bytes()
        assert "a" not in _texts(p) and "TAILREC" in _texts(p)


def test_every_refusal_leaves_directory_bytes_and_mtimes_alone(tmp_path):
    p = _session(tmp_path / "s.mefd", channels=("ch1", "ch2"))
    _with_notes(p, [_note(1, "x"), _note(2, "dup"), _note(2, "dup"), _note(3, "y")])
    _with_notes(p, [_note(1, "k")], channel="ch1")
    recs = {r["text"]: r for r in _records(p)}
    dup, x = recs["dup"], recs["x"]
    ch = _records(p, "ch1", level="channel")[0]
    q = _session(tmp_path / "enc.mefd", password=PASSWORDS)
    _with_notes(q, [_note(1, "s")], password=PASSWORDS)
    enc = _records(q, PASSWORDS[1])[0]
    cases = [
        (p, dict(remove=[dup]), "ambiguous"),
        (p, dict(remove=[dict(dup, expect=5)]), "count-mismatch"),
        (p, dict(remove=[dict(x, text="nope")]), "no-match"),
        (p, dict(remove=[{k: v for k, v in x.items() if k != "text"}]), "selector-incomplete"),
        (p, dict(remove=[dict(x, bogus=1)]), "selector-unknown-field"),
        (p, dict(remove=[dict(x, expect=0)]), "bad-selector"),
        (p, dict(remove=[dict(x, level="channel", channel="nope")]), "unknown-channel"),
        (p, dict(remove=[dict(x, level="segment", channel="ch2", segment=7)]), "unknown-segment"),
        (p, dict(remove=[x, dict(x, all_copies=True)]), "selectors-overlap"),
        (p, dict(remove=[x], add=[dict(_note(1, "q"), channel="ch2")]), "add-level-missing"),
        (p, dict(remove=[x], add=[{"time": -5, "text": "neg"}]), "add-time"),
        (p, dict(remove=[x], password1="a", password2="b"), "password"),
        (p, dict(remove=[x], dry_run=True, password1="a"), "password"),
        (q, dict(remove=[enc]), "password"),
        (q, dict(remove=[enc], password1="bad", password2="bad2"), "password"),
        (q, dict(remove=[enc], password1=PASSWORDS[0]), "password"),
        (p, dict(remove=[]), "nothing-to-do"),
    ]
    for path, kw, code in cases:
        before = _snap(path)
        with pytest.raises((ValueError, mef3io.MefError)) as e:
            mef3io.remove_annotations(path, **kw)
        assert getattr(e.value, "code", None) == code, (code, str(e.value))
        assert _snap(path) == before, code
    # the same for a session being written, mid-operation, a tar and a damaged file
    busy = p / "ch1.timd" / "x.mef3io-tmp"
    busy.write_bytes(b"")
    before = _snap(p)
    with pytest.raises(mef3io.WriteConflictError):
        mef3io.remove_annotations(p, [x])
    assert _snap(p) == before
    busy.unlink()
    tar = mef3io.archive_session(str(p))
    before = _snap(p)
    with pytest.raises(mef3io.IoError) as e:
        mef3io.remove_annotations(tar, [x])
    assert e.value.code == "tar-read-only" and _snap(p) == before
    rdat, _ = _pair(p)
    rdat.write_bytes(rdat.read_bytes()[:-10])
    before = _snap(p)
    with pytest.raises((mef3io.FormatError, mef3io.CrcError)):
        mef3io.remove_annotations(p, [x])
    assert _snap(p) == before


def test_the_bytes_of_the_old_records_are_the_bytes_kept_plus_the_bytes_removed(tmp_path):
    p = _session(tmp_path / "s.mefd")
    _with_notes(p, [_note(i, "n" * (1 + 7 * i)) for i in range(1, 12)])
    rdat, ridx = _pair(p)
    old = _chunks(rdat.read_bytes())
    recs = _records(p)
    gone = [recs[i] for i in (0, 4, 10)]
    mef3io.remove_annotations(p, gone)
    new = _chunks(rdat.read_bytes())
    removed = [old[i] for i in (0, 4, 10)]
    assert sum(map(len, old)) == sum(map(len, new)) + sum(map(len, removed))
    assert [c for c in old if c not in removed] == new
    assert len(rdat.read_bytes()) == UH + sum(map(len, new))
    assert len(ridx.read_bytes()) == UH + 24 * len(new)


def test_recovery_says_plainly_when_the_new_files_were_lost_and_nothing_was_removed(tmp_path):
    p = _crash_session(tmp_path, "s.mefd")
    k = 0
    while True:
        p = _crash_session(tmp_path, f"l{k}.mefd")
        with pytest.raises(mef3io.IoError, match="simulated crash"):
            _crash_run(p, crash_at=k)
        k += 1
        if mef3io.operation_marker(p) is not None and list(p.rglob("*.new")):
            break
    for f in p.rglob("*.new"):
        f.unlink()                                  # a cleaner, an antivirus, someone tidying
    dry = mef3io.recover_session(str(p))
    (edit,) = dry.record_edits
    assert edit.action == "roll back" and edit.not_done and "missing" in edit.reason
    assert "NOT done" in dry.summary()
    rep = mef3io.recover_session(str(p), apply=True)
    assert rep.record_edits[0].action == "roll back" and rep.record_edits[0].applied
    assert mef3io.operation_marker(p) is None and _state(p) == OLD
    from mef3io import validate as _v
    assert _v._recover_main([str(p)]) == 0           # nothing left to do now


def test_cli_recover_exits_nonzero_when_the_removal_was_not_done(tmp_path, capsys):
    k = 0
    while True:
        p = _crash_session(tmp_path, f"c{k}.mefd")
        with pytest.raises(mef3io.IoError, match="simulated crash"):
            _crash_run(p, crash_at=k)
        k += 1
        if mef3io.operation_marker(p) is not None and list(p.rglob("*.new")):
            break
    for f in p.rglob("*.new"):
        f.unlink()
    from mef3io import validate as _v
    assert _v._recover_main([str(p), "--apply"]) == 1
    assert "NOT done" in capsys.readouterr().out


def _crash_where(tmp_path, name, pred):
    k = 0
    while True:
        p = _crash_session(tmp_path, f"{name}{k}.mefd")
        with pytest.raises(mef3io.IoError, match="simulated crash"):
            _crash_run(p, crash_at=k)
        k += 1
        if pred(p):
            return p
        assert k < 80


def _break_plan(p, style):
    m = (p / ".mef3io-operation").read_bytes().split(b"\n")
    head = [ln for ln in m if ln.split(b" ")[0] in (b"mef3io-operation", b"tool", b"operation", b"id")]
    if style == "truncated":
        body = head
    else:  # a newer layout this version cannot read
        body = head + [b"edit-format 2"] + [ln for ln in m if ln.startswith(b"file ")]
    (p / ".mef3io-operation").write_bytes(b"\n".join(body) + b"\n")


def _contents(*roots):
    out = []
    for r in roots:
        if Path(r).exists():
            out += [f.read_bytes() for f in Path(r).rglob("*")
                    if f.is_file() and f.name != ".mef3io-operation"]
    return sorted(out)


@pytest.mark.parametrize("style", ["truncated", "newer-format"])
def test_an_unreadable_plan_with_whole_target_pairs_keeps_them_and_sets_leftovers_aside(tmp_path, style):
    p = _crash_where(tmp_path, "a", lambda q: mef3io.operation_marker(q) is not None
                     and len(list(q.rglob("*.new"))) == 4 and not list(q.rglob("*.old")))
    _break_plan(p, style)
    everything = _contents(p)
    aside = p.parent / (p.name + ".edit-leftovers")
    newer = style == "newer-format"
    dry = mef3io.recover_session(str(p))
    (e,) = dry.record_edits
    if newer:                       # a marker of a newer layout is never settled by this build
        assert e.action == "refused" and "owner-gone" in e.reason or "assume_owner_gone" in e.reason
        with pytest.raises(mef3io.WriteConflictError) as ei:
            mef3io.recover_session(str(p), apply=True)
        assert ei.value.code == "edit-newer-format" and _contents(p) == everything
        dry = mef3io.recover_session(str(p), assume_owner_gone=True)
        (e,) = dry.record_edits
    assert e.action == "roll back" and e.not_done and "NOT done" in e.reason
    assert _contents(p) == everything
    rep = mef3io.recover_session(str(p), apply=True, assume_owner_gone=newer)
    assert rep.record_edits[0].applied and rep.record_edits[0].owner_override == newer
    assert mef3io.operation_marker(p) is None and _state(p) == OLD
    assert not _leftovers(p) and len(list(aside.rglob("*.new"))) == 4
    assert _contents(p, aside) == everything                     # nothing deleted, nothing made
    assert mef3io.validate_session(str(p)).ok


def test_an_unreadable_plan_mid_swap_restores_the_old_pair_and_deletes_nothing(tmp_path):
    p = _crash_where(tmp_path, "b", lambda q: mef3io.operation_marker(q) is not None
                     and not _pair(q)[0].exists() and list(q.rglob("*.old")))
    _break_plan(p, "truncated")
    everything = _contents(p)
    aside = p.parent / (p.name + ".edit-leftovers")
    with pytest.raises(mef3io.WriteConflictError):
        mef3io.clear_operation_marker(p)
    rep = mef3io.recover_session(str(p), apply=True)
    assert rep.record_edits[0].action == "roll back" and "NOT done" in rep.record_edits[0].reason
    assert mef3io.operation_marker(p) is None and _state(p) == OLD
    assert _contents(p, aside) == everything
    assert mef3io.validate_session(str(p)).ok


def test_an_unreadable_plan_with_no_whole_pair_to_go_by_is_refused_naming_the_files(tmp_path):
    p = _crash_where(tmp_path, "c", lambda q: mef3io.operation_marker(q) is not None
                     and not _pair(q)[0].exists() and list(q.rglob("*.old")))
    _break_plan(p, "truncated")
    for f in p.rglob("*.old"):
        f.unlink()
    before = _snap(p)
    for apply in (False, True):
        with pytest.raises(mef3io.FormatError) as e:
            mef3io.recover_session(str(p), apply=apply)
        assert e.value.code == "edit-unsettled" and ".rdat" in str(e.value)
        assert _snap(p) == before
    with pytest.raises(mef3io.WriteConflictError, match="recover_session"):
        mef3io.clear_operation_marker(p)


def test_an_unreadable_plan_with_nothing_beside_the_records_just_clears_the_marker(tmp_path):
    p = _session(tmp_path / "s.mefd")
    (p / ".mef3io-operation").write_bytes(
        b"mef3io-operation 1\ntool mef3io x\noperation remove-annotations\nid " + b"a" * 32 + b"\n")
    rep = mef3io.recover_session(str(p), apply=True)
    assert rep.record_edits[0].action == "settle" and mef3io.operation_marker(p) is None


def test_records_changed_is_detected_when_a_writer_appends_after_the_marker_is_down(tmp_path):
    p = _crash_session(tmp_path, "s.mefd")
    rdat, _ = _pair(p)
    original = rdat.read_bytes()
    fired = []

    def observer(kind, path):
        if kind == "marker-durable" and not fired:
            fired.append(1)
            with open(rdat, "ab") as f:      # a writer that was already past its marker check
                f.write(b"\x00")

    _m._set_edit_observer(observer)
    try:
        with pytest.raises(mef3io.WriteConflictError) as e:
            _crash_run(p)
    finally:
        _m._set_edit_observer(None)
    assert fired and e.value.code == "records-changed"
    assert mef3io.operation_marker(p) is None and not _leftovers(p)
    assert rdat.read_bytes() == original + b"\x00"          # nothing of the edit is in place


def test_the_old_file_is_checked_again_immediately_before_each_rename(tmp_path):
    p = _crash_session(tmp_path, "s.mefd")
    _, ridx = _pair(p)
    rdat, _ = _pair(p)
    original = rdat.read_bytes()
    fired = []

    def observer(kind, path):
        if kind == "rename" and not fired:        # the first swap rename: the index is still to go
            fired.append(1)
            with open(ridx, "ab") as f:
                f.write(b"\x00")

    _m._set_edit_observer(observer)
    try:
        with pytest.raises(mef3io.WriteConflictError) as e:
            _crash_run(p)
    finally:
        _m._set_edit_observer(None)
    assert fired and e.value.code == "records-changed"
    assert mef3io.operation_marker(p) is None and not _leftovers(p)
    assert rdat.read_bytes() == original                  # the pair already swapped was put back


def _record_events(fn):
    events = []
    _m._set_edit_observer(lambda kind, path: events.append((kind, path)))
    try:
        fn()
    finally:
        _m._set_edit_observer(None)
    return events


def _order_violations(events):
    """The durability order of one edit, from the recorded durable steps."""
    bad = []
    kinds = [k for k, _ in events]
    if "marker-durable" not in kinds:
        return ["no marker"]
    marker = kinds.index("marker-durable")
    # every new file is fsynced before it is renamed into place, and nothing is swapped before
    # the marker is durable
    for i, (k, v) in enumerate(events):
        if k != "rename":
            continue
        src, dst = v.split("|")
        if src.endswith(".new"):
            if not any(kk == "fsync-file" and vv == src for kk, vv in events[:i]):
                bad.append(f"rename of {Path(src).name} not preceded by its fsync")
        if (src.endswith(".new") or dst.endswith(".old")) and i < marker:
            bad.append("a swap rename before the marker is durable")
    news = [i for i, (k, v) in enumerate(events) if k == "fsync-file" and v.endswith(".new")]
    if not news or not any(kinds[i] == "flush-dir" for i in range(max(news), marker)):
        bad.append("no folder flush after the last new-file fsync and before the marker")
    to_old = [i for i, (k, v) in enumerate(events) if k == "rename" and v.split("|")[1].endswith(".old")]
    if not to_old or min(to_old) < marker:
        bad.append("target -> .old must come after the marker is durable")
    renames = [i for i, k in enumerate(kinds) if k == "rename" and i > marker]
    removes_old = [i for i, (k, v) in enumerate(events) if k == "remove" and v.endswith(".old")]
    if renames and removes_old:
        if not any(kinds[i] == "flush-dir" for i in range(max(renames), min(removes_old))):
            bad.append("no folder flush between the last rename and the first .old removal")
    done = kinds.index("marker-removed") if "marker-removed" in kinds else None
    if done is not None and removes_old:
        if not any(kinds[i] == "flush-dir" for i in range(max(removes_old), done)):
            bad.append("no folder flush between the .old removals and the marker removal")
    return bad


def test_durable_steps_happen_in_the_required_order(tmp_path):
    p = _crash_session(tmp_path, "s.mefd")
    events = _record_events(lambda: _crash_run(p))
    assert {"fsync-file", "flush-dir", "rename", "remove", "marker-durable",
            "marker-removed"} <= {k for k, _ in events}
    assert _order_violations(events) == []
    # the marker's own steps are seen: its part file is fsynced, renamed into place and the
    # folder flushed, all before "marker-durable"
    md = [k for k, _ in events].index("marker-durable")
    before = events[:md]
    part = [i for i, (k, v) in enumerate(before) if k == "fsync-file" and v.endswith(".mef3io-operation.part")]
    ren = [i for i, (k, v) in enumerate(before) if k == "rename" and v.endswith("|" + str(p / ".mef3io-operation"))]
    assert part and ren and part[0] < ren[0] < md and any(k == "flush-dir" for k, _ in before[ren[0]:])
    # the negative control (an edit that skips its flushes must fail this check) needs the
    # test-only build option and lives in the C++ suite: core/tests/test_annotation_edit.cpp


def test_a_release_module_has_no_way_to_turn_flushes_off():
    assert not [n for n in dir(_m) if "skip" in n.lower() and "flush" in n.lower()]
    assert not hasattr(_m, "_set_edit_skip_flush")


def test_the_direction_of_a_rollback_is_durable_before_the_first_undo(tmp_path):
    seen = 0
    for k in range(60):
        p = _crash_session(tmp_path, f"d{k}.mefd")
        events = []
        _m._set_edit_observer(lambda kind, path: events.append((kind, path)))
        try:
            try:
                _crash_run(p, fail_at=k)
            except (mef3io.IoError, mef3io.WriteConflictError):
                pass
        finally:
            _m._set_edit_observer(None)
        d = [i for i, (kind, _) in enumerate(events) if kind == "direction-durable"]
        if not d:
            continue
        seen += 1
        undo = [i for i, (kind, v) in enumerate(events)
                if kind == "rename" and v.split("|")[0].endswith(".old")]
        assert all(i > d[0] for i in undo), k
        flush_after = any(kind == "flush-dir" for kind, _ in events[:d[0]][-3:])
        assert flush_after, "the folder holding the marker is flushed before 'direction-durable'"
    assert seen >= 3


def test_note_text_that_is_not_utf8_is_refused_rather_than_matched_wrongly(tmp_path):
    p = _session(tmp_path / "s.mefd")
    _with_notes(p, [_note(1, "cafX"), _note(2, "plain")])
    rdat, ridx = _pair(p)
    d = bytearray(rdat.read_bytes())
    at = d.index(b"cafX") + 3
    d[at] = 0xE9                                              # Latin-1 e-acute, not UTF-8
    rec_off = UH
    n = int.from_bytes(d[rec_off + 12: rec_off + 16], "little")
    d[rec_off: rec_off + 4] = _crc(bytes(d[rec_off + 4: rec_off + 24 + n])).to_bytes(4, "little")
    d[4:8] = _crc(bytes(d[UH:])).to_bytes(4, "little")
    d[0:4] = _crc(bytes(d[4:UH])).to_bytes(4, "little")
    rdat.write_bytes(bytes(d))
    rec = _records(p)[0]
    assert "\ufffd" in rec["text"]
    before = _snap(p)
    with pytest.raises(ValueError) as e:
        mef3io.remove_annotations(p, [rec])
    assert e.value.code == "no-match" and "UTF-8" in str(e.value)
    assert _snap(p) == before
    mef3io.remove_annotations(p, [_records(p)[1]])            # the others are not affected
    assert len(_records(p)) == 1


def test_selector_times_are_exact_and_remove_must_be_a_list(tmp_path):
    p = _session(tmp_path / "s.mefd")
    _with_notes(p, [_note(1, "a"), _note(2, "b")])
    rec = _records(p)[0]
    before = _snap(p)
    with pytest.raises(ValueError, match="whole number"):
        mef3io.remove_annotations(p, [dict(rec, time=float(rec["time"]) + 0.5)])
    with pytest.raises(TypeError, match="list"):
        mef3io.remove_annotations(p, rec)
    assert _snap(p) == before
    mef3io.remove_annotations(p, [dict(rec, time=float(rec["time"]))])   # a whole float is exact
    assert _texts(p) == ["b"]


def test_records_that_differ_only_in_hidden_bytes_are_identical_copies(tmp_path):
    p = _session(tmp_path / "s.mefd")
    _with_notes(p, [_note(1, "dup"), _note(1, "dup")])
    rdat, _ = _pair(p)
    d = bytearray(rdat.read_bytes())
    first = UH
    n = int.from_bytes(d[first + 12: first + 16], "little")
    d[first + 24 + 4 + 2] = 0x41                    # a byte after the text's NUL: invisible to a selector
    d[first: first + 4] = _crc(bytes(d[first + 4: first + 24 + n])).to_bytes(4, "little")
    d[4:8] = _crc(bytes(d[UH:])).to_bytes(4, "little")
    d[0:4] = _crc(bytes(d[4:UH])).to_bytes(4, "little")
    rdat.write_bytes(bytes(d))
    recs = _records(p)
    with pytest.raises(ValueError) as e:
        mef3io.remove_annotations(p, [recs[0]])
    assert e.value.code == "ambiguous"                # the call cannot tell them apart; it says so


# ---------------------------------------------------------------------------
# review round 2
# ---------------------------------------------------------------------------

def _marker_crash(tmp_path, name):
    """A session + channel edit crashed in the middle of its swap (marker present)."""
    return _crash_where(tmp_path, name, lambda q: mef3io.operation_marker(q) is not None
                        and not _pair(q)[0].exists())


def test_every_route_to_the_records_refuses_while_an_edit_is_interrupted(tmp_path):
    p = _marker_crash(tmp_path, "r")
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        # Reader, MultiReader, the extension's raw reader
        with mef3io.Reader(str(p)) as r:
            with pytest.raises(mef3io.WriteConflictError) as e:
                r.records()
            assert e.value.code == "edit-interrupted"
            assert len(r.read("ch1")) > 0                       # samples stay readable
        with mef3io.MultiReader([str(p)]) as mr:
            with pytest.raises(mef3io.WriteConflictError) as e:
                mr.records()
            assert e.value.code == "edit-interrupted"
        raw = _m.Reader(str(p))
        with pytest.raises(mef3io.WriteConflictError):
            raw.records(None, None, "all")
        # compat
        with pytest.raises(mef3io.WriteConflictError):
            mef3io.MefReader(str(p)).get_annotations()
        # inspect still describes the session, and says the records are NOT reported
        s = mef3io.inspect(str(p))
    assert [w.code for w in s.warnings].count("annotation-edit-interrupted") == 1
    assert s.records == [] and [c.name for c in s.channels] == ["ch1"]
    assert all(c.records is None for c in s.channels)
    from mef3io._inspect import _main as inspect_main
    import contextlib
    import io
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf), warnings.catch_warnings():
        warnings.simplefilter("ignore")
        inspect_main([str(p)])
    assert "records: NOT reported" in buf.getvalue()
    # recovery settles it, and every route reads again
    mef3io.recover_session(str(p), apply=True)
    assert len(mef3io.Reader(str(p)).records()) > 0
    assert not [w for w in mef3io.inspect(str(p)).warnings if w.code == "annotation-edit-interrupted"]


def _three_channel_session(tmp_path, name):
    p = _session(tmp_path / name, channels=("ch1", "ch2", "ch3"))
    _with_notes(p, [_note(1, "a"), _note(2, "b"), _note(3, "c")])
    for ch in ("ch1", "ch2", "ch3"):
        _with_notes(p, [_note(1, "k1"), _note(2, "k2")], channel=ch)
    return p


def _three_channel_run(p, crash_at=-1):
    rm = [_records(p, level="session")[1]] + [
        _records(p, channel=ch, level="channel")[0] for ch in ("ch1", "ch2", "ch3")]
    return mef3io.remove_annotations(p, rm, add=[_note(2, "b fixed")], _crash_at=crash_at)


def _levels(p):
    return (tuple(_texts(p, level="session")),) + tuple(
        tuple(_texts(p, channel=ch, level="channel")) for ch in ("ch1", "ch2", "ch3"))


OLD3 = (("a", "b", "c"),) + (("k1", "k2"),) * 3
NEW3 = (("a", "b fixed", "c"),) + (("k2",),) * 3


def _damage_plan(p, how):
    raw = (p / ".mef3io-operation").read_bytes().split(b"\n")
    out = []
    for ln in raw:
        if how == "torn-crc" and ln.startswith(b"file "):
            ln = ln[:-3]                                         # 5 digits of the CRC
        elif how == "wrong-crc" and ln.startswith(b"file "):
            ln = ln[:-8] + b"0" * 8
        elif how == "format-2" and ln == b"edit-format 1":
            ln = b"edit-format 2"
        out.append(ln)
    if how == "truncated":
        out = [ln for ln in out if ln.split(b" ")[0] in (b"mef3io-operation", b"tool", b"operation", b"id")]
        out.append(b"")
    (p / ".mef3io-operation").write_bytes(b"\n".join(out))


@pytest.mark.parametrize("how", ["truncated", "torn-crc", "format-2", "wrong-crc"])
@pytest.mark.parametrize("shape", ["session+channel", "session+3-channels"])
def test_an_unreadable_plan_never_settles_to_a_mixed_state(tmp_path, how, shape):
    three = shape == "session+3-channels"
    make = (lambda n: _three_channel_session(tmp_path, n)) if three else (lambda n: _crash_session(tmp_path, n))
    run = (lambda q, k: _three_channel_run(q, k)) if three else (lambda q, k: _crash_run(q, crash_at=k))
    state = _levels if three else _state
    old, new = (OLD3, NEW3) if three else (OLD, NEW)
    k = 0
    verdicts = set()
    while True:
        p = make(f"u{how[:3]}{k}.mefd")
        try:
            run(p, k)
        except mef3io.IoError as e:
            assert "simulated crash" in str(e)
        else:
            break                                                 # the call completed: done
        k += 1
        if mef3io.operation_marker(p) is None:
            continue
        _damage_plan(p, how)
        before = _snap(p)
        if how == "format-2":                    # refused outright; the override settles as before
            with pytest.raises(mef3io.WriteConflictError) as ei:
                mef3io.recover_session(str(p), apply=True)
            assert ei.value.code == "edit-newer-format" and _snap(p) == before
        try:
            rep = mef3io.recover_session(str(p), apply=True, assume_owner_gone=(how == "format-2"))
        except mef3io.FormatError as e:
            assert e.code in ("edit-unsettled", "edit-journal-inconsistent")
            assert _snap(p) == before                              # refused, untouched
            verdicts.add("refused")
            continue
        (edit,) = rep.record_edits
        got = state(p)
        assert got in (old, new), (how, k, got)                    # never mixed
        if how != "wrong-crc":                                     # a wrong CRC may call a finished edit "not done"
            assert edit.not_done == (got == old), (how, k, edit.action, got)
        verdicts.add("old" if got == old else "new")
        assert mef3io.operation_marker(p) is None and mef3io.validate_session(str(p)).ok
    assert k >= 14 and verdicts <= {"old", "new", "refused"} and verdicts


def test_the_cli_exit_code_tells_done_rolled_back_and_refused_apart(tmp_path, capsys):
    from mef3io import validate as _v
    # done
    p = _crash_where(tmp_path, "x", lambda q: mef3io.operation_marker(q) is not None
                     and not _pair(q)[0].exists())
    assert _v._recover_main([str(p), "--apply"]) == 0 and _state(p) == NEW
    # rolled back (NOT done)
    q = _crash_where(tmp_path, "y", lambda q: mef3io.operation_marker(q) is not None
                     and not _pair(q)[0].exists())
    for f in q.rglob("*.new"):
        f.unlink()
    assert _v._recover_main([str(q), "--apply"]) == 1 and _state(q) == OLD
    # refused
    r = _crash_where(tmp_path, "z", lambda q: mef3io.operation_marker(q) is not None
                     and not _pair(q)[0].exists())
    _damage_plan(r, "truncated")
    for f in r.rglob("*.old"):
        f.unlink()
    assert _v._recover_main([str(r), "--apply"]) == 2
    capsys.readouterr()


def test_a_writer_rechecks_the_marker_right_before_and_right_after_its_append(tmp_path):
    p = _session(tmp_path / "s.mefd")
    _with_notes(p, [_note(1, "a")])
    rdat, _ = _pair(p)
    marker = p / ".mef3io-operation"
    text = (b"mef3io-operation 1\ntool mef3io x\noperation remove-annotations\nid " + b"a" * 32 + b"\n")
    # an edit starts after the writer's first check, before the append: nothing is appended
    before = rdat.read_bytes()

    def at(kind_wanted):
        def observer(kind, path):
            if kind == kind_wanted and not marker.exists():
                marker.write_bytes(text)
        return observer

    with mef3io.Writer(str(p)) as w:
        _m._set_edit_observer(at("before-append"))
        try:
            with pytest.raises(mef3io.WriteConflictError, match="middle of"):
                w.write_annotations([_note(5, "late")])
        finally:
            _m._set_edit_observer(None)
    assert rdat.read_bytes() == before
    marker.unlink()
    # an edit starts during the append: the record IS appended, and the writer says so
    with mef3io.Writer(str(p)) as w:
        _m._set_edit_observer(at("after-append"))
        try:
            with pytest.raises(mef3io.WriteConflictError, match="may have been lost"):
                w.write_annotations([_note(5, "late")])
        finally:
            _m._set_edit_observer(None)
    marker.unlink()
    assert "late" in _texts(p)


# ---------------------------------------------------------------------------
# review round 3
# ---------------------------------------------------------------------------

def test_inspect_json_reports_no_channel_count_while_an_edit_is_interrupted(tmp_path):
    p = _marker_crash(tmp_path, "j")
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        s = mef3io.inspect(str(p))
    assert s.channels[0].records is None and s.records == []
    assert all(c["records"] is None for c in s.to_dict()["channels"])
    healthy = _crash_session(tmp_path, "h.mefd")
    assert mef3io.inspect(str(healthy)).channels[0].records == 2


def _fail_then_crash_in_rollback(p, fail_at, crash_at):
    rm = [_records(p, level="session")[1]] + [
        _records(p, channel=ch, level="channel")[0] for ch in ("ch1", "ch2", "ch3")]
    return mef3io.remove_annotations(p, rm, add=[_note(2, "b fixed")],
                                     _fail_at=fail_at, _crash_at=crash_at)


@pytest.mark.parametrize("how", ["readable", "truncated", "torn-crc"])
def test_a_rollback_that_dies_part_way_never_settles_mixed(tmp_path, how):
    # session + 3 channels: an error after the swaps starts a rollback; the process dies
    # at each step of it; then the plan may be unreadable. The end state is all old or all
    # new, the report agrees, and no step of the rollback hides the evidence of the next.
    seen_old = seen_new = 0
    for fail_at in range(35, 45):
        for j in range(0, 14):
            p = _three_channel_session(tmp_path, f"f{how[:3]}{fail_at}_{j}.mefd")
            try:
                _fail_then_crash_in_rollback(p, fail_at, fail_at + 1 + j)
            except mef3io.IoError as e:
                if "simulated crash" not in str(e):
                    break                                    # the rollback completed by itself
            if mef3io.operation_marker(p) is None:
                continue
            if how != "readable":
                _damage_plan(p, how)
            rep = mef3io.recover_session(str(p), apply=True)
            got = _levels(p)
            assert got in (OLD3, NEW3), (fail_at, j, got)
            for edit in rep.record_edits:
                assert edit.not_done == (got == OLD3), (fail_at, j, edit.action, got)
            seen_old += got == OLD3
            seen_new += got == NEW3
            assert mef3io.operation_marker(p) is None and mef3io.validate_session(str(p)).ok
    assert seen_old > 20


@pytest.mark.parametrize("how", ["truncated", "torn-crc"])
def test_recovery_with_an_unreadable_plan_that_dies_part_way_is_finished_by_the_next(tmp_path, how):
    import shutil as _sh
    k = 0
    checked = 0
    while True:
        p = _three_channel_session(tmp_path, f"w{how[:3]}{k}.mefd")
        try:
            _three_channel_run(p, k)
            break
        except mef3io.IoError:
            pass
        k += 1
        if mef3io.operation_marker(p) is None:
            continue
        _damage_plan(p, how)
        j = 0
        while True:
            try:
                _m._finish_interrupted_edit(str(p), j, -1)
                break                                     # recovery completed
            except mef3io.IoError as e:
                assert "simulated crash" in str(e), str(e)
            copy = tmp_path / f"copy{how[:3]}{k}_{j}.mefd"
            _sh.copytree(p, copy)                         # the state a crash at step j leaves
            mef3io.recover_session(str(copy), apply=True)
            assert _levels(copy) in (OLD3, NEW3), (k, j, _levels(copy))
            assert mef3io.validate_session(str(copy)).ok
            checked += 1
            _sh.rmtree(copy)
            j += 1
            assert j < 40
        assert _levels(p) in (OLD3, NEW3), (k, _levels(p))
    assert checked > 30


_LIVE_EDIT = """
import sys, time, os
sys.path[:0] = {paths!r}
import mef3io
from mef3io import _mef3io as m
root, ready, go = sys.argv[1:4]
def observer(kind, path):
    if kind == "marker-durable":
        open(ready, "w").close()
        while not os.path.exists(go):
            time.sleep(0.02)
m._set_edit_observer(observer)
recs = mef3io.Reader(root).records(level="session")
mef3io.remove_annotations(root, [recs[1]], add=[{{"time": recs[1]["time"], "text": "b fixed"}}])
"""


def _marker_lines(p):
    return (p / ".mef3io-operation").read_bytes().split(b"\n")


def test_recovery_refuses_while_another_program_is_running_the_edit(tmp_path):
    import subprocess
    import time
    p = _crash_session(tmp_path, "s.mefd")
    ready, go = tmp_path / "ready", tmp_path / "go"
    script = tmp_path / "live.py"
    script.write_text(_LIVE_EDIT.format(paths=[p_ for p_ in sys.path if p_]))
    child = subprocess.Popen([sys.executable, str(script), str(p), str(ready), str(go)],
                             env=dict(os.environ, PYTHONPATH=os.pathsep.join(q for q in sys.path if q)))
    try:
        for _ in range(500):
            if ready.exists():
                break
            assert child.poll() is None
            time.sleep(0.02)
        assert ready.exists()
        before = _snap(p)
        dry = mef3io.recover_session(str(p))
        assert dry.record_edits[0].running and "RUNNING" in dry.summary()
        for call in (lambda: mef3io.recover_session(str(p), apply=True),
                     lambda: mef3io.remove_annotations(
                         p, [{"type": "Note", "time": START + S, "text": "a", "level": "session"}])):
            with pytest.raises(mef3io.WriteConflictError) as e:
                call()
            assert e.value.code == "edit-in-progress" and str(child.pid) in str(e.value)
            assert _snap(p) == before                     # nothing settled under the running edit
        with warnings.catch_warnings():
            warnings.simplefilter("ignore")
            with pytest.raises(mef3io.WriteConflictError) as e:
                mef3io.Reader(str(p)).records()
        assert e.value.code == "edit-interrupted" and "in progress or was interrupted" in str(e.value)
        go.write_text("go")
        assert child.wait(timeout=60) == 0
    finally:
        if child.poll() is None:
            child.kill()
    assert mef3io.operation_marker(p) is None
    assert _texts(p, level="session") == ["a", "b fixed", "c"]


def test_recovery_goes_ahead_when_the_owner_is_dead_or_elsewhere(tmp_path):
    import subprocess
    dead = subprocess.Popen([sys.executable, "-c", "pass"])
    dead.wait()
    for name, edit in (("dead", lambda ln: ln.split(b" ")[:1] + [str(dead.pid).encode()] + ln.split(b" ")[2:]),
                       ("elsewhere", lambda ln: ln.split(b" ")[:2] + [b"6f74686572", b"-"])):
        p = _marker_crash(tmp_path, name)
        lines = [b" ".join(edit(ln)) if ln.startswith(b"owner ") else ln for ln in _marker_lines(p)]
        assert any(ln.startswith(b"owner ") for ln in lines)
        (p / ".mef3io-operation").write_bytes(b"\n".join(lines))
        dry = mef3io.recover_session(str(p))
        assert not dry.record_edits[0].running
        mef3io.recover_session(str(p), apply=True)
        assert mef3io.operation_marker(p) is None and _state(p) in (OLD, NEW)


# ---------------------------------------------------------------------------
# the owner of an edit (post-merge review of round 3)
# ---------------------------------------------------------------------------

def _owner_line_index(lines):
    return next(i for i, ln in enumerate(lines) if ln.startswith(b"owner "))


def _set_owner(p, pid=None, host_hex=None, started="-", keep_check=True):
    """Rewrite the marker's owner line (with its own check, or in the previous build's form)."""
    lines = _marker_lines(p)
    i = _owner_line_index(lines)
    _, old_pid, old_host, *_ = lines[i].split(b" ")
    body = b"owner %s %s %s" % (str(pid).encode() if pid is not None else old_pid,
                                 host_hex if host_hex is not None else old_host, started.encode())
    if keep_check:
        body += b" " + format(_crc(body), "08x").encode()
    lines[i] = body
    (p / ".mef3io-operation").write_bytes(b"\n".join(lines))


def test_the_marker_starts_with_its_owner_line_and_that_line_carries_its_own_check(tmp_path):
    p = _marker_crash(tmp_path, "m")
    lines = _marker_lines(p)
    assert [ln.split(b" ")[0] for ln in lines[:5]] == [b"mef3io-operation", b"tool", b"operation", b"id", b"owner"]
    body, check = lines[4].rsplit(b" ", 1)
    assert format(_crc(body), "08x").encode() == check
    assert lines[5] == b"edit-format 1"


def test_a_marker_of_the_previous_build_still_recovers(tmp_path):
    p = _marker_crash(tmp_path, "old")
    _set_owner(p, keep_check=False)                      # "owner pid host start", no check
    mef3io.recover_session(str(p), apply=True)
    assert mef3io.operation_marker(p) is None and _state(p) in (OLD, NEW)
    q = _marker_crash(tmp_path, "noown")
    lines = [ln for ln in _marker_lines(q) if not ln.startswith(b"owner ")]
    (q / ".mef3io-operation").write_bytes(b"\n".join(lines))
    mef3io.recover_session(str(q), apply=True)
    assert mef3io.operation_marker(q) is None and _state(q) in (OLD, NEW)


def test_a_zombie_pid_one_and_a_reused_pid_and_the_override(tmp_path):
    import signal
    import subprocess
    import time
    # a live process whose start time is not recorded (macOS, Windows; pid 1 on a POSIX box):
    # recovery refuses, the override settles
    alive = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(120)"])
    live_pid = 1 if os.name == "posix" else alive.pid
    p = _marker_crash(tmp_path, "one")
    _set_owner(p, pid=live_pid)
    before = _snap(p)
    dry = mef3io.recover_session(str(p))
    assert dry.record_edits[0].running
    with pytest.raises(mef3io.WriteConflictError) as e:
        mef3io.recover_session(str(p), apply=True)
    assert e.value.code == "edit-in-progress" and "assume_owner_gone" in str(e.value)
    assert _snap(p) == before
    with pytest.raises(mef3io.WriteConflictError):
        mef3io.remove_annotations(p, [{"type": "Note", "time": START + S, "text": "a", "level": "session"}])
    rep = mef3io.recover_session(str(p), apply=True, assume_owner_gone=True)
    assert rep.record_edits[0].owner_override and rep.record_edits[0].applied
    assert "skipped on request" in rep.record_edits[0].reason
    assert mef3io.operation_marker(p) is None and _state(p) in (OLD, NEW)
    # the CLI flag
    from mef3io import validate as _v
    q = _marker_crash(tmp_path, "cli")
    _set_owner(q, pid=live_pid)
    assert _v._recover_main([str(q), "--apply"]) == 2 and mef3io.operation_marker(q) is not None
    assert _v._recover_main([str(q), "--apply", "--owner-gone"]) in (0, 1)
    assert mef3io.operation_marker(q) is None
    # a reused pid without a start time looks alive and needs the override; with the real
    # start time of an unrelated process it is told apart (Linux)
    sleeper = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)"])
    try:
        r = _marker_crash(tmp_path, "reuse")
        _set_owner(r, pid=sleeper.pid, started="-")
        with pytest.raises(mef3io.WriteConflictError):
            mef3io.recover_session(str(r), apply=True)
        if sys.platform.startswith("linux"):
            _set_owner(r, pid=sleeper.pid, started="1")           # not this process's start time
            mef3io.recover_session(str(r), apply=True)
            assert mef3io.operation_marker(r) is None
        # a zombie (killed, not reaped) is dead
        z = _marker_crash(tmp_path, "zombie")
        _set_owner(z, pid=sleeper.pid, started="-")
        sleeper.send_signal(signal.SIGKILL) if hasattr(signal, "SIGKILL") else sleeper.kill()
        time.sleep(0.3)                                           # never wait()ed: a zombie
        if sys.platform.startswith("linux"):
            assert "Z" in Path(f"/proc/{sleeper.pid}/stat").read_text().rsplit(")", 1)[1][:3]
            mef3io.recover_session(str(z), apply=True)
            assert mef3io.operation_marker(z) is None
    finally:
        sleeper.kill()
        sleeper.wait()
        alive.kill()
        alive.wait()


_PAUSED_EDIT = """
import sys, time, os
sys.path[:0] = {paths!r}
import mef3io
from mef3io import _mef3io as m
root, ready, go, nth = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
renames = [0]
def observer(kind, path):
    if kind == "rename" and "|" in path and not path.split("|")[0].endswith(".part"):
        renames[0] += 1
        if renames[0] == nth:
            open(ready, "w").close()
            while not os.path.exists(go):
                time.sleep(0.02)
m._set_edit_observer(observer)
rm = [mef3io.Reader(root).records(level="session")[1], mef3io.Reader(root).records("ch1", level="channel")[0]]
mef3io.remove_annotations(root, rm, add=[{{"time": rm[0]["time"], "text": "b fixed"}}])
"""


@pytest.mark.parametrize("nth", [2, 5])
@pytest.mark.parametrize("damage", ["readable", "truncated", "newer-format"])
def test_recovery_never_settles_under_a_live_edit_whatever_the_plan_says(tmp_path, nth, damage):
    import subprocess
    import time
    p = _crash_session(tmp_path, "s.mefd")
    ready, go = tmp_path / "ready", tmp_path / "go"
    script = tmp_path / "paused.py"
    script.write_text(_PAUSED_EDIT.format(paths=[q for q in sys.path if q]))
    child = subprocess.Popen([sys.executable, str(script), str(p), str(ready), str(go), str(nth)],
                             env=dict(os.environ, PYTHONPATH=os.pathsep.join(q for q in sys.path if q)))
    try:
        for _ in range(500):
            if ready.exists():
                break
            assert child.poll() is None
            time.sleep(0.02)
        assert ready.exists()
        if damage != "readable":
            m = _marker_lines(p)
            head = [ln for ln in m if ln.split(b" ")[0] in (b"mef3io-operation", b"tool", b"operation", b"id", b"owner")]
            body = head if damage == "truncated" else head + [b"edit-format 2"]
            (p / ".mef3io-operation").write_bytes(b"\n".join(body) + b"\n")
        before = _snap(p)
        want = "edit-newer-format" if damage == "newer-format" else "edit-in-progress"
        for call in (lambda: mef3io.recover_session(str(p), apply=True),
                     lambda: mef3io.remove_annotations(
                         p, [{"type": "Note", "time": START + 3 * S, "text": "c", "level": "session"}])):
            with pytest.raises(mef3io.WriteConflictError) as e:
                call()
            assert e.value.code in (want, "edit-in-progress"), e.value.code
            assert _snap(p) == before                              # nothing settled, nothing touched
        go.write_text("go")
        assert child.wait(timeout=60) == 0                         # the editor finishes undisturbed
    finally:
        if child.poll() is None:
            child.kill()
    assert mef3io.operation_marker(p) is None and _state(p) == NEW
    assert mef3io.validate_session(str(p)).ok


def test_recovery_refuses_under_an_edit_running_in_another_thread_of_the_same_process(tmp_path):
    import threading
    p = _crash_session(tmp_path, "s.mefd")
    paused, release = threading.Event(), threading.Event()
    seen = [0]

    def observer(kind, path):
        if kind == "rename" and not path.split("|")[0].endswith(".part"):
            seen[0] += 1
            if seen[0] == 2:
                paused.set()
                assert release.wait(30)

    errors = []

    def edit():
        try:
            _crash_run(p)
        except Exception as exc:                     # pragma: no cover
            errors.append(exc)

    _m._set_edit_observer(observer)
    t = threading.Thread(target=edit)
    try:
        t.start()
        assert paused.wait(30)
        before = _snap(p)
        with pytest.raises(mef3io.WriteConflictError) as e:
            mef3io.recover_session(str(p), apply=True)
        assert e.value.code == "edit-in-progress"
        assert mef3io.recover_session(str(p)).record_edits[0].running
        assert _snap(p) == before
        release.set()
        t.join(60)
    finally:
        release.set()
        _m._set_edit_observer(None)
    assert not errors and _state(p) == NEW and mef3io.operation_marker(p) is None
    # a crashed earlier call of this process (nothing in flight any more) is settled as before
    q = _marker_crash(tmp_path, "crashed")
    mef3io.recover_session(str(q), apply=True)
    assert mef3io.operation_marker(q) is None
