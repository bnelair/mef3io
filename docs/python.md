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
  the [encryption model](encryption_model.md)).
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
`valid` mask (bool/uint8, same length): `False` marks gap samples.

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
    r.records("ch1")                              # annotations (None -> session level)
```

`Reader(path, password="", backend="cpp", n_threads=0, cache=None)`

- A **level-2 password** unlocks everything; a **level-1 password** reads the
  signal and technical metadata while section-3 subject metadata stays locked
  (`info()["section3_available"] == False`, subject fields `None`) and
  records are unavailable.

### `read(channel, t0=None, t1=None, n_threads=None)`

Float64 samples on the uniform fs grid over `[t0, t1)` (whole channel by
default): `N = round((t1 - t0) * fs / 1e6)` samples, discontinuity gaps
filled with NaN, values scaled by the conversion factor. Windowed reads
fetch only the needed bytes — cheap on huge sessions.

### `read_raw(channel, t0=None, t1=None, n_threads=None)`

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
`number_of_blocks`, `path` — from metadata only (nothing decoded), so it is
cheap even for huge, gap-riddled sessions. `toc` is the finer per-RED-block
view (start time, sample counts, extrema, discontinuity flags).

### Warm-start cache (opt-in)

```python
mef3io.Reader("session.mefd", cache="auto")           # per-user OS cache dir
mef3io.Reader("session.mefd", cache="/data/s.cache")  # persistent snapshot
```

Snapshots channel metadata so warm opens serve `channels`/`info` without
touching the session tree, deferring the backend until a data read.
Invalidated automatically on any size/mtime change and on write.

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

## Parallelism

RED block decode/encode run on an internal thread pool (`n_threads`: 0 = all
cores, 1 = serial; per-call override on reads). Output is byte-identical
regardless of thread count, and the bindings release the GIL, so external
dataloader parallelism is safe.
