"""P1 gate: C++ struct parsing + CRC + decryption must reproduce, for every
golden .tmet, the metadata the legacy stack recorded in manifest.json."""
import json
from pathlib import Path

import pytest

from mef3io import _mef3io as m

GOLDEN = Path(__file__).parent / "golden"
MANIFEST = json.loads((GOLDEN / "manifest.json").read_text())


def _tmet_paths(session_dir: Path):
    return sorted(session_dir.glob("*.timd/*.segd/*.tmet"))


@pytest.mark.parametrize("case", MANIFEST["cases"], ids=lambda c: c["name"])
def test_tmet_matches_manifest(case):
    session = GOLDEN / f"{case['name']}.mefd"
    password = case["password"] or ""
    for tmet in _tmet_paths(session):
        raw = tmet.read_bytes()
        md = m.parse_tmet(raw, password)
        cname = md["channel_name"]
        assert cname in case["channels"], f"{tmet}: unexpected channel {cname}"
        exp = case["channels"][cname]
        assert md["sampling_frequency"] == pytest.approx(exp["fs"])
        assert md["units_conversion_factor"] == pytest.approx(exp["ufact"])
        # start_sample==0 segments hold the channel-wide start; multi-segment
        # channels have per-segment values, so only assert fs/ufact per file.


def test_crc_detects_corruption():
    tmet = _tmet_paths(GOLDEN / "plain_single.mefd")[0]
    raw = bytearray(tmet.read_bytes())
    raw[100] ^= 0xFF  # flip a byte inside the universal header body
    with pytest.raises(Exception):
        m.parse_tmet(bytes(raw), "")


def test_wrong_password_rejected():
    case = next(c for c in MANIFEST["cases"] if c["name"] == "enc_both")
    tmet = _tmet_paths(GOLDEN / "enc_both.mefd")[0]
    raw = tmet.read_bytes()
    # correct password works
    md = m.parse_tmet(raw, case["password"])
    assert md["access_level"] == 2
    # wrong password raises
    with pytest.raises(Exception):
        m.parse_tmet(raw, "wrongpass")


def test_level1_password_gives_partial_access():
    """Reading an enc_both file with only the L1 password: section 2 decrypts
    (access level 1), section 3 is unavailable."""
    case = next(c for c in MANIFEST["cases"] if c["name"] == "enc_both")
    tmet = _tmet_paths(GOLDEN / "enc_both.mefd")[0]
    raw = tmet.read_bytes()
    md = m.parse_tmet(raw, case["password1"])
    assert md["access_level"] == 1
    assert md["section3_available"] is False
    # section 2 still readable
    assert md["sampling_frequency"] == pytest.approx(case["channels"]["ch1"]["fs"])


def test_universal_header_roundtrip_byte_identical():
    """Parse -> serialize of the universal header reproduces the on-disk bytes,
    validating the write-side field offsets."""
    for case in MANIFEST["cases"]:
        for tmet in _tmet_paths(GOLDEN / f"{case['name']}.mefd"):
            raw = tmet.read_bytes()
            out = m.roundtrip_universal_header(raw)
            assert out == raw[:1024], f"UH round-trip mismatch in {tmet}"
