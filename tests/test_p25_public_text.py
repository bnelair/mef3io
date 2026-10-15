"""User-facing text must not name private third-party products.

What users read: the README (also the PyPI page), the documentation site
source, the examples, the package metadata, contributor-facing files, the
Python package's docstrings and messages, the extension's docstrings, the
MATLAB classes' help text and README, and the C++ public headers (Doxygen)
and core sources (validator findings, exceptions and warnings reach users).
Exempt by decision: CLAUDE.md, tests, scripts. The docs workflow runs the
same pattern over the built site, which also covers the generated API
reference — keep the encoded pattern there in sync with this one.
"""
from __future__ import annotations

import base64
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# Kept encoded so this file does not itself spell out the name it guards; the
# pattern also matches spaced/hyphenated/underscored spellings ([ _-]: the same
# in Python re and grep -E). Same value as
# the "Check the built site" step in .github/workflows/docs.yml.
_PATTERN = re.compile(base64.b64decode(b"Y3liZXJbIF8tXSpwc2c=").decode(), re.IGNORECASE)

PUBLIC = [
    "README.md",
    "pyproject.toml",
    "CITATION.cff",
    "CONTRIBUTING.md",
    "SECURITY.md",
    "METADATA.md",
    ".github/PULL_REQUEST_TEMPLATE.md",
    ".github/ISSUE_TEMPLATE",
    "docs",
    "examples",
    "python/mef3io",
    "bindings",
    "matlab",
    "core/include",
    "core/src",
]
SUFFIXES = {
    ".md", ".py", ".toml", ".txt", ".yml", ".yaml", ".cff", ".m",
    ".html", ".cpp", ".hpp", ".h", ".c",
}


def _public_files():
    for rel in PUBLIC:
        p = ROOT / rel
        if p.is_file():
            yield p
        elif p.is_dir():
            yield from (f for f in p.rglob("*") if f.is_file() and f.suffix in SUFFIXES)


def test_pattern_matches_the_spellings_it_guards():
    name = base64.b64decode(b"Y3liZXJwc2c=").decode()  # the plain name, lower case
    for variant in (name, name.upper(), name[:5] + " " + name[5:], name[:5] + "-" + name[5:]):
        assert _PATTERN.search(variant), variant


def test_public_text_names_no_private_product():
    files = list(_public_files())
    names = {f.name for f in files}
    assert {"README.md", "_reader.py", "validate.cpp"} <= names  # the scan saw them
    hits = sorted(
        str(f.relative_to(ROOT))
        for f in files
        if _PATTERN.search(f.read_text(encoding="utf-8", errors="replace"))
    )
    assert not hits, f"private product name found in user-facing files: {hits}"
