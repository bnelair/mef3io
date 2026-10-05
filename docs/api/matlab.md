# MATLAB API reference

The MATLAB binding mirrors the [Python API](python.md) one-to-one — a parity
test (`matlab/test_api_parity.m`, run in CI) asserts that every public method
of `mef3io.Reader` / `mef3io.Writer` corresponds to the Python method of the
same name and carries help text, so the two never drift.

The authoritative in-product docs are the class help, available in MATLAB via:

```matlab
doc mef3io.Reader        % or:  help mef3io.Reader
help mef3io.Writer/write
```

For usage and every method's arguments, see the [MATLAB guide](../matlab.md);
the semantics (NaN gaps, precision inference, in-segment append, the strict
`writeInt32` contract, encryption levels) are identical to Python because both
call the same C++ core.

## Method correspondence

| Python | MATLAB |
|---|---|
| `Reader.channels` | `Reader.channels` |
| `Reader.info` | `Reader.info` |
| `Reader.read` | `Reader.read` |
| `Reader.read_raw` | `Reader.readRaw` |
| `Reader.segments` | `Reader.segments` |
| `Reader.toc` | `Reader.toc` |
| `Reader.records` | `Reader.records` |
| `Reader.problems` | `Reader.problems` |
| `Writer.write` | `Writer.write` |
| `Writer.write_int32` | `Writer.writeInt32` |
| `Writer.write_annotations` | `Writer.writeAnnotations` |
| `mef3io.archive_session` | `mef3io.archiveSession` |
| `mef3io.extract_session` | `mef3io.extractSession` |
| `mef3io.recover_session` | `mef3io.recoverSession` |

## Unreadable segments

Both bindings default to **strict**: one segment that cannot be read fails the
whole session. That is deliberate — a skipped segment's samples come back as
NaN, and nothing in the returned array distinguishes that from a real recording
gap, so silently continuing is how missing data reaches an analysis unnoticed.

To salvage the intact remainder of a damaged archive, open lenient and check
what was skipped:

```matlab
r = mef3io.Reader(path, '', 0, false);   % path, password, nThreads, strict
p = r.problems();                        % channel, segment, path, reason
```

```python
r = mef3io.Reader(path, strict=False)
r.problems                               # same fields
```

Opening lenient raises `mef3io:unreadableSegment` in MATLAB and
`UnreadableSegmentWarning` in Python whenever anything was skipped. Both are
empty and silent on a healthy session.

## Durability and recovery

`mef3io.Writer` takes `Durability` (`'fast'`, the default, or `'full'`),
matching the Python `durability=` argument. These files are built by appending
for days to months, so the append is the hot path; `'fast'` performs no flushes,
as every MEF writer before this one did.

`mef3io.recoverSession` is the counterpart — it makes a session's block index
and data agree again after an unclean shutdown, rebuilding missing index
entries from the RED block headers. Dry run unless `Apply=true`.

```matlab
w = mef3io.Writer(p, Overwrite=true, Durability='fast');   % the default
w.writeInt32('ch1', counts, 0.1, t0, 512);
delete(w)

summary = mef3io.recoverSession(p);                        % dry run
summary = mef3io.recoverSession(p, Apply=true);
```

!!! note "Still Python-only"
    `Validator` / `validate_session` / `repair_session` have no MATLAB surface
    yet (see issue #18). Reading, writing, appending, archiving, extracting and
    recovering are at parity.
