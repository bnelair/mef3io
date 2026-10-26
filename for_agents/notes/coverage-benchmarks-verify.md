# Coverage, verification gates, benchmarks

> Read before changing CI/release coverage, scripts/verify_local.sh, the oracle/ledger gates or benchmarks.
> Split out of CLAUDE.md verbatim; "the private reader" = the private third-party reader named in CLAUDE.md.

COVERAGE runs ONLY on a release (user decision 2026-10-19, never per-push):
`scripts/coverage.sh` (atomic gcov counters: the decoder's threads otherwise
race into negative hits) → gcovr for core/src + core/include (both suites
exercise it) and coverage.py for python/mef3io → coverage/ HTML + shields
endpoint JSONs; release.yml's `coverage` job attaches them to the GitHub
release (never blocking it) and re-runs docs.yml, which copies the latest
report (and the badge JSONs) into the site; README badges read
bnelair.github.io/mef3io/coverage/*.json (no redirect, unlike release
downloads). The script restores the dev extension link on exit.
VERIFY BEFORE PUBLISHING: `scripts/verify_local.sh` (`--full-bench`,
`--no-bench`) is the single entry point — builds, asserts the IN-TREE extension
is the one under test (not an installed wheel), runs the C++ + Python suites,
then the two gates that catch what round trips cannot, and finally the
benchmark. It EXITS NON-ZERO if the oracle is missing rather than reporting a
pass it cannot justify. The two gates: `tests/test_p15_oracle_acceptance.py`
(bidirectional vs mef_tools/pymef/meflib, including the MIXED sequences where
one stack modifies the other's session) and `tests/test_p16_header_parity.py` —
a KNOWN-DIVERGENCE LEDGER that writes the same signal with both stacks and
compares EVERY modelled header field; each difference must be zero or listed in
`KNOWN_DIVERGENCES` with a written reason, anything else fails. That ledger is
the test that would have caught `maximum_difference_bytes = 0`: it was invisible
to every round trip (mef3io and pymef both read the files perfectly) because
nothing compared the DECLARATIONS against the oracle's. It also documents four
fields where mef3io is RIGHT and the legacy stack is wrong (the filter settings:
-1.0 IS meflib's NO_ENTRY, meflib.h:431-437, while mef_tools writes made-up
values).

`scripts/make_reader_compat_check.py` regenerates the private-reader/meflib check set
(9 sessions + README into `~/mef3io_reader_compat_check`): fresh write, append,
legacy before/after repair, encrypted, padded-`.tmet`-then-appended,
recovered-after-interrupted-write, plus TWO DELIBERATELY BROKEN CONTROLS —
05 (the 1.1.2 zeros, mechanism (a)) and 03 (the legacy stack's own
`number_of_discontinuities = 0`, mechanism (b), never yet reproduced). A
matrix where everything passes proves nothing, which is why the controls are
there. All parameters are constants at the top of the script and are written
into the README; derived figures are read off the generated files, never
hard-coded. Every session is verified through mef3io AND pymef before the
script exits.

Benchmarks: `benchmarks/mef_benchmark.py` (write/open/seq/parallel vs mef_tools
& NWB-Zarr) and `benchmarks/compression_test.py` (file size / compression).
