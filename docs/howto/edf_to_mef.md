---
description: "Convert an EDF, EDF+ or BDF recording to MEF 3.0 with mef3io and pyedflib - sampling rates, units, time zones, annotations, gaps and patient fields, with a check that the result matches the original."
---

# Convert EDF, EDF+ or BDF to MEF 3.0

```bash
pip install "mef3io[edf]"     # mef3io plus pyedflib, the EDF reader
python 12_edf_to_mef.py recording.edf recording.mefd --timezone Europe/Prague
```

The script is
[`examples/12_edf_to_mef.py`](https://github.com/bnelair/mef3io/blob/main/examples/12_edf_to_mef.py).
Run it without arguments to convert a small demo file first. It prints each
channel and ends with `verified` once it has read the new session back and
compared it with the EDF file:

```text
EEG Fp1: 256 Hz, uV, stored to 0.01 uV
ECG: 64 Hz, mV, stored to 0.0001 mV
2 annotation(s); verified /tmp/.../demo.mefd
```

From your own code, save the script beside it as `edf2mef.py`, then:

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
  is applied for the recording date. On Windows this needs
  `pip install tzdata`.
- A fixed offset, such as `+01:00`, or `UTC`. Use this when you know the
  offset the recording system used. A start time that falls in the hour
  skipped or repeated by a daylight-saving change is refused with a request
  for one.

The offset is also stored in the session (`Subject.gmt_offset`), so a viewer
can show local clock times again.

## Channels, sampling rates and units

Each EDF signal becomes one MEF channel with **its own sampling rate**: a
256 Hz EEG channel and a 64 Hz ECG channel stay 256 Hz and 64 Hz. Nothing is
resampled. The EDF label becomes the channel name, with characters that are
not safe in a folder name replaced by `_` (`EEG Fp1/Ref` becomes
`EEG Fp1_Ref`). The physical dimension (`uV`, `mV`) becomes the channel's
units. The transducer and prefilter text go into the channel description.

## Values: how precise the copy is

EDF stores whole numbers per sample plus a linear scale to physical units.
MEF 3.0 also stores whole numbers ("counts") with a scale, but the scale is a
power of ten. The script picks, per channel, the number of decimal places so
that one MEF count is **no larger than one EDF step**. For the demo EEG
channel, an EDF step is about 0.098 uV and the MEF count is 0.01 uV.

Every sample therefore comes back within half a MEF count of the EDF value,
and the final check asserts exactly that. The script lowers the precision if
a channel's values would not fit the 32-bit counts MEF uses. More on the
format's scaling: [Python guide, writing](../python.md#writing).

## Annotations

EDF+ and BDF+ annotations become session-level MEF records at the same
absolute time. An annotation with a duration becomes an `EDFA` record that
keeps it; one without becomes a `Note`. Read them with `Reader.records()`.

## Discontinuous files (EDF+D) become gaps

In an EDF+D file each data record carries its own start time, and time can
jump between records, for example when acquisition was paused. The script
writes each unbroken run of records at its real time. The time between runs
is stored as a gap and reads back as `NaN`, at the right place, as for any
[recording with gaps](long_recordings.md).

pyedflib refuses EDF+D and BDF+D files, so for those the script reads the
file itself (`read_discontinuous`): the EDF header has a fixed layout and the
record times are the first annotation of each record.

## Patient identity is not copied

An EDF header holds the patient name, code, birth date and sex in plain text.
The script **does not copy them** unless you pass `--copy-patient`, which
copies the name and code into the subject fields. In a session without
passwords those fields are readable by anyone with the files; to protect
them, add `password1=` and `password2=` to the `Writer` calls
([what encryption protects](../encryption_model.md#what-encryption-protects)).

Leaving the name out does not make a recording anonymous: the start date,
annotation text and file names can identify a person too. See
[De-identification](../anonymization.md).

## Checking the result

`verify()` reopens the session and checks, per channel, that every sample is
within half a MEF count of the EDF value, that gaps are exactly where the
EDF+D record times put them, and that every annotation was written. You can
also look at the session without reading any samples:

```bash
python -m mef3io inspect recording.mefd
```

## Why pyedflib

The script reads EDF with [pyedflib](https://github.com/holgern/pyedflib), a
small, maintained wrapper around the EDFlib C library. It is installed only
with the `[edf]` extra, never by `pip install mef3io`. It reads EDF, EDF+ and
BDF, keeps each channel at its own sampling rate, and gives the stored whole
numbers, so the conversion controls the scaling itself.
[MNE-Python](https://mne.tools) also reads EDF and is the better choice for
analysis, but it is a much larger install and brings channels with different
sampling rates to one common rate, which a lossless conversion must not do.

## Related

- [Read MEF 3.0 in Python](read_python.md) and
  [in MATLAB](read_matlab.md)
- [Examples](../examples.md): the other runnable scripts
