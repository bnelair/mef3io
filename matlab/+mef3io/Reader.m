classdef Reader < handle
    %READER Read a MEF 3.0 session (mef3io MEX backend).
    %
    %   r = mef3io.Reader(path)                 % unencrypted
    %   r = mef3io.Reader(path, password)       % level-1 or level-2 password
    %   r = mef3io.Reader(path, '', 0, false)   % lenient: skip unreadable segments
    %
    % `path` is a .mefd session directory or an uncompressed tar archive of
    % one (name.mefd.tar, from mef3io.archiveSession) — read in place,
    % without extraction. The suffix is enforced: anything else is refused,
    % so a stray directory or file can never be misread as a session.
    %   r.channels                              % cellstr
    %   info = r.info('ch1')                    % fs, ufact, times, subject metadata
    %   x = r.read('ch1')                       % whole channel, double, NaN gaps
    %   x = r.read('ch1', t0, t1)               % [t0, t1) window in uUTC
    %   raw = r.readRaw('ch1')                  % int32 counts + validity mask
    %   segs = r.segments('ch1')                % what data is where, per segment
    %   blocks = r.toc('ch1')                   % block-level table of contents
    %   anns = r.records('ch1')                 % annotations ([] -> session level)
    %   p = r.problems()                        % segments skipped (lenient only)
    %   r.setThreads(4); n = r.threads()        % thread count (0 = automatic)
    %
    % nThreads is how many threads the Reader may use for everything it does
    % (opening, file access, decoding, decrypting): 0 = automatic (default),
    % 1 = no extra threads. Results never depend on it. See the threads guide.
    %
    % STRICT is the default: one unreadable segment fails the whole session,
    % because a skipped segment's samples come back as NaN and nothing in the
    % returned array distinguishes that from a real recording gap. Pass
    % strict=false to salvage the intact remainder of a damaged archive, and
    % check problems() before trusting the data.
    %
    % Times are uUTC: microseconds since the Unix epoch (int64 or double).

    properties (Access = private)
        h uint64 = 0
    end

    methods
        function obj = Reader(path, password, nThreads, strict)
            arguments
                path (1, :) char
                password (1, :) char = ''
                nThreads (1, 1) double = 0
                strict (1, 1) logical = true
            end
            obj.h = mef3io_mex('reader_open', path, password, nThreads, double(strict));
            nLegacy = mef3io_mex('reader_legacy_password_channels', obj.h);
            if nLegacy > 0
                % Mirrors Python's LegacyPasswordWarning (issue #84).
                warning('mef3io:legacyPassword', ...
                    ['%s: the password opened %d channel(s) only through the ' ...
                     'password-to-key mapping that mef3io used up to 1.2.0, which ' ...
                     'differs from meflib''s for non-ASCII characters (issue #84); ' ...
                     'they were most likely encrypted by one of those versions. ' ...
                     'The data is read correctly, but meflib and pymef cannot open ' ...
                     'these files with this password. Re-encrypt the session to make ' ...
                     'it readable everywhere.'], path, nLegacy);
            end
            if ~strict
                p = mef3io_mex('reader_problems', obj.h);
                if ~isempty(p)
                    warning('mef3io:unreadableSegment', ...
                        ['%s: %d segment(s) could not be read and were SKIPPED; their ' ...
                         'time spans read back as NaN, which is indistinguishable from ' ...
                         'a real recording gap. First: %s - %s. Full list in problems().'], ...
                        path, numel(p), p(1).path, p(1).reason);
                end
            end
        end

        function p = problems(obj)
            %PROBLEMS Segments that could not be read, when opened lenient.
            %   p = r.problems()
            %
            % Struct array with fields channel, segment, path, reason, kind.
            % Always empty for a Reader opened strict (the default), where the
            % same failure raises instead of being skipped. kind is:
            %   'segment' - the segment could not be read; its span reads
            %               back as NaN (found when the session is opened).
            %   'records' - one record (.rdat) file was skipped by records();
            %               only its annotations are missing. path is the
            %               file, and segment is -1 for a session- or
            %               channel-level file.
            % Mirrors Python's Reader.problems.
            p = mef3io_mex('reader_problems', obj.h);
        end

        function delete(obj)
            if obj.h ~= 0
                mef3io_mex('reader_close', obj.h);
                obj.h = 0;
            end
        end

        function setThreads(obj, n)
            %SETTHREADS Change how many threads this Reader may use.
            %   r.setThreads(n)   % 0 = automatic, 1 = no extra threads
            %
            % Takes effect on the next call; calls already running keep
            % theirs. Mirrors setting Python's Reader.n_threads.
            arguments
                obj
                n (1, 1) double {mustBeInteger, mustBeNonnegative}
            end
            mef3io_mex('reader_set_threads', obj.h, n);
        end

        function n = threads(obj)
            %THREADS The Reader's thread count as set (0 = automatic).
            %   n = r.threads()   % mirrors Python's Reader.n_threads
            n = mef3io_mex('reader_threads', obj.h);
        end

        function c = channels(obj)
            c = mef3io_mex('reader_channels', obj.h);
        end

        function s = info(obj, channel)
            s = mef3io_mex('reader_info', obj.h, channel);
        end

        function m = metadata(obj)
            %METADATA Session subject/acquisition metadata as a
            %   mef3io.Metadata object (.subject / .acquisition), from the
            %   first channel (metadata is session-wide). Subject fields are
            %   empty unless the reader was opened with a level-2 password.
            chans = obj.channels();
            if isempty(chans)
                m = mef3io.Metadata;
                return;
            end
            m = mef3io.Metadata.fromInfo(obj.info(chans{1}));
        end

        function x = read(obj, channel, t0, t1)
            %READ Float64 samples over [t0, t1) uUTC; gaps are NaN.
            %   Each sample is scaled by its own segment's conversion factor.
            if nargin < 3, t0 = []; end
            if nargin < 4, t1 = []; end
            x = mef3io_mex('reader_read', obj.h, channel, t0, t1);
        end

        function s = readRaw(obj, channel, t0, t1)
            %READRAW Stored int32 counts plus a validity mask (false in gaps).
            %   Segments of one channel may store different conversion factors:
            %   units_conversion_factors lists where each applies (start_index
            %   and end_index, 1-based, inclusive), and units_conversion_factor
            %   is NaN when the window holds more than one.
            if nargin < 3, t0 = []; end
            if nargin < 4, t1 = []; end
            s = mef3io_mex('reader_read_raw', obj.h, channel, t0, t1);
        end

        function s = segments(obj, channel)
            %SEGMENTS Per-segment map: time range, sample range, block count,
            %   the segment's own units_conversion_factor,
            %   and provenance (created_by / last_modified_by versions, last_operation,
            %   operations, modification_count; [] for a file mef3io did not stamp).
            s = mef3io_mex('reader_segments', obj.h, channel);
        end

        function s = toc(obj, channel)
            %TOC Block-level table of contents (struct of column vectors).
            s = mef3io_mex('reader_toc', obj.h, channel);
        end

        function s = records(obj, channel, segment, level)
            %RECORDS Records (annotations) at the session, channel or segment level.
            %   s = r.records()                     % session-level records
            %   s = r.records('ch1')                % ch1's channel- AND segment-level
            %   s = r.records('ch1', 0)             % segment 0 of ch1 only
            %   s = r.records('ch1', [], 'channel') % level: 'session', 'channel',
            %   s = r.records([], [], 'all')        %   'segment' or 'all'
            %
            % Struct array with type, time (int64 uUTC), level, channel ('' at
            % session level), segment ([] unless segment level),
            % version_major, version_minor, text, duration, then every body
            % field any returned record carries ([] where a record has none),
            % e.g. a Seiz record's channels struct array (name/onset/offset).
            % Body times are absolute uUTC like time. Mirrors Python's
            % Reader.records(channel, segment, level).
            %
            % A lenient Reader skips a record file it cannot read or place,
            % lists it in problems() (kind 'records') and warns with
            % mef3io:unreadableRecords, as Python does.
            if nargin < 2, channel = []; end
            if nargin < 3, segment = []; end
            if nargin < 4, level = []; end
            nBefore = numel(mef3io_mex('reader_problems', obj.h));
            nLegacy = mef3io_mex('reader_legacy_password_record_files', obj.h);
            s = mef3io_mex('reader_records', obj.h, channel, segment, level);
            nNew = mef3io_mex('reader_legacy_password_record_files', obj.h) - nLegacy;
            if nNew > 0
                % Mirrors Python's LegacyPasswordWarning (issue #84).
                warning('mef3io:legacyPassword', ...
                    ['the password opened %d record file(s) only through the ' ...
                     'password-to-key mapping that mef3io used up to 1.2.0 (issue ' ...
                     '#84); meflib and pymef cannot open them with this password. ' ...
                     'Re-encrypt the session to make it readable everywhere.'], nNew);
            end
            p = mef3io_mex('reader_problems', obj.h);
            if numel(p) > nBefore
                added = p(nBefore + 1:end);
                warning('mef3io:unreadableRecords', ...
                    ['%d record file(s) could not be read and were SKIPPED. ' ...
                     'First: %s - %s. Full list in problems().'], ...
                    numel(added), added(1).path, added(1).reason);
            end
        end
    end
end
