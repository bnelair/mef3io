---
description: "MATLAB guide to mef3io: read and write MEF 3.0 (Multiscale Electrophysiology Format) recordings with the +mef3io Reader and Writer classes."
---

# MATLAB guide

The MATLAB binding wraps the same C++ core as the Python package — identical
semantics, identical performance (see the
[benchmark](legacy_comparison.md#matlab-vs-python-binding-same-c-core)), and
the same version number. It ships as a `+mef3io` package (two handle classes)
over a single compiled MEX.

## Setup

Either download the `mef3io-matlab-vX.Y.Z.zip` bundle from a
[GitHub release](https://github.com/bnelair/mef3io/releases) — classes and
the CI-tested MEX for all platforms in one ready-to-addpath folder (see
[Install](install.md#matlab) for the caveats) — or compile once from the repo
(~30 s, needs a C++20 compiler via `mex -setup C++`):

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
(both required for encryption), `Units`, `BlockLength`, `Threads`,
`Metadata` (below) and `Durability` (`'fast'`, the default, or `'full'`; see
[Long recordings](long_recordings.md)). After an unclean shutdown with
`'fast'`, `mef3io.recoverSession(path)` reports what is wrong (a dry run) and
`mef3io.recoverSession(path, Apply=true)` fixes it.

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
- **`setMetadata(md)`** / the `Metadata=` constructor option — session-wide
  subject/acquisition metadata. Use the object API (mirrors Python):

```matlab
md = mef3io.Metadata( ...
    subject=mef3io.Subject(id="MRN-123", name_1="Jane", recording_location="EMU-4"), ...
    acquisition=mef3io.Acquisition(session_description="pre-surgical", line_frequency=60));
w = mef3io.Writer('s.mefd', Overwrite=true, Metadata=md);
```

Every field defaults, so set only what you have. `setMetadata` also accepts a
plain struct with the flat field names (`subject_id`, `session_description`,
…). Read it back with `r.metadata`, a `mef3io.Metadata` object with `.subject`
and `.acquisition` (subject fields empty without a level-2 password).

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
anns = r.records('ch1');                      % ch1's channel- + segment-level annotations
anns = r.records();                           % session level; r.records([], [], 'all') = all
seg0 = r.records('ch1', 0);                   % one segment's records
delete(r);
```

Text comes back as UTF-8 decoded into MATLAB `char` (UTF-16) on every
platform — the same encoding strings are read in with — rather than through
the locale codepage, so non-ASCII annotations and subject names round-trip on
Windows too. Bytes that are not valid UTF-8 become U+FFFD, as in Python.

`mef3io.Reader(path, password, nThreads, strict)`: strict (`true`, the
default) fails on a segment that cannot be read; `false` skips it, reads its
span as NaN, raises the warning `mef3io:unreadableSegment`, and lists it in
`r.problems()`.

`info` returns `sampling_frequency`, `units_conversion_factor`,
`units_description`, `start_time`/`end_time` (int64 uUTC),
`number_of_samples` (stored samples; gaps not counted), `n_segments`,
`section3_available` and the subject metadata fields (empty under a level-1
password — the signal still reads correctly, unlike the legacy stack).

## Tar archives (single-file sessions)

Pack a session into one uncompressed tar file and read it in place — handy
for sharing and safe archiving:

```matlab
tarPath = mef3io.archiveSession('session.mefd');   % -> 'session.mefd.tar'
r = mef3io.Reader(tarPath);                        % no extraction needed
```

!!! note "A mef3io addition, not part of MEF 3.0"
    meflib, pymef and mef_tools cannot open a `.mefd.tar` directly. It is a
    plain uncompressed tar file, so it can always be unpacked:
    `mef3io.extractSession`, `tar -xf session.mefd.tar` (built into Windows 10 and
    later, macOS and Linux) or the free 7-Zip on Windows. The unpacked
    `.mefd` folder works with the legacy tools.

Windowed reads stay cheap (the archive is uncompressed, so only the needed
byte ranges are fetched), and any tar tool restores the directory
(`tar -xf session.mefd.tar`). Tar sessions are read-only — `mef3io.Writer`
refuses `.tar` paths; unpack first to modify:

```matlab
sessionDir = mef3io.extractSession(tarPath);       % -> 'session.mefd' back
w = mef3io.Writer(sessionDir);                     % writable again
```

`archiveSession(dir, tarPath, Overwrite=true)` / `extractSession(tarPath,
destDir, Overwrite=true)` pick targets and replace existing ones; they mirror
Python's `mef3io.archive_session` / `extract_session`.

Session naming is enforced at every entry point: directories must end
`.mefd`, archives `.mefd.tar` — anything else is refused, so a stray
directory or file can never be misread (or overwritten) as a session.

## Not in MATLAB: inspect, cancelling and progress

`mef3io.inspect` (a description of a session without reading its samples, see
[the Python guide](python.md#inspecting-a-session-without-reading-its-samples))
exists in Python and C++ only; neither MATLAB nor the C ABI has it yet.

The Python, C++ and C APIs can cancel a read or a long operation from another
thread and report its progress
([Cancelling and progress](python.md#cancelling-and-progress)). The MATLAB
classes do not expose this yet: a MEX call blocks MATLAB's single interpreter
thread, so there is no second MATLAB thread to cancel from. The C ABI has the
hooks (`mef3io_cancel_token_*`, the `*_ex` functions) for when this is wired
up.

## How it is put together

`matlab/mef3io_mex.cpp` is a single command-dispatch MEX over the flat C ABI
([`core/include/mef3io/c_api.h`](cpp.md#the-flat-c-abi)), so C++ exceptions
never cross the MEX boundary. The `+mef3io` classes manage the native
handles: `delete` closes them, and the MEX keeps itself locked while any
handle is alive, so `clear mex` cannot dangle a session.
