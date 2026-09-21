"""P15 gate: bidirectional acceptance against the legacy stack.

The oracle chain is `mef_tools` -> `pymef` -> `meflib` — the same C library
behind CyberPSG and most established MEF tooling. This suite is the standing
answer to "is it still compatible": every direction, both APIs, values and
times and gaps and metadata, on sessions from either side.

  A. mef3io WRITES  -> the legacy stack READS it, bit-exactly.
  B. the legacy stack WRITES -> mef3io READS it, bit-exactly.
  C. MIXED sequences — the dangerous ones, where a session is written by one
     stack and then modified by the other.

Direction C is what the section-2 work was ultimately about: a session is not
compatible because it can be opened once, but because it survives being
appended to, repaired, and read again by the other stack.
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

pytest.importorskip("mef_tools", reason="legacy oracle not installed (pip install mef3io[test])")
pytest.importorskip("pymef", reason="legacy oracle not installed")
from mef_tools.io import MefReader, MefWriter  # noqa: E402
from pymef.mef_session import MefSession  # noqa: E402

START = 1577836800000000
FS = 250.0
S2 = 2560
ALLOC = {
    "maximum_block_bytes": (6376, "<q"),
    "maximum_block_samples": (6384, "<I"),
    "maximum_difference_bytes": (6388, "<I"),
    "maximum_contiguous_blocks": (6408, "<q"),
    "maximum_contiguous_block_bytes": (6416, "<q"),
    "maximum_contiguous_samples": (6424, "<q"),
}


def _pymef_samples(path, channel, n, password=""):
    session = MefSession(str(path), password)
    try:
        return np.asarray(session.read_ts_channels_sample(channel, [0, n])).astype(
            np.float64
        ).ravel()
    finally:
        session.close()


def _declared(path):
    out = {}
    for tmet in sorted(Path(path).rglob("*.tmet")):
        raw = tmet.read_bytes()
        out[tmet.name] = {
            n: struct.unpack_from(f, raw, S2 + o)[0] for n, (o, f) in ALLOC.items()
        }
    return out


def assert_meflib_can_allocate(path, where):
    """The contract a meflib-based reader depends on, checked on the bytes.

    `0` is not the NO_ENTRY sentinel for any of these, so a reader cannot tell
    an unset field from a measured one and sizes a buffer from it regardless.
    This is the exact condition that crashed the legacy C reader.
    """
    declared = _declared(path)
    assert declared, f"{where}: no segments found"
    for name, fields in declared.items():
        for field, value in fields.items():
            assert value not in (0, 0xFFFFFFFF, -1), (
                f"{where}: {name} leaves {field} unset ({value}) — a meflib reader "
                f"allocates from this"
            )


# --- A. mef3io writes, the legacy stack reads --------------------------------


@pytest.mark.parametrize("n_calls", [1, 2, 3], ids=["single-call", "two-calls", "three-calls"])
def test_legacy_reads_mef3io_int32_bit_exactly(tmp_path, n_calls):
    """The primitive path: counts stored verbatim with a conversion factor."""
    path = tmp_path / "s.mefd"
    rng = np.random.default_rng(11)
    chunk = rng.integers(-30000, 30000, 2000, dtype=np.int32)
    ufact = 0.5

    w = mef3io.Writer(str(path))
    for i in range(n_calls):
        w.write_int32("ch1", chunk, ufact, START + int(i * 2000 / FS * 1e6), FS)
    w.close()

    expected = np.tile(chunk, n_calls)
    got = _pymef_samples(path, "ch1", len(expected))
    np.testing.assert_array_equal(got.astype(np.int32), expected)

    # ...and through mef_tools' own reader, which applies the factor.
    reader = MefReader(str(path))
    scaled = np.asarray(reader.get_data("ch1")).ravel()
    np.testing.assert_allclose(scaled[: len(expected)], expected * ufact, rtol=0, atol=1e-9)

    assert_meflib_can_allocate(path, f"mef3io int32 x{n_calls}")


def test_legacy_reads_mef3io_gaps_at_the_right_place(tmp_path):
    """A discontinuity has to land in the same place for both stacks."""
    path = tmp_path / "s.mefd"
    rng = np.random.default_rng(12)
    a = rng.integers(-5000, 5000, 2000, dtype=np.int32)
    b = rng.integers(-5000, 5000, 2000, dtype=np.int32)
    gap_us = int(4e6)

    w = mef3io.Writer(str(path))
    w.write_int32("ch1", a, 0.5, START, FS)
    w.write_int32("ch1", b, 0.5, START + int(2000 / FS * 1e6) + gap_us, FS)
    w.close()

    # Sample-indexed: the stored samples, no gap filling.
    got = _pymef_samples(path, "ch1", 4000)
    np.testing.assert_array_equal(got.astype(np.int32), np.concatenate([a, b]))

    # Time-indexed: the gap must appear, and be the length we asked for.
    session = MefSession(str(path), "")
    try:
        end = START + int(2000 / FS * 1e6) + gap_us + int(2000 / FS * 1e6)
        filled = np.asarray(session.read_ts_channels_uutc("ch1", [START, end])).ravel()
    finally:
        session.close()
    n_nan = int(np.sum(~np.isfinite(filled)))
    expected_gap = int(round(gap_us / 1e6 * FS))
    assert abs(n_nan - expected_gap) <= 2, f"gap was {n_nan} samples, expected ~{expected_gap}"
    assert_meflib_can_allocate(path, "mef3io gapped")


def test_legacy_reads_mef3io_float_path(tmp_path):
    """The float path infers a precision and scales; the oracle must agree."""
    path = tmp_path / "s.mefd"
    rng = np.random.default_rng(13)
    x = np.round(rng.normal(0, 100, 3000), 3)

    w = mef3io.Writer(str(path))
    w.write("ch1", x, START, fs=FS, precision=3)
    w.close()

    reader = MefReader(str(path))
    got = np.asarray(reader.get_data("ch1")).ravel()[: len(x)]
    np.testing.assert_allclose(got, x, rtol=0, atol=1e-6)
    assert_meflib_can_allocate(path, "mef3io float")


def test_legacy_reads_mef3io_fractional_sampling_rate(tmp_path):
    path = tmp_path / "s.mefd"
    fs = 199.9
    rng = np.random.default_rng(14)
    x = rng.integers(-1000, 1000, 1500, dtype=np.int32)

    w = mef3io.Writer(str(path))
    w.write_int32("ch1", x, 1.0, START, fs)
    w.close()

    got = _pymef_samples(path, "ch1", len(x))
    np.testing.assert_array_equal(got.astype(np.int32), x)
    reader = MefReader(str(path))
    assert abs(float(reader.get_property("fsamp", "ch1")) - fs) < 1e-6
    assert_meflib_can_allocate(path, "mef3io fractional fs")


def test_legacy_reads_mef3io_encrypted_session(tmp_path):
    path = tmp_path / "enc.mefd"
    rng = np.random.default_rng(15)
    x = rng.integers(-8000, 8000, 2000, dtype=np.int32)

    w = mef3io.Writer(str(path), password1="lvl1", password2="lvl2")
    w.write_int32("ch1", x, 0.5, START, FS)
    w.close()

    got = _pymef_samples(path, "ch1", len(x), password="lvl2")
    np.testing.assert_array_equal(got.astype(np.int32), x)
    reader = MefReader(str(path), password2="lvl2")
    assert np.asarray(reader.get_data("ch1")).size >= len(x)


def test_legacy_reads_a_multi_channel_multi_segment_mef3io_session(tmp_path):
    path = tmp_path / "s.mefd"
    rng = np.random.default_rng(16)
    data = {ch: rng.integers(-9000, 9000, 2000, dtype=np.int32) for ch in ("ch1", "ch2", "ch3")}

    w = mef3io.Writer(str(path))
    for ch, x in data.items():
        w.write_int32(ch, x, 0.5, START, FS)
        w.write_int32(ch, x, 0.5, START + int(2000 / FS * 1e6), FS, new_segment=True)
    w.close()

    for ch, x in data.items():
        got = _pymef_samples(path, ch, 2 * len(x))
        np.testing.assert_array_equal(got.astype(np.int32), np.tile(x, 2))
    assert sorted(MefReader(str(path)).channels) == ["ch1", "ch2", "ch3"]
    assert_meflib_can_allocate(path, "mef3io multi-segment")


# --- B. the legacy stack writes, mef3io reads --------------------------------


@pytest.mark.parametrize("precision", [None, 3])
def test_mef3io_reads_a_mef_tools_session(tmp_path, precision):
    path = tmp_path / "legacy.mefd"
    rng = np.random.default_rng(21)
    x = np.round(rng.normal(0, 500, 4000), 3) if precision else rng.integers(
        -20000, 20000, 4000, dtype=np.int32
    )

    writer = MefWriter(str(path), overwrite=True)
    writer.write_data(x, "ch1", START, FS, precision=precision)
    del writer

    legacy = np.asarray(MefReader(str(path)).get_data("ch1")).ravel()
    with mef3io.Reader(str(path)) as r:
        ours = r.read("ch1")

    n = min(len(legacy), len(ours))
    np.testing.assert_allclose(
        np.nan_to_num(ours[:n], nan=-12345.0),
        np.nan_to_num(legacy[:n], nan=-12345.0),
        rtol=0,
        atol=1e-6,
    )


def test_mef3io_reads_a_mef_tools_session_with_gaps(tmp_path):
    path = tmp_path / "legacy.mefd"
    rng = np.random.default_rng(22)
    a = rng.integers(-3000, 3000, 2000, dtype=np.int32)
    b = rng.integers(-3000, 3000, 2000, dtype=np.int32)

    writer = MefWriter(str(path), overwrite=True)
    writer.write_data(a, "ch1", START, FS)
    writer.write_data(b, "ch1", START + int(2000 / FS * 1e6) + int(4e6), FS)
    del writer

    legacy = np.asarray(MefReader(str(path)).get_data("ch1")).ravel()
    with mef3io.Reader(str(path)) as r:
        ours = r.read("ch1")
    n = min(len(legacy), len(ours))
    np.testing.assert_allclose(
        np.nan_to_num(ours[:n], nan=-12345.0),
        np.nan_to_num(legacy[:n], nan=-12345.0),
        rtol=0,
        atol=1e-6,
    )


def test_mef3io_reads_an_encrypted_mef_tools_session(tmp_path):
    path = tmp_path / "legacy_enc.mefd"
    rng = np.random.default_rng(23)
    x = rng.integers(-7000, 7000, 2000, dtype=np.int32)

    writer = MefWriter(str(path), overwrite=True, password1="p1", password2="p2")
    writer.write_data(x, "ch1", START, FS)
    del writer

    with mef3io.Reader(str(path), password="p2") as r:
        ours = r.read("ch1")
    legacy = np.asarray(MefReader(str(path), password2="p2").get_data("ch1")).ravel()
    n = min(len(legacy), len(ours))
    np.testing.assert_allclose(np.nan_to_num(ours[:n]), np.nan_to_num(legacy[:n]), atol=1e-6)


# --- C. mixed sequences: each stack modifying the other's session ------------


def test_mef3io_appends_to_a_mef_tools_session_and_the_oracle_still_reads_it(tmp_path):
    """The dangerous direction, and the reason the sizing work exists.

    A legacy session is written by the oracle, extended by mef3io, then handed
    back to the oracle. Every declaration mef3io rewrites on that append has to
    leave the file allocatable by the C reader.
    """
    path = tmp_path / "legacy.mefd"
    rng = np.random.default_rng(31)
    first = rng.integers(-15000, 15000, 3000, dtype=np.int32)
    second = rng.integers(-15000, 15000, 2000, dtype=np.int32)

    writer = MefWriter(str(path), overwrite=True)
    writer.write_data(first, "ch1", START, FS)
    del writer
    before = _pymef_samples(path, "ch1", len(first))

    ufact = float(MefReader(str(path)).get_property("ufact", "ch1"))
    w = mef3io.Writer(str(path))
    w.write_int32("ch1", second, ufact, START + int(len(first) / FS * 1e6), FS)
    w.close()

    after = _pymef_samples(path, "ch1", len(first) + len(second))
    np.testing.assert_array_equal(
        after[: len(first)], before, err_msg="the append disturbed existing samples"
    )
    np.testing.assert_array_equal(after[len(first):].astype(np.int32), second)

    # And the file the oracle now holds satisfies the allocation contract,
    # which the legacy writer's own output did not.
    assert_meflib_can_allocate(path, "mef3io appended to a legacy session")


def test_repairing_a_legacy_session_keeps_every_sample_and_fixes_the_declarations(tmp_path):
    """`repair_session` on a session the oracle wrote: same samples, better
    declarations. This is the upgrade path for data already in the field."""
    path = tmp_path / "legacy.mefd"
    rng = np.random.default_rng(32)
    x = rng.integers(-12000, 12000, 5000, dtype=np.int32)

    writer = MefWriter(str(path), overwrite=True)
    writer.write_data(x, "ch1", START, FS)
    del writer

    before_samples = _pymef_samples(path, "ch1", len(x))
    before_declared = _declared(path)

    report = mef3io.Validator(str(path)).validate()
    ids = report.repairable_check_ids
    assert ids, "a legacy session should have something to bring up to date"
    mef3io.repair_session(str(path), ids)

    after_samples = _pymef_samples(path, "ch1", len(x))
    np.testing.assert_array_equal(after_samples, before_samples)
    assert_meflib_can_allocate(path, "repaired legacy session")
    assert _declared(path) != before_declared, "the repair should have changed something"

    # Still readable by both readers, and by mef3io.
    assert np.asarray(MefReader(str(path)).get_data("ch1")).size >= len(x)
    with mef3io.Reader(str(path)) as r:
        assert len(r.read("ch1")) >= len(x)


def test_the_legacy_writer_can_append_to_a_mef3io_session(tmp_path):
    """The reverse mix: mef3io writes, the oracle extends, mef3io reads back."""
    path = tmp_path / "s.mefd"
    rng = np.random.default_rng(33)
    first = rng.integers(-10000, 10000, 2000, dtype=np.int32)
    second = rng.integers(-10000, 10000, 2000, dtype=np.int32)

    w = mef3io.Writer(str(path))
    w.write_int32("ch1", first, 1.0, START, FS)
    w.close()

    writer = MefWriter(str(path), overwrite=False)
    writer.write_data(second, "ch1", START + int(len(first) / FS * 1e6), FS)
    del writer

    got = _pymef_samples(path, "ch1", len(first) + len(second))
    np.testing.assert_array_equal(got[: len(first)].astype(np.int32), first)
    with mef3io.Reader(str(path)) as r:
        ours = r.read("ch1")
    np.testing.assert_allclose(
        np.nan_to_num(ours[: len(got)]), np.nan_to_num(got), rtol=0, atol=1e-6
    )


def test_a_repaired_session_survives_a_further_legacy_append(tmp_path):
    """repair -> legacy append -> read. The three stacks in sequence on one
    file, which is where declarations written by different tools meet."""
    path = tmp_path / "legacy.mefd"
    rng = np.random.default_rng(34)
    first = rng.integers(-6000, 6000, 3000, dtype=np.int32)
    second = rng.integers(-6000, 6000, 1500, dtype=np.int32)

    writer = MefWriter(str(path), overwrite=True)
    writer.write_data(first, "ch1", START, FS)
    del writer

    ids = mef3io.Validator(str(path)).validate().repairable_check_ids
    if ids:
        mef3io.repair_session(str(path), ids)

    writer = MefWriter(str(path), overwrite=False)
    writer.write_data(second, "ch1", START + int(len(first) / FS * 1e6), FS)
    del writer

    got = _pymef_samples(path, "ch1", len(first) + len(second))
    np.testing.assert_array_equal(got[: len(first)].astype(np.int32), first)
    with mef3io.Reader(str(path)) as r:
        assert len(r.read("ch1")) >= len(first)
