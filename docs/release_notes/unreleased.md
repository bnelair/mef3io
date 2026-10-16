# Unreleased

*Changes merged since the last release. They ship with the next version.*

## Highlights

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
- **`ChannelSummary` and `SessionSummary` have new fields**
  (`channel_description`, `legacy_password_mapping`). Code that builds them
  positionally (rather than receiving them from `inspect`) must add them.
