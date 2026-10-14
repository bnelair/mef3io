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
    % Durability: these files are built by appending for days to months, so
    % the append is the hot path. Durability='fast' (the default) performs no
    % flushes, matching every MEF writer before this one — the reference C
    % library flushes nothing at all. An unclean shutdown can then leave the
    % block index and the data disagreeing; mef3io.recoverSession detects and
    % repairs exactly that, rebuilding missing index entries from the RED
    % block headers. Durability='full' adds the barriers, making each append
    % all-or-nothing across the three files with no recovery step, at roughly
    % 2.5x the cost per append.
    %
    % Encrypted sessions: pass Password1 and Password2 (both required).
    % Times are uUTC: microseconds since the Unix epoch (int64 or double).
    % `path` must end .mefd (enforced); tar archives are read-only and
    % refused — pack a directory with mef3io.archiveSession instead.

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
                opts.Metadata = []   % mef3io.Metadata or a struct of fields
                opts.Durability (1, :) char {mustBeMember(opts.Durability, ...
                    {'fast', 'full'})} = 'fast'
            end
            obj.h = mef3io_mex('writer_open', path, double(opts.Overwrite), ...
                               opts.Password1, opts.Password2);
            mef3io_mex('writer_set_durable', obj.h, double(strcmp(opts.Durability, 'full')));
            if ~isempty(opts.Units)
                mef3io_mex('writer_set_units', obj.h, opts.Units);
            end
            if opts.BlockLength > 0
                mef3io_mex('writer_set_block_length', obj.h, opts.BlockLength);
            end
            mef3io_mex('writer_set_threads', obj.h, opts.Threads);
            if ~isempty(opts.Metadata)
                obj.setMetadata(opts.Metadata);
            end
        end

        function setMetadata(obj, md)
            %SETMETADATA Session-wide subject/acquisition metadata. Accepts a
            %   mef3io.Metadata object (recommended) or a struct with any of:
            %   subject_name_1/2, subject_id, recording_location, gmt_offset,
            %   session_description, channel_description, reference_description,
            %   acquisition_channel_number, low_frequency_filter,
            %   high_frequency_filter, notch_filter, line_frequency. Set before
            %   writing; applies to every channel.
            if isa(md, 'mef3io.Metadata')
                md = md.toStruct();
            end
            mef3io_mex('writer_set_metadata', obj.h, md);
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
            nBefore = numel(obj.unreadableMetadata());
            s = mef3io_mex('writer_write', obj.h, channel, double(data(:)), ...
                           startUutc, fs, opts.Precision, double(opts.NewSegment));
            obj.warnUnreadable(nBefore);
        end

        function s = writeInt32(obj, channel, data, ufact, startUutc, fs, opts)
            %WRITEINT32 Verbatim int32 counts + conversion factor (bit-exact).
            %   Data must be integer-typed: counts are stored verbatim, so
            %   floating-point input errors (use write() for float data) and
            %   values outside the int32 range error instead of saturating.
            %   Valid: any numeric/logical mask, nonzero = valid sample.
            arguments
                obj
                channel (1, :) char
                data
                ufact (1, 1) double
                startUutc (1, 1) {mustBeNumeric}
                fs (1, 1) double
                opts.Valid = []
                opts.NewSegment (1, 1) logical = false
            end
            if ~isinteger(data)
                error('mef3io:type', ['writeInt32 stores counts verbatim and ' ...
                      'requires integer data (e.g. int32); got %s. Convert ' ...
                      'explicitly, or use write() for floating-point data.'], class(data));
            end
            if any(data(:) > intmax('int32')) || any(data(:) < intmin('int32'))
                error('mef3io:range', ['writeInt32: values exceed the int32 ' ...
                      'range and would be altered; rescale them or use write().']);
            end
            if ~isempty(opts.Valid), opts.Valid = logical(opts.Valid(:)); end
            nBefore = numel(obj.unreadableMetadata());
            s = mef3io_mex('writer_write_int32', obj.h, channel, int32(data(:)), ...
                           opts.Valid, ufact, startUutc, fs, double(opts.NewSegment));
            obj.warnUnreadable(nBefore);
        end

        function writeAnnotations(obj, records, channel)
            %WRITEANNOTATIONS Struct array with fields: time (required),
            %   type (default 'Note'), text, duration. Omit `channel` for
            %   session-level records. Replaces records at that level.
            if nargin < 3, channel = []; end
            if istable(records), records = table2struct(records); end
            nBefore = numel(obj.unreadableMetadata());
            mef3io_mex('writer_write_records', obj.h, channel, records);
            obj.warnUnreadable(nBefore);
        end

        function u = unreadableMetadata(obj)
            %UNREADABLEMETADATA Segment metadata this writer could not use.
            %   u = w.unreadableMetadata()   % cellstr of 'path: reason'
            %
            % .tmet files that could not be read while choosing the
            % recording-time offset, and empty segment directories ignored
            % on open. In a session at offset 0 such a file is skipped so it
            % does not block unrelated writes (with a mef3io:unreadableMetadata
            % warning); with a non-zero offset, or nothing else readable, the
            % write is refused instead. Mirrors Python's
            % Writer.unreadable_metadata.
            u = mef3io_mex('writer_unreadable_metadata', obj.h);
        end
    end

    methods (Access = private)
        function warnUnreadable(obj, nBefore)
            u = obj.unreadableMetadata();
            if numel(u) > nBefore
                warning('mef3io:unreadableMetadata', ...
                    ['%d segment metadata file(s) could not be read and were skipped ' ...
                     'when choosing the recording-time offset: %s'], ...
                    numel(u) - nBefore, u{nBefore + 1});
            end
        end
    end
end
