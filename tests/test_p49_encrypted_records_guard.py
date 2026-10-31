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


# ---- review round 1 -----------------------------------------------------------

import shutil  # noqa: E402


def _plain_session(path, channel="aa", note=None):
    with mef3io.Writer(str(path), overwrite=True) as w:
        w.write_int32(channel, np.arange(2560, dtype=np.int32), 1.0, START, 256.0)
        if note:
            w.write_annotations([_note(START + S, note)])
    return Path(path)


def _mixed(tmp_path, plain_first=True):
    """Channels 'aa' and 'zz': one unprotected, one protected."""
    names = ("aa", "zz") if plain_first else ("zz", "aa")
    plain, prot = names
    p = tmp_path / "m.mefd"
    with mef3io.Writer(str(p), overwrite=True, password1="p1", password2="p2") as w:
        w.write_int32(prot, np.arange(2560, dtype=np.int32), 1.0, START, 256.0)
    q = _plain_session(tmp_path / "q.mefd", channel=plain)
    shutil.copytree(q / f"{plain}.timd", p / f"{plain}.timd")
    return p, plain, prot


@pytest.mark.parametrize("plain_first", [True, False])
def test_mixed_session_is_judged_per_channel_and_any_protected_at_session_level(tmp_path, plain_first):
    p, plain, prot = _mixed(tmp_path, plain_first)
    n = lambda txt: [_note(START + S, txt)]  # noqa: E731
    before = _tree(p)
    with mef3io.Writer(str(p)) as w:  # no passwords
        with pytest.raises(mef3io.PasswordError):
            w.write_annotations(n("S"))  # session level: some channel is protected
        with pytest.raises(mef3io.PasswordError):
            w.write_annotations(n("S"), channel=prot)
        w.write_annotations(n("PLAINNOTE"), channel=plain)  # its own tmet is unprotected
    assert b"PLAINNOTE" in (p / f"{plain}.timd" / f"{plain}.rdat").read_bytes()
    with mef3io.Writer(str(p), password1="p1", password2="p2") as w:
        with pytest.raises(mef3io.PasswordError, match="no passwords"):
            w.write_annotations(n("S"), channel=plain)
        w.write_annotations(n("SECRETTEXT"), channel=prot)
        w.write_annotations(n("SECRETSESSION"))
    assert b"SECRET" not in (p / f"{prot}.timd" / f"{prot}.rdat").read_bytes()
    assert b"SECRET" not in (p / "m.rdat").read_bytes()
    assert before != _tree(p)
    rep = mef3io.Validator(str(p), password="p2").validate()
    assert not [f for f in rep.findings if f.check_id == "records.encryption"]


def test_unprotected_session_refuses_passwords(tmp_path):
    p = _plain_session(tmp_path / "u.mefd")
    before = _tree(p)
    with mef3io.Writer(str(p), password1="p1", password2="p2") as w:
        for ch in (None, "aa"):
            with pytest.raises(mef3io.PasswordError, match="no passwords"):
                w.write_annotations([_note(START + S, "x")], channel=ch)
    assert _tree(p) == before


def test_new_encrypted_session_notes_before_and_after_data(tmp_path):
    p = tmp_path / "n.mefd"
    with mef3io.Writer(str(p), overwrite=True, password1="p1", password2="p2") as w:
        w.write_annotations([_note(START + S, "BEFORE")])
        w.write_int32("ch1", np.arange(2560, dtype=np.int32), 1.0, START, 256.0)
        w.write_annotations([_note(START + 2 * S, "AFTER")])
        w.write_annotations([_note(START + 3 * S, "CHAN")], channel="ch1")
    for f in p.rglob("*.rdat"):
        assert b"BEFORE" not in f.read_bytes() and b"AFTER" not in f.read_bytes() and b"CHAN" not in f.read_bytes()
    assert mef3io.Validator(str(p), password="p2").validate().ok


@pytest.mark.parametrize("pw", [("", "p2"), ("p1", "")])
def test_new_session_with_one_password_is_refused_up_front(tmp_path, pw):
    p = tmp_path / "n.mefd"
    with mef3io.Writer(str(p), overwrite=True, password1=pw[0], password2=pw[1]) as w:
        with pytest.raises(mef3io.PasswordError, match="both"):
            w.write_annotations([_note(START + S, "x")])


def test_guard_scans_the_session_once_not_on_every_call(tmp_path):
    """Work per annotation call must not grow with the number of segments."""

    def syscalls_per_call(nseg):
        p = tmp_path / f"seg{nseg}.mefd"
        with mef3io.Writer(str(p), overwrite=True, password1="p1", password2="p2") as w:
            for i in range(nseg):
                w.write_int32("ch1", np.arange(256, dtype=np.int32), 1.0, START + i * 10 * S, 256.0,
                              new_segment=True)

        def rd():
            for line in open("/proc/self/io"):
                if line.startswith("syscr"):
                    return int(line.split()[1])

        with mef3io.Writer(str(p), password1="p1", password2="p2") as w:
            w.write_annotations([_note(START + S, "first")])  # may scan once
            w.write_annotations([_note(START + S, "second")], channel="ch1")
            a = rd()
            for i in range(10):
                w.write_annotations([_note(START + 2 * S + i, "n")])
                w.write_annotations([_note(START + 2 * S + i, "c")], channel="ch1")
            return (rd() - a) / 20

    small, big = syscalls_per_call(3), syscalls_per_call(150)
    assert big <= small + 3, (small, big)


def test_session_level_finding_has_no_misleading_channel_or_segment(tmp_path):
    p = _plaintext_in_encrypted(tmp_path)
    f = [x for x in mef3io.Validator(str(p), password="p2").validate().findings
         if x.check_id == "records.encryption"][0]
    assert f.channel == "" and f.segment == -1 and f.path.endswith(".rdat")


def test_validator_judges_session_records_against_the_session_not_one_channel(tmp_path):
    p, plain, prot = _mixed(tmp_path, plain_first=True)  # first channel (aa) is plain
    with mef3io.Writer(str(p), password1="p1", password2="p2") as w:
        w.write_annotations([_note(START + S, "SECRETSESSION")])
    rep = mef3io.Validator(str(p), password="p2").validate()
    assert not [f for f in rep.findings if f.check_id == "records.encryption"]


def test_validator_visits_records_only_channels(tmp_path):
    p = _session(tmp_path / "e.mefd")
    q = _plain_session(tmp_path / "q.mefd", channel="ghost", note="SECRETTEXT")
    (p / "ghost.timd").mkdir()
    shutil.copy(q / "q.rdat", p / "ghost.timd" / "ghost.rdat")
    shutil.copy(q / "q.ridx", p / "ghost.timd" / "ghost.ridx")
    hits = [f for f in mef3io.Validator(str(p), password="p2").validate().findings
            if f.check_id == "records.encryption"]
    assert len(hits) == 1 and hits[0].channel == "ghost" and hits[0].segment == -1


def test_validator_warns_on_legacy_unencrypted_bodies_in_a_protected_file(tmp_path):
    p = _session(tmp_path / "e.mefd")
    with mef3io.Writer(str(p), password1="p1", password2="p2") as w:
        w.write_annotations([_note(START + S, "a"), _note(START + 2 * S, "b")])
    ridx = p / "e.ridx"
    b = bytearray(ridx.read_bytes())
    for o in range(UH, len(b), 24):
        b[o + 7] = 0
    ridx.write_bytes(bytes(b))
    hits = [f for f in mef3io.Validator(str(p), password="p2").validate().findings
            if f.check_id == "records.encryption"]
    assert len(hits) == 1 and hits[0].severity == "warning"
    assert "without encryption" in hits[0].message
