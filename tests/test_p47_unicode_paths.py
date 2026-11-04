"""Non-ASCII folder and session names go through every file-system call.

The core takes UTF-8 text and must hand the operating system a wide path on
Windows (never the ANSI code page): every path goes through detail::utf8_path
and comes back through detail::path_utf8. On Linux and macOS this pins that
the bytes survive; on Windows (GitHub's CI) it is the real check.
"""
from __future__ import annotations

import tarfile
import warnings
from pathlib import Path

import numpy as np
import pytest

import mef3io

FS = 100.0
START = 1_700_000_000_000_000
SECOND = 1_000_000

NAMES = [
    pytest.param(("テスト", "患者.mefd"), id="japanese"),
    pytest.param(("Ünïcödé dir", "Ünïcödé.mefd"), id="accents"),
]


def _quiet(fn, *a, **kw):
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        return fn(*a, **kw)


def _write(path, offset_s=0, *, overwrite=True, channels=("A", "B"), note="n"):
    with mef3io.Writer(str(path), overwrite=overwrite) as w:
        for c, ch in enumerate(channels):
            x = np.arange(int(FS * 10)) * 1.0 + 100 * c
            w.write(ch, x, START + int(offset_s * SECOND), FS, precision=0)
        w.write_annotations([{"type": "Note", "time": START + int(offset_s * SECOND), "text": note}])


def _read(path, ch="A"):
    with mef3io.Reader(str(path)) as r:
        return np.asarray(r.read(ch), float), [x["text"] for x in r.records(level="all")]


def _session_path(tmp_path, parts):
    return tmp_path.joinpath(*parts)


@pytest.mark.parametrize("parts", NAMES)
def test_write_read_append_annotate(tmp_path, parts):
    sess = _session_path(tmp_path, parts)
    sess.parent.mkdir(parents=True)
    _write(sess, 0, note="first")
    x, notes = _read(sess)
    assert len(x) == 1000 and notes == ["first"]
    assert sess.is_dir() and (sess / "A.timd").is_dir()

    # append more samples to the same channels, then more annotations
    with mef3io.Writer(str(sess)) as w:
        for c, ch in enumerate(("A", "B")):
            w.write(ch, np.arange(int(FS * 10)) * 1.0 + 100 * c + 5000,
                    START + 10 * SECOND, FS, precision=0)
        w.write_annotations([{"type": "Note", "time": START + 12 * SECOND, "text": "second"}])
    x, notes = _read(sess)
    assert len(x) == 2000 and notes == ["first", "second"]
    rep = mef3io.validate_session(str(sess))
    assert rep.ok, rep


@pytest.mark.parametrize("parts", NAMES)
def test_archive_extract_validate_recover(tmp_path, parts):
    sess = _session_path(tmp_path, parts)
    sess.parent.mkdir(parents=True)
    _write(sess)
    want, notes = _read(sess)

    tar = mef3io.archive_session(str(sess))
    assert Path(tar).name == sess.name + ".tar"
    got, got_notes = _read(tar)
    assert np.array_equal(want, got, equal_nan=True) and got_notes == notes
    with tarfile.open(tar) as tf:  # member names are UTF-8 too
        assert any(m.name.startswith(sess.name + "/") for m in tf.getmembers())

    dest = tmp_path / "抽出 Ünï" / sess.name
    dest.parent.mkdir()
    out = mef3io.extract_session(tar, str(dest))
    root = Path(out)
    assert root.is_dir() and root == dest
    got, _ = _read(root)
    assert np.array_equal(want, got, equal_nan=True)
    assert mef3io.validate_session(str(root)).ok

    rep = _quiet(mef3io.recover_session, str(root))  # dry run: reads every file
    assert rep is not None
    rt = mef3io.archive_session(str(sess), str(tmp_path / "また.mefd.tar"))
    assert Path(rt).is_file()


@pytest.mark.parametrize("parts", NAMES)
def test_copies_anonymize_transform_merge(tmp_path, parts):
    sess = _session_path(tmp_path, parts)
    sess.parent.mkdir(parents=True)
    _write(sess)
    other = _session_path(tmp_path, (parts[0], "別" + parts[1]))
    _write(other, 100, note="later")
    want, _ = _read(sess)

    anon = tmp_path / parts[0] / "匿名 Çopy.mefd"
    rep = _quiet(mef3io.anonymize_session, str(sess), str(anon), shift_us=2 * 86_400 * SECOND)
    assert rep.verification.ok, rep.summary()
    got, _ = _read(anon)
    assert np.array_equal(want, got, equal_nan=True)

    tr = tmp_path / parts[0] / "変換 Çopy.mefd"
    rep = _quiet(mef3io.transform_session, str(sess), str(tr), channels=["A"])
    assert rep.verification.ok, rep.summary()
    got, _ = _read(tr)
    assert np.array_equal(want, got, equal_nan=True)

    merged = tmp_path / parts[0] / "統合 Çopy.mefd"
    rep = _quiet(mef3io.merge_sessions, [str(sess), str(other)], str(merged))
    assert rep.verification.ok, rep.summary()
    x, notes = _read(merged)
    assert int(np.sum(~np.isnan(x))) == 2000 and notes == ["n", "later"]
    assert mef3io.validate_session(str(merged)).ok

    # a tar source and a tar-derived copy
    tar = mef3io.archive_session(str(sess))
    again = tmp_path / parts[0] / "再 Çopy.mefd"
    rep = _quiet(mef3io.transform_session, tar, str(again))
    assert rep.verification.ok, rep.summary()

    info = mef3io.inspect(str(merged))
    assert info is not None


@pytest.mark.parametrize("parts", NAMES)
def test_rename_to_a_non_ascii_name(tmp_path, parts):
    sess = _session_path(tmp_path, parts)
    sess.parent.mkdir(parents=True)
    _write(sess)
    want, _ = _read(sess)
    new = "新しい名前"
    rep = _quiet(mef3io.rename_session, str(sess), new)
    moved = Path(sess.parent / (new + ".mefd"))
    assert moved.is_dir() and not sess.exists(), rep
    got, _ = _read(moved)
    assert np.array_equal(want, got, equal_nan=True)
