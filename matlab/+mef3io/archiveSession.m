function tarPath = archiveSession(sessionDir, tarPath, opts)
%ARCHIVESESSION Pack a session directory into one uncompressed tar archive.
%
%   tarPath = mef3io.archiveSession(sessionDir)
%   tarPath = mef3io.archiveSession(sessionDir, tarPath)
%   tarPath = mef3io.archiveSession(sessionDir, tarPath, Overwrite=true)
%
% The archive (conventionally name.mefd.tar) is a plain ustar file:
% mef3io.Reader opens it directly — no extraction — and any tar tool
% (tar -xf) reproduces the original directory. The source directory is left
% untouched; output is deterministic. Tar sessions are read-only:
% mef3io.Writer refuses .tar paths. Omitting tarPath derives
% "<sessionDir>.tar" (name.mefd becomes name.mefd.tar). Naming is enforced:
% sessionDir must end .mefd, an explicit tarPath must end .mefd.tar.
arguments
    sessionDir (1, :) char
    tarPath (1, :) char = ''
    opts.Overwrite (1, 1) logical = false
end
tarPath = mef3io_mex('archive_session', sessionDir, tarPath, double(opts.Overwrite));
end
