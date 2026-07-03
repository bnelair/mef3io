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
        data = np.ascontiguousarray(data, dtype=np.float64)
        return self._impl.write_float(channel, data, int(start_uutc), float(fs), precision, new_segment)

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

        ``valid`` (optional, same length, bool/uint8) marks discontinuity gaps.
        """
        data = np.ascontiguousarray(data, dtype=np.int32)
        if valid is not None:
            valid = np.ascontiguousarray(valid, dtype=np.uint8)
        return self._impl.write_int32(
            channel, data, float(ufact), int(start_uutc), float(fs), valid, new_segment
        )
