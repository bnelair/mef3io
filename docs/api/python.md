# Python API reference

Auto-generated from the source docstrings. For a task-oriented walkthrough see
the [Python guide](../python.md).

The native API:

::: mef3io.Reader

::: mef3io.MultiReader

::: mef3io.MultiReaderDifferenceWarning

::: mef3io.Writer

::: mef3io.archive_session

::: mef3io.extract_session

::: mef3io.password_interop_issue

## Inspecting a session

See the [Python guide](../python.md#inspecting-a-session-without-reading-its-samples).

::: mef3io.inspect

::: mef3io.SessionSummary

::: mef3io.ChannelSummary

::: mef3io.SegmentSummary

::: mef3io.RecordSummary

::: mef3io.IdentityField

::: mef3io.SummaryWarning

## Ordering and naming sessions

::: mef3io.time_range

::: mef3io.naming.TimeRange

::: mef3io.name_drift

::: mef3io.naming.NameDrift

::: mef3io.rename_archive

::: mef3io.naming.ArchiveRename

::: mef3io.rename_session

::: mef3io.fix_name_drift

::: mef3io.naming.RenameReport

::: mef3io.operation_marker

::: mef3io.clear_operation_marker

::: mef3io.SessionOperationWarning

## De-identified copies

See [De-identification](../anonymization.md#de-identified-copies-with-anonymize_session)
for the workflow. Python and C++ only (no MATLAB, no C ABI).

::: mef3io.anonymize_session

::: mef3io.anonymize.check

::: mef3io.anonymize.verify

::: mef3io.anonymize.reference_shift

::: mef3io.anonymize.redact_text

::: mef3io.AnonymizeReport

::: mef3io.anonymize.CheckReport

::: mef3io.anonymize.Refusal

::: mef3io.anonymize.REFUSAL_CODES

::: mef3io.VerifyReport

::: mef3io.anonymize.VerifyIssue

## Modified copies

See [Modified copies](../transform.md) for what changes and what is copied as
stored. `time_range=` (a cut) is described there too. The report, check and refusal types are shared with the
de-identified copies above. Python and C++ only (no MATLAB, no C ABI).

::: mef3io.transform_session

::: mef3io.transform.check

::: mef3io.transform.verify

::: mef3io.TransformReport

::: mef3io.transform.REFUSAL_CODES

::: mef3io.transform.METADATA_FIELDS

## Merged sessions

See [Merging recordings](../merge.md). Python and C++ only (no MATLAB, no C ABI).

::: mef3io.merge_sessions

::: mef3io.merge.check

::: mef3io.merge.verify

::: mef3io.MergeReport

::: mef3io.merge.REFUSAL_CODES

## Cancelling and progress

See the [Python guide](../python.md#cancelling-and-progress).

::: mef3io.CancelToken

::: mef3io.Cancelled

## Validation and repair

See the [validation guide](../validation.md) for what each check means.

::: mef3io.Validator

::: mef3io.Report

::: mef3io.Finding

::: mef3io.Check

::: mef3io.available_checks

::: mef3io.describe_check

::: mef3io.SkippedSegment

::: mef3io.validate_session

::: mef3io.repair_session

::: mef3io.recover_session

::: mef3io.RecoveryReport

::: mef3io.RecoveredSegment

::: mef3io.RecoveredRecordFile

## Errors

mef3io raises `mef3io.MefError` and its subclasses `FormatError` (damaged
or malformed files), `CrcError` (a checksum does not match),
`PasswordError` (a missing or wrong password), `IoError` (files, paths,
session naming) and `WriteConflictError` (a write that would break the
session). All of them subclass `RuntimeError`. A refusal of
`anonymize_session` also carries a stable `code` and `details`
([Refusal codes](../anonymization.md#refusal-codes)); bad arguments raise
`ValueError` or `TypeError`. `mef3io.Cancelled` is deliberately not a
`MefError`. `mef3io.have_cpp_backend()` says whether the compiled core
is available.

## Warnings

Every warning carries its session in `w.message.path` and never in its text;
see [Warnings carry the path as data](../python.md#warnings-carry-the-path-as-data).

::: mef3io.MefWarning

::: mef3io.SessionDeclarationWarning

::: mef3io.UnreadableSegmentWarning

::: mef3io.BlockCopyWarning

::: mef3io.UnscaledSamplesWarning

::: mef3io.LegacyPasswordWarning

::: mef3io.PasswordInteropWarning

::: mef3io.StepFallbackWarning

::: mef3io.ClampedSamplesWarning

::: mef3io.WriterFlushWarning

::: mef3io.BackupAlwaysKeptWarning

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
