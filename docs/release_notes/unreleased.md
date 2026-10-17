# Unreleased

*Changes merged since the last release. They ship with the next version.*

## Highlights

- **De-identified copies.** `mef3io.anonymize_session(src, dst, shift_us)`
  writes a copy of a session with every time moved by one constant and the
  patient's identity removed; the original is never changed. Times move
  everywhere they are stored, including the times inside seizure, epoch and
  cursor annotations. The time-zone fields are set to fixed values
  (recording-time offset 0, GMT offset 0, no daylight-saving dates). Subject
  names, ID and location, the session name, descriptions, annotation text
  (drop, keep with names removed, or your own function), file identifiers and
  other programs' private header areas are removed or replaced (keeping
  annotation text removes only the names mef3io knows, not dates or other
  names written in it). Channels can
  be left out (`exclude_channels=` or `channels=`), and nothing of a left-out
  channel stays in the copy. Samples are copied without decoding, so they are
  bit-identical. The copy is checked by `mef3io.anonymize.verify` before it is
  published; a failure or a cancel leaves nothing behind. Sessions using
  meflib's deprecated recording-time offset are accepted when their offset is
  unambiguous. Encrypted data blocks stay encrypted under the copy's new
  passwords, or are decrypted only when the copy has none. From a terminal:
  `python -m mef3io anonymize SRC DST --shift-us N` or
  `--reference-date YYYY-MM-DD [--tz ZONE]`. Python and C++ only
  ([De-identification](https://bnelair.github.io/mef3io/anonymization/),
  [#70](https://github.com/bnelair/mef3io/issues/70)).
- **Anonymizing from an application** ([#111](https://github.com/bnelair/mef3io/issues/111)):
  - `mef3io.anonymize.check(src, dst, shift_us, ...)` runs every check
    `anonymize_session` makes before it writes, with the same arguments, and
    returns all the refusals instead of raising the first. It writes nothing
    and reads only metadata (never the data files), so a name, a channel
    selection or a date can be checked while the user is still choosing it.
    Both run one pre-flight function in the C++ core, so they cannot
    disagree. `shift_us=None` leaves the date open.
  - **Every refusal has a stable code**, e.g. `dst-exists`,
    `name-contains-identity`, `shift-below-floor`, `rto-disputed`,
    `channel-unknown`, `id-number-time`: as `exc.code` (with `exc.details`)
    on the exception `anonymize_session` raises, whose type is unchanged, and
    as `Refusal.code` in a check. The full table is in
    [Refusal codes](https://bnelair.github.io/mef3io/anonymization/#refusal-codes)
    and in `mef3io.anonymize.REFUSAL_CODES`. Match on the code, not the
    message.
  - `AnonymizeReport.source_start_us`, `source_end_us` and
    `source_channel_times`: the source's original times. They are the
    re-identification key, returned to the caller only: never written into
    the copy, never logged or put in a message, and left out of `repr`,
    `summary()`, pickles and (unless asked) `to_dict()`. No refusal or
    verification message names a source time any more.
  - `mef3io.password_interop_issue(password)` returns why a password will
    not work as expected in other MEF readers (more than 15 characters, more
    than 15 UTF-8 bytes, a character above U+00FF), or `None`: the rule
    behind `PasswordInteropWarning`, so an application can warn before it
    writes.

  - Also: a time in an `Epoc` or `Curs` ID number is now refused before
    anything is written (`id-number-time`), not after the whole copy; and a
    channel that holds only annotations no longer makes the copy fail its
    own verification (a kept data channel that lost its samples still does).

  Python and C++ (`mef3io::check_anonymize`, `mef3io::AnonymizeError`);
  `password_interop_issue` is Python only. Not in MATLAB.
- **Provenance operation 6, `anonymize`**, is now assigned: every file of a
  de-identified copy carries it.
- **Warnings carry the session as data, not in their text.** Every warning
  mef3io emits is now a `mef3io.MefWarning` (still a `UserWarning`) with a
  `path` attribute (the session, as you passed it) and `details` (the
  structured items behind the message, such as the `Reader.problems` entries
  or the block-copy mismatches). The message no longer starts with the path,
  and the session's location and name inside it are replaced by `<session>`
  (also the folder inside a renamed archive). Only those are scrubbed: channel
  names and file names inside the session can still appear, and they can be
  identifying. `warnings.filterwarnings(
  "ignore", category=mef3io.MefWarning)` silences all of them. Python's
  `"default"` and `"module"` filter actions show each warning once per
  session
  ([Warnings carry the path as data](https://bnelair.github.io/mef3io/python/#warnings-carry-the-path-as-data),
  [#96](https://github.com/bnelair/mef3io/issues/96)).
- **`inspect()` reports a legacy password match.**
  `SessionSummary.legacy_password_mapping` is `True` when the password opened
  some metadata or record file only through the mapping of mef3io 1.2.0 and
  earlier (#84), with a `legacy-password` entry in `warnings`. As with every
  other condition, `inspect` reports it as data and does not emit
  `LegacyPasswordWarning` ([#96](https://github.com/bnelair/mef3io/issues/96)).
- **`inspect()` returns each channel's description.**
  `ChannelSummary.channel_description` holds section 2's channel description.
  It can identify the subject, so, like the other identity values, it is
  returned only with `include_values=True`, and it is `None` when section 2
  is locked ([#96](https://github.com/bnelair/mef3io/issues/96)).

## Fixes


- **Opening a session no longer freezes other Python threads.** `Reader(...)`
  (and the low-level `Session` and `SessionWriter` constructors, which the
  `Writer` uses to reopen a session) now scan the session and parse its
  metadata with the GIL released, as reads and writes already did. Before,
  every other thread, including a GUI's event loop, stopped for the whole
  open: about 0.6 s on 64 channels x 60 segments, seconds on a long
  recording. Opening on a worker thread now leaves the rest of the program
  responsive, and several sessions can be opened at once
  ([#79](https://github.com/bnelair/mef3io/issues/79)). `toc`, `segments`,
  `records` and `Writer.write_annotations` release it too.
- **Reading one session from several threads at once no longer crashes.**
  Sharing one `Reader` across threads (a data loader, a viewer) could free a
  block index that another thread was still reading when the index cache
  evicted it, and the process aborted. Each call now holds its own reference
  to the index it reads, and the reader's shared state is locked.
- **A data race in decrypting encrypted metadata** when two threads did it for
  the first time at once (the AES inverse S-box was filled lazily) is gone:
  the table is now fixed at compile time.

## Upgrading

- **Warning text changed.** The messages of `SessionDeclarationWarning`,
  `UnreadableSegmentWarning`, `BlockCopyWarning`, `LegacyPasswordWarning` and
  `PasswordInteropWarning` no longer start with the session path, and file
  paths inside them read `<session>/...`. A warning about record files or
  appended segments that used to name the first one no longer does (they are
  in `w.message.details`). Code that matched on the path in the text should
  use `w.message.path` instead; filters by category are unaffected, and so is
  a filter on the rest of the text (for example
  `ignore:.*leaves size declarations unset.*`).
- **`anonymize_session` with ambiguous session-level annotation files**
  (several `.rdat` at the session root, none named after the session) now
  raises `FormatError` (code `records-ambiguous`), as `inspect` and the
  Reader do, instead of `WriteConflictError`. Both are `MefError`s; code
  that caught `WriteConflictError` for it should catch `MefError` or check
  `exc.code`.
- **`ChannelSummary` and `SessionSummary` have new fields**
  (`channel_description`, `legacy_password_mapping`). Code that builds them
  positionally (rather than receiving them from `inspect`) must add them.
