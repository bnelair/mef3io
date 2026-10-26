"""P44 gate: every state a power cut during an append can leave is recovered.

2.0 flushes only the `.tmet` before its rename (no `durability` option), so
after a power cut each piece of an append may or may not have reached the
disk, independently: the `.tdat` body, the `.tdat` header patch, the `.tidx`
entries, the `.tidx` header patch, and the `.tmet` rename. Each state below is
BUILT by mixing the bytes of the session before and after one append (the
method of review round 1 on PR #153), then:

    recover_session(apply=True) alone  ->  validates clean
                                       ->  every sample whose bytes are on
                                           disk reads back, nothing else
                                       ->  a recorder re-sending what it lost
                                           (or carrying on) appends cleanly

and the CLI (`python -m mef3io recover --apply`) does the same.
"""
import shutil
import subprocess
import sys
from pathlib import Path

import numpy as np
import pytest

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO))

import mef3io  # noqa: E402

START = 1577836800000000
FS = 256.0
BLOCK = 256                # 1-s RED blocks: one append = 10 blocks
OLD_S, NEW_S = 20, 10      # seconds before / in the interrupted append
UH = 1024
TSI = 56


def _samples(n, seed):
    return np.random.default_rng(seed).integers(-9000, 9000, n, dtype=np.int32)


OLD = _samples(int(OLD_S * FS), 1)
NEW = _samples(int(NEW_S * FS), 2)
T_NEW = START + int(OLD_S * 1e6)


def _seg(path):
    seg = next(Path(path).glob("ch1.timd/*.segd"))
    base = seg.name[: -len(".segd")]
    return {k: seg / f"{base}.{k}" for k in ("tdat", "tidx", "tmet")}


@pytest.fixture(scope="module")
def images(tmp_path_factory):
    """The three files before and after one append."""
    root = tmp_path_factory.mktemp("crash")
    path = root / "s.mefd"
    with mef3io.Writer(str(path), block_length=BLOCK) as w:
        w.write_int32("ch1", OLD, 1.0, START, FS)
    old = {k: p.read_bytes() for k, p in _seg(path).items()}
    with mef3io.Writer(str(path), block_length=BLOCK) as w:
        w.write_int32("ch1", NEW, 1.0, T_NEW, FS)
    new = {k: p.read_bytes() for k, p in _seg(path).items()}
    assert len(new["tdat"]) > len(old["tdat"]) and len(new["tidx"]) == len(old["tidx"]) + 10 * TSI
    return path, old, new


def _tdat(old, new, body, header):
    """body: old | new | zero (extended with zeros) | torn (half the new bytes)."""
    o, n = old["tdat"], new["tdat"]
    if body == "old":
        out = o
    elif body == "new":
        out = n
    elif body == "zero":
        out = o + bytes(len(n) - len(o))
    else:  # torn
        out = n[: len(o) + (len(n) - len(o)) // 2]
    head = (o if header == "old" else n)[:UH]
    return head + out[UH:]


def _tidx(old, new, entries, header):
    """entries: old | new | zero (extended with zeros)."""
    o, n = old["tidx"], new["tidx"]
    if entries == "old":
        body = o[UH:]
    elif entries == "new":
        body = n[UH:]
    else:
        body = o[UH:] + bytes(len(n) - len(o))
    return (o if header == "old" else n)[:UH] + body


# (id, tdat body, tdat header, tidx entries, tidx header, tmet)
STATES = [
    ("tmet-only", "old", "old", "old", "old", "new"),
    ("rename-lost", "new", "new", "new", "new", "old"),
    ("tidx-entries-not-header", "new", "new", "new", "old", "old"),
    ("tidx-entries-not-header-tmet-new", "new", "new", "new", "old", "new"),
    ("tidx-header-not-entries", "new", "new", "old", "new", "old"),
    ("tidx-header-not-entries-tmet-new", "new", "new", "old", "new", "new"),
    ("tidx-zero-extended", "new", "new", "zero", "new", "new"),
    ("index-past-data", "old", "old", "new", "new", "old"),
    ("index-past-data-tmet-new", "old", "new", "new", "new", "new"),
    ("data-ahead-of-index", "new", "old", "old", "old", "old"),
    ("data-ahead-tmet-new", "new", "new", "old", "old", "new"),
    ("tdat-zero-extended", "zero", "new", "new", "new", "new"),
    ("tdat-torn", "torn", "new", "new", "new", "new"),
    ("tdat-torn-index-old", "torn", "old", "old", "old", "old"),
    ("tdat-header-only", "old", "new", "old", "old", "old"),
]


def _build(images, tmp_path, state):
    src, old, new = images
    _, tdat_body, tdat_head, tidx_entries, tidx_head, tmet = state
    path = tmp_path / "s.mefd"
    shutil.copytree(src, path)
    seg = _seg(path)
    seg["tdat"].write_bytes(_tdat(old, new, tdat_body, tdat_head))
    seg["tidx"].write_bytes(_tidx(old, new, tidx_entries, tidx_head))
    seg["tmet"].write_bytes((old if tmet == "old" else new)["tmet"])
    return path


def _expected(state):
    """The samples whose bytes are on disk: the old ones, plus every WHOLE new
    block that reached the .tdat."""
    body = state[1]
    if body == "new":
        return np.concatenate([OLD, NEW])
    if body == "torn":
        return None  # whole blocks of the first half: checked as a prefix
    return OLD


def _check(path, state):
    rep = mef3io.Validator(str(path)).validate()
    assert rep.ok, rep.summary()
    with mef3io.Reader(str(path)) as r:
        got = r.read("ch1")
    want = _expected(state)
    if want is None:
        n_old = len(OLD)
        np.testing.assert_array_equal(got[:n_old], OLD)
        tail = got[n_old:]
        kept = len(tail) - np.isnan(tail).sum()
        assert not np.isnan(tail[:kept]).any()
        assert 0 < kept < len(NEW) and kept % BLOCK == 0
        np.testing.assert_array_equal(tail[:kept], NEW[:kept])
        return n_old + kept
    np.testing.assert_array_equal(got, want.astype(float))
    return len(want)


def _reappend(path, have):
    """A recorder re-sends what was lost (or carries on) and the result is
    one valid session holding everything."""
    full = np.concatenate([OLD, NEW])
    with mef3io.Writer(str(path), block_length=BLOCK) as w:
        if have < len(full):
            w.write_int32("ch1", full[have:], 1.0, START + int(have / FS * 1e6), FS)
        more = _samples(int(5 * FS), 3)
        w.write_int32("ch1", more, 1.0, START + int((OLD_S + NEW_S) * 1e6), FS)
    rep = mef3io.Validator(str(path)).validate()
    assert rep.ok, rep.summary()
    with mef3io.Reader(str(path)) as r:
        np.testing.assert_array_equal(r.read("ch1"), np.concatenate([full, more]).astype(float))


@pytest.mark.parametrize("state", STATES, ids=[s[0] for s in STATES])
def test_recover_alone_restores_every_crash_state(images, tmp_path, state):
    path = _build(images, tmp_path, state)
    dry = mef3io.recover_session(str(path))
    assert not dry.skipped, dry.summary()
    assert dry.segments, "a torn append must not be reported as healthy:\n" + dry.summary()
    rep = mef3io.recover_session(str(path), apply=True, backup=False)
    assert not rep.skipped, rep.summary()
    have = _check(path, state)
    # Idempotent: a second pass finds nothing.
    again = mef3io.recover_session(str(path))
    assert again.nothing_to_do, again.summary()
    _reappend(path, have)


@pytest.mark.parametrize("state", STATES[:5] + STATES[-3:-1], ids=lambda s: s[0])
def test_the_cli_restores_them_too(images, tmp_path, state):
    path = _build(images, tmp_path, state)
    proc = subprocess.run(
        [sys.executable, "-m", "mef3io", "recover", str(path), "--apply", "--no-backup"],
        capture_output=True, text=True, cwd=str(REPO),
        env={**__import__("os").environ, "PYTHONPATH": str(REPO / "python")},
    )
    assert proc.returncode == 0, proc.stdout + proc.stderr
    _check(path, state)


def test_a_healthy_session_is_left_alone(images, tmp_path):
    path = _build(images, tmp_path, ("healthy", "new", "new", "new", "new", "new"))
    rep = mef3io.recover_session(str(path), apply=True, backup=False)
    assert rep.nothing_to_do, rep.summary()
    assert not Path(str(path) + ".repair-backup").exists()


@pytest.mark.parametrize("what", ["segment", "channel"])
@pytest.mark.parametrize("empty", [("tdat", "tidx", "tmet"), ("tdat", "tidx")],
                         ids=["all-empty", "tmet-landed"])
def test_a_new_segment_cut_off_before_its_data_is_removed(tmp_path, what, empty):
    """A new segment (or channel) whose files are empty after a power cut fails
    the strict open of the WHOLE session. A Writer that created the session
    does not flush them (a conversion writes thousands of segments); recovery
    removes such a segment when its data holds no block, losing nothing."""
    path = tmp_path / "s.mefd"
    with mef3io.Writer(str(path), block_length=BLOCK) as w:
        w.write_int32("ch1", OLD, 1.0, START, FS)
        if what == "segment":
            w.write_int32("ch1", NEW, 1.0, T_NEW, FS, new_segment=True)
        else:
            w.write_int32("ch2", NEW, 1.0, T_NEW, FS)
    folder = sorted(Path(path).glob("ch1.timd/*.segd"))[-1] if what == "segment" \
        else next(Path(path).glob("ch2.timd/*.segd"))
    for f in folder.iterdir():
        if f.suffix[1:] in empty:
            f.write_bytes(b"")
    with pytest.raises(Exception):   # the open, or the first read, fails
        with mef3io.Reader(str(path)) as r:
            for ch in r.channels:
                r.read(ch)

    rep = mef3io.recover_session(str(path), apply=True, backup=True)
    assert not rep.skipped, rep.summary()
    assert rep.segments and "removed" in rep.segments[0].action, rep.summary()
    assert not folder.exists()
    assert Path(str(path) + ".recover-backup").exists()
    assert mef3io.Validator(str(path)).validate().ok
    with mef3io.Reader(str(path)) as r:
        np.testing.assert_array_equal(r.read("ch1"), OLD.astype(float))
        assert ("ch2" in r.channels) is False
    # The recorder re-sends what was lost.
    with mef3io.Writer(str(path), block_length=BLOCK) as w:
        w.write_int32("ch1" if what == "segment" else "ch2", NEW, 1.0, T_NEW, FS)
    assert mef3io.Validator(str(path)).validate().ok


def test_a_new_segment_whose_data_landed_but_not_its_tmet_is_left_with_advice(tmp_path):
    """Samples on disk but no .tmet: it cannot be rebuilt, so it is skipped
    (never deleted) with what to do."""
    path = tmp_path / "s.mefd"
    with mef3io.Writer(str(path), block_length=BLOCK) as w:
        w.write_int32("ch1", OLD, 1.0, START, FS)
        w.write_int32("ch1", NEW, 1.0, T_NEW, FS, new_segment=True)
    folder = sorted(Path(path).glob("ch1.timd/*.segd"))[-1]
    next(folder.glob("*.tmet")).write_bytes(b"")
    rep = mef3io.recover_session(str(path), apply=True, backup=False)
    assert rep.skipped and "strict=False" in rep.skipped[0], rep.summary()
    assert folder.exists()
    with mef3io.Reader(str(path), strict=False) as r:
        np.testing.assert_array_equal(r.read("ch1")[: len(OLD)], OLD.astype(float))
