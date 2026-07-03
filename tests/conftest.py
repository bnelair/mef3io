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

# mef3io package (python/mef3io)
sys.path.insert(0, str(MEF3IO_ROOT / "python"))

# legacy mef_tools oracle: prefer a local checkout when running in the original
# repo (matches the golden fixtures), otherwise fall through to a pip-installed
# `mef-tools` (`pip install mef3io[test]`). If neither is present, the oracle
# tests will fail to import — install mef-tools to run them standalone.
for parent in [MEF3IO_ROOT, *MEF3IO_ROOT.parents]:
    if (parent / "mef_tools" / "__init__.py").exists():
        sys.path.insert(0, str(parent))
        break
