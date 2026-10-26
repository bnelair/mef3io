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
import os
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


def _cli(*args):
    """The CLI under THIS interpreter and THIS mef3io (an installed wheel in CI,
    the dev tree locally): the parent of the imported package goes first on
    the child's path, the rest of the parent's sys.path after it."""
    pkg_root = str(Path(mef3io.__file__).resolve().parent.parent)
    env = dict(os.environ)
    env["PYTHONPATH"] = os.pathsep.join([pkg_root] + [p for p in sys.path if p])
    return subprocess.run([sys.executable, "-m", "mef3io", *args],
                          capture_output=True, text=True, env=env, cwd=str(REPO))


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
    proc = _cli("recover", str(path), "--apply", "--no-backup")
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


def test_a_first_segment_whose_tmet_is_lost_is_skipped_with_advice(tmp_path):
    """A channel's FIRST segment has no previous .tmet to rebuild from (its
    files are flushed when written, so only another tool's session or a lying
    disk gets here): skipped, never deleted, with what to do."""
    path = tmp_path / "s.mefd"
    with mef3io.Writer(str(path), block_length=BLOCK) as w:
        w.write_int32("ch1", OLD, 1.0, START, FS)
        w.write_int32("ch2", OLD, 1.0, START, FS)
    folder = next(Path(path).glob("ch2.timd/*.segd"))
    next(folder.glob("*.tmet")).write_bytes(b"")
    rep = mef3io.recover_session(str(path), apply=True, backup=False)
    assert rep.skipped and "strict=False" in rep.skipped[0], rep.summary()
    assert "first segment" in rep.skipped[0]
    assert folder.exists()
    with pytest.warns(mef3io.UnreadableSegmentWarning):
        r = mef3io.Reader(str(path), strict=False)
    with r:
        np.testing.assert_array_equal(r.read("ch1"), OLD.astype(float))


@pytest.mark.parametrize("size", [0, 1000], ids=["empty", "short"])
def test_a_lost_tmet_of_a_later_segment_is_rebuilt(tmp_path, size):
    """A long-lived Writer's new segment: its files are not flushed (only a
    channel's first segment's are), so a power cut can leave the .tmet empty
    while the data and index landed. Recovery rebuilds it from the previous
    segment's .tmet plus this segment's own files, then re-derives the
    declarations: valid, every sample readable, and the recorder carries on."""
    path = tmp_path / "s.mefd"
    with mef3io.Writer(str(path), block_length=BLOCK) as w:
        w.write_int32("ch1", OLD, 1.0, START, FS)
        w.write_int32("ch1", NEW, 1.0, T_NEW, FS, new_segment=True)
    with mef3io.Reader(str(path)) as r:
        want = r.read("ch1")
        info = r.info("ch1")
    folder = sorted(Path(path).glob("ch1.timd/*.segd"))[-1]
    tmet = next(folder.glob("*.tmet"))
    original = tmet.read_bytes()
    tmet.write_bytes(original[:size])

    dry = mef3io.recover_session(str(path))
    assert dry.segments and "rebuilt" in dry.segments[0].action, dry.summary()
    assert tmet.read_bytes() == original[:size], "a dry run must not write"
    rep = mef3io.recover_session(str(path), apply=True)
    assert not rep.skipped, rep.summary()
    assert mef3io.Validator(str(path)).validate().ok
    with mef3io.Reader(str(path)) as r:
        np.testing.assert_array_equal(r.read("ch1"), want)
        assert r.info("ch1")["end_time"] == info["end_time"]
        prov = r.segments("ch1")[-1]["provenance"]
    assert "recovery" in str(prov), prov
    # Sections 1-3 equal the lost original byte for byte, but for section 2's
    # maximum_difference_bytes (s2 + 6388): an allocation size, which the
    # validator only requires to be large enough — the rebuilt one may be the
    # previous segment's larger value, never a smaller one than needed.
    import struct
    rebuilt = tmet.read_bytes()
    md = 2560 + 6388
    assert rebuilt[1024:md] == original[1024:md]
    assert rebuilt[md + 4:] == original[md + 4:]
    got_diff, want_diff = (struct.unpack_from("<I", b, md)[0] for b in (rebuilt, original))
    assert got_diff >= want_diff
    again = mef3io.recover_session(str(path))
    assert again.nothing_to_do, again.summary()
    more = _samples(int(5 * FS), 3)
    with mef3io.Writer(str(path), block_length=BLOCK) as w:
        w.write_int32("ch1", more, 1.0, START + int((OLD_S + NEW_S) * 1e6), FS)
    assert mef3io.Validator(str(path)).validate().ok


def test_a_healthy_legacy_session_is_left_byte_for_byte(tmp_path):
    """recover --apply is not a "repair everything": a healthy mef_tools session
    carries the legacy writer's declaration findings, and recovery must
    neither report nor touch them (that is `repair`'s job, by explicit id)."""
    mef_tools = pytest.importorskip("mef_tools.io")
    path = tmp_path / "legacy.mefd"
    w = mef_tools.MefWriter(str(path), overwrite=True)
    w.mef_block_len = BLOCK
    x = np.random.default_rng(9).normal(0, 50, int(40 * FS))
    w.write_data(x[: len(x) // 2], "ch1", START, FS, precision=3)
    w.write_data(x[len(x) // 2:], "ch1", START + int(20 * 1e6), FS, precision=3)
    del w
    assert not mef3io.Validator(str(path)).validate().ok, "fixture must carry legacy findings"
    before = {f: f.read_bytes() for f in Path(path).rglob("*") if f.is_file()}

    dry = mef3io.recover_session(str(path))
    assert dry.nothing_to_do, dry.summary()
    proc = _cli("recover", str(path), "--apply")
    assert proc.returncode == 0, proc.stdout + proc.stderr
    after = {f: f.read_bytes() for f in Path(path).rglob("*") if f.is_file()}
    assert after == before
    assert not Path(str(path) + ".repair-backup").exists()
    assert not Path(str(path) + ".recover-backup").exists()


# A channel's FIRST segment: only its .tmet is flushed when written (once per
# channel), so after a power cut its .tidx and .tdat can be anything from
# empty to complete. (tidx, tdat) state -> what recovery must leave.
FIRST_SEGMENT_STATES = [
    ("tidx-empty", "empty", "full"),
    ("tidx-short", "short", "full"),
    ("tidx-header-zeroed", "zeroed", "full"),
    ("tidx-empty-tdat-torn", "empty", "torn"),
    ("tidx-empty-tdat-header-zeroed", "empty", "header-zeroed"),
    ("both-empty", "empty", "empty"),
]


@pytest.mark.parametrize("state", FIRST_SEGMENT_STATES, ids=[s[0] for s in FIRST_SEGMENT_STATES])
def test_a_first_segment_with_a_lost_index_is_rebuilt_from_its_data(tmp_path, state):
    _, tidx_state, tdat_state = state
    path = tmp_path / "s.mefd"
    with mef3io.Writer(str(path), block_length=BLOCK) as w:
        w.write_int32("ch1", OLD, 1.0, START, FS)
        w.write_int32("ch2", NEW, 1.0, START, FS)       # the channel under test
    seg = next(Path(path).glob("ch2.timd/*.segd"))
    tidx, tdat = next(seg.glob("*.tidx")), next(seg.glob("*.tdat"))
    t, d = tidx.read_bytes(), tdat.read_bytes()
    tidx.write_bytes({"empty": b"", "short": t[:500], "zeroed": bytes(1024) + t[1024:]}[tidx_state])
    tdat.write_bytes({"full": d, "empty": b"", "torn": d[: 1024 + (len(d) - 1024) // 2],
                      "header-zeroed": bytes(1024) + d[1024:]}[tdat_state])

    dry = mef3io.recover_session(str(path))
    assert dry.segments and not dry.skipped, dry.summary()
    rep = mef3io.recover_session(str(path), apply=True)
    assert not rep.skipped, rep.summary()
    assert mef3io.Validator(str(path)).validate().ok
    with mef3io.Reader(str(path)) as r:
        np.testing.assert_array_equal(r.read("ch1"), OLD.astype(float))
        if tdat_state == "empty":
            assert "ch2" not in r.channels      # nothing landed: removed
            have = 0
        else:
            got = r.read("ch2")
            if tdat_state == "torn":
                assert 0 < len(got) < len(NEW) and len(got) % BLOCK == 0
            else:
                assert len(got) == len(NEW)
            np.testing.assert_array_equal(got, NEW[: len(got)].astype(float))
            have = len(got)
    again = mef3io.recover_session(str(path))
    assert again.nothing_to_do, again.summary()
    # The recorder re-sends what is missing.
    with mef3io.Writer(str(path), block_length=BLOCK) as w:
        if have < len(NEW):
            w.write_int32("ch2", NEW[have:], 1.0, START + int(have / FS * 1e6), FS)
    assert mef3io.Validator(str(path)).validate().ok
    with mef3io.Reader(str(path)) as r:
        np.testing.assert_array_equal(r.read("ch2"), NEW.astype(float))


def test_a_later_segment_with_tmet_and_index_lost_is_rebuilt(tmp_path):
    """A long-lived Writer's later segment is not flushed at all: its .tmet
    AND .tidx can both be lost while the data landed. The .tmet is rebuilt
    from the previous segment's, the index from the data."""
    path = tmp_path / "s.mefd"
    with mef3io.Writer(str(path), block_length=BLOCK) as w:
        w.write_int32("ch1", OLD, 1.0, START, FS)
        w.write_int32("ch1", NEW, 1.0, T_NEW, FS, new_segment=True)
    with mef3io.Reader(str(path)) as r:
        want = r.read("ch1")
    seg = sorted(Path(path).glob("ch1.timd/*.segd"))[-1]
    next(seg.glob("*.tmet")).write_bytes(b"")
    next(seg.glob("*.tidx")).write_bytes(b"")
    rep = mef3io.recover_session(str(path), apply=True)
    assert not rep.skipped, rep.summary()
    assert mef3io.Validator(str(path)).validate().ok
    with mef3io.Reader(str(path)) as r:
        np.testing.assert_array_equal(r.read("ch1"), want)
    assert mef3io.recover_session(str(path)).nothing_to_do
