"""#72 (second part) — rename_session, fix_name_drift and the operation marker.

* a rename changes all four places that carry the name, and only them: every
  other byte of every file (samples, index, times) is unchanged; pymef finds
  the session's annotations afterwards, where a folder-only rename loses them;
* an undo after an append is exact (the append's bytes are kept, only the
  descriptions the rename changed are reverted);
* a crash at every point of the operation is rolled back by the next call;
  a journal without a marker is discarded, a marker without a journal
  refuses writes, and clear_operation_marker only clears a consistent one;
* every write path refuses a session mid-operation, and the Reader warns;
* fix_name_drift repairs a folder renamed by another tool, refuses an
  ambiguous old name, and its undo leaves the folder alone.
"""
from __future__ import annotations

import os
import shutil
import warnings
from pathlib import Path

import numpy as np
import pytest

import mef3io

from test_p29_anonymize import START, SECOND, _legacy

UH = 1024
NAME = slice(308, 308 + 256)
REGION = slice(960, 1024)
S2 = 2560
S2_DESC = slice(S2 + 2048, S2 + 4096)


def _session(path, segments=2, channels=("A", "B"), password=False, description=None):
    kw = dict(password1="pw-one", password2="pw-two") if password else {}
    if description is not None:
        kw["metadata"] = mef3io.Metadata(
            acquisition=mef3io.Acquisition(session_description=description))
    with mef3io.Writer(str(path), overwrite=True, **kw) as w:
        for k in range(segments):
            for ch in channels:
                w.write(ch, np.arange(2560) * 1.0, START + k * 60 * SECOND, 256.0, precision=0,
                        new_segment=k > 0)
        w.write_annotations([{"type": "Note", "time": START + SECOND, "text": "night one"}])
    return Path(path)


def _files(root):
    """Every file with a universal header, keyed by its path with the
    session-level record files' name taken out."""
    root = Path(root)
    out = {}
    for p in sorted(root.rglob("*")):
        if p.suffix in (".tmet", ".tidx", ".tdat", ".rdat", ".ridx"):
            rel = p.relative_to(root).as_posix()
            if "/" not in rel:
                rel = "<session>" + p.suffix
            out[rel] = p.read_bytes()
    return out


def _masked(rel, b):
    """The bytes a rename may change, blanked: the header CRC, the name, the
    provenance region and, in a .tmet, the description and its body CRC."""
    b = bytearray(b)
    b[0:8 if rel.endswith(".tmet") else 4] = bytes(8 if rel.endswith(".tmet") else 4)
    b[NAME] = bytes(256)
    b[REGION] = bytes(64)
    return bytes(b)


def _assert_only_names_changed(before, after):
    """Encrypted too: section 2 is AES-ECB and the description is
    block-aligned in it, so only its own ciphertext blocks change."""
    assert before.keys() == after.keys()
    for rel in before:
        a, b = _masked(rel, before[rel]), _masked(rel, after[rel])
        if rel.endswith(".tmet"):
            a, b = bytearray(a), bytearray(b)
            a[S2_DESC] = b[S2_DESC] = bytes(2048)
        assert bytes(a) == bytes(b), rel


def _stored_names(root):
    return {bytes(b[NAME]).split(b"\0")[0].decode() for b in _files(root).values()}


def _clean(path, **kw):
    r = mef3io.validate_session(str(path), **kw)
    assert r.ok, r.summary()


# --- rename ------------------------------------------------------------------

def test_rename_changes_the_four_places_and_nothing_else(tmp_path):
    src = _session(tmp_path / "pt_a1b2.mefd")
    before = _files(src)
    rep = mef3io.rename_session(src, "pt1")
    dst = tmp_path / "pt1.mefd"
    assert rep.new_path == str(dst) and dst.is_dir() and not src.exists()
    assert sorted(p.name for p in dst.iterdir() if p.is_file()) == ["pt1.rdat", "pt1.ridx"]
    assert _stored_names(dst) == {"pt1"}
    assert mef3io.name_drift(dst).consistent
    _assert_only_names_changed(before, _files(dst))
    with mef3io.Reader(str(dst)) as r:
        assert r.metadata.acquisition.session_description == "pt1"
    # Every file changed is stamped "rename"; the backups are gone.
    assert len(rep.files_changed) == len(before)
    with mef3io.Reader(str(dst)) as r:
        prov = r.segments("A")[0]["provenance"]
        assert prov["last_operation"] == "rename"
    assert not any(p.name.endswith(".repair-backup") for p in tmp_path.iterdir())
    _clean(dst)


def test_a_description_that_only_mentions_the_name_is_kept(tmp_path):
    src = _session(tmp_path / "jd.mefd", description="jd night 2")
    rep = mef3io.rename_session(src, "pt1")
    assert rep.descriptions_changed == ()
    with mef3io.Reader(rep.new_path) as r:
        assert r.metadata.acquisition.session_description == "jd night 2"


def test_pymef_finds_the_annotations_after_a_rename_but_not_after_a_folder_rename(tmp_path):
    pytest.importorskip("pymef")
    from pymef.mef_session import MefSession

    def session_notes(path):
        s = MefSession(str(path), "")
        try:
            return [r for r in s.read_records() if r.get("type") == "Note"]
        except KeyError:  # pymef: no session-level records found at all
            return []
        finally:
            s.close()

    a = _session(tmp_path / "a.mefd")
    shutil.copytree(a, tmp_path / "b.mefd")
    os.rename(tmp_path / "b.mefd", tmp_path / "moved.mefd")  # another tool's rename
    assert session_notes(tmp_path / "moved.mefd") == []  # the regression #72 exists for
    rep = mef3io.rename_session(a, "renamed")
    assert len(session_notes(rep.new_path)) == 1


def test_undo_after_an_append_keeps_the_append(tmp_path):
    src = _session(tmp_path / "a.mefd", description="a")
    rep = mef3io.rename_session(src, "b")
    with mef3io.Writer(rep.new_path) as w:
        w.write("A", np.arange(2560) * 1.0, START + 5 * 60 * SECOND, 256.0, precision=0)
    appended = _files(rep.new_path)
    back = rep.undo()
    assert back.new_path == str(src) and _stored_names(src) == {"a"}
    assert back.descriptions_changed == rep.descriptions_changed
    _assert_only_names_changed(appended, _files(src))
    with mef3io.Reader(str(src)) as r:
        assert r.metadata.acquisition.session_description == "a"
    _clean(src)


def test_the_undo_reverts_only_what_the_rename_changed(tmp_path):
    # The description already read "b" before a -> b: the rename leaves it
    # (it is not the old name), so the undo must leave it too, although it
    # now equals the name being undone.
    src = _session(tmp_path / "a.mefd", description="b")
    rep = mef3io.rename_session(src, "b")
    assert rep.descriptions_changed == ()
    rep.undo()
    with mef3io.Reader(str(src)) as r:
        assert r.metadata.acquisition.session_description == "b"


def test_encrypted_needs_the_level_1_password(tmp_path):
    src = _session(tmp_path / "a.mefd", password=True)
    before = _files(src)
    with pytest.raises(mef3io.PasswordError):
        mef3io.rename_session(src, "b")
    assert _files(src) == before and src.is_dir()
    assert mef3io.operation_marker(src) is None
    rep = mef3io.rename_session(src, "b", password="pw-one")
    assert len(rep.descriptions_changed) == 4
    _assert_only_names_changed(before, _files(rep.new_path))
    with mef3io.Reader(rep.new_path, password="pw-two") as r:
        assert r.metadata.acquisition.session_description == "b"
    _clean(rep.new_path, password="pw-two")


def test_a_legacy_session(tmp_path):
    src = _legacy(tmp_path / "legacy.mefd")
    before = _files(src)
    rep = mef3io.rename_session(src, "pt7")
    _assert_only_names_changed(before, _files(rep.new_path))
    assert mef3io.name_drift(rep.new_path).consistent


@pytest.mark.parametrize("name, why", [
    ("", "empty"), ("a/b", "'/'"), ("a\0b", "control"), ("CON", "device"), ("x.", "dot"),
    ("é" * 126, "250"),  # 252 bytes
])
def test_names_refused(tmp_path, name, why):
    src = _session(tmp_path / "a.mefd", segments=1, channels=("A",))
    with pytest.raises(ValueError, match=why):
        mef3io.rename_session(src, name)
    assert src.is_dir() and _stored_names(src) == {"a"}


def test_name_length_limit(tmp_path):
    src = _session(tmp_path / "a.mefd", segments=1, channels=("A",))
    with pytest.raises(ValueError):
        mef3io.rename_session(src, "x" * 251)
    rep = mef3io.rename_session(src, "x" * 250)
    assert _stored_names(rep.new_path) == {"x" * 250}


def test_refusals(tmp_path):
    src = _session(tmp_path / "a.mefd", segments=1, channels=("A",))
    _session(tmp_path / "b.mefd", segments=1, channels=("A",))
    with pytest.raises(mef3io.IoError, match="exists"):
        mef3io.rename_session(src, "b")
    with pytest.raises(ValueError, match="already named"):
        mef3io.rename_session(src, "a")
    tar = mef3io.archive_session(str(src))
    with pytest.raises(ValueError, match="rename_archive"):
        mef3io.rename_session(tar, "c")
    (tmp_path / "c.mefd.partial").mkdir()
    with pytest.raises(mef3io.WriteConflictError, match="partial"):
        mef3io.rename_session(src, "c")
    shutil.rmtree(tmp_path / "c.mefd.partial")
    tmp = next(src.rglob("*.tmet")).with_suffix(".tmet.mef3io-tmp")
    tmp.write_bytes(b"")
    with pytest.raises(mef3io.WriteConflictError, match="being written"):
        mef3io.rename_session(src, "c")
    tmp.unlink()
    # A name already out of step: fix_name_drift first.
    os.rename(src, tmp_path / "moved.mefd")
    with pytest.raises(mef3io.WriteConflictError, match="fix_name_drift"):
        mef3io.rename_session(tmp_path / "moved.mefd", "c")


def test_a_damaged_header_refuses_before_anything_changes(tmp_path):
    src = _session(tmp_path / "a.mefd", segments=1, channels=("A",))
    tdat = next(src.rglob("*.tdat"))
    b = bytearray(tdat.read_bytes())
    b[100] ^= 0xFF
    tdat.write_bytes(bytes(b))
    before = _files(src)
    with pytest.raises(mef3io.IoError, match="CRC"):
        mef3io.rename_session(src, "b")
    assert _files(src) == before and mef3io.operation_marker(src) is None


def test_case_only_rename(tmp_path):
    src = _session(tmp_path / "patient.mefd", segments=1, channels=("A",))
    rep = mef3io.rename_session(src, "Patient")
    assert sorted(p.name for p in tmp_path.iterdir()) == ["Patient.mefd"]
    assert sorted(p.name for p in Path(rep.new_path).iterdir() if p.is_file()) == [
        "Patient.rdat", "Patient.ridx"]


# --- crashes -----------------------------------------------------------------

def _points(tmp_path):
    """How many crash points a rename of the standard session has."""
    s = _session(tmp_path / "count.mefd")
    n = len(_files(s)) + 2 + 1  # headers, two record files, the folder
    shutil.rmtree(s)
    return 2 + n + 1  # journal, marker, the changes, marker removed


def test_a_crash_at_every_point_is_rolled_back(tmp_path):
    points = _points(tmp_path)
    for k in range(points):
        root = tmp_path / f"run{k}"
        root.mkdir()
        src = _session(root / "a.mefd")
        before = _files(src)
        with pytest.raises(mef3io.IoError, match="simulated crash"):
            mef3io.rename_session(src, "b", _crash_at=k)
        where = src if src.exists() else root / "b.mefd"
        marked = mef3io.operation_marker(where) is not None
        assert marked == (1 <= k < points - 1), k
        if marked:
            with pytest.raises(mef3io.WriteConflictError, match="middle of an operation"):
                mef3io.Writer(str(where))
            with pytest.warns(mef3io.SessionOperationWarning):
                mef3io.Reader(str(where)).close()
        # The next call settles it: a rollback that restores every byte
        # (nothing else wrote meanwhile), or, at the first and last point, a
        # journal discarded. fix_name_drift then has nothing to do.
        settled = mef3io.fix_name_drift(where)
        assert (settled.rolled_back != "") == marked, k
        assert settled.journals_discarded == (0 if marked else 1), k
        assert not settled.changed, k
        if k < points - 1:
            assert _files(root / "a.mefd") == before, k  # exactly, provenance included
            rep = mef3io.rename_session(root / "a.mefd", "b")
            assert rep.changed
        dst = root / "b.mefd"
        _assert_only_names_changed(before, _files(dst))
        assert _stored_names(dst) == {"b"} and mef3io.name_drift(dst).consistent
        assert mef3io.operation_marker(dst) is None
        assert [p.name for p in root.iterdir()] == ["b.mefd"], k
        _clean(dst)


def test_a_failure_at_every_point_is_rolled_back_in_process(tmp_path):
    points = _points(tmp_path)
    for k in range(1, points - 1):
        root = tmp_path / f"run{k}"
        root.mkdir()
        src = _session(root / "a.mefd")
        before = _files(src)
        with pytest.raises(mef3io.IoError, match="simulated failure"):
            mef3io.rename_session(src, "b", _fail_at=k)
        assert _files(src) == before, k
        assert mef3io.operation_marker(src) is None
        assert [p.name for p in root.iterdir()] == ["a.mefd"], k


def test_a_copy_made_mid_operation_does_not_roll_back_the_original(tmp_path):
    src = _session(tmp_path / "a.mefd", segments=1, channels=("A",))
    with pytest.raises(mef3io.IoError):
        mef3io.rename_session(src, "b", _crash_at=3)
    copy = tmp_path / "c.mefd"
    shutil.copytree(src, copy)
    original = _files(src)
    with pytest.raises(mef3io.WriteConflictError, match="another folder"):
        mef3io.rename_session(copy, "z")
    assert _files(src) == original and not (tmp_path / "z.mefd").exists()
    mef3io.fix_name_drift(src)  # the original's own call rolls it back
    assert _stored_names(src) == {"a"}


def test_a_rollback_keeps_an_append_made_meanwhile(tmp_path):
    """A writer that ignored the marker (another tool) appended between the
    crash and the rollback: the rollback restores the names, not the bytes."""
    src = _session(tmp_path / "a.mefd", segments=1, channels=("A",), description="a")
    with pytest.raises(mef3io.IoError):
        mef3io.rename_session(src, "b", _crash_at=4)
    marker = src / ".mef3io-operation"
    saved = marker.read_bytes()
    marker.unlink()
    with mef3io.Writer(str(src)) as w:
        w.write("A", np.arange(2560) * 1.0, START + 10 * SECOND, 256.0, precision=0)
    marker.write_bytes(saved)
    rep = mef3io.fix_name_drift(src)
    assert rep.rolled_back
    assert _stored_names(src) == {"a"} and mef3io.name_drift(src).consistent
    _clean(src)
    with mef3io.Reader(str(src)) as r:
        assert r.metadata.acquisition.session_description == "a"
        assert np.count_nonzero(~np.isnan(r.read("A"))) == 2 * 2560


def test_rollback_restores_the_old_bytes(tmp_path):
    src = _session(tmp_path / "a.mefd", description="a")
    before = _files(src)
    with pytest.raises(mef3io.IoError):
        mef3io.rename_session(src, "b", _crash_at=6)  # mid-headers
    # Roll back by renaming to the old name's opposite... the next call
    # rolls back first; a rename to "a" after that is refused as "already
    # named", which proves the rollback left "a" in place.
    with pytest.raises(ValueError, match="already named"):
        mef3io.rename_session(src, "a")
    assert _files(src) == before
    assert mef3io.operation_marker(src) is None


def test_the_journal_names_temporary_entries_by_token(tmp_path):
    src = _session(tmp_path / ("n" * 200 + ".mefd"), segments=1, channels=("A",))
    with pytest.raises(mef3io.IoError):
        mef3io.rename_session(src, "m" * 250, _crash_at=1)
    backup = tmp_path / ("n" * 200 + ".mefd.repair-backup")
    (journal,) = backup.glob("*.journal")
    text = journal.read_text()
    temp = bytes.fromhex(next(line.split()[1] for line in text.splitlines()
                              if line.startswith("temp_folder"))).decode()
    assert len(temp) == len("x" * 32 + ".mefd") and temp[:32].isalnum()
    assert all(len(bytes.fromhex(line.split()[3]).decode()) == 37
               for line in text.splitlines() if line.startswith("record"))
    marker = (src / ".mef3io-operation").read_text()
    assert "n" * 20 not in marker and "m" * 20 not in marker  # nothing identifying
    mef3io.rename_session(src, "m" * 250)


def test_a_marker_without_its_journal(tmp_path):
    src = _session(tmp_path / "a.mefd", segments=1, channels=("A",))
    with pytest.raises(mef3io.IoError):
        mef3io.rename_session(src, "b", _crash_at=4)  # some headers changed
    shutil.rmtree(tmp_path / "a.mefd.repair-backup")  # the journal is lost
    for call in (lambda: mef3io.rename_session(src, "c"),
                 lambda: mef3io.fix_name_drift(src),
                 lambda: mef3io.Writer(str(src)),
                 lambda: mef3io.repair_session(str(src), ["sizing.maximum-block-bytes"]),
                 lambda: mef3io.archive_session(str(src))):
        with pytest.raises(mef3io.WriteConflictError, match="middle of an operation"):
            call()
    with pytest.raises(mef3io.WriteConflictError, match="marker stays"):
        mef3io.clear_operation_marker(src)  # half-changed: never cleared
    assert mef3io.operation_marker(src)["operation"] == "rename"

    # Nothing changed yet (crash right after the marker): clearing is allowed.
    other = _session(tmp_path / "x.mefd", segments=1, channels=("A",))
    with pytest.raises(mef3io.IoError):
        mef3io.rename_session(other, "y", _crash_at=1)
    shutil.rmtree(tmp_path / "x.mefd.repair-backup")
    mef3io.clear_operation_marker(other)
    assert mef3io.operation_marker(other) is None
    mef3io.rename_session(other, "y")
    with pytest.raises(ValueError, match="no operation marker"):
        mef3io.clear_operation_marker(tmp_path / "y.mefd")


def test_a_copy_refuses_a_source_mid_operation(tmp_path):
    src = _session(tmp_path / "a.mefd", segments=1, channels=("A",))
    with pytest.raises(mef3io.IoError):
        mef3io.rename_session(src, "b", _crash_at=1)
    check = mef3io.transform.check(src, tmp_path / "copy.mefd")
    assert "source-mid-operation" in [r.code for r in check.refusals]


def test_an_open_writer_refuses_after_its_session_was_renamed(tmp_path):
    src = _session(tmp_path / "a.mefd", segments=1, channels=("A",))
    w = mef3io.Writer(str(src))
    mef3io.rename_session(src, "b")
    with pytest.raises(mef3io.IoError, match="no longer there"):
        w.write("A", np.arange(2560) * 1.0, START + 9 * 60 * SECOND, 256.0, precision=0)
    w.close()
    assert not src.exists()  # it did not start a new session at the old path


def test_backup_dir_holds_the_journal_and_is_emptied(tmp_path):
    src = _session(tmp_path / "a.mefd", segments=1, channels=("A",))
    backups = tmp_path / "private"
    backups.mkdir()
    with pytest.raises(mef3io.IoError):
        mef3io.rename_session(src, "b", backup_dir=backups, _crash_at=3)
    assert len(list(backups.glob("*.journal"))) == 1
    # Without the backup folder the journal is not found: refused.
    with pytest.raises(mef3io.WriteConflictError):
        mef3io.rename_session(src, "b")
    rep = mef3io.rename_session(src, "b", backup_dir=backups)
    assert rep.rolled_back and list(backups.iterdir()) == []


# --- fix_name_drift ----------------------------------------------------------

def test_fix_name_drift_after_another_tools_folder_rename(tmp_path):
    pytest.importorskip("pymef")
    from pymef.mef_session import MefSession

    src = _session(tmp_path / "a.mefd", description="a")
    before = _files(src)
    os.rename(src, tmp_path / "pt3.mefd")
    path = tmp_path / "pt3.mefd"
    assert not mef3io.name_drift(path).consistent
    rep = mef3io.fix_name_drift(path)
    assert (rep.old_name, rep.new_name, rep.folder_renamed) == ("a", "pt3", False)
    assert mef3io.name_drift(path).consistent
    _assert_only_names_changed(before, _files(path))
    s = MefSession(str(path), "")
    try:
        assert len([r for r in s.read_records() if r.get("type") == "Note"]) == 1
    finally:
        s.close()
    # Its undo restores the stored names and leaves the folder alone.
    back = rep.undo()
    assert back.new_path == str(path) and _stored_names(path) == {"a"}
    assert sorted(p.name for p in path.iterdir() if p.is_file()) == ["a.rdat", "a.ridx"]
    assert mef3io.fix_name_drift(path).changed  # and can be fixed again


def test_fix_name_drift_refuses_an_ambiguous_old_name(tmp_path):
    a = _session(tmp_path / "a.mefd", segments=1, channels=("A",))
    x = _session(tmp_path / "x.mefd", segments=1, channels=("A",))
    shutil.copy(x / "x.rdat", a / "x.rdat")
    with pytest.raises(mef3io.WriteConflictError, match="ambiguous"):
        mef3io.fix_name_drift(a)
    (a / "x.rdat").unlink()
    tidx = next(a.rglob("*.tidx"))
    shutil.copy(next(x.rglob("*.tidx")), tidx)  # a file that stores "x"
    with pytest.raises(mef3io.WriteConflictError, match="different session names"):
        mef3io.fix_name_drift(a)


def test_fix_name_drift_on_a_consistent_session_changes_nothing(tmp_path):
    src = _session(tmp_path / "a.mefd", segments=1, channels=("A",))
    before = _files(src)
    rep = mef3io.fix_name_drift(src)
    assert not rep.changed and _files(src) == before
