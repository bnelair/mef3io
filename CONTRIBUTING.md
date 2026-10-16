# Contributing to mef3io

Thanks for your interest in improving mef3io. This is a single C++ core for
MEF 3.0 wrapped for Python (nanobind) and MATLAB (MEX); the guiding principle
is that the high-level behavior lives in C++ so every binding behaves
identically.

## Ways to help

- **Report a bug** or **request a feature** via
  [GitHub issues](https://github.com/bnelair/mef3io/issues) — please use the
  templates.
- **Ask a question** via
  [GitHub issues](https://github.com/bnelair/mef3io/issues).
- **Send a pull request** for fixes and features (see below).

## Development setup

Requires CMake ≥ 3.26, a C++20 compiler, and Python 3.10+.

```bash
git clone https://github.com/bnelair/mef3io && cd mef3io
pip install ".[test]"            # numpy + the pymef/mef_tools oracle stack
scripts/dev_build.sh             # builds build/dev, symlinks the extension in
python -m pytest tests           # full suite (uses the oracle where present)

# C++ unit tests
cmake -S core -B build-core -DMEF3IO_BUILD_TESTS=ON
cmake --build build-core && ctest --test-dir build-core
```

Do **not** `pip install -e .` while developing the C++: scikit-build-core's
editable install shadows the `dev_build.sh` extension with an install-time
snapshot. Use `dev_build.sh` and rerun it after every C++ change.

MATLAB binding: `run('matlab/build_mex.m')` then
`run('matlab/test_mef3io.m')` (needs a C++20 compiler via `mex -setup C++`).

More context: [`CLAUDE.md`](CLAUDE.md) (architecture + format gotchas),
[`docs/`](https://bnelair.github.io/mef3io/) (guides, format reference,
design).

## Pull requests

- Branch from `main`; open the PR against `main`.
- **Keep the C++ core, Python, and MATLAB in sync** when you change behavior —
  the semantics live in C++ and are exposed identically to each binding.
- **Add or update tests.** New behavior needs coverage; correctness is pinned
  by the committed `tests/golden/` fixtures cross-validated against pymef.
- Run the full suite (`pytest tests`, the C++ tests, and — if the MEX changed
  — `test_mef3io.m`) and keep them green.
- Match the surrounding style (the C++ is close to the MEF/meflib spec naming;
  keep offset tables faithful to the format).
- CI must pass: it builds and tests on Linux/macOS across Python 3.10–3.14.

## Documentation rules

The documentation is part of the product: a wrong sentence in it costs a user
as much as a bug. When you change behavior, change the docs in the same pull
request.

- **Revised against the code before every release.** The docs, the README,
  the examples and the release notes are checked against the code before each
  release; the checklist is in
  [Versioning & releases](https://bnelair.github.io/mef3io/releasing/)
  (`docs/releasing.md`). Every example must run, and
  `PYTHONPATH=python mkdocs build --strict` must stay green.
- **No facts that go stale in the README.** It is also the PyPI page, frozen
  at each release. Leave out test counts, version histories and "new in"
  lists; those belong in the release notes.
- **One name for each thing, everywhere.** "MEF 3.0" for the format, "mef3io"
  for the library, and one docs address, https://bnelair.github.io/mef3io/.
- **No private product names.** Public text never names a private
  third-party product; `tests/test_p25_public_text.py` and the docs workflow
  check this.
- **No institution branding.** A plain affiliation line for the maintainers
  is the only mention.
- **Say what it does for the user.** Describe a feature by what the reader
  gains or must do, in plain words, with runnable code first where it fits.
  The how-to pages (`docs/howto/`) answer one question each and link to the
  full guides instead of repeating them.

## Reporting good bugs

The most useful reports include the mef3io version (`mef3io.__version__`), OS
and Python/MATLAB version, a minimal snippet, and the full error/traceback. If
a specific session triggers it, note whether it came from mef3io, `mef_tools`,
or another writer — and, when possible, whether a small anonymized session
reproduces it. Never attach clinical or subject-identifying data.

## Reporting security issues

Please follow [`SECURITY.md`](SECURITY.md) rather than opening a public issue.

## License

By contributing you agree that your contributions are licensed under the
project's [Apache 2.0 license](LICENSE).
