# Python API reference

Auto-generated from the source docstrings. For a task-oriented walkthrough see
the [Python guide](../python.md).

The native API:

::: mef3io.Reader

::: mef3io.Writer

::: mef3io.archive_session

::: mef3io.extract_session

## Validation and repair

See the [validation guide](../validation.md) for what each check means.

::: mef3io.Validator

::: mef3io.Report

::: mef3io.Finding

::: mef3io.Check

::: mef3io.available_checks

::: mef3io.validate_session

::: mef3io.repair_session

::: mef3io.recover_session

::: mef3io.RecoveryReport

::: mef3io.RecoveredSegment

::: mef3io.SessionDeclarationWarning

## Metadata objects

::: mef3io.Metadata

::: mef3io.Subject

::: mef3io.Acquisition

## Legacy `mef_tools` compatibility

Drop-in replacements for `mef_tools.io` — `from mef3io import MefReader,
MefWriter`. Same call shapes and defaults as the legacy classes; see the
[legacy comparison](../legacy_comparison.md) for the measured differences.

::: mef3io.compat.MefReader

::: mef3io.compat.MefWriter
