"""P24: examples that docs pages rely on stay runnable.

docs/anonymization.md points readers at examples/11_legacy_rto_sessions.py as
the proof that mef3io reads and extends a legacy recording-time-offset
session consistently. The example checks itself and exits non-zero on any
mismatch; this runs it so a regression shows up in the suite, not in a
reader's terminal. The same page teaches examples/13_anonymize.py, which needs
nothing beyond mef3io and is run the same way.
"""
from __future__ import annotations

import os
import re
import subprocess
import sys
from pathlib import Path

import pytest

import mef3io

REPO = Path(__file__).resolve().parent.parent


def _run_example(name):
    # Point the subprocess at whichever mef3io this process imported (the
    # in-tree package after dev_build.sh, the installed one on CI).
    # Prepend, so a local mef_tools checkout on the caller's path stays visible.
    paths = [str(Path(mef3io.__file__).resolve().parent.parent), os.environ.get("PYTHONPATH", "")]
    env = dict(os.environ, PYTHONPATH=os.pathsep.join(p for p in paths if p))
    run = subprocess.run(
        [sys.executable, str(REPO / "examples" / name)],
        capture_output=True, text=True, env=env, timeout=600,
    )
    assert run.returncode == 0, run.stdout + run.stderr
    return run


def test_legacy_rto_example_runs_clean():
    pytest.importorskip("mef_tools", reason="legacy oracle not installed")
    pytest.importorskip("pymef", reason="legacy oracle not installed")
    run = _run_example("11_legacy_rto_sessions.py")
    assert "all checks passed" in run.stdout
    assert "FAIL" not in run.stdout


def test_anonymize_example_runs_clean():
    run = _run_example("13_anonymize.py")
    out = run.stdout
    assert "refused: name-contains-identity" in out
    assert "verification: passed" in out
    assert "copy starts: 2000-01-01T09:00:00+00:00" in out
    assert "4. copy without a shift: S002.mefd - times kept" in out
    assert "the source is unchanged; done" in out
    # The planted overlap (the ID "42" inside the kept channel LAH42) is a note.
    assert "note: LAH42.timd" in out and "notes: 2" in out
    # The example promises not to print the original recording date
    # (2024-06-03, fictional), its year, nor the shift that would give it back.
    assert "2024" not in out and "1717" not in out
    assert "shifted by" not in out
    # No uUTC time or shift of any kind (12+ digits) reaches the output.
    assert not re.search(r"\d{12,}", out), out
