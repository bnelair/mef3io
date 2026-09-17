"""P13: the session Validator — checking declarations against the data, and
repairing only what the caller explicitly selects.

Three contracts are pinned hard here, because they are what makes the tool safe
to point at real recordings:

* ``Validator`` never writes a byte, and has no method that could — writing
  lives in the separate ``repair_session()`` function.
* ``repair_session()`` writes only the checks named in its argument, and
  refuses an empty selection — there is no implicit "fix everything".
* ``Finding.repaired`` means "this was written", never "a repair was offered".
"""
import hashlib
import os
import struct
import subprocess
import sys
import warnings
from pathlib import Path

import numpy as np
import pytest

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO))
# NOTE: no blanket filter here — this module asserts on warnings.

import mef3io  # noqa: E402
from mef3io import _mef3io as m  # noqa: E402

START = 1577836800000000
FS = 256.0

UH_BYTES = 1024
METADATA_FILE_BYTES = 16384
S2 = 1024 + 1536

# section-2 fields, (section-relative offset, struct format)
S2_FIELDS = {
    "recording_duration": (4096, "<q"),
    "start_sample": (6352, "<q"),
    "number_of_samples": (6360, "<q"),
    "number_of_blocks": (6368, "<q"),
    "maximum_block_bytes": (6376, "<q"),
    "maximum_block_samples": (6384, "<I"),
    "maximum_difference_bytes": (6388, "<I"),
    "block_interval": (6392, "<q"),
    "number_of_discontinuities": (6400, "<q"),
    "maximum_contiguous_blocks": (6408, "<q"),
    "maximum_contiguous_block_bytes": (6416, "<q"),
    "maximum_contiguous_samples": (6424, "<q"),
}
# universal-header fields, (absolute offset, struct format)
UH_FIELDS = {
    "start_time": (16, "<q"),
    "end_time": (24, "<q"),
    "number_of_entries": (32, "<q"),
    "maximum_entry_size": (40, "<q"),
}


# --- fixtures and byte-level helpers ----------------------------------------


def _write(path, gap_us=int(5e6), n=4000, channels=("ch1",)):
    rng = np.random.default_rng(0)
    x = rng.normal(0, 3000, n).astype(np.int32)
    w = mef3io.Writer(str(path))
    for ch in channels:
        w.write_int32(ch, x, 0.5, START, FS)
        w.write_int32(ch, x, 0.5, START + int(n / FS * 1e6) + gap_us, FS)
    w.close()
    return x


def _tmet(path, channel="ch1"):
    return sorted(Path(path).rglob(f"{channel}-*.tmet"))[0]


def _patch_s2(tmet, field, value):
    """Set one section-2 field, repairing both universal-header CRCs."""
    raw = bytearray(Path(tmet).read_bytes())
    off, fmt = S2_FIELDS[field]
    struct.pack_into(fmt, raw, S2 + off, value)
    struct.pack_into("<I", raw, 4, m.crc32(bytes(raw[UH_BYTES:METADATA_FILE_BYTES])))
    struct.pack_into("<I", raw, 0, m.crc32(bytes(raw[4:UH_BYTES])))
    Path(tmet).write_bytes(bytes(raw))


def _patch_uh(file, field, value):
    """Set one universal-header field. The body is untouched, so only the
    header CRC (over bytes [4, 1024)) needs recomputing."""
    raw = bytearray(Path(file).read_bytes())
    off, fmt = UH_FIELDS[field]
    struct.pack_into(fmt, raw, off, value)
    struct.pack_into("<I", raw, 0, m.crc32(bytes(raw[4:UH_BYTES])))
    Path(file).write_bytes(bytes(raw))


def _read_s2(tmet, field):
    off, fmt = S2_FIELDS[field]
    return struct.unpack_from(fmt, Path(tmet).read_bytes(), S2 + off)[0]


def _tree_digest(path):
    h = hashlib.sha256()
    for f in sorted(Path(path).rglob("*")):
        if f.is_file():
            h.update(str(f.relative_to(path)).encode())
            h.update(f.read_bytes())
    return h.hexdigest()


def _ids(report):
    return {f.check_id for f in report.findings}


def _repair(validator, check_ids, **kwargs):
    """Repair the session a validator points at, with its same options.

    A Validator is read-only by construction, so repairs go through the
    separate module-level function; tests keep using a validator to say which
    session and options are in play.
    """
    return mef3io.repair_session(
        validator.path,
        check_ids,
        password=validator.password,
        channels=validator.channels,
        segments=validator.segments,
        exact_difference_bytes=validator.exact_difference_bytes,
        **kwargs,
    )


# --- the registry ------------------------------------------------------------


def test_registry_is_stable_and_ordered():
    ids = [c.id for c in mef3io.available_checks()]
    assert ids[:2] == ["crc.metadata", "crc.index"], "integrity checks run first"
    assert len(ids) == len(set(ids)), "check ids must be unique"
    for check in mef3io.available_checks():
        assert check.severity in ("info", "warning", "error")
        assert check.title and check.description
    # CRC failures mean the bytes cannot be trusted, so they are never repaired.
    by_id = {c.id: c for c in mef3io.available_checks()}
    assert not by_id["crc.metadata"].repairable
    assert not by_id["crc.index"].repairable
    assert not by_id["index.block-offsets"].repairable
    assert by_id["sizing.difference-bytes"].repairable


def test_describe_check_round_trips():
    assert mef3io.Validator.describe_check("sizing.contiguous").id == "sizing.contiguous"
    assert mef3io.Validator.describe_check("no.such.check") is None


# --- a healthy session -------------------------------------------------------


def test_clean_session_has_no_findings(tmp_path):
    path = tmp_path / "s.mefd"
    _write(path, channels=("ch1", "ch2"))
    report = mef3io.Validator(str(path)).validate()
    assert report.findings == ()
    assert report.skipped == ()
    assert report.ok
    assert report.segments_checked == 2
    assert len(report.checks_run) == len(mef3io.available_checks())
    assert "No problems found" in report.summary()


def test_validator_has_no_way_to_write():
    """A checker checks. Writing lives in repair_session(), under its own name.

    Pinned as a test and not just a convention: the point of the split is that
    an operator cannot reach a mutation from an object they opened to inspect,
    so re-attaching one to Validator must fail here rather than in the field.
    """
    public = {n for n in dir(mef3io.Validator) if not n.startswith("_")}
    assert not (public & {"repair", "fix", "write", "apply"}), public
    assert callable(mef3io.repair_session)


def test_validate_never_writes(tmp_path):
    path = tmp_path / "s.mefd"
    _write(path)
    _patch_s2(_tmet(path), "maximum_difference_bytes", 0)
    before = _tree_digest(path)
    report = mef3io.Validator(str(path)).validate()
    assert report.findings, "the corruption should have been found"
    assert _tree_digest(path) == before, "validate() must not modify the session"


# --- every repairable check: detect in isolation, then fix in isolation ------

# check id -> (how to corrupt the segment, the field name it reports)
CORRUPTIONS = {
    "index.block-count": (lambda t, path: _patch_s2(t, "number_of_blocks", 99), "number_of_blocks"),
    "index.sample-count": (
        lambda t, path: _patch_s2(t, "number_of_samples", 7),
        "number_of_samples",
    ),
    "sizing.block-maxima": (
        lambda t, path: _patch_s2(t, "maximum_block_bytes", 1),
        "maximum_block_bytes",
    ),
    "sizing.difference-bytes": (
        lambda t, path: _patch_s2(t, "maximum_difference_bytes", 0),
        "maximum_difference_bytes",
    ),
    "sizing.contiguous": (
        lambda t, path: _patch_s2(t, "maximum_contiguous_block_bytes", 0),
        "maximum_contiguous_block_bytes",
    ),
    "times.recording-duration": (
        lambda t, path: _patch_s2(t, "recording_duration", 123),
        "recording_duration",
    ),
    "times.block-interval": (lambda t, path: _patch_s2(t, "block_interval", 0), "block_interval"),
    "times.discontinuities": (
        lambda t, path: _patch_s2(t, "number_of_discontinuities", 0),
        "number_of_discontinuities",
    ),
    "times.segment-bounds": (
        lambda t, path: _patch_uh(t, "start_time", -(START - int(60e6))),
        "metadata start_time",
    ),
    "header.entry-count": (
        lambda t, path: _patch_uh(t.with_suffix(".tidx"), "number_of_entries", 3),
        "index number_of_entries",
    ),
    "header.max-entry-size": (
        lambda t, path: _patch_uh(t.with_suffix(".tdat"), "maximum_entry_size", 2),
        "data maximum_entry_size",
    ),
}


def test_start_sample_is_reported_but_never_repaired(tmp_path):
    """Writers disagree on what .tidx start_sample means: mef3io stores
    channel-absolute values in both places, pymef resets the index to 0 in
    every segment and keeps section 2 cumulative — and its reader depends on
    exactly that split. Rewriting one convention into the other makes a working
    session segfault pymef, so this check reports and never repairs."""
    path = tmp_path / "s.mefd"
    _write(path)
    tmet = _tmet(path)

    assert not mef3io.Validator.describe_check("index.start-sample").repairable
    with pytest.raises(ValueError, match="not repairable"):
        mef3io.repair_session(str(path), ["index.start-sample"])

    # The pymef layout — index restarts at 0, section 2 stays cumulative — must
    # NOT be reported: it is correct for that writer.
    _patch_s2(tmet, "start_sample", 4000)
    assert "index.start-sample" not in _ids(mef3io.Validator(str(path)).validate())


def test_every_repairable_check_is_covered():
    """A new repairable check must come with a corruption case, or this fails."""
    repairable = {c.id for c in mef3io.available_checks() if c.repairable}
    assert repairable == set(CORRUPTIONS), "CORRUPTIONS must cover every repairable check"


@pytest.mark.parametrize("check_id", sorted(CORRUPTIONS))
def test_check_detects_and_fix_repairs(tmp_path, check_id):
    corrupt, expected_field = CORRUPTIONS[check_id]
    path = tmp_path / "s.mefd"
    _write(path)
    tmet = _tmet(path)
    corrupt(tmet, path)

    validator = mef3io.Validator(str(path))

    # The named check finds it...
    isolated = validator.check(check_id)
    assert _ids(isolated) == {check_id}
    assert isolated.findings[0].field == expected_field
    assert isolated.findings[0].repairable
    assert not isolated.findings[0].repaired
    assert check_id in isolated.repairable_check_ids

    # ...a full pass finds it too...
    full = validator.validate()
    assert check_id in _ids(full)

    # ...and fixing that one check clears it, without needing a second pass.
    repaired = _repair(validator, [check_id])
    assert any(f.check_id == check_id and f.repaired for f in repaired.findings)
    after = validator.validate()
    assert check_id not in _ids(after)
    assert after.ok, after.summary()


def test_repair_touches_only_the_selected_check(tmp_path):
    path = tmp_path / "s.mefd"
    _write(path)
    tmet = _tmet(path)
    _patch_s2(tmet, "maximum_difference_bytes", 0)
    _patch_s2(tmet, "block_interval", 0)

    report = mef3io.repair_session(str(path), ["sizing.difference-bytes"])
    assert {f.check_id for f in report.repaired} == {"sizing.difference-bytes"}

    remaining = _ids(mef3io.Validator(str(path)).validate())
    assert "sizing.difference-bytes" not in remaining
    assert "times.block-interval" in remaining, "an unselected check must be left alone"
    assert _read_s2(tmet, "block_interval") == 0


def test_repair_respects_channel_and_segment_filters(tmp_path):
    path = tmp_path / "s.mefd"
    _write(path, channels=("ch1", "ch2"))
    for ch in ("ch1", "ch2"):
        _patch_s2(_tmet(path, ch), "maximum_difference_bytes", 0)

    report = mef3io.repair_session(str(path), ["sizing.difference-bytes"], channels=["ch1"])
    assert {f.channel for f in report.findings} == {"ch1"}
    assert _read_s2(_tmet(path, "ch1"), "maximum_difference_bytes") > 0
    assert _read_s2(_tmet(path, "ch2"), "maximum_difference_bytes") == 0


# --- repairs are never implicit ---------------------------------------------


def test_repair_refuses_an_empty_selection(tmp_path):
    path = tmp_path / "s.mefd"
    _write(path)
    _patch_s2(_tmet(path), "maximum_difference_bytes", 0)
    before = _tree_digest(path)
    with pytest.raises(ValueError, match="never implicit"):
        mef3io.repair_session(str(path), [])
    assert _tree_digest(path) == before


@pytest.mark.parametrize("bad", ["no.such.check", "crc.metadata"])
def test_repair_rejects_unknown_or_unrepairable_checks(tmp_path, bad):
    path = tmp_path / "s.mefd"
    _write(path)
    before = _tree_digest(path)
    with pytest.raises(ValueError):
        mef3io.repair_session(str(path), [bad])
    assert _tree_digest(path) == before


def test_repair_of_a_clean_session_changes_nothing(tmp_path):
    path = tmp_path / "s.mefd"
    _write(path)
    before = _tree_digest(path)
    report = mef3io.repair_session(str(path), ["sizing.difference-bytes"])
    assert report.segments_repaired == 0
    assert _tree_digest(path) == before


# --- backups -----------------------------------------------------------------


def test_repair_backs_up_before_rewriting(tmp_path):
    path = tmp_path / "s.mefd"
    _write(path)
    tmet = _tmet(path)
    _patch_s2(tmet, "maximum_difference_bytes", 0)
    original = tmet.read_bytes()

    mef3io.repair_session(str(path), ["sizing.difference-bytes"])
    backups = list(Path(str(path) + ".repair-backup").rglob("*.tmet"))
    assert len(backups) == 1
    assert backups[0].read_bytes() == original
    assert tmet.read_bytes() != original

    # A second repair must not clobber the pristine backup.
    _patch_s2(tmet, "maximum_difference_bytes", 0)
    mef3io.repair_session(str(path), ["sizing.difference-bytes"])
    assert backups[0].read_bytes() == original


def test_backup_can_be_declined(tmp_path):
    path = tmp_path / "s.mefd"
    _write(path)
    _patch_s2(_tmet(path), "maximum_difference_bytes", 0)
    mef3io.repair_session(str(path), ["sizing.difference-bytes"], backup=False)
    assert not Path(str(path) + ".repair-backup").exists()


# --- damaged input is reported, never repaired ------------------------------


def test_bad_crc_is_reported_and_left_alone(tmp_path):
    path = tmp_path / "s.mefd"
    _write(path)
    tmet = _tmet(path)
    raw = bytearray(tmet.read_bytes())
    struct.pack_into("<q", raw, S2 + S2_FIELDS["number_of_blocks"][0], 99)  # no CRC repair
    tmet.write_bytes(bytes(raw))
    before = _tree_digest(path)

    report = mef3io.repair_session(str(path), ["index.block-count"])
    assert "crc.metadata" in _ids(report)
    assert report.errors
    assert not report.ok
    assert report.skipped and "CRC" in report.skipped[0].reason
    assert report.segments_repaired == 0
    assert _tree_digest(path) == before, "a segment with a bad CRC must not be rewritten"


@pytest.mark.parametrize("suffix", [".tmet", ".tidx", ".tdat"])
def test_incomplete_segment_is_reported_not_dropped(tmp_path, suffix):
    """A traversal skips a segment missing one of its files; surfacing exactly
    that is the point of a validator, so it must never be quietly omitted."""
    path = tmp_path / "s.mefd"
    _write(path, channels=("ch1", "ch2"))
    _tmet(path, "ch1").with_suffix(suffix).unlink()

    report = mef3io.Validator(str(path)).validate()
    assert not report.ok
    assert len(report.skipped) == 1
    skipped = report.skipped[0]
    assert skipped.channel == "ch1"
    assert suffix in skipped.reason
    assert report.segments_checked == 1, "the intact channel is still checked"


def test_truncated_data_file_is_an_error(tmp_path):
    path = tmp_path / "s.mefd"
    _write(path)
    tdat = _tmet(path).with_suffix(".tdat")
    tdat.write_bytes(tdat.read_bytes()[: UH_BYTES + 100])

    report = mef3io.Validator(str(path)).validate()
    assert "index.block-offsets" in _ids(report)
    assert not report.ok


# --- repairs preserve the data ----------------------------------------------


def test_repaired_session_still_reads_identically(tmp_path):
    path = tmp_path / "s.mefd"
    _write(path)
    with mef3io.Reader(str(path)) as r:
        expected = r.read_raw("ch1")

    tmet = _tmet(path)
    for fld, value in [
        ("maximum_difference_bytes", 0),
        ("maximum_contiguous_block_bytes", 0),
        ("block_interval", 0),
        ("number_of_discontinuities", 0),
    ]:
        _patch_s2(tmet, fld, value)

    validator = mef3io.Validator(str(path))
    report = _repair(validator, validator.validate().repairable_check_ids)
    assert report.segments_repaired == 1
    assert validator.validate().ok

    with mef3io.Reader(str(path)) as r:
        got = r.read_raw("ch1")
    np.testing.assert_array_equal(got["samples"], expected["samples"])
    np.testing.assert_array_equal(got["valid"], expected["valid"])


def _patch_encrypted_s2(tmet, field, value, password1):
    """Set one section-2 field in an encrypted .tmet.

    Section 2 is ciphertext on disk, so the field cannot be poked directly:
    decrypt with the level-1 key (which is the padded password bytes — see
    ``validate_password`` in core/src/password.cpp), edit the plaintext,
    re-encrypt, then repair both universal-header CRCs.
    """
    raw = bytearray(Path(tmet).read_bytes())
    key = m.extract_password_bytes(password1)
    end = S2 + 10752
    plain = bytearray(m.aes128_ecb_decrypt(bytes(raw[S2:end]), key))
    off, fmt = S2_FIELDS[field]
    struct.pack_into(fmt, plain, off, value)
    raw[S2:end] = m.aes128_ecb_encrypt(bytes(plain), key)
    struct.pack_into("<I", raw, 4, m.crc32(bytes(raw[UH_BYTES:METADATA_FILE_BYTES])))
    struct.pack_into("<I", raw, 0, m.crc32(bytes(raw[4:UH_BYTES])))
    Path(tmet).write_bytes(bytes(raw))


def test_encrypted_session_is_repaired_and_stays_readable(tmp_path):
    """Section 2 is ciphertext on disk, so repairing it means decrypt, edit and
    re-encrypt with the same key. The session must come back both fixed and
    still protected."""
    path = tmp_path / "enc.mefd"
    rng = np.random.default_rng(3)
    x = rng.normal(0, 3000, 4000).astype(np.int32)
    w = mef3io.Writer(str(path), password1="lvl1", password2="lvl2")
    w.write_int32("ch1", x, 0.5, START, FS)
    w.close()

    validator = mef3io.Validator(str(path), password="lvl2")
    assert validator.validate().ok

    tmet = _tmet(path)
    _patch_encrypted_s2(tmet, "maximum_difference_bytes", 0, "lvl1")
    ciphertext_before = Path(tmet).read_bytes()[S2 : S2 + 10752]
    assert "sizing.difference-bytes" in _ids(validator.validate())

    report = _repair(validator, ["sizing.difference-bytes"])
    assert report.segments_repaired == 1
    assert [f.repaired for f in report.findings if f.check_id == "sizing.difference-bytes"] == [True]
    assert validator.validate().ok

    # Section 2 must still be ciphertext, and still the *same* key: a level-2
    # read has to return both the repaired value and the original samples.
    after = Path(tmet).read_bytes()[S2 : S2 + 10752]
    assert after != ciphertext_before, "section 2 was not rewritten"
    plain = m.aes128_ecb_decrypt(bytes(after), m.extract_password_bytes("lvl1"))
    off, fmt = S2_FIELDS["maximum_difference_bytes"]
    assert struct.unpack_from(fmt, plain, off)[0] > 0
    assert b"\x00" * 64 != after[:64], "section 2 must not have been left in plaintext"

    with mef3io.Reader(str(path), password="lvl2") as r:
        np.testing.assert_array_equal(r.read_raw("ch1")["samples"], x)
    # And the protection is intact: no password still means no section 2.
    assert mef3io.Validator(str(path)).validate().skipped


def test_encrypted_session_without_password_is_skipped(tmp_path):
    path = tmp_path / "enc.mefd"
    x = np.arange(2000, dtype=np.int32)
    w = mef3io.Writer(str(path), password1="lvl1", password2="lvl2")
    w.write_int32("ch1", x, 1.0, START, FS)
    w.close()

    report = mef3io.Validator(str(path)).validate()
    assert report.skipped, "an unreadable section 2 must be reported, not silently passed"
    assert not report.ok


# --- tar sessions ------------------------------------------------------------


def test_tar_session_validates_but_cannot_be_repaired(tmp_path):
    path = tmp_path / "s.mefd"
    _write(path)
    archive = mef3io.archive_session(str(path))

    assert mef3io.Validator(archive).validate().ok
    with pytest.raises(RuntimeError, match="tar"):
        mef3io.repair_session(archive, ["sizing.difference-bytes"])


# --- the fast path -----------------------------------------------------------


def test_fast_mode_still_catches_an_unset_difference_bytes(tmp_path):
    path = tmp_path / "s.mefd"
    _write(path)
    tmet = _tmet(path)
    _patch_s2(tmet, "maximum_difference_bytes", 0)

    validator = mef3io.Validator(str(path), exact_difference_bytes=False)
    assert "sizing.difference-bytes" in _ids(validator.validate())
    _repair(validator, ["sizing.difference-bytes"])
    # Without reading .tdat the repair uses meflib's worst case, so it is a safe
    # over-declaration rather than the exact maximum.
    bound = 5 * _read_s2(tmet, "maximum_block_samples")
    exact = mef3io.Validator(str(path)).validate()
    assert _read_s2(tmet, "maximum_difference_bytes") == bound
    assert exact.ok, "an over-declaration is safe and must not be reported"


# --- reporting ---------------------------------------------------------------


def test_report_groups_and_summarizes(tmp_path):
    path = tmp_path / "s.mefd"
    _write(path, channels=("ch1", "ch2"))
    for ch in ("ch1", "ch2"):
        _patch_s2(_tmet(path, ch), "maximum_difference_bytes", 0)

    report = mef3io.Validator(str(path)).validate()
    assert set(report.by_check()) == {"sizing.difference-bytes"}
    assert set(report.by_channel()) == {"ch1", "ch2"}
    assert len(report.errors) == 2
    text = report.summary()
    assert "sizing.difference-bytes" in text
    assert "2 segment(s)" in text
    assert "repair_session(" in text, "the summary should say how to opt in to the fix"


# --- command line ------------------------------------------------------------


def test_cli_reports_and_repairs_only_when_asked(tmp_path):
    path = tmp_path / "s.mefd"
    _write(path)
    _patch_s2(_tmet(path), "maximum_difference_bytes", 0)
    # Point the subprocess at whichever mef3io this process imported: the
    # in-tree package when dev_build.sh has linked the extension into it, and
    # the installed one on CI. Hardcoding the source tree gives CI a package
    # with no compiled backend.
    env = dict(os.environ, PYTHONPATH=str(Path(mef3io.__file__).resolve().parent.parent))

    listing = subprocess.run(
        [sys.executable, "-m", "mef3io", "validate", "--list-checks"],
        capture_output=True, text=True, env=env,
    )
    assert "sizing.difference-bytes" in listing.stdout

    report = subprocess.run(
        [sys.executable, "-m", "mef3io", "validate", str(path)],
        capture_output=True, text=True, env=env,
    )
    assert report.returncode == 1, "a failing session must exit non-zero"
    assert "sizing.difference-bytes" in report.stdout
    assert _read_s2(_tmet(path), "maximum_difference_bytes") == 0, "reporting must not write"

    # Writing is a different command; `validate` has no way to reach it.
    no_such_flag = subprocess.run(
        [sys.executable, "-m", "mef3io", "validate", str(path),
         "--repair", "sizing.difference-bytes"],
        capture_output=True, text=True, env=env,
    )
    assert no_such_flag.returncode == 2, "validate must not accept --repair"
    assert _read_s2(_tmet(path), "maximum_difference_bytes") == 0

    fixed = subprocess.run(
        [sys.executable, "-m", "mef3io", "repair", str(path),
         "--check", "sizing.difference-bytes"],
        capture_output=True, text=True, env=env,
    )
    assert fixed.returncode == 0, fixed.stdout + fixed.stderr
    assert "repaired" in fixed.stdout
    assert _read_s2(_tmet(path), "maximum_difference_bytes") > 0
    assert "found in sys.modules" not in fixed.stderr, (
        "`python -m mef3io` must not double-import the package"
    )


# --- the open-time warning ---------------------------------------------------


def test_clean_session_opens_without_a_warning(tmp_path):
    path = tmp_path / "s.mefd"
    _write(path)
    with warnings.catch_warnings():
        warnings.simplefilter("error")  # any warning fails the test
        with mef3io.Reader(str(path)) as r:
            r.channels


def test_open_warns_about_unset_declarations_but_reads_fine(tmp_path):
    path = tmp_path / "s.mefd"
    x = _write(path)
    with mef3io.Reader(str(path)) as r:
        expected = r.read_raw("ch1")
    _patch_s2(_tmet(path), "maximum_difference_bytes", 0)  # a mef3io <= 1.1.2 session

    with warnings.catch_warnings(record=True) as caught:
        warnings.simplefilter("always")
        with mef3io.Reader(str(path)) as r:
            got = r.read_raw("ch1")

    hits = [w for w in caught if issubclass(w.category, mef3io.SessionDeclarationWarning)]
    assert len(hits) == 1, "exactly one warning per session open"
    text = str(hits[0].message)
    assert "maximum_difference_bytes" in text
    assert "does NOT affect reading" in text, "the message must say reads are unaffected"
    assert "Validator" in text, "the message must say how to look closer"
    # And the claim it makes must be true.
    np.testing.assert_array_equal(got["samples"], expected["samples"])
    np.testing.assert_array_equal(got["valid"], expected["valid"])
    assert len(got["samples"]) == len(x) * 2 + round(5e6 * FS / 1e6)


def test_open_warning_can_be_declined_or_filtered(tmp_path):
    path = tmp_path / "s.mefd"
    _write(path)
    _patch_s2(_tmet(path), "maximum_difference_bytes", 0)

    with warnings.catch_warnings(record=True) as caught:
        warnings.simplefilter("always")
        mef3io.Reader(str(path), warn_declarations=False)
    assert not [w for w in caught if issubclass(w.category, mef3io.SessionDeclarationWarning)]

    with warnings.catch_warnings(record=True) as caught:
        warnings.simplefilter("always")
        warnings.filterwarnings("ignore", category=mef3io.SessionDeclarationWarning)
        mef3io.Reader(str(path))
    assert not caught


def test_warm_cache_open_still_warns(tmp_path):
    """A warm start skips the session tree entirely, so the issue list has to
    travel in the cache snapshot or the second open would look clean."""
    path = tmp_path / "s.mefd"
    _write(path)
    _patch_s2(_tmet(path), "maximum_difference_bytes", 0)
    cache_dir = str(tmp_path / "cache")

    mef3io.Reader(str(path), cache=cache_dir, warn_declarations=False)  # populate
    with warnings.catch_warnings(record=True) as caught:
        warnings.simplefilter("always")
        reader = mef3io.Reader(str(path), cache=cache_dir)
        assert reader._impl is None, "the warm open must not build a backend"
    assert [w for w in caught if issubclass(w.category, mef3io.SessionDeclarationWarning)]


def test_repairing_clears_the_open_warning(tmp_path):
    path = tmp_path / "s.mefd"
    _write(path)
    _patch_s2(_tmet(path), "maximum_difference_bytes", 0)
    mef3io.repair_session(str(path), ["sizing.difference-bytes"])

    with warnings.catch_warnings():
        warnings.simplefilter("error")
        mef3io.Reader(str(path))


# --- against the legacy writer ----------------------------------------------


def test_legacy_written_session_reports_known_divergences(tmp_path):
    """The legacy pymef writer leaves several declarations wrong; the checks
    must name exactly those, and nothing else."""
    pytest.importorskip("mef_tools", reason="legacy oracle not installed")
    from mef_tools.io import MefWriter

    path = tmp_path / "legacy.mefd"
    rng = np.random.default_rng(0)
    x = rng.normal(0, 3000, 4000).astype(np.int32)
    w = MefWriter(str(path), overwrite=True)
    w.write_data(x, "ch1", START, FS, precision=0)
    w.write_data(x, "ch1", START + int(4000 / FS * 1e6) + int(5e6), FS, precision=0)
    del w

    report = mef3io.Validator(str(path)).validate()
    assert _ids(report) == {
        "sizing.contiguous",            # pymef ignores discontinuities
        "times.recording-duration",     # pymef stores n/fs, not the span
        "times.block-interval",         # left at 0
        "times.discontinuities",        # left at 0 despite writing the flags
        "header.max-entry-size",        # .tdat gets samples, not bytes
    }, report.summary()
    # Both errors here are zero-size allocation hazards, and both are rated so
    # on purpose:
    #
    #   times.discontinuities — number_of_discontinuities = 0 with the flags
    #   written. meflib's find_discontinuity_indices (meflib.c:3548) mallocs
    #   exactly that many entries and then writes one per flagged block.
    #
    #   sizing.contiguous — pymef leaves maximum_contiguous_samples at 0 while
    #   the longest run really is a full block's worth. 0 is not the si8
    #   NO_ENTRY sentinel (-1), so a reader sizing a run buffer from the field
    #   cannot tell it was never set. Under-declaring is the truncating
    #   direction and is an error; pymef's over-declared
    #   maximum_contiguous_block_bytes in the same segment is only a warning.
    assert {f.check_id for f in report.errors} == {
        "times.discontinuities",
        "sizing.contiguous",
    }
    contiguous = [f for f in report.findings if f.check_id == "sizing.contiguous"]
    assert contiguous[0].field == "maximum_contiguous_samples"
    assert contiguous[0].stored == "0" and contiguous[0].severity == "error"


# --- data-safety regressions (each of these once destroyed data) -------------

def test_repair_refuses_an_index_that_stops_short_of_the_data(tmp_path):
    """An index missing entries must never become the new declarations.

    A truncated index stays internally consistent — monotonic offsets, every
    block inside .tdat, its own CRC covering what is left — so neither the CRC
    gate nor index.block-offsets sees it. But the blocks it no longer mentions
    are still on disk, and every declaration derived from it is SMALLER than
    the truth. Writing that back over section 2 destroys the last record of
    what the data file holds.
    """
    path = tmp_path / "s.mefd"
    _write(path, gap_us=0)
    tmet = _tmet(path)
    tidx = Path(tmet).with_suffix(".tidx")
    true_samples = _read_s2(tmet, "number_of_samples")
    assert true_samples > 0

    # Drop the second half of the index and make the file internally
    # consistent again, exactly as a tool that rewrote it would leave things.
    raw = bytearray(tidx.read_bytes())
    n = (len(raw) - UH_BYTES) // 56
    keep = max(1, n // 2)
    raw = raw[: UH_BYTES + keep * 56]
    struct.pack_into("<q", raw, 32, keep)  # number_of_entries
    struct.pack_into("<I", raw, 4, m.crc32(bytes(raw[UH_BYTES:])))
    struct.pack_into("<I", raw, 0, m.crc32(bytes(raw[4:UH_BYTES])))
    tidx.write_bytes(bytes(raw))

    report = mef3io.Validator(str(path)).validate()
    assert "index.data-coverage" in _ids(report), report.summary()
    assert not report.ok

    digest = _tree_digest(path)
    repaired = mef3io.repair_session(
        str(path), ["index.sample-count", "index.block-count", "sizing.contiguous"]
    )
    assert _tree_digest(path) == digest, "a short index must not be written back"
    assert repaired.segments_repaired == 0
    assert not any(f.repaired for f in repaired.findings)
    assert any("nothing was repaired" in s.reason for s in repaired.skipped), repaired.summary()
    assert _read_s2(tmet, "number_of_samples") == true_samples


def test_repair_refuses_a_structurally_unsound_index(tmp_path):
    """index.block-offsets firing must stop the repairs derived from that index.

    Reported-then-repaired-from-anyway is what turned a readable session into
    an unreadable one: the declarations were rewritten out of bytes the
    validator had just called unsound.
    """
    path = tmp_path / "s.mefd"
    _write(path, gap_us=0)
    tmet = _tmet(path)
    tidx = Path(tmet).with_suffix(".tidx")

    # Trailing padding past the declared entry count: the body CRC is bounded
    # by that count, so crc.index still passes and the phantom entry is read.
    tidx.write_bytes(tidx.read_bytes() + b"\x00" * 56)

    report = mef3io.Validator(str(path)).validate()
    assert "index.block-offsets" in _ids(report), report.summary()

    digest = _tree_digest(path)
    repaired = mef3io.repair_session(
        str(path),
        ["index.block-count", "header.entry-count", "times.segment-bounds",
         "times.recording-duration", "sizing.contiguous"],
    )
    assert _tree_digest(path) == digest, "an unsound index must not be written back"
    assert repaired.segments_repaired == 0
    assert any("nothing was repaired" in s.reason for s in repaired.skipped)


def test_time_checks_stand_down_when_the_offset_is_unknown(tmp_path):
    """An unreadable recording-time offset is UNKNOWN, not zero.

    Section 3 carries the recording-time offset and is level-2 encrypted by
    default, so opening an encrypted session with a level-1 password — an
    ordinary, valid thing to do — hides it. Treating that as zero made the time
    checks compare against the wrong baseline, and repairing then rewrote a
    correct session's declared bounds to garbage.

    The session here must have a NON-ZERO offset: with rto == 0 "unknown" and
    "zero" coincide and the test would prove nothing. mef3io's own writer
    always writes zero, so this uses the legacy writer.
    """
    pytest.importorskip("mef_tools", reason="legacy oracle not installed")
    from mef_tools.io import MefWriter

    path = tmp_path / "rto.mefd"
    w = MefWriter(str(path), overwrite=True, password1="lvl1", password2="lvl2")
    w.record_offset = START  # non-zero recording-time offset
    w.write_data(np.arange(1000, dtype=np.int32), "ch1", START, FS, precision=0)
    del w

    # Level 2 sees section 3 and the offset; level 1 does not. Neither may
    # report a time defect on a session whose times are correct.
    for password in ("lvl2", "lvl1"):
        report = mef3io.Validator(str(path), password=password).validate()
        assert "times.segment-bounds" not in _ids(report), (
            f"password {password!r} reported a time defect on a correct session:\n"
            + report.summary()
        )

    # ...and a repair asked for with only level-1 access must write nothing.
    digest = _tree_digest(path)
    mef3io.repair_session(str(path), ["times.segment-bounds"], password="lvl1")
    assert _tree_digest(path) == digest, "times must not be rewritten against an unknown offset"




S1_ENCRYPTION_OFFSET = 1024  # section 1, byte 0: section-2 encryption level
S2_BYTES = 10752


def _fix_tmet_crcs(raw):
    struct.pack_into("<I", raw, 4, m.crc32(bytes(raw[UH_BYTES:METADATA_FILE_BYTES])))
    struct.pack_into("<I", raw, 0, m.crc32(bytes(raw[4:UH_BYTES])))


def test_repair_preserves_bytes_it_does_not_model(tmp_path):
    """Section 2 carries a 2160-byte protected region at 6432 and a 2160-byte
    discretionary region at 8592 that this struct does not model. Re-serializing
    the section would zero all 4320 of them."""
    path = tmp_path / "s.mefd"
    _write(path)
    tmet = _tmet(path)

    raw = bytearray(tmet.read_bytes())
    raw[S2 + 6432 : S2 + 6432 + 24] = b"PROTECTED-REGION-PAYLOAD"
    raw[S2 + 8592 : S2 + 8592 + 22] = b"DISCRETIONARY-PAYLOAD!"
    _fix_tmet_crcs(raw)
    tmet.write_bytes(bytes(raw))

    _patch_s2(tmet, "maximum_difference_bytes", 0)
    mef3io.repair_session(str(path), ["sizing.difference-bytes"])

    after = tmet.read_bytes()
    assert after[S2 + 6432 : S2 + 6432 + 24] == b"PROTECTED-REGION-PAYLOAD"
    assert after[S2 + 8592 : S2 + 8592 + 22] == b"DISCRETIONARY-PAYLOAD!"
    assert _read_s2(tmet, "maximum_difference_bytes") > 0


def test_level_2_encrypted_section_2_is_re_encrypted_with_the_right_key(tmp_path):
    """meflib allows section 2 at either encryption level. Assuming level 1
    re-encrypts a level-2 section with the wrong key, leaving CRC-valid garbage
    that no reader can open."""
    path = tmp_path / "enc.mefd"
    x = np.arange(4000, dtype=np.int32)
    w = mef3io.Writer(str(path), password1="lvl1", password2="lvl2")
    w.write_int32("ch1", x, 0.5, START, FS)
    w.close()
    tmet = _tmet(path)

    # Re-stage the file as a level-2 section 2: decrypt with L1, re-encrypt
    # with L2, and say so in section 1.
    l1, l2 = m.extract_password_bytes("lvl1"), m.extract_password_bytes("lvl2")
    raw = bytearray(tmet.read_bytes())
    plain = bytearray(m.aes128_ecb_decrypt(bytes(raw[S2 : S2 + S2_BYTES]), l1))
    struct.pack_into("<I", plain, 6388, 0)  # clear maximum_difference_bytes
    raw[S2 : S2 + S2_BYTES] = m.aes128_ecb_encrypt(bytes(plain), l2)
    raw[S1_ENCRYPTION_OFFSET] = 2
    _fix_tmet_crcs(raw)
    tmet.write_bytes(bytes(raw))

    validator = mef3io.Validator(str(path), password="lvl2")
    assert "sizing.difference-bytes" in _ids(validator.validate())
    assert _repair(validator, ["sizing.difference-bytes"]).segments_repaired == 1

    # It must still decrypt with the key section 1 names.
    after = m.aes128_ecb_decrypt(
        bytes(bytearray(tmet.read_bytes())[S2 : S2 + S2_BYTES]), l2
    )
    assert struct.unpack_from("<I", after, 6388)[0] > 0
    assert struct.unpack_from("<d", after, 6160)[0] == FS, "sampling frequency survived"
    with mef3io.Reader(str(path), password="lvl2") as r:
        np.testing.assert_array_equal(r.read_raw("ch1")["samples"], x)


def test_tdat_backup_copies_only_the_universal_header(tmp_path):
    """Only a .tdat's first 1024 bytes are ever rewritten. Copying the whole
    file to protect them would be a multi-gigabyte write on a real session."""
    path = tmp_path / "s.mefd"
    _write(path)
    tdat = _tmet(path).with_suffix(".tdat")
    assert tdat.stat().st_size > 4 * UH_BYTES, "need a body worth not copying"

    _patch_uh(tdat, "maximum_entry_size", 2)
    header_before = tdat.read_bytes()[:UH_BYTES]
    mef3io.repair_session(str(path), ["header.max-entry-size"])

    backups = list(Path(str(path) + ".repair-backup").rglob("*.tdat.universal-header"))
    assert len(backups) == 1
    assert backups[0].stat().st_size == UH_BYTES, "the whole .tdat must not be copied"
    assert backups[0].read_bytes() == header_before, "and it must be the pre-repair header"
    assert tdat.read_bytes()[:UH_BYTES] != header_before, "the live header did change"


def test_backup_stays_outside_a_session_given_with_a_trailing_separator(tmp_path):
    """Shell tab-completion produces 's.mefd/'. Naive concatenation then puts
    the backup inside the session, where an archive would pack it."""
    path = tmp_path / "s.mefd"
    _write(path)
    _patch_s2(_tmet(path), "maximum_difference_bytes", 0)

    mef3io.repair_session(str(path) + os.sep, ["sizing.difference-bytes"])
    assert Path(str(path) + ".repair-backup").is_dir()
    assert not (path / ".repair-backup").exists()


def test_empty_index_is_reported_never_repaired(tmp_path):
    """A .tidx truncated to its header is CRC-valid (an empty body hashes to the
    seed). Deriving a truth from it would write zeros over the only remaining
    record of what the intact .tdat holds."""
    path = tmp_path / "s.mefd"
    _write(path)
    tmet = _tmet(path)
    tidx = tmet.with_suffix(".tidx")
    before = _read_s2(tmet, "number_of_samples")
    assert before > 0

    head = bytearray(tidx.read_bytes()[:UH_BYTES])
    struct.pack_into("<I", head, 4, m.crc32(b""))
    struct.pack_into("<I", head, 0, m.crc32(bytes(head[4:UH_BYTES])))
    tidx.write_bytes(bytes(head))

    validator = mef3io.Validator(str(path))
    report = validator.validate()
    assert not report.ok
    assert report.skipped and "damaged" in report.skipped[0].reason
    digest = _tree_digest(path)
    _repair(validator, ["index.sample-count", "times.segment-bounds"])
    assert _tree_digest(path) == digest, "a damaged segment must not be rewritten"
    assert _read_s2(tmet, "number_of_samples") == before


def test_crc_no_entry_sentinel_is_not_corruption(tmp_path):
    """CRC_START_VALUE is meflib's 'no entry'. metadata.cpp accepts it, so the
    validator must not call such a session corrupt — it reads perfectly."""
    path = tmp_path / "s.mefd"
    _write(path)
    tmet = _tmet(path)
    raw = bytearray(tmet.read_bytes())
    struct.pack_into("<I", raw, 4, 0xFFFFFFFF)  # body_CRC = no entry
    struct.pack_into("<I", raw, 0, m.crc32(bytes(raw[4:UH_BYTES])))
    tmet.write_bytes(bytes(raw))

    with mef3io.Reader(str(path)) as r:
        assert len(r.read_raw("ch1")["samples"]) > 0
    report = mef3io.Validator(str(path)).validate()
    assert "crc.metadata" not in _ids(report)
    assert report.ok, report.summary()


def test_index_trailing_padding_is_tolerated(tmp_path):
    """Foreign writers pad past the last index entry; hashing the padding would
    reject an intact block table."""
    path = tmp_path / "s.mefd"
    _write(path)
    tidx = _tmet(path).with_suffix(".tidx")
    tidx.write_bytes(tidx.read_bytes() + b"\x00" * 16)

    report = mef3io.Validator(str(path)).validate()
    assert "crc.index" not in _ids(report)
    assert report.ok, report.summary()


def test_contiguous_severity_follows_the_direction(tmp_path):
    """Under-declaring is an error; over-declaring is only a warning.

    The two are not equally dangerous. A reader that sizes a run buffer from
    these fields truncates it when they are too small — and `0` is not the si8
    NO_ENTRY sentinel, so it reads as a real zero. Declaring too much only
    wastes memory.

    This must not be downgraded on the grounds that no reader in
    reference_files consumes the trio: that is one meflib build, and a deployed
    build is known to allocate from section-2 sizes that the vendored one
    ignores.
    """
    path = tmp_path / "s.mefd"
    _write(path)
    tmet = _tmet(path)
    truth = _read_s2(tmet, "maximum_contiguous_samples")
    assert truth > 0

    _patch_s2(tmet, "maximum_contiguous_samples", 0)
    under = [f for f in mef3io.Validator(str(path)).validate().findings
             if f.check_id == "sizing.contiguous"]
    assert under and under[0].severity == "error", "under-declaration truncates"
    assert not mef3io.Validator(str(path)).validate().ok

    _patch_s2(tmet, "maximum_contiguous_samples", truth * 1000)
    over = [f for f in mef3io.Validator(str(path)).validate().findings
            if f.check_id == "sizing.contiguous"]
    assert over and over[0].severity == "warning", "over-declaration only wastes"


def test_contiguous_repair_states_what_the_index_holds(tmp_path):
    """The declaration describes the data, in whichever direction it is wrong.

    Over-declaring is the case seen in the field (recorders over-declare these
    by orders of magnitude, wasting allocation that scales with channel count);
    under-declaring truncates a reader's run buffer. Both are corrected to the
    longest run between discontinuity flags.
    """
    path = tmp_path / "s.mefd"
    _write(path)
    tmet = _tmet(path)
    truth_samples = _read_s2(tmet, "maximum_contiguous_samples")
    truth_bytes = _read_s2(tmet, "maximum_contiguous_block_bytes")
    assert truth_samples > 0 and truth_bytes > 0, "writer must measure these"

    _patch_s2(tmet, "maximum_contiguous_samples", 10**9)  # over
    _patch_s2(tmet, "maximum_contiguous_block_bytes", 0)  # under

    mef3io.repair_session(str(path), ["sizing.contiguous"])
    assert _read_s2(tmet, "maximum_contiguous_samples") == truth_samples, "over-declaration lowered"
    assert _read_s2(tmet, "maximum_contiguous_block_bytes") == truth_bytes, "under-declaration raised"
    assert "sizing.contiguous" not in _ids(mef3io.Validator(str(path)).validate())


def test_repaired_means_bytes_changed(tmp_path):
    """`repaired` and segments_repaired must track what was actually written.

    A repair may decline (RepairFn returns whether it changed a declaration).
    Reporting a declined one as repaired would tell an operator a session was
    fixed while the defect stayed on disk, let Report.ok discount an
    error-severity finding, and make a "repair until clean" loop non-terminating.
    Both halves of the correspondence are pinned here.
    """
    path = tmp_path / "s.mefd"
    _write(path)
    tmet = _tmet(path)

    # Nothing wrong -> nothing written, nothing claimed.
    clean = tmet.read_bytes()
    report = mef3io.repair_session(str(path), ["sizing.contiguous"])
    assert report.segments_repaired == 0
    assert not any(f.repaired for f in report.findings)
    assert tmet.read_bytes() == clean, "a clean session must not be rewritten"

    # Something wrong -> written, and claimed exactly once.
    _patch_s2(tmet, "maximum_contiguous_samples", 22_129_876)
    report = mef3io.repair_session(str(path), ["sizing.contiguous"])
    hits = [f for f in report.findings if f.check_id == "sizing.contiguous"]
    assert hits and all(f.repaired for f in hits)
    assert report.segments_repaired == 1
    # Restoring the one corrupted field reproduces the original file exactly,
    # CRCs included — the repair touched that field and nothing else.
    assert tmet.read_bytes() == clean
    assert "sizing.contiguous" not in _ids(mef3io.Validator(str(path)).validate())


def test_garbage_block_header_never_becomes_the_declaration(tmp_path):
    """.tdat carries no CRC check, so a corrupt block header can present any
    value. Writing it back would install the NO_ENTRY sentinel the check exists
    to remove, and the repair would never converge."""
    path = tmp_path / "s.mefd"
    _write(path)
    tmet = _tmet(path)
    tdat = tmet.with_suffix(".tdat")
    raw = bytearray(tdat.read_bytes())
    struct.pack_into("<I", raw, UH_BYTES + 28, 0xFFFFFFFF)  # first block's difference_bytes
    tdat.write_bytes(bytes(raw))
    _patch_s2(tmet, "maximum_difference_bytes", 0)

    mef3io.repair_session(str(path), ["sizing.difference-bytes"])
    stored = _read_s2(tmet, "maximum_difference_bytes")
    assert stored not in (0, 0xFFFFFFFF)
    assert stored <= 5 * _read_s2(tmet, "maximum_block_samples")
    assert mef3io.Validator(str(path)).validate().ok, "the repair must converge"


# --- API and CLI safety regressions -----------------------------------------


def test_a_filter_matching_nothing_is_not_a_clean_result(tmp_path):
    """A typo'd channel once examined zero segments, found zero problems, and
    exited 0 — a CI gate written that way would pass forever."""
    path = tmp_path / "s.mefd"
    _write(path)
    _patch_s2(_tmet(path), "maximum_difference_bytes", 0)

    report = mef3io.Validator(str(path), channels=["nosuchchannel"]).validate()
    assert report.segments_checked == 0
    assert not report.ok
    assert "NOT a clean result" in report.summary()
    assert mef3io.Validator(str(path), segments=[42]).validate().ok is False


@pytest.mark.parametrize("kwargs", [{"channels": "ch1"}, {"segments": "0"}])
def test_a_bare_string_filter_is_rejected(tmp_path, kwargs):
    """list("ch1") is ['c','h','1'] — which matches no channel and would have
    reported the session clean."""
    with pytest.raises(TypeError, match="not a single string"):
        mef3io.Validator(str(tmp_path / "s.mefd"), **kwargs)


def test_repair_rejects_a_bare_string_of_check_ids(tmp_path):
    path = tmp_path / "s.mefd"
    _write(path)
    with pytest.raises(TypeError, match="not a single string"):
        mef3io.repair_session(str(path), "sizing.difference-bytes")


def test_summary_keeps_pointing_at_what_is_still_repairable(tmp_path):
    path = tmp_path / "s.mefd"
    _write(path)
    tmet = _tmet(path)
    _patch_s2(tmet, "maximum_difference_bytes", 0)
    _patch_s2(tmet, "block_interval", 0)

    report = mef3io.repair_session(str(path), ["sizing.difference-bytes"])
    assert "times.block-interval" in report.repairable_check_ids
    assert "sizing.difference-bytes" not in report.repairable_check_ids
    assert "Still repairable" in report.summary()


def test_by_check_follows_registry_order(tmp_path):
    path = tmp_path / "s.mefd"
    _write(path)
    tmet = _tmet(path)
    _patch_s2(tmet, "block_interval", 0)
    _patch_s2(tmet, "number_of_blocks", 99)

    grouped = list(mef3io.Validator(str(path)).validate().by_check())
    registry = [c.id for c in mef3io.available_checks()]
    assert grouped == sorted(grouped, key=registry.index)


def test_legacy_drop_in_warns_like_the_native_reader(tmp_path):
    """MefReader is the entry point legacy users actually use, so it must carry
    the same warning as mef3io.Reader."""
    path = tmp_path / "s.mefd"
    _write(path)
    _patch_s2(_tmet(path), "maximum_difference_bytes", 0)

    with warnings.catch_warnings(record=True) as caught:
        warnings.simplefilter("always")
        mef3io.MefReader(str(path))
    assert [w for w in caught if issubclass(w.category, mef3io.SessionDeclarationWarning)]


def test_reader_exposes_the_structured_declaration_issues(tmp_path):
    path = tmp_path / "s.mefd"
    _write(path)
    _patch_s2(_tmet(path), "maximum_difference_bytes", 0)

    with mef3io.Reader(str(path), warn_declarations=False) as r:
        issues = r.declaration_issues
    assert [i["field"] for i in issues] == ["maximum_difference_bytes"]
    assert issues[0]["channel"] == "ch1"


def test_cli_reports_bad_input_without_a_traceback(tmp_path):
    env = dict(os.environ, PYTHONPATH=str(Path(mef3io.__file__).resolve().parent.parent))
    path = tmp_path / "s.mefd"
    _write(path)
    digest_before_bad_input = _tree_digest(path)

    missing = subprocess.run(
        [sys.executable, "-m", "mef3io", "validate", str(tmp_path / "nope.mefd")],
        capture_output=True, text=True, env=env,
    )
    assert missing.returncode == 2, "a usage error must differ from 'session has defects'"
    assert "Traceback" not in missing.stderr
    assert missing.stderr.startswith("error: ")

    bad_check = subprocess.run(
        [sys.executable, "-m", "mef3io", "validate", str(path), "--check", "nope.id"],
        capture_output=True, text=True, env=env,
    )
    assert bad_check.returncode == 2
    assert "Traceback" not in bad_check.stderr
    assert "--list-checks" in bad_check.stderr

    # `repair` without a --check is the "fix everything" shortcut that must not
    # exist: it has to name what it is about to rewrite.
    unselected = subprocess.run(
        [sys.executable, "-m", "mef3io", "repair", str(path)],
        capture_output=True, text=True, env=env,
    )
    assert unselected.returncode == 2, "repair must refuse an empty selection"
    assert "Traceback" not in unselected.stderr
    assert _tree_digest(path) == digest_before_bad_input


def test_cli_reads_a_password_from_the_environment(tmp_path):
    """A password on argv lands in ps output, shell history and CI logs."""
    path = tmp_path / "enc.mefd"
    w = mef3io.Writer(str(path), password1="lvl1", password2="lvl2")
    w.write_int32("ch1", np.arange(2000, dtype=np.int32), 1.0, START, FS)
    w.close()

    env = dict(
        os.environ,
        PYTHONPATH=str(Path(mef3io.__file__).resolve().parent.parent),
        MEF_PW="lvl2",
    )
    out = subprocess.run(
        [sys.executable, "-m", "mef3io", "validate", str(path), "--password-env", "MEF_PW"],
        capture_output=True, text=True, env=env,
    )
    assert out.returncode == 0, out.stdout + out.stderr
    assert "1 segment(s) checked" in out.stdout


def test_docs_check_table_matches_the_registry():
    """The table in docs/validation.md is the reference users act on; it must
    not drift from the registry (it already did once, on severities)."""
    import re

    doc = (REPO / "docs" / "validation.md").read_text()
    rows = re.findall(r"^\| `([a-z.\-]+)` \| (\w+) \| (yes|no) \|", doc, re.M)
    documented = [(cid, sev, rep == "yes") for cid, sev, rep in rows]
    actual = [(c.id, c.severity, c.repairable) for c in mef3io.available_checks()]
    assert documented == actual
