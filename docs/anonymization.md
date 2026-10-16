---
description: "Why meflib's recording-time offset does not de-identify a MEF 3.0 recording, and what mef3io does instead."
---

# De-identification and the recording-time offset

!!! warning "meflib's recording-time-offset “anonymization” is deprecated in mef3io"
    A recording-time offset (RTO) does **not** remove the recording date from
    a MEF 3.0 session. The date stays in every metadata file, in plain text
    unless the session is encrypted. mef3io never creates such a session. It
    still reads existing ones and extends them consistently. Real
    de-identification will come as a separate de-identified copy, which is
    **planned, not yet available** (`anonymize_session`, [#70](https://github.com/bnelair/mef3io/issues/70)).

## In plain words

*For clinicians, students and anyone sharing recordings. The technical
details follow below.*

**What the offset is.** meflib, the C library behind MEF 3.0, can save a
recording's times as "time since midnight on the day of the recording"
instead of as calendar times. The midnight it counted from is the *offset*.
The idea was that someone looking at the times would see "07:47:05" rather
than "14 May 2031, 07:47:05".

**Why that does not hide the date.** The offset itself, the midnight of the
recording day, is saved inside the same files, in a place every MEF reader
looks. Reading software adds it back automatically, so most programs show
the real date anyway. Someone without the software can still read it with
a few lines of code, or with a hex editor. It is like covering the date on
a letter with a sticky note that has the date written on it.

The offset only helps in one situation: the session is encrypted **and**
the person has only the lower-level (level-1) password. Then the offset takes
the date out of the times, and the encryption hides the offset. Without the
encryption, the offset hides nothing. Even with it, other parts of the
session can still give the date away (see
[What else the offset leaves in place](#what-else-the-offset-leaves-in-place)).

MEF encryption itself is no substitute for de-identification either: it
hides the subject fields and annotation text, but the signal and its timing
stay readable without any password (in an offset session, as times of day).
See
[What encryption protects](encryption_model.md#what-encryption-protects).

**What this means for you.**

- Treat a session "anonymized" this way as **identifiable** data: it holds
  the recording date, and may hold names too.
- mef3io will never make new sessions like this.
- mef3io still opens existing ones, shows their true dates and times, and
  can add data or notes to them without breaking them.
- For sharing, mef3io will offer a proper de-identified **copy**, where all
  dates are moved and names removed. That feature is planned; see
  [What mef3io will provide instead](#what-mef3io-will-provide-instead).

## What the offset is

meflib computes the offset in `generate_recording_time_offset`
(meflib.c:4276-4324). It takes the UTC midnight of the recording's UTC date,
adds one day minus the site's GMT offset, and steps back a day at a time
until the result is at or before the recording start:

```
rto = UTC_midnight(start) + 86400 s − GMT_offset;  while rto > start: rto −= 86400 s
```

The result is the **local midnight** of the recording day. meflib copies it
into section 3 of every metadata file it writes (`initialize_metadata`,
meflib.c:4597). Every stored time then holds `rto − t`, a negative number
(`apply_recording_time_offset`, meflib.c:744). Readers recover the true time
as `−stored + rto` (`remove_recording_time_offset`, meflib.c:7660). So:

- **The time of day is kept.** It is the stored value, counted from local
  midnight.
- **The date is moved into `rto`**, one 8-byte field.

| Where | Field | Offset in the `.tmet` | Encrypted? |
|---|---|---|---|
| Section 1 | section-3 encryption level (si1) | 1025 | never; `−2` or `0` means section 3 is plain text, `+2` means encrypted |
| Section 3 | `recording_time_offset` (si8, µs) | 13312 (section 3, field 0) | **only** if the session has a level-2 password |
| Section 3 | `GMT_offset` (si4, seconds) | 13336 | same as above |

The offsets are meflib's (`METADATA_SECTION_3_ENCRYPTION_OFFSET`,
`METADATA_RECORDING_TIME_OFFSET_OFFSET`, `METADATA_GMT_OFFSET_OFFSET`,
meflib.h:404, 496 and 502) and mef3io's (`fmt::METADATA_SECTION_3_OFFSET` =
13312 in `types.hpp`; layout in the [format reference](mef3_format.md#metadata-file-tmet-exactly-16-384-b)).

Section 3 is encrypted only when the session has a level-2 password
(`encrypt_metadata`, meflib.c:2198, encrypts a section only when the writer
holds that level's key). In an unencrypted session it is plain text. In an
encrypted session, anyone given the level-2 password can read it.

## Worked example: getting the date back

A small session written by the legacy Python stack (`mef_tools` + `pymef`,
meflib's own write path), with synthetic data and a **fictional** date:
14 May 2031, 13:47:05 UTC, at a site six hours behind UTC. The offset is
computed exactly as meflib does. [Example 11](#backward-compatibility) builds
it.

| Value | Where it is stored | Stored bytes, as si8 µs | Meaning |
|---|---|---|---|
| start time | universal header, offset 16 | `-28025000000` | minus 7 h 47 min 05 s: the **local time of day** |
| `recording_time_offset` | section 3, `.tmet` offset 13312 | `1936504800000000` | 2031-05-14 06:00:00 UTC = **local midnight** |
| section-3 level | section 1, `.tmet` offset 1025 | `-2` | **plain text** |

The arithmetic: `−(−28 025 000 000) + 1 936 504 800 000 000 =
1 936 532 825 000 000` µs, which is **2031-05-14 13:47:05 UTC**, the real start.

No MEF library and no password is needed:

```python
import datetime as dt
import struct
from pathlib import Path

tmet = next(Path("legacy_rto.mefd").glob("*.timd/*.segd/*.tmet")).read_bytes()
s3_level = struct.unpack_from("<b", tmet, 1025)[0]    # section 3 encryption level
stored = struct.unpack_from("<q", tmet, 16)[0]        # start time, universal header
rto = struct.unpack_from("<q", tmet, 13312)[0]        # recording_time_offset, section 3

print("section 3 level:", s3_level)
print("stored start:   ", stored)
print("offset (rto):   ", rto)
print("real start:     ", dt.datetime.fromtimestamp((-stored + rto) / 1e6, dt.timezone.utc))
```

Output:

```
section 3 level: -2
stored start:    -28025000000
offset (rto):    1936504800000000
real start:      2031-05-14 13:47:05+00:00
```

Every `.tmet` in a meflib session carries the same offset (meflib uses one
per session), so any one of them is normally enough. The other stored
times (block index, data block headers, records) come back the same way.

## What else the offset leaves in place

Even if section 3 could not be read, the offset only changes **times**, and
not all of them:

| What | Where | Touched by the offset? |
|---|---|---|
| The date itself | `recording_time_offset`, section 3 of every `.tmet` | it *is* the offset: plain text unless encrypted |
| Time zone | `GMT_offset` (and the DST start/end fields when filled), section 3 | no |
| Time of day | every stored time | kept on purpose |
| Seizure, epoch and cursor times | record **bodies**: `Seiz` earliest onset / latest offset / per-channel onset and offset, `Epoc` timestamp / end, `Curs` trace timestamp | **no**: meflib applies the offset to the record **header** time only (meflib.c:1846, 2281), and `mefrec.c` never touches a body. Body times stay as the writing program stored them, normally the real absolute time |
| Session name | `session_name`, in the universal header of **every** file (offset 308) | no |
| Channel names, anonymized name | universal header (offsets 52, 564) | no |
| File and folder names | `<session>.mefd`, `<channel>.timd`, … | no |
| Subject name, ID, recording location | section 3 | no |
| Descriptions | section 2 (`session_description`, `channel_description`) | no |
| Annotation text | record bodies (Note, SyLg, EDFA …) | no |
| File modification times | the file system, outside the format | no |

Record bodies are plain text in an unencrypted session. In an encrypted one
they are encrypted only if the writer did so. mef3io encrypts them at level
2; the legacy Python stack writes them unencrypted (see
[example 09](examples.md)). So a seizure record can give away the date even
in a session whose header times all count from midnight.

### The sign convention is not reliable either

`rto − t` is negative, and so marked as "offset applied", only for times
after the offset. meflib's own offset is at or before the recording start,
so its times are normally after it. The legacy Python stack lets
the caller choose any offset (`MefWriter.record_offset`) and does not check
it. For a time **before** the offset, `rto − t` is positive. Every reader
then takes it as an absolute time, close to 1 January 1970. We wrote such a
session with `mef_tools` 1.2.3 / `pymef` 1.4.8 and the offset one hour after
the start. Its index then held `+3600000000`, which every reader takes as
1970-01-01 01:00 UTC. The same session's universal headers (`.tmet`, `.tidx`,
`.tdat`) and data block header held the **real start time, unconverted**.

A correct absolute time is also positive, so the bytes alone cannot tell the
two apart. That is why mef3io's [validator](validation.md) stands its time
checks down whenever `rto ≠ 0`. It is also why the planned
`anonymize_session` will refuse a session with an ambiguous legacy time
convention.

## Deprecated in mef3io

This is a decision about how mef3io **uses** the format, not a change to the
format. MEF 3.0 still defines the field, and mef3io still reads it exactly
as meflib does.

- **mef3io never creates an offset session.** A new session gets
  `recording_time_offset = 0` (`SegmentSpec::recording_time_offset = 0`,
  `writer.hpp`). No public API sets anything else. A new segment or channel
  takes an offset only from the existing session it is added to
  (`SessionWriter::resolve_offset`, `session_writer.cpp`), and an empty
  session resolves to 0.
- **mef3io reads offset sessions correctly.** Every time comes back as true
  UTC, the same values pymef returns.
- **mef3io extends them consistently.** Appended data, new segments, new
  channels and annotations are stored in that session's own form, `rto − t`
  (`to_stored_time`, `time.hpp`). The session stays consistent for every
  reader, and mef3io never writes the date there in plain. It also does not
  make the session any less identifiable than it already was.
- **mef3io refuses when it cannot do that safely**, and writes nothing:
  - a time at or before the offset (`WriteConflictError`); the only correct
    bytes for it would be the real date in plain;
  - segments that disagree on the offset (`WriteConflictError`);
  - an offset it cannot read: an encrypted session opened with only the
    level-1 password (`PasswordError`), or damaged metadata where the
    offset matters (`WriteConflictError`).
  The full rules are in the [format reference](mef3_format.md#times).
- **New de-identification goes through `anonymize_session`** (planned,
  [#70](https://github.com/bnelair/mef3io/issues/70)). It produces a separate
  copy with `recording_time_offset = 0`.

## What mef3io will provide instead

!!! note "Planned, not yet implemented"
    This section summarises [issue #70](https://github.com/bnelair/mef3io/issues/70).
    None of it is available yet, and the details may change before it ships.

```python
mef3io.anonymize_session(src, dst, shift_us, *, pseudonym=None,
    clear=("subject", "location", "descriptions"), text="drop" | "keep" | callable,
    rename=True, password="", output_password=None,
    progress=None, cancel=None) -> AnonymizeReport
mef3io.anonymize.verify(src, dst, shift_us) -> VerifyReport
python -m mef3io anonymize SRC DST (--shift-us N | --reference-date ISO --tz IANA_ZONE) [...]
```

- **A copy, never an edit.** The original session is never modified. The
  copy is written to `dst.partial` and renamed only after verification
  passes.
- **Every time moved by one constant shift**, each in the sign convention it
  came in: universal-header start/end in every file, `.tidx` entries, RED
  block headers (with the block CRC recomputed), record headers and `.ridx`
  entries at all three levels, and record **body** times (`Seiz`, `Epoc`,
  `Curs`). The shift is a whole number of days, plus, when the local time of
  day is to be kept, the reference recording's UTC offset. Options keep the
  weekday, and a batch of recordings can share one shift.
- **Time-zone fields set, not shifted:** `recording_time_offset = 0`,
  `GMT_offset = 0`, DST start/end = "no entry". The copy shows its times
  directly in UTC and carries no location hint.
- **Identity removed:** subject names, ID and location; `session_name` in
  every universal header and in `session_description`; `anonymized_name` set
  to the pseudonym; the level, file and provenance UUIDs regenerated; section
  2 descriptions; record free text (drop, keep, or a caller's function);
  directory names; foreign discretionary and protected regions; and file
  modification times (fresh files).
- **Samples untouched:** nothing is decoded or re-encoded, so the samples are
  bit-identical.
- **Encrypted input** needs the level-2 password. The output is unencrypted,
  or re-encrypted with new passwords. `.mefd.tar` archives work as input.
- **Refusals:** an existing `dst`, a `dst` inside `src`, video channels, an
  ambiguous legacy time convention, a shift that would take any time to 0 or
  below, or a shift whose magnitude is smaller than the session's time span
  plus one day.
- **Verification:** the validator passes the copy cleanly; every RED payload
  is byte-identical to the source; every time differs by exactly the shift
  (the time-zone fields must hold their set values instead); records match in
  count and order. A field-wise leak scan checks every time field and record
  body against the original time range, plain or negated; data-derived bytes
  (the RED payload and each block's statistics table) are checked
  byte-identical instead of scanned.
- **Provenance and progress:** the copy is stamped with a new operation code
  (6, `anonymize`), and long runs report progress and can be cancelled
  ([#69](https://github.com/bnelair/mef3io/issues/69)).

## Backward compatibility

[`examples/11_legacy_rto_sessions.py`](https://github.com/bnelair/mef3io/blob/main/examples/11_legacy_rto_sessions.py)
demonstrates every claim on this page, and exits non-zero if any check
fails. The test suite runs it (`tests/test_p24_examples.py`). It needs the
legacy stack (`pip install mef3io[test]`). It:

1. builds the session above with `mef_tools`, using meflib's offset rule;
2. recovers the real date from the plain `.tmet` bytes;
3. reads it with mef3io and with pymef: both return the true UTC start,
   annotation time and samples;
4. with mef3io, appends 30 s to `ch1`, adds a channel `ch2`, and adds an
   annotation;
5. checks that both readers still agree, that the new channel took the
   session's offset, that a new index entry holds `rto − t`, and that **no
   8-byte value at any byte position in the session** equals any sample time
   or whole second of the recording, plain or negated (legacy files
   included);
6. shows the three refusals (time before the offset, disputed offset,
   offset locked behind level 2), each leaving every file and folder exactly
   as it was.

Its output (the temporary path differs per run):

```
session: /tmp/.../legacy_rto.mefd
fictional recording start 2031-05-14 13:47:05 UTC; meflib-style offset (local midnight) 2031-05-14 06:00:00 UTC

2. Recover the date from the .tmet bytes (no library, no password)
  section 3 encryption level: -2 (<= 0: stored in plaintext)
  stored start time: -28025000000 us  (minus 7:47:05: the local time of day)
  recording_time_offset:  1936504800000000 us  (2031-05-14 06:00:00 UTC)
  -stored + rto = 1936532825000000 us  ->  2031-05-14 13:47:05 UTC
  ok   section 3 is not encrypted
  ok   the real start date comes back exactly

3. Read with mef3io and pymef
  mef3io: start 2031-05-14 13:47:05 UTC, record at 2031-05-14 13:47:10 UTC
  pymef:  start 2031-05-14 13:47:05 UTC, record at 2031-05-14 13:47:10 UTC
  ok   mef3io reports the session's offset
  ok   both readers place the data at the true UTC start
  ok   both readers place the record at the true UTC time
  ok   both readers return the same samples

4. Append data, add a channel and an annotation with mef3io
  wrote ch1 [2031-05-14 13:48:05 UTC, 2031-05-14 13:48:35 UTC), ch2 likewise, a note at 2031-05-14 13:48:15 UTC

5. Check the extended session
  ok   ch1 reads 90 s seamlessly in both readers
  ok   the new channel ch2 is placed right in both readers
  ok   both readers agree on both annotations
  ok   the new channel inherited the session's offset
  ch2's first index entry stores -28085000000 us = rto - t (rto - t = -28085000000)
  ok   new times are stored in the session's own form, rto - t
  23041 recording times x 2 signs vs every 8-byte value in the session: 0 hits
  ok   no recording time appears in plain or negated form anywhere in the session
  ok   the session grew (sanity)

6. Refusals (each leaves the files untouched)
  WriteConflictError: cannot write record time 1936501200000000 (uUTC): it is at or before this session's anonymization offset (reco...
  ok   a time at or before the offset is refused
  WriteConflictError: cannot write session-level records: the segments it belongs to disagree on the recording-time offset (19365048...
  ok   a session-level note is refused when the channels disagree on the offset
  ok   the encrypted session's section 3 is encrypted (level 2)
  PasswordError: writing to channel ch1 needs the full-access (level-2) password: section 3 of /tmp/.../locked.mefd/ch1...
  ok   an append with only the level-1 password is refused (offset unknown)

all checks passed
```

The same behaviour is pinned in more detail by `tests/test_p22_records.py`.

## If you have RTO-"anonymized" data today

- **Treat it as identifiable.** It holds the recording date, and the offset
  alone removes no names. Store and share it under the rules for identifiable
  data.
- **If it must stay in this form, restrict access to it.** Keep it on
  encrypted storage, or behind access controls meant for identifiable data.
  mef3io has no tool that adds MEF encryption to an existing session, and do
  not rewrite such a session into a new one to encrypt it: a fresh mef3io
  write uses offset 0, so it would store the dates directly. If a session
  already has a level-2 password, give out only the level-1 password to
  people who need the signal but not the date. Record bodies written
  unencrypted, file and folder names, and the session name stay readable
  whatever the password, and so do the samples and their times of day
  ([What encryption protects](encryption_model.md#what-encryption-protects)).
- **To share it de-identified, re-share it through `anonymize_session` once
  that ships** ([#70](https://github.com/bnelair/mef3io/issues/70)). It will
  produce a copy with every date moved, the offset set to 0 and identity
  removed. Until then, do not treat a recording-time offset as
  de-identification.
