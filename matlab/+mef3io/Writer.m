classdef Writer < handle
    %WRITER Write a MEF 3.0 session (mef3io MEX backend).
    %
    %   w = mef3io.Writer(path, Overwrite=true);
    %   w.write('ch1', data, startUutc, fs);            % NaN = gap, precision inferred
    %   w.write('ch1', more, t2, fs);                   % appends in-segment
    %   w.write('ch1', x, t3, fs, NewSegment=true);     % force a new segment
    %   w.writeInt32('ch1', counts, ufact, startUutc, fs);
    %   w.writeAnnotations(struct('time', {t}, 'text', {'note'}), 'ch1');
    %   delete(w)                                       % or let it go out of scope
    %
    % Encrypted sessions: pass Password1 and Password2 (both required).
    % Times are uUTC: microseconds since the Unix epoch (int64 or double).

    properties (Access = private)
        h uint64 = 0
    end

    methods
        function obj = Writer(path, opts)
            arguments
                path (1, :) char
                opts.Overwrite (1, 1) logical = false
                opts.Password1 (1, :) char = ''
                opts.Password2 (1, :) char = ''
                opts.Units (1, :) char = ''
                opts.BlockLength (1, 1) double = 0
                opts.Threads (1, 1) double = 0
            end
            obj.h = mef3io_mex('writer_open', path, double(opts.Overwrite), ...
                               opts.Password1, opts.Password2);
            if ~isempty(opts.Units)
                mef3io_mex('writer_set_units', obj.h, opts.Units);
            end
            if opts.BlockLength > 0
                mef3io_mex('writer_set_block_length', obj.h, opts.BlockLength);
            end
            mef3io_mex('writer_set_threads', obj.h, opts.Threads);
        end

        function delete(obj)
            if obj.h ~= 0
                mef3io_mex('writer_close', obj.h);
                obj.h = 0;
            end
        end

        function s = write(obj, channel, data, startUutc, fs, opts)
            %WRITE Float data; NaN runs become discontinuity gaps.
            %   Precision sets the conversion factor to 10^-precision; -1
            %   (default) infers it, or reuses the segment's on append.
            arguments
                obj
                channel (1, :) char
                data double
                startUutc (1, 1) {mustBeNumeric}
                fs (1, 1) double
                opts.Precision (1, 1) double = -1
                opts.NewSegment (1, 1) logical = false
            end
            s = mef3io_mex('writer_write', obj.h, channel, double(data(:)), ...
                           startUutc, fs, opts.Precision, double(opts.NewSegment));
        end

        function s = writeInt32(obj, channel, data, ufact, startUutc, fs, opts)
            %WRITEINT32 Verbatim int32 counts + conversion factor (bit-exact).
            %   Valid: logical/uint8 mask, false marks gap samples.
            arguments
                obj
                channel (1, :) char
                data int32
                ufact (1, 1) double
                startUutc (1, 1) {mustBeNumeric}
                fs (1, 1) double
                opts.Valid = []
                opts.NewSegment (1, 1) logical = false
            end
            if ~isempty(opts.Valid), opts.Valid = logical(opts.Valid(:)); end
            s = mef3io_mex('writer_write_int32', obj.h, channel, data(:), ...
                           opts.Valid, ufact, startUutc, fs, double(opts.NewSegment));
        end

        function writeAnnotations(obj, records, channel)
            %WRITEANNOTATIONS Struct array with fields: time (required),
            %   type (default 'Note'), text, duration. Omit `channel` for
            %   session-level records. Replaces records at that level.
            if nargin < 3, channel = []; end
            if istable(records), records = table2struct(records); end
            mef3io_mex('writer_write_records', obj.h, channel, records);
        end
    end
end
