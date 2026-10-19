---
description: "How to make a modified copy of a MEF 3.0 session with transform_session: a choice of channels, new passwords, a new name and edited details, with every time and sample copied as stored and the copy verified against its source."
---

# Modified copies with `transform_session`

*Python and C++. Not available in MATLAB.*

`transform_session` writes a **new session** from an existing one, with
only the changes you ask for:

- **channels**: keep a few, or leave some out;
- **passwords**: none, or a new pair. Passwords are only ever set on a
  copy; mef3io never changes them in place;
- **the session name**: the copy is named after its folder;
- **descriptive details**: the subject's names and ID, the recording
  location, the session description, and each channel's description and
  reference.

Everything else is copied exactly as stored, and the original is never
changed. To share a recording without the patient's identity and dates, use
[`anonymize_session`](anonymization.md) instead. It is the same copy engine
with stricter rules.

```python
import mef3io

rep = mef3io.transform_session(
    "patient.mefd", "patient_eeg.mefd",
    exclude_channels=["ECG", "EMG"],
    metadata={
        "subject_id": "S-017",
        "channel_descriptions": {"Fp1": "frontal pole, left"},
    },
)
print(rep.summary())
```

From the command line:

```bash
python -m mef3io transform patient.mefd patient_eeg.mefd \
    --exclude-channel ECG --exclude-channel EMG \
    --set subject_id=S-017 --channel-description "Fp1=frontal pole, left"
```

## What stays exactly as stored

- **Every time.** That covers the universal headers of every file, the block
  index, the data blocks, the annotations at all three levels with the times
  inside their bodies, the recording-time offset and the time-zone fields.
  No date is moved or revealed, and a session that uses meflib's
  recording-time offset keeps it.
- **Every sample.** Data blocks are copied without being decoded or
  re-encoded. Only the 16 encrypted statistics bytes of an encrypted block
  are re-keyed for the copy's passwords, or decrypted when the copy has none.
- **The regions other programs fill** in MEF headers (meflib's protected and
  discretionary regions). They are another program's data, so mef3io keeps
  them. `anonymize_session` clears them.
- **Annotation bodies, byte for byte**, including types mef3io cannot parse.
  The one exception: a `Seiz` record's entries for channels left out of the
  copy are removed.

## What changes

- **Only what you asked for:**
  - the channel selection;
  - the passwords;
  - the session name, in every header and in the session's own annotation
    file (`<name>.rdat`);
  - the `metadata` fields you set.
- **File identity:**
  - every file gets a new file UUID;
  - level UUIDs are remapped consistently, because a copy is a new session;
  - each file's `provenance_UUID` names the source file it came from.
- **Provenance:** each file's [provenance region](mef3_format.md) records the
  transform (operation 8, `transform`), unless the region belongs to another
  program, in which case it is left untouched.

## Metadata edits

`metadata=` is a mapping of the fields to set. Fields you leave out are
copied as stored; `""` clears a field.

| Key | Where | Holds (UTF-8 bytes) |
|---|---|---|
| `subject_name_1`, `subject_name_2`, `subject_id` | section 3 | 127 |
| `recording_location` | section 3 | 511 |
| `session_description` | section 2, every channel | 2047 |
| `channel_descriptions` | section 2, `{channel: text}` | 2047 each |
| `reference_descriptions` | section 2, `{channel: text}` | 2047 each |

- **A value that does not fit is refused, never cut.**
- **A per-channel entry must name a channel of the copy.**
- **How edits are written:** each one goes into the stored image of every
  segment's metadata, without re-serializing it, so fields mef3io does not
  model are kept.
- **Changing the subject fields** needs the level-2 password on an encrypted
  source, as every copy does.

## How the copy is checked

The copy is built in `<dst>.partial/`. It is moved to `dst` only after
`mef3io.transform.verify` passes. A refusal, a failure or a cancel leaves
nothing behind. The verification reads the source and the copy and checks
that:

- the validator reports nothing on the copy that the source did not already
  have;
- every data block's payload and statistics (compared decrypted) are
  byte-identical;
- every time equals the source's;
- every other byte of every header, metadata section and annotation body
  equals the source's. The allowed exceptions are the names (when renamed),
  the UUIDs, the password fields, the encryption flags, the provenance
  region, the CRCs and the edited fields, which must hold exactly the edit.

Call `mef3io.transform.verify(src, dst, metadata=..., output_password=...)`
to re-check a copy later. `mef3io.transform.check(src, dst, ...)` runs every
check the copy makes before writing and returns all the refusals, without
writing anything.

## Refusal codes

Refusals work as they do for `anonymize_session`:
- each has a stable code, `exc.code` on the exception and `Refusal.code` in a
  `check` report, plus `exc.details`;
- `mef3io.transform.REFUSAL_CODES` holds them all.

**Codes shared with `anonymize_session`.** Most codes are shared and are
documented in [the shared table](anonymization.md#refusal-codes): paths,
passwords, channel selection, a damaged source, the copy pass. A transform
never raises the anonymization-only ones:
- the shift codes;
- the identity checks of the new names;
- the recording-time-offset codes (a transform copies every time as stored);
- `text-*`, `clear-unknown` and `id-number-time`.

**Codes only a transform raises:**

| Code | Raised as | Where | Meaning |
|---|---|---|---|
| `metadata-unknown` | `ValueError` | check | `metadata=` names a field that is not in the table above. |
| `metadata-too-long` | `ValueError` | check | A value does not fit its field (`details["field"]`); it is never cut. |
| `metadata-invalid` | `ValueError` | check | A value contains a NUL character. |
| `metadata-channel-unknown` | `ValueError` | check | A per-channel entry names a channel that is not in the copy (`details["channel"]`). |
