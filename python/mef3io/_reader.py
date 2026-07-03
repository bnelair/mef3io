"""High-level Python Reader wrapping the C++ backend (or, later, the pure
backend). Thin: adds context-manager support and pandas-friendly helpers while
delegating all real work to the backend."""
from __future__ import annotations

from typing import Optional

import numpy as np


class Reader:
    """Read-only interface to a MEF 3.0 session.

    Parameters
    ----------
    path : str
        Path to the ``.mefd`` session directory.
    password : str, optional
        Password (level 1 or level 2) for encrypted sessions.
    backend : {"cpp", "pure"}, optional
        Which implementation to use. Defaults to the C++ backend, falling back
        to pure Python if the extension is unavailable.
    """

    def __init__(
        self,
        path: str,
        password: str = "",
        backend: str = "cpp",
        n_threads: int = 0,
        cache=None,
    ):
        self._path = str(path)
        self._password = password or ""
        self._backend_name = backend
        self._n_threads = n_threads
        self._impl = None  # constructed lazily so a warm start stays cheap

        # Warm start: serve channel metadata from a valid cache snapshot, and
        # defer building the backend until data is actually read. Caching is
        # opt-in (cache=None disables it).
        from . import cache as _cache

        self._cache_path = _cache.resolve_cache_path(self._path, cache)
        self._infos = None
        if self._cache_path is not None:
            snap = _cache.load_valid(self._cache_path, self._path)
            if snap is not None:
                self._infos = snap["channel_infos"]

        if self._infos is None:
            self._ensure_impl()
            self._infos = {ch: self._impl.info(ch) for ch in self._impl.channels}
            if self._cache_path is not None:
                _cache.save(self._cache_path, _cache.build_snapshot(self._path, self._infos))

    def _ensure_impl(self):
        if self._impl is None:
            if self._backend_name == "cpp":
                from . import _mef3io

                self._impl = _mef3io.Reader(self._path, self._password, self._n_threads)
            elif self._backend_name == "pure":
                from .pure import Reader as PureReader

                self._impl = PureReader(self._path, self._password)
            else:
                raise ValueError(f"unknown backend: {self._backend_name!r}")
        return self._impl

    def __enter__(self) -> "Reader":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def close(self) -> None:
        # RAII in C++; nothing to release explicitly. Present for API parity.
        self._impl = None

    @property
    def channels(self) -> list[str]:
        # Served from the cache snapshot when warm; no backend needed.
        if self._infos is not None:
            return list(self._infos.keys())
        return list(self._ensure_impl().channels)

    def info(self, channel: str) -> dict:
        if self._infos is not None and channel in self._infos:
            return self._infos[channel]
        return self._ensure_impl().info(channel)

    def read(
        self,
        channel: str,
        t0: Optional[int] = None,
        t1: Optional[int] = None,
        n_threads: Optional[int] = None,
    ) -> np.ndarray:
        """Float64 samples for ``channel`` over ``[t0, t1)`` uUTC (whole channel
        by default). Discontinuity gaps are NaN; values are scaled by the
        channel's units-conversion factor. ``n_threads`` overrides the reader
        default for this call."""
        impl = self._ensure_impl()
        if n_threads is None:
            return impl.read(channel, t0, t1)
        return impl.read(channel, t0, t1, n_threads)

    def read_raw(
        self,
        channel: str,
        t0: Optional[int] = None,
        t1: Optional[int] = None,
        n_threads: Optional[int] = None,
    ) -> dict:
        """Int32 samples plus a validity mask (0 in gaps) and metadata."""
        impl = self._ensure_impl()
        if n_threads is None:
            return impl.read_raw(channel, t0, t1)
        return impl.read_raw(channel, t0, t1, n_threads)

    def segments(self, channel: str) -> list[dict]:
        """Per-segment map of ``channel``: one dict per segment with its
        ``start_time``/``end_time`` (uUTC), ``start_sample``,
        ``number_of_samples``, ``number_of_blocks``, and on-disk ``path``.
        Metadata only (no data is decoded), so it is cheap even for huge
        sessions — use it to locate data across large recording gaps, then
        :meth:`toc` for the block-level view within segments."""
        return self._ensure_impl().segments(channel)

    def toc(self, channel: str) -> list[dict]:
        """Block-level table of contents for seeking / viewers."""
        return self._ensure_impl().toc(channel)

    def records(self, channel: Optional[str] = None) -> list[dict]:
        """Records (annotations) at session level (default) or for a channel."""
        return self._ensure_impl().records(channel)
