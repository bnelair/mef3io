"""Merging several recordings into one (#103).

:func:`merge_sessions` writes ONE new session from two or more: recordings of
the same patient that follow each other, take turns, or hold different
channels of the same period. Every time and every sample is copied as stored
(the transform's rules, :mod:`mef3io.transform`); the sources are never
modified. :func:`check` runs every check it makes before writing, without
writing. :func:`verify` checks a merged session against its sources.

Every refusal carries a stable ``code`` (see :data:`REFUSAL_CODES` and
docs/merge.md).

Available in Python and C++. There is no MATLAB binding and no C ABI for it.
"""
from __future__ import annotations

import argparse
import json
import os
from dataclasses import asdict, dataclass, field
from typing import Callable, Dict, Mapping, Optional, Sequence, Tuple

from ._cancel import CancelToken, _hooks
from ._copy import (
    CheckReport,
    VerifyReport,
    _backend,
    _check_report,
    _cli_error,
    _copy_arguments,
    _error,
    _Refusals,
    _warn_output_passwords,
)
from .transform import REFUSAL_CODES as _TRANSFORM_CODES
from .transform import _metadata, _pairs

__all__ = ["MergeReport", "REFUSAL_CODES", "check", "merge_sessions", "verify"]

#: Every refusal code: ``code -> (exception type, where it is decided)``, as in
#: :data:`mef3io.transform.REFUSAL_CODES`, plus the merge's own.
#: docs/merge.md explains each.
REFUSAL_CODES: Dict[str, Tuple[str, str]] = {
    **_TRANSFORM_CODES,
    "merge-too-few": ("ValueError", "check"),
    "merge-passwords-count": ("ValueError", "check"),
    "merge-time-range": ("ValueError", "check"),
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
    segments : int
        Segments of the result.
    segments_split : int
        Source segments split in two or more pieces because another source's
        data lies in one of their gaps.
    blocks, records, blocks_reencrypted, blocks_decrypted : int
    seizure_channel_entries_dropped, metadata_fields_changed : int
    bytes_written : int
    output_encrypted : bool
    not_copied, notes : tuple of str
    verification : VerifyReport
        The check the result passed before it was published.
    """

    destination: str
    session_name: str
    sources: Tuple[str, ...]
    channels_kept: Tuple[str, ...]
    channels_excluded: Tuple[str, ...]
    segments: int
    segments_split: int
    blocks: int
    blocks_reencrypted: int
    blocks_decrypted: int
    records: int
    seizure_channel_entries_dropped: int
    metadata_fields_changed: int
    bytes_written: int
    output_encrypted: bool
    not_copied: Tuple[str, ...]
    notes: Tuple[str, ...]
    verification: VerifyReport = field(repr=False)

    @classmethod
    def _from(cls, d: dict) -> "MergeReport":
        keep = {f for f in cls.__dataclass_fields__}
        d = {k: v for k, v in d.items() if k in keep}
        d["verification"] = VerifyReport._from(d["verification"])
        for k in ("sources", "channels_kept", "channels_excluded", "not_copied", "notes"):
            d[k] = tuple(d[k])
        return cls(**d)

    def to_dict(self) -> dict:
        """Plain ``dict`` (JSON-serialisable)."""
        return asdict(self)

    def summary(self) -> str:
        """A few lines for a terminal."""
        lines = [
            f"wrote {self.destination} from {len(self.sources)} recording(s): "
            f"{len(self.channels_kept)} channel(s), {self.segments} segment(s), "
            f"{self.blocks} block(s), {self.records} record(s)",
            "  every time and sample copied as stored",
        ]
        if self.segments_split:
            lines.append(f"  {self.segments_split} segment(s) split around another recording's data")
        if self.channels_excluded:
            lines.append(f"  channels left out: {', '.join(self.channels_excluded)}")
        for n in self.notes:
            lines.append(f"  note: {n}")
        lines.append(self.verification.summary())
        return "\n".join(lines)


def _arguments(refusals: _Refusals, *, sources, passwords, channels, exclude_channels,
               output_password, metadata) -> dict:
    if isinstance(sources, (str, bytes, os.PathLike)) or not all(
            isinstance(s, (str, os.PathLike)) for s in sources):
        raise _error("TypeError", "argument-type", "sources must be a list of session paths",
                     field="sources")
    pw = [] if passwords is None else list(passwords)
    if not all(isinstance(p, str) for p in pw):
        raise _error("TypeError", "argument-type", "passwords must be strings", field="passwords")
    a = _copy_arguments(refusals, channels=channels, exclude_channels=exclude_channels,
                        output_password=output_password)
    for k in ("time_range", "exact"):  # not combined with a merge yet
        a.pop(k)
    return dict(sources=[os.fspath(s) for s in sources], passwords=pw, **a,
                **_metadata(metadata, refusals))


def merge_sessions(
    sources: Sequence,
    dst,
    *,
    passwords: Optional[Sequence[str]] = None,
    channels: Optional[Sequence[str]] = None,
    exclude_channels: Optional[Sequence[str]] = None,
    output_password: Optional[Sequence[str]] = None,
    metadata: Optional[Mapping] = None,
    progress: Optional[Callable[[int, int], object]] = None,
    cancel: Optional[CancelToken] = None,
) -> MergeReport:
    """Merge two or more recordings into ONE new session.

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
      the result with ``metadata`` to merge anyway.

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
    metadata : mapping, optional
        Descriptive fields for the result, as for
        :func:`mef3io.transform_session`.
    progress, cancel : optional
        As for :func:`mef3io.transform_session`.

    Returns
    -------
    MergeReport
    """
    a = _arguments(_Refusals(collect=False), sources=sources, passwords=passwords,
                   channels=channels, exclude_channels=exclude_channels,
                   output_password=output_password, metadata=metadata)
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
    cancel: Optional[CancelToken] = None,
) -> CheckReport:
    """Every check :func:`merge_sessions` makes before it writes, without writing.

    Reads only metadata and the block indexes of segments that overlap in
    time across sources; never a ``.tdat``.
    """
    refusals = _Refusals(collect=True)
    a = _arguments(refusals, sources=sources, passwords=passwords, channels=channels,
                   exclude_channels=exclude_channels, output_password=output_password,
                   metadata=metadata)
    raw = _backend().check_merge(dst=os.fspath(dst), **a, **_hooks(cancel, None))
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
    progress: Optional[Callable[[int, int], object]] = None,
    cancel: Optional[CancelToken] = None,
) -> VerifyReport:
    """Check a merged session against its sources. Reads all; writes nothing.

    Pass the arguments the result was made with. Every segment of the result
    is checked against the piece of the source segment it came from (blocks
    byte-identical, metadata as the source's plus the edits); annotations
    against the sources' merged in time order; segments must not overlap; and
    the validator must report nothing on the result.
    """
    a = _arguments(_Refusals(collect=False), sources=sources, passwords=passwords,
                   channels=channels, exclude_channels=exclude_channels,
                   output_password=output_password, metadata=metadata)
    raw = _backend().verify_merge(dst=os.fspath(dst), **a, **_hooks(cancel, progress))
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
            metadata=metadata)
    except (RuntimeError, ValueError, TypeError, OSError) as exc:
        return _cli_error(exc)
    print(json.dumps(report.to_dict(), indent=2) if args.json else report.summary())
    return 0
