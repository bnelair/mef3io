"""Test path setup for the self-contained mef3io package.

Makes the mef3io package importable without needing PYTHONPATH, and locates the
legacy `mef_tools` package (the correctness oracle) by walking up the directory
tree — so the suite runs both in place (inside the original mef_tools repo) and,
later, when mef3io/ is extracted to its own repo (where the legacy-oracle tests
should be guarded with pytest.importorskip).
"""
import sys
from pathlib import Path

MEF3IO_ROOT = Path(__file__).resolve().parent.parent  # mef3io/

# mef3io package: prefer the in-tree package only when its compiled extension
# has been built (scripts/dev_build.sh symlinks it in). Otherwise fall through
# to an installed mef3io (e.g. `pip install ".[test]"` on CI) — inserting the
# source tree unconditionally would shadow the installed extension and leave
# the package without a backend.
_pkg = MEF3IO_ROOT / "python" / "mef3io"
_ext = [p for pat in ("_mef3io*.so", "_mef3io*.pyd") for p in _pkg.glob(pat)]
if any(p.exists() for p in _ext):  # .exists() also filters dangling symlinks
    sys.path.insert(0, str(MEF3IO_ROOT / "python"))

# legacy mef_tools oracle: prefer a local checkout when running in the original
# repo (matches the golden fixtures), otherwise fall through to a pip-installed
# `mef-tools` (`pip install mef3io[test]`). If neither is present, the oracle
# tests will fail to import — install mef-tools to run them standalone.
for parent in [MEF3IO_ROOT, *MEF3IO_ROOT.parents]:
    if (parent / "mef_tools" / "__init__.py").exists():
        sys.path.insert(0, str(parent))
        break
