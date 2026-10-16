"""#70 — anonymize_session: a de-identified copy with every time shifted.

What is pinned here, on real files:

* every time field moves by exactly the shift and keeps its sign convention:
  universal headers of every file, .tidx entries, RED block headers, record
  headers and .ridx entries at all three levels, and the body times of Seiz
  (incl. per-channel onsets/offsets), Epoc and Curs;
* the time-zone fields are SET (offset 0, GMT 0, DST NO_ENTRY), not shifted;
* samples are bit-identical (mef3io and pymef), identity is gone, every file
  is stamped with provenance operation 6;
* encrypted input to plain and to re-encrypted output; meflib-encrypted RED
  blocks re-encrypted or decrypted, never silently downgraded;
* mef_tools-written sessions, tar input, sessions with meflib's
  recording-time offset (and its refusals);
* refusals leave nothing behind, cancellation leaves nothing behind;
* channel selection (exclude / include) leaves no trace of a left-out channel;
* verify's FIELD-WISE leak scan catches a time or name planted in any
  structured field, and reports nothing on a realistically sized clean copy.
"""
from __future__ import annotations

import datetime as dt
import shutil
import struct
import warnings
from pathlib import Path

import numpy as np
import pytest

import mef3io
from mef3io import _mef3io as m

FS = 256.0
SECOND = 1_000_000
DAY = 86_400 * SECOND
START = 1_577_880_000_000_000      # 2020-01-01T12:00:00Z
SHIFT = -7_000 * DAY               # ~ 2000-11 — well past the floor
UUTC_NO_ENTRY = -(2 ** 63)

UH = 1024
TMET = 16384
S2 = 2560
S3 = 13312
RHDR = 24
STATS = 48
L1_MASK, L2_MASK = 0x02, 0x04

SUBJECT = mef3io.Subject(name_1="Jane", name_2="Roe", id="MRN4711",
                         recording_location="Springfield General")
GOLDEN = Path(__file__).parent / "golden"


# --------------------------------------------------------------------------- helpers


def _fixed(s, n):
    b = s.encode("utf-8")
    assert len(b) < n
    return b + b"\0" * (n - len(b))


def _reseal(raw, body_end=None):
    body_end = len(raw) if body_end is None else body_end
    struct.pack_into("<I", raw, 4, m.crc32(bytes(raw[UH:body_end])))
    struct.pack_into("<I", raw, 0, m.crc32(bytes(raw[4:UH])))


def _record(rtype, body, disk_time, vmaj=1, vmin=0):
    body = bytes(body)
    body += b"\x7e" * ((-len(body)) % 16)
    hdr = bytearray(RHDR)
    hdr[4:8] = rtype.encode("ascii")
    hdr[9], hdr[10] = vmaj, vmin
    struct.pack_into("<b", hdr, 11, -1)
    struct.pack_into("<I", hdr, 12, len(body))
    struct.pack_into("<q", hdr, 16, disk_time)
    rec = bytearray(hdr) + body
    struct.pack_into("<I", rec, 0, m.crc32(bytes(rec[4:])))
    return bytes(rec)


def _install_records(rdat, records, disk_times):
    """Replace a .rdat's records with hand-built ones (and its .ridx to match)."""
    rdat = Path(rdat)
    raw = bytearray(rdat.read_bytes()[:UH])
    offsets = []
    for r in records:
        offsets.append(len(raw))
        raw += r
    struct.pack_into("<q", raw, 32, len(records))
    _reseal(raw)
    rdat.write_bytes(bytes(raw))
    ridx = rdat.with_suffix(".ridx")
    iraw = bytearray(ridx.read_bytes()[:UH])
    for r, off, t in zip(records, offsets, disk_times):
        e = bytearray(24)
        e[0:4] = r[4:8]
        e[5], e[6] = r[9], r[10]
        e[7] = r[11]
        struct.pack_into("<q", e, 8, off)
        struct.pack_into("<q", e, 16, t)
        iraw += e
    struct.pack_into("<q", iraw, 32, len(records))
    _reseal(iraw)
    ridx.write_bytes(bytes(iraw))


def _seiz_body(earliest, latest, channels):
    head = struct.pack("<qqqii", earliest, latest, latest - earliest, len(channels), 1)
    head += _fixed("Jane's aura", 128) + _fixed("marker-b", 128) + _fixed("focal onset", 1024)
    for name, on, off in channels:
        head += _fixed(name, 256) + struct.pack("<qq", on, off)
    return head


def _epoc_body(ts, end):
    return struct.pack("<qqqq", 42, ts, end, end - ts) + _fixed("sleep", 32) + _fixed("N2", 128)


def _curs_body(ts):
    return struct.pack("<qqqd", 7, ts, 1500, -12.5) + _fixed("cursor-1", 128)


def _signal(n, seed):
    rng = np.random.default_rng(seed)
    return np.cumsum(rng.normal(size=n)).round(3)


def _rich_session(path, password=False, channels=("ch1", "ch2", "ch3"), unknown=True):
    """Three channels (ch1 in two segments), subject metadata, records at all
    three levels including Seiz / Epoc / Curs bodies, an EDFA, an unknown
    type, and a session-level Seiz naming every channel."""
    kw = dict(password1="pw-one", password2="pw-two") if password else {}
    meta = mef3io.Metadata(subject=SUBJECT, acquisition=mef3io.Acquisition(
        session_description="Jane Roe overnight", channel_description="left hippocampus"))
    with mef3io.Writer(str(path), overwrite=True, metadata=meta, **kw) as w:
        for i, ch in enumerate(channels):
            w.write(ch, _signal(int(FS) * 60, i), START, FS, precision=3)
        w.write(channels[0], _signal(int(FS) * 30, 9), START + 120 * SECOND, FS, precision=3,
                new_segment=True)
        w.write_annotations([{"type": "Note", "time": START + SECOND, "text": "x"}])
        w.write_annotations([{"type": "Note", "time": START + 2 * SECOND, "text": "Jane slept"}],
                            channels[0])
    t = START + 5 * SECOND
    recs = [
        ("Note", b"Jane Roe pressed the button\0"),
        ("EDFA", struct.pack("<q", 7 * SECOND) + b"MRN4711 artefact\0"),
        ("Seiz", _seiz_body(t, t + 30 * SECOND,
                            [(c, t + SECOND, t + 20 * SECOND) for c in channels]
                            + [("unknown", -1, -1)])),
        ("Epoc", _epoc_body(t, t + 60 * SECOND)),
        ("Curs", _curs_body(t + 3 * SECOND)),
        ("LNTP", struct.pack("<q", 4) + struct.pack("<4i", 1, -2, 3, -4)),
    ]
    if unknown:  # pymef refuses (and crashes on) record types it does not know
        recs.append(("UnRc", b"opaque body that may hold anything 2020-01-01"))
    disk = -t
    _install_records(Path(path) / f"{Path(path).stem}.rdat",
                     [_record(rt, b, disk) for rt, b in recs], [disk] * len(recs))
    # Segment-level records: move the second channel's records into its segment.
    c2 = channels[1]
    with mef3io.Writer(str(path), **kw) as w:
        w.write_annotations([{"type": "Note", "time": START + 3 * SECOND, "text": "seg"}], c2)
    timd = Path(path) / f"{c2}.timd"
    for ext in ("rdat", "ridx"):
        shutil.move(timd / f"{c2}.{ext}", timd / f"{c2}-000000.segd" / f"{c2}-000000.{ext}")
    return Path(path)


def _all_records(path, password=""):
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        with mef3io.Reader(str(path), password) as r:
            return r.records(level="all")


def _read_all(path, password=""):
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        with mef3io.Reader(str(path), password) as r:
            return {ch: (r.info(ch), r.read(ch)) for ch in r.channels}


def _tmets(path):
    return sorted(Path(path).rglob("*.tmet"))


def _anon(src, dst, shift=SHIFT, **kw):
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        return mef3io.anonymize_session(src, dst, shift, **kw)


@pytest.fixture(scope="module")
def rich(tmp_path_factory):
    base = tmp_path_factory.mktemp("rich")
    src = _rich_session(base / "jane_roe.mefd")
    rep = _anon(src, base / "S001.mefd")
    return src, base / "S001.mefd", rep


# --------------------------------------------------------------------------- the copy


def test_every_time_moves_by_the_shift_and_samples_are_identical(rich):
    src, dst, rep = rich
    assert rep.verification.ok, rep.verification.summary()
    a, b = _read_all(src), _read_all(dst)
    assert sorted(a) == sorted(b) == ["ch1", "ch2", "ch3"]
    for ch in a:
        ia, xa = a[ch]
        ib, xb = b[ch]
        assert ib["start_time"] == ia["start_time"] + SHIFT
        assert ib["end_time"] == ia["end_time"] + SHIFT
        np.testing.assert_array_equal(xa, xb)
    with mef3io.Reader(str(src)) as r1, mef3io.Reader(str(dst)) as r2:
        for ch in r1.channels:
            for s1, s2 in zip(r1.segments(ch), r2.segments(ch)):
                assert s2["start_time"] == s1["start_time"] + SHIFT
                assert s2["provenance"]["last_operation"] == "anonymize"
            t1 = [e["start_uutc"] for e in r1.toc(ch)]
            t2 = [e["start_uutc"] for e in r2.toc(ch)]
            assert t2 == [t + SHIFT for t in t1]


def test_record_times_move_at_every_level_and_in_every_body(rich):
    src, dst, _ = rich
    ra, rb = _all_records(src), _all_records(dst)
    assert [(r["type"], r["level"], r["channel"]) for r in ra] == \
           [(r["type"], r["level"], r["channel"]) for r in rb]
    assert {r["level"] for r in rb} == {"session", "channel", "segment"}
    for x, y in zip(ra, rb):
        assert y["time"] == x["time"] + SHIFT
        for key in ("earliest_onset", "latest_offset", "timestamp", "end_timestamp",
                    "trace_timestamp"):
            if x.get(key) is not None:
                assert y[key] == x[key] + SHIFT, key
        if x["type"] == "Seiz":
            assert [c["name"] for c in y["channels"]] == [c["name"] for c in x["channels"]]
            for c1, c2 in zip(x["channels"], y["channels"]):
                for k in ("onset", "offset"):
                    assert c2[k] == (c1[k] if c1[k] == -1 else c1[k] + SHIFT)
            assert y["duration"] == x["duration"]


def test_raw_time_fields_keep_their_sign_and_tz_fields_are_set(rich):
    src, dst, _ = rich
    for t1, t2 in zip(_tmets(src), _tmets(dst)):
        a, b = t1.read_bytes(), t2.read_bytes()
        for off in (16, 24):
            (x,), (y,) = struct.unpack_from("<q", a, off), struct.unpack_from("<q", b, off)
            assert y == (-(-x + SHIFT) if x < 0 else x + SHIFT)   # same sign, shifted
        rto, dst_s, dst_e, gmt = struct.unpack_from("<qqqi", b, S3)
        assert (rto, dst_s, dst_e, gmt) == (0, UUTC_NO_ENTRY, UUTC_NO_ENTRY, 0)
        assert b[S3 + 28:S3 + 924] == b"\0" * (924 - 28)      # subject + location cleared
        assert b[S2 + 6432:S2 + 10752] == b"\0" * (10752 - 6432)
    # the index and block copies, raw
    for tidx in Path(dst).rglob("*.tidx"):
        raw = tidx.read_bytes()
        tdat = tidx.with_suffix(".tdat").read_bytes()
        for off in range(UH, len(raw), 56):
            (fo, t) = struct.unpack_from("<qq", raw, off)
            assert t < 0 and struct.unpack_from("<q", tdat, fo + 40)[0] == t


def test_time_zone_fields_are_set_whatever_the_source_held(tmp_path):
    src = tmp_path / "tz.mefd"
    meta = mef3io.Metadata(subject=mef3io.Subject(gmt_offset=-21600))
    with mef3io.Writer(str(src), overwrite=True, metadata=meta) as w:
        w.write("ch1", _signal(2560, 1), START, FS, precision=3)
    for tmet in _tmets(src):                       # and DST fields, as a recorder fills them
        raw = bytearray(tmet.read_bytes())
        struct.pack_into("<qq", raw, S3 + 8, START - 30 * DAY, START + 30 * DAY)
        _reseal(raw, TMET)
        tmet.write_bytes(bytes(raw))
    assert struct.unpack_from("<i", _tmets(src)[0].read_bytes(), S3 + 24)[0] == -21600
    rep = _anon(src, tmp_path / "TZ.mefd")
    assert rep.verification.ok
    for tmet in _tmets(tmp_path / "TZ.mefd"):
        assert struct.unpack_from("<qqqi", tmet.read_bytes(), S3) == \
            (0, UUTC_NO_ENTRY, UUTC_NO_ENTRY, 0)


def test_identity_is_gone_and_names_follow_the_copy(rich):
    src, dst, rep = rich
    assert rep.session_name == "S001" and rep.pseudonym == "S001"
    assert (dst / "S001.rdat").exists() and not (dst / "jane_roe.rdat").exists()
    with mef3io.Reader(str(dst)) as r:
        info = r.info("ch1")
        for k in ("subject_name_1", "subject_name_2", "subject_id", "recording_location"):
            assert info[k] == ""
        assert info["session_description"] == "" and info["channel_description"] == ""
        assert info["gmt_offset"] == 0 if "gmt_offset" in info else True
    # Text that would identify: nowhere in any byte of the copy. (A text scan
    # is fine for names; it is TIMES that must never be scanned bytewise.)
    blob = b"".join(p.read_bytes() for p in Path(dst).rglob("*") if p.is_file())
    for s in (b"jane_roe", b"Jane", b"Roe", b"MRN4711", b"Springfield", b"opaque body"):
        assert s not in blob, s
    # UUIDs regenerated, provenance stamped everywhere
    uuids = set()
    for p in Path(src).rglob("*"):
        if p.is_file():
            h = p.read_bytes()[:UH]
            uuids |= {h[820:836], h[836:852], h[852:868]}
    uuids.discard(b"\0" * 16)
    for p in Path(dst).rglob("*"):
        if p.is_file():
            h = p.read_bytes()[:UH]
            assert not ({h[820:836], h[836:852], h[852:868]} & uuids)
            assert h[960:964] == b"M3IO" and h[965] == 6     # last operation: anonymize
            assert h[900:960] == b"\0" * 60


def test_text_is_dropped_but_records_are_kept(rich):
    src, dst, rep = rich
    recs = _all_records(dst)
    note = [r for r in recs if r["type"] == "Note" and r["level"] == "session"]
    assert note[0]["text"] == ""
    seiz = next(r for r in recs if r["type"] == "Seiz")
    assert seiz["marker_name_1"] == "" and seiz["annotation"] == ""
    assert next(r for r in recs if r["type"] == "Epoc")["text"] == ""
    assert next(r for r in recs if r["type"] == "Curs")["name"] == ""
    xxxx = next(r for r in recs if r["type"] == "UnRc")    # no layout: body not carried
    assert rep.record_bodies_cleared == 1 and xxxx["time"] is not None
    lntp = next(r for r in recs if r["type"] == "LNTP")
    assert lntp["template"] == [1, -2, 3, -4]


def test_pymef_reads_the_copy(tmp_path):
    pytest.importorskip("pymef")
    from pymef.mef_session import MefSession

    src = _rich_session(tmp_path / "jane_roe.mefd", unknown=False)
    dst = tmp_path / "S001.mefd"
    _anon(src, dst)
    for ch in ("ch1", "ch2", "ch3"):
        n = _read_all(src)[ch][0]["number_of_samples"]
        out = []
        for p in (src, dst):
            s = MefSession(str(p), "")
            try:
                out.append(np.asarray(s.read_ts_channels_sample(ch, [0, n])))
            finally:
                s.close()
        np.testing.assert_array_equal(out[0], out[1])
    times = []
    for p in (src, dst):
        s = MefSession(str(p), "")
        try:
            times.append([int(r["time"]) for r in s.read_records()])
        finally:
            s.close()
    assert times[1] == [t + SHIFT for t in times[0]] and times[0]


def test_keep_and_callable_text_policies(tmp_path):
    src = _rich_session(tmp_path / "jane_roe.mefd")
    _anon(src, tmp_path / "K.mefd", text="keep")
    recs = _all_records(tmp_path / "K.mefd")
    note = next(r for r in recs if r["type"] == "Note" and r["level"] == "session")
    assert note["text"] == "  pressed the button"            # names redacted, rest kept
    edfa = next(r for r in recs if r["type"] == "EDFA")
    assert edfa["text"] == " artefact" and edfa["duration"] == 7 * SECOND
    seen = []

    def upper(text, info):
        seen.append((info["record_type"], info["field"], info["level"]))
        return text.upper()

    _anon(src, tmp_path / "C.mefd", text=upper)
    recs = _all_records(tmp_path / "C.mefd")
    assert next(r for r in recs if r["type"] == "Epoc")["epoch_type"] == "SLEEP"
    # redaction still applies after the callable
    assert next(r for r in recs if r["type"] == "Note" and r["level"] == "session")["text"] \
        == "  PRESSED THE BUTTON"
    assert ("Seiz", "annotation", "session") in seen


def test_a_raising_text_callable_aborts_and_leaves_nothing(tmp_path):
    src = _rich_session(tmp_path / "jane_roe.mefd")

    def boom(text, info):
        raise KeyError("nope")

    with pytest.raises(KeyError):
        _anon(src, tmp_path / "B.mefd", text=boom)
    assert not (tmp_path / "B.mefd").exists() and not (tmp_path / "B.mefd.partial").exists()


def test_descriptions_kept_get_the_new_session_name(tmp_path):
    src = tmp_path / "jane_roe.mefd"
    with mef3io.Writer(str(src), overwrite=True) as w:      # session_description = the name
        w.write("ch1", _signal(2560, 1), START, FS, precision=3)
    _anon(src, tmp_path / "P7.mefd", clear=("subject", "location"))
    with mef3io.Reader(str(tmp_path / "P7.mefd")) as r:
        assert r.info("ch1")["session_description"] == "P7"
        assert r.info("ch1")["channel_description"] == "ch1"


# --------------------------------------------------------------------------- encryption


def test_encrypted_input_to_plain_and_to_reencrypted_output(tmp_path):
    src = _rich_session(tmp_path / "enc.mefd", password=True)
    with pytest.raises(mef3io.PasswordError):
        _anon(src, tmp_path / "X.mefd", password="pw-one")          # level 1 only
    assert not (tmp_path / "X.mefd").exists()
    _anon(src, tmp_path / "plain.mefd", password="pw-two")
    a = _read_all(src, "pw-two")
    b = _read_all(tmp_path / "plain.mefd")
    for ch in a:
        np.testing.assert_array_equal(a[ch][1], b[ch][1])
    rep = _anon(src, tmp_path / "enc2.mefd", password="pw-two",
                output_password=("new-one", "new-two"))
    assert rep.output_encrypted and rep.verification.ok
    c = _read_all(tmp_path / "enc2.mefd", "new-two")
    for ch in a:
        np.testing.assert_array_equal(a[ch][1], c[ch][1])
    with pytest.raises(mef3io.PasswordError):
        mef3io.Reader(str(tmp_path / "enc2.mefd"), "pw-two").info("ch1")
    tmet = _tmets(tmp_path / "enc2.mefd")[0].read_bytes()
    assert tmet[1024] == 1 and tmet[1025] == 2                     # s2 L1, s3 L2
    with pytest.raises(ValueError):
        _anon(src, tmp_path / "Y.mefd", password="pw-two", output_password="only-one")


def test_meflib_encrypted_red_blocks_are_reencrypted_or_decrypted(tmp_path):
    src = GOLDEN / "meflib_red_encrypted.mefd"
    re = _anon(src, tmp_path / "re.mefd", password="pass2", output_password=("a1", "a2"))
    de = _anon(src, tmp_path / "de.mefd", password="pass2")
    assert re.blocks_reencrypted == re.blocks > 0 and de.blocks_decrypted == de.blocks
    for ch, mask in (("l1_blocks", L1_MASK), ("l2_blocks", L2_MASK)):
        for path, want in ((tmp_path / "re.mefd", mask), (tmp_path / "de.mefd", 0)):
            tdat = next(Path(path).glob(f"{ch}.timd/*.segd/*.tdat")).read_bytes()
            assert tdat[UH + 4] & (L1_MASK | L2_MASK) == want    # level kept, or decrypted
    # Samples equal to the source, decoded with the source's password.
    a = _read_all(src, "pass2")
    for path, pw in ((tmp_path / "re.mefd", "a2"), (tmp_path / "de.mefd", "")):
        b = _read_all(path, pw)
        for ch in a:
            np.testing.assert_array_equal(a[ch][1], b[ch][1])


def test_decrypted_blocks_are_readable_by_pymef(tmp_path):
    pytest.importorskip("pymef")
    from pymef.mef_session import MefSession

    src = GOLDEN / "meflib_red_encrypted.mefd"
    truth = np.load(GOLDEN / "meflib_red_encrypted.npz")  # int32, decoded before encryption
    _anon(src, tmp_path / "de.mefd", password="pass2")
    s = MefSession(str(tmp_path / "de.mefd"), "")
    try:
        for ch in ("l1_blocks", "l2_blocks"):
            want = np.asarray(truth[ch]).ravel()
            y = np.asarray(s.read_ts_channels_sample(ch, [0, want.size])).ravel()
            np.testing.assert_array_equal(y.astype(np.int64), want.astype(np.int64))
    finally:
        s.close()


# --------------------------------------------------------------------------- other inputs


def _legacy(path, rto=0, chans=("ch1", "ch2")):
    pytest.importorskip("mef_tools", reason="legacy oracle not installed")
    pd = pytest.importorskip("pandas")
    from mef_tools.io import MefWriter

    w = MefWriter(str(path), overwrite=True)
    if rto:
        w.record_offset = rto
    for ch in chans:
        w.write_data(np.arange(5000, dtype=np.int32) * 1.0, ch, START, FS, precision=0)
    w.write_annotations(pd.DataFrame([{"time": START + SECOND, "type": "Note", "text": "s"}]))
    del w
    return Path(path)


def test_mef_tools_session(tmp_path):
    src = _legacy(tmp_path / "legacy.mefd")
    rep = _anon(src, tmp_path / "L.mefd")
    assert rep.verification.ok
    a, b = _read_all(src), _read_all(tmp_path / "L.mefd")
    for ch in a:
        np.testing.assert_array_equal(a[ch][1], b[ch][1])
        assert b[ch][0]["start_time"] == a[ch][0]["start_time"] + SHIFT
    assert _all_records(tmp_path / "L.mefd")[0]["time"] == _all_records(src)[0]["time"] + SHIFT


def test_tar_input_is_read_in_place(tmp_path, rich):
    src, _, _ = rich
    tar = mef3io.archive_session(str(src), str(tmp_path / "jane_roe.mefd.tar"))
    rep = _anon(tar, tmp_path / "T.mefd")
    assert rep.verification.ok
    a, b = _read_all(src), _read_all(tmp_path / "T.mefd")
    for ch in a:
        np.testing.assert_array_equal(a[ch][1], b[ch][1])


def test_a_recording_time_offset_session_is_accepted(tmp_path):
    rto = START - 6 * 3600 * SECOND        # meflib-style: before the start
    src = _legacy(tmp_path / "rto.mefd", rto=rto)
    rep = _anon(src, tmp_path / "R.mefd")
    assert rep.source_recording_time_offset == rto and rep.verification.ok
    a, b = _read_all(src), _read_all(tmp_path / "R.mefd")
    for ch in a:
        assert a[ch][0]["start_time"] == START
        assert b[ch][0]["start_time"] == START + SHIFT
        assert b[ch][0]["recording_time_offset"] == 0
        np.testing.assert_array_equal(a[ch][1], b[ch][1])
    assert _all_records(tmp_path / "R.mefd")[0]["time"] == START + SECOND + SHIFT


def _set_rto(path, rto, channels):
    for tmet in sorted(Path(path).glob("*.timd/*.segd/*.tmet")):
        if tmet.parent.parent.name[:-5] not in channels:
            continue
        raw = bytearray(tmet.read_bytes())
        struct.pack_into("<q", raw, S3, rto)
        _reseal(raw, TMET)
        tmet.write_bytes(bytes(raw))


def test_offset_refusals(tmp_path):
    # Segments disagree on the offset.
    src = tmp_path / "dis.mefd"
    with mef3io.Writer(str(src), overwrite=True) as w:
        for ch in ("a", "b"):
            w.write(ch, _signal(2560, 1), START, FS, precision=3)
    _set_rto(src, START - DAY, ["a"])
    with pytest.raises(mef3io.WriteConflictError):
        _anon(src, tmp_path / "D.mefd")
    # A time at or before the offset (the legacy positive-delta convention).
    src2 = _legacy(tmp_path / "late.mefd", rto=START + 3600 * SECOND)
    with pytest.raises(mef3io.WriteConflictError):
        _anon(src2, tmp_path / "E.mefd")
    # The offset cannot be read: level-1 password on an encrypted session.
    src3 = _rich_session(tmp_path / "enc.mefd", password=True)
    with pytest.raises(mef3io.PasswordError):
        _anon(src3, tmp_path / "F.mefd", password="pw-one")
    for d in ("D", "E", "F"):
        assert not (tmp_path / f"{d}.mefd").exists()
        assert not (tmp_path / f"{d}.mefd.partial").exists()


# --------------------------------------------------------------------------- refusals


def test_shift_refusals(tmp_path, rich):
    src, _, _ = rich
    for bad in (0, 3600 * SECOND, -(START + DAY)):
        with pytest.raises(ValueError):
            _anon(src, tmp_path / "Z.mefd", shift=bad)
    assert not (tmp_path / "Z.mefd").exists()
    with pytest.raises(ValueError):
        _anon(src, tmp_path / "Z.mefd", clear=("subject", "nonsense"))


def test_destination_refusals(tmp_path, rich):
    src, dst, _ = rich
    with pytest.raises(mef3io.IoError):
        _anon(src, dst)                                       # exists
    with pytest.raises(mef3io.IoError):
        _anon(src, src / "inner.mefd")                        # inside the source
    with pytest.raises(mef3io.IoError):
        _anon(src, tmp_path / "noext")                        # not .mefd
    (tmp_path / "busy.mefd.partial").mkdir()
    with pytest.raises(mef3io.IoError):
        _anon(src, tmp_path / "busy.mefd")
    with pytest.raises(ValueError):
        _anon(src, tmp_path / "other.mefd", rename=False)
    rep = _anon(src, tmp_path / "jane_roe.mefd", rename=False)
    assert rep.session_name == "jane_roe" and rep.verification.ok


def test_cancel_leaves_nothing_behind(tmp_path, rich):
    src, _, _ = rich
    token = mef3io.CancelToken()
    calls = []

    def progress(done, total):
        calls.append(done)
        if done > 0:
            token.cancel()

    with pytest.raises(mef3io.Cancelled):
        _anon(src, tmp_path / "C.mefd", cancel=token, progress=progress)
    assert calls and not (tmp_path / "C.mefd").exists()
    assert not (tmp_path / "C.mefd.partial").exists()
    # a token cancelled up front: nothing at all
    with pytest.raises(mef3io.Cancelled):
        _anon(src, tmp_path / "C.mefd", cancel=token)
    assert list(tmp_path.iterdir()) == []


def test_progress_reaches_total_once(tmp_path, rich):
    src, _, _ = rich
    seen = []
    _anon(src, tmp_path / "G.mefd", progress=lambda d, t: seen.append((d, t)))
    assert seen[0][0] == 0 and seen[-1][0] == seen[-1][1]
    assert [d for d, _ in seen] == sorted(d for d, _ in seen)


# --------------------------------------------------------------------------- channel selection


def test_exclude_a_channel_leaves_no_trace(tmp_path):
    src = _rich_session(tmp_path / "jane_roe.mefd", channels=("LAH1", "LAH12", "RAH3"))
    rep = _anon(src, tmp_path / "X.mefd", exclude_channels=["LAH1"], text="keep")
    assert rep.channels_kept == ("LAH12", "RAH3") and rep.channels_excluded == ("LAH1",)
    assert rep.seizure_channel_entries_dropped == 1 and rep.verification.ok
    assert rep.verification.excluded_channels == ("LAH1",)
    dst = tmp_path / "X.mefd"
    assert not (dst / "LAH1.timd").exists()
    seiz = next(r for r in _all_records(dst) if r["type"] == "Seiz")
    assert [c["name"] for c in seiz["channels"]] == ["LAH12", "RAH3", "unknown"]
    assert seiz["number_of_channels"] == 3
    # The name is nowhere as a word — LAH12 (a kept channel) contains it as a
    # prefix, which is not the excluded channel.
    import re
    for p in dst.rglob("*"):
        if p.is_file():
            assert not re.search(rb"(?<![A-Za-z0-9_])LAH1(?![A-Za-z0-9_])", p.read_bytes()), p


def test_include_a_subset_and_pymef_reads_it(tmp_path):
    src = _rich_session(tmp_path / "jane_roe.mefd", unknown=False)
    rep = _anon(src, tmp_path / "I.mefd", channels=["ch2", "ch3"])
    assert rep.channels_kept == ("ch2", "ch3") and rep.channels_excluded == ("ch1",)
    with mef3io.Reader(str(tmp_path / "I.mefd")) as r:
        assert r.channels == ["ch2", "ch3"]
    pytest.importorskip("pymef")
    from pymef.mef_session import MefSession

    s = MefSession(str(tmp_path / "I.mefd"), "")
    try:
        assert sorted(s.session_md["time_series_channels"]) == ["ch2", "ch3"]
        n = _read_all(src)["ch2"][0]["number_of_samples"]
        y = np.asarray(s.read_ts_channels_sample("ch2", [0, n])).ravel()
        assert y.size == n
    finally:
        s.close()


def test_channel_selection_errors(tmp_path, rich):
    src, _, _ = rich
    with pytest.raises(ValueError):
        _anon(src, tmp_path / "E.mefd", channels=["ch1"], exclude_channels=["ch2"])
    with pytest.raises(ValueError):
        _anon(src, tmp_path / "E.mefd", channels=["nope"])
    with pytest.raises(ValueError):
        _anon(src, tmp_path / "E.mefd", exclude_channels=["nope"])
    with pytest.raises(ValueError):
        _anon(src, tmp_path / "E.mefd", exclude_channels=["ch1", "ch2", "ch3"])
    with pytest.raises(ValueError):
        _anon(src, tmp_path / "E.mefd", channels=[])
    assert not (tmp_path / "E.mefd").exists()


# --------------------------------------------------------------------------- verify


def _copy(dst, tmp_path, name):
    out = tmp_path / name / "S001.mefd"
    shutil.copytree(dst, out)
    return out


def _plant_uh(path, rel, off, value, body_end=None):
    p = path / rel
    raw = bytearray(p.read_bytes())
    struct.pack_into("<q", raw, off, value)
    _reseal(raw, body_end)
    p.write_bytes(bytes(raw))


def _first(path, pattern):
    return next(Path(path).glob(pattern)).relative_to(path)


def _plant_block_time(path, value):
    tdat = path / _first(path, "ch1.timd/ch1-000000.segd/*.tdat")
    raw = bytearray(tdat.read_bytes())
    n = struct.unpack_from("<I", raw, UH + 36)[0]
    struct.pack_into("<q", raw, UH + 40, value)
    struct.pack_into("<I", raw, UH, m.crc32(bytes(raw[UH + 4:UH + n])))
    _reseal(raw)
    tdat.write_bytes(bytes(raw))


def _plant_record(path, field, value):
    rdat = path / "S001.rdat"
    raw = bytearray(rdat.read_bytes())
    off, i = UH, 0
    while off < len(raw):
        rtype = bytes(raw[off + 4:off + 8])
        n = struct.unpack_from("<I", raw, off + 12)[0]
        b = off + RHDR
        if field == "header" and i == 0:
            struct.pack_into("<q", raw, off + 16, value)
        elif field == "seiz" and rtype == b"Seiz":
            struct.pack_into("<q", raw, b + 1312 + 256, value)   # channel 0 onset
        elif field == "epoc" and rtype == b"Epoc":
            struct.pack_into("<q", raw, b + 16, value)
        elif field == "curs" and rtype == b"Curs":
            struct.pack_into("<q", raw, b + 8, value)
        elif field == "text" and rtype == b"Epoc":
            raw[b + 64:b + 64 + 8] = b"MRN4711\0"
        else:
            off, i = b + n, i + 1
            continue
        struct.pack_into("<I", raw, off, m.crc32(bytes(raw[off + 4:b + n])))
        break
    _reseal(raw)
    rdat.write_bytes(bytes(raw))


def _plant_ridx(path, value):
    p = path / "S001.ridx"
    raw = bytearray(p.read_bytes())
    struct.pack_into("<q", raw, UH + 16, value)
    _reseal(raw)
    p.write_bytes(bytes(raw))


def _plant_s3(path, off, value, fmt="<q"):
    p = path / _first(path, "ch1.timd/ch1-000000.segd/*.tmet")
    raw = bytearray(p.read_bytes())
    struct.pack_into(fmt, raw, S3 + off, value)
    _reseal(raw, TMET)
    p.write_bytes(bytes(raw))


def _plant_text(path, where, text):
    p = path / _first(path, "ch1.timd/ch1-000000.segd/*.tmet")
    raw = bytearray(p.read_bytes())
    off = {"channel_description": S2, "subject_name_1": S3 + 28,
           "session_name": 308, "anonymized_name": 564}[where]
    raw[off:off + len(text) + 1] = text + b"\0"
    _reseal(raw, TMET)
    p.write_bytes(bytes(raw))


T_NEG = -(START + 10 * SECOND)        # a source time, stored negated
T_POS = START + 10 * SECOND           # ... and plain

PLANTS = {
    "tmet start (negated)": lambda p: _plant_uh(p, _first(p, "ch1.timd/*/*.tmet"), 16, T_NEG, TMET),
    "tmet end (plain)": lambda p: _plant_uh(p, _first(p, "ch1.timd/*/*.tmet"), 24, T_POS, TMET),
    "tidx header start": lambda p: _plant_uh(p, _first(p, "ch2.timd/*/*.tidx"), 16, T_NEG),
    "tdat header end": lambda p: _plant_uh(p, _first(p, "ch2.timd/*/*.tdat"), 24, T_NEG),
    "tidx entry time": lambda p: _plant_uh(p, _first(p, "ch3.timd/*/*.tidx"), UH + 8, T_NEG),
    "RED block time": lambda p: _plant_block_time(p, T_NEG),
    "record header time": lambda p: _plant_record(p, "header", T_NEG),
    "ridx time": lambda p: _plant_ridx(p, T_NEG),
    "Seiz channel onset": lambda p: _plant_record(p, "seiz", T_POS),
    "Epoc end": lambda p: _plant_record(p, "epoc", T_POS),
    "Curs trace": lambda p: _plant_record(p, "curs", T_POS),
    "rdat header start": lambda p: _plant_uh(p, Path("S001.rdat"), 16, T_NEG),
    "recording_time_offset": lambda p: _plant_s3(p, 0, START - 12 * 3600 * SECOND),
    "DST start": lambda p: _plant_s3(p, 8, T_POS),
    "GMT offset": lambda p: _plant_s3(p, 24, -21600, "<i"),
    "record text": lambda p: _plant_record(p, "text", 0),
    "channel_description": lambda p: _plant_text(p, "channel_description", b"Jane"),
    "subject_name_1": lambda p: _plant_text(p, "subject_name_1", b"Roe"),
    "session_name": lambda p: _plant_text(p, "session_name", b"jane_roe"),
}


@pytest.mark.parametrize("name", list(PLANTS))
def test_verify_catches_a_planted_leak(tmp_path, rich, name):
    src, dst, _ = rich
    bad = _copy(dst, tmp_path, "bad")
    PLANTS[name](bad)
    rep = mef3io.anonymize.verify(src, bad, SHIFT)
    assert not rep.ok, name
    assert rep.problems
    # nothing in the report repeats an identifying string
    for p in rep.problems:
        for s in ("Jane", "Roe", "MRN4711", "jane_roe"):
            assert s not in p.message


def test_verify_clean_copy_and_wrong_shift(rich):
    src, dst, _ = rich
    rep = mef3io.anonymize.verify(src, dst, SHIFT)
    assert rep.ok and not rep.leaks and rep.times_checked > 50
    wrong = mef3io.anonymize.verify(src, dst, SHIFT + SECOND)
    assert not wrong.ok


def test_no_false_positives_on_a_realistic_copy(tmp_path):
    """4 channels x 2 h at 512 Hz, appended in 10-minute blocks with a gap:
    hundreds of blocks whose compressed payload is effectively random bytes.
    The field-wise scan must report nothing."""
    src = tmp_path / "big.mefd"
    fs = 512.0
    with mef3io.Writer(str(src), overwrite=True) as w:
        for k in range(12):
            t = START + k * 600 * SECOND + (3600 * SECOND if k >= 6 else 0)
            for c in range(4):
                w.write(f"c{c}", _signal(int(fs) * 600, 100 * k + c) * 50, t, fs, precision=2)
    rep = _anon(src, tmp_path / "B.mefd", shift=-(5000 * DAY + 3 * 3600 * SECOND))
    v = rep.verification
    assert v.ok and not v.leaks and v.blocks > 500, v.summary()


# --------------------------------------------------------------------------- CLI + helpers


def test_reference_shift():
    start = int(dt.datetime(2031, 5, 14, 13, 47, 5, tzinfo=dt.timezone.utc).timestamp() * 1e6)
    utc = mef3io.anonymize.reference_shift(start, "2000-01-03")
    assert dt.datetime.fromtimestamp((start + utc) / 1e6, dt.timezone.utc) == \
        dt.datetime(2000, 1, 3, 13, 47, 5, tzinfo=dt.timezone.utc)
    loc = mef3io.anonymize.reference_shift(start, "2000-01-03", tz="America/Chicago")
    assert dt.datetime.fromtimestamp((start + loc) / 1e6, dt.timezone.utc) == \
        dt.datetime(2000, 1, 3, 8, 47, 5, tzinfo=dt.timezone.utc)   # CDT: UTC-5
    wk = mef3io.anonymize.reference_shift(start, "2000-01-03", keep_weekday=True)
    a = dt.datetime.fromtimestamp(start / 1e6, dt.timezone.utc)
    b = dt.datetime.fromtimestamp((start + wk) / 1e6, dt.timezone.utc)
    assert a.weekday() == b.weekday() and abs((b.date() - dt.date(2000, 1, 3)).days) <= 3


def test_cli(tmp_path, rich, capsys):
    from mef3io.__main__ import main

    src, _, _ = rich
    assert main(["anonymize", str(src), str(tmp_path / "A.mefd"), "--shift-us", str(SHIFT)]) == 0
    assert "verification: passed" in capsys.readouterr().out
    assert main(["anonymize", str(src), str(tmp_path / "B.mefd"), "--reference-date",
                 "2000-01-03", "--tz", "Europe/Prague", "--exclude-channel", "ch3",
                 "--json"]) == 0
    with mef3io.Reader(str(tmp_path / "B.mefd")) as r:
        assert r.channels == ["ch1", "ch2"]
        start = dt.datetime.fromtimestamp(r.info("ch1")["start_time"] / 1e6, dt.timezone.utc)
    assert start.date() == dt.date(2000, 1, 3) and start.hour == 13   # 12:00 UTC = 13:00 CET
    assert main(["anonymize", str(src), str(tmp_path / "C.mefd"), "--shift-us", "5"]) == 2
    with pytest.raises(SystemExit) as exc:                     # argparse: mutually exclusive
        main(["anonymize", str(src), str(tmp_path / "D.mefd"), "--shift-us", str(SHIFT),
              "--channel", "ch1", "--exclude-channel", "ch2"])
    assert exc.value.code == 2
    assert not (tmp_path / "C.mefd").exists() and not (tmp_path / "D.mefd").exists()
