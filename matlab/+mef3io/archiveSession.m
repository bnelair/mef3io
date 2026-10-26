function tarPath = archiveSession(sessionDir, tarPath, opts)
%ARCHIVESESSION Pack a session directory into one uncompressed tar archive.
%
%   tarPath = mef3io.archiveSession(sessionDir)
%   tarPath = mef3io.archiveSession(sessionDir, tarPath)
%
% The archive (conventionally name.mefd.tar) is a plain ustar file:
% mef3io.Reader opens it directly — no extraction — and any tar tool
% (tar -xf) reproduces the original directory. The source directory is left
% untouched; output is deterministic. Tar sessions are read-only:
% mef3io.Writer refuses .tar paths. Omitting tarPath derives
% "<sessionDir>.tar" (name.mefd becomes name.mefd.tar). Naming is enforced:
% sessionDir must end .mefd, an explicit tarPath must end .mefd.tar. An
% existing target is refused: mef3io never deletes or replaces data (the
% Overwrite option was removed; remove or rename it yourself).
arguments
    sessionDir (1, :) char
    tarPath (1, :) char = ''
    opts.Overwrite = []   % removed: refused below with a message
end
if ~isempty(opts.Overwrite)
    error('mef3io:overwriteRemoved', 'mef3io never deletes a session; remove or rename the folder yourself, or write to a new path');
end
tarPath = mef3io_mex('archive_session', sessionDir, tarPath);
end
