"""Drop-in compatibility layer for ``mef_tools.io``.

Existing code can switch from the legacy package with an import change::

    # from mef_tools.io import MefReader, MefWriter
    from mef3io.compat import MefReader, MefWriter

These classes mirror the signatures, return shapes, and defaults of
``mef_tools.io`` while delegating to the mef3io C++ backend. They are a
transition aid; new code should prefer ``mef3io.Reader`` / ``mef3io.Writer``.
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

    Note: unlike the legacy writer, appends create new segments rather than
    extending an existing segment's data file. Reads are unaffected.
    """

    __version__ = "mef3io"

    def __init__(self, session_path, overwrite=False, password1=None, password2=None, verbose=False):
        self._path = str(session_path)
        self._w = _mef3io.SessionWriter(self._path, overwrite, password1 or "", password2 or "")
        self.verbose = verbose
        self._data_units = "uV"

    @property
    def data_units(self) -> str:
        return self._data_units

    @data_units.setter
    def data_units(self, value: str):
        self._data_units = value
        self._w.set_units(value)

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
        data = np.asarray(data_write)
        if np.issubdtype(data.dtype, np.integer):
            # Treat as raw int32 with a precision-derived ufact if given.
            ufact = 10.0 ** -(precision if precision is not None else 0)
            self._w.write_int32(
                channel, np.ascontiguousarray(data, np.int32), ufact, int(start_uutc),
                float(sampling_freq), None, new_segment,
            )
        else:
            self._w.write_float(
                channel, np.ascontiguousarray(data, np.float64), int(start_uutc),
                float(sampling_freq), precision if precision is not None else -1, new_segment,
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
