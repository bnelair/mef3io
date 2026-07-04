"""P10: session/subject/acquisition metadata objects — write, read back, and
level-2 gating; cross-checked against the pymef oracle."""
import sys
import warnings
from pathlib import Path

import numpy as np
import pytest

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO))
warnings.filterwarnings("ignore")

import mef3io  # noqa: E402
from mef3io import Acquisition, Metadata, Subject  # noqa: E402

START = 1577836800000000
FS = 256.0


def _full_md():
    return Metadata(
        subject=Subject(id="MRN-123", name_1="Jane", name_2="Doe",
                        recording_location="EMU-4", gmt_offset=-6 * 3600),
        acquisition=Acquisition(session_description="pre-surgical eval",
                                channel_description="LFP",
                                reference_description="Cz",
                                acquisition_channel_number=7,
                                low_frequency_filter=0.1,
                                high_frequency_filter=100.0,
                                notch_filter=60.0,
                                line_frequency=60.0),
    )


def _write(path, md=None, p1="", p2=""):
    with mef3io.Writer(path, overwrite=True, password1=p1, password2=p2, metadata=md) as w:
        w.write("ch1", np.sin(np.arange(2000) / 20), START, FS, precision=3)


def test_metadata_round_trip(tmp_path):
    path = str(tmp_path / "s.mefd")
    _write(path, _full_md())
    md = mef3io.Reader(path).metadata

    assert md.subject.id == "MRN-123"
    assert md.subject.name_1 == "Jane" and md.subject.name_2 == "Doe"
    assert md.subject.recording_location == "EMU-4"
    assert md.subject.gmt_offset == -6 * 3600
    assert md.acquisition.session_description == "pre-surgical eval"
    assert md.acquisition.channel_description == "LFP"
    assert md.acquisition.reference_description == "Cz"
    assert md.acquisition.acquisition_channel_number == 7
    assert md.acquisition.high_frequency_filter == 100.0
    assert md.acquisition.line_frequency == 60.0


def test_partial_population_defaults(tmp_path):
    # Fill in only a couple of fields; the rest keep their defaults.
    path = str(tmp_path / "s.mefd")
    _write(path, Metadata(subject=Subject(id="X1", name_1="Solo")))
    md = mef3io.Reader(path).metadata
    assert md.subject.id == "X1" and md.subject.name_1 == "Solo"
    assert md.subject.name_2 == "" and md.subject.recording_location == ""
    assert md.acquisition.line_frequency == -1.0  # unset sentinel


def test_no_metadata_is_fine(tmp_path):
    path = str(tmp_path / "s.mefd")
    _write(path)  # no metadata arg
    md = mef3io.Reader(path).metadata
    assert md.subject.id == "" and isinstance(md, Metadata)
    # channel_description defaults to the channel name
    assert md.acquisition.channel_description == "ch1"


def test_level2_gates_subject_metadata(tmp_path):
    path = str(tmp_path / "s.mefd")
    _write(path, _full_md(), p1="p1", p2="p2")

    # L2: subject visible
    md2 = mef3io.Reader(path, "p2").metadata
    assert md2.subject.id == "MRN-123"
    # L1: subject locked, but section-2 acquisition still readable
    md1 = mef3io.Reader(path, "p1").metadata
    assert md1.subject.id == "" and md1.subject.name_1 == ""
    assert md1.acquisition.session_description == "pre-surgical eval"


def test_dict_metadata_accepted(tmp_path):
    # set_metadata also accepts a flat dict (no dataclass required).
    path = str(tmp_path / "s.mefd")
    with mef3io.Writer(path, overwrite=True) as w:
        w.set_metadata({"subject_id": "D1", "session_description": "via dict"})
        w.write("ch1", np.zeros(500), START, FS, precision=0)
    md = mef3io.Reader(path).metadata
    assert md.subject.id == "D1"
    assert md.acquisition.session_description == "via dict"


def test_to_dict_and_from_info(tmp_path):
    path = str(tmp_path / "s.mefd")
    _write(path, _full_md())
    r = mef3io.Reader(path)
    d = r.metadata.to_dict()
    assert d["subject"]["id"] == "MRN-123"
    assert d["acquisition"]["line_frequency"] == 60.0
    # from_info mirrors info()
    assert Metadata.from_info(r.info("ch1")).subject.id == "MRN-123"


def test_compat_writer_metadata_object(tmp_path):
    from mef3io import MefReader, MefWriter

    path = str(tmp_path / "s.mefd")
    w = MefWriter(path, overwrite=True, metadata=Metadata(subject=Subject(id="C1")))
    w.write_data(np.zeros(500), "ch1", START, FS, precision=0)
    w.close()
    assert mef3io.Reader(path).metadata.subject.id == "C1"
    # readable through the legacy reader's basic info too
    assert MefReader(path).channels == ["ch1"]


def test_compat_writer_legacy_dicts(tmp_path):
    # mef_tools-style: mutate section dicts (bytes values), incl. subject_ID.
    from mef3io import MefWriter

    path = str(tmp_path / "s.mefd")
    w = MefWriter(path, overwrite=True)
    w.section3_dict["subject_ID"] = b"MRN-legacy"
    w.section3_dict["subject_name_1"] = "Bob"  # str also fine
    w.section2_ts_dict["session_description"] = b"legacy sess"
    w.section2_ts_dict["AC_line_frequency"] = 50.0
    w.write_data(np.zeros(500), "ch1", START, FS, precision=0)
    w.close()

    md = mef3io.Reader(path).metadata
    assert md.subject.id == "MRN-legacy"
    assert md.subject.name_1 == "Bob"
    assert md.acquisition.session_description == "legacy sess"
    assert md.acquisition.line_frequency == 50.0


def test_pymef_reads_metadata(tmp_path):
    pytest.importorskip("pymef")
    from pymef.mef_session import MefSession

    path = str(tmp_path / "s.mefd")
    _write(path, _full_md())
    ms = MefSession(path, None, True)
    seg = ms.session_md["time_series_channels"]["ch1"]["segments"]["ch1-000000"]
    assert seg["section_3"]["subject_ID"][0] == b"MRN-123"
    assert seg["section_3"]["subject_name_1"][0] == b"Jane"
    assert seg["section_2"]["session_description"][0] == b"pre-surgical eval"
    assert seg["section_2"]["AC_line_frequency"][0] == 60.0
