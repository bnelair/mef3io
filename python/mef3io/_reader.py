"""High-level Python Reader wrapping the C++ backend (or, later, the pure
backend). Thin: adds context-manager support and pandas-friendly helpers while
delegating all real work to the backend."""
from __future__ import annotations

from typing import Optional

import numpy as np


class Reader:
    """Read-only interface to a MEF 3.0 session.

    Times throughout are **uUTC** (microseconds since the Unix epoch). Windowed
    reads fetch only the bytes they need, so they stay cheap on huge sessions.

    Parameters
    ----------
    path : str
        Path to the ``.mefd`` session directory.
    password : str, optional
        Password for encrypted sessions. A **level-2** password unlocks
        everything; a **level-1** password reads the signal and technical
        metadata but leaves subject metadata locked. Empty for unencrypted
        sessions.
    backend : {"cpp", "pure"}, optional
        Which implementation to use. Defaults to the C++ backend; the pure
        backend is not yet implemented.
    n_threads : int, optional
        Worker threads for RED block decoding. ``0`` (default) uses all cores,
        ``1`` is serial. Output is byte-identical regardless of thread count.
    cache : str or None, optional
        Opt-in warm-start cache for channel metadata. ``None`` (default)
        disables it; ``"auto"`` uses the per-user OS cache directory; a path
        makes it persistent. Warm opens serve :attr:`channels` / :meth:`info`
        without touching the session tree.

    Examples
    --------
    >>> with mef3io.Reader("session.mefd") as r:
    ...     x = r.read(r.channels[0], t0, t1)   # float64, NaN in gaps
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
        """Release the backend. Optional (cleanup is automatic); present for
        API parity and context-manager use."""
        self._impl = None

    @property
    def channels(self) -> list[str]:
        """Channel names in the session, sorted.

        Returns
        -------
        list of str
        """
        # Served from the cache snapshot when warm; no backend needed.
        if self._infos is not None:
            return list(self._infos.keys())
        return list(self._ensure_impl().channels)

    def info(self, channel: str) -> dict:
        """Channel metadata.

        Parameters
        ----------
        channel : str
            Channel name.

        Returns
        -------
        dict
            Keys: ``sampling_frequency`` (Hz), ``units_conversion_factor``,
            ``units_description``, ``start_time`` / ``end_time`` (uUTC),
            ``number_of_samples`` (stored samples — NaN gaps are not counted,
            so a gridded :meth:`read` usually returns more),
            ``recording_time_offset``, ``n_segments``, ``section3_available``,
            and the section-3 subject fields (``subject_name_1`` /
            ``subject_name_2`` / ``subject_id`` / ``recording_location``;
            ``None`` without level-2 access).
        """
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
        """Read float64 samples on the uniform sampling grid.

        Parameters
        ----------
        channel : str
            Channel name.
        t0, t1 : int or float, optional
            Half-open time window ``[t0, t1)`` in uUTC. Defaults span the whole
            channel. The number of samples returned is
            ``round((t1 - t0) * fs / 1e6)``.
        n_threads : int, optional
            Per-call override of the reader's thread count (``0`` = all cores,
            ``1`` = serial). ``None`` (default) uses the reader default.

        Returns
        -------
        numpy.ndarray
            1-D float64 array on the sampling grid. Discontinuity gaps are
            filled with ``NaN``; values are scaled by the channel's
            units-conversion factor.

        See Also
        --------
        read_raw : the unscaled int32 form with an explicit validity mask.
        """
        impl = self._ensure_impl()
        if n_threads is None:
            return impl.read(channel, t0, t1)
        return impl.read(channel, t0, t1, int(n_threads))

    def read_raw(
        self,
        channel: str,
        t0: Optional[int] = None,
        t1: Optional[int] = None,
        n_threads: Optional[int] = None,
    ) -> dict:
        """Read the stored int32 counts with an explicit validity mask.

        Parameters
        ----------
        channel : str
            Channel name.
        t0, t1 : int or float, optional
            Half-open ``[t0, t1)`` window in uUTC; defaults span the channel.
        n_threads : int, optional
            Per-call thread-count override (see :meth:`read`).

        Returns
        -------
        dict
            Keys: ``samples`` (int32 ``ndarray``, on the grid),
            ``valid`` (uint8 ``ndarray``; ``0`` marks gap samples with no
            data), ``start_uutc``, ``sampling_frequency``,
            ``units_conversion_factor``. Physical units are
            ``samples * units_conversion_factor`` where ``valid``.
        """
        impl = self._ensure_impl()
        if n_threads is None:
            return impl.read_raw(channel, t0, t1)
        return impl.read_raw(channel, t0, t1, int(n_threads))

    def segments(self, channel: str) -> list[dict]:
        """Per-segment map of a channel — what data is where.

        Read from metadata only (nothing is decoded), so it is cheap even for
        huge, gap-riddled sessions. Use it to locate data across large
        recording gaps, then :meth:`toc` for the block-level view within a
        segment.

        Parameters
        ----------
        channel : str
            Channel name.

        Returns
        -------
        list of dict
            One dict per segment (sorted by segment number) with keys
            ``segment``, ``start_time`` / ``end_time`` (uUTC), ``start_sample``
            (channel-wide index of the first sample), ``number_of_samples``,
            ``number_of_blocks``, and the on-disk ``path``.
        """
        return self._ensure_impl().segments(channel)

    def toc(self, channel: str) -> list[dict]:
        """Block-level table of contents, for seeking and viewers.

        Parameters
        ----------
        channel : str
            Channel name.

        Returns
        -------
        list of dict
            One dict per RED block with ``start_uutc``, ``start_sample``,
            ``number_of_samples``, ``maximum_sample_value``,
            ``minimum_sample_value``, and ``discontinuity`` (``True`` when the
            block does not continue seamlessly from the previous one).
        """
        return self._ensure_impl().toc(channel)

    def records(self, channel: Optional[str] = None) -> list[dict]:
        """Read records (annotations).

        Parameters
        ----------
        channel : str or None, optional
            Channel name for channel-level records, or ``None`` (default) for
            session-level records.

        Returns
        -------
        list of dict
            One dict per record with ``type`` (e.g. ``"Note"``, ``"EDFA"``),
            ``time`` (uUTC), optional ``text`` and ``duration``.
        """
        return self._ensure_impl().records(channel)
