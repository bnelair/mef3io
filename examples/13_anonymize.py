"""De-identified copies with anonymize_session.

Builds a small session with a fictional subject, an annotation and an ECG
channel, then:

1. asks ``mef3io.anonymize.check`` what would stop a copy: first with a copy
   name that contains the subject's name (refused, with a stable code), then
   with a neutral code name;
2. makes the copy, leaving the ECG channel out and moving every date so the
   recording starts on 1 January 2000 (dates in the copy then read as "days
   since the start of the study", with the time of day kept);
3. prints the copy's verification and its notes, and reads the copy back.

The original recording is never changed. Nothing here prints the original
times: they, together with the shift, are what would give the real dates
back. Python and C++ only (not MATLAB).
"""
import datetime as dt
import tempfile
from pathlib import Path

import numpy as np

import mef3io

FS = 256.0
START = 1_717_372_800_000_000 + 9 * 3600 * 1_000_000  # a fictional morning, 09:00 UTC

tmp = Path(tempfile.mkdtemp())
src = str(tmp / "roe_jane.mefd")

# ---------------------------------------------------------------- the source
meta = mef3io.Metadata(
    subject=mef3io.Subject(name_1="Jane", name_2="Roe", id="42",
                           recording_location="Example Hospital"),
    acquisition=mef3io.Acquisition(session_description="Jane Roe, overnight EEG"),
)
rng = np.random.default_rng(0)
with mef3io.Writer(src, overwrite=True, units="uV", metadata=meta) as w:
    # "LAH42" is a real electrode label; the 42 inside it overlaps the subject
    # ID's digits, so the verification lists a note (and keeps the name).
    for ch in ("Fp1", "LAH42", "ECG"):
        w.write(ch, rng.standard_normal(int(60 * FS)), START, fs=FS)
    w.write_annotations([{"time": START + 5_000_000, "text": "Jane Roe asked for a break"}],
                        channel="Fp1")
with mef3io.Reader(src) as r:
    print("source:", Path(src).name, "with channels", sorted(r.channels))

# The shift moves the recording so it starts on 2000-01-01, keeping the UTC
# time of day. Compute it once and use it for every recording of this subject
# to keep them in order. The original start is used here and never printed.
start = mef3io.inspect(src).start_time
shift = mef3io.anonymize.reference_shift(start, "2000-01-01")

# ------------------------------------------------------ 1. check before copying
bad = str(tmp / "Roe_copy.mefd")
rep = mef3io.anonymize.check(src, bad, shift, exclude_channels=["ECG"])
print("\n1. check with a copy named after the subject:")
for r in rep.refusals:
    print(f"   refused: {r.code}")  # match on the code, not the message
assert not rep.ok and "name-contains-identity" in rep.codes

dst = str(tmp / "S001.mefd")
rep = mef3io.anonymize.check(src, dst, shift, exclude_channels=["ECG"])
print("   check with the code name S001:", "ok" if rep.ok else rep.codes)
assert rep.ok, rep.summary()

# ---------------------------------------------------------------- 2. the copy
report = mef3io.anonymize_session(src, dst, shift, exclude_channels=["ECG"])
print("\n2. copy written:", Path(report.destination).name)
print("   channels kept:    ", ", ".join(report.channels_kept))
print("   channels left out:", ", ".join(report.channels_excluded))
print("   segments, blocks, records copied:", report.segments, report.blocks, report.records)
print("   record text fields changed:", report.record_text_fields_changed)
# report.summary() would also print the shift; keep it, like the original
# times in report.source_start_us, apart from the copy.

# ----------------------------------------------- 3. verification and the notes
v = report.verification  # anonymize_session ran verify() before publishing
# summary() lists the notes: overlaps worth a look before sharing, such as
# a short ID inside a kept channel name. They never repeat the value itself.
print("\n3.", v.summary())
print("   notes:", len(v.notes))
assert v.ok and v.notes

# The same check can be run again at any time, on any copy:
assert mef3io.anonymize.verify(src, dst, shift).ok

with mef3io.Reader(dst) as r:
    first = dt.datetime.fromtimestamp(r.info("Fp1")["start_time"] / 1e6, dt.timezone.utc)
    print("\ncopy starts:", first.isoformat())
    print("copy channels:", sorted(r.channels))
    print("copy subject:", r.metadata.subject)
    print("copy annotations:", [(rec["type"], rec.get("text", "")) for rec in r.records("Fp1")])
    assert "ECG" not in r.channels and first.date() == dt.date(2000, 1, 1)

# The source is unchanged.
with mef3io.Reader(src) as r:
    assert sorted(r.channels) == ["ECG", "Fp1", "LAH42"]
    assert r.metadata.subject.name_2 == "Roe"
print("\nthe source is unchanged; done")
