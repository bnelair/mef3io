---
description: "Work with long MEF 3.0 recordings that have gaps - how gaps are stored and read, how to find them without reading samples, and how to append new data over days or months with mef3io."
---

# Long recordings with gaps

```python
import mef3io

s = mef3io.inspect("recording.mefd", read_index=True)    # reads no samples
for c in s.channels:
    for start, end in c.gaps:                            # uUTC
        print(c.name, "no data for", (end - start) / 1e6, "s from", start)
```

Or from a terminal: `python -m mef3io inspect recording.mefd --index`.
`inspect` needs mef3io 1.3 or later; `Reader.segments()` and `Reader.toc()`
below work in every version.

## How gaps appear

A recording that runs for days rarely runs without a break: an electrode
comes off, the amplifier restarts, acquisition is paused overnight. MEF 3.0
stores only the samples that were recorded, each block with its own start
time, and marks where the time jumps. Nothing is stored for the missing span,
so a gap costs no disk space.

When you read, mef3io puts every sample at its own time and fills the
missing span with `NaN`:

```python
with mef3io.Reader("recording.mefd") as r:
    x = r.read("Fp1", t0, t1)        # NaN where nothing was recorded
```

A plain `NaN` and a gap are the same thing on write, too: `NaN` values you
pass to `Writer.write` are not stored and come back as a gap.

## Finding the data without reading it

- `mef3io.inspect(path, read_index=True)` lists every gap per channel, as
  above. It reads metadata and the small block indices, never the samples.
  Without `read_index=True` it is faster still but lists only the gaps
  between segments (`c.gaps_complete` is then `False`).
- `Reader.segments(channel)` gives each segment's start and end time and
  sample count, from metadata only: a quick map of where the data is.
- `Reader.toc(channel)` lists every compressed block with its start time
  (`start_uutc`), sample count and a `discontinuity` flag. The flag is set on
  the first block after a break, and also on the first block of each write,
  so compare the times to tell a real gap.

In MATLAB, `r.segments(ch)` and `r.toc(ch)` work the same way;
`mef3io.inspect` is in Python and C++ only, not MATLAB.

## Adding data for days or months

Keep one `Writer` open for the whole acquisition and write each new block at
its start time. Time that passes between blocks becomes a gap by itself.

```python
fs = 512.0
with mef3io.Writer("recording.mefd") as w:      # creates, or reopens and extends
    for block, block_start in acquisition():    # your source: (numpy array, uUTC) pairs
        w.write("Fp1", block, block_start, fs)
```

- Opening a `Writer` without `overwrite=True` reopens an existing session
  and appends to it, so a program can stop and continue the next day. One
  `Writer` kept open is faster, because it does not re-read the index on
  each block.
- With one `Writer` kept open, each append costs time in proportion to the
  **new** data only, however long the recording already is.
- New data must start after the channel's last sample, at the same sampling
  rate; otherwise the write is refused and nothing is changed.
- `new_segment=True` starts a new segment, for example once per day. That
  keeps each segment's files a manageable size, and a damaged file then
  affects only its own segment when read with `strict=False`
  ([Python guide](../python.md#reading)).
- If the program or the computer stops in the middle of a write, run
  `python -m mef3io recover recording.mefd` (it only reports; add `--apply`
  to fix). Data from earlier appends is never at risk.

## More

[Long recordings: days to months](../long_recordings.md) explains the append
path, the `durability` setting, recovery and the measured costs.
