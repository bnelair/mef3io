"""Modified copies of a session (#73).

:func:`transform_session` writes a **copy** of a session with a choice of
channels, new passwords (or none), a new session name, and edited
descriptive metadata. Every time and every sample is copied as stored; the
original is never modified. :func:`check` runs every check it makes before
writing, without writing. :func:`verify` checks a copy against its source.

It is the same copy engine as :func:`mef3io.anonymize_session`, with
different rules: nothing is shifted or removed unless you ask for it.

Every refusal carries a stable ``code`` (see :data:`REFUSAL_CODES` and
docs/transform.md): as ``.code`` on the raised exception, and in
:class:`CheckReport`.

Available in Python and C++. There is no MATLAB binding and no C ABI for it.
"""
from __future__ import annotations

import argparse
import json
from dataclasses import asdict, dataclass, field
from typing import Callable, Dict, Mapping, Optional, Sequence, Tuple

from ._cancel import CancelToken, _hooks
from ._copy import (
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
    _error,
    _fspaths,
    _Refusals,
    _warn_output_passwords,
)

__all__ = [
    "CheckReport",
    "METADATA_FIELDS",
    "REFUSAL_CODES",
    "Refusal",
    "TransformReport",
    "VerifyIssue",
    "VerifyReport",
    "check",
    "transform_session",
    "verify",
]

#: Every refusal code: ``code -> (exception type, where it is decided)``;
#: ``"check"``, ``"copy"`` and ``"defensive"`` as in
#: :data:`mef3io.anonymize.REFUSAL_CODES`. docs/transform.md explains each.
REFUSAL_CODES: Dict[str, Tuple[str, str]] = {
    **COPY_REFUSAL_CODES,  # every copy's (mef3io._copy)
    # what only a transform checks
    "metadata-unknown": ("ValueError", "check"),
    "metadata-too-long": ("ValueError", "check"),
    "metadata-invalid": ("ValueError", "check"),
    "metadata-channel-unknown": ("ValueError", "check"),
}

#: The descriptive fields ``metadata=`` can set, and the stored size of each
#: (in UTF-8 bytes, terminator included). The first four are in metadata
#: section 3, the rest in section 2.
METADATA_FIELDS: Dict[str, int] = {
    "subject_name_1": 128,
    "subject_name_2": 128,
    "subject_id": 128,
    "recording_location": 512,
    "session_description": 2048,
    "channel_descriptions": 2048,
    "reference_descriptions": 2048,
}
_PER_CHANNEL = ("channel_descriptions", "reference_descriptions")


@dataclass(frozen=True)
class TransformReport:
    """Result of :func:`transform_session`.

    Attributes
    ----------
    destination : str
        The copy.
    session_name : str
        The copy's session name (its folder's stem).
    channels_kept, channels_excluded : tuple of str
    segments, blocks, records : int
        What was copied.
    blocks_reencrypted, blocks_decrypted : int
        Data blocks the source had encrypted, written encrypted with the new
        passwords, or decrypted because the copy has none.
    seizure_channel_entries_dropped : int
        Per-channel entries of ``Seiz`` records that named a left-out channel.
    metadata_fields_changed : int
        Metadata fields whose stored value changed (one per field per
        segment).
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
    channels_kept: Tuple[str, ...]
    channels_excluded: Tuple[str, ...]
    segments: int
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
    def _from(cls, d: dict) -> "TransformReport":
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
            "  every time and sample copied as stored",
        ]
        if self.channels_excluded:
            lines.append(f"  channels left out: {', '.join(self.channels_excluded)}")
        if self.metadata_fields_changed:
            lines.append(f"  {self.metadata_fields_changed} metadata field(s) changed")
        if self.blocks_reencrypted or self.blocks_decrypted:
            lines.append(
                f"  encrypted data blocks: {self.blocks_reencrypted} re-encrypted, "
                f"{self.blocks_decrypted} decrypted"
            )
        if self.not_copied:
            lines.append(f"  {len(self.not_copied)} file(s) outside the MEF layout not copied")
        for n in self.notes:
            lines.append(f"  note: {n}")
        lines.append(self.verification.summary())
        return "\n".join(lines)


def _metadata(metadata: Optional[Mapping], refusals: _Refusals) -> dict:
    """``metadata=`` as the extension's keyword arguments (None = unchanged).

    Its type is checked here; whether each value fits its field, and whether
    a per-channel entry names a channel of the copy, is the C++ pre-flight's.
    """
    out = {k: None for k in METADATA_FIELDS if k not in _PER_CHANNEL}
    out.update({k: {} for k in _PER_CHANNEL})
    if metadata is None:
        return out
    if not isinstance(metadata, Mapping):
        raise _error("TypeError", "argument-type", "metadata must be a mapping of field -> value",
                     field="metadata")
    for key, value in metadata.items():
        if key not in METADATA_FIELDS:
            refusals.add("ValueError", "metadata-unknown",
                         f"metadata has no field {key!r}; it takes {sorted(METADATA_FIELDS)}",
                         field=str(key))
            continue
        if key in _PER_CHANNEL:
            if not isinstance(value, Mapping) or not all(
                    isinstance(c, str) and isinstance(v, str) for c, v in value.items()):
                raise _error("TypeError", "argument-type",
                             f"metadata[{key!r}] must map channel names to str", field=key)
            out[key] = dict(value)
        elif value is not None:
            if not isinstance(value, str):
                raise _error("TypeError", "argument-type", f"metadata[{key!r}] must be a str",
                             field=key)
            out[key] = value
    return out


def _arguments(refusals: _Refusals, *, channels, exclude_channels, output_password,
               metadata) -> dict:
    return dict(**_copy_arguments(refusals, channels=channels, exclude_channels=exclude_channels,
                                  output_password=output_password),
                **_metadata(metadata, refusals))


def transform_session(
    src,
    dst,
    *,
    channels: Optional[Sequence[str]] = None,
    exclude_channels: Optional[Sequence[str]] = None,
    output_password: Optional[Sequence[str]] = None,
    metadata: Optional[Mapping] = None,
    rename: bool = True,
    password: str = "",
    progress: Optional[Callable[[int, int], object]] = None,
    cancel: Optional[CancelToken] = None,
) -> TransformReport:
    """Write a modified copy of a session: a choice of channels, passwords, name and details.

    The source is never modified. The copy is built next to ``dst`` in
    ``<dst>.partial/``, checked with :func:`verify`, and only then moved to
    ``dst``. A failure, a refusal or a cancel leaves nothing behind.

    **What stays as stored.** Every time: universal headers, block index,
    data blocks, records, the recording-time offset and the time-zone
    fields, so no date is moved or revealed. Every sample: data blocks are
    copied without being decoded, only the 16 encrypted statistics bytes of
    an encrypted block are re-keyed. The regions other programs fill in MEF
    headers. Record bodies (except the ``Seiz`` entries of left-out
    channels).

    **What changes.** Only what you ask for: the channels (``channels`` or
    ``exclude_channels``), the passwords (``output_password``; the source's
    are never carried over), the session name (the stem of ``dst``, in every
    header and in the session's own annotation files) and the ``metadata``
    fields you set. Every file gets a new file UUID, and its provenance
    records the transform (operation 8) while keeping the link to the source
    file it came from.

    Parameters
    ----------
    src : str or path-like
        The session: a ``.mefd`` directory or a ``.mefd.tar`` archive (read
        in place).
    dst : str or path-like
        The copy, a new ``.mefd`` directory. Its stem becomes the session
        name.
    channels : sequence of str, optional
        Copy only these channels. Mutually exclusive with ``exclude_channels``.
    exclude_channels : sequence of str, optional
        Leave these channels out: no folder, no records, no entry in a
        ``Seiz`` record.
    output_password : (str, str), optional
        ``(level1, level2)`` to encrypt the copy, with the same pairing rule as
        :class:`Writer`; ``None`` (default) writes it unencrypted. Data blocks
        the source had encrypted are re-encrypted with these passwords at
        their level, or decrypted when there are none.
    metadata : mapping, optional
        Descriptive fields to set in the copy (:data:`METADATA_FIELDS`):
        ``subject_name_1``, ``subject_name_2``, ``subject_id``,
        ``recording_location``, ``session_description`` (each a str; ``""``
        clears it, ``None`` or absent keeps it), and ``channel_descriptions``
        / ``reference_descriptions`` (dicts from a channel of the copy to its
        str). Each value must fit its field (``ValueError`` otherwise; it is
        never cut). Fields not given are copied as stored.
    rename : bool, optional
        ``True`` (default) gives the copy the stem of ``dst`` as its session
        name. ``False`` keeps the source's session name, and the stem of
        ``dst`` must equal it.
    password : str, optional
        Opens an encrypted source. It must grant level-2 (full) access.
    progress : callable, optional
        ``progress(done, total)`` in bytes (the copy and its verification).
    cancel : CancelToken, optional
        Stop the copy from another thread; :class:`Cancelled` is raised and
        nothing is left behind.

    Returns
    -------
    TransformReport

    Raises
    ------
    ValueError
        A bad argument: an unknown channel, both ``channels`` and
        ``exclude_channels``, a selection that leaves no channel, an unpaired
        output password, ``rename=False`` with a differently named ``dst``, a
        metadata field that is unknown, too long, or names a channel not in
        the copy.
    TypeError
        A channel list or a metadata value of the wrong type.
    WriteConflictError
        Parts of the session cannot be copied whole (video channels,
        unreadable or incomplete segments or annotation files), or the copy
        failed verification.
    PasswordError
        The source is encrypted and the password does not grant level 2.
    IoError
        ``dst`` or ``<dst>.partial`` exists, or ``dst`` is inside ``src``.
    FormatError, CrcError
        Damaged source data (never re-sealed into the copy).

    Every refusal carries ``.code`` (:data:`REFUSAL_CODES`) and ``.details``
    (``channel``, ``field``, ``inspect_code``), as for
    :func:`mef3io.anonymize_session`. :func:`check` returns the same refusals
    without writing.

    Examples
    --------
    >>> rep = mef3io.transform_session(
    ...     "patient.mefd", "patient_eeg.mefd", exclude_channels=["ECG"],
    ...     metadata={"channel_descriptions": {"Fp1": "frontal pole, left"}})
    >>> rep.verification.ok
    True
    """
    a = _arguments(_Refusals(collect=False), channels=channels, exclude_channels=exclude_channels,
                   output_password=output_password, metadata=metadata)
    _warn_output_passwords(a)
    raw = _backend().transform_session(
        *_fspaths(src, dst), rename=bool(rename), password=password or "", **a,
        **_hooks(cancel, progress))
    return TransformReport._from(raw)


def check(
    src,
    dst,
    *,
    channels: Optional[Sequence[str]] = None,
    exclude_channels: Optional[Sequence[str]] = None,
    output_password: Optional[Sequence[str]] = None,
    metadata: Optional[Mapping] = None,
    rename: bool = True,
    password: str = "",
    cancel: Optional[CancelToken] = None,
) -> CheckReport:
    """Every check :func:`transform_session` makes before it writes, without writing.

    Takes the same arguments (no ``progress``) and runs the same checks,
    through the same pre-flight code in the C++ core, so the two cannot
    disagree. Instead of raising the first refusal it returns all of them,
    each with its stable ``code`` (:data:`REFUSAL_CODES`). It writes nothing
    and reads only metadata (never a ``.tdat``). What it cannot decide: a
    data block failing its CRC (``block-crc``, ``data-damaged``) and the
    final verification (``verify-failed``).

    Returns
    -------
    CheckReport
        ``pseudonym`` is always "" (a transform has none).
    """
    refusals = _Refusals(collect=True)
    a = _arguments(refusals, channels=channels, exclude_channels=exclude_channels,
                   output_password=output_password, metadata=metadata)
    raw = _backend().check_transform(*_fspaths(src, dst), rename=bool(rename),
                                     password=password or "", **a, **_hooks(cancel, None))
    return _check_report(raw, refusals, a)


def verify(
    src,
    dst,
    *,
    channels: Optional[Sequence[str]] = None,
    exclude_channels: Optional[Sequence[str]] = None,
    output_password: Optional[Sequence[str]] = None,
    metadata: Optional[Mapping] = None,
    password: str = "",
    progress: Optional[Callable[[int, int], object]] = None,
    cancel: Optional[CancelToken] = None,
) -> VerifyReport:
    """Check a transformed copy against its source. Reads both; writes nothing.

    :func:`transform_session` runs this on every copy before publishing it.
    Pass the ``metadata`` (and ``output_password``) the copy was made with.
    The channels are read from the copy; ``channels`` and
    ``exclude_channels`` are accepted for symmetry and not needed.

    It checks that the validator reports nothing new; the copy holds a subset
    of the source's channels, segment for segment; every data block's
    payload and statistics (compared decrypted) are byte-identical; every
    time equals the source's; every other byte of every header, metadata
    section and record body equals the source's, except the names (when
    renamed), UUIDs, password fields, encryption flags, provenance and CRCs,
    and the metadata fields, which hold exactly the requested edits.

    Returns
    -------
    VerifyReport
    """
    del channels, exclude_channels  # read from the copy
    refusals = _Refusals(collect=False)
    a = _arguments(refusals, channels=None, exclude_channels=None,
                   output_password=output_password, metadata=metadata)
    raw = _backend().verify_transform(*_fspaths(src, dst), password=password or "", **a,
                                      **_hooks(cancel, progress))
    return VerifyReport._from(raw)


# --------------------------------------------------------------------------- CLI


def _build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="python -m mef3io transform",
        description="Write a modified copy of a session: a choice of channels, new passwords, a "
        "new name and edited details. Every time and sample is copied as stored; the source is "
        "never modified.",
    )
    p.add_argument("src", help="the session (.mefd directory or .mefd.tar archive)")
    p.add_argument("dst", help="the copy, a new .mefd directory (its name becomes the session name)")
    p.add_argument("--set", action="append", default=[], metavar="FIELD=VALUE",
                   help="set a session-wide metadata field (repeatable): "
                   + ", ".join(k for k in METADATA_FIELDS if k not in _PER_CHANNEL)
                   + "; an empty VALUE clears it")
    p.add_argument("--channel-description", action="append", default=[],
                   metavar="CHANNEL=TEXT", help="set one channel's description (repeatable)")
    p.add_argument("--reference-description", action="append", default=[],
                   metavar="CHANNEL=TEXT", help="set one channel's reference (repeatable)")
    _add_copy_cli_arguments(p)
    return p


def _pairs(parser, items, what) -> Dict[str, str]:
    out: Dict[str, str] = {}
    for item in items:
        key, sep, value = item.partition("=")
        if not sep or not key:
            parser.error(f"{what} takes NAME=VALUE, got {item!r}")
        out[key] = value
    return out


def _main(argv: Sequence[str] | None = None) -> int:
    parser = _build_parser()
    args = parser.parse_args(argv)
    password = _cli_password(parser, args)
    metadata: Dict[str, object] = dict(_pairs(parser, args.set, "--set"))
    if args.channel_description:
        metadata["channel_descriptions"] = _pairs(parser, args.channel_description,
                                                  "--channel-description")
    if args.reference_description:
        metadata["reference_descriptions"] = _pairs(parser, args.reference_description,
                                                    "--reference-description")
    try:
        report = transform_session(args.src, args.dst, metadata=metadata or None,
                                   **_cli_copy_kwargs(args, password))
    except (RuntimeError, ValueError, TypeError, OSError) as exc:
        return _cli_error(exc)
    print(json.dumps(report.to_dict(), indent=2) if args.json else report.summary())
    return 0
