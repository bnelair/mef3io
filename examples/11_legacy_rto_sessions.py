"""Legacy "anonymized" sessions: a recording-time offset (RTO) hides nothing.

meflib can "anonymize" a session by storing every time relative to a
recording-time offset (rto, the local midnight of the recording day). Readers
add the offset back, so the date seems to disappear from the times. But the
offset itself sits in section 3 of every .tmet, and section 3 is plaintext
unless the session is encrypted. mef3io therefore DEPRECATES this scheme:
it never creates such a session (see docs/anonymization.md).

It still reads them and extends them correctly. This script shows both:

  1. builds a small rto session with the legacy stack (mef_tools + pymef),
     with a FICTIONAL date and synthetic data;
  2. recovers the real date from the plaintext .tmet bytes with `struct`;
  3. reads it with mef3io and pymef: both return the true UTC times;
  4. appends data, adds a channel and adds an annotation with mef3io;
  5. checks both readers still agree, and that none of the new bytes holds
     a written time in plain or negated form (the session's own convention,
     rto - t, is kept);
  6. shows the refusals: a time at or before the offset, segments that
     disagree on the offset, and an offset locked behind a level-2 password.

Exits non-zero on any mismatch. Requires the legacy stack:
pip install mef3io[test]  (mef-tools + pymef).
"""
import contextlib
import datetime as dt
import io
import shutil
import struct
import sys
import tempfile
import warnings
from pathlib import Path

import numpy as np

import mef3io

try:
    import pandas as pd
    from mef_tools.io import MefWriter as LegacyWriter
    from pymef.mef_session import MefSession
except ImportError:
    sys.exit("This example needs the legacy stack: pip install mef3io[test]")

S = 1_000_000                                  # microseconds per second
FS = 256.0
# A FICTIONAL recording: 14 May 2031, 13:47:05 UTC, at a site 6 h behind UTC
# (the GMT offset mef_tools writes by default).
START = int(dt.datetime(2031, 5, 14, 13, 47, 5, tzinfo=dt.timezone.utc).timestamp()) * S
GMT_OFFSET = -6 * 3600                         # seconds, as stored in section 3

UH_BYTES = 1024
S3_ENCRYPTION = 1025                           # section 1, byte 1 (si1)
S3_RTO = 13312                                 # section 3, field 0 (si8)

failures = []


def check(ok, what):
    print(("  ok   " if ok else "  FAIL ") + what)
    if not ok:
        failures.append(what)


def utc(t_us):
    return dt.datetime.fromtimestamp(t_us / S, dt.timezone.utc).strftime("%Y-%m-%d %H:%M:%S UTC")


def meflib_rto(start_us, gmt_offset_s):
    """meflib's generate_recording_time_offset (meflib.c:4276): the local
    midnight of the recording day, in uUTC."""
    start_s = start_us // S
    rto = start_s - start_s % 86400 + 86400 - gmt_offset_s
    while rto > start_s:
        rto -= 86400
    return rto * S


@contextlib.contextmanager
def quiet():
    """The legacy writer prints progress; keep this script's output readable."""
    with contextlib.redirect_stdout(io.StringIO()), warnings.catch_warnings():
        warnings.simplefilter("ignore")
        yield


def legacy_session(path, offsets, password1=None, password2=None, note=True):
    """A session the legacy stack writes with `record_offset` (its name for
    the rto): one channel per (name, offset), 60 s of a ramp each."""
    with quiet():
        w = LegacyWriter(str(path), overwrite=True, password1=password1, password2=password2)
        for ch, rto in offsets:
            w.record_offset = rto
            w.write_data(np.arange(int(60 * FS), dtype=np.int32) * 1.0, ch, START, FS,
                         precision=0)
        if note:
            w.write_annotations(pd.DataFrame(
                [{"time": START + 5 * S, "type": "Note", "text": "event (fictional)"}]))
        del w


def all_bytes(path):
    return b"".join(f.read_bytes() for f in sorted(Path(path).rglob("*")) if f.is_file())


def tree(path):
    """Every file and folder, with each file's bytes: a refused write must
    leave this exactly as it was, not even an empty directory behind."""
    return sorted((str(f.relative_to(path)), f.read_bytes() if f.is_file() else None)
                  for f in Path(path).rglob("*"))


def pymef_view(path, channel, start, end):
    """What pymef sees: the channel's start time, its samples in [start, end),
    and the session-level record times."""
    with quiet():
        ms = MefSession(str(path), "")
        info = {i["name"]: i for i in ms.read_ts_channel_basic_info()}[channel]
        data = ms.read_ts_channels_uutc(channel, [start, end])
        recs = [(r["text"], int(r["time"])) for r in ms.read_records()]
        ms.close()
    return int(np.asarray(info["start_time"]).ravel()[0]), np.asarray(data), recs


root = Path(tempfile.mkdtemp())
session = root / "legacy_rto.mefd"
RTO = meflib_rto(START, GMT_OFFSET)
legacy_session(session, [("ch1", RTO)])
print(f"session: {session}")
print(f"fictional recording start {utc(START)}; meflib-style offset (local midnight) {utc(RTO)}")

# --- 2. the real date, straight from the bytes ------------------------------
print("\n2. Recover the date from the .tmet bytes (no library, no password)")
tmet = next(session.glob("ch1.timd/*.segd/*.tmet")).read_bytes()
s3_encryption = struct.unpack_from("<b", tmet, S3_ENCRYPTION)[0]
stored_start = struct.unpack_from("<q", tmet, 16)[0]       # universal header start time
rto = struct.unpack_from("<q", tmet, S3_RTO)[0]
real = -stored_start + rto                                  # meflib.c:7672
print(f"  section 3 encryption level: {s3_encryption} (<= 0: stored in plaintext)")
since_midnight = dt.timedelta(microseconds=-stored_start)
print(f"  stored start time: {stored_start} us  (minus {since_midnight}: the local time of day)")
print(f"  recording_time_offset:  {rto} us  ({utc(rto)})")
print(f"  -stored + rto = {real} us  ->  {utc(real)}")
check(s3_encryption <= 0, "section 3 is not encrypted")
check(rto == RTO and real == START, "the real start date comes back exactly")

# --- 3. both readers return true UTC ---------------------------------------
print("\n3. Read with mef3io and pymef")
end = START + 60 * S
# warn_declarations=False: the legacy writer leaves one section-2 size
# declaration unset (see docs/validation.md). Unrelated to the offset.
with mef3io.Reader(str(session), warn_declarations=False) as r:
    ours_start = r.info("ch1")["start_time"]
    ours_rto = r.info("ch1")["recording_time_offset"]
    ours_data = r.read("ch1", START, end)
    ours_recs = [(a["text"], a["time"]) for a in r.records()]
py_start, py_data, py_recs = pymef_view(session, "ch1", START, end)
print(f"  mef3io: start {utc(ours_start)}, record at {utc(ours_recs[0][1])}")
print(f"  pymef:  start {utc(py_start)}, record at {utc(py_recs[0][1])}")
check(ours_rto == RTO, "mef3io reports the session's offset")
check(ours_start == py_start == START, "both readers place the data at the true UTC start")
check(ours_recs == py_recs == [("event (fictional)", START + 5 * S)],
      "both readers place the record at the true UTC time")
check(np.array_equal(ours_data, py_data), "both readers return the same samples")

# --- 4. extend it with mef3io ----------------------------------------------
print("\n4. Append data, add a channel and an annotation with mef3io")
before = all_bytes(session)
append_at = end                                  # right after ch1's last sample
extra = np.arange(int(60 * FS), int(90 * FS)) * 1.0
new_note = START + 70 * S
with mef3io.Writer(str(session)) as w:
    w.write("ch1", extra, append_at, FS, precision=0)          # in-segment append
    w.write("ch2", extra, append_at, FS, precision=0)          # a new channel
    # write_annotations APPENDS: the stored "event" stays, this one is added.
    w.write_annotations([{"type": "Note", "time": new_note, "text": "added by mef3io"}])
new_end = append_at + 30 * S
written = {append_at, new_end, new_note}
print(f"  wrote ch1 [{utc(append_at)}, {utc(new_end)}), ch2 likewise, a note at {utc(new_note)}")

# --- 5. still consistent, and no date in plain -----------------------------
print("\n5. Check the extended session")
with mef3io.Reader(str(session), warn_declarations=False) as r:
    ours_ch1 = r.read("ch1", START, new_end)
    ours_ch2 = r.read("ch2", append_at, new_end)
    ours_recs = [(a["text"], a["time"]) for a in r.records()]
    rtos = [r.info(ch)["recording_time_offset"] for ch in ("ch1", "ch2")]
_, py_ch1, py_recs = pymef_view(session, "ch1", START, new_end)
py_ch2_start, py_ch2, _ = pymef_view(session, "ch2", append_at, new_end)
expected = np.arange(int(90 * FS)) * 1.0
check(np.array_equal(ours_ch1, expected) and np.array_equal(py_ch1, expected),
      "ch1 reads 90 s seamlessly in both readers")
check(py_ch2_start == append_at and np.array_equal(ours_ch2, extra)
      and np.array_equal(py_ch2, extra), "the new channel ch2 is placed right in both readers")
check(ours_recs == py_recs == [("event (fictional)", START + 5 * S), ("added by mef3io", new_note)],
      "both readers agree on both annotations")
check(rtos == [RTO, RTO], "the new channel inherited the session's offset")

ch2_tidx = next(session.glob("ch2.timd/*.segd/*.tidx")).read_bytes()
stored = struct.unpack_from("<q", ch2_tidx, UH_BYTES + 8)[0]
print(f"  ch2's first index entry stores {stored} us = rto - t "
      f"(rto - t = {RTO - append_at})")
check(stored == RTO - append_at, "new times are stored in the session's own form, rto - t")

blob = all_bytes(session)
# Every sample time and whole second of the recording, plain and negated,
# against the 8-byte value at EVERY byte position of every file (legacy ones
# included): a writer storing the date directly, in any field, would hit.
times = {START + round(i * S / FS) for i in range(int(90 * FS) + 1)}
times |= {START + k * S for k in range(91)} | written | {START + 5 * S}
windows = {blob[i:i + 8] for i in range(len(blob) - 7)}
leaks = sorted(t for t in times
               if struct.pack("<q", t) in windows or struct.pack("<q", -t) in windows)
print(f"  {len(times)} recording times x 2 signs vs every 8-byte value in the session:"
      f" {len(leaks)} hits")
check(not leaks, "no recording time appears in plain or negated form anywhere in the session")
check(len(blob) > len(before), "the session grew (sanity)")

# --- 6. refusals -----------------------------------------------------------
print("\n6. Refusals (each leaves the files untouched)")


def refused(path, attempt, error, open_kwargs=None):
    snapshot = tree(path)
    try:
        with warnings.catch_warnings():
            warnings.simplefilter("ignore")
            with mef3io.Writer(str(path), **(open_kwargs or {})) as w:
                attempt(w)
    except error as e:
        print(f"  {error.__name__}: {str(e).splitlines()[0][:110]}...")
        return tree(path) == snapshot
    return False


# (a) at or before the offset there is no hidden form: rto - t >= 0 would be
#     read as an absolute date, so the only "correct" bytes are the real date.
check(refused(session, lambda w: w.write_annotations(
    [{"type": "Note", "time": RTO - 3600 * S, "text": "too early"}]), mef3io.WriteConflictError),
    "a time at or before the offset is refused")

# (b) segments that disagree on the offset: no single offset to store against.
disputed = root / "disputed.mefd"
legacy_session(disputed, [("ch1", RTO), ("ch2", RTO - 86400 * S)], note=False)
check(refused(disputed, lambda w: w.write_annotations(
    [{"type": "Note", "time": new_note, "text": "x"}]), mef3io.WriteConflictError),
    "a session-level note is refused when the channels disagree on the offset")

# (c) offset locked: section 3 is level-2 encrypted and only level 1 was given.
locked = root / "locked.mefd"
legacy_session(locked, [("ch1", RTO)], password1="l1", password2="l2", note=False)
s3_enc = struct.unpack_from("<b", next(locked.glob("ch1.timd/*.segd/*.tmet")).read_bytes(),
                            S3_ENCRYPTION)[0]
check(s3_enc > 0, f"the encrypted session's section 3 is encrypted (level {s3_enc})")
check(refused(locked, lambda w: w.write("ch1", extra, append_at, FS, precision=0),
              mef3io.PasswordError, {"password1": "l1"}),
      "an append with only the level-1 password is refused (offset unknown)")

shutil.rmtree(root, ignore_errors=True)
print()
if failures:
    print(f"{len(failures)} check(s) FAILED")
    sys.exit(1)
print("all checks passed")
