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
w = mef3io.Writer(fresh(path), Units='uV');
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

% One thread count per object, changeable later; results never depend on it.
assert(r.threads() == 0);                              % automatic by default
r.setThreads(1);
assert(r.threads() == 1);
assert(isequaln(r.read('ch1'), y));
r.setThreads(0);

yw = r.read('ch1', start + int64(1e6), start + int64(2e6));
assert(numel(yw) == fs);

% ---- argument errors: clean MATLAB errors, never a crash -----------------
% A fractional double time (what pure-double code computing n/fs*1e6 makes)
% is rounded to the nearest microsecond, not rejected.
yf = r.read('ch1', double(start) + 1e6 + 0.2, double(start) + 2e6 + 0.2);
assert(isequaln(yf, yw), 'a fractional time must round to the nearest microsecond');
gotError = false;                                      % a string is not a time
try r.read('ch1', 'abc', []); catch err, gotError = strcmp(err.identifier, 'mef3io:error'); end
assert(gotError, 'a string time must raise a clean mef3io:error');
gotError = false;                                      % neither is NaN
try r.read('ch1', NaN, []); catch err, gotError = strcmp(err.identifier, 'mef3io:error'); end
assert(gotError, 'a NaN time must raise a clean mef3io:error');
gotError = false;                                      % segment must be an integer
try r.records('ch1', 0.5); catch err, gotError = strcmp(err.identifier, 'mef3io:error'); end
assert(gotError, 'a fractional segment must raise a clean mef3io:error');

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
% every record says where it came from (session / channel / segment level)
assert(strcmp(anns(1).level, 'channel') && strcmp(anns(1).channel, 'ch1'));
assert(isempty(anns(1).segment) && anns(1).version_major == 1);
assert(isequal(r.records('ch1', [], 'channel'), anns));
assert(isempty(r.records('ch1', [], 'segment')));      % none written at that level
assert(isempty(r.records('ch1', 0)));
assert(numel(r.records([], [], 'all')) == numel(r.records()) + 2);
gotError = false;                                      % a level outside the scope
try r.records('ch1', [], 'session'); catch, gotError = true; end
assert(gotError, 'session-level records cannot be asked of a channel');
delete(r);

% ---- encrypted round trip --------------------------------------------------
encPath = fullfile(sessionDir, 'matlab_enc.mefd');
w = mef3io.Writer(fresh(encPath), Password1='pwd1', Password2='pwd2');
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

% ---- non-ASCII passwords (#84): meflib's mapping, legacy fallback ---------
% char(233) = 'é'. A session keyed by mef3io <= 1.2.0 for 'bé' is reproduced
% exactly by keying with char([98 169]) ('b©'): meflib maps © to 0xA9, the
% byte the old mapping took for é.
utfPath = fullfile(sessionDir, 'matlab_utf8.mefd');
pw1 = ['b' char(233)]; pw2 = ['x' char(252) 'y'];      % 'bé', 'xüy'
w = mef3io.Writer(fresh(utfPath), Password1=pw1, Password2=pw2);
w.write('ch1', x, start, fs, Precision=3);
delete(w);
lastwarn('');
r = mef3io.Reader(utfPath, pw2);
[~, wid] = lastwarn();
assert(~strcmp(wid, 'mef3io:legacyPassword'), 'a new session must not need the fallback');
assert(isequaln(r.read('ch1'), yEnc));
delete(r);
oldPath = fullfile(sessionDir, 'matlab_utf8_old.mefd');
w = mef3io.Writer(fresh(oldPath), Password1=['b' char(169)], ...
                  Password2=['x' char(188) 'y']);       % old bytes of 'bé' / 'xüy'
w.write('ch1', x, start, fs, Precision=3);
delete(w);
lastwarn('');
r = mef3io.Reader(oldPath, pw2);
[~, wid] = lastwarn();
assert(strcmp(wid, 'mef3io:legacyPassword'), 'an old-mapping session must warn');
assert(isequaln(r.read('ch1'), yEnc));
delete(r);

% ---- input flexibility: types are coerced, edge windows are legal ----------
flexPath = fullfile(sessionDir, 'matlab_flex.mefd');
w = mef3io.Writer(fresh(flexPath));
w.write('s', single(x(1:100)), start, fs, Precision=3);        % single -> double
w.write('i', int16(1:100), start, fs, Precision=0);            % ints -> double
w.writeInt32('c', int32(1:100), 0.5, single(start), fs, ...    % single timestamp
             Valid=[ones(1, 50), zeros(1, 10), ones(1, 40)]);  % double mask
w.writeInt32('c16', int16(1:100), 0.5, start + 1e6, fs);       % narrower ints OK
gotError = false;                                              % floats must error
try w.writeInt32('cf', 1:100, 0.5, start, fs); catch, gotError = true; end
assert(gotError, 'writeInt32 must reject floating-point data');
gotError = false;                                              % out-of-range must error
try w.writeInt32('cr', int64(2^31), 1, start, fs); catch, gotError = true; end
assert(gotError, 'writeInt32 must reject values beyond int32');
delete(w);
r = mef3io.Reader(flexPath);
assert(max(abs(r.read('i') - (1:100)')) < 1e-12);
raw = r.readRaw('c');
assert(~any(raw.valid(51:60)) && sum(raw.valid) == 90);
assert(isempty(r.read('s', start, start)));                    % empty window
assert(isempty(r.read('s', start + 1e6, start)));              % inverted window
delete(r);

% ---- metadata round trip (object API, mirrors Python) ----------------------
mdPath = fullfile(sessionDir, 'matlab_md.mefd');
md = mef3io.Metadata( ...
    subject=mef3io.Subject(id="MRN-9", name_1="Alice", recording_location="ICU-2"), ...
    acquisition=mef3io.Acquisition(session_description="nightly", line_frequency=60));
w = mef3io.Writer(fresh(mdPath), Metadata=md);
w.write('ch1', x, start, fs, Precision=3);
delete(w);
r = mef3io.Reader(mdPath);
got = r.metadata;
assert(isa(got, 'mef3io.Metadata'));
assert(strcmp(got.subject.id, 'MRN-9'));
assert(strcmp(got.subject.name_1, 'Alice'));
assert(strcmp(got.subject.recording_location, 'ICU-2'));
assert(strcmp(got.acquisition.session_description, 'nightly'));
assert(got.acquisition.line_frequency == 60);
delete(r);

% ---- tar archive: single-file session, read in place, writer refuses -------
tarPath = mef3io.archiveSession(path);
assert(strcmp(tarPath, [path '.tar']));
rDir = mef3io.Reader(path);
rTar = mef3io.Reader(tarPath);
assert(isequal(rDir.channels, rTar.channels));
assert(isequaln(rDir.read('ch1'), rTar.read('ch1')));
assert(isequal(rDir.readRaw('ch2'), rTar.readRaw('ch2')));
assert(isequal(rDir.records('ch1'), rTar.records('ch1')));
segsTar = rTar.segments('ch1');
assert(contains(segsTar(1).path, '::'));               % tar member notation
delete(rDir); delete(rTar);
gotError = false;                                      % existing target refused
try mef3io.archiveSession(path); catch, gotError = true; end
assert(gotError, 'existing archive target must be refused');
firstBytes = fileread(tarPath);                        % deterministic re-archive
delete(tarPath);                                       % (the test removes its own file)
tarPath = mef3io.archiveSession(path);
assert(isequal(fileread(tarPath), firstBytes), 'archiveSession must be deterministic');
gotError = false;                                      % the removed option is refused
try mef3io.archiveSession(path, '', Overwrite=true); catch, gotError = true; end
assert(gotError, 'archiveSession Overwrite option was removed');
gotError = false;
try mef3io.Writer(path, Overwrite=true); catch, gotError = true; end
assert(gotError, 'Writer Overwrite option was removed');
assert(exist(fullfile(path, 'ch1.timd'), 'dir') == 7, 'a refused Writer must not delete');
gotError = false;                                      % tar sessions are read-only
try mef3io.Writer(tarPath); catch, gotError = true; end
assert(gotError, 'Writer must refuse tar archives');
assert(exist(tarPath, 'file') == 2);                   % and never delete them
gotError = false;                                      % naming: .mefd / .mefd.tar only
try mef3io.Writer(fullfile(sessionDir, 'bad_name')); catch, gotError = true; end
assert(gotError, 'Writer must refuse non-.mefd session names');
gotError = false;
try mef3io.extractSession(tarPath, fullfile(sessionDir, 'bad_name')); catch, gotError = true; end
assert(gotError, 'extractSession must refuse non-.mefd targets');
restored = mef3io.extractSession(tarPath, fullfile(sessionDir, 'restored.mefd'));
rRest = mef3io.Reader(restored);
rTar = mef3io.Reader(tarPath);
assert(isequaln(rRest.read('ch1'), rTar.read('ch1')));  % untar restores the session
delete(rRest); delete(rTar);
w = mef3io.Writer(restored);                            % ... and it is writable again
w.write('ch1', x, start + 3 * chunkUs, fs);
delete(w);
delete(tarPath);

% plain struct still accepted by setMetadata
mdPath2 = fullfile(sessionDir, 'matlab_md2.mefd');
w = mef3io.Writer(fresh(mdPath2), ...
                  Metadata=struct('subject_id', 'S2', 'session_description', 'via struct'));
w.write('ch1', x, start, fs, Precision=3);
delete(w);
r = mef3io.Reader(mdPath2);
assert(strcmp(r.metadata.subject.id, 'S2'));
delete(r);

% --- append + recovery -----------------------------------------------------
% 2.0 removed the Durability option: every write takes the one path (only the
% .tmet is flushed before its rename). A reopened writer appends; a healthy
% session needs no recovery and a dry run changes nothing.
durCounts = int32(mod(0:1999, 100))';
durStep = int64(round(2000 / fs * 1e6));
dp = fullfile(sessionDir, 'matlab_append_recover.mefd');
w = mef3io.Writer(fresh(dp));
w.writeInt32('ch1', durCounts, 0.5, start, fs);
delete(w);
w = mef3io.Writer(dp, WaitForDisk=true);                 % reopened: appends
w.writeInt32('ch1', durCounts, 0.5, start + durStep, fs); % returns once on disk
w.flush();                                               % nothing left: returns at once
delete(w);

r = mef3io.Reader(dp);
got = r.read('ch1');
delete(r);
assert(numel(got) == 4000, 'append: expected 4000 samples, got %d', numel(got));

summary = mef3io.recoverSession(dp);
assert(ischar(summary) && ~isempty(summary), 'recoverSession returned no summary');
[~, nSeg] = mef3io.recoverSession(dp, Apply=true);
assert(nSeg == 0, 'a healthy session should need no recovery, got %d', nSeg);

r = mef3io.Reader(dp);                                   % still reads the same
assert(isequaln(r.read('ch1'), got), 'recoverSession altered a healthy session');
delete(r);

% The removed Durability option is an error, and must not have written anything.
gotError = false;
try
    mef3io.Writer(fresh(fullfile(sessionDir, 'matlab_bad_dur.mefd')), ...
                  Durability='full');
catch
    gotError = true;
end
assert(gotError, 'Writer must reject the removed Durability option');
assert(exist(fullfile(sessionDir, 'matlab_bad_dur.mefd'), 'dir') ~= 7, ...
    'a rejected Durability must not create a session');

% Tar archives are read-only, so recovery must refuse one rather than write.
% Archive built HERE rather than reusing `tarPath` from the tar block above:
% that one is deleted at its end (line ~167), so depending on it made this
% assertion fail on a file the suite had already removed.
recTarPath = mef3io.archiveSession(dp);
gotError = false;
try mef3io.recoverSession(recTarPath, Apply=true); catch, gotError = true; end
assert(gotError, 'recoverSession must refuse a tar archive');
assert(exist(recTarPath, 'file') == 2, 'the archive must be left alone');
delete(recTarPath);

% --- per-segment isolation: one bad file must not fail the whole session ----
% Strict (the default) still fails loudly; lenient skips the damaged segment,
% reports it, and returns the intact one. Mirrors tests/test_p20 in Python.
isoPath = fullfile(sessionDir, 'matlab_iso.mefd');
isoCounts = int32(mod(0:1999, 100))';
w = mef3io.Writer(fresh(isoPath));
w.writeInt32('ch1', isoCounts, 0.5, start, fs);
w.writeInt32('ch1', isoCounts, 0.5, start + int64(1e7), fs, NewSegment=true);
delete(w);

r = mef3io.Reader(isoPath);
assert(numel(r.segments('ch1')) == 2, 'expected two segments');
assert(isempty(r.problems()), 'a healthy session has no problems');
delete(r);

% Corrupt the SECOND segment's metadata body, past the universal header.
segDirs = dir(fullfile(isoPath, 'ch1.timd', '*.segd'));
assert(numel(segDirs) == 2, 'expected two .segd directories');
badMeta = dir(fullfile(segDirs(2).folder, segDirs(2).name, '*.tmet'));
badMetaPath = fullfile(badMeta(1).folder, badMeta(1).name);
fid = fopen(badMetaPath, 'r+');
assert(fid > 0, 'could not open the .tmet for corruption');
fseek(fid, 2000, 'bof');
origByte = fread(fid, 1, 'uint8');                     % flip it, do not set it:
fseek(fid, 2000, 'bof');                               % writing a fixed value
fwrite(fid, bitxor(uint8(origByte), uint8(255)), 'uint8');  % that already matched
fclose(fid);                                           % would corrupt nothing

gotError = false;                                      % strict: fail loudly
try mef3io.Reader(isoPath); catch, gotError = true; end
assert(gotError, 'a corrupt segment must fail a strict Reader');

warnState = warning('off', 'mef3io:unreadableSegment');
r = mef3io.Reader(isoPath, '', 0, false);              % lenient: contain it
warning(warnState);
probs = r.problems();
assert(numel(probs) == 1, 'expected exactly one skipped segment');
assert(strcmp(probs(1).channel, 'ch1'));
assert(probs(1).segment == 1, 'the SECOND segment (number 1) is the bad one');
assert(~isempty(probs(1).reason), 'a skipped segment must say why');
assert(strcmp(probs(1).kind, 'segment'), 'a skipped segment is kind ''segment''');
assert(numel(r.segments('ch1')) == 1, 'only the intact segment remains');
isoGot = r.read('ch1');
assert(sum(~isnan(isoGot)) == 2000, 'the intact segment must still read');
delete(r);

% ---- #83: refused before writing; sparse and string input ----------------
% A rate that is not finite and > 0, or a conversion factor that is 0, NaN or
% infinite, is a clean error and leaves nothing on disk.
badPath = fullfile(sessionDir, 'matlab_bad_args.mefd');
w = mef3io.Writer(fresh(badPath));
for badFs = [0, -256, NaN, Inf]
    gotError = false;
    try w.write('ch1', x, start, badFs); catch, gotError = true; end
    assert(gotError, 'write must refuse an invalid sampling frequency');
    gotError = false;
    try w.writeInt32('ch1', counts, 0.01, start, badFs); catch, gotError = true; end
    assert(gotError, 'writeInt32 must refuse an invalid sampling frequency');
end
for badUfact = [0, NaN, Inf, -Inf]
    gotError = false;
    try w.writeInt32('ch1', counts, badUfact, start, fs); catch, gotError = true; end
    assert(gotError, 'writeInt32 must refuse an invalid conversion factor');
end
assert(~exist(fullfile(badPath, 'ch1.timd'), 'dir'), 'a refused write must create nothing');
% Sparse data and a sparse Valid mask (a sparse array crashed the MEX, and a
% sparse mask stored the wrong validity).
z = zeros(n, 1); z(1:100:end) = 7;
w.write('sp', sparse(z), start, fs, Precision=0);
spMask = true(n, 1); spMask(101:110) = false;   % a gap inside the data
w.writeInt32('spv', counts, 0.01, start, fs, Valid=sparse(spMask));
% A negative conversion factor is a polarity inversion, not an error.
w.writeInt32('neg', counts, -0.5, start, fs);
% string-class annotation text, type and channel.
w.writeAnnotations(struct('time', {start + 1e6}, 'type', {"Note"}, ...
                          'text', {"string text"}), "sp");
delete(w);
r = mef3io.Reader(badPath);
spGot = r.read('sp');
assert(isequal(spGot(:), z), 'sparse data must round-trip like full data');
spvGot = r.read('spv');
assert(all(isnan(spvGot(101:110))) && sum(~isnan(spvGot)) == n - 10, ...
       'a sparse Valid mask must mark exactly its false entries');
negGot = r.read('neg');
assert(isequal(negGot(:), -0.5 * double(counts)), 'a negative factor must read back');
spAnns = r.records('sp');
assert(numel(spAnns) == 1 && strcmp(spAnns(1).text, 'string text'), ...
       'string annotation text must be written');
delete(r);

% A refused block length must not cost the existing session: the check runs
% BEFORE writer_open (#83 review).
gotError = false;
try
    mef3io.Writer(badPath, BlockLength=1e9);
catch
    gotError = true;
end
assert(gotError, 'a block length above the cap must be refused');
assert(exist(fullfile(badPath, 'sp.timd'), 'dir') == 7, ...
       'the existing session must survive a refused Writer');
% Sparse SCALARS are valid arguments (S(i,j) of a sparse matrix is one).
w = mef3io.Writer(badPath);
w.write('spscalar', x(1:300), start, sparse(fs), Precision=3);
delete(w);

% ---- per-segment conversion factors (#150) ---------------------------------
% Each segment keeps its own factor; read scales each sample by its own,
% info says NaN, readRaw lists where each factor applies (1-based, inclusive).
mixPath = fullfile(sessionDir, 'matlab_factors.mefd');
c = int32(mod(0:511, 50))' - 25;
w = mef3io.Writer(fresh(mixPath));
w.writeInt32('m', c, 0.01, start, fs);
w.writeInt32('m', c, -0.5, start + int64(4e6), fs, NewSegment=true);
delete(w);
r = mef3io.Reader(mixPath);
assert(isnan(r.info('m').units_conversion_factor));
segs = r.segments('m');
assert(segs(1).units_conversion_factor == 0.01 && segs(2).units_conversion_factor == -0.5);
y = r.read('m');
assert(isequal(y(1:512), double(c) * 0.01) && isequal(y(1025:1536), double(c) * -0.5));
raw = r.readRaw('m');
assert(isnan(raw.units_conversion_factor) && numel(raw.units_conversion_factors) == 2);
assert(raw.units_conversion_factors(2).start_index == 1025 && ...
       raw.units_conversion_factors(2).end_index == 1536 && ...
       raw.units_conversion_factors(2).units_conversion_factor == -0.5);
delete(r);

fprintf('test_mef3io: all assertions passed (%s)\n', sessionDir);
end

function p = fresh(p)
% The test's own scratch folder: removed here so each Writer starts empty (the
% library itself never deletes a session).
if isfolder(p), rmdir(p, 's'); end
end
