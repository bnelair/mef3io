"""#73 — transform_session: a copy with a choice of channels, passwords, name and details.

What is pinned here, on real files:

* every time (universal headers, index, blocks, records at all three levels,
  record body times, the recording-time offset) and every sample is copied
  verbatim, and so is every record body, including types mef3io cannot parse;
* only what was asked changes: the channels, the passwords, the session name
  (also in the session's own annotation file name) and the metadata edits;
* every file is stamped with provenance operation 8 and keeps a link to the
  source file; the regions other writers fill are kept;
* encrypted and offset-using sources, pymef reading the copy, tar input;
* the refusals (codes, check() collects them, nothing is left behind), the
  CLI, and a tampered copy failing verify();
* docs/transform.md documents every refusal code.
"""
from __future__ import annotations

import json
import re
import struct
import warnings
from pathlib import Path

import numpy as np
import pytest

import mef3io
from mef3io import _mef3io as m
from mef3io.transform import REFUSAL_CODES

from test_p29_anonymize import (  # one fixture builder for every copy test
    GOLDEN,
    S2,
    S3,
    START,
    SECOND,
    TMET,
    UH,
    _all_records,
    _legacy,
    _read_all,
    _reseal,
    _rich_session,
    _set_rto,
)

DOCS = Path(__file__).resolve().parents[1] / "docs" / "transform.md"
S2_DISCRETIONARY = 8592


def _transform(src, dst, **kw):
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        return mef3io.transform_session(src, dst, **kw)


@pytest.fixture(scope="module")
def rich(tmp_path_factory):
    base = tmp_path_factory.mktemp("rich_t")
    src = _rich_session(base / "jane_roe.mefd")
    rep = _transform(src, base / "study.mefd")
    return src, base / "study.mefd", rep


def _records_without_paths(recs):
    return [{k: v for k, v in r.items() if k not in ("path", "file")} for r in recs]


# --------------------------------------------------------------------------- the copy


def test_times_samples_and_records_are_copied_verbatim(rich):
    src, dst, rep = rich
    assert rep.verification.ok, rep.summary()
    assert rep.session_name == "study" and rep.metadata_fields_changed == 0
    a, b = _read_all(src), _read_all(dst)
    assert set(a) == set(b)
    for ch in a:
        np.testing.assert_array_equal(a[ch][1], b[ch][1])
        for k in ("start_time", "end_time", "recording_time_offset", "subject_name_1",
                  "subject_id", "recording_location", "channel_description",
                  "session_description"):
            assert b[ch][0][k] == a[ch][0][k], k
    # Every record at every level, with its time and body, including the
    # unknown UnRc body anonymize drops.
    ra, rb = _all_records(src), _all_records(dst)
    assert _records_without_paths(ra) == _records_without_paths(rb)
    assert any(r["type"] == "UnRc" for r in rb)


def test_raw_bytes_are_the_sources_except_what_a_copy_must_change(rich):
    src, dst, _ = rich
    for s in sorted(Path(src).rglob("*.tdat")):
        d = Path(dst) / s.relative_to(src)
        sb, db = s.read_bytes(), d.read_bytes()
        assert sb[UH:] == db[UH:]  # every block: header, statistics, payload
        assert sb[16:32] == db[16:32]  # start / end time as stored
        assert db[308:308 + 5] == b"study"
        assert sb[836:852] != db[836:852]  # a new file UUID ...
        assert db[852:868] in (sb[836:852], sb[852:868])  # ... naming its source
        assert db[960:964] == b"M3IO" and db[965] == 8  # provenance: operation 8, transform
    # The session's own annotation file follows the new name.
    assert (Path(dst) / "study.rdat").exists() and not (Path(dst) / "jane_roe.rdat").exists()


def test_another_writers_regions_are_kept(tmp_path):
    src = _rich_session(tmp_path / "jane_roe.mefd", unknown=False)
    tmet = sorted(Path(src).rglob("*.tmet"))[0]
    raw = bytearray(tmet.read_bytes())
    raw[S2 + S2_DISCRETIONARY:S2 + S2_DISCRETIONARY + 8] = b"OTHERAPP"
    _reseal(raw, TMET)
    tmet.write_bytes(bytes(raw))
    rep = _transform(src, tmp_path / "copy.mefd")
    assert rep.verification.ok
    out = (tmp_path / "copy.mefd" / tmet.relative_to(src)).read_bytes()
    assert out[S2 + S2_DISCRETIONARY:S2 + S2_DISCRETIONARY + 8] == b"OTHERAPP"
    # anonymize clears the same region
    mef3io.anonymize_session(src, tmp_path / "anon.mefd", -7000 * 86_400 * SECOND)
    anon = (tmp_path / "anon.mefd" / tmet.relative_to(src)).read_bytes()
    assert anon[S2 + S2_DISCRETIONARY:S2 + S2_DISCRETIONARY + 8] == bytes(8)


def test_left_out_channels_leave_nothing(tmp_path):
    src = _rich_session(tmp_path / "jane_roe.mefd")
    rep = _transform(src, tmp_path / "two.mefd", exclude_channels=["ch3"])
    assert rep.verification.ok
    assert rep.channels_kept == ("ch1", "ch2") and rep.channels_excluded == ("ch3",)
    assert not (tmp_path / "two.mefd" / "ch3.timd").exists()
    assert rep.seizure_channel_entries_dropped == 1
    seiz = [r for r in _all_records(tmp_path / "two.mefd") if r["type"] == "Seiz"]
    assert [c["name"] for c in seiz[0]["channels"]] == ["ch1", "ch2", "unknown"]


def test_metadata_edits_are_applied_and_nothing_else_changes(tmp_path):
    src = _rich_session(tmp_path / "jane_roe.mefd")
    edits = {
        "subject_name_1": "",
        "subject_name_2": "",
        "subject_id": "S-01",
        "recording_location": "Site A",
        "session_description": "overnight study",
        "channel_descriptions": {"ch1": "left hippocampus, contact 1"},
        "reference_descriptions": {"ch2": "average"},
    }
    rep = _transform(src, tmp_path / "S01.mefd", metadata=edits)
    assert rep.verification.ok, rep.summary()
    a, b = _read_all(src), _read_all(tmp_path / "S01.mefd")
    for ch in b:
        info = b[ch][0]
        assert info["subject_name_1"] == "" and info["subject_name_2"] == ""
        assert info["subject_id"] == "S-01" and info["recording_location"] == "Site A"
        assert info["session_description"] == "overnight study"
        want = "left hippocampus, contact 1" if ch == "ch1" else a[ch][0]["channel_description"]
        assert info["channel_description"] == want
        np.testing.assert_array_equal(a[ch][1], b[ch][1])
    # Each changed field counts once per segment (ch1 has two segments).
    assert rep.metadata_fields_changed > 0
    assert mef3io.transform.verify(src, tmp_path / "S01.mefd", metadata=edits).ok
    assert not mef3io.transform.verify(src, tmp_path / "S01.mefd").ok  # without the edits


def test_encrypted_source_to_plain_and_plain_to_encrypted(tmp_path):
    enc = _rich_session(tmp_path / "enc.mefd", password=True, unknown=False)
    plain = _transform(enc, tmp_path / "plain.mefd", password="pw-two")
    assert plain.verification.ok and not plain.output_encrypted
    a, b = _read_all(enc, "pw-two"), _read_all(tmp_path / "plain.mefd")
    for ch in a:
        np.testing.assert_array_equal(a[ch][1], b[ch][1])
        assert b[ch][0]["subject_id"] == a[ch][0]["subject_id"]
    locked = _transform(tmp_path / "plain.mefd", tmp_path / "locked.mefd",
                        output_password=("new-1", "new-2"))
    assert locked.verification.ok and locked.output_encrypted
    with pytest.raises(mef3io.PasswordError):
        with mef3io.Reader(str(tmp_path / "locked.mefd"), "pw-two") as r:
            r.read(r.channels[0])
    c = _read_all(tmp_path / "locked.mefd", "new-2")
    for ch in a:
        np.testing.assert_array_equal(a[ch][1], c[ch][1])


def test_meflib_encrypted_red_blocks_are_rekeyed_or_decrypted(tmp_path):
    src = GOLDEN / "meflib_red_encrypted.mefd"
    re_ = _transform(src, tmp_path / "re.mefd", password="pass2", output_password=("a1", "a2"))
    de = _transform(src, tmp_path / "de.mefd", password="pass2")
    assert re_.blocks_reencrypted == re_.blocks > 0 and de.blocks_decrypted == de.blocks
    a = _read_all(src, "pass2")
    for path, pw in ((tmp_path / "re.mefd", "a2"), (tmp_path / "de.mefd", "")):
        b = _read_all(path, pw)
        for ch in a:
            np.testing.assert_array_equal(a[ch][1], b[ch][1])


def test_an_offset_session_keeps_its_offset_and_times(tmp_path):
    rto = START - 6 * 3600 * SECOND
    src = _legacy(tmp_path / "rto.mefd", rto=rto)
    rep = _transform(src, tmp_path / "R.mefd")
    assert rep.verification.ok
    a, b = _read_all(src), _read_all(tmp_path / "R.mefd")
    for ch in a:
        assert b[ch][0]["recording_time_offset"] == rto  # not normalised away
        assert b[ch][0]["start_time"] == a[ch][0]["start_time"]
    # Segments that disagree on the offset block anonymize, not a verbatim copy.
    _set_rto(src, rto + SECOND, ["ch2"])
    assert _transform(src, tmp_path / "R2.mefd").verification.ok
    with pytest.raises(mef3io.WriteConflictError):
        mef3io.anonymize_session(src, tmp_path / "A.mefd", -7000 * 86_400 * SECOND)


def test_pymef_reads_the_copy(tmp_path):
    pytest.importorskip("pymef")
    from pymef.mef_session import MefSession

    src = _legacy(tmp_path / "legacy.mefd")
    _transform(src, tmp_path / "copy.mefd", exclude_channels=["ch2"],
               metadata={"subject_id": "S-02"})
    s = MefSession(str(tmp_path / "copy.mefd"), "")
    try:
        y = np.asarray(s.read_ts_channels_sample("ch1", [0, 5000])).ravel()
        np.testing.assert_array_equal(y.astype(np.int64), np.arange(5000))
    finally:
        s.close()


def test_tar_input_is_read_in_place(tmp_path, rich):
    src, _, _ = rich
    tar = mef3io.archive_session(str(src), str(tmp_path / "jane_roe.mefd.tar"))
    rep = _transform(tar, tmp_path / "T.mefd")
    assert rep.verification.ok
    a, b = _read_all(src), _read_all(tmp_path / "T.mefd")
    for ch in a:
        np.testing.assert_array_equal(a[ch][1], b[ch][1])


def test_a_tampered_copy_fails_verification(tmp_path, rich):
    src, dst, _ = rich
    copy = tmp_path / "copy.mefd"
    _transform(src, copy)
    rdat = copy / "copy.rdat"
    raw = bytearray(rdat.read_bytes())
    i = raw.index(b"Jane Roe pressed")
    raw[i] = ord("j")
    # re-seal the touched record and the file so only the content differs
    off = UH
    while off < len(raw):
        nb = struct.unpack_from("<I", raw, off + 12)[0]
        if off <= i < off + 24 + nb:
            struct.pack_into("<I", raw, off, m.crc32(bytes(raw[off + 4:off + 24 + nb])))
            break
        off += 24 + nb
    _reseal(raw)
    rdat.write_bytes(bytes(raw))
    v = mef3io.transform.verify(src, copy)
    assert not v.ok
    assert any(p.field == "record body" for p in v.problems), v.summary()


# --------------------------------------------------------------------------- refusals


def test_check_collects_every_refusal_and_the_copy_raises_the_first(tmp_path, rich):
    src, _, _ = rich
    (tmp_path / "taken.mefd").mkdir()
    kw = dict(exclude_channels=["ch3"],
              metadata={"subject_id": "x" * 200, "nope": "y",
                        "channel_descriptions": {"ch3": "left out", "zz": "unknown"}})
    rep = mef3io.transform.check(src, tmp_path / "taken.mefd", **kw)
    assert not rep.ok and rep.pseudonym == ""
    assert rep.codes == ("metadata-unknown", "dst-exists", "metadata-too-long",
                         "metadata-channel-unknown", "metadata-channel-unknown"), rep.summary()
    with pytest.raises(ValueError) as exc:
        _transform(src, tmp_path / "new.mefd", **kw)
    assert exc.value.code == "metadata-unknown"
    del kw["metadata"]["nope"]
    with pytest.raises(ValueError) as exc:
        _transform(src, tmp_path / "new.mefd", **kw)
    assert exc.value.code == "metadata-too-long" and exc.value.details["field"] == "subject_id"
    assert not (tmp_path / "new.mefd").exists() and not (tmp_path / "new.mefd.partial").exists()


@pytest.mark.parametrize("metadata", [["subject_id"], {"subject_id": 3},
                                      {"channel_descriptions": {"ch1": 1}}])
def test_wrong_metadata_types_have_a_code(tmp_path, rich, metadata):
    src, _, _ = rich
    for fn in (mef3io.transform.check, mef3io.transform_session):
        with pytest.raises(TypeError) as exc:
            fn(src, tmp_path / "x.mefd", metadata=metadata)
        assert exc.value.code == "argument-type"


def test_cancel_leaves_nothing_behind(tmp_path, rich):
    src, _, _ = rich
    token = mef3io.CancelToken()
    calls = []

    def progress(done, total):
        calls.append(done)
        if len(calls) == 3:
            token.cancel()

    with pytest.raises(mef3io.Cancelled):
        mef3io.transform_session(src, tmp_path / "c.mefd", progress=progress, cancel=token)
    assert not (tmp_path / "c.mefd").exists() and not (tmp_path / "c.mefd.partial").exists()


def test_every_code_is_documented():
    def rows(path):
        return {code: (error, where) for code, error, where in re.findall(
            r"^\| `([a-z0-9-]+)` \| `(\w+)` \| (\w+) \|", path.read_text(encoding="utf-8"),
            re.MULTILINE)}

    shared = mef3io._copy.COPY_REFUSAL_CODES
    own = {k: v for k, v in REFUSAL_CODES.items() if k not in shared}
    # The transform's own codes on its page; the shared ones once, in the
    # anonymization page's table it links to.
    assert rows(DOCS) == own, "docs/transform.md and REFUSAL_CODES disagree"
    anon_rows = rows(DOCS.parent / "anonymization.md")
    assert {k: anon_rows.get(k) for k in shared} == shared
    # Every code only a transform has is reached by a test above.
    reached = {"metadata-unknown", "metadata-too-long", "metadata-channel-unknown",
               "metadata-invalid"}
    assert set(own) == reached


def test_a_nul_in_a_metadata_value_is_refused(tmp_path, rich):
    src, _, _ = rich
    rep = mef3io.transform.check(src, tmp_path / "x.mefd", metadata={"subject_id": "a\0b"})
    assert rep.codes == ("metadata-invalid",)


# --------------------------------------------------------------------------- CLI


def test_the_cli_writes_a_copy(tmp_path, rich, capsys):
    from mef3io.__main__ import main

    src, _, _ = rich
    dst = tmp_path / "cli.mefd"
    assert main(["transform", str(src), str(dst), "--exclude-channel", "ch3", "--set",
                 "subject_id=S-03", "--channel-description", "ch1=contact 1", "--json"]) == 0
    rep = json.loads(capsys.readouterr().out)
    assert rep["channels_kept"] == ["ch1", "ch2"] and rep["verification"]["ok"]
    b = _read_all(dst)
    assert b["ch1"][0]["subject_id"] == "S-03"
    assert b["ch1"][0]["channel_description"] == "contact 1"
    assert main(["transform", str(src), str(dst)]) == 2
    assert "error (dst-exists)" in capsys.readouterr().err


# --------------------------------------------------------------------------- review round 1


def test_a_source_without_a_record_index_is_copied(tmp_path):
    src = _rich_session(tmp_path / "jane_roe.mefd")
    (Path(src) / "ch1.timd" / "ch1.ridx").unlink()  # the copy rebuilds it
    rep = _transform(src, tmp_path / "copy.mefd")
    assert rep.verification.ok, rep.summary()
    assert (tmp_path / "copy.mefd" / "ch1.timd" / "ch1.ridx").exists()
    assert _transform(src, tmp_path / "again.mefd").verification.ok


def _reseal_uh(path):
    raw = bytearray(Path(path).read_bytes())
    struct.pack_into("<I", raw, 0, m.crc32(bytes(raw[4:UH])))
    Path(path).write_bytes(bytes(raw))
    return raw


def _poke(path, offset, fmt, value):
    raw = bytearray(Path(path).read_bytes())
    struct.pack_into(fmt, raw, offset, value)
    Path(path).write_bytes(bytes(raw))
    _reseal_uh(path)


@pytest.mark.parametrize("rel, offset, fmt, value, field", [
    ("study2.rdat", 40, "<q", 1, "maximum_entry_size"),
    ("study2.ridx", 32, "<q", 2, "number_of_entries"),
    ("ch1.timd/ch1-000000.segd/ch1-000000.tdat", 40, "<q", 1, "maximum_entry_size"),
    ("ch1.timd/ch1-000000.segd/ch1-000000.tidx", 40, "<q", 7, "maximum_entry_size"),
    ("study2.ridx", UH, "4s", b"Junk", "record index entry"),
    ("ch1.timd/ch1-000000.segd/ch1-000000.tidx", 820, "16s", None, "level_UUID"),
    ("ch1.timd/ch1-000000.segd/ch1-000000.tmet", 960 + 52, "<I", 99, "provenance region"),
])
def test_verify_catches_a_wrong_count_entry_uuid_or_provenance(tmp_path, rich, rel, offset, fmt,
                                                               value, field):
    src, _, _ = rich
    copy = tmp_path / "study2.mefd"
    assert _transform(src, copy).verification.ok
    if value is None:  # the source's own level UUID: no longer remapped
        value = (Path(src) / rel).read_bytes()[820:836]
        assert value != bytes(16)
    _poke(copy / rel, offset, fmt, value)
    v = mef3io.transform.verify(src, copy)
    assert not v.ok and any(field in p.field for p in v.problems), v.summary()


def test_a_records_own_not_encrypted_byte_is_kept(tmp_path, rich):
    src, _, _ = rich
    rdat = Path(src) / "jane_roe.rdat"
    stored = rdat.read_bytes()[UH + 11]  # the hand-built records store -1
    assert stored == 0xFF
    copy = tmp_path / "keep.mefd"
    _transform(src, copy)
    assert (copy / "keep.rdat").read_bytes()[UH + 11] == 0xFF
    assert (copy / "keep.ridx").read_bytes()[UH + 7] == 0xFF


def test_the_cli_points_per_channel_fields_to_their_options(tmp_path, rich, capsys):
    from mef3io.__main__ import main

    src, _, _ = rich
    with pytest.raises(SystemExit):
        main(["transform", str(src), str(tmp_path / "x.mefd"), "--set", "channel_descriptions=x"])
    assert "--channel-description CHANNEL=TEXT" in capsys.readouterr().err


# --------------------------------------------------------------------------- cut (#103)

SIX = 6 * SECOND


def _two_segment_session(path, password=False):
    """ch1: two segments (0-60 s, 120-150 s); ch2: one (0-60 s); notes in
    and out of a window, and a Seiz that straddles its start."""
    kw = dict(password1="pw-one", password2="pw-two") if password else {}
    with mef3io.Writer(str(path), overwrite=True, **kw) as w:
        for i, ch in enumerate(("ch1", "ch2")):
            w.write(ch, np.cumsum(np.random.default_rng(i).normal(size=256 * 60)).round(3),
                    START, 256.0, precision=3)
        w.write("ch1", np.arange(256 * 30) * 1.0, START + 120 * SECOND, 256.0, precision=3,
                new_segment=True)
        w.write_annotations([{"type": "Note", "time": START + 5 * SECOND, "text": "inside"},
                             {"type": "Note", "time": START + 50 * SECOND, "text": "outside"}])
    return Path(path)


def _same_over(src, dst, t0, t1, pw_src="", pw_dst=""):
    a, b = _read_all(src, pw_src), _read_all(dst, pw_dst)
    with mef3io.Reader(str(src), pw_src) as r, mef3io.Reader(str(dst), pw_dst) as c:
        for ch in c.channels:
            x, y = r.read(ch, t0, t1), c.read(ch, t0, t1)
            np.testing.assert_array_equal(x, y)
            assert c.info(ch)["start_time"] >= t0 and c.info(ch)["end_time"] <= t1
    return a, b


def test_an_exact_cut_holds_exactly_the_window(tmp_path):
    src = _two_segment_session(tmp_path / "p.mefd")
    t0, t1 = START + 3_123_456, START + 20 * SECOND
    rep = _transform(src, tmp_path / "cut.mefd", time_range=(t0, t1))
    assert rep.verification.ok, rep.summary()
    assert rep.blocks_trimmed > 0 and rep.records_outside_window == 1
    _same_over(src, tmp_path / "cut.mefd", t0, t1)
    with mef3io.Reader(str(tmp_path / "cut.mefd")) as c:
        assert [s["segment"] for s in c.segments("ch1")] == [0]
        assert [r["text"] for r in c.records(level="all") if r["type"] == "Note"] == ["inside"]
    assert mef3io.validate_session(str(tmp_path / "cut.mefd")).ok


def test_a_cut_across_segments_renumbers_them(tmp_path):
    src = _two_segment_session(tmp_path / "p.mefd")
    t0, t1 = START + 50 * SECOND, START + 130 * SECOND
    rep = _transform(src, tmp_path / "two.mefd", time_range=(t0, t1))
    assert rep.verification.ok, rep.summary()
    _same_over(src, tmp_path / "two.mefd", t0, t1)
    with mef3io.Reader(str(tmp_path / "two.mefd")) as c:
        segs = c.segments("ch1")
        assert [s["segment"] for s in segs] == [0, 1]
        assert segs[1]["start_sample"] == segs[0]["number_of_samples"]
    assert mef3io.validate_session(str(tmp_path / "two.mefd")).ok


def test_a_whole_block_cut_decodes_nothing(tmp_path):
    src = _two_segment_session(tmp_path / "p.mefd")
    t0, t1 = START + 3_123_456, START + 20 * SECOND
    rep = _transform(src, tmp_path / "whole.mefd", time_range=(t0, t1), exact=False)
    assert rep.verification.ok and rep.blocks_trimmed == 0
    with mef3io.Reader(str(src)) as r, mef3io.Reader(str(tmp_path / "whole.mefd")) as c:
        np.testing.assert_array_equal(r.read("ch2", t0, t1), c.read("ch2", t0, t1))
        assert c.info("ch2")["start_time"] <= t0  # a whole first block


def test_an_anonymized_cut(tmp_path):
    src = _two_segment_session(tmp_path / "p.mefd")
    t0, t1 = START + 3_123_456, START + 20 * SECOND
    shift = -7000 * 86_400 * SECOND
    rep = mef3io.anonymize_session(src, tmp_path / "S001.mefd", shift, time_range=(t0, t1))
    assert rep.verification.ok and rep.blocks_trimmed > 0
    with mef3io.Reader(str(src)) as r, mef3io.Reader(str(tmp_path / "S001.mefd")) as c:
        for ch in c.channels:
            np.testing.assert_array_equal(r.read(ch, t0, t1), c.read(ch, t0 + shift, t1 + shift))
    assert mef3io.anonymize.verify(src, tmp_path / "S001.mefd", shift, time_range=(t0, t1)).ok


def test_a_cut_of_an_encrypted_source_rekeys_the_trimmed_blocks(tmp_path):
    src = GOLDEN / "meflib_red_encrypted.mefd"
    with mef3io.Reader(str(src), "pass2") as r:
        info = r.info(r.channels[0])
        t0 = info["start_time"] + (info["end_time"] - info["start_time"]) // 3
        t1 = info["start_time"] + 2 * (info["end_time"] - info["start_time"]) // 3
    for name, out in (("re", ("a1", "a2")), ("de", None)):
        rep = _transform(src, tmp_path / f"{name}.mefd", password="pass2", output_password=out,
                         time_range=(t0, t1))
        assert rep.verification.ok, rep.summary()
        _same_over(src, tmp_path / f"{name}.mefd", t0, t1, "pass2", "a2" if out else "")


def test_a_tampered_trimmed_block_fails_verification(tmp_path):
    src = _two_segment_session(tmp_path / "p.mefd")
    t0, t1 = START + 3_123_456, START + 20 * SECOND
    copy = tmp_path / "cut.mefd"
    _transform(src, copy, time_range=(t0, t1))
    tdat = copy / "ch2.timd" / "ch2-000000.segd" / "ch2-000000.tdat"
    raw = bytearray(tdat.read_bytes())
    raw[UH + 304 + 20] ^= 0x01  # the first (trimmed) block's payload
    nb = struct.unpack_from("<I", raw, UH + 36)[0]
    struct.pack_into("<I", raw, UH, m.crc32(bytes(raw[UH + 4:UH + nb])))
    _reseal(raw)
    tdat.write_bytes(bytes(raw))
    v = mef3io.transform.verify(src, copy, time_range=(t0, t1))
    assert not v.ok and any(p.field in ("samples", "block") for p in v.problems), v.summary()


def test_cut_refusals(tmp_path, rich):
    src, _, _ = rich
    rep = mef3io.transform.check(src, tmp_path / "x.mefd", time_range=(START + SECOND, START))
    assert rep.codes == ("time-range-invalid",)
    rep = mef3io.transform.check(src, tmp_path / "x.mefd", time_range=(1, 2))
    assert rep.codes == ("time-range-empty",)
    with pytest.raises(TypeError) as exc:
        mef3io.transform.check(src, tmp_path / "x.mefd", time_range=(1.5, 2))
    assert exc.value.code == "argument-type"


def test_the_cli_cuts(tmp_path, capsys):
    from mef3io.__main__ import main

    src = _two_segment_session(tmp_path / "p.mefd")
    t0, t1 = START + 3_123_456, START + 20 * SECOND
    assert main(["transform", str(src), str(tmp_path / "c.mefd"), "--start", str(t0), "--end",
                 str(t1), "--json"]) == 0
    assert json.loads(capsys.readouterr().out)["blocks_trimmed"] > 0
