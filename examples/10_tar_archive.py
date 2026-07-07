"""Tar session archives: one file instead of a directory tree.

archive_session packs a .mefd directory into a single uncompressed tar
(name.mefd.tar). The Reader opens the archive in place — no extraction — and
windowed reads still fetch only the byte ranges they need. Handy for sharing
a recording as one file and for archival (a single file cannot lose parts of
its tree). Tar sessions are read-only: the Writer refuses .tar paths;
extract_session (or any tar tool) restores the writable directory.
"""
import tarfile
import tempfile
from pathlib import Path

import numpy as np

import mef3io

FS = 256.0
START = 1_577_836_800_000_000

root = Path(tempfile.mkdtemp())
session = str(root / "demo.mefd")

x = np.sin(np.arange(20_000) / 20)
x[5000:5100] = np.nan  # a gap
with mef3io.Writer(session, overwrite=True, units="uV") as w:
    w.write("ch1", x, START, FS, precision=3)
    w.write_annotations([{"time": START + 1_000_000, "text": "marker"}])

tar = mef3io.archive_session(session)  # -> demo.mefd.tar next to the dir
print("archive:", tar, f"({Path(tar).stat().st_size} bytes)")

# Read the archive exactly like the directory — same API, same values.
with mef3io.Reader(session) as rd, mef3io.Reader(tar) as rt:
    assert rd.channels == rt.channels
    assert np.array_equal(rd.read("ch1"), rt.read("ch1"), equal_nan=True)
    assert rd.records() == rt.records()
    print("tar read identical; windowed read:",
          len(rt.read("ch1", START + 10_000_000, START + 11_000_000)), "samples")
    print("segment lives at:", rt.segments("ch1")[0]["path"])

# The Writer refuses archives (tar sessions are read-only).
try:
    mef3io.Writer(tar)
except RuntimeError as e:
    print("writer refused:", e)

# extract_session is the exact inverse: the restored dir is writable again.
restored = mef3io.extract_session(tar, root / "restored.mefd")
with mef3io.Writer(restored) as w:
    w.write("ch1", np.cos(np.arange(1000) / 20), START + int(20_000 / FS * 1e6) + 60_000_000, FS)
print("restored + appended:", mef3io.Reader(restored).info("ch1")["number_of_samples"], "samples")

# It is also a plain ustar file — standard tooling reads it fine.
with tarfile.open(tar) as tf:
    print("tarfile sees:", len(tf.getnames()), "members")
