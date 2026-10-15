"""Regenerate tests/golden/meflib_red_encrypted.mefd — RED blocks encrypted BY MEFLIB.

Issue #80: meflib encrypts only the first 16 bytes of a RED block's 256-byte
statistics table (one AES block; meflib.c:292 / :6676 / :7027). mef3io used to
cipher all 256, which round-tripped against itself and so was never caught. The
fixture this script builds is the interop oracle: its blocks are encrypted by
the vendored meflib's own RED_encode, never by mef3io.

pymef cannot produce it — its writer never sets a block encryption level
(RED_ENCRYPTION_LEVEL_DEFAULT = NO_ENCRYPTION, meflib.h:1082) — so:

1. The legacy stack (mef_tools -> pymef -> meflib) writes an encrypted session
   (metadata L1+L2, blocks plain, as always).
2. pymef decodes the int32 samples: the ground truth, saved to the .npz.
3. ``tests/meflib_harness/red_blocks.c``, compiled here against the vendored
   meflib, re-encodes every block of channel ``l1_blocks`` at level 1 and of
   ``l2_blocks`` at level 2 with meflib's RED_encode, in place (sizes are
   unchanged; the harness checks), and re-seals the file CRCs.
4. Verification before anything is kept: meflib itself (harness ``decode``)
   decodes the encrypted blocks back to the ground truth, and every block
   really carries the encryption flag.

pymef CANNOT read the result, and that is pymef's limitation, not the
fixture's: its time-series reader ``read_mef_ts_data`` (behind both
``read_ts_channels_sample`` and ``_uutc``) callocs its RED processing struct
(pymef3_file.c:2151) and never sets ``password_data``, so RED_decode
dereferences NULL at meflib.c:6675 on the first encrypted block (segfault).
meflib — the harness — is therefore the oracle for the encrypted blocks; pymef
supplies the plaintext ground truth before encryption.

Needs a C compiler, the legacy stack (``pip install mef3io[test]``) and the
meflib source (``reference_files/``, gitignored; pass ``--meflib`` otherwise).
The suite never runs this; it reads the committed output.

    python tests/make_meflib_encrypted_fixture.py [--meflib DIR] [--cc CC]
"""
from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import tempfile
import warnings
from pathlib import Path

import numpy as np

warnings.filterwarnings("ignore", message="invalid value encountered in cast")

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
GOLDEN = HERE / "golden"
NAME = "meflib_red_encrypted"
HARNESS = HERE / "meflib_harness" / "red_blocks.c"

PASSWORD1 = "pass1"
PASSWORD2 = "pass2"
FS = 256.0
N_SAMPLES = 7680  # 30 s; mef_tools splits it into several blocks
START_UUTC = 1_577_836_800_000_000  # 2020-01-01
# channel -> RED block encryption level applied by meflib
CHANNELS = {"l1_blocks": 1, "l2_blocks": 2}


def _find_meflib() -> Path | None:
    for parent in [ROOT, *ROOT.parents]:
        for rel in ("reference_files/reference_files/meflib-multiplatform/meflib",
                    "reference_files/meflib-multiplatform/meflib"):
            if (parent / rel / "meflib.c").exists():
                return parent / rel
    return None


def _signal(rng: np.random.Generator, n: int) -> np.ndarray:
    t = np.arange(n)
    x = 400 * np.sin(2 * np.pi * t / 97.0) + 90 * np.sin(2 * np.pi * t / 13.0)
    return np.round(x + rng.normal(0, 20, n))


def _pymef_samples(path: Path, channel: str, n: int) -> np.ndarray:
    from pymef.mef_session import MefSession

    ms = MefSession(str(path), PASSWORD2, True)
    try:
        return np.asarray(ms.read_ts_channels_sample([channel], [0, n])[0]).astype(np.int32)
    finally:
        ms.close()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--meflib", type=Path, default=None, help="dir holding meflib.c/mefrec.c")
    ap.add_argument("--cc", default="cc")
    args = ap.parse_args()
    meflib = args.meflib or _find_meflib()
    if meflib is None or not (meflib / "meflib.c").exists():
        print("meflib source not found; pass --meflib", file=sys.stderr)
        return 2

    from mef_tools.io import MefWriter

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        exe = tmp / "red_blocks"
        subprocess.run([args.cc, "-O1", "-w", f"-I{meflib}", "-o", str(exe), str(HARNESS),
                        str(meflib / "meflib.c"), str(meflib / "mefrec.c"), "-lm"], check=True)

        session = tmp / f"{NAME}.mefd"
        rng = np.random.default_rng(80)
        w = MefWriter(str(session), overwrite=True, password1=PASSWORD1, password2=PASSWORD2)
        for ch in CHANNELS:
            w.write_data(_signal(rng, N_SAMPLES), ch, START_UUTC, FS, precision=0)
        del w

        truth = {ch: _pymef_samples(session, ch, N_SAMPLES) for ch in CHANNELS}
        for ch, x in truth.items():
            assert x.shape == (N_SAMPLES,), (ch, x.shape)

        for ch, level in CHANNELS.items():
            segs = sorted((session / f"{ch}.timd").glob("*.segd"))
            assert len(segs) == 1, segs
            seg = segs[0]
            stem = seg / seg.stem
            subprocess.run([str(exe), "encrypt", f"{stem}.tdat", f"{stem}.tidx", f"{stem}.tmet",
                            PASSWORD2, str(level)], check=True)
            # meflib decodes its own encrypted blocks back to the truth.
            out = tmp / f"{ch}.i32"
            subprocess.run([str(exe), "decode", f"{stem}.tdat", f"{stem}.tmet", PASSWORD2,
                            str(out)], check=True)
            assert np.array_equal(np.fromfile(out, "<i4"), truth[ch]), f"meflib decode {ch}"
            # Every block really is flagged encrypted at the requested level.
            tdat = (Path(f"{stem}.tdat")).read_bytes()
            off, blocks = 1024, 0
            mask = 0x02 if level == 1 else 0x04
            while off < len(tdat):
                assert tdat[off + 4] & mask, f"{ch}: block at {off} not encrypted"
                off += int.from_bytes(tdat[off + 36:off + 40], "little")
                blocks += 1
            print(f"  {ch}: {blocks} blocks encrypted by meflib at level {level}")

        dest = GOLDEN / f"{NAME}.mefd"
        if dest.exists():
            shutil.rmtree(dest)
        shutil.copytree(session, dest)
        np.savez_compressed(GOLDEN / f"{NAME}.npz", **truth)
    print(f"wrote {dest.relative_to(ROOT)} and {NAME}.npz")
    return 0


if __name__ == "__main__":
    sys.exit(main())
