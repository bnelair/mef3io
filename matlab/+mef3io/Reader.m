classdef Reader < handle
    %READER Read a MEF 3.0 session (mef3io MEX backend).
    %
    %   r = mef3io.Reader(path)                 % unencrypted
    %   r = mef3io.Reader(path, password)       % level-1 or level-2 password
    %   r.channels                              % cellstr
    %   info = r.info('ch1')                    % fs, ufact, times, subject metadata
    %   x = r.read('ch1')                       % whole channel, double, NaN gaps
    %   x = r.read('ch1', t0, t1)               % [t0, t1) window in uUTC
    %   raw = r.readRaw('ch1')                  % int32 counts + validity mask
    %   segs = r.segments('ch1')                % what data is where, per segment
    %   blocks = r.toc('ch1')                   % block-level table of contents
    %   anns = r.records('ch1')                 % annotations ([] -> session level)
    %
    % Times are uUTC: microseconds since the Unix epoch (int64 or double).

    properties (Access = private)
        h uint64 = 0
    end

    methods
        function obj = Reader(path, password, nThreads)
            arguments
                path (1, :) char
                password (1, :) char = ''
                nThreads (1, 1) double = 0
            end
            obj.h = mef3io_mex('reader_open', path, password, nThreads);
        end

        function delete(obj)
            if obj.h ~= 0
                mef3io_mex('reader_close', obj.h);
                obj.h = 0;
            end
        end

        function c = channels(obj)
            c = mef3io_mex('reader_channels', obj.h);
        end

        function s = info(obj, channel)
            s = mef3io_mex('reader_info', obj.h, channel);
        end

        function x = read(obj, channel, t0, t1)
            %READ Float64 samples over [t0, t1) uUTC; gaps are NaN.
            if nargin < 3, t0 = []; end
            if nargin < 4, t1 = []; end
            x = mef3io_mex('reader_read', obj.h, channel, t0, t1);
        end

        function s = readRaw(obj, channel, t0, t1)
            %READRAW Stored int32 counts plus a validity mask (false in gaps).
            if nargin < 3, t0 = []; end
            if nargin < 4, t1 = []; end
            s = mef3io_mex('reader_read_raw', obj.h, channel, t0, t1);
        end

        function s = segments(obj, channel)
            %SEGMENTS Per-segment map: time range, sample range, block count.
            s = mef3io_mex('reader_segments', obj.h, channel);
        end

        function s = toc(obj, channel)
            %TOC Block-level table of contents (struct of column vectors).
            s = mef3io_mex('reader_toc', obj.h, channel);
        end

        function s = records(obj, channel)
            %RECORDS Annotations for a channel, or session-level when omitted.
            if nargin < 2, channel = []; end
            s = mef3io_mex('reader_records', obj.h, channel);
        end
    end
end
