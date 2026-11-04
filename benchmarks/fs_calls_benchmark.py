"""File-system call counts of the copy operations, per file kind and phase.

For network drives the NUMBER of requests matters more than the bytes: one
open/stat on an SMB or NFS share is a round trip. This script builds a
synthetic set of sources (SOURCES x CHANNELS x SEGMENTS, with annotations),
then runs one operation in a child process under `strace -f -y` and counts,
per file kind (src/dst .tdat .tidx .tmet .rdat .ridx, session folders) and per
phase, the opens, stats, reads, writes, seeks, fsyncs and renames. Needs
strace (Linux). Synthetic data only.

    python benchmarks/fs_calls_benchmark.py [--op merge|anonymize|transform|archive|all]
        [--sources 4] [--channels 64] [--segments 3] [--seconds 60] [--threads 1]
        [--records keep|drop] [--json out.json]

PHASES are told from the order of events in the (single-threaded by default)
trace: PLAN = before the first file is created under the destination;
COPY = until the first directory listing of the destination (the verifier's); VERIFY = until the first
rename; PUBLISH = from the first rename on. For `archive` there is one phase.
With --threads > 1 the phases overlap and the split is only approximate.
"""
from __future__ import annotations

import argparse
import collections
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import warnings
from pathlib import Path

import numpy as np

SECOND = 1_000_000
START = 1_577_880_000_000_000
STAT_CALLS = {"stat", "lstat", "statx", "newfstatat", "access", "faccessat", "faccessat2",
              "fstat", "readlink", "readlinkat"}
READ_CALLS = {"read", "pread64", "readv", "preadv"}
WRITE_CALLS = {"write", "pwrite64", "writev", "pwritev"}
SYNC_CALLS = {"fsync", "fdatasync", "syncfs", "sync_file_range"}
TRACE = ("openat,open,creat,stat,lstat,statx,newfstatat,access,faccessat,faccessat2,fstat,"
         "rename,renameat,renameat2,unlink,unlinkat,rmdir,mkdir,mkdirat,fsync,fdatasync,syncfs,"
         "read,pread64,readv,pwrite64,write,writev,lseek,ftruncate,getdents64,close")
COLS = ["opens", "stats", "reads", "rbytes", "writes", "wbytes", "seeks", "syncs", "dirops",
        "renames", "unlinks"]


def make_sources(root: Path, n: int, channels: int, segments: int, seconds: int, fs: float):
    import mef3io
    paths = []
    for k in range(n):
        rng = np.random.default_rng(k)
        p = root / f"src{k}.mefd"
        t0 = START + k * (segments * seconds + 10) * SECOND
        meta = mef3io.Metadata(subject=mef3io.Subject(name_1=f"Patient{k}", id=f"ID{k:03d}"))
        with mef3io.Writer(str(p), overwrite=True, metadata=meta) as w:
            for s in range(segments):
                ts = t0 + s * seconds * SECOND
                for c in range(channels):
                    x = np.cumsum(rng.integers(-5, 6, int(seconds * fs))).astype(float)
                    w.write(f"ch{c:03d}", x, ts, fs, precision=0, new_segment=(s > 0))
            w.write_annotations([{"type": "Note", "time": t0 + SECOND, "text": f"note {k}"}])
        paths.append((p, t0))
    return paths


def run_inner(op: str, work: Path, threads: int, records: str, n_sources: int, seconds: int,
              segments: int):
    """The traced child: only the operation."""
    import mef3io
    warnings.simplefilter("ignore")
    srcs = [work / f"src{k}.mefd" for k in range(n_sources)]
    shift = 50 * 365 * 86400 * SECOND
    dst = work / "out.mefd"
    if op == "merge":
        span = segments * seconds * SECOND
        ranges = [(START + k * (span + 10 * SECOND) + span // 10,
                   START + k * (span + 10 * SECOND) + span * 9 // 10) for k in range(n_sources)]
        rep = mef3io.merge_sessions(srcs, dst, time_ranges=ranges, records=records,
                                    anonymize={"shift_us": shift}, n_threads=threads)
        assert rep.verification.ok
    elif op == "anonymize":
        rep = mef3io.anonymize_session(srcs[0], dst, shift_us=shift, records=records)
        assert rep.verification.ok
    elif op == "transform":
        rep = mef3io.transform_session(srcs[0], dst, records=records)
        assert rep.verification.ok
    elif op == "archive":
        mef3io.archive_session(srcs[0], work / "out.mefd.tar")
    else:
        raise SystemExit(op)


LINE = re.compile(r"^(?:(\d+)\s+)?(\w+)\((.*)\)\s+=\s+(-?\d+|\?)")
UNF = re.compile(r"^(?:(\d+)\s+)?(\w+)\((.*?)(?:, )?<unfinished \.\.\.>")
RES = re.compile(r"^(?:(\d+)\s+)?<\.\.\. (\w+) resumed>(.*)\)\s+=\s+(-?\d+|\?)")
QUOTED = re.compile(r'"((?:[^"\\]|\\.)*)"')
FDPATH = re.compile(r"^(\d+)<(.*?)>")


def parse(logfile: Path):
    pend = {}
    for raw in logfile.read_text(errors="replace").splitlines():
        m = UNF.match(raw)
        if m:
            pend[m.group(1)] = (m.group(2), m.group(3))
            continue
        m = RES.match(raw)
        if m:
            name, args = pend.pop(m.group(1), (m.group(2), ""))
            yield name, args + m.group(3), int(m.group(4)) if m.group(4) != "?" else -1
            continue
        m = LINE.match(raw)
        if m:
            yield m.group(2), m.group(3), int(m.group(4)) if m.group(4) != "?" else -1


def kind_of(path: str, work: Path):
    try:
        rel = Path(path).relative_to(work)
    except ValueError:
        return None
    top = rel.parts[0] if rel.parts else ""
    if top.startswith("src"):
        side = "src"
    elif top.startswith("out.mefd.tar"):
        side = "dst"
    elif top.startswith("out"):
        side = "dst"
    else:
        return None
    name = rel.name
    for ext in (".tdat", ".tidx", ".tmet", ".rdat", ".ridx"):
        if ext in name:
            return f"{side} {ext}"
    if ".tar" in name:
        return f"{side} .tar"
    return f"{side} folder/other"


def analyse(logfile: Path, work: Path, one_phase: bool):
    cur = "plan"
    phases = ["plan", "copy", "verify", "publish"]
    stats = {p: collections.defaultdict(lambda: collections.Counter()) for p in phases}
    files = collections.defaultdict(set)
    last_dst_open = None
    for name, args, ret in parse(logfile):
        paths = QUOTED.findall(args)
        fdp = FDPATH.match(args)
        path = fdp.group(2) if (fdp and name not in ("openat", "open")) else (paths[0] if paths else None)
        if name == "renameat2" or name in ("rename", "renameat"):
            if cur != "publish" and not one_phase:
                cur = "publish"
        k = kind_of(path, work) if path else None
        if k is None:
            continue
        if name in ("openat", "open", "creat"):
            if ret < 0:
                kk = "stats"          # a failed open is a probe
            else:
                kk = "opens"
                if k.startswith("dst") and cur == "plan" and not one_phase and (
                        "O_CREAT" in args or "O_WRONLY" in args or "O_RDWR" in args):
                    cur = "copy"
                if k.startswith("dst"):
                    last_dst_open = (cur, k)
            files[k].add(path)
            stats[cur][k][kk] += 1
        elif name in STAT_CALLS:
            stats[cur][k]["stats"] += 1
        elif name in READ_CALLS:
            stats[cur][k]["reads"] += 1
            stats[cur][k]["rbytes"] += max(ret, 0)
        elif name in WRITE_CALLS:
            stats[cur][k]["writes"] += 1
            stats[cur][k]["wbytes"] += max(ret, 0)
        elif name == "lseek":
            stats[cur][k]["seeks"] += 1
        elif name in SYNC_CALLS:
            stats[cur][k]["syncs"] += 1
        elif name in ("getdents64", "mkdir", "mkdirat", "rmdir"):
            if name == "getdents64" and k.startswith("dst") and cur == "copy" and not one_phase:
                # the copy never lists what it wrote: the verifier starts here
                # (the open of the folder that led to this belongs to it)
                if last_dst_open and last_dst_open[0] == "copy":
                    stats["copy"][last_dst_open[1]]["opens"] -= 1
                    stats["verify"][last_dst_open[1]]["opens"] += 1
                cur = "verify"
            stats[cur][k]["dirops"] += 1
        elif name.startswith("rename"):
            stats[cur][k]["renames"] += 1
        elif name in ("unlink", "unlinkat"):
            stats[cur][k]["unlinks"] += 1
    return stats, {k: len(v) for k, v in files.items()}


def report(op, stats, nfiles, one_phase):
    print(f"\n== {op} ==")
    for ph, d in stats.items():
        if not d or (one_phase and ph != "plan"):
            continue
        label = "all" if one_phase else ph.upper()
        print(f"-- {label}")
        print(f"{'kind':<20}{'files':>7}" + "".join(f"{c:>9}" for c in COLS) + f"{'opens/file':>12}")
        for k in sorted(d):
            c = d[k]
            nf = max(nfiles.get(k, 1), 1)
            print(f"{k:<20}{nfiles.get(k, 0):>7}" + "".join(
                f"{c[x] if x not in ('rbytes', 'wbytes') else c[x] // 1024:>9}" for x in COLS)
                + f"{c['opens'] / nf:>12.2f}")
    print("(rbytes / wbytes in KiB)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--op", default="merge", choices=["merge", "anonymize", "transform", "archive", "all"])
    ap.add_argument("--sources", type=int, default=4)
    ap.add_argument("--channels", type=int, default=64)
    ap.add_argument("--segments", type=int, default=3)
    ap.add_argument("--seconds", type=int, default=60, help="per segment")
    ap.add_argument("--fs", type=float, default=250.0)
    ap.add_argument("--threads", type=int, default=1)
    ap.add_argument("--records", default="keep", choices=["keep", "drop"])
    ap.add_argument("--json")
    ap.add_argument("--inner", help=argparse.SUPPRESS)
    a = ap.parse_args()
    if a.inner:
        run_inner(a.inner, Path(os.environ["FSBENCH_WORK"]), a.threads, a.records, a.sources,
                  a.seconds, a.segments)
        return
    if not shutil.which("strace"):
        sys.exit("strace is required")
    ops = ["merge", "anonymize", "transform", "archive"] if a.op == "all" else [a.op]
    warnings.simplefilter("ignore")
    work = Path(tempfile.mkdtemp(prefix="mef3io_fscalls_")).resolve()
    out = {}
    try:
        make_sources(work, a.sources, a.channels, a.segments, a.seconds, a.fs)
        for op in ops:
            for p in (work / "out.mefd", work / "out.mefd.partial", work / "out.mefd.tar"):
                if p.is_dir():
                    shutil.rmtree(p)
                elif p.exists():
                    p.unlink()
            log = work / f"{op}.strace"
            env = dict(os.environ, FSBENCH_WORK=str(work))
            cmd = ["strace", "-f", "-y", "-s", "200", "-e", "trace=" + TRACE, "-o", str(log),
                   sys.executable, __file__, "--inner", op, "--sources", str(a.sources),
                   "--segments", str(a.segments), "--seconds", str(a.seconds),
                   "--threads", str(a.threads), "--records", a.records]
            subprocess.run(cmd, check=True, env=env)
            one = op == "archive"
            stats, nfiles = analyse(log, work, one)
            report(f"{op} ({a.sources if op == 'merge' else 1} src x {a.channels} ch x {a.segments} seg, threads={a.threads}, "
                   f"records={a.records})", stats, nfiles, one)
            out[op] = {ph: {k: dict(v) for k, v in d.items()} for ph, d in stats.items()}
            out[op]["files"] = nfiles
    finally:
        shutil.rmtree(work, ignore_errors=True)
    if a.json:
        Path(a.json).write_text(json.dumps(out, indent=1))


if __name__ == "__main__":
    main()
