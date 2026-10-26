# Writer: precision step, clamping, session metadata

> Read before touching write_float, precision inference/step, quantization, int32 clamping or Writer metadata.
> Split out of CLAUDE.md verbatim; "the private reader" = the private third-party reader named in CLAUDE.md.

Precision step (#81, `core/src/quantize_step.cpp`, `SessionWriter::write_float`
with `Precision::detect_step()`, Python `precision="step"`, also
`MefWriter.write_data`; NOT C ABI/MATLAB): OPT-IN, the default
(infer_precision) is unchanged. `detect_step` ESTIMATES from a BOUNDED sample
(first 65,536 + 16 windows of 2,048; only an all-0/NaN sample scans on):
ascending unique |v|, real Euclid (coarse tol 1e-6 step) + least-squares refit
at power-of-two indices, then SNAPPED (shortest decimal 1..17 digits, then
max|v|/k_max and its float64 neighbours) to the candidate reproducing every
sampled value bitwise. `quantize_to_step` DECIDES: one pass (the conversion
pass, no second read), every value must satisfy
`|x - double(k)*step| <= 64 eps |x|` (the reader's own product) and |k| <=
INT32_MAX (INT32_MIN = RED_NAN), else false. Fresh channel failing either ->
the default path for that write (byte-identical to a default write's
counts), `step_fallback` reason + Python `StepFallbackWarning`. MINIMUM
EVIDENCE: a sample with < 16 distinct non-zero magnitudes (DC, square wave,
a few levels) gives no step (the default path) — the level itself would
become the channel's factor. A CHANNEL KEEPS ONE FACTOR: the reader scales a
whole channel by its FIRST segment's factor (session.cpp
load_channel_basic_info; per-segment scaling is #150) — a new segment with
another factor reads back silently mis-scaled (PRE-EXISTING for the default
path with new_segment=True; #150) — so "step" on a channel with segments
(append OR new_segment) uses the channel's factor itself (never via a
`precision` int: factors >= 10 have p < 0, the "infer" sentinel — review
round 1 found that): exact if the data fits, else REFUSED
(WriteConflictError, nothing written), never rounded (decided in review;
once #150 lands a later change may start a new segment instead — not now).
The default `precision=-1` append onto a non-power-of-ten factor is refused
with a WriteConflictError naming "step"/write_int32. NO INT32 WRAP anywhere — CLAMP (maintainer decision, #149 round 3):
`quantize_to_int32` and `quantize_to_step` store a count beyond
+-INT32_MAX, and +-inf, as +-(2^31 - 1) (meflib RED_POSITIVE/NEGATIVE_
INFINITY, meflib.h:1060; never INT32_MIN = RED_NAN) via
`detail::clamp_count` (core/src/clamp.hpp) and count it in a `ClampCount`;
`WriteSummary::clamped` / `clamped_infinite` -> Python summary keys and ONE
`ClampedSamplesWarning` per write (`warn_clamped`, _writer.py; Writer.write
and MefWriter.write_data). NaN stays a gap. A finite value beyond int32 must
still lie ON the step for the step path (else the usual off-step refusal).
infer_precision and detect_step ignore +-inf like NaN. `int32_range_ok`
requires BOTH ends to fit (it was mef_tools' "both ends overflow" rule, which
let one-sided data wrap; pinned against the oracle as test_p16
`CASE_DIVERGENCES["one-sided offset"]`, a per-case ledger so
units_conversion_factor stays unlisted in KNOWN_DIVERGENCES), so ordinary
data never clamps (test_p43 pins it). The estimator's least-squares refit sums in units of
the step (no overflow at 1e300). WriteSummary gained
`units_conversion_factor`, `quantization` (Counts/Decimal/Step),
`step_fallback`; the Python dict the same keys. `Precision` is implicitly
constructible from int so every `write_float(..., 3)` caller compiles.
Tests: tests/test_p43_write_step.py (exactness, fallbacks incl. an off-step
value OUTSIDE the sample and int32 overflow, evidence cases, refusals on
append/new segment incl. factors 10 and 1e6, clamp + one warning on every path, size
within 5% of write_int32, detection on 50 M < 0.25 s, a loose 2.5x write-time
sanity bound; the tight ratios (step <= 1.10x default, worst-case fallback
<= 1.60x) are `@pytest.mark.benchmark`, opt-in via `--run-benchmark`,
because wall-clock ratios flake on shared runners), core/tests/
test_write_step.cpp. Gotcha for tests: a filter with short-decimal or dyadic
taps keeps data on a finer grid (found, stored exactly) — use irrational
taps to force a fallback.

Session metadata (subject/acquisition): `mef3io.Metadata`/`Subject`/
`Acquisition` dataclasses (`python/mef3io/metadata.py`), settable via
`Writer(metadata=)` / `set_metadata`, read via `Reader.metadata`. Threaded
through C++ `SessionMetadata` (writer.hpp) → SegmentSpec → writer.cpp s2/s3
population; `ChannelInfo` carries the fields on read. C ABI
`mef3io_writer_set_metadata` + MEX `writer_set_metadata`; MATLAB
`Writer(Metadata=struct)` / `Reader.metadata`. Subject fields are L2-gated.
