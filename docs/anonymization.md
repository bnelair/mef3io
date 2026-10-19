---
description: "How to make a de-identified copy of a MEF 3.0 session with anonymize_session, and why meflib's recording-time offset does not de-identify a recording."
---

# De-identification and the recording-time offset

This page has two parts:

1. **[De-identified copies](#de-identified-copies-with-anonymize_session)**:
   how to make a copy of a recording that you can share, with every date
   moved and the patient's identity removed, using `anonymize_session`
   (Python and C++; not MATLAB). A runnable example is
   [`examples/13_anonymize.py`](https://github.com/bnelair/mef3io/blob/main/examples/13_anonymize.py).
2. **[The recording-time offset](#the-recording-time-offset-deprecated)**:
   why meflib's older way of "anonymizing" a session does not hide the
   recording date, and how mef3io handles sessions that use it.

!!! warning "meflib's recording-time-offset “anonymization” is deprecated in mef3io"
    A recording-time offset (RTO) does **not** remove the recording date from
    a MEF 3.0 session. The date stays in every metadata file, in plain text
    unless the session is encrypted. mef3io never creates such a session. It
    still reads existing ones and extends them consistently. To de-identify a
    session, make a de-identified copy with `anonymize_session`; see
    [De-identified copies](#de-identified-copies-with-anonymize_session).

## De-identified copies with `anonymize_session`

*Python and C++. Not available in MATLAB.*

### In plain words

`anonymize_session` makes a **copy** of a recording that you can share. In
the copy:

- **every date and time is moved** by the same amount, so the copy no longer
  says when the recording was made. Times keep their order and spacing, so
  seizures, sleep stages and gaps still line up with the signal;
- **the patient's details are removed**: name, ID, recording location, the
  session's name, the free-text descriptions and, by default, the text of
  annotations;
- **the signal itself is copied unchanged**, sample for sample.

Your original recording is never changed. The copy is checked before it is
saved under its final name; if anything is wrong, or you cancel, nothing is
left behind.

If you anonymize several recordings of one patient with the **same** shift,
they stay in the right order and the right distance apart.

### What it costs

The copy itself reads the source once and writes the copy once; data blocks
are streamed, never held whole. The check that follows reads the source's
data and the copy again, so in total the source is read twice and the copy
once.

### Quick start

```python
import mef3io

# Move the recording so it starts on 3 January 2000, keeping the local time of day.
start = mef3io.inspect("patient.mefd").start_time            # µs since 1970, UTC
shift = mef3io.anonymize.reference_shift(start, "2000-01-03", tz="Europe/Prague")

report = mef3io.anonymize_session("patient.mefd", "S001.mefd", shift)
print(report.summary())     # includes the shift: keep the output with the original
```

From a terminal:

```
python -m mef3io anonymize patient.mefd S001.mefd --reference-date 2000-01-03 --tz Europe/Prague
python -m mef3io anonymize patient.mefd S001.mefd --shift-us -630720000000000
```

The copy's folder name (`S001` above) becomes its session name everywhere.
Choose a code that says nothing about the patient: a copy name or
`pseudonym` that contains the patient's names (or any of their words), ID or
location, or the original session name, is refused before anything is
written. Values of 3 or more characters count anywhere ("Roe1", "XRoe"),
shorter ones as a whole word; case and accents are ignored (see
[What counts as a name](#how-the-copy-is-checked)).

### Step by step

A typical workflow, which
[`examples/13_anonymize.py`](https://github.com/bnelair/mef3io/blob/main/examples/13_anonymize.py)
runs end to end on a small made-up session:

1. **Choose the shift** once per patient (see below) and keep it, with the
   original recording, away from the copy: together with the copy it gives
   the real dates back.
2. **Check first.** `mef3io.anonymize.check(src, dst, shift, ...)` takes the
   same arguments as the copy and lists everything that would stop it, each
   with a stable code, without writing anything
   ([Checking before copying](#checking-before-copying)).
3. **Make the copy** with `anonymize_session`, leaving out channels you do
   not want to share (`exclude_channels=`) and choosing whether the copy is
   encrypted (`output_password=`).
4. **Read the verification.** `report.verification` says whether the copy
   passed and lists **notes**: overlaps worth a look, such as a short ID
   that also appears inside a longer number. Look at them before sharing
   ([How the copy is checked](#how-the-copy-is-checked)).

### Choosing the shift

The shift is a number of microseconds added to every time. Pick it from the
date you want the recording to start on:

- **Keep the UTC time of day:** `reference_shift(start, new_date)`. The shift
  is a whole number of days.
- **Keep the local time of day:** `reference_shift(start, new_date, tz=...)`
  with the site's time zone (an IANA name such as `America/Chicago`). The
  copy's UTC clock then shows the original *local* time of day. The copy
  stores no time zone, so read its times as plain clock times.
- **Keep the weekday:** add `keep_weekday=True`; the date moves to the
  nearest one with the same weekday.
- **Several recordings of one patient:** compute the shift once, from one
  reference recording, and use it for all of them. A recording made in the
  other daylight-saving season then shows its local time one hour off.

Two rules protect the copy:

- the shift must be at least the recording's length **plus one day** (in
  either direction), so that no moved time can land inside the original
  dates, which is what lets the check below recognise a leaked date;
- every moved time must stay after 1 January 1970.

### What changes, in detail

| | What happens |
|---|---|
| Times | Moved by the shift, each kept in the form it was stored in: universal-header start and end in every file, the block index, every data block's header (its checksum is recomputed), annotation headers and the annotation index at session, channel and segment level, and the times inside `Seiz` (onset, offset, per-channel onset and offset), `Epoc` (start, end) and `Curs` (trace time) annotations. |
| Time-zone fields | Set, not moved: `recording_time_offset = 0`, `GMT_offset = 0`, daylight-saving start and end = "no entry". |
| Subject | `subject_name_1`, `subject_name_2`, `subject_id` and `recording_location` cleared (`clear=`). |
| Session name | Replaced by the copy's name in every file header, in the names of the session-level annotation files, and in `session_description`. `anonymized_name` is set to `pseudonym` (default: the copy's name). |
| Descriptions | `session_description`, `channel_description` and `reference_description` cleared. With `clear=("subject", "location")` they are kept, with the old session name replaced and the removed names taken out. |
| Annotation text | `text="drop"` (default) clears every free-text field; the annotations, their types and their (moved) times stay. `text="keep"` keeps the text and takes out only the names mef3io knows: the old session name, the subject's names, ID and location, and excluded channels (short names and numbers only as whole words or numbers; kept channel names are never changed). **It does not find dates, ages, other people's names or anything else written in free text**; read the text yourself, or use `"drop"`. A function `text(value, info)` decides itself; the known names are still taken out afterwards. Text that is not valid UTF-8 reaches it with each bad byte as a surrogate (`errors="surrogateescape"`) and is written back byte for byte. An annotation of a type or version mef3io does not know keeps its type and time, but its contents are not copied, since they may hold text or dates nothing can find. |
| Seizure channel lists | An entry naming a kept channel exactly stays. An entry naming an excluded channel (compared without case or surrounding spaces) is removed. Any other entry name is free text: cleared with `text="drop"`, names taken out otherwise. |
| Identifiers | Every file identifier (level, file and provenance UUID) is new, so nothing links the copy back to the original. |
| Other programs' data | The private areas other programs may fill in are cleared: the protected and discretionary regions of every file header, of metadata sections 1–3, of every block-index entry and of every data block's header. Every text field is rewritten whole, so nothing hidden after a field's end survives. |
| Files | Every file is written new, so file dates are those of the copy. Segment folders and files are named as mef3io names them (`<channel>-000000.segd`), whatever the source called them. Files that are not part of the MEF layout are not copied (listed in `report.not_copied`). |
| Samples | Not decoded, not re-encoded: byte-identical. |
| Provenance | Every file is stamped with operation 6, `anonymize`. |

Channel names are **kept**: they name electrodes, and studies need them.

### Leaving channels out

`exclude_channels=["ECG"]` leaves those channels out; `channels=[...]` copies
only the ones named. Use one or the other. An excluded channel leaves no
trace: no folder, no annotations, no entry in a seizure annotation's channel
list, and its name is taken out of any text that is kept. Naming an unknown
channel, or leaving none, is refused.

### Encryption

- An **encrypted source** needs its level-2 (full-access) password: the
  recording-time offset and the subject fields are in the part only that
  password opens.
- The **copy** is unencrypted by default. Pass
  `output_password=("level1", "level2")` to encrypt it, with the same rules as
  `Writer`. The source's passwords are never carried over.
- **Data blocks** that the source had encrypted are encrypted again with the
  copy's passwords, at the same level, so protection is never silently
  dropped. They are decrypted only when the copy has no passwords. Blocks the
  source had unencrypted (everything mef3io writes) stay unencrypted, and the
  copy stays readable by pymef and meflib. pymef cannot read encrypted data
  blocks at all, and meflib needs the level-2 password even for level-1
  blocks.

### What is refused

Nothing is written when:

- the destination exists, lies inside the source, or does not end in `.mefd`;
  a leftover `<dst>.partial` from an interrupted run is in the way (remove it
  if no copy is running);
- the session has something `inspect` marks as blocking: video channels,
  unreadable or incomplete segments, parts the password does not open,
  segments that disagree on the recording-time offset, or a time stored in
  the legacy convention that cannot be told apart from an absolute one (see
  [The sign convention is not reliable either](#the-sign-convention-is-not-reliable-either));
- the shift is zero, smaller than the recording's length plus one day, or
  would move a time to 1970 or before;
- a data block or annotation fails its checksum: damaged data is never
  re-sealed into a copy that would then look intact.

A session that uses meflib's recording-time offset **is** accepted when the
offset is unambiguous: its times are read as true UTC, moved, and the copy
has offset 0.

### Checking before copying

`mef3io.anonymize.check` runs every check `anonymize_session` makes before it
writes, and writes nothing. It takes the same arguments (without
`progress=`), so an application can tell the user that a name, a channel
selection or a date will be refused while they are still choosing it:

```python
rep = mef3io.anonymize.check("patient.mefd", "S001.mefd", shift, pseudonym="P-17")
if not rep.ok:
    for r in rep.refusals:
        print(r.code, r.field, r.message)
```

- **The same checks, through the same code.** One pre-flight function in the
  C++ core serves both: `anonymize_session` raises the first refusal it
  finds, `check` returns all of them, in the same order. They cannot
  disagree. `anonymize_session` makes all of these checks before it creates
  any file or reports any progress.
- **It writes nothing and reads only metadata**: what `inspect` reads, plus
  each kept segment's 1 KB metadata header and the kept channels' annotation
  files. It never reads the data (`.tdat`) files, so it takes about as long
  as `inspect`. It releases the GIL and accepts `cancel=`.
- **`shift_us=None`** skips the shift checks, for a date not chosen yet.
- A check that depends on one that failed is skipped: when the source cannot
  be opened, nothing about it is checked.
- `rep.notes` holds what the copy would note up front: a source using
  meflib's recording-time offset, an output password other MEF readers will
  not handle as expected.

`rep.ok` means the copy will start. A few refusals can only come from the
copy itself (marked *copy* below): a data block that fails its checksum or
runs past the end of its file, which needs the data blocks, and a failed
verification, which needs the finished copy. Every time in the metadata
and annotation files goes through the copy's own shifting rule up front, so
a time at or before a non-zero recording-time offset, or one the shift would
take to 1970 or before, is caught there; only a time in a block index or
data file's header, or a data block's own time, is met by the copy alone,
with the same code.

No refusal or verification message holds one of the source's times: they
name the field and describe the problem in words. Messages may name the
source's and the copy's paths.

`check` is in Python and C++ (`mef3io::check_anonymize`), not in MATLAB.

### Refusal codes

Every refusal has a stable `code`. On the exception `anonymize_session`
raises it is `exc.code`, next to `exc.details`, a dict with `channel`,
`field` and `inspect_code` (each `None` when it does not apply); in a
`check` report it is `Refusal.code`. The exception types are mef3io's usual ones (`ValueError`, `TypeError` and
the `MefError` subclasses in the table), so ordinary `except` clauses work. Match on the
code, never on the message: messages may be reworded, codes are not. Errors
that are not refusals (a cancel, an exception raised by your own callback, an
internal error) have no code. `mef3io.anonymize.REFUSAL_CODES` holds this
table.

*Where*: **check** = found before anything is written, by `check` and by
`anonymize_session`; **copy** = only the copy pass can find it, and the
partial copy is removed; **defensive** = guards a state an earlier check
already rules out, or a fault of the system (a full disk), so that every
refusal has a code.

| Code | Raised as | Where | Meaning |
|---|---|---|---|
| `argument-type` | `TypeError` | check | A channel list holds something other than names. Raised by `check` too. |
| `clear-unknown` | `ValueError` | check | `clear=` names something other than subject, location, descriptions. |
| `text-invalid` | `ValueError` | check | `text=` is not `"drop"`, `"keep"` or a callable. |
| `shift-zero` | `ValueError` | check | The shift is 0. |
| `channel-selection-conflict` | `ValueError` | check | Both `channels` and `exclude_channels` given. |
| `text-callback-missing` | `ValueError` | defensive | C++ only: the callback policy without a callback. |
| `output-password-unpaired` | `ValueError` | check | One output password, or not a pair of non-empty strings. |
| `output-passwords-equal` | `ValueError` | check | The two output passwords are the same. |
| `dst-not-mefd` | `IoError` | check | The destination does not end in `.mefd`. |
| `dst-exists` | `IoError` | check | The destination exists (also when it appears while copying). |
| `partial-exists` | `IoError` | check | A `<dst>.partial` from an earlier run is in the way. |
| `source-missing` | `IoError` | check | The source does not exist. |
| `dst-inside-src` | `IoError` | check | The destination lies inside the source (or the other way round). |
| `source-unreadable` | `IoError` | check | The source cannot be opened as a session (not `.mefd` / `.mefd.tar`, a compressed archive, an unreadable file). |
| `source-damaged` | `FormatError` | check | Source metadata or annotation files are damaged (truncated, malformed). |
| `source-crc` | `CrcError` | check | Source metadata or an annotation fails its checksum. |
| `records-ambiguous` | `FormatError` | check | Several session-level annotation files and none named after the session. |
| `password-wrong` | `PasswordError` | check | The password opens nothing. |
| `password-required` | `PasswordError` | check | The source is encrypted and no password was given. |
| `password-level1-only` | `PasswordError` | check | The password is the level-1 one; anonymizing needs level 2. |
| `video-channels` | `WriteConflictError` | check | The session has video channels. |
| `unreadable-segments` | `WriteConflictError` | defensive | `inspect` could not read a segment (a strict open fails first with `source-damaged`). |
| `incomplete-segments` | `WriteConflictError` | check | A segment folder lacks its metadata, index or data file. |
| `unread-record-files` | `WriteConflictError` | check | Annotation files at the session root that are not the session's own. |
| `unreadable-records` | `WriteConflictError` | defensive | `inspect` could not read an annotation file (a strict open fails first). |
| `parts-locked` | `WriteConflictError` | check | Parts the password does not open (comes with a password refusal). |
| `rto-disputed` | `WriteConflictError` | check | Segments disagree on the recording-time offset. |
| `rto-unreadable` | `WriteConflictError` | check | The recording-time offset cannot be read, so some times are unknown. |
| `time-before-rto` | `WriteConflictError` | check | A time stored at or before a non-zero recording-time offset (a block index, data header or block time: only in the copy pass). |
| `inspect-blocker` | `WriteConflictError` | defensive | A blocking `inspect` warning this table does not know yet (`details["inspect_code"]` names it). |
| `channel-unknown` | `ValueError` | check | A named channel is not in the session (`details["channel"]`). |
| `no-channels-left` | `ValueError` | check | The selection leaves no channel. |
| `time-range-invalid` | `ValueError` | check | `time_range` does not start before it ends. |
| `time-range-empty` | `ValueError` | check | No channel of the copy has samples inside `time_range`. |
| `shift-below-floor` | `ValueError` | check | The shift is smaller than the recording's span plus one day. |
| `shift-not-positive` | `ValueError` | check | The shift moves a time to 1970 or before (a block index, data header or block time: only in the copy pass). |
| `rename-name-mismatch` | `ValueError` | check | `rename=False` and the destination is not named after the source. |
| `name-contains-identity` | `ValueError` | check | The copy's name holds the patient's name, ID, location or the original session name. |
| `pseudonym-contains-identity` | `ValueError` | check | The pseudonym holds one of them. |
| `name-too-long` | `ValueError` | check | The copy's name or pseudonym is longer than 255 bytes. |
| `segment-folder-unnumbered` | `WriteConflictError` | check | A segment folder's name does not end in a number. |
| `segment-number-mismatch` | `WriteConflictError` | check | A segment folder's number differs from its metadata's. |
| `segment-number-duplicate` | `WriteConflictError` | check | Two segment folders of one channel have the same number. |
| `record-damaged` | `FormatError` | defensive | An annotation file is damaged (`inspect` fails first: `source-damaged`). |
| `record-crc` | `CrcError` | defensive | An annotation fails its checksum (`inspect` fails first: `source-crc`). |
| `record-locked` | `PasswordError` | check | An annotation the password does not open (listed by `check` after the password refusal). |
| `record-index-mismatch` | `FormatError` | check | An annotation index does not match its annotation file. |
| `id-number-time` | `WriteConflictError` | check | An `Epoc` or `Curs` ID number holds a time inside the recording, which the copy would carry. |
| `text-unremovable` | `WriteConflictError` | check | Text the copy keeps (`text="keep"`, or kept descriptions) holds a removed name in a form the removal does not take out, such as a left-out channel's name glued into a longer word; the copy's check would refuse it. The message names the field, never the text. Drop the text, or clear the descriptions, for this copy. |
| `block-crc` | `CrcError` | copy | A data block fails its checksum; damaged data is never re-sealed. |
| `data-damaged` | `FormatError` | copy | A data block runs past its file, or the index and data disagree. |
| `verify-failed` | `WriteConflictError` | copy | The copy failed its verification (see below); it was removed. |
| `data-locked` | `PasswordError` | defensive | An encrypted data block the password does not open. |
| `time-overflow` | `FormatError` | defensive | A stored time out of range. |
| `write-failed` | `IoError` | defensive | Writing the copy failed (disk full, permissions). |
| `io-error` | `IoError` | defensive | Any other input/output error without a code of its own. |
| `write-conflict` | `WriteConflictError` | defensive | Any other conflict without a code of its own. |
| `invalid-argument` | `ValueError` | defensive | Any other bad argument without a code of its own. |

Several blocking conditions found by `inspect` are raised as ONE exception
whose code is the first one's and whose message lists them all; `check`
returns each as its own refusal, with `inspect_code` naming the `inspect`
warning.

### The original times in the report

`AnonymizeReport.source_start_us` and `source_end_us` are the source's first
and last sample times (µs since 1970, UTC), and `source_channel_times` maps
each kept channel to its own `(start, end)` (`(None, None)` for a channel
that holds only annotations).

**They are the re-identification key**: with `shift_us` they give the real
dates back. They are returned to your program in memory and nowhere else.
mef3io never writes them into the copy (its leak scan would refuse the copy
if one appeared there), never logs them, and never puts them in a warning or
an error message. They are left out of `repr(report)`, `report.summary()`
and `report.to_dict()`; `report.to_dict(include_original_times=True)`
includes them. A pickled report arrives **without** them unless it was
marked with `report.with_original_times_in_pickle()` (to hand it to a helper
process you control, never to a cache); `copy.copy` and `copy.deepcopy` keep
them, and so does `dataclasses.asdict(report)`, so prefer `to_dict()`. Keep them as you would the original recording, apart from the
copy.

### How the copy is checked

`anonymize_session` checks every copy with `mef3io.anonymize.verify` before
giving it its final name; you can run the same check later:

```python
report = mef3io.anonymize.verify("patient.mefd", "S001.mefd", shift)
assert report.ok, report.summary()
```

It confirms that:

- the [validator](validation.md) finds nothing in the copy that the source
  did not already have;
- every data block's compressed data and its compression table are
  byte-identical to the source's (compared decrypted where encrypted), so the
  samples are too;
- every time differs from the source's by exactly the shift, in the same
  stored form, and the time-zone fields hold their set values;
- the annotations match in number, order and type;
- **no date leaked.** Every time field, one field at a time, is compared with
  the source's recording dates (widened by a day, plain or negated), and so
  is the ID number of `Epoc` and `Curs` annotations, which a recorder may
  fill with a time; every text field, every name in a seizure's channel list
  and every folder and file name is searched for the names that should be
  gone, as plain text, ignoring case and word boundaries; every byte after a
  text field's end must be empty; no source identifier is reused. Sizes,
  counts and sample values are never read as times, and the compressed data,
  whose bytes look random, is compared rather than searched, so the check
  does not raise false alarms from the signal.

The name search never uses the same matcher as the replacement, so anything
the replacement misses stops the copy instead of reaching it.

**What counts as a name.** Each subject name and the recording location are
searched as a whole and word by word (every run of 3 or more letters), so
"Jane Roe" in one field, or "Roe, Jane", also finds "Roe" and "Jane" on their
own. The subject ID is searched as a whole and, for each run of 3 or more
digits in it, as that number standing on its own ("EEG-0042" also finds
"0042", but not "EEG": an ID's letters are usually an institution's prefix).
The location's single words are searched in the text the copy writes, not in
channel names, where words such as "EEG" or "ECG" are ordinary labels; the
location as a whole still counts there. Values are trimmed and their inner spaces collapsed,
since fixed-width fields are often padded. Upper and lower case are treated
alike for the Latin scripts (ASCII, Latin-1 and Latin Extended-A, which
cover the accented letters of most European languages); the search also
ignores accents there, so "Muller" or "JIRI" in a copy is found for
"Müller" or "Jiří". Names in other scripts are matched exactly as stored.

It follows two rules:

- **Fields the copy writes** (descriptions, annotation text, header names):
  a name that should be gone stops the copy wherever it appears, even inside
  another word. Four overlaps are reported as **notes** instead: a value of
  fewer than 3 characters inside the copy's own name ("12" in "S012"); a
  value inside a kept channel's name (the next rule decides); a number of
  fewer than 4 digits inside a longer number ("12" in "12:30", "12.5" or
  "1200"; a comma separates numbers, so "1,123" holds the number 123); and a
  word of fewer than 4 letters inside a longer word ("Ann" in "channel",
  "Lee" in "asleep"). Standing on its own, such a word or number is removed
  by the copy, or stops it.
- **Channel names**, which the copy keeps exactly as they are, and the
  folder, file and header names made from them: the name is split into
  parts at anything that is not a letter or digit (`_` and `-` included).
  The copy is stopped when a part **equals** a removed name of 3 or more
  characters ("Jane_C3" when the subject is Jane), when a removed name of
  3 or more letters stands next to digits ("Roe1"), or when a removed name of
  4 or more characters is inside a part ("JaneRoe"). Anything shorter or
  looser ("12" in "LAH12", "P3" as the electrode P3) is a note. This rule
  is strict on purpose: a subject named "Ann" stops a copy with a channel
  called "ANN1", and the way out is to leave that channel out with
  `exclude_channels=`. The
  **original session name** follows only the first of these rules, because
  recorders often name sessions after the montage: "eeg" in a channel
  "EEG1" is a note.

Notes are listed in `report.verification.notes` and printed by `summary()`.
They never repeat the value itself. Look at them before sharing the copy.

With `text="keep"`, names shorter than 4 characters and numbers are replaced
only as whole words or whole numbers, so "rec" does not change "recorded"
and the ID "12" does not change "12:30"; those overlaps are notes.

The replacement ignores case and accents the same way the search does, so
"Muller" in a kept note is taken out for the patient "Müller". What the
search would still stop is stopped **before** anything is written:
- an excluded channel "a3" glued into "a3a3" in a kept note is one example;
- `check` and `anonymize_session` both refuse it with the code
  `text-unremovable`, naming the field but never the text;
- the fix is to drop the text, or clear the descriptions, for that copy.

`mef3io.anonymize.redact_text(src, dst, texts, ...)` returns exactly the
text the copy would keep for the given strings, with the same options. An
application can use it to compare a copy's text with what mef3io itself would
leave, without copying the rules. It writes nothing.

For a source that used meflib's recording-time offset, times are also
compared with the source's *stored* values (the time of day). A negative
shift that lands the copy within a few days of 1 January 1970 can therefore
be refused although nothing leaked; choose a later date.

The report never repeats a name it found, so it can be logged.

### What it does not do

- It does not change the **signal**. A recording can identify a person by
  its content (an unusual rhythm, a voice on an audio channel). Leave such
  channels out.
- It keeps **channel names**, sampling rates, filter settings and file
  sizes.
- It does not handle **video** (sessions with video channels are refused).

## The recording-time offset (deprecated)

meflib can store a session's times relative to a *recording-time offset*,
which was meant to hide the recording date. It does not, and mef3io treats
it as deprecated: it reads and extends such sessions correctly, but never
creates one. Use [`anonymize_session`](#de-identified-copies-with-anonymize_session)
to de-identify a recording.

### In plain words

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
- For sharing, make a de-identified **copy**: every date moved, names
  removed. See
  [De-identified copies](#de-identified-copies-with-anonymize_session).

### What the offset is

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

### Worked example: getting the date back

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

### What else the offset leaves in place

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

#### The sign convention is not reliable either

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
checks down whenever `rto ≠ 0`. It is also why
`anonymize_session` refuses a session with an ambiguous legacy time
convention.

### Deprecated in mef3io

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
- **New de-identification goes through `anonymize_session`.** It produces a
  separate copy with `recording_time_offset = 0`; see the next section.

### Backward compatibility

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

### If you have RTO-"anonymized" data today

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
- **To share it de-identified, make a copy with `anonymize_session`**
  ([De-identified copies](#de-identified-copies-with-anonymize_session)). It
  reads the offset session's true times, moves every date, sets the offset to
  0 and removes identity. Do not treat a recording-time offset as
  de-identification.
