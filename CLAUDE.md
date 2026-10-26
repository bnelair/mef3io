# mef3io — assistant context

A single C++17/20 core for **MEF 3.0** read+write, wrapped for Python via
nanobind. The high-level semantics of the legacy `mef_tools` (float scaling,
NaN discontinuities, precision inference, the int32-counts + conversion-factor
primitive path) live in **C++** so all bindings behave identically. Video is out
of scope. The legacy `mef_tools`/`pymef` stack is the correctness oracle.

Status: read + write complete, cross-validated **both directions** vs
pymef/mef_tools (values, NaN gaps, times, encryption none/L1+L2, fractional fs,
records). Annotation writes (`write_annotations`) ONLY APPEND, in place, creating the files when absent, never replacing/deleting (#156, notes/records-and-time.md). In-segment append + per-segment map implemented. Tar session
archives (single-file `.mefd.tar`, read in place) implemented. Session
validator + targeted repair + recovery implemented (+ an open-time warning).
`inspect` (metadata-only description, #67), cancel/progress on long
operations, de-identified copies (`anonymize_session` +
`anonymize.check`/`verify`, #70/#111) and modified copies (`transform_session`
+ `transform.check`/`verify`, #73) implemented — all Python + C++ only, not
MATLAB. Reads/toc/segments/records on one Reader/Session may overlap across
threads (#79; GIL released on open and every long call; the Python races of
#110 fixed, rules in for_agents/INVARIANTS.md §12). ONE thread rule
(docs/threads.md): `n_threads` per object covers open, file access, decode,
encode; settable at runtime; no per-call count (2.0). Parallel open (#141),
decode, encode, all byte-/result-deterministic across thread counts.
Python (pytest) + standalone C++ Catch2 suites.
Wheel builds via `python -m build`.

## Build & test (use the active conda env for everything)

```bash
scripts/dev_build.sh              # builds build/dev, symlinks _mef3io*.so into python/mef3io/
python -m pytest tests            # conftest.py sets up paths; pymef needed as oracle
# C++ unit tests:
cmake -S core -B build-core -DMEF3IO_BUILD_TESTS=ON && cmake --build build-core && ctest --test-dir build-core
python -m build --wheel           # wheel
```

This is the standalone mef3io repo (github.com/bnelair/mef3io), migrated out of
the original `mef_tools` repo in July 2026. The legacy `mef_tools` oracle now
comes from the pip-installed `mef-tools` package (`pip install mef3io[test]`);
`tests/conftest.py` still prefers a local checkout if one exists in a parent
directory. `reference_files/` (meflib/pymef/mef3_dump) stayed in the old repo.

## Module map (short; full text in `for_agents/notes/module-map.md`)

- `core/` (C++17/20): byte/CRC/crypto/header layers (`byteio`, `crc`, `crypto`, `headers`, `metadata`), codec `red`, read side (`session`, `source` = Directory/Tar, `tar`, `reader`, `records`, `inspect`), write side (`writer`, `session_writer`), `validate`/`recover`, copy engine (`session_copy.cpp` behind `anonymize.hpp`/`transform.hpp`, `segment_copy.hpp`), `parallel.hpp` (the thread rule), `control.hpp` (cancel/progress), `errors`, `time`, `c_api.h` (flat C ABI under the MEX).
- `python/mef3io/`: `Reader`, `MultiReader`, `Writer`, `compat` (mef_tools drop-in), `cache`, `anonymize`, `transform`, `_copy`, `_inspect`, `validate`, `metadata`, `_archive`, `_warnings`, `__main__` (CLI). Bindings: `bindings/python/mef3io_ext.cpp` (nanobind); `matlab/` (MEX + `+mef3io`); `examples/` 01-13; docs site from `docs/` (MkDocs Material, `PYTHONPATH=python mkdocs build --strict` must stay green; deployed by `.github/workflows/docs.yml`). `for_agents/` (repo root) is agent handoff docs, NOT on the site. `docs/design.md` = full design; `docs/encryption_model.md` = crypto model; `docs/mef3_format.md` = format reference.

## Topic notes (read the file BEFORE touching its area)

Everything that used to live in this file is in `for_agents/notes/` (verbatim except retargeted cross-references and the private reader's name)
(plus the short invariants list `for_agents/INVARIANTS.md`, numbered sections
cited from code and tests). Public repo: keep private names out of those
files (see "Public text" below; they say "the private reader").

| File | Read before touching |
|---|---|
| `notes/format-gotchas.md` | any on-disk field: negated times, encryption pairing, passwords, RED encode, records layout, fixed-width strings, `.tmet`/`.tidx` offsets, sign conventions, `reference_files/`, the oracle |
| `notes/large-files-and-append.md` | any `.tdat`/`.tidx` read or write, append (O(new data)), flush/close/wait_for_disk, fsync/durability, recovery backups (INVARIANTS §1, §2, §4) |
| `notes/sizing-contract.md` | section-2 `maximum_*` declarations, sizing/times validator checks |
| `notes/provenance.md` | any universal-header write, any new write path editing a segment |
| `notes/records-and-time.md` | records read/write, `to_stored_time`, rto, damaged record files |
| `notes/reader-placement-and-factors.md` | `reader.cpp` grid rules, `claim_range`, RED-header authority, `block_envelope`, `read_many`, per-segment factors, `strict=False` isolation |
| `notes/threads-gil-concurrency.md` | a new long binding call or constructor, shared Session/Reader state, the parallel open (INVARIANTS §12) |
| `notes/copy-engine.md` | anonymize / transform / cut / merge, `session_copy.cpp`, refusal codes (INVARIANTS §11, §14) |
| `notes/validate-repair-recover.md` | validator registry, `repair_session`, `recover_session`, their CLI (INVARIANTS §5) |
| `notes/cache.md` | `python/mef3io/cache.py`, `Reader(cache=)`, anything adding per-file work to an open |
| `notes/naming-rename-tar.md` | tar archives, session naming rules, `naming.cpp`, `rename.cpp` (INVARIANTS §15) |
| `notes/writer-precision-metadata.md` | `write_float`, precision inference/step, int32 clamping, Writer metadata |
| `notes/multireader.md` | `python/mef3io/_multi.py` |
| `notes/warnings.md` | emitting any Python warning, `inspect()` warning data (INVARIANTS §10) |
| `notes/matlab-c-abi.md` | `c_api.h`, the MEX, `+mef3io` (also `matlab/test_api_parity.m`) |
| `notes/coverage-benchmarks-verify.md` | release coverage, `scripts/verify_local.sh`, oracle/ledger gates, benchmarks (INVARIANTS §6-§9) |
| `notes/module-map.md` | where a file lives (full, unabridged module map) |

## Keep notes current

REQUIREMENT: this file and the topic notes are updated together with the code
in every PR; fix or remove a stale note without asking. A rule moves between
files only whole (verbatim or tightened, never weakened); keep "test-pinned"
facts and the reason (WHY) with the rule. Keep this file small: new detail goes
into the matching topic note, plus one index line here only for a new topic.

## Dev gotcha, release flow, public text

- **Do NOT `pip install -e .` for C++ dev** — scikit-build-core's editable hook
  loads an install-time extension snapshot that shadows the dev_build symlink
  (meta-path beats sys.path). Keep mef3io uninstalled; use scripts/dev_build.sh.

- Distribution: **version single source of truth = repo-root `VERSION` file**
  (pyproject regex provider → wheel metadata; CMake → `mef3io::version()` and
  the extension's `__version__`; `mef3io.__version__` prefers installed dist
  metadata). Release flow: manually run the `bump-version` workflow
  (patch/minor/major or explicit) → commits VERSION, tags vX.Y.Z, dispatches
  `release.yml` → cibuildwheel on linux x86_64+aarch64, windows AMD64+ARM64
  (cp311+ on ARM), macOS arm64+x86_64, + sdist → twine upload using the
  `PYPI_Token_General` secret. `ci.yml` = tests on push/PR (main, dev).
  Published on PyPI as `mef3io`; mef_tools is archived and mef3io replaces it
  (`from mef3io import MefReader, MefWriter`). Before every release, a full
  docs revision PR (docs/releasing.md); notes collect in
  docs/release_notes/unreleased.md between releases and are renamed to
  X.Y.Z.md then. Keep mef3io brand-neutral; brainmaze-mef3-server should
  depend on it (see docs/design.md).


Public text: `tests/test_p25_public_text.py` (and the docs workflow's check of
the built site) fails if README, docs, examples, the package, bindings, MATLAB
or core sources name the private third-party reader; CLAUDE.md, tests and
scripts are exempt. Keep it out of for_agents/ too (public, not scanned).
Institution mentions allowed: the affiliation line ("Maintained by Filip
Mivalt, Bioelectronics Neurophysiology and Engineering Laboratory (BNEL) at
Mayo Clinic.") in README, docs/index.md and CONTRIBUTORS.md, and the
CITATION.cff author `affiliation` field (maintainer-approved). Nothing else.

The private third-party reader that public text must never name is
CyberPSG. Topic notes call it "the private
reader" because `for_agents/` is public; only CLAUDE.md, tests and scripts may
spell the name. Facts about it live in `notes/sizing-contract.md` and
`notes/coverage-benchmarks-verify.md`.
