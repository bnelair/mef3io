"""The MATLAB linter's own regression suite.

`scripts/lint_matlab_tests.py` exists because two MATLAB typos reached a
release (there is no MATLAB in CI — the `.m` suites run only in the release
job). A linter nobody tests is worse than none: it reports clean and everyone
believes it. So these cases pin both halves — that it REPORTS each mistake it
claims to catch, and that it stays SILENT on the constructs real MATLAB uses.
"""
from __future__ import annotations

import importlib.util
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parent.parent
SCRIPT = REPO / "scripts" / "lint_matlab_tests.py"
SUITE = REPO / "matlab" / "test_mef3io.m"


def _load():
    spec = importlib.util.spec_from_file_location("lint_matlab_tests", SCRIPT)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


lint = _load()


def _scan_text(tmp_path: Path, text: str) -> list[str]:
    f = tmp_path / "test_mef3io.m"
    f.write_text(text)
    return lint.scan(f)


@pytest.fixture(scope="module")
def suite_src() -> str:
    return SUITE.read_text()


def test_the_shipped_matlab_files_are_clean():
    """The checked-in suites must pass, or the CI job is just noise."""
    problems = {p.name: lint.scan(p) for p in sorted((REPO / "matlab").glob("*.m"))}
    assert problems, "no MATLAB files found"
    assert not any(problems.values()), problems


def test_reports_a_parameter_name_borrowed_from_another_function(tmp_path, suite_src):
    """Release failure 1: `startUutc` is the parameter name inside
    Writer.writeInt32, not a variable of the calling script (that is `start`)."""
    broken = suite_src.replace(
        "w.writeInt32('ch1', durCounts, 0.5, start, fs);",
        "w.writeInt32('ch1', durCounts, 0.5, startUutc, fs);", 1)
    assert broken != suite_src
    assert any("startUutc" in p and "never assigned" in p for p in _scan_text(tmp_path, broken))


def test_reports_a_name_used_after_it_was_deleted(tmp_path, suite_src):
    """Release failure 2: the tar-recovery assertion ran against a path the
    suite had already removed."""
    broken = suite_src.replace(
        "assert(exist(recTarPath, 'file') == 2, 'the archive must be left alone');",
        "assert(exist(tarPath, 'file') == 2, 'the archive must be left alone');", 1)
    assert broken != suite_src
    assert any("tarPath" in p and "after delete" in p for p in _scan_text(tmp_path, broken))


def test_indexing_a_deleted_name_is_still_a_use(tmp_path, suite_src):
    """`name(...)` is both a call and an index in MATLAB. A variable shadows a
    function, so once `got` is assigned, `got(1)` INDEXES it — and reaching it
    after delete(got) is the same defect as reaching it bare."""
    broken = suite_src.replace(
        "    summary = mef3io.recoverSession(dp);",
        "    delete(got);\n    summary = mef3io.recoverSession(dp);\n"
        "    assert(got(1) == 0);", 1)
    assert broken != suite_src
    assert any("got" in p and "after delete" in p for p in _scan_text(tmp_path, broken))


# Each snippet must be self-contained: a free variable is a REAL finding, so
# leaving one in would test the fixture rather than the construct.
PREAMBLE = "p = 'x.mefd';\nx = 1:10;\nok = true;\nitems = dir('.');\nmc = meta.class.fromName('c');\n"


@pytest.mark.parametrize(
    "label, code",
    [
        # The delete(handle)/reopen pattern these suites use everywhere.
        ("reassignment after delete",
         "w = mef3io.Writer(p);\ndelete(w);\nw = mef3io.Writer(p);\nw.write('c', x);\n"),
        # A Name=value argument is not an assignment, and not a use either.
        ("Name=value argument", "w = mef3io.Writer(p, Overwrite=true, Units='uV');\n"),
        # A quote after ') ' opens a string; only a tight one is a transpose.
        ("bracket string concat", "s = ['ldd ' fullfile(p, p) ' | grep -i stdc'];\n"),
        ("transpose", "y = x';\nz = mc.MethodList';\n"),
        # Anonymous functions bind their own parameters.
        ("anonymous function parameter",
         "f = arrayfun(@(s) fullfile(s.folder, s.name), items);\n"),
        # Multi-output assignment is the only place some names are ever bound.
        ("multi-output assignment",
         "[~, nSeg] = mef3io.recoverSession(p, Apply=true);\nassert(nSeg == 0);\n"),
        # Words inside string literals are not identifiers.
        ("words in a string literal", "assert(ok, 'the archive must be left alone');\n"),
        # A continuation is one statement, so depth still separates the two.
        ("continued statement", "w = mef3io.Writer(p, ...\n                  Overwrite=true);\n"),
    ],
)
def test_stays_silent_on_real_matlab(tmp_path, label, code):
    assert _scan_text(tmp_path, PREAMBLE + code) == [], label


def test_subfunction_parameters_are_bound(tmp_path):
    """Every function in the file binds its parameters, not just the first:
    `function checkClass(className, nameMap)` is where those names come from."""
    code = ("function outer()\n"
            "helper('mef3io.Reader');\n"
            "end\n"
            "function helper(className)\n"
            "disp(className);\n"
            "end\n")
    assert _scan_text(tmp_path, code) == []


def test_indexing_an_unassigned_name_is_left_alone(tmp_path):
    """The documented blind spot, asserted so it stays deliberate: an
    unassigned `name(` cannot be told from a call to a toolbox function this
    script does not enumerate, so it is not reported."""
    assert _scan_text(tmp_path, "y = someToolboxFunction(1);\n") == []
