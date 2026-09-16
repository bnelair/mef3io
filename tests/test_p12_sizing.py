"""P12: the section-2 buffer-sizing declarations.

Metadata section 2 tells a reader how large a buffer to allocate for the
segment. meflib-based readers (CyberPSG and friends) trust those numbers, and
`0` is not the NO_ENTRY sentinel for any of them — a reader cannot tell an
unset field from a real zero, so an under-declared value truncates or NULLs its
buffer. mef3io <= 1.1.2 left `maximum_difference_bytes` and
`maximum_contiguous_block_bytes` at 0 and over-declared the other
`maximum_contiguous_*` fields.

These tests pin the written values against the blocks actually on disk, and pin
that *reading* never depends on the fields — sessions written by older mef3io
(zeros) or by a foreign writer (NO_ENTRY) must keep reading identically.
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

UH_BYTES = 1024
METADATA_FILE_BYTES = 16384
S2 = 1024 + 1536  # universal header + section 1
TIDX_RECORD_BYTES = 56
RED_DIFFERENCE_BYTES_OFFSET = 28  # ui4 within the 304-byte RED block header
DISCONTINUITY_MASK = 0x01

UI4_NO_ENTRY = 0xFFFFFFFF
SI8_NO_ENTRY = -1

# name -> (section-relative offset, struct format)
FIELDS = {
    "number_of_samples": (6360, "<q"),
    "number_of_blocks": (6368, "<q"),
    "maximum_block_bytes": (6376, "<q"),
    "maximum_block_samples": (6384, "<I"),
    "maximum_difference_bytes": (6388, "<I"),
    "number_of_discontinuities": (6400, "<q"),
    "maximum_contiguous_blocks": (6408, "<q"),
    "maximum_contiguous_block_bytes": (6416, "<q"),
    "maximum_contiguous_samples": (6424, "<q"),
}

SIZING_FIELDS = (
    "maximum_block_bytes",
    "maximum_block_samples",
    "maximum_difference_bytes",
    "maximum_contiguous_blocks",
    "maximum_contiguous_block_bytes",
    "maximum_contiguous_samples",
)


def _segments(path):
    return sorted(Path(path).rglob("*.tmet"))


def _read_section2(tmet):
    raw = Path(tmet).read_bytes()
    return {n: struct.unpack_from(f, raw, S2 + off)[0] for n, (off, f) in FIELDS.items()}


def _patch_section2(tmet, **values):
    """Overwrite section-2 fields in place, repairing both universal-header CRCs.

    The body CRC covers raw[1024:METADATA_FILE_BYTES] and lives inside the
    header CRC's window, so it has to be written first.
    """
    tmet = Path(tmet)
    raw = bytearray(tmet.read_bytes())
    for name, value in values.items():
        off, fmt = FIELDS[name]
        struct.pack_into(fmt, raw, S2 + off, value)
    struct.pack_into("<I", raw, 4, m.crc32(bytes(raw[UH_BYTES:METADATA_FILE_BYTES])))
    struct.pack_into("<I", raw, 0, m.crc32(bytes(raw[4:UH_BYTES])))
    tmet.write_bytes(bytes(raw))


def _real_stats(tmet):
    """Recompute the sizing statistics from the blocks on disk.

    `maximum_difference_bytes` comes from the RED block headers in .tdat; the
    contiguous run maxima come from the .tidx, delimited by the same
    discontinuity flag a reader uses.
    """
    tmet = Path(tmet)
    entries = tmet.with_suffix(".tidx").read_bytes()[UH_BYTES:]
    stats = dict.fromkeys(SIZING_FIELDS, 0)
    stats["number_of_blocks"] = len(entries) // TIDX_RECORD_BYTES
    run_blocks = run_bytes = run_samples = 0
    with open(tmet.with_suffix(".tdat"), "rb") as tdat:
        for i in range(stats["number_of_blocks"]):
            e = entries[i * TIDX_RECORD_BYTES : (i + 1) * TIDX_RECORD_BYTES]
            (file_offset,) = struct.unpack_from("<q", e, 0)
            (n_samples,) = struct.unpack_from("<I", e, 24)
            (block_bytes,) = struct.unpack_from("<I", e, 28)
            (flags,) = struct.unpack_from("<B", e, 44)

            tdat.seek(file_offset + RED_DIFFERENCE_BYTES_OFFSET)
            (difference_bytes,) = struct.unpack("<I", tdat.read(4))

            stats["maximum_block_bytes"] = max(stats["maximum_block_bytes"], block_bytes)
            stats["maximum_block_samples"] = max(stats["maximum_block_samples"], n_samples)
            stats["maximum_difference_bytes"] = max(
                stats["maximum_difference_bytes"], difference_bytes
            )

            if flags & DISCONTINUITY_MASK:
                run_blocks = run_bytes = run_samples = 0
            run_blocks += 1
            run_bytes += block_bytes
            run_samples += n_samples
            stats["maximum_contiguous_blocks"] = max(stats["maximum_contiguous_blocks"], run_blocks)
            stats["maximum_contiguous_block_bytes"] = max(
                stats["maximum_contiguous_block_bytes"], run_bytes
            )
            stats["maximum_contiguous_samples"] = max(
                stats["maximum_contiguous_samples"], run_samples
            )
    return stats


def _write(path, gap_us=int(5e6), n=4000, channels=("ch1",)):
    rng = np.random.default_rng(0)
    x = rng.normal(0, 3000, n).astype(np.int32)
    w = mef3io.Writer(path)
    for ch in channels:
        w.write_int32(ch, x, 0.5, START, FS)
        w.write_int32(ch, x, 0.5, START + int(n / FS * 1e6) + gap_us, FS)
    w.close()
    return x


# --- what gets written -------------------------------------------------------


def test_sizing_fields_match_the_blocks_on_disk(tmp_path):
    path = str(tmp_path / "s.mefd")
    _write(path, channels=("ch1", "ch2"))

    for tmet in _segments(path):
        stored = _read_section2(tmet)
        real = _real_stats(tmet)
        for name in SIZING_FIELDS:
            assert stored[name] == real[name], f"{name} in {tmet.name}"


def test_no_sizing_field_is_zero_or_no_entry(tmp_path):
    """0 is indistinguishable from 'unset' to a reader, and NO_ENTRY makes it
    allocate nothing (or 4 GB). Every field must carry a real measurement."""
    path = str(tmp_path / "s.mefd")
    _write(path)

    for tmet in _segments(path):
        stored = _read_section2(tmet)
        for name in SIZING_FIELDS:
            assert stored[name] > 0, f"{name} left unset"
        assert stored["maximum_difference_bytes"] != UI4_NO_ENTRY
        assert stored["maximum_block_samples"] != UI4_NO_ENTRY
        assert stored["maximum_contiguous_block_bytes"] != SI8_NO_ENTRY


def test_contiguous_fields_respect_discontinuities(tmp_path):
    """A gap splits the segment: the contiguous maxima describe the longest run,
    not the whole channel (which is what <= 1.1.2 declared)."""
    path = str(tmp_path / "s.mefd")
    _write(path, gap_us=int(5e6))

    stored = _read_section2(_segments(path)[0])
    assert stored["number_of_blocks"] > stored["maximum_contiguous_blocks"]
    assert stored["number_of_samples"] > stored["maximum_contiguous_samples"]
    assert stored["maximum_contiguous_samples"] == 4000


def test_difference_bytes_within_meflib_worst_case(tmp_path):
    """meflib sizes its difference buffer at 5 bytes/sample worst case; a real
    maximum above that would mean a reader trusting the bound overflows."""
    path = str(tmp_path / "s.mefd")
    _write(path)

    stored = _read_section2(_segments(path)[0])
    assert stored["maximum_difference_bytes"] <= 5 * stored["maximum_block_samples"]


# --- appends -----------------------------------------------------------------


def test_append_keeps_sizing_fields_exact(tmp_path):
    path = str(tmp_path / "s.mefd")
    x = _write(path, gap_us=0)
    w = mef3io.Writer(path)
    w.write_int32("ch1", x, 0.5, START + int(3 * 4000 / FS * 1e6), FS)
    w.close()

    tmet = _segments(path)[0]
    stored = _read_section2(tmet)
    real = _real_stats(tmet)
    for name in SIZING_FIELDS:
        assert stored[name] == real[name], name


def test_append_repairs_fields_left_unset_by_older_mef3io(tmp_path):
    """A segment written by mef3io <= 1.1.2 carries 0s. Appending must not
    propagate them: the result has to cover every block, old ones included."""
    path = str(tmp_path / "s.mefd")
    x = _write(path, gap_us=0)
    tmet = _segments(path)[0]
    before = _real_stats(tmet)

    _patch_section2(
        tmet,
        maximum_difference_bytes=0,
        maximum_contiguous_block_bytes=0,
        maximum_contiguous_blocks=before["number_of_blocks"],
    )
    assert _read_section2(tmet)["maximum_difference_bytes"] == 0

    w = mef3io.Writer(path)
    w.write_int32("ch1", x, 0.5, START + int(3 * 4000 / FS * 1e6), FS)
    w.close()

    stored = _read_section2(tmet)
    real = _real_stats(tmet)
    # The old blocks' difference_bytes are only in .tdat, so the append bounds
    # them by meflib's worst case instead of re-reading: never below the truth.
    assert stored["maximum_difference_bytes"] >= real["maximum_difference_bytes"]
    assert stored["maximum_difference_bytes"] <= 5 * real["maximum_block_samples"]
    # Everything the index describes is repaired exactly.
    assert stored["maximum_contiguous_block_bytes"] == real["maximum_contiguous_block_bytes"]
    assert stored["maximum_contiguous_blocks"] == real["maximum_contiguous_blocks"]
    assert stored["maximum_contiguous_samples"] == real["maximum_contiguous_samples"]


def test_append_bounds_no_entry_difference_bytes(tmp_path):
    """A foreign writer may leave NO_ENTRY; that must not survive an append
    either, or a reader sizing from it tries to allocate 4 GB."""
    path = str(tmp_path / "s.mefd")
    x = _write(path, gap_us=0)
    tmet = _segments(path)[0]
    _patch_section2(tmet, maximum_difference_bytes=UI4_NO_ENTRY)

    w = mef3io.Writer(path)
    w.write_int32("ch1", x, 0.5, START + int(3 * 4000 / FS * 1e6), FS)
    w.close()

    stored = _read_section2(tmet)
    real = _real_stats(tmet)
    assert stored["maximum_difference_bytes"] != UI4_NO_ENTRY
    assert stored["maximum_difference_bytes"] >= real["maximum_difference_bytes"]


# --- reading stays independent of the fields ---------------------------------


@pytest.mark.parametrize(
    "patch",
    [
        pytest.param(
            dict(
                maximum_difference_bytes=0,
                maximum_contiguous_block_bytes=0,
                maximum_contiguous_blocks=0,
                maximum_contiguous_samples=0,
                maximum_block_bytes=0,
                maximum_block_samples=0,
            ),
            id="zeros-as-written-by-1.1.2",
        ),
        pytest.param(
            dict(
                maximum_difference_bytes=UI4_NO_ENTRY,
                maximum_block_samples=UI4_NO_ENTRY,
                maximum_contiguous_block_bytes=SI8_NO_ENTRY,
                maximum_contiguous_blocks=SI8_NO_ENTRY,
                maximum_contiguous_samples=SI8_NO_ENTRY,
                maximum_block_bytes=SI8_NO_ENTRY,
            ),
            id="no-entry-sentinels",
        ),
        pytest.param(
            dict(maximum_difference_bytes=1, maximum_contiguous_samples=1),
            id="absurdly-under-declared",
        ),
    ],
)
def test_reader_ignores_section2_sizing_fields(tmp_path, patch):
    """Backwards compatibility: mef3io sizes from the block headers themselves,
    so every session it could read before it still reads, byte for byte."""
    path = str(tmp_path / "s.mefd")
    _write(path, gap_us=int(5e6))

    with mef3io.Reader(path) as r:
        expected = r.read("ch1")
        expected_raw = r.read_raw("ch1")

    for tmet in _segments(path):
        _patch_section2(tmet, **patch)

    with mef3io.Reader(path) as r:
        got = r.read("ch1")
        got_raw = r.read_raw("ch1")
    np.testing.assert_array_equal(
        np.nan_to_num(got, nan=-12345), np.nan_to_num(expected, nan=-12345)
    )
    np.testing.assert_array_equal(got_raw["samples"], expected_raw["samples"])
    np.testing.assert_array_equal(got_raw["valid"], expected_raw["valid"])
