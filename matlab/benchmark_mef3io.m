function benchmark_mef3io(outDir)
%BENCHMARK_MEF3IO Write/read benchmark of the MATLAB binding.
%   Same workload as examples/08: 5 channels x 5 h at 512 Hz (~88 MB
%   session) of smoothed noise with one NaN gap, precision 3 — written and
%   read back plain and encrypted. Compare with
%   benchmarks/bindings_benchmark.py (the Python twin) on the same machine.

if nargin < 1, outDir = tempname; end
if ~exist(outDir, 'dir'), mkdir(outDir); end

fs = 512;
lenS = 3600 * 5;
nCh = 5;
n = fs * lenS;
start = int64(1577836800000000);
channels = arrayfun(@(i) sprintf('ch%d', i), 1:nCh, 'UniformOutput', false);

rng(42);
x = filter(ones(10, 1) / 10, 1, randn(n, nCh));   % smoothed noise (like 08)
x(1001:2000, 1) = NaN;

fprintf('mef3io MATLAB benchmark — %d ch x %d h @ %d Hz (%.1f M samples)\n', ...
        nCh, lenS / 3600, fs, n * nCh / 1e6);
fprintf('MATLAB %s, mef3io %s\n\n', version('-release'), mef3io_mex('version'));

for enc = [false true]
    label = 'plain';
    p1 = ''; p2 = '';
    if enc, label = 'encrypted'; p1 = 'pwd1'; p2 = 'pwd2'; end
    path = fullfile(outDir, ['bench_' label '.mefd']);

    t = tic;
    w = mef3io.Writer(path, Overwrite=true, Password1=p1, Password2=p2, Units='uV');
    for i = 1:nCh
        w.write(channels{i}, x(:, i), start, fs, Precision=3);
    end
    delete(w);
    tWrite = toc(t);

    d = dir(fullfile(path, '**', '*'));
    sizeMB = sum([d(~[d.isdir]).bytes]) / 1e6;

    t = tic;
    r = mef3io.Reader(path, p2);
    total = 0;
    for i = 1:nCh
        y = r.read(channels{i});
        total = total + numel(y);
    end
    tRead = toc(t);

    % sanity: values survive the round trip
    assert(total == n * nCh);
    ok = ~isnan(x(:, 1));
    yv = r.read(channels{1});
    assert(max(abs(yv(ok) - round(x(ok, 1), 3))) < 1e-9);
    assert(sum(isnan(yv)) == 1000);
    delete(r);

    fprintf('%-9s  write %6.2f s (%5.1f MS/s)   read %6.2f s (%5.1f MS/s)   %.1f MB\n', ...
            label, tWrite, n * nCh / tWrite / 1e6, tRead, n * nCh / tRead / 1e6, sizeMB);
end
end
