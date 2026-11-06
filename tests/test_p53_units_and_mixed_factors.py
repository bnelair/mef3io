"""Units strings and mixed conversion factors across merged sessions.

* merge_sessions refuses (``merge-units-mismatch``) a channel whose units
  differ between sources, in check and run; compared exactly as stored; a
  source that is ALREADY mixed is kept and noted;
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
from mef3io.merge import REFUSAL_CODES

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


def test_refused_in_check_and_run(two, tmp_path):
    a, b = two
    assert "merge-units-mismatch" in REFUSAL_CODES
    rep = mef3io.merge.check([a, b], tmp_path / "m.mefd")
    hits = [r for r in rep.refusals if r.code == "merge-units-mismatch"]
    assert len(hits) == 1
    assert hits[0].channel == "X"
    assert '"uV"' in hits[0].message and '"mV"' in hits[0].message
    with pytest.raises(mef3io.WriteConflictError) as e:
        mef3io.merge_sessions([a, b], tmp_path / "m.mefd")
    assert e.value.code == "merge-units-mismatch"
    assert not (tmp_path / "m.mefd").exists()


def test_same_units_merge_and_values_unchanged(tmp_path):
    a = _src(tmp_path / "a.mefd", "mV", T0)
    b = _src(tmp_path / "b.mefd", "mV", T0 + HOUR)
    mef3io.merge_sessions([a, b], tmp_path / "m.mefd")
    with mef3io.Reader(str(tmp_path / "m.mefd")) as r:
        assert r.info("X")["units_description"] == "mV"
        assert np.count_nonzero(~np.isnan(r.read("X"))) == 1024


def test_exact_comparison_not_folded(tmp_path):
    a = _src(tmp_path / "a.mefd", "uV", T0)
    b = _src(tmp_path / "b.mefd", "µV", T0 + HOUR)
    rep = mef3io.merge.check([a, b], tmp_path / "m.mefd")
    assert [r.code for r in rep.refusals] == ["merge-units-mismatch"]


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


def test_already_mixed_source_is_kept_and_noted(tmp_path):
    a = _src(tmp_path / "a.mefd", "uV", T0, segs=((0.1, "uV"), (0.1, "uV")))
    _set_units(a, "X", 1, "mV")
    b = _src(tmp_path / "b.mefd", "uV", T0 + HOUR)
    rep = mef3io.merge.check([a, b], tmp_path / "m.mefd")
    assert not [r for r in rep.refusals if r.code == "merge-units-mismatch"]
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
    # A's first ("uV") segment + B ("mV"): relabel -> refused
    rngs = [(T0, T0 + 1_000_000), None]
    rep = mef3io.merge.check([a, b], tmp_path / "n.mefd", time_ranges=rngs)
    assert [r.code for r in rep.refusals] == ["merge-units-mismatch"]
    with pytest.raises(mef3io.WriteConflictError):
        mef3io.merge_sessions([a, b], tmp_path / "n.mefd", time_ranges=rngs)
    # all of mixed A alone-in-merge + B "mV": B's data would carry "uV": refused
    rep = mef3io.merge.check([a, b], tmp_path / "n.mefd")
    assert [r.code for r in rep.refusals] == ["merge-units-mismatch"]


def test_mixed_source_note_names_label(tmp_path):
    a = _mixed_a(tmp_path)
    b = _src(tmp_path / "b.mefd", "uV", T0 + HOUR)
    rep = mef3io.merge.check([a, b], tmp_path / "m.mefd")
    assert any("first kept segment" in n for n in rep.notes)
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
