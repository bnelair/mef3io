"""#80 — RED block statistics encryption follows meflib: 16 bytes, not 256.

meflib encrypts ONE AES block, bytes 0..15 of a RED block's 256-byte statistics
table (AES_cipher on a single 4x4 state, meflib.c:292; RED_encode :7027,
RED_decode :6676; block CRC over the ciphertext, :7045). mef3io used to cipher
all 256 bytes. That is self-consistent, so every mef3io round trip passed, but
it decoded meflib's encrypted blocks wrong and wrote blocks meflib cannot read.

The fixture ``golden/meflib_red_encrypted.mefd`` was produced by the legacy
stack and then had its blocks encrypted by the VENDORED MEFLIB's own RED_encode
(``tests/make_meflib_encrypted_fixture.py`` + ``tests/meflib_harness/red_blocks.c``)
— never by mef3io. Channel ``l1_blocks`` is level-1 encrypted, ``l2_blocks``
level-2. The .npz holds the int32 samples pymef decoded BEFORE encryption.

pymef itself cannot read encrypted RED blocks at all (its read path never sets
``rps->password_data``, pymef3_file.c:2151, so RED_decode dereferences NULL at
meflib.c:6675). The reverse direction is therefore pinned against meflib's
bytes directly: mef3io re-encoding each block must reproduce meflib's block
byte for byte, which is strictly stronger than "pymef decodes it".
"""
from __future__ import annotations

import hashlib
from pathlib import Path

import numpy as np
import pytest

from mef3io import _mef3io as m

GOLDEN = Path(__file__).parent / "golden"
SESSION = GOLDEN / "meflib_red_encrypted.mefd"
TRUTH = np.load(GOLDEN / "meflib_red_encrypted.npz")
LEVELS = {"l1_blocks": 1, "l2_blocks": 2}
MASK = {1: 0x02, 2: 0x04}
UH = 1024
STATS = 48  # statistics table offset inside the RED block header


def _segment(ch: str) -> Path:
    seg = SESSION / f"{ch}.timd" / f"{ch}-000000.segd"
    return seg / f"{ch}-000000"


def test_bindings_reject_short_validation_fields():
    with pytest.raises(ValueError):
        m.red_decode_block(_blocks("l1_blocks")[0], "pass2", b"\0" * 4, b"\0" * 16)
    with pytest.raises(ValueError):
        m.red_encode_block(np.arange(10, dtype=np.int32), 0, True, 0, "", b"\0" * 16, b"")


def _fields(ch: str) -> tuple[bytes, bytes]:
    uh = Path(f"{_segment(ch)}.tmet").read_bytes()[:UH]
    return uh[868:884], uh[884:900]


def _blocks(ch: str) -> list[bytes]:
    tdat = Path(f"{_segment(ch)}.tdat").read_bytes()
    out, off = [], UH
    while off < len(tdat):
        n = int.from_bytes(tdat[off + 36:off + 40], "little")
        out.append(tdat[off:off + n])
        off += n
    return out


def _key(ch: str, password: str = "pass2") -> bytes:
    """The raw key meflib uses for this channel's blocks (L2 password given)."""
    l1f, l2f = _fields(ch)
    l2 = m.extract_password_bytes(password)
    if LEVELS[ch] == 2:
        return l2
    sha = hashlib.sha256(l2).digest()
    return bytes(a ^ b for a, b in zip(sha[:16], l2f))  # putative L1 password bytes


def test_fixture_blocks_are_meflib_encrypted():
    for ch, level in LEVELS.items():
        blocks = _blocks(ch)
        assert len(blocks) >= 2, ch
        for b in blocks:
            assert b[4] & MASK[level], (ch, "block not flagged encrypted")


@pytest.mark.parametrize("n_threads", [1, 4])
def test_mef3io_reads_meflib_encrypted_blocks(n_threads):
    r = m.Reader(str(SESSION), "pass2")
    for ch in LEVELS:
        raw = r.read_raw(ch, None, None, n_threads)
        assert np.asarray(raw["valid"]).astype(bool).all(), ch
        np.testing.assert_array_equal(np.asarray(raw["samples"]), TRUTH[ch], err_msg=ch)


def test_level1_password_reads_level1_blocks_only():
    # A DELIBERATE divergence from meflib, which needs the level-2 password even
    # here: process_password_data stores a level-1-only password's key in the
    # level-2 slot (meflib.c:5080), leaving its level-1 key zero.
    r = m.Reader(str(SESSION), "pass1")
    raw = r.read_raw("l1_blocks", None, None)
    np.testing.assert_array_equal(np.asarray(raw["samples"]), TRUTH["l1_blocks"])
    # A level-2 block needs the level-2 password.
    with pytest.raises(m.PasswordError, match="level-2"):
        r.read_raw("l2_blocks", None, None)


def test_block_by_block_decode_matches_truth():
    for ch in LEVELS:
        l1f, l2f = _fields(ch)
        got = np.concatenate([m.red_decode_block(b, "pass2", l1f, l2f)["samples"]
                              for b in _blocks(ch)])
        np.testing.assert_array_equal(got, TRUTH[ch], err_msg=ch)


def test_mef3io_encodes_encrypted_blocks_byte_identical_to_meflib():
    """The reverse direction: what mef3io writes is exactly what meflib wrote."""
    for ch, level in LEVELS.items():
        l1f, l2f = _fields(ch)
        for i, b in enumerate(_blocks(ch)):
            dec = m.red_decode_block(b, "pass2", l1f, l2f)
            start = int.from_bytes(b[40:48], "little", signed=True)
            ours = m.red_encode_block(dec["samples"], start, bool(dec["discontinuity"]),
                                      level, "pass2", l1f, l2f)
            assert ours == b, f"{ch} block {i}: mef3io bytes differ from meflib's"


def test_only_the_first_16_statistics_bytes_are_ciphertext():
    """The fixture discriminates: deciphering all 256 bytes (the pre-#80 rule)
    does NOT recover the block, deciphering 16 does."""
    for ch in LEVELS:
        key = _key(ch)
        b = bytearray(_blocks(ch)[0])
        stats = bytes(b[STATS:STATS + 256])

        def plain_block(table: bytes) -> bytes:
            out = bytearray(b)
            out[STATS:STATS + 256] = table
            out[4] &= ~0x06 & 0xFF
            out[0:4] = m.crc32(bytes(out[4:])).to_bytes(4, "little")
            return bytes(out)

        right = m.aes128_ecb_decrypt(stats[:16], key) + stats[16:]
        dec = m.red_decode_block(plain_block(right), "", b"\0" * 16, b"\0" * 16)
        np.testing.assert_array_equal(dec["samples"], TRUTH[ch][:dec["samples"].size])

        wrong = m.aes128_ecb_decrypt(stats, key)
        assert wrong[:16] == right[:16] and wrong[16:] != right[16:]
        try:
            bad = m.red_decode_block(plain_block(wrong), "", b"\0" * 16, b"\0" * 16)
        except (m.FormatError, m.CrcError):
            continue  # scrambled statistics may not even decode
        assert not np.array_equal(bad["samples"], TRUTH[ch][:bad["samples"].size]), ch
