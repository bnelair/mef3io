"""P7: compat shim (mef_tools.io drop-in), records round-trip, and the
metadata cache (warm start) behaviors."""
import glob
import os
import sys
import warnings
from pathlib import Path

import numpy as np
import pytest

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO))
warnings.filterwarnings("ignore")

import mef3io  # noqa: E402
from mef3io import cache as C  # noqa: E402
from mef3io.compat import MefReader as CompatReader  # noqa: E402
from mef3io.compat import MefWriter as CompatWriter  # noqa: E402

pytest.importorskip("mef_tools", reason="legacy oracle not installed (pip install mef3io[test])")
from mef_tools.io import MefReader as LegacyReader  # noqa: E402

START = 1577836800000000
FS = 256.0


def _sig(n=3000):
    return 40 * np.sin(np.arange(n) / 20)


def test_compat_writer_read_by_legacy(tmp_path):
    path = str(tmp_path / "c.mefd")
    data = _sig()
    data[1000:1200] = np.nan
    w = CompatWriter(path, overwrite=True)
    w.write_data(data, "ch1", START, FS, precision=3)
    w.close()
    lr = LegacyReader(path)
    got = np.asarray(lr.get_data("ch1"), float)
    exp = np.where(np.isnan(data), np.nan, np.round(data, 3))
    assert np.array_equal(np.isnan(got), np.isnan(exp))
    assert np.allclose(got[~np.isnan(got)], exp[~np.isnan(exp)], atol=6e-4)


def test_compat_reader_api(tmp_path):
    path = str(tmp_path / "c.mefd")
    with mef3io.Writer(path, overwrite=True) as w:
        w.write("ch1", _sig(), START, FS, 3)
    r = CompatReader(path)
    assert r.channels == ["ch1"]
    assert r.get_property("fsamp", "ch1") == pytest.approx(FS)
    assert r.get_channel_info("ch1")["name"] == "ch1"
    assert np.asarray(r.get_data("ch1")).shape[0] == 3000


def test_top_level_legacy_imports(tmp_path):
    # Legacy code should run with just the import changed to `mef3io`:
    # NaN gaps, inferred precision, annotations, legacy writer properties.
    from mef3io import MefReader as TopReader
    from mef3io import MefWriter as TopWriter

    path = str(tmp_path / "c.mefd")
    data = _sig()
    data[500:700] = np.nan

    w = TopWriter(path, overwrite=True)
    w.data_units = "mV"
    w.mef_block_len = 512
    w.max_nans_written = 0   # legacy knobs accepted
    w.record_offset = 0
    assert w.get_mefblock_len(FS) == 512
    w.write_data(data, "ch1", START, FS)  # precision inferred
    w.write_annotations([{"time": START + 1000, "text": "note", "type": "Note"}], "ch1")
    w.close()

    r = TopReader(path)
    assert r.channels == ["ch1"]
    got = np.asarray(r.get_data("ch1"), float)
    assert np.array_equal(np.isnan(got), np.isnan(data))
    ufact = r.get_property("ufact", "ch1")
    assert np.allclose(got[~np.isnan(got)], data[~np.isnan(data)], atol=ufact / 2 + 1e-12)
    assert r.get_property("unit", "ch1") == b"mV"
    assert r.get_annotations("ch1")[0]["text"] == "note"

    # oracle agreement, and the int32 primitive path through the legacy API
    legacy = np.asarray(LegacyReader(path).get_data("ch1"), float)
    assert np.array_equal(np.isnan(legacy), np.isnan(got))
    assert np.allclose(legacy[~np.isnan(legacy)], got[~np.isnan(got)])

    path2 = str(tmp_path / "i.mefd")
    counts = (np.arange(1000) % 100).astype(np.int32)
    w2 = TopWriter(path2, overwrite=True)
    w2.write_data(counts, "ch1", START, FS, precision=2)  # ufact = 10^-2
    w2.close()
    r2 = TopReader(path2)
    assert np.allclose(np.asarray(r2.get_data("ch1")), counts * 0.01)


def test_records_write_read_roundtrip(tmp_path):
    path = str(tmp_path / "r.mefd")
    annotations = [
        {"type": "Note", "time": START + 100000, "text": "onset"},
        {"type": "Note", "time": START + 500000, "text": "offset"},
    ]
    with mef3io.Writer(path, overwrite=True) as w:
        w._impl.write_float("ch1", np.ascontiguousarray(_sig()), START, FS, 3)
        w._impl.write_records("ch1", annotations)
    # read back via mef3io and via legacy
    with mef3io.Reader(path) as r:
        recs = r.records("ch1")
    assert [(x["type"], x["time"], x["text"]) for x in recs] == [
        (a["type"], a["time"], a["text"]) for a in annotations
    ]
    legacy = LegacyReader(path).get_annotations("ch1")
    assert [x["text"] for x in legacy] == ["onset", "offset"]


def test_encrypted_records_roundtrip(tmp_path):
    path = str(tmp_path / "re.mefd")
    with mef3io.Writer(path, overwrite=True, password1="p1", password2="p2") as w:
        w._impl.write_float("ch1", np.ascontiguousarray(_sig()), START, FS, 3)
        w._impl.write_records("ch1", [{"type": "Note", "time": START + 1000, "text": "secret"}])
    with mef3io.Reader(path, "p2") as r:
        recs = r.records("ch1")
    assert recs[0]["text"] == "secret"


# --- cache behaviors ---


def _make_session(tmp_path):
    path = str(tmp_path / "s.mefd")
    with mef3io.Writer(path, overwrite=True) as w:
        w.write("ch1", _sig(), START, FS, 3)
    return path


def test_default_open_uses_no_cache(tmp_path):
    path = _make_session(tmp_path)
    r = mef3io.Reader(path)
    assert r._impl is not None  # backend built eagerly, no cache path


def test_auto_cache_warm_start_defers_backend(tmp_path):
    path = _make_session(tmp_path)
    cp = C.auto_cache_path(path)
    if cp.exists():
        cp.unlink()
    cold = mef3io.Reader(path, cache="auto")
    assert cp.exists()
    _ = cold.channels
    warm = mef3io.Reader(path, cache="auto")
    assert warm._impl is None  # served from cache
    assert warm.channels == ["ch1"]
    assert warm.info("ch1")["number_of_samples"] == 3000
    assert warm._impl is None  # still deferred after metadata queries
    _ = warm.read("ch1")  # data read builds the backend
    assert warm._impl is not None
    cp.unlink(missing_ok=True)


def test_cache_invalidated_by_fingerprint_change(tmp_path):
    path = _make_session(tmp_path)
    cp = C.auto_cache_path(path)
    mef3io.Reader(path, cache="auto")  # create snapshot
    assert C.load_valid(cp, path) is not None
    tmet = glob.glob(path + "/ch1.timd/*/*.tmet")[0]
    os.utime(tmet, None)  # touch -> mtime changes
    assert C.load_valid(cp, path) is None
    cp.unlink(missing_ok=True)


def test_write_removes_auto_cache(tmp_path):
    path = _make_session(tmp_path)
    cp = C.auto_cache_path(path)
    mef3io.Reader(path, cache="auto")
    assert cp.exists()
    with mef3io.Writer(path) as _:
        pass
    assert not cp.exists()


def test_persistent_cache_path(tmp_path):
    path = _make_session(tmp_path)
    cache_file = tmp_path / "explicit.mef3cache"
    mef3io.Reader(path, cache=str(cache_file))
    assert cache_file.exists()
    warm = mef3io.Reader(path, cache=str(cache_file))
    assert warm._impl is None
    assert warm.channels == ["ch1"]
