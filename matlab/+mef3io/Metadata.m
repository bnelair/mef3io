classdef Metadata
    %METADATA Session-wide subject/acquisition metadata. Mirror of the Python
    %   mef3io.Metadata: a Subject (section 3) and an Acquisition (section 2),
    %   every field defaulted so you set only what you have.
    %
    %   md = mef3io.Metadata(subject=mef3io.Subject(id="MRN-123"));
    %   md.acquisition.line_frequency = 60;
    %   w = mef3io.Writer("s.mefd", Overwrite=true, Metadata=md);

    properties
        subject mef3io.Subject = mef3io.Subject
        acquisition mef3io.Acquisition = mef3io.Acquisition
    end

    methods
        function obj = Metadata(opts)
            arguments
                opts.subject (1, 1) mef3io.Subject = mef3io.Subject
                opts.acquisition (1, 1) mef3io.Acquisition = mef3io.Acquisition
            end
            obj.subject = opts.subject;
            obj.acquisition = opts.acquisition;
        end

        function s = toStruct(obj)
            %TOSTRUCT Flat struct for the MEX writer_set_metadata command.
            s = struct( ...
                'subject_name_1', obj.subject.name_1, ...
                'subject_name_2', obj.subject.name_2, ...
                'subject_id', obj.subject.id, ...
                'recording_location', obj.subject.recording_location, ...
                'gmt_offset', obj.subject.gmt_offset, ...
                'session_description', obj.acquisition.session_description, ...
                'channel_description', obj.acquisition.channel_description, ...
                'reference_description', obj.acquisition.reference_description, ...
                'acquisition_channel_number', obj.acquisition.acquisition_channel_number, ...
                'low_frequency_filter', obj.acquisition.low_frequency_filter, ...
                'high_frequency_filter', obj.acquisition.high_frequency_filter, ...
                'notch_filter', obj.acquisition.notch_filter, ...
                'line_frequency', obj.acquisition.line_frequency);
        end
    end

    methods (Static)
        function obj = fromInfo(i)
            %FROMINFO Build a Metadata from a Reader.info struct. Subject fields
            %   stay empty unless the reader had level-2 access.
            obj = mef3io.Metadata;
            if isfield(i, 'section3_available') && i.section3_available
                obj.subject.name_1 = i.subject_name_1;
                obj.subject.name_2 = i.subject_name_2;
                obj.subject.id = i.subject_id;
                obj.subject.recording_location = i.recording_location;
                obj.subject.gmt_offset = double(i.gmt_offset);
            end
            obj.acquisition.session_description = i.session_description;
            obj.acquisition.channel_description = i.channel_description;
            obj.acquisition.reference_description = i.reference_description;
            obj.acquisition.acquisition_channel_number = double(i.acquisition_channel_number);
            obj.acquisition.low_frequency_filter = i.low_frequency_filter;
            obj.acquisition.high_frequency_filter = i.high_frequency_filter;
            obj.acquisition.notch_filter = i.notch_filter;
            obj.acquisition.line_frequency = i.line_frequency;
        end
    end
end
