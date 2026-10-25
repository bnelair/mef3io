"""Command-line entry point: ``python -m mef3io <command> ...``.

``inspect`` and ``validate`` read; ``repair`` writes. They are separate commands so that
neither can be reached by accident from the other::

    python -m mef3io inspect SESSION.mefd [--json] [--values] [--index]
    python -m mef3io validate SESSION.mefd
    python -m mef3io repair SESSION.mefd --check sizing.difference-bytes
    python -m mef3io validate --list-checks
    python -m mef3io recover SESSION.mefd            # dry run
    python -m mef3io recover SESSION.mefd --apply

``recover`` is the only one that may touch the block index or the data file,
and only after an interrupted write. It is a dry run unless ``--apply`` is
given.

``anonymize`` never touches the source; it writes a new, de-identified copy.
By default the copy keeps every time (it still shows when the recording was
made); ``--shift-us`` or ``--reference-date`` moves every time instead::

    python -m mef3io anonymize SRC.mefd DST.mefd
    python -m mef3io anonymize SRC.mefd DST.mefd --shift-us N
    python -m mef3io anonymize SRC.mefd DST.mefd --reference-date 2000-01-03 --tz Europe/Prague

``transform`` never touches the source either; it writes a copy with a choice of channels,
passwords, name and details, every time and sample as stored::

    python -m mef3io transform SRC.mefd DST.mefd --exclude-channel ECG --set subject_id=S01
"""
from __future__ import annotations

import sys
from typing import Sequence

COMMANDS = {
    "inspect": "describe a session without reading its samples (never writes)",
    "validate": "check a session's declarations against its data (never writes)",
    "repair": "rewrite only the declarations for the checks you name",
    "recover": "after an interrupted write, make the index and data agree again",
    "anonymize": "write a de-identified copy: identity removed, times kept or shifted",
    "transform": "write a copy with chosen channels, passwords, name and details",
    "merge": "merge two or more recordings into one new session",
}


def _usage() -> str:
    lines = ["usage: python -m mef3io <command> [options]", "", "commands:"]
    for name, help_text in COMMANDS.items():
        lines.append(f"  {name:<10} {help_text}")
    lines.append("")
    lines.append("Run 'python -m mef3io <command> --help' for a command's options.")
    return "\n".join(lines)


def main(argv: Sequence[str] | None = None) -> int:
    args = list(sys.argv[1:] if argv is None else argv)
    if args and args[0] in ("-h", "--help"):
        print(_usage())          # asked for: stdout, success
        return 0
    if not args:
        print(_usage(), file=sys.stderr)   # an error: stderr, non-zero
        return 2
    command, rest = args[0], args[1:]
    if command not in COMMANDS:
        print(f"unknown command: {command}\n", file=sys.stderr)
        print(_usage(), file=sys.stderr)
        return 2
    if command == "inspect":
        from ._inspect import _main as _inspect_main

        return _inspect_main(rest)
    if command in ("validate", "repair"):
        from .validate import _main

        return _main(rest, repair=command == "repair")
    if command == "recover":
        from .validate import _recover_main

        return _recover_main(rest)
    if command == "anonymize":
        from .anonymize import _main as _anonymize_main

        return _anonymize_main(rest)
    if command == "transform":
        from .transform import _main as _transform_main

        return _transform_main(rest)
    if command == "merge":
        from .merge import _main as _merge_main

        return _merge_main(rest)
    raise AssertionError(f"unhandled command {command}")  # pragma: no cover


if __name__ == "__main__":
    sys.exit(main())
