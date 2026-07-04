"""High-level Python Writer wrapping the C++ SessionWriter.

Two write paths:
- ``write`` — float64 data; NaN marks discontinuities; ``precision`` sets the
  conversion factor (10**-precision) or is inferred when omitted.
- ``write_int32`` — the primitive path: integer counts plus a conversion factor
  (e.g. an amplifier's V/bit), stored verbatim.
"""
from __future__ import annotations

from typing import Optional

import numpy as np

_INT32_MIN, _INT32_MAX = -(2**31), 2**31 - 1


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


class Writer:
    """Write a MEF 3.0 session.

    Times throughout are **uUTC** (microseconds since the Unix epoch). The
    first write to a channel creates segment 0; later writes append in-segment
    (extending the existing files) unless ``new_segment=True``.

    Parameters
    ----------
    path : str
        Path to the ``.mefd`` session directory to create or extend.
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
        (``fs`` samples for ``fs`` >= 5000, else ``10*fs``).
    n_threads : int, optional
        Worker threads for RED encoding (``0`` = all cores, ``1`` = serial).
        Output is byte-identical regardless of thread count.

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
    ):
        from . import _mef3io

        self._path = str(path)
        self._impl = _mef3io.SessionWriter(str(path), overwrite, password1 or "", password2 or "")
        # A write changes the tree; drop any stale auto cache for this session.
        from . import cache as _cache

        _cache.invalidate(self._path, "auto")
        if units is not None:
            self._impl.set_units(units)
        if block_length is not None:
            self._impl.set_block_length(int(block_length))
        self._impl.set_threads(int(n_threads))

    def __enter__(self) -> "Writer":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

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
        precision: int = -1,
        new_segment: bool = False,
    ) -> dict:
        """Write float data; NaN runs become discontinuity gaps.

        Values are quantized to int32 counts as ``round(data * 10**precision)``
        with the conversion factor ``10**-precision`` kept in metadata. NaN
        samples are not stored — they read back as ``NaN``.

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
        precision : int, optional
            Decimal precision for quantization. ``-1`` (default) infers it from
            the data — or, when appending, reuses the segment's stored
            precision so the append cannot conflict.
        new_segment : bool, optional
            Force a fresh segment instead of appending in-segment.

        Returns
        -------
        dict
            ``samples_written``, ``blocks``, ``gaps_skipped``, ``segment``.

        Raises
        ------
        RuntimeError
            On an append conflict (fs / conversion-factor mismatch, or data
            starting before the segment's end).
        """
        data = np.atleast_1d(np.ascontiguousarray(data, dtype=np.float64))
        return self._impl.write_float(
            channel, data, start_uutc, float(fs), int(precision), bool(new_segment)
        )

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
            If any value is outside the int32 range (it would wrap).
        RuntimeError
            On an append conflict.
        """
        data = _as_int32_counts(data)
        if valid is not None:
            valid = np.ascontiguousarray(
                np.atleast_1d(np.asarray(valid)) != 0, dtype=np.uint8
            )
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
        self._impl.write_records(channel, norm)
