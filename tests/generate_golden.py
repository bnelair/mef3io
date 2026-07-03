"""Generate golden MEF 3.0 sessions with the legacy mef_tools/pymef stack.

These fixtures are the oracle for the mef3io C++/pure backends: every session
is written by mef_tools.MefWriter and then read back by mef_tools.MefReader,
and the read-back values (raw int32, scaled float64, times, metadata) are
stored alongside as an .npz. The mef3io test suite loads the same .mefd
directories and must reproduce the recorded values.

Run inside the active conda env:
    python tests/generate_golden.py
"""
from __future__ import annotations

import json
import shutil
import sys
import warnings
from pathlib import Path

import numpy as np

# Legacy writer casts NaN -> int32 for sub-fs gaps; the resulting overflow
# warning is expected sentinel behavior, not an error.
warnings.filterwarnings("ignore", message="invalid value encountered in cast")

MEF3IO_ROOT = Path(__file__).resolve().parent.parent  # mef3io/
# Locate the legacy mef_tools oracle: prefer a local checkout (original repo),
# else fall through to a pip-installed `mef-tools`. This dev-only regeneration
# script therefore works both in the original repo and standalone.
for _parent in [MEF3IO_ROOT, *MEF3IO_ROOT.parents]:
    if (_parent / "mef_tools" / "__init__.py").exists():
        sys.path.insert(0, str(_parent))
        break

import pandas as pd  # noqa: E402
from mef_tools.io import MefReader, MefWriter  # noqa: E402

GOLDEN_DIR = Path(__file__).resolve().parent / "golden"
RNG = np.random.default_rng(20260702)

# uUTC ~ 2020-01-01 to keep recording_time_offset arithmetic realistic.
BASE_UUTC = 1_577_836_800_000_000


def _signal(n: int, scale: float = 50.0) -> np.ndarray:
    """Smooth-ish float signal with a bit of structure (compresses like EEG)."""
    t = np.arange(n)
    x = (
        scale * np.sin(2 * np.pi * t / 137.0)
        + 0.3 * scale * np.sin(2 * np.pi * t / 31.0)
        + RNG.normal(0, 0.05 * scale, n)
    )
    return x.astype(np.float64)


def _write_case(name: str, *, password1=None, password2=None, channels: list[dict]):
    """Write one session and return the manifest entry with read-back truth."""
    session_path = GOLDEN_DIR / f"{name}.mefd"
    if session_path.exists():
        shutil.rmtree(session_path)

    w = MefWriter(str(session_path), overwrite=True, password1=password1, password2=password2)
    for ch in channels:
        for seg in ch["segments"]:
            w.write_data(
                seg["data"],
                ch["name"],
                seg["start_uutc"],
                ch["fs"],
                precision=ch.get("precision"),
                new_segment=seg.get("new_segment", False),
            )
        if ch.get("annotations") is not None:
            w.write_annotations(ch["annotations"], channel=ch["name"])
    del w  # flush/close

    # Read-back truth with the legacy reader (the compat gate target).
    # L2 password grants full access; store both so reader tests can exercise
    # L1-access (section 2 only) vs L2-access separately.
    read_pwd = password2 or password1
    r = MefReader(str(session_path), password2=read_pwd)
    entry = {
        "name": name,
        "password": read_pwd,
        "password1": password1,
        "password2": password2,
        "channels": {},
    }
    npz = {}
    for ch in channels:
        cname = ch["name"]
        info = r.get_channel_info(cname)
        start = int(r.get_property("start_time", cname))
        end = int(r.get_property("end_time", cname))
        # raw/scaled may carry NaN in discontinuity gaps -> keep float64 so the
        # gap structure is preserved for comparison against mef3io.read().
        raw = np.asarray(r.get_raw_data(cname, start, end)[0], dtype=np.float64)
        scaled = np.asarray(r.get_data(cname, start, end), dtype=np.float64)
        entry["channels"][cname] = {
            "fs": float(r.get_property("fsamp", cname)),
            "ufact": float(info["ufact"][0]),
            "start_time": start,
            "end_time": end,
            "nsamp": int(raw.shape[0]),
        }
        npz[f"{cname}__raw"] = raw
        npz[f"{cname}__scaled"] = scaled
    del r

    np.savez_compressed(GOLDEN_DIR / f"{name}.npz", **npz)
    print(f"  wrote {name}: {len(channels)} channel(s)")
    return entry


def _annotations(t0: int) -> pd.DataFrame:
    return pd.DataFrame(
        {
            "time": [t0 + 100_000, t0 + 500_000],
            "type": ["Note", "Note"],
            "text": ["onset", "offset"],
            "duration": [1000, 2000],
        }
    )


def main():
    GOLDEN_DIR.mkdir(parents=True, exist_ok=True)
    manifest = {"base_uutc": BASE_UUTC, "cases": []}

    fs = 256.0
    n = 2000

    # --- 1. plain, single channel, integer fs, float input + precision ---
    manifest["cases"].append(
        _write_case(
            "plain_single",
            channels=[
                {
                    "name": "ch1",
                    "fs": fs,
                    "precision": 3,
                    "segments": [{"data": _signal(n), "start_uutc": BASE_UUTC}],
                }
            ],
        )
    )

    # --- 2. discontinuity via long NaN gap (split into blocks) ---
    d = _signal(n)
    d[800:1400] = np.nan  # > fs samples -> real gap
    manifest["cases"].append(
        _write_case(
            "nan_gap_long",
            channels=[
                {
                    "name": "ch1",
                    "fs": fs,
                    "precision": 3,
                    "segments": [{"data": d, "start_uutc": BASE_UUTC}],
                }
            ],
        )
    )

    # --- 3. short NaN gap (< fs, embedded as sentinel by legacy) ---
    d = _signal(n)
    d[1000:1010] = np.nan
    manifest["cases"].append(
        _write_case(
            "nan_gap_short",
            channels=[
                {
                    "name": "ch1",
                    "fs": fs,
                    "precision": 3,
                    "segments": [{"data": d, "start_uutc": BASE_UUTC}],
                }
            ],
        )
    )

    # --- 4. multi-segment channel ---
    seg_dur_us = int(n / fs * 1e6)
    manifest["cases"].append(
        _write_case(
            "multi_segment",
            channels=[
                {
                    "name": "ch1",
                    "fs": fs,
                    "precision": 3,
                    "segments": [
                        {"data": _signal(n), "start_uutc": BASE_UUTC},
                        {
                            "data": _signal(n),
                            "start_uutc": BASE_UUTC + seg_dur_us + 1_000_000,
                            "new_segment": True,
                        },
                    ],
                }
            ],
        )
    )

    # --- 5. multi-channel ---
    manifest["cases"].append(
        _write_case(
            "multi_channel",
            channels=[
                {"name": "ch1", "fs": fs, "precision": 3,
                 "segments": [{"data": _signal(n), "start_uutc": BASE_UUTC}]},
                {"name": "ch2", "fs": fs, "precision": 2,
                 "segments": [{"data": _signal(n, 200.0), "start_uutc": BASE_UUTC}]},
            ],
        )
    )

    # --- 6. encrypted (section 2 -> L1, section 3 -> L2; the canonical
    #        encrypted MEF file produced by meflib when both passwords set) ---
    manifest["cases"].append(
        _write_case(
            "enc_both",
            password1="pass1",
            password2="pass2",
            channels=[
                {"name": "ch1", "fs": fs, "precision": 3,
                 "segments": [{"data": _signal(n), "start_uutc": BASE_UUTC}]}
            ],
        )
    )

    # --- 8. with annotations ---
    manifest["cases"].append(
        _write_case(
            "with_annotations",
            channels=[
                {"name": "ch1", "fs": fs, "precision": 3,
                 "segments": [{"data": _signal(n), "start_uutc": BASE_UUTC}],
                 "annotations": _annotations(BASE_UUTC)}
            ],
        )
    )

    # --- 9. fractional sampling frequency ---
    manifest["cases"].append(
        _write_case(
            "fractional_fs",
            channels=[
                {"name": "ch1", "fs": 512.5, "precision": 3,
                 "segments": [{"data": _signal(n), "start_uutc": BASE_UUTC}]}
            ],
        )
    )

    with open(GOLDEN_DIR / "manifest.json", "w") as f:
        json.dump(manifest, f, indent=2)
    print(f"\nmanifest: {len(manifest['cases'])} cases -> {GOLDEN_DIR / 'manifest.json'}")


if __name__ == "__main__":
    main()
