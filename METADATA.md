# Session metadata

mef3io lets you attach **subject** and **acquisition** metadata to a session —
patient names, subject id, recording location, session/channel descriptions,
filter settings, and more — and read it back. This page documents every field,
where it lives on disk, how encryption gates it, and the API in Python,
MATLAB, and the legacy compatibility writer.

Metadata is **session-wide**: it is written into every channel and, on read,
reflects the session (taken from the first channel).

## The two groups

MEF 3.0 stores this metadata in two encrypted sections, and mef3io mirrors that
split with two objects:

| Object | MEF section | Encryption | Holds |
| --- | --- | --- | --- |
| `Subject` | section 3 | **level&nbsp;2** | subject identity + time zone |
| `Acquisition` | section 2 | **level&nbsp;1** | descriptions + acquisition/filter settings |

**Encryption gating matters on read:** subject fields are in the level-2
section, so with only a *level-1* password they come back empty (the signal
and acquisition metadata still read fine). With a level-2 password — or an
unencrypted session — everything is available. See the
[encryption model](https://bnelair.github.io/mef3io/encryption_model/).

## Fields

Every field has a default, so you set only what you have.

### Subject (section 3, level-2)

| Field | Type | Default | Meaning |
| --- | --- | --- | --- |
| `name_1` | str | `""` | subject name (e.g. family name) |
| `name_2` | str | `""` | subject name (e.g. given name) |
| `id` | str | `""` | subject identifier (MRN, study id, …) |
| `recording_location` | str | `""` | where the recording was made |
| `gmt_offset` | int | `0` | seconds east of UTC |

### Acquisition (section 2, level-1)

| Field | Type | Default | Meaning |
| --- | --- | --- | --- |
| `session_description` | str | session name | free-text session description |
| `channel_description` | str | channel name | free-text per-channel description |
| `reference_description` | str | `""` | reference/montage note |
| `acquisition_channel_number` | int | `1` | amplifier channel index |
| `low_frequency_filter` | float | `-1.0` | high-pass cutoff, Hz (`-1` = not recorded) |
| `high_frequency_filter` | float | `-1.0` | low-pass cutoff, Hz |
| `notch_filter` | float | `-1.0` | notch frequency, Hz |
| `line_frequency` | float | `-1.0` | mains frequency, Hz (e.g. 50 / 60) |

Not listed here are the *computed* section-2 fields — sample counts, extrema,
durations, block statistics — which the writer fills in automatically from the
data and which you should not set.

## Python

```python
from mef3io import Metadata, Subject, Acquisition

md = Metadata(
    subject=Subject(id="MRN-123", name_1="Doe", name_2="Jane",
                    recording_location="EMU-4", gmt_offset=-6 * 3600),
    acquisition=Acquisition(session_description="pre-surgical eval",
                            reference_description="Cz",
                            low_frequency_filter=0.1,
                            high_frequency_filter=100.0,
                            line_frequency=60.0),
)

with mef3io.Writer("session.mefd", overwrite=True, metadata=md) as w:
    w.write("ch1", data, start_uutc, fs=256.0)
```

Set it later (before writing) with `Writer.set_metadata(md)`, which also
accepts a flat dict. Read it back:

```python
r = mef3io.Reader("session.mefd", password="l2pw")
r.metadata                          # -> Metadata
r.metadata.subject.id               # "MRN-123"
r.metadata.acquisition.line_frequency
r.metadata.to_dict()                # nested plain dict
```

Helpers: `Metadata.to_dict()` / `Subject.to_dict()` / `Acquisition.to_dict()`,
and `Metadata.from_info(reader.info(channel))` to build one from an `info`
dict. All fields are also present on `Reader.info(channel)` directly.

## MATLAB

The MATLAB binding mirrors the Python objects one-to-one:

```matlab
md = mef3io.Metadata( ...
    subject=mef3io.Subject(id="MRN-123", name_1="Doe", recording_location="EMU-4"), ...
    acquisition=mef3io.Acquisition(session_description="pre-surgical eval", line_frequency=60));

w = mef3io.Writer("session.mefd", Overwrite=true, Metadata=md);
w.write("ch1", data, startUutc, 256);
delete(w);

r = mef3io.Reader("session.mefd", "l2pw");
r.metadata.subject.id                 % 'MRN-123'
r.metadata.acquisition.line_frequency
```

`setMetadata` and the `Metadata=` option also accept a plain struct with the
flat field names (`subject_id`, `session_description`, `line_frequency`, …).

## Legacy `mef_tools` compatibility

`mef3io.MefWriter` supports metadata two ways. The modern object:

```python
from mef3io import MefWriter, Metadata, Subject
w = MefWriter("session.mefd", overwrite=True,
              metadata=Metadata(subject=Subject(id="MRN-123")))
```

…or the mef_tools-style mutable section dicts (bytes or str values), so
existing legacy code keeps working with only the import changed:

```python
w = MefWriter("session.mefd", overwrite=True)
w.section3_dict["subject_ID"] = b"MRN-123"        # note legacy caps
w.section3_dict["subject_name_1"] = "Doe"
w.section2_ts_dict["session_description"] = b"pre-surgical"
w.section2_ts_dict["AC_line_frequency"] = 60.0
w.write_data(data, "ch1", start_uutc, 256.0, precision=3)
```

The legacy keys (`subject_ID`, `GMT_offset`, `low_frequency_filter_setting`,
`AC_line_frequency`, …) are mapped onto the mef3io fields and applied on write.

## Notes

- **Partial population is the norm** — fill in a couple of fields; the rest
  keep their defaults.
- **Session-wide** — the same metadata is written to every channel;
  `channel_description` defaults to each channel's name.
- **Round-trips through the oracle** — sessions written with mef3io metadata
  are read by pymef/mef_tools (and vice versa); subject/acquisition values
  match.
- **Set before writing** — `set_metadata` affects segments written after it;
  it applies to new segments, not to already-written data.

The dataclasses are also auto-documented in the
[Python API reference](https://bnelair.github.io/mef3io/api/python/#metadata-objects).
