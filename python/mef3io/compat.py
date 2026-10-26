"""Drop-in compatibility layer for ``mef_tools.io``.

Existing code can switch from the legacy package with an import change::

    # from mef_tools.io import MefReader, MefWriter
    from mef3io import MefReader, MefWriter        # or from mef3io.compat import ...

These classes mirror the signatures, return shapes, and defaults of
``mef_tools.io`` while delegating to the mef3io C++ backend: float data is
split on NaN runs into discontinuity gaps, precision (and with it the units
conversion factor) is inferred when not given, integer data takes the int32
primitive path, and appends extend the channel's last segment in place. New
code should prefer ``mef3io.Reader`` / ``mef3io.Writer``.
"""
from __future__ import annotations

from typing import Optional, Union

from ._writer import (
    reporting_unreadable_metadata,
    warn_password_interop,
    warn_unreadable_metadata,
)
from ._reader import (
    SessionDeclarationWarning,
    _declaration_warning_text,
    _flush,
    _note_unscaled,
    _warn_legacy_password,
    _Announced,
    _warning_on_legacy_records,
)
from ._warnings import emit as _emit, identity_of as _identity_of

import numpy as np

from . import _mef3io


class MefReader:
    """``mef_tools.io.MefReader``-compatible reader.

    ``session_path`` may also be an uncompressed tar archive of a session
    (``name.mefd.tar``, see :func:`mef3io.archive_session`).
    """

    __version__ = "mef3io"

    def __init__(
        self,
        session_path: str,
        password2: Optional[str] = None,
        warn_declarations: bool = True,
    ):
        self._r = _mef3io.Reader(str(session_path), password2 or "")
        self._announced = _Announced()
        self._path_str = str(session_path)
        # The legacy drop-in is exactly the entry point people with legacy-
        # written sessions use, so it must carry the same open-time warning as
        # mef3io.Reader — otherwise the population most affected is the one
        # population never told.
        #
        # It also needs the same opt-out. `mef_tools.io.MefReader` never
        # warned, so a project that swaps in the drop-in and runs its suite
        # with -W error turns working code into a hard failure — on a session
        # whose data this very warning describes as intact.
        issues = list(self._r.declaration_issues()) if warn_declarations else []
        if issues:
            _emit(SessionDeclarationWarning, _declaration_warning_text(issues),
                  path=self._path_str, details=issues, stacklevel=2,
                  also=_identity_of(self._r))
        # Only sessions an older mef3io wrote can trigger it (mef_tools always
        # used meflib's mapping), so it costs a legacy user nothing.
        pending: list = []
        _warn_legacy_password(self._r, str(session_path), pending)
        _flush(pending, stacklevel=2)
        self.bi = [self._basic_info(ch) for ch in self._r.channels]

    def _basic_info(self, channel: str) -> dict:
        info = self._r.info(channel)
        # mef_tools returns list-valued fields; mirror the ones people use.
        return {
            "name": channel,
            "fsamp": [info["sampling_frequency"]],
            "nsamp": [info["number_of_samples"]],
            "ufact": [info["units_conversion_factor"]],
            "unit": [info["units_description"].encode()],
            "start_time": [info["start_time"]],
            "end_time": [info["end_time"]],
            "channel": channel,
        }

    @property
    def channels(self) -> list[str]:
        return list(self._r.channels)

    @property
    def properties(self) -> list[str]:
        props: set[str] = set()
        for ch in self.bi:
            props.update(ch.keys())
        return sorted(props)

    def get_property(self, property_name: str, channel: Optional[str] = None):
        def one(ch_info):
            v = ch_info[property_name]
            return v[0] if isinstance(v, list) and len(v) == 1 else v

        if channel is None:
            return [one(ch) for ch in self.bi]
        for ch in self.bi:
            if ch["name"] == channel:
                return one(ch)
        return None

    def get_channel_info(self, channel: Optional[str] = None):
        if channel is None:
            return self.bi
        for ch in self.bi:
            if ch["name"] == channel:
                return ch
        return None

    def get_raw_data(self, channels, t_stamp1=None, t_stamp2=None):
        picked = self._pick(channels)
        out = []
        for ch in picked:
            raw = self._r.read_raw(ch, t_stamp1, t_stamp2)
            samples = np.asarray(raw["samples"]).astype(np.float64)
            valid = np.asarray(raw["valid"]).astype(bool)
            samples[~valid] = np.nan  # legacy returns NaN in gaps
            out.append(samples)
        return out

    def get_data(self, channels, t_stamp1=None, t_stamp2=None):
        # Reader.read scales each sample by its OWN segment's factor (#150);
        # counts * ufact[0] was wrong for a channel whose segments differ
        # (ufact is then [None]).
        picked = self._pick(channels if isinstance(channels, list) else [channels])
        if not isinstance(channels, list):
            picked = picked[:1] or [channels]  # an unknown name: the backend's error
        out = [self._r.read(ch, t_stamp1, t_stamp2) for ch in picked]
        pending: list = []
        for ch in picked:  # a segment with no factor set read as raw counts
            _note_unscaled(self._r, self._announced, self._path_str, ch, pending)
        _flush(pending, stacklevel=2)
        return out if isinstance(channels, list) else out[0]

    def block_envelope(self, channel: str, t_stamp1=None, t_stamp2=None) -> dict:
        """Per-block minimum and maximum from the block index alone.

        Not part of the legacy ``mef_tools`` API: the same as
        :meth:`mef3io.Reader.block_envelope`, here so code that holds a
        ``MefReader`` (such as a server) needs no second reader. Returns NumPy
        arrays ``start_uutc``, ``end_uutc``, ``min``, ``max``, one row per
        data block overlapping ``[t_stamp1, t_stamp2)``, scaled like
        :meth:`get_data`.
        """
        out = self._r.block_envelope(channel, t_stamp1, t_stamp2)
        pending: list = []
        _note_unscaled(self._r, self._announced, self._path_str, channel, pending)
        _flush(pending, stacklevel=2)
        return out

    def get_annotations(self, channel: Optional[str] = None):
        """Records as a list of dicts (legacy ``mef_tools`` signature).

        ``channel=None`` returns session-level records; a channel name returns
        that channel's CHANNEL-level records only, as the legacy reader did.
        Segment-level records are not included here (the native
        :meth:`mef3io.Reader.records` does include them for a channel, or use
        ``Reader.records(channel, level="segment")``). Every dict carries
        ``level`` / ``channel`` / ``segment``; see :meth:`mef3io.Reader.records`
        for the fields. Returns ``None`` (with a printed warning) on failure,
        as the legacy API did.
        """
        # The warning sits OUTSIDE the try: under -W error it must propagate,
        # not be mistaken for a failed read and swallowed.
        pending: list = []
        try:
            with _warning_on_legacy_records(self._announced, self._r, self._path_str, pending):
                try:
                    if channel is None:
                        return self._r.records()
                    return self._r.records(channel, level="channel")
                except Exception:
                    print("WARNING: read of annotations record failed, no annotations returned")
                    return None
        finally:
            _flush(pending, stacklevel=2)

    def close(self):
        self._r = None

    def _pick(self, channels) -> list[str]:
        chs = self.channels
        if isinstance(channels, (int, np.integer)):
            return [chs[int(channels)]]
        if isinstance(channels, str):
            if channels not in chs:
                raise ValueError("Channel name is not present in MEF file.")
            return [channels]
        picked = []
        for c in channels:
            if isinstance(c, (int, np.integer)):
                picked.append(chs[int(c)])
            elif isinstance(c, str) and c in chs and c not in picked:
                picked.append(c)
        return picked


class MefWriter:
    """``mef_tools.io.MefWriter``-compatible writer.

    Like the legacy writer, appends extend the channel's last segment in place
    (in-segment append); pass ``new_segment=True`` to start a fresh segment.

    Metadata works two ways: mutate the legacy ``section3_dict`` /
    ``section2_ts_dict`` (e.g. ``w.section3_dict['subject_ID'] = 'Smith'``) as
    with mef_tools, or use the modern object via ``set_metadata`` / the
    ``metadata=`` argument (``mef3io.Metadata``).
    """

    __version__ = "mef3io"

    def __init__(self, session_path, overwrite=False, password1=None, password2=None,
                 verbose=False, metadata=None, durability="fast"):
        # Checked before constructing: SessionWriter removes an existing session
        # when overwrite=True, so a late check could delete a good one first.
        if durability not in ("full", "fast"):
            raise ValueError(f"durability must be 'full' or 'fast', not {durability!r}")
        self._path = str(session_path)
        self._w = _mef3io.SessionWriter(self._path, overwrite, password1 or "", password2 or "")
        warn_unreadable_metadata(self._w.unreadable_metadata, path=self._path)
        warn_password_interop(password1, password2, path=self._path)
        # "fast" by default, matching mef3io.Writer and the legacy stack this
        # class stands in for — meflib flushes nothing at all. See
        # mef3io.Writer for what an unclean shutdown costs and how
        # mef3io.recover_session puts it back.
        self._w.set_durable(durability == "full")
        self.verbose = verbose
        self._data_units = "uV"
        self._mef_block_len = None
        self._max_nans_written = 0
        self._record_offset = 0
        # Legacy mef_tools-style mutable metadata dicts: set keys on these
        # before writing and they are applied (e.g. section3_dict['subject_ID']
        # = 'Smith'). The modern mef3io.Metadata is also accepted via
        # set_metadata() or the metadata= argument.
        self.section2_ts_dict = {}
        self.section3_dict = {}
        self._metadata_flat = {}
        if metadata is not None:
            self.set_metadata(metadata)

    # Map legacy mef_tools section-dict keys onto mef3io metadata fields.
    _LEGACY_KEYS = {
        "subject_name_1": "subject_name_1", "subject_name_2": "subject_name_2",
        "subject_ID": "subject_id", "subject_id": "subject_id",
        "recording_location": "recording_location", "GMT_offset": "gmt_offset",
        "gmt_offset": "gmt_offset", "session_description": "session_description",
        "channel_description": "channel_description",
        "reference_description": "reference_description",
        "acquisition_channel_number": "acquisition_channel_number",
        "low_frequency_filter_setting": "low_frequency_filter",
        "high_frequency_filter_setting": "high_frequency_filter",
        "notch_filter_frequency_setting": "notch_filter",
        "AC_line_frequency": "line_frequency",
    }

    def set_metadata(self, metadata) -> None:
        """Set session-wide subject/acquisition metadata (a
        :class:`mef3io.Metadata` or a flat dict). Applied on the next write."""
        self._metadata_flat = metadata._flat() if hasattr(metadata, "_flat") else dict(metadata)

    def _apply_metadata(self) -> None:
        """Merge the modern metadata with the legacy section dicts (decoding
        any bytes) and push it to the backend before a write."""
        flat = dict(self._metadata_flat)
        for src in (self.section2_ts_dict, self.section3_dict):
            for k, v in src.items():
                key = self._LEGACY_KEYS.get(k)
                if key is None:
                    continue
                flat[key] = v.decode() if isinstance(v, (bytes, bytearray)) else v
        if flat:
            self._w.set_metadata(flat)

    @property
    def data_units(self) -> str:
        return self._data_units

    @data_units.setter
    def data_units(self, value: str):
        self._data_units = value
        self._w.set_units(value)

    @property
    def mef_block_len(self):
        """RED block length in samples (None = derive from fs, like legacy)."""
        return self._mef_block_len

    @mef_block_len.setter
    def mef_block_len(self, value):
        # Falsy (None/0) means "derive from fs": normalize so get_mefblock_len
        # applies the legacy formula instead of returning a literal 0.
        # set_block_length first: a refused value (#83) must not be kept.
        self._w.set_block_length(int(value) if value else 0)
        self._mef_block_len = int(value) if value else None

    def get_mefblock_len(self, fs: float) -> int:
        """Block length that will be used for data at ``fs`` (legacy formula)."""
        if self._mef_block_len is not None:
            return int(self._mef_block_len)
        return int(fs) if fs >= 5000 else int(fs * 10)

    @property
    def max_nans_written(self) -> int:
        """Kept for legacy compatibility. mef3io always splits data on NaN runs
        (never stores NaN as values), i.e. it behaves like the legacy writer's
        recommended setting of 0; other values are accepted and ignored."""
        return self._max_nans_written

    @max_nans_written.setter
    def max_nans_written(self, value):
        self._max_nans_written = value

    @property
    def record_offset(self) -> int:
        """Kept for legacy compatibility; non-zero values are accepted and
        ignored -- mef3io never creates a recording-time offset.

        Times are UTC, always. A new session is written at offset 0 with
        times stored as -t, as before. A session that already carries a
        non-zero offset (e.g. one the legacy stack wrote with
        ``record_offset``) keeps it: segments and channels mef3io adds reuse
        it, and every time mef3io writes there -- data and annotations -- is
        stored as rto - t, so the real date stays hidden and the existing
        annotations stay readable. A time at or before the offset cannot be
        stored that way and is refused, as is any write while the session's
        segments disagree on the offset or it is locked (level-1 password
        only)."""
        return self._record_offset

    @record_offset.setter
    def record_offset(self, value):
        self._record_offset = value

    def write_data(
        self,
        data_write: np.ndarray,
        channel: str,
        start_uutc: int,
        sampling_freq: float,
        end_uutc: Optional[int] = None,
        precision: Optional[int] = None,
        new_segment: bool = False,
        discont_handler: bool = True,
        reload_metadata: bool = True,
    ):
        self._apply_metadata()
        data = np.atleast_1d(np.asarray(data_write))
        if np.issubdtype(data.dtype, np.integer):
            # Treat as raw int32 with a precision-derived ufact if given.
            from ._writer import _as_int32_counts

            counts = _as_int32_counts(data)
            ufact = 10.0 ** -(precision if precision is not None else 0)
            with reporting_unreadable_metadata(self._w, self._path):
                self._w.write_int32(
                    channel, counts, ufact, int(start_uutc),
                    float(sampling_freq), None, bool(new_segment),
                )
        else:
            with reporting_unreadable_metadata(self._w, self._path):
                self._w.write_float(
                    channel, np.ascontiguousarray(data, np.float64), int(start_uutc),
                    float(sampling_freq), int(precision) if precision is not None else -1,
                    bool(new_segment),
                )
        return True

    def write_annotations(self, annotations, channel: Optional[str] = None):
        # Accept a pandas DataFrame (time,type,text[,duration]) or list of dicts.
        if hasattr(annotations, "to_dict"):
            records = annotations.to_dict("records")
        else:
            records = list(annotations)
        norm = []
        for r in records:
            rec = {"type": r.get("type", "Note"), "time": int(r["time"]), "text": r.get("text", "")}
            if "duration" in r and r["duration"] is not None:
                rec["duration"] = int(r["duration"])
            norm.append(rec)
        with reporting_unreadable_metadata(self._w, self._path):
            self._w.write_records(channel, norm)

    def close(self):
        self._w = None
