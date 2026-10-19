"""What every session copy shares (#70 anonymize, #73 transform).

One C++ engine makes both kinds of copy; this module is its Python side:
the verification report, refusals with stable codes, the checks made in
Python before the extension is called, and the refusal codes common to both.
:mod:`mef3io.anonymize` and :mod:`mef3io.transform` build on it.
"""
from __future__ import annotations

import os
from dataclasses import dataclass
from typing import Dict, List, Optional, Sequence, Tuple

#: The refusal codes every copy operation can raise:
#: ``code -> (exception type, where it is decided)``.
#:
#: * ``"check"``: decided before anything is written; ``check()`` reports it
#:   and the copy raises it without writing.
#: * ``"copy"``: only the copy pass can meet it (it needs the data blocks,
#:   or the finished copy); the copy raises it and removes the partial copy.
#: * ``"defensive"``: guards a state an earlier check already rules out, or a
#:   fault of the environment (a full disk); kept so that every refusal has a
#:   code.
COPY_REFUSAL_CODES: Dict[str, Tuple[str, str]] = {
    # arguments (checked in Python)
    "argument-type": ("TypeError", "check"),
    # options
    "channel-selection-conflict": ("ValueError", "check"),
    "output-password-unpaired": ("ValueError", "check"),
    "output-passwords-equal": ("ValueError", "check"),
    # paths
    "dst-not-mefd": ("IoError", "check"),
    "dst-exists": ("IoError", "check"),
    "partial-exists": ("IoError", "check"),
    "source-missing": ("IoError", "check"),
    "dst-inside-src": ("IoError", "check"),
    # opening the source
    "source-unreadable": ("IoError", "check"),
    "source-damaged": ("FormatError", "check"),
    "source-crc": ("CrcError", "check"),
    "records-ambiguous": ("FormatError", "check"),
    "password-wrong": ("PasswordError", "check"),
    "password-required": ("PasswordError", "check"),
    "password-level1-only": ("PasswordError", "check"),
    # what inspect marks as blocking (Refusal.inspect_code names the warning)
    "video-channels": ("WriteConflictError", "check"),
    "unreadable-segments": ("WriteConflictError", "defensive"),
    "incomplete-segments": ("WriteConflictError", "check"),
    "unread-record-files": ("WriteConflictError", "check"),
    "unreadable-records": ("WriteConflictError", "defensive"),
    "parts-locked": ("WriteConflictError", "check"),
    "inspect-blocker": ("WriteConflictError", "defensive"),
    # channel selection
    "channel-unknown": ("ValueError", "check"),
    "no-channels-left": ("ValueError", "check"),
    # a cut (time_range)
    "time-range-invalid": ("ValueError", "check"),
    "time-range-empty": ("ValueError", "check"),
    # the copy's names
    "rename-name-mismatch": ("ValueError", "check"),
    "name-too-long": ("ValueError", "check"),
    # segments and records, read from metadata
    "segment-folder-unnumbered": ("WriteConflictError", "check"),
    "segment-number-mismatch": ("WriteConflictError", "check"),
    "segment-number-duplicate": ("WriteConflictError", "check"),
    "record-damaged": ("FormatError", "defensive"),
    "record-crc": ("CrcError", "defensive"),
    "record-locked": ("PasswordError", "check"),
    "record-index-mismatch": ("FormatError", "check"),
    # the copy pass
    "block-crc": ("CrcError", "copy"),
    "data-damaged": ("FormatError", "copy"),
    "verify-failed": ("WriteConflictError", "copy"),
    "data-locked": ("PasswordError", "defensive"),
    "write-failed": ("IoError", "defensive"),
    # generic codes for an error of shared code that carries none
    "io-error": ("IoError", "defensive"),
    "write-conflict": ("WriteConflictError", "defensive"),
    "invalid-argument": ("ValueError", "defensive"),
}


@dataclass(frozen=True)
class VerifyIssue:
    """One problem a verification found.

    ``message`` names what is wrong in plain words. It never repeats an
    identifying string (a name it found is described, not quoted), so a
    report can be logged or shared.
    """

    location: str
    field: str
    message: str

    def __str__(self) -> str:
        return f"{self.location}: {self.field} {self.message}"


@dataclass(frozen=True)
class VerifyReport:
    """Result of a copy's verification (``anonymize.verify``, ``transform.verify``).

    Attributes
    ----------
    ok : bool
        True when nothing is wrong: no problem and no leak.
    problems : tuple of VerifyIssue
        Everything wrong with the copy, leaks included.
    leaks : tuple of VerifyIssue
        Anonymize only: the problems found by the leak scan (a time field
        holding a value in the source's recording range, plain or negated; a
        name that should be gone; a source UUID; an uncleared field).
    notes : tuple of VerifyIssue
        Anonymize only: overlaps reported but not refused, worth showing the
        user (an identity value inside a kept channel's name as part of a
        longer token or shorter than 3 characters, inside the copy's own name,
        or a number of fewer than 4 digits inside a longer number in kept text).
    inherited_findings : tuple of str
        Validator findings the source already had; a copy carries them over
        unchanged and they do not count against ``ok``.
    channels, excluded_channels : tuple of str
        Channels in the copy, and the source's channels it leaves out.
    segments, blocks, records, times_checked, text_fields_checked : int
        How much was compared.
    """

    ok: bool
    problems: Tuple[VerifyIssue, ...]
    leaks: Tuple[VerifyIssue, ...]
    notes: Tuple[VerifyIssue, ...]
    inherited_findings: Tuple[str, ...]
    channels: Tuple[str, ...]
    excluded_channels: Tuple[str, ...]
    segments: int
    blocks: int
    records: int
    times_checked: int
    text_fields_checked: int

    @classmethod
    def _from(cls, d: dict) -> "VerifyReport":
        return cls(
            ok=bool(d["ok"]),
            problems=tuple(VerifyIssue(**p) for p in d["problems"]),
            leaks=tuple(VerifyIssue(**p) for p in d["leaks"]),
            notes=tuple(VerifyIssue(**p) for p in d.get("notes", ())),
            inherited_findings=tuple(d["inherited_findings"]),
            channels=tuple(d["channels"]),
            excluded_channels=tuple(d["excluded_channels"]),
            segments=d["segments"],
            blocks=d["blocks"],
            records=d["records"],
            times_checked=d["times_checked"],
            text_fields_checked=d["text_fields_checked"],
        )

    def summary(self) -> str:
        """A few lines for a terminal."""
        lines = [
            f"verification: {'passed' if self.ok else 'FAILED'} — {self.segments} segment(s), "
            f"{self.blocks} block(s), {self.records} record(s), {self.times_checked} time(s) "
            f"and {self.text_fields_checked} text field(s) checked"
        ]
        if self.excluded_channels:
            lines.append(f"  channels left out: {', '.join(self.excluded_channels)}")
        for p in self.problems[:20]:
            lines.append(f"  - {p}")
        if len(self.problems) > 20:
            lines.append(f"  ... and {len(self.problems) - 20} more")
        for n in self.notes[:20]:
            lines.append(f"  note: {n}")
        if len(self.notes) > 20:
            lines.append(f"  ... and {len(self.notes) - 20} more notes")
        if self.inherited_findings:
            lines.append(
                f"  {len(self.inherited_findings)} validator finding(s) carried over from the "
                "source unchanged"
            )
        return "\n".join(lines)


@dataclass(frozen=True)
class Refusal:
    """One reason a copy would be refused.

    Attributes
    ----------
    code : str
        Stable identifier (``REFUSAL_CODES`` of :mod:`mef3io.anonymize` or
        :mod:`mef3io.transform`). Match on this, never on ``message``.
    message : str
        Plain words, as the raised exception carries them. It may name the
        source's or the destination's path.
    error : str
        The exception type the copy raises for it: ``"ValueError"``,
        ``"TypeError"``, ``"IoError"``, ``"WriteConflictError"``,
        ``"PasswordError"``, ``"FormatError"`` or ``"CrcError"``.
    channel : str or None
        The channel concerned, if any.
    field : str or None
        The argument or field concerned (``"dst"``, ``"pseudonym"``,
        ``"channels"``, ``"subject_id"``, ...), if any.
    inspect_code : str or None
        For a condition :func:`mef3io.inspect` marks as blocking, that
        warning's code (e.g. ``"video-channels"``).
    """

    code: str
    message: str
    error: str
    channel: Optional[str] = None
    field: Optional[str] = None
    inspect_code: Optional[str] = None

    def __str__(self) -> str:
        return f"{self.code}: {self.message}"


@dataclass(frozen=True)
class CheckReport:
    """Result of a pre-flight check (``anonymize.check``, ``transform.check``).

    Attributes
    ----------
    ok : bool
        True when the copy would start. It can still be refused by what only
        the copy pass meets (codes marked ``"copy"`` in ``REFUSAL_CODES``).
    refusals : tuple of Refusal
        Every refusal found, in the order the copy meets them (it raises the
        first). Checks that depend on one that failed are skipped: nothing
        about the source is checked when it cannot be opened.
    notes : tuple of str
        What would be noted up front: a source that uses meflib's
        recording-time offset (anonymize), an output password other MEF
        readers will not handle as expected.
    session_name : str
        The copy's session name ("" when ``dst`` is unusable).
    pseudonym : str
        Anonymize only: the copy's pseudonym ("" for a transform).
    channels_kept, channels_excluded : tuple of str
        The channel selection (empty when the source could not be read).
    """

    ok: bool
    refusals: Tuple[Refusal, ...]
    notes: Tuple[str, ...]
    session_name: str
    pseudonym: str
    channels_kept: Tuple[str, ...]
    channels_excluded: Tuple[str, ...]

    @property
    def codes(self) -> Tuple[str, ...]:
        """The refusals' codes, in order."""
        return tuple(r.code for r in self.refusals)

    def summary(self) -> str:
        """A few lines for a terminal."""
        if self.ok:
            lines = ["check: nothing stops the copy"]
        else:
            lines = [f"check: {len(self.refusals)} refusal(s)"]
        lines += [f"  - {r}" for r in self.refusals]
        lines += [f"  note: {n}" for n in self.notes]
        return "\n".join(lines)


def _backend():
    from . import _mef3io

    return _mef3io


def _error(error: str, code: str, message: str, **details) -> Exception:
    """A ValueError/TypeError raised in Python, with ``.code`` and ``.details``
    like the ones the extension raises."""
    exc = {"ValueError": ValueError, "TypeError": TypeError}[error](message)
    exc.code = code
    exc.details = {"channel": None, "field": None, "inspect_code": None, **details}
    return exc


class _Refusals:
    """Raise the first refusal (``collect=False``) or gather them all."""

    def __init__(self, collect: bool):
        self.collect = collect
        self.found: List[Refusal] = []

    def add(self, error: str, code: str, message: str, **details) -> None:
        if not self.collect:
            raise _error(error, code, message, **details)
        self.found.append(Refusal(code=code, message=message, error=error, **details))


def _names(value, what: str) -> list:
    if value is None:
        return []
    if isinstance(value, str):
        return [value]
    out = list(value)
    if not all(isinstance(v, str) for v in out):
        # A programming error, raised even by check(); it still has a code.
        raise _error("TypeError", "argument-type", f"{what} must be channel names (str)",
                     field=what)
    return out


def _output_passwords(output_password, refusals: _Refusals) -> Tuple[str, str]:
    if output_password is None:
        return "", ""
    if isinstance(output_password, str):
        refusals.add("ValueError", "output-password-unpaired",
                     "output_password takes a pair (level1, level2): MEF has no level-1-only "
                     "files", field="output_password")
        return "", ""
    pw = tuple(output_password)
    if len(pw) != 2 or not all(isinstance(p, str) and p for p in pw):
        refusals.add("ValueError", "output-password-unpaired",
                     "output_password must be a pair of two non-empty strings (level1, level2)",
                     field="output_password")
        return "", ""
    return pw  # type: ignore[return-value]


def _time_range(time_range):
    """``time_range=(start_us, end_us)`` as the extension takes it, or None."""
    if time_range is None:
        return None
    try:
        t0, t1 = time_range
    except (TypeError, ValueError):
        t0 = t1 = None
    if not all(isinstance(t, int) and not isinstance(t, bool) for t in (t0, t1)):
        raise _error("TypeError", "argument-type",
                     "time_range must be a pair of integers (start_us, end_us)", field="time_range")
    return (int(t0), int(t1))


def _copy_arguments(refusals: _Refusals, *, channels, exclude_channels, output_password,
                    time_range=None, exact=True) -> dict:
    """The checks every copy makes in Python, in the order the copy makes
    them, and the extension's keyword arguments for them."""
    if channels is not None and exclude_channels is not None:
        refusals.add("ValueError", "channel-selection-conflict",
                     "pass channels or exclude_channels, not both", field="channels")
    names = _names(channels, "channels")
    excluded = _names(exclude_channels, "exclude_channels")
    if channels is not None and not names:
        refusals.add("ValueError", "no-channels-left",
                     "the selection leaves no channel to copy (channels=[])", field="channels")
    out1, out2 = _output_passwords(output_password, refusals)
    return dict(output_password_1=out1, output_password_2=out2, channels=names,
                exclude_channels=excluded, time_range=_time_range(time_range), exact=bool(exact))


def _warn_output_passwords(a: dict) -> None:
    if a["output_password_1"]:
        from ._writer import warn_password_interop

        warn_password_interop(a["output_password_1"], a["output_password_2"])


def _check_report(raw: dict, refusals: _Refusals, a: dict) -> CheckReport:
    """The extension's check result, merged with the refusals found in Python
    (never twice) and the output-password notes."""
    found = list(refusals.found)
    seen = {(r.code, r.field) for r in found}
    for r in raw["refusals"]:
        if (r["code"], r["field"]) not in seen:  # also checked in Python
            found.append(Refusal(**r))
    notes = list(raw["notes"])
    from ._writer import password_interop_issue

    for label, pw in (("level-1", a["output_password_1"]), ("level-2", a["output_password_2"])):
        issue = password_interop_issue(pw) if pw else None
        if issue:
            notes.append(f"the {label} output password will not interoperate as expected: {issue}")
    return CheckReport(
        ok=not found,
        refusals=tuple(found),
        notes=tuple(notes),
        session_name=raw["session_name"],
        pseudonym=raw.get("pseudonym", ""),
        channels_kept=tuple(raw["channels_kept"]),
        channels_excluded=tuple(raw["channels_excluded"]),
    )


def _copy_output_password(output_password) -> str:
    """The password that opens a copy: its level-2 password, or the pair's."""
    if output_password is None:
        return ""
    if isinstance(output_password, str):
        return output_password
    return tuple(output_password)[-1]


# --------------------------------------------------------------------------- CLI


def _add_copy_cli_arguments(p) -> None:
    """The command-line options every copy command shares."""
    p.add_argument("--no-rename", action="store_true",
                   help="keep the source's session name (DST must be named after it)")
    p.add_argument("--password", default="", help="level-2 password of an encrypted source")
    p.add_argument("--password-env", help="read the source password from this variable")
    p.add_argument("--output-password", nargs=2, metavar=("LEVEL1", "LEVEL2"),
                   help="encrypt the copy with these two passwords")
    sel = p.add_mutually_exclusive_group()
    sel.add_argument("--channel", action="append", default=None,
                     help="copy only this channel (repeatable)")
    sel.add_argument("--exclude-channel", action="append", default=None,
                     help="leave this channel out (repeatable)")
    p.add_argument("--start", type=int, metavar="UUTC",
                   help="cut: keep only samples from this time (microseconds since 1970, UTC)")
    p.add_argument("--end", type=int, metavar="UUTC",
                   help="cut: keep only samples before this time")
    p.add_argument("--whole-blocks", action="store_true",
                   help="cut: keep whole blocks at the edges instead of trimming them exactly")
    p.add_argument("--json", action="store_true", help="print the report as JSON")


def _cli_password(parser, args) -> str:
    password = args.password
    if args.password_env:
        if args.password:
            parser.error("pass --password or --password-env, not both")
        password = os.environ.get(args.password_env)
        if not password:
            parser.error(f"environment variable {args.password_env} is not set or empty")
    return password


def _cli_copy_kwargs(args, password: str) -> dict:
    if (args.start is None) != (args.end is None):
        raise SystemExit("error: --start and --end go together")
    return dict(
        time_range=None if args.start is None else (args.start, args.end),
        exact=not args.whole_blocks,
        rename=not args.no_rename,
        password=password,
        output_password=tuple(args.output_password) if args.output_password else None,
        channels=args.channel,
        exclude_channels=args.exclude_channel,
    )


def _cli_error(exc: BaseException) -> int:
    import sys

    code = getattr(exc, "code", None)
    print(f"error ({code}): {exc}" if isinstance(code, str) else f"error: {exc}",
          file=sys.stderr)
    return 2


def _fspaths(src, dst) -> Tuple[str, str]:
    return os.fspath(src), os.fspath(dst)


__all__: Sequence[str] = ()
