"""P27: examples/12_edf_to_mef.py converts EDF / EDF+ / BDF correctly.

docs/howto/edf_to_mef.md teaches this script, so it is held to what the page
promises: per-channel rates kept, samples within half a stored count, the start
time converted with the zone the user names (and refused without one), EDF+
annotations as Note/EDFA records, EDF+D gaps as NaN at the right time, and
patient identity copied only on request. Needs the optional pyedflib
(``pip install pyedflib``; from mef3io 1.3 also ``"mef3io[edf]"``); skipped cleanly without it.
"""
from __future__ import annotations

import datetime as dt
import importlib.util
import shutil
from pathlib import Path

import numpy as np
import pytest

import mef3io

pyedflib = pytest.importorskip("pyedflib", reason="optional EDF reader not installed")

REPO = Path(__file__).resolve().parent.parent
_spec = importlib.util.spec_from_file_location("edf_to_mef", REPO / "examples" / "12_edf_to_mef.py")
ex = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(ex)

LOCAL_START = dt.datetime(2024, 3, 5, 10, 20, 30)  # CET (UTC+1) in Europe/Prague
UTC_START = 1_709_630_430_000_000  # 2024-03-05 09:20:30 UTC
SECONDS = 6


def _write_edf(path, bdf=False, patient=False, eeg_range=(-500, 500)):
    ftype = pyedflib.FILETYPE_BDFPLUS if bdf else pyedflib.FILETYPE_EDFPLUS
    dmax = 2**23 - 1 if bdf else 32767
    w = pyedflib.EdfWriter(str(path), 2, file_type=ftype)
    w.setSignalHeaders([
        dict(label="EEG Fp1/Ref", dimension="uV", sample_frequency=200, physical_min=eeg_range[0],
             physical_max=eeg_range[1], digital_min=-dmax - 1, digital_max=dmax),
        dict(label="ECG", dimension="mV", sample_frequency=50, physical_min=-4,
             physical_max=4, digital_min=-dmax - 1, digital_max=dmax),
    ])
    w.setStartdatetime(LOCAL_START)
    if patient:
        w.setPatientName("Doe_Jane")
        w.setPatientCode("MRN123")
    w.writeAnnotation(1.5, 0.25, "artifact")
    w.writeAnnotation(4.0, -1, "eyes closed")
    rng = np.random.default_rng(0)
    lo, hi = sorted(eeg_range)
    eeg = rng.uniform(lo + 0.1 * (hi - lo), hi - 0.1 * (hi - lo), 200 * SECONDS)
    w.writeSamples([eeg, rng.uniform(-3, 3, 50 * SECONDS)])
    w.close()


def _shift_records(path, from_record, shift_s, discontinuous=True):
    """Records >= from_record start shift_s later (and the file becomes +D)."""
    b = bytearray(Path(path).read_bytes())
    width = 3 if b[0] == 0xFF else 2
    ns, hb, nrec = int(b[252:256]), int(b[184:192]), int(b[236:244])
    nsamp = [int(b[256 + ns * 216 + 8 * i:256 + ns * 216 + 8 * i + 8]) for i in range(ns)]
    ann = next(i for i in range(ns) if b[256 + 16 * i:256 + 16 * i + 16].strip().endswith(b"Annotations"))
    start = sum(nsamp[:ann]) * width
    size = nsamp[ann] * width
    if discontinuous:
        b[192:197] = b"BDF+D" if width == 3 else b"EDF+D"
    for k in range(from_record, nrec):
        off = hb + k * sum(nsamp) * width + start
        rec = bytes(b[off:off + size])
        stamp = rec.split(b"\x14")[0]
        new = ("+%g" % (float(stamp) + shift_s)).encode() + rec[len(stamp):]
        b[off:off + size] = new.ljust(size, b"\0")[:size]
    Path(path).write_bytes(bytes(b))


@pytest.mark.parametrize("bdf", [False, True], ids=["edf", "bdf"])
def test_continuous_file_round_trips(tmp_path, bdf):
    src, out = tmp_path / ("x.bdf" if bdf else "x.edf"), str(tmp_path / "x.mefd")
    _write_edf(src, bdf=bdf)
    res = ex.convert(src, out, "Europe/Prague")
    ex.verify(out, res)
    with mef3io.Reader(out) as r:
        assert sorted(r.channels) == ["ECG", "EEG Fp1_Ref"]
        eeg, ecg = r.info("EEG Fp1_Ref"), r.info("ECG")
        assert (eeg["sampling_frequency"], ecg["sampling_frequency"]) == (200, 50)
        assert (eeg["units_description"], ecg["units_description"]) == ("uV", "mV")
        assert eeg["start_time"] == ecg["start_time"] == UTC_START
        assert len(r.read("ECG")) == 50 * SECONDS
        recs = sorted(r.records(), key=lambda x: x["time"])
        assert [(x["type"], x["text"]) for x in recs] == [("EDFA", "artifact"), ("Note", "eyes closed")]
        assert recs[0]["time"] == UTC_START + 1_500_000 and recs[0]["duration"] == 250_000
        assert recs[1]["time"] == UTC_START + 4_000_000


def test_sub_second_start_is_kept(tmp_path):
    # EDF+ puts the sub-second part of the start in the first record's time
    # stamp; pyedflib's getStartdatetime() mis-scales it, the example does not.
    src, out = tmp_path / "s.edf", str(tmp_path / "s.mefd")
    _write_edf(src)
    _shift_records(src, 0, 0.25, discontinuous=False)
    ex.verify(out, ex.convert(src, out, "Europe/Prague"))
    with mef3io.Reader(out) as r:
        assert r.info("ECG")["start_time"] == UTC_START + 250_000
        # annotation onsets are relative to the header second, not the stamp
        assert min(x["time"] for x in r.records()) == UTC_START + 1_500_000


@pytest.mark.parametrize("bdf", [False, True], ids=["edf", "bdf"])
def test_discontinuous_records_become_gaps(tmp_path, bdf):
    src, out = tmp_path / ("d.bdf" if bdf else "d.edf"), str(tmp_path / "d.mefd")
    _write_edf(src, bdf=bdf)
    _shift_records(src, from_record=3, shift_s=2)  # 2 s gap after 3 s of data
    with pytest.raises(OSError):
        pyedflib.EdfReader(str(src)).close()  # why the example reads +D itself
    res = ex.convert(src, out, "+01:00")
    ex.verify(out, res)
    with mef3io.Reader(out) as r:
        y = r.read("EEG Fp1_Ref")
        assert len(y) == 200 * (SECONDS + 2)
        assert np.isnan(y[600:1000]).all() and not np.isnan(y[:600]).any()
        assert not np.isnan(y[1000:]).any()
        assert r.info("EEG Fp1_Ref")["start_time"] == UTC_START
        assert len(r.records()) == 2


def test_time_zone_is_required_and_never_guessed(tmp_path):
    src = tmp_path / "x.edf"
    _write_edf(src)
    with pytest.raises(ValueError, match="time zone"):
        ex.convert(src, str(tmp_path / "x.mefd"))
    assert not (tmp_path / "x.mefd").exists()
    # 02:30 on 2024-03-31 does not exist in Prague (clocks jump 02:00 -> 03:00).
    with pytest.raises(ValueError, match="daylight-saving"):
        ex.local_to_uutc(dt.datetime(2024, 3, 31, 2, 30), ex.parse_timezone("Europe/Prague"))
    assert ex.local_to_uutc(LOCAL_START, ex.parse_timezone("UTC")) == UTC_START + 3_600_000_000


@pytest.mark.parametrize("discontinuous", [False, True], ids=["pyedflib", "edf+d"])
def test_patient_identity_is_copied_only_on_request(tmp_path, discontinuous):
    src = tmp_path / "p.edf"
    _write_edf(src, patient=True)
    if discontinuous:
        _shift_records(src, 3, 1)
    plain, copied = str(tmp_path / "plain.mefd"), str(tmp_path / "copied.mefd")
    ex.convert(src, plain, "UTC")
    ex.convert(src, copied, "UTC", copy_patient=True)
    with mef3io.Reader(plain) as r:
        assert (r.metadata.subject.name_1, r.metadata.subject.id) == ("", "")
    with mef3io.Reader(copied) as r:
        assert (r.metadata.subject.name_1, r.metadata.subject.id) == ("Doe Jane", "MRN123")  # EDF+ "_" = space


def test_existing_session_is_never_replaced(tmp_path):
    src, out = tmp_path / "x.edf", str(tmp_path / "x.mefd")
    _write_edf(src)
    ex.convert(src, out, "UTC")
    with pytest.raises(FileExistsError):
        ex.convert(src, out, "UTC")
    shutil.rmtree(out)  # the test removes its own session; the example never does
    ex.verify(out, ex.convert(src, out, "UTC"))


def test_command_line_encrypts_without_passwords_on_the_command_line(tmp_path, monkeypatch, capsys):
    src, out = tmp_path / "e.edf", str(tmp_path / "e.mefd")
    _write_edf(src, patient=True)
    answers = iter(["one", "one", "two", "two"])  # each asked twice
    monkeypatch.setattr("getpass.getpass", lambda prompt="": next(answers))
    assert ex.main([str(src), out, "--timezone", "UTC", "--copy-patient", "--encrypt"]) == 0
    assert "verified" in capsys.readouterr().out
    with pytest.raises(mef3io.PasswordError):
        mef3io.Reader(out)
    with mef3io.Reader(out, "two") as r:
        assert r.metadata.subject.id == "MRN123"


def test_command_line_refuses_mistyped_passwords(tmp_path, monkeypatch):
    src, out = tmp_path / "e.edf", str(tmp_path / "e.mefd")
    _write_edf(src)
    answers = iter(["one", "onf"])
    monkeypatch.setattr("getpass.getpass", lambda prompt="": next(answers))
    with pytest.raises(SystemExit):
        ex.main([str(src), out, "--timezone", "UTC", "--encrypt"])
    assert not (tmp_path / "e.mefd").exists()


def test_bad_edf_path_creates_nothing(tmp_path):
    out = str(tmp_path / "x.mefd")
    with pytest.raises(OSError):
        ex.convert(tmp_path / "missing.edf", out, "UTC")
    assert not (tmp_path / "x.mefd").exists()


def test_copy_patient_warns_when_nothing_to_copy(tmp_path, capsys):
    src = tmp_path / "x.edf"
    _write_edf(src)  # no patient fields
    ex.convert(src, str(tmp_path / "x.mefd"), "UTC", copy_patient=True)
    assert "no patient name or code" in capsys.readouterr().out


def test_demo_runs_end_to_end(capsys):
    assert ex.main([]) == 0
    assert "verified" in capsys.readouterr().out


def _pyedflib_view(path):
    """Digital values and annotations as pyedflib itself reads them."""
    with pyedflib.EdfReader(str(path)) as f:
        digital = [f.readSignal(i, digital=True) for i in range(f.signals_in_file)]
        onset, dur, text = f.readAnnotations()
    return digital, sorted(zip(np.round(onset, 6), text))


@pytest.mark.parametrize("bdf", [False, True], ids=["edf", "bdf"])
def test_discontinuous_reader_agrees_with_pyedflib(tmp_path, bdf):
    # Independent check of open_discontinuous: same values and annotations as
    # pyedflib reads from the file before it is made discontinuous (with no
    # time shift, only the +D mark, which pyedflib refuses).
    src = tmp_path / ("a.bdf" if bdf else "a.edf")
    _write_edf(src, bdf=bdf)
    digital, anns = _pyedflib_view(src)
    _shift_records(src, 0, 0)
    with ex.open_edf(str(src)) as edf:
        assert edf["onsets"].tolist() == list(range(SECONDS))
        for sig, want in zip(edf["signals"], digital):
            assert np.array_equal(sig["read"](0, SECONDS), want)
            assert np.array_equal(sig["read"](2, 4), want[len(want) // SECONDS * 2:len(want) // SECONDS * 4])
        assert sorted((round(o, 6), t) for o, _, t in edf["annotations"]) == anns


def test_inverted_polarity_keeps_full_resolution(tmp_path):
    src, out = tmp_path / "i.edf", str(tmp_path / "i.mefd")
    _write_edf(src, eeg_range=(200, -200))  # physical_min > physical_max: allowed
    res = ex.convert(src, out, "UTC")
    assert res["channels"]["EEG Fp1_Ref"]["precision"] == 3  # EDF step ~0.0061
    ex.verify(out, res)


def test_precision_lowered_only_with_a_warning(tmp_path, capsys):
    src, out = tmp_path / "o.edf", str(tmp_path / "o.mefd")
    _write_edf(src, eeg_range=(500000, 500010))  # step 1.5e-4, values ~5e5
    res = ex.convert(src, out, "UTC")
    ch = res["channels"]["EEG Fp1_Ref"]
    assert ch["lowered"] and ch["precision"] == 3
    assert "warning: EEG Fp1_Ref" in capsys.readouterr().out
    ex.verify(out, res)
    ch["lowered"] = False  # without the warning, verify refuses the coarser copy
    with pytest.raises(AssertionError, match="too coarse"):
        ex.verify(out, res)


def test_long_channels_are_converted_in_chunks(tmp_path, monkeypatch):
    monkeypatch.setattr(ex, "CHUNK_SECONDS", 2)  # 6 s file -> 3 chunks per run
    src, out = tmp_path / "c.edf", str(tmp_path / "c.mefd")
    _write_edf(src)
    _shift_records(src, 3, 1.5)
    ex.verify(out, ex.convert(src, out, "UTC"))
    with mef3io.Reader(out) as r:
        assert r.info("ECG")["number_of_samples"] == 50 * SECONDS


def test_unfinished_and_truncated_files(tmp_path):
    src = tmp_path / "u.edf"
    _write_edf(src)
    _shift_records(src, 0, 0)
    b = bytearray(src.read_bytes())
    b[236:244] = b"-1      "  # "still being written": count from the file size
    src.write_bytes(bytes(b))
    with ex.open_edf(str(src)) as edf:
        assert len(edf["onsets"]) == SECONDS
    b[236:244] = b"9       "  # claims more records than the file holds
    src.write_bytes(bytes(b))
    with pytest.raises(ValueError, match="9 data records"):
        with ex.open_edf(str(src)):
            pass


@pytest.mark.parametrize("tz, hours", [("+1", 1), ("+01:00", 1), ("-05:30", -5.5), ("Z", 0), ("UTC", 0)])
def test_fixed_offsets(tz, hours):
    assert ex.parse_timezone(tz).utcoffset(None) == dt.timedelta(hours=hours)


def test_encrypted_session(tmp_path):
    src, out = tmp_path / "e.edf", str(tmp_path / "e.mefd")
    _write_edf(src)
    res = ex.convert(src, out, "UTC", password1="one", password2="two")
    ex.verify(out, res, password="two")
    with pytest.raises(mef3io.PasswordError):
        mef3io.Reader(out)


def test_discontinuous_record_without_time_stamp_fails_clearly(tmp_path):
    src = tmp_path / "b.edf"
    _write_edf(src)
    _shift_records(src, 0, 0)
    b = bytearray(src.read_bytes())
    ns, hb = int(b[252:256]), int(b[184:192])
    nsamp = [int(b[256 + ns * 216 + 8 * i:256 + ns * 216 + 8 * i + 8]) for i in range(ns)]
    off = hb + 2 * sum(nsamp) * 2 + sum(nsamp[:-1]) * 2  # record 2, annotation signal (last)
    b[off:off + nsamp[-1] * 2] = bytes(nsamp[-1] * 2)
    src.write_bytes(bytes(b))
    with pytest.raises(ValueError, match="no time stamp"):
        with ex.open_edf(str(src)):
            pass
