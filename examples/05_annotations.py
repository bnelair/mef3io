"""Annotations (MEF records).

Records attach either to the whole session or to one channel. Each record has
a type ("Note", "EDFA", "SyLg", "Seiz"), a uUTC time, and typically text;
"EDFA" carries a duration. In encrypted sessions record bodies are encrypted
at level 2. write_annotations always APPENDS to the records already stored at
that level (and creates them the first time); it never replaces or deletes
one. Taking a wrong annotation back is a separate, explicit call,
remove_annotations (shown at the end): it names the record by its content,
rewrites only that level's annotation files, and a removed annotation cannot
be recovered. Readers return the records in time order.
"""
import tempfile
from pathlib import Path

import numpy as np

import mef3io

FS = 256.0
START = 1_577_836_800_000_000

session = str(Path(tempfile.mkdtemp()) / "annotated.mefd")

with mef3io.Writer(session, overwrite=True) as w:
    w.write("ch1", np.sin(np.arange(5000) / 20), START, FS, precision=3)

    # session-level records (channel=None)
    w.write_annotations([
        {"time": START, "text": "recording start"},              # type defaults to Note
        {"time": START + 4_000_000, "type": "SyLg", "text": "impedance check ok"},
    ])
    # channel-level records
    w.write_annotations([
        {"time": START + 1_000_000, "type": "Note", "text": "spike"},
        {"time": START + 2_500_000, "type": "EDFA", "text": "artifact", "duration": 500_000},
    ], channel="ch1")

with mef3io.Reader(session) as r:
    print("session records:")
    for rec in r.records():
        print("  ", rec)
    print("ch1 records:")
    for rec in r.records("ch1"):
        print("  ", rec)
    assert r.records("ch1")[1]["duration"] == 500_000

# The legacy-style API reads the same annotations:
from mef3io import MefReader  # noqa: E402

anns = MefReader(session).get_annotations("ch1")
assert [a["text"] for a in anns] == ["spike", "artifact"]
print("legacy get_annotations sees:", [a["text"] for a in anns])

# Taking a note back: name it by what Reader.records() returned (add= puts the
# corrected one in the same, crash-safe rewrite). There is no undo.
with mef3io.Reader(session) as r:
    wrong = [x for x in r.records("ch1") if x["text"] == "spike"]
report = mef3io.remove_annotations(
    session, remove=wrong, add=[{"time": wrong[0]["time"], "text": "spike (checked)",
                                 "channel": "ch1"}])
print(f"removed {report.removed}, added {report.added}")
with mef3io.Reader(session) as r:
    assert [x["text"] for x in r.records("ch1")] == ["spike (checked)", "artifact"]
