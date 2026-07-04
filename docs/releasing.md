# Versioning & releases

## One source of truth

The repo-root **`VERSION`** file is the only place the version lives:

| Consumer | How it gets the version |
|---|---|
| PyPI wheel / sdist | `pyproject.toml` reads `VERSION` via scikit-build-core's regex metadata provider |
| Python runtime | `mef3io.__version__` (installed metadata; dev trees fall back to the value baked into the extension) |
| C++ | CMake reads `VERSION` → `mef3io::version()` (`version.hpp`) |
| MATLAB | `build_mex.m` bakes the same define → `mef3io_mex('version')` |

CI asserts `mef3io.__version__ == VERSION`, and git tags mirror the file —
they are created by the release trigger, so they cannot drift.

## Releasing

One button: run the **bump-version** workflow (GitHub → Actions →
bump-version → Run workflow), choosing `patch`, `minor`, `major`, or an
explicit `X.Y.Z`. It commits the bumped `VERSION` as `Release vX.Y.Z`, tags,
and dispatches the **release** workflow, which:

1. builds wheels natively per platform/arch — Linux x86_64 + aarch64,
   Windows AMD64 + ARM64 (CPython 3.11+ on ARM), macOS arm64 + x86_64 — plus
   the sdist, via cibuildwheel;
2. compiles and **tests** the MATLAB MEX with the latest MATLAB release on
   Linux, Windows, and macOS runners;
3. publishes wheels + sdist to PyPI (`twine`, `PYPI_Token_General` secret);
4. assembles the MATLAB bundle (`mef3io-matlab-vX.Y.Z.zip`: the `+mef3io`
   classes plus the tested MEX binaries for all three platforms) and creates
   the GitHub release with it attached.

Dispatching the release workflow manually on a branch is a **dry run**:
wheels and MEX build as artifacts, publishing is skipped.

## CI

`ci.yml` runs on pushes/PRs to `main` and `dev`: the C++ Catch2 suite, the
full oracle pytest matrix (Ubuntu 3.10/3.12/3.13, macOS 3.13), a Windows
build + import smoke test, and the version-consistency check. The docs site
deploys to GitHub Pages from `main`.
