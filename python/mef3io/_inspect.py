"""``mef3io.inspect``: everything about a session without reading its samples.

The work is done in C++ (``core/src/inspect.cpp``); this module only turns
the backend's nested dicts into dataclasses and renders the CLI text. Every
object here converts back to plain JSON-ready data with ``to_dict()``, which
is what a helper process (mef3io-studio's Explorer and anonymizer) ships.
"""
from __future__ import annotations

import dataclasses
import datetime as _dt
import json
import sys
from dataclasses import dataclass, field
from typing import Callable, Optional, Sequence

from ._cancel import CancelToken, _hooks


def _fields(cls, d: dict) -> dict:
    names = {f.name for f in dataclasses.fields(cls)}
    return {k: v for k, v in d.items() if k in names}


@dataclass(frozen=True)
class SegmentSummary:
    """One segment, from its metadata only.

    Times are absolute uUTC (UTC). With section 3 locked (level-1 password,
    or none) a time stored relative to the offset is resolved ASSUMING
    offset 0, as :class:`mef3io.Reader` does, and ``offset_assumed_zero`` is
    ``True``: not verified (absolute stored times need no assumption). They are
    ``None`` only for a segment that could not be read. ``time_offset`` is
    ``"none"``, ``"applied"`` (meflib's offset is set), ``"locked"`` (section
    3 encrypted and not opened) or ``"unknown"`` (the segment is unreadable);
    ``recording_time_offset`` holds its value only with
    ``include_values=True`` (the value is itself identifying: it is the local
    midnight of the recording day). ``number_of_discontinuities`` is the
    DECLARED count (legacy writers leave 0 there even with gaps).
    ``units_conversion_factor`` is the segment's own, as stored (``0.0`` is
    MEF's "no entry"; ``None`` when section 2 is locked). File sizes
    are in bytes; the ``.tdat`` is measured, never read. ``provenance`` is the
    dict described in :meth:`mef3io.Reader.segments`, or ``None``.
    """

    segment: int
    path: str
    readable: bool
    problem: Optional[str]
    section2_available: bool
    section3_available: bool
    start_time: Optional[int]
    end_time: Optional[int]
    start_sample: Optional[int]
    number_of_samples: Optional[int]
    number_of_blocks: Optional[int]
    number_of_discontinuities: Optional[int]
    units_conversion_factor: Optional[float]
    time_offset: str
    recording_time_offset: Optional[int]
    offset_assumed_zero: bool
    tmet_bytes: int
    tidx_bytes: int
    tdat_bytes: int
    record_bytes: int
    has_records: bool
    provenance: Optional[dict]
    unset_declarations: list

    @classmethod
    def _from(cls, d: dict) -> "SegmentSummary":
        return cls(**_fields(cls, d))


@dataclass(frozen=True)
class ChannelSummary:
    """One channel: signal metadata, its segment map and its gaps.

    ``gaps`` are ``(start, end)`` uUTC spans with no samples. Without
    ``read_index=True`` they are only the gaps BETWEEN segments and
    ``gaps_complete`` is ``False``: a gap inside a segment is found only in the
    block index (whose copy of each block's time and count is used; the
    authoritative RED block header lives in the ``.tdat``, which is never
    read). ``gaps_certain`` is ``False`` when a listed gap may not be one,
    because a segment that could not be read or placed may hold data there,
    or because a segment whose times assume offset 0 sits next to one whose
    non-zero offset was read. ``number_of_samples`` counts stored samples (gaps excluded).
    ``bytes`` is every file of the channel; ``records`` counts its channel-
    and segment-level records.

    ``sampling_frequency``, ``units_conversion_factor`` and
    ``units_description`` come from the first segment whose metadata section 2
    is readable, and are ``None`` when every section 2 is locked (encrypted,
    no level-1 password). Segments CAN differ (each has its own section 2);
    only the first readable one is reported here, except that
    ``units_conversion_factor`` is ``None`` when the readable segments store
    different factors (each segment's is in ``segments``). ``channel_description`` is
    section 2's free-text description from the same segment; every distinct
    value across segments and channels is in the identity table
    (``identity_field("channel_description").values``, with
    ``include_values=True``). It can identify the subject, so, like
    every identity value, it is returned only with ``include_values=True``:
    otherwise it is ``None``, and so it is when section 2 is locked. Its
    presence is always reported in the identity table
    (``identity_field("channel_description")``).
    """

    name: str
    sampling_frequency: Optional[float]
    units_conversion_factor: Optional[float]
    units_description: Optional[str]
    channel_description: Optional[str]
    start_time: Optional[int]
    end_time: Optional[int]
    number_of_samples: Optional[int]
    gaps: list
    gaps_complete: bool
    gaps_certain: bool
    offset_assumed_zero: bool
    bytes: int
    records: int
    segments: list

    @classmethod
    def _from(cls, d: dict) -> "ChannelSummary":
        d = dict(d)
        d["gaps"] = [tuple(g) for g in d["gaps"]]
        d["segments"] = [SegmentSummary._from(s) for s in d["segments"]]
        return cls(**_fields(cls, d))


@dataclass(frozen=True)
class RecordSummary:
    """One record (annotation), at any level.

    ``time`` is the header time (absolute uUTC). ``first_time`` /
    ``last_time`` span every time the record mentions — the header, the body
    times (Seiz onsets/offsets, Epoc timestamp/end, Curs trace timestamp) and
    ``time + duration`` — which is what a time range must cover. They are
    ``None`` when the file's times depend on an offset that is disputed or
    unknown (an unreadable segment in scope); ``times_known`` is then
    ``False``, and the times inside ``fields`` are ``None`` too — never a
    placeholder. ``offset_assumed_zero`` is ``True`` when they were placed
    with an assumed offset 0 because a section 3 in their scope is locked
    (as the Reader does): not verified. ``text_bytes`` is the length of its free text; the text itself
    is in ``fields`` only with ``include_values=True`` (the full record, as
    :meth:`mef3io.Reader.records` returns it). ``body_locked`` means the body
    is encrypted and the password does not open it.
    """

    type: str
    level: str
    channel: Optional[str]
    segment: Optional[int]
    time: Optional[int]
    times_known: bool
    offset_assumed_zero: bool
    duration: Optional[int]
    first_time: Optional[int]
    last_time: Optional[int]
    text_bytes: int
    body_locked: bool
    fields: Optional[dict]

    @classmethod
    def _from(cls, d: dict) -> "RecordSummary":
        return cls(**_fields(cls, d))


@dataclass(frozen=True)
class IdentityField:
    """Whether one identifying field holds anything, and where.

    ``occurrences`` is the number of places (files, or records for
    ``record_text``) where it is non-empty, ``locked`` the number of places
    it could not be read (encrypted, or a file that failed in lenient mode),
    ``max_length`` the longest value in bytes and ``distinct`` the number of
    different values. ``values`` lists them only with
    ``include_values=True``; otherwise it is empty. ``present`` is ``True``
    if it is set anywhere, ``False`` if it is set nowhere, and ``None`` if it
    may be set somewhere that could not be read.
    """

    name: str
    location: str
    occurrences: int
    locked: int
    max_length: int
    distinct: int
    values: list
    present: Optional[bool]  # a stored field, so to_dict() / JSON carries it

    @classmethod
    def _from(cls, d: dict) -> "IdentityField":
        d = dict(d)
        d["present"] = True if d["occurrences"] else (None if d["locked"] else False)
        return cls(**_fields(cls, d))


@dataclass(frozen=True)
class SummaryWarning:
    """Something a caller should know before using the session.

    ``blocks_anonymize`` marks the conditions ``anonymize_session`` refuses:
    video channels, an ambiguous or disputed time
    convention, unresolved times, locked (encrypted) parts, unreadable files.
    """

    code: str
    message: str
    blocks_anonymize: bool

    @classmethod
    def _from(cls, d: dict) -> "SummaryWarning":
        return cls(**_fields(cls, d))


@dataclass(frozen=True)
class SessionSummary:
    """Everything about a session that does not need its samples.

    Returned by :func:`mef3io.inspect`. All times are absolute uUTC (UTC), or
    ``None`` where they cannot be known without guessing.

    Attributes
    ----------
    name, path, layout
        The session's name, the path given, and ``"directory"`` or ``"tar"``.
    total_bytes, archive_bytes
        Sizes of every MEF file (``.tdat`` measured, never read); the archive
        file's size for a tar (else ``None``).
    encryption : dict
        ``encrypted``; ``access`` — what the password opened for the worst
        segment: ``"full"``, ``"level-1"`` or ``"none"``; the highest stored
        ``section2_level`` / ``section3_level`` (0 = plain); and how many
        segments' ``section2_locked`` / ``section3_locked`` stayed closed and
        how many ``record_bodies_locked``.
    time_offset : dict
        meflib's recording-time offset (RTO). ``status``: ``"none"``,
        ``"applied"``, ``"disputed"`` (segments disagree), ``"locked"``
        (section 3 encrypted and not opened, so unknown) or ``"unknown"`` (no
        segment could be read). The status describes the READABLE segments;
        ``segments_unknown`` counts those that could not be read, whose
        offset is unknown (and which make dependent record times ``None``). ``date_recoverable`` is ``True``
        when an offset is set — the real date is then stored in section 3,
        and ``recoverable_by`` says by whom (``"anyone"`` when section 3 is
        plain, ``"level-2 password"`` when encrypted). ``ambiguous``: a time
        is stored non-negative under a non-zero offset. ``value`` holds the
        offset only with ``include_values=True``. ``interpretation`` says in
        plain words how the times were read. See ``docs/anonymization.md``.
    gmt_offset, dst_start_time, dst_end_time
        Section 3 time-zone fields (``None`` when locked or unset).
    start_time, end_time
        First / last sample over all channels.
    records_start_time, records_end_time
        Earliest / latest time any record mentions.
    first_time, last_time
        Samples and records together: the span a time shift must cover.
    channels : list of ChannelSummary
    record_only_channels : list of str
        Channels with records but no data yet.
    video_channels : list of str
        ``.vidd`` channels. mef3io does not read video; listed only.
    incomplete_channels : list of str
        Channel folders without one complete segment (a segment needs its
        ``.tmet``, ``.tidx`` and ``.tdat``). A :class:`mef3io.Reader` does not
        list them. Incomplete segment folders of other channels appear in
        their channel's ``segments`` as ``readable=False``; both raise an
        ``"incomplete-segments"`` warning.
    records : list of RecordSummary
        Session, then channel and segment records.
    identity : list of IdentityField
        Subject name/ID/location (section 3), descriptions (section 2),
        ``session_name`` / ``anonymized_name`` (every universal header) and
        record free text — presence and length, values only on request.
    warnings : list of SummaryWarning
    problems : list of dict
        What a ``strict=False`` inspection skipped, shaped like
        :attr:`mef3io.Reader.problems`.
    declaration_issues : list of dict
        Section-2 size declarations left unset (as
        :attr:`mef3io.Reader.declaration_issues`).
    index_read : bool
        Whether the block indices were read (``read_index=True``).
    legacy_password_mapping : bool
        The password opened some metadata or record file only through the
        password-to-key mapping of mef3io 1.2.0 and earlier (issue #84): the
        session was most likely encrypted by one of those versions with a
        non-ASCII password. The data is read correctly, but meflib and pymef
        cannot open those files with this password. A ``"legacy-password"``
        warning (in ``warnings``) says how many. :func:`inspect` reports this
        as data and does NOT emit :class:`mef3io.LegacyPasswordWarning`, as it
        reports every other condition (a :class:`mef3io.Reader` opened on the
        same session does emit it).
    offset_assumed_zero : bool
        Some time here assumes recording-time offset 0 because a section 3
        is locked (level-1 password, or none). Only a time stored negated
        (relative to the offset) needs the assumption; absolute stored times
        do not set it. The same flag is on every
        channel, segment and record it concerns. mef3io and pymef always
        write offset 0, so such times are right for almost every file; under
        meflib's deprecated date masking they are the masked times, which is
        what meflib meant a level-1 user to see. They are never called
        verified, and an ``"offset-assumed-zero"`` warning blocks
        anonymization (which needs the level-2 password anyway).
    """

    name: str
    path: str
    layout: str
    total_bytes: int
    archive_bytes: Optional[int]
    index_read: bool
    offset_assumed_zero: bool
    legacy_password_mapping: bool
    encryption: dict
    time_offset: dict
    gmt_offset: Optional[int]
    dst_start_time: Optional[int]
    dst_end_time: Optional[int]
    start_time: Optional[int]
    end_time: Optional[int]
    records_start_time: Optional[int]
    records_end_time: Optional[int]
    first_time: Optional[int]
    last_time: Optional[int]
    channels: list
    record_only_channels: list
    video_channels: list
    incomplete_channels: list
    records: list
    identity: list
    warnings: list
    problems: list
    declaration_issues: list

    @classmethod
    def _from(cls, d: dict) -> "SessionSummary":
        d = dict(d)
        d["channels"] = [ChannelSummary._from(c) for c in d["channels"]]
        d["records"] = [RecordSummary._from(r) for r in d["records"]]
        d["identity"] = [IdentityField._from(i) for i in d["identity"]]
        d["warnings"] = [SummaryWarning._from(w) for w in d["warnings"]]
        return cls(**_fields(cls, d))

    def channel(self, name: str) -> ChannelSummary:
        """The :class:`ChannelSummary` called ``name`` (``KeyError`` if none)."""
        for c in self.channels:
            if c.name == name:
                return c
        raise KeyError(name)

    def identity_field(self, name: str) -> IdentityField:
        """The :class:`IdentityField` called ``name`` (``KeyError`` if none)."""
        for f in self.identity:
            if f.name == name:
                return f
        raise KeyError(name)

    @property
    def anonymize_blockers(self) -> list:
        """The warnings ``anonymize_session`` refuses on."""
        return [w for w in self.warnings if w.blocks_anonymize]

    def to_dict(self) -> dict:
        """Plain nested dicts/lists, ready for ``json.dumps``. Gap spans
        become 2-element lists."""
        return dataclasses.asdict(self)

    def __str__(self) -> str:  # the CLI's text form
        return _render(self)


def inspect(
    path,
    password: str = "",
    *,
    include_values: bool = False,
    read_index: bool = False,
    strict: bool = True,
    cancel: Optional[CancelToken] = None,
    progress: Optional[Callable[[int, int], object]] = None,
) -> SessionSummary:
    """Describe a session without reading any sample data.

    Reads each segment's metadata (``.tmet``, the same bytes a
    :class:`mef3io.Reader` reads when it opens), the record files, and — only
    with ``read_index=True`` — the block indices. **It never reads a
    ``.tdat``**: those are measured by size only, so the cost does not grow
    with the amount of data recorded.

    Available in Python and C++ (``mef3io::inspect_session``) only; there is
    no MATLAB or C ABI equivalent.

    Parameters
    ----------
    path : str or os.PathLike
        A ``.mefd`` directory or a ``.mefd.tar`` archive (read in place).
    password : str, optional
        Opens encrypted sections. Without it an encrypted session is still
        described: what stays locked is reported (``encryption``, ``None``
        values, a ``"locked"`` warning) rather than guessed. A password that
        is given and wrong raises :class:`mef3io.PasswordError`. The level-2
        password opens everything; the level-1 password leaves section 3
        (subject fields, the recording-time offset) and level-2 record bodies
        locked. A non-ASCII password that matches only through the mapping of
        mef3io 1.2.0 and earlier is accepted, as the Reader accepts it, and
        flagged in ``legacy_password_mapping`` rather than warned about.
    include_values : bool, default False
        Return identity VALUES: subject name/ID/location, descriptions,
        ``session_name``/``anonymized_name``, record text and the full
        records, and the recording-time offset. Off by default, when identity
        fields report presence and length only (and
        ``ChannelSummary.channel_description`` is ``None``). Names and paths are NOT
        de-identified either way: the session's folder name (usually the same
        as ``session_name``), channel names and file paths are always
        returned, so a summary of an identifiable session is identifiable.
    read_index : bool, default False
        Also read every segment's block index (about 2% of the data) to find
        gaps inside segments. Off by default.
    strict : bool, default True
        As everywhere in mef3io, a segment or record file that cannot be read
        raises. ``False`` reports it in ``problems`` and an
        ``"unreadable-segments"`` / ``"unreadable-records"`` warning, marks
        the segment ``readable=False``, and describes the rest — the mode for
        an explorer that must show a damaged session.
    cancel : CancelToken, optional
        Checked before each segment and each record file. Once set, the call
        raises :class:`mef3io.Cancelled` and returns nothing.
    progress : callable, optional
        ``progress(done, total)`` in bytes of the files the segment pass reads
        (every ``.tmet``, plus every ``.tidx`` with ``read_index=True``;
        record files are not counted), called at a bounded rate and never
        concurrently. Sizing the total costs one stat per file, paid only when
        a callback is given.

    Returns
    -------
    SessionSummary

    Raises
    ------
    mef3io.IoError
        ``path`` is not a ``.mefd`` directory or ``.mefd.tar`` archive.
    mef3io.PasswordError
        A password was given and is wrong.
    mef3io.MefError
        (strict only) A segment or record file cannot be read.
    mef3io.Cancelled
        ``cancel`` was set.

    Examples
    --------
    >>> s = mef3io.inspect("session.mefd")
    >>> s.first_time, s.last_time            # samples AND annotations
    >>> s.identity_field("subject_id").present
    >>> [w.code for w in s.anonymize_blockers]
    """
    from . import _mef3io

    raw = _mef3io.inspect_session(
        str(path), password or "", bool(strict), bool(include_values), bool(read_index),
        **_hooks(cancel, progress),
    )
    return SessionSummary._from(raw)


# ----------------------------------------------------------------------- CLI
def _t(us: Optional[int]) -> str:
    if us is None:
        return "unknown"
    try:
        return _dt.datetime.fromtimestamp(us / 1e6, _dt.timezone.utc).strftime(
            "%Y-%m-%d %H:%M:%S.%f UTC")
    except (OverflowError, OSError, ValueError):
        return f"{us} us"


def _size(n: int) -> str:
    for unit in ("B", "KB", "MB", "GB", "TB"):
        if n < 1000 or unit == "TB":
            return f"{n:.0f} {unit}" if unit == "B" else f"{n:.1f} {unit}"
        n /= 1000
    return f"{n} B"  # pragma: no cover


def _render(s: SessionSummary) -> str:
    out = [f"{s.name}  ({s.layout}, {_size(s.total_bytes)})", f"  path: {s.path}"]
    enc = s.encryption
    out.append("  encryption: " + ("none" if not enc["encrypted"] else
               f"yes, access {enc['access']} (section 2 level {enc['section2_level']}, "
               f"section 3 level {enc['section3_level']})"))
    to = s.time_offset
    out.append(f"  recording-time offset: {to['status']}"
               + (f", date recoverable by {to['recoverable_by']}" if to["date_recoverable"] else "")
               + (f", value {to['value']}" if to["value"] is not None else ""))
    if s.offset_assumed_zero:
        out.append("  NOTE: section 3 is locked; times marked * assume offset 0 (not verified)")
    mark = " *" if s.offset_assumed_zero else ""
    out.append(f"  samples: {_t(s.start_time)} -> {_t(s.end_time)}{mark}")
    out.append(f"  records: {len(s.records)}"
               + (f", {_t(s.records_start_time)} -> {_t(s.records_end_time)}{mark}"
                  if s.records_start_time is not None else ""))
    out.append(f"  overall: {_t(s.first_time)} -> {_t(s.last_time)}{mark}")
    out.append(f"  channels: {len(s.channels)}"
               + (f"; record-only: {', '.join(s.record_only_channels)}" if s.record_only_channels else "")
               + (f"; video: {', '.join(s.video_channels)}" if s.video_channels else ""))
    for c in s.channels:
        fs = "?" if c.sampling_frequency is None else f"{c.sampling_frequency:g} Hz"
        gaps = f"{len(c.gaps)} gap(s)" + ("" if c.gaps_complete else " between segments") \
            + ("" if c.gaps_certain else " (uncertain)")
        out.append(f"    {c.name}: {fs}, {len(c.segments)} segment(s), {gaps}, "
                   f"{_t(c.start_time)} -> {_t(c.end_time)}"
                   + (" *" if c.offset_assumed_zero else ""))
    out.append("  identity fields:")
    for f in s.identity:
        state = {True: "present", False: "empty",
                 None: "unknown (locked or unreadable)"}[f.present]
        line = f"    {f.name:<22} {state}"
        if f.occurrences:
            line += f" ({f.occurrences} place(s), up to {f.max_length} bytes)"
        if f.values:
            line += ": " + "; ".join(repr(v) for v in f.values)
        out.append(line)
    if s.warnings:
        out.append("  warnings:")
        for w in s.warnings:
            mark = " [blocks anonymize]" if w.blocks_anonymize else ""
            out.append(f"    {w.code}{mark}: {w.message}")
    if s.problems:
        out.append("  problems:")
        for p in s.problems:
            out.append(f"    {p['segment']}: {p['reason']}")
    return "\n".join(out)


def _main(argv: Sequence[str]) -> int:
    import argparse

    ap = argparse.ArgumentParser(
        prog="python -m mef3io inspect",
        description="Describe a MEF 3.0 session without reading its samples. Never writes.",
    )
    ap.add_argument("path", help="a .mefd directory or .mefd.tar archive")
    ap.add_argument("--password", default="", help="opens encrypted sections")
    ap.add_argument("--password-env", metavar="VAR",
                    help="read the password from this environment variable instead of the "
                         "command line, which is visible in ps output and shell history")
    ap.add_argument("--values", action="store_true",
                    help="show identity values (subject fields, descriptions, record text, "
                         "the recording-time offset); off by default")
    ap.add_argument("--index", action="store_true",
                    help="also read the block indices, to find gaps inside segments")
    ap.add_argument("--lenient", action="store_true",
                    help="report unreadable segments instead of failing")
    ap.add_argument("--json", action="store_true", help="print JSON instead of text")
    a = ap.parse_args(list(argv))
    password = a.password
    if a.password_env:
        import os

        if a.password:
            ap.error("pass --password or --password-env, not both")
        password = os.environ.get(a.password_env)
        if password is None:
            ap.error(f"environment variable {a.password_env} is not set")
        if password == "":
            ap.error(f"environment variable {a.password_env} is set but empty; "
                     "drop --password-env to inspect without a password")
    try:
        s = inspect(a.path, password, include_values=a.values, read_index=a.index,
                    strict=not a.lenient)
    except Exception as e:  # noqa: BLE001 - one plain line, not a traceback
        print(f"{type(e).__name__}: {e}", file=sys.stderr)
        return 1
    print(json.dumps(s.to_dict(), indent=2) if a.json else str(s))
    return 0
