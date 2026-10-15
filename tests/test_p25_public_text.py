"""User-facing text must not name private third-party products.

The README (also the PyPI page), the documentation site, the examples, the
package metadata and the Python package's docstrings and messages are what
users read. Internal notes (CLAUDE.md, code comments, tests, scripts) are
exempt. The docs workflow runs the same check over the built site, which also
covers the generated API reference.
"""
from __future__ import annotations

import base64
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# Kept encoded so this file does not itself spell out the names it guards.
_FORBIDDEN = [base64.b64decode(b).decode() for b in (b"Y3liZXJwc2c=",)]

PUBLIC = ["README.md", "pyproject.toml", "docs", "examples", "python/mef3io"]
SUFFIXES = {".md", ".py", ".toml", ".txt", ".yml", ".yaml", ".cff", ".m"}


def _public_files():
    for rel in PUBLIC:
        p = ROOT / rel
        if p.is_file():
            yield p
        elif p.is_dir():
            yield from (f for f in p.rglob("*") if f.is_file() and f.suffix in SUFFIXES)


def test_public_text_names_no_private_product():
    files = list(_public_files())
    assert any(f.name == "README.md" for f in files)  # the scan saw something
    hits = []
    for f in files:
        text = f.read_text(encoding="utf-8", errors="replace").lower()
        for name in _FORBIDDEN:
            if name in text:
                hits.append(str(f.relative_to(ROOT)))
    assert not hits, f"private product name found in user-facing files: {sorted(set(hits))}"
