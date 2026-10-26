"""High-level Python Writer wrapping the C++ SessionWriter.

Two write paths:
- ``write`` — float64 data; NaN marks discontinuities; ``precision`` sets the
  conversion factor (10**-precision), is inferred when omitted, or is
  ``"step"``: the data's own quantization step (#81).
- ``write_int32`` — the primitive path: integer counts plus a conversion factor
  (e.g. an amplifier's V/bit), stored verbatim.
"""
from __future__ import annotations

import contextlib
from typing import Optional

import numpy as np

from ._warnings import MefWarning

_INT32_MIN, _INT32_MAX = -(2**31), 2**31 - 1


class StepFallbackWarning(MefWarning):
    """``Writer.write(..., precision="step")`` on a NEW channel could not use a step.

    Raised once per such write. The data was still written, with the default
    (inferred power-of-ten) precision — what a default write stores: the
    data was not a whole multiple of one step (filtered or resampled data
    never is), showed too few distinct values to establish one (a DC level,
    a square wave: fewer than 16 distinct magnitudes in the sample), or its
    counts would not fit int32. The summary's ``step_fallback`` says which.

    (On a channel that already has data, ``"step"`` never falls back: data
    off the channel's factor is refused with ``WriteConflictError``.)

    ``w.message.details`` is ``[channel, reason]``; ``w.message.path`` the
    session.
    """


def _as_int32_counts(data):
    """Validate user input as int32 counts — never alter the values.

    The primitive path stores counts verbatim, so no silent conversion is
    allowed: floating-point data raises TypeError (quantization belongs to
    ``write()`` or to the caller, explicitly), and integer values outside
    the int32 range raise ValueError rather than wrapping.
    """
    a = np.atleast_1d(np.asarray(data))
    if not np.issubdtype(a.dtype, np.integer):
        raise TypeError(
            f"write_int32 stores counts verbatim and requires integer data "
            f"(e.g. int32); got dtype {a.dtype}. Convert explicitly, or use "
            f"write() for floating-point data."
        )
    if a.size and (int(a.min()) < _INT32_MIN or int(a.max()) > _INT32_MAX):
        raise ValueError(
            f"write_int32: values in [{a.min()}, {a.max()}] exceed the int32 "
            f"range and would wrap; rescale them or use write()"
        )
    return np.ascontiguousarray(a, dtype=np.int32)


class ClampedSamplesWarning(MefWarning):
    """A float write stored some samples as the nearest storable value.

    MEF stores int32 counts; a sample whose count at the conversion factor
    would leave that range, or that is ``+inf``/``-inf``, is stored as the
    largest count, ``+-(2**31 - 1)`` — meflib's own "positive/negative
    infinity" values (``RED_POSITIVE_INFINITY``/``RED_NEGATIVE_INFINITY``,
    meflib.h, which its ``RED_round`` clamps to as well) — so it reads back
    as ``+-(2**31 - 1) * factor``. The write succeeds; nothing wraps. Typical
    cause: a saturation artifact in an append to a channel whose factor is
    fine. ``NaN`` is never clamped: it stays a gap.

    Raised once per such write. ``w.message.details`` is
    ``({"channel": ..., "clamped": n, "infinite": k},)`` (``k`` of the ``n``
    were infinite); the text carries counts only, never sample values or
    times. The write summary has the same numbers (``clamped``,
    ``clamped_infinite``).
    """


def warn_clamped(channel, summary, stacklevel=3, path=None):
    """One :class:`ClampedSamplesWarning` when a float write clamped samples."""
    n = summary.get("clamped", 0)
    if n:
        from ._warnings import emit

        k = summary.get("clamped_infinite", 0)
        emit(ClampedSamplesWarning,
             f"{n} {'sample' if n == 1 else 'samples'} of channel {channel!r} {'was' if n == 1 else 'were'} outside what MEF can store "
             f"at the conversion factor ({k} of them infinite) and were stored as the "
             f"nearest storable value.",
             path=path, details=[{"channel": channel, "clamped": n, "infinite": k}],
             stacklevel=stacklevel)


def _is_step(precision) -> bool:
    """True for ``precision="step"``; ValueError for any other string."""
    if isinstance(precision, str):
        if precision != "step":
            raise ValueError(
                f"precision must be an int or 'step', not {precision!r}"
            )
        return True
    return False


def warn_step_fallback(channel, summary, stacklevel=3, path=None):
    """One :class:`StepFallbackWarning` when a ``precision="step"`` write fell back."""
    reason = summary.get("step_fallback")
    if reason:
        from ._warnings import emit

        emit(StepFallbackWarning,
             f"precision='step': channel {channel!r} was not stored on its own step: "
             f"{reason}.",
             path=path, details=[channel, reason], stacklevel=stacklevel)


def warn_unreadable_metadata(entries, stacklevel=3, path=None):
    """One warning for segment metadata a writer could not use (or [] = none)."""
    if entries:
        from ._reader import UnreadableSegmentWarning
        from ._warnings import emit

        emit(UnreadableSegmentWarning,
            f"{len(entries)} segment metadata problem(s) were skipped by the writer "
            f"(unreadable .tmet, or a segment directory without metadata): {entries[0]}. "
            "Full list in Writer.unreadable_metadata.",
            path=path, details=entries, stacklevel=stacklevel)


def password_interop_issue(password: str) -> "str | None":
    """Why a password will not behave as expected in other MEF readers, or None.

    The same rule :class:`Writer` (and :func:`mef3io.anonymize_session`) use
    for :class:`PasswordInteropWarning`, so an application can warn before it
    writes. A password has an issue when it has:

    * more than 15 characters: meflib and pymef refuse it, so only mef3io
      opens the session;
    * more than 15 bytes in UTF-8: pymef (and so mef_tools) copies it into a
      16-byte buffer;
    * a character above U+00FF: MEF keeps only the low byte of each
      character's code point, so a different password with the same low bytes
      also opens the session.

    mef3io itself writes and reads such a password correctly.

    Parameters
    ----------
    password : str

    Returns
    -------
    str or None
        The reason in plain words (it never repeats the password), or None
        when the password interoperates.

    Examples
    --------
    >>> mef3io.password_interop_issue("secret")
    >>> mef3io.password_interop_issue("a" * 20)  # doctest: +ELLIPSIS
    'it has 20 characters; meflib and pymef refuse passwords longer than 15 characters, ...'
    """
    from . import _mef3io

    if not isinstance(password, str):
        raise TypeError("password must be a str")
    return _mef3io.password_interop_issue(password) or None


def warn_password_interop(password1, password2, stacklevel=3, path=None):
    """Warn about a password other MEF readers will not handle as expected."""
    from . import _mef3io
    from ._reader import PasswordInteropWarning
    from ._warnings import emit

    if not password1:
        return  # no password1 -> nothing is encrypted
    for label, pw in (("level-1", password1), ("level-2", password2)):
        issue = _mef3io.password_interop_issue(pw) if pw else ""
        if issue:
            emit(PasswordInteropWarning,
                 f"the {label} password will not interoperate as expected: {issue}.",
                 path=path, details=(label,), stacklevel=stacklevel)


def warn_legacy_password_segments(entries, stacklevel=3, path=None):
    """One warning for segments appended to under the password mapping of mef3io 1.2.0 and earlier."""
    if entries:
        from ._reader import LegacyPasswordWarning
        from ._warnings import emit

        emit(
            LegacyPasswordWarning,
            f"{len(entries)} segment(s) were appended to whose password matched only "
            f"through the password-to-key mapping that mef3io used up to 1.2.0 (issue #84; "
            f"most likely written by one of those versions). They keep their old keys, so meflib and pymef cannot open them "
            "with this password; new segments use meflib's mapping. A re-keyed copy is "
            "mef3io.anonymize_session(..., output_password=(level1, level2)), but it also "
            "de-identifies the copy and shifts every time (by at least the recording's span "
            "+ 1 day); a pure re-key is mef3io.transform_session. "
            "The segments are listed in the warning's details.",
            path=path, details=entries, stacklevel=stacklevel)


@contextlib.contextmanager
def reporting_unreadable_metadata(impl, path=None):
    """Warn about segment metadata the core skipped during one write call.

    Choosing the recording-time offset reads other segments' ``.tmet`` files;
    in a session at offset 0 one that cannot be read is skipped so the write
    can go ahead (a damaged segment must not block unrelated writes). It is
    still damage, so it is reported, never silent. Shared by
    :class:`Writer` and the ``mef_tools``-compatible ``MefWriter``.
    """
    before = set(impl.unreadable_metadata)
    legacy_before = set(getattr(impl, "legacy_password_segments", []))
    try:
        yield
    finally:
        # By content, not position: an entry is dropped once its slot is
        # filled, so the list can shrink and grow within one call.
        warn_unreadable_metadata(
            [e for e in impl.unreadable_metadata if e not in before], stacklevel=5, path=path)
        warn_legacy_password_segments(
            [e for e in getattr(impl, "legacy_password_segments", []) if e not in legacy_before],
            stacklevel=5, path=path)


class Writer:
    """Write a MEF 3.0 session.

    Times throughout are **uUTC** (microseconds since the Unix epoch). The
    first write to a channel creates segment 0; later writes append in-segment
    (extending the existing files) unless ``new_segment=True``.

    Parameters
    ----------
    path : str
        Path to the ``.mefd`` session directory to create or extend (the
        suffix is enforced). Tar archives are read-only sessions: a ``.tar``
        path raises — write a directory and pack it with
        :func:`mef3io.archive_session`.
    overwrite : bool, optional
        ``True`` deletes any existing session first. ``False`` (default)
        reopens an existing session for appending — state is recovered from
        disk, so appends work across program runs.
    password1, password2 : str, optional
        Level-1 and level-2 passwords. MEF has no "level-1 only" files, so to
        encrypt a session pass **both**; leave both empty for no encryption.
    units : str or None, optional
        Physical units label stored in metadata (e.g. ``"uV"``).
    block_length : int or None, optional
        RED block size in samples. ``None`` (default) derives it from ``fs``
        (``fs`` samples for ``fs`` >= 5000, else ``10*fs``, at least 1).
        At most ``mef3io._mef3io.MAX_BLOCK_SAMPLES`` (858 993 396: a block's
        byte size is a 32-bit field); more raises ``ValueError`` before
        anything is created, as does a rate whose derived block exceeds it.
    n_threads : int, optional
        How many threads this Writer may use (RED encoding): ``0`` (default)
        = automatic, ``1`` = no extra threads. Output is byte-identical
        regardless of thread count. Changeable later through
        :attr:`n_threads`. See [the threads guide](https://bnelair.github.io/mef3io/threads/).
    metadata : mef3io.Metadata or dict, optional
        Session-wide subject/acquisition metadata written to every channel.
        Also settable later via :meth:`set_metadata` (before writing).

    Notes
    -----
    **Durability.** An append flushes exactly one thing to stable storage:
    each segment's ``.tmet``, before it is renamed over the previous one
    (a short ``.tmet`` would make the whole session fail to open). The
    ``.tdat`` and ``.tidx`` are extended in place without flushing, so a power
    cut or OS crash mid-append can leave the **last** append of a segment
    torn: a short tail, an index pointing past the data, blocks the index
    never listed, or a stale header count. :func:`mef3io.validate_session`
    detects each of these and :func:`mef3io.recover_session` repairs them
    (rebuilding missing index entries from the RED block headers). Earlier
    appends are never at risk: nothing rewrites them. A fresh write flushes
    nothing; there is nothing underneath it to lose. The ``durability=``
    argument of 1.x was removed in 2.0 (passing it raises ``TypeError``).

    Examples
    --------
    >>> with mef3io.Writer("session.mefd", overwrite=True, units="uV") as w:
    ...     w.write("ch1", data, start_uutc, fs=256.0)   # NaN marks gaps
    """

    def __init__(
        self,
        path: str,
        overwrite: bool = False,
        password1: str = "",
        password2: str = "",
        units: Optional[str] = None,
        block_length: Optional[int] = None,
        n_threads: int = 0,
        metadata=None,
    ):
        from . import _mef3io

        # Validated FIRST: the SessionWriter constructor removes an existing
        # session when overwrite=True, so checking afterwards could delete a
        # good session and only then raise.
        if block_length is not None:
            # More than a MEF 3.0 block can describe (its size is a 32-bit
            # field): refused here, before the folder exists (#83).
            _mef3io.check_block_length(int(block_length))
        self._path = str(path)
        self._impl = _mef3io.SessionWriter(str(path), overwrite, password1 or "", password2 or "")
        warn_unreadable_metadata(self._impl.unreadable_metadata, path=self._path)
        warn_password_interop(password1, password2, path=self._path)
        # Deliberately cache-unaware: a snapshot is judged only when a cached
        # open happens (mef3io.cache), never maintained by write paths.
        if units is not None:
            self._impl.set_units(units)
        if block_length is not None:
            self._impl.set_block_length(int(block_length))
        from ._reader import _check_threads

        self._n_threads = _check_threads(n_threads)
        self._impl.set_threads(self._n_threads)
        if metadata is not None:
            self.set_metadata(metadata)

    def set_metadata(self, metadata) -> None:
        """Set session-wide subject/acquisition metadata (a
        :class:`mef3io.Metadata`, or a flat dict of fields). Call before
        writing; applies to every channel."""
        flat = metadata._flat() if hasattr(metadata, "_flat") else dict(metadata)
        self._impl.set_metadata(flat)

    def __enter__(self) -> "Writer":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    @property
    def n_threads(self) -> int:
        """How many threads this Writer may use (``0`` = automatic).

        Settable at any time; applies from the next write. See [the
        threads guide](https://bnelair.github.io/mef3io/threads/).
        """
        return self._n_threads

    @n_threads.setter
    def n_threads(self, value: int) -> None:
        from ._reader import _check_threads

        value = _check_threads(value)
        if self._impl is not None:  # a closed Writer just remembers it
            self._impl.set_threads(value)
        self._n_threads = value

    def close(self) -> None:
        """Finalize and release the writer. Called automatically on
        context-manager exit."""
        self._impl = None

    def write(
        self,
        channel: str,
        data: np.ndarray,
        start_uutc: int,
        fs: float,
        precision: "int | str" = -1,
        new_segment: bool = False,
    ) -> dict:
        """Write float data; NaN runs become discontinuity gaps.

        Values are quantized to int32 counts as ``round(data * 10**precision)``
        with the conversion factor ``10**-precision`` kept in metadata. NaN
        samples are not stored — they read back as ``NaN``.

        ``precision="step"`` (opt-in) stores the data as whole counts of its
        own quantization step instead — e.g. an amplifier's 0.195 uV per
        count — which gives much smaller files for data that came from an
        ADC. The step is estimated from a bounded sample of the data (fast
        on any length) and every value is checked while it is converted: a
        value stored this way reads back equal to what was written (exactly,
        when the data was ``counts * step`` in float64; otherwise within
        float64 rounding, 64 eps relative). Data that is not on one step
        (filtered, resampled) falls back to the default path with a
        :class:`StepFallbackWarning`, as does data showing too few distinct
        values to establish a step (a DC level, a square wave). A channel
        keeps the conversion factor of its first segment: on a channel that
        already has data, ``"step"`` stores data on that factor exactly, and
        REFUSES data off it (``WriteConflictError``, nothing written) rather
        than round it. Appending with the default precision to a channel
        whose factor is not a power of ten (a step, or ``write_int32``) is
        refused too: keep passing ``"step"``.

        Parameters
        ----------
        channel : str
            Channel name (created on first write).
        data : array_like
            Any numeric input; coerced to float64 (lists, int arrays,
            ``float32`` all accepted).
        start_uutc : int or float
            Timestamp of the first sample, uUTC.
        fs : float
            Sampling frequency in Hz.
        precision : int or "step", optional
            Decimal precision for quantization. ``-1`` (default) infers it from
            the data — or, when appending, reuses the segment's stored
            precision so the append cannot conflict. ``"step"`` detects the
            data's own quantization step (see above).
        new_segment : bool, optional
            Force a fresh segment instead of appending in-segment.

        Returns
        -------
        dict
            ``samples_written``, ``blocks``, ``gaps_skipped``, ``segment``,
            ``units_conversion_factor`` (the factor the stored counts carry),
            ``quantization`` (``"decimal"``: rounded to ``10**-precision``;
            ``"step"``: stored exactly on the factor) and
            ``step_fallback`` (``precision="step"`` only: ``None`` when the
            step was used, else why not), ``clamped`` and
            ``clamped_infinite`` (samples stored as the nearest storable
            value; see :class:`ClampedSamplesWarning`).

        Raises
        ------
        ValueError
            If ``fs`` is not a finite number > 0, ``precision`` makes the
            conversion factor ``10**-precision`` 0, or is a string other
            than ``"step"`` — checked before anything is written. (A value
            whose count would leave int32, or ``+-inf``, is not an error: it
            is stored as the nearest storable value, counted in the
            summary's ``clamped`` and reported by
            :class:`ClampedSamplesWarning`.)
        WriteConflictError
            On an append conflict (fs / conversion-factor mismatch, data
            starting before the segment's end, a default-precision append
            to a factor that is no power of ten, or ``"step"`` data off the
            channel's factor). Nothing is written.
        """
        step = _is_step(precision)
        data = np.atleast_1d(np.ascontiguousarray(data, dtype=np.float64))
        with self._reporting_unreadable_metadata():
            summary = self._impl.write_float(
                channel, data, start_uutc, float(fs), -1 if step else int(precision),
                bool(new_segment), step,
            )
        warn_step_fallback(channel, summary, path=self._path)
        warn_clamped(channel, summary, path=self._path)
        return summary

    def write_int32(
        self,
        channel: str,
        data: np.ndarray,
        ufact: float,
        start_uutc: int,
        fs: float,
        valid: Optional[np.ndarray] = None,
        new_segment: bool = False,
    ) -> dict:
        """Write integer counts verbatim with a conversion factor (bit-exact).

        The primitive path: counts are stored exactly as given, with ``ufact``
        (e.g. an amplifier's volts-per-bit) in metadata. Physical units on read
        are ``counts * ufact``.

        Parameters
        ----------
        channel : str
            Channel name (created on first write).
        data : array_like of int
            Integer counts (any integer width). Stored bit-exact.
        ufact : float
            Conversion factor from counts to physical units.
        start_uutc : int or float
            Timestamp of the first sample, uUTC.
        fs : float
            Sampling frequency in Hz.
        valid : array_like or None, optional
            Same length as ``data``; any numeric/bool mask where nonzero marks
            a real sample and zero a discontinuity gap. ``None`` = all valid.
        new_segment : bool, optional
            Force a fresh segment instead of appending in-segment.

        Returns
        -------
        dict
            ``samples_written``, ``blocks``, ``gaps_skipped``, ``segment``.

        Raises
        ------
        TypeError
            If ``data`` is floating-point (use :meth:`write` for float data —
            counts are never silently rounded here).
        ValueError
            If any value is outside the int32 range (it would wrap), if
            ``fs`` or ``ufact`` is not a finite number > 0 (a ``ufact`` of 0
            or NaN would make every sample read back as 0 or NaN), or if
            ``valid`` does not have one entry per sample — all checked
            before anything is written.
        RuntimeError
            On an append conflict.
        """
        data = _as_int32_counts(data)
        if valid is not None:
            valid = np.ascontiguousarray(
                np.atleast_1d(np.asarray(valid)) != 0, dtype=np.uint8
            )
        with self._reporting_unreadable_metadata():
            return self._impl.write_int32(
                channel, data, float(ufact), start_uutc, float(fs), valid, bool(new_segment)
            )

    def write_annotations(self, annotations, channel: Optional[str] = None) -> None:
        """Write records (annotations).

        Replaces the records at the given level. In encrypted sessions record
        bodies are level-2 encrypted.

        Parameters
        ----------
        annotations : iterable of dict or pandas.DataFrame
            Each record needs ``time`` (uUTC); ``type`` defaults to ``"Note"``;
            ``text`` and ``duration`` are optional. A DataFrame with those
            columns is accepted.
        channel : str or None, optional
            Channel name for channel-level records, or ``None`` (default) for
            session-level records.
        """
        if hasattr(annotations, "to_dict"):
            annotations = annotations.to_dict("records")
        norm = []
        for r in annotations:
            rec = {"type": r.get("type", "Note"), "time": int(r["time"]), "text": r.get("text", "")}
            if r.get("duration") is not None:
                rec["duration"] = int(r["duration"])
            norm.append(rec)
        with self._reporting_unreadable_metadata():
            self._impl.write_records(channel, norm)

    @property
    def unreadable_metadata(self) -> list[str]:
        """Segment metadata this writer could not use, as ``"path: reason"``.

        ``.tmet`` files that could not be read while choosing the
        recording-time offset, and empty segment directories ignored on open
        (what a crashed write can leave behind). In a session at offset 0 an
        unreadable file is skipped so it does not block unrelated writes (with
        an :class:`UnreadableSegmentWarning`); where the offset is non-zero,
        or nothing else is readable, the write is refused instead.
        """
        return list(self._impl.unreadable_metadata)

    def _reporting_unreadable_metadata(self):
        return reporting_unreadable_metadata(self._impl, self._path)
