"""De-identified copies of a session (#70).

:func:`anonymize_session` writes a **copy** of a session with every time
shifted by one constant and the patient's identity removed; the original is
never modified. :func:`check` runs every check it makes before writing,
without writing. :func:`verify` checks a copy against its source.

Every refusal carries a stable ``code`` (see :data:`REFUSAL_CODES` and
docs/anonymization.md): as ``.code`` on the raised exception, and in
:class:`CheckReport`.

Available in Python and C++. There is no MATLAB binding and no C ABI for it.
"""
from __future__ import annotations

import argparse
import datetime as _dt
import json
import copy as _copy
from dataclasses import asdict, dataclass, field, fields, replace
from typing import Callable, Dict, Iterable, List, Optional, Sequence, Tuple, Union

from ._cancel import CancelToken, _hooks
from ._copy import (  # shared with mef3io.transform (#73); re-exported here
    COPY_REFUSAL_CODES,
    CheckReport,
    Refusal,
    VerifyIssue,
    VerifyReport,
    _add_copy_cli_arguments,
    _backend,
    _check_report,
    _cli_copy_kwargs,
    _cli_error,
    _cli_password,
    _copy_arguments,
    _copy_output_password,
    _error,
    _fspaths,
    _Refusals,
    _warn_output_passwords,
)

__all__ = [
    "AnonymizeReport",
    "CheckReport",
    "REFUSAL_CODES",
    "Refusal",
    "VerifyIssue",
    "VerifyReport",
    "anonymize_session",
    "check",
    "redact_text",
    "reference_shift",
    "verify",
]

#: Microseconds in one day.
DAY_US = 86_400_000_000

_CLEAR_CHOICES = ("subject", "location", "descriptions")

#: Every refusal code: ``code -> (exception type, where it is decided)``.
#:
#: * ``"check"``: decided before anything is written; :func:`check` reports
#:   it and :func:`anonymize_session` raises it without writing.
#: * ``"copy"``: only the copy pass can meet it (it needs the data blocks,
#:   or the finished copy); :func:`anonymize_session` raises it and removes
#:   the partial copy. :func:`check` cannot report it.
#: * ``"defensive"``: guards a state an earlier check already rules out, or
#:   a fault of the environment (a full disk); kept so that every refusal
#:   has a code.
#:
#: docs/anonymization.md explains each one.
REFUSAL_CODES: Dict[str, Tuple[str, str]] = {
    **COPY_REFUSAL_CODES,  # every copy's (mef3io._copy)
    # what only anonymization checks or does
    "clear-unknown": ("ValueError", "check"),
    "text-invalid": ("ValueError", "check"),
    "shift-zero": ("ValueError", "check"),
    "text-callback-missing": ("ValueError", "defensive"),
    "rto-disputed": ("WriteConflictError", "check"),
    "rto-unreadable": ("WriteConflictError", "check"),
    "time-before-rto": ("WriteConflictError", "check"),
    "shift-below-floor": ("ValueError", "check"),
    "shift-not-positive": ("ValueError", "check"),
    "name-contains-identity": ("ValueError", "check"),
    "pseudonym-contains-identity": ("ValueError", "check"),
    "id-number-time": ("WriteConflictError", "check"),
    "time-overflow": ("FormatError", "defensive"),
    "text-unremovable": ("WriteConflictError", "check"),
}

#: ``text(value, info) -> str | None`` for ``text=`` callables. ``info`` is a
#: dict with ``record_type``, ``field``, ``level``, ``channel``, ``segment``
#: and ``time`` (the record's time in the copy, uUTC).
TextCallback = Callable[[str, dict], Optional[str]]


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
    source_start_us, source_end_us : int or None
        The SOURCE's first and last sample time, uUTC (None without samples).
    source_channel_times : dict of str to (int or None, int or None)
        The same for each kept channel (None for a channel with records only).

    Notes
    -----
    ``source_start_us``, ``source_end_us`` and ``source_channel_times`` are
    the **re-identification key**: with ``shift_us`` they give the real dates
    back. They are returned to the caller, in memory, and nowhere else:
    mef3io never writes them into the copy (the copy's leak scan refuses it
    if any appears), never logs them and never puts them in a warning or a
    message. They are left out of ``repr()``, :meth:`summary`, :meth:`to_dict`
    (unless ``include_original_times=True``) and **pickling**: a pickled
    report arrives without them unless it was marked with
    :meth:`with_original_times_in_pickle` (for example to pass it to a helper
    process you control; never to a cache). ``copy.copy`` and
    ``copy.deepcopy`` keep them. ``dataclasses.asdict(report)`` INCLUDES
    them: prefer :meth:`to_dict`. Store them as you would the original
    recording.
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
    # The re-identification key: out of repr() (see Notes).
    source_start_us: Optional[int] = field(default=None, repr=False)
    source_end_us: Optional[int] = field(default=None, repr=False)
    source_channel_times: Dict[str, Tuple[Optional[int], Optional[int]]] = field(
        default_factory=dict, repr=False, compare=False
    )

    @classmethod
    def _from(cls, d: dict) -> "AnonymizeReport":
        d = dict(d)
        d["verification"] = VerifyReport._from(d["verification"])
        for k in ("channels_kept", "channels_excluded", "not_copied", "notes"):
            d[k] = tuple(d[k])
        d["source_channel_times"] = {k: tuple(v) for k, v in d["source_channel_times"].items()}
        return cls(**d)

    def with_original_times_in_pickle(self) -> "AnonymizeReport":
        """A copy of this report whose pickle KEEPS the original times.

        By default a pickled report drops them (the re-identification key).
        Use this only to hand the report to a process you control, never for
        a cache or a file.
        """
        out = replace(self)
        object.__setattr__(out, "_pickle_original_times", True)
        return out

    def __reduce__(self):
        values = {f.name: getattr(self, f.name) for f in fields(self)}
        keep = getattr(self, "_pickle_original_times", False)
        if not keep:
            values.update(source_start_us=None, source_end_us=None, source_channel_times={})
        return (_unpickle_report, (values, keep))

    def __copy__(self):
        out = replace(self)
        if getattr(self, "_pickle_original_times", False):
            object.__setattr__(out, "_pickle_original_times", True)
        return out

    def __deepcopy__(self, memo):
        values = {f.name: _copy.deepcopy(getattr(self, f.name), memo) for f in fields(self)}
        return _unpickle_report(values, getattr(self, "_pickle_original_times", False))

    def to_dict(self, *, include_original_times: bool = False) -> dict:
        """Plain ``dict`` (JSON-serialisable).

        The source's original times (the re-identification key) are left out
        unless ``include_original_times=True``, so that logging or sharing
        the dict cannot leak them by accident.
        """
        d = asdict(self)
        if not include_original_times:
            for k in ("source_start_us", "source_end_us", "source_channel_times"):
                del d[k]
        return d

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


def _unpickle_report(values: dict, keep: bool) -> AnonymizeReport:
    out = AnonymizeReport(**values)
    if keep:
        object.__setattr__(out, "_pickle_original_times", True)
    return out


def _clear_flags(clear: Iterable[str], refusals: Optional[_Refusals] = None) -> dict:
    if isinstance(clear, str):
        clear = (clear,)
    chosen = set(clear)
    unknown = chosen - set(_CLEAR_CHOICES)
    if unknown:
        (refusals or _Refusals(False)).add(
            "ValueError", "clear-unknown",
            f"clear= takes any of {_CLEAR_CHOICES}; unknown: {sorted(unknown)}", field="clear")
    return dict(
        clear_subject="subject" in chosen,
        clear_location="location" in chosen,
        clear_descriptions="descriptions" in chosen,
    )


def _arguments(refusals: _Refusals, *, channels, exclude_channels, output_password, text,
               clear) -> dict:
    """The checks made in Python, in the order anonymize_session makes them,
    and the extension's keyword arguments."""
    copy = _copy_arguments(refusals, channels=channels, exclude_channels=exclude_channels,
                           output_password=output_password)
    if callable(text):
        text_mode, text_fn = "callback", text
    elif isinstance(text, str) and text in ("drop", "keep"):
        text_mode, text_fn = text, None
    else:
        refusals.add("ValueError", "text-invalid", "text must be 'drop', 'keep' or a callable",
                     field="text")
        text_mode, text_fn = "drop", None
    return dict(text_mode=text_mode, text_fn=text_fn, **copy, **_clear_flags(clear, refusals))


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
        patient's names (or any of their words), ID or location, or the
        original session name (3+ characters anywhere, shorter values as a
        whole word, ignoring case and Latin accents): ``ValueError`` before
        anything is written.
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
        ``dst``, a copy name or pseudonym holding the patient's identity.
    WriteConflictError
        The session cannot be copied without risking a misplaced or leaked
        date, or the copy failed verification.
    PasswordError
        The source is encrypted and the password does not grant level 2.
    IoError
        ``dst`` or ``<dst>.partial`` exists, or ``dst`` is inside ``src``.
    FormatError, CrcError
        Damaged source data (never re-sealed into the copy).

    Every refusal above carries ``.code``, a stable string from
    :data:`REFUSAL_CODES` (e.g. ``"dst-exists"``, ``"shift-below-floor"``,
    ``"name-contains-identity"``), and ``.details``, a dict with
    ``channel``, ``field`` and ``inspect_code`` (each ``None`` when it does
    not apply). Match on ``code``, never on the message. :func:`check` returns
    the same refusals without writing. Errors that are not refusals (a
    cancel, an exception raised by a callback, an internal error) carry no
    code. The refusals :func:`check` can report are all raised before
    anything is written; those marked ``"copy"`` in :data:`REFUSAL_CODES`
    come from the copy pass, which removes the partial copy.

    Examples
    --------
    >>> shift = mef3io.anonymize.reference_shift(start_us, "2000-01-03", tz="Europe/Prague")
    >>> rep = mef3io.anonymize_session("patient.mefd", "S001.mefd", shift)
    >>> rep.verification.ok
    True
    """
    a = _arguments(_Refusals(collect=False), channels=channels,
                   exclude_channels=exclude_channels, output_password=output_password,
                   text=text, clear=clear)
    _warn_output_passwords(a)
    text_mode = a.pop("text_mode")
    raw = _backend().anonymize_session(
        *_fspaths(src, dst),
        int(shift_us),
        pseudonym=pseudonym or "",
        text=text_mode,
        rename=bool(rename),
        password=password or "",
        **a,
        **_hooks(cancel, progress),
    )
    return AnonymizeReport._from(raw)


def check(
    src,
    dst,
    shift_us: Optional[int] = None,
    *,
    pseudonym: Optional[str] = None,
    clear: Iterable[str] = _CLEAR_CHOICES,
    text: Union[str, TextCallback] = "drop",
    rename: bool = True,
    password: str = "",
    output_password: Optional[Sequence[str]] = None,
    channels: Optional[Sequence[str]] = None,
    exclude_channels: Optional[Sequence[str]] = None,
    cancel: Optional[CancelToken] = None,
) -> CheckReport:
    """Every check :func:`anonymize_session` makes before it writes, without writing.

    Takes the same arguments as :func:`anonymize_session` (no ``progress``)
    and runs the same checks, through the same code: one pre-flight function
    in the C++ core serves both, so the two cannot disagree. Instead of
    raising the first refusal it returns all of them, each with its stable
    ``code`` (:data:`REFUSAL_CODES`).

    It writes nothing (no ``dst``, no ``<dst>.partial``) and reads only
    metadata: what :func:`mef3io.inspect` reads, plus each kept segment's
    1 KB metadata header and the kept channels' record files. It never reads
    the data (``.tdat``) files. The GIL is released while it runs.

    What it checks, in order: the options (shift zero, both ``channels`` and
    ``exclude_channels``, output passwords); the destination (exists, a
    leftover ``<dst>.partial``, not ``.mefd``, inside the source); the source
    (opens, password grants level 2, nothing :func:`mef3io.inspect` marks as
    blocking, including the recording-time-offset refusals); the channel
    selection; the shift (floor, times above 0); the copy's name and
    pseudonym (identity, length, ``rename=False``); segment numbering; record
    files (readable, index matches, an ``Epoc``/``Curs`` ``id_number``
    holding a time inside the recording).

    What it cannot decide (codes marked ``"copy"`` in :data:`REFUSAL_CODES`):
    a data block failing its CRC or running past its file (``block-crc``,
    ``data-damaged``), and the final verification of the copy
    (``verify-failed``), which needs the copy. Every time in the metadata
    headers and the record files is put through the copy's own shifting
    rule up front (``time-before-rto``, ``shift-not-positive``,
    ``time-overflow``); only a time in a block index or data file header, or
    a block's own time, is met by the copy pass alone, with the same code.

    No refusal message, here or raised, holds one of the source's times.

    Available in Python and C++ (``mef3io::check_anonymize``); not in MATLAB.

    Parameters
    ----------
    src, dst
        As for :func:`anonymize_session`.
    shift_us : int, optional
        The shift to check. ``None`` skips the shift checks (for a shift not
        chosen yet).
    pseudonym, clear, text, rename, password, output_password, channels, exclude_channels
        As for :func:`anonymize_session`. A ``text`` callable is never called.
    cancel : CancelToken, optional
        Stop the check from another thread; :class:`Cancelled` is raised.

    Returns
    -------
    CheckReport

    Raises
    ------
    TypeError
        A channel list that is not made of strings (code ``argument-type``).
    Cancelled
        ``cancel`` was set.

    Examples
    --------
    >>> rep = mef3io.anonymize.check("patient.mefd", "S001.mefd", shift, pseudonym="S001")
    >>> rep.ok, rep.codes
    (False, ('pseudonym-contains-identity',))
    """
    refusals = _Refusals(collect=True)
    a = _arguments(refusals, channels=channels, exclude_channels=exclude_channels,
                   output_password=output_password, text=text, clear=clear)
    for k in ("clear_subject", "clear_location", "clear_descriptions"):
        del a[k]  # nothing up front depends on them
    text_mode = a.pop("text_mode")
    text_fn = a.pop("text_fn")
    raw = _backend().check_anonymize(
        *_fspaths(src, dst),
        None if shift_us is None else int(shift_us),
        pseudonym=pseudonym or "",
        text=text_mode,
        has_text_fn=text_fn is not None,
        rename=bool(rename),
        password=password or "",
        **a,
        **_hooks(cancel, None),
    )
    return _check_report(raw, refusals, a)


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
    raw = _backend().verify_anonymized(
        *_fspaths(src, dst),
        int(shift_us),
        password=password or "",
        output_password=_copy_output_password(output_password),
        **_clear_flags(clear),
        **_hooks(cancel, progress),
    )
    return VerifyReport._from(raw)


def redact_text(
    src,
    dst,
    texts: Sequence[str],
    *,
    pseudonym: Optional[str] = None,
    clear: Iterable[str] = _CLEAR_CHOICES,
    rename: bool = True,
    password: str = "",
    channels: Optional[Sequence[str]] = None,
    exclude_channels: Optional[Sequence[str]] = None,
    cancel: Optional[CancelToken] = None,
) -> List[str]:
    """The text :func:`anonymize_session` would keep, for each of ``texts``.

    Runs the exact removal the copy applies to text it keeps (record text with
    ``text="keep"``, and the descriptions when ``"descriptions"`` is not in
    ``clear``), for the same source, destination name and options. An
    application can then compare a copy's text with what mef3io itself would
    have left, without copying mef3io's rules. It writes nothing, reads only
    metadata (what :func:`check` reads), and never logs or stores its inputs.

    It is not a safety check on the result: :func:`check` refuses text the
    removal cannot take out (code ``text-unremovable``), and :func:`verify`
    checks every copy.

    Parameters
    ----------
    src, dst
        As for :func:`anonymize_session`; ``dst`` gives the copy's name and
        need not exist (an existing one is fine: nothing is written).
    texts : sequence of str
        The texts to redact. Text that is not valid UTF-8 can be passed with
        ``errors="surrogateescape"``; it comes back the same way.
    pseudonym, clear, rename, password, channels, exclude_channels
        As for :func:`anonymize_session`.

    Returns
    -------
    list of str
        One redacted text per input, in order.

    Raises
    ------
    The first refusal that leaves the redaction unknown: the source cannot be
    read (``source-missing``, ``password-required``, ...) or ``dst`` is not a
    ``.mefd`` name (``dst-not-mefd``).
    """
    if isinstance(texts, str):
        raise _error("TypeError", "argument-type", "texts must be a sequence of str",
                     field="texts")
    items = list(texts)
    if not all(isinstance(t, str) for t in items):
        raise _error("TypeError", "argument-type", "texts must be str", field="texts")
    a = _copy_arguments(_Refusals(collect=False), channels=channels,
                        exclude_channels=exclude_channels, output_password=None)
    raw = _backend().redact_text(
        *_fspaths(src, dst),
        [t.encode("utf-8", "surrogateescape") for t in items],
        pseudonym=pseudonym or "",
        rename=bool(rename),
        password=password or "",
        channels=a["channels"],
        exclude_channels=a["exclude_channels"],
        **_clear_flags(clear),
        **_hooks(cancel, None),
    )
    return [bytes(b).decode("utf-8", "surrogateescape") for b in raw]


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
    _add_copy_cli_arguments(p)
    return p


def _main(argv: Sequence[str] | None = None) -> int:
    parser = _build_parser()
    args = parser.parse_args(argv)
    password = _cli_password(parser, args)
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
            **_cli_copy_kwargs(args, password),
        )
    except (RuntimeError, ValueError, OSError) as exc:
        return _cli_error(exc)
    if args.json:
        print(json.dumps(report.to_dict(), indent=2))
    else:
        print(report.summary())
    return 0
