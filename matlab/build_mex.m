function build_mex()
%BUILD_MEX Compile the mef3io MEX extension in place.
%
%   Run from anywhere:  >> run('<repo>/matlab/build_mex.m')
%   Requires a C++20 compiler configured for MEX (`mex -setup C++`):
%   Xcode/clang on macOS, GCC >= 11 on Linux, Visual Studio 2022 on Windows.
%
%   Produces matlab/mef3io_mex.<mexext>; add the matlab/ folder to your path
%   to use the mef3io.Reader / mef3io.Writer classes.

here = fileparts(mfilename('fullpath'));
root = fileparts(here);
core = fullfile(root, 'core');

sources = [dir(fullfile(core, 'src', '*.cpp')); ...
           dir(fullfile(here, 'mef3io_mex.cpp'))];
files = arrayfun(@(s) fullfile(s.folder, s.name), sources, 'UniformOutput', false);

version = strtrim(fileread(fullfile(root, 'VERSION')));
def = ['-DMEF3IO_VERSION_STRING=\"' version '\"'];

if ispc
    flags = {'COMPFLAGS=$COMPFLAGS /std:c++20 /EHsc'};
elseif ismac
    flags = {'CXXFLAGS=$CXXFLAGS -std=c++20'};
else
    % Linux: MATLAB ships its own (older) libstdc++, which the loader prefers
    % over the system one, so the MEX must not require newer GLIBCXX symbols.
    % -static-libstdc++ does NOT work here: MATLAB's link line passes
    % -lstdc++ explicitly, which overrides the driver flag. Instead, feed the
    % static archive as a regular link input — every C++ runtime symbol then
    % resolves at link time and the MEX carries no GLIBCXX version needs.
    flags = {'CXXFLAGS=$CXXFLAGS -std=c++20', ...
             'LDFLAGS=$LDFLAGS -static-libgcc'};
    [rc, a] = system('g++ -print-file-name=libstdc++.a');
    assert(rc == 0 && ~isempty(strtrim(a)), 'could not locate libstdc++.a');
    files{end+1} = strtrim(a);
end

fprintf('Building mef3io_mex (%s) from %d sources...\n', version, numel(files));
mex('-R2018a', flags{:}, def, ['-I' fullfile(core, 'include')], ...
    '-output', fullfile(here, 'mef3io_mex'), files{:});
if isunix && ~ismac  % show C++ runtime deps (should list no versioned needs)
    system(['ldd ' fullfile(here, ['mef3io_mex.' mexext]) ' | grep -i "stdc\|gcc" || true']);
end
fprintf('Done: %s\n', fullfile(here, ['mef3io_mex.' mexext]));
end
