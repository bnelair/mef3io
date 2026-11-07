# Unreleased

*Draft; not released yet.*

## Fixes

- **Merging different units warns, it never refuses.** Units are free text.
  When the data a merge keeps holds different units strings for a channel (for
  example `mV` and `uV`), `merge_sessions` goes ahead, every segment keeps its
  own string, samples are never rescaled, and the channel's label is the first
  kept segment's. The report names each such channel and its strings
  (`notes`, `units_mixed`) in `merge.check` and in the merge, and the merge
  emits one `mef3io.MixedUnitsWarning` per call. `uV`, `µV` and `μV` count as
  the same unit. Silence it like `MixedFactorsWarning`:
  `warnings.filterwarnings("ignore", category=mef3io.MixedUnitsWarning)`.
- **Validator check `units.mixed`** (warning, not repairable) reports a
  channel whose segments declare different units (`uV`, `µV`, `μV` equal).

## Mixed conversion factors

- Channels whose segments have different conversion factors (recordings
  merged from different gains) are read correctly by mef3io (`Reader`,
  `MultiReader`, the compat `MefReader`) and by pymef. The older `mef_tools`
  `MefReader.get_data` returns all zeros for them. `merge_sessions` and
  `transform_session` now emit `mef3io.MixedFactorsWarning` when the result
  has such a channel. See [Merging recordings](../merge.md).
