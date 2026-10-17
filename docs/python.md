---
description: "Python guide to mef3io: read and write MEF 3.0 EEG/iEEG recordings with Reader and Writer, inspect sessions, make de-identified copies, cancel long reads, threads, tar archives and the mef_tools drop-in."
---

# Python guide

```python
import mef3io
```

Two API layers ship in the package:

- **`mef3io.Reader` / `mef3io.Writer`** — the native API (recommended).
- **`mef3io.MefReader` / `mef3io.MefWriter`** — a drop-in for legacy
  `mef_tools.io` code.

Times everywhere are **uUTC**: microseconds since the Unix epoch, as `int`.

Errors from the core raise a subclass of `mef3io.MefError` carrying the C++
message: `PasswordError` (missing or wrong password), `CrcError` (a checksum
does not match), `FormatError` (not valid MEF 3.0), `IoError` (file system,
naming) and `WriteConflictError` (a write that does not fit the session, such
as an append that overlaps). `MefError` subclasses `RuntimeError`, so older
`except RuntimeError` code keeps working. `mef3io.Cancelled` (see
[Cancelling and progress](#cancelling-and-progress)) is deliberately not a
`MefError`.

## Writing

```python
with mef3io.Writer("session.mefd", overwrite=True, units="uV") as w:
    w.write("ch1", data, start_uutc, fs=256.0)
```

`Writer(path, overwrite=False, password1="", password2="", units=None,
block_length=None, n_threads=0, metadata=None, durability="fast")`

- `overwrite=True` deletes an existing session first. With
  `overwrite=False`, an existing session is **reopened for appending** —
  state is recovered from disk, so this works across program runs.
- Encryption: pass **both** passwords (MEF has no "level-1 only" files; see
  the [encryption model](encryption_model.md)). It encrypts the subject
  fields, technical metadata and annotation text, **not the samples or their
  timing**, which decode without a password; see
  [What encryption protects](encryption_model.md#what-encryption-protects).
  Passwords are keyed exactly as meflib keys them, so a session opens with
  the same password in pymef and `mef_tools`. A password longer than 15
  characters (or 15 UTF-8 bytes), or one with a character above U+00FF,
  raises a `PasswordInteropWarning`; `mef3io.password_interop_issue(password)`
  returns the same reason (or `None`) without writing anything, so an
  application can warn first. A session that mef3io 1.2.0 or earlier
  encrypted with a non-ASCII password still opens, with a
  `LegacyPasswordWarning` (see
  [password derivation](encryption_model.md#passwordkey-derivation-for-completeness)).
- **Appending to an encrypted session needs the level-2 password.** That
  includes sessions at offset 0. Section 3, which holds the recording-time
  offset every new time is stored against, is level-2 encrypted, and without
  it the offset is unknown. A Writer opened with only the level-1 password is
  refused (`PasswordError`) for anything that stores times into that
  session, rather than guessing the offset.
- `Writer.unreadable_metadata` lists segment metadata the writer could not
  use (`"path: reason"`):
  - `.tmet` files it could not read while choosing the offset. At offset 0
    such a file is skipped, so one damaged segment does not block unrelated
    writes. Where the offset is non-zero, or nothing else is readable, the
    write is refused instead.
  - Segment directories without metadata (what a crashed write can leave
    behind). These are ignored, never adopted. Segments are numbered by the
    highest real segment number, so such a directory is neither appended to
    nor collides with a new segment. An entry is dropped once a new segment
    fills that directory.

  Every entry raises an `UnreadableSegmentWarning`, when the Writer opens or
  during the write that finds it (`mef3io:unreadableMetadata` in MATLAB).
- **A refused write leaves the session exactly as it was.** Every check runs
  before any directory or file is created: the record type, the time, and the
  offset (known and agreed). A write that fails later anyway removes the
  directories it created.
- **Cost of choosing the offset.** A session-level annotation or a new
  channel needs the offset that every segment of the session agrees on. The
  first one in a Writer reads and CRC-checks each segment's `.tmet` record
  (16 KB; the offset in section 3 has no CRC of its own, so it is trusted
  only after the body CRC), e.g. 1.4–1.8 s and 157 MB at 64 channels × 150
  segments. A channel-level annotation reads only that channel's
  segments. Each answer is kept for the Writer's lifetime, so every later
  one costs nothing. Keep one Writer open across an acquisition, as for
  appends.
- `metadata=` sets subject and acquisition fields (see
  [Session metadata](#session-metadata-subject-acquisition)).
- `durability="fast"` (the default) writes without flushing to disk;
  `durability="full"` adds the flushes that make an append survive a power
  cut. After an unclean shutdown in fast mode, run `recover_session`. See
  [Long recordings](long_recordings.md).
- `block_length` overrides the RED block size (default: `fs` samples for
  fs ≥ 5000, else `10*fs`). `n_threads=0` uses all cores.

### `write(channel, data, start_uutc, fs, precision=-1, new_segment=False)`

Float path. `data` is float64; **NaN runs are not stored** — they become
discontinuity gaps that read back as NaN. Values are quantized to int32
counts as `round(data * 10**precision)` with the conversion factor
`10**-precision` kept in metadata; `precision=-1` infers it from the data
(and on appends reuses the segment's stored precision so appends can never
conflict).

The first write to a channel creates segment 0; later writes **append
in-segment** (extending the existing files, like the legacy writer);
`new_segment=True` forces a fresh segment. Appends must match the segment's
fs and conversion factor and start at/after its end time — violations raise
`WriteConflictError`.

Returns a summary dict: `samples_written`, `blocks`, `gaps_skipped`,
`segment`.

### `write_int32(channel, data, ufact, start_uutc, fs, valid=None, new_segment=False)`

The primitive path: int32 counts stored **verbatim** (bit-exact round trip)
with `ufact` (e.g. an amplifier's volts-per-bit) in metadata. Optional
`valid` mask (same length, anything numeric — nonzero = valid): marks gap
samples. Because counts are never altered, `data` must be integer-typed
(any width): floating-point input raises `TypeError` (use `write()` for
float data), and values outside the int32 range raise `ValueError` instead
of silently wrapping.

### `write_annotations(annotations, channel=None)`

Records: an iterable of dicts (`time` required; `type` defaults to `"Note"`;
optional `text`, `duration`) or a pandas DataFrame with those columns.
`channel=None` writes session-level records. Replaces the records at that
level; in encrypted sessions record bodies are level-2 encrypted.

## Reading

```python
with mef3io.Reader("session.mefd") as r:          # password="..." if encrypted
    r.channels                                    # list[str]
    info = r.info("ch1")
    x  = r.read("ch1", t0, t1)                    # float64, NaN gaps
    xi = r.read_raw("ch1", t0, t1)                # int32 + validity mask
    r.segments("ch1")                             # per-segment map
    r.toc("ch1")                                  # block-level index
    r.records()                                   # session-level annotations
    r.records("ch1")                              # ch1's channel- + segment-level ones
```

`Reader(path, password="", backend="cpp", n_threads=0, cache=None,
warn_declarations=True, strict=True)`

- A **level-2 password** unlocks everything; a **level-1 password** reads the
  signal and technical metadata while section-3 subject metadata stays locked
  (`info()["section3_available"] == False`, subject fields `None`) and
  records are unavailable. Without a password the Reader refuses an
  encrypted session; that is mef3io's policy, not protection, because the
  samples themselves are not encrypted
  ([details](encryption_model.md#what-encryption-protects)).
- `strict=True` (the default) raises when any segment cannot be read.
  `strict=False` skips it instead: its time span reads back as NaN, it is
  listed in `Reader.problems`, and an `UnreadableSegmentWarning` is raised.
  Check `problems` before trusting such a read, because a skipped segment
  looks exactly like a recording gap.
- `warn_declarations=True` raises one `SessionDeclarationWarning` when the
  session leaves buffer-size declarations unset (files from mef3io 1.1.2 and
  earlier, and from the legacy stack). Reading with mef3io is unaffected; see
  [Validating and repairing](validation.md).
- A `BlockCopyWarning` means a block's index entry and its own header
  disagree on its time or length. The header is used, as meflib and pymef do;
  the detail is in `read_raw(...)["block_copy_mismatches"]`.

### Warnings carry the path as data

Every warning mef3io emits is a `mef3io.MefWarning` (a `UserWarning`). Its
message never contains the session's location (its directory or archive path)
or its name: they appear as `<session>`, also inside an archive and in the
session's own file names. **Only those are scrubbed.** Channel names and the
names of files inside the session (`<session>/ch1.timd/ch1-000001.segd/...`)
can appear, and they can be identifying too. The session is on the warning
object instead, with the structured items the message summarises:

```python
with warnings.catch_warnings(record=True) as caught:
    warnings.simplefilter("always")
    r = mef3io.Reader(path, strict=False)
for w in caught:
    if issubclass(w.category, mef3io.MefWarning):
        log.warning("%s", w.message)   # no session path or name (channel names may appear)
        w.message.path                 # the session, as it was passed in
        w.message.details              # e.g. the Reader.problems entries (may hold paths)
```

`warnings.filterwarnings("ignore", category=mef3io.MefWarning)` silences all
of them. Python's `"default"` (and `"module"`) filter action shows each
warning once per session, not once per process.

### `read(channel, t0=None, t1=None, n_threads=None, *, cancel=None, progress=None)`

Float64 samples on the uniform fs grid over `[t0, t1)` (whole channel by
default): `N = round((t1 - t0) * fs / 1e6)` samples, discontinuity gaps
filled with NaN, values scaled by the conversion factor. Windowed reads
fetch only the needed bytes — cheap on huge sessions. `cancel=` /
`progress=`: see [Cancelling and progress](#cancelling-and-progress).

### `read_raw(channel, t0=None, t1=None, n_threads=None, *, cancel=None, progress=None)`

Dict with the stored `samples` (int32), a `valid` mask (0 in gaps),
`start_uutc`, `sampling_frequency`, `units_conversion_factor`.

### `info(channel)`

Dict: `sampling_frequency`, `units_conversion_factor`, `units_description`,
`start_time`, `end_time`, `number_of_samples` (stored samples — gaps are not
counted, so a gridded read usually returns more), `recording_time_offset`,
`n_segments`, `section3_available`, and the section-3 subject fields
(`subject_name_1/2`, `subject_id`, `recording_location`; `None` without
level-2 access).

### `segments(channel)` and `toc(channel)`

`segments` maps what data is where — one dict per segment with
`start_time`/`end_time`, `start_sample`, `number_of_samples`,
`number_of_blocks`, `path`, and `provenance` (which mef3io versions created
and last modified the segment and which operations it has been through; `None`
for files from another writer or from mef3io before 1.2; see the
[provenance region](mef3_format.md#provenance-region-mef3io-12-frozen-format)) — from metadata only (nothing decoded), so it is
cheap even for huge, gap-riddled sessions. `toc` is the finer per-RED-block
view (start time, sample counts, extrema, discontinuity flags).

### `records(channel=None, segment=None, level=None)`

MEF keeps record (annotation) files at three levels — session, channel and
segment — and every returned dict says which: `level`
(`"session"`/`"channel"`/`"segment"`), `channel` (`None` at session level) and
`segment` (`None` unless segment level).

```python
r.records()                       # session-level records
r.records("ch1")                  # ch1's channel-level AND segment-level records
r.records("ch1", segment=0)       # only segment 0's
r.records("ch1", level="channel") # only the channel file
r.records(level="all")            # every record in the session (see below)
```

Each dict has `type`, `time` (absolute uUTC), the three location keys,
`version_major`/`version_minor`, then the body fields of its type, always the
same keys for a type and `None` where the body does not carry one: `text`
(Note, SyLg); `duration` + `text` (EDFA); `earliest_onset`, `latest_offset`,
`duration`, `number_of_channels`, `onset_code`, `marker_name_1/2`,
`annotation` and `channels` (a list of `{"name", "onset", "offset"}`) for Seiz;
and the Epoc, Curs, CSti, ESti and LNTP fields listed in the
[format reference](mef3_format.md#body-layouts-mefrech). Body times
(`earliest_onset`, `latest_offset`, a Seiz channel's `onset`/`offset`, Epoc
`timestamp` / `end_timestamp`, Curs `trace_timestamp`) are absolute uUTC like
`time`; the
[format reference](mef3_format.md#record-times-and-the-recording-time-offset)
explains how they are resolved. If a channel's (or the session's) segments
disagree on the recording-time offset and its record file stores
offset-dependent times, that one file raises `FormatError` rather than
misplacing its records (a lenient Reader skips it, lists it in `problems` and
warns; the segment-level records are unaffected). A record file that cannot be
read is handled the same way. Reading every body type is supported; writing is
limited to Note, SyLg and EDFA.

Written times are stored as `−t` in a session at offset 0 (every session
mef3io creates). In a foreign session "anonymized" with meflib's deprecated
recording-time offset they are stored as `rto − t`, the session's own form,
so mef3io never writes the date there in plain. A segment or channel mef3io
adds to such a session inherits its offset. The offset does not
de-identify the session, though: the date is in section 3 (see
[De-identification](anonymization.md)).

A write is refused, rather than misplacing times or breaking the session's own
`rto − t` form, when:
- the time is at or before the offset (`WriteConflictError`);
- the level's segments disagree on the offset (`WriteConflictError`);
- the offset is locked because only the level-1 password was given
  (`PasswordError`);
- the offset cannot be verified because segment metadata in scope is
  unreadable, and the readable segments use a non-zero offset or none are
  readable at all (`WriteConflictError`).

A channel that has no data yet takes the session's offset for its
annotations. Its records are listed: `records("chNew")` and
`records(level="all")` include it. It is not added to `Reader.channels`,
which lists channels with samples.

The `mef_tools`-compatible `MefReader.get_annotations(channel)` keeps the
legacy meaning — that channel's channel-level records only — while
`Reader.records(channel)` also includes its segment-level records.

### Warm-start cache (opt-in)

```python
mef3io.Reader("session.mefd", cache="auto")           # per-user OS cache dir
mef3io.Reader("session.mefd", cache="/data/s.cache")  # persistent snapshot
```

Snapshots channel metadata so warm opens serve `channels`/`info` without
reading the session tree, deferring the backend until a data read.

Whether a snapshot is still valid is decided only when a cached open
happens; writing to a session never touches it. The check is small and costs
the same whatever the number of segments. Per channel it compares:

- the segment count;
- the start and end times;
- the last segment's file sizes;
- the last segment's provenance region: which mef3io operation last modified
  it, and how many times;
- the random file IDs of the first and last segment's metadata files. This
  catches a session rewritten with `overwrite=True` from the same samples but
  with new units, conversion factor or subject.

A snapshot is used only by the exact mef3io build that wrote it (the version
plus the compiled extension). On any mismatch it is ignored, the session is
opened normally, and a fresh snapshot is written.

No metadata snapshot is saved for a session with no more segments than
channels, for example 128 channels with one segment each. There a snapshot
cannot make an open faster: measured, a cached open matched a plain one, and
building the snapshot made the first open about twice as slow. A tiny marker
is saved instead. It makes later `cache=` opens go straight to a plain open,
at the cost of one small file read. The marker is dropped once the session
grows past one segment per channel, and a real snapshot follows.

**Encrypted sessions are never cached.** They are always opened normally, and
any snapshot file found for one is deleted, whatever version wrote it.

**Blind spot:** an in-place change to a segment that is *not* the channel's
last is not detected if it leaves the channel's start and end, and everything
checked in its last segment, unchanged. That covers:

- edits by other tools;
- mef3io's own `repair_session` or `recover_session` on an older segment, in
  the cases where it could not stamp the channel's last segment. Normally such
  a repair also stamps the last segment `segment-repaired`, which the check
  does see. The stamp is skipped when the last segment's provenance region
  belongs to another application, its CRC fails, or a writer's `.mef3io-tmp`
  file sits next to it. The repair report then says that cached opens will
  not see the change.

If a session may have been edited that way, open it without `cache=`: a plain
open always reads everything.

## Inspecting a session without reading its samples

`mef3io.inspect(path, password="")` returns a `SessionSummary`: everything a
tool needs to know about a session, read from the metadata and the record
files only. It **never reads a `.tdat`**. Data files appear only as file sizes,
so inspecting a terabyte session costs about the same as opening a `Reader`.

```python
s = mef3io.inspect("session.mefd")            # or "session.mefd.tar", read in place

s.layout, s.total_bytes                       # "directory" / "tar", bytes
s.start_time, s.end_time                      # first / last sample, all channels (uUTC)
s.first_time, s.last_time                     # samples AND annotations
for c in s.channels:
    c.name, c.sampling_frequency, c.number_of_samples
    c.channel_description                     # with include_values=True, else None
    c.segments                                # SegmentSummary: times, samples, sizes, provenance
    c.gaps                                    # [(start, end), ...] uUTC spans with no samples
s.records                                     # RecordSummary at every level, with times
s.encryption                                  # what is encrypted, what the password opened
s.time_offset                                 # meflib's recording-time offset, in plain words
s.identity_field("subject_id").present        # True / False / None (locked)
s.legacy_password_mapping                     # opened only through mef3io <= 1.2.0's password mapping
[w.code for w in s.warnings]
s.to_dict()                                   # plain data, ready for json.dumps
```

From the command line:

```bash
python -m mef3io inspect session.mefd           # text
python -m mef3io inspect session.mefd --json    # the same as to_dict()
```

What it reports:

| Part | Contents |
|---|---|
| Session | name, path, layout, total size, first/last sample and record times, the recording-time offset and how it was read, GMT offset and DST fields |
| `channels` | sampling rate, units, the channel description (with `include_values=True`), start/end, stored samples, the segment map (`segments`, the same fields as `Reader.segments()` plus file sizes, declared discontinuities and the offset state), gaps, record count |
| `records` | every record at the session, channel and segment level: type, time, duration, and `first_time` / `last_time` over everything the record mentions (header time, body times such as `Seiz` onsets, `time + duration`) |
| `identity` | subject name 1/2, ID and location (section 3), the session, channel and reference descriptions (section 2), `session_name` and `anonymized_name` (every universal header), and record text: where each lives, in how many places it is set, its longest length, how many distinct values. **Values only with `include_values=True`.** |
| `warnings` | each with a `code`, a plain-words `message`, and `blocks_anonymize`; `s.anonymize_blockers` lists the ones `anonymize_session` refuses on |
| `problems` | with `strict=False`, what could not be read (same shape as `Reader.problems`) |
| `incomplete_channels` | channel folders without one complete segment. A segment folder missing its `.tmet`, `.tidx` or `.tdat` is skipped silently by a `Reader`; `inspect` lists it as `readable=False`, counts its files, reports what it may hold as unknown, and warns (`incomplete-segments`) |

Options:

- `include_values=True` returns identity values: subject fields, descriptions,
  record text and the full records (`RecordSummary.fields`, as
  `Reader.records()` returns them), each channel's `channel_description`, and
  the recording-time offset. Off by default. **Names and paths are not de-identified either way**: the session's
  folder name (MEF names it after the session, so it usually *is* the
  `session_name`), channel names and file paths, also inside warning and
  problem messages, are always returned. A summary of an identifiable session
  is itself identifiable.
- `read_index=True` also reads every segment's block index (about 2% of the
  data), which finds gaps **inside** a segment. Without it, `gaps` lists only
  the gaps between segments and `gaps_complete` is `False`. An appended
  recording is usually one segment per channel, so ask for the index when
  the gaps matter. Gaps come from the index's copy of each block's time and
  sample count; the authoritative copy, the RED block header, is in the
  `.tdat` and is never read. `gaps_certain` is `False` when a listed gap
  might hold data from a segment that could not be read or placed, or when a
  segment whose times assume offset 0 sits next to one whose non-zero offset
  was read.
- `strict=False` reports an unreadable segment or record file in `problems`
  and a warning, marks the segment `readable=False`, and describes the rest.
  Strict, the default, raises as a `Reader` does.
- `cancel=` and `progress=` work as for the other long operations (see
  [Cancelling and progress](#cancelling-and-progress)).

**Encrypted sessions are described, not refused.** Without a password,
`inspect` still lists channels, segments, sizes and the plain universal-header
fields. What the password does not open is reported as locked:
`encryption["access"]` is `"none"`, `"level-1"` or `"full"`, the locked
sections and record bodies are counted, and values that live there are `None`.
A password that is given and wrong raises `PasswordError`.

A non-ASCII password that opens the session only through the password mapping
of mef3io 1.2.0 and earlier (#84) is accepted, as a `Reader` accepts it.
`inspect` reports it as data: `legacy_password_mapping` is `True` and a
`legacy-password` warning in `warnings` counts the files. Unlike a `Reader`, it
does not emit `LegacyPasswordWarning`, as it emits no Python warnings at all.

**Times are never guessed silently.** Every time is absolute UTC. A
segment's times are resolved with its own offset.

- **Section 3 locked** (the level-1 password, or none on an encrypted
  session): the offset cannot be read, so times are resolved **assuming
  offset 0**, as a `Reader` does, and flagged `offset_assumed_zero=True` on
  the session, the channel, the segment and each record concerned, with an
  `offset-assumed-zero` warning. mef3io and pymef always write offset 0, so
  these times are right for almost every file. Under meflib's deprecated date
  masking they are the masked times, which is what meflib meant a level-1
  user to see. They are never called verified, and the warning blocks
  anonymization (which needs the level-2 password anyway).
- **Segment unreadable** (lenient mode): its times are `None`, with a
  `times-unresolved` warning.

A channel- or session-level record file is read with the offset all the
segments in its scope share. When they disagree, or one is unreadable, its
offset-dependent times are `None` (`RecordSummary.times_known` is `False`, and
the times in `fields` are `None` too). A locked segment takes part with its
assumed 0, exactly as in a `Reader`, so against a read non-zero offset it is a
dispute (`time_offset["status"]` is `"disputed"`), never a silent choice.

**The recording-time offset is reported plainly.** meflib's recording-time
offset is deprecated as anonymization: the real date stays in section 3 (see
[De-identification and the recording-time offset](anonymization.md)).
`time_offset["status"]` is `"none"`, `"applied"`, `"disputed"`, `"locked"` or
`"unknown"` (no segment readable). It describes the readable segments;
`segments_unknown` counts the ones that could not be read, whose offset is
unknown;
when an offset is set, `date_recoverable` is `True` and `recoverable_by` says
by whom (`"anyone"` if section 3 is plain, `"level-2 password"` if encrypted),
with a `legacy-time-offset` warning. The offset's value is itself identifying
(it is the local midnight of the recording day), so it is returned only with
`include_values=True`. `ambiguous` is `True` when a time is stored
non-negative under a non-zero offset, where it cannot be told apart from an
absolute time; that blocks anonymization. It is checked on the metadata
universal headers, record header times and, with `read_index=True`, the block
index (header and entries). The `.tdat` universal header is never checked,
because `inspect` never opens a `.tdat`.

!!! note "Python and C++ only"
    `inspect` is available in Python and C++ (`mef3io/inspect.hpp`,
    `inspect_session`), not in MATLAB or the C ABI.

## De-identified copies

`mef3io.anonymize_session(src, dst, shift_us)` writes a copy of a session
that can be shared: every time moved by `shift_us`, the subject's identity,
the session name, descriptions and (by default) annotation text removed,
and the samples copied byte for byte. The original is never changed.
`mef3io.anonymize.check` runs the same checks first without writing
anything, and every copy is verified against its source before it gets its
final name.

```python
start = mef3io.inspect("patient.mefd").start_time
shift = mef3io.anonymize.reference_shift(start, "2000-01-01")   # copy starts on that date

rep = mef3io.anonymize.check("patient.mefd", "S001.mefd", shift, exclude_channels=["ECG"])
if not rep.ok:
    print(rep.codes)            # stable refusal codes, e.g. ('name-contains-identity',)
else:
    report = mef3io.anonymize_session("patient.mefd", "S001.mefd", shift,
                                      exclude_channels=["ECG"])
    print(report.verification.summary())
```

Everything else (choosing the shift, what changes, encryption of the copy,
the refusal codes, how the copy is checked, what it does not do) is in
[De-identification](anonymization.md); a runnable example is
[`examples/13_anonymize.py`](https://github.com/bnelair/mef3io/blob/main/examples/13_anonymize.py).
From a terminal: `python -m mef3io anonymize SRC DST --reference-date
2000-01-01`. Python and C++ only, not MATLAB.

## Tar archives (single-file sessions)

A session directory can be packed into a single **uncompressed tar** file —
one file to share or archive, immune to piecemeal corruption of the tree:

```python
tar = mef3io.archive_session("session.mefd")   # -> "session.mefd.tar"
r = mef3io.Reader(tar)                          # read in place, no extraction
```

`Reader` (and the compat `MefReader`) accept the archive path anywhere a
`.mefd` directory is accepted; because the archive is uncompressed, windowed
reads still fetch only the byte ranges they need. Everything else carries
over — encryption, records, the warm-start cache. The source directory is
left untouched, output is deterministic (same session ⇒ identical bytes), and
any tar tool restores the directory (`tar -xf session.mefd.tar`). Foreign
archives work too (GNU/bsdtar/`tarfile`, with or without directory entries);
compressed input (`.tar.gz` etc.) is rejected with a clear error.

!!! note "A mef3io addition, not part of MEF 3.0"
    meflib, pymef and mef_tools cannot open a `.mefd.tar` directly. It is a
    plain uncompressed tar file, so it can always be unpacked:
    `extract_session`, `tar -xf session.mefd.tar` (built into Windows 10 and
    later, macOS and Linux) or the free 7-Zip on Windows. The unpacked
    `.mefd` folder works with the legacy tools.

Tar sessions are **read-only**: `Writer` refuses `.tar` paths. To modify one,
unpack it first — `extract_session` is the exact inverse (archive → extract →
archive reproduces identical bytes), and the restored directory is a normal
writable session again:

```python
session = mef3io.extract_session(tar)          # -> "session.mefd" back
with mef3io.Writer(session) as w:              # appends work again
    w.write("ch1", more, t, fs=256.0)
```

Pass `tar_path=` / `dest_dir=` to choose targets and `overwrite=True` to
replace an existing one.

Session naming is **enforced at every entry point** (reading, writing,
archiving, extracting, in all language bindings): directories must end
`.mefd`, archives `.mefd.tar` — anything else is refused with a clear error.
This is a safety contract: a stray directory or file can never be misread as
a session, and a session can never be created or overwritten under an
unmarked name.

## Cancelling and progress

Reads and the long operations take two optional keyword arguments:

| | `cancel=` | `progress=` |
|---|---|---|
| `Reader.read`, `Reader.read_raw` | yes | compressed bytes decoded |
| `archive_session`, `extract_session` | yes | bytes copied |
| `inspect` | yes (per segment and record file) | bytes of `.tmet` (+ `.tidx` with `read_index=True`) read |
| `Validator.validate`, `validate_session` | yes | bytes of `.tmet` + `.tidx` + `.tdat` covered |
| `repair_session` | yes (validation pass only) | as validate |
| `recover_session` | yes (scan only) | bytes of `.tidx` + `.tdat` covered |
| `anonymize_session` | yes | bytes, over the copy and its verification |
| `anonymize.check` | yes | — |
| `anonymize.verify` | yes | bytes, as for `anonymize_session` |

```python
token = mef3io.CancelToken()

def show(done, total):          # both in bytes
    bar.set(done / total if total else 1.0)

# on a worker thread:
try:
    x = reader.read("ch1", t0, t1, cancel=token, progress=show)
except mef3io.Cancelled:
    pass                        # superseded by a newer request

# from any other thread (a UI's Stop button, a newer pan/zoom):
token.cancel()
```

**Several reads at once.** One `Reader` can serve several threads at once
(see [Parallelism](#parallelism)). When a newer request supersedes a read,
cancel the old one with its own token; the cancelled call returns promptly
and the other reads are unaffected.

**Cancelling.** `token.cancel()` is safe from any thread. The operation
notices between units of work — before each segment is fetched and each block
is decoded on a read, between 1 MB chunks when archiving or extracting,
per segment (and per block examined) when validating or recovering — and
raises `mef3io.Cancelled`. Tokens are one-shot: once cancelled they stay
cancelled, so make a new one per request; one token can be passed to several
calls to stop them together (`token.raise_if_cancelled()` lets your own loop
stop at the same points). `Cancelled` subclasses `Exception` directly, not
`MefError`: stopping on request is not a failure.

**Nothing is left behind.** A cancelled call returns nothing:

- a read returns no partial array;
- `archive_session` builds the archive in a temporary file and
  `extract_session` in a temporary sibling directory, renamed into place only
  when complete — a cancel leaves neither, and with `overwrite=True` the
  existing target is untouched;
- `repair_session` and `recover_session` decide everything during a read-only
  pass and write only after it. A cancel during that pass (which is where the
  time goes) leaves the session byte-identical and creates no backup
  directory. The short write phase that follows — a `.tmet` replaced by
  rename and 1024-byte header patches — is not interruptible: a cancel that
  arrives then is ignored and the call returns its report, so a session is
  never left half-repaired;
- `anonymize_session` builds the copy in `<dst>.partial/` and renames it to
  `dst` only after it has been verified; a cancel removes the partial copy.

**Progress.** `progress(done, total)` is called first with `(0, total)`, then
at a bounded rate (about every 1 % of `total`, otherwise at most every
250 ms), with `done` never decreasing, and finally with `(total, total)`
exactly once, when the cancellable work completes (only the short commit
step after it — a rename, the header patches of a repair — can still fail,
with an I/O error, never with `Cancelled`). For `repair_session` and
`recover_session` that final call comes at the end of the read-only pass,
before anything is written, so a callback that raises there still leaves the
session untouched. Calls never overlap. The work runs
with the GIL released; the callback takes the GIL only for its own call and
may run on a decode worker thread rather than the calling one, so keep it
short and hand UI updates to your UI thread. If it raises, the operation
stops as if cancelled and *your* exception propagates. The callback must not
call back into the same operation.

On a read, progress counts decoding. Each segment's bytes are fetched in one
read first, so a whole-channel read of a huge segment on slow storage shows
`(0, total)` until that fetch is done, and a cancel during it takes effect at
the next segment or block. Windowed (viewer) reads fetch only their window.
With `total == 0` (an empty window) the single `(0, 0)` call is also the
final one.

`extract_session(..., overwrite=True)` briefly needs room for both trees. A
hard kill mid-extract can leave a `<dest>.mef3io-extract-<hex>` staging
directory, and a replaced tree that could not be deleted is left as
`<dest>.mef3io-replaced-<hex>`; both are safe to delete.

When neither keyword is given, nothing changes: the hooks cost one pointer
test per unit of work (see `benchmarks/cancel_overhead_benchmark.py`).

MATLAB does not offer cancelling or progress (a MEX call blocks MATLAB's
only thread); the C ABI does
(`mef3io_cancel_token_*` and the `*_ex` functions, see the
[C++ and C ABI guide](cpp.md#cancelling-and-progress)).

## Session metadata (subject, acquisition)

Set subject and acquisition metadata with the `Metadata` object — grouped
into a `Subject` block (MEF section 3) and an `Acquisition` block (section 2).
Every field is defaulted, so fill in only what you have:

```python
from mef3io import Metadata, Subject, Acquisition

md = Metadata(
    subject=Subject(id="MRN-123", name_1="Jane", recording_location="EMU-4"),
    acquisition=Acquisition(session_description="pre-surgical", line_frequency=60.0),
)
with mef3io.Writer("session.mefd", overwrite=True, metadata=md) as w:
    w.write("ch1", data, start_uutc, fs=256.0)
# or later, before writing:  w.set_metadata(md)   (also accepts a flat dict)
```

Read it back via `Reader.metadata`:

```python
r = mef3io.Reader("session.mefd", password="l2pw")
r.metadata.subject.id                     # "MRN-123"
r.metadata.acquisition.session_description
r.metadata.to_dict()                      # nested plain dict
```

Subject fields (names, id, location) live in the **level-2** encrypted
section, so they read back empty with only a level-1 password; the
acquisition descriptions and filter settings are level-1. Metadata is
session-wide (written to every channel); `channel_description` defaults to the
channel name. See the [API reference](api/python.md#metadata-objects) for
every field.

## Legacy drop-in (`mef_tools` compatibility)

```python
# from mef_tools.io import MefReader, MefWriter
from mef3io import MefReader, MefWriter
```

Same call shapes, defaults, and return shapes as `mef_tools.io`:
`write_data` (NaN-gap splitting, precision inference, integer arrays take the
int32 primitive path, in-segment appends), `write_annotations`,
`get_data` / `get_raw_data` / `get_property` / `get_channel_info` /
`get_annotations`, plus the legacy writer knobs (`mef_block_len`,
`get_mefblock_len`, `max_nans_written`, `record_offset`). Differences from
the legacy stack are measured and documented in the
[comparison](legacy_comparison.md).

Metadata works both the legacy and the modern way on `MefWriter`: mutate the
`section3_dict` / `section2_ts_dict` (e.g.
`w.section3_dict['subject_ID'] = 'Smith'`) as in mef_tools, or pass a
`mef3io.Metadata` via `metadata=` / `set_metadata`.

## Parallelism

RED block decode/encode run on an internal thread pool (`n_threads`: 0 = all
cores, 1 = serial; per-call override on reads). Output is byte-identical
regardless of thread count, and the bindings release the GIL, so external
dataloader parallelism is safe.

Opening a session releases the GIL too: `Reader(...)` (and the writer's
reopen of an existing session) scans the tree and parses every segment's
metadata in C++ while other Python threads keep running. So a GUI can open a
large session on a worker thread without freezing its event loop, and several
sessions can be opened at once on different threads. One `Reader` can also
be shared across threads: reads, `toc`, `segments` and `records` may run
concurrently on it. `toc`, `segments`, `records` and
`Writer.write_annotations` release the GIL as well. The Python-level parts
of an open (the warm-start cache, building the open-time warnings) are short
and run with the GIL as usual.
