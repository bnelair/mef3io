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

import numpy as np

from . import _mef3io


class MefReader:
    """``mef_tools.io.MefReader``-compatible reader."""

    __version__ = "mef3io"

    def __init__(self, session_path: str, password2: Optional[str] = None):
        self._r = _mef3io.Reader(str(session_path), password2 or "")
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
        if isinstance(channels, list):
            data = self.get_raw_data(channels, t_stamp1, t_stamp2)
            for i, ch in enumerate(channels):
                uf = self.get_channel_info(ch)["ufact"][0]
                data[i] = data[i] * uf
            return data
        raw = self.get_raw_data([channels], t_stamp1, t_stamp2)[0]
        return raw * self.get_channel_info(channels)["ufact"][0]

    def get_annotations(self, channel: Optional[str] = None):
        try:
            return self._r.records(channel)
        except Exception:
            print("WARNING: read of annotations record failed, no annotations returned")
            return None

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
    """

    __version__ = "mef3io"

    def __init__(self, session_path, overwrite=False, password1=None, password2=None, verbose=False):
        self._path = str(session_path)
        self._w = _mef3io.SessionWriter(self._path, overwrite, password1 or "", password2 or "")
        self.verbose = verbose
        self._data_units = "uV"
        self._mef_block_len = None
        self._max_nans_written = 0
        self._record_offset = 0

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
        self._mef_block_len = int(value) if value else None
        self._w.set_block_length(int(value) if value else 0)

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
        """Kept for legacy compatibility. mef3io writes records with a zero
        recording-time offset (annotation times round-trip unchanged either
        way); non-zero values are accepted and ignored."""
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
        data = np.atleast_1d(np.asarray(data_write))
        if np.issubdtype(data.dtype, np.integer):
            # Treat as raw int32 with a precision-derived ufact if given.
            from ._writer import _as_int32_counts

            counts = _as_int32_counts(data)
            ufact = 10.0 ** -(precision if precision is not None else 0)
            self._w.write_int32(
                channel, counts, ufact, int(start_uutc),
                float(sampling_freq), None, bool(new_segment),
            )
        else:
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
        self._w.write_records(channel, norm)

    def close(self):
        self._w = None
