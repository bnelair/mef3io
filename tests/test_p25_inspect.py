"""P25: ``mef3io.inspect`` (#67) — everything about a session without its samples.

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
    assert read <= rest + 64 * 1024, (
        f"inspect read {read:,} B; everything but the .tdat is {rest:,} B — a .tdat was read")


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
    # Times were stored relative to an offset that is locked: unknown, not guessed.
    assert c.start_time is None and s.start_time is None
    assert s.time_offset["status"] == "locked"
    assert s.identity_field("subject_id").present is None   # may be there; locked
    assert s.identity_field("session_name").present is True  # universal header is plain
    assert all(r.body_locked and r.text_bytes == 0 for r in s.records)
    codes = {w.code for w in s.anonymize_blockers}
    assert {"locked", "times-unresolved"} <= codes


def test_level_1_password_leaves_section_3_locked(encrypted):
    s = mef3io.inspect(encrypted, "pw1")
    assert s.encryption["access"] == "level-1"
    c = s.channel("ch1")
    assert c.sampling_frequency == FS and c.number_of_samples == 5120
    assert s.encryption["section3_locked"] == 4 and s.encryption["section2_locked"] == 0
    assert c.start_time is None and s.time_offset["status"] == "locked"
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
