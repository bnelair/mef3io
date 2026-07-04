"""mef3io — MEF 3.0 read/write.

C++ backend (``_mef3io`` extension) with a pure-Python fallback. The public
API (``Reader``, ``Writer``) is filled in over the implementation phases; P0
exposes only backend availability so the build can be validated end to end.
"""

try:
    from . import _mef3io  # noqa: F401

    _HAVE_CPP = True
except ImportError:  # pragma: no cover - exercised only on exotic platforms
    _mef3io = None
    _HAVE_CPP = False

# Version: single source of truth is the repo-root VERSION file. Installed
# wheels carry it in the package metadata; dev trees fall back to the value
# baked into the extension at build time (also from VERSION).
try:
    from importlib.metadata import version as _dist_version

    __version__ = _dist_version("mef3io")
except Exception:  # pragma: no cover - uninstalled source tree
    __version__ = getattr(_mef3io, "__version__", "0.0.0") if _mef3io else "0.0.0"


def have_cpp_backend() -> bool:
    """True if the compiled C++ backend is importable."""
    return _HAVE_CPP


from ._reader import Reader  # noqa: E402
from ._writer import Writer  # noqa: E402
from .metadata import Acquisition, Metadata, Subject  # noqa: E402

__all__ = [
    "Reader", "Writer", "Metadata", "Subject", "Acquisition",
    "MefReader", "MefWriter", "have_cpp_backend", "__version__",
]


def __getattr__(name):
    # Legacy mef_tools-style entry points, importable straight off the package
    # (``from mef3io import MefReader, MefWriter``). Resolved lazily so that
    # importing mef3io works even where the compat layer's C++ backend doesn't.
    if name in ("MefReader", "MefWriter"):
        from . import compat

        return getattr(compat, name)
    raise AttributeError(f"module {__name__!r} has no attribute {name!r}")
