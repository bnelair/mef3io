classdef Acquisition
    %ACQUISITION Descriptive / acquisition metadata (MEF section 2, level-1).
    %   Filter settings default to -1 ("not recorded"). Mirror of the Python
    %   mef3io.Acquisition dataclass.
    %
    %   a = mef3io.Acquisition(session_description="pre-surgical", line_frequency=60);

    properties
        session_description (1, :) char = ''
        channel_description (1, :) char = ''
        reference_description (1, :) char = ''
        acquisition_channel_number (1, 1) double = 1
        low_frequency_filter (1, 1) double = -1
        high_frequency_filter (1, 1) double = -1
        notch_filter (1, 1) double = -1
        line_frequency (1, 1) double = -1
    end

    methods
        function obj = Acquisition(opts)
            arguments
                opts.session_description (1, :) char = ''
                opts.channel_description (1, :) char = ''
                opts.reference_description (1, :) char = ''
                opts.acquisition_channel_number (1, 1) double = 1
                opts.low_frequency_filter (1, 1) double = -1
                opts.high_frequency_filter (1, 1) double = -1
                opts.notch_filter (1, 1) double = -1
                opts.line_frequency (1, 1) double = -1
            end
            obj.session_description = opts.session_description;
            obj.channel_description = opts.channel_description;
            obj.reference_description = opts.reference_description;
            obj.acquisition_channel_number = opts.acquisition_channel_number;
            obj.low_frequency_filter = opts.low_frequency_filter;
            obj.high_frequency_filter = opts.high_frequency_filter;
            obj.notch_filter = opts.notch_filter;
            obj.line_frequency = opts.line_frequency;
        end
    end
end
