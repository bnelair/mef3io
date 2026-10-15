# The MEF 3.0 on-disk format

A practical reference for the parts of MEF 3.0 that mef3io reads and writes
(time series + records; video is out of scope). Offsets below are what the
code actually uses (`core/include/mef3io/types.hpp`, `core/src/headers.cpp`)
and are cross-validated against meflib/pymef. All integers are
**little-endian**; type names follow meflib (`si8` = int64, `ui4` = uint32,
`sf8` = float64, …). Strings live in fixed-size, null-padded UTF-8 fields.

## Directory tree

A "file" in MEF is really a directory tree; the suffix of each directory says
what it is:

```
session.mefd/                     # session
├── session.rdat / session.ridx   #   optional session-level records
└── ch1.timd/                     # one time-series channel
    ├── ch1.rdat / ch1.ridx       #   optional channel-level records
    ├── ch1-000000.segd/          # segment 0
    │   ├── ch1-000000.tmet       #   metadata     (fixed 16 384 B)
    │   ├── ch1-000000.tidx       #   block index  (1024 B UH + 56 B/block)
    │   ├── ch1-000000.tdat       #   compressed data (1024 B UH + RED blocks)
    │   └── ch1-000000.rdat/.ridx #   optional segment-level records
    └── ch1-000001.segd/          # segment 1, ...
```

A channel is split into **segments** (numbered directories); each segment is
a self-contained triple of metadata / index / data files. Appending more data
either extends the last segment's three files in place or starts a new
segment. Use `Reader.segments(channel)` to see which segment covers which
time and sample range, and `Reader.toc(channel)` for the per-block view.

## Times

All times are **uUTC**: microseconds since the Unix epoch, `si8`. Two format
quirks matter:

- **Times are stored negated.** meflib marks "recording-time-offset applied"
  by storing `rto - t` (a negative number). Readers recover the true time as
  `t >= 0 ? t : -t + rto`, where `rto` (the recording time offset) lives in
  metadata section 3. This applies to universal-header start/end times, block
  start times, and record header times alike (record body times: see
  [Records](#records-rdat-ridx-annotations)). The rule is implemented once,
  in `core/include/mef3io/time.hpp`.
- **What mef3io writes.** Times are UTC, always; reading returns UTC.
  - **rto = 0** (every session mef3io creates): times are stored as `−t`, the
    form shown above. The bytes are unchanged from earlier releases.
  - **rto ≠ 0** (only in a foreign session that used meflib's anonymization,
    which mef3io is extending): times are stored as `rto − t`, meflib's own
    offset-applied form. This covers data blocks, index, universal headers and
    records alike. mef3io never writes a date there in plain, and every
    reader places the time correctly. (The date is still in the file: the
    offset itself sits in section 3, see below.)
  - **New segments and channels** added to a session whose segments all agree
    on a non-zero `rto` carry that same `rto` in section 3, and their times
    follow the same rule. The session keeps agreeing, so its existing
    records stay readable.
  - **Refused writes.** These raise an error and leave the files untouched,
    rather than store something that would misplace times or break the
    session's `rto − t` form:
    - **A time at or before a non-zero `rto`** (`WriteConflictError`). `rto − t`
      would be non-negative, and every reader takes a non-negative time as
      the absolute date (meflib.c:7665). So the only correct encoding would be
      the absolute date, outside the session's own `rto − t` form; mef3io
      refuses rather than mix the two.
    - **A write to a level whose segments disagree on `rto`**
      (`WriteConflictError`, naming the segments). There is no single offset
      to store against.
    - **A write that needs the offset while it is locked**: section 3 of an
      L2-encrypted session opened with only the level-1 password
      (`PasswordError`; open the writer with the level-2 password). A locked
      offset is unknown, never assumed to be 0.
  - **Unreadable metadata.** Section 3 has no CRC of its own, so mef3io
    trusts a segment's offset only after the same checks the full loader
    makes: the header CRC, and the body CRC over the fixed 16 384-byte record
    (never to EOF). Without them a damaged, e.g. zeroed, offset would read as
    0, "no offset", and a write would store the real date. A `.tmet` that
    fails either check, or is truncated, is unreadable. That is always
    reported (`Writer.unreadable_metadata`, plus a warning). What happens next
    depends on the readable segments:
    - **They agree on rto 0:** the damaged file is skipped, so one damaged
      segment in an ordinary session does not block unrelated writes.
    - **They agree on a non-zero rto:** the write is refused, because the
      agreement is unverified.
    - **None are readable:** the write is refused, because the offset is
      unknown.
  - **A channel with no segment yet** takes the session's offset for its
    records, under the same rules.
  - **A refused write changes nothing.** Every check (record type, time,
    offset) runs before any directory or file is created. A segment
    directory without metadata left by an earlier crash is ignored (and
    reported) instead of being adopted. Segments are numbered by the highest
    real segment number, so such a directory never misplaces an append or a
    new segment.
- The empty value ("no entry") for a time is `INT64_MIN`.

**The recording-time offset as "anonymization" is deprecated by mef3io.**
This is not a format change: MEF 3.0 still defines the field, and mef3io
reads and extends offset sessions exactly as described above. But the
offset only moves the date into `recording_time_offset` in section 3 of
every `.tmet`, which is plain text unless the session is encrypted, so
`−stored + rto` gives the real date back to anyone. mef3io therefore never
creates an offset session (a new session gets `rto = 0`), and
de-identification is planned as a separate copy
([#70](https://github.com/bnelair/mef3io/issues/70)). See
[De-identification and the recording-time offset](anonymization.md).

## Universal header — 1024 B, prefixes every file

Every `.tmet/.tidx/.tdat/.rdat/.ridx` file starts with the same 1024-byte
header:

| Offset | Type | Field |
|---|---|---|
| 0 | ui4 | header CRC (over bytes 4..1023) |
| 4 | ui4 | body CRC (over bytes 1024..end of file) |
| 8 | char[5] | file type string (`tmet`, `tidx`, `tdat`, `rdat`, `ridx`) |
| 13 / 14 | ui1 | MEF version major / minor (3 / 0) |
| 15 | ui1 | byte order (1 = little-endian) |
| 16 / 24 | si8 | start / end time (negated uUTC, see above) |
| 32 | si8 | number of entries (blocks / index entries / records) |
| 40 | si8 | maximum entry size (bytes) |
| 48 | si4 | segment number |
| 52 / 308 / 564 | char[256] | channel name / session name / anonymized name |
| 820 / 836 / 852 | ui1[16] | level UUID / file UUID / provenance UUID |
| 868 / 884 | ui1[16] | level-1 / level-2 password validation fields |
| 900 / 960 | ui1[60] / ui1[64] | protected / discretionary regions |

CRCs use CRC-32 with the Koopman polynomial (0x741B8CD7, reflected, start
value 0xFFFFFFFF). The password validation fields hold the two-level key
material (see [encryption_model.md](encryption_model.md)); all-zero fields
mean the file is unencrypted.

### Provenance region (mef3io ≥ 1.2) — frozen format

The universal header's 64-byte **discretionary region** (file offset 960) is
set aside for the writing application. meflib never interprets it, and pymef
only reports it as raw bytes. mef3io uses it, in every file it creates or
modifies, to record **which version created the file, which version last
modified it, and what was done to it**.

**No times are recorded.** The universal header is never encrypted, so a
write time would reveal when a recording was made. (`recording_time_offset`
was meant to hide that, but does not: see
[De-identification](anonymization.md).)

| Offset | Bytes | Type | Field |
|---|---|---|---|
| 0 | 4 | ASCII | magic `M3IO` |
| 4 | 1 | ui1 | layout version, currently `1` |
| 5 | 1 | ui1 | last operation (code, below) |
| 6 | 2 | — | reserved, zero |
| 8 | 20 | ASCII | created-by version, e.g. `1.2.0` |
| 28 | 20 | ASCII | last-modified-by version |
| 48 | 4 | ui4 LE | operations-ever mask: bit *n* set = code *n* was applied at some point |
| 52 | 4 | ui4 LE | modification count: operations after creation |
| 56 | 8 | — | reserved, zero |

Offsets are relative to the region; add 960 for the position in the file.

**Operation codes**

| Code | Name | Meaning |
|---|---|---|
| 0 | `unset` | — |
| 1 | `create` | file written fresh |
| 2 | `append` | samples added to an existing segment |
| 3 | `header-repair` | `repair_session`: declarations rewritten, samples untouched |
| 4 | `recovery` | `recover_session`: index and data reconciled after an interrupted write |
| 5 | `metadata-update` | reserved: descriptive metadata rewritten in place |
| 6 | — | reserved for anonymize (#70), not yet assigned |
| 7 | — | reserved for rename (#72), not yet assigned |
| 8 | — | reserved for transform (#73), not yet assigned |
| 9 | `segment-repaired` | stamped on a channel's **last** segment's `.tmet` when `repair_session` or `recover_session` modified **another** segment of that channel. The last segment itself was not repaired: that file only records that something earlier in its channel was. |

`segment-repaired` exists so that one file per channel, the last segment's
`.tmet`, shows that the channel changed, without anyone reading every segment.
mef3io's warm-start snapshot relies on it. The stamp is header-only: just this
region and the header CRC change. It is backed up like the repair's own writes.
It is skipped, and reported as a note rather than an error, in three cases:
the last segment's region belongs to another application, the last segment's
CRCs do not verify, or a writer's `.mef3io-tmp` temp file sits in the last
segment. The repair or recovery itself always proceeds.

**Rules. These are the compatibility contract and do not change.**

1. **The layout is frozen.** Offsets, widths, byte order and encodings never
   change. A later layout version may only *assign* the reserved bytes
   (6–7, 56–63); it never redefines an existing field.
2. **Codes are append-only.** A code is never renumbered, reused or
   redefined. Codes stay below 32 so that each has a bit in the mask. A
   reader that meets an unknown code reports it as unknown and does not fail.
3. **Version strings** are ASCII, NUL-padded, at most 19 characters plus a
   NUL terminator. A longer version is truncated, and any non-printable or
   non-ASCII byte is written as `?`. A string can never extend past its field.
4. **The count saturates.** It stops at `0xFFFFFFFF` and never wraps back to
   zero, so even an acquisition appended to for months cannot overflow it or
   the fields next to it.
5. **The mask only grows.** A bit, once set, is never cleared, so a later
   append cannot hide that a file was repaired or recovered.
6. **Ownership.** A region of all zeros, as written by meflib, pymef,
   mef_tools and mef3io ≤ 1.1, has never been stamped. The first mef3io
   modification initialises it but leaves created-by **empty**, so mef3io never
   claims a file it did not create. A region holding anything else without the
   magic belongs to another application and is **never written**.
7. **Unknown bytes are preserved.** A writer updates only the fields above:
   last operation, last-modified-by, mask and count. Every other byte is
   copied through, and a newer layout-version byte is never downgraded.
8. `archive_session` / `extract_session` are byte-exact copies, not
   modifications, and never change the region.

The region lies under the universal header's CRC (bytes 4–1023), which every
mef3io write recomputes. `Reader.segments(ch)` reports the provenance of each
segment's `.tmet` (`None` when there is none). The C ABI provides
`mef3io_reader_segment_provenance` and `mef3io_operation_name`.

The layout is pinned by a golden-bytes test and the codes by a literal table
(`core/tests/test_core.cpp`, "provenance region"); behaviour on real files is
covered by `tests/test_p21_writer_stamp.py`.

## Metadata file (`.tmet`) — exactly 16 384 B

```
[    0..1023 ]  universal header
[ 1024..2559 ]  section 1 (1536 B)  — never encrypted
[ 2560..13311]  section 2 (10752 B) — encrypted with the LEVEL-1 key
[13312..16383]  section 3 (3072 B)  — encrypted with the LEVEL-2 key
```

**Section 1** (offsets relative to the section):

| Offset | Type | Field |
|---|---|---|
| 0 | si1 | section-2 encryption level (+1 encrypted / −1 decrypted-on-disk) |
| 1 | si1 | section-3 encryption level (+2 / −2) |

A *positive* level means the section bytes are AES-128-ECB ciphertext; a
negative level means the same content is stored decrypted. Unencrypted files
carry −1/−2 (bytes `0xFF`/`0xFE` — beware tools that read this byte as
unsigned).

**Section 2** (time-series flavor; the technical metadata):

| Offset | Type | Field |
|---|---|---|
| 0 / 2048 | char[2048] | channel / session description |
| 4096 | si8 | recording duration (µs, spans gaps) |
| 4104 | char[2048] | reference description |
| 6152 | si8 | acquisition channel number |
| 6160 | sf8 | sampling frequency (Hz) |
| 6168–6192 | sf8 ×4 | LFF / HFF / notch filter, AC line frequency |
| 6200 | sf8 | **units conversion factor** (physical units per count) |
| 6208 | char[128] | units description (e.g. `uV`) |
| 6336 / 6344 | sf8 | maximum / minimum native sample value |
| 6352 | si8 | start sample (channel-wide index of this segment's first sample) |
| 6360 | si8 | **number of samples** (stored samples; gaps are *not* counted) |
| 6368 | si8 | number of blocks |
| 6376 | si8 | maximum block bytes |
| 6384 / 6388 | ui4 | maximum block samples / maximum difference bytes |
| 6392 | si8 | block interval (µs) |
| 6400 | si8 | number of discontinuities |
| 6408–6424 | si8 ×3 | maximum contiguous blocks / block bytes / samples |

Note the two easy-to-confuse quantities: `number_of_samples` counts samples
physically stored (NaN gaps are skipped at write time), while the
start/end times and `recording_duration` span the gaps. A gridded read
(`Reader.read`) therefore usually returns *more* samples than
`number_of_samples`, with NaN filling the gaps.

### The buffer-sizing declarations

The `maximum_*` block of fields at 6376–6424 is not descriptive trivia — it is the
allocation contract. meflib does not allocate from them itself, but it exposes
helpers that take one as a buffer size and validate none of them, and
applications built on it pass them in before decoding
anything — so a writer that under-declares hands that reader a buffer too small
for the data about to go into it.

`RED_allocate_processing_struct` skips the allocation entirely for a
`difference_buffer_size` of `0`, leaving `difference_buffer` NULL for
`RED_decode` to write through; meflib ships a guard for exactly this
(`RED_check_RPS_allocation`) but never calls it, so there is no error path at
all. Separately, `find_discontinuity_indices` (`meflib.c:3548`) `malloc`s
`number_of_discontinuities` entries and then writes one per flagged block — so
an under-declared discontinuity count is a heap overflow inside meflib itself.

**`0` is not the NO_ENTRY sentinel for any of them**, so a reader cannot tell
an unset field from a genuine measurement:

| Field | NO_ENTRY | mef3io writes |
|---|---|---|
| `maximum_block_bytes` | `-1` | largest encoded block in the segment |
| `maximum_block_samples` | `0xFFFFFFFF` | largest block sample count |
| `maximum_difference_bytes` | `0xFFFFFFFF` | largest `difference_bytes` over the RED block headers |
| `maximum_contiguous_blocks` | `-1` | longest run of blocks between discontinuities |
| `maximum_contiguous_block_bytes` | `-1` | encoded bytes in the longest such run |
| `maximum_contiguous_samples` | `-1` | samples in the longest such run |

A *contiguous run* is delimited by the `.tidx` discontinuity flag — the same
flag a reader uses — so the declarations and the data agree by construction.
Each maximum is tracked independently: over-declaring only costs a reader some
allocation, whereas under-declaring truncates its buffer.

Two asymmetries worth knowing. `maximum_difference_bytes` is the one field not
derivable from the index, since it lives in the RED block headers inside
`.tdat`; meflib's own worst case is `maximum_block_samples × 5`
(`RED_MAX_DIFFERENCE_BYTES`), which is a tight bound in practice. And the
legacy pymef writer sets `maximum_contiguous_block_bytes` to the whole `.tdat`
body and leaves `maximum_contiguous_samples` at `0`, both of which ignore
discontinuities; mef3io's values are per-run and exact.

mef3io's own reader never consults any of these fields — it sizes from each
block's own header — so sessions that declare zeros, sentinels, or nonsense
still read correctly.

**Section 3** (the sensitive, level-2 part):

| Offset | Type | Field |
|---|---|---|
| 0 | si8 | recording time offset (`rto`, used to de-negate times; holds the recording date, see [De-identification](anonymization.md)) |
| 8 / 16 | si8 | DST start / end time |
| 24 | si4 | GMT offset (seconds) |
| 28 / 156 | char[128] | subject name 1 / 2 |
| 284 | char[128] | subject ID |
| 412 | char[512] | recording location |

## Block index (`.tidx`) — 1024 B UH + one 56 B entry per RED block

| Offset | Type | Field |
|---|---|---|
| 0 | si8 | **file offset** of the block in the `.tdat` — *file*-relative, so the first block is at 1024, not 0 |
| 8 | si8 | block start time (negated uUTC) |
| 16 | si8 | start sample (channel-wide index) |
| 24 | ui4 | number of samples in the block |
| 28 | ui4 | block bytes (header + payload, padded) |
| 32 / 36 | si4 | maximum / minimum sample value in the block |
| 44 | ui1 | RED flags (bit 0 = discontinuity) |

The index is what makes windowed reads cheap: a reader can binary-search the
time range and fetch only the needed byte range from the `.tdat`.

## Data file (`.tdat`) — 1024 B UH + concatenated RED blocks

Samples are stored as int32 counts, compressed per block with **RED** (Range
Encoded Differences): difference coding followed by an adaptive range coder.
Each block starts with a 304-byte header:

| Offset | Type | Field |
|---|---|---|
| 0 | ui4 | block CRC |
| 4 | ui1 | flags (bit 0 = discontinuity; bits 1/2 = level-1/2 encrypted) |
| 16 / 20 | sf4 | detrend slope / intercept (unused when written lossless) |
| 24 | sf4 | scale factor (1.0 when lossless) |
| 28 | ui4 | difference bytes |
| 32 | ui4 | number of samples |
| 36 | ui4 | block bytes |
| 40 | si8 | block start time (negated uUTC) |
| 48 | ui1[256] | symbol statistics table for the range coder |

The compressed payload follows at offset 304; blocks are padded with `0x7e`
to an 8-byte boundary. A set discontinuity flag means "this block does not
continue seamlessly from the previous one" — that is how gaps (NaN runs in
the original signal) are represented; nothing is stored for the gap itself.
RED blocks are written **unencrypted** even in encrypted sessions (meflib
default): the passwords protect metadata and records, and without section 2 a
reader has no fs/ufact/counts to interpret the samples with.

To recover physical units: `value = stored_int32 * units_conversion_factor`.

## Records (`.rdat` + `.ridx`) — annotations

`.rdat` holds the records; `.ridx` is a parallel index. Both start with the
universal header. Record header (24 B) in `.rdat`:

| Offset | Type | Field |
|---|---|---|
| 0 | ui4 | record CRC |
| 4 | char[4] | type (`Note`, `EDFA`, `SyLg`, `Seiz`, …) |
| 9 / 10 | ui1 | version major / minor |
| 11 | si1 | encryption level of the body |
| 12 | ui4 | body bytes |
| 16 | si8 | record time (negated uUTC) |

The body follows, padded with `0x7e` to a 16-byte multiple (AES block size);
in encrypted sessions record bodies are **level-2 encrypted**. Each `.ridx`
entry (24 B): type[4] @ 0, version @ 5/6, encryption @ 7, file offset
(file-relative) @ 8, time @ 16.

**Damaged record files fail.** Rather than returning fewer records or
corrupted text, a record file is rejected for any of:
- a record cut off mid-way, or a header claiming more `body_bytes` than the
  file holds;
- a per-record CRC mismatch (the coverage meflib checks, meflib.c:1839; a CRC
  of 0, meflib's "no entry", is not checked);
- a universal header that declares **more** records than are present.

A file that declares fewer records than it holds is tolerated, as in meflib:
only the declared records are read (meflib.c:1869). A rejected file is
contained like any other: strict raises, lenient skips it and lists it in
`Reader.problems` with kind `"records"`.

### Where record files live

Record files can exist at **three** levels (meflib.c:6343 / 5660 / 6030), each
named after its directory:

| Level | Path |
|---|---|
| session | `session.mefd/session.rdat` (+ `.ridx`) |
| channel | `session.mefd/ch1.timd/ch1.rdat` |
| segment | `session.mefd/ch1.timd/ch1-000000.segd/ch1-000000.rdat` |

`Reader.records()` reads the session level; `Reader.records("ch1")` reads the
channel level AND every segment level of `ch1`; `records("ch1", segment=0)`
one segment; `records(level="all")` everything, including a channel that has
records but no segments yet. Every record carries `level`,
`channel` and `segment` so the source is never ambiguous. mef3io writes only
the session and channel levels.

The session-level file is `<session name>.rdat`, the only name meflib reads
(meflib.c:6335). The session name is the `.mefd` directory's name without
its suffix. For a `.mefd.tar` archive it is the session folder inside the
archive, so the archive file itself can be renamed freely. A differently named `.rdat` at the root is used only
when it is the only one there. With several and none matching the name, the
session-level records are ambiguous: strict raises, lenient reports it in
`problems` and returns none.

### Record times and the recording-time offset

The header time is negated like every other MEF time and is resolved with the
offset of what the file belongs to: a segment's own section 3; for a channel
or the session, the offset **all** of their segments agree on.

If those segments disagree, there is no correct single value. Only times
stored negated depend on the offset, so a file holding none is read normally.
One that does is refused with `FormatError` (strict), or skipped and listed in
`Reader.problems` with an `UnreadableSegmentWarning` (lenient). Either way only
that one file is affected: the segment-level files carry their own segment's
offset and still read (`records(ch, level="segment")` in strict mode).

This is a **deliberate divergence from meflib**. In its default
`RTO_REMOVE_ON_INPUT` mode (meflib.h:278) meflib resolves record times with
the global offset left by whichever `.tmet` it read last (meflib.c:1806 →
1846); the "no entry" it records for a disagreeing channel or session
(meflib.c:5529, 6208) is only the metadata aggregate. Following the read order
would misplace the records by the difference without a word.

**Writing.** Record times follow the rule in [Times](#times):
- **rto = 0 level:** stored as `−t`, byte-identical to earlier releases.
- **Non-zero rto:** stored as `rto − t`. A record at or before the offset is
  refused.
- **A level whose segments disagree, or whose offset is locked:** the write
  is refused.

meflib applies the offset to the record **header** only (meflib.c:1846);
`mefrec.c` never rewrites a body. It does, however, display every body time
through `remove_recording_time_offset` (via `local_date_time_string`,
meflib.c:4649), i.e. with the same sign rule. mef3io does the same: **body
times go through the same conversion as the header time.** A non-negative body
time — the normal case, even in an offset session — is returned unchanged; a
negative one is resolved with the record's offset. So every time mef3io
returns, header or body, is absolute uUTC. The sentinels `INT64_MIN` and a
Seiz channel's `-1` onset/offset are passed through as-is.

### Body layouts (mefrec.h)

Offsets are relative to the start of the body. **Every** body type is
version-gated: meflib decodes each one only at version 1.0 (mefrec.c:201, 232,
540, …), so mef3io reads a body only when the header says 1.0 — which is what
mef3io, pymef and mef_tools write. Another version, or a body shorter than the
fixed part of its layout (e.g. an EDFA body without its 8-byte duration),
reads back with every body field `None`. The field names are the keys
`Reader.records()` returns (and the MATLAB struct fields); every record of a
type always carries all of its type's keys.

| Type | Body (offset: type name) |
|---|---|
| `Note`, `SyLg` | 0: utf8 `text` (to the first NUL) |
| `EDFA` | 0: si8 `duration` (µs); 8: utf8 `text` |
| `Seiz` (1312 B + 272 B/channel) | 0: si8 `earliest_onset` (uUTC); 8: si8 `latest_offset` (uUTC); 16: si8 `duration`; 24: si4 `number_of_channels`; 28: si4 `onset_code` (−1 none, 0 unknown, 1 focal, 2 generalized, 3 propagated, 4 mixed); 32: utf8[128] `marker_name_1`; 160: utf8[128] `marker_name_2`; 288: utf8[1024] `annotation`; then `channels`: per channel 0: utf8[256] `name`, 256: si8 `onset`, 264: si8 `offset` (uUTC, −1 = none) |
| `Epoc` (192 B) | 0: si8 `id_number`; 8: si8 `timestamp` (uUTC); 16: si8 `end_timestamp` (uUTC); 24: si8 `duration`; 32: utf8[32] `epoch_type`; 64: utf8[128] `text` |
| `Curs` (160 B) | 0: si8 `id_number`; 8: si8 `trace_timestamp` (uUTC); 16: si8 `latency`; 24: sf8 `value`; 32: utf8[128] `name` |
| `CSti` (200 B) | 0: utf8[64] `task_type`; 64: si8 `stimulus_duration`; 72: utf8[64] `stimulus_type`; 136: utf8[64] `patient_response` |
| `ESti` (416 B) | 0: sf8 `amplitude`; 8: sf8 `frequency`; 16: si8 `pulse_width`; 24: si4 `ampunit_code` (−1 none, 0 unknown, 1 mA, 2 V); 28: si4 `mode_code` (−1 none, 0 unknown, 1 current, 2 voltage; mefrec.h:242-250); 32/160/288: utf8[128] `waveform` / `anode` / `catode` |
| `LNTP` | 0: si8 `length`; 8: si4[`length`] `template` (pymef: si8, see below) |
| `UnRc` | no body fields |

`Seiz` returns at most `number_of_channels` channel entries, and fewer when
the body is shorter than it declares. For `LNTP` mef3io follows the
`MEFREC_LNTP_1_0_TEMPLATE_OFFSET` define (body offset 8); meflib's own display
code (mefrec.c:306) applies that offset with `si4*` pointer arithmetic and so
reads 4× too far.

**LNTP incompatibility.** pymef (and so the legacy `mef_tools`) writes the
template as **si8** (pymef3_file.c:4159-4160; its reader notes the discrepancy
at :3729), not the si4 `mefrec.h` defines, and the header cannot say which.
mef3io reads si4 by default and takes the pymef layout only when the body
holds at least 8 × `length` bytes after the length AND every high 4 bytes are
the sign extension of the low 4 — which a si4 template followed by meflib's
`0x7e` padding never is. The values are returned as integers either way.
(A si4 file padded with zeros instead of `0x7e`, whose template happens to
alternate value / sign-extension, would be misread; no known writer does that.)

Reading these bodies is supported; writing remains limited to `Note`, `SyLg`
and `EDFA`.

## Sentinels and other conventions

| Meaning | Value |
|---|---|
| "no entry" time (si8) | `INT64_MIN` |
| "no entry" si8 / si4 / ui4 | −1 / −1 / `0xFFFFFFFF` |
| RED NaN sample | `INT32_MIN` |
| GMT offset "no entry" | −86401 |
| pad byte (blocks, record bodies) | `0x7e` |

## How this maps to the mef3io API

| On-disk concept | API |
|---|---|
| Section 2 + universal header | `Reader.info(ch)` (fs, ufact, times, counts) |
| Section 3 (subject metadata) | `Reader.info(ch)` `subject_*` fields; `None` without L2 access |
| Segment triples | `Reader.segments(ch)` — time/sample range per segment |
| `.tidx` entries | `Reader.toc(ch)` — per-block start time, extrema, discontinuity |
| RED blocks | decoded transparently by `read` / `read_raw` |
| Records (session / channel / segment level) | `Reader.records(ch, segment, level)` / `Writer.write_annotations(...)` |

Related: [encryption_model.md](encryption_model.md) (how the two password
levels derive keys and what they unlock),
[legacy_comparison.md](legacy_comparison.md) (measured differences vs
pymef/mef_tools), and [design.md](design.md) (mef3io internals).
