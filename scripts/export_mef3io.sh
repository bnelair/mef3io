#!/usr/bin/env bash
# Copy the mef3io export set into a target directory (a fresh repo).
# Does NOT copy reference_files/ (upstream oracles), build artifacts, or the
# legacy mef_tools package. See docs/mef3io_handoff.md for the full plan.
#
# Usage: scripts/export_mef3io.sh <target-dir>
set -euo pipefail

if [[ $# -lt 1 ]]; then
  echo "usage: $0 <target-dir>" >&2
  exit 1
fi

SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DST="$1"
mkdir -p "$DST"

# rsync each path, excluding build/cache junk.
EXCLUDES=(--exclude '__pycache__' --exclude '*.pyc' --exclude '.pytest_cache'
          --exclude 'build' --exclude 'build-core' --exclude 'dist'
          --exclude 'wheelhouse' --exclude '*.so' --exclude '*.pyd')

copy() {  # copy <relpath>
  local rel="$1"
  if [[ -e "$SRC/$rel" ]]; then
    mkdir -p "$DST/$(dirname "$rel")"
    rsync -a "${EXCLUDES[@]}" "$SRC/$rel" "$DST/$(dirname "$rel")/"
    echo "  + $rel"
  else
    echo "  ! missing: $rel" >&2
  fi
}

echo "Exporting mef3io from $SRC to $DST"
copy core
copy bindings/python
copy python/mef3io
copy tests/generate_golden.py
copy tests/test_p1_headers.py
copy tests/test_p2_read.py
copy tests/test_p3_reader.py
copy tests/test_p4_write.py
copy tests/test_p5_write.py
copy tests/test_p6_threads.py
copy tests/test_p7_compat_cache.py
copy tests/golden
copy docs/encryption_model.md
copy docs/mef3io_readme.md
copy docs/mef3io_handoff.md
copy requirements/mef3io_design.md
copy scripts/dev_build.sh
copy scripts/export_mef3io.sh
copy CMakeLists.txt
copy pyproject.toml
copy .github/workflows/wheels.yml

# a sensible starter .gitignore for the new repo
cat > "$DST/.gitignore" <<'EOF'
build/
build-core/
dist/
wheelhouse/
*.egg-info/
__pycache__/
*.pyc
.pytest_cache/
python/mef3io/_mef3io*.so
python/mef3io/_mef3io*.pyd
reference_files/
EOF
echo "  + .gitignore"

echo
echo "Done. Next:"
echo "  cd $DST && git init -b main"
echo "  # copy the 'Knowledge base' section of docs/mef3io_handoff.md into CLAUDE.md"
echo "  git add -A && git commit -m 'Initial import of mef3io'"
echo "  # keep reference_files/ locally (or add as submodules) for the oracle tests"
