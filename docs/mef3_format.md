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
    │   └── ch1-000000.tdat       #   compressed data (1024 B UH + RED blocks)
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
  start times, and record times alike. mef3io writes `rto = 0`.
- The empty value ("no entry") for a time is `INT64_MIN`.

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

**Section 3** (the sensitive, level-2 part):

| Offset | Type | Field |
|---|---|---|
| 0 | si8 | recording time offset (`rto`, used to de-negate times) |
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
in encrypted sessions record bodies are **level-2 encrypted**. Body layouts:
`Note` = text; `EDFA` = si8 duration + text; `Seiz` = si8 earliest onset,
si8 latest offset, si8 duration. Each `.ridx` entry (24 B): type[4] @ 0,
version @ 5/6, encryption @ 7, file offset (file-relative) @ 8, time @ 16.

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
| Records | `Reader.records(ch)` / `Writer.write_annotations(...)` |

Related: [encryption_model.md](encryption_model.md) (how the two password
levels derive keys and what they unlock),
[legacy_comparison.md](legacy_comparison.md) (measured differences vs
pymef/mef_tools), and [design.md](design.md) (mef3io internals).
