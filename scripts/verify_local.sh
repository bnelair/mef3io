#!/usr/bin/env bash
# Full local verification for mef3io.
#
# One entry point, no assumptions: it builds the extension, runs every suite,
# proves compatibility against the legacy mef_tools/pymef/meflib stack field by
# field, and benchmarks the workloads that matter — including the append
# workload these files are actually produced by.
#
# RUN THIS BEFORE PUBLISHING. The defect this project spent a release cycle on
# (metadata section 2 declaring buffer sizes a meflib reader allocates from)
# was invisible to round-trip tests: mef3io read its own files perfectly, and so
# did pymef. It only showed up when the DECLARATIONS were compared against the
# oracle's, which is what the parity gate below does.
#
#   scripts/verify_local.sh              # everything, quick benchmark
#   scripts/verify_local.sh --full-bench # everything, full benchmark (slow)
#   scripts/verify_local.sh --no-bench   # suites and parity only
#   scripts/verify_local.sh --help
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"

BENCH=quick
for arg in "$@"; do
  case "$arg" in
    --full-bench) BENCH=full ;;
    --no-bench)   BENCH=none ;;
    -h|--help)    sed -n '2,18p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option: $arg (try --help)" >&2; exit 2 ;;
  esac
done

# --- report ------------------------------------------------------------------
PASS=(); FAIL=(); SKIP=()
step() {  # step "name" command...
  local name="$1"; shift
  printf '\n\033[1m=== %s ===\033[0m\n' "$name"
  if "$@"; then PASS+=("$name"); printf '\033[32m  OK: %s\033[0m\n' "$name"
  else FAIL+=("$name"); printf '\033[31m  FAILED: %s\033[0m\n' "$name"; fi
}

# --- environment discovery ---------------------------------------------------
# Deliberately explicit: a future run must not silently test the wrong build or
# a stale installed copy.
PY="${MEF3IO_PYTHON:-$(command -v python3 || command -v python || true)}"
[[ -n "$PY" ]] || { echo "no python found; set MEF3IO_PYTHON" >&2; exit 2; }

echo "repo:   $REPO"
echo "python: $PY  ($("$PY" -c 'import sys;print(sys.version.split()[0])'))"
command -v cmake >/dev/null || { echo "cmake not found on PATH" >&2; exit 2; }

missing=()
for mod in numpy pytest; do
  "$PY" -c "import $mod" 2>/dev/null || missing+=("$mod")
done
[[ ${#missing[@]} -eq 0 ]] || { echo "missing required modules: ${missing[*]}" >&2; exit 2; }

HAVE_ORACLE=1
for mod in pymef mef_tools; do
  if ! "$PY" -c "import $mod" 2>/dev/null; then
    HAVE_ORACLE=0
    echo "NOTE: '$mod' not importable — the oracle/parity gates will be SKIPPED."
    echo "      install with: $PY -m pip install mef-tools pymef"
  fi
done

# --- build -------------------------------------------------------------------
step "build (C++ core + extension)" bash -c '
  cmake -S . -B build/dev -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DPython_EXECUTABLE="'"$PY"'" -DMEF3IO_BUILD_TESTS=OFF >/dev/null &&
  cmake --build build/dev >/dev/null &&
  ext=$(find build/dev -maxdepth 1 -name "_mef3io*.so" -o -maxdepth 1 -name "_mef3io*.pyd" | head -1) &&
  [ -n "$ext" ] && ln -sf "../../$ext" "python/mef3io/$(basename "$ext")"'

# Confirm we are testing the tree, not an installed wheel.
step "in-tree extension is the one under test" "$PY" - <<'PY'
import sys, pathlib
sys.path.insert(0, "python")
import mef3io
where = pathlib.Path(mef3io.__file__).resolve()
assert "python/mef3io" in where.as_posix(), f"testing an installed copy at {where}"
assert mef3io.have_cpp_backend(), "the C++ backend did not load"
print(f"  {where}  backend=ok  version={mef3io.__version__}")
PY

# --- suites ------------------------------------------------------------------
step "C++ unit tests (Catch2)" bash -c '
  cmake -S core -B build-core -DMEF3IO_BUILD_TESTS=ON >/dev/null &&
  cmake --build build-core -j"$(nproc 2>/dev/null || echo 4)" >/dev/null &&
  ctest --test-dir build-core --output-on-failure'

step "Python test suite (all gates)" "$PY" -m pytest tests -q

if [[ $HAVE_ORACLE -eq 1 ]]; then
  # Called out separately because these are the compatibility gates: they are
  # the reason a change that reads back fine can still be wrong.
  step "P15 bidirectional oracle acceptance (mef_tools/pymef/meflib)" \
      "$PY" -m pytest tests/test_p15_oracle_acceptance.py -q
  step "P16 header parity ledger (declaration-by-declaration vs the oracle)" \
      "$PY" -m pytest tests/test_p16_header_parity.py -q -s
else
  SKIP+=("P15 oracle acceptance" "P16 header parity")
fi

step "P14 cross-operation coherence (write/append/repair/archive)" \
    "$PY" -m pytest tests/test_p14_coherence.py -q

# --- docs --------------------------------------------------------------------
if "$PY" -c "import mkdocs" 2>/dev/null; then
  step "docs build (--strict)" env PYTHONPATH=python "$PY" -m mkdocs build --strict --site-dir /tmp/mef3io-site
  rm -rf /tmp/mef3io-site
else
  SKIP+=("docs build (mkdocs not installed)")
fi

# --- benchmark ---------------------------------------------------------------
case "$BENCH" in
  none) SKIP+=("benchmark (--no-bench)") ;;
  quick)
    step "benchmark (quick) — write/open/read + APPEND, single-threaded" \
        env PYTHONPATH=python "$PY" benchmarks/mef_benchmark.py --quick \
            --backends mef_tools mef3io ;;
  full)
    step "benchmark (full) — write/open/read + APPEND, single-threaded" \
        env PYTHONPATH=python "$PY" benchmarks/mef_benchmark.py \
            --backends mef_tools mef3io --append-chunks 32 ;;
esac

# --- summary -----------------------------------------------------------------
printf '\n\033[1m================ SUMMARY ================\033[0m\n'
for s in "${PASS[@]:-}"; do [[ -n "$s" ]] && printf '\033[32m  PASS\033[0m  %s\n' "$s"; done
for s in "${SKIP[@]:-}"; do [[ -n "$s" ]] && printf '\033[33m  SKIP\033[0m  %s\n' "$s"; done
for s in "${FAIL[@]:-}"; do [[ -n "$s" ]] && printf '\033[31m  FAIL\033[0m  %s\n' "$s"; done
if [[ ${#FAIL[@]} -gt 0 ]]; then
  printf '\n\033[31m%d step(s) failed — do not publish.\033[0m\n' "${#FAIL[@]}"
  exit 1
fi
if [[ $HAVE_ORACLE -eq 0 ]]; then
  printf '\n\033[33mAll run steps passed, but the ORACLE gates were skipped.\033[0m\n'
  printf '\033[33mThat is not a compatibility check. Install mef-tools and pymef and re-run.\033[0m\n'
  exit 1
fi
printf '\n\033[32mAll steps passed.\033[0m\n'
