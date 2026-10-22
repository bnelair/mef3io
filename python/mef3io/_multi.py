"""Several recordings read as one: :class:`MultiReader`.

A recorder that starts a new session every day leaves a patient's stay in
several ``.mefd`` folders (or ``.mefd.tar`` archives). :class:`MultiReader`
reads them as ONE recording without writing anything: it opens a
:class:`mef3io.Reader` per recording and combines what they return. Every
public member of :class:`Reader` is here with the same arguments, return
types, dict keys and dtypes, so code written for a Reader works unchanged.

The contract (pinned by ``tests/test_p39_multi_reader.py``): a
:meth:`MultiReader.read` returns exactly what a :class:`Reader` returns for
the same window of the same recordings merged on disk with
:func:`mef3io.merge_sessions`.
"""
from __future__ import annotations

import math
import os
import threading
from concurrent.futures import ThreadPoolExecutor
from typing import Callable, Optional

import numpy as np

from ._cancel import CancelToken, Cancelled, _hooks
from ._reader import Reader, _flush
from ._warnings import MefWarning, emit as _emit, identity_of as _identity_of

__all__ = ["MultiReader", "MultiReaderDifferenceWarning"]

# info() keys that describe the session, not the channel: combined over EVERY
# recording (also those without the channel), so info() and metadata agree.
_SESSION_WIDE = ("subject_name_1", "subject_name_2", "subject_id", "recording_location",
                 "gmt_offset", "session_description")


class MultiReaderDifferenceWarning(MefWarning):
    """The recordings a :class:`MultiReader` combines disagree on a value.

    Raised once when the MultiReader is opened. A value the recordings agree
    on reads as that value; a value they disagree on (a subject field, the
    units, a filter setting, a session description, ...) reads as ``None``
    in :meth:`MultiReader.info` and :attr:`MultiReader.metadata`, never as one
    recording's value. Different patient names or IDs can mean the
    recordings are not the same patient's: check before analysing them
    together.

    The message names the differing FIELDS only, never their values or a
    path. ``w.message.details`` is the tuple of field names;
    ``w.message.path`` is the first recording (in time order). Each
    recording's own values are in :attr:`MultiReader.metadata_by_source`, or
    open it with :class:`Reader`.
    """


def _paths_argument(paths) -> list:
    if isinstance(paths, (str, bytes, os.PathLike)):
        raise TypeError(
            "MultiReader takes a sequence of session paths, e.g. "
            "MultiReader([path1, path2]); for one session use mef3io.Reader(path)"
        )
    try:
        items = list(paths)
    except TypeError:
        raise TypeError("paths must be a sequence of session paths "
                        f"(str or os.PathLike), not {type(paths).__name__}") from None
    for i, p in enumerate(items):
        if not isinstance(p, (str, os.PathLike)) or isinstance(p, bytes):
            raise TypeError(f"paths[{i}] must be a str or os.PathLike, not {type(p).__name__}")
    out = [os.fspath(p) for p in items]
    if not all(isinstance(p, str) for p in out):
        raise TypeError("paths must be text paths (str or os.PathLike of str)")
    if not out:
        raise ValueError("MultiReader needs at least one session path")
    seen: dict = {}
    for i, p in enumerate(out):
        key = os.path.normcase(os.path.realpath(p.rstrip("/\\") or p))
        if key in seen:
            raise ValueError(f"paths[{seen[key]}] and paths[{i}] are the same recording")
        seen[key] = i
    return out


def _cache_argument(cache, n: int) -> list:
    if cache is None or cache is False or cache is True or cache == "auto":
        return [cache] * n
    if isinstance(cache, (str, bytes, os.PathLike)):
        raise ValueError(
            "a MultiReader cannot share one cache file between recordings: pass "
            "cache='auto', or a sequence with one cache path per recording (in the "
            "order of paths)"
        )
    caches = list(cache)
    if len(caches) != n:
        raise ValueError(f"cache: {len(caches)} cache path(s) for {n} recording(s)")
    return caches


def _llround(x: float) -> int:
    # C's llround: halves away from zero (Python's round() goes to even).
    # x - floor(x) is exact for a double, so the comparison is too.
    f = math.floor(x)
    r = f + 1 if x - f >= 0.5 else f
    if x < 0 and x - f == 0.5:
        r = f
    return int(r)


def _agree(values: list):
    """The value every recording holds, or None when they differ."""
    first = values[0]
    for v in values[1:]:
        if v != first or type(v) is not type(first):
            return None
    return first


def _session_name(path: str) -> str:
    base = os.path.basename(os.fspath(path).rstrip("/\\"))
    for suffix in (".mefd.tar", ".mefd"):
        if base.lower().endswith(suffix):
            return base[: -len(suffix)]
    return base


def _is_default_description(description, path: str, reader) -> bool:
    """True for an unset session description or the writer's default: the
    recording's own session name (the folder or archive name without its
    suffix, or, for a renamed archive, the folder inside it)."""
    if description is None or description == "":
        return True
    names = {_session_name(path)}
    for item in _identity_of(reader._impl):
        if item:
            names.add(_session_name(str(item).partition("::")[2] or str(item)))
    return description in names


def _threads_total(n_threads: int) -> int:
    return n_threads if n_threads and n_threads > 0 else (os.cpu_count() or 1)


def _run(fn, items: list, workers: int) -> list:
    """``[fn(x) for x in items]``, on up to ``workers`` threads.

    Every call finishes before this returns (none is left running behind the
    caller's back); the first exception in item order is raised, preferring
    one that is not :class:`Cancelled`.
    """
    if len(items) <= 1 or workers <= 1:
        return [fn(x) for x in items]
    with ThreadPoolExecutor(max_workers=min(workers, len(items))) as ex:
        futures = [ex.submit(fn, x) for x in items]
    results, error = [], None
    for f in futures:
        exc = f.exception()
        # A Cancelled that one failure caused in the others is not the cause.
        if exc is not None and (error is None or (isinstance(error, Cancelled)
                                                  and not isinstance(exc, Cancelled))):
            error = exc
        results.append(None if exc is not None else f.result())
    if error is not None:
        raise error
    return results


class MultiReader:
    """Several MEF 3.0 recordings read as one, without merging them on disk.

    A drop-in for :class:`mef3io.Reader`: the same methods and properties,
    with the same arguments, return types, dict keys and dtypes. The only
    differences are the first argument (a sequence of sessions) and that
    ``n_threads`` is the budget for the whole call, shared between the
    recordings a read touches and the decode threads of each.

    What a read returns is exactly what :class:`Reader` returns for the same
    window of the same recordings merged with :func:`mef3io.merge_sessions`:
    the same length, the same values and NaN in the same places. Samples are
    placed on the same sampling grid by the same rule (each recording is read
    on the window's own grid, so nothing is re-rounded), and where two
    recordings touch within half a sample, the block that starts later wins,
    as it does in a merged session.

    Parameters
    ----------
    paths : sequence of str or os.PathLike
        The sessions, each anything :class:`Reader` opens (a ``.mefd`` folder
        or a ``.mefd.tar`` archive), in any order: they are ordered by their
        first sample. A single path (not in a sequence) is a ``TypeError``;
        an empty sequence or the same session twice is a ``ValueError``.
    password : str, optional
        The password of every recording.
    backend, warn_declarations, strict : optional
        Passed to every :class:`Reader`.
    n_threads : int, optional
        Total thread budget (``0``, the default, = all cores). A read that
        touches several recordings reads them at once, splitting the budget
        between them. Results do not depend on it.
    cache : None, "auto" or sequence, optional
        ``None`` (default) or ``"auto"`` as for :class:`Reader`, applied to
        every recording; or one cache path per recording, in the order of
        ``paths``. One path for all is a ``ValueError`` (the recordings would
        overwrite each other's snapshot).

    Raises
    ------
    ValueError
        A channel has different sampling rates in different recordings, or
        two recordings hold samples of the same channel at the same time
        (compared on the recorded blocks; overlaps up to half a sample are
        recorder rounding and allowed, the rule of
        :func:`mef3io.merge_sessions`). Recordings that take turns (A, then
        B, then A again) are fine.

    Notes
    -----
    How values are combined:

    * :attr:`channels`: every channel of any recording, sorted. A channel a
      recording does not have reads as a gap over that recording's time.
    * :meth:`info`: ``start_time`` the earliest, ``end_time`` the latest,
      ``number_of_samples`` and ``n_segments`` the sums,
      ``section3_available`` only if every recording has it. The patient
      fields (``subject_*``, ``recording_location``, ``gmt_offset``) and
      ``session_description`` are combined over EVERY recording, also those
      without the channel, so :meth:`info` and :attr:`metadata` always
      agree. Every other field is the value the recordings holding the
      channel agree on. A field they disagree on is ``None`` (a
      :class:`MultiReaderDifferenceWarning` names such fields once, on
      open). A session description equal to its own recording's name
      (what mef3io and the legacy stack write by default) counts as unset:
      day files named by date do not "disagree" on it, and when every
      recording has only the default it is ``None`` (one recording keeps
      its own). :func:`merge_sessions` keeps the FIRST source's
      description, so for this one field ``metadata`` is not what a merged
      session gives.
    * :attr:`metadata`: a :class:`mef3io.Metadata` with those same combined
      patient fields and session description, the rest from :meth:`info` of
      the first channel (as :class:`Reader` does). Each recording's own
      values: :attr:`metadata_by_source`.
    * :meth:`segments`: every recording's segments in time order, numbered
      from 0 like the segments of a merged session; ``path`` is the segment's
      real folder. ``start_sample`` counts the samples stored before it.
      Unlike a merged session, a segment is never split around another
      recording's data, so recordings that take turns can give fewer
      segments than :func:`merge_sessions` would.
    * :meth:`toc` and :meth:`block_envelope`: every block in time order;
      ``start_sample`` as above.
    * :meth:`records`: session- and channel-level records merged in time
      order, then each segment's own; ``segment`` uses the numbering of
      :meth:`segments` (``records(channel, segment=k)`` too).
    * :attr:`problems`, :attr:`declaration_issues`: every recording's, in time
      order; a declaration issue's ``segment`` uses the numbering of
      :meth:`segments`, a problem keeps the number of the recording's own
      folder (an unreadable segment is in no segment list) and its path.
    * Warnings: each recording's own (declarations, unreadable segments, ...)
      are emitted once, on open, with ``w.message.path`` naming that
      recording, plus one :class:`MultiReaderDifferenceWarning`.

    Where it cannot equal a merged session:

    * Recordings that store different units-conversion factors for one
      channel: :meth:`read` scales each recording's samples by its own factor
      (a merged session would scale them all by one), :meth:`read_raw`
      refuses a window that touches recordings with different factors (the
      counts would mean different things), and ``info`` reports ``None``.
    * A :meth:`read` reads each recording it touches from the window's start,
      so the window's own grid applies with no re-rounding: a recording late
      in a long window allocates int32 counts and a validity byte for the
      part of the window before it as well: about 5 bytes per sample per
      recording touched, plus 5 for the combined counts, beside the float64
      result.
    * ``progress(done, total)``: ``total`` can grow while the recordings
      start reading (each part knows its size only when it starts); ``done``
      never goes back.

    Calls on one MultiReader may overlap across threads, as on a
    :class:`Reader` (with the same limits; see the Python guide).

    Examples
    --------
    >>> days = sorted(Path("/data/patient7").glob("*.mefd"))
    >>> with mef3io.MultiReader(days) as r:
    ...     x = r.read("Fp1", t0, t0 + 3_600_000_000)   # across midnight
    """

    def __init__(
        self,
        paths,
        password: str = "",
        backend: str = "cpp",
        n_threads: int = 0,
        cache=None,
        warn_declarations: bool = True,
        strict: bool = True,
    ):
        given = _paths_argument(paths)
        caches = _cache_argument(cache, len(given))
        self._n_threads = n_threads
        self._closed = False
        self._lock = threading.Lock()
        self._segment_maps: dict = {}
        self._declaration_issues_global = None

        def open_one(i):
            return Reader(given[i], password, backend, n_threads, caches[i],
                          warn_declarations, strict, _defer_warnings=True)

        opened: list = [None] * len(given)
        error = None
        if len(given) == 1:
            opened[0] = open_one(0)
        else:
            workers = min(len(given), _threads_total(n_threads), 16)
            with ThreadPoolExecutor(max_workers=workers) as ex:
                futures = [ex.submit(open_one, i) for i in range(len(given))]
            for i, f in enumerate(futures):
                if f.exception() is not None:
                    error = error or f.exception()
                else:
                    opened[i] = f.result()
        if error is not None:
            for r in opened:
                if r is not None:
                    r.close()
            raise error

        try:
            def first_sample(i):
                r = opened[i]
                starts = [r.info(ch)["start_time"] for ch in r.channels]
                return (min(starts) if starts else math.inf, i)

            order = sorted(range(len(given)), key=first_sample)
            self._readers = [opened[i] for i in order]
            self._paths = tuple(given[i] for i in order)
            self._prepare()
        except BaseException:
            for r in opened:
                r.close()
            raise

        try:  # a warning raised as an error (-W error) must not leave them open
            for r in self._readers:
                pending, r._open_warnings = r._open_warnings, []
                _flush(pending, stacklevel=2)
            if self._differing:
                _emit(MultiReaderDifferenceWarning,
                      f"the {len(self._readers)} recordings differ in "
                      f"{', '.join(self._differing)}; MultiReader.info and .metadata give None "
                      "for these fields (each recording's own values: "
                      "MultiReader.metadata_by_source, or open it with mef3io.Reader)",
                      path=self._paths[0], details=self._differing, stacklevel=2)
        except BaseException:
            self.close()
            raise

    # ---- open-time combination and checks ---------------------------------

    def _prepare(self) -> None:
        holders: dict = {}
        for k, r in enumerate(self._readers):
            for ch in r.channels:
                holders.setdefault(ch, []).append(k)
        self._holders = {ch: holders[ch] for ch in sorted(holders)}

        differing: set = set()
        self._session_wide = self._combine_session_wide(differing)
        self._infos = {}
        for ch, ks in self._holders.items():
            infos = [self._readers[k].info(ch) for k in ks]
            rates = sorted({i["sampling_frequency"] for i in infos})
            if len(rates) > 1:
                raise ValueError(
                    f"channel {ch} has different sampling rates in different recordings "
                    f"({', '.join(f'{x:g} Hz' for x in rates)}); a channel keeps one rate"
                )
            combined = {}
            for key in infos[0]:
                vals = [i.get(key) for i in infos]
                if key == "start_time":
                    combined[key] = min(vals)
                elif key == "end_time":
                    combined[key] = max(vals)
                elif key in ("number_of_samples", "n_segments"):
                    combined[key] = sum(vals)
                elif key == "section3_available":
                    combined[key] = all(vals)
                elif key in self._session_wide:
                    combined[key] = self._session_wide[key]  # one rule: every recording
                else:
                    combined[key] = _agree(vals)
                    if combined[key] is None and any(v is not None for v in vals):
                        differing.add(key)
            self._infos[ch] = combined
            self._check_overlap(ch, ks, infos)

        self._metadata_by_source = {p: r.metadata for p, r in zip(self._paths, self._readers)}
        self._differing = tuple(sorted(differing))

    def _combine_session_wide(self, differing: set) -> dict:
        """The patient fields and the session description over EVERY recording.

        Each recording's values come from its first channel, as
        :attr:`Reader.metadata` takes them. A session description equal to
        its own recording's name is the writer's default (mef3io and the
        legacy stack both write the session name there), so it is left out
        of the comparison: day files named after their dates must not
        "disagree" on it. Real descriptions that agree give that value;
        none (only defaults) gives ``None``, except for a single recording,
        which keeps its own value as a Reader does.
        """
        per = []
        for path, r in zip(self._paths, self._readers):
            chans = r.channels
            per.append((path, r, r.info(chans[0]) if chans else {}))
        # As Reader.metadata: subject fields only with level-2 access, here
        # to every recording.
        self._section3_everywhere = all(info.get("section3_available") for _, _, info in per
                                        if info)
        out = {}
        for key in _SESSION_WIDE:
            if key == "session_description":
                continue
            vals = [info.get(key) for _, _, info in per if info]
            if not vals:
                continue
            out[key] = _agree(vals)
            if out[key] is None and any(v is not None for v in vals):
                differing.add(key)
        descriptions, real = [], []
        for path, r, info in per:
            if not info:
                continue
            d = info.get("session_description")
            descriptions.append(d)
            if not _is_default_description(d, path, r):
                real.append(d)
        if len(descriptions) == 1:
            out["session_description"] = descriptions[0]
        elif descriptions:
            out["session_description"] = _agree(real) if real else None
            if real and out["session_description"] is None:
                differing.add("session_description")
        return out

    def _check_overlap(self, ch: str, ks: list, infos: list) -> None:
        """Refuse two recordings holding samples of ``ch`` at the same time.

        The rule of merge_sessions: compared on the recorded blocks, with
        half a sample of tolerance. Recording spans that do not meet are
        never looked into, so consecutive recordings cost nothing here.
        """
        if len(ks) < 2:
            return
        fs = infos[0]["sampling_frequency"]
        period = 1e6 / max(fs, 1e-9)
        tol = _llround(0.5e6 / max(fs, 1e-9))
        for a in range(len(ks)):
            for b in range(a + 1, len(ks)):
                ia, ib = infos[a], infos[b]
                if ia["end_time"] + period <= ib["start_time"] or \
                        ib["end_time"] + period <= ia["start_time"]:
                    continue
                lo = max(ia["start_time"], ib["start_time"]) - int(period) - 1
                hi = min(ia["end_time"], ib["end_time"]) + int(period) + 1
                ea = self._readers[ks[a]].block_envelope(ch, lo, hi)
                eb = self._readers[ks[b]].block_envelope(ch, lo, hi)
                bs, be = eb["start_uutc"], eb["end_uutc"]
                order = np.argsort(bs, kind="stable")
                bs, be = bs[order], be[order]
                for xs, xe in sorted(zip(ea["start_uutc"].tolist(), ea["end_uutc"].tolist())):
                    # b blocks that start before x ends (less the tolerance)
                    # and end after x starts (plus the tolerance).
                    q = np.searchsorted(bs, xe - tol, side="left")
                    if q and np.any(be[:q] > xs + tol):
                        j = int(np.flatnonzero(be[:q] > xs + tol)[0])
                        raise ValueError(
                            f"channel {ch}: {self._paths[ks[a]]} and {self._paths[ks[b]]} "
                            f"both hold samples from {max(xs, int(bs[j]))} to "
                            f"{min(xe, int(be[j]))} (uUTC); recordings read together must "
                            "not overlap"
                        )

    # ---- Reader's public members ------------------------------------------

    @property
    def paths(self) -> tuple:
        """The recordings, in time order (as given, as ``str``). Not on :class:`Reader`."""
        return self._paths

    @property
    def metadata_by_source(self) -> dict:
        """Each recording's own :attr:`Reader.metadata`, keyed by its path as given, in
        time order. Not on :class:`Reader`."""
        return dict(self._metadata_by_source)

    @property
    def problems(self) -> list:
        """Every recording's :attr:`Reader.problems`, in time order.

        Same dicts as :attr:`Reader.problems`; ``segment`` holds the path of
        the file, so it says which recording. ``segment_number`` is the
        number of that recording's own segment folder (an unreadable segment
        is not in :meth:`segments`).
        """
        out = []
        for r in self._readers:
            out.extend(r.problems)
        return out

    @property
    def declaration_issues(self) -> list:
        """Every recording's :attr:`Reader.declaration_issues`, in time order.

        ``segment`` is the number in :meth:`segments`; the warning emitted on
        open (one per recording) carries each recording's own numbers.
        """
        with self._lock:
            cached = self._declaration_issues_global
        if cached is None:
            cached = []
            for k, r in enumerate(self._readers):
                for issue in r.declaration_issues:
                    issue = dict(issue)
                    ch, seg = issue.get("channel"), issue.get("segment")
                    if ch in self._holders and isinstance(seg, int):
                        issue["segment"] = self._segment_map(ch)[0].get((k, seg), seg)
                    cached.append(issue)
            with self._lock:
                self._declaration_issues_global = cached
        return [dict(i) for i in cached]

    def __enter__(self) -> "MultiReader":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def close(self) -> None:
        """Close every recording. As :meth:`Reader.close`: a further read raises."""
        self._closed = True
        for r in self._readers:
            r.close()

    def _check_open(self) -> None:
        if self._closed:
            raise ValueError(
                "this Reader is closed; open a new one "
                "(close() releases the backend's file handles, so reusing it "
                "would silently reopen the session)"
            )

    @property
    def channels(self) -> list[str]:
        """Channel names of every recording, sorted.

        Returns
        -------
        list of str
        """
        return list(self._infos.keys())

    def info(self, channel: str) -> dict:
        """Channel metadata, combined over the recordings that hold the channel.

        Same keys as :meth:`Reader.info`. ``start_time`` is the earliest,
        ``end_time`` the latest, ``number_of_samples`` and ``n_segments`` the
        sums, ``section3_available`` true only if every recording has it. The
        patient fields and ``session_description`` are combined over every
        recording (as :attr:`metadata`); any other field is the value the
        recordings holding the channel agree on. A field they disagree on is
        ``None``. See the class notes for the session-description rule.
        """
        if channel not in self._infos:
            return self._readers[0].info(channel)  # Reader's own error
        return dict(self._infos[channel])

    @property
    def metadata(self):
        """Session metadata as a :class:`mef3io.Metadata`, combined over every recording.

        The subject fields and the session description: the value every
        recording holds, or ``None`` where they differ (a description equal
        to its own recording's name is the default and does not count; see
        the class notes). The other acquisition
        fields come from :meth:`info` of the first channel, as on
        :class:`Reader` (so ``None`` where the recordings holding it differ).
        Subject fields are empty unless every recording was opened with
        level-2 access.
        """
        from .metadata import Acquisition, Metadata, Subject

        chans = self.channels
        if not chans:
            return Metadata()
        info = self._infos[chans[0]]
        wide = self._session_wide

        subj = Subject()
        if self._section3_everywhere:
            subj = Subject(
                name_1=wide.get("subject_name_1"),
                name_2=wide.get("subject_name_2"),
                id=wide.get("subject_id"),
                recording_location=wide.get("recording_location"),
                gmt_offset=wide.get("gmt_offset"),
            )
        base = Metadata.from_info(info).acquisition

        def field(key, value):
            return None if info.get(key, 0) is None else value

        acq = Acquisition(
            session_description=wide.get("session_description"),
            channel_description=field("channel_description", base.channel_description),
            reference_description=field("reference_description", base.reference_description),
            acquisition_channel_number=field("acquisition_channel_number",
                                             base.acquisition_channel_number),
            low_frequency_filter=field("low_frequency_filter", base.low_frequency_filter),
            high_frequency_filter=field("high_frequency_filter", base.high_frequency_filter),
            notch_filter=field("notch_filter", base.notch_filter),
            line_frequency=field("line_frequency", base.line_frequency),
        )
        return Metadata(subject=subj, acquisition=acq)

    # ---- reads -------------------------------------------------------------

    def _window(self, channel, t0, t1):
        info = self.info(channel)
        fs = info["sampling_frequency"]
        t0 = info["start_time"] if t0 is None else int(t0)
        t1 = info["end_time"] if t1 is None else int(t1)
        n = max(_llround(float(t1 - t0) * fs / 1e6), 0)
        period = 1e6 / max(fs, 1e-9)
        margin = max(period, 1e6)  # header times can be off; reading one too many costs nothing
        touched = []
        for k in self._holders[channel]:
            i = self._readers[k].info(channel)
            if i["start_time"] >= t1 + margin or i["end_time"] + margin <= t0:
                continue
            touched.append(k)
        return fs, t0, t1, n, period, touched

    def _gather(self, channel, t0, t1, n_threads, cancel, progress):
        """Read ``[t0, t1)`` from every recording it touches, on the window's grid.

        Returns ``(fs, t0, n, touched, results)``: results[j] is recording
        touched[j]'s read_raw over ``[t0, t1_j)`` with t1_j <= t1. The same t0
        for every recording is what makes the placement exact: each sample
        lands in the slot a read of the merged session gives it, computed by
        the same expression, with nothing re-rounded.
        """
        self._check_open()
        _hooks(cancel, progress)  # validate the arguments as Reader does
        fs, t0, t1, n, period, touched = self._window(channel, t0, t1)
        budget = _threads_total(self._n_threads if n_threads is None else n_threads)
        workers = min(len(touched), budget) or 1
        per_read = max(1, budget // workers)
        if len(touched) == 1:
            per_read = n_threads  # exactly the Reader call

        internal = None
        if cancel is None and len(touched) > 1:
            internal = CancelToken()  # stops the others when one read fails
        token = cancel if cancel is not None else internal
        lock = threading.Lock()
        done: dict = {}
        totals: dict = {}

        def hooks_for(j):
            h = {}
            if token is not None:
                h["cancel"] = token._impl
            if progress is not None:
                if len(touched) == 1:
                    h["progress"] = progress
                else:
                    def cb(d, t, j=j):
                        with lock:  # never concurrently, as Reader promises
                            done[j], totals[j] = d, t
                            return progress(sum(done.values()), sum(totals.values()))
                    h["progress"] = cb
            return h

        def one(j):
            r = self._readers[touched[j]]
            end = r.info(channel)["end_time"]
            limit = t1 if j == len(touched) - 1 else min(t1, int(math.ceil(end + 2 * period)) + 1)
            pending: list = []
            try:
                out = r._read_raw(channel, t0, limit, per_read, hooks_for(j), pending)
                v = out["valid"]
                if limit < t1 and len(v) and v[-1]:
                    # Data reached the shortened window's end: the header end
                    # time understated it. Read the full window.
                    pending = []
                    out = r._read_raw(channel, t0, t1, per_read, hooks_for(j), pending)
            except BaseException:
                if internal is not None:
                    internal.cancel()
                raise
            return out, pending

        results = _run(one, list(range(len(touched))), workers)
        return fs, t0, n, touched, results

    def _assemble(self, channel, fs, t0, n, touched, results, need_owner: bool):
        samples = np.zeros(n, dtype=np.int32)
        valid = np.zeros(n, dtype=np.uint8)
        owner = np.full(n, -1, dtype=np.int32) if need_owner else None
        collided = []
        for j, (out, _) in enumerate(results):
            s, v = out["samples"], out["valid"]
            m = v != 0
            nk = len(v)
            hit = np.flatnonzero(m & (valid[:nk] != 0))
            if hit.size:
                collided.append(hit)
            np.copyto(samples[:nk], s, where=m)
            valid[:nk] |= v
            if owner is not None:
                owner[:nk][m] = j
        if collided:
            self._resolve(channel, fs, t0, touched, results, np.unique(np.concatenate(collided)),
                          samples, owner)
        return samples, valid, owner

    def _resolve(self, channel, fs, t0, touched, results, slots, samples, owner):
        """Give each slot two recordings claim to the block that starts later.

        Two recordings touch within half a sample (recorder rounding) and a
        sample of each rounds into the same slot. A merged session orders its
        segments by time and the later block wins (Reader's rule), so the
        same here, from each block's start as the index gives it.
        """
        starts = {}
        period = 1e6 / fs
        # Only the index around the shared slots: every block covering one
        # overlaps this range.
        lo = int(t0 + int(slots[0]) * period - 2 * period)
        hi = int(t0 + (int(slots[-1]) + 1) * period + 2 * period)
        for j, k in enumerate(touched):
            env = self._readers[k].block_envelope(channel, lo, hi)
            st = np.sort(env["start_uutc"])
            slot = np.ceil((st - t0).astype(np.float64) * fs / 1e6 - 0.5)
            starts[j] = (st, slot)
        for i in slots.tolist():
            best, best_key = None, None
            for j, (out, _) in enumerate(results):
                v = out["valid"]
                if i >= len(v) or not v[i]:
                    continue
                st, slot = starts[j]
                pos = int(np.searchsorted(slot, i, side="right")) - 1
                key = (int(st[pos]) if pos >= 0 else -(2 ** 62), j)
                if best_key is None or key > best_key:
                    best, best_key = j, key
            samples[i] = results[best][0]["samples"][i]
            if owner is not None:
                owner[i] = best

    def _ufacts(self, channel, touched) -> list:
        return [self._readers[k].info(channel)["units_conversion_factor"] for k in touched]

    def read(
        self,
        channel: str,
        t0: Optional[int] = None,
        t1: Optional[int] = None,
        n_threads: Optional[int] = None,
        *,
        cancel: Optional["CancelToken"] = None,
        progress: Optional[Callable[[int, int], object]] = None,
    ) -> np.ndarray:
        """Read float64 samples on the uniform sampling grid, across recordings.

        As :meth:`Reader.read`, and equal to it on the same recordings merged
        with :func:`mef3io.merge_sessions`. Time a recording does not cover
        (or a channel it does not have) is NaN. Each recording's samples are
        scaled by its own units-conversion factor. ``n_threads`` is the total
        for the call; ``cancel`` stops every recording's read; ``progress``
        reports the sum over them.
        """
        fs, t0, n, touched, results = self._gather(channel, t0, t1, n_threads, cancel, progress)
        # Reader.read never warns (its block-copy detail is read_raw's): the
        # warnings collected with each part are dropped here, not emitted.
        ufs = self._ufacts(channel, touched)
        same = len(set(ufs)) <= 1
        samples, valid, owner = self._assemble(channel, fs, t0, n, touched, results,
                                               need_owner=not same)
        if same:
            uf = ufs[0] if ufs else 1.0
            out = samples.astype(np.float64) * uf
        else:
            out = np.empty(n, dtype=np.float64)
            for j, uf in enumerate(ufs):
                m = owner == j
                out[m] = samples[m].astype(np.float64) * uf
        out[valid == 0] = np.nan
        return out

    def _flush_results(self, results) -> None:
        for _, pending in results:
            _flush(pending, stacklevel=3)

    def read_raw(
        self,
        channel: str,
        t0: Optional[int] = None,
        t1: Optional[int] = None,
        n_threads: Optional[int] = None,
        *,
        cancel: Optional["CancelToken"] = None,
        progress: Optional[Callable[[int, int], object]] = None,
    ) -> dict:
        """Read the stored int32 counts with a validity mask, across recordings.

        As :meth:`Reader.read_raw` (same keys and dtypes); the counts are
        placed as :meth:`read` places them. ``block_copy_mismatches`` holds
        every recording's, in time order (each ``segment`` is a path, so it
        says which recording).

        Raises
        ------
        ValueError
            The recordings the window touches store the channel with different
            units-conversion factors, so one factor cannot turn these counts
            into units. Use :meth:`read`, or read each recording's part.
        """
        self._check_open()
        fs, t0_, t1_, n, period, touched = self._window(channel, t0, t1)
        ufs = self._ufacts(channel, touched)
        if len(set(ufs)) > 1:
            raise ValueError(
                f"channel {channel}: the recordings this window touches store different "
                f"units-conversion factors ({', '.join(f'{u:g}' for u in sorted(set(ufs)))}), "
                "so their counts cannot share one; use read(), or read_raw() each "
                "recording's part"
            )
        fs, t0, n, touched, results = self._gather(channel, t0, t1, n_threads, cancel, progress)
        self._flush_results(results)
        if ufs:
            uf = ufs[0]
        else:
            uf = self._infos[channel]["units_conversion_factor"]
            uf = float("nan") if uf is None else uf
        samples, valid, _ = self._assemble(channel, fs, t0, n, touched, results, need_owner=False)
        mismatches = []
        for out, _ in results:
            mismatches.extend(out.get("block_copy_mismatches") or [])
        return {
            "start_uutc": t0,
            "sampling_frequency": fs,
            "units_conversion_factor": uf,
            "samples": samples,
            "valid": valid,
            "block_copy_mismatches": mismatches,
        }

    # ---- maps --------------------------------------------------------------

    def _workers(self) -> int:
        return min(_threads_total(self._n_threads), 16)

    def _segment_map(self, channel: str):
        """``({(k, folder number): number in segments()}, segments list)``, cached."""
        with self._lock:
            cached = self._segment_maps.get(channel)
        if cached is not None:
            return cached
        self._check_open()
        ks = self._holders[channel]
        lists = _run(lambda k: self._readers[k].segments(channel), ks, self._workers())
        rows = []
        for k, segs in zip(ks, lists):
            for s in segs:
                rows.append((s["start_time"], k, s["segment"], s))
        rows.sort(key=lambda r: (r[0], r[1], r[2]))
        mapping, out = {}, []
        seen_total, seen_by = 0, {}
        for num, (_, k, local, s) in enumerate(rows):
            s = dict(s)
            mapping[(k, local)] = num
            s["segment"] = num
            s["start_sample"] = s["start_sample"] + (seen_total - seen_by.get(k, 0))
            seen_total += s["number_of_samples"]
            seen_by[k] = seen_by.get(k, 0) + s["number_of_samples"]
            out.append(s)
        cached = (mapping, out)
        with self._lock:
            self._segment_maps[channel] = cached
        return cached

    def segments(self, channel: str) -> list[dict]:
        """Every recording's segments of a channel, in time order.

        As :meth:`Reader.segments` (same keys). ``segment`` numbers them from
        0 across the recordings, as a merged session does; ``path`` is the
        segment's own folder (so it says which recording); ``start_sample``
        counts the samples stored before it.
        """
        self._check_open()  # also when the map is cached: Reader raises after close()
        if channel not in self._holders:
            return self._readers[0].segments(channel)  # Reader's own error
        return [dict(s) for s in self._segment_map(channel)[1]]

    def block_envelope(self, channel: str, t0: Optional[int] = None,
                       t1: Optional[int] = None) -> dict:
        """Minimum and maximum of every data block, across recordings.

        As :meth:`Reader.block_envelope` (same keys and dtypes): one row per
        block, every recording's in time order.
        """
        self._check_open()
        if channel not in self._holders:
            return self._readers[0].block_envelope(channel, t0, t1)
        ks = self._holders[channel]
        if t0 is not None or t1 is not None:
            info = self._infos[channel]
            lo = info["start_time"] if t0 is None else t0
            hi = info["end_time"] if t1 is None else t1
            margin = 1e6
            keep = [k for k in ks
                    if not (self._readers[k].info(channel)["start_time"] >= hi + margin
                            or self._readers[k].info(channel)["end_time"] + margin <= lo)]
            ks = keep or ks[:1]
        parts = _run(lambda k: self._readers[k].block_envelope(channel, t0, t1), ks,
                     self._workers())
        if len(parts) == 1:
            return parts[0]
        order = np.argsort(np.concatenate([p["start_uutc"] for p in parts]), kind="stable")
        return {key: np.concatenate([p[key] for p in parts])[order] for key in parts[0]}

    def toc(self, channel: str) -> list[dict]:
        """Block-level table of contents, across recordings.

        As :meth:`Reader.toc` (same keys): every block in time order;
        ``start_sample`` counts the samples stored before it.
        """
        self._check_open()
        if channel not in self._holders:
            return self._readers[0].toc(channel)
        ks = self._holders[channel]
        tocs = _run(lambda k: self._readers[k].toc(channel), ks, self._workers())
        if len(tocs) == 1:
            return tocs[0]
        rows = [(b["start_uutc"], j, i, b) for j, t in enumerate(tocs) for i, b in enumerate(t)]
        rows.sort(key=lambda r: (r[0], r[1], r[2]))
        out, seen_total, seen_by = [], 0, {}
        for _, j, _, b in rows:
            b = dict(b)
            b["start_sample"] = b["start_sample"] + (seen_total - seen_by.get(j, 0))
            seen_total += b["number_of_samples"]
            seen_by[j] = seen_by.get(j, 0) + b["number_of_samples"]
            out.append(b)
        return out

    def records(
        self,
        channel: Optional[str] = None,
        segment: Optional[int] = None,
        level: Optional[str] = None,
    ) -> list[dict]:
        """Records (annotations) of every recording, as :meth:`Reader.records`.

        Same arguments and dicts. Session- and channel-level records are
        merged in time order; segment-level ones follow their segment, whose
        ``segment`` is the number in :meth:`segments` (also for the
        ``segment`` argument). The grouping is Reader's: session, then each
        channel's own, then its segments'.
        """
        self._check_open()
        pending: list = []
        try:
            return self._records(channel, segment, level, pending)
        finally:
            _flush(pending, stacklevel=2)

    def _records(self, channel, segment, level, pending: list) -> list:
        if segment is not None and channel is not None:
            if channel not in self._holders:
                return self._readers[0]._records(channel, segment, level, pending)
            mapping, segs = self._segment_map(channel)
            where = {v: key for key, v in mapping.items()}.get(segment)
            if where is None:
                if level not in (None, "segment", "all"):
                    # Reader checks the level first: let a recording raise it.
                    k0 = self._holders[channel][0]
                    any_local = self._readers[k0].segments(channel)[0]["segment"]
                    self._readers[k0]._records(channel, any_local, level, [])
                raise IndexError(f"no such segment: {channel} segment {segment}")
            k, local = where
            got = [(k, rec) for rec in self._readers[k]._records(channel, local, level, pending)]
        elif segment is not None:
            return self._readers[0]._records(channel, segment, level, pending)  # Reader's error
        else:
            got, missing, answered = [], None, 0
            for k, r in enumerate(self._readers):
                try:
                    got.extend((k, rec) for rec in r._records(channel, None, level, pending))
                    answered += 1
                except IndexError as exc:  # this recording has no such channel
                    if channel is None:
                        raise
                    missing = missing or exc
            if not answered:
                raise missing  # no recording knows the channel: Reader's error

        out = []
        for k, rec in got:
            rec = dict(rec)
            lvl, ch = rec.get("level"), rec.get("channel")
            seg_key = -1
            if lvl == "segment" and ch in self._holders and rec.get("segment") is not None:
                num = self._segment_map(ch)[0].get((k, rec["segment"]))
                if num is not None:
                    rec["segment"] = num
                seg_key = rec["segment"]
            rank = 0 if lvl == "session" else 1
            time_key = rec.get("time") if lvl in ("session", "channel") else 0
            out.append(((rank, ch or "", 0 if lvl != "segment" else 1, seg_key,
                         time_key if time_key is not None else 0), rec))
        out.sort(key=lambda x: x[0])  # stable: a segment's own records keep their order
        return [rec for _, rec in out]
