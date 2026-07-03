# mef3io for MATLAB

MATLAB binding over the same C++ core as the Python package — identical
semantics (NaN gaps, precision inference, in-segment appends, encryption) and
the same version number.

## Build (one-time, ~30 s)

The MEX is compiled from source; you need a **C++20 compiler** configured for
MEX (`mex -setup C++`): Xcode on macOS, GCC ≥ 11 on Linux, Visual Studio 2022
on Windows. Then:

```matlab
run('<repo>/matlab/build_mex.m')     % produces matlab/mef3io_mex.<mexext>
addpath('<repo>/matlab')             % put +mef3io and the MEX on the path
```

## Usage

```matlab
% ---- write ----
w = mef3io.Writer('session.mefd', Overwrite=true, Units='uV');
w.write('ch1', data, startUutc, 256);                 % NaN = gap, precision inferred
w.write('ch1', more, t2, 256);                        % appends in-segment
w.writeInt32('ch2', counts, 0.01, startUutc, 256);    % verbatim counts + factor
w.writeAnnotations(struct('time', t, 'text', 'note'), 'ch1');
delete(w);                                            % or let it go out of scope

% ---- read ----
r = mef3io.Reader('session.mefd');                    % password as 2nd arg if encrypted
r.channels
info = r.info('ch1');                                 % fs, ufact, times, subject metadata
x = r.read('ch1');                                    % double column, NaN gaps
x = r.read('ch1', t0, t1);                            % [t0, t1) window, uUTC
raw = r.readRaw('ch2');                               % int32 counts + logical valid mask
segs = r.segments('ch1');                             % what data is where, per segment
blocks = r.toc('ch1');                                % block-level table of contents
anns = r.records('ch1');                              % annotations
```

Times are **uUTC** (microseconds since the Unix epoch), int64 or double.

## Testing

```matlab
run('<repo>/matlab/test_mef3io.m')   % round trip incl. append, int32, encryption
```

The sessions it writes are readable by the Python package and pymef (and vice
versa) — that cross-check runs as part of the repo's validation.

## How it is put together

`mef3io_mex.cpp` is a single command-dispatch MEX over the flat C ABI
(`core/include/mef3io/c_api.h`), so C++ exceptions never cross the MEX
boundary — they surface as MATLAB errors with the C++ message. The `+mef3io`
package classes (`Reader`, `Writer`) manage the native handles (`delete`
closes; the MEX stays locked while handles are alive, so `clear mex` cannot
dangle them).
