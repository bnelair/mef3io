"""Two-level encryption.

MEF 3.0 uses two passwords: level 1 (technical metadata) and level 2 (full
access incl. subject metadata and records). Sessions are either unencrypted
or encrypted with BOTH levels — the level-2 password also unlocks level 1.
Signal (RED) data blocks themselves are not encrypted (meflib default); the
passwords gate metadata and record bodies.
"""
import tempfile
from pathlib import Path

import numpy as np

import mef3io

FS = 256.0
START = 1_577_836_800_000_000
L1, L2 = "tech-password", "research-password"

session = str(Path(tempfile.mkdtemp()) / "secret.mefd")

with mef3io.Writer(session, overwrite=True, password1=L1, password2=L2) as w:
    w.write("ch1", np.sin(np.arange(5000) / 20), START, FS, precision=3)
    w.write_annotations([{"time": START + 1_000_000, "text": "confidential note"}], "ch1")

# Level-2 password: everything, including records.
with mef3io.Reader(session, L2) as r:
    y = r.read("ch1")
    notes = r.records("ch1")
print("L2 access:", len(y), "samples;", notes[0]["text"])

# Level-1 password: signal + technical metadata, but not level-2 content.
with mef3io.Reader(session, L1) as r:
    y1 = r.read("ch1")
    assert np.array_equal(np.isnan(y), np.isnan(y1)) and np.allclose(y[~np.isnan(y)], y1[~np.isnan(y1)])
print("L1 access: signal reads identically")

# Wrong/missing password fails fast.
try:
    mef3io.Reader(session, "wrong")
except RuntimeError as e:
    print("wrong password rejected:", type(e).__name__)
