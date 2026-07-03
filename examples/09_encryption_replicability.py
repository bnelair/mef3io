"""Encryption replicability: legacy vs new, plain vs encrypted.

Writes the SAME data four ways — {legacy mef_tools, new mef3io} writer ×
{no encryption, two-level encryption} — reads every session with BOTH
readers, and checks three things:

  1. reader agreement — on any given session the legacy and the new reader
     return BIT-IDENTICAL arrays;
  2. encryption replicability — for each writer, the encrypted session decodes
     bit-identical to its unencrypted twin (encryption only wraps metadata; it
     must never touch sample values);
  3. writer agreement — the two writers' outputs match within a couple of
     quantization counts. They are NOT bit-identical by design: for
     precision=3 legacy `convert_data_to_int32` does `np.round(x, 3)` and then
     a truncating int32 cast of `1000 * rounded` (up to ~2 counts off the
     exact value on boundary samples), while mef3io stores `round(x * 1000)`.
     Both are valid MEF; each round-trips its own quantization exactly.

The second part probes what each credential unlocks on the encrypted
sessions ("does L1 encryption work?"):

  - level-2 password: everything, with either reader;
  - level-1 password: the signal reads fine, but level-2 content differs by
    writer: the NEW writer L2-encrypts annotation record bodies, so with L1
    the new reader reports them unavailable (and the legacy reader chokes on
    the ciphertext). The LEGACY (PyPI mef_tools 1.2.3) writer stores record
    bodies UNENCRYPTED, so its annotations are readable with a mere L1
    password — legacy "encrypted" annotations are not actually protected;
  - the legacy reader with L1 misreads the SIGNAL on either writer's
    sessions: pymef applies the recording-time offset from section 3, which
    L1 cannot decrypt, so it grids the data against garbage times. The new
    reader detects the inaccessible section 3 and falls back cleanly — with
    L1 it reads the signal correctly from both writers' files;
  - wrong or missing password: rejected at open by both readers.

Requires the legacy stack: pip install mef3io[test]  (mef-tools + pymef).
"""
import sys
import tempfile
from pathlib import Path

import numpy as np
import pandas as pd

from mef_tools.io import MefReader as MefReaderLegacy
from mef_tools.io import MefWriter as MefWriterLegacy

from mef3io import MefReader, MefWriter

# The legacy reader's __del__ raises when construction failed (no session
# attribute); silence those unraisable-exception reports for readable output.
sys.unraisablehook = lambda unraisable: None

FS = 256.0
START = 1_577_836_800_000_000
PWD1, PWD2 = "pwd1", "pwd2"
CHANNELS = ["ch1", "ch2"]
PRECISION = 3
UFACT = 10.0 ** -PRECISION

root = Path(tempfile.mkdtemp())
print("sessions under:", root)

# --- the one signal everybody writes --------------------------------------
rng = np.random.default_rng(42)
x = rng.normal(0, 40, (len(CHANNELS), int(FS) * 60))
x[0, 1000:2000] = np.nan                       # a gap in ch1

annotations = pd.DataFrame([
    {"time": START + 1_000_000, "type": "Note", "text": "probe"},
])


def write_session(writer_cls, path, encrypted):
    p1, p2 = (PWD1, PWD2) if encrypted else (None, None)
    w = writer_cls(str(path), overwrite=True, password1=p1, password2=p2)
    w.data_units = "uV"
    w.max_nans_written = 0                     # split on NaN in both writers
    for i, ch in enumerate(CHANNELS):
        w.write_data(x[i].copy(), ch, START, FS, precision=PRECISION)
    w.write_annotations(annotations.copy(), channel="ch1")
    if hasattr(w, "close"):                    # the legacy writer has no close()
        w.close()


def read_data(reader_cls, path, password):
    r = reader_cls(str(path), password2=password)
    data = np.array(r.get_data(CHANNELS), dtype=float)
    r.close()
    return data


WRITERS = {"legacy": MefWriterLegacy, "new": MefWriter}
READERS = {"legacy": MefReaderLegacy, "new": MefReader}

sessions = {}
for wname, wcls in WRITERS.items():
    for encrypted in (False, True):
        path = root / f"{wname}_{'enc' if encrypted else 'plain'}.mefd"
        write_session(wcls, path, encrypted)
        sessions[(wname, encrypted)] = path

# --- part 1: reader agreement, encryption replicability, writer agreement --
print("\n" + "X" * 20)
print("Reader agreement (per session, legacy vs new reader, bit-exact)")
all_ok = True
decoded = {}   # (writer, encrypted) -> array (readers agree, so keep one)
for (wname, encrypted), path in sessions.items():
    pwd = PWD2 if encrypted else None
    reads = {rname: read_data(rcls, path, pwd) for rname, rcls in READERS.items()}
    identical = np.array_equal(reads["legacy"], reads["new"], equal_nan=True)
    all_ok &= identical
    decoded[(wname, encrypted)] = reads["new"]
    print(f"  {wname:6s} writer / {'encrypted' if encrypted else 'plain    '}: {identical}")

print("\nEncryption replicability (per writer, encrypted vs plain, bit-exact)")
for wname in WRITERS:
    identical = np.array_equal(decoded[(wname, True)], decoded[(wname, False)], equal_nan=True)
    all_ok &= identical
    print(f"  {wname:6s} writer: encrypted == plain: {identical}")

print("\nWriter agreement (legacy vs new quantization, on the plain sessions)")
leg, new = decoded[("legacy", False)], decoded[("new", False)]
nan_ok = np.array_equal(np.isnan(leg), np.isnan(new)) and np.array_equal(
    np.isnan(new), np.isnan(x))
both = ~np.isnan(x)
max_dev = np.abs(leg[both] - new[both]).max()
writer_ok = nan_ok and max_dev <= 2 * UFACT + 1e-12
all_ok &= writer_ok
print(f"  NaN gaps identical: {nan_ok}; max value deviation: {max_dev:.6f}"
      f" (<= 2 quantization counts: {writer_ok})")
print("\nmatrix:", "ALL PASS" if all_ok else "FAILURES PRESENT")

# --- part 2: what does each credential unlock on the encrypted sessions? ---
def probe(reader_cls, path, password, baseline):
    """Try data + annotations with one credential; report, never raise."""
    try:
        r = reader_cls(str(path), password2=password)
    except Exception as e:
        return f"REJECTED at open ({type(e).__name__})"
    try:
        got = np.array(r.get_data(CHANNELS), dtype=float)
        data_msg = ("data OK" if got.shape == baseline.shape
                    and np.array_equal(got, baseline, equal_nan=True) else "data WRONG")
    except Exception as e:
        data_msg = f"data rejected ({type(e).__name__})"
    try:
        anns = r.get_annotations("ch1")
        ann_msg = ("annotations OK" if anns and any(a.get("text") == "probe" for a in anns)
                   else "annotations UNAVAILABLE")
    except Exception as e:
        ann_msg = f"annotations rejected ({type(e).__name__})"
    r.close()
    return f"{data_msg}; {ann_msg}"


print("\n" + "X" * 20)
print("Access levels on the encrypted sessions")
for wname in WRITERS:
    path = sessions[(wname, True)]
    baseline = decoded[(wname, False)]   # this writer's own plain output
    print(f"\n  session written by the {wname} writer:")
    for rname, rcls in READERS.items():
        for cred, pwd in [("L2 password", PWD2), ("L1 password", PWD1),
                          ("wrong password", "nope"), ("no password", None)]:
            print(f"    {rname:6s} reader, {cred:14s}: {probe(rcls, path, pwd, baseline)}")

print("\ndone;", "replicability holds" if all_ok else "REPLICABILITY BROKEN")
