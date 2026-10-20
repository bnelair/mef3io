"""#72 (first part) — time_range, name_drift, rename_archive.

* time_range equals what inspect reports, reads only the first and last
  segment's metadata of each channel (and nothing of the data), works on
  tar archives, and says when the times cannot be shown as absolute;
* name_drift finds a folder renamed behind the session's back;
* rename_archive renames without ever replacing, and refuses what it must.
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


def _session(path, segments=4, channels=("A", "B"), password=False):
    kw = dict(password1="pw-one", password2="pw-two") if password else {}
    with mef3io.Writer(str(path), overwrite=True, **kw) as w:
        for k in range(segments):
            for ch in channels:
                w.write(ch, np.arange(2560) * 1.0, START + k * 60 * SECOND, 256.0, precision=0,
                        new_segment=k > 0)
        w.write_annotations([{"type": "Note", "time": START + SECOND, "text": "x"}])
    return Path(path)


def test_time_range_matches_inspect(tmp_path):
    src = _session(tmp_path / "s.mefd")
    r = mef3io.time_range(src)
    s = mef3io.inspect(src)
    assert (r.start_uutc, r.end_uutc) == (s.start_time, s.end_time)
    assert r.offset_known and r.channels == 2 and r.files_read == 4  # first + last per channel


@pytest.mark.skipif(not Path("/proc/self/io").exists(), reason="needs /proc/self/io")
def test_time_range_reads_only_a_few_metadata_files(tmp_path):
    src = _session(tmp_path / "s.mefd", segments=60, channels=("A",))
    data = sum(p.stat().st_size for p in src.rglob("*") if p.suffix in (".tdat", ".tidx"))

    def rchar():
        for line in Path("/proc/self/io").read_text().splitlines():
            if line.startswith("rchar:"):
                return int(line.split()[1])

    before = rchar()
    mef3io.time_range(src)
    read = rchar() - before
    assert read < 3 * 16384 + 64 * 1024, (read, data)  # two .tmet, plus listing


def test_time_range_of_a_tar_and_without_a_password(tmp_path):
    src = _session(tmp_path / "s.mefd", password=True)
    tar = mef3io.archive_session(str(src), str(tmp_path / "s.mefd.tar"))
    with_pw = mef3io.time_range(tar, "pw-two")
    without = mef3io.time_range(tar)
    assert with_pw.offset_known and not without.offset_known
    assert (without.start_uutc, without.end_uutc) == (with_pw.start_uutc, with_pw.end_uutc)
    with pytest.raises(mef3io.PasswordError):
        mef3io.time_range(src, "wrong")


def test_time_range_of_a_legacy_session_with_an_offset(tmp_path):
    rto = START - 6 * 3600 * SECOND
    src = _legacy(tmp_path / "rto.mefd", rto=rto)
    r = mef3io.time_range(src)
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        s = mef3io.inspect(src)
    assert r.offset_known and r.start_uutc == s.start_time == START


def test_time_range_with_segments_numbered_against_time(tmp_path):
    with mef3io.Writer(str(tmp_path / "s.mefd"), overwrite=True) as w:
        w.write("A", np.arange(2560) * 1.0, START + 1000 * SECOND, 256.0, precision=0)
    seg0 = tmp_path / "s.mefd" / "A.timd" / "A-000000.segd"
    with mef3io.Writer(str(tmp_path / "t.mefd"), overwrite=True) as w:
        w.write("A", np.arange(2560) * 1.0, START, 256.0, precision=0)
    seg1 = tmp_path / "s.mefd" / "A.timd" / "A-000001.segd"
    shutil.copytree(tmp_path / "t.mefd" / "A.timd" / "A-000000.segd", seg1)
    for f in list(seg1.iterdir()):
        f.rename(seg1 / ("A-000001" + f.suffix))
    r = mef3io.time_range(tmp_path / "s.mefd")
    assert r.start_uutc == START  # the later-numbered, earlier segment still counts


def test_name_drift_sees_a_renamed_record_index(tmp_path):
    src = _session(tmp_path / "P.mefd", segments=1)
    os.rename(src / "P.ridx", src / "Q.ridx")
    assert not mef3io.name_drift(src).consistent


def test_name_drift_finds_a_folder_renamed_behind_the_sessions_back(tmp_path):
    src = _session(tmp_path / "Patient.mefd", segments=2)
    d = mef3io.name_drift(src)
    assert d.consistent and d.folder == "Patient" and set(d.stored_names) == {"Patient"}
    assert d.record_files == ("Patient",)
    moved = tmp_path / "Renamed.mefd"
    os.rename(src, moved)
    d = mef3io.name_drift(moved)
    assert not d.consistent
    assert d.folder == "Renamed" and set(d.stored_names) == {"Patient"}
    assert d.files_read == sum(1 for p in moved.rglob("*") if p.is_file())


def test_rename_archive(tmp_path):
    src = _session(tmp_path / "s.mefd", segments=1)
    tar = mef3io.archive_session(str(src), str(tmp_path / "s.mefd.tar"))
    r = mef3io.rename_archive(tar, "S-001")
    assert r.inside_keeps_old_name and Path(r.new_path).name == "S-001.mefd.tar"
    assert not Path(tar).exists() and Path(r.new_path).exists()
    with mef3io.Reader(r.new_path) as reader:  # still a readable session
        assert reader.channels == ["A", "B"]
    # Undo is the same call back.
    assert Path(mef3io.rename_archive(r.new_path, "s").new_path) == Path(tar)


def test_rename_archive_refusals(tmp_path):
    src = _session(tmp_path / "s.mefd", segments=1)
    tar = Path(mef3io.archive_session(str(src), str(tmp_path / "s.mefd.tar")))
    shutil.copy(tar, tmp_path / "taken.mefd.tar")
    with pytest.raises(mef3io.IoError, match="already exists"):
        mef3io.rename_archive(tar, "taken")
    with pytest.raises(ValueError, match="246"):
        mef3io.rename_archive(tar, "x" * 247)
    mef3io.rename_archive(tar, "y" * 246).new_path  # the longest that fits
    tar = Path(mef3io.rename_archive(tmp_path / ("y" * 246 + ".mefd.tar"), "s").new_path)
    with pytest.raises(ValueError):
        mef3io.rename_archive(tar, "sub/dir")
    with pytest.raises(ValueError):
        mef3io.rename_archive(src, "z")  # a folder session: not this function
    for bad in ("a\x00b", "a:b", "a*b", "CON", "lpt1", "name.", "name ", "tab\tx"):
        with pytest.raises(ValueError):
            mef3io.rename_archive(tar, bad)
        assert tar.exists()  # nothing changed
    # archive_session writes <archive>.tmp while it works
    (tmp_path / "s.mefd.tar.tmp").write_bytes(b"")
    with pytest.raises(mef3io.IoError, match="being written"):
        mef3io.rename_archive(tar, "z")
