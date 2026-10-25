"""P40: the writer refuses arguments it cannot store, BEFORE anything is
written, and accepts the unusual-but-valid ones (#83).

- A sampling frequency of 0, a negative one, NaN or infinity used to hang the
  writer (a block length of 0 never advances), throw from inside std::vector,
  or kill MATLAB. A conversion factor of 0, NaN or infinity was stored, and
  every sample then read back as 0 or NaN: the signal was gone. All of them
  are now a ValueError on every write path (write, write_int32, append), with
  no channel folder created and no byte of an existing segment moved.
- A NEGATIVE conversion factor is a polarity inversion and is allowed:
  pymef handles it explicitly (pymef3_file.c:972-978 swaps the native
  min/max), and meflib's NO_ENTRY for the field is 0.0 (meflib.h:439), which
  is one more reason 0 is refused rather than written.
- A block longer than its 32-bit size fields can describe is refused, and a
  rate whose derived block would be longer too (the derivation used to be an
  undefined double -> int cast).
- Appending to a foreign writer's un-padded segment folder (``ch1-5.segd``)
  extends that folder under its own name.

Oracle-free except the negative-factor interop case, which skips without
pymef / mef_tools.
"""
from __future__ import annotations

import math
import shutil
import sys
import warnings
from pathlib import Path

import numpy as np
import pytest

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO))
warnings.filterwarnings("ignore")

import mef3io  # noqa: E402
from mef3io import _mef3io as m  # noqa: E402

START = 1577836800000000
FS = 256.0
BAD_FS = [0.0, -256.0, math.nan, math.inf, -math.inf]
BAD_UFACT = [0.0, math.nan, math.inf, -math.inf]


def _tree(path: Path) -> dict:
    """Every file under ``path`` with its bytes."""
    return {p.relative_to(path): p.read_bytes() for p in sorted(path.rglob("*")) if p.is_file()}


@pytest.mark.parametrize("fs", BAD_FS)
def test_a_bad_rate_is_refused_before_anything_is_written(tmp_path, fs):
    path = tmp_path / "s.mefd"
    with mef3io.Writer(str(path)) as w:
        with pytest.raises(ValueError, match="sampling frequency"):
            w.write("ch1", np.zeros(100), START, fs)
        with pytest.raises(ValueError, match="sampling frequency"):
            w.write_int32("ch1", np.zeros(100, np.int32), 1.0, START, fs)
    assert list(path.iterdir()) == [], "a refused write must create no channel folder"


@pytest.mark.parametrize("ufact", BAD_UFACT)
def test_a_bad_conversion_factor_is_refused_before_anything_is_written(tmp_path, ufact):
    path = tmp_path / "s.mefd"
    with mef3io.Writer(str(path)) as w:
        with pytest.raises(ValueError, match="conversion factor"):
            w.write_int32("ch1", np.arange(100, dtype=np.int32), ufact, START, FS)
    assert list(path.iterdir()) == []


def test_a_precision_whose_factor_underflows_to_zero_is_refused(tmp_path):
    """10**-400 is 0.0: the same unrecoverable factor, reached via write()."""
    path = tmp_path / "s.mefd"
    with mef3io.Writer(str(path)) as w:
        with pytest.raises(ValueError, match="conversion factor"):
            w.write("ch1", np.zeros(10), START, FS, precision=400)
    assert list(path.iterdir()) == []


@pytest.mark.parametrize("bad", [
    {"fs": 0.0}, {"fs": math.nan}, {"fs": -FS}, {"ufact": 0.0}, {"ufact": math.nan},
])
def test_an_append_with_a_bad_argument_moves_no_byte(tmp_path, bad):
    path = tmp_path / "s.mefd"
    with mef3io.Writer(str(path)) as w:
        w.write_int32("ch1", np.arange(2560, dtype=np.int32), 0.5, START, FS)
    before = _tree(path)
    with mef3io.Writer(str(path)) as w:
        args = {"fs": FS, "ufact": 0.5, **bad}
        with pytest.raises(ValueError):
            w.write_int32("ch1", np.arange(256, dtype=np.int32), args["ufact"],
                          START + 10_000_000, args["fs"])
        with pytest.raises(ValueError):
            w.write_int32("ch1", np.arange(256, dtype=np.int32), args["ufact"],
                          START + 10_000_000, args["fs"], new_segment=True)
    assert _tree(path) == before


def test_a_tiny_rate_is_refused_instead_of_hanging(tmp_path):
    """1e-300 Hz is finite and positive, but 10 samples would end ~1e307 us
    later. It used to never return."""
    path = tmp_path / "s.mefd"
    with mef3io.Writer(str(path)) as w:
        with pytest.raises(ValueError, match="representable time"):
            w.write_int32("ch1", np.arange(10, dtype=np.int32), 1.0, START, 1e-300)
    assert list(path.iterdir()) == []


def test_a_slow_rate_still_writes_and_reads(tmp_path):
    """Positive control: below 0.1 Hz the derived block (10 * fs) is less than
    one sample. It is clamped to one, not refused."""
    path = tmp_path / "s.mefd"
    with mef3io.Writer(str(path)) as w:
        w.write("temp", np.array([20.5, 21.0, 21.5]), START, 0.01, precision=1)
    with mef3io.Reader(str(path)) as r:
        np.testing.assert_array_equal(r.read("temp"), [20.5, 21.0, 21.5])
        assert r.info("temp")["sampling_frequency"] == 0.01


def test_a_block_too_long_for_its_size_fields_is_refused(tmp_path):
    limit = m.MAX_BLOCK_SAMPLES
    # 5 bytes per sample worst case + 304-byte header + 7 pad must stay below
    # the ui4 NO_ENTRY sentinel; one more sample would not.
    assert 5 * limit + 311 <= 0xFFFFFFFE < 5 * (limit + 1) + 311
    path = tmp_path / "s.mefd"
    with pytest.raises(ValueError, match="block length"):
        mef3io.Writer(str(path), block_length=limit + 1)
    assert not path.exists(), "refused before the session folder is created"
    mef3io.Writer(str(path), block_length=limit).close()  # the limit itself is fine


def test_a_rate_whose_default_block_is_too_long_needs_an_explicit_one(tmp_path):
    path = tmp_path / "s.mefd"
    x = np.arange(64, dtype=np.int32)
    with mef3io.Writer(str(path)) as w:
        with pytest.raises(ValueError, match="block length"):
            w.write_int32("ch1", x, 1.0, START, 1e12)
    assert list(path.iterdir()) == []
    with mef3io.Writer(str(path), block_length=16) as w:  # positive control
        assert w.write_int32("ch1", x, 1.0, START, 1e12)["blocks"] == 4


def test_a_valid_mask_of_the_wrong_length_is_refused(tmp_path):
    """It used to be read past its end."""
    path = tmp_path / "s.mefd"
    with mef3io.Writer(str(path)) as w:
        with pytest.raises(ValueError, match="valid mask"):
            w.write_int32("ch1", np.arange(100, dtype=np.int32), 1.0, START, FS,
                          valid=np.ones(50, bool))
    assert list(path.iterdir()) == []


# --- negative conversion factor: allowed, and read the same everywhere -------


def _negative_session(tmp_path):
    path = tmp_path / "neg.mefd"
    counts = (np.arange(5120) % 200 - 50).astype(np.int32)  # -50 .. 149
    with mef3io.Writer(str(path)) as w:
        w.write_int32("ch1", counts[:2560], -0.25, START, FS)
        w.write_int32("ch1", counts[2560:], -0.25, START + 10_000_000, FS)  # append
    return path, counts


def test_a_negative_factor_round_trips_with_min_and_max_in_order(tmp_path):
    path, counts = _negative_session(tmp_path)
    with mef3io.Reader(str(path)) as r:
        np.testing.assert_array_equal(r.read("ch1"), counts * -0.25)
        env = r.block_envelope("ch1")
        assert np.all(env["min"] <= env["max"])
        assert env["min"].min() == 149 * -0.25 and env["max"].max() == -50 * -0.25
    assert mef3io.Validator(str(path)).validate().ok


def test_a_negative_factor_reads_the_same_in_pymef_and_mef_tools(tmp_path):
    pymef = pytest.importorskip("pymef.mef_session", reason="legacy oracle not installed")
    mef_tools = pytest.importorskip("mef_tools.io", reason="legacy oracle not installed")
    path, counts = _negative_session(tmp_path)
    session = pymef.MefSession(str(path), "")
    raw = session.read_ts_channels_sample(["ch1"], [0, len(counts)])[0]
    np.testing.assert_array_equal(np.asarray(raw, dtype=np.int64), counts)
    s2 = session.session_md["time_series_channels"]["ch1"]["section_2"]
    ufact = float(np.asarray(s2["units_conversion_factor"]).ravel()[0])
    assert ufact == -0.25
    # The native extremes are stored swapped, as pymef itself writes them.
    assert float(np.asarray(s2["maximum_native_sample_value"]).ravel()[0]) == -50 * -0.25
    assert float(np.asarray(s2["minimum_native_sample_value"]).ravel()[0]) == 149 * -0.25
    session.close()
    legacy = mef_tools.MefReader(str(path)).get_data("ch1")
    with mef3io.Reader(str(path)) as r:
        np.testing.assert_allclose(np.asarray(legacy, dtype=float), r.read("ch1"))


# --- a foreign writer's un-padded segment folder ------------------------------


def _unpad(path: Path, channel="ch1"):
    """Rename every ``<ch>-NNNNNN`` segment folder and its files to
    ``<ch>-N``, the way a foreign writer may name them."""
    timd = path / f"{channel}.timd"
    for seg in sorted(timd.glob("*.segd")):
        number = int(seg.stem.rsplit("-", 1)[1])
        new = f"{channel}-{number}"
        for f in seg.iterdir():
            f.rename(seg / (new + f.suffix))
        seg.rename(timd / (new + ".segd"))


def test_an_append_extends_an_unpadded_foreign_segment_in_place(tmp_path):
    path = tmp_path / "s.mefd"
    x = np.arange(5120, dtype=np.int32) % 300
    with mef3io.Writer(str(path)) as w:
        w.write_int32("ch1", x[:2560], 0.5, START, FS)
        w.write_int32("ch1", x[:2560], 0.5, START + 60_000_000, FS, new_segment=True)
    _unpad(path)
    with mef3io.Writer(str(path)) as w:  # appends to ch1-1, the LAST segment
        w.write_int32("ch1", x[2560:], 0.5, START + 70_000_000, FS)
    names = sorted(p.name for p in (path / "ch1.timd").iterdir())
    assert names == ["ch1-0.segd", "ch1-1.segd"], "nothing renamed, nothing added"
    with mef3io.Reader(str(path)) as r:
        assert r.info("ch1")["number_of_samples"] == 3 * 2560
        got = r.read("ch1", START + 60_000_000)
        np.testing.assert_array_equal(got[~np.isnan(got)], np.concatenate([x[:2560], x[2560:]]) * 0.5)
    assert mef3io.Validator(str(path)).validate().ok
    # A NEW segment after them takes the next number, in the canonical form.
    with mef3io.Writer(str(path)) as w:
        w.write_int32("ch1", x[:256], 0.5, START + 200_000_000, FS, new_segment=True)
    assert (path / "ch1.timd" / "ch1-000002.segd").is_dir()
    with mef3io.Reader(str(path)) as r:
        assert r.info("ch1")["number_of_samples"] == 3 * 2560 + 256


def test_the_unpadded_append_matches_the_padded_one_byte_for_byte(tmp_path):
    """Negative control for the folder lookup: the same append through the
    padded and the un-padded name must write the same bytes."""
    x = np.arange(5120, dtype=np.int32) % 300
    padded = tmp_path / "a.mefd"
    with mef3io.Writer(str(padded)) as w:
        w.write_int32("ch1", x[:2560], 0.5, START, FS)
    unpadded = tmp_path / "b.mefd"
    shutil.copytree(padded, unpadded)
    _unpad(unpadded)
    for p in (padded, unpadded):
        with mef3io.Writer(str(p)) as w:
            w.write_int32("ch1", x[2560:], 0.5, START + 10_000_000, FS)

    def data(p):
        seg = next((p / "ch1.timd").glob("*.segd"))
        return {f.suffix: f.read_bytes()[1024:] for f in seg.iterdir()}

    a, b = data(padded), data(unpadded)
    assert a[".tdat"] == b[".tdat"] and a[".tidx"] == b[".tidx"]
