"""Write/read benchmark of the Python binding — the twin of
matlab/benchmark_mef3io.m. Same workload as examples/08: 5 channels x 5 h at
512 Hz (~88 MB session) of smoothed noise with one NaN gap, precision 3,
written and read back plain and encrypted. Run both on one machine to see the
MATLAB-vs-Python tradeoff (both sit on the same C++ core).

Usage: python benchmarks/bindings_benchmark.py [out_dir]
"""
import sys
import tempfile
import time
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "python"))
import mef3io  # noqa: E402

FS = 512
LEN_S = 3600 * 5
N_CH = 5
N = FS * LEN_S
START = 1_577_836_800_000_000
CHANNELS = [f"ch{i + 1}" for i in range(N_CH)]


def main(out_dir: str) -> None:
    rng = np.random.default_rng(42)
    x = rng.standard_normal((N_CH, N))
    kernel = np.ones(10) / 10
    for i in range(N_CH):  # smoothed noise (like examples/08)
        x[i] = np.convolve(x[i], kernel, mode="same")
    x[0, 1000:2000] = np.nan

    print(f"mef3io Python benchmark — {N_CH} ch x {LEN_S // 3600} h @ {FS} Hz "
          f"({N * N_CH / 1e6:.1f} M samples)")
    print(f"Python {sys.version.split()[0]}, mef3io {mef3io.__version__}\n")

    for encrypted in (False, True):
        label = "encrypted" if encrypted else "plain"
        p1, p2 = ("pwd1", "pwd2") if encrypted else ("", "")
        path = str(Path(out_dir) / f"bench_{label}.mefd")

        t = time.perf_counter()
        with mef3io.Writer(path, overwrite=True, password1=p1, password2=p2, units="uV") as w:
            for i, ch in enumerate(CHANNELS):
                w.write(ch, x[i], START, FS, precision=3)
        t_write = time.perf_counter() - t

        size_mb = sum(f.stat().st_size for f in Path(path).rglob("*") if f.is_file()) / 1e6

        t = time.perf_counter()
        with mef3io.Reader(path, p2) as r:
            total = 0
            for ch in CHANNELS:
                y = r.read(ch)
                total += len(y)
        t_read = time.perf_counter() - t

        assert total == N * N_CH
        with mef3io.Reader(path, p2) as r:
            y1 = r.read(CHANNELS[0])
        ok = ~np.isnan(x[0])
        assert np.isnan(y1).sum() == 1000
        assert np.abs(y1[ok] - np.round(x[0][ok], 3)).max() < 1e-9

        print(f"{label:9s}  write {t_write:6.2f} s ({N * N_CH / t_write / 1e6:5.1f} MS/s)"
              f"   read {t_read:6.2f} s ({N * N_CH / t_read / 1e6:5.1f} MS/s)"
              f"   {size_mb:.1f} MB")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else tempfile.mkdtemp())
