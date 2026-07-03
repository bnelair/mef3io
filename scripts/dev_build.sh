#!/usr/bin/env bash
# Dev build for the mef3io C++ extension using the active conda env.
# Builds into build/dev and symlinks the extension into python/mef3io so
# `PYTHONPATH=python python -c "import mef3io"` works in place.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"

: "${MEF3IO_BUILD_TESTS:=OFF}"

cmake -S . -B build/dev -G Ninja \
  -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-Release}" \
  -DPython_EXECUTABLE="$(which python3)" \
  -DMEF3IO_BUILD_TESTS="${MEF3IO_BUILD_TESTS}"
cmake --build build/dev

EXT="$(find build/dev -maxdepth 1 -name '_mef3io*.so' -o -maxdepth 1 -name '_mef3io*.pyd' | head -1)"
if [[ -n "$EXT" ]]; then
  ln -sf "../../$EXT" "python/mef3io/$(basename "$EXT")"
  echo "linked $(basename "$EXT") into python/mef3io/"
fi
