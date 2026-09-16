"""P13: the session Validator — checking declarations against the data, and
repairing only what the caller explicitly selects.

Two contracts are pinned hard here, because they are what makes the tool safe
to point at real recordings:

* ``validate()`` never writes a byte.
* ``repair()`` writes only the checks named in its argument, and refuses an
  empty selection — there is no implicit "fix everything".
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
    "index.start-sample": (lambda t, path: _patch_s2(t, "start_sample", 5), "start_sample"),
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
    repaired = validator.fix(check_id)
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

    report = mef3io.Validator(str(path)).repair(["sizing.difference-bytes"])
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

    report = mef3io.Validator(str(path), channels=["ch1"]).repair(["sizing.difference-bytes"])
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
        mef3io.Validator(str(path)).repair([])
    assert _tree_digest(path) == before


@pytest.mark.parametrize("bad", ["no.such.check", "crc.metadata"])
def test_repair_rejects_unknown_or_unrepairable_checks(tmp_path, bad):
    path = tmp_path / "s.mefd"
    _write(path)
    before = _tree_digest(path)
    with pytest.raises(ValueError):
        mef3io.Validator(str(path)).repair([bad])
    assert _tree_digest(path) == before


def test_repair_of_a_clean_session_changes_nothing(tmp_path):
    path = tmp_path / "s.mefd"
    _write(path)
    before = _tree_digest(path)
    report = mef3io.Validator(str(path)).repair(["sizing.difference-bytes"])
    assert report.segments_repaired == 0
    assert _tree_digest(path) == before


# --- backups -----------------------------------------------------------------


def test_repair_backs_up_before_rewriting(tmp_path):
    path = tmp_path / "s.mefd"
    _write(path)
    tmet = _tmet(path)
    _patch_s2(tmet, "maximum_difference_bytes", 0)
    original = tmet.read_bytes()

    mef3io.Validator(str(path)).repair(["sizing.difference-bytes"])
    backups = list(Path(str(path) + ".repair-backup").rglob("*.tmet"))
    assert len(backups) == 1
    assert backups[0].read_bytes() == original
    assert tmet.read_bytes() != original

    # A second repair must not clobber the pristine backup.
    _patch_s2(tmet, "maximum_difference_bytes", 0)
    mef3io.Validator(str(path)).repair(["sizing.difference-bytes"])
    assert backups[0].read_bytes() == original


def test_backup_can_be_declined(tmp_path):
    path = tmp_path / "s.mefd"
    _write(path)
    _patch_s2(_tmet(path), "maximum_difference_bytes", 0)
    mef3io.Validator(str(path)).repair(["sizing.difference-bytes"], backup=False)
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

    report = mef3io.Validator(str(path)).repair(["index.block-count"])
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
    report = validator.repair(validator.validate().repairable_check_ids)
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

    report = validator.repair(["sizing.difference-bytes"])
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
        mef3io.Validator(archive).repair(["sizing.difference-bytes"])


# --- the fast path -----------------------------------------------------------


def test_fast_mode_still_catches_an_unset_difference_bytes(tmp_path):
    path = tmp_path / "s.mefd"
    _write(path)
    tmet = _tmet(path)
    _patch_s2(tmet, "maximum_difference_bytes", 0)

    validator = mef3io.Validator(str(path), exact_difference_bytes=False)
    assert "sizing.difference-bytes" in _ids(validator.validate())
    validator.fix("sizing.difference-bytes")
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
    assert "repair(" in text, "the summary should say how to opt in to the fix"


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

    fixed = subprocess.run(
        [sys.executable, "-m", "mef3io", "validate", str(path),
         "--repair", "sizing.difference-bytes"],
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
    mef3io.Validator(str(path)).fix("sizing.difference-bytes")

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
    assert not report.errors, "none of the legacy quirks are reader-fatal"
