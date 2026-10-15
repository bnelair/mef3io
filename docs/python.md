# Python guide

```python
import mef3io
```

Two API layers ship in the package:

- **`mef3io.Reader` / `mef3io.Writer`** — the native API (recommended).
- **`mef3io.MefReader` / `mef3io.MefWriter`** — a drop-in for legacy
  `mef_tools.io` code.

Times everywhere are **uUTC**: microseconds since the Unix epoch, as `int`.
Errors raise `RuntimeError` with the C++ message (e.g. wrong password, append
conflicts, CRC failures).

## Writing

```python
with mef3io.Writer("session.mefd", overwrite=True, units="uV") as w:
    w.write("ch1", data, start_uutc, fs=256.0)
```

`Writer(path, overwrite=False, password1="", password2="", units=None,
block_length=None, n_threads=0)`

- `overwrite=True` deletes an existing session first. With
  `overwrite=False`, an existing session is **reopened for appending** —
  state is recovered from disk, so this works across program runs.
- Encryption: pass **both** passwords (MEF has no "level-1 only" files; see
  the [encryption model](encryption_model.md)). It encrypts the subject
  fields, technical metadata and annotation text, **not the samples or their
  timing**, which decode without a password; see
  [What encryption protects](encryption_model.md#what-encryption-protects).
- **Appending to an encrypted session needs the level-2 password.** That
  includes sessions at offset 0. Section 3, which holds the recording-time
  offset every new time is stored against, is level-2 encrypted, and without
  it the offset is unknown. A Writer opened with only the level-1 password is
  refused (`PasswordError`) for anything that stores times into that
  session, rather than guessing the offset. Earlier releases took it as 0,
  which in a session with a recording-time offset placed appended data
  decades off.
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
`RuntimeError`.

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

`Reader(path, password="", backend="cpp", n_threads=0, cache=None)`

- A **level-2 password** unlocks everything; a **level-1 password** reads the
  signal and technical metadata while section-3 subject metadata stays locked
  (`info()["section3_available"] == False`, subject fields `None`) and
  records are unavailable. Without a password the Reader refuses an
  encrypted session; that is mef3io's policy, not protection, because the
  samples themselves are not encrypted
  ([details](encryption_model.md#what-encryption-protects)).

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
mef3io creates). In a foreign session "anonymized" with meflib's
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
| `Validator.validate`, `validate_session` | yes | bytes of `.tmet` + `.tidx` + `.tdat` covered |
| `repair_session` | yes (validation pass only) | as validate |
| `recover_session` | yes (scan only) | bytes of `.tidx` + `.tdat` covered |

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

**One read at a time per `Reader`.** A `Reader` must not be read from two
threads at once. When a newer request supersedes a read, cancel it and wait
for the cancelled call to return (it does so promptly) before reading again
on the same `Reader` — or give each thread its own `Reader`.

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
  never left half-repaired.

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

The MATLAB binding does not expose these yet; the C ABI does
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
