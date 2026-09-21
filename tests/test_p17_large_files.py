"""P17 gate: the read paths must not touch a whole `.tdat`.

About 30 GB is ONE channel, so a session runs to hundreds of gigabytes or
terabytes. Reading one file whole to serve a small request makes the library
unusable on exactly the data it exists for — and it has been reintroduced more
than once, most recently in `read_runs`, which pulled an entire segment in to
serve a one-minute window.

Measured with `/proc/self/io` `rchar`, which counts bytes actually read by the
process. That is exact and deterministic, unlike peak RSS, which is noisy enough
that a bound loose enough not to flake would not catch anything.
"""
import sys
import warnings
from pathlib import Path

import numpy as np
import pytest

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO))
warnings.filterwarnings("ignore")

import mef3io  # noqa: E402
from mef3io import _mef3io as m  # noqa: E402

pytestmark = pytest.mark.skipif(
    not Path("/proc/self/io").exists(), reason="needs /proc/self/io (Linux) to count bytes read"
)

START = 1577836800000000
FS = 256.0


def _rchar() -> int:
    for line in open("/proc/self/io"):
        if line.startswith("rchar"):
            return int(line.split()[1])
    raise RuntimeError("rchar not present in /proc/self/io")


@pytest.fixture(scope="module")
def big_session(tmp_path_factory):
    """One channel, incompressible, large enough that a whole-file read is
    unmistakable next to a windowed one."""
    path = tmp_path_factory.mktemp("large") / "big.mefd"
    rng = np.random.default_rng(0)
    w = mef3io.Writer(str(path), durability="fast")
    for i in range(60):  # 10 h at 256 Hz
        block = rng.integers(-(2**30), 2**30, int(FS * 600), dtype=np.int32)
        w.write_int32("ch1", block, 0.1, START + i * 600_000_000, FS)
    w.close()
    tdat = next(Path(path).rglob("*.tdat"))
    return str(path), tdat.stat().st_size


def test_a_windowed_read_reads_only_the_window(big_session):
    """`Reader.read` over a small time range must not read the whole segment."""
    path, size = big_session
    assert size > 20_000_000, "fixture too small to distinguish the two behaviours"

    with mef3io.Reader(path) as r:
        before = _rchar()
        got = r.read("ch1", START, START + 60_000_000)  # 1 minute of 10 hours
        read_bytes = _rchar() - before

    assert len(got) > 0
    assert read_bytes < size * 0.10, (
        f"read {read_bytes / 1e6:.1f} MB of a {size / 1e6:.1f} MB .tdat to serve a "
        f"one-minute window — the read path is not windowed"
    )


def test_read_runs_reads_only_the_window(big_session):
    """The regression this file exists for.

    `read_runs` called `read_all` on the `.tdat`, so a one-minute request
    allocated and read the entire segment. At 30 GB per channel that is fatal,
    and no test noticed because the fixtures were all a few kilobytes.
    """
    path, size = big_session
    session = m.Session(path, "")
    before = _rchar()
    runs = session.read_runs("ch1", START, START + 60_000_000)
    read_bytes = _rchar() - before

    assert sum(len(x["samples"]) for x in runs) > 0
    assert read_bytes < size * 0.10, (
        f"read_runs read {read_bytes / 1e6:.1f} MB of a {size / 1e6:.1f} MB .tdat for a "
        f"one-minute window"
    )


def test_validation_never_reads_the_data_body(big_session):
    """The validator checks the index against the .tdat's SIZE, not its bytes.

    It reads the 1024-byte universal header and stats the file; the block
    headers it needs for `sizing.difference-bytes` are one small read each, and
    `--fast` skips even those.
    """
    path, size = big_session
    before = _rchar()
    report = mef3io.Validator(path, exact_difference_bytes=False).validate()
    read_bytes = _rchar() - before

    assert report.segments_checked > 0
    assert read_bytes < size * 0.10, (
        f"validation read {read_bytes / 1e6:.1f} MB of a {size / 1e6:.1f} MB .tdat"
    )


def test_recovery_streams_rather_than_loading_the_data_file(big_session, tmp_path):
    """Recovery needs the file's SIZE plus a few block headers, never its body."""
    path, size = big_session
    before = _rchar()
    report = m.recover_session(path, False, True, "")   # dry run
    read_bytes = _rchar() - before

    assert report["segments_examined"] > 0
    assert not report["segments"], "a healthy session needs no recovery"
    assert read_bytes < size * 0.10, (
        f"recovery read {read_bytes / 1e6:.1f} MB of a {size / 1e6:.1f} MB .tdat"
    )


def test_an_append_does_not_rewrite_the_whole_data_file(big_session, tmp_path):
    """An append extends `.tdat` in place and patches its 1024-byte header.

    It must never read the body back — which a naive CRC recomputation would do.
    """
    path, size = big_session
    rng = np.random.default_rng(1)
    block = rng.integers(-(2**30), 2**30, int(FS * 60), dtype=np.int32)

    w = mef3io.Writer(path, durability="fast")
    before = _rchar()
    w.write_int32("ch1", block, 0.1, START + 60 * 600_000_000, FS)
    read_bytes = _rchar() - before
    w.close()

    assert read_bytes < size * 0.10, (
        f"an append read {read_bytes / 1e6:.1f} MB of a {size / 1e6:.1f} MB .tdat; it should "
        f"only extend it and patch the header"
    )


def _io():
    d = {}
    for line in open("/proc/self/io"):
        k, v = line.split()
        d[k.rstrip(":")] = int(v)
    return d["rchar"], d["wchar"]


@pytest.mark.parametrize("durability", ["full", "fast"])
def test_an_append_never_rewrites_the_index_or_the_data(tmp_path, durability):
    """Write amplification must be bounded by the NEW data, not the session.

    Both files an append touches grow without limit over a months-long
    recording: the `.tdat` to tens of gigabytes *per channel*, the `.tidx` to
    megabytes. Neither may be rewritten. The `.tdat` is extended in place and
    its 1024-byte header patched; the `.tidx` likewise — it used to be rewritten
    whole, which for a caller that reopens its Writer per block (a natural way
    to use this from Python) was ~47x write amplification against a month-long
    index.

    Checked for BOTH durability settings: `durability="full"` buys ordering
    barriers, not a different write strategy.
    """
    path = tmp_path / "s.mefd"
    rng = np.random.default_rng(5)
    # Small blocks so the index is large relative to the data appended.
    w = mef3io.Writer(str(path), block_length=256, durability=durability)
    for i in range(20):
        w.write_int32("ch1", rng.integers(-(2**30), 2**30, int(FS * 600), dtype=np.int32),
                      0.1, START + i * 600_000_000, FS)
    w.close()

    tidx = next(Path(path).rglob("*.tidx"))
    tdat = next(Path(path).rglob("*.tdat"))
    index_size, data_size = tidx.stat().st_size, tdat.stat().st_size
    assert index_size > 200_000, "fixture index too small to detect a rewrite"

    block = rng.integers(-(2**30), 2**30, int(FS * 600), dtype=np.int32)

    # A REOPENED writer: the case that used to rewrite the whole index.
    w = mef3io.Writer(str(path), block_length=256, durability=durability)
    before = _io()
    w.write_int32("ch1", block, 0.1, START + 20 * 600_000_000, FS)
    read_bytes, written = (a - b for a, b in zip(_io(), before))
    w.close()

    # Self-calibrating: how much did the files actually GROW? A correct append
    # writes about that much, plus the 16 KB .tmet. One that rewrites the index
    # writes `index_size` more than that, which is the whole point.
    grew = (tidx.stat().st_size - index_size) + (tdat.stat().st_size - data_size)
    slack = 64 * 1024 + 16 * 1024          # filesystem slop + the .tmet
    assert written < grew + slack + index_size * 0.25, (
        f"{durability}: wrote {written / 1e6:.2f} MB while the files grew only "
        f"{grew / 1e6:.2f} MB, against a {index_size / 1e6:.2f} MB index and a "
        f"{data_size / 1e6:.1f} MB .tdat — something is being rewritten whole"
    )
    # Reading the index once on a reopen is expected; reading the DATA never is.
    assert read_bytes < data_size * 0.5, f"{durability}: the .tdat was read back"


def test_the_block_index_cache_is_bounded_and_eviction_is_safe(tmp_path):
    """The index is ~2% of the data, so an unbounded cache is O(session).

    A reader that touches every channel of a few-hundred-gigabyte session used
    to keep several GB of block index resident for the life of the `Session`,
    and never release it. The cache is now capped and evicts least-recently-used
    segments — but only at the END of an operation, because a caller holds a
    span into those bytes while it is reading, and evicting one mid-read would
    be a use-after-free.

    So this checks both halves: the bound is honoured, and reads stay correct
    across eviction.
    """
    path = tmp_path / "s.mefd"
    rng = np.random.default_rng(11)
    block = (np.sin(np.arange(int(FS * 120)) / 40) * 500).astype(np.int32)
    w = mef3io.Writer(str(path), block_length=128)   # small blocks => fat index
    for i in range(6):
        for c in range(8):
            w.write_int32(f"ch{c}", block, 0.1, START + i * 120_000_000, FS)
    w.close()

    index_total = sum(f.stat().st_size for f in Path(path).rglob("*.tidx"))
    assert index_total > 100_000, "fixture index too small to exercise eviction"

    session = m.Session(str(path), "")
    truth = {}
    for c in range(8):
        truth[c] = session.read_runs(f"ch{c}", START, START + 720_000_000)

    # A budget far below the total forces eviction on every pass.
    session.set_index_cache_bytes(index_total // 8)
    for _ in range(3):
        for c in range(8):
            got = session.read_runs(f"ch{c}", START, START + 720_000_000)
            assert len(got) == len(truth[c]), f"ch{c}: run count changed under eviction"
            for a, b in zip(got, truth[c]):
                np.testing.assert_array_equal(a["samples"], b["samples"])
                assert a["start_uutc"] == b["start_uutc"]
        assert session.index_cache_bytes() <= index_total // 8, (
            f"cache is {session.index_cache_bytes()} bytes against a "
            f"{index_total // 8}-byte budget"
        )

    # 0 means unlimited, which must still work.
    session.set_index_cache_bytes(0)
    for c in range(8):
        assert len(session.read_runs(f"ch{c}", START, START + 720_000_000)) == len(truth[c])
