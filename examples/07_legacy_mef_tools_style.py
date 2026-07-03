"""Using mef3io as a mef_tools drop-in.

Existing mef_tools code keeps working with just the import changed:

    # from mef_tools.io import MefReader, MefWriter
    from mef3io import MefReader, MefWriter

Same semantics as the legacy writer: NaN runs split the data into
discontinuity gaps, precision (and the conversion factor) is inferred when
not given, integer arrays take the int32 primitive path, and appends extend
the channel's last segment in place.
"""
import tempfile
from pathlib import Path

import numpy as np

from mef3io import MefReader, MefWriter

FS = 500.0
START = 1_577_836_800_000_000

session = str(Path(tempfile.mkdtemp()) / "legacy_style.mefd")

# --- write, mef_tools style ------------------------------------------------
writer = MefWriter(session, overwrite=True)
writer.data_units = "mV"
writer.mef_block_len = 1000

data = np.random.randn(10000) * 2.5
data[4000:4400] = np.nan                       # gap, skipped on write
writer.write_data(data, "ch1", START, FS)      # precision inferred

# append (extends the same segment, like legacy)
t2 = START + int(10000 / FS * 1e6)
writer.write_data(np.random.randn(10000), "ch1", t2, FS)

# integer data -> int32 primitive path, ufact = 10**-precision
counts = np.arange(5000, dtype=np.int32)
writer.write_data(counts, "ch2", START, FS, precision=2)

writer.write_annotations([{"time": START + 500_000, "type": "Note", "text": "hello"}], "ch1")
writer.close()

# --- read, mef_tools style ---------------------------------------------------
reader = MefReader(session)
print("channels:", reader.channels)
print("fsamp:", reader.get_property("fsamp", "ch1"),
      "ufact:", reader.get_property("ufact", "ch1"),
      "unit:", reader.get_property("unit", "ch1"))

y = np.asarray(reader.get_data("ch1"), float)
assert np.isnan(y[4000:4400]).all()            # the gap came back as NaN
assert len(y) == 20000                         # the append is in the same channel

y2 = np.asarray(reader.get_data("ch2"), float)
assert np.allclose(y2, counts * 0.01)

t_from, t_to = START + 1_000_000, START + 3_000_000
window = reader.get_data("ch1", t_from, t_to)  # windowed read, legacy call shape
print("window:", np.asarray(window).shape)

print("annotations:", reader.get_annotations("ch1"))
reader.close()
print("legacy-style round trip complete")
