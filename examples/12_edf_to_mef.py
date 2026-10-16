"""Convert an EDF, EDF+ or BDF file to a MEF 3.0 session.

    pip install mef3io pyedflib         # or, from mef3io 1.3: pip install "mef3io[edf]"
    python 12_edf_to_mef.py recording.edf recording.mefd --timezone Europe/Prague
    python 12_edf_to_mef.py            # no arguments: converts a small demo file

What it does, per channel: keeps the channel's own sampling rate, stores the
samples as counts at least as fine as the EDF resolution, writes EDF+
annotations as records, turns the jumps in time of a discontinuous EDF+D file
into gaps (NaN on read), and finally reads the session back and checks it
against the EDF values. It works through the file a few minutes of one channel
at a time, so memory use does not grow with the length of the recording.

Two things are deliberately NOT automatic:

* The time zone. EDF stores the start as a local clock time with no zone;
  MEF 3.0 stores UTC. Guessing would shift every timestamp by hours, so you
  must pass --timezone (an IANA name such as Europe/Prague, or a fixed offset
  such as +01:00 or UTC).
* Patient identity. EDF headers hold the patient name and code in plain text.
  They are not copied unless you pass --copy-patient.

Library: pyedflib reads EDF, EDF+C, BDF and BDF+C. It refuses discontinuous
EDF+D / BDF+D files, so those are read by the short `open_discontinuous` below.
"""
from __future__ import annotations

import argparse
import contextlib
import datetime as dt
import math
import re
import sys
import tempfile
from pathlib import Path

import numpy as np

import mef3io

INT32_MAX = 2**31 - 1
CHUNK_SECONDS = 600  # convert (and check) at most this much of one channel at a time


# --- 1. time zone -------------------------------------------------------------
def parse_timezone(tz: str) -> dt.tzinfo:
    """'UTC' / 'Z', '+01:00' / '+1' / '-05:30', or an IANA name ('Europe/Prague')."""
    m = re.fullmatch(r"([+-])(\d{1,2})(?::?(\d{2}))?", tz)
    if m:
        sign = 1 if m[1] == "+" else -1
        return dt.timezone(sign * dt.timedelta(hours=int(m[2]), minutes=int(m[3] or 0)))
    if tz.upper() in ("UTC", "Z"):
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
# Both readers yield the same dict, valid while the `with` block is open:
#   start       local start time (naive datetime, sub-second included)
#   signals     [{label, fs, units, gain, offset, peak, read, transducer, prefilter}]
#               physical value = digital * gain + offset; peak = largest |physical|
#               read(k0, k1) = digital values of data records k0..k1-1
#   onsets      start of each data record, seconds after `start`
#   record_s    data record duration, seconds
#   annotations [(onset_s, duration_s or None, text)]
#   patient     {"name": ..., "code": ...}


def _signal(label, fs, units, pmin, pmax, dmin, dmax, read, transducer, prefilter):
    gain = (pmax - pmin) / (dmax - dmin)  # negative for inverted polarity
    return dict(label=label, fs=fs, units=units, gain=gain, offset=pmin - dmin * gain,
                peak=max(abs(pmin), abs(pmax)), read=read,
                transducer=transducer, prefilter=prefilter)


@contextlib.contextmanager
def open_with_pyedflib(path: str):
    import pyedflib

    with pyedflib.EdfReader(path) as f:
        # starttime_subsecond is in units of 100 ns. (getStartdatetime() scales
        # it wrongly in pyedflib 0.1.42, so it is applied here.)
        start = f.getStartdatetime().replace(microsecond=0)
        start += dt.timedelta(microseconds=f.starttime_subsecond // 10)
        record_s = f.datarecord_duration
        signals = []
        for i, h in enumerate(f.getSignalHeaders()):
            spr = f.samples_in_datarecord(i)
            read = (lambda k0, k1, i=i, spr=spr:
                    f.readSignal(i, k0 * spr, (k1 - k0) * spr, digital=True))
            signals.append(_signal(h["label"], f.getSampleFrequency(i), h["dimension"],
                                   h["physical_min"], h["physical_max"], h["digital_min"],
                                   h["digital_max"], read, h["transducer"], h["prefilter"]))
        yield dict(
            start=start, signals=signals, record_s=record_s,
            onsets=np.arange(f.datarecords_in_file) * record_s,
            annotations=[(float(o), float(d) if d > 0 else None, str(t))
                         for o, d, t in zip(*f.readAnnotations())],
            patient={"name": f.getPatientName(), "code": f.getPatientCode()},
        )


@contextlib.contextmanager
def open_discontinuous(path: str):
    """EDF+D / BDF+D: the fixed-layout header, then each data record's own time."""
    raw = np.memmap(path, np.uint8, mode="r")  # maps the file; read on access
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
    complete = (len(raw) - header_bytes) // rec_bytes
    if nrec == -1:  # allowed while a recording is still being written
        nrec = complete
    if complete < nrec:
        raise ValueError(f"{path}: header says {nrec} data records, file holds {complete}")
    records = raw[header_bytes:header_bytes + nrec * rec_bytes].reshape(nrec, rec_bytes)
    edges = np.concatenate([[0], np.cumsum(nsamp)]) * width

    def read(i, k0, k1):
        part = records[k0:k1, edges[i]:edges[i + 1]]
        if not bdf:
            return part.copy().view("<i2").ravel().astype(np.int32)
        b = part.reshape(-1, 3).astype(np.int32)  # 24-bit little-endian
        v = b[:, 0] | b[:, 1] << 8 | b[:, 2] << 16
        return np.where(v >= 2**23, v - 2**24, v)

    signals, onsets, annotations = [], None, []
    for i in range(ns):
        if labels[i] in ("EDF Annotations", "BDF Annotations"):
            times = []
            for rec in records[:, edges[i]:edges[i + 1]]:
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
        signals.append(_signal(labels[i], nsamp[i] / record_s, units[i], pmin[i], pmax[i],
                               dmin[i], dmax[i], lambda k0, k1, i=i: read(i, k0, k1),
                               transducer[i], prefilter[i]))

    d, t = field(168, 176), field(176, 184)
    day, month, yy = map(int, d.split("."))
    year = 1900 + yy if yy >= 85 else 2000 + yy
    m = re.search(r"Startdate \d\d-\w{3}-(\d{4})", field(88, 168))  # EDF+: 4-digit year
    year = int(m[1]) if m else year
    start = dt.datetime(year, month, day, *map(int, t.split(".")))
    # EDF+ patient field: "code sex birthdate name ...", "_" for spaces, "X" if unknown.
    pf = [v.replace("_", " ") if v != "X" else "" for v in field(8, 88).split(" ")] + [""] * 4
    yield dict(start=start, signals=signals, onsets=onsets, record_s=record_s,
               annotations=annotations, patient={"code": pf[0], "name": pf[3]})


def open_edf(path: str):
    with open(path, "rb") as fh:
        reserved = fh.read(236)[192:197]
    if reserved in (b"EDF+D", b"BDF+D"):
        return open_discontinuous(path)
    return open_with_pyedflib(path)


# --- 3. writing MEF 3.0 -------------------------------------------------------
def precision_for(sig: dict) -> tuple[int, bool]:
    """Decimal places so one stored count is no coarser than one EDF step.

    Returns (precision, lowered): lowered is True when the values would not
    fit the 32-bit counts at that precision and a coarser one was used.
    """
    if sig["gain"] == 0:
        return 0, False
    p = max(0, math.ceil(-math.log10(abs(sig["gain"]))))
    wanted = p
    while p > 0 and sig["peak"] * 10**p > INT32_MAX:
        p -= 1
    return p, p < wanted


def runs(onsets: np.ndarray, record_s: float):
    """Group data records into contiguous runs: (first record, end record)."""
    first = 0
    for k in range(1, len(onsets) + 1):
        if k == len(onsets) or abs(onsets[k] - onsets[k - 1] - record_s) > 1e-6:
            yield first, k
            first = k


def chunks(edf: dict):
    """(first record, end record) pieces of at most CHUNK_SECONDS within each run."""
    step = max(1, round(CHUNK_SECONDS / edf["record_s"]))
    for k0, k1 in runs(edf["onsets"], edf["record_s"]):
        for c in range(k0, k1, step):
            yield c, min(c + step, k1)


def channel_name(label: str, taken: set) -> str:
    """EDF labels become folder names: keep them readable and unique."""
    name = re.sub(r"[^A-Za-z0-9 ._+-]", "_", label).strip(" .") or "channel"
    base, n = name, 1
    while name.lower() in taken:
        n += 1
        name = f"{base}_{n}"
    taken.add(name.lower())
    return name


def records_for(annotations, start_uutc):
    """EDF+ annotations -> records: EDFA when there is a duration, else Note."""
    return [{"type": "EDFA" if dur else "Note", "text": text,
             "time": start_uutc + round(onset * 1e6),
             **({"duration": round(dur * 1e6)} if dur else {})}
            for onset, dur, text in annotations]


def convert(edf_path, mef_path, timezone=None, copy_patient=False, overwrite=False,
            password1="", password2="") -> dict:
    """Write the session; returns what verify() needs.

    Passwords: give both to encrypt the session (MEF 3.0 has no level-1-only
    files), and the level-2 one to verify(). See docs/encryption_model.md.
    """
    keys = dict(password1=password1, password2=password2)
    if not timezone:
        raise ValueError(
            "EDF stores local time without a time zone. Pass the zone the "
            "recording was made in, e.g. timezone='Europe/Prague' or '+01:00'."
        )
    tz = parse_timezone(timezone)
    if overwrite:
        mef3io.Writer(mef_path, overwrite=True, **keys).close()
    elif Path(mef_path).exists():
        raise FileExistsError(f"{mef_path} exists; pass overwrite=True to replace it")

    with open_edf(str(edf_path)) as edf:
        start_uutc = local_to_uutc(edf["start"], tz)
        subject = mef3io.Subject(
            gmt_offset=int(edf["start"].replace(tzinfo=tz).utcoffset().total_seconds()))
        if copy_patient:
            subject.name_1, subject.id = edf["patient"]["name"], edf["patient"]["code"]

        taken, channels = set(), {}
        for sig in edf["signals"]:
            name = channel_name(sig["label"], taken)
            p, lowered = precision_for(sig)
            if lowered:
                print(f"warning: {name}: values too large for full EDF resolution; "
                      f"stored to {10.0**-p:g} {sig['units']} (EDF step {abs(sig['gain']):.3g})")
            described = [sig["transducer"], sig["prefilter"] and f"prefilter: {sig['prefilter']}"]
            acq = mef3io.Acquisition(channel_description="; ".join(filter(None, described)))
            md = mef3io.Metadata(subject=subject, acquisition=acq)
            # One Writer per channel, because the units label is per Writer.
            with mef3io.Writer(mef_path, units=sig["units"], metadata=md, **keys) as w:
                for k0, k1 in chunks(edf):
                    physical = sig["read"](k0, k1) * sig["gain"] + sig["offset"]
                    if np.abs(physical).max(initial=0) * 10**p > INT32_MAX:
                        raise ValueError(f"{name}: values outside the EDF header's range")
                    t0 = start_uutc + round(edf["onsets"][k0] * 1e6)
                    w.write(name, physical, t0, sig["fs"], precision=p)
            channels[name] = dict(index=edf["signals"].index(sig), precision=p, lowered=lowered)

        annotations = edf["annotations"]
        with mef3io.Writer(mef_path, **keys) as w:  # EDF+ annotations -> session records
            w.write_annotations(records_for(annotations, start_uutc))
    return dict(edf_path=str(edf_path), start_uutc=start_uutc, channels=channels,
                n_annotations=len(annotations))


# --- 4. verify ----------------------------------------------------------------
def verify(mef_path, result, password="") -> None:
    """Re-read the EDF and the session, chunk by chunk, and compare.

    Checks: each stored count is no coarser than the EDF step (unless a warning
    said so), every sample within half a count, the right number of samples
    (so nothing extra fills a gap), and every annotation's time, type, text
    and duration.
    """
    start_uutc = result["start_uutc"]
    with open_edf(result["edf_path"]) as edf, mef3io.Reader(mef_path, password) as r:
        for name, ch in result["channels"].items():
            sig, p = edf["signals"][ch["index"]], ch["precision"]
            if not ch["lowered"]:
                assert 10.0**-p <= abs(sig["gain"]) or sig["gain"] == 0, f"{name}: too coarse"
            stored = 0
            for k0, k1 in chunks(edf):
                want = sig["read"](k0, k1) * sig["gain"] + sig["offset"]
                t0 = start_uutc + round(edf["onsets"][k0] * 1e6)
                got = r.read(name, t0, t0 + len(want) * 1e6 / sig["fs"])
                err = np.abs(got - want).max(initial=0)  # NaN (a misplaced gap) fails too
                assert err <= 0.5 * 10.0**-p + 1e-9 * max(1.0, sig["peak"]), \
                    f"{name}: max error {err}"
                stored += len(want)
            assert r.info(name)["number_of_samples"] == stored, f"{name}: sample count"
        key = lambda x: (x["time"], x["type"], x["text"], x.get("duration"))  # noqa: E731
        got = sorted(map(key, r.records()))
        assert got == sorted(map(key, records_for(edf["annotations"], start_uutc))), \
            "annotations differ"


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
    ap.add_argument("--timezone",
                    help="zone of the EDF start time: Europe/Prague, +01:00 (+HH:MM) or UTC")
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
    with mef3io.Reader(a.mefd) as r:
        for name, ch in result["channels"].items():
            info = r.info(name)
            print(f"{name}: {info['sampling_frequency']:g} Hz, {info['units_description']}, "
                  f"stored to {10.0**-ch['precision']:g} {info['units_description']}")
    print(f"{result['n_annotations']} annotation(s); verified {a.mefd}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
