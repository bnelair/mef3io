---
description: "A plain introduction to MEF 3.0 (Multiscale Electrophysiology Format) for students and new researchers - what it stores, when to use it, and how to read a recording with mef3io in Python or MATLAB."
---

# MEF 3.0 for students

You have been handed a folder ending in `.mefd`, or you need to store a long
recording. This page explains what MEF 3.0 is and when it is a good choice.

## What it is

**MEF 3.0 (Multiscale Electrophysiology Format)** stores signals sampled at
a regular rate. It was built for long clinical recordings, mainly EEG,
intracranial EEG (iEEG) and ECG, but nothing in it is specific to the brain
or heart: EMG, other biosignals or sensor data fit just as well.

A recording is a **session**: a folder such as `recording.mefd` with one
folder per channel (`ch1.timd`), each split into **segments**
(`ch1-000000.segd`). A
segment holds three files: the metadata (`.tmet`: sampling rate, units,
subject), a small index (`.tidx`) and the compressed samples (`.tdat`).
Annotations, such as notes or seizure markings, sit in record files beside
them. The [format reference](mef3_format.md) has every byte.

## When it is a good fit

- **Recordings far larger than memory.** Samples are stored in small
  compressed blocks with an index, so a program can read ten seconds from
  the middle of a week-long recording without loading the rest.
- **Recordings with gaps.** A disconnected electrode or a paused acquisition
  is stored as missing data, and reads back as `NaN` at the right time,
  rather than being silently stitched together.
- **Recordings that keep growing.** New data can be appended for days or
  months; see [Long recordings](long_recordings.md).
- **Sharing with other MEF users.** Sessions written by mef3io open in the
  established tools (meflib, pymef, mef_tools), and theirs open in mef3io.
- **Separating identity from data.** Two passwords can protect the subject's
  identity and the technical metadata. They do not hide the samples
  themselves; see [what encryption protects](encryption_model.md#what-encryption-protects).
- **Sharing without identifying the patient.** To give a recording to
  someone outside your study, make a de-identified copy with
  `mef3io.anonymize_session`: every date is moved, names and other
  identifying text are removed, and the signal stays the same. See
  [De-identification](anonymization.md).

It is a poor fit for signals without a fixed sampling rate (event streams,
irregular measurements), and for video, which mef3io does not handle.

## A first read

Times in MEF are **microseconds since 1 January 1970, UTC** ("uUTC").

```python
import mef3io

with mef3io.Reader("recording.mefd") as r:   # password="..." if encrypted
    print(r.channels)
    info = r.info(r.channels[0])               # sampling rate, start, end, ...
    t0 = info["start_time"]
    x = r.read(r.channels[0], t0, t0 + 10_000_000)   # first 10 s, gaps = NaN
```

To see what a session holds before reading anything (channels, gaps,
annotations, whether it is encrypted):

```bash
python -m mef3io inspect recording.mefd
```

In MATLAB:

```matlab
r = mef3io.Reader('recording.mefd');
ch = r.channels{1};
meta = r.info(ch);
x = r.read(ch, meta.start_time, meta.start_time + 10e6);   % first 10 s
```

Next: the [Python](python.md) or [MATLAB](matlab.md) guide, and the
[examples](examples.md).
