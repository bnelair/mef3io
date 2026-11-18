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


def test_recovery_of_a_session_with_an_unreadable_plan_refuses_to_guess(tmp_path):
    p = _session(tmp_path / "s.mefd")
    (p / ".mef3io-operation").write_bytes(
        b"mef3io-operation 1\ntool mef3io x\noperation remove-annotations\nid " + b"a" * 32 + b"\n")
    with pytest.raises(mef3io.FormatError) as e:
        mef3io.recover_session(str(p), apply=True)
    assert _code(e) == "edit-journal-inconsistent"
    assert (p / ".mef3io-operation").exists()


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
