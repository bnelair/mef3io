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
        """Write float64 ``data`` for ``channel``. NaN = discontinuity gap.

        Returns a summary dict: ``samples_written``, ``blocks``,
        ``gaps_skipped``, ``segment``.
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
        """Write integer ``data`` with conversion factor ``ufact`` verbatim.

        ``valid`` (optional, same length; anything numeric — nonzero = valid)
        marks discontinuity gaps. Data must be integer-typed: counts are
        stored bit-exact, so floating-point input raises TypeError (use
        ``write()`` for float data) and values outside the int32 range raise
        ValueError instead of silently wrapping.
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
        """Write records (annotations): session-level by default, or attached
        to ``channel``. Accepts an iterable of dicts (``time`` required;
        ``type`` defaults to ``"Note"``; optional ``text``, ``duration``) or a
        pandas DataFrame with those columns. Replaces existing records at that
        level. Encrypted sessions encrypt record bodies at level 2."""
        if hasattr(annotations, "to_dict"):
            annotations = annotations.to_dict("records")
        norm = []
        for r in annotations:
            rec = {"type": r.get("type", "Note"), "time": int(r["time"]), "text": r.get("text", "")}
            if r.get("duration") is not None:
                rec["duration"] = int(r["duration"])
            norm.append(rec)
        self._impl.write_records(channel, norm)
