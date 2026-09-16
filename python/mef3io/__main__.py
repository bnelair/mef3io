"""Command-line entry point: ``python -m mef3io <command> ...``.

Today there is one command, ``validate``, which checks a session's declarations
against its data and repairs only what you explicitly name::

    python -m mef3io validate SESSION.mefd
    python -m mef3io validate SESSION.mefd --repair sizing.difference-bytes
    python -m mef3io validate --list-checks
"""
from __future__ import annotations

import sys
from typing import Sequence

COMMANDS = {
    "validate": "check a session's declarations against its data, and repair "
                "only the checks you name",
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
    if not args or args[0] in ("-h", "--help"):
        print(_usage())
        return 0 if args else 2
    command, rest = args[0], args[1:]
    if command not in COMMANDS:
        print(f"unknown command: {command}\n", file=sys.stderr)
        print(_usage(), file=sys.stderr)
        return 2
    if command == "validate":
        from .validate import _main

        return _main(rest)
    raise AssertionError(f"unhandled command {command}")  # pragma: no cover


if __name__ == "__main__":
    sys.exit(main())
