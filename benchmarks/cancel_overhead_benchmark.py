"""Overhead of the cancel/progress hooks (#69) — "zero cost when unused".

Times the operations that gained `cancel=` / `progress=` on one session, in
one process, so two builds can be compared on the same files:

* full read of every channel, and 300 random 10-s windowed reads;
* archive_session, extract_session, validate_session (exact), and a dry-run
  recover_session.

Each figure is the MINIMUM of `--repeat` runs (the least-disturbed one, which
is what an overhead comparison wants on a shared machine), page cache warm.
`--threads 1` takes the decode thread pool out of the picture. Run it once
against the build to compare with (e.g. the previous release's python/ tree
plus its extension on PYTHONPATH) and once against this one, pointing both at
the same `--session` directory so the input bytes are identical. When the
running mef3io has `CancelToken`, the same operations are ALSO timed with a
token and a progress callback attached, to show what using them costs.

Usage:
    python benchmarks/cancel_overhead_benchmark.py --session /tmp/bench.mefd \
        [--repeat 7] [--threads 1]
"""
from __future__ import annotations

import argparse
import shutil
import sys
import time
from pathlib import Path

import numpy as np

try:  # whichever mef3io is importable first — so another build can be compared
    import mef3io
except ImportError:  # otherwise this checkout's in-tree build
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "python"))
    import mef3io

FS = 512.0
HOURS = 2
N_CH = 16
START = 1_600_000_000_000_000


def make_session(path: Path) -> None:
    rng = np.random.default_rng(7)
    n = int(FS * 3600 * HOURS)
    shutil.rmtree(path, ignore_errors=True)  # the benchmark's own scratch data; mef3io never deletes a session
    with mef3io.Writer(str(path)) as w:
        for c in range(N_CH):
            x = np.cumsum(rng.standard_normal(n)) * 0.1
            w.write(f"ch{c:02d}", x, START, FS, precision=3)


def timed(fn, repeat: int) -> float:
    out = []
    for _ in range(repeat):
        t = time.perf_counter()
        fn()
        out.append(time.perf_counter() - t)
    return min(out)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--session", required=True, help="a .mefd path (created if missing)")
    ap.add_argument("--repeat", type=int, default=7)
    ap.add_argument("--threads", type=int, default=0, help="decode threads (0 = all cores)")
    args = ap.parse_args()
    session = Path(args.session)
    if not session.exists():
        print(f"writing {N_CH} ch x {HOURS} h @ {FS:g} Hz to {session} ...", flush=True)
        make_session(session)
    work = session.parent / (session.name + ".bench-work")
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir()
    tar = work / "copy.mefd.tar"
    out_dir = work / "copy.mefd"

    hooks = hasattr(mef3io, "CancelToken")
    print(f"mef3io {mef3io.__version__} from {Path(mef3io.__file__).parent}; hooks: {hooks}")

    variants = [("no hooks", {})]
    if hooks:
        calls = [0]

        def progress(done, total):
            calls[0] += 1

        variants.append(("token+progress", {"cancel": mef3io.CancelToken(), "progress": progress}))

    rng = np.random.default_rng(1)
    with mef3io.Reader(str(session), n_threads=args.threads) as r:
        chans = r.channels
        info = r.info(chans[0])
        t_lo, t_hi = info["start_time"], info["end_time"] - 10_000_000
        windows = [(chans[int(rng.integers(len(chans)))], int(rng.integers(t_lo, t_hi)))
                   for _ in range(300)]

        rows = []
        for label, kw in variants:
            full = timed(lambda: [r.read(c, **kw) for c in chans], args.repeat)
            win = timed(lambda: [r.read(c, t, t + 10_000_000, **kw) for c, t in windows],
                        args.repeat)
            rows.append((label, "read: all channels", full))
            rows.append((label, "read: 300 x 10 s windows", win))

    for label, kw in variants:
        rows.append((label, "archive_session", timed(
            lambda: mef3io.archive_session(str(session), str(tar), **kw),
            args.repeat)))
        rows.append((label, "extract_session", timed(
            lambda: mef3io.extract_session(str(tar), str(out_dir), **kw),
            args.repeat)))
        rows.append((label, "validate_session", timed(
            lambda: mef3io.validate_session(str(session), **kw), args.repeat)))
        rows.append((label, "recover_session (dry run)", timed(
            lambda: mef3io.recover_session(str(session), **kw), args.repeat)))

    print(f"\n{'variant':<16} {'operation':<28} {'min s':>10}")
    for label, op, sec in rows:
        print(f"{label:<16} {op:<28} {sec:>10.4f}")
    shutil.rmtree(work, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
