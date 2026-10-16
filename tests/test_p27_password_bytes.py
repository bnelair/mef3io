"""#84 — password -> key bytes: meflib's rule on write, legacy fallback on read.

meflib keys each password character by the LOW BYTE OF ITS UNICODE CODE POINT
(extract_terminal_password_bytes, meflib.c:2439). mef3io 1.2.0 and earlier kept the
LAST UTF-8 BYTE of each character instead. Both give the same bytes for ASCII
and different bytes for everything else, so a non-ASCII password set by one
stack was rejected by the other. (Oracle cases, both directions, live in
test_p15_oracle_acceptance.py; Catch2 golden bytes in core/tests/
test_password.cpp.)

Sessions an older mef3io encrypted with a non-ASCII password are reproduced
EXACTLY by ``_old_mef3io_session``: writing with passwords whose meflib bytes
ARE the old bytes ("bé" was keyed 62 A9, which is meflib's key for "b©") gives
the same validation fields and the same keys. That equivalence was checked
against a session written by the released mef3io 1.1.3 wheel (identical
universal-header validation fields), so no binary fixture is committed.
"""
from __future__ import annotations

import hashlib
import shutil
import warnings

import numpy as np
import pytest

import mef3io
from mef3io import _mef3io as m

START = 1577836800000000
FS = 250.0
PW1, PW2 = "bé", "Ünïcödé"


def _old_bytes_as_password(pw: str) -> str:
    """A password whose meflib key equals the key mef3io 1.2.0 and earlier derived from ``pw``."""
    legacy = m.extract_password_bytes_legacy(pw)
    out = "".join(chr(b) for b in legacy.rstrip(b"\0"))
    assert m.extract_password_bytes(out) == legacy
    return out


def _write(path, pw1, pw2, x, **kw):
    w = mef3io.Writer(str(path), password1=pw1, password2=pw2, **kw)
    w.write_int32("ch1", x, 0.5, START, FS)
    w.close()


def _old_mef3io_session(path, x):
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", mef3io.PasswordInteropWarning)
        _write(path, _old_bytes_as_password(PW1), _old_bytes_as_password(PW2), x)


@pytest.fixture
def x():
    return np.random.default_rng(84).integers(-5000, 5000, 2000, dtype=np.int32)


# --- the mapping itself --------------------------------------------------------


@pytest.mark.parametrize("pw", ["a", "pass1", "lvl2", "~!@#$%^&*()_+{}|", "0123456789abcdef",
                                "0123456789abcdefXYZ", " spaces in it "])
def test_ascii_passwords_map_identically_under_both_rules(pw):
    assert m.extract_password_bytes(pw) == m.extract_password_bytes_legacy(pw)


@pytest.mark.parametrize("pw,meflib,old", [
    ("bé", "62e9", "62a9"),
    ("ÿ", "ff", "bf"),             # Latin-1 boundary, U+00FF
    ("Ā", "00", "80"),             # U+0100: low byte 0x00, the same as the padding
    ("Łx", "4178", "8178"),        # U+0141
])
def test_non_ascii_passwords_use_the_code_point_low_byte(pw, meflib, old):
    assert m.extract_password_bytes(pw)[: len(meflib) // 2].hex() == meflib
    assert m.extract_password_bytes_legacy(pw)[: len(old) // 2].hex() == old


def test_characters_above_latin1_collide_exactly_as_in_meflib():
    # Only the low byte counts: U+0141 "Ł" and U+0041 "A" give the same key.
    assert m.extract_password_bytes("Łx") == m.extract_password_bytes("Ax")


# --- reading: meflib first, the old rule only as a fallback --------------------


def test_an_ascii_session_never_takes_the_fallback(tmp_path, x):
    path = tmp_path / "ascii.mefd"
    _write(path, "lvl1", "lvl2", x)
    with warnings.catch_warnings():
        warnings.simplefilter("error")
        with mef3io.Reader(str(path), password="lvl2") as r:
            np.testing.assert_array_equal(r.read("ch1"), x * 0.5)


def test_a_new_non_ascii_session_opens_without_the_fallback(tmp_path, x):
    path = tmp_path / "new.mefd"
    _write(path, PW1, PW2, x)
    with warnings.catch_warnings():
        warnings.simplefilter("error", mef3io.LegacyPasswordWarning)
        for pw in (PW2, PW1):
            with mef3io.Reader(str(path), password=pw) as r:
                np.testing.assert_array_equal(r.read("ch1"), x * 0.5)


def test_an_old_mef3io_session_opens_through_the_fallback_and_warns(tmp_path, x):
    path = tmp_path / "old.mefd"
    _old_mef3io_session(path, x)
    for pw in (PW2, PW1):  # level 2, level 1
        with pytest.warns(mef3io.LegacyPasswordWarning, match="up to 1.2.0"):
            r = mef3io.Reader(str(path), password=pw)
        with r:
            np.testing.assert_array_equal(r.read("ch1"), x * 0.5)


def test_the_fallback_is_not_a_back_door(tmp_path, x):
    path = tmp_path / "old.mefd"
    _old_mef3io_session(path, x)
    for wrong in ("bè", "be", "Ünïcödè", ""):
        with pytest.raises(RuntimeError):
            with mef3io.Reader(str(path), password=wrong) as r:
                r.read("ch1")


def test_the_legacy_drop_in_warns_too(tmp_path, x):
    path = tmp_path / "old.mefd"
    _old_mef3io_session(path, x)
    with pytest.warns(mef3io.LegacyPasswordWarning):
        mef3io.MefReader(str(path), password2=PW2)


def test_the_validator_and_records_read_an_old_session(tmp_path, x):
    path = tmp_path / "old.mefd"
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", mef3io.PasswordInteropWarning)
        w = mef3io.Writer(str(path), password1=_old_bytes_as_password(PW1),
                          password2=_old_bytes_as_password(PW2))
        w.write_int32("ch1", x, 0.5, START, FS)
        w.write_annotations([{"time": START + 1_000_000, "text": "héllo"}])
        w.close()
    report = mef3io.validate_session(str(path), password=PW2)
    assert not any("password" in f.message.lower() for f in report.findings)
    with pytest.warns(mef3io.LegacyPasswordWarning):
        r = mef3io.Reader(str(path), password=PW2)
    with r:
        with pytest.warns(mef3io.LegacyPasswordWarning, match="record file"):
            assert [a["text"] for a in r.records()] == ["héllo"]


def test_old_record_files_warn_even_when_the_metadata_is_new(tmp_path, x):
    # A session whose .tmet files are keyed the meflib way but whose .rdat is
    # not (records written separately by an older mef3io): the open is quiet,
    # records() is the only place that can say so.
    old = tmp_path / "old.mefd"
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", mef3io.PasswordInteropWarning)
        w = mef3io.Writer(str(old), password1=_old_bytes_as_password(PW1),
                          password2=_old_bytes_as_password(PW2))
        w.write_int32("ch1", x, 0.5, START, FS)
        w.write_annotations([{"time": START + 1_000_000, "text": "old"}])
        w.close()
    path = tmp_path / "mixed.mefd"
    _write(path, PW1, PW2, x)
    for ext in (".rdat", ".ridx"):
        (path / f"mixed{ext}").write_bytes((old / f"old{ext}").read_bytes())
    with warnings.catch_warnings():
        warnings.simplefilter("error", mef3io.LegacyPasswordWarning)
        r = mef3io.Reader(str(path), password=PW2)
    with r:
        with pytest.warns(mef3io.LegacyPasswordWarning, match="1 record file") as rec:
            assert [a["text"] for a in r.records()] == ["old"]
    assert rec[0].filename == __file__  # points at the caller
    with pytest.warns(mef3io.LegacyPasswordWarning, match="1 record file") as rec:
        anns = mef3io.MefReader(str(path), password2=PW2).get_annotations()
    assert [a["text"] for a in anns] == ["old"]
    assert rec[0].filename == __file__
    with warnings.catch_warnings():  # -W error must surface, not read as a failed read
        warnings.simplefilter("error", mef3io.LegacyPasswordWarning)
        with pytest.raises(mef3io.LegacyPasswordWarning):
            with warnings.catch_warnings():
                warnings.simplefilter("ignore", mef3io.LegacyPasswordWarning)
                reader = mef3io.MefReader(str(path), password2=PW2)
            reader.get_annotations()


# --- writing: always meflib's rule ------------------------------------------------


def test_appending_to_an_old_session_keeps_its_keys_and_warns(tmp_path, x):
    path = tmp_path / "old.mefd"
    _old_mef3io_session(path, x)
    end = START + int(len(x) / FS * 1e6)
    w = mef3io.Writer(str(path), password1=PW1, password2=PW2)
    with pytest.warns(mef3io.LegacyPasswordWarning, match="appended"):
        w.write_int32("ch1", x, 0.5, end, FS)               # in-segment: old keys
    with warnings.catch_warnings():
        warnings.simplefilter("error", mef3io.LegacyPasswordWarning)
        w.write_int32("ch1", x, 0.5, 2 * end - START, FS, new_segment=True)  # meflib keys
    w.close()

    with pytest.warns(mef3io.LegacyPasswordWarning):
        r = mef3io.Reader(str(path), password=PW2)
    with r:
        np.testing.assert_array_equal(r.read("ch1"), np.tile(x, 3) * 0.5)
    tmet = lambda seg: (path / "ch1.timd" / f"ch1-{seg:06d}.segd" / f"ch1-{seg:06d}.tmet").read_bytes()
    l1_field = lambda b: b[868:884]
    old_l1 = m.extract_password_bytes_legacy(PW1)
    new_l1 = m.extract_password_bytes(PW1)
    assert l1_field(tmet(0)) == hashlib.sha256(old_l1).digest()[:16]
    assert l1_field(tmet(1)) == hashlib.sha256(new_l1).digest()[:16]


@pytest.mark.parametrize("pw,what", [
    ("a" * 16, "16 characters"),
    ("Ł" + "x", r"U\+00FF"),
    ("é" * 8, "16 bytes"),
])
def test_the_writer_warns_about_passwords_other_readers_handle_differently(tmp_path, x, pw, what):
    with pytest.warns(mef3io.PasswordInteropWarning, match=what):
        _write(tmp_path / "w.mefd", pw, "lvl2", x)
    with mef3io.Reader(str(tmp_path / "w.mefd"), password=pw) as r:
        np.testing.assert_array_equal(r.read("ch1"), x * 0.5)


@pytest.mark.parametrize("pw", ["abc", "bé", "Ünïcödé", "ÿ" * 7, "a" * 15])
def test_the_writer_is_silent_for_passwords_every_reader_accepts(tmp_path, x, pw):
    with warnings.catch_warnings():
        warnings.simplefilter("error", mef3io.PasswordInteropWarning)
        _write(tmp_path / "w.mefd", pw, "lvl2", x)



def test_a_damaged_old_record_file_is_a_problem_not_a_legacy_read(tmp_path, x):
    old = tmp_path / "old.mefd"
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", mef3io.PasswordInteropWarning)
        w = mef3io.Writer(str(old), password1=_old_bytes_as_password(PW1),
                          password2=_old_bytes_as_password(PW2))
        w.write_int32("ch1", x, 0.5, START, FS)
        w.write_annotations([{"time": START + 1_000_000, "text": "old"}])
        w.close()
    rdat = old / "old.rdat"
    b = bytearray(rdat.read_bytes())
    b[1024 + 24 + 2] ^= 0xFF  # inside the first record body: its CRC fails
    rdat.write_bytes(bytes(b))
    with pytest.warns(mef3io.LegacyPasswordWarning):  # the metadata, on open
        r = mef3io.Reader(str(old), password=PW2, strict=False)
    with r:
        with warnings.catch_warnings(record=True) as rec:
            warnings.simplefilter("always")
            r.records()
        assert [p["kind"] for p in r.problems] == ["records"]
    assert not [w for w in rec if issubclass(w.category, mef3io.LegacyPasswordWarning)]


# --- inspect(): the same condition, reported as data ----------------------------


def test_inspect_flags_an_old_mef3io_session_without_warning(tmp_path, x):
    """inspect() accepts the fallback like the Reader, and REPORTS it (a flag
    and a "legacy-password" entry) instead of emitting LegacyPasswordWarning,
    as it reports every other condition. Studio gap #96."""
    path = tmp_path / "old.mefd"
    _old_mef3io_session(path, x)
    for pw in (PW2, PW1):  # level 2, level 1
        with warnings.catch_warnings():
            warnings.simplefilter("error", mef3io.LegacyPasswordWarning)
            s = mef3io.inspect(str(path), pw)
        assert s.legacy_password_mapping is True
        hit = [w for w in s.warnings if w.code == "legacy-password"]
        assert len(hit) == 1 and "1 channel(s) (ch1)" in hit[0].message
        assert not hit[0].blocks_anonymize
        assert s.to_dict()["legacy_password_mapping"] is True


def test_inspect_does_not_flag_meflib_keyed_or_plain_sessions(tmp_path, x):
    new = tmp_path / "new.mefd"
    _write(new, PW1, PW2, x)
    plain = tmp_path / "plain.mefd"
    _write(plain, "", "", x)
    for path, pw in ((new, PW2), (new, PW1), (new, ""), (plain, "")):
        s = mef3io.inspect(str(path), pw)
        assert s.legacy_password_mapping is False
        assert "legacy-password" not in {w.code for w in s.warnings}


def test_inspect_flags_old_record_files_under_new_metadata(tmp_path, x):
    """Only the record file is old-keyed: the Reader can only say so from
    records(); inspect reads the record files, so it says so up front."""
    old = tmp_path / "old.mefd"
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", mef3io.PasswordInteropWarning)
        w = mef3io.Writer(str(old), password1=_old_bytes_as_password(PW1),
                          password2=_old_bytes_as_password(PW2))
        w.write_int32("ch1", x, 0.5, START, FS)
        w.write_annotations([{"time": START + 1_000_000, "text": "old"}])
        w.close()
    path = tmp_path / "mixed.mefd"
    _write(path, PW1, PW2, x)
    for ext in (".rdat", ".ridx"):
        (path / f"mixed{ext}").write_bytes((old / f"old{ext}").read_bytes())
    s = mef3io.inspect(str(path), PW2)
    assert s.legacy_password_mapping is True
    (hit,) = [w for w in s.warnings if w.code == "legacy-password"]
    assert "1 record file(s)" in hit.message and "channel" not in hit.message


def test_inspect_flags_a_channel_with_one_old_segment_among_new_ones(tmp_path, x):
    """An old-keyed segment, then a new segment keyed the meflib way (an
    append with new_segment=True): one old segment is enough for the flag."""
    path = tmp_path / "mixed_segments.mefd"
    _old_mef3io_session(path, x)
    end = START + int(len(x) / FS * 1e6)
    with warnings.catch_warnings():  # the writer's own warning is pinned above
        warnings.simplefilter("ignore", mef3io.LegacyPasswordWarning)
        with mef3io.Writer(str(path), password1=PW1, password2=PW2) as w:
            w.write_int32("ch1", x, 0.5, end + 1_000_000, FS, new_segment=True)
    s = mef3io.inspect(str(path), PW2)
    assert len(s.channel("ch1").segments) == 2
    assert s.legacy_password_mapping is True
    (hit,) = [w for w in s.warnings if w.code == "legacy-password"]
    assert "1 channel(s) (ch1)" in hit.message
    # the meflib-keyed segment alone is not flagged
    shutil.rmtree(next(path.glob("ch1.timd/ch1-000000.segd")))
    assert mef3io.inspect(str(path), PW2).legacy_password_mapping is False
