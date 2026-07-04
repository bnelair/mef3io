classdef Subject
    %SUBJECT Subject / recording-context metadata (MEF section 3, level-2).
    %   All fields default to empty; set only what you have. Mirror of the
    %   Python mef3io.Subject dataclass.
    %
    %   s = mef3io.Subject(id="MRN-123", name_1="Jane");
    %   s.recording_location = "EMU-4";

    properties
        name_1 (1, :) char = ''
        name_2 (1, :) char = ''
        id (1, :) char = ''
        recording_location (1, :) char = ''
        gmt_offset (1, 1) double = 0   % seconds east of UTC
    end

    methods
        function obj = Subject(opts)
            arguments
                opts.name_1 (1, :) char = ''
                opts.name_2 (1, :) char = ''
                opts.id (1, :) char = ''
                opts.recording_location (1, :) char = ''
                opts.gmt_offset (1, 1) double = 0
            end
            obj.name_1 = opts.name_1;
            obj.name_2 = opts.name_2;
            obj.id = opts.id;
            obj.recording_location = opts.recording_location;
            obj.gmt_offset = opts.gmt_offset;
        end
    end
end
