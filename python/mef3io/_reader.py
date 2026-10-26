"""High-level Python Reader wrapping the C++ backend (or, later, the pure
backend). Thin: adds context-manager support and pandas-friendly helpers while
delegating all real work to the backend."""
from __future__ import annotations

import contextlib
import operator
import threading
from typing import Callable, Optional

import numpy as np

from ._cancel import CancelToken, _hooks
from ._warnings import MefWarning, emit as _emit, identity_of as _identity_of


class SessionDeclarationWarning(MefWarning):
    """A session leaves size declarations unset that other readers rely on.

    Raised on open, once per process per session (Python's default warning
    filter suppresses a repeat of the same message from the same line). It
    never affects mef3io's own reads —
    mef3io sizes every buffer from the block headers themselves — but
    meflib-based readers (most established MEF tooling) allocate
    from metadata section 2 before decoding, and cannot tell an unset field
    from a real measurement.

    Silence it like any warning::

        warnings.filterwarnings("ignore", category=mef3io.SessionDeclarationWarning)

    or per call with ``Reader(path, warn_declarations=False)``. To see the
    detail, or to fix it, use :class:`mef3io.Validator`.

    Like every :class:`MefWarning`, the message carries no path:
    ``w.message.path`` is the session and ``w.message.details`` the unset
    declarations (dicts with ``channel``, ``segment`` and ``field``, as
    :attr:`Reader.declaration_issues`).
    """


class UnreadableSegmentWarning(MefWarning):
    """Part of this session could not be read and is being skipped.

    Only ever raised by a Reader opened ``strict=False``. The skipped segments'
    time spans read back as NaN — indistinguishable, in the returned array,
    from a genuine recording gap. That is the hazard lenient mode trades for
    availability, so it is never silent: see :attr:`Reader.problems` for the
    segment paths and the reason each one failed.

    Strict is the default. An unreported CRC failure is how corrupt scaling
    reaches an analysis unnoticed; lenient mode exists to salvage the intact
    99% of an archive, not to relax the checks.

    Also raised by :class:`Writer` (and the compat ``MefWriter``) for segment
    metadata it had to skip; ``details`` are then the strings of
    ``Writer.unreadable_metadata``. From a Reader, ``details`` are the
    :attr:`Reader.problems` dicts it is about. ``w.message.path`` is the
    session; the message itself carries no path (see :class:`MefWarning`).
    """


class SessionOperationWarning(MefWarning):
    """The session is in the middle of an in-place operation (a rename, #72).

    The operation was interrupted, or is still running in another process:
    its marker is in the session folder. Until it is finished or rolled back
    the session's name can be half-changed, so other MEF software may miss
    its annotations, and mef3io refuses to write to it. Reads are not
    affected. Run the same operation again (with the same backup folder) to
    roll it back; ``details`` holds the marker (tool, operation, id).
    """


class BlockCopyWarning(MefWarning):
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

    ``w.message.details`` holds the same mismatch dicts and
    ``w.message.path`` the session; the message carries no path (see
    :class:`MefWarning`).
    """


class UnscaledSamplesWarning(MefWarning):
    """Values read from a segment whose conversion factor is not set.

    A segment stores ``units_conversion_factor`` 0 — MEF's "no entry", which
    meflib's and pymef's section-2 templates hold until a writer sets it — so
    there is no factor to convert its counts into units. :meth:`Reader.read`,
    :meth:`Reader.block_envelope`, :class:`MultiReader` and the compat
    ``MefReader.get_data`` then return those samples as the RAW STORED COUNTS
    (factor 1); before 2.0 they read back as zeros. Emitted once per Reader
    and channel, the first time such a segment is read.
    :meth:`Reader.segments` shows which segments store 0, and
    :meth:`Reader.read_raw` reports the factor as stored (0) in
    ``units_conversion_factors``. ``w.message.details`` holds the channel name.
    """


class LegacyPasswordWarning(MefWarning):
    """The password opened this session only through mef3io's OLD byte mapping.

    MEF derives a 16-byte key from the password one byte per character.
    meflib (and so pymef and ``mef_tools``) keeps the low byte of each
    character's Unicode code point; mef3io 1.2.0 and earlier kept the last byte of
    its UTF-8 encoding instead. The two agree on ASCII and disagree on
    everything else (``"é"`` is 0xE9 in meflib, 0xA9 in old mef3io) — issue #84.

    mef3io now writes with meflib's mapping and reads with it first. A session
    an older mef3io encrypted with a non-ASCII password still opens, through a
    fallback to the old mapping, and this warning says so (on open for the
    metadata, from :meth:`Reader.records` for record files): the data is intact,
    but meflib and pymef cannot open that session with that password. To make
    it readable everywhere, make a re-keyed copy:
    :func:`anonymize_session` with ``output_password=(level1, level2)``
    re-encrypts every file with meflib's mapping. That copy is NOT a pure
    re-key: it is de-identified and every time is shifted (by at least the
    recording's span + 1 day). A pure re-key is ``transform_session``
    (see the transform guide).

    The fallback cannot tell who wrote a file: it also accepts a SECOND
    password, the one whose old-mapping bytes equal the real key (for a
    session keyed with ``"b©"``, also ``"bé"``), whoever wrote it. That is
    inherent to any compatibility fallback and is why it is announced.

    Appending to such a session keeps the existing segments' keys (rewriting
    them would mean re-encrypting them); new segments use meflib's mapping.

    ``w.message.details`` lists what matched that way: channel names (on
    open), record file paths (from :meth:`Reader.records`) or segment paths
    (from :class:`Writer`). ``w.message.path`` is the session; the message
    carries no path (see :class:`MefWarning`). :func:`mef3io.inspect` does not
    emit it: it reports the same condition as
    ``SessionSummary.legacy_password_mapping``.
    """


class PasswordInteropWarning(MefWarning):
    """A password that other MEF readers handle differently than you may expect.

    Raised by :class:`Writer` for a password longer than 15 characters (meflib
    and pymef refuse it, so only mef3io will open the session) or one that
    contains a character above U+00FF (MEF keeps only the low byte of each
    code point, as meflib does, so a different password can unlock the same
    session). The session is still written; the warning only tells you about
    its reach. Short Latin-1 passwords (ASCII, or e.g. ``"é"``, ``"ü"``) raise
    nothing.

    ``w.message.path`` is the session being written; ``details`` is
    ``("level-1",)`` or ``("level-2",)``, which password it is about. The
    password itself never appears in the warning.
    """


def _legacy_password_warning_text(what: str) -> str:
    return (
        f"the password opened {what} only through the password-to-key "
        "mapping that mef3io used up to 1.2.0, which differs from meflib's for "
        "non-ASCII characters (issue #84); such files were most likely encrypted "
        "by one of those versions. The data is read correctly, but meflib and "
        "pymef cannot open these files with this password. A re-keyed copy is "
        "mef3io.anonymize_session(..., output_password=(level1, level2)), but it "
        "also de-identifies the copy and shifts every time (by at least the "
        "recording's span + 1 day); a pure re-key is mef3io.transform_session."
    )


def _warn_legacy_password(impl, path: str, pending: list) -> None:
    fn = getattr(impl, "legacy_password_channels", None)
    channels = list(fn()) if fn is not None else []
    if channels:
        shown = ", ".join(channels[:3]) + (", ..." if len(channels) > 3 else "")
        pending.append((LegacyPasswordWarning,
                        _legacy_password_warning_text(f"{len(channels)} channel(s) ({shown})"),
                        dict(path=path, details=channels, also=_identity_of(impl))))


class _Announced:
    """What a reader has already warned about, so each item is warned ONCE.

    Concurrent calls share the backend's cumulative lists; diffing them per
    call warned twice or announced another call's items (#110). Whichever
    call sees an item first announces it.
    """

    def __init__(self):
        self._lock = threading.Lock()
        self._seen: dict = {}

    def new(self, kind: str, items: list, key) -> list:
        with self._lock:
            seen = self._seen.setdefault(kind, set())
            out = [x for x in items if key(x) not in seen]
            seen.update(key(x) for x in out)
            return out


@contextlib.contextmanager
def _warning_on_legacy_records(announced: _Announced, impl, path: str, pending: list):
    """Warn about record files a records() call opened via the old mapping."""
    # finally: a strict call can read an old-keyed file and then fail on the
    # next one. Each file is announced ONCE per Reader, by whichever call sees
    # it first: concurrent records() calls share the cumulative list, so
    # diffing it per call warned twice or announced another call's files.
    try:
        yield
    finally:
        added = announced.new("legacy", impl.legacy_password_record_files(), lambda f: f)
        if added:
            pending.append((LegacyPasswordWarning,
                            _legacy_password_warning_text(f"{len(added)} record file(s)"),
                            dict(path=path, details=added, also=_identity_of(impl))))


def _unreadable_warning_text(problems: list) -> str:
    chans = sorted({p["channel"] for p in problems})
    first = problems[0]
    return (
        f"{len(problems)} segment(s) could not be read and were SKIPPED "
        f"across channel(s) {', '.join(chans)}; their time spans read back as NaN, "
        f"which is indistinguishable from a real recording gap. "
        f"First: {first['segment']} — {first['reason']}. "
        "Full list in Reader.problems. Open with strict=True to fail instead."
    )


def _block_copy_warning_text(mismatches: list) -> str:
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
        "the block index and the RED block headers disagree — "
        + " and ".join(parts)
        + f". First: {first['segment']} block {first['block_index']} "
        f"(index t={first['index_start_uutc']} n={first['index_number_of_samples']}, "
        f"header t={first['header_start_uutc']} n={first['header_number_of_samples']}). "
        "The header is authoritative and was used, which is what meflib/pymef do "
        "too, so this read matches theirs; the FILE is inconsistent. "
        "Inspect with Reader.read_raw()['block_copy_mismatches']."
    )


def _declaration_warning_text(issues: list) -> str:
    fields: dict[str, int] = {}
    channels = set()
    for issue in issues:
        fields[issue["field"]] = fields.get(issue["field"], 0) + 1
        channels.add(issue["channel"])
    listed = ", ".join(f"{name} ({n} segment(s))" for name, n in sorted(fields.items()))
    return (
        f"metadata section 2 leaves size declarations unset across "
        f"{len(channels)} channel(s): {listed}. "
        "This does NOT affect reading with mef3io — every buffer is sized from the "
        "block headers themselves, and the data is intact. It does affect "
        "meflib-based readers, which allocate from these fields. "
        "Run mef3io.Validator(path).validate() for detail, or "
        "python -m mef3io repair <path> --check <check-id> to fix the file."
    )


def _flush(pending: list, stacklevel: int) -> None:
    """Emit warnings collected as ``(category, message, kwargs)``.

    ``stacklevel`` counts from the caller of this function, as for
    ``warnings.warn``. Collecting first lets :class:`MultiReader` emit a
    sub-Reader's warnings at its own caller, from the calling thread.
    """
    for category, message, kw in pending:
        _emit(category, message, stacklevel=stacklevel + 1, **kw)


def _usable_factor(f) -> bool:
    """A conversion factor a read in units accepts: finite (#150)."""
    return f == f and f not in (float("inf"), float("-inf"))


def _effective_factor(f):
    """The factor a read scales by: 0 (not set, MEF's "no entry") counts as 1."""
    return 1.0 if f == 0 else f


def _note_unscaled(impl, announced, path, channel: str, pending: list) -> None:
    """UnscaledSamplesWarning once per reader and channel, after the backend
    recorded that this channel returned raw counts (factor not set)."""
    if impl is None or channel not in impl.unscaled_channels():
        return
    if announced.new("unscaled", [channel], lambda c: c):
        pending.append((UnscaledSamplesWarning, _unscaled_warning_text(channel),
                        dict(path=path, details=channel, also=_identity_of(impl))))


def _unscaled_warning_text(channel: str) -> str:
    return (f"channel {channel}: a segment read here has no conversion factor set "
            "(units_conversion_factor 0, MEF's \"no entry\"), so its values are the raw "
            "stored counts, not physical units; segments() shows which segments store 0")


def _unusable_factor_message(channel: str, f: float, alternative: str) -> str:
    # Reader's text exactly (unusable_factor_message in session.cpp), so a
    # MultiReader raises the same words.
    value = "nan" if f != f else ("-inf" if f < 0 else "inf")
    return (f"channel {channel}: samples in this window come from a segment that stores "
            f"units_conversion_factor {value}, so they have no value in units; "
            f"{alternative} returns the stored counts, and segments() lists each segment's "
            "factor")


def _format_error(message: str) -> Exception:
    """A mef3io.FormatError (the extension's type, imported when needed)."""
    from . import FormatError
    return FormatError(message)


def _channels_argument(channel, what: str = "read"):
    """``None`` for one channel name (a str); else the list of names.

    A multi-channel read (#90) takes a list or tuple of names; anything else
    is a TypeError, an empty list a ValueError (there is no grid to return).
    """
    if isinstance(channel, str):
        return None
    if isinstance(channel, (list, tuple)):
        names = list(channel)
        if not names:
            raise ValueError(f"{what}: no channels given (pass at least one name)")
        for c in names:
            if not isinstance(c, str):
                raise TypeError(f"{what}: channel names must be str, not {type(c).__name__}")
        return [str(c) for c in names]
    raise TypeError(f"{what}: channel must be a str or a list/tuple of str, "
                    f"not {type(channel).__name__}")


def _one_rate_message(first: str, fs0: float, name: str, fs: float) -> str:
    # Reader's text exactly (read_many_impl in reader.cpp; C++ prints the
    # rates with 17 significant digits), so a MultiReader raises the same words.
    return ("read: a multi-channel read needs ONE sampling rate for all its channels, but "
            f"{first} is sampled at {format(fs0, '.17g')} Hz and {name} at "
            f"{format(fs, '.17g')} Hz; read channels of different rates in separate calls "
            "(one call per rate)")


def _float32_argument(dtype) -> bool:
    """``dtype=`` of a read: True for float32, False for float64 (the default)."""
    if dtype is None:
        raise TypeError("dtype must be float64 or float32, not None")
    try:
        dt = np.dtype(dtype)
    except TypeError:
        raise TypeError(f"dtype must be float64 or float32, not {dtype!r}") from None
    if dt == np.float64:
        return False
    if dt == np.float32:
        return True
    raise ValueError(f"dtype must be float64 or float32, not {dt}")


def _check_threads(value) -> int:
    """An ``n_threads`` value: an int >= 0 (0 = automatic)."""
    if isinstance(value, bool):
        raise TypeError("n_threads must be an int, not bool")
    try:
        n = operator.index(value)
    except TypeError:
        raise TypeError(f"n_threads must be an int, not {type(value).__name__}") from None
    if n < 0:
        raise ValueError(f"n_threads must be >= 0 (0 = automatic), not {n}")
    return n


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
        sessions. MEF encryption does not cover the samples themselves:
        refusing an encrypted session without a password is mef3io's
        policy, not protection (see the encryption model in the docs).
    backend : {"cpp", "pure"}, optional
        Which implementation to use. Defaults to the C++ backend; the pure
        backend is not yet implemented.
    n_threads : int, optional
        How many threads this Reader may use for everything it does —
        opening, file access, decoding, decrypting. ``0`` (default) =
        automatic, ``1`` = no extra threads. Results never depend on it.
        Changeable later through :attr:`n_threads`. See
        [the threads guide](https://bnelair.github.io/mef3io/threads/).
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

    Notes
    -----
    The open itself (scanning the tree, parsing every segment's metadata)
    runs in C++ with the GIL released, as reads do, so opening a large
    session on a worker thread does not freeze other Python threads (a GUI's
    event loop, for instance).

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
        *,
        _defer_warnings: bool = False,
    ):
        # _defer_warnings (private, for MultiReader): keep the open's warnings
        # in self._open_warnings instead of emitting them, so the caller can
        # emit them itself, at ITS caller and from the calling thread.
        pending: list = []
        self._path = str(path)
        self._password = password or ""
        self._backend_name = backend
        self._n_threads = _check_threads(n_threads)
        self._strict = bool(strict)
        self._impl = None  # constructed lazily so a warm start stays cheap
        self._closed = False
        # Guards _impl/_closed/_n_threads (a warm Reader builds its backend on
        # the first call, possibly from several threads at once, and close()
        # may race it) and the once-per-Reader warning bookkeeping.
        self._lock = threading.RLock()
        self._announced = _Announced()

        # Warm start: serve channel metadata from a valid cache snapshot, and
        # defer building the backend until data is actually read. Caching is
        # opt-in (cache=None disables it).
        from . import cache as _cache

        self._cache_path = _cache.resolve_cache_path(self._path, cache)
        self._infos = None
        self._declaration_issues = []
        self._cached_problems = []
        marker = False
        if self._cache_path is not None:
            snap = _cache.load_valid(self._cache_path, self._path)
            # A marker says "a plain open is the fast path for this session"
            # (cache.build_marker): open normally, skipping even the listing.
            marker = _cache.is_marker(snap)
            # A snapshot taken by a lenient open that skipped segments describes
            # the session WITHOUT them; a strict open has to see the failure,
            # so it ignores the snapshot and opens cold (and raises).
            if snap is not None and not marker and not (self._strict and snap.get("problems")):
                self._infos = snap["channel_infos"]
                self._declaration_issues = snap.get("declaration_issues", [])
                self._cached_problems = snap.get("problems", [])

        if self._infos is None:
            # Take the signature BEFORE the backend reads the metadata: a write landing
            # in between then leaves a snapshot that the next load rejects,
            # rather than one whose signature vouches for content it never saw.
            # A path that cannot be listed gets no snapshot; the backend below
            # reports the real error.
            # A layout with no more segments than channels gets a marker, not a
            # snapshot (cache.worth_caching): a snapshot could not make a later
            # open faster, and computing its signature would slow this one down.
            sig = None
            save_marker = False
            if self._cache_path is not None and not marker:
                try:
                    lay = _cache.layout(self._path)
                    if _cache.worth_caching(lay):
                        sig = _cache.signature(self._path, lay)
                    else:
                        save_marker = True
                except OSError:
                    pass
            self._ensure_impl()
            self._infos = {ch: self._impl.info(ch) for ch in self._impl.channels}
            # Free at this point: every segment's metadata is already parsed.
            self._declaration_issues = self._collect_declaration_issues()
            try:
                if self._cache_path is None:
                    pass
                elif self._password_protected():
                    # Encrypted sessions are never cached; drop any snapshot or
                    # marker already there rather than leave it to be found
                    # later — but only one of ours: cache=<path> can name any file.
                    _cache.discard_if_ours(self._cache_path)
                elif sig is not None:
                    _cache.save(
                        self._cache_path,
                        _cache.build_snapshot(self._path, self._infos,
                                              self._declaration_issues, self.problems,
                                              signature_=sig),
                    )
                elif save_marker:
                    _cache.save(self._cache_path, _cache.build_marker(self._path))
                elif marker and _cache.worth_caching_infos(self._infos):
                    # Grown past the threshold since the marker was written; the
                    # next cached open takes the normal path and signs first.
                    _cache.discard(self._cache_path)
            except OSError:
                pass  # a snapshot is an accelerator; failing to save never fails an open

        # Never from a cache snapshot: encrypted sessions are never cached, so
        # an encrypted session always has its backend built here.
        if self._impl is not None:
            _warn_legacy_password(self._impl, self._path, pending)

        if not self._strict:
            problems = self.problems  # from the cache snapshot on a warm open
            if problems:
                pending.append((UnreadableSegmentWarning, _unreadable_warning_text(problems),
                                dict(path=self._path, details=problems,
                                     also=_identity_of(self._impl))))

        from . import _mef3io as _ext  # noqa: PLC0415 - one stat, never cached

        marker = _ext.operation_marker(self._path)
        if marker is not None:
            pending.append((SessionOperationWarning,
                  f"the session is in the middle of an operation ({marker['operation']}, "
                  f"started by {marker['tool']}) that was interrupted or is still running: "
                  "reads are unaffected, writes are refused until it is finished or rolled "
                  "back (run the same operation again)",
                  dict(path=self._path, details=[marker])))

        if warn_declarations and self._declaration_issues:
            pending.append((SessionDeclarationWarning,
                            _declaration_warning_text(self._declaration_issues),
                            dict(path=self._path, details=self._declaration_issues,
                                 also=_identity_of(self._impl))))
        if _defer_warnings:
            self._open_warnings = pending
        else:
            _flush(pending, stacklevel=2)

    def _password_protected(self) -> bool:
        # Fail closed: a backend that cannot say is treated as encrypted, so
        # nothing it decrypted is ever written to a snapshot.
        fn = getattr(self._impl, "password_protected", None)
        return True if fn is None else bool(fn())

    def _collect_declaration_issues(self) -> list:
        # getattr rather than try/except: a bare `except AttributeError` around
        # the call would also swallow one raised *inside* it, and a backend with
        # a typo would then report every session as clean.
        fn = getattr(self._impl, "declaration_issues", None)
        return list(fn()) if fn is not None else []

    @property
    def problems(self) -> list:
        """What a ``strict=False`` Reader skipped instead of raising.

        A list of ``{"channel", "segment_number", "segment", "reason",
        "kind"}`` dicts, empty when everything loaded. **Always empty in strict
        mode**, where the same failure raises instead. ``kind`` is:

        * ``"segment"`` — a segment that could not be read; its time span
          reads back as NaN. Found when the session is opened.
        * ``"records"`` — one record (``.rdat``) file that could not be read
          or placed, found by :meth:`records` (so the list can grow after the
          open). Only its annotations are missing; the samples are unaffected.
          ``segment`` is then the file's path, and ``segment_number`` is -1 for
          a session- or channel-level file (``channel`` is ``""`` at session
          level).

        Check it before trusting a read from a lenient Reader: a skipped
        segment's samples come back as NaN, exactly like a genuine gap, so the
        array alone cannot tell you data is missing.
        """
        impl = self._impl
        if impl is None:
            # Warm open: the backend is not built yet, so report what the
            # snapshot recorded rather than an empty list.
            # (Snapshots written before "kind" existed only held segments.)
            return [{"kind": "segment", **p} for p in self._cached_problems]
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
        # Locked: two threads on a warm (cache=) Reader each built a backend
        # (#110), and a close() racing the first build could leave one set
        # after close, silently reopening the session. The build itself runs
        # with the GIL released, so other threads wait for it on this lock.
        with self._lock:
            if self._closed:
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

    @property
    def n_threads(self) -> int:
        """How many threads this Reader may use (``0`` = automatic).

        Settable at any time: the new count applies from the next call (calls
        already running keep theirs). The open used the value given to the
        constructor. One count covers everything the Reader does — opening,
        file access, decoding, decrypting; see
        [the threads guide](https://bnelair.github.io/mef3io/threads/).
        Results never depend on it.
        """
        return self._n_threads

    @n_threads.setter
    def n_threads(self, value: int) -> None:
        value = _check_threads(value)
        with self._lock:
            self._n_threads = value
            impl = self._impl
            if impl is not None and hasattr(impl, "set_threads"):
                impl.set_threads(value)

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
        with self._lock:
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
            Keys: ``sampling_frequency`` (Hz), ``units_conversion_factor``
            (the factor every segment stores, ``None`` when the channel's
            segments store different ones: :meth:`segments` lists each
            segment's, and the reads scale each sample by its own),
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
        channel,
        t0: Optional[int] = None,
        t1: Optional[int] = None,
        *,
        dtype="float64",
        cancel: Optional["CancelToken"] = None,
        progress: Optional[Callable[[int, int], object]] = None,
    ) -> np.ndarray:
        """Read samples in units on the uniform sampling grid.

        Parameters
        ----------
        channel : str or list of str
            A channel name, or a list (or tuple) of names for a
            MULTI-CHANNEL read: one call returns a 2-D array with one row per
            name, in the order given (duplicates allowed). Every row equals
            :meth:`read` of that channel alone over the same window, value
            for value and NaN for NaN. The channels must share ONE sampling
            rate (they share one grid): mixed rates raise ``ValueError``;
            read each rate in its own call. All channels' file reads run
            concurrently on this Reader's threads and one decode pool works
            through every block of every channel, which is much faster than a
            loop of single reads (see the benchmarks page).
        t0, t1 : int or float, optional
            Half-open time window ``[t0, t1)`` in uUTC. Defaults span the whole
            channel (for a list: from the earliest start to the latest end of
            the listed channels). The number of samples returned is
            ``round((t1 - t0) * fs / 1e6)``.
        dtype : {"float64", "float32"}, optional
            The output type (any spelling ``numpy.dtype`` accepts). float32 is
            the float64 value rounded to the nearest float32, exactly
            ``read(...).astype(numpy.float32)``, in half the memory: about 7
            significant digits (relative error at most 2**-24), so counts up
            to 2**24 in magnitude stay exact with factor 1. Gaps stay NaN.
        cancel : CancelToken, optional
            Stop the read from another thread: it is checked before each
            segment is fetched and before each block is decoded, and a
            cancelled read raises :class:`Cancelled` and returns nothing.
            For a list it stops the whole call.
        progress : callable, optional
            ``progress(done, total)`` — compressed bytes of the blocks being
            decoded (for a list: of every channel, one sequence for the whole
            call). Called at a bounded rate, possibly from a decode worker
            thread (with the GIL held for the call only), never concurrently.
            If it raises, the read stops and the exception propagates.
            Reads on one Reader may run from several threads at once (also
            with ``cache=``), and cancelling one does not affect the others
            (see the Python guide, "Parallelism").

        Returns
        -------
        numpy.ndarray
            For a name: a 1-D array on the sampling grid; for a list: a 2-D
            array of shape ``(len(channel), n)``. Discontinuity gaps are
            filled with ``NaN``; each value is scaled by the units-conversion
            factor of the segment it comes from (segments of one channel may
            store different ones). A segment whose factor is not set (``0``,
            MEF's "no entry") gives its raw stored counts (factor 1), with
            one :class:`UnscaledSamplesWarning` per Reader and channel.

        Raises
        ------
        ValueError
            An empty list, or channels of different sampling rates.
        IndexError
            An unknown channel (for a list: checked before anything is read).
        mef3io.FormatError
            A sample in the window comes from a segment whose factor is
            invalid: NaN or infinite. Nothing is decoded; :meth:`read_raw`
            still returns the counts.

        See Also
        --------
        read_raw : the unscaled int32 form with an explicit validity mask
            (one channel per call).
        """
        names = _channels_argument(channel)
        float32 = _float32_argument(dtype)
        impl = self._ensure_impl()
        hooks = _hooks(cancel, progress)
        if (hooks or names is not None or float32) and self._backend_name != "cpp":
            raise NotImplementedError(
                "cancel=/progress=, dtype= and multi-channel reads need the C++ backend")
        # The thread count is the Reader's (n_threads); there is no per-call
        # override (removed in 2.0, see the threads guide).
        if names is None:
            out = impl.read(channel, t0, t1, float32=float32, **hooks)
        else:
            out = impl.read_many(names, t0, t1, float32=float32, **hooks)
        pending: list = []
        for ch in ([channel] if names is None else dict.fromkeys(names)):
            self._note_unscaled(ch, pending)
        _flush(pending, stacklevel=2)
        return out

    def _note_unscaled(self, channel: str, pending: list) -> None:
        _note_unscaled(self._impl, self._announced, self._path, channel, pending)

    def read_raw(
        self,
        channel: str,
        t0: Optional[int] = None,
        t1: Optional[int] = None,
        *,
        cancel: Optional["CancelToken"] = None,
        progress: Optional[Callable[[int, int], object]] = None,
    ) -> dict:
        """Read the stored int32 counts with an explicit validity mask.

        Parameters
        ----------
        channel : str
            Channel name.
        t0, t1 : int or float, optional
            Half-open ``[t0, t1)`` window in uUTC; defaults span the channel.
        cancel, progress : optional
            As in :meth:`read`.

        Returns
        -------
        dict
            Keys: ``samples`` (int32 ``ndarray``, on the grid),
            ``valid`` (uint8 ``ndarray``; ``0`` marks gap samples with no
            data), ``start_uutc``, ``sampling_frequency``,
            ``units_conversion_factor``, ``units_conversion_factors``.
            Physical units are ``samples * factor`` where ``valid``, with the
            factor of the sample's own segment: ``units_conversion_factors``
            lists the spans (dicts with ``start_index``, ``end_index`` —
            half-open sample indices — and ``units_conversion_factor``), one
            per run of valid samples sharing a factor, sorted, each from its
            first valid sample to just past its last; one span for an
            ordinary window, none when nothing in it is valid.
            Factors are as stored: ``0`` means "not set" (:meth:`read` then
            gives the counts unscaled). ``units_conversion_factor`` is that
            span's factor, ``None`` when
            the window holds samples of segments with different factors, and
            :meth:`info`'s value when nothing is valid.
            ``block_copy_mismatches`` (list of dict) lists any block whose
            `.tidx` entry disagreed with its RED block header, each with
            ``segment``, ``block_index``, ``index_start_uutc``,
            ``header_start_uutc``, ``index_number_of_samples`` and
            ``header_number_of_samples``. Empty for a well-formed session;
            non-empty raises :class:`BlockCopyWarning` as well, and means the
            FILE is inconsistent — the header copy was used, which is what
            meflib and pymef do, so the read still matches theirs.
        """
        pending: list = []
        out = self._read_raw(channel, t0, t1, _hooks(cancel, progress), pending)
        _flush(pending, stacklevel=2)
        return out

    def _read_raw(self, channel, t0, t1, hooks: dict, pending: list, _split=None) -> dict:
        # read_raw with its warning collected into `pending` (see _flush).
        # `_split` is PRIVATE to MultiReader: this call's share of ITS one
        # thread budget (the backend's `_n_threads`). Never expose it — the
        # public rule is one thread count per object.
        impl = self._ensure_impl()
        if hooks and self._backend_name != "cpp":
            raise NotImplementedError("cancel=/progress= need the C++ backend")
        if _split is None:
            out = impl.read_raw(channel, t0, t1, **hooks)
        else:
            out = impl.read_raw(channel, t0, t1, _n_threads=int(_split), **hooks)
        mismatches = out.get("block_copy_mismatches") or []
        if mismatches:
            pending.append((BlockCopyWarning, _block_copy_warning_text(mismatches),
                            dict(path=self._path, details=mismatches,
                                 also=_identity_of(impl))))
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
            ``number_of_blocks``, ``units_conversion_factor`` (the
            segment's own, as stored: ``0.0`` is MEF's "no entry"), the
            on-disk ``path``, and ``provenance``:
            ``None`` for a file from another writer or from mef3io before 1.2,
            otherwise a dict with ``created_by`` / ``last_modified_by``
            (version strings, ``""`` if unknown), ``last_operation``,
            ``operations`` (every operation ever applied: ``"create"``,
            ``"append"``, ``"header-repair"``, ``"recovery"``, ...),
            ``modification_count`` and ``layout_version``. Versions and
            operations only — no times are recorded.
        """
        return self._ensure_impl().segments(channel)

    def block_envelope(self, channel, t0: Optional[int] = None,
                       t1: Optional[int] = None) -> dict:
        """Minimum and maximum of every data block, from the block index alone.

        A fast envelope for viewers: only each segment's block index is read
        (about 2% of the data), no signal is decoded, and segments outside the
        window are skipped. A whole day of a channel recorded in 10-second
        blocks is 8,640 rows.

        Parameters
        ----------
        channel : str or list of str
            Channel name, or a list (or tuple) of names: then the result is a
            dict keyed by channel name (in the order given, duplicates once),
            each value the dict one name returns. The indices of all listed
            channels are read concurrently on this Reader's threads; still
            index only. Any mix of sampling rates is fine here.
        t0, t1 : int, optional
            Window in uUTC, ``[t0, t1)``; a block is returned when it overlaps
            the window. ``None`` (default) means the start or end of the
            channel.

        Returns
        -------
        dict of numpy.ndarray
            One row per block, in time order: ``start_uutc`` (int64, the first
            sample), ``end_uutc`` (int64, just past the last sample: start +
            samples / fs), ``min`` and ``max`` (float64, in the channel's
            units, scaled like :meth:`read`: each block by its own segment's
            conversion factor). ``min``/``max`` are NaN where
            the writer left a block's extremes unset. Gaps in the recording
            are simply absent rows: compare ``end_uutc`` with the next
            ``start_uutc``.

        Notes
        -----
        The resolution is the block length: mef3io writes 10-second blocks by
        default (1-second blocks at 5 kHz and above), ``Writer(block_length=)``
        sets it, and other writers choose their own. Block times come from the
        index; :meth:`read` places samples by each block's own header, which
        agrees on every consistent file (see :meth:`read_raw` for blocks
        whose two copies disagree).

        Raises
        ------
        mef3io.FormatError
            A block that :meth:`read` of the same window would take samples
            from (decided by read's own window and grid rule) comes from a
            segment whose conversion factor is NaN or infinite, as
            :meth:`read` refuses it; :meth:`toc` returns the counts. A block of such a
            segment that the read would not touch (one overlapping the
            window by less than half a sample) gets NaN ``min``/``max``.
        """
        names = _channels_argument(channel, "block_envelope")
        pending: list = []
        if names is None:
            out = self._block_envelope(channel, t0, t1, pending)
        else:
            out = self._block_envelope_many(names, t0, t1, pending)
        _flush(pending, stacklevel=2)
        return out

    def _block_envelope(self, channel, t0, t1, pending: list) -> dict:
        # block_envelope with its warning collected into `pending` (MultiReader).
        out = self._ensure_impl().block_envelope(channel, t0, t1)
        self._note_unscaled(channel, pending)
        return out

    def _block_envelope_many(self, names, t0, t1, pending: list) -> dict:
        unique = list(dict.fromkeys(names))
        parts = self._ensure_impl().block_envelope_many(unique, t0, t1)
        for ch in unique:
            self._note_unscaled(ch, pending)
        return dict(zip(unique, parts))

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

    def records(
        self,
        channel: Optional[str] = None,
        segment: Optional[int] = None,
        level: Optional[str] = None,
    ) -> list[dict]:
        """Read records (annotations) at the session, channel or segment level.

        MEF 3.0 keeps record files at three levels: the session
        (``<session>.mefd``), each channel (``<ch>.timd``) and each segment
        (``<ch>-NNNNNN.segd``). Every returned record says where it came from.

        Parameters
        ----------
        channel : str or None, optional
            ``None`` (default) for the session scope; a channel name for that
            channel's records.
        segment : int or None, optional
            A segment number (requires ``channel``): only that segment's
            records.
        level : {None, "session", "channel", "segment", "all"}, optional
            Which levels to return. ``None`` (default) keeps each scope's
            natural meaning: session-level records when ``channel`` is
            ``None``; the channel's channel-level AND segment-level records
            when it is given; the segment's when ``segment`` is given.
            ``"all"`` without a channel returns every record in the session,
            including channels that have records but no data yet (not in
            :attr:`channels`, which lists channels with samples).
            A level outside the scope (``"session"`` with a channel) raises
            ``ValueError``.

        Returns
        -------
        list of dict
            One dict per record, grouped session -> channel -> its segments.
            Every dict has ``type`` (e.g. ``"Note"``), ``time`` (absolute
            uUTC), ``level`` (``"session"``/``"channel"``/``"segment"``),
            ``channel`` (``None`` at session level), ``segment`` (``None``
            unless segment level), ``version_major`` and ``version_minor``,
            then the body fields of its type, always the same keys for a type
            (``None`` where the body does not carry one) — e.g. ``text`` for
            Note/SyLg, ``duration`` + ``text`` for EDFA, ``earliest_onset`` ...
            ``channels`` for Seiz. Body times are absolute uUTC like ``time``.
            The full per-type list is in the MEF format reference.

        Raises
        ------
        mef3io._mef3io.FormatError
            A record file cannot be read, or the segments a channel or the
            session spans disagree on the recording-time offset AND its file
            stores offset-dependent (negated) times, so they cannot be placed.
            Only when strict: a lenient Reader skips that file, lists it in
            :attr:`problems` and emits :class:`UnreadableSegmentWarning`; the
            other files (e.g. the segment-level ones) are still returned.
        ValueError
            A ``segment`` without a ``channel``, or a level outside the scope.
        IndexError
            Unknown channel or segment number.
        """
        pending: list = []
        try:
            return self._records(channel, segment, level, pending)
        finally:
            _flush(pending, stacklevel=2)

    def _records(self, channel, segment, level, pending: list) -> list:
        # records() with its warnings collected into `pending` (see _flush);
        # they are delivered even when the call raises.
        impl = self._ensure_impl()
        with _warning_on_legacy_records(self._announced, impl, self._path, pending):
            out = impl.records(channel, segment, level)
        # Each skipped record file is warned about once per Reader (see
        # _unannounced); segment problems were reported by the open.
        added = self._announced.new(
            "records", [p for p in impl.problems() if p.get("kind") == "records"],
            lambda p: (p["channel"], p["segment_number"], p["segment"]))
        if added:
            # Lenient only (strict raises): a record file that could not be
            # read was skipped. Say so — a missing annotation is otherwise
            # indistinguishable from one that was never written.
            pending.append((UnreadableSegmentWarning,
                            f"{len(added)} record file(s) could not be read and were "
                            f"SKIPPED. First: {added[0]['segment']} — {added[0]['reason']}. "
                            "Full list in Reader.problems. Open with strict=True to fail instead.",
                            dict(path=self._path, details=added, also=_identity_of(impl))))
        return out
