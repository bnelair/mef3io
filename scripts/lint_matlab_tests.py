#!/usr/bin/env python3
"""Catch the MATLAB mistakes that only a release run would otherwise find.

There is no MATLAB in CI — the MEX is built and the `.m` suites are run only by
the `release` workflow. So a typo in a MATLAB test is invisible until a release
is attempted, which is exactly how it went twice:

  1. `startUutc` — the PARAMETER name inside `Writer.writeInt32`, used as if it
     were a variable of the calling script, which is called `start`.
  2. `tarPath` used ~60 lines after `delete(tarPath)`, so the assertion ran
     against a file the suite had already removed.

Both are statically detectable without MATLAB. This checks:

  * a name is used that is never assigned anywhere in the file (and is not a
    known MATLAB builtin or a name the project defines);
  * a name is used after `delete(name)` with no reassignment in between —
    tracked sequentially, so the `delete(handle)` / reopen pattern the suites
    use everywhere is not flagged.

It is a linter, not a parser: it does not understand MATLAB control flow, and it
errs toward silence. Passing it does not mean the suite runs — only the release
job proves that — but failing it means it certainly will not.

    python scripts/lint_matlab_tests.py
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

# MATLAB builtins, project names, and the toolbox functions these suites use.
# Anything here is never reported as "used but never assigned".
KNOWN = set(
    """
    for end if else elseif while try catch function return break continue switch case otherwise
    assert exist fullfile tempname mkdir rmdir delete dir what pwd fileparts filesep
    isequal isequaln isempty ischar isnumeric islogical isfield isstruct isa
    int8 int16 int32 int64 uint8 uint16 uint32 uint64 double single logical char cell struct
    true false nan inf pi eps
    sin cos tan exp log sqrt abs round floor ceil fix mod rem sign
    zeros ones rand randn linspace repmat reshape permute squeeze sort unique numel length size
    sum max min mean std cumsum diff all any find isnan isinf
    strcmp strcmpi strrep strtrim strsplit strjoin sprintf fprintf disp contains regexprep
    num2str str2double cellfun arrayfun structfun
    error warning class nargin nargout varargin varargout
    meta which help methods properties
    tic toc NaN Inf Nan pause datestr datetime now clock
    ispc ismac isunix computer mexext mex system getenv setenv
    fileread fopen fclose fwrite fread fprintf exist addpath genpath
    ones eye numel ndims fieldnames isrow iscolumn iscell iscellstr
    lower upper strfind regexp validatestring inputname deal
    mef3io mef3io_mex
    """.split()
)


def strip_code(line: str) -> str:
    """Drop comments and string literals, keeping only executable text.

    MATLAB overloads `'` for both string delimiter and transpose. A quote that
    directly follows an identifier, a closing bracket or a dot is a transpose;
    anything else opens a string. `"` is unambiguously a string.
    """
    out: list[str] = []
    i, n, in_str, delim = 0, len(line), False, ""
    while i < n:
        c = line[i]
        if in_str:
            if c == delim:
                # A doubled delimiter is an escaped quote inside the string.
                if i + 1 < n and line[i + 1] == delim:
                    i += 2
                    continue
                in_str = False
            out.append(" ")
            i += 1
            continue
        if c == "%":
            break                       # comment to end of line
        if c == '"':
            in_str, delim = True, '"'
            out.append(" ")
            i += 1
            continue
        if c == "'":
            # A transpose binds tight: `x'`, `)'`, `]'`. With whitespace before
            # it the quote opens a string -- which is how `['ldd ' f(a) ' | x']`
            # concatenates. `prev` is "" at the start of a line; test it
            # explicitly, since "" is a substring of everything.
            prev = out[-1] if out else ""
            if prev and (prev.isalnum() or prev in "_)]}."):
                out.append("'")         # transpose
            else:
                in_str, delim = True, "'"
                out.append(" ")
            i += 1
            continue
        out.append(c)
        i += 1
    return "".join(out)


def names(code: str):
    """Identifiers used as variables: not after a dot, not a call target."""
    for m in re.finditer(r"(?<![\w.])([A-Za-z]\w*)\b(\s*\()?(\s*=(?!=))?", code):
        if m.group(2):
            continue                     # name( -> a function call
        if m.group(3):
            continue                     # name= -> assignment target or Name=value arg
        yield m.start(1), m.group(1)


def assign_targets(code: str):
    """Names this line assigns: `x = ...`, `for x = ...`, and `[~, n] = f(...)`.

    The multi-output form matters: `[~, nSeg] = mef3io.recoverSession(...)` is
    the only place some names are ever bound, and missing it would report them
    as never assigned.
    """
    m = re.match(r"\s*\[([^\]]*)\]\s*=(?!=)", code)
    if m:
        for part in m.group(1).split(","):
            part = part.strip()
            if re.fullmatch(r"[A-Za-z]\w*", part):
                yield part
    for m in re.finditer(r"(?:^|[\s\[,;])([A-Za-z]\w*)\s*=(?!=)", code):
        yield m.group(1)
    for m in re.finditer(r"for\s+([A-Za-z]\w*)\s*=", code):
        yield m.group(1)


def scan(path: Path) -> list[str]:
    src = path.read_text().splitlines()
    problems: list[str] = []
    known = set(KNOWN)
    assigned: set[str] = set()
    live_deleted: set[str] = set()

    # Names bound by a signature rather than by an assignment: the parameters
    # and outputs of EVERY function in the file (local subfunctions included),
    # and the parameters of anonymous functions.
    for line in src:
        code = strip_code(line)
        fn = re.match(
            r"\s*function\s+(?:\[([^\]]*)\]\s*=\s*|([\w.]+)\s*=\s*)?([\w.]+)\s*\((.*?)\)",
            code,
        )
        if fn:
            bound = (fn.group(1) or "").split(",") + [fn.group(2) or ""] + fn.group(4).split(",")
            assigned |= {b.strip() for b in bound if re.fullmatch(r"[A-Za-z]\w*", b.strip())}
            known.add(fn.group(3).split(".")[-1])
        for anon in re.finditer(r"@\s*\(([^)]*)\)", code):
            assigned |= {a.strip() for a in anon.group(1).split(",")
                         if re.fullmatch(r"[A-Za-z]\w*", a.strip())}

    # Pass 1: every name assigned anywhere, for the "never assigned" check.
    # Two passes, because MATLAB has no declarations and a name may be assigned
    # further down inside a branch this linter does not model.
    ever_assigned: set[str] = set(assigned)
    for line in src:
        code = strip_code(line)
        ever_assigned.update(assign_targets(code))

    # Pass 2: sequential state, for the use-after-delete check.
    for i, line in enumerate(src, 1):
        code = strip_code(line)
        if not code.strip():
            continue
        for _, name in names(code):
            if name in known:
                continue
            if name in set(assign_targets(code)):
                continue  # this is the assignment target
            if re.search(rf"\bdelete\(\s*{re.escape(name)}\s*\)", code):
                continue  # this is the delete itself
            if name not in ever_assigned:
                # Not assigned anywhere and not a name we know: almost always a
                # typo, or a parameter name borrowed from another function.
                problems.append(
                    f"{path.name}:{i}: '{name}' is used but never assigned in this file "
                    f"(typo, or a parameter name from another function?)"
                )
            elif name in live_deleted:
                problems.append(
                    f"{path.name}:{i}: '{name}' is used after delete({name}) with no "
                    f"reassignment — it may refer to a file that no longer exists"
                )
        for target in assign_targets(code):
            assigned.add(target)
            live_deleted.discard(target)
        for m in re.finditer(r"\bdelete\(\s*([A-Za-z]\w*)\s*\)", code):
            live_deleted.add(m.group(1))
    return problems


def main() -> int:
    targets = sorted((REPO / "matlab").glob("*.m"))
    if not targets:
        print("no MATLAB files found", file=sys.stderr)
        return 2
    failures = 0
    for path in targets:
        problems = scan(path)
        status = "OK" if not problems else f"{len(problems)} problem(s)"
        print(f"  {path.name:28} {status}")
        for p in problems:
            print(f"      {p}")
        failures += len(problems)
    if failures:
        print(f"\n{failures} problem(s). There is no MATLAB in CI, so these would "
              f"surface only in a release run.", file=sys.stderr)
        return 1
    print("\nMATLAB test scripts pass the static checks.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
