"""#167: annotations in a password-protected session are never plaintext.

The session decides (its .tmet password fields), not which passwords the
Writer was given; the validator flags a record file protected differently.
"""
import hashlib
from pathlib import Path

import numpy as np
import pytest

import mef3io

START = 1577836800000000
S = 1_000_000
UH = 1024


def _session(path):
    with mef3io.Writer(str(path), overwrite=True, password1="p1", password2="p2") as w:
        w.write_int32("ch1", np.arange(2560, dtype=np.int32), 1.0, START, 256.0)
    return Path(path)


def _tree(path):
    return {
        str(f.relative_to(path)): hashlib.sha256(f.read_bytes()).hexdigest()
        for f in sorted(Path(path).rglob("*"))
        if f.is_file()
    } | {str(d.relative_to(path)): "dir" for d in Path(path).rglob("*") if d.is_dir()}


def _note(t, text):
    return {"time": t, "text": text}


@pytest.mark.parametrize("channel", [None, "ch1"])
@pytest.mark.parametrize("pw", [("", "p2"), ("p1", ""), ("", ""), ("p1", "wrong"), ("wrong", "p2"),
                                ("p2", "p2")])
def test_wrong_or_missing_password_is_refused_before_anything_is_created(tmp_path, channel, pw):
    p = _session(tmp_path / "e.mefd")
    before = _tree(p)
    with mef3io.Writer(str(p), password1=pw[0], password2=pw[1]) as w:
        with pytest.raises(mef3io.PasswordError, match="both passwords"):
            w.write_annotations([_note(START + S, "SECRETTEXT")], channel=channel)
    assert _tree(p) == before


def test_existing_notes_file_is_refused_too(tmp_path):
    p = _session(tmp_path / "e.mefd")
    with mef3io.Writer(str(p), password1="p1", password2="p2") as w:
        w.write_annotations([_note(START + S, "first")])
    before = _tree(p)
    with mef3io.Writer(str(p), password1="", password2="p2") as w:
        with pytest.raises(Exception):
            w.write_annotations([_note(START + 2 * S, "SECRETTEXT")])
    assert _tree(p) == before


@pytest.mark.parametrize("channel", [None, "ch1"])
def test_correct_passwords_write_encrypted_records(tmp_path, channel):
    p = _session(tmp_path / "e.mefd")
    with mef3io.Writer(str(p), password1="p1", password2="p2") as w:
        w.write_annotations([_note(START + S, "SECRETTEXT")], channel=channel)
    for f in p.rglob("*.rdat"):
        assert b"SECRETTEXT" not in f.read_bytes()
    with mef3io.Reader(str(p), password="p2") as r:
        got = [x["text"] for x in r.records(channel, **({} if channel else {"level": "session"}))]
    assert got == ["SECRETTEXT"]
    assert mef3io.Validator(str(p), password="p2").validate().ok


def test_correct_output_keeps_the_encrypted_layout(tmp_path):
    """Headers carry the validation fields, bodies are level-2 encrypted."""
    a = _session(tmp_path / "a.mefd")
    with mef3io.Writer(str(a), password1="p1", password2="p2") as w:
        w.write_annotations([_note(START + S, "SECRETTEXT")])
    raw = (a / "a.rdat").read_bytes()
    assert any(raw[868:900])  # password validation fields (UH @ 868 / 884)
    assert raw[UH + 11] == 2  # record encryption level byte


def test_unprotected_session_behaviour_unchanged(tmp_path):
    p = tmp_path / "u.mefd"
    with mef3io.Writer(str(p), overwrite=True) as w:
        w.write_int32("ch1", np.arange(2560, dtype=np.int32), 1.0, START, 256.0)
    with mef3io.Writer(str(p)) as w:
        w.write_annotations([_note(START + S, "plain note")])
    assert b"plain note" in (p / "u.rdat").read_bytes()


def _plaintext_in_encrypted(tmp_path):
    """Craft the #167 state: an unprotected record file in an encrypted session."""
    p = _session(tmp_path / "e.mefd")
    q = tmp_path / "plain.mefd"
    with mef3io.Writer(str(q), overwrite=True) as w:
        w.write_int32("ch1", np.arange(2560, dtype=np.int32), 1.0, START, 256.0)
        w.write_annotations([_note(START + S, "SECRETTEXT")])
    (q / "plain.rdat").rename(p / "e.rdat")
    (q / "plain.ridx").rename(p / "e.ridx")
    return p


def test_validator_flags_mixed_record_encryption(tmp_path):
    p = _plaintext_in_encrypted(tmp_path)
    rep = mef3io.Validator(str(p), password="p2").validate()
    hits = [f for f in rep.findings if f.check_id == "records.encryption"]
    assert len(hits) == 1 and hits[0].severity == "error"
    assert not rep.ok
    assert not mef3io.Validator.describe_check("records.encryption").repairable
    with pytest.raises(ValueError, match="not repairable"):
        mef3io.repair_session(str(p), ["records.encryption"], password="p2")


def test_validator_is_quiet_for_consistent_sessions(tmp_path):
    p = _session(tmp_path / "e.mefd")
    with mef3io.Writer(str(p), password1="p1", password2="p2") as w:
        w.write_annotations([_note(START + S, "a")])
        w.write_annotations([_note(START + S, "b")], channel="ch1")
    assert not [f for f in mef3io.Validator(str(p), password="p2").validate().findings
                if f.check_id == "records.encryption"]
