# Unreleased

*Draft; not released yet.*

## Fixes

- **`merge_sessions` no longer relabels data with other units.** A channel
  whose units differ between the sources (for example `mV` in one and `uV` in
  another) is refused with `merge-units-mismatch`, in `merge.check` and in the
  merge itself, naming the channel and both strings. Before, the merged
  channel silently took the first segment's units. Units are compared exactly
  as stored (`uV` and `µV` are different strings). A source whose segments
  already disagree is kept as it is and noted. The decision is made on the
  segments the result keeps (after `time_ranges`), and the channel carries the
  first kept segment's units. Samples are never rescaled.
- **Validator check `units.mixed`** (warning, not repairable) reports a
  channel whose segments declare different units.

## Mixed conversion factors

- Channels whose segments have different conversion factors (recordings
  merged from different gains) are read correctly by mef3io (`Reader`,
  `MultiReader`, the compat `MefReader`) and by pymef. The older `mef_tools`
  `MefReader.get_data` returns all zeros for them. `merge_sessions` and
  `transform_session` now emit `mef3io.MixedFactorsWarning` when the result
  has such a channel. See [Merging recordings](../merge.md).
