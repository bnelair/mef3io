# Validator, repair, recovery

> Read before touching validate.cpp/validate.py, repair_session, recover_session or the CLI validate/repair/recover.
> Split out of CLAUDE.md (verbatim except retargeted cross-references); "the private reader" = the private third-party reader named in CLAUDE.md.

Validator/repair (`core/{include,src}/…/validate.{hpp,cpp}`, `python/mef3io/
validate.py`, `python/mef3io/__main__.py`): a REGISTRY of 18 checks comparing a
session's declarations against its data, run in a fixed order (integrity →
structure → sizing → times → headers). Adding a check = ONE entry with
`detect` + optional `repair` lambdas; ordering/filtering/reporting/bindings/CLI
pick it up free. THREE invariants, all test-pinned: (1) **a Validator only
validates** — it has NO repair/fix method, writing lives in the separate
`mef3io.repair_session(path, ids)` function, and a test asserts the class never
regrows one, so nobody mutates a session through an object they opened to
inspect; (2) `repair_session()` requires an explicit non-empty check-id list
(no "fix everything"; empty/unknown/non-repairable → `std::invalid_argument` →
ValueError); (3) `Finding.repaired` / `segments_repaired` count ONLY what was
actually written — `RepairFn` returns whether it changed a declaration, so a
repair that declines is reported as still outstanding (this bit: the caller used
to mark every selected finding repaired, so a declined one reported success and
left the defect on disk). Repairs rewrite ONLY declarations (s2 + the three
universal headers) — never samples or the index — back up to
`<session>.repair-backup/`, skip any segment whose CRC fails, and reject tar
archives. `Report.ok` ignores findings repaired in the same pass. CLI:
`python -m mef3io validate <path> [--check ID] [--list-checks] [--fast]` reads,
`python -m mef3io repair <path> --check ID [--no-backup]` writes and refuses an
empty selection — separate subcommands, neither reachable from the other
(`__main__.py` exists so `-m` doesn't double-import the package). On READ,
`Session::declaration_issues()` scans the already-parsed s2 for unset sizes
(free — no extra I/O, sees only missing, not wrong) and `Reader` emits one
`SessionDeclarationWarning` per open saying reads are unaffected; it rides in
the cache snapshot so warm opens still warn; `warn_declarations=False` or a
category filter silences it (pyproject filters it by MESSAGE for the suite —
naming the class there imports the wrong mef3io at pytest config time). Checks
validated against the meflib C source: `recording_duration` is a SPAN
(meflib.c:5479), `.tdat maximum_entry_size` is BYTES (pymef writes samples —
its bug), and times must be compared as ABSOLUTE uUTC because pymef negates
block times but not universal-header times. Docs: `docs/validation.md`. Tests:
`tests/test_p13_validate.py` (asserts every repairable check has a corruption
case, so a new one cannot ship untested) + a Catch2 case.
