"""P30: every mef3io warning carries its session as DATA, never in its text (#96).

A caller that must keep paths out of its logs (mef3io-studio) has to be able
to log ``str(w.message)`` as it is. So every warning mef3io emits is a
:class:`mef3io.MefWarning` with ``.path`` (the session, as given) and
``.details`` (the structured items it summarises), and its message contains
neither the session's location nor its name. Pinned here for every warning
class and every call site, on a session whose folder name looks like a
subject identifier, in a directory and in a tar archive.

Also pinned: Python's default filter still shows a warning once per SESSION
(the text used to start with the path, which is what made it per session),
pickling keeps the attributes, and ``-W error`` raises an object that has them.
"""
from __future__ import annotations

import ast
import os
import pickle
import shutil
import warnings
from pathlib import Path

import numpy as np
import pytest

import mef3io

import test_p13_validate as p13
import test_p19_block_copies as p19
import test_p20_segment_isolation as p20
import test_p22_records as p22
import test_p27_password_bytes as p27

NAME = "Jane_Doe_MRN123"  # a session name that identifies someone


def _record(fn, *args, **kw):
    with warnings.catch_warnings(record=True) as caught:
        warnings.simplefilter("always")
        out = fn(*args, **kw)
    return out, [w for w in caught if issubclass(w.category, mef3io.MefWarning)]


def _assert_path_free(w, path, tmp_path):
    text = str(w.message)
    assert isinstance(w.message, mef3io.MefWarning)
    assert w.message.path == str(path)
    assert str(tmp_path) not in text, text
    assert NAME not in text, text
    return text


def _declaration_session(path):
    with mef3io.Writer(str(path), overwrite=True) as w:
        w.write("ch1", np.arange(2560.0), p20.START, 256.0, precision=0)
    tmet = next(Path(path).glob("ch1.timd/*.segd/*.tmet"))
    p13._patch_s2(tmet, "maximum_difference_bytes", 0)
    return path


# --------------------------------------------------------------------------- the base class


def test_every_warning_class_is_a_mef_warning():
    for cls in (mef3io.SessionDeclarationWarning, mef3io.UnreadableSegmentWarning,
                mef3io.BlockCopyWarning, mef3io.LegacyPasswordWarning,
                mef3io.PasswordInteropWarning):
        assert issubclass(cls, mef3io.MefWarning)
        assert issubclass(cls, UserWarning)  # what every release so far raised
    w = mef3io.SessionDeclarationWarning("text")  # how warnings.warn builds one
    assert (str(w), w.path, w.details) == ("text", None, ())
    assert "MefWarning" in mef3io.__all__


def test_only_the_helper_touches_the_warnings_module():
    """A bare warnings.warn() would skip the scrub and the path attribute, so
    no module but _warnings.py may import `warnings` at all (any spelling,
    subpackages included)."""
    pkg = Path(mef3io.__file__).parent
    files = [f for f in pkg.rglob("*.py") if f.name != "_warnings.py"]
    assert any(f.parent.name == "pure" for f in files)  # rglob reaches subpackages
    for f in files:
        for node in ast.walk(ast.parse(f.read_text(encoding="utf-8"))):
            if isinstance(node, ast.Import):
                bad = [a.name for a in node.names if a.name.split(".")[0] == "warnings"]
            elif isinstance(node, ast.ImportFrom):
                bad = [node.module] if (node.module or "").split(".")[0] == "warnings" else []
            else:
                bad = []
            assert not bad, f"{f.relative_to(pkg)}:{node.lineno} imports {bad}"


def test_pickling_keeps_the_attributes():
    w = mef3io.UnreadableSegmentWarning("m", path="/x/s.mefd", details=[{"a": 1}])
    back = pickle.loads(pickle.dumps(w))
    assert (type(back), str(back), back.path, back.details) == (
        mef3io.UnreadableSegmentWarning, "m", "/x/s.mefd", ({"a": 1},))


# --------------------------------------------------------------------------- each call site


def test_declaration_warning(tmp_path):
    path = _declaration_session(tmp_path / f"{NAME}.mefd")
    _, ws = _record(mef3io.Reader, str(path))
    (w,) = [w for w in ws if w.category is mef3io.SessionDeclarationWarning]
    text = _assert_path_free(w, path, tmp_path)
    assert "maximum_difference_bytes" in text
    assert w.message.details[0]["field"] == "maximum_difference_bytes"
    # the legacy drop-in too
    _, ws = _record(mef3io.MefReader, str(path))
    (w,) = [w for w in ws if w.category is mef3io.SessionDeclarationWarning]
    _assert_path_free(w, path, tmp_path)


@pytest.mark.parametrize("layout", ["directory", "tar"])
def test_unreadable_segment_warning(tmp_path, layout):
    path = tmp_path / f"{NAME}.mefd"
    p20._write_segments(path)
    p20._corrupt_tmet(p20._segd_dirs(path)[1])
    if layout == "tar":
        path = Path(mef3io.archive_session(str(path)))
    _, ws = _record(mef3io.Reader, str(path), strict=False)
    (w,) = [w for w in ws if w.category is mef3io.UnreadableSegmentWarning]
    text = _assert_path_free(w, path, tmp_path)
    assert "SKIPPED" in text and "<session>" in text  # the location, scrubbed
    assert w.message.details[0]["segment_number"] == 1
    assert str(tmp_path) in w.message.details[0]["segment"]  # the data keeps it


def test_unreadable_record_file_warning(tmp_path):
    path = tmp_path / f"{NAME}.mefd"
    p20._write_segments(path, n_segments=2)
    seg = Path(p20._segd_dirs(path)[0])
    (seg / (seg.stem + ".rdat")).write_bytes(b"\0" * 100)
    r = mef3io.Reader(str(path), strict=False)
    _, ws = _record(r.records, "ch1")
    (w,) = ws
    assert w.category is mef3io.UnreadableSegmentWarning
    assert "record file" in _assert_path_free(w, path, tmp_path)


def test_session_record_file_name_is_scrubbed(tmp_path):
    """The session's own record file is named after the session."""
    path = tmp_path / f"{NAME}.mefd"
    p20._write_segments(path, n_segments=1)
    with mef3io.Writer(str(path)) as wr:
        wr.write_annotations([{"type": "Note", "time": p20.START, "text": "x"}])
    (path / f"{NAME}.rdat").write_bytes(b"\0" * 100)
    r = mef3io.Reader(str(path), strict=False)
    _, ws = _record(r.records)
    (w,) = ws
    text = _assert_path_free(w, path, tmp_path)
    assert "<session>.rdat" in text


def test_bare_session_name_is_scrubbed_anywhere_in_the_text(tmp_path):
    """The name can follow a space, not only a separator: two stray root
    .rdat files make the core say "no <name>.rdat but 2 .rdat files"."""
    path = tmp_path / f"{NAME}.mefd"
    p20._write_segments(path, n_segments=1)
    for stray in ("other1", "other2"):
        (path / f"{stray}.rdat").write_bytes(b"\0" * 100)
    r = mef3io.Reader(str(path), strict=False)
    _, ws = _record(r.records)
    (w,) = ws
    text = _assert_path_free(w, path, tmp_path)
    assert "no <session>.rdat but 2 .rdat files" in text


def test_a_renamed_archive_does_not_leak_its_inner_folder(tmp_path):
    """The folder inside an archive keeps the original name after the archive
    file is renamed; the backend's own view of the name is scrubbed too."""
    src = tmp_path / f"{NAME}.mefd"
    p20._write_segments(src)
    p20._corrupt_tmet(p20._segd_dirs(src)[1])
    for stray in ("other1", "other2"):
        (src / f"{stray}.rdat").write_bytes(b"\0" * 100)
    tar = Path(mef3io.archive_session(str(src)))
    shutil.rmtree(src)
    renamed = tmp_path / "anon.mefd.tar"
    os.rename(tar, renamed)
    r, ws = _record(mef3io.Reader, str(renamed), strict=False)
    (w,) = ws
    text = _assert_path_free(w, renamed, tmp_path)
    assert "<session>::<session>/ch1.timd/" in text
    _, ws = _record(r.records)
    (w,) = ws
    text = _assert_path_free(w, renamed, tmp_path)
    assert "no <session>.rdat" in text


def test_scrub_rules():
    from mef3io._warnings import scrub

    # bare name after a space, inside parentheses, at the end
    assert scrub("x Jane y (Jane) Jane", "/d/Jane.mefd") == "x <session> y (<session>) <session>"
    # part of a longer word is left alone
    assert scrub("Janeway JaneX", "/d/Jane.mefd") == "Janeway JaneX"
    # a session NAMED "session" does not eat its own placeholders
    assert scrub("/d/session.mefd/ch1 and session", "/d/session.mefd") == \
        "<session>/ch1 and <session>"
    # backend identity of a renamed archive
    assert scrub("a.mefd.tar::Old.mefd/x; no Old.rdat", "a.mefd.tar",
                 also=["a.mefd.tar::Old.mefd", "Old"]) == "<session>::<session>/x; no <session>.rdat"


def test_block_copy_warning(tmp_path):
    path = tmp_path / f"{NAME}.mefd"
    p19._write(path)

    def fn(raw, dat, i, off, boff, bbytes):
        if i == 1:
            raw[off + 24:off + 28] = (123).to_bytes(4, "little")
    p19._edit(path, fn)
    r = mef3io.Reader(str(path))
    _, ws = _record(r.read_raw, "ch1")
    (w,) = ws
    assert w.category is mef3io.BlockCopyWarning
    text = _assert_path_free(w, path, tmp_path)
    assert "block 1" in text and ".tdat" in text
    assert w.message.details[0]["block_index"] == 1


def test_legacy_password_warnings(tmp_path):
    x = np.arange(2000, dtype=np.int32)
    path = tmp_path / f"{NAME}.mefd"
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", mef3io.PasswordInteropWarning)
        wr = mef3io.Writer(str(path), password1=p27._old_bytes_as_password(p27.PW1),
                           password2=p27._old_bytes_as_password(p27.PW2))
        wr.write_int32("ch1", x, 0.5, p27.START, p27.FS)
        wr.write_annotations([{"time": p27.START + 1_000_000, "text": "old"}])
        wr.close()
    r, ws = _record(mef3io.Reader, str(path), password=p27.PW2)
    (w,) = ws
    assert w.category is mef3io.LegacyPasswordWarning
    _assert_path_free(w, path, tmp_path)
    assert w.message.details == ("ch1",)
    _, ws = _record(r.records)
    (w,) = ws
    assert "1 record file" in _assert_path_free(w, path, tmp_path)
    assert w.message.details[0].endswith(".rdat")
    # appending under the old keys
    end = p27.START + int(len(x) / p27.FS * 1e6)
    wr = mef3io.Writer(str(path), password1=p27.PW1, password2=p27.PW2)
    _, ws = _record(wr.write_int32, "ch1", x, 0.5, end, p27.FS)
    wr.close()
    (w,) = [w for w in ws if w.category is mef3io.LegacyPasswordWarning]
    assert "appended" in _assert_path_free(w, path, tmp_path)
    assert len(w.message.details) == 1


def test_password_interop_warning(tmp_path):
    path = tmp_path / f"{NAME}.mefd"
    wr, ws = _record(mef3io.Writer, str(path), password1="p" * 20, password2="q" * 20)
    wr.close()
    assert {w.message.details for w in ws} == {("level-1",), ("level-2",)}
    for w in ws:
        assert w.category is mef3io.PasswordInteropWarning
        text = _assert_path_free(w, path, tmp_path)
        assert "p" * 20 not in text


def test_writer_unreadable_metadata_warning(tmp_path):
    from mef3io.compat import MefWriter

    path = tmp_path / f"{NAME}.mefd"
    p22._segment_layout(path, [0], [1])
    for make in (lambda: mef3io.Writer(str(path)), lambda: MefWriter(str(path), overwrite=False)):
        wr, ws = _record(make)
        wr.close()
        (w,) = ws
        assert w.category is mef3io.UnreadableSegmentWarning
        assert "without metadata" in _assert_path_free(w, path, tmp_path)
        assert str(tmp_path) in w.message.details[0]


def test_minus_w_error_raises_the_object_with_its_path(tmp_path):
    path = _declaration_session(tmp_path / f"{NAME}.mefd")
    with warnings.catch_warnings():
        warnings.simplefilter("error", mef3io.MefWarning)
        with pytest.raises(mef3io.SessionDeclarationWarning) as e:
            mef3io.Reader(str(path))
    assert e.value.path == str(path) and str(tmp_path) not in str(e.value)


def test_the_warning_points_at_the_caller(tmp_path):
    path = _declaration_session(tmp_path / f"{NAME}.mefd")
    _, ws = _record(mef3io.Reader, str(path))
    assert ws[0].filename == __file__


# --------------------------------------------------------------------------- deduplication


def test_default_filter_still_warns_once_per_session(tmp_path):
    """Same text for two sessions: both are shown, a repeat of either is not."""
    a = _declaration_session(tmp_path / "a.mefd")
    b = _declaration_session(tmp_path / "b.mefd")

    def open_(p):
        return mef3io.Reader(str(p))  # one call site for every open

    with warnings.catch_warnings(record=True) as caught:
        warnings.simplefilter("default")
        for p in (a, b, a, b):
            open_(p)
    hits = [w for w in caught if w.category is mef3io.SessionDeclarationWarning]
    assert [w.message.path for w in hits] == [str(a), str(b)]
    assert str(hits[0].message) == str(hits[1].message)  # the path really is out of the text


def test_dedup_registry_is_per_calling_file(tmp_path):
    """Same session, same text, same line number, different files: both show."""
    from mef3io._warnings import emit

    path = str(tmp_path / "s.mefd")
    src = "def f(emit, cls, path):\n    emit(cls, 'same text', path=path, stacklevel=2)\n"
    callers = []
    for name in ("one.py", "two.py"):
        ns = {}
        exec(compile("def g(f, emit, cls, path):\n    f(emit, cls, path)\n", str(tmp_path / name),
                     "exec"), ns)
        exec(compile(src, str(tmp_path / ("f_" + name)), "exec"), ns)
        callers.append(ns)
    with warnings.catch_warnings(record=True) as caught:
        warnings.simplefilter("default")
        for _ in range(2):
            for ns in callers:
                ns["g"](ns["f"], emit, mef3io.BlockCopyWarning, path)
    assert sorted(Path(w.filename).name for w in caught) == ["one.py", "two.py"]
