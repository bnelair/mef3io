---
description: "Read a MEF 3.0 recording in MATLAB with mef3io - a whole channel, a time window, gaps as NaN, encrypted sessions and single-file .mefd.tar archives."
---

# Read MEF 3.0 in MATLAB

```matlab
addpath('<somewhere>/mef3io-matlab')   % the unzipped release bundle
r = mef3io.Reader('recording.mefd');
r.channels                             % channel names
x = r.read('Fp1');                     % whole channel, double column, gaps are NaN
info = r.info('Fp1');
fs = info.sampling_frequency;
```

Get the `mef3io-matlab-vX.Y.Z.zip` bundle from a
[GitHub release](https://github.com/bnelair/mef3io/releases); it works without
compiling anything ([Install](../install.md#matlab) has the details). `x`
holds the values in physical units (`info.units_description`, for
example `uV`), one per sample period from the channel's first sample to its
last.

## A time window

MEF 3.0 times are **microseconds since 1970-01-01 UTC** ("uUTC"). Ask for a
window `[t0, t1)`; only the data needed for it is read from disk, so this is
fast even on a recording of many gigabytes.

```matlab
firstMinute = r.read('Fp1', info.start_time, info.start_time + int64(60e6));

when = datetime(2024, 3, 5, 9, 30, 0, 'TimeZone', 'UTC');
t0 = int64(posixtime(when) * 1e6);
tenSeconds = r.read('Fp1', t0, t0 + int64(10e6));
```

A window returns `round((t1 - t0) * fs / 1e6)` samples. Sample `i` (counting
from 1) is at `t0 + (i - 1) * 1e6 / fs`.

## Gaps are NaN

Where the recording has no data (an electrode was off, acquisition paused),
`read` returns `NaN` at those positions instead of shifting later samples
earlier. Times stay correct.

```matlab
missing = isnan(x);
fprintf('%.1f%% of the span has no data\n', 100 * mean(missing));
clean = x(~missing);                   % only the recorded samples
segs = r.segments('Fp1');              % where the data is, without reading it
```

See [Long recordings with gaps](long_recordings.md) for finding gaps.

## Annotations

```matlab
anns = r.records();                    % session level: type, time (uUTC), text
anns = r.records('Fp1');               % one channel's annotations
```

## Encrypted sessions and archives

```matlab
r = mef3io.Reader('recording.mefd', 'your password');
r = mef3io.Reader('recording.mefd.tar');   % single-file archive, read in place
```

The level-2 password opens everything; the level-1 password reads the signals
but not the subject fields or the annotations
([what encryption protects](../encryption_model.md#what-encryption-protects)).

## More

The [MATLAB guide](../matlab.md) covers the rest: stored integer values
(`readRaw`), lenient opening of damaged sessions, writing and archives.
