"""#111 — anonymize for apps: check() without writing, stable refusal codes,
the source's original times in the report.

What is pinned here:

* ``mef3io.anonymize.check`` AGREES with ``anonymize_session`` on every
  refusal: for each case (every refusal case of test_p29 plus one per code
  that can be decided up front) the check lists the code, and its FIRST
  refusal is exactly what ``anonymize_session`` raises: same code, same
  exception type, same message (inspect blockers: one exception lists them
  all). Neither writes anything;
* the same agreement for the default copy, which keeps every time (no
  shift): every refusal that is not about a shift is still made, the
  shift-only ones are not, and where ``check`` passes the copy is published;
* the refusals only the copy pass can meet are raised with their code, and
  ``check`` passes those sessions;
* every code in ``REFUSAL_CODES`` is documented in docs/anonymization.md
  with the same exception type and category, and every code not marked
  defensive is reached by a case below;
* ``check`` writes nothing, reads no ``.tdat`` (``/proc/self/io`` rchar
  against a session whose data dwarfs its metadata), honours cancel and
  releases the GIL;
* the report's original start/end (the re-identification key) are right,
  stay out of repr/summary/to_dict, and appear nowhere in the copy;
* ``mef3io.password_interop_issue``.
"""
from __future__ import annotations

import copy
import os
import sys
import pickle
import re
import shutil
import struct
import warnings
from pathlib import Path

import numpy as np
import pytest

import mef3io
from mef3io.anonymize import REFUSAL_CODES

from test_p29_anonymize import (
    DAY,
    SECOND,
    SHIFT,
    START,
    T_POS,
    _edit_record,
    _legacy,
    _named_session,
    _plant_uh,
    _rename_segment,
    _rich_session,
    _set_rto,
    _signal,
    _src_rdat,
)

DOCS = Path(__file__).resolve().parents[1] / "docs" / "anonymization.md"
FS = 256.0


@pytest.fixture(scope="module")
def templates(tmp_path_factory):
    base = tmp_path_factory.mktemp("templates")
    plain = _rich_session(base / "plain" / "jane_roe.mefd")
    enc = _rich_session(base / "enc" / "jane_roe.mefd", password=True)
    return {"plain": plain, "enc": enc}


def _fresh(templates, tmp_path, which="plain"):
    dst = tmp_path / "src" / "jane_roe.mefd"
    shutil.copytree(templates[which], dst)
    return dst


def _quiet(fn, *a, **kw):
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        return fn(*a, **kw)


def _tree(root):
    return sorted(str(p.relative_to(root)) for p in Path(root).rglob("*"))


# --------------------------------------------------------------------------- cases
#
# Each case: (expected code, build(templates, tmp_path) -> (src, dst, kwargs)).
# kwargs may hold "shift" (default SHIFT); everything else goes to both calls.


def _plain(templates, tmp_path, dst="S001.mefd", **kw):
    return _fresh(templates, tmp_path), tmp_path / dst, kw


def _mutated(mutate, which="plain", dst="S001.mefd", **kw):
    def build(templates, tmp_path):
        src = _fresh(templates, tmp_path, which)
        mutate(src)
        return src, tmp_path / dst, kw
    return build


def _named(dst, channels=("c1", "c2"), subject=None, folder="p.mefd", **kw):
    def build(templates, tmp_path):
        src = _named_session(tmp_path / "src" / folder, channels,
                             subject if subject is not None else mef3io.Subject(
                                 name_1="Jane", name_2="Roe Müller"))
        return src, tmp_path / dst, kw
    return build


def _truncate(glob, keep):
    def f(src):
        p = next(src.glob(glob))
        b = p.read_bytes()
        p.write_bytes(b[:keep] if keep >= 0 else b[:keep])
    return f


def _flip(glob, off):
    def f(src):
        p = next(src.glob(glob))
        b = bytearray(p.read_bytes())
        b[off] ^= 0xFF
        p.write_bytes(bytes(b))
    return f


def _two_foreign_record_files(src):
    r = src / "jane_roe.rdat"
    for n in ("a", "b"):
        shutil.copy(r, src / f"{n}.rdat")
        shutil.copy(r.with_suffix(".ridx"), src / f"{n}.ridx")
    r.unlink()
    r.with_suffix(".ridx").unlink()


def _duplicate_segment(src):
    a = next(src.glob("ch3.timd/*.segd"))
    b = a.parent / "ch3-0.segd"
    shutil.copytree(a, b)
    for f in list(b.iterdir()):
        f.rename(b / ("ch3-0" + f.suffix))


def _move_an_index_entry(src):
    p = src / "jane_roe.ridx"
    raw = bytearray(p.read_bytes())
    struct.pack_into("<q", raw, 1024 + 8, 7)  # entry 0 names an offset no record has
    p.write_bytes(bytes(raw))


def _disputed_offset(templates, tmp_path):
    src = tmp_path / "src" / "dis.mefd"
    with mef3io.Writer(str(src), overwrite=True) as w:
        for ch in ("a", "b"):
            w.write(ch, _signal(2560, 1), START, FS, precision=3)
    _set_rto(src, START - DAY, ["a"])
    return src, tmp_path / "S001.mefd", {}


def _disputed_offset_with_session_records(templates, tmp_path):
    # Session-level records stored against an offset (negated) whose
    # segments disagree on it: a cut cannot place them.
    src = _fresh(templates, tmp_path)
    _set_rto(src, START - DAY, ["ch1"])
    return src, tmp_path / "S001.mefd", {"time_range": (START, START + 30 * SECOND)}


def _late_legacy(templates, tmp_path):
    src = _legacy(tmp_path / "src" / "late.mefd", rto=START + 3600 * SECOND)
    return src, tmp_path / "S001.mefd", {}


def _glued(templates, tmp_path):
    """A left-out channel's name glued into kept note text (#115): the
    removal takes whole words only, the copy's check would refuse it."""
    src = tmp_path / "src" / "glued.mefd"
    with mef3io.Writer(str(src), overwrite=True) as w:
        for ch in ("a3", "LH1"):
            w.write(ch, _signal(2560, 1), START, FS, precision=3)
        w.write_annotations([{"type": "Note", "time": START + SECOND, "text": "a3a3 artefact"}])
    return src, tmp_path / "S001.mefd", {"exclude_channels": ["a3"], "text": "keep"}


def _inside(templates, tmp_path):
    src = _fresh(templates, tmp_path)
    return src, src / "inner.mefd", {}


def _busy(templates, tmp_path):
    (tmp_path / "busy.mefd.partial").mkdir()
    return _fresh(templates, tmp_path), tmp_path / "busy.mefd", {}


def _mid_operation(templates, tmp_path):
    # A rename interrupted right after its marker was placed (#72).
    src = _fresh(templates, tmp_path)
    (src / ".mef3io-operation").write_text("mef3io-operation 1\ntool mef3io\noperation rename\nid 0\n")
    return src, tmp_path / "S001.mefd", {}


def _exists(templates, tmp_path):
    (tmp_path / "S001.mefd").mkdir()
    return _fresh(templates, tmp_path), tmp_path / "S001.mefd", {}


def _missing(templates, tmp_path):
    return tmp_path / "nowhere.mefd", tmp_path / "S001.mefd", {}


def _not_a_session(templates, tmp_path):
    src = tmp_path / "src" / "recording"
    shutil.copytree(templates["plain"], src)
    return src, tmp_path / "S001.mefd", {}


def _non_utf8_segment_folder(src):
    # Only Linux filesystems take arbitrary bytes in a name: APFS rejects a
    # non-UTF-8 name (EILSEQ) and Windows paths are UTF-16.
    if not sys.platform.startswith("linux"):
        pytest.skip("non-UTF-8 file names need a Linux filesystem")
    seg = next((src / "ch3.timd").glob("*.segd"))
    for f in list(seg.iterdir()):
        os.rename(bytes(f), bytes(seg) + b"/ch3-\xff" + f.suffix.encode())
    os.rename(bytes(seg), bytes(seg.parent) + b"/ch3-\xff.segd")


def _record_file_header_before_the_shift(src):
    # The session record file's header start time a day before the recording
    # (inspect does not count it in the session's range): the shift below
    # keeps the session's first time above 0 but not this one. Before #111
    # only the copy pass met it.
    _plant_uh(src, "jane_roe.rdat", 16, -(START - DAY))


def _source_times_in(text):
    """10+-digit numbers in `text` that could be one of the fixtures' source
    times (all start at START), plain or negated."""
    return [n for n in re.findall(r"\d{10,}", text) if abs(int(n) - START) <= 3 * DAY]


CASES = {
    # --- the refusal cases of test_p29 -----------------------------------
    "shift 0": ("shift-zero", lambda t, p: _plain(t, p, shift=0)),
    "shift below the floor": ("shift-below-floor", lambda t, p: _plain(t, p, shift=3600 * SECOND)),
    "shift to 1970": ("shift-not-positive", lambda t, p: _plain(t, p, shift=-(START + DAY))),
    "unknown clear entry": ("clear-unknown",
                            lambda t, p: _plain(t, p, clear=("subject", "nonsense"))),
    "dst exists": ("dst-exists", _exists),
    "dst inside src": ("dst-inside-src", _inside),
    "dst not .mefd": ("dst-not-mefd", lambda t, p: _plain(t, p, dst="noext")),
    "partial in the way": ("partial-exists", _busy),
    "source mid-rename": ("source-mid-operation", _mid_operation),
    "rename=False, other name": ("rename-name-mismatch",
                                 lambda t, p: _plain(t, p, dst="other.mefd", rename=False)),
    "rename=False keeps the name": ("name-contains-identity",
                                    lambda t, p: _plain(t, p, dst="jane_roe.mefd", rename=False)),
    "pseudonym = the name": ("pseudonym-contains-identity",
                             lambda t, p: _plain(t, p, pseudonym="Jane Roe")),
    "pseudonym = a word": ("pseudonym-contains-identity", lambda t, p: _plain(t, p, pseudonym="jane")),
    "copy name holds the ID": ("name-contains-identity",
                               lambda t, p: _plain(t, p, dst="P_MRN4711_anon.mefd")),
    "copy name holds the name": ("name-contains-identity",
                                 lambda t, p: _plain(t, p, dst="Jane_Roe.mefd")),
    "copy name holds the session name": (
        "name-contains-identity",
        _named("night7_copy.mefd", subject=mef3io.Subject(), folder="night7.mefd")),
    "copy name Roe1": ("name-contains-identity", _named("Roe1.mefd")),
    "copy name S_muller": ("name-contains-identity", _named("S_muller.mefd")),
    "pseudonym XRoe": ("pseudonym-contains-identity", _named("S001.mefd", pseudonym="XRoe")),
    "both channel lists": ("channel-selection-conflict",
                           lambda t, p: _plain(t, p, channels=["ch1"], exclude_channels=["ch2"])),
    "unknown channel": ("channel-unknown", lambda t, p: _plain(t, p, channels=["nope"])),
    "unknown excluded channel": ("channel-unknown",
                                 lambda t, p: _plain(t, p, exclude_channels=["nope"])),
    "every channel excluded": ("no-channels-left",
                               lambda t, p: _plain(t, p, exclude_channels=["ch1", "ch2", "ch3"])),
    "channels=[]": ("no-channels-left", lambda t, p: _plain(t, p, channels=[])),
    "segments disagree on the offset": ("rto-disputed", _disputed_offset),
    "a cut of session records under a disputed offset": (
        "time-range-offset-disputed", _disputed_offset_with_session_records),
    "a time at or before the offset": ("time-before-rto", _late_legacy),
    "level-1 password": ("password-level1-only",
                         _mutated(lambda s: None, which="enc", password="pw-one")),
    "one output password": ("output-password-unpaired",
                            lambda t, p: _plain(t, p, output_password="only-one")),
    "a time in an id_number": ("id-number-time", _mutated(lambda s: _edit_record(
        _src_rdat(s), b"Epoc", lambda r, b, n: struct.pack_into("<q", r, b, T_POS)))),
    "segment folder names another number": (
        "segment-number-mismatch", _mutated(lambda s: _rename_segment(s, "ch3-000007", "ch3"))),
    "kept text glued to a left-out channel": ("text-unremovable", _glued),
    "a backwards time range": ("time-range-invalid",
                               lambda t, p: _plain(t, p, time_range=(START + DAY, START))),
    "a time range outside the recording": (
        "time-range-empty", lambda t, p: _plain(t, p, time_range=(START - 2 * DAY, START - DAY))),
    "segment folder without a number": (
        "segment-folder-unnumbered", _mutated(lambda s: _rename_segment(s, "ch3-Jane", "ch3"))),
    # --- one per further code that can be decided up front ---------------
    "source missing": ("source-missing", _missing),
    "source not named .mefd": ("source-unreadable", _not_a_session),
    "metadata truncated": ("source-damaged", _mutated(_truncate("ch1.timd/*/*.tmet", 5000))),
    "metadata CRC": ("source-crc", _mutated(_flip("ch1.timd/*/*.tmet", 3000))),
    "session records ambiguous": ("records-ambiguous", _mutated(_two_foreign_record_files)),
    "wrong password": ("password-wrong", _mutated(lambda s: None, which="enc", password="nope")),
    "no password": ("password-required", _mutated(lambda s: None, which="enc")),
    "video channel": ("video-channels", _mutated(lambda s: (s / "cam.vidd").mkdir())),
    "incomplete segment": ("incomplete-segments",
                           _mutated(lambda s: next(s.glob("ch3.timd/*/*.tdat")).unlink())),
    "foreign record file": ("unread-record-files",
                            _mutated(lambda s: shutil.copy(s / "jane_roe.rdat", s / "x.rdat"))),
    "level-1 password: locked parts": ("parts-locked",
                                       _mutated(lambda s: None, which="enc", password="pw-one")),
    "level-1 password: offset unknown": ("rto-unreadable",
                                         _mutated(lambda s: None, which="enc", password="pw-one")),
    "level-1 password: records locked": ("record-locked",
                                         _mutated(lambda s: None, which="enc", password="pw-one")),
    "name too long": ("name-too-long", lambda t, p: _plain(t, p, pseudonym="x" * 300)),
    "equal output passwords": ("output-passwords-equal",
                               lambda t, p: _plain(t, p, output_password=("aa", "aa"))),
    "bad text policy": ("text-invalid", lambda t, p: _plain(t, p, text="redact")),
    "two folders, one number": ("segment-number-duplicate", _mutated(_duplicate_segment)),
    "record index names no record": ("record-index-mismatch", _mutated(_move_an_index_entry)),
    "a non-UTF-8 folder name": ("segment-folder-unnumbered", _mutated(_non_utf8_segment_folder)),
    "a body time the shift takes below 0": (
        "shift-not-positive",
        _mutated(_record_file_header_before_the_shift, shift=-(START - DAY // 2))),
}

# Refusals only the copy pass can meet: check() passes these sessions.
COPY_CASES = {
    "a damaged data block": ("block-crc", _mutated(_flip("ch3.timd/*/*.tdat", 1024 + 400))),
    "data file cut short": ("data-damaged", _mutated(_truncate("ch3.timd/*/*.tdat", -100))),
    "a channel named after the patient": (
        "verify-failed", _named("S001.mefd", channels=("Jane_C3", "C4"),
                                subject=mef3io.Subject(name_1="Jane"))),
}


def _run(build, templates, tmp_path):
    src, dst, kw = build(templates, tmp_path)
    shift = kw.pop("shift", SHIFT)
    return src, Path(dst), shift, kw


@pytest.mark.parametrize("case", list(CASES))
def test_check_agrees_with_anonymize_session(templates, tmp_path, case):
    code, build = CASES[case]
    src, dst, shift, kw = _run(build, templates, tmp_path)
    before = _tree(tmp_path)

    rep = _quiet(mef3io.anonymize.check, src, dst, shift, **kw)
    assert not rep.ok and code in rep.codes, rep.summary()
    assert _tree(tmp_path) == before, "check wrote something"

    with pytest.raises(Exception) as exc:
        _quiet(mef3io.anonymize_session, src, dst, shift, **kw)
    e = exc.value
    first = rep.refusals[0]
    assert getattr(e, "code", None) == first.code, (first, e)
    assert type(e).__name__ == first.error
    assert REFUSAL_CODES[first.code][0] == first.error
    if first.inspect_code:  # one exception lists every blocker
        assert f"{first.inspect_code}: {first.message}" in str(e)
    else:
        assert str(e) == first.message
    assert set(e.details) == {"channel", "field", "inspect_code"}
    assert e.details["channel"] == first.channel and e.details["field"] == first.field
    assert _tree(tmp_path) == before, "anonymize_session left something behind"
    # the re-identification key never reaches a message
    for text in [str(e)] + [r.message for r in rep.refusals]:
        assert not _source_times_in(text), text


# Codes that concern only a SHIFTED copy: one that keeps every time (no
# shift) copies each time and offset as stored, so none of them applies.
SHIFT_ONLY = {"shift-zero", "shift-below-floor", "shift-not-positive", "time-before-rto",
              "rto-disputed", "rto-unreadable", "id-number-time"}


@pytest.mark.parametrize("case", list(CASES) + list(COPY_CASES))
def test_check_agrees_without_a_shift(templates, tmp_path, case):
    """The default copy (no shift, every time kept) through BOTH paths: every
    refusal that is not about a shift is still made, and check() agrees with
    anonymize_session on the first one, or on a clean pass."""
    code, build = CASES[case] if case in CASES else COPY_CASES[case]
    src, dst, kw = build(templates, tmp_path)
    kw.pop("shift", None)
    dst = Path(dst)
    before = _tree(tmp_path)
    rep = _quiet(mef3io.anonymize.check, src, dst, **kw)
    assert _tree(tmp_path) == before, "check wrote something"
    if case in COPY_CASES or code in SHIFT_ONLY:
        assert code not in rep.codes, rep.summary()
    else:
        assert code in rep.codes, rep.summary()
    if rep.ok:
        if case in COPY_CASES:
            with pytest.raises(Exception) as exc:
                _quiet(mef3io.anonymize_session, src, dst, **kw)
            assert exc.value.code == code
            assert _tree(tmp_path) == before
        else:
            out = _quiet(mef3io.anonymize_session, src, dst, **kw)
            assert out.shift_us is None and out.verification.ok, out.verification.summary()
        return
    with pytest.raises(Exception) as exc:
        _quiet(mef3io.anonymize_session, src, dst, **kw)
    e, first = exc.value, rep.refusals[0]
    assert getattr(e, "code", None) == first.code, (first, e)
    assert type(e).__name__ == first.error
    if first.inspect_code:
        assert f"{first.inspect_code}: {first.message}" in str(e)
    else:
        assert str(e) == first.message
    assert _tree(tmp_path) == before, "anonymize_session left something behind"
    for text in [str(e)] + [r.message for r in rep.refusals]:
        assert not _source_times_in(text), text


@pytest.mark.parametrize("case", list(COPY_CASES))
def test_copy_pass_refusals_carry_codes(templates, tmp_path, case):
    code, build = COPY_CASES[case]
    src, dst, shift, kw = _run(build, templates, tmp_path)
    assert REFUSAL_CODES[code][1] == "copy"
    rep = _quiet(mef3io.anonymize.check, src, dst, shift, **kw)
    assert rep.ok, rep.summary()  # only the copy pass can tell
    before = _tree(tmp_path)
    with pytest.raises(Exception) as exc:
        _quiet(mef3io.anonymize_session, src, dst, shift, **kw)
    assert exc.value.code == code and type(exc.value).__name__ == REFUSAL_CODES[code][0]
    assert _tree(tmp_path) == before  # the partial copy is gone
    assert not _source_times_in(str(exc.value)), str(exc.value)


def test_verify_messages_name_no_source_time(templates, tmp_path):
    src = _fresh(templates, tmp_path)
    _quiet(mef3io.anonymize_session, src, tmp_path / "S001.mefd", SHIFT)
    rep = _quiet(mef3io.anonymize.verify, src, tmp_path / "S001.mefd", SHIFT + SECOND)
    assert not rep.ok and len(rep.problems) > 10   # every time is off by one second
    for i in rep.problems + rep.notes:
        assert not _source_times_in(i.field + " " + i.message), i
    for f in rep.inherited_findings:
        assert not _source_times_in(f), f


def test_verify_refuses_a_copy_that_lost_a_channels_samples(templates, tmp_path):
    """A kept data channel left with only its records in the copy is a loss."""
    src = _fresh(templates, tmp_path)
    dst = tmp_path / "S001.mefd"
    _quiet(mef3io.anonymize_session, src, dst, SHIFT)
    assert (dst / "ch1.timd" / "ch1.rdat").exists()
    for seg in (dst / "ch1.timd").glob("*.segd"):
        shutil.rmtree(seg)
    rep = _quiet(mef3io.anonymize.verify, src, dst, SHIFT)
    assert not rep.ok
    assert any(p.location == "ch1" and "has no segments in the copy; the source's has 2"
               in p.message for p in rep.problems), rep.summary()


def test_a_wrong_argument_type_has_a_code_in_both(templates, tmp_path):
    src = _fresh(templates, tmp_path)
    for fn in (mef3io.anonymize.check, mef3io.anonymize_session):
        with pytest.raises(TypeError) as exc:
            _quiet(fn, src, tmp_path / "S001.mefd", SHIFT, channels=[1])
        assert exc.value.code == "argument-type"


def test_every_code_is_documented_and_reached():
    text = DOCS.read_text(encoding="utf-8")
    rows = dict()
    for code, error, where in re.findall(r"^\| `([a-z0-9-]+)` \| `(\w+)` \| (\w+) \|", text,
                                         re.MULTILINE):
        rows[code] = (error, where)
    assert rows == REFUSAL_CODES, "docs/anonymization.md and REFUSAL_CODES disagree"
    reached = {c for c, _ in CASES.values()} | {c for c, _ in COPY_CASES.values()}
    reached.add("argument-type")
    for code, (_, where) in REFUSAL_CODES.items():
        if where == "defensive":
            assert code not in reached, f"{code} is reached: it is not defensive"
        else:
            assert code in reached, f"no test reaches {code}"
    for code, _ in CASES.values():
        assert REFUSAL_CODES[code][1] == "check"
    # The "(shift only)" markers in the table are exactly the codes a copy
    # that keeps the times never meets: SHIFT_ONLY, plus the defensive
    # time-overflow (only the shifting rule computes a time).
    marked = set(re.findall(r"^\| `([a-z0-9-]+)` \|[^\n]*\*\(shift only\)\*", text,
                            re.MULTILINE))
    assert marked == SHIFT_ONLY | {"time-overflow"}, marked


def test_check_lists_every_refusal_not_just_the_first(templates, tmp_path):
    src = _fresh(templates, tmp_path)
    (tmp_path / "S001.mefd").mkdir()
    rep = mef3io.anonymize.check(src, tmp_path / "S001.mefd", 3600 * SECOND,
                                 channels=["nope"], pseudonym="Jane")
    assert rep.codes == ("dst-exists", "channel-unknown", "no-channels-left", "shift-below-floor",
                         "pseudonym-contains-identity"), rep.summary()
    unknown = rep.refusals[1]
    assert unknown.channel == "nope" and unknown.field == "channels"
    assert unknown.error == "ValueError" and unknown.inspect_code is None
    # with the level-1 password: the password refusal and the blockers it causes
    rep = mef3io.anonymize.check(_fresh(templates, tmp_path / "e", "enc"), tmp_path / "S002.mefd",
                                 SHIFT, password="pw-one")
    assert rep.codes[:3] == ("password-level1-only", "parts-locked", "rto-unreadable")
    assert set(rep.codes[3:]) == {"record-locked"}, rep.summary()
    blocker = rep.refusals[1]
    assert blocker.inspect_code == "locked" and blocker.error == "WriteConflictError"


def test_a_clean_check_and_the_shift_left_open(templates, tmp_path):
    src = _fresh(templates, tmp_path)
    rep = mef3io.anonymize.check(src, tmp_path / "S001.mefd", SHIFT, exclude_channels=["ch2"],
                                 pseudonym="P-17", output_password=("a" * 20, "two"))
    assert rep.ok and rep.refusals == () and rep.codes == ()
    assert rep.session_name == "S001" and rep.pseudonym == "P-17"
    assert rep.channels_kept == ("ch1", "ch3") and rep.channels_excluded == ("ch2",)
    assert any("level-1 output password" in n for n in rep.notes)
    assert "nothing stops the copy" in rep.summary()
    # no shift: the copy that keeps the times; a bad shift is caught once given
    assert mef3io.anonymize.check(src, tmp_path / "S001.mefd").ok
    assert mef3io.anonymize.check(src, tmp_path / "S001.mefd", 0).codes == ("shift-zero",)
    assert not list(tmp_path.glob("S001*"))


def test_check_notes_a_recording_time_offset(tmp_path):
    src = _legacy(tmp_path / "rto.mefd", rto=START - 6 * 3600 * SECOND)
    rep = mef3io.anonymize.check(src, tmp_path / "R.mefd", SHIFT)
    assert rep.ok and any("recording-time offset" in n for n in rep.notes)


def test_check_honours_cancel(templates, tmp_path):
    token = mef3io.CancelToken()
    token.cancel()
    with pytest.raises(mef3io.Cancelled):
        mef3io.anonymize.check(_fresh(templates, tmp_path), tmp_path / "S001.mefd", SHIFT,
                               cancel=token)


def _rchar() -> int:
    for line in open("/proc/self/io"):
        if line.startswith("rchar"):
            return int(line.split()[1])
    raise RuntimeError("rchar not present in /proc/self/io")


@pytest.mark.skipif(not Path("/proc/self/io").exists(), reason="needs /proc/self/io")
def test_check_reads_metadata_only(tmp_path):
    """A session whose data dwarfs its metadata: check reads about what
    inspect reads, never the .tdat files."""
    src = tmp_path / "big.mefd"
    rng = np.random.default_rng(0)
    with mef3io.Writer(str(src), metadata=mef3io.Metadata(subject=mef3io.Subject(name_1="Jane"))) as w:
        for i in range(4):
            for ch in ("a", "b"):
                block = rng.integers(-(2**30), 2**30, int(FS * 3600), dtype=np.int32)
                w.write_int32(ch, block, 0.1, START + i * 3_600_000_000, FS, new_segment=True)
        w.write_annotations([{"type": "Note", "time": START + SECOND, "text": "x"}])
    tdat = sum(p.stat().st_size for p in src.rglob("*.tdat"))
    assert tdat > 20_000_000
    mef3io.inspect(str(src))  # warm imports and caches alike
    before = _rchar()
    mef3io.inspect(str(src))
    inspect_bytes = _rchar() - before
    before = _rchar()
    rep = mef3io.anonymize.check(src, tmp_path / "S001.mefd", SHIFT)
    check_bytes = _rchar() - before
    assert rep.ok
    assert check_bytes < tdat * 0.02, f"check read {check_bytes} bytes of {tdat} bytes of data"
    assert check_bytes <= 3 * inspect_bytes + 65536, (check_bytes, inspect_bytes)
    assert sorted(p.name for p in tmp_path.iterdir()) == ["big.mefd"]


def test_check_releases_the_gil(tmp_path):
    from test_p31_gil_release import _assert_releases

    src = tmp_path / "many.mefd"
    w = mef3io._mef3io.SessionWriter(str(src), True)
    x = np.arange(256, dtype=np.int32)
    for seg in range(1200):
        w.write_int32("ch", x, 1.0, START + seg * 10_000_000, 256.0, None, seg > 0)
    del w
    call = lambda: mef3io.anonymize.check(src, tmp_path / "S001.mefd", SHIFT)  # noqa: E731
    call()
    _assert_releases(lambda: call)


def test_the_cli_names_the_code(templates, tmp_path, capsys):
    from mef3io.__main__ import main

    src = _fresh(templates, tmp_path)
    assert main(["anonymize", str(src), str(tmp_path / "C.mefd"), "--shift-us", "5"]) == 2
    assert "error (shift-below-floor):" in capsys.readouterr().err


# --------------------------------------------------------------------------- original times


def test_the_report_carries_the_original_times_and_the_copy_does_not(templates, tmp_path):
    src = _fresh(templates, tmp_path)
    rep = _quiet(mef3io.anonymize_session, src, tmp_path / "S001.mefd", SHIFT,
                 exclude_channels=["ch2"])
    s = mef3io.inspect(str(src))
    assert rep.source_start_us == s.start_time == START
    assert rep.source_end_us == s.end_time
    by_name = {c.name: c for c in s.channels}
    assert set(rep.source_channel_times) == {"ch1", "ch3"}
    for ch, (a, b) in rep.source_channel_times.items():
        assert (a, b) == (by_name[ch].start_time, by_name[ch].end_time)
    # in memory only: not in repr, summary or the default dict
    key = {rep.source_start_us, rep.source_end_us}
    key |= {t for pair in rep.source_channel_times.values() for t in pair}
    shown = repr(rep) + rep.summary() + str(rep.to_dict())
    for t in key:
        assert str(t) not in shown
    # pickling drops them unless asked; copies keep them
    assert pickle.loads(pickle.dumps(rep)).source_start_us is None
    assert pickle.loads(pickle.dumps(rep)).source_channel_times == {}
    assert pickle.loads(pickle.dumps(rep)).verification == rep.verification
    kept = pickle.loads(pickle.dumps(rep.with_original_times_in_pickle()))
    assert kept.source_start_us == START and kept.source_channel_times == rep.source_channel_times
    assert pickle.loads(pickle.dumps(kept)).source_start_us == START   # the mark survives
    assert copy.copy(rep).source_start_us == START
    assert copy.deepcopy(rep).source_channel_times == rep.source_channel_times
    full = rep.to_dict(include_original_times=True)
    assert full["source_start_us"] == START and "ch1" in full["source_channel_times"]
    # ... and nowhere in the copy, plain or negated (the leak scan refuses it)
    assert rep.verification.ok and not rep.verification.leaks
    for p in (tmp_path / "S001.mefd").rglob("*"):
        if p.is_file():
            data = p.read_bytes()
            for t in key:
                for v in (t, -t):
                    assert struct.pack("<q", v) not in data, (p, v)


def test_a_record_only_channel_has_no_times(tmp_path):
    src = _named_session(tmp_path / "p.mefd", ("c1", "c2"), mef3io.Subject())
    with mef3io.Writer(str(src)) as w:
        w.write_annotations([{"type": "Note", "time": START + SECOND, "text": "x"}], "events")
    rep = _quiet(mef3io.anonymize_session, src, tmp_path / "S001.mefd", SHIFT)
    assert rep.source_channel_times["events"] == (None, None)
    assert rep.source_channel_times["c1"][0] == START


# --------------------------------------------------------------------------- password interop


@pytest.mark.parametrize("password, issue", [
    ("secret", None),
    ("fifteen-chars-x", None),
    ("a" * 16, "16 characters"),
    ("ééééééééé", "bytes long in UTF-8"),
    ("passΩ", "above U+00FF"),
])
def test_password_interop_issue(password, issue):
    got = mef3io.password_interop_issue(password)
    if issue is None:
        assert got is None
    else:
        assert issue in got and password not in got


def test_password_interop_issue_is_the_warning_rule(tmp_path):
    with pytest.warns(mef3io.PasswordInteropWarning) as rec:
        with mef3io.Writer(str(tmp_path / "w.mefd"), password1="a" * 20, password2="b") as w:
            w.write("c", _signal(256, 0), START, FS, precision=3)
    assert mef3io.password_interop_issue("a" * 20) in str(rec[0].message)
    with pytest.raises(TypeError):
        mef3io.password_interop_issue(b"bytes")
