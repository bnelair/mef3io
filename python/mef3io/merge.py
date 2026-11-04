"""Merging several recordings into one (#103).

:func:`merge_sessions` writes ONE new session from two or more: recordings of
the same patient that follow each other, take turns, or hold different
channels of the same period. Every time and every sample is copied as stored
(the transform's rules, :mod:`mef3io.transform`); the sources are never
modified. :func:`check` runs every check it makes before writing, without
writing. :func:`verify` checks a merged session against its sources.

ONE call can do the whole pipeline: leave channels out (``channels`` /
``exclude_channels``), trim each recording to its own time range
(``time_ranges``), make the result anonymous (``anonymize``), leave the
annotations out (``records="drop"``) and write one new session. The copy reads
each source byte once and writes it once, on ``n_threads`` threads.

Every refusal carries a stable ``code`` (see :data:`REFUSAL_CODES` and
docs/merge.md).

Available in Python and C++. There is no MATLAB binding and no C ABI for it.
"""
from __future__ import annotations

import argparse
import copy as _copy
import json
import os
from dataclasses import asdict, dataclass, field, fields, replace
from typing import Callable, Dict, Mapping, Optional, Sequence, Tuple, Union

from ._cancel import CancelToken, _hooks
from ._copy import (
    CheckReport,
    _time_range,
    VerifyReport,
    _backend,
    _check_report,
    _cli_error,
    _copy_arguments,
    _error,
    _Refusals,
    _warn_output_passwords,
)
from .anonymize import _CLEAR_CHOICES, _clear_flags
from .anonymize import REFUSAL_CODES as _ANONYMIZE_CODES
from .transform import REFUSAL_CODES as _TRANSFORM_CODES
from .transform import _metadata, _pairs

__all__ = ["MergeReport", "REFUSAL_CODES", "check", "merge_sessions", "verify"]

#: Every refusal code: ``code -> (exception type, where it is decided)``, as in
#: :data:`mef3io.transform.REFUSAL_CODES`, the anonymization's own (they apply
#: with ``anonymize=``; docs/anonymization.md), plus the merge's own.
#: docs/merge.md explains the merge's.
REFUSAL_CODES: Dict[str, Tuple[str, str]] = {
    **_TRANSFORM_CODES,
    **_ANONYMIZE_CODES,
    "merge-too-few": ("ValueError", "check"),
    "merge-passwords-count": ("ValueError", "check"),
    "merge-time-range": ("ValueError", "check"),
    "merge-time-ranges-count": ("ValueError", "check"),
    "merge-anonymize-metadata": ("ValueError", "check"),
    "merge-threads-invalid": ("ValueError", "check"),
    "merge-rename": ("ValueError", "check"),
    "merge-rto-differs": ("WriteConflictError", "check"),
    "merge-rate-differs": ("WriteConflictError", "check"),
    "merge-identity-differs": ("WriteConflictError", "check"),
    "merge-overlap": ("WriteConflictError", "check"),
}


@dataclass(frozen=True)
class MergeReport:
    """Result of :func:`merge_sessions`.

    Attributes
    ----------
    destination, session_name : str
    sources : tuple of str
        As given.
    channels_kept, channels_excluded : tuple of str
        Over all sources.
    channels_without_samples : tuple of str
        ``time_ranges``: channels left out because no source has a sample of
        them inside its range (also in ``channels_excluded``).
    segments : int
        Segments of the result.
    segments_split : int
        Source segments split in two or more pieces because another source's
        data lies in one of their gaps.
    blocks, records, blocks_reencrypted, blocks_decrypted : int
    blocks_trimmed, records_outside_window : int
        ``time_ranges``: edge blocks re-encoded to a range, and annotations
        left out for lying outside their source's range.
    records_dropped : int
        ``records="drop"``: annotations of the channels and segments the copy
        keeps (and the session's own) that were left out; ``records`` is then 0.
        Those of a channel the copy leaves out are not counted.
    seizure_channel_entries_dropped, metadata_fields_changed : int
    bytes_written : int
    output_encrypted : bool
    not_copied, notes : tuple of str
    verification : VerifyReport
        The check the result passed before it was published.
    pseudonym : str
        ``anonymize=``: the value written to every ``anonymized_name`` (empty
        otherwise).
    shift_us : int or None
        ``anonymize=``: the one shift applied to every time of every source;
        ``None`` when the times were kept (or without ``anonymize``).
    source_recording_time_offset : int
        ``anonymize=``: the sources' shared recording-time offset (0 without).
    record_text_fields_changed, record_bodies_cleared : int
        ``anonymize=``: record text fields the text policy and the name
        redaction changed; record bodies of an unknown type or version not
        carried over.
    source_start_us, source_end_us : int or None
    source_channel_times : dict of str to (int or None, int or None)
        ``anonymize=``: THE RE-IDENTIFICATION KEY over all sources (earliest
        first sample, latest end, and the same per kept channel). Handled
        exactly as in :class:`mef3io.anonymize.AnonymizeReport`: in memory
        only, out of ``repr()``, :meth:`summary`, :meth:`to_dict` (unless
        ``include_original_times=True``) and pickles.
    """

    destination: str
    session_name: str
    sources: Tuple[str, ...]
    channels_kept: Tuple[str, ...]
    channels_excluded: Tuple[str, ...]
    channels_without_samples: Tuple[str, ...]
    segments: int
    segments_split: int
    blocks: int
    blocks_reencrypted: int
    blocks_decrypted: int
    blocks_trimmed: int
    records: int
    records_outside_window: int
    seizure_channel_entries_dropped: int
    metadata_fields_changed: int
    bytes_written: int
    output_encrypted: bool
    not_copied: Tuple[str, ...]
    notes: Tuple[str, ...]
    pseudonym: str
    shift_us: Optional[int]
    source_recording_time_offset: int
    record_text_fields_changed: int
    record_bodies_cleared: int
    verification: VerifyReport = field(repr=False)
    # The re-identification key: out of repr() (see above).
    source_start_us: Optional[int] = field(default=None, repr=False)
    source_end_us: Optional[int] = field(default=None, repr=False)
    source_channel_times: Dict[str, Tuple[Optional[int], Optional[int]]] = field(
        default_factory=dict, repr=False, compare=False
    )
    # Last, with a default: a report from 2.1.1 (no such key) still loads.
    records_dropped: int = 0

    @classmethod
    def _from(cls, d: dict) -> "MergeReport":
        keep = {f for f in cls.__dataclass_fields__}
        d = {k: v for k, v in d.items() if k in keep}
        d["verification"] = VerifyReport._from(d["verification"])
        for k in ("sources", "channels_kept", "channels_excluded", "channels_without_samples",
                  "not_copied", "notes"):
            d[k] = tuple(d[k])
        d["source_channel_times"] = {k: tuple(v) for k, v in d.get("source_channel_times", {}).items()}
        return cls(**d)

    def with_original_times_in_pickle(self) -> "MergeReport":
        """A copy of this report whose pickle KEEPS the original times (see
        :meth:`mef3io.anonymize.AnonymizeReport.with_original_times_in_pickle`)."""
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
        """Plain ``dict`` (JSON-serialisable). The sources' original times (the
        re-identification key) are left out unless ``include_original_times``."""
        d = asdict(self)
        if not include_original_times:
            for k in ("source_start_us", "source_end_us", "source_channel_times"):
                del d[k]
        return d

    def summary(self) -> str:
        """A few lines for a terminal."""
        lines = [
            f"wrote {self.destination} from {len(self.sources)} recording(s): "
            f"{len(self.channels_kept)} channel(s), {self.segments} segment(s), "
            f"{self.blocks} block(s), {self.records} record(s)",
        ]
        if self.pseudonym:
            lines.append(
                (f"  every time shifted by {self.shift_us} us; " if self.shift_us is not None
                 else "  every time kept as recorded (the result shows the real dates); ")
                + "identity of every recording removed")
        else:
            lines.append("  every time and sample copied as stored")
        if self.blocks_trimmed or self.records_outside_window:
            lines.append(f"  time ranges: {self.blocks_trimmed} edge block(s) trimmed, "
                         f"{self.records_outside_window} annotation(s) outside the ranges left out")
        if self.segments_split:
            lines.append(f"  {self.segments_split} segment(s) split around another recording's data")
        if self.channels_excluded:
            lines.append(f"  channels left out: {', '.join(self.channels_excluded)}")
        if self.records_dropped:
            lines.append(f"  {self.records_dropped} annotation(s) left out")
        if self.record_bodies_cleared:
            lines.append(f"  {self.record_bodies_cleared} record bod(ies) of an unknown type or "
                         "version not carried over")
        for n in self.notes:
            lines.append(f"  note: {n}")
        lines.append(self.verification.summary())
        return "\n".join(lines)


def _unpickle_report(values: dict, keep: bool) -> MergeReport:
    out = MergeReport(**values)
    if keep:
        object.__setattr__(out, "_pickle_original_times", True)
    return out


_ANONYMIZE_KEYS = ("shift_us", "pseudonym", "clear", "text", "rename")


def _anonymize(anonymize, refusals: _Refusals) -> dict:
    """``anonymize=`` as the extension's arguments (anonymize_session's own)."""
    off = dict(anonymize=False, shift_us=None, pseudonym="", clear_subject=True,
               clear_location=True, clear_descriptions=True, text="drop", anonymize_rename=True,
               text_fn=None)
    if anonymize is None or anonymize is False:
        return off
    if anonymize is True:
        spec: Mapping = {}
    elif isinstance(anonymize, Mapping):
        spec = anonymize
    else:
        raise _error("TypeError", "argument-type",
                     "anonymize must be True or a mapping of anonymize_session's options "
                     f"({', '.join(_ANONYMIZE_KEYS)})", field="anonymize")
    unknown = set(spec) - set(_ANONYMIZE_KEYS)
    if unknown:
        raise _error("TypeError", "argument-type",
                     f"anonymize takes {', '.join(_ANONYMIZE_KEYS)}; unknown: {sorted(unknown)}",
                     field="anonymize")
    shift = spec.get("shift_us")
    if shift is not None and (isinstance(shift, bool) or not isinstance(shift, int)):
        raise _error("TypeError", "argument-type", "shift_us must be an integer or None",
                     field="shift_us")
    text = spec.get("text", "drop")
    text_fn = None
    if callable(text):
        text_mode, text_fn = "callback", text
    elif isinstance(text, str) and text in ("drop", "keep"):
        text_mode = text
    else:
        refusals.add("ValueError", "text-invalid", "text must be 'drop', 'keep' or a callable",
                     field="text")
        text_mode = "drop"
    return dict(
        anonymize=True, shift_us=shift, pseudonym=spec.get("pseudonym") or "",
        text=text_mode, text_fn=text_fn, anonymize_rename=bool(spec.get("rename", True)),
        **_clear_flags(spec.get("clear", _CLEAR_CHOICES), refusals))


def _ranges(time_ranges) -> list:
    """``time_ranges=`` as the extension's list (one pair or None per source)."""
    if time_ranges is None:
        return []
    if isinstance(time_ranges, (str, bytes)):
        raise _error("TypeError", "argument-type",
                     "time_ranges must be a list with one (start_us, end_us) or None per source",
                     field="time_ranges")
    out = []
    for r in time_ranges:
        out.append(None if r is None else _time_range(r))
    return out


def _arguments(refusals: _Refusals, *, sources, passwords, channels, exclude_channels,
               output_password, metadata, records="keep", time_ranges=None, exact=True,
               anonymize=None, n_threads=0) -> dict:
    if isinstance(sources, (str, bytes, os.PathLike)) or not all(
            isinstance(s, (str, os.PathLike)) for s in sources):
        raise _error("TypeError", "argument-type", "sources must be a list of session paths",
                     field="sources")
    pw = [] if passwords is None else list(passwords)
    if not all(isinstance(p, str) for p in pw):
        raise _error("TypeError", "argument-type", "passwords must be strings", field="passwords")
    a = _copy_arguments(refusals, channels=channels, exclude_channels=exclude_channels,
                        output_password=output_password, records=records, exact=exact)
    a.pop("time_range")  # a merge takes one per source (time_ranges)
    if isinstance(n_threads, bool) or not isinstance(n_threads, int):
        raise _error("TypeError", "argument-type", "n_threads must be an integer", field="n_threads")
    if n_threads < 0:
        refusals.add("ValueError", "merge-threads-invalid",
                     f"n_threads must be >= 0 (0 = automatic), not {n_threads}", field="n_threads")
        n_threads = 0
    return dict(sources=[os.fspath(s) for s in sources], passwords=pw,
                time_ranges=_ranges(time_ranges), n_threads=n_threads, **a,
                **_anonymize(anonymize, refusals), **_metadata(metadata, refusals))


def merge_sessions(
    sources: Sequence,
    dst,
    *,
    passwords: Optional[Sequence[str]] = None,
    channels: Optional[Sequence[str]] = None,
    exclude_channels: Optional[Sequence[str]] = None,
    output_password: Optional[Sequence[str]] = None,
    metadata: Optional[Mapping] = None,
    records: str = "keep",
    time_ranges: Optional[Sequence[Optional[Tuple[int, int]]]] = None,
    exact: bool = True,
    anonymize: Union[None, bool, Mapping] = None,
    n_threads: int = 0,
    progress: Optional[Callable[[int, int], object]] = None,
    cancel: Optional[CancelToken] = None,
) -> MergeReport:
    """Merge two or more recordings into ONE new session.

    One call can do the whole pipeline: leave channels out, trim each
    recording to its own time range, make the result anonymous, leave the
    annotations out, and write ONE new session. The copy pass reads each
    source byte once and writes it once (several channels at a time, see
    ``n_threads``); :func:`verify` then reads the sources and the result
    again before anything is published.

    The sources are never modified. The result is built next to ``dst`` in
    ``<dst>.partial/``, checked with :func:`verify` against every source, and
    only then moved to ``dst``. A failure, a refusal or a cancel leaves
    nothing behind.

    **What is refused before anything is written** (:func:`check` lists
    every one):

    * ``merge-overlap``: two sources hold samples of the same channel at the
      same moment. This is decided on the RECORDED data (from the block
      indexes), so recordings that take turns merge: A records and stops, B
      records and stops, A continues. A source's own slightly overlapping
      blocks (recorder jitter) never count.
    * ``merge-rate-differs``: a channel has different sampling rates.
    * ``merge-rto-differs``: the sources use different recording-time offsets.
    * ``merge-identity-differs``: the sources' subject fields (names, ID,
      location) differ, which may mean different patients. Set the fields for
      the result with ``metadata`` to merge anyway. With ``anonymize`` the
      fields it clears (``clear=``, default all) may differ: the result
      carries none of them.

    **How the result is laid out.** Each source segment becomes a segment of
    the result. A segment that has another source's data in one of its gaps
    is split there (at a block boundary, nothing decoded), so that the
    result's segments never overlap in time. They are ordered by time and
    numbered from 0 in each channel. Channels held by only some sources are
    kept. Annotations at the session and channel level are merged in time
    order; a segment's own annotations stay with it.

    Parameters
    ----------
    sources : sequence of str or path-like
        Two or more sessions (``.mefd`` directories or ``.mefd.tar`` archives).
    dst : str or path-like
        The result, a new ``.mefd`` directory; its stem is the session name.
    passwords : sequence of str, optional
        The password opening each source, in order (level 2 for encrypted
        ones). Omit for unencrypted sources.
    channels, exclude_channels : sequence of str, optional
        Which channels the result holds, over all sources.
    output_password : (str, str), optional
        Encrypt the result with these two passwords; ``None`` = unencrypted.
    records : str, optional
        ``"keep"`` (default) copies the annotations. ``"drop"`` leaves ALL of
        them out (session, channel and segment level): the copy has no
        ``.rdat``/``.ridx`` file, like a session that never had annotations;
        a channel that holds only annotations is left out too. The source's
        annotations are not copied or checked; the report's
        ``records_dropped`` counts them. Anything else is refused
        (``records-invalid``).
    metadata : mapping, optional
        Descriptive fields for the result, as for
        :func:`mef3io.transform_session`. Not combined with ``anonymize``
        (``merge-anonymize-metadata``).
    time_ranges : sequence, optional
        One entry per source, in order: ``(start_us, end_us)`` keeps only the
        samples of that source in ``[start, end)`` (uUTC as the SOURCE stores
        them), ``None`` keeps the whole source. The cut rules of
        :func:`mef3io.transform_session`'s ``time_range`` apply to each
        source: edge blocks are decoded, trimmed and re-encoded, annotations
        outside the range are left out. Overlap between sources is judged on
        the TRIMMED data, so two recordings that overlap only outside their
        ranges merge. A source with no sample in its range is refused
        (``time-range-empty``); a channel no source has a sample of in its
        range is left out (``channels_without_samples``). The number of
        entries must equal the number of sources (``merge-time-ranges-count``).
    exact : bool, optional
        With ``time_ranges``: ``True`` (default) trims the edge blocks to the
        range exactly; ``False`` keeps whole blocks (nothing is decoded).
    anonymize : True or mapping, optional
        Make the result anonymous in the same pass, with
        :func:`mef3io.anonymize_session`'s options: ``shift_us``,
        ``pseudonym``, ``clear`` and ``text`` (``rename`` only as ``True``:
        a merge is named after ``dst``). ``True`` = the defaults (times kept,
        identity cleared, record text dropped). ONE shift applies to every
        source, judged against the span of ALL of them (the whole
        recordings, trimmed or not). The identity of EVERY source (subject
        names, ID, location, session names, earlier pseudonyms) is removed
        and scanned for in the result, and the result is verified exactly as
        :func:`mef3io.anonymize.verify` verifies a copy. The report's
        ``pseudonym`` and ``shift_us`` say what was applied. A text callable
        is called serially, in a fixed order (the copy pass then uses one
        thread).
    n_threads : int, optional
        Threads for the whole operation (:doc:`threads </threads>`): opening
        the sources, copying channels, verifying. ``0`` (default) =
        automatic (at least 8, as for any file access), ``1`` = no extra
        threads. The result never depends on it: every data, index and
        metadata byte after the 1024-byte universal headers is identical
        (the headers hold fresh random UUIDs).
    progress, cancel : optional
        As for :func:`mef3io.transform_session`. ``progress`` is called at a
        bounded rate, never concurrently and never backwards, from whichever
        worker thread crosses a step (with the GIL held).

    Returns
    -------
    MergeReport
    """
    a = _arguments(_Refusals(collect=False), sources=sources, passwords=passwords,
                   channels=channels, exclude_channels=exclude_channels,
                   output_password=output_password, metadata=metadata, records=records,
                   time_ranges=time_ranges, exact=exact, anonymize=anonymize,
                   n_threads=n_threads)
    _warn_output_passwords(a)
    raw = _backend().merge_sessions(dst=os.fspath(dst), **a, **_hooks(cancel, progress))
    return MergeReport._from(raw)


def check(
    sources: Sequence,
    dst,
    *,
    passwords: Optional[Sequence[str]] = None,
    channels: Optional[Sequence[str]] = None,
    exclude_channels: Optional[Sequence[str]] = None,
    output_password: Optional[Sequence[str]] = None,
    metadata: Optional[Mapping] = None,
    records: str = "keep",
    time_ranges: Optional[Sequence[Optional[Tuple[int, int]]]] = None,
    exact: bool = True,
    anonymize: Union[None, bool, Mapping] = None,
    n_threads: int = 0,
    cancel: Optional[CancelToken] = None,
) -> CheckReport:
    """Every check :func:`merge_sessions` makes before it writes, without writing.

    Reads only metadata and the block indexes of segments that overlap in
    time across sources (with ``time_ranges``, of the blocks around each
    range); never a ``.tdat``. Takes the arguments of :func:`merge_sessions`.
    """
    refusals = _Refusals(collect=True)
    a = _arguments(refusals, sources=sources, passwords=passwords, channels=channels,
                   exclude_channels=exclude_channels, output_password=output_password,
                   metadata=metadata, records=records, time_ranges=time_ranges, exact=exact,
                   anonymize=anonymize, n_threads=n_threads)
    fn = a.pop("text_fn")
    raw = _backend().check_merge(dst=os.fspath(dst), **a, has_text_fn=fn is not None,
                                 **_hooks(cancel, None))
    return _check_report(raw, refusals, a)


def verify(
    sources: Sequence,
    dst,
    *,
    passwords: Optional[Sequence[str]] = None,
    channels: Optional[Sequence[str]] = None,
    exclude_channels: Optional[Sequence[str]] = None,
    output_password: Optional[Sequence[str]] = None,
    metadata: Optional[Mapping] = None,
    records: str = "keep",
    time_ranges: Optional[Sequence[Optional[Tuple[int, int]]]] = None,
    exact: bool = True,
    anonymize: Union[None, bool, Mapping] = None,
    n_threads: int = 0,
    progress: Optional[Callable[[int, int], object]] = None,
    cancel: Optional[CancelToken] = None,
) -> VerifyReport:
    """Check a merged session against its sources. Reads all; writes nothing.

    Pass the arguments the result was made with. Every segment of the result
    is checked against the piece of the source segment it came from (blocks
    byte-identical, metadata as the source's plus the edits); annotations
    against the sources' merged in time order; segments must not overlap; and
    the validator must report nothing on the result. With ``anonymize`` every
    time must be exactly the shift away and no text field or path may hold
    the identity of ANY source. ``time_ranges`` and ``anonymize`` are the
    ones the result was made with.
    """
    a = _arguments(_Refusals(collect=False), sources=sources, passwords=passwords,
                   channels=channels, exclude_channels=exclude_channels,
                   output_password=output_password, metadata=metadata, records=records,
                   time_ranges=time_ranges, exact=exact, anonymize=anonymize,
                   n_threads=n_threads)
    fn = a.pop("text_fn")
    raw = _backend().verify_merge(dst=os.fspath(dst), **a, has_text_fn=fn is not None,
                                  **_hooks(cancel, progress))
    return VerifyReport._from(raw)


# --------------------------------------------------------------------------- CLI


def _build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="python -m mef3io merge",
        description="Merge two or more recordings into one new session. Every time and sample "
        "is copied as stored; the sources are never modified.",
    )
    p.add_argument("dst", help="the result, a new .mefd directory (its name becomes the session name)")
    p.add_argument("sources", nargs="+", help="the recordings to merge (.mefd or .mefd.tar)")
    p.add_argument("--password", action="append", default=None,
                   help="the password of each source, in order (repeat once per source)")
    p.add_argument("--set", action="append", default=[], metavar="FIELD=VALUE",
                   help="set a session-wide metadata field of the result (repeatable)")
    p.add_argument("--output-password", nargs=2, metavar=("LEVEL1", "LEVEL2"),
                   help="encrypt the result with these two passwords")
    sel = p.add_mutually_exclusive_group()
    sel.add_argument("--channel", action="append", default=None,
                     help="keep only this channel (repeatable)")
    sel.add_argument("--exclude-channel", action="append", default=None,
                     help="leave this channel out (repeatable)")
    p.add_argument("--drop-records", action="store_true",
                   help="leave every annotation out of the result")
    p.add_argument("--json", action="store_true", help="print the report as JSON")
    return p


def _main(argv: Sequence[str] | None = None) -> int:
    parser = _build_parser()
    args = parser.parse_args(argv)
    metadata = dict(_pairs(parser, args.set, "--set")) or None
    try:
        report = merge_sessions(
            args.sources, args.dst, passwords=args.password, channels=args.channel,
            exclude_channels=args.exclude_channel,
            output_password=tuple(args.output_password) if args.output_password else None,
            metadata=metadata, records="drop" if args.drop_records else "keep")
    except (RuntimeError, ValueError, TypeError, OSError) as exc:
        return _cli_error(exc)
    print(json.dumps(report.to_dict(), indent=2) if args.json else report.summary())
    return 0
