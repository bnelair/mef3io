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


def _set_rto(path, rto, channels=None):
    """Give section 3 of every (or the named channels') .tmet this offset.

    The fixed-length .tmet body CRC is bounded by METADATA_FILE_BYTES — never
    to EOF (CLAUDE.md, ".tmet is a FIXED-length record")."""
    for tmet in sorted(Path(path).glob("*.timd/*.segd/*.tmet")):
        if channels is not None and tmet.parent.parent.name[:-5] not in channels:
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


def test_compat_get_annotations_includes_segment_records(tmp_path):
    from mef3io.compat import MefReader

    path = tmp_path / "seg.mefd"
    _write_session(path)
    _add_segment_records(path)
    anns = MefReader(str(path)).get_annotations("ch1")
    assert sorted(a["level"] for a in anns) == ["segment"]   # channel file was moved down


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
