function test_api_parity()
%TEST_API_PARITY Assert the MATLAB API mirrors the Python one and is documented.
%   Every public method of mef3io.Reader/Writer must (a) correspond to the
%   Python method of the same name (camelCase <-> snake_case) and (b) carry
%   help text (an H1 line). Errors on any mismatch. Run:
%       run('<repo>/matlab/test_api_parity.m')

    % Python method name -> MATLAB method name. Keep in sync with the classes.
    % 'channels' is a property in Python, a method in MATLAB — same API surface.
    readerMap = struct( ...
        'channels', 'channels', 'read', 'read', 'read_raw', 'readRaw', ...
        'info', 'info', 'segments', 'segments', 'toc', 'toc', ...
        'records', 'records', 'close', 'delete');
    writerMap = struct( ...
        'write', 'write', 'write_int32', 'writeInt32', ...
        'write_annotations', 'writeAnnotations', 'close', 'delete');

    checkClass('mef3io.Reader', readerMap);
    checkClass('mef3io.Writer', writerMap);
    fprintf('test_api_parity: MATLAB API mirrors Python and every method is documented.\n');
end

function checkClass(className, nameMap)
    mc = meta.class.fromName(className);
    assert(~isempty(mc), 'class %s not found on the path', className);

    % public, concrete, non-hidden methods defined on this class
    defined = {};
    for m = mc.MethodList'
        if strcmp(m.DefiningClass.Name, className) && strcmp(m.Access, 'public') ...
                && ~m.Hidden && ~m.Static
            defined{end+1} = m.Name; %#ok<AGROW>
        end
    end

    pyNames = fieldnames(nameMap);
    expected = cellfun(@(n) nameMap.(n), pyNames, 'UniformOutput', false);
    % the constructor shares the class name; it is documented via the classdef
    expected{end+1} = regexprep(className, '.*\.', '');

    % (a) every mapped MATLAB method exists
    for i = 1:numel(expected)
        assert(any(strcmp(expected{i}, defined)), ...
            '%s: expected method %s (Python parity) not found', className, expected{i});
    end
    % (b) no undocumented public methods slipped in beyond the mapped set
    for i = 1:numel(defined)
        name = defined{i};
        assert(any(strcmp(name, expected)), ...
            ['%s: public method %s has no Python counterpart in the parity ' ...
             'map — add it to both bindings or hide it'], className, name);
        % (c) every method carries help text
        h = help([className '/' name]);
        assert(~isempty(strtrim(h)), '%s.%s is undocumented (no help text)', className, name);
    end

    % class itself is documented
    assert(~isempty(strtrim(help(className))), '%s has no class help text', className);
end
