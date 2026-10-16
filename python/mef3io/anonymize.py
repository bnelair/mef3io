"""De-identified copies of a session (#70).

:func:`anonymize_session` writes a **copy** of a session with every time
shifted by one constant and the patient's identity removed; the original is
never modified. :func:`verify` checks a copy against its source.

Available in Python and C++. There is no MATLAB binding and no C ABI for it.
"""
from __future__ import annotations

import argparse
import datetime as _dt
import json
import os
import sys
from dataclasses import asdict, dataclass, field
from typing import Callable, Iterable, Optional, Sequence, Tuple, Union

from ._cancel import CancelToken, _hooks

__all__ = [
    "AnonymizeReport",
    "VerifyIssue",
    "VerifyReport",
    "anonymize_session",
    "reference_shift",
    "verify",
]

#: Microseconds in one day.
DAY_US = 86_400_000_000

_CLEAR_CHOICES = ("subject", "location", "descriptions")

#: ``text(value, info) -> str | None`` for ``text=`` callables. ``info`` is a
#: dict with ``record_type``, ``field``, ``level``, ``channel``, ``segment``
#: and ``time`` (the record's time in the copy, uUTC).
TextCallback = Callable[[str, dict], Optional[str]]


@dataclass(frozen=True)
class VerifyIssue:
    """One problem :func:`verify` found.

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
    """Result of :func:`verify`.

    Attributes
    ----------
    ok : bool
        True when nothing is wrong: no problem and no leak.
    problems : tuple of VerifyIssue
        Everything wrong with the copy, leaks included.
    leaks : tuple of VerifyIssue
        The problems found by the leak scan: a time field holding a value in
        the source's recording range (plain or negated), a name that should
        be gone, a source UUID, an uncleared field.
    notes : tuple of VerifyIssue
        Overlaps reported but not refused, worth showing the user: an
        identity value inside a kept channel's name as part of a longer token
        (or one shorter than 3 characters), inside the copy's own name, or a
        number of fewer than 4 digits inside a longer number in kept text.
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
class AnonymizeReport:
    """Result of :func:`anonymize_session`.

    Attributes
    ----------
    destination : str
        The copy.
    session_name, pseudonym : str
        The copy's session name (its folder's stem) and the value written to
        every ``anonymized_name``.
    shift_us : int
        The shift applied to every time, in microseconds.
    source_recording_time_offset : int
        The source's recording-time offset (meflib's deprecated date
        masking); the copy always has 0.
    channels_kept, channels_excluded : tuple of str
    segments, blocks, records : int
        What was copied.
    blocks_reencrypted, blocks_decrypted : int
        Data blocks the source had encrypted, written encrypted with the new
        passwords, or decrypted because the copy has none.
    record_text_fields_changed : int
        Record text fields the ``text=`` policy (and the name redaction)
        changed.
    record_bodies_cleared : int
        Record bodies of an unknown type or version, not carried over because
        they may hold text or dates that cannot be found.
    seizure_channel_entries_dropped : int
        Per-channel entries of ``Seiz`` records that named an excluded channel.
    bytes_written : int
    output_encrypted : bool
    not_copied : tuple of str
        Files in the source that are not part of the MEF layout and were left
        out (source-relative paths).
    notes : tuple of str
    verification : VerifyReport
        The check the copy passed before it was published.
    """

    destination: str
    session_name: str
    pseudonym: str
    shift_us: int
    source_recording_time_offset: int
    channels_kept: Tuple[str, ...]
    channels_excluded: Tuple[str, ...]
    segments: int
    blocks: int
    blocks_reencrypted: int
    blocks_decrypted: int
    records: int
    record_text_fields_changed: int
    record_bodies_cleared: int
    seizure_channel_entries_dropped: int
    bytes_written: int
    output_encrypted: bool
    not_copied: Tuple[str, ...]
    notes: Tuple[str, ...]
    verification: VerifyReport = field(repr=False)

    @classmethod
    def _from(cls, d: dict) -> "AnonymizeReport":
        d = dict(d)
        d["verification"] = VerifyReport._from(d["verification"])
        for k in ("channels_kept", "channels_excluded", "not_copied", "notes"):
            d[k] = tuple(d[k])
        return cls(**d)

    def to_dict(self) -> dict:
        """Plain ``dict`` (JSON-serialisable)."""
        return asdict(self)

    def summary(self) -> str:
        """A few lines for a terminal."""
        lines = [
            f"wrote {self.destination}: {len(self.channels_kept)} channel(s), "
            f"{self.segments} segment(s), {self.blocks} block(s), {self.records} record(s)",
            f"  every time shifted by {self.shift_us} us "
            f"({self.shift_us / DAY_US:+.4f} days); time-zone fields set, identity removed",
        ]
        if self.channels_excluded:
            lines.append(f"  channels left out: {', '.join(self.channels_excluded)}")
        if self.blocks_reencrypted or self.blocks_decrypted:
            lines.append(
                f"  encrypted data blocks: {self.blocks_reencrypted} re-encrypted, "
                f"{self.blocks_decrypted} decrypted"
            )
        if self.record_bodies_cleared:
            lines.append(
                f"  {self.record_bodies_cleared} record bod(ies) of an unknown type or version "
                "not carried over"
            )
        if self.not_copied:
            lines.append(f"  {len(self.not_copied)} file(s) outside the MEF layout not copied")
        for n in self.notes:
            lines.append(f"  note: {n}")
        lines.append(self.verification.summary())
        return "\n".join(lines)


def _backend():
    from . import _mef3io

    return _mef3io


def _clear_flags(clear: Iterable[str]) -> dict:
    if isinstance(clear, str):
        clear = (clear,)
    chosen = set(clear)
    unknown = chosen - set(_CLEAR_CHOICES)
    if unknown:
        raise ValueError(
            f"clear= takes any of {_CLEAR_CHOICES}; unknown: {sorted(unknown)}"
        )
    return dict(
        clear_subject="subject" in chosen,
        clear_location="location" in chosen,
        clear_descriptions="descriptions" in chosen,
    )


def _names(value, what: str) -> list:
    if value is None:
        return []
    if isinstance(value, str):
        return [value]
    out = list(value)
    if not all(isinstance(v, str) for v in out):
        raise TypeError(f"{what} must be channel names (str)")
    return out


def _output_passwords(output_password) -> Tuple[str, str]:
    if output_password is None:
        return "", ""
    if isinstance(output_password, str):
        raise ValueError(
            "output_password takes a pair (level1, level2): MEF has no level-1-only files"
        )
    pw = tuple(output_password)
    if len(pw) != 2 or not all(isinstance(p, str) and p for p in pw):
        raise ValueError("output_password must be a pair of two non-empty strings (level1, level2)")
    return pw  # type: ignore[return-value]


def anonymize_session(
    src,
    dst,
    shift_us: int,
    *,
    pseudonym: Optional[str] = None,
    clear: Iterable[str] = _CLEAR_CHOICES,
    text: Union[str, TextCallback] = "drop",
    rename: bool = True,
    password: str = "",
    output_password: Optional[Sequence[str]] = None,
    channels: Optional[Sequence[str]] = None,
    exclude_channels: Optional[Sequence[str]] = None,
    progress: Optional[Callable[[int, int], object]] = None,
    cancel: Optional[CancelToken] = None,
) -> AnonymizeReport:
    """Write a de-identified copy of a session: every time shifted, identity removed.

    The source is never modified. The copy is built next to ``dst`` in
    ``<dst>.partial/``, checked with :func:`verify`, and only then moved to
    ``dst``. A failure, a refusal or a cancel leaves nothing behind.

    The copy pass reads the source once and writes the copy once, streaming
    data blocks; :func:`verify` then reads the source's data and the copy
    again. Data blocks are never decoded or re-encoded: only their start time
    (and, for encrypted blocks, the 16 encrypted statistics bytes) changes,
    and the regions other programs may fill are cleared, so the samples are
    bit-identical.

    **What changes.** Every time moves by ``shift_us``, each stored in the
    sign convention it came in: universal-header start/end in every file,
    block-index entries, data-block headers, record headers, record-index
    entries and the times inside ``Seiz``, ``Epoc`` and ``Curs`` records. The
    time-zone fields are SET, not shifted: ``recording_time_offset = 0``,
    ``GMT_offset = 0``, DST start/end = "no entry". Identity: subject names,
    ID and recording location are cleared (``clear=``); the session name is
    replaced in every header, in the session-level record files' names and in
    the descriptions; ``anonymized_name`` is set to ``pseudonym``; every UUID
    is regenerated; other applications' private header regions are cleared;
    every file is written fresh. Each file is stamped with provenance
    operation 6, ``anonymize``.

    **What is refused** (nothing is written): a destination that exists or
    lies inside the source; a session ``inspect`` marks as blocking (video
    channels, unreadable or incomplete segments, a disputed or ambiguous
    recording-time offset, parts the password does not open); a stored time
    at or before a non-zero recording-time offset; a shift that is zero,
    smaller in magnitude than the session's time span plus one day, or that
    would take any time to 0 or below; damaged data (a failing block or record
    CRC is never re-sealed).

    Parameters
    ----------
    src : str or path-like
        The session: a ``.mefd`` directory or a ``.mefd.tar`` archive (read
        in place).
    dst : str or path-like
        The copy, a new ``.mefd`` directory. Its stem becomes the session
        name.
    shift_us : int
        Microseconds added to every time. :func:`reference_shift` computes one
        from a chosen new date. Must satisfy ``abs(shift_us) >= span + 1 day``
        so that no copied time can fall inside the original range.
    pseudonym : str, optional
        Written to ``anonymized_name`` in every file. Default: the copy's
        session name. The pseudonym and the copy's name must not contain the
        patient's names, ID or location, or the original session name (4+
        characters anywhere, shorter values as a whole word): ``ValueError``
        before anything is written.
    clear : iterable of {"subject", "location", "descriptions"}, optional
        What to clear. Default: all three. Leaving ``"descriptions"`` out
        keeps the section-2 descriptions with the old session name replaced
        by the new one and every removed name redacted.
    text : {"drop", "keep"} or callable, optional
        Record free text (Note/SyLg/EDFA text, Seiz markers and annotation,
        Epoc type and text, Curs name, CSti and ESti strings). ``"drop"``
        (default) clears it; the records themselves, their types and shifted
        times are kept. ``"keep"`` keeps it, with only the names mef3io knows
        removed (the old session name, cleared subject and location values,
        excluded channel names). Names shorter than 4 characters and numbers
        are replaced only as whole words / whole numbers, and kept channel
        names are never changed; a short name left inside a longer word makes
        :func:`verify` refuse the copy. It does **not** find dates, ages,
        other people's names or anything else written in free text. A callable
        ``text(value, info)`` returns the new text (``None`` clears); the same
        redaction is applied after it. Text that is not valid UTF-8 is passed
        with ``errors="surrogateescape"`` and encoded back the same way, so
        every byte survives a callable that returns it unchanged. Python only.
        ``info`` is a dict with ``record_type``, ``field``, ``level``,
        ``channel``, ``segment`` and ``time`` (shifted, uUTC). Names in a
        seizure's channel list that are not a kept channel's exact name are
        cleared with ``"drop"`` and redacted otherwise.
    rename : bool, optional
        ``True`` (default) gives the copy the stem of ``dst`` as its session
        name everywhere. ``False`` keeps the source's session name, and the
        stem of ``dst`` must equal it.
    password : str, optional
        Opens an encrypted source. It must grant level-2 (full) access:
        section 3 holds the recording-time offset and the subject fields.
    output_password : (str, str), optional
        ``(level1, level2)`` to encrypt the copy, with the same pairing rule as
        :class:`Writer`; ``None`` (default) writes it unencrypted. The
        source's passwords are never carried over. Data blocks the source had
        encrypted are re-encrypted with these passwords at their level, or
        decrypted when there are none; blocks the source had plain stay plain
        (and readable by pymef and meflib).
    channels : sequence of str, optional
        Copy only these channels. Mutually exclusive with ``exclude_channels``.
    exclude_channels : sequence of str, optional
        Leave these channels out. Nothing of an excluded channel stays in the
        copy: no folder, no records, no entry in a ``Seiz`` record, and its
        name is redacted from kept text.
    progress : callable, optional
        ``progress(done, total)`` in bytes (the copy and its verification).
    cancel : CancelToken, optional
        Stop the copy from another thread; :class:`Cancelled` is raised and
        nothing is left behind.

    Returns
    -------
    AnonymizeReport

    Raises
    ------
    ValueError
        A bad argument: the shift (zero, below the floor, into times <= 0), an
        unknown channel, both ``channels`` and ``exclude_channels``, a
        selection that leaves no channel, an unknown ``clear=`` entry, an
        unpaired output password, ``rename=False`` with a differently named
        ``dst``.
    WriteConflictError
        The session cannot be copied without risking a misplaced or leaked
        date, or the copy failed verification.
    PasswordError
        The source is encrypted and the password does not grant level 2.
    IoError
        ``dst`` or ``<dst>.partial`` exists, or ``dst`` is inside ``src``.

    Examples
    --------
    >>> shift = mef3io.anonymize.reference_shift(start_us, "2000-01-03", tz="Europe/Prague")
    >>> rep = mef3io.anonymize_session("patient.mefd", "S001.mefd", shift)
    >>> rep.verification.ok
    True
    """
    if channels is not None and exclude_channels is not None:
        raise ValueError("pass channels or exclude_channels, not both")
    if channels is not None and not _names(channels, "channels"):
        raise ValueError("the selection leaves no channel to copy (channels=[])")
    out1, out2 = _output_passwords(output_password)
    if out1:
        from ._writer import warn_password_interop

        warn_password_interop(out1, out2)
    if callable(text):
        text_mode, text_fn = "callback", text
    elif text in ("drop", "keep"):
        text_mode, text_fn = text, None
    else:
        raise ValueError("text must be 'drop', 'keep' or a callable")
    raw = _backend().anonymize_session(
        os.fspath(src),
        os.fspath(dst),
        int(shift_us),
        pseudonym=pseudonym or "",
        text=text_mode,
        text_fn=text_fn,
        rename=bool(rename),
        password=password or "",
        output_password_1=out1,
        output_password_2=out2,
        channels=_names(channels, "channels"),
        exclude_channels=_names(exclude_channels, "exclude_channels"),
        **_clear_flags(clear),
        **_hooks(cancel, progress),
    )
    return AnonymizeReport._from(raw)


def verify(
    src,
    dst,
    shift_us: int,
    *,
    password: str = "",
    output_password: Union[None, str, Sequence[str]] = None,
    clear: Iterable[str] = _CLEAR_CHOICES,
    progress: Optional[Callable[[int, int], object]] = None,
    cancel: Optional[CancelToken] = None,
) -> VerifyReport:
    """Check a de-identified copy against its source. Reads both; writes nothing.

    :func:`anonymize_session` runs this on every copy before publishing it;
    call it yourself to re-check a copy later or one made elsewhere.

    It checks that:

    * the validator reports nothing on the copy the source did not already
      have;
    * the copy holds a subset of the source's channels, segment for segment;
    * every data block's compressed payload and its 256-byte statistics
      table (compared decrypted) are byte-identical to the source's, so the
      samples are too, with nothing decoded;
    * every time differs from the source's by exactly ``shift_us``, in the
      same sign convention, and the time-zone fields hold their set values
      (offset 0, GMT offset 0, DST "no entry");
    * the records match in count, order and type;
    * a **field-wise** leak scan finds nothing: every time field (universal
      headers, section 3, block index, data-block headers, record headers,
      record index, Seiz/Epoc/Curs body times) is checked against the
      source's recording range, plain or negated, widened by a day; every
      text field (session and anonymized names, descriptions, subject fields,
      record text) is checked for the names the copy must not carry; no
      source UUID is reused. Sizes, counts and sample values are never read as
      times, and the compressed data is compared rather than scanned, so the
      scan has no false positives from random bytes.

    Parameters
    ----------
    src, dst : str or path-like
        The source session and the copy.
    shift_us : int
        The shift the copy was made with.
    password : str, optional
        Opens an encrypted source (level 2).
    output_password : str or (str, str), optional
        Opens an encrypted copy: its level-2 password, or the pair.
    clear : iterable of str, optional
        What the copy was asked to clear (as for :func:`anonymize_session`).
    progress, cancel : optional
        As for :func:`anonymize_session`.

    Returns
    -------
    VerifyReport
    """
    if output_password is None:
        out = ""
    elif isinstance(output_password, str):
        out = output_password
    else:
        out = tuple(output_password)[-1]
    raw = _backend().verify_anonymized(
        os.fspath(src),
        os.fspath(dst),
        int(shift_us),
        password=password or "",
        output_password=out,
        **_clear_flags(clear),
        **_hooks(cancel, progress),
    )
    return VerifyReport._from(raw)


def reference_shift(
    reference_start_us: int,
    new_date,
    *,
    tz: Optional[str] = None,
    keep_weekday: bool = False,
) -> int:
    """The shift that moves a reference recording to ``new_date``.

    ``D`` is a whole number of days, measured on the clock being kept:

    * ``tz`` given (keep the LOCAL time of day): ``D = new_date - (the
      reference's local start date in tz)`` and the shift is
      ``D + off_ref``, where ``off_ref`` is the reference start's UTC offset
      in ``tz`` at that instant. The copy's UTC clock then reads the original
      local time of day.
    * ``tz`` omitted (keep the UTC time of day): ``D = new_date - (the
      reference's UTC start date)`` and the shift is ``D``.

    ``keep_weekday`` rounds ``D`` to the nearest multiple of 7 days, so the
    copy falls on the same weekday (``off_ref`` is never rounded).

    Parameters
    ----------
    reference_start_us : int
        The reference recording's start, uUTC.
    new_date : datetime.date or str
        The date the reference should start on, e.g. ``"2000-01-03"``.
    tz : str, optional
        An IANA time zone (e.g. ``"America/Chicago"``).
    keep_weekday : bool, optional

    Returns
    -------
    int
        The shift, in microseconds.
    """
    if isinstance(new_date, str):
        new_date = _dt.date.fromisoformat(new_date)
    if isinstance(new_date, _dt.datetime):
        new_date = new_date.date()
    start = _dt.datetime.fromtimestamp(0, _dt.timezone.utc) + _dt.timedelta(
        microseconds=int(reference_start_us)
    )
    off_us = 0
    if tz:
        from zoneinfo import ZoneInfo

        local = start.astimezone(ZoneInfo(tz))
        ref_date = local.date()
        off = local.utcoffset() or _dt.timedelta(0)
        off_us = (off.days * 86_400 + off.seconds) * 1_000_000 + off.microseconds
    else:
        ref_date = start.date()
    days = (new_date - ref_date).days
    if keep_weekday:
        days = int(round(days / 7.0)) * 7
    return days * DAY_US + off_us


# --------------------------------------------------------------------------- CLI


def _build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="python -m mef3io anonymize",
        description="Write a de-identified copy of a session: every time shifted by one "
        "constant, identity removed. The source is never modified.",
    )
    p.add_argument("src", help="the session (.mefd directory or .mefd.tar archive)")
    p.add_argument("dst", help="the copy, a new .mefd directory (its name becomes the session name)")
    how = p.add_mutually_exclusive_group(required=True)
    how.add_argument("--shift-us", type=int, help="shift every time by this many microseconds")
    how.add_argument(
        "--reference-date",
        help="move the session's start to this date (YYYY-MM-DD); keeps the UTC time of day, "
        "or the local one with --tz",
    )
    p.add_argument("--tz", help="IANA time zone whose local time of day --reference-date keeps")
    p.add_argument("--keep-weekday", action="store_true",
                   help="with --reference-date: land on the same weekday (nearest date)")
    p.add_argument("--pseudonym", help="written to every anonymized_name (default: the new name)")
    p.add_argument("--keep", action="append", default=[], choices=_CLEAR_CHOICES,
                   help="do NOT clear this (repeatable): subject, location, descriptions")
    p.add_argument("--text", choices=("drop", "keep"), default="drop",
                   help="record free text: drop (default) or keep with names redacted")
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
    p.add_argument("--json", action="store_true", help="print the report as JSON")
    return p


def _main(argv: Sequence[str] | None = None) -> int:
    parser = _build_parser()
    args = parser.parse_args(argv)
    password = args.password
    if args.password_env:
        if args.password:
            parser.error("pass --password or --password-env, not both")
        password = os.environ.get(args.password_env)
        if not password:
            parser.error(f"environment variable {args.password_env} is not set or empty")
    if args.tz and args.reference_date is None:
        parser.error("--tz only applies with --reference-date")
    if args.keep_weekday and args.reference_date is None:
        parser.error("--keep-weekday only applies with --reference-date")
    try:
        if args.reference_date is not None:
            from ._inspect import inspect as _inspect

            start = _inspect(args.src, password).start_time
            if start is None:
                parser.error("the session has no samples to take a reference start from; "
                             "use --shift-us")
            shift = reference_shift(start, args.reference_date, tz=args.tz,
                                    keep_weekday=args.keep_weekday)
        else:
            shift = args.shift_us
        report = anonymize_session(
            args.src,
            args.dst,
            shift,
            pseudonym=args.pseudonym,
            clear=[c for c in _CLEAR_CHOICES if c not in args.keep],
            text=args.text,
            rename=not args.no_rename,
            password=password,
            output_password=tuple(args.output_password) if args.output_password else None,
            channels=args.channel,
            exclude_channels=args.exclude_channel,
        )
    except (RuntimeError, ValueError, OSError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    if args.json:
        print(json.dumps(report.to_dict(), indent=2))
    else:
        print(report.summary())
    return 0
