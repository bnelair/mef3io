"""High-level Python Reader wrapping the C++ backend (or, later, the pure
backend). Thin: adds context-manager support and pandas-friendly helpers while
delegating all real work to the backend."""
from __future__ import annotations

import warnings
from typing import Optional

import numpy as np


class SessionDeclarationWarning(UserWarning):
    """A session leaves size declarations unset that other readers rely on.

    Raised on open, once per process per session (Python's default warning
    filter suppresses a repeat of the same message from the same line). It
    never affects mef3io's own reads —
    mef3io sizes every buffer from the block headers themselves — but
    meflib-based readers (CyberPSG and most established MEF tooling) allocate
    from metadata section 2 before decoding, and cannot tell an unset field
    from a real measurement.

    Silence it like any warning::

        warnings.filterwarnings("ignore", category=mef3io.SessionDeclarationWarning)

    or per call with ``Reader(path, warn_declarations=False)``. To see the
    detail, or to fix it, use :class:`mef3io.Validator`.
    """


class UnreadableSegmentWarning(UserWarning):
    """Part of this session could not be read and is being skipped.

    Only ever raised by a Reader opened ``strict=False``. The skipped segments'
    time spans read back as NaN — indistinguishable, in the returned array,
    from a genuine recording gap. That is the hazard lenient mode trades for
    availability, so it is never silent: see :attr:`Reader.problems` for the
    segment paths and the reason each one failed.

    Strict is the default. An unreported CRC failure is how corrupt scaling
    reaches an analysis unnoticed; lenient mode exists to salvage the intact
    99% of an archive, not to relax the checks.
    """


class BlockCopyWarning(UserWarning):
    """A block's two stored copies of its start time or sample count disagree.

    Every RED block records its start time and sample count TWICE — once in the
    `.tidx` entry and once in the block's own header inside the `.tdat`.
    Nothing in the format keeps them in step. mef3io places data by the HEADER,
    which is the copy the per-block CRC protects and the copy meflib and pymef
    read, so a file whose copies have drifted apart still reads the same as it
    does under the legacy stack.

    This warning means the file itself is internally inconsistent — it does not
    mean the read is wrong. It fires only on such a file: on anything mef3io or
    the legacy stack wrote, the copies are identical and nothing is emitted.

    The structured detail is in the ``block_copy_mismatches`` key returned by
    :meth:`Reader.read_raw`. Silence it like any warning::

        warnings.filterwarnings("ignore", category=mef3io.BlockCopyWarning)
    """


def _unreadable_warning_text(problems: list, path: str) -> str:
    chans = sorted({p["channel"] for p in problems})
    first = problems[0]
    return (
        f"{path}: {len(problems)} segment(s) could not be read and were SKIPPED "
        f"across channel(s) {', '.join(chans)}; their time spans read back as NaN, "
        f"which is indistinguishable from a real recording gap. "
        f"First: {first['segment']} — {first['reason']}. "
        "Full list in Reader.problems. Open with strict=True to fail instead."
    )


def _block_copy_warning_text(mismatches: list, path: str) -> str:
    times = sum(1 for m in mismatches
                if m["index_start_uutc"] != m["header_start_uutc"])
    counts = sum(1 for m in mismatches
                 if m["index_number_of_samples"] != m["header_number_of_samples"])
    parts = []
    if times:
        parts.append(f"{times} block(s) disagree on start time")
    if counts:
        parts.append(f"{counts} block(s) disagree on sample count")
    first = mismatches[0]
    return (
        f"{path}: the block index and the RED block headers disagree — "
        + " and ".join(parts)
        + f". First: {first['segment']} block {first['block_index']} "
        f"(index t={first['index_start_uutc']} n={first['index_number_of_samples']}, "
        f"header t={first['header_start_uutc']} n={first['header_number_of_samples']}). "
        "The header is authoritative and was used, which is what meflib/pymef do "
        "too, so this read matches theirs; the FILE is inconsistent. "
        "Inspect with Reader.read_raw()['block_copy_mismatches']."
    )


def _declaration_warning_text(issues: list, path: str) -> str:
    fields: dict[str, int] = {}
    channels = set()
    for issue in issues:
        fields[issue["field"]] = fields.get(issue["field"], 0) + 1
        channels.add(issue["channel"])
    listed = ", ".join(f"{name} ({n} segment(s))" for name, n in sorted(fields.items()))
    return (
        f"{path}: metadata section 2 leaves size declarations unset across "
        f"{len(channels)} channel(s): {listed}. "
        "This does NOT affect reading with mef3io — every buffer is sized from the "
        "block headers themselves, and the data is intact. It does affect "
        "meflib-based readers (e.g. CyberPSG), which allocate from these fields. "
        "Run mef3io.Validator(path).validate() for detail, or "
        "python -m mef3io repair <path> --check <check-id> to fix the file."
    )


class Reader:
    """Read-only interface to a MEF 3.0 session.

    Times throughout are **uUTC** (microseconds since the Unix epoch). Windowed
    reads fetch only the bytes they need, so they stay cheap on huge sessions.

    Parameters
    ----------
    path : str
        Path to the ``.mefd`` session directory, or to an uncompressed tar
        archive of one (``name.mefd.tar``, from :func:`mef3io.archive_session`)
        — tar sessions are read in place, without extraction. The suffix is
        enforced: anything else is refused, so a stray directory or file can
        never be misread as a session.
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
    warn_declarations : bool, default True
        Emit a :class:`SessionDeclarationWarning` when the session leaves
        section-2 size declarations unset. The check is free — the metadata is
        already parsed at open — and never affects the data read here; it
        flags a file that can mislead *other* MEF readers. Set ``False``, or
        filter the warning category, to silence it.

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
        warn_declarations: bool = True,
        strict: bool = True,
    ):
        self._path = str(path)
        self._password = password or ""
        self._backend_name = backend
        self._n_threads = n_threads
        self._strict = bool(strict)
        self._impl = None  # constructed lazily so a warm start stays cheap

        # Warm start: serve channel metadata from a valid cache snapshot, and
        # defer building the backend until data is actually read. Caching is
        # opt-in (cache=None disables it).
        from . import cache as _cache

        self._cache_path = _cache.resolve_cache_path(self._path, cache)
        self._infos = None
        self._declaration_issues = []
        if self._cache_path is not None:
            snap = _cache.load_valid(self._cache_path, self._path)
            if snap is not None:
                self._infos = snap["channel_infos"]
                self._declaration_issues = snap.get("declaration_issues", [])

        if self._infos is None:
            self._ensure_impl()
            self._infos = {ch: self._impl.info(ch) for ch in self._impl.channels}
            # Free at this point: every segment's metadata is already parsed.
            self._declaration_issues = self._collect_declaration_issues()
            if self._cache_path is not None:
                _cache.save(
                    self._cache_path,
                    _cache.build_snapshot(self._path, self._infos, self._declaration_issues),
                )

        if not self._strict and self._impl is not None:
            problems = self.problems
            if problems:
                warnings.warn(
                    _unreadable_warning_text(problems, self._path),
                    UnreadableSegmentWarning,
                    stacklevel=2,
                )

        if warn_declarations and self._declaration_issues:
            warnings.warn(
                _declaration_warning_text(self._declaration_issues, self._path),
                SessionDeclarationWarning,
                stacklevel=2,
            )

    def _collect_declaration_issues(self) -> list:
        # getattr rather than try/except: a bare `except AttributeError` around
        # the call would also swallow one raised *inside* it, and a backend with
        # a typo would then report every session as clean.
        fn = getattr(self._impl, "declaration_issues", None)
        return list(fn()) if fn is not None else []

    @property
    def problems(self) -> list:
        """Segments that could not be read, when opened ``strict=False``.

        A list of ``{"channel", "segment_number", "segment", "reason"}`` dicts,
        empty when everything loaded. **Always empty in strict mode**, where an
        unreadable segment raises instead of being skipped.

        Check it before trusting a read from a lenient Reader: a skipped
        segment's samples come back as NaN, exactly like a genuine gap, so the
        array alone cannot tell you data is missing.
        """
        impl = self._impl
        if impl is None:
            return []
        fn = getattr(impl, "problems", None)
        return list(fn()) if fn is not None else []

    @property
    def declaration_issues(self) -> list:
        """Section-2 size declarations this session leaves unset.

        The structured form of the :class:`SessionDeclarationWarning` raised on
        open: a list of ``{"channel", "segment", "field"}`` dicts, empty when
        the session declares everything. Free — computed at open from metadata
        that was already parsed. It sees only what is *missing*; use
        :class:`mef3io.Validator` to find what is merely wrong.
        """
        return list(self._declaration_issues)

    def _ensure_impl(self):
        if getattr(self, "_closed", False):
            raise ValueError(
                "this Reader is closed; open a new one "
                "(close() releases the backend's file handles, so reusing it "
                "would silently reopen the session)"
            )
        if self._impl is None:
            if self._backend_name == "cpp":
                from . import _mef3io

                self._impl = _mef3io.Reader(
                    self._path, self._password, self._n_threads, self._strict
                )
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
        """Release the backend and its file handles.

        After this the reader is CLOSED: a further read raises rather than
        quietly reopening the session. A caller who closed to release handles —
        before archiving the directory, or to stay under an fd limit — got them
        back without notice, which is the opposite of what they asked for.
        Closing twice is harmless.
        """
        self._impl = None
        self._closed = True

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

    @property
    def metadata(self):
        """Session subject/acquisition metadata as a :class:`mef3io.Metadata`.

        Built from the first channel (metadata is session-wide). Subject fields
        are empty unless the reader was opened with a level-2 password.

        Returns
        -------
        mef3io.Metadata
        """
        from .metadata import Metadata

        chans = self.channels
        if not chans:
            return Metadata()
        return Metadata.from_info(self.info(chans[0]))

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
            out = impl.read_raw(channel, t0, t1)
        else:
            out = impl.read_raw(channel, t0, t1, int(n_threads))
        mismatches = out.get("block_copy_mismatches") or []
        if mismatches:
            warnings.warn(
                _block_copy_warning_text(mismatches, self._path),
                BlockCopyWarning,
                stacklevel=2,
            )
        return out

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
