---
description: "How to merge two or more MEF 3.0 recordings into one session with merge_sessions: recordings that follow each other, take turns, or hold different channels, checked for overlapping data before anything is written and verified against the sources."
---

# Merging recordings with `merge_sessions`

*Python and C++. Not available in MATLAB.*

`merge_sessions` writes **one new session** from two or more recordings, for
example:
- a monitoring stay that was recorded into several files;
- two amplifiers that recorded different channels of the same period.

Every time and every sample is copied as stored, and the sources are never
changed.

```python
import mef3io

rep = mef3io.merge_sessions(["stay_part1.mefd", "stay_part2.mefd"], "stay.mefd")
print(rep.summary())
```

From the command line:

```bash
python -m mef3io merge stay.mefd stay_part1.mefd stay_part2.mefd
```

## When a merge is refused

These checks run **before anything is written**. `mef3io.merge.check(...)`
lists every refusal without writing. What only the copy itself can find (a
damaged data block, or a block whose index and header disagree about its
time) stops the merge later and leaves nothing behind:

- **Overlapping data.** Two recordings hold samples of the same channel at
  the same moment. The check compares the **recorded data** (from each
  segment's block index), not just where each recording starts and ends.
  Recordings that take turns merge fine: A records and stops, B records and
  stops, then A continues. Overlaps up to half a sample are recorder
  rounding (times are stored in whole microseconds) and are allowed, and a
  recording's own slightly overlapping blocks never count. The message names the channel, the
  two recordings and the time.
- **Different sampling rates** for the same channel: a channel keeps one rate.
- **Different recording-time offsets** (meflib's date masking): times are
  copied as stored, so they must be read the same way.
- **Different patients?** The recordings' patient names, ID or location
  differ. Set them for the result with `metadata=` to merge anyway (or
  check that these really are the same patient's recordings).

Every check a [modified copy](transform.md) makes also applies to each
recording: passwords, damaged files, channel selection and so on.

## How the result is laid out

- **Segments:** each segment of each recording becomes a segment of the
  result. A segment whose gap holds another recording's data is split there,
  at a block boundary (nothing is decoded), so the result's segments never
  overlap in time. Segments are ordered by time and numbered from 0 in each
  channel.
- **Channels** held by only some recordings are kept; the result simply has
  no data for them where those recordings had none.
- **Annotations:** at the session and channel level they are merged in time
  order. A segment's own annotations stay with it, or with the part of a
  split segment their time falls in.
- **Options:** `channels` / `exclude_channels`, `output_password` and
  `metadata` work as for [modified copies](transform.md).
- **Passwords:** `passwords` gives the password of each source, in order.

## How the result is checked

The result is built in `<dst>.partial/`, then checked against every source,
and only published when the check passes:
- every segment against the part of the source segment it came from (blocks
  byte-identical, metadata as the source's plus the edits);
- the annotations against the sources' merged in time order;
- no two segments of a channel overlapping;
- the validator reporting nothing on the result.

A recording that already fails the validator should be
[repaired](validation.md) first.

Splitting into many segments costs some extra time at the end: every file of
the result is flushed to disk before it is published, and each segment has a
few files.

`mef3io.merge.verify(sources, dst, ...)` re-checks a merged session later.

To merge and anonymize, merge first, then anonymize the result with
[`anonymize_session`](anonymization.md). To keep only part of the result,
cut it with `transform_session(..., time_range=...)`.

## Refusal codes

Refusals work as for the other copies:
- each has a stable `code`, `exc.code` on the exception and `Refusal.code` in
  a `check` report;
- `mef3io.merge.REFUSAL_CODES` holds them all;
- the codes shared with every copy are in
  [the shared table](anonymization.md#refusal-codes), and the metadata codes
  in [Modified copies](transform.md#refusal-codes).

**Codes only a merge raises:**

| Code | Raised as | Where | Meaning |
|---|---|---|---|
| `merge-too-few` | `ValueError` | check | Fewer than two sources. |
| `merge-passwords-count` | `ValueError` | check | `passwords` is not one password per source. |
| `merge-time-range` | `ValueError` | check | `time_range` was given; cut the result afterwards. |
| `merge-rename` | `ValueError` | check | `rename=False`: a merge is always named after its destination. |
| `merge-rto-differs` | `WriteConflictError` | check | The sources use different recording-time offsets. |
| `merge-rate-differs` | `WriteConflictError` | check | A channel has different sampling rates in different sources (`details["channel"]`). |
| `merge-identity-differs` | `WriteConflictError` | check | The sources' patient field `details["field"]` differs; set it with `metadata`. |
| `merge-overlap` | `WriteConflictError` | check | Two sources hold samples of the same channel at the same moment (`details["channel"]`). |
