"""P26: ``mef3io.inspect`` (#67) — everything about a session without its samples.

Pins: the summary agrees with the Reader on everything both can see; it NEVER
reads a ``.tdat`` (``/proc/self/io`` rchar, like test_p17); identity fields
report presence and length unless values are asked for; the recording-time
offset is reported plainly (status, date recoverable, by whom) and its value
only on request; encrypted sessions are described, not refused, and what is
locked is reported rather than guessed; tar archives read in place; lenient
mode reports unreadable segments; record times widen the session's span.
"""
from __future__ import annotations

import json
import shutil
import struct
import subprocess
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
UH_BYTES = 1024
METADATA_FILE_BYTES = 16384
S3_RTO_OFFSET = 13312
SEIZ_BYTES = 1312                 # Seiz 1.0 fixed part (records.cpp, mefrec.h)
SEIZ_CHANNEL_BYTES = 272          # one channel entry (records.cpp)
SUBJECT = mef3io.Subject(name_1="Jane", name_2="Doe", id="MRN-123", recording_location="Ward 9")


# --------------------------------------------------------------------------- helpers


def _write(path, channels=("ch1", "ch2"), metadata=None, password1="", password2="", records=True):
    md = metadata if metadata is not None else mef3io.Metadata(
        subject=SUBJECT,
        acquisition=mef3io.Acquisition(session_description="night study"))
    with mef3io.Writer(str(path), overwrite=True, metadata=md,
                       password1=password1, password2=password2) as w:
        for ch in channels:
            w.write(ch, np.sin(np.arange(2560) / 10.0), START, FS, precision=3)
            w.write(ch, np.sin(np.arange(2560) / 10.0), START + 60 * SECOND, FS, precision=3,
                    new_segment=True)
        if records:
            w.write_annotations([{"type": "Note", "time": START + SECOND, "text": "seizure?"}])
            # An EDFA whose duration runs past the last sample: the span a time
            # shift must cover is longer than the data.
            w.write_annotations([{"type": "EDFA", "time": START + 65 * SECOND,
                                  "duration": 3600 * SECOND, "text": "long event"}], channels[0])
    return str(path)


def _reseal(raw, body_end):
    struct.pack_into("<I", raw, 4, m.crc32(bytes(raw[UH_BYTES:body_end])))
    struct.pack_into("<I", raw, 0, m.crc32(bytes(raw[4:UH_BYTES])))


def _tmets(path, channel=None):
    return sorted(p for p in Path(path).glob("*.timd/*.segd/*.tmet")
                  if channel is None or p.parent.parent.name == f"{channel}.timd")


def _set_rto(path, rto, channel=None):
    for tmet in _tmets(path, channel):
        raw = bytearray(tmet.read_bytes())
        struct.pack_into("<q", raw, S3_RTO_OFFSET, rto)
        _reseal(raw, METADATA_FILE_BYTES)
        tmet.write_bytes(bytes(raw))


def _rchar() -> int:
    for line in open("/proc/self/io"):
        if line.startswith("rchar"):
            return int(line.split()[1])
    raise RuntimeError("rchar not present")


@pytest.fixture
def session(tmp_path):
    return _write(tmp_path / "s.mefd")


# --------------------------------------------------------------------------- shape / agreement


def test_summary_agrees_with_the_reader(session):
    s = mef3io.inspect(session)
    assert isinstance(s, mef3io.SessionSummary)
    assert (s.name, s.layout, s.archive_bytes) == ("s", "directory", None)
    with mef3io.Reader(session) as r:
        assert [c.name for c in s.channels] == r.channels
        for c in s.channels:
            info = r.info(c.name)
            assert c.sampling_frequency == info["sampling_frequency"]
            assert c.units_conversion_factor == info["units_conversion_factor"]
            assert (c.start_time, c.end_time) == (info["start_time"], info["end_time"])
            assert c.number_of_samples == info["number_of_samples"]
            segs = r.segments(c.name)
            assert len(c.segments) == len(segs) == 2
            for got, want in zip(c.segments, segs):
                for k in ("segment", "start_time", "end_time", "start_sample",
                          "number_of_samples", "number_of_blocks", "provenance", "path"):
                    assert getattr(got, k) == want[k], k
                assert got.readable and got.time_offset == "none"
                seg_dir = Path(want["path"])
                assert got.tdat_bytes == (seg_dir / (seg_dir.name[:-5] + ".tdat")).stat().st_size
        recs = r.records(level="all")
    assert s.start_time == min(c.start_time for c in s.channels)
    assert s.end_time == max(c.end_time for c in s.channels)
    assert [(x.type, x.time, x.level, x.channel) for x in s.records] == \
        [(x["type"], x["time"], x["level"], x["channel"]) for x in recs]
    assert s.time_offset["status"] == "none" and not s.time_offset["date_recoverable"]
    assert s.encryption["encrypted"] is False and s.encryption["access"] == "full"
    assert s.warnings == [] and s.problems == []


def test_total_bytes_is_every_file(session):
    s = mef3io.inspect(session)
    on_disk = sum(p.stat().st_size for p in Path(session).rglob("*") if p.is_file())
    assert s.total_bytes == on_disk
    assert sum(c.bytes for c in s.channels) <= s.total_bytes


def test_gaps_between_segments(session):
    c = mef3io.inspect(session).channel("ch1")
    assert len(c.gaps) == 1 and not c.gaps_complete
    a, b = c.gaps[0]
    assert a == START + 10 * SECOND and b == START + 60 * SECOND


def test_gaps_inside_a_segment_need_the_index(tmp_path):
    path = tmp_path / "g.mefd"
    with mef3io.Writer(str(path)) as w:
        w.write("ch1", np.zeros(2560), START, FS)
        w.write("ch1", np.zeros(2560), START + 20 * SECOND, FS)  # same segment, a 10 s gap
    plain = mef3io.inspect(path).channel("ch1")
    assert len(plain.segments) == 1 and plain.gaps == [] and not plain.gaps_complete
    full = mef3io.inspect(path, read_index=True)
    c = full.channel("ch1")
    assert full.index_read and c.gaps_complete
    assert c.gaps == [(START + 10 * SECOND, START + 20 * SECOND)]


def test_record_times_widen_the_span(session):
    s = mef3io.inspect(session)
    edfa = next(r for r in s.records if r.type == "EDFA")
    assert edfa.duration == 3600 * SECOND
    assert edfa.last_time == START + 65 * SECOND + 3600 * SECOND
    assert s.records_end_time == edfa.last_time > s.end_time
    assert (s.first_time, s.last_time) == (s.start_time, edfa.last_time)
    assert s.channel("ch1").records == 1


def test_to_dict_is_json(session):
    s = mef3io.inspect(session, include_values=True, read_index=True)
    d = json.loads(json.dumps(s.to_dict()))
    assert d["channels"][0]["name"] == "ch1"
    assert d["records"][0]["fields"]["text"] == "seizure?"


# --------------------------------------------------------------------------- identity


def test_identity_reports_presence_and_length_only(session):
    s = mef3io.inspect(session)
    f = s.identity_field("subject_id")
    assert f.present is True and f.max_length == len("MRN-123") and f.values == []
    assert f.occurrences == 4 and f.distinct == 1          # 2 channels x 2 segments
    assert s.identity_field("subject_name_1").max_length == 4
    assert s.identity_field("anonymized_name").present is False
    text = s.identity_field("record_text")
    assert text.present and text.occurrences == 2 and text.values == []
    assert all(r.fields is None for r in s.records)
    assert {r.text_bytes for r in s.records} == {len("seizure?"), len("long event")}
    # No identifying VALUE anywhere in the summary.
    blob = json.dumps(s.to_dict())
    for secret in ("Jane", "Doe", "MRN-123", "Ward 9", "night study", "seizure?", "long event"):
        assert secret not in blob, secret


def test_identity_values_on_request(session):
    s = mef3io.inspect(session, include_values=True)
    assert s.identity_field("subject_id").values == ["MRN-123"]
    assert s.identity_field("recording_location").values == ["Ward 9"]
    assert s.identity_field("session_description").values == ["night study"]
    assert s.identity_field("session_name").values == ["s"]
    assert sorted(s.identity_field("record_text").values) == ["long event", "seizure?"]
    with mef3io.Reader(session) as r:
        assert [x.fields for x in s.records] == r.records(level="all")


def test_identity_table_has_a_fixed_shape(tmp_path):
    a = mef3io.inspect(_write(tmp_path / "a.mefd"))
    b = mef3io.inspect(_write(tmp_path / "b.mefd", metadata=mef3io.Metadata(), records=False))
    assert [f.name for f in a.identity] == [f.name for f in b.identity]
    assert b.identity_field("subject_id").present is False


# --------------------------------------------------------------------------- never the .tdat


@pytest.mark.skipif(not Path("/proc/self/io").exists(), reason="needs /proc/self/io")
@pytest.mark.parametrize("read_index", [False, True])
def test_inspect_never_reads_a_tdat(tmp_path, read_index):
    path = tmp_path / "big.mefd"
    rng = np.random.default_rng(0)
    with mef3io.Writer(str(path), durability="fast") as w:
        for i in range(20):  # incompressible: the .tdat is ~the raw size
            w.write_int32("ch1", rng.integers(-(2**30), 2**30, int(FS * 600), dtype=np.int32),
                          0.1, START + i * 600 * SECOND, FS)
        w.write_annotations([{"type": "Note", "time": START, "text": "x"}])
    tdat = sum(p.stat().st_size for p in path.rglob("*.tdat"))
    rest = sum(p.stat().st_size for p in path.rglob("*") if p.is_file() and p.suffix != ".tdat")
    assert tdat > 10_000_000 and rest < tdat / 10
    before = _rchar()
    s = mef3io.inspect(path, read_index=read_index)
    read = _rchar() - before
    assert s.channel("ch1").segments[0].tdat_bytes == tdat
    # Without the index it reads the .tmet and record files ONLY; with it, the
    # .tidx too. Never the .tdat.
    allowed = sum(p.stat().st_size for p in path.rglob("*")
                  if p.is_file() and p.suffix in ((".tmet", ".rdat", ".ridx", ".tidx")
                                                  if read_index else (".tmet", ".rdat", ".ridx")))
    assert read <= allowed + 64 * 1024, (
        f"inspect read {read:,} B; the files it may read are {allowed:,} B")


# --------------------------------------------------------------------------- tar


def test_tar_is_read_in_place(session):
    tar = mef3io.archive_session(session)
    d, t = mef3io.inspect(session).to_dict(), mef3io.inspect(tar).to_dict()
    assert t["layout"] == "tar" and t["archive_bytes"] == Path(tar).stat().st_size
    for x in (d, t):
        x.pop("path"), x.pop("layout"), x.pop("archive_bytes")
        for c in x["channels"]:
            for g in c["segments"]:
                g.pop("path")
    assert d == t


# --------------------------------------------------------------------------- recording-time offset


def test_a_recording_time_offset_is_reported_plainly(session):
    _set_rto(session, THREE_DAYS)
    s = mef3io.inspect(session)
    to = s.time_offset
    assert to["status"] == "applied" and to["date_recoverable"]
    assert to["recoverable_by"] == "anyone" and to["value"] is None
    assert to["segments_with_offset"] == 4 and not to["ambiguous"]
    assert "true UTC" in to["interpretation"]
    assert all(g.recording_time_offset is None and g.time_offset == "applied"
               for c in s.channels for g in c.segments)
    with mef3io.Reader(session) as r:
        assert s.start_time == min(r.info(c)["start_time"] for c in r.channels) \
            == START + THREE_DAYS
    w = [x for x in s.warnings if x.code == "legacy-time-offset"]
    assert w and not w[0].blocks_anonymize and "identifiable" in w[0].message
    assert str(THREE_DAYS) not in json.dumps(s.to_dict())  # the value stays hidden
    v = mef3io.inspect(session, include_values=True)
    assert v.time_offset["value"] == THREE_DAYS
    assert v.channels[0].segments[0].recording_time_offset == THREE_DAYS


def test_a_disputed_offset_is_reported_and_blocks(session):
    _set_rto(session, THREE_DAYS, channel="ch2")
    s = mef3io.inspect(session)
    assert s.time_offset["status"] == "disputed"
    codes = {w.code for w in s.anonymize_blockers}
    assert "time-offset-disputed" in codes
    # The session-level note is stored negated, so it cannot be placed.
    note = next(r for r in s.records if r.level == "session")
    assert note.time is None and "times-unresolved" in codes


def test_an_ambiguous_legacy_time_convention_is_flagged(session):
    _set_rto(session, THREE_DAYS)
    tmet = _tmets(session, "ch1")[0]
    raw = bytearray(tmet.read_bytes())
    struct.pack_into("<q", raw, 16, START)  # a NON-negated start under a non-zero offset
    _reseal(raw, METADATA_FILE_BYTES)
    tmet.write_bytes(bytes(raw))
    s = mef3io.inspect(session)
    assert s.time_offset["ambiguous"]
    w = next(w for w in s.warnings if w.code == "ambiguous-time-convention")
    assert w.blocks_anonymize and tmet.name in w.message


# --------------------------------------------------------------------------- encryption


@pytest.fixture
def encrypted(tmp_path):
    return _write(tmp_path / "e.mefd", password1="pw1", password2="pw2")


def test_encrypted_without_password_is_described_not_refused(encrypted):
    s = mef3io.inspect(encrypted)
    e = s.encryption
    assert e["encrypted"] and e["access"] == "none"
    assert e["section2_locked"] == e["section3_locked"] == 4
    assert e["record_bodies_locked"] == 2
    c = s.channel("ch1")
    assert c.sampling_frequency is None and c.number_of_samples is None
    # The offset is locked: times assume 0, as the Reader does — and say so.
    assert c.start_time == START and s.start_time == START
    assert s.offset_assumed_zero and c.offset_assumed_zero
    assert all(g.offset_assumed_zero for g in c.segments)
    assert s.time_offset["status"] == "locked"
    assert s.identity_field("subject_id").present is None   # may be there; locked
    assert s.identity_field("session_name").present is True  # universal header is plain
    assert all(r.body_locked and r.text_bytes == 0 for r in s.records)
    codes = {w.code for w in s.anonymize_blockers}
    assert {"locked", "offset-assumed-zero"} <= codes
    assert "times-unresolved" not in codes


def test_level_1_password_leaves_section_3_locked(encrypted):
    s = mef3io.inspect(encrypted, "pw1")
    assert s.encryption["access"] == "level-1"
    c = s.channel("ch1")
    assert c.sampling_frequency == FS and c.number_of_samples == 5120
    assert s.encryption["section3_locked"] == 4 and s.encryption["section2_locked"] == 0
    assert s.time_offset["status"] == "locked"
    assert s.identity_field("session_description").present is True


def test_level_2_password_opens_everything(encrypted, tmp_path):
    s = mef3io.inspect(encrypted, "pw2", include_values=True)
    plain = mef3io.inspect(_write(tmp_path / "e.mefd"), include_values=True)
    assert s.encryption["access"] == "full" and s.encryption["encrypted"]
    assert s.identity_field("subject_id").values == ["MRN-123"]
    assert [r.fields for r in s.records] == [r.fields for r in plain.records]
    assert (s.start_time, s.last_time) == (plain.start_time, plain.last_time)
    assert not s.anonymize_blockers


def test_a_wrong_password_raises(encrypted):
    with pytest.raises(mef3io.PasswordError):
        mef3io.inspect(encrypted, "nope")
    with pytest.raises(mef3io.PasswordError):
        mef3io.inspect(encrypted, "nope", strict=False)  # never "contained"


# --------------------------------------------------------------------------- damage / layout


def _corrupt(tmet):
    raw = bytearray(tmet.read_bytes())
    raw[2000] ^= 0xFF
    tmet.write_bytes(bytes(raw))


def test_strict_raises_and_lenient_reports(session):
    _corrupt(_tmets(session, "ch2")[1])
    with pytest.raises(mef3io.CrcError):
        mef3io.inspect(session)
    s = mef3io.inspect(session, strict=False)
    bad = s.channel("ch2").segments[1]
    assert not bad.readable and "CRC" in bad.problem and bad.start_time is None
    assert [p["segment_number"] for p in s.problems] == [1]
    assert s.problems[0]["kind"] == "segment"
    assert "unreadable-segments" in {w.code for w in s.anonymize_blockers}
    assert s.channel("ch2").number_of_samples is None
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        with mef3io.Reader(session, strict=False) as r:
            assert [p["segment_number"] for p in r.problems] == [1]


def test_video_and_record_only_channels_are_listed(session):
    (Path(session) / "cam.vidd").mkdir()
    with mef3io.Writer(session) as w:
        w.write_annotations([{"type": "Note", "time": START, "text": "later"}], "chNew")
    s = mef3io.inspect(session)
    assert s.video_channels == ["cam"] and s.record_only_channels == ["chNew"]
    assert [c.name for c in s.channels] == ["ch1", "ch2"]
    assert any(r.channel == "chNew" for r in s.records)
    assert "video-channels" in {w.code for w in s.anonymize_blockers}


def test_unset_declarations_are_reported(session):
    tmet = _tmets(session, "ch1")[0]
    raw = bytearray(tmet.read_bytes())
    s2 = UH_BYTES + 1536
    struct.pack_into("<I", raw, s2 + 6388, 0)  # maximum_difference_bytes (headers.cpp)
    _reseal(raw, METADATA_FILE_BYTES)
    tmet.write_bytes(bytes(raw))
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        with mef3io.Reader(session) as r:
            want = r.declaration_issues
    s = mef3io.inspect(session)
    assert s.declaration_issues == want and want
    assert "unset-declarations" in {w.code for w in s.warnings}


def test_not_a_session_path_raises(tmp_path):
    (tmp_path / "x").mkdir()
    with pytest.raises(mef3io.IoError):
        mef3io.inspect(tmp_path / "x")


# --------------------------------------------------------------------------- CLI


def _cli(*args):
    import os

    # The whole environment (Windows needs SYSTEMROOT etc.), with the package
    # under test first on the path — the same one this process imported.
    env = dict(os.environ)
    pkg_root = str(Path(mef3io.__file__).resolve().parent.parent)
    env["PYTHONPATH"] = os.pathsep.join([pkg_root, env.get("PYTHONPATH", "")])
    return subprocess.run([sys.executable, "-m", "mef3io", "inspect", *args],
                          capture_output=True, text=True, env=env)


def test_cli_text_and_json(session):
    out = _cli(session)
    assert out.returncode == 0, out.stderr
    assert "subject_id" in out.stdout and "MRN-123" not in out.stdout
    out = _cli(session, "--json", "--values")
    assert out.returncode == 0, out.stderr
    assert json.loads(out.stdout)["identity"][2]["values"] == ["MRN-123"]
    bad = _cli(str(Path(session).parent / "missing.mefd"))
    assert bad.returncode == 1 and bad.stderr


# --------------------------------------------------------------------------- review round 1


def _rdat_record_offsets(raw):
    """(offset, body_bytes) of each record in a .rdat image."""
    out, off = [], UH_BYTES
    n = struct.unpack_from("<q", raw, 32)[0]
    for _ in range(n):
        body = struct.unpack_from("<I", raw, off + 12)[0]
        out.append((off, body))
        off += 24 + body
    return out


def _set_record_time(rdat, stored):
    raw = bytearray(rdat.read_bytes())
    off, body = _rdat_record_offsets(raw)[0]
    struct.pack_into("<q", raw, off + 16, stored)
    struct.pack_into("<I", raw, off, m.crc32(bytes(raw[off + 4:off + 24 + body])))
    _reseal(raw, len(raw))
    rdat.write_bytes(bytes(raw))


def _time_fields(rec):
    f = rec.fields or {}
    out = [f.get(k) for k in ("time", "earliest_onset", "latest_offset", "timestamp",
                              "end_timestamp", "trace_timestamp")]
    for c in f.get("channels") or []:
        out += [c["onset"], c["offset"]]
    return out


def _disk_bytes(path):
    return sum(p.stat().st_size for p in Path(path).rglob("*") if p.is_file())


def test_fields_never_carry_a_guessed_time_when_the_offset_is_disputed(session):
    _set_rto(session, THREE_DAYS, channel="ch2")
    s = mef3io.inspect(session, include_values=True)
    note = next(r for r in s.records if r.level == "session")
    assert note.time is None and not note.times_known
    assert note.fields["text"] == "seizure?"            # the record is still there
    assert all(t is None for t in _time_fields(note))   # but no placeholder date
    # ch1's channel-level record: ch1's segments agree (offset 0), so it is placed.
    edfa = next(r for r in s.records if r.type == "EDFA")
    assert edfa.times_known and edfa.fields["time"] == START + 65 * SECOND


def test_locked_offset_times_are_flagged_wherever_they_appear(tmp_path):
    """Section 3 locked: record times assume offset 0 like the Reader's, and
    every record (and the fields inside it) is flagged, never silently."""
    path = _write(tmp_path / "e.mefd", password1="pw1", password2="pw2")
    s = mef3io.inspect(path, "pw1", include_values=True)
    # (Reader.records refuses level-2 bodies with level 1; the headers are plain.)
    assert [(x.type, x.time) for x in s.records] == [("Note", START + SECOND),
                                                     ("EDFA", START + 65 * SECOND)]
    for rec in s.records:
        assert rec.times_known and rec.offset_assumed_zero and rec.body_locked
        assert rec.fields["time"] == rec.time
    full = mef3io.inspect(path, "pw2")
    assert not full.offset_assumed_zero
    assert not any(x.offset_assumed_zero for x in full.records)
    assert not any(g.offset_assumed_zero for c in full.channels for g in c.segments)


def _legacy_locked_session(path, rto):
    """A session the legacy stack wrote with record_offset = rto and both
    passwords: section 3 (and so the offset) is level-2 encrypted."""
    pytest.importorskip("mef_tools", reason="legacy oracle not installed")
    pd = pytest.importorskip("pandas")
    from mef_tools.io import MefWriter

    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        w = MefWriter(str(path), overwrite=True, password1="l1", password2="l2")
        w.record_offset = rto
        w.write_data(np.arange(5000, dtype=np.int32) * 1.0, "ch1", START, FS, precision=0)
        w.write_annotations(pd.DataFrame([{"time": START + SECOND, "type": "Note", "text": "n"}]))
        del w
    return str(path)


def test_level_1_on_an_offset_zero_session_gives_the_right_times_unverified(tmp_path):
    """User decision for #88: align with the Reader. rto = 0 (what mef3io and
    pymef always write): the assumed 0 is right — but never called verified."""
    path = _legacy_locked_session(tmp_path / "z.mefd", 0)
    one, two = mef3io.inspect(path, "l1"), mef3io.inspect(path, "l2")
    assert two.time_offset["status"] == "none" and not two.offset_assumed_zero
    assert one.start_time == two.start_time == START
    assert [r.time for r in one.records] == [r.time for r in two.records] == [START + SECOND]
    assert one.offset_assumed_zero and one.time_offset["status"] == "locked"
    assert all(r.offset_assumed_zero for r in one.records)
    assert "offset-assumed-zero" in {w.code for w in one.anonymize_blockers}
    with mef3io.Reader(path, "l1") as r:
        assert r.info("ch1")["start_time"] == one.start_time


def test_level_1_on_an_offset_session_gives_the_masked_times_unverified(tmp_path):
    """rto != 0 (meflib's deprecated date masking): level 1 sees the MASKED
    times, as meflib intended and as the Reader shows — flagged, and
    date_recoverable stays what section 1 says: by the level-2 password."""
    rto = START - 3600 * SECOND
    path = _legacy_locked_session(tmp_path / "m.mefd", rto)
    one, two = mef3io.inspect(path, "l1"), mef3io.inspect(path, "l2")
    assert two.start_time == START and two.time_offset["status"] == "applied"
    assert two.time_offset["recoverable_by"] == "level-2 password"
    assert one.start_time == START - rto                  # the masked time of day
    assert one.records[0].time == START + SECOND - rto
    assert one.offset_assumed_zero and one.records[0].offset_assumed_zero
    assert one.time_offset["status"] == "locked"
    assert "not verified" in one.time_offset["interpretation"]
    with mef3io.Reader(path, "l1") as r:
        assert r.info("ch1")["start_time"] == one.start_time
    assert "offset-assumed-zero" in {w.code for w in one.anonymize_blockers}


def test_a_locked_segment_against_a_readable_offset_is_a_dispute_not_a_choice(tmp_path):
    """A locked section 3 takes part with its assumed 0 (as the Reader's
    segment_rto does): next to a readable non-zero offset, a session-level
    record that depends on the offset is unplaced, not placed with either."""
    path = _write(tmp_path / "e.mefd", password1="pw1", password2="pw2")
    plain = _write(tmp_path / "p.mefd")
    _set_rto(plain, THREE_DAYS, channel="ch2")
    # Swap ch1 of the plain session for the encrypted one's: ch1 locked at
    # level 1, ch2 readable with an offset.
    shutil.rmtree(Path(plain) / "ch1.timd")
    shutil.copytree(Path(path) / "ch1.timd", Path(plain) / "ch1.timd")
    s = mef3io.inspect(plain, "pw1")
    note = next(r for r in s.records if r.level == "session")
    assert note.time is None and not note.times_known
    # ... and the session says so, not "applied".
    assert s.time_offset["status"] == "disputed"
    codes = {w.code for w in s.anonymize_blockers}
    assert {"time-offset-disputed", "offset-assumed-zero"} <= codes


def test_an_assumed_offset_next_to_a_read_one_makes_the_gaps_uncertain(tmp_path):
    """Round 6: one channel, segment 0 locked (assumed 0), segment 1 read with
    a 3-day offset. The 3-day 'gap' between them may be the assumption."""
    enc = _write(tmp_path / "e.mefd", password1="pw1", password2="pw2", records=False)
    plain = _write(tmp_path / "p.mefd", records=False)
    _set_rto(plain, THREE_DAYS, channel="ch1")
    seg0 = sorted(Path(plain).glob("ch1.timd/*.segd"))[0]
    shutil.rmtree(seg0)
    shutil.copytree(sorted(Path(enc).glob("ch1.timd/*.segd"))[0], seg0)
    for read_index in (False, True):
        c = mef3io.inspect(plain, "pw1", read_index=read_index).channel("ch1")
        assert c.offset_assumed_zero and c.gaps
        assert not c.gaps_certain and not c.gaps_complete
    # ch2 is all read, no assumption: certain as before.
    assert mef3io.inspect(plain, "pw1").channel("ch2").gaps_certain


def test_a_locked_segment_with_absolute_times_is_not_flagged(tmp_path):
    """Round 6: the flag marks what the assumption TOUCHED. Universal-header
    times stored non-negative are absolute whatever the offset; a channel can
    still be flagged through its channel-level records."""
    path = _write(tmp_path / "e.mefd", password1="pw1", password2="pw2")
    for tmet in _tmets(path, "ch1"):
        raw = bytearray(tmet.read_bytes())
        for off in (16, 24):                       # start_time, end_time
            stored = struct.unpack_from("<q", raw, off)[0]
            struct.pack_into("<q", raw, off, -stored)   # absolute: non-negated
        struct.pack_into("<I", raw, 0, m.crc32(bytes(raw[4:UH_BYTES])))  # header CRC only
        tmet.write_bytes(bytes(raw))
    s = mef3io.inspect(path, "pw1")
    c = s.channel("ch1")
    assert c.start_time == START                    # absolute, nothing assumed
    assert not any(g.offset_assumed_zero for g in c.segments)
    assert all(g.time_offset == "locked" for g in c.segments)
    edfa = next(r for r in s.records if r.type == "EDFA")
    assert edfa.offset_assumed_zero                 # its time was stored negated
    assert c.offset_assumed_zero                    # flagged through that record only


def test_read_index_under_a_locked_offset_matches_full_access(tmp_path):
    path = _write(tmp_path / "e.mefd", password1="pw1", password2="pw2")
    one = mef3io.inspect(path, "pw1", read_index=True).channel("ch1")
    two = mef3io.inspect(path, "pw2", read_index=True).channel("ch1")
    assert one.gaps == two.gaps and one.gaps_complete and two.gaps_complete
    assert one.offset_assumed_zero and not two.offset_assumed_zero


def test_cli_marks_assumed_times(tmp_path):
    path = _write(tmp_path / "e.mefd", password1="pw1", password2="pw2")
    text = str(mef3io.inspect(path, "pw1"))
    assert "assume offset 0" in text and "samples:" in text
    assert "*" in text.split("samples:")[1].splitlines()[0]
    assert "assume offset 0" not in str(mef3io.inspect(path, "pw2"))


def test_an_unreadable_segment_leaves_identity_unknown_not_absent(session):
    for tmet in _tmets(session):
        _corrupt(tmet)
    s = mef3io.inspect(session, strict=False)
    assert all(not g.readable and g.time_offset == "unknown"
               for c in s.channels for g in c.segments)
    for name in ("subject_id", "subject_name_1", "session_description", "anonymized_name"):
        assert s.identity_field(name).present is None, name
    # session_name is still SEEN in the record files' universal headers.
    assert s.identity_field("session_name").present is True
    # And no offset is guessed for the records of segments nobody could read.
    assert all(r.time is None for r in s.records)
    assert "times-unresolved" in {w.code for w in s.warnings}
    # Sizes are facts about the disk and still count — the .tmet included.
    assert all(g.tdat_bytes > 0 and g.tmet_bytes > 0 for c in s.channels for g in c.segments)
    assert s.total_bytes == _disk_bytes(session)
    assert s.time_offset["segments_unknown"] == 4


def test_a_failed_record_file_contributes_nothing_but_its_size(session):
    rdat = Path(session) / "s.rdat"
    healthy = mef3io.inspect(session)
    raw = bytearray(rdat.read_bytes())
    off, body = _rdat_record_offsets(raw)[0]
    raw[off + 30] ^= 0xFF                      # inside the body: the record CRC fails
    rdat.write_bytes(bytes(raw))
    with pytest.raises(mef3io.CrcError):
        mef3io.inspect(session)
    s = mef3io.inspect(session, strict=False)
    assert [p["kind"] for p in s.problems] == ["records"]
    assert "unreadable-records" in {w.code for w in s.anonymize_blockers}
    assert [r.level for r in s.records] == ["channel"]          # only the healthy file
    name = s.identity_field("session_name")
    assert name.occurrences == healthy.identity_field("session_name").occurrences - 1
    assert name.locked == 1
    assert s.total_bytes == healthy.total_bytes                 # still on disk
    assert s.records_start_time == START + 65 * SECOND          # the failed note is gone


def test_ambiguity_from_a_record_header_time(session):
    _set_rto(session, THREE_DAYS)
    assert not mef3io.inspect(session).time_offset["ambiguous"]
    _set_record_time(Path(session) / "s.rdat", START)  # non-negated under an offset
    s = mef3io.inspect(session)
    assert s.time_offset["ambiguous"]
    assert "s.rdat" in next(w for w in s.warnings if w.code == "ambiguous-time-convention").message


@pytest.mark.parametrize("where", ["header", "entry"])
def test_ambiguity_from_the_block_index_needs_the_index(session, where):
    _set_rto(session, THREE_DAYS)
    tidx = sorted(Path(session).glob("ch1.timd/*.segd/*.tidx"))[0]
    raw = bytearray(tidx.read_bytes())
    struct.pack_into("<q", raw, 16 if where == "header" else UH_BYTES + 8, START)
    tidx.write_bytes(bytes(raw))
    assert not mef3io.inspect(session).time_offset["ambiguous"]     # the index is not read
    s = mef3io.inspect(session, read_index=True)
    assert s.time_offset["ambiguous"]
    assert tidx.name in next(w for w in s.warnings if w.code == "ambiguous-time-convention").message


def test_gap_tolerance(tmp_path):
    fs = 1000.0 / 3.0                           # fractional rate: block ends are not whole us
    path = tmp_path / "t.mefd"
    with mef3io.Writer(str(path)) as w:
        w.write("ch1", np.zeros(3000), START, fs)
        # Back to back on the grid: not a gap.
        w.write("ch1", np.zeros(3000), START + round(3000 * 1e6 / fs), fs)
        # One whole missing second later: a gap.
        w.write("ch1", np.zeros(3000), START + round(6000 * 1e6 / fs) + SECOND, fs)
    c = mef3io.inspect(path, read_index=True).channel("ch1")
    assert c.gaps_complete and c.gaps_certain
    assert len(c.gaps) == 1
    a, b = c.gaps[0]
    assert abs((b - a) - SECOND) <= 3 and abs(a - (START + round(6000 * 1e6 / fs))) <= 3


def test_gaps_next_to_an_unreadable_segment_are_uncertain(session):
    _corrupt(_tmets(session, "ch1")[0])
    s = mef3io.inspect(session, strict=False)
    assert not s.channel("ch1").gaps_certain
    assert s.channel("ch2").gaps_certain is True


def test_tar_of_an_encrypted_session(tmp_path):
    path = _write(tmp_path / "e.mefd", password1="pw1", password2="pw2")
    tar = mef3io.archive_session(path)
    for pw in ("", "pw1", "pw2"):
        d, t = mef3io.inspect(path, pw), mef3io.inspect(tar, pw)
        assert t.encryption == d.encryption
        assert t.start_time == d.start_time and t.records_end_time == d.records_end_time


def test_present_is_in_the_json(session):
    d = mef3io.inspect(session).to_dict()
    assert {f["name"]: f["present"] for f in d["identity"]}["subject_id"] is True


def test_where_an_identifying_session_name_can_appear(tmp_path):
    """The session folder is named after the session, so its name IS the
    session_name. Without include_values it may appear only where paths and
    names are returned — never as an identity value."""
    secret = "JaneDoeMRN123"
    path = _write(tmp_path / f"{secret}.mefd")
    s = mef3io.inspect(path)
    assert s.identity_field("session_name").values == []
    hits = []

    def walk(x, where):
        if isinstance(x, dict):
            for k, v in x.items():
                walk(v, where + [k])
        elif isinstance(x, (list, tuple)):
            for v in x:
                walk(v, where)
        elif isinstance(x, str) and secret in x:
            hits.append(where[-1])

    walk(s.to_dict(), [])
    assert set(hits) <= {"name", "path"}, hits


def test_star_import_does_not_shadow_the_standard_library_inspect():
    ns = {}
    exec("import inspect\nfrom mef3io import *", ns)
    import inspect as stdlib_inspect

    assert ns["inspect"] is stdlib_inspect
    assert callable(mef3io.inspect) and mef3io.inspect is not stdlib_inspect


def test_cli_password_from_the_environment(tmp_path):
    import os

    path = _write(tmp_path / "e.mefd", password1="pw1", password2="pw2")
    env = dict(os.environ)
    env["PYTHONPATH"] = os.pathsep.join([str(Path(mef3io.__file__).resolve().parent.parent),
                                         env.get("PYTHONPATH", "")])
    env["MEF3IO_TEST_PW"] = "pw2"
    out = subprocess.run([sys.executable, "-m", "mef3io", "inspect", path, "--json",
                          "--password-env", "MEF3IO_TEST_PW"],
                         capture_output=True, text=True, env=env)
    assert out.returncode == 0, out.stderr
    assert json.loads(out.stdout)["encryption"]["access"] == "full"


# --------------------------------------------------------------------------- review round 2


def test_a_channel_whose_segments_all_failed_does_not_borrow_another_offset(session):
    """N1: ch2 says 3 days, ch1's segments are unreadable. ch1's records must
    not be placed with ch2's offset — ch1 may carry another one."""
    _set_rto(session, THREE_DAYS, channel="ch2")
    for tmet in _tmets(session, "ch1"):
        _corrupt(tmet)
    s = mef3io.inspect(session, strict=False)
    for r in s.records:
        if r.level in ("session", "channel"):
            assert r.time is None and not r.times_known, r
    assert "times-unresolved" in {w.code for w in s.anonymize_blockers}
    assert s.time_offset["segments_unknown"] == 2
    assert "could not be read" in s.time_offset["interpretation"]


def test_an_unreadable_segment_makes_a_zero_offset_session_unknown_too(session):
    """N1: the readable segments say 0, but a failed one may not."""
    _corrupt(_tmets(session, "ch2")[1])
    s = mef3io.inspect(session, strict=False)
    note = next(r for r in s.records if r.level == "session")
    assert note.time is None and not note.times_known
    assert s.time_offset["segments_unknown"] == 1
    # ch1's own records are unaffected: all of ch1's segments were read.
    assert next(r for r in s.records if r.type == "EDFA").times_known


def test_an_incomplete_segment_folder_is_reported_not_dropped(session):
    """N2: a .segd missing its .tdat used to vanish without a trace, taking its
    records' free text out of the identity table."""
    seg = sorted(Path(session).glob("ch1.timd/*.segd"))[0]
    for ext in ("rdat", "ridx"):
        shutil.copy(Path(session) / "ch1.timd" / f"ch1.{ext}", seg / f"{seg.stem}.{ext}")
    next(seg.glob("*.tdat")).unlink()
    with mef3io.Reader(session) as r:  # the Reader still skips it, as before
        assert len(r.segments("ch1")) == 1
    for strict in (True, False):
        s = mef3io.inspect(session, strict=strict)
        c = s.channel("ch1")
        assert [g.segment for g in c.segments] == [0, 1]
        bad = c.segments[0]
        assert not bad.readable and ".tdat" in bad.problem and bad.time_offset == "unknown"
        assert bad.has_records and bad.tdat_bytes == 0 and bad.tmet_bytes > 0
        assert s.identity_field("record_text").locked >= 1
        assert s.total_bytes == _disk_bytes(session)
        assert not c.gaps_certain and c.number_of_samples is None
        assert "incomplete-segments" in {w.code for w in s.anonymize_blockers}
    # Its index was never read, so the gaps cannot be complete (F2).
    assert not mef3io.inspect(session, read_index=True).channel("ch1").gaps_complete


def test_a_channel_folder_without_a_complete_segment_is_reported(session):
    for f in Path(session).glob("ch2.timd/*.segd/*.tdat"):
        f.unlink()
    s = mef3io.inspect(session)
    assert [c.name for c in s.channels] == ["ch1"]
    assert s.incomplete_channels == ["ch2"]
    assert s.total_bytes == _disk_bytes(session)
    assert s.identity_field("subject_id").locked == 2
    assert "incomplete-segments" in {w.code for w in s.anonymize_blockers}


def test_ambiguous_session_records_still_count_and_stay_unknown(session):
    """N4: several .rdat at the root and no <session>.rdat — none is read."""
    root = Path(session)
    for ext in ("rdat", "ridx"):
        shutil.copy(root / f"s.{ext}", root / f"a.{ext}")
        (root / f"s.{ext}").rename(root / f"b.{ext}")
    s = mef3io.inspect(session, strict=False)
    assert [p["kind"] for p in s.problems] == ["records"]
    assert s.total_bytes == _disk_bytes(session)
    assert s.identity_field("record_text").locked == 2
    assert s.identity_field("session_name").locked == 2


def test_negative_sample_counts_are_unknown_not_summed(session):
    """N5: a hostile section 2 count must not overflow or go negative."""
    tmet = _tmets(session, "ch1")[0]
    raw = bytearray(tmet.read_bytes())
    s2 = UH_BYTES + 1536
    n_samples_off = s2 + 6360            # number_of_samples (headers.cpp)
    struct.pack_into("<q", raw, n_samples_off, -5)
    _reseal(raw, METADATA_FILE_BYTES)
    tmet.write_bytes(bytes(raw))
    assert mef3io.inspect(session).channel("ch1").number_of_samples is None


# --------------------------------------------------------------------------- review round 3


def test_an_odd_entry_in_an_incomplete_folder_is_contained(session):
    """F1: measuring the files of a half-written folder is contained too."""
    seg = sorted(Path(session).glob("ch1.timd/*.segd"))[0]
    next(seg.glob("*.tdat")).unlink()
    (seg / f"{seg.stem}.rdat").mkdir()          # a directory where a file should be
    s = mef3io.inspect(session, strict=False)   # must not abort, on any OS
    assert not s.channel("ch1").segments[0].readable
    # POSIX refuses to size a directory, and that failure is contained and
    # reported. MSVC's std::filesystem::file_size returns without an error for
    # one, so there is nothing to contain there.
    if sys.platform != "win32":
        assert any(p["kind"] == "segment" for p in s.problems)


def test_a_stray_root_record_file_is_reported(session):
    """F3: a second .rdat next to <session>.rdat is not read (meflib and the
    Reader ignore it) but it is on disk and may hold text."""
    root = Path(session)
    for ext in ("rdat", "ridx"):
        shutil.copy(root / f"s.{ext}", root / f"other.{ext}")
    s = mef3io.inspect(session)
    assert s.total_bytes == _disk_bytes(session)
    w = next(w for w in s.warnings if w.code == "unread-record-files")
    assert w.blocks_anonymize and "other.rdat" in w.message
    assert s.identity_field("record_text").locked == 1
    assert [r.level for r in s.records].count("session") == 1


def test_no_readable_segment_is_status_unknown(session):
    """F4: "none" would read as "no offset"; nothing could be read at all."""
    for f in Path(session).rglob("*.tdat"):
        f.unlink()
    s = mef3io.inspect(session)
    assert s.time_offset["status"] == "unknown"
    assert s.time_offset["segments_unknown"] == 4


def _seiz_record(disk_time, onset):
    """A Seiz 1.0 record with one channel entry (mefrec.h:141/152)."""
    body = bytearray(SEIZ_BYTES + SEIZ_CHANNEL_BYTES)
    struct.pack_into("<q", body, 0, onset)            # earliest_onset
    struct.pack_into("<q", body, 8, onset + SECOND)   # latest_offset
    struct.pack_into("<q", body, 16, SECOND)          # duration
    struct.pack_into("<i", body, 24, 1)               # number_of_channels
    ch = SEIZ_BYTES
    body[ch:ch + 3] = b"LA1"
    struct.pack_into("<q", body, ch + 256, onset)
    struct.pack_into("<q", body, ch + 264, onset + SECOND)
    body += b"\x7e" * ((-len(body)) % 16)
    hdr = bytearray(24)
    hdr[4:8] = b"Seiz"
    hdr[9], hdr[10] = 1, 0
    struct.pack_into("<b", hdr, 11, -1)
    struct.pack_into("<I", hdr, 12, len(body))
    struct.pack_into("<q", hdr, 16, disk_time)
    rec = hdr + body
    struct.pack_into("<I", rec, 0, m.crc32(bytes(rec[4:])))
    return bytes(rec)


def test_seiz_channel_times_are_none_when_the_offset_is_disputed(session):
    """F5 / N7: the per-channel onset/offset of a Seiz body must not keep a
    placeholder when the file's offset is unknown."""
    rdat = Path(session) / "s.rdat"
    t = START + 2 * SECOND
    raw = bytearray(rdat.read_bytes()[:UH_BYTES]) + _seiz_record(-t, -t)  # negated: offset-dependent
    struct.pack_into("<q", raw, 32, 1)
    _reseal(raw, len(raw))
    rdat.write_bytes(bytes(raw))
    ok = mef3io.inspect(session, include_values=True)
    seiz = next(r for r in ok.records if r.type == "Seiz")
    assert seiz.times_known and seiz.fields["channels"][0]["onset"] == t
    _set_rto(session, THREE_DAYS, channel="ch2")
    s = mef3io.inspect(session, include_values=True)
    seiz = next(r for r in s.records if r.type == "Seiz")
    assert not seiz.times_known
    assert seiz.fields["channels"][0]["name"] == "LA1"
    assert all(t is None for t in _time_fields(seiz)), seiz.fields


# --------------------------------------------------------------------------- cancel / progress (#69)


def test_inspect_progress_counts_the_metadata_bytes(session):
    calls = []
    plain = mef3io.inspect(session)
    s = mef3io.inspect(session, progress=lambda d, t: calls.append((d, t)))
    assert s == plain                                     # the hooks change nothing
    tmet = sum(p.stat().st_size for p in Path(session).rglob("*.tmet"))
    assert calls[0] == (0, tmet) and calls[-1] == (tmet, tmet)
    assert all(a[0] <= b[0] for a, b in zip(calls, calls[1:]))
    tidx = sum(p.stat().st_size for p in Path(session).rglob("*.tidx"))
    idx_calls = []
    mef3io.inspect(session, read_index=True, progress=lambda d, t: idx_calls.append((d, t)))
    assert idx_calls[-1] == (tmet + tidx, tmet + tidx)


def test_inspect_cancel_raises_and_is_never_contained(session):
    token = mef3io.CancelToken()
    token.cancel()
    for strict in (True, False):  # lenient mode must not report a cancel as damage
        with pytest.raises(mef3io.Cancelled):
            mef3io.inspect(session, strict=strict, cancel=token)


def test_inspect_a_raising_progress_callback_aborts_with_its_own_exception(session):
    class Boom(Exception):
        pass

    def cb(done, total):
        if done:
            raise Boom("stop")

    with pytest.raises(Boom):
        mef3io.inspect(session, strict=False, progress=cb)


def test_inspect_bad_hook_arguments_are_type_errors(session):
    with pytest.raises(TypeError):
        mef3io.inspect(session, cancel=object())
    with pytest.raises(TypeError):
        mef3io.inspect(session, progress=3)


def test_inspect_cancel_noticed_mid_run_in_lenient_mode(session):
    token = mef3io.CancelToken()

    def cb(done, total):
        if done:
            token.cancel()

    with pytest.raises(mef3io.Cancelled):
        mef3io.inspect(session, strict=False, read_index=True, cancel=token, progress=cb)


def test_inspect_progress_stays_monotonic_past_a_failed_segment(session):
    _corrupt(_tmets(session, "ch1")[1])
    calls = []
    s = mef3io.inspect(session, strict=False, read_index=True,
                       progress=lambda d, t: calls.append((d, t)))
    assert s.problems
    assert all(a[0] <= b[0] for a, b in zip(calls, calls[1:]))
    assert calls[-1][0] == calls[-1][1] == calls[0][1]
