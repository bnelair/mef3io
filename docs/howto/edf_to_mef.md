---
description: "Convert an EDF, EDF+ or BDF recording to MEF 3.0 with mef3io and pyedflib - sampling rates, units, time zones, annotations, gaps and patient fields, with a check that the result matches the original."
---

# Convert EDF, EDF+ or BDF to MEF 3.0

Download
[`examples/12_edf_to_mef.py`](https://github.com/bnelair/mef3io/blob/main/examples/12_edf_to_mef.py)
from the mef3io repository (pip does not install the examples), then:

```bash
pip install "mef3io[edf]"     # mef3io plus pyedflib, which reads EDF
python 12_edf_to_mef.py recording.edf recording.mefd --timezone Europe/Prague
```

Run it without arguments to convert a small demo file first. It prints each
channel and ends with `verified` once it has read the new session back and
checked it against the values it reads from the EDF file:

```text
EEG Fp1: 256 Hz, uV, stored to 0.01 uV
ECG: 64 Hz, mV, stored to 0.0001 mV
2 annotation(s); verified /tmp/.../demo.mefd
```

From your own code, save the script next to your code as `edf2mef.py`, then:

```python
from edf2mef import convert, verify

result = convert("recording.edf", "recording.mefd", timezone="Europe/Prague")
verify("recording.mefd", result)
```

The script is short and meant to be read and adapted. The rest of this page
explains each choice it makes.

## The time zone: you must say it

An EDF file stores its start as a local clock time, such as
`05.03.24 10.20.30`, with no time zone. MEF 3.0 stores every time in UTC. The
same file recorded in Prague and in New York would be six hours apart, and
nothing in the file says which. So the script **refuses to guess**: pass
`--timezone` with the zone of the recording site.

- An IANA name, such as `Europe/Prague` or `America/Chicago`. Daylight saving
  is applied for the recording date. A start time in the hour skipped or
  repeated by a daylight-saving change is refused; pass a fixed offset
  instead. On Windows this needs `pip install tzdata`.
- A fixed offset, such as `+01:00`, or `UTC`. Use this when you know the
  offset the recording system used.

The offset is also stored in the session (`Subject.gmt_offset`), so a viewer
can show local clock times again.

## Channels, sampling rates and units

Each EDF signal becomes one MEF 3.0 channel with **its own sampling rate**: a
256 Hz EEG channel and a 64 Hz ECG channel stay 256 Hz and 64 Hz. Nothing is
resampled. The EDF label becomes the channel name, with characters that are
not safe in a folder name replaced by `_` (`EEG Fp1/Ref` becomes
`EEG Fp1_Ref`). The physical dimension (`uV`, `mV`) becomes the channel's
units. The transducer and prefilter text go into the channel description.

## Values: how precise the copy is

EDF stores whole numbers per sample plus a linear scale to physical units.
MEF 3.0 also stores whole numbers ("counts") with a scale, but the scale is a
power of ten. The script picks, per channel, the number of decimal places so
that one count is **no larger than one EDF step**. For the demo EEG channel,
an EDF step is about 0.098 uV and a count is 0.01 uV.

Every sample therefore comes back within half a count of the EDF value. The
counts are 32-bit numbers; if a channel's values are too large for that at
full resolution (rare: large values measured very finely), the script uses
fewer decimal places and prints a warning naming the channel. More on the
scaling: [Python guide, writing](../python.md#writing).

## Annotations

EDF+ and BDF+ annotations become session-level records at the same absolute
time. An annotation with a duration becomes an `EDFA` record that keeps it;
one without becomes a `Note`. Read them with `Reader.records()`.

## Discontinuous files (EDF+D) become gaps

In an EDF+D file each data record carries its own start time, and time can
jump between records, for example when acquisition was paused. The script
writes each unbroken run of records at its real time. The time between runs
is stored as a gap and reads back as `NaN`, at the right place, as for any
[recording with gaps](long_recordings.md).

pyedflib refuses EDF+D and BDF+D files, so for those the script reads the
file itself (`open_discontinuous`): the EDF header has a fixed layout and the
record times are the first annotation of each record.

## Large files

The script converts one channel at a time, in pieces of ten minutes
(`CHUNK_SECONDS`), so a recording of days needs no more memory than a short
one. The check at the end reads the same way. EDF stores all channels
interleaved, so the file is read once per channel (and again by the check);
it is fastest when the EDF file fits in the computer's memory cache.

Each piece is written as its own block of data, and MEF 3.0 marks the first
block of every write with a "possible break" flag. That flag appears every ten
minutes in `Reader.toc()` even in a continuous recording; the block times show
there is no gap ([more](long_recordings.md#finding-the-data-without-reading-it)).

## Patient identity is not copied

An EDF header holds the patient name, code, birth date and sex in plain text.
The script **does not copy them** unless you pass `--copy-patient`, which
copies the name and code of an EDF+ or BDF+ file into the subject fields.
(A plain EDF or BDF file has only a free-text patient field; it is not
copied, and the script says so.) In a session without passwords those fields
are readable by anyone with the files. To protect them, encrypt the session:
add `--encrypt`, and the script asks for the two passwords (they are not
typed on the command line, where others could see them). From Python, use
`convert(..., password1="...", password2="...")` and then
`verify(..., password="<the level-2 password>")`
([what encryption protects](../encryption_model.md#what-encryption-protects)).

Leaving the name out does not make a recording anonymous: the start date,
annotation text and file names can identify a person too. To share a
converted recording, make a de-identified copy with
`mef3io.anonymize_session`; see [De-identification](../anonymization.md).

## Checking the result

`verify()` reads the EDF file again and the new session, piece by piece, and
checks per channel that every sample is within half a count of the EDF value
at the same time, that each count is no coarser than the EDF step (unless a
warning said so), that no extra samples fill a gap, and that every annotation
was written with the same time, type, text and duration. It uses the
script's own EDF reading, so it confirms the conversion, not the EDF reader.
With mef3io 1.3 or later you can also look at the session without reading
any samples:

```bash
python -m mef3io inspect recording.mefd
```

## Why pyedflib

The script reads EDF with [pyedflib](https://github.com/holgern/pyedflib), a
small, maintained wrapper around the EDFlib C library. mef3io never installs
it by itself: `pip install "mef3io[edf]"` (or `pip install pyedflib`). It reads EDF, EDF+ and
BDF, keeps each channel at its own sampling rate, and gives the stored whole
numbers, so the conversion controls the scaling itself.
[MNE-Python](https://mne.tools) also reads EDF and is the better choice for
analysis, but it is a much larger install and brings channels with different
sampling rates to one common rate, which a lossless conversion must not do.

## Related

- [Read MEF 3.0 in Python](read_python.md) and
  [in MATLAB](read_matlab.md)
- [Examples](../examples.md): the other runnable scripts
