#!/usr/bin/env python3
"""Generate the CyberPSG / meflib compatibility check set.

Nine small sessions that between them exercise every way mef3io can produce or
touch a MEF 3 session, plus two deliberately broken controls. They exist because
the defect this project spent a release cycle on is **invisible to mef3io and to
pymef** — both size their buffers from each block's own header rather than from
metadata section 2 — so only a meflib-based reader can confirm the fix.

    python scripts/make_cyberpsg_check.py                 # ~/mef3io_cyberpsg_check
    python scripts/make_cyberpsg_check.py --out /tmp/x --seconds 120

Every session is verified through both mef3io and pymef before the script
exits, and the parameters used are written into the README next to the files so
a future run can be compared against an earlier one.
"""
from __future__ import annotations

import argparse
import shutil
import struct
import sys
import warnings
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "python"))
warnings.filterwarnings("ignore")

import mef3io  # noqa: E402
from mef3io import _mef3io as m  # noqa: E402

# --- the parameters, in one place ------------------------------------------ #
# Chosen to match the shape of the original report: a ~50 s record with a 3.0 s
# discontinuity a little under half way through. Small enough to open instantly,
# long enough that a drawn trace is recognisable and the gap is obvious.
FS = 256.0                       # Hz
CHANNELS = ["Fp1", "Fp2", "C3", "C4"]
SECONDS = 50.0                   # per session, before any append
GAP_SECONDS = 3.0                # discontinuity length
GAP_AT = 0.47                    # fraction of the record where the gap starts
APPEND_SECONDS = 10.0            # extra data for the appended sessions (02, 08)
UFACT = 0.1                      # 0.1 uV per bit
START = 1577836800000000         # 2020-01-01T00:00:00Z, uUTC
LEGACY_PRECISION = 1             # mef_tools `precision` => 0.1 uV/bit, same as UFACT
PASSWORD_1, PASSWORD_2 = "level1", "level2"
TMET_PADDING = b"\x7e" * 32      # foreign trailing padding for session 08
DROP_ENTRIES = 2                 # index entries discarded to simulate a crash (09)

# EEG-shaped rather than noise, so a drawn trace looks like a recording and the
# RED encoder sees realistic entropy (~1.5-2.5 bytes/sample).
ALPHA_UV, ALPHA_HZ = 40.0, 10.0
THETA_UV, THETA_HZ = 18.0, 6.0
BETA_UV, BETA_HZ = 9.0, 21.0
NOISE_UV = 7.0

UH, METADATA_FILE_BYTES, S2 = 1024, 16384, 2560
TSI = 56
S2_FIELDS = {
    "number_of_samples": (6360, "<q"),
    "number_of_blocks": (6368, "<q"),
    "maximum_difference_bytes": (6388, "<I"),
    "number_of_discontinuities": (6400, "<q"),
    "maximum_contiguous_blocks": (6408, "<q"),
    "maximum_contiguous_block_bytes": (6416, "<q"),
    "maximum_contiguous_samples": (6424, "<q"),
}


def eeg(n: int, seed: int) -> np.ndarray:
    rng = np.random.default_rng(seed)
    t = np.arange(n) / FS
    sig = (
        ALPHA_UV * np.sin(2 * np.pi * ALPHA_HZ * t)
        + THETA_UV * np.sin(2 * np.pi * THETA_HZ * t + 1.1)
        + BETA_UV * np.sin(2 * np.pi * BETA_HZ * t + 0.4)
        + rng.normal(0, NOISE_UV, n)
    )
    return np.round(sig / UFACT).astype(np.int32)


def halves(seconds: float) -> tuple[int, int]:
    """Sample counts either side of the gap."""
    n1 = int(seconds * GAP_AT * FS)
    n2 = int(seconds * (1 - GAP_AT) * FS) - int(GAP_SECONDS * FS)
    return n1, n2


def write_gapped(path: Path, seconds: float, **writer_kw) -> tuple[int, int]:
    n1, n2 = halves(seconds)
    w = mef3io.Writer(str(path), **writer_kw)
    for i, ch in enumerate(CHANNELS):
        w.write_int32(ch, eeg(n1, i), UFACT, START, FS)
        w.write_int32(ch, eeg(n2, 100 + i), UFACT,
                      START + int((n1 / FS + GAP_SECONDS) * 1e6), FS)
    w.close()
    return n1, n2


def append_to(path: Path, after_samples: int, seed_base: int) -> None:
    w = mef3io.Writer(str(path))            # REOPENED: the in-segment append path
    for i, ch in enumerate(CHANNELS):
        w.write_int32(ch, eeg(int(APPEND_SECONDS * FS), seed_base + i), UFACT,
                      START + int((after_samples / FS + GAP_SECONDS) * 1e6), FS)
    w.close()


def fix_tmet_crcs(tmet: Path) -> None:
    raw = bytearray(tmet.read_bytes())
    struct.pack_into("<I", raw, 4, m.crc32(bytes(raw[UH:METADATA_FILE_BYTES])))
    struct.pack_into("<I", raw, 0, m.crc32(bytes(raw[4:UH])))
    tmet.write_bytes(bytes(raw))


def break_like_1_1_2(path: Path) -> None:
    """Exactly what mef3io <= 1.1.2 left behind."""
    for tmet in sorted(path.rglob("*.tmet")):
        raw = bytearray(tmet.read_bytes())
        blocks, = struct.unpack_from("<q", raw, S2 + S2_FIELDS["number_of_blocks"][0])
        samples, = struct.unpack_from("<q", raw, S2 + S2_FIELDS["number_of_samples"][0])
        struct.pack_into("<I", raw, S2 + S2_FIELDS["maximum_difference_bytes"][0], 0)
        struct.pack_into("<q", raw, S2 + S2_FIELDS["maximum_contiguous_block_bytes"][0], 0)
        # ...and the channel-total over-declaration for the other two.
        struct.pack_into("<q", raw, S2 + S2_FIELDS["maximum_contiguous_blocks"][0], blocks)
        struct.pack_into("<q", raw, S2 + S2_FIELDS["maximum_contiguous_samples"][0], samples)
        tmet.write_bytes(bytes(raw))
        fix_tmet_crcs(tmet)


def truncate_index(path: Path, drop: int) -> None:
    """Simulate a crash where blocks reached .tdat but the index was not extended."""
    for tidx in sorted(path.rglob("*.tidx")):
        entries = (tidx.stat().st_size - UH) // TSI
        keep = entries - drop
        raw = bytearray(tidx.read_bytes()[: UH + keep * TSI])
        struct.pack_into("<q", raw, 32, keep)          # universal header count
        struct.pack_into("<I", raw, 4, m.crc32(bytes(raw[UH:])))
        struct.pack_into("<I", raw, 0, m.crc32(bytes(raw[4:UH])))
        tidx.write_bytes(bytes(raw))


def declared(path: Path) -> dict:
    tmet = sorted(path.rglob("*.tmet"))[0]
    raw = tmet.read_bytes()
    return {n: struct.unpack_from(f, raw, S2 + o)[0] for n, (o, f) in S2_FIELDS.items()}


# --------------------------------------------------------------------------- #
def build(out: Path, seconds: float) -> list[tuple[str, str, str]]:
    from mef_tools.io import MefWriter

    notes: list[tuple[str, str, str]] = []
    n1, n2 = halves(seconds)

    p = out / "01_mef3io_current.mefd"
    write_gapped(p, seconds)
    notes.append((p.name, "MUST WORK",
                  "Written by the CURRENT build with default durability. The reference for "
                  "what every other session should look like."))

    p = out / "02_mef3io_appended.mefd"
    write_gapped(p, seconds)
    append_to(p, n1 + n2, 200)
    notes.append((p.name, "MUST WORK",
                  f"01, then extended by a REOPENED Writer — the in-segment append path, which "
                  f"rewrites the declarations in place. {APPEND_SECONDS:g} s longer, same gap, "
                  f"continuous across the join."))

    p = out / "03_written_by_mef_tools.mefd"
    mw = MefWriter(str(p), overwrite=True)
    for i, ch in enumerate(CHANNELS):
        mw.write_data(eeg(n1, 300 + i), ch, START, FS, precision=LEGACY_PRECISION)
        mw.write_data(eeg(n2, 350 + i), ch, START + int((n1 / FS + GAP_SECONDS) * 1e6), FS,
                      precision=LEGACY_PRECISION)
    del mw
    notes.append((p.name, "MAY ALSO FAIL",
                  "Written by the LEGACY mef_tools/pymef stack, untouched — same shape as 01, so "
                  "the traces are directly comparable. It declares number_of_discontinuities = 0 "
                  "while the file really has 2, and maximum_contiguous_samples = 0. "
                  "meflib.c:3548 mallocs that many entries then writes one per FLAGGED block, so "
                  "0 allocated against 2 written is a heap overflow — a SECOND crash mechanism, "
                  "established by reading the C source but never reproduced. 04 is the same data "
                  "with it fixed."))

    p4 = out / "04_mef_tools_repaired_by_mef3io.mefd"
    shutil.copytree(out / "03_written_by_mef_tools.mefd", p4)
    ids4 = mef3io.Validator(str(p4)).validate().repairable_check_ids
    if ids4:
        mef3io.repair_session(str(p4), ids4, backup=False)
    notes.append((p4.name, "MUST WORK",
                  f"03 after repair_session (fixed: {', '.join(ids4) or 'nothing'}). Identical "
                  f"samples to 03, declarations corrected. The upgrade path for data already in "
                  f"the field."))

    p5 = out / "05_NEGATIVE_CONTROL_broken_like_1_1_2.mefd"
    write_gapped(p5, seconds)
    break_like_1_1_2(p5)
    notes.append((p5.name, "EXPECTED TO FAIL",
                  "DELIBERATELY BROKEN — maximum_difference_bytes and "
                  "maximum_contiguous_block_bytes set to 0, contiguous maxima set to channel "
                  "totals: exactly what mef3io <= 1.1.2 wrote. Expect the ORIGINAL FAILURE, an "
                  "access violation inside RED_decode when a trace is drawn."))

    p6 = out / "06_that_same_session_repaired.mefd"
    shutil.copytree(p5, p6)
    ids6 = mef3io.Validator(str(p6)).validate().repairable_check_ids
    mef3io.repair_session(str(p6), ids6, backup=False)
    notes.append((p6.name, "MUST WORK",
                  f"05 after repair_session (fixed: {', '.join(ids6)}). Byte-for-byte the SAME "
                  f"SAMPLES as 05 — only declarations differ. 05 vs 06 is the pair that isolates "
                  f"the cause to metadata section 2."))

    p7 = out / "07_mef3io_encrypted.mefd"
    write_gapped(p7, seconds, password1=PASSWORD_1, password2=PASSWORD_2)
    notes.append((p7.name, "MUST WORK",
                  f"Encrypted. Level-1 password '{PASSWORD_1}', level-2 '{PASSWORD_2}'."))

    p8 = out / "08_padded_tmet_then_appended.mefd"
    write_gapped(p8, seconds)
    for tmet in sorted(p8.rglob("*.tmet")):
        with open(tmet, "ab") as fh:
            fh.write(TMET_PADDING)
    append_to(p8, n1 + n2, 500)
    notes.append((p8.name, "MUST WORK",
                  f"A .tmet carrying {len(TMET_PADDING)} bytes of trailing padding — which "
                  f"foreign writers really do leave — then appended. Until this release the "
                  f"append hashed the body CRC past the end of the fixed-length record and the "
                  f"WHOLE SESSION became unreadable. This file is the fix."))

    p9 = out / "09_recovered_after_interrupted_write.mefd"
    write_gapped(p9, seconds, durability="fast")
    truncate_index(p9, DROP_ENTRIES)
    rec = mef3io.recover_session(str(p9), apply=True, backup=False)
    ids9 = mef3io.Validator(str(p9)).validate().repairable_check_ids
    if ids9:
        mef3io.repair_session(str(p9), ids9, backup=False)
    recovered = sum(s.blocks_recovered for s in rec.segments)
    notes.append((p9.name, "MUST WORK",
                  f"Written with durability='fast', then an interrupted append simulated (blocks "
                  f"reached .tdat, the index was not extended), then recover_session rebuilt the "
                  f"missing entries from the RED block headers ({recovered} blocks recovered). "
                  f"Tests that recovery output is readable by the C reader."))
    shutil.rmtree(str(p9) + ".recover-backup", ignore_errors=True)
    return notes


def verify(out: Path, notes) -> list[tuple]:
    """Read every session through BOTH stacks before handing it over."""
    from pymef.mef_session import MefSession

    rows = []
    for name, _, _ in notes:
        path = out / name
        pw = PASSWORD_2 if "encrypted" in name else ""
        kw = {"password": pw} if pw else {}
        with mef3io.Reader(str(path), **kw) as r:
            channels = r.channels
            data = r.read(channels[0])
            gap = int(np.sum(~np.isfinite(data)))
        session = MefSession(str(path), pw)
        try:
            session.read_ts_channel_basic_info()
        finally:
            session.close()
        ok = mef3io.Validator(str(path), **kw).validate().ok
        rows.append((name, len(channels), len(data), gap, ok))
    return rows


def write_readme(out: Path, notes, rows, seconds: float) -> None:
    # Read the 03/04 divergence off the files rather than hard-coding it, so the
    # README stays true for any --seconds.
    before = declared(out / "03_written_by_mef_tools.mefd")
    after = declared(out / "04_mef_tools_repaired_by_mef3io.mefd")
    L = ["# CyberPSG / meflib compatibility check", "",
         f"Generated by `scripts/make_cyberpsg_check.py` from mef3io "
         f"{mef3io.__version__}.", "",
         "Open each session and **draw the traces**. The original failure let",
         "`ReadSession` succeed and blew up later inside `RED_decode`, so \"it opens\" is",
         "not evidence.", "",
         "## Parameters", "",
         "Everything here is reproducible — rerun the script to get the same files.", "",
         "| parameter | value |", "|---|---|",
         f"| sampling rate | {FS:g} Hz |",
         f"| channels | {', '.join(CHANNELS)} |",
         f"| duration | {seconds:g} s (+{APPEND_SECONDS:g} s for the appended sessions) |",
         f"| discontinuity | {GAP_SECONDS:g} s starting at {GAP_AT:.0%} of the record |",
         f"| conversion factor | {UFACT} uV/bit (mef_tools `precision={LEGACY_PRECISION}`) |",
         f"| start time | {START} uUTC (2020-01-01T00:00:00Z) |",
         f"| signal | {ALPHA_UV:g} uV @ {ALPHA_HZ:g} Hz + {THETA_UV:g} uV @ {THETA_HZ:g} Hz "
         f"+ {BETA_UV:g} uV @ {BETA_HZ:g} Hz + N(0, {NOISE_UV:g}) uV |",
         f"| passwords (07) | level 1 `{PASSWORD_1}`, level 2 `{PASSWORD_2}` |",
         f"| `.tmet` padding (08) | {len(TMET_PADDING)} bytes of 0x7e |",
         f"| index entries dropped (09) | {DROP_ENTRIES} per segment |",
         "", "## The sessions", "", "| # | session | expectation |", "|---|---|---|"]
    for name, verdict, text in notes:
        L.append(f"| {name.split('_')[0]} | `{name}` | **{verdict}** — {text} |")

    L += ["", "## Verified here before handing over", "",
          "Every session was read through **both** mef3io and pymef, and validated.", "",
          "| session | ch | samples | duration | gap | mef3io validator |",
          "|---|---|---|---|---|---|"]
    for name, nch, n, gap, ok in rows:
        L.append(f"| `{name}` | {nch} | {n} | {n / FS:.1f} s | {gap / FS:.1f} s | "
                 f"{'clean' if ok else '**reports defects** (expected)'} |")

    L += ["", "## Two crash mechanisms, not one", "",
          "The original report is mechanism **(a)**: `maximum_difference_bytes = 0` makes",
          "`RED_allocate_processing_struct` skip the allocation entirely, leaving a NULL",
          "buffer for `RED_decode` to write through. That is **05**, and it has a",
          "post-mortem behind it.", "",
          "Mechanism **(b)** has only ever been established by reading the C source:",
          "`find_discontinuity_indices` (`meflib.c:3548`) mallocs",
          "`number_of_discontinuities` entries and then writes one per **flagged** block.",
          "The legacy stack writes `0` there while still setting the flags, so a file with",
          "gaps overflows that buffer. That is **03**.", "",
          "If 03 misbehaves, (b) is real and this is its first reproduction. If it does",
          "not, (b) may be latent in that build — say so either way.", "",
          "## What each pair proves", "",
          "- **05 vs 06** — same samples, same blocks, only the declarations differ. 05",
          "  failing and 06 working isolates the cause to metadata section 2.",
          "- **03 vs 04** — the legacy stack's own output before and after",
          f"  `repair_session`, with identical samples. The declarations that move:",
          f"  `number_of_discontinuities` {before['number_of_discontinuities']} → "
          f"{after['number_of_discontinuities']}, `maximum_contiguous_samples` "
          f"{before['maximum_contiguous_samples']} → {after['maximum_contiguous_samples']},",
          f"  `maximum_contiguous_block_bytes` {before['maximum_contiguous_block_bytes']} → "
          f"{after['maximum_contiguous_block_bytes']} (it had declared the whole `.tdat`",
          "  body, not the longest run).",
          "- **01 vs 02 vs 08** — a fresh write, one through the append path, and one whose",
          "  `.tmet` carried foreign padding across an append.",
          "- **09** — a session rebuilt by `recover_session` after an interrupted write.", "",
          "## If 05 does NOT fail", "",
          "Then the crash has a cause other than these declarations, and the rest proves",
          "less than it appears. Please say so rather than treating the matrix as a pass.",
          ""]
    (out / "README.md").write_text("\n".join(L))


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--out", default=str(Path.home() / "mef3io_cyberpsg_check"))
    p.add_argument("--seconds", type=float, default=SECONDS,
                   help="record length per session, before any append")
    cfg = p.parse_args(argv)

    out = Path(cfg.out)
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)

    notes = build(out, cfg.seconds)
    rows = verify(out, notes)
    write_readme(out, notes, rows, cfg.seconds)

    for name, nch, n, gap, ok in rows:
        print(f"  {name:48} ch={nch} n={n:6d} {n / FS:5.1f}s gap={gap / FS:.1f}s "
              f"valid={ok}")
    print(f"\n{len(notes)} sessions in {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
