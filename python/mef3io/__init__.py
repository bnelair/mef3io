"""mef3io — read, write, validate and de-identify MEF 3.0 recordings.

The work is done by the compiled C++ core (the ``_mef3io`` extension); the
pure-Python backend is a stub. Start with :class:`Reader` (or
:class:`MultiReader` for several recordings read as one), :class:`Writer`,
:func:`inspect` and :func:`anonymize_session`; the guide is at
https://bnelair.github.io/mef3io/python/.
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


from ._archive import archive_session, extract_session  # noqa: E402
from . import anonymize  # noqa: E402  (mef3io.anonymize.verify, ...)
from .anonymize import AnonymizeReport, VerifyReport, anonymize_session  # noqa: E402
from . import transform  # noqa: E402  (mef3io.transform.check, .verify, ...)
from .transform import TransformReport, transform_session  # noqa: E402
from . import merge  # noqa: E402  (mef3io.merge.check, .verify, ...)
from .merge import MergeReport, merge_sessions  # noqa: E402
from . import naming  # noqa: E402
from .naming import (  # noqa: E402
    RenameReport,
    TimeRange,
    clear_operation_marker,
    fix_name_drift,
    name_drift,
    operation_marker,
    rename_archive,
    rename_session,
    time_range,
)
from ._inspect import (  # noqa: E402
    ChannelSummary,
    IdentityField,
    RecordSummary,
    SegmentSummary,
    SessionSummary,
    SummaryWarning,
    inspect,
)
from ._cancel import CancelToken, Cancelled  # noqa: E402
from ._reader import (  # noqa: E402
    BlockCopyWarning,
    LegacyPasswordWarning,
    PasswordInteropWarning,
    Reader,
    SessionDeclarationWarning,
    SessionOperationWarning,
    UnreadableSegmentWarning,
)
from ._multi import MultiReader, MultiReaderDifferenceWarning  # noqa: E402
from ._warnings import MefWarning  # noqa: E402
from ._writer import Writer, password_interop_issue  # noqa: E402
from .metadata import Acquisition, Metadata, Subject  # noqa: E402
from .validate import (  # noqa: E402
    Check,
    Finding,
    Report,
    SkippedSegment,
    Validator,
    available_checks,
    describe_check,
    RecoveredSegment,
    RecoveryReport,
    recover_session,
    repair_session,
    validate_session,
)

# The core exception types, so a caller can tell "wrong password" from
# "corrupt file" without matching on message text. All of them subclass
# RuntimeError, which is what every release so far raised, so existing
# `except RuntimeError` keeps working unchanged.
if _HAVE_CPP:
    MefError = _mef3io.MefError
    FormatError = _mef3io.FormatError
    CrcError = _mef3io.CrcError
    PasswordError = _mef3io.PasswordError
    IoError = _mef3io.IoError
    WriteConflictError = _mef3io.WriteConflictError
else:  # pragma: no cover - only without the extension
    MefError = FormatError = CrcError = PasswordError = IoError = WriteConflictError = RuntimeError

__all__ = [
    "Reader", "MultiReader", "Writer", "Metadata", "Subject", "Acquisition",
    "MefReader", "MefWriter", "archive_session", "extract_session",
    "password_interop_issue",
    "anonymize_session", "AnonymizeReport", "VerifyReport",
    "transform_session", "TransformReport", "merge_sessions", "MergeReport",
    "time_range", "TimeRange", "name_drift", "rename_archive",
    "rename_session", "fix_name_drift", "RenameReport", "operation_marker",
    "clear_operation_marker",
    # Not "inspect": `from mef3io import *` would shadow the standard library's.
    "SessionSummary", "ChannelSummary", "SegmentSummary",
    "RecordSummary", "IdentityField", "SummaryWarning",
    "Validator", "Check", "Finding", "Report", "SkippedSegment",
    "available_checks", "describe_check",
    "validate_session", "repair_session", "recover_session",
    "RecoveryReport", "RecoveredSegment",
    "MefWarning",
    "BlockCopyWarning",
    "LegacyPasswordWarning",
    "PasswordInteropWarning",
    "SessionDeclarationWarning",
    "UnreadableSegmentWarning",
    "SessionOperationWarning",
    "MultiReaderDifferenceWarning",
    "MefError", "FormatError", "CrcError", "PasswordError", "IoError",
    "WriteConflictError",
    "CancelToken", "Cancelled",
    "have_cpp_backend", "__version__",
]


def __getattr__(name):
    # Legacy mef_tools-style entry points, importable straight off the package
    # (``from mef3io import MefReader, MefWriter``). Resolved lazily so that
    # importing mef3io works even where the compat layer's C++ backend doesn't.
    if name in ("MefReader", "MefWriter"):
        from . import compat

        return getattr(compat, name)
    raise AttributeError(f"module {__name__!r} has no attribute {name!r}")
