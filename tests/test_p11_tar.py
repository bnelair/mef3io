"""P11 gate: tar session archives. archive_session output reads identically to
the directory session (values, gaps, records, encryption), interoperates with
standard tar tooling both directions, and the Writer refuses tar paths."""
import gzip
import json
import tarfile
from pathlib import Path

import numpy as np
import pytest

import mef3io

REPO = Path(__file__).resolve().parent.parent
GOLDEN = REPO / "tests" / "golden"
MANIFEST = json.loads((GOLDEN / "manifest.json").read_text())
CASES = MANIFEST["cases"]


def _nan_equal(a, b):
    a = np.asarray(a, float)
    b = np.asarray(b, float)
    if a.shape != b.shape:
        return False
    ma, mb = np.isnan(a), np.isnan(b)
    return np.array_equal(ma, mb) and np.allclose(a[~ma], b[~mb])


def _assert_same_session(dir_path, tar_path, password=""):
    with mef3io.Reader(str(dir_path), password) as rd, mef3io.Reader(str(tar_path), password) as rt:
        assert rd.channels == rt.channels
        assert rd.records() == rt.records()
        for ch in rd.channels:
            assert rd.info(ch) == rt.info(ch)
            assert _nan_equal(rd.read(ch), rt.read(ch))
            raw_d, raw_t = rd.read_raw(ch), rt.read_raw(ch)
            assert np.array_equal(raw_d["samples"], raw_t["samples"])
            assert np.array_equal(raw_d["valid"], raw_t["valid"])
            assert rd.toc(ch) == rt.toc(ch)
            assert rd.records(ch) == rt.records(ch)
            seg_d, seg_t = rd.segments(ch), rt.segments(ch)
            assert len(seg_d) == len(seg_t)
            for a, b in zip(seg_d, seg_t):
                # paths differ by design: tar segments use <archive>::<member>
                assert "::" in b.pop("path")
                a.pop("path")
                assert a == b


@pytest.mark.parametrize("case", CASES, ids=lambda c: c["name"])
def test_tar_reads_identically_to_directory(case, tmp_path):
    src = GOLDEN / f"{case['name']}.mefd"
    tar = mef3io.archive_session(src, tmp_path / f"{case['name']}.mefd.tar")
    assert Path(tar) == tmp_path / f"{case['name']}.mefd.tar"
    _assert_same_session(src, tar, case["password"] or "")


def test_default_tar_path_and_overwrite(tmp_path):
    src = tmp_path / "sess.mefd"
    _write_demo(src)
    tar = mef3io.archive_session(src)
    assert tar == str(src) + ".tar"
    with pytest.raises(RuntimeError, match="exists"):
        mef3io.archive_session(src)
    first = Path(tar).read_bytes()
    assert mef3io.archive_session(src, overwrite=True) == tar
    assert Path(tar).read_bytes() == first  # deterministic output


def test_outbound_interop_stdlib_tarfile_extracts_identical_tree(tmp_path):
    src = GOLDEN / "with_annotations.mefd"
    tar = mef3io.archive_session(src, tmp_path / "a.mefd.tar")
    with tarfile.open(tar) as tf:
        if hasattr(tarfile, "data_filter"):
            tf.extractall(tmp_path / "out", filter="data")
        else:  # 3.10.x before the filter backport (3.10.12)
            tf.extractall(tmp_path / "out")
    extracted = tmp_path / "out" / "with_annotations.mefd"
    src_files = sorted(p.relative_to(src) for p in src.rglob("*") if p.is_file())
    ext_files = sorted(p.relative_to(extracted) for p in extracted.rglob("*") if p.is_file())
    assert src_files == ext_files
    for rel in src_files:
        assert (src / rel).read_bytes() == (extracted / rel).read_bytes()


def _tar_with_tarfile(src, dest, fmt, prefix="", include_dirs=True):
    """Archive `src` with the stdlib, controlling format and member naming."""
    with tarfile.open(dest, "w", format=fmt) as tf:
        root = prefix + src.name
        if include_dirs:
            tf.add(src, arcname=root, recursive=False)
        for p in sorted(src.rglob("*")):
            arcname = root + "/" + str(p.relative_to(src))
            if p.is_dir():
                if include_dirs:
                    tf.add(p, arcname=arcname, recursive=False)
            else:
                tf.add(p, arcname=arcname)
    return dest


@pytest.mark.parametrize(
    "fmt,prefix,include_dirs",
    [
        (tarfile.USTAR_FORMAT, "", True),
        (tarfile.GNU_FORMAT, "", True),
        (tarfile.PAX_FORMAT, "", True),
        (tarfile.PAX_FORMAT, "./", True),   # bsdtar-style ./-prefixed members
        (tarfile.GNU_FORMAT, "", False),    # no directory entries at all
    ],
    ids=["ustar", "gnu", "pax", "dot-slash", "no-dir-entries"],
)
def test_inbound_interop_foreign_tars(tmp_path, fmt, prefix, include_dirs):
    src = GOLDEN / "multi_channel.mefd"
    tar = _tar_with_tarfile(src, tmp_path / "foreign.mefd.tar", fmt, prefix, include_dirs)
    _assert_same_session(src, tar)


def test_tar_without_session_root_dir(tmp_path):
    # Tarred from *inside* the session dir: .timd trees at top level.
    src = GOLDEN / "multi_channel.mefd"
    dest = tmp_path / "inside.mefd.tar"
    with tarfile.open(dest, "w", format=tarfile.PAX_FORMAT) as tf:
        for p in sorted(src.rglob("*")):
            tf.add(p, arcname=str(p.relative_to(src)), recursive=False)
    _assert_same_session(src, dest)


def test_writer_rejects_tar_paths(tmp_path):
    src = tmp_path / "sess.mefd"
    _write_demo(src)
    tar = mef3io.archive_session(src)
    with pytest.raises(RuntimeError, match="read-only"):
        mef3io.Writer(tar)
    with pytest.raises(RuntimeError, match="read-only"):
        mef3io.Writer(tar, overwrite=True)
    assert Path(tar).exists()  # overwrite must never delete the archive
    with pytest.raises(RuntimeError, match="read-only"):
        mef3io.Writer(tmp_path / "new.mefd.tar", overwrite=True)
    with pytest.raises(RuntimeError, match="read-only"):
        mef3io.MefWriter(tar, overwrite=True)


def test_compressed_archive_rejected(tmp_path):
    src = tmp_path / "sess.mefd"
    _write_demo(src)
    tar = mef3io.archive_session(src)
    # a .tar.gz name never reaches the magic check: the naming rule refuses it
    gz = tmp_path / "sess.mefd.tar.gz"
    gz.write_bytes(gzip.compress(Path(tar).read_bytes()))
    with pytest.raises(RuntimeError, match="must end with"):
        mef3io.Reader(str(gz))
    # gzip content hiding behind a well-formed name is caught by the magic
    disguised = tmp_path / "disguised.mefd.tar"
    disguised.write_bytes(gzip.compress(Path(tar).read_bytes()))
    with pytest.raises(RuntimeError, match="gzip"):
        mef3io.Reader(str(disguised))
    junk = tmp_path / "junk.mefd.tar"
    junk.write_bytes(b"A" * 2048)
    with pytest.raises(RuntimeError, match="not a tar"):
        mef3io.Reader(str(junk))


def test_session_naming_enforced_everywhere(tmp_path):
    src = tmp_path / "sess.mefd"
    _write_demo(src)
    tar = Path(mef3io.archive_session(src))

    # Reading: directory must end .mefd, archive must end .mefd.tar.
    plain_dir = tmp_path / "not_a_session"
    plain_dir.mkdir()
    with pytest.raises(RuntimeError, match=r"must end with \.mefd"):
        mef3io.Reader(str(plain_dir))
    plain_tar = tmp_path / "renamed.tar"
    plain_tar.write_bytes(tar.read_bytes())
    with pytest.raises(RuntimeError, match=r"must end with \.mefd\.tar"):
        mef3io.Reader(str(plain_tar))
    # trailing separator on a well-named session is fine
    assert mef3io.Reader(str(src) + "/").channels == ["ch1"]

    # Writing: target must end .mefd, and nothing may be created on refusal.
    with pytest.raises(RuntimeError, match=r"must end with \.mefd"):
        mef3io.Writer(tmp_path / "bad_name", overwrite=True)
    assert not (tmp_path / "bad_name").exists()

    # Archiving: source must be .mefd, an explicit target must be .mefd.tar.
    with pytest.raises(RuntimeError, match=r"must end with \.mefd"):
        mef3io.archive_session(plain_dir)
    with pytest.raises(RuntimeError, match=r"must end with \.mefd\.tar"):
        mef3io.archive_session(src, tmp_path / "out.tar")

    # Extracting: source must be .mefd.tar, an explicit target must be .mefd.
    with pytest.raises(RuntimeError, match=r"must end with \.mefd\.tar"):
        mef3io.extract_session(plain_tar)
    with pytest.raises(RuntimeError, match=r"must end with \.mefd"):
        mef3io.extract_session(tar, tmp_path / "outdir")


def test_archive_target_inside_source_rejected(tmp_path):
    src = tmp_path / "sess.mefd"
    _write_demo(src)
    with pytest.raises(RuntimeError, match="inside"):
        mef3io.archive_session(src, src / "self.mefd.tar")


def test_compat_mefreader_over_tar(tmp_path):
    src = GOLDEN / "plain_single.mefd"
    tar = mef3io.archive_session(src, tmp_path / "c.mefd.tar")
    md = mef3io.MefReader(str(src))
    mt = mef3io.MefReader(tar)
    ch = md.channels[0]
    assert _nan_equal(md.get_data(ch), mt.get_data(ch))


def test_cache_over_tar(tmp_path):
    src = tmp_path / "sess.mefd"
    _write_demo(src)
    tar = Path(mef3io.archive_session(src))
    cache_file = tmp_path / "tar.mef3cache"
    r1 = mef3io.Reader(str(tar), cache=str(cache_file))
    assert cache_file.exists()
    r2 = mef3io.Reader(str(tar), cache=str(cache_file))
    assert r2._impl is None  # warm start served from the snapshot
    assert r1.info("ch1") == r2.info("ch1")
    # Touching the archive invalidates the snapshot (single-file fingerprint).
    import os

    st = tar.stat()
    os.utime(tar, ns=(st.st_atime_ns, st.st_mtime_ns + 1))
    r3 = mef3io.Reader(str(tar), cache=str(cache_file))
    assert r3._impl is not None


def test_extract_session_restores_identical_tree(tmp_path):
    src = GOLDEN / "with_annotations.mefd"
    tar = mef3io.archive_session(src, tmp_path / "with_annotations.mefd.tar")
    out = mef3io.extract_session(tar)  # default: strip .tar
    assert out == str(tmp_path / "with_annotations.mefd")
    src_files = sorted(p.relative_to(src) for p in src.rglob("*") if p.is_file())
    out_files = sorted(p.relative_to(out) for p in Path(out).rglob("*") if p.is_file())
    assert src_files == out_files
    for rel in src_files:
        assert (src / rel).read_bytes() == (Path(out) / rel).read_bytes()
    # and the round trip re-archives byte-identically
    tar2 = mef3io.archive_session(out, tmp_path / "again.mefd.tar")
    assert Path(tar2).read_bytes() == Path(tar).read_bytes()


def test_extract_session_makes_session_writable_again(tmp_path):
    src = tmp_path / "sess.mefd"
    _write_demo(src)
    tar = mef3io.archive_session(src)
    out = mef3io.extract_session(tar, tmp_path / "restored.mefd")
    with mef3io.Reader(out) as r:
        before = r.info("ch1")["number_of_samples"]
    with mef3io.Writer(out) as w:  # append to the restored session
        w.write("ch1", np.arange(100, dtype=float), 1577836900000000, fs=250.0)
    with mef3io.Reader(out) as r:
        assert r.info("ch1")["number_of_samples"] == before + 100


def test_extract_session_guards(tmp_path):
    src = tmp_path / "sess.mefd"
    _write_demo(src)
    tar = mef3io.archive_session(src)
    out = mef3io.extract_session(tar, tmp_path / "x.mefd")
    with pytest.raises(RuntimeError, match="exists"):
        mef3io.extract_session(tar, tmp_path / "x.mefd")
    assert mef3io.extract_session(tar, tmp_path / "x.mefd", overwrite=True) == out
    # archives must carry the .mefd.tar suffix (default target derives from it)
    no_suffix = tmp_path / "archive_without_suffix"
    no_suffix.write_bytes(Path(tar).read_bytes())
    with pytest.raises(RuntimeError, match="must end with"):
        mef3io.extract_session(no_suffix)


def test_extract_foreign_tar_and_unsafe_members(tmp_path):
    src = GOLDEN / "plain_single.mefd"
    foreign = _tar_with_tarfile(src, tmp_path / "f.mefd.tar", tarfile.PAX_FORMAT, "./", True)
    out = mef3io.extract_session(foreign, tmp_path / "f.mefd")
    with mef3io.Reader(str(src)) as rd, mef3io.Reader(out) as re_:
        assert _nan_equal(rd.read(rd.channels[0]), re_.read(re_.channels[0]))

    evil = tmp_path / "evil.mefd.tar"
    with tarfile.open(evil, "w", format=tarfile.USTAR_FORMAT) as tf:
        info = tarfile.TarInfo("d.mefd/../../escape.txt")
        payload = b"nope"
        info.size = len(payload)
        import io as _io

        tf.addfile(info, _io.BytesIO(payload))
    with pytest.raises(RuntimeError, match="unsafe"):
        mef3io.extract_session(evil, tmp_path / "evil.mefd")
    assert not (tmp_path.parent / "escape.txt").exists()


def _write_demo(path):
    with mef3io.Writer(str(path), overwrite=True, units="uV") as w:
        x = np.sin(np.arange(2000) / 20.0)
        x[500:520] = np.nan
        w.write("ch1", x, 1577836800000000, fs=250.0)
        w.write_annotations([{"time": 1577836800100000, "text": "note1"}])
