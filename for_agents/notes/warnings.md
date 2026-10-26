# Warnings

> Read before emitting any Python warning or touching inspect()'s warning data.
> Split out of CLAUDE.md verbatim; "the private reader" = the private third-party reader named in CLAUDE.md.

Warnings (#96): EVERY Python warning is a `MefWarning` (`python/mef3io/
_warnings.py`, a UserWarning) emitted through `_warnings.emit(cls, msg,
path=, details=, stacklevel=)` — NEVER a bare `warnings.warn` (test_p30 bans
importing `warnings` outside _warnings.py, by AST). The session goes in `w.message.path`, the structured
items in `.details`; the TEXT carries no path: `emit` scrubs the session's
LOCATION and NAME to `<session>`: the path as given/abs/real, the backend's
own location and name (`Reader.session_identity()` — a RENAMED tar keeps
its original inner folder name), `<session>::<first component>`, and the name
as a WHOLE WORD anywhere (it turns up after a space, e.g. "no <name>.rdat").
ONLY those: channel and file names inside the session still appear and can
be identifying — never claim more. `emit` keys a warning registry by
(session, calling file) (`warn_explicit`), so "default"/"module" stay once
per session now that the path is out of the text. `stacklevel` means what it means to
`warnings.warn` at the call site. The pyproject filter matches the text after
the old path prefix, so it still works. `inspect()` emits NO Python warnings:
it reports conditions as `SummaryWarning` data (incl. `legacy-password` +
`SessionSummary.legacy_password_mapping`, from the .tmet and the record
files). `ChannelSummary.channel_description` is an identity value: only with
`include_values`, None when section 2 is locked. inspect's `SummaryWarning`
messages and `problems` still contain paths (they are data, documented as
identifiable). Tests: tests/test_p30_warning_paths.py, p26, p27.
