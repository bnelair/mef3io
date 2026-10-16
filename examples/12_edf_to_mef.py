"""Convert an EDF, EDF+ or BDF file to a MEF 3.0 session.

    pip install "mef3io[edf]"          # adds pyedflib
    python 12_edf_to_mef.py recording.edf recording.mefd --timezone Europe/Prague
    python 12_edf_to_mef.py            # no arguments: converts a small demo file

What it does, per channel: keeps the channel's own sampling rate, stores the
samples as MEF counts at least as fine as the EDF resolution, writes EDF+
annotations as MEF records, turns the gaps of a discontinuous EDF+D file into
MEF gaps (NaN on read), and finally reads everything back and compares it.

Two things are deliberately NOT automatic:

* The time zone. EDF stores the start as a local clock time with no zone; MEF
  stores UTC. Guessing would shift every timestamp by hours, so you must pass
  --timezone (an IANA name such as Europe/Prague, or a fixed offset such as
  +01:00 or UTC).
* Patient identity. EDF headers hold the patient name and code in plain text.
  They are not copied unless you pass --copy-patient.

Library: pyedflib reads EDF, EDF+C, BDF and BDF+C. It refuses discontinuous
EDF+D / BDF+D files, so those are read by the short `read_discontinuous` below.
"""
from __future__ import annotations

import argparse
import datetime as dt
import math
import re
import sys
import tempfile
from pathlib import Path

import numpy as np

import mef3io

INT32_MAX = 2**31 - 1


# --- 1. time zone -------------------------------------------------------------
def parse_timezone(tz: str) -> dt.tzinfo:
    """'UTC', '+01:00' / '-05:30', or an IANA name such as 'Europe/Prague'."""
    m = re.fullmatch(r"([+-])(\d{2}):?(\d{2})", tz)
    if m:
        sign = 1 if m[1] == "+" else -1
        return dt.timezone(sign * dt.timedelta(hours=int(m[2]), minutes=int(m[3])))
    if tz.upper() == "UTC":
        return dt.timezone.utc
    from zoneinfo import ZoneInfo  # needs the tzdata package on Windows

    return ZoneInfo(tz)


def local_to_uutc(local: dt.datetime, tz: dt.tzinfo) -> int:
    """Local clock time (no zone) -> microseconds since 1970-01-01 UTC."""
    a, b = local.replace(tzinfo=tz, fold=0), local.replace(tzinfo=tz, fold=1)
    if a.utcoffset() != b.utcoffset():  # e.g. 02:30 on a daylight-saving change day
        raise ValueError(
            f"{local} happens twice or never in {tz} (daylight-saving change); "
            "pass a fixed offset such as --timezone +01:00 instead"
        )
    delta = a.astimezone(dt.timezone.utc) - dt.datetime(1970, 1, 1, tzinfo=dt.timezone.utc)
    return delta // dt.timedelta(microseconds=1)


# --- 2. reading EDF -----------------------------------------------------------
# Both readers return the same dict:
#   start       local start time (naive datetime, sub-second included)
#   signals     [{label, fs, units, gain, offset, digital, transducer, prefilter}]
#               physical value = digital * gain + offset
#   onsets      start of each data record, seconds after `start`
#   record_s    data record duration, seconds
#   annotations [(onset_s, duration_s or None, text)]
#   patient     {"name": ..., "code": ...}


def _scaling(pmin, pmax, dmin, dmax):
    gain = (pmax - pmin) / (dmax - dmin)
    return gain, pmin - dmin * gain


def read_with_pyedflib(path: str) -> dict:
    import pyedflib

    with pyedflib.EdfReader(path) as f:
        # starttime_subsecond is in units of 100 ns. (getStartdatetime() scales
        # it wrongly in pyedflib 0.1.42, so it is applied here.)
        start = f.getStartdatetime().replace(microsecond=0)
        start += dt.timedelta(microseconds=f.starttime_subsecond // 10)
        record_s = f.datarecord_duration
        signals = []
        for i, h in enumerate(f.getSignalHeaders()):
            gain, offset = _scaling(h["physical_min"], h["physical_max"],
                                    h["digital_min"], h["digital_max"])
            signals.append(dict(
                label=h["label"], fs=f.getSampleFrequency(i), units=h["dimension"],
                gain=gain, offset=offset, digital=f.readSignal(i, digital=True),
                transducer=h["transducer"], prefilter=h["prefilter"],
            ))
        onsets = np.arange(f.datarecords_in_file) * record_s
        annotations = [(float(o), float(d) if d > 0 else None, str(t))
                       for o, d, t in zip(*f.readAnnotations())]
        patient = {"name": f.getPatientName(), "code": f.getPatientCode()}
    return dict(start=start, signals=signals, onsets=onsets, record_s=record_s,
                annotations=annotations, patient=patient)


def read_discontinuous(path: str) -> dict:
    """EDF+D / BDF+D: the fixed-layout header, then each data record's own time."""
    raw = np.memmap(path, np.uint8, mode="r")  # maps the file; nothing read yet
    bdf = raw[0] == 0xFF
    width = 3 if bdf else 2
    field = lambda a, b: bytes(raw[a:b]).decode("latin-1").strip()  # noqa: E731
    ns, nrec = int(field(252, 256)), int(field(236, 244))
    record_s, header_bytes = float(field(244, 252)), int(field(184, 192))

    def column(offset, size):  # one per-signal header field, for every signal
        base = 256 + offset * ns
        return [field(base + i * size, base + (i + 1) * size) for i in range(ns)]

    labels, transducer, units = column(0, 16), column(16, 80), column(96, 8)
    pmin, pmax, dmin, dmax = (list(map(float, column(o, 8))) for o in (104, 112, 120, 128))
    prefilter, nsamp = column(136, 80), list(map(int, column(216, 8)))

    rec_bytes = sum(nsamp) * width
    records = raw[header_bytes:header_bytes + nrec * rec_bytes].reshape(nrec, rec_bytes)
    edges = np.concatenate([[0], np.cumsum(nsamp)]) * width

    signals, onsets, annotations = [], None, []
    for i in range(ns):
        part = records[:, edges[i]:edges[i + 1]]
        if labels[i] in ("EDF Annotations", "BDF Annotations"):
            times = []
            for rec in part:
                tals = bytes(rec).split(b"\x00")
                for j, tal in enumerate(t for t in tals if t):
                    when, *texts = tal.decode("utf-8", "replace").split("\x14")
                    onset, _, dur = when.partition("\x15")
                    if j == 0 and onsets is None:
                        times.append(float(onset))  # the record's time-keeping stamp
                    for text in filter(None, texts):
                        annotations.append((float(onset), float(dur) if dur else None, text))
            onsets = np.array(times) if onsets is None else onsets
            continue  # (an EDF+D file always has an annotation signal)
        if bdf:  # 24-bit little-endian, sign-extended
            b = part.reshape(nrec, nsamp[i], 3).astype(np.int32)
            digital = (b[..., 0] | b[..., 1] << 8 | b[..., 2] << 16).ravel()
            digital = np.where(digital >= 2**23, digital - 2**24, digital)
        else:
            digital = part.copy().view("<i2").ravel()
        gain, offset = _scaling(pmin[i], pmax[i], dmin[i], dmax[i])
        signals.append(dict(label=labels[i], fs=nsamp[i] / record_s, units=units[i],
                            gain=gain, offset=offset, digital=digital,
                            transducer=transducer[i], prefilter=prefilter[i]))

    d, t = field(168, 176), field(176, 184)
    day, month, yy = map(int, d.split("."))
    year = 1900 + yy if yy >= 85 else 2000 + yy
    m = re.search(r"Startdate \d\d-\w{3}-(\d{4})", field(88, 168))  # EDF+: 4-digit year
    year = int(m[1]) if m else year
    start = dt.datetime(year, month, day, *map(int, t.split(".")))
    # EDF+ patient field: "code sex birthdate name ...", "_" for spaces, "X" if unknown.
    pf = [v.replace("_", " ") if v != "X" else "" for v in field(8, 88).split(" ")] + [""] * 4
    patient = {"code": pf[0], "name": pf[3]}
    return dict(start=start, signals=signals, onsets=onsets, record_s=record_s,
                annotations=annotations, patient=patient)


def read_edf(path: str) -> dict:
    with open(path, "rb") as fh:
        reserved = fh.read(236)[192:197]
    if reserved in (b"EDF+D", b"BDF+D"):
        return read_discontinuous(path)
    return read_with_pyedflib(path)


# --- 3. writing MEF 3.0 -------------------------------------------------------
def precision_for(sig: dict) -> int:
    """Decimal places so a MEF count is no coarser than one EDF step."""
    if sig["gain"] <= 0:
        return 0
    p = max(0, math.ceil(-math.log10(abs(sig["gain"]))))
    peak = np.abs(sig["digital"] * sig["gain"] + sig["offset"]).max(initial=0)
    while p > 0 and peak * 10**p > INT32_MAX:  # counts are int32
        p -= 1
    return p


def runs(onsets: np.ndarray, record_s: float):
    """Group data records into contiguous runs: (first record, end record)."""
    first = 0
    for k in range(1, len(onsets) + 1):
        if k == len(onsets) or abs(onsets[k] - onsets[k - 1] - record_s) > 1e-6:
            yield first, k
            first = k


def channel_name(label: str, taken: set) -> str:
    """EDF labels become folder names: keep them readable and unique."""
    name = re.sub(r"[^A-Za-z0-9 ._+-]", "_", label).strip(" .") or "channel"
    base, n = name, 1
    while name.lower() in taken:
        n += 1
        name = f"{base}_{n}"
    taken.add(name.lower())
    return name


def convert(edf_path, mef_path, timezone=None, copy_patient=False, overwrite=False) -> dict:
    if not timezone:
        raise ValueError(
            "EDF stores local time without a time zone. Pass the zone the "
            "recording was made in, e.g. timezone='Europe/Prague' or '+01:00'."
        )
    edf = read_edf(str(edf_path))
    tz = parse_timezone(timezone)
    start_uutc = local_to_uutc(edf["start"], tz)
    subject = mef3io.Subject(
        gmt_offset=int(edf["start"].replace(tzinfo=tz).utcoffset().total_seconds()))
    if copy_patient:
        subject.name_1, subject.id = edf["patient"]["name"], edf["patient"]["code"]

    if overwrite:
        mef3io.Writer(mef_path, overwrite=True).close()
    elif Path(mef_path).exists():
        raise FileExistsError(f"{mef_path} exists; pass overwrite=True to replace it")
    taken, channels = set(), {}
    for sig in edf["signals"]:
        name = channel_name(sig["label"], taken)
        p = precision_for(sig)
        spr = round(sig["fs"] * edf["record_s"])  # samples per data record
        described = [sig["transducer"], sig["prefilter"] and f"prefilter: {sig['prefilter']}"]
        acq = mef3io.Acquisition(channel_description="; ".join(filter(None, described)))
        md = mef3io.Metadata(subject=subject, acquisition=acq)
        # One Writer per channel, because the units label is per Writer.
        with mef3io.Writer(mef_path, units=sig["units"], metadata=md) as w:
            for k0, k1 in runs(edf["onsets"], edf["record_s"]):
                physical = sig["digital"][k0 * spr:k1 * spr] * sig["gain"] + sig["offset"]
                t0 = start_uutc + round(edf["onsets"][k0] * 1e6)
                w.write(name, physical, t0, sig["fs"], precision=p)
        channels[name] = (sig, p, spr)

    with mef3io.Writer(mef_path) as w:  # EDF+ annotations -> session records
        w.write_annotations([
            {"type": "EDFA" if dur else "Note", "text": text,
             "time": start_uutc + round(onset * 1e6),
             **({"duration": round(dur * 1e6)} if dur else {})}
            for onset, dur, text in edf["annotations"]
        ])
    return dict(edf=edf, start_uutc=start_uutc, channels=channels)


# --- 4. verify ----------------------------------------------------------------
def verify(mef_path, result) -> None:
    """Every sample within half a MEF count, gaps NaN, annotations present."""
    edf, start_uutc = result["edf"], result["start_uutc"]
    with mef3io.Reader(mef_path) as r:
        for name, (sig, p, spr) in result["channels"].items():
            y = r.read(name)
            ch_start = r.info(name)["start_time"]
            stored = 0
            for k0, k1 in runs(edf["onsets"], edf["record_s"]):
                want = sig["digital"][k0 * spr:k1 * spr] * sig["gain"] + sig["offset"]
                t0 = start_uutc + round(edf["onsets"][k0] * 1e6)
                i0 = round((t0 - ch_start) * sig["fs"] / 1e6)
                got = y[i0:i0 + len(want)]
                err = np.abs(got - want).max(initial=0)
                assert err <= 0.5 * 10.0**-p + 1e-9 * max(1.0, np.abs(want).max(initial=0)), \
                    f"{name}: max error {err}"
                stored += len(want)
            assert np.count_nonzero(~np.isnan(y)) == stored, f"{name}: gaps differ"
        assert len(r.records()) == len(edf["annotations"])


# --- demo ---------------------------------------------------------------------
def make_demo_edf(path: str) -> None:
    """A small EDF+ file: two channels at different rates, two annotations."""
    import pyedflib

    w = pyedflib.EdfWriter(path, 2, file_type=pyedflib.FILETYPE_EDFPLUS)
    w.setSignalHeaders([
        dict(label="EEG Fp1", dimension="uV", sample_frequency=256, physical_min=-3200,
             physical_max=3200, digital_min=-32768, digital_max=32767),
        dict(label="ECG", dimension="mV", sample_frequency=64, physical_min=-5,
             physical_max=5, digital_min=-32768, digital_max=32767),
    ])
    w.setStartdatetime(dt.datetime(2024, 3, 5, 10, 20, 30))
    w.writeAnnotation(1.5, 0.5, "artifact")
    w.writeAnnotation(3.0, -1, "eyes closed")
    t = np.arange(10 * 256) / 256
    w.writeSamples([100 * np.sin(2 * np.pi * 10 * t), np.cos(2 * np.pi * t[::4])])
    w.close()


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("edf", nargs="?", help="EDF, EDF+ or BDF file")
    ap.add_argument("mefd", nargs="?", help="output session, must end in .mefd")
    ap.add_argument("--timezone", help="zone of the EDF start time: Europe/Prague, +01:00, UTC")
    ap.add_argument("--copy-patient", action="store_true",
                    help="copy the EDF patient name and code (off by default)")
    ap.add_argument("--overwrite", action="store_true", help="replace an existing session")
    a = ap.parse_args(argv)
    if a.edf is None:  # demo
        tmp = Path(tempfile.mkdtemp())
        a.edf, a.mefd, a.timezone, a.overwrite = str(tmp / "demo.edf"), str(tmp / "demo.mefd"), "UTC", True
        make_demo_edf(a.edf)
    if a.mefd is None:
        ap.error("give the output session path (ending in .mefd)")
    result = convert(a.edf, a.mefd, a.timezone, a.copy_patient, a.overwrite)
    verify(a.mefd, result)
    for name, (sig, p, _) in result["channels"].items():
        print(f"{name}: {sig['fs']:g} Hz, {sig['units']}, stored to {10.0**-p:g} {sig['units']}")
    print(f"{len(result['edf']['annotations'])} annotation(s); verified {a.mefd}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
