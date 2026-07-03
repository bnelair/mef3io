"""The int32 primitive path.

Acquisition systems usually produce integer counts plus a calibration factor
(e.g. an amplifier's volts-per-bit). write_int32 stores those counts verbatim
— no quantization, bit-exact round trip — with the factor kept in metadata.
A validity mask marks gap samples without needing a float NaN pass.
"""
import tempfile
from pathlib import Path

import numpy as np

import mef3io

FS = 1024.0
START = 1_577_836_800_000_000
UFACT = 0.042           # physical units per count (from the amplifier)

session = str(Path(tempfile.mkdtemp()) / "counts.mefd")

counts = (1000 * np.sin(np.arange(8000) / 50)).astype(np.int32)
valid = np.ones(len(counts), dtype=bool)
valid[3000:3500] = False      # acquisition dropout — stored as a gap

with mef3io.Writer(session, overwrite=True) as w:
    summary = w.write_int32("ch1", counts, UFACT, START, FS, valid=valid)
print("blocks:", summary["blocks"], "gaps skipped:", summary["gaps_skipped"])

with mef3io.Reader(session) as r:
    raw = r.read_raw("ch1")
    got, got_valid = raw["samples"], raw["valid"].astype(bool)
    # bit-exact where valid
    assert np.array_equal(got[got_valid], counts[valid])
    assert not got_valid[3000:3500].any()

    # scaled read applies the conversion factor and fills the gap with NaN
    y = r.read("ch1")
    assert np.allclose(y[:3000], counts[:3000] * UFACT)
    assert np.isnan(y[3000:3500]).all()

print("int32 counts round-tripped bit-exact; ufact =", raw["units_conversion_factor"])
