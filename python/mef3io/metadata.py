"""Session metadata objects.

Modern, user-friendly containers for MEF 3.0 session/subject/acquisition
metadata, grouped by the on-disk section they map to. Every field has a
sensible default, so you fill in only what you have::

    from mef3io import Metadata, Subject, Acquisition

    md = Metadata(subject=Subject(id="MRN-123", name_1="Jane"))
    with mef3io.Writer("s.mefd", overwrite=True, metadata=md) as w:
        ...

    md = mef3io.Reader("s.mefd", "l2pw").metadata
    md.subject.id                 # "MRN-123"

Subject fields live in the level-2-encrypted metadata section; the acquisition
descriptions live in the level-1 section. On read, subject fields are empty
unless the password grants level-2 access.
"""
from __future__ import annotations

from dataclasses import asdict, dataclass, field

# "No entry" sentinel for filter settings not recorded (matches the format).
_UNSET_HZ = -1.0


@dataclass
class Subject:
    """Subject / recording-context metadata (MEF section 3, level-2 encrypted)."""

    name_1: str = ""
    name_2: str = ""
    id: str = ""
    recording_location: str = ""
    gmt_offset: int = 0  # seconds east of UTC

    def to_dict(self) -> dict:
        return asdict(self)


@dataclass
class Acquisition:
    """Descriptive / acquisition metadata (MEF section 2, level-1 encrypted).

    Filter settings default to ``-1.0`` meaning "not recorded".
    """

    session_description: str = ""
    channel_description: str = ""
    reference_description: str = ""
    acquisition_channel_number: int = 1
    low_frequency_filter: float = _UNSET_HZ
    high_frequency_filter: float = _UNSET_HZ
    notch_filter: float = _UNSET_HZ
    line_frequency: float = _UNSET_HZ

    def to_dict(self) -> dict:
        return asdict(self)


@dataclass
class Metadata:
    """Session-wide metadata: a :class:`Subject` and an :class:`Acquisition`
    block. Written to every channel; on read, reflects the first channel."""

    subject: Subject = field(default_factory=Subject)
    acquisition: Acquisition = field(default_factory=Acquisition)

    def to_dict(self) -> dict:
        """Nested plain-dict view: ``{"subject": {...}, "acquisition": {...}}``."""
        return {"subject": self.subject.to_dict(), "acquisition": self.acquisition.to_dict()}

    def _flat(self) -> dict:
        """Flat dict of every settable field, for the C++ set_metadata call."""
        return {**self.acquisition.to_dict(),
                "subject_name_1": self.subject.name_1,
                "subject_name_2": self.subject.name_2,
                "subject_id": self.subject.id,
                "recording_location": self.subject.recording_location,
                "gmt_offset": self.subject.gmt_offset}

    @classmethod
    def from_info(cls, info: dict) -> "Metadata":
        """Build a Metadata from a :meth:`Reader.info` dict. Subject fields are
        empty when the reader lacks level-2 access (``section3_available``)."""
        def g(key, default):
            v = info.get(key, default)
            return default if v is None else v

        subj = Subject()
        if info.get("section3_available"):
            subj = Subject(
                name_1=g("subject_name_1", ""),
                name_2=g("subject_name_2", ""),
                id=g("subject_id", ""),
                recording_location=g("recording_location", ""),
                gmt_offset=int(g("gmt_offset", 0)),
            )
        acq = Acquisition(
            session_description=g("session_description", ""),
            channel_description=g("channel_description", ""),
            reference_description=g("reference_description", ""),
            acquisition_channel_number=int(g("acquisition_channel_number", 1)),
            low_frequency_filter=float(g("low_frequency_filter", _UNSET_HZ)),
            high_frequency_filter=float(g("high_frequency_filter", _UNSET_HZ)),
            notch_filter=float(g("notch_filter", _UNSET_HZ)),
            line_frequency=float(g("line_frequency", _UNSET_HZ)),
        )
        return cls(subject=subj, acquisition=acq)


__all__ = ["Metadata", "Subject", "Acquisition"]
