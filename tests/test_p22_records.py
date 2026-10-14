"""P22: record (annotation) handling — times, levels and bodies.

Four defects, each reproduced here before it was fixed:

* #60 — session-level records were parsed with a recording-time offset of 0
  whatever section 3 said, so on any session with a non-zero offset they
  landed ``rto`` away from the data they annotate. Silently.
* #61 — MEF 3.0 keeps records at THREE levels (session ``.mefd``, channel
  ``.timd`` and segment ``.segd``, meflib.c:6343 / 5660 / 6030). The segment
  level was never read, and nothing said it had been skipped.
* #62 — only Note / SyLg / EDFA and the first three ``Seiz`` fields were
  decoded; every other body defined in meflib's ``mefrec.h`` came back as a
  bare type + time.
* #63 (C++ side, see core/tests) — one time-conversion definition.

The body-time decision (#62) is pinned in ``test_body_times_*``: body times go
through the SAME sign-aware conversion as the header time. A non-negative
body time is returned unchanged (meflib's mefrec.c never applies the offset to
a body, so that is the normal case even in an offset session); a negative one
is resolved with the record's offset, exactly as meflib's own display path
does (mefrec.c:377 etc. -> local_date_time_string -> remove_recording_time_
offset, meflib.c:4649). Documented sentinels (-1 for a Seiz channel onset /
offset, mefrec.h:129/131, and UUTC_NO_ENTRY) pass through untouched.
"""
from __future__ import annotations

import shutil
import struct
import sys
import warnings
from pathlib import Path

import numpy as np
import pytest

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO))

import mef3io  # noqa: E402
from mef3io import _mef3io as m  # noqa: E402

FS = 256.0
START = 1577836800000000          # 2020-01-01T00:00:00Z
SECOND = 1_000_000
THREE_DAYS = 3 * 86400 * SECOND
UUTC_NO_ENTRY = -(2 ** 63)

UH_BYTES = 1024
METADATA_FILE_BYTES = 16384
S3_RTO_OFFSET = 13312             # UH 1024 + s1 1536 + s2 10752: section 3, field 0 (si8)
RECORD_HEADER_BYTES = 24


# --------------------------------------------------------------------------- helpers


def _write_session(path, channels=("ch1", "ch2"), session_text="session",
                   channel_text="channel", annotate=True):
    with mef3io.Writer(str(path), overwrite=True) as w:
        for ch in channels:
            w.write(ch, np.sin(np.arange(2560) / 10.0), START, FS, precision=3)
        if annotate:
            w.write_annotations([{"type": "Note", "time": START + SECOND, "text": session_text}])
            w.write_annotations(
                [{"type": "Note", "time": START + SECOND, "text": channel_text}], channels[0])


def _reseal(raw, body_end):
    """Body CRC over [1024, body_end), then the header CRC over [4, 1024)."""
    struct.pack_into("<I", raw, 4, m.crc32(bytes(raw[UH_BYTES:body_end])))
    struct.pack_into("<I", raw, 0, m.crc32(bytes(raw[4:UH_BYTES])))


def _set_rto(path, rto, channels=None, segments=None):
    """Give section 3 of every (or the named channels' / segment numbers')
    .tmet this offset.

    The fixed-length .tmet body CRC is bounded by METADATA_FILE_BYTES — never
    to EOF (CLAUDE.md, ".tmet is a FIXED-length record")."""
    for tmet in sorted(Path(path).glob("*.timd/*.segd/*.tmet")):
        if channels is not None and tmet.parent.parent.name[:-5] not in channels:
            continue
        if segments is not None and int(tmet.stem.rsplit("-", 1)[1]) not in segments:
            continue
        raw = bytearray(tmet.read_bytes())
        struct.pack_into("<q", raw, S3_RTO_OFFSET, rto)
        _reseal(raw, METADATA_FILE_BYTES)
        tmet.write_bytes(bytes(raw))


def _record(rtype, body, disk_time, vmaj=1, vmin=0):
    """One record exactly as meflib lays it out (meflib.h:517-530): 24-byte
    header, body padded to a 16-byte multiple with 0x7e, CRC over [4, end)."""
    body = bytes(body)
    body += b"\x7e" * ((-len(body)) % 16)
    hdr = bytearray(RECORD_HEADER_BYTES)
    hdr[4:8] = rtype.encode("ascii")
    hdr[9], hdr[10] = vmaj, vmin
    struct.pack_into("<b", hdr, 11, -1)            # not encrypted
    struct.pack_into("<I", hdr, 12, len(body))
    struct.pack_into("<q", hdr, 16, disk_time)
    rec = bytearray(hdr) + body
    struct.pack_into("<I", rec, 0, m.crc32(bytes(rec[4:])))
    return bytes(rec)


def _install_records(rdat, records):
    """Replace a mef3io-written .rdat's records with hand-built ones, keeping
    its universal header (session name, password fields) and resealing it."""
    raw = bytearray(rdat.read_bytes()[:UH_BYTES]) + b"".join(records)
    struct.pack_into("<q", raw, 32, len(records))   # number_of_entries
    _reseal(raw, len(raw))
    rdat.write_bytes(bytes(raw))


def _fixed(s, n):
    b = s.encode("utf-8")
    assert len(b) < n
    return b + b"\0" * (n - len(b))


def _times(recs):
    return sorted((r["text"], r["time"]) for r in recs)


# --------------------------------------------------------------------------- #60


def test_session_records_use_the_session_recording_time_offset(tmp_path):
    """The issue's reproduction: one session- and one channel-level Note, both
    at T0 + 1 s; then every section 3 gets rto = 3 days. The data and the
    channel record move by 3 days, so the session record must move with them."""
    path = tmp_path / "rto.mefd"
    _write_session(path)
    _set_rto(path, THREE_DAYS)

    r = mef3io.Reader(str(path))
    t0 = r.info("ch1")["start_time"]
    assert t0 == START + THREE_DAYS                      # the data moved
    (chan,) = r.records("ch1")
    (sess,) = r.records()
    assert chan["time"] - t0 == SECOND
    assert sess["time"] - t0 == SECOND, (
        f"session record landed {(sess['time'] - t0) / SECOND:+.0f} s from the data")


def test_session_records_refuse_when_channels_disagree_on_the_offset(tmp_path):
    """meflib marks a disagreeing session offset NO_ENTRY (meflib.c:6208); one
    of the two values is necessarily wrong for the session records, so mef3io
    refuses rather than guessing."""
    path = tmp_path / "split.mefd"
    _write_session(path)
    _set_rto(path, THREE_DAYS, channels=["ch2"])

    r = mef3io.Reader(str(path))
    with pytest.raises(m.FormatError, match="recording_time_offset"):
        r.records()
    # Channel-level records are unaffected: each channel agrees with itself.
    assert r.records("ch1")[0]["time"] == START + SECOND


def test_session_without_session_records_does_not_refuse(tmp_path):
    """The disagreement only matters for records that exist; a session with
    none must not start raising."""
    path = tmp_path / "split.mefd"
    _write_session(path, annotate=False)
    _set_rto(path, THREE_DAYS, channels=["ch2"])
    assert mef3io.Reader(str(path)).records() == []


def test_legacy_session_records_agree_with_the_legacy_reader(tmp_path):
    """The legacy stack (mef_tools -> pymef) writes a non-zero offset when
    ``record_offset`` is set, and stores record header times meflib-style
    (negated, offset removed). Its own reader places both levels at T0 + 1 s;
    so must mef3io's — before #60 the session record read back as the bare
    delta, 10 days after the epoch instead of in 2020."""
    pytest.importorskip("mef_tools", reason="legacy oracle not installed")
    pd = pytest.importorskip("pandas")
    from mef_tools.io import MefReader, MefWriter

    path = str(tmp_path / "legacy.mefd")
    w = MefWriter(path, overwrite=True)
    w.record_offset = START - 10 * 86400 * SECOND
    w.write_data(np.arange(5000, dtype=np.int32) * 1.0, "ch1", START, FS, precision=0)
    w.write_annotations(pd.DataFrame([{"time": START + SECOND, "type": "Note", "text": "s"}]))
    w.write_annotations(pd.DataFrame([{"time": START + SECOND, "type": "Note", "text": "c"}]),
                        "ch1")
    del w

    legacy = MefReader(path)
    want_session = [int(a["time"]) for a in legacy.get_annotations()]
    want_channel = [int(a["time"]) for a in legacy.get_annotations("ch1")]
    assert want_session == want_channel == [START + SECOND]   # the oracle agrees with itself

    with warnings.catch_warnings():
        warnings.simplefilter("ignore")   # legacy declarations warning
        r = mef3io.Reader(path)
    assert [a["time"] for a in r.records()] == want_session
    assert [a["time"] for a in r.records("ch1")] == want_channel


# --------------------------------------------------------------------------- #61


def _add_segment_records(path, channel="ch1", segment=0, text="segment"):
    """Write channel-level records, then move them to where meflib looks for
    SEGMENT-level ones (meflib.c:6026/6030: <seg>.segd/<seg>.rdat + .ridx)."""
    seg = f"{channel}-{segment:06d}"
    timd = Path(path) / f"{channel}.timd"
    with mef3io.Writer(str(path)) as w:
        w.write_annotations([{"type": "Note", "time": START + 2 * SECOND, "text": text}], channel)
    for ext in ("rdat", "ridx"):
        shutil.move(timd / f"{channel}.{ext}", timd / f"{seg}.segd" / f"{seg}.{ext}")


def test_segment_level_records_are_read(tmp_path):
    path = tmp_path / "seg.mefd"
    _write_session(path, channel_text="unused")
    _add_segment_records(path)                                   # moves the channel file away
    with mef3io.Writer(str(path)) as w:                          # ...so write a fresh one
        w.write_annotations([{"type": "Note", "time": START + SECOND, "text": "channel"}], "ch1")

    r = mef3io.Reader(str(path))
    recs = r.records("ch1")
    assert sorted(a["text"] for a in recs) == ["channel", "segment"], (
        "segment-level records must not be invisible")
    by_text = {a["text"]: a for a in recs}
    assert by_text["channel"]["level"] == "channel"
    assert by_text["channel"]["channel"] == "ch1" and by_text["channel"]["segment"] is None
    assert by_text["segment"]["level"] == "segment"
    assert by_text["segment"]["channel"] == "ch1" and by_text["segment"]["segment"] == 0
    assert by_text["segment"]["time"] == START + 2 * SECOND

    # Narrowing by segment / level.
    assert [a["text"] for a in r.records("ch1", segment=0)] == ["segment"]
    assert [a["text"] for a in r.records("ch1", level="channel")] == ["channel"]
    assert [a["text"] for a in r.records("ch1", level="segment")] == ["segment"]
    # Session level, unchanged default.
    (sess,) = r.records()
    assert sess["level"] == "session" and sess["channel"] is None and sess["segment"] is None
    # Everything in the session, at every level.
    assert sorted(a["text"] for a in r.records(level="all")) == ["channel", "segment", "session"]
    assert [a["text"] for a in r.records(level="segment")] == ["segment"]


def test_record_query_arguments_are_checked(tmp_path):
    path = tmp_path / "seg.mefd"
    _write_session(path)
    r = mef3io.Reader(str(path))
    with pytest.raises(ValueError):
        r.records(segment=0)                       # a segment needs its channel
    with pytest.raises(ValueError):
        r.records("ch1", level="session")          # no session records inside a channel
    with pytest.raises(ValueError):
        r.records(level="bogus")
    with pytest.raises((IndexError, KeyError, ValueError)):
        r.records("ch1", segment=7)                # no such segment
    with pytest.raises((IndexError, KeyError, ValueError)):
        r.records("nope")


def test_segment_records_use_the_segment_offset(tmp_path):
    """Each level resolves its times with the offset of what it belongs to;
    for a segment that is its own section 3."""
    path = tmp_path / "seg.mefd"
    _write_session(path)
    _add_segment_records(path)
    _set_rto(path, THREE_DAYS)
    r = mef3io.Reader(str(path))
    (seg,) = r.records("ch1", segment=0)
    assert seg["time"] - r.info("ch1")["start_time"] == 2 * SECOND


def test_segment_records_are_read_from_a_tar_archive(tmp_path):
    path = tmp_path / "seg.mefd"
    _write_session(path)
    _add_segment_records(path)
    tar = mef3io.archive_session(str(path))
    assert mef3io.Reader(tar).records(level="all") == mef3io.Reader(str(path)).records(level="all")


def test_compat_get_annotations_keeps_the_legacy_channel_level_meaning(tmp_path):
    """The legacy reader (pymef) returns a channel's CHANNEL-level records
    only, so the drop-in shim does too; the native Reader includes the
    segment level."""
    from mef3io.compat import MefReader

    path = tmp_path / "seg.mefd"
    _write_session(path, channel_text="channel")
    _add_segment_records(path)                                   # moves the channel file away
    with mef3io.Writer(str(path)) as w:
        w.write_annotations([{"type": "Note", "time": START + SECOND, "text": "channel"}], "ch1")
    anns = MefReader(str(path)).get_annotations("ch1")
    assert [(a["level"], a["text"]) for a in anns] == [("channel", "channel")]
    assert sorted(a["level"] for a in mef3io.Reader(str(path)).records("ch1")) == [
        "channel", "segment"]


# --------------------------------------------------------------------------- #62


def _seiz_body(earliest, latest, channels, onset_code=1):
    head = struct.pack("<qqqii", earliest, latest, latest - earliest, len(channels), onset_code)
    head += _fixed("marker-a", 128) + _fixed("marker-b", 128) + _fixed("focal onset", 1024)
    assert len(head) == 1312                                # MEFREC_Seiz_1_0_BYTES
    for name, on, off in channels:
        ch = _fixed(name, 256) + struct.pack("<qq", on, off)
        assert len(ch) == 272                               # MEFREC_Seiz_1_0_CHANNEL_BYTES
        head += ch
    return head


def _epoc_body(ident, ts, end, etype, text):
    b = struct.pack("<qqqq", ident, ts, end, end - ts) + _fixed(etype, 32) + _fixed(text, 128)
    assert len(b) == 192                                    # MEFREC_Epoc_1_0_BYTES
    return b


def _curs_body(ident, trace_ts, latency, value, name):
    b = struct.pack("<qqqd", ident, trace_ts, latency, value) + _fixed(name, 128)
    assert len(b) == 160                                    # MEFREC_Curs_1_0_BYTES
    return b


def _csti_body():
    b = _fixed("naming", 64) + struct.pack("<q", 2500) + _fixed("picture", 64) + _fixed("ok", 64)
    assert len(b) == 200                                    # MEFREC_CSti_1_0_BYTES
    return b


def _esti_body():
    b = struct.pack("<ddqii", 2.5, 50.0, 300, 1, 1)
    b += _fixed("biphasic", 128) + _fixed("LA1", 128) + _fixed("LA2", 128)
    assert len(b) == 416                                    # MEFREC_ESti_1_0_BYTES
    return b


def _bodies_session(tmp_path, rto=0):
    """A session whose session-level .rdat holds one record of every type
    mefrec.h defines, built byte-for-byte from its struct layout."""
    path = tmp_path / "bodies.mefd"
    _write_session(path)
    if rto:
        _set_rto(path, rto)
    t = START + 5 * SECOND
    disk = rto - t                          # meflib: negated, offset removed (meflib.c:756)
    recs = [
        _record("Note", b"a note\0", disk),
        _record("SyLg", b"a log line\0", disk),
        _record("EDFA", struct.pack("<q", 7 * SECOND) + b"edf text\0", disk),
        _record("Seiz", _seiz_body(t, t + 30 * SECOND,
                                   [("LA1", t + SECOND, t + 20 * SECOND), ("LA2", -1, -1)]), disk),
        _record("Epoc", _epoc_body(42, t, t + 60 * SECOND, "sleep", "N2"), disk),
        _record("Curs", _curs_body(7, t + 3 * SECOND, 1500, -12.5, "cursor-1"), disk),
        _record("CSti", _csti_body(), disk),
        _record("ESti", _esti_body(), disk),
        _record("LNTP", struct.pack("<q", 4) + struct.pack("<4i", 1, -2, 3, -4), disk),
        _record("UnRc", b"", disk),
    ]
    _install_records(next(path.glob("*.rdat")), recs)
    return path, t


def test_every_mefrec_body_type_is_parsed(tmp_path):
    path, t = _bodies_session(tmp_path)
    recs = {r["type"]: r for r in mef3io.Reader(str(path)).records()}
    assert set(recs) == {"Note", "SyLg", "EDFA", "Seiz", "Epoc", "Curs", "CSti", "ESti",
                         "LNTP", "UnRc"}
    assert all(r["time"] == t for r in recs.values())

    assert recs["Note"]["text"] == "a note"
    assert recs["SyLg"]["text"] == "a log line"
    assert recs["EDFA"]["duration"] == 7 * SECOND and recs["EDFA"]["text"] == "edf text"

    s = recs["Seiz"]
    assert s["earliest_onset"] == t and s["latest_offset"] == t + 30 * SECOND
    assert s["duration"] == 30 * SECOND
    assert s["number_of_channels"] == 2 and s["onset_code"] == 1
    assert s["marker_name_1"] == "marker-a" and s["marker_name_2"] == "marker-b"
    assert s["annotation"] == "focal onset"
    assert s["channels"] == [
        {"name": "LA1", "onset": t + SECOND, "offset": t + 20 * SECOND},
        {"name": "LA2", "onset": -1, "offset": -1},          # mefrec.h:129/131 NO_ENTRY kept
    ]

    e = recs["Epoc"]
    assert (e["id_number"], e["timestamp"], e["end_timestamp"], e["duration"]) == (
        42, t, t + 60 * SECOND, 60 * SECOND)
    assert e["epoch_type"] == "sleep" and e["text"] == "N2"

    c = recs["Curs"]
    assert (c["id_number"], c["trace_timestamp"], c["latency"]) == (7, t + 3 * SECOND, 1500)
    assert c["value"] == -12.5 and c["name"] == "cursor-1"

    cs = recs["CSti"]
    assert (cs["task_type"], cs["stimulus_duration"], cs["stimulus_type"],
            cs["patient_response"]) == ("naming", 2500, "picture", "ok")

    es = recs["ESti"]
    assert (es["amplitude"], es["frequency"], es["pulse_width"]) == (2.5, 50.0, 300)
    assert (es["ampunit_code"], es["mode_code"]) == (1, 1)
    assert (es["waveform"], es["anode"], es["catode"]) == ("biphasic", "LA1", "LA2")

    ln = recs["LNTP"]
    assert ln["length"] == 4 and list(ln["template"]) == [1, -2, 3, -4]

    assert set(recs["UnRc"]) == {"type", "time", "level", "channel", "segment",
                                 "version_major", "version_minor"}


def test_record_dict_shape_is_stable_per_type(tmp_path):
    """Every record of a type carries the same keys, whatever its body held —
    a truncated or unrecognised-version body reports None, not a missing key."""
    path = tmp_path / "short.mefd"
    _write_session(path)
    _install_records(next(path.glob("*.rdat")), [
        _record("Epoc", _epoc_body(1, START, START + SECOND, "a", "b"), -START),
        _record("Epoc", b"\0" * 8, -START),                       # truncated body
        _record("Epoc", _epoc_body(1, START, START + SECOND, "a", "b"), -START, vmaj=2),
    ])
    full, short, future = mef3io.Reader(str(path)).records()
    assert set(full) == set(short) == set(future)
    assert full["timestamp"] == START
    assert short["timestamp"] is None and short["text"] is None
    assert future["timestamp"] is None, "meflib decodes only version 1.0 bodies (mefrec.c:886)"


def test_seiz_channel_count_is_bounded_by_the_body(tmp_path):
    """number_of_channels is a declaration; a body shorter than it claims must
    not be read past its end."""
    path = tmp_path / "seiz.mefd"
    _write_session(path)
    body = _seiz_body(START, START + SECOND, [("A", START, START + 1)])
    body = body[:16] + struct.pack("<q", SECOND) + struct.pack("<i", 50) + body[28:]
    _install_records(next(path.glob("*.rdat")), [_record("Seiz", body, -START)])
    (s,) = mef3io.Reader(str(path)).records()
    assert s["number_of_channels"] == 50
    assert [c["name"] for c in s["channels"]] == ["A"]


# --------------------------------------------------------------------------- body times


def test_body_times_positive_are_returned_unchanged_under_an_offset(tmp_path):
    """meflib never applies the offset to a body (mefrec.c does not touch
    them), so in an offset session the body holds raw absolute uUTC while the
    header holds the negated delta. Both must come back as the same absolute
    time."""
    path, t = _bodies_session(tmp_path, rto=THREE_DAYS)
    recs = {r["type"]: r for r in mef3io.Reader(str(path)).records()}
    assert recs["Epoc"]["time"] == t                      # header: offset removed
    assert recs["Epoc"]["timestamp"] == t                 # body: positive, unchanged
    assert recs["Seiz"]["earliest_onset"] == t
    assert recs["Seiz"]["channels"][0]["onset"] == t + SECOND
    assert recs["Curs"]["trace_timestamp"] == t + 3 * SECOND


def test_body_times_negative_are_resolved_with_the_offset(tmp_path):
    """A writer that DID negate a body time (offset applied) is read the way
    meflib displays it: remove_recording_time_offset (meflib.c:7660) via
    local_date_time_string (meflib.c:4649), which mefrec.c calls on every body
    time. Sentinels are not times and pass through."""
    path = tmp_path / "neg.mefd"
    _write_session(path)
    _set_rto(path, THREE_DAYS)
    t = START + 5 * SECOND
    disk = THREE_DAYS - t
    _install_records(next(path.glob("*.rdat")), [
        _record("Epoc", struct.pack("<qqqq", 1, disk, UUTC_NO_ENTRY, 0)
                + _fixed("a", 32) + _fixed("b", 128), disk),
        _record("Seiz", _seiz_body(disk, disk, [("A", disk, -1)]), disk),
    ])
    epoc, seiz = mef3io.Reader(str(path)).records()
    assert epoc["timestamp"] == t
    assert epoc["end_timestamp"] == UUTC_NO_ENTRY
    assert seiz["earliest_onset"] == t and seiz["latest_offset"] == t
    assert seiz["channels"] == [{"name": "A", "onset": t, "offset": -1}]


def test_records_written_by_mef3io_keep_their_shape(tmp_path):
    """The extra keys are additive: what callers read before is unchanged."""
    path = tmp_path / "rt.mefd"
    _write_session(path)
    (sess,) = mef3io.Reader(str(path)).records()
    assert sess["type"] == "Note" and sess["text"] == "session"
    assert sess["time"] == START + SECOND
    assert (sess["version_major"], sess["version_minor"]) == (1, 0)


# --------------------------------------------------------------------------- #77 writer


def _stored_times(rdat):
    """Header times of every record in a .rdat, exactly as stored."""
    raw = Path(rdat).read_bytes()
    out, off = [], UH_BYTES
    while off + RECORD_HEADER_BYTES <= len(raw):
        out.append(struct.unpack_from("<q", raw, off + 16)[0])
        off += RECORD_HEADER_BYTES + struct.unpack_from("<I", raw, off + 12)[0]
    return out


def _rdat(path, channel=None):
    if channel is None:
        return next(Path(path).glob("*.rdat"))
    return Path(path) / f"{channel}.timd" / f"{channel}.rdat"


def test_records_written_into_an_offset_session_round_trip(tmp_path):
    """#77: the writer stored record times with rto 0 whatever section 3 said,
    so once #60 made the reader honour the offset, mef3io's own records in an
    offset session read back rto late. Both levels must round-trip."""
    path = tmp_path / "rto.mefd"
    _write_session(path, annotate=False)
    _set_rto(path, THREE_DAYS)
    t = START + THREE_DAYS + SECOND
    with mef3io.Writer(str(path)) as w:
        w.write_annotations([{"type": "Note", "time": t, "text": "s"}])
        w.write_annotations([{"type": "Note", "time": t, "text": "c"}], "ch1")
    r = mef3io.Reader(str(path))
    assert [a["time"] for a in r.records()] == [t]
    assert [a["time"] for a in r.records("ch1")] == [t]
    # rto != 0: stored offset-applied, rto - t (meflib.c:756) -- the real
    # date never reaches the file.
    assert _stored_times(_rdat(path)) == [THREE_DAYS - t]
    assert _stored_times(_rdat(path, "ch1")) == [THREE_DAYS - t]


def test_records_written_with_rto_zero_are_stored_exactly_as_before(tmp_path):
    """A session mef3io wrote has rto 0, and the stored times must not change:
    -t, the value every earlier release wrote."""
    path = tmp_path / "zero.mefd"
    _write_session(path)
    assert _stored_times(_rdat(path)) == [-(START + SECOND)]
    assert _stored_times(_rdat(path, "ch1")) == [-(START + SECOND)]
    # ...and so is the universal-header span: start = -(earliest), end = -(latest).
    with mef3io.Writer(str(path)) as w:
        w.write_annotations([{"type": "Note", "time": START + 5 * SECOND, "text": "b"},
                             {"type": "Note", "time": START, "text": "a"}])
    for f in (_rdat(path), _rdat(path).with_suffix(".ridx")):
        start, end = struct.unpack_from("<qq", f.read_bytes(), 16)
        assert (start, end) == (-START, -(START + 5 * SECOND))
    assert _stored_times(_rdat(path)) == [-START, -(START + 5 * SECOND)]


def test_records_written_into_a_legacy_offset_session_agree_with_the_legacy_reader(tmp_path):
    """#77 as reported: a mef_tools session with record_offset = START - 10 d;
    mef3io writes annotations at START + 1 s at both levels. mef3io and the
    legacy reader (pymef underneath) must both read them back at START + 1 s.
    Before the fix mef3io read them 1576972800 s late."""
    pytest.importorskip("mef_tools", reason="legacy oracle not installed")
    from mef_tools.io import MefReader, MefWriter

    path = str(tmp_path / "legacy.mefd")
    w = MefWriter(path, overwrite=True)
    w.record_offset = START - 10 * 86400 * SECOND
    w.write_data(np.arange(5000, dtype=np.int32) * 1.0, "ch1", START, FS, precision=0)
    del w

    with warnings.catch_warnings():
        warnings.simplefilter("ignore")   # legacy declarations warning
        with mef3io.Writer(path) as mw:
            mw.write_annotations([{"type": "Note", "time": START + SECOND, "text": "s"}])
            mw.write_annotations([{"type": "Note", "time": START + SECOND, "text": "c"}], "ch1")
        r = mef3io.Reader(path)
    assert [a["time"] for a in r.records()] == [START + SECOND]
    assert [a["time"] for a in r.records("ch1")] == [START + SECOND]

    legacy = MefReader(path)
    assert [int(a["time"]) for a in legacy.get_annotations()] == [START + SECOND]
    assert [int(a["time"]) for a in legacy.get_annotations("ch1")] == [START + SECOND]


def test_writer_refuses_records_when_the_level_disagrees_on_the_offset(tmp_path):
    """A level whose segments disagree has no single offset to store against
    -- any choice misplaces the times or reveals the date an anonymized
    segment hides -- so the write is refused, naming the segments, before any
    file is written. A channel that agrees with itself is unaffected."""
    path = tmp_path / "split.mefd"
    _write_session(path, annotate=False)
    _set_rto(path, THREE_DAYS, channels=["ch2"])
    with mef3io.Writer(str(path)) as w:
        with pytest.raises(m.WriteConflictError, match=r"disagree.*ch1-000000\.tmet.*ch2|"
                                                       r"disagree.*ch2-000000\.tmet.*ch1"):
            w.write_annotations([{"type": "Note", "time": START + THREE_DAYS, "text": "s"}])
        w.write_annotations([{"type": "Note", "time": START + SECOND, "text": "c"}], "ch1")
    assert not list(Path(path).glob("*.rdat"))
    assert _stored_times(_rdat(path, "ch1")) == [-(START + SECOND)]


# --------------------------------------------------------------------------- review fixes


def test_a_short_edfa_body_reports_absent_fields_instead_of_failing(tmp_path):
    """An EDFA body without its complete 8-byte duration used to throw from
    read_string and take the whole records() call down."""
    path = tmp_path / "edfa.mefd"
    _write_session(path)
    _install_records(next(path.glob("*.rdat")), [
        _record("EDFA", b"", -START),
        _record("Note", b"still here\0", -START),
    ])
    edfa, note = mef3io.Reader(str(path)).records()
    assert edfa["duration"] is None and edfa["text"] is None
    assert note["text"] == "still here"


@pytest.mark.parametrize("rtype,body", [
    ("Note", b"text\0"),
    ("SyLg", b"text\0"),
    ("EDFA", struct.pack("<q", 5) + b"text\0"),
])
def test_every_body_type_is_version_gated(tmp_path, rtype, body):
    """meflib decodes Note/SyLg/EDFA only at version 1.0 too (mefrec.c:201,
    232, 540); another version's layout is unknown, so its fields are absent.
    Version 1.0 — what mef3io, pymef and mef_tools all write — is unchanged."""
    path = tmp_path / "v.mefd"
    _write_session(path)
    _install_records(next(path.glob("*.rdat")), [
        _record(rtype, body, -START, vmaj=1, vmin=0),
        _record(rtype, body, -START, vmaj=2, vmin=0),
        _record(rtype, body, -START, vmaj=1, vmin=1),
    ])
    v10, v20, v11 = mef3io.Reader(str(path)).records()
    assert v10["text"] == "text"
    assert v20["text"] is None and v11["text"] is None


def test_lntp_written_by_pymef_uses_int64_and_is_detected(tmp_path):
    """pymef writes the LNTP template as int64 (pymef3_file.c:4159-4160)
    where mefrec.h says si4; read as si4 the template came back as
    [1, 0, -2, -1, 3]. Detected, and read as the values written."""
    pytest.importorskip("pymef")
    from pymef.mef_session import MefSession

    path = tmp_path / "lntp.mefd"
    _write_session(path, annotate=False)
    ms = MefSession(str(path), "", read_metadata=False)
    ms.write_mef_records("", "", START, START + SECOND, 0, [
        {"type": "LNTP", "time": START + SECOND, "length": 5,
         "template": np.array([1, -2, 3, 4, 5])}])
    (rec,) = mef3io.Reader(str(path)).records()
    assert rec["length"] == 5 and rec["template"] == [1, -2, 3, 4, 5]


@pytest.mark.parametrize("values", [[7], [1, 2, 3], [-1, 0, -1], [-5]])
def test_lntp_si4_layout_is_not_mistaken_for_int64(tmp_path, values):
    """The mefrec.h layout (si4) must still win whenever the int64 reading is
    not self-consistent — including short templates whose 0x7e padding leaves
    room for 8 bytes per value."""
    path = tmp_path / "lntp.mefd"
    _write_session(path)
    body = struct.pack("<q", len(values)) + struct.pack(f"<{len(values)}i", *values)
    _install_records(next(path.glob("*.rdat")), [_record("LNTP", body, -START)])
    (rec,) = mef3io.Reader(str(path)).records()
    assert rec["template"] == values


def _two_segment_session(path):
    """ch1 with two segments; channel-level and segment-0 records."""
    with mef3io.Writer(str(path), overwrite=True) as w:
        w.write("ch1", np.zeros(2560), START, FS, precision=0)
        w.write("ch1", np.zeros(2560), START + 20 * SECOND, FS, precision=0, new_segment=True)
    _add_segment_records(path, text="segment")
    with mef3io.Writer(str(path)) as w:
        w.write_annotations([{"type": "Note", "time": START + SECOND, "text": "channel"}], "ch1")


def test_a_disputed_offset_only_matters_to_offset_dependent_times(tmp_path):
    """Non-negative stored times are absolute whatever the offset is, so a
    file holding only those is placed even when the segments disagree."""
    path = tmp_path / "pos.mefd"
    _write_session(path)
    _set_rto(path, THREE_DAYS, channels=["ch2"])
    _install_records(next(path.glob("*.rdat")),
                     [_record("Note", b"absolute\0", START + SECOND)])   # stored positive
    (rec,) = mef3io.Reader(str(path)).records()
    assert rec["time"] == START + SECOND


def test_a_disputed_channel_offset_does_not_cost_the_segment_records(tmp_path):
    """Segment-level records carry their own segment's offset, so a dispute
    at the channel level must not take them down: strict raises for the
    channel file but level="segment" still reads; lenient skips only the
    channel file and reports it."""
    path = tmp_path / "split.mefd"
    _two_segment_session(path)
    _set_rto(path, THREE_DAYS, segments=[1])          # the segments of ch1 now disagree

    strict = mef3io.Reader(str(path))
    with pytest.raises(m.FormatError, match="recording_time_offset"):
        strict.records("ch1")
    assert [a["text"] for a in strict.records("ch1", level="segment")] == ["segment"]

    lenient = mef3io.Reader(str(path), strict=False)
    with pytest.warns(mef3io.UnreadableSegmentWarning, match="record file"):
        recs = lenient.records("ch1")
    assert [(a["level"], a["text"]) for a in recs] == [("segment", "segment")]
    (problem,) = lenient.problems
    assert problem["segment_number"] == -1 and "ch1.rdat" in problem["segment"]
    assert "recording_time_offset" in problem["reason"]


# --------------------------------------------------------------------------- times at or before rto


def _legacy_session(path, rto, annotate=True, chans=("ch1",)):
    """A session the legacy stack wrote with record_offset = rto, optionally
    with its own session- and channel-level Note at START + 1 s."""
    pytest.importorskip("mef_tools", reason="legacy oracle not installed")
    pd = pytest.importorskip("pandas")
    from mef_tools.io import MefWriter

    w = MefWriter(str(path), overwrite=True)
    w.record_offset = rto
    for ch in chans:
        w.write_data(np.arange(5000, dtype=np.int32) * 1.0, ch, START, FS, precision=0)
    if annotate:
        w.write_annotations(pd.DataFrame([{"time": START + SECOND, "type": "Note", "text": "s"}]))
        w.write_annotations(
            pd.DataFrame([{"time": START + SECOND, "type": "Note", "text": "c"}]), "ch1")
    del w


def _three_readers(path):
    """Session-level (text, time) pairs as mef3io, mef_tools and pymef see them."""
    from mef_tools.io import MefReader
    from pymef.mef_session import MefSession

    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        ours = [(a["text"], a["time"]) for a in mef3io.Reader(str(path)).records()]
    legacy = [(a["text"], int(a["time"])) for a in MefReader(str(path)).get_annotations()]
    pym = [(r["text"], int(r["time"])) for r in MefSession(str(path), "").read_records()]
    return ours, legacy, pym


@pytest.mark.parametrize("rto", [START, START + 3600 * SECOND],
                         ids=["rto=START", "rto=START+1h"])
def test_a_time_at_or_before_the_offset_is_refused(tmp_path, rto):
    """Under an anonymization offset a time t <= rto has no hidden form: rto - t
    would be non-negative, which every reader takes as the absolute date
    (meflib.c:7665), so the only correct encoding is the real date in plain.
    The write is refused, with nothing written; a time after the offset is
    stored as rto - t and all three readers agree on it."""
    pytest.importorskip("pymef")
    path = tmp_path / "early.mefd"
    _legacy_session(path, rto, annotate=False)
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        with mef3io.Writer(str(path)) as w:
            for t in (START - SECOND, START, rto):
                with pytest.raises(m.WriteConflictError, match="anonymization offset"):
                    w.write_annotations([{"type": "Note", "time": t, "text": "early"}])
            assert not list(Path(path).glob("*.rdat"))
            w.write_annotations([{"type": "Note", "time": rto + SECOND, "text": "late"}])
    ours, legacy, pym = _three_readers(path)
    assert ours == legacy == pym == [("late", rto + SECOND)]
    assert _stored_times(_rdat(path)) == [-SECOND]           # rto - t: the date stays hidden


def test_extending_a_legacy_offset_session_keeps_its_annotations(tmp_path):
    """A new channel and a new segment added by mef3io inherit the session's
    offset, so the levels keep agreeing and the legacy session- and
    channel-level annotations stay readable; new data is stored as rto - t
    (date hidden) and every reader places it; write_annotations still works."""
    pytest.importorskip("pymef")
    from mef3io.compat import MefReader as CompatReader
    from mef_tools.io import MefReader
    from pymef.mef_session import MefSession

    rto = START - 10 * 86400 * SECOND
    path = tmp_path / "extend.mefd"
    _legacy_session(path, rto)
    seg1 = START + 60 * SECOND
    n = 5000
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        with mef3io.Writer(str(path)) as w:
            w.write("ch2", np.arange(n) * 1.0, START, FS, precision=0)                 # new channel
            w.write("ch1", np.arange(n) * 1.0, seg1, FS, precision=0, new_segment=True)  # new segment
        r = mef3io.Reader(str(path))
        assert [s["recording_time_offset"] for s in (r.info("ch1"), r.info("ch2"))] == [rto, rto]
        assert [(a["text"], a["time"]) for a in r.records()] == [("s", START + SECOND)]
        assert [(a["text"], a["time"]) for a in r.records("ch1")] == [("c", START + SECOND)]
        assert [(a["text"], a["time"]) for a in CompatReader(str(path)).get_annotations()] == [
            ("s", START + SECOND)]
        np.testing.assert_array_equal(r.read("ch2"), np.arange(n))
        assert r.segments("ch1")[1]["start_time"] == seg1

        with mef3io.Writer(str(path)) as w:
            w.write_annotations([{"type": "Note", "time": START - SECOND, "text": "new"}])
            w.write_annotations([{"type": "Note", "time": seg1, "text": "newc"}], "ch2")

    ours, legacy, pym = _three_readers(path)
    assert ours == legacy == pym == [("new", START - SECOND)]
    assert [int(a["time"]) for a in MefReader(str(path)).get_annotations("ch2")] == [seg1]

    ms = MefSession(str(path), "")
    span = int(round(n / FS * 1e6))
    np.testing.assert_array_equal(ms.read_ts_channels_uutc("ch2", [START, START + span]),
                                  np.arange(n))
    np.testing.assert_array_equal(ms.read_ts_channels_uutc("ch1", [seg1, seg1 + span]),
                                  np.arange(n))

    # The validator finds nothing on what mef3io added (the legacy segment's
    # own declaration defects are its own).
    report = mef3io.Validator(str(path)).validate()
    assert all(f.channel == "ch1" and f.segment == 0 for f in report.findings), report.summary()


# --------------------------------------------------------------------------- damaged record files


def _two_notes(path):
    _write_session(path, annotate=False)
    with mef3io.Writer(str(path)) as w:
        w.write_annotations([{"type": "Note", "time": START, "text": "first note"},
                             {"type": "Note", "time": START + SECOND, "text": "second note"}])
    return _rdat(path)


@pytest.mark.parametrize("damage", ["tail-truncated", "huge-body-bytes", "flipped-body-byte",
                                    "declares-more-than-present"])
def test_a_damaged_record_file_fails_instead_of_returning_less(tmp_path, damage):
    """Damage used to come back silently: a cut-off file returned fewer
    records, a flipped byte returned corrupted text. Now it raises (strict) or
    is skipped and reported (lenient), like any other damaged file."""
    path = tmp_path / "dmg.mefd"
    rdat = _two_notes(path)
    raw = bytearray(rdat.read_bytes())
    if damage == "tail-truncated":
        raw = raw[:-10]
    elif damage == "huge-body-bytes":
        struct.pack_into("<I", raw, UH_BYTES + 12, 0x7FFFFFF0)
    elif damage == "flipped-body-byte":
        raw[UH_BYTES + RECORD_HEADER_BYTES + 2] ^= 0x01        # "fi" -> "fh"
    else:
        struct.pack_into("<q", raw, 32, 3)                      # declares 3, holds 2
    rdat.write_bytes(bytes(raw))

    with pytest.raises(m.MefError):
        mef3io.Reader(str(path)).records()
    r = mef3io.Reader(str(path), strict=False)
    with pytest.warns(mef3io.UnreadableSegmentWarning, match="record file"):
        assert r.records() == []
    (problem,) = r.problems
    assert problem["kind"] == "records" and problem["segment_number"] == -1


def test_problems_say_which_kind_they_are(tmp_path):
    """A skipped segment and a skipped record file are different failures:
    one leaves a gap in the samples, the other only misses annotations."""
    path = tmp_path / "kinds.mefd"
    _write_session(path)
    tmet = next(Path(path).glob("ch2.timd/*.segd/*.tmet"))
    raw = bytearray(tmet.read_bytes())
    raw[2000] ^= 0xFF                                          # break ch2's metadata CRC
    tmet.write_bytes(bytes(raw))
    _rdat(path).write_bytes(b"\0" * 10)                       # and the session record file
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        r = mef3io.Reader(str(path), strict=False)
        r.records()
    kinds = {(p["kind"], p["channel"], p["segment_number"]) for p in r.problems}
    assert kinds == {("segment", "ch2", 0), ("records", "", -1)}


def test_a_strict_channel_dispute_points_at_the_segment_records(tmp_path):
    path = tmp_path / "split.mefd"
    _two_segment_session(path)
    _set_rto(path, THREE_DAYS, segments=[1])
    with pytest.raises(m.FormatError, match=r'records\("ch1", level="segment"\)'):
        mef3io.Reader(str(path)).records("ch1")


def test_a_record_file_declaring_fewer_records_than_it_holds_is_tolerated(tmp_path):
    """meflib reads exactly the declared count (meflib.c:1869); so does mef3io.
    Only declaring MORE than present is damage."""
    path = tmp_path / "fewer.mefd"
    rdat = _two_notes(path)
    raw = bytearray(rdat.read_bytes())
    struct.pack_into("<q", raw, 32, 1)
    rdat.write_bytes(bytes(raw))
    assert [a["text"] for a in mef3io.Reader(str(path)).records()] == ["first note"]


# --------------------------------------------------------------------------- session file naming


def test_session_records_prefer_the_session_named_file(tmp_path):
    """meflib reads exactly <session>.mefd/<session>.rdat (meflib.c:6335); a
    stray .rdat at the root that sorts first must not be picked instead."""
    path = tmp_path / "named.mefd"
    _write_session(path, session_text="the session's")
    stray = Path(path) / "aaa_stray.rdat"
    stray.write_bytes(_rdat(path).read_bytes())
    _install_records(stray, [_record("Note", b"stray\0", -START)])
    assert [a["text"] for a in mef3io.Reader(str(path)).records()] == ["the session's"]


def test_a_single_differently_named_session_file_is_still_read(tmp_path):
    path = tmp_path / "renamed.mefd"
    _write_session(path)
    _rdat(path).rename(Path(path) / "other.rdat")
    assert [a["text"] for a in mef3io.Reader(str(path)).records()] == ["session"]


def test_two_session_files_and_none_named_for_the_session_are_ambiguous(tmp_path):
    path = tmp_path / "two.mefd"
    _write_session(path)
    original = _rdat(path)
    (Path(path) / "a.rdat").write_bytes(original.read_bytes())
    original.rename(Path(path) / "b.rdat")
    with pytest.raises(m.FormatError, match="ambiguous"):
        mef3io.Reader(str(path)).records()
    r = mef3io.Reader(str(path), strict=False)
    with pytest.warns(mef3io.UnreadableSegmentWarning, match="record file"):
        assert r.records() == []
    (problem,) = r.problems
    assert problem["kind"] == "records" and "a.rdat, b.rdat" in problem["reason"]
    assert [a["text"] for a in r.records("ch1")] == ["channel"]    # other levels unaffected


# --------------------------------------------------------------------------- one damaged .tmet


@pytest.mark.parametrize("write", ["session annotations", "new channel ch3",
                                   "ch1 new_segment=True"])
def test_one_damaged_tmet_does_not_block_unrelated_writes(tmp_path, write):
    """The writer reads other segments' metadata to choose an offset. A single
    damaged .tmet used to make that throw, so annotations, a new channel and a
    new segment elsewhere all failed where main wrote them fine. It is now
    skipped (with a warning) and the write goes ahead."""
    path = tmp_path / "dmg.mefd"
    _write_session(path, annotate=False)
    tmet = next(Path(path).glob("ch2.timd/*.segd/*.tmet"))
    raw = bytearray(tmet.read_bytes())
    raw[2000] ^= 0xFF                                     # break ch2's metadata CRC
    tmet.write_bytes(bytes(raw))

    # A new segment of ch1 is scoped to ch1's own segments, so it never even
    # reads ch2's metadata; the other two consult the whole session and warn.
    expect = (pytest.warns(mef3io.UnreadableSegmentWarning, match="ch2")
              if write != "ch1 new_segment=True" else warnings.catch_warnings())
    with mef3io.Writer(str(path)) as w:
        with expect:
            if write == "ch1 new_segment=True":
                warnings.simplefilter("error")                # ...so it must not warn
            if write == "session annotations":
                w.write_annotations([{"type": "Note", "time": START, "text": "ok"}])
            elif write == "new channel ch3":
                w.write("ch3", np.arange(512.0), START, FS, precision=0)
            else:
                w.write("ch1", np.arange(512.0), START + 60 * SECOND, FS, precision=0,
                        new_segment=True)

    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        r = mef3io.Reader(str(path), strict=False)            # ch2 itself stays unreadable
        if write == "session annotations":
            assert [(a["text"], a["time"]) for a in r.records()] == [("ok", START)]
            assert _stored_times(_rdat(path)) == [-START]      # rto 0: classic form
        elif write == "new channel ch3":
            np.testing.assert_array_equal(r.read("ch3"), np.arange(512.0))
        else:
            assert r.segments("ch1")[1]["start_time"] == START + 60 * SECOND


# --------------------------------------------------------------------------- keep the date hidden


def _all_bytes(path):
    return b"".join(f.read_bytes() for f in sorted(Path(path).rglob("*")) if f.is_file())


def test_writes_into_an_anonymized_session_never_store_the_real_date(tmp_path):
    """The recording-time offset exists to keep the real date out of the
    files. An in-segment append, a new segment, a new channel and records all
    store rto - t: no 8-byte value anywhere in the session equals one of the
    absolute times written. All of it still reads back at the right time."""
    path = tmp_path / "anon.mefd"
    _write_session(path, annotate=False)              # ch1, ch2 at START, 10 s each
    _set_rto(path, THREE_DAYS)                        # now an anonymized session
    t0 = START + THREE_DAYS                           # where the data now reads
    append_at = t0 + 10 * SECOND                      # right after ch1's end
    seg_at = t0 + 60 * SECOND
    data = np.arange(2560.0)                          # one 10 s block each
    with mef3io.Writer(str(path)) as w:
        w.write("ch1", data, append_at, FS)                                # in-segment append
        w.write("ch1", data, seg_at, FS, precision=3, new_segment=True)    # new segment
        w.write("ch3", data, seg_at, FS, precision=0)                      # new channel
        w.write_annotations([{"type": "Note", "time": seg_at + SECOND, "text": "s"}])
        w.write_annotations([{"type": "Note", "time": seg_at + 2 * SECOND, "text": "c"}], "ch3")

    end = int(round(len(data) / FS * 1e6))
    written = {append_at, append_at + end, seg_at, seg_at + end, seg_at + SECOND,
               seg_at + 2 * SECOND}
    blob = _all_bytes(path)
    leaked = [t for t in written if struct.pack("<q", t) in blob]
    assert leaked == [], f"real dates stored in plain: {leaked}"

    r = mef3io.Reader(str(path))
    assert [s["recording_time_offset"] for s in map(r.info, ("ch1", "ch3"))] == [THREE_DAYS] * 2
    np.testing.assert_array_equal(r.read("ch1", append_at, append_at + end), data)
    np.testing.assert_array_equal(r.read("ch3"), data)
    assert [s["start_time"] for s in r.segments("ch1")] == [t0, seg_at]
    assert [(a["text"], a["time"]) for a in r.records(level="all")] == [
        ("s", seg_at + SECOND), ("c", seg_at + 2 * SECOND)]


def test_a_level_1_writer_refuses_what_needs_the_locked_offset(tmp_path):
    """With only the level-1 password, section 3 — and the offset in it — is
    locked. That offset is UNKNOWN, not 0: storing against 0 placed an
    appended block decades off. Every write that needs it is refused, saying
    the level-2 password is required; with it, the same writes work."""
    pytest.importorskip("mef_tools", reason="legacy oracle not installed")
    from mef_tools.io import MefWriter

    rto = START - 10 * 86400 * SECOND
    path = str(tmp_path / "locked.mefd")
    lw = MefWriter(path, overwrite=True, password1="lvl1", password2="lvl2")
    lw.record_offset = rto
    lw.write_data(np.arange(5000, dtype=np.int32) * 1.0, "ch1", START, FS, precision=0)
    del lw
    before = _all_bytes(path)

    later = START + 60 * SECOND
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        w = mef3io.Writer(path, password1="lvl1")
        for attempt in (
            lambda: w.write_annotations([{"type": "Note", "time": later, "text": "x"}]),
            lambda: w.write_annotations([{"type": "Note", "time": later, "text": "x"}], "ch1"),
            lambda: w.write("ch1", np.zeros(512), later, FS, precision=0),
            lambda: w.write("ch2", np.zeros(512), later, FS, precision=0),
        ):
            with pytest.raises(m.PasswordError, match="level-2"):
                attempt()
        w.close()
    assert _all_bytes(path) == before, "a refused write must leave the session untouched"

    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        with mef3io.Writer(path, password1="lvl1", password2="lvl2") as w:
            w.write_annotations([{"type": "Note", "time": later, "text": "x"}])
        r = mef3io.Reader(path, password="lvl2")
        assert [a["time"] for a in r.records()] == [later]


def test_a_disputed_session_refuses_a_new_channel(tmp_path):
    """A new channel takes the session's offset; with the segments disputing
    it there is none to take. A new segment of a channel that agrees with
    itself is unaffected."""
    path = tmp_path / "split.mefd"
    _write_session(path, annotate=False)
    _set_rto(path, THREE_DAYS, channels=["ch2"])
    before = _tree(path)
    with mef3io.Writer(str(path)) as w:
        with pytest.raises(m.WriteConflictError, match="disagree"):
            w.write("ch3", np.zeros(512), START, FS, precision=0)
    assert _tree(path) == before, "a refused write must leave the tree exactly as it was"
    with mef3io.Writer(str(path)) as w:
        w.write("ch1", np.zeros(512), START + 60 * SECOND, FS, precision=0, new_segment=True)
    assert len(mef3io.Reader(str(path)).segments("ch1")) == 2


def test_mixed_segments_stay_consistent(tmp_path):
    """The reviewer's invariants on a legacy session extended by mef3io:
    every segment's start <= end, and the block index runs forward in
    absolute time across the legacy and the mef3io-written segments."""
    rto = START - 10 * 86400 * SECOND
    path = tmp_path / "mixed.mefd"
    _legacy_session(path, rto, annotate=False)
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        with mef3io.Writer(str(path)) as w:
            w.write("ch1", np.arange(2560.0), START + 60 * SECOND, FS, precision=0,
                    new_segment=True)
            w.write("ch1", np.arange(2560.0), START + 70 * SECOND, FS, precision=0)
        r = mef3io.Reader(str(path))
    for s in r.segments("ch1"):
        assert s["start_time"] <= s["end_time"]
    starts = [b["start_uutc"] for b in r.toc("ch1")] if isinstance(r.toc("ch1"), list) \
        else list(r.toc("ch1")["start_uutc"])
    assert starts == sorted(starts) and len(set(starts)) == len(starts)
    assert starts[0] == START and starts[-1] == START + 70 * SECOND


# --------------------------------------------------------------------------- final review


def _tree(path):
    """Every directory and file under `path`, with file contents hashed."""
    import hashlib

    out = {}
    for f in sorted(Path(path).rglob("*")):
        rel = str(f.relative_to(path))
        out[rel] = "<dir>" if f.is_dir() else hashlib.sha256(f.read_bytes()).hexdigest()
    return out


def _corrupt_tmet(tmet):
    raw = bytearray(tmet.read_bytes())
    raw[2000] ^= 0xFF
    tmet.write_bytes(bytes(raw))


def test_annotations_for_a_channel_without_data_take_the_session_offset(tmp_path):
    """A channel with no segment yet has no offset of its own; its records
    must use the SESSION's. Taking 0 there stored the real date, and once the
    channel got data every reader placed the note ~50 years off."""
    pytest.importorskip("pymef")
    from pymef.mef_session import MefSession

    rto = START - 3600 * SECOND
    path = tmp_path / "chnew.mefd"
    _legacy_session(path, rto, annotate=False)
    note_at = START + 30 * SECOND
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        with mef3io.Writer(str(path)) as w:
            w.write_annotations([{"type": "Note", "time": note_at, "text": "n"}], "chNew")
            w.write("chNew", np.arange(512.0), START + 20 * SECOND, FS, precision=0)
        r = mef3io.Reader(str(path))
        assert [a["time"] for a in r.records("chNew")] == [note_at]
    assert _stored_times(_rdat(path, "chNew")) == [rto - note_at]
    assert struct.pack("<q", note_at) not in _rdat(path, "chNew").read_bytes()
    ms = MefSession(str(path), "")
    assert [int(x["time"]) for x in ms.read_records("chNew")] == [note_at]


def _damaged_anonymized(path, rto, chans):
    _legacy_session(path, rto, annotate=False, chans=chans)


@pytest.mark.parametrize("write", ["session note", "ch1 note", "new channel ch9"])
def test_unreadable_metadata_with_nothing_readable_is_refused(tmp_path, write):
    """Every .tmet in scope unreadable: the offset is UNKNOWN, never 0 --
    taking 0 stored the real date in an anonymized session."""
    rto = START - 3600 * SECOND
    path = tmp_path / "a.mefd"
    _damaged_anonymized(path, rto, ("ch1",))
    _corrupt_tmet(next(Path(path).glob("ch1.timd/*.segd/*.tmet")))
    before = _tree(path)
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        with mef3io.Writer(str(path)) as w:
            with pytest.raises(m.WriteConflictError, match="cannot be read"):
                if write == "session note":
                    w.write_annotations([{"type": "Note", "time": START + SECOND, "text": "x"}])
                elif write == "ch1 note":
                    w.write_annotations([{"type": "Note", "time": START + SECOND, "text": "x"}],
                                        "ch1")
                else:
                    w.write("ch9", np.arange(512.0), START + 100 * SECOND, FS, precision=0)
            assert any("ch1-000000.tmet" in u for u in w.unreadable_metadata)
    assert _tree(path) == before


def test_unreadable_metadata_beside_a_nonzero_agreement_is_refused(tmp_path):
    """The readable segments agree on an offset, but an unreadable one may
    not: writing against an unverified offset produced session records that,
    once the damaged segment was restored, could not be read at all."""
    rto = START - 3600 * SECOND
    path = tmp_path / "b.mefd"
    _damaged_anonymized(path, rto, ("ch1", "ch2"))
    _corrupt_tmet(next(Path(path).glob("ch2.timd/*.segd/*.tmet")))
    before = _tree(path)
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        with mef3io.Writer(str(path)) as w:
            with pytest.raises(m.WriteConflictError, match="cannot be verified"):
                w.write_annotations([{"type": "Note", "time": START + SECOND, "text": "s"}])
            with pytest.raises(m.WriteConflictError, match="cannot be verified"):
                w.write("ch3", np.arange(512.0), START + 100 * SECOND, FS, precision=0)
            # ch1's own level is fully readable: its records are fine.
            w.write_annotations([{"type": "Note", "time": START + SECOND, "text": "c"}], "ch1")
    after = _tree(path)
    assert {k: v for k, v in after.items() if "ch1.rdat" not in k and "ch1.ridx" not in k} \
        == before


def test_unreadable_metadata_beside_offset_zero_is_skipped_with_a_warning(tmp_path):
    """In an ordinary (offset 0) session a damaged segment must not block
    unrelated writes: skipped, reported, and the compat writer says so too."""
    from mef3io.compat import MefWriter as CompatWriter

    path = tmp_path / "zero.mefd"
    _write_session(path, annotate=False)
    _corrupt_tmet(next(Path(path).glob("ch2.timd/*.segd/*.tmet")))
    cw = CompatWriter(str(path), overwrite=False)
    with pytest.warns(mef3io.UnreadableSegmentWarning, match="ch2"):
        cw.write_annotations([{"type": "Note", "time": START, "text": "ok"}])
    cw.close()
    cw = CompatWriter(str(path), overwrite=False)       # a fresh writer reports it again
    with pytest.warns(mef3io.UnreadableSegmentWarning, match="ch2"):
        cw.write_data(np.arange(512.0), "ch4", START, FS, precision=0)
    cw.close()
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        r = mef3io.Reader(str(path), strict=False)
        assert [a["text"] for a in r.records()] == ["ok"]
        np.testing.assert_array_equal(r.read("ch4"), np.arange(512.0))


@pytest.mark.parametrize("how", ["new channel", "new segment", "write_int32", "compat write_data"])
def test_a_refused_write_leaves_the_tree_untouched_and_the_channel_usable(tmp_path, how):
    """The refusal used to come AFTER the segment directory was created; the
    next Writer adopted the empty directory as a segment and every later
    write failed ("cannot open segment metadata for append")."""
    from mef3io.compat import MefWriter as CompatWriter

    rto = START - 3600 * SECOND
    path = tmp_path / "wedge.mefd"
    _legacy_session(path, rto, annotate=False)
    early, late = rto - SECOND, START + 500 * SECOND
    ch = "ch1" if how == "new segment" else "chX"

    def attempt(t):
        if how == "compat write_data":
            cw = CompatWriter(str(path), overwrite=False)
            try:
                cw.write_data(np.arange(512.0), ch, t, FS, precision=0)
            finally:
                cw.close()
            return
        with mef3io.Writer(str(path)) as w:
            if how == "write_int32":
                w.write_int32(ch, np.arange(512, dtype=np.int32), 1.0, t, FS)
            else:
                w.write(ch, np.arange(512.0), t, FS, precision=0,
                        new_segment=how == "new segment")

    before = _tree(path)
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        with pytest.raises(m.WriteConflictError, match="anonymization offset"):
            attempt(early)
        assert _tree(path) == before
        attempt(late)                                   # a later valid write works
        r = mef3io.Reader(str(path))
        assert r.segments(ch)[-1]["start_time"] == late


def test_a_wedged_empty_segment_directory_is_ignored(tmp_path):
    """Sessions already wedged by an earlier release: an empty .segd (no
    .tmet) is not adopted as a segment; it is reported and its number reused."""
    path = tmp_path / "wedged.mefd"
    _write_session(path, annotate=False)
    empty = Path(path) / "ch1.timd" / "ch1-000001.segd"
    empty.mkdir()
    with mef3io.Writer(str(path)) as w:
        w.write("ch1", np.arange(512.0), START + 10 * SECOND, FS, precision=3)   # in-segment
        w.write("ch1", np.arange(512.0), START + 60 * SECOND, FS, precision=3,
                new_segment=True)
        assert any("empty segment directory" in u for u in w.unreadable_metadata)
    segs = mef3io.Reader(str(path)).segments("ch1")
    assert [s["segment"] for s in segs] == [0, 1]
    assert segs[1]["start_time"] == START + 60 * SECOND


def test_a_renamed_archive_still_finds_its_session_records(tmp_path):
    """For a .mefd.tar the session name is the folder INSIDE the archive; the
    archive file can be renamed freely."""
    path = tmp_path / "orig.mefd"
    _write_session(path, session_text="the session's")
    stray = Path(path) / "aaa_stray.rdat"
    stray.write_bytes(_rdat(path).read_bytes())
    _install_records(stray, [_record("Note", b"stray\0", -START)])
    tar = Path(mef3io.archive_session(str(path)))
    renamed = tar.with_name("renamed.mefd.tar")
    tar.rename(renamed)
    assert [a["text"] for a in mef3io.Reader(str(renamed)).records()] == ["the session's"]
