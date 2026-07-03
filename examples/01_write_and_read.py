"""Basic writing and reading.

Float data is quantized to int32 counts with a conversion factor of
10**-precision. When precision is omitted, it is inferred from the data.
NaN runs are not stored: they become discontinuity gaps and come back as NaN.
"""
import tempfile
from pathlib import Path

import numpy as np

import mef3io

FS = 256.0                      # Hz
START = 1_577_836_800_000_000   # uUTC = microseconds since epoch (2020-01-01)

session = str(Path(tempfile.mkdtemp()) / "example.mefd")

# --- write ---------------------------------------------------------------
data = 40 * np.sin(np.arange(5000) / 20)
data[1000:1250] = np.nan        # a ~1 s dropout: stored as a gap, not as data

with mef3io.Writer(session, overwrite=True, units="uV") as w:
    summary = w.write("ch1", data, START, FS)          # precision inferred
    w.write("ch2", data * 0.5, START, FS, precision=3)  # ufact = 10**-3
print("write summary:", summary)   # samples_written excludes the NaN gap

# --- read ----------------------------------------------------------------
with mef3io.Reader(session) as r:
    print("channels:", r.channels)
    info = r.info("ch1")
    print("fs:", info["sampling_frequency"], "ufact:", info["units_conversion_factor"])

    # Whole channel on a uniform grid; the dropout comes back as NaN.
    y = r.read("ch1")
    assert len(y) == 5000 and np.isnan(y[1000:1250]).all()

    # Windowed read over [t0, t1) in uUTC — reads only the needed blocks.
    t0 = START + 2_000_000
    y_win = r.read("ch1", t0, t0 + 1_000_000)
    assert len(y_win) == int(FS)

    # read_raw gives the stored int32 counts plus a validity mask.
    raw = r.read_raw("ch1")
    counts, valid = raw["samples"], raw["valid"].astype(bool)
    assert not valid[1000:1250].any()
    assert np.allclose(counts[valid] * raw["units_conversion_factor"], y[valid])

print("read back", len(y), "samples; gap preserved as NaN")
