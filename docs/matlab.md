# MATLAB guide

The MATLAB binding wraps the same C++ core as the Python package — identical
semantics, identical performance (see the
[benchmark](legacy_comparison.md#matlab-vs-python-binding-same-c-core)), and
the same version number. It ships as a `+mef3io` package (two handle classes)
over a single compiled MEX.

## Setup

Get `mef3io_mex.<mexext>` — either download the prebuilt binary for your
platform from a [GitHub release](https://github.com/bnelair/mef3io/releases)
into `matlab/`, or compile once (~30 s, needs a C++20 compiler via
`mex -setup C++`):

```matlab
run('<repo>/matlab/build_mex.m')
addpath('<repo>/matlab')
```

Verify: `mef3io_mex('version')` prints the library version. Test suite:
`run('<repo>/matlab/test_mef3io.m')`.

Times everywhere are **uUTC** (microseconds since the Unix epoch), int64 or
double. Errors from the core surface as MATLAB errors (id `mef3io:error`)
with the C++ message.

## Writing

```matlab
w = mef3io.Writer('session.mefd', Overwrite=true, Units='uV');
w.write('ch1', data, startUutc, 256);                 % NaN = gap, precision inferred
w.write('ch1', more, t2, 256);                        % appends in-segment
w.write('ch1', x, t3, 256, NewSegment=true);          % force a new segment
w.writeInt32('ch2', counts, 0.01, startUutc, 256, Valid=mask);
w.writeAnnotations(struct('time', t, 'text', 'note'), 'ch1');
delete(w);                                            % or let it go out of scope
```

`mef3io.Writer(path, ...)` name-value options: `Overwrite` (default false —
an existing session is reopened for appending), `Password1`/`Password2`
(both required for encryption), `Units`, `BlockLength`, `Threads`.

- **`write(channel, data, startUutc, fs, Precision=-1, NewSegment=false)`** —
  float path: NaN runs become gaps; `Precision` sets the conversion factor to
  `10^-precision`, `-1` infers it (or reuses the segment's on append).
  Returns a summary struct (`samples_written`, `blocks`, `gaps_skipped`,
  `segment`).
- **`writeInt32(channel, data, ufact, startUutc, fs, Valid=[], NewSegment=false)`**
  — verbatim int32 counts + conversion factor (bit-exact round trip).
  `data` must be integer-typed: floating-point input errors (use `write`
  for float data) and values beyond the int32 range error instead of
  saturating. `Valid` is any numeric/logical mask, nonzero = valid.
- **`writeAnnotations(records, channel)`** — struct array (or table) with
  fields `time` (required), `type` (default `'Note'`), `text`, `duration`.
  Omit `channel` for session-level records. Replaces records at that level.

## Reading

```matlab
r = mef3io.Reader('session.mefd');            % password as 2nd argument if encrypted
r.channels                                    % cellstr
info = r.info('ch1');                         % fs, ufact, times, subject metadata
x = r.read('ch1');                            % whole channel: double column, NaN gaps
x = r.read('ch1', t0, t1);                    % [t0, t1) window
raw = r.readRaw('ch2');                       % int32 counts + logical valid mask
segs = r.segments('ch1');                     % what data is where, per segment
blocks = r.toc('ch1');                        % block-level index (struct of vectors)
anns = r.records('ch1');                      % annotations; r.records() = session level
delete(r);
```

`info` returns `sampling_frequency`, `units_conversion_factor`,
`units_description`, `start_time`/`end_time` (int64 uUTC),
`number_of_samples` (stored samples; gaps not counted), `n_segments`,
`section3_available` and the subject metadata fields (empty under a level-1
password — the signal still reads correctly, unlike the legacy stack).

## How it is put together

`matlab/mef3io_mex.cpp` is a single command-dispatch MEX over the flat C ABI
([`core/include/mef3io/c_api.h`](cpp.md#the-flat-c-abi)), so C++ exceptions
never cross the MEX boundary. The `+mef3io` classes manage the native
handles: `delete` closes them, and the MEX keeps itself locked while any
handle is alive, so `clear mex` cannot dangle a session.
