"""Options that were removed because mef3io never deletes or replaces data.

``overwrite=`` (Writer, MefWriter, archive_session, extract_session) and
``backup=`` (recover_session, repair_session) are gone; passing one raises
``TypeError`` with the migration, instead of Python's bare "unexpected keyword
argument" (or, for a positional value, a confusing failure further down).
"""
from __future__ import annotations

OVERWRITE_MESSAGE = (
    "mef3io never deletes a session; remove or rename the folder yourself, "
    "or write to a new path (the overwrite= option was removed)"
)
BACKUP_MESSAGE = (
    "recovery and repair always keep a backup of what they change or drop; "
    "the backup= option was removed"
)


def reject_removed(who: str, removed: dict) -> None:
    """Raise ``TypeError`` for leftover keyword arguments in ``removed``."""
    if "overwrite" in removed:
        raise TypeError(f"{who}: {OVERWRITE_MESSAGE}")
    if "backup" in removed:
        raise TypeError(f"{who}: {BACKUP_MESSAGE}")
    if removed:
        names = ", ".join(sorted(removed))
        raise TypeError(f"{who}() got unexpected keyword argument(s): {names}")


def reject_positional_bool(who: str, value, message: str) -> None:
    """A bool where a password / path should be: an old positional ``overwrite``."""
    if isinstance(value, bool):
        raise TypeError(f"{who}: {message}")
