"""Units strings and mixed conversion factors across merged sessions.

* merge_sessions never refuses on units (free text): a channel whose kept
  segments differ in units gets a note (check and run) and one
  MixedUnitsWarning; uV / micro sign / Greek mu are one unit; values and
  per-segment strings never change; the label is the first kept segment's;
* the validator reports ``units.mixed`` (warning) for a channel whose
  segments disagree;
* a channel with different conversion factors per segment reads correctly in
  mef3io (Reader and the mef_tools-compatible MefReader) and pymef, reads as
  zeros in the legacy mef_tools, and merge/transform warn about it.
"""
from __future__ import annotations

import warnings

import numpy as np
import pytest

import mef3io
from test_p29_anonymize import _reseal

FS = 256.0
T0 = 1_600_000_000_000_000
HOUR = 3_600_000_000


def _src(path, units, t0, *, segs=((0.1, "uV"),), ch="X"):
    """One channel; `segs` = (factor, units) per segment (int32 counts)."""
    with mef3io.Writer(str(path), overwrite=True, units=units) as w:
        for i, (fac, u) in enumerate(segs):
            c = (np.arange(512, dtype=np.int32) % 50) - 25
            w.write_int32(ch, c, fac, t0 + i * 2_000_000, FS, new_segment=i > 0)
    return path


def _set_units(path, ch, seg, text):
    """Rewrite one segment's units string in place (CRC re-sealed)."""
    f = next((path / f"{ch}.timd").glob(f"*{seg:06d}.segd/*.tmet"))
    raw = bytearray(f.read_bytes())
    raw[2560 + 6208:2560 + 6208 + 128] = text.encode().ljust(128, b"\0")
    _reseal(raw)
    f.write_bytes(bytes(raw))


@pytest.fixture
def two(tmp_path):
    a = _src(tmp_path / "a.mefd", "uV", T0)
    b = _src(tmp_path / "b.mefd", "mV", T0 + HOUR)
    return a, b


def _units_notes(rep):
    return [n for n in rep.notes if "different units" in n]


def test_warns_and_notes_never_refuses(two, tmp_path):
    a, b = two
    assert "merge-units-mismatch" not in mef3io.merge.REFUSAL_CODES
    rep = mef3io.merge.check([a, b], tmp_path / "m.mefd")
    assert rep.ok and not rep.refusals
    notes = _units_notes(rep)
    assert len(notes) == 1 and "X" in notes[0] and '"uV"' in notes[0] and '"mV"' in notes[0]
    with pytest.warns(mef3io.MixedUnitsWarning) as rec:
        out = mef3io.merge_sessions([a, b], tmp_path / "m.mefd")
    ws = [x.message for x in rec if x.category is mef3io.MixedUnitsWarning]
    assert len(ws) == 1 and ws[0].details == ("X",)
    assert '"uV"' in str(ws[0]) and '"mV"' in str(ws[0]) and str(tmp_path) not in str(ws[0])
    assert out.units_mixed == {"X": ("uV", "mV")}
    assert len(_units_notes(out)) == 1
    assert out.verification.ok  # verify accepts mixed units
    assert _label(tmp_path / "m.mefd") == "uV"  # first kept segment's
    with mef3io.Reader(str(tmp_path / "m.mefd")) as r:
        assert np.count_nonzero(~np.isnan(r.read("X"))) == 1024
    # per-segment strings are untouched (the validator sees both)
    rep = mef3io.validate_session(str(tmp_path / "m.mefd"))
    assert [f.check_id for f in rep.findings if f.check_id == "units.mixed"] == ["units.mixed"]


@pytest.mark.parametrize("u1,u2", [("uV", "\u00b5V"), ("\u00b5V", "\u03bcV"), ("uV", "\u03bcV")])
def test_micro_spellings_are_one_unit(tmp_path, u1, u2):
    a = _src(tmp_path / "a.mefd", u1, T0)
    b = _src(tmp_path / "b.mefd", u2, T0 + HOUR)
    rep = mef3io.merge.check([a, b], tmp_path / "m.mefd")
    assert rep.ok and not _units_notes(rep)
    with warnings.catch_warnings():
        warnings.simplefilter("error", mef3io.MixedUnitsWarning)
        out = mef3io.merge_sessions([a, b], tmp_path / "m.mefd")
    assert not out.units_mixed and out.verification.ok
    rep = mef3io.validate_session(str(tmp_path / "m.mefd"))
    assert not [f for f in rep.findings if f.check_id == "units.mixed"]


def test_other_prefixes_still_differ(tmp_path):
    a = _src(tmp_path / "a.mefd", "uV", T0)
    b = _src(tmp_path / "b.mefd", "mV", T0 + HOUR)
    assert _units_notes(mef3io.merge.check([a, b], tmp_path / "m.mefd"))


def test_same_units_merge_and_values_unchanged(tmp_path):
    a = _src(tmp_path / "a.mefd", "mV", T0)
    b = _src(tmp_path / "b.mefd", "mV", T0 + HOUR)
    mef3io.merge_sessions([a, b], tmp_path / "m.mefd")
    with mef3io.Reader(str(tmp_path / "m.mefd")) as r:
        assert r.info("X")["units_description"] == "mV"
        assert np.count_nonzero(~np.isnan(r.read("X"))) == 1024


def test_validator_units_mixed(tmp_path):
    p = _src(tmp_path / "s.mefd", "uV", T0, segs=((0.1, "uV"), (0.1, "uV")))
    assert "units.mixed" in [getattr(c, "id", c) for c in mef3io.available_checks()]
    assert not mef3io.Validator.describe_check("units.mixed").repairable
    def mixed(rep):
        return [(f.check_id, f.severity, f.channel, f.segment)
                for f in rep.findings if f.check_id == "units.mixed"]
    assert mixed(mef3io.validate_session(str(p))) == []
    _set_units(p, "X", 1, "mV")
    rep = mef3io.validate_session(str(p))
    assert mixed(rep) == [("units.mixed", "warning", "X", 1)]
    assert rep.ok  # a warning, not an error
    _set_units(p, "X", 1, "\u00b5V")  # micro sign vs "uV": one unit
    assert mixed(mef3io.validate_session(str(p))) == []


def test_already_mixed_source_is_kept_and_noted(tmp_path):
    a = _src(tmp_path / "a.mefd", "uV", T0, segs=((0.1, "uV"), (0.1, "uV")))
    _set_units(a, "X", 1, "mV")
    b = _src(tmp_path / "b.mefd", "uV", T0 + HOUR)
    rep = mef3io.merge.check([a, b], tmp_path / "m.mefd")
    assert rep.ok and not rep.refusals
    assert any("different units" in n for n in rep.notes)


def _label(path):
    with mef3io.Reader(str(path)) as r:
        return r.info("X")["units_description"]


def _mixed_a(tmp_path):
    a = _src(tmp_path / "a.mefd", "uV", T0, segs=((0.1, "uV"), (0.1, "uV")))
    _set_units(a, "X", 1, "mV")  # segment 0 "uV", segment 1 "mV"
    return a


def test_keeping_only_part_of_mixed_a(tmp_path):
    a = _mixed_a(tmp_path)
    b = _src(tmp_path / "b.mefd", "mV", T0 + HOUR)
    # only A's second ("mV") segment + B ("mV"): agrees, label mV
    rngs = [(T0 + 2_000_000, T0 + 3_000_000), None]
    mef3io.merge_sessions([a, b], tmp_path / "m.mefd", time_ranges=rngs)
    assert _label(tmp_path / "m.mefd") == "mV"
    # A's first ("uV") segment + B ("mV"): mixed, warns, label is the first's
    rngs = [(T0, T0 + 1_000_000), None]
    rep = mef3io.merge.check([a, b], tmp_path / "n.mefd", time_ranges=rngs)
    assert rep.ok and len(_units_notes(rep)) == 1
    with pytest.warns(mef3io.MixedUnitsWarning):
        mef3io.merge_sessions([a, b], tmp_path / "n.mefd", time_ranges=rngs)
    assert _label(tmp_path / "n.mefd") == "uV"
    # all of mixed A + B "mV": noted, no refusal
    rep = mef3io.merge.check([a, b], tmp_path / "o.mefd")
    assert rep.ok and len(_units_notes(rep)) == 1


def test_mixed_source_note_names_label(tmp_path):
    a = _mixed_a(tmp_path)
    b = _src(tmp_path / "b.mefd", "uV", T0 + HOUR)
    rep = mef3io.merge.check([a, b], tmp_path / "m.mefd")
    assert any("first kept segment" in n for n in rep.notes)
    with pytest.warns(mef3io.MixedUnitsWarning):
        mef3io.merge_sessions([a, b], tmp_path / "m.mefd")
    assert _label(tmp_path / "m.mefd") == "uV"


# ---- mixed conversion factors ------------------------------------------------

def _mixed_factor_session(path):
    return _src(path, "uV", T0, segs=((0.1, "uV"), (0.25, "uV")))


def _expected():
    c = (np.arange(512, dtype=np.int32) % 50) - 25
    return np.concatenate([c * 0.1, c * 0.25])


def test_mef3io_readers_correct_on_mixed_factors(tmp_path):
    p = _mixed_factor_session(tmp_path / "mf.mefd")
    with mef3io.Reader(str(p)) as r:
        np.testing.assert_allclose(r.read("X"), _expected(), atol=1e-9)
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        got = np.asarray(mef3io.MefReader(str(p)).get_data("X")).ravel()
    np.testing.assert_allclose(got, _expected(), atol=1e-9)
    assert np.any(got != 0)


def test_legacy_mef_tools_reads_zeros_pymef_reads_correctly(tmp_path):
    pytest.importorskip("mef_tools")
    pymef = pytest.importorskip("pymef")
    from mef_tools.io import MefReader as LegacyReader
    p = _mixed_factor_session(tmp_path / "mf.mefd")
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        legacy = np.asarray(LegacyReader(str(p)).get_data("X")).ravel()
    assert legacy.size == 1024 and not np.any(legacy)  # documented legacy behaviour
    from pymef.mef_session import MefSession
    s = MefSession(str(p), "")
    got = np.asarray(s.read_ts_channels_sample(["X"], [0, 1024])[0]).ravel()
    counts = (np.arange(512, dtype=np.int32) % 50) - 25
    np.testing.assert_array_equal(got, np.concatenate([counts, counts]))  # pymef: raw counts, intact


def test_merge_and_transform_warn_about_mixed_factors(tmp_path):
    a = _src(tmp_path / "a.mefd", "uV", T0)
    b = _src(tmp_path / "b.mefd", "uV", T0 + HOUR, segs=((0.25, "uV"),))
    with pytest.warns(mef3io.MixedFactorsWarning) as rec:
        mef3io.merge_sessions([a, b], tmp_path / "m.mefd")
    w = [x for x in rec if x.category is mef3io.MixedFactorsWarning][0].message
    assert w.details == ("X",) and "zeros" in str(w) and "pymef" in str(w)
    assert str(tmp_path) not in str(w)
    with pytest.warns(mef3io.MixedFactorsWarning):
        mef3io.transform_session(tmp_path / "m.mefd", tmp_path / "t.mefd")
    with warnings.catch_warnings():
        warnings.simplefilter("error", mef3io.MixedFactorsWarning)
        mef3io.transform_session(a, tmp_path / "t2.mefd")  # one factor: silent
