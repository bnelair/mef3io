# Install

All bindings share **one version**: the repo-root `VERSION` file feeds the
PyPI wheel metadata, `mef3io::version()` in C++, and the MATLAB MEX build.

## Python

Prebuilt wheels (no compiler needed) for Linux (x86_64, aarch64), macOS
(arm64, x86_64), and Windows (AMD64, ARM64), Python 3.10+:

```bash
pip install mef3io                 # runtime (numpy only)
pip install "mef3io[compat]"       # + pandas (DataFrame annotations in compat)
pip install "mef3io[test]"         # + oracle test stack (pymef, mef-tools, pandas, pytest)
pip install "mef3io[bench]"        # + NWB-Zarr benchmark stack
```

## MATLAB

Two options (details in the [MATLAB guide](matlab.md)):

**Prebuilt** — every [GitHub release](https://github.com/bnelair/mef3io/releases)
has a single `mef3io-matlab-vX.Y.Z.zip` with the `+mef3io` classes and the
compiled, CI-tested MEX for all supported platforms (Linux x86_64, Windows
AMD64, macOS arm64 — MATLAB loads the right one automatically). Unzip and:

```matlab
addpath('<somewhere>/mef3io-matlab')
```

Caveats: browser-downloaded zips are quarantined on macOS (unsigned MEX →
`xattr -dr com.apple.quarantine ...` on first use); very old Linux glibc
(RHEL 8 era) and Intel Macs need the source build below.

**Build from source** (one-time, ~30 s) — needs a C++20 compiler configured
for MEX (`mex -setup C++`): Xcode on macOS, GCC ≥ 11 on Linux, Visual Studio
2022 on Windows:

```matlab
run('<repo>/matlab/build_mex.m')
addpath('<repo>/matlab')
```

!!! warning "After updating the repo"
    Rebuild the MEX and restart MATLAB (or `clear all; clear mex`). The C++
    core is statically embedded in the MEX and MATLAB caches the loaded
    binary, so an old build silently keeps running.

## C++

The core is a self-contained static library with no external dependencies
(crypto and CRC are built in). Consume it with CMake — see the
[C++ guide](cpp.md):

```cmake
add_subdirectory(mef3io/core)          # target: mef3io_core
target_link_libraries(your_app PRIVATE mef3io_core)
```

## From source (Python development)

Needs CMake ≥ 3.26, Ninja, and a C++20 compiler; uses the active
environment's Python:

```bash
git clone https://github.com/bnelair/mef3io && cd mef3io
scripts/dev_build.sh              # builds build/dev, symlinks the extension into python/mef3io
python -m pytest tests            # full suite; pip install "mef3io[test]" first for the oracle
python -m build --wheel           # build a wheel

# standalone C++ unit tests
cmake -S core -B build-core -DMEF3IO_BUILD_TESTS=ON
cmake --build build-core && ctest --test-dir build-core
```

!!! note "Don't `pip install -e .` while developing the C++"
    scikit-build-core's editable install loads an extension snapshot compiled
    at install time (its import hook beats `sys.path`), so later C++ changes
    silently don't take effect. Use `scripts/dev_build.sh` and the test
    suite's path setup instead.
