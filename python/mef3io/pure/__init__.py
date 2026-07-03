"""Pure-Python backend (fallback / benchmark / oracle).

Placeholder: the pure read path is planned to be assembled from the verified
decoder in ``reference_files/mef3_dump`` plus ``mef_tools/reimplementation.py``.
Until then, selecting ``backend="pure"`` raises a clear error so callers aren't
silently downgraded. The C++ backend is the supported implementation.
"""
from __future__ import annotations


class Reader:  # pragma: no cover - not yet implemented
    def __init__(self, *args, **kwargs):
        raise NotImplementedError(
            "The pure-Python backend is not implemented yet; use the default "
            "C++ backend (backend='cpp')."
        )
