function test_mef3io(sessionDir)
%TEST_MEF3IO Round-trip test of the MATLAB binding. Errors on failure.
%   test_mef3io()            % uses tempdir
%   test_mef3io(dir)         % writes <dir>/matlab_rt.mefd (kept for
%                            % cross-language checks)

if nargin < 1, sessionDir = tempname; end
if ~exist(sessionDir, 'dir'), mkdir(sessionDir); end
path = fullfile(sessionDir, 'matlab_rt.mefd');

fs = 256;
start = int64(1577836800000000);
n = 5000;
x = 40 * sin((0:n-1)' / 20);
x(1001:1250) = NaN;                       % a gap
chunkUs = int64(round(n / fs * 1e6));

% ---- write: float with gap, in-segment append, int32, annotations --------
w = mef3io.Writer(path, Overwrite=true, Units='uV');
s1 = w.write('ch1', x, start, fs, Precision=3);
assert(s1.samples_written == n - 250 && s1.segment == 0);
s2 = w.write('ch1', x, start + chunkUs, fs);          % in-segment append
assert(s2.segment == 0);

counts = int32(mod(0:n-1, 100))';
valid = true(n, 1); valid(11:20) = false;
w.writeInt32('ch2', counts, 0.01, start, fs, Valid=valid);

w.writeAnnotations(struct('time', {start + 1e6, start + 2e6}, ...
                          'type', {'Note', 'EDFA'}, ...
                          'text', {'marker', 'artifact'}, ...
                          'duration', {[], int64(5e5)}), 'ch1');
delete(w);

% ---- read back ------------------------------------------------------------
r = mef3io.Reader(path);
assert(isequal(r.channels, {'ch1'; 'ch2'}));

info = r.info('ch1');
assert(info.sampling_frequency == fs);
assert(info.number_of_samples == 2 * (n - 250));      % stored, gaps excluded
assert(info.n_segments == 1);                          % append stayed in-segment
assert(strcmp(info.units_description, 'uV'));
assert(info.section3_available);

y = r.read('ch1');
assert(numel(y) == 2 * n);
assert(all(isnan(y(1001:1250))) && all(isnan(y(n + (1001:1250)))));
ok = ~isnan(x);
assert(max(abs(y(ok) - round(x(ok), 3))) < 1e-9);      % quantized round trip

yw = r.read('ch1', start + int64(1e6), start + int64(2e6));
assert(numel(yw) == fs);

raw = r.readRaw('ch2');
assert(isa(raw.samples, 'int32') && islogical(raw.valid));
assert(isequal(raw.samples(raw.valid), counts(valid)));  % bit-exact
assert(~any(raw.valid(11:20)));
y2 = r.read('ch2');
assert(max(abs(y2(valid) - double(counts(valid)) * 0.01)) < 1e-12);

segs = r.segments('ch1');
assert(numel(segs) == 1 && segs(1).number_of_samples == 2 * (n - 250));
blocks = r.toc('ch1');
assert(sum(blocks.number_of_samples) == 2 * (n - 250));
assert(blocks.discontinuity(1));

anns = r.records('ch1');
assert(numel(anns) == 2);
assert(strcmp(anns(1).text, 'marker') && isempty(anns(1).duration));
assert(strcmp(anns(2).type, 'EDFA') && anns(2).duration == int64(5e5));
delete(r);

% ---- encrypted round trip --------------------------------------------------
encPath = fullfile(sessionDir, 'matlab_enc.mefd');
w = mef3io.Writer(encPath, Overwrite=true, Password1='pwd1', Password2='pwd2');
w.write('ch1', x, start, fs, Precision=3);
delete(w);
r = mef3io.Reader(encPath, 'pwd2');
assert(r.info('ch1').section3_available);
yEnc = r.read('ch1');
assert(isequaln(yEnc, y(1:n)));
delete(r);
r = mef3io.Reader(encPath, 'pwd1');                    % L1: signal readable
assert(~r.info('ch1').section3_available);
assert(isequaln(r.read('ch1'), yEnc));
delete(r);
gotError = false;
try mef3io.Reader(encPath, 'wrong'); catch, gotError = true; end
assert(gotError, 'wrong password must be rejected');

% ---- input flexibility: types are coerced, edge windows are legal ----------
flexPath = fullfile(sessionDir, 'matlab_flex.mefd');
w = mef3io.Writer(flexPath, Overwrite=true);
w.write('s', single(x(1:100)), start, fs, Precision=3);        % single -> double
w.write('i', int16(1:100), start, fs, Precision=0);            % ints -> double
w.writeInt32('c', int32(1:100), 0.5, single(start), fs, ...    % single timestamp
             Valid=[ones(1, 50), zeros(1, 10), ones(1, 40)]);  % double mask
delete(w);
r = mef3io.Reader(flexPath);
assert(max(abs(r.read('i') - (1:100)')) < 1e-12);
raw = r.readRaw('c');
assert(~any(raw.valid(51:60)) && sum(raw.valid) == 90);
assert(isempty(r.read('s', start, start)));                    % empty window
assert(isempty(r.read('s', start + 1e6, start)));              % inverted window
delete(r);

fprintf('test_mef3io: all assertions passed (%s)\n', sessionDir);
end
