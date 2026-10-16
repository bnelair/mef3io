---
description: "Read a MEF 3.0 recording in Python with mef3io - a whole channel, a time window, gaps as NaN, encrypted sessions and single-file .mefd.tar archives."
---

# Read MEF 3.0 in Python

```python
import mef3io

with mef3io.Reader("recording.mefd") as r:
    print(r.channels)                  # channel names
    x = r.read(r.channels[0])          # whole channel, numpy float64, gaps are NaN
    fs = r.info(r.channels[0])["sampling_frequency"]
```

Install with `pip install mef3io`. `x` holds the values in physical units
(`r.info(ch)["units_description"]`, for example `uV`), one per sample period
from the channel's first sample to its last.

## A time window

MEF 3.0 times are **microseconds since 1970-01-01 UTC** ("uUTC"). Ask for a
window `[t0, t1)`; only the data needed for it is read from disk, so this is
fast even on a recording of many gigabytes.

```python
from datetime import datetime, timezone

def uutc(when: datetime) -> int:
    return round(when.timestamp() * 1e6)

with mef3io.Reader("recording.mefd") as r:
    start = r.info("Fp1")["start_time"]                 # first sample, uUTC
    first_minute = r.read("Fp1", start, start + 60_000_000)
    t0 = uutc(datetime(2024, 3, 5, 9, 30, tzinfo=timezone.utc))
    ten_seconds = r.read("Fp1", t0, t0 + 10_000_000)
```

A window returns `round((t1 - t0) * fs / 1e6)` samples. To turn a sample
index `i` back into a time: `t0 + i * 1e6 / fs`.

## Gaps are NaN

Where the recording has no data (an electrode was off, acquisition paused),
`read` returns `NaN` at those positions instead of shifting later samples
earlier. Times stay correct.

```python
import numpy as np

missing = np.isnan(x)
print(f"{missing.mean():.1%} of the span has no data")
clean = x[~missing]                    # only the recorded samples
```

To find where the gaps are without reading samples, see
[Long recordings with gaps](long_recordings.md).

## Annotations

```python
r.records()            # session-level annotations: type, time (uUTC), text, ...
r.records("Fp1")       # one channel's annotations
```

## Encrypted sessions

```python
with mef3io.Reader("recording.mefd", password="your password") as r:
    ...
```

The level-2 password opens everything. The level-1 password reads the
signals but not the subject fields or the annotations. See
[what encryption protects](../encryption_model.md#what-encryption-protects).

## Single-file archives (`.mefd.tar`)

A session packed into one `.mefd.tar` file is read the same way, in place,
without unpacking:

```python
with mef3io.Reader("recording.mefd.tar") as r:
    x = r.read("Fp1")
```

## Code written for mef_tools

```python
from mef3io import MefReader             # was: from mef_tools.io import MefReader
```

## More

The [Python guide](../python.md) covers every option: the stored integer
values (`read_raw`), segment maps, lenient opening of damaged sessions,
cancelling long reads, and writing.
