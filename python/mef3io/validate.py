"""Validate a MEF 3.0 session, and — separately — repair what the data proves.

A MEF session declares, in metadata section 2 and the universal headers, a
number of quantities that are really re-derivable from the data: how many
blocks there are, how many samples, and how large a buffer a reader must
allocate. Readers trust those declarations — meflib-based ones (CyberPSG and
similar) allocate from them *before* decoding anything — so a wrong declaration
is a defect in the file even when every sample on disk is intact.

Three rules shape this API:

* **A Validator only validates.** :class:`Validator` cannot write a byte: it
  has no repair method at all. Writing lives in :func:`repair_session`, a
  separate function with a name that says so, so no one can modify a session
  while believing they are inspecting one.
* **Nothing is repaired implicitly.** :func:`repair_session` takes an explicit
  list of check ids and touches nothing else; there is deliberately no "fix
  everything" shortcut.
* **Every call returns the full report.** A repair still runs every check, so
  you see the whole picture and not only the part you chose to fix.

Typical use::

    import mef3io

    report = mef3io.Validator("subject.mefd").validate()
    print(report.summary())

    if not report.ok:
        # Writing is a different call, made deliberately.
        mef3io.repair_session("subject.mefd", ["sizing.difference-bytes"])

On the command line::

    python -m mef3io validate SESSION.mefd
    python -m mef3io repair SESSION.mefd --check sizing.difference-bytes
    python -m mef3io validate --list-checks
"""
from __future__ import annotations

import dataclasses
import os
import sys
from dataclasses import dataclass
from typing import Iterable, Sequence

__all__ = [
    "Check",
    "describe_check",
    "Finding",
    "SkippedSegment",
    "Report",
    "Validator",
    "available_checks",
    "validate_session",
    "repair_session",
    "recover_session",
    "RecoveryReport",
    "RecoveredSegment",
]

def _backend():
    from . import _mef3io

    if _mef3io is None:  # pragma: no cover - exercised only without the extension
        raise RuntimeError("mef3io validation requires the C++ backend")
    return _mef3io


@dataclass(frozen=True)
class Check:
    """One check in the registry."""

    id: str
    title: str
    description: str
    severity: str
    repairable: bool

    def __str__(self) -> str:
        mark = "repairable" if self.repairable else "report only"
        return f"{self.id:<28} {self.severity:<8} {mark:<12} {self.title}"


@dataclass(frozen=True)
class Finding:
    """One problem, found by one check, in one segment."""

    check_id: str
    severity: str
    channel: str
    segment: int
    path: str
    field: str
    stored: str
    expected: str
    message: str
    repairable: bool
    repaired: bool

    @property
    def location(self) -> str:
        return f"{self.channel} seg{self.segment}"

    def __str__(self) -> str:
        head = f"[{self.severity}] {self.check_id} — {self.location}"
        if self.field:
            head += f": {self.field} = {self.stored}, expected {self.expected}"
        tail = "  (repaired)" if self.repaired else ""
        return f"{head}\n    {self.message}{tail}"


@dataclass(frozen=True)
class SkippedSegment:
    """A segment that could not be checked, and why. Never silently dropped."""

    channel: str
    segment: int
    path: str
    reason: str

    def __str__(self) -> str:
        return f"{self.channel} seg{self.segment}: {self.reason}"


@dataclass(frozen=True)
class Report:
    """The result of a validate or repair pass."""

    findings: tuple[Finding, ...] = ()
    skipped: tuple[SkippedSegment, ...] = ()
    segments_checked: int = 0
    segments_repaired: int = 0
    checks_run: tuple[str, ...] = ()
    checks_repaired: tuple[str, ...] = ()
    path: str = ""
    #: What this run did NOT look at. A report can only speak for the work it
    #: actually did, and every one of these narrows it: `channels`/`segments`
    #: leave whole parts of the session unexamined, and `measured_difference_
    #: bytes=False` (the `--fast` path) judges only a clearly-unset
    #: `maximum_difference_bytes`, so a wrong-but-plausible one — the value
    #: that crashes a meflib reader — is taken at its word. Without these,
    #: `summary()` printed an unqualified all-clear for a run that had checked
    #: one channel in fast mode.
    channels: tuple[str, ...] = ()
    segments: tuple[int, ...] = ()
    measured_difference_bytes: bool = True

    @property
    def is_narrowed(self) -> bool:
        """True when this run cannot speak for the whole session."""
        return bool(
            self.channels
            or self.segments
            or not self.measured_difference_bytes
            or len(self.checks_run) < len(available_checks())
        )

    def scope_caveat(self) -> str:
        """One sentence naming what this run did not cover, or "" if it covered
        everything."""
        parts = []
        if len(self.checks_run) < len(available_checks()):
            missing = len(available_checks()) - len(self.checks_run)
            parts.append(f"{missing} of {len(available_checks())} check(s) were not run")
        if self.channels:
            parts.append(f"only channel(s) {', '.join(self.channels)} were examined")
        if self.segments:
            parts.append(
                "only segment(s) " + ", ".join(str(s) for s in self.segments) + " were examined"
            )
        if not self.measured_difference_bytes:
            parts.append(
                "maximum_difference_bytes was bounded rather than measured (--fast), so a "
                "wrong-but-plausible value would not have been seen"
            )
        return "; ".join(parts)

    @property
    def ok(self) -> bool:
        """True when nothing worse than a warning is left outstanding.

        Findings this pass repaired no longer count against it, so a repair
        that resolved everything reports ``ok``. A skipped segment always
        does count — an unchecked segment is not a clean one.
        """
        return bool(self.segments_checked) and not self.unresolved_errors and not self.skipped

    @property
    def errors(self) -> tuple[Finding, ...]:
        """Every error-severity finding, repaired or not."""
        return tuple(f for f in self.findings if f.severity == "error")

    @property
    def unresolved(self) -> tuple[Finding, ...]:
        """Findings that are still outstanding — nothing repaired this pass."""
        return tuple(f for f in self.findings if not f.repaired)

    @property
    def unresolved_errors(self) -> tuple[Finding, ...]:
        return tuple(f for f in self.findings if f.severity == "error" and not f.repaired)

    @property
    def warnings(self) -> tuple[Finding, ...]:
        return tuple(f for f in self.findings if f.severity == "warning")

    @property
    def infos(self) -> tuple[Finding, ...]:
        return tuple(f for f in self.findings if f.severity == "info")

    @property
    def repaired(self) -> tuple[Finding, ...]:
        return tuple(f for f in self.findings if f.repaired)

    @property
    def repairable_check_ids(self) -> list[str]:
        """Distinct check ids that reported something a repair could fix.

        Pass these to :func:`repair_session` to opt in deliberately.
        """
        out: list[str] = []
        for f in self.findings:
            if f.repairable and not f.repaired and f.check_id not in out:
                out.append(f.check_id)
        return out

    def by_check(self) -> dict[str, list[Finding]]:
        """Findings grouped by check id, in registry order."""
        order = {c: i for i, c in enumerate(self.checks_run)}
        grouped: dict[str, list[Finding]] = {}
        for check_id in sorted({f.check_id for f in self.findings}, key=lambda c: order.get(c, 1 << 30)):
            grouped[check_id] = [f for f in self.findings if f.check_id == check_id]
        return grouped

    def by_channel(self) -> dict[str, list[Finding]]:
        grouped: dict[str, list[Finding]] = {}
        for f in self.findings:
            grouped.setdefault(f.channel, []).append(f)
        return grouped

    def _append_skipped(self, lines: list[str], max_examples: int) -> None:
        """Render the skipped segments, with the reason each was skipped.

        A skipped segment is one the validator could not vouch for, so its
        reason is often the single most useful line in the report — "the
        password is wrong", "the CRC does not verify". It must never be
        dropped; :class:`SkippedSegment` promises as much.
        """
        if not self.skipped:
            return
        lines.append("")
        lines.append(f"skipped {len(self.skipped)} segment(s):")
        for s in self.skipped[:max_examples]:
            lines.append(f"    {s}")
        if max_examples and len(self.skipped) > max_examples:
            lines.append(f"    ... and {len(self.skipped) - max_examples} more")

    def summary(self, max_examples: int = 3) -> str:
        """A full human-readable report: counts, then each check that fired."""
        lines = []
        head = f"mef3io validation report — {self.path}" if self.path else "mef3io validation report"
        lines.append(head)
        lines.append("=" * min(len(head), 78))
        lines.append(
            f"{self.segments_checked} segment(s) checked, "
            f"{len(self.checks_run)} check(s) run, "
            f"{len(self.errors)} error(s), {len(self.warnings)} warning(s)"
        )
        if self.segments_repaired:
            # What was WRITTEN, not what the caller asked for. `checks_repaired`
            # is the request (the C++ assigns it before touching a segment), so
            # rendering it here claimed repairs that never happened.
            written = sorted({f.check_id for f in self.repaired})
            lines.append(
                f"{self.segments_repaired} segment(s) repaired ({', '.join(written)})"
            )
            not_written = [c for c in self.checks_repaired if c not in written]
            if not_written:
                lines.append(
                    f"    selected but wrote nothing: {', '.join(not_written)}"
                )
        if not self.segments_checked:
            lines.append("")
            lines.append(
                "No segments were examined — nothing matched the channel/segment filter, "
                "or the session holds no time-series data. This is NOT a clean result."
            )
            # Fall through to the skipped block rather than returning: when
            # every segment was skipped, the reason each was skipped is the
            # only thing the user actually needs, and returning here discarded
            # it — a wrong password printed the filter explanation instead.
            self._append_skipped(lines, max_examples)
            return "\n".join(lines)
        if not self.findings and not self.skipped:
            lines.append("")
            # An all-clear may only be as wide as the run that produced it. Any
            # narrowing at all — a check filter, a channel or segment filter, or
            # --fast, which bounds maximum_difference_bytes instead of measuring
            # it — makes the unqualified sentence false, and it is printed under
            # a heading naming the whole session.
            caveat = self.scope_caveat()
            if caveat:
                lines.append(
                    f"No problems found by the checks that ran — but {caveat}. "
                    f"This is NOT a clean bill of health for the session."
                )
            else:
                lines.append("No problems found — every declaration matches the data on disk.")
            return "\n".join(lines)

        grouped = self.by_check()
        for check_id in [c for c in self.checks_run if c in grouped]:
            hits = grouped[check_id]
            info = describe_check(check_id)
            severity = hits[0].severity
            n_fixed = sum(1 for h in hits if h.repaired)
            state = f", {n_fixed} repaired" if n_fixed else ""
            lines.append("")
            lines.append(f"{check_id}  [{severity}]  {len(hits)} segment(s){state}")
            if info is not None:
                lines.append(f"    {info.title}")
            for hit in hits[:max_examples]:
                where = f"{hit.location}"
                if hit.field:
                    lines.append(
                        f"    {where}: {hit.field} = {hit.stored}  ->  {hit.expected}"
                    )
                else:
                    lines.append(f"    {where}: {hit.message}")
            if max_examples and len(hits) > max_examples:
                lines.append(f"    ... and {len(hits) - max_examples} more")

        self._append_skipped(lines, max_examples)

        fixable = self.repairable_check_ids
        if fixable:
            lines.append("")
            lines.append("Still repairable. To fix, pass the ids explicitly:")
            if self.path.lower().rstrip("/\\").endswith(".tar"):
                # A tar session is read in place and never written in place, so
                # the obvious next command would always fail. Say what to do.
                lines.append("    session = mef3io.extract_session(path)   # archives are read-only")
                lines.append(f"    mef3io.repair_session(session, {fixable!r})")
            else:
                lines.append(f"    mef3io.repair_session(path, {fixable!r})")
        return "\n".join(lines)

    def __str__(self) -> str:  # pragma: no cover - convenience
        return self.summary()


def _to_report(raw: dict, path: str, *, channels=(), segments=(),
               measured_difference_bytes: bool = True) -> Report:
    finding_fields = {f.name for f in dataclasses.fields(Finding)}
    skipped_fields = {f.name for f in dataclasses.fields(SkippedSegment)}
    return Report(
        findings=tuple(
            Finding(**{k: v for k, v in f.items() if k in finding_fields})
            for f in raw["findings"]
        ),
        skipped=tuple(
            SkippedSegment(**{k: v for k, v in s.items() if k in skipped_fields})
            for s in raw["skipped"]
        ),
        segments_checked=raw["segments_checked"],
        segments_repaired=raw["segments_repaired"],
        checks_run=tuple(raw["checks_run"]),
        checks_repaired=tuple(raw["checks_repaired"]),
        path=path,
        channels=tuple(channels or ()),
        segments=tuple(segments or ()),
        measured_difference_bytes=bool(measured_difference_bytes),
    )


def _as_filter(value, name: str, item_type):
    """Normalise a channel/segment filter, where empty means "everything".

    `None` means unfiltered. A bare scalar does NOT: segments are numbered from
    0, so `segments=0` is the obvious way to name the first one, and treating a
    falsy scalar as "no filter" silently widened a repair from one segment to
    the whole session. A string is rejected outright rather than being iterated
    into characters.
    """
    if value is None:
        return []
    if isinstance(value, (str, bytes)):
        raise TypeError(
            f"{name} must be a sequence, not a single string — "
            f"pass [{value!r}] for one item"
        )
    if isinstance(value, (int, float)) and not isinstance(value, bool):
        raise TypeError(
            f"{name} must be a sequence, not a bare {type(value).__name__} — "
            f"pass [{value!r}] for one item. ({name}=0 would otherwise read as "
            f"'no filter', i.e. every segment.)"
        )
    try:
        return [item_type(v) for v in value]
    except TypeError as exc:  # not iterable at all
        raise TypeError(f"{name} must be a sequence or None, got {type(value).__name__}") from exc


def available_checks() -> list[Check]:
    """Every check, in the order they run. Ids are stable API."""
    return [Check(**c) for c in _backend().validation_checks()]


def describe_check(check_id: str) -> Check | None:
    """One check by id, or None if unknown."""
    for c in available_checks():
        if c.id == check_id:
            return c
    return None


class Validator:
    """Check one MEF 3.0 session. Read-only: it cannot write a byte.

    There is no repair method here by design — see :func:`repair_session`.

    Parameters
    ----------
    path : str
        A ``.mefd`` directory or a ``.mefd.tar`` archive.
    password : str, optional
        Needed to read section 2 of an encrypted session.
    channels : sequence of str, optional
        Restrict to these channels. Default: every channel.
    segments : sequence of int, optional
        Restrict to these segment numbers. Default: every segment.
    exact_difference_bytes : bool, default True
        Read every RED block header to learn the real
        ``maximum_difference_bytes``. It is the one declaration not derivable
        from the block index, so the exact answer costs one small read per
        block. Set ``False`` on very large sessions: the check then only
        reports a clearly-unset value, bounded by meflib's worst case, without
        touching ``.tdat`` at all.
    """

    def __init__(
        self,
        path: str,
        password: str = "",
        channels: Sequence[str] | None = None,
        segments: Sequence[int] | None = None,
        exact_difference_bytes: bool = True,
    ) -> None:
        self.path = str(path)
        # None is the idiom this package teaches elsewhere (Reader accepts it),
        # and the extension only takes str.
        self.password = password or ""
        self.channels = _as_filter(channels, "channels", str)
        self.segments = _as_filter(segments, "segments", int)
        self.exact_difference_bytes = bool(exact_difference_bytes)

    # --- inspection -------------------------------------------------------

    @staticmethod
    def available_checks() -> list[Check]:
        """Every check in the registry, in the order they run."""
        return available_checks()

    @staticmethod
    def describe_check(check_id: str) -> Check | None:
        """One check by id, or None if unknown."""
        return describe_check(check_id)

    # --- checking (read-only) ---------------------------------------------

    def validate(self, check_ids: Iterable[str] | None = None) -> Report:
        """Run every check (or only ``check_ids``) and return the full report.

        Never modifies the session.
        """
        raw = _backend().validate_session(
            self.path,
            password=self.password,
            channels=self.channels,
            # A bare string here used to be iterated into characters, producing
            # "unknown check id: s". repair_session and the constructor both
            # guard this; validate() was the one hole.
            check_ids=_as_filter(check_ids, "check_ids", str),
            segments=self.segments,
            exact_difference_bytes=self.exact_difference_bytes,
        )
        return _to_report(
            raw,
            self.path,
            channels=self.channels,
            segments=self.segments,
            measured_difference_bytes=self.exact_difference_bytes,
        )

    def check(self, check_id: str) -> Report:
        """Run one named check. Never modifies the session."""
        return self.validate([check_id])

    def __repr__(self) -> str:  # pragma: no cover - convenience
        return f"Validator({self.path!r})"


def validate_session(path: str, **kwargs) -> Report:
    """Check a session and return the report. Reads only.

    Accepts the same keyword arguments as :class:`Validator`.
    """
    return Validator(path, **kwargs).validate()


def repair_session(
    path: str,
    check_ids: Sequence[str],
    backup: bool = True,
    **kwargs,
) -> Report:
    """Rewrite the declarations named in ``check_ids`` — and only those.

    This is the only function in mef3io that modifies an existing session, and
    it is deliberately not a method of :class:`Validator`: inspecting a session
    and rewriting one should not be reachable through the same object.

    Every check still runs, so the returned report describes the whole session;
    ``Finding.repaired`` marks what was actually written — a repair that
    declines to change anything is not counted.

    Only declarations are rewritten: metadata section 2 and the universal
    headers of ``.tmet``/``.tidx``/``.tdat``. Sample data and the block index
    are never touched. A segment whose CRCs do not verify is reported and left
    alone, and a ``.mefd.tar`` archive is refused outright (extract it first).

    Parameters
    ----------
    path : str
        A ``.mefd`` directory. Archives cannot be rewritten in place.
    check_ids : sequence of str
        Which repairs to apply. Required and non-empty — repairs are never
        implicit. :attr:`Report.repairable_check_ids` lists the candidates.
    backup : bool, default True
        Copy each file before rewriting it, into ``<session>.repair-backup/``
        (outside the session tree, so no reader mistakes a backup for data).
        An existing backup is never overwritten.
    **kwargs
        The same keyword arguments as :class:`Validator` (``password``,
        ``channels``, ``segments``, ``exact_difference_bytes``).

    Raises
    ------
    ValueError
        If ``check_ids`` is empty, or names an unknown or non-repairable check.
    """
    if isinstance(check_ids, (str, bytes)):
        raise TypeError(
            "check_ids must be a sequence of ids, not a single string — "
            "pass [check_id] to apply one repair"
        )
    ids = list(check_ids)
    if not ids:
        raise ValueError(
            "repair_session() needs an explicit list of check ids — repairs are never "
            "implicit. Use Validator(path).validate().repairable_check_ids to see the "
            "candidates."
        )
    v = Validator(path, **kwargs)
    raw = _backend().repair_session(
        v.path,
        ids,
        password=v.password,
        channels=v.channels,
        segments=v.segments,
        check_ids=[],
        exact_difference_bytes=v.exact_difference_bytes,
        backup=bool(backup),
    )
    return _to_report(
        raw,
        v.path,
        channels=v.channels,
        segments=v.segments,
        measured_difference_bytes=v.exact_difference_bytes,
    )


@dataclasses.dataclass(frozen=True)
class RecoveredSegment:
    """One segment that an interrupted write left inconsistent."""

    channel: str = ""
    segment: int = 0
    path: str = ""
    blocks_before: int = 0
    blocks_after: int = 0
    blocks_recovered: int = 0
    blocks_dropped: int = 0
    tdat_bytes_dropped: int = 0
    action: str = ""

    def __str__(self) -> str:  # pragma: no cover - convenience
        return f"{self.path}: {self.action}"


@dataclasses.dataclass(frozen=True)
class RecoveryReport:
    """What `recover_session` found, and did or would do."""

    segments: tuple[RecoveredSegment, ...] = ()
    skipped: tuple[str, ...] = ()
    segments_examined: int = 0
    applied: bool = False
    backup_root: str = ""

    @property
    def nothing_to_do(self) -> bool:
        return not self.segments and not self.skipped

    def summary(self) -> str:
        lines = [
            f"{self.segments_examined} segment(s) examined, "
            f"{len(self.segments)} needing recovery"
            + ("" if self.applied else "  (DRY RUN — nothing was written)")
        ]
        for s in self.segments:
            lines.append(f"    {s.path}: {s.action}")
        for s in self.skipped:
            lines.append(f"    skipped {s}")
        if self.applied and self.backup_root:
            lines.append(f"  originals backed up under {self.backup_root}")
        if self.segments and not self.applied:
            lines.append("")
            lines.append("To apply:  mef3io.recover_session(path, apply=True)")
        if self.segments and self.applied:
            lines.append("")
            lines.append(
                "Now re-derive the declarations from the repaired index:\n"
                "    mef3io.repair_session(path, "
                "mef3io.Validator(path).validate().repairable_check_ids)"
            )
        return "\n".join(lines)


def recover_session(
    path: str,
    apply: bool = False,
    backup: bool = True,
    password: str = "",
) -> RecoveryReport:
    """Make each segment's block index and data agree after an interrupted write.

    This is **not** :func:`repair_session`, which only ever rewrites
    declarations and is safe to run on anything. Recovery may truncate the
    index, rebuild index entries from the data file, and drop a trailing
    fragment — so it is a dry run unless you ask for it, and it backs up what it
    changes first.

    An interrupted append leaves one of two shapes, and they are handled
    differently because one has lost data and the other has not:

    * **index ahead of data** — entries reference bytes that never landed.
      Those samples do not exist, so the entries are dropped.
    * **data ahead of index** — blocks reached ``.tdat`` but the index was not
      extended. Those samples *do* exist, so they are **recovered**: a RED block
      header carries the sample count, byte count, start time and discontinuity
      flag, which is everything an index entry needs. Only blocks whose CRC
      verifies are indexed; a torn tail is not a block.

    With the default ``durability="full"`` an append cannot leave either state.
    They are reachable with ``durability="fast"``, which is what makes that
    trade a reasonable one to offer.

    Parameters
    ----------
    path : str
        ``.mefd`` session directory. Tar archives are refused.
    apply : bool, default False
        False reports what it would do and writes nothing.
    backup : bool, default True
        Save what changes to ``<session>.recover-backup/`` first. Only the
        ``.tidx``, the ``.tdat``'s 1024-byte header and any dropped fragment —
        never the whole data file, which may be tens of gigabytes.
    password : str, optional
        Only needed for encrypted sessions.

    Returns
    -------
    RecoveryReport

    Notes
    -----
    Declarations are not updated here. Run :func:`repair_session` afterwards —
    ``python -m mef3io recover`` does it for you.
    """
    raw = _backend().recover_session(str(path), bool(apply), bool(backup), password or "")
    fields = {f.name for f in dataclasses.fields(RecoveredSegment)}
    return RecoveryReport(
        segments=tuple(
            RecoveredSegment(**{k: v for k, v in s.items() if k in fields})
            for s in raw["segments"]
        ),
        skipped=tuple(raw["skipped"]),
        segments_examined=raw["segments_examined"],
        applied=raw["applied"],
        backup_root=raw["backup_root"],
    )


# --- command line ----------------------------------------------------------


def _recover_main(argv) -> int:
    """`python -m mef3io recover` — see recover_session()."""
    import argparse

    p = argparse.ArgumentParser(
        prog="python -m mef3io recover",
        description=(
            "Make each segment's block index and data agree after an interrupted "
            "write. Dry run unless --apply. Unlike 'repair', this may change the "
            "block index and drop an incomplete trailing block."
        ),
    )
    p.add_argument("path", help="session .mefd directory")
    p.add_argument("--apply", action="store_true", help="actually write (default: dry run)")
    p.add_argument("--no-backup", action="store_true",
                   help="do not save what changes to <session>.recover-backup/")
    p.add_argument("--password", default="", help="for encrypted sessions")
    p.add_argument("--no-repair", action="store_true",
                   help="skip re-deriving the declarations afterwards")
    args = p.parse_args(list(argv))

    try:
        report = recover_session(
            args.path, apply=args.apply, backup=not args.no_backup, password=args.password
        )
    except (ValueError, RuntimeError, OSError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    print(report.summary())

    # A recovered index means the declarations derived from it are now stale.
    # Doing it here keeps the two operations separate in the API but joined in
    # the workflow, which is what an operator actually wants.
    if report.applied and report.segments and not args.no_repair:
        outstanding = Validator(args.path, password=args.password).validate()
        ids = outstanding.repairable_check_ids
        if ids:
            print()
            print(f"re-deriving declarations from the recovered index: {', '.join(ids)}")
            repair_session(args.path, ids, password=args.password)
        final = Validator(args.path, password=args.password).validate()
        print()
        print(final.summary())
        return 0 if final.ok else 1

    if report.skipped:
        return 1
    return 0



def _build_parser(repair: bool) -> "argparse.ArgumentParser":
    import argparse

    if repair:
        parser = argparse.ArgumentParser(
            prog="python -m mef3io repair",
            description="Rewrite the declarations named with --check, and only those. "
            "This command writes to the session; 'validate' never does.",
        )
    else:
        parser = argparse.ArgumentParser(
            prog="python -m mef3io validate",
            description="Check a MEF 3.0 session's declarations against its data. "
            "Never writes — use 'python -m mef3io repair' for that.",
        )
    parser.add_argument("path", nargs="?", help="a .mefd directory or .mefd.tar archive")
    parser.add_argument("--password", default="", help="password for an encrypted session")
    parser.add_argument(
        "--password-env",
        metavar="VAR",
        help="read the password from this environment variable instead of the command "
        "line, which is visible in ps output, shell history and CI logs",
    )
    parser.add_argument(
        "--channel",
        action="append",
        default=[],
        help="restrict to this channel (repeatable). A name that matches nothing "
        "examines no segments and is reported as such, not as a clean session.",
    )
    parser.add_argument(
        "--segment", action="append", type=int, default=[], help="restrict to this segment number"
    )
    parser.add_argument(
        "--check",
        action="append",
        default=[],
        metavar="CHECK_ID",
        help="repair this check id (repeatable, required)"
        if repair
        else "run only this check id (repeatable)",
    )
    if repair:
        parser.add_argument(
            "--no-backup", action="store_true", help="do not back up rewritten files"
        )
    parser.add_argument(
        "--fast",
        action="store_true",
        help="skip the per-block .tdat reads; maximum_difference_bytes is then bounded, "
        "not measured",
    )
    parser.add_argument("--list-checks", action="store_true", help="print the registry and exit")
    return parser


def _main(argv: Sequence[str] | None = None, repair: bool = False) -> int:
    parser = _build_parser(repair)
    args = parser.parse_args(argv)

    if args.list_checks:
        for c in available_checks():
            print(c)
        return 0
    if not args.path:
        parser.error("a session path is required (or --list-checks)")
    if repair and not args.check:
        parser.error(
            "repair needs at least one --check CHECK_ID — repairs are never implicit. "
            "Run 'python -m mef3io validate PATH' first to see what is repairable."
        )

    password = args.password
    if args.password_env:
        if args.password:
            parser.error("pass --password or --password-env, not both")
        password = os.environ.get(args.password_env)
        if password == "":
            # Set-but-empty is almost always a shell quoting slip. Left alone it
            # degrades into "the password is missing or incorrect" on every
            # segment, which sends the operator looking for the wrong problem.
            parser.error(
                f"environment variable {args.password_env} is set but empty; "
                f"unset it to validate without a password"
            )
        if password is None:
            parser.error(f"environment variable {args.password_env} is not set")
    opts = dict(
        password=password,
        channels=args.channel,
        segments=args.segment,
        exact_difference_bytes=not args.fast,
    )
    for check_id in args.check:
        if describe_check(check_id) is None:
            parser.error(
                f"unknown check id: {check_id}\n"
                "run 'python -m mef3io validate --list-checks' to see them"
            )
    try:
        if repair:
            report = repair_session(
                args.path, args.check, backup=not args.no_backup, **opts
            )
        else:
            report = Validator(args.path, **opts).validate(args.check)
    except (RuntimeError, ValueError, OSError) as exc:
        # A bad path or password reaches here as a C++ exception. A researcher
        # who typos a path should get one line, not a stack trace — and the
        # exit code must be distinguishable from "the session has defects".
        print(f"error: {exc}", file=sys.stderr)
        return 2
    print(report.summary())
    return 0 if report.ok else 1


if __name__ == "__main__":  # pragma: no cover
    sys.exit(_main())
