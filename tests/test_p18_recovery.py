"""P18 gate: crash recovery after an interrupted write.

`durability="fast"` trades the ordering barriers for speed, so a power cut can
leave a segment's index and data disagreeing. That trade is only reasonable
because the disagreement is detectable and repairable — this is the suite that
says so.

Two shapes, handled differently because one has lost data and the other has not:

  INDEX AHEAD OF DATA — entries reference bytes that never landed. Those samples
  do not exist; the entries go.

  DATA AHEAD OF INDEX — blocks reached .tdat but the index was not extended.
  Those samples DO exist and are recovered from the RED block headers.
"""
import struct
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
UH = 1024
TSI = 56


def _session(tmp_path, chunks=6, n=2000):
    path = tmp_path / "s.mefd"
    rng = np.random.default_rng(3)
    w = mef3io.Writer(str(path), durability="fast")
    for i in range(chunks):
        w.write_int32("ch1", rng.integers(-9000, 9000, n, dtype=np.int32), 0.5,
                      START + int(i * n / FS * 1e6), FS)
    w.close()
    seg = next(Path(path).glob("*.timd/*.segd"))
    return path, next(seg.glob("*.tidx")), next(seg.glob("*.tdat"))


def _entries(tidx):
    return (tidx.stat().st_size - UH) // TSI


def _fix_index_header(tidx, n_entries):
    """Make the .tidx header self-consistent after truncating entries."""
    raw = bytearray(tidx.read_bytes())
    struct.pack_into("<q", raw, 32, n_entries)
    struct.pack_into("<I", raw, 4, m.crc32(bytes(raw[UH:])))
    struct.pack_into("<I", raw, 0, m.crc32(bytes(raw[4:UH])))
    tidx.write_bytes(bytes(raw))


def test_data_ahead_of_index_is_recovered_not_discarded(tmp_path):
    """Blocks that reached .tdat but were never indexed hold real samples."""
    path, tidx, tdat = _session(tmp_path)
    full = _entries(tidx)
    with mef3io.Reader(str(path)) as r:
        expected = r.read("ch1")

    # Simulate: the .tdat write landed, the .tidx extension did not.
    keep = full - 3
    raw = tidx.read_bytes()[: UH + keep * TSI]
    tidx.write_bytes(raw)
    _fix_index_header(tidx, keep)
    # The reader grids by time, so the array stays the same length — the lost
    # blocks appear as NaN. Count real samples, not array length.
    live = lambda a: int(np.sum(np.isfinite(a)))
    with mef3io.Reader(str(path)) as r:
        assert live(r.read("ch1")) < live(expected), "the fixture must really lose samples"

    dry = mef3io.recover_session(str(path))
    assert not dry.applied
    assert dry.segments and dry.segments[0].blocks_recovered == 3, dry.summary()
    assert _entries(tidx) == keep, "a dry run must not write"

    done = mef3io.recover_session(str(path), apply=True)
    assert done.applied
    assert _entries(tidx) == full
    mef3io.repair_session(str(path), mef3io.Validator(str(path)).validate().repairable_check_ids)

    with mef3io.Reader(str(path)) as r:
        got = r.read("ch1")
    assert live(got) == live(expected), "recovery did not restore every sample"
    np.testing.assert_array_equal(
        np.nan_to_num(got, nan=-1.0), np.nan_to_num(expected, nan=-1.0)
    )
    assert mef3io.Validator(str(path)).validate().ok


def test_index_ahead_of_data_drops_the_unbacked_entries(tmp_path):
    """Entries referencing bytes that never landed describe samples that do not
    exist. They have to go, and the rest of the segment must survive."""
    path, tidx, tdat = _session(tmp_path)
    full = _entries(tidx)

    # Simulate: the .tidx was extended, the .tdat write did not land.
    raw = tdat.read_bytes()
    tdat.write_bytes(raw[: len(raw) // 2])

    report = mef3io.recover_session(str(path), apply=True)
    assert report.segments, report.summary()
    seg = report.segments[0]
    assert seg.blocks_dropped > 0
    assert _entries(tidx) < full

    mef3io.repair_session(str(path), mef3io.Validator(str(path)).validate().repairable_check_ids)
    assert mef3io.Validator(str(path)).validate().ok, "recovery must leave a valid session"
    with mef3io.Reader(str(path)) as r:
        assert len(r.read("ch1")) > 0, "the surviving blocks must still read"


def test_a_healthy_session_is_left_completely_alone(tmp_path):
    """Recovery must be a no-op on anything it does not need to touch."""
    path, tidx, tdat = _session(tmp_path)
    before = {f: f.read_bytes() for f in Path(path).rglob("*") if f.is_file()}

    report = mef3io.recover_session(str(path), apply=True)
    assert report.nothing_to_do, report.summary()
    assert report.segments_examined > 0
    for f, data in before.items():
        assert f.read_bytes() == data, f"{f.name} was modified"


def test_recovery_backs_up_what_it_changes_but_not_the_data_file(tmp_path):
    """A .tdat can be tens of gigabytes. Copying it to undo a header patch
    would make the tool unusable on the files it exists for."""
    path, tidx, tdat = _session(tmp_path)
    raw = tdat.read_bytes()
    tdat.write_bytes(raw[: len(raw) // 2])
    tdat_size = tdat.stat().st_size

    mef3io.recover_session(str(path), apply=True, backup=True)
    backup = Path(str(path) + ".recover-backup")
    assert backup.is_dir(), "a backup should have been taken"
    saved = list(backup.rglob("*"))
    names = {f.name for f in saved if f.is_file()}
    assert any(n.endswith(".tidx") for n in names), names
    assert not any(n.endswith(".tdat") for n in names), (
        f"the whole data file was copied: {names}"
    )
    total = sum(f.stat().st_size for f in saved if f.is_file())
    assert total < tdat_size, (
        f"backup is {total} bytes against a {tdat_size}-byte .tdat — it is copying the data"
    )


def test_recovery_refuses_a_tar_archive(tmp_path):
    path, _, _ = _session(tmp_path)
    archive = mef3io.archive_session(str(path))
    with pytest.raises(RuntimeError, match="tar"):
        mef3io.recover_session(archive, apply=True)
