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

To read several recordings as one **without** writing a merged copy, use
[`MultiReader`](python.md#several-recordings-read-as-one): its reads equal a
`Reader`'s over the merged session.

```python
import mef3io

rep = mef3io.merge_sessions(["stay_part1.mefd", "stay_part2.mefd"], "stay.mefd")
print(rep.summary())
```

From the command line:

```bash
python -m mef3io merge stay.mefd stay_part1.mefd stay_part2.mefd
```

`records="drop"` leaves all annotations out of the result (see
[transform](transform.md#leave-out-the-annotations)); `verify` then expects none. The report's `records_dropped` counts the annotations
of the channels and segments the result keeps (and the sources' session-level ones);
those of a channel left out are not counted.

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
  differ. Set them for the result with `metadata=` (or clear them by
  anonymizing) to merge anyway; or check that these really are the same
  patient's recordings.

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

## The whole pipeline in one call

Several long recordings on a network drive can become one anonymous session
on a local drive in a single call. Each source byte is read once and written
once by the copy; the check at the end then reads the sources and the result
again.

```python
rep = mef3io.merge_sessions(
    ["//nas/pt7/day1.mefd", "//nas/pt7/day2.mefd", "//nas/pt7/day3.mefd"],
    "D:/local/S007.mefd",
    exclude_channels=["EKG", "Pulse"],                  # leave channels out
    time_ranges=[(t0_a, t1_a), None, (t0_c, t1_c)],     # one range per source
    anonymize={"shift_us": shift, "pseudonym": "S007"},  # same options as anonymize_session
    records="drop",                                     # no notes or events
    n_threads=16,
)
```

**Time ranges.** `time_ranges` has one entry per source, in order:
`(start_us, end_us)` keeps the samples in `[start, end)` (microseconds, as
that source stores them), `None` keeps the whole source. The cut rules are
those of [`transform_session`](transform.md) (edge blocks trimmed exactly, or
kept whole with `exact=False`; annotations outside the range left out).
Overlap is judged on the **trimmed** data, so two recordings that overlap
only outside their ranges merge. A source with no sample in its range, or a
list of the wrong length, is refused.

**Anonymizing.** `anonymize=True` or a mapping with `shift_us`, `pseudonym`,
`clear` and `text`, exactly as for [`anonymize_session`](anonymization.md).
One shift applies to every source and is judged against the span of **all**
the sources (whole recordings, trimmed or not). The identity of **every**
source (patient names, ID, location, session names) is removed and searched
for in the result, and the result is verified as `anonymize.verify` verifies a
copy, plus the merge checks. Patient fields that are cleared may differ
between the sources (no `merge-identity-differs`); a field you keep with
`clear=` must still agree. `metadata=` cannot be combined with it. The
report's `pseudonym` and `shift_us` say what was applied; the sources' real
start and end times are in the report only in memory, like
`AnonymizeReport.source_start_us`.

**Threads.** `n_threads` (0 = automatic, at least 8; 1 = no extra thread)
covers opening the sources, copying channels and the verification, per the
[one thread rule](threads.md#merging-and-copying). The result does not depend
on it: apart from the 1024-byte file headers (fresh random IDs) every byte is
identical. A text callback is called one record at a time, so the copy then
uses one thread. Progress is reported at a bounded rate, never concurrently
and never backwards; cancelling stops all channels and leaves nothing behind.

**Sources and result on network drives.** Every open of a file is a round
trip on a share, so a merge opens each source file once for the whole
operation (the check before copying, the copy and the verification share the
open files) and creates, fills, flushes and closes each result file through
one handle. The verification still reads the sources again and still reads
the result back from disk; only the number of opens changes, not what is
checked. For 4 sources of 144 files each this took the opens of the source
files from 5,824 to 576 and of the result files from 3,649 to 1,152; see
[Benchmarks](benchmarks.md#copying-over-a-slow-file-system). The
sources must not be changed while the merge runs.

To keep only part of an existing result, cut it with
`transform_session(..., time_range=...)`.

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
| `merge-time-range` | `ValueError` | check | The single `time_range` was given; a merge takes `time_ranges`, one per source. |
| `merge-time-ranges-count` | `ValueError` | check | `time_ranges` is not one entry per source. |
| `merge-anonymize-metadata` | `ValueError` | check | `metadata` was combined with `anonymize`. |
| `merge-threads-invalid` | `ValueError` | check | `n_threads` is negative. |
| `merge-rename` | `ValueError` | check | `rename=False`: a merge is always named after its destination. |
| `merge-rto-differs` | `WriteConflictError` | check | The sources use different recording-time offsets. |
| `merge-rate-differs` | `WriteConflictError` | check | A channel has different sampling rates in different sources (`details["channel"]`). |
| `merge-identity-differs` | `WriteConflictError` | check | The sources' patient field `details["field"]` differs; set it with `metadata`, or clear it with `anonymize`. |
| `merge-overlap` | `WriteConflictError` | check | Two sources hold samples of the same channel at the same moment (`details["channel"]`). |
