"""P24: examples that docs pages rely on stay runnable.

docs/anonymization.md points readers at examples/11_legacy_rto_sessions.py as
the proof that mef3io reads and extends a legacy recording-time-offset
session consistently. The example checks itself and exits non-zero on any
mismatch; this runs it so a regression shows up in the suite, not in a
reader's terminal.
"""
from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

import pytest

import mef3io

REPO = Path(__file__).resolve().parent.parent


def test_legacy_rto_example_runs_clean():
    pytest.importorskip("mef_tools", reason="legacy oracle not installed")
    pytest.importorskip("pymef", reason="legacy oracle not installed")
    # Point the subprocess at whichever mef3io this process imported (the
    # in-tree package after dev_build.sh, the installed one on CI).
    # Prepend, so a local mef_tools checkout on the caller's path stays visible.
    paths = [str(Path(mef3io.__file__).resolve().parent.parent), os.environ.get("PYTHONPATH", "")]
    env = dict(os.environ, PYTHONPATH=os.pathsep.join(p for p in paths if p))
    run = subprocess.run(
        [sys.executable, str(REPO / "examples" / "11_legacy_rto_sessions.py")],
        capture_output=True, text=True, env=env, timeout=600,
    )
    assert run.returncode == 0, run.stdout + run.stderr
    assert "all checks passed" in run.stdout
    assert "FAIL" not in run.stdout
