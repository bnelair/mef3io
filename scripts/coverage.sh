#!/usr/bin/env bash
# Line coverage of mef3io: the C++ core (as the C++ AND the Python suites
# exercise it) and the Python layer. Runs on a release (release.yml) and by
# hand; never in the per-push CI, which stays fast.
#
#   scripts/coverage.sh            # writes coverage/ (HTML + badge JSON)
#
# Output:
#   coverage/core/index.html        C++ core, line by line (gcovr)
#   coverage/python/index.html      Python layer (coverage.py)
#   coverage/coverage-core.json     shields.io endpoint badges
#   coverage/coverage-python.json
#
# Needs: a C++ toolchain, cmake, ninja, Python with nanobind, numpy, pytest,
# coverage and gcovr (pymef + mef-tools for the oracle tests, as usual).
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"
OUT="$REPO/coverage"
rm -rf "$OUT" build/coverage build/coverage-core
mkdir -p "$OUT"

# Atomic counters: the decoder runs on many threads, and plain counters
# race into negative hit counts that gcov rejects.
FLAGS="--coverage -O0 -g -fprofile-update=atomic"
LINK="--coverage"

# 1. The Python extension, instrumented, linked into python/mef3io like
#    scripts/dev_build.sh does (never pip install -e: see CLAUDE.md).
cmake -S . -B build/coverage -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DPython_EXECUTABLE="$(which python3)" \
  -DCMAKE_CXX_FLAGS="$FLAGS" -DCMAKE_SHARED_LINKER_FLAGS="$LINK" \
  -DCMAKE_MODULE_LINKER_FLAGS="$LINK"
cmake --build build/coverage
EXT="$(find build/coverage -maxdepth 1 -name '_mef3io*.so' | head -1)"
ln -sf "../../$EXT" "python/mef3io/$(basename "$EXT")"

# 2. The C++ suite, instrumented.
cmake -S core -B build/coverage-core -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DMEF3IO_BUILD_TESTS=ON -DCMAKE_CXX_FLAGS="$FLAGS" -DCMAKE_EXE_LINKER_FLAGS="$LINK"
cmake --build build/coverage-core
ctest --test-dir build/coverage-core --output-on-failure

# 3. The Python suite, measured on the Python side too.
python3 -m coverage run --source=python/mef3io --omit='*/pure/*' -m pytest tests -q -p no:cacheprovider
python3 -m coverage html -d "$OUT/python" --title "mef3io Python layer"
PY_PCT="$(python3 -m coverage report --format=total)"

# 4. The C++ core, from both builds' counters.
mkdir -p "$OUT/core"
# (Third-party code built alongside — Catch2, nanobind — is not measured.)
python3 -m gcovr --root . --filter 'core/src/' --filter 'core/include/' \
  --exclude 'core/tests/' --exclude-directories '.*/_deps/.*' \
  --exclude-directories '.*/nanobind-static.dir/.*' \
  --html-details "$OUT/core/index.html" --html-title "mef3io C++ core" \
  --json-summary "$OUT/core-summary.json" build/coverage build/coverage-core
CORE_PCT="$(python3 -c "import json;print(round(json.load(open('$OUT/core-summary.json'))['line_percent']))")"

# 5. shields.io endpoint badges.
badge() {  # label, percent, file
  python3 - "$1" "$2" "$3" <<'EOF'
import json, sys
label, pct, path = sys.argv[1], int(float(sys.argv[2])), sys.argv[3]
color = ("brightgreen" if pct >= 90 else "green" if pct >= 80 else "yellowgreen" if pct >= 70
         else "yellow" if pct >= 60 else "orange")
json.dump({"schemaVersion": 1, "label": label, "message": f"{pct}%", "color": color},
          open(path, "w"))
EOF
}
badge "coverage: core" "$CORE_PCT" "$OUT/coverage-core.json"
badge "coverage: Python" "$PY_PCT" "$OUT/coverage-python.json"
echo "coverage: C++ core ${CORE_PCT}%, Python ${PY_PCT}% -> $OUT"
