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
| `Writer.write` | `Writer.write` |
| `Writer.write_int32` | `Writer.writeInt32` |
| `Writer.write_annotations` | `Writer.writeAnnotations` |
| `mef3io.archive_session` | `mef3io.archiveSession` |
| `mef3io.extract_session` | `mef3io.extractSession` |
