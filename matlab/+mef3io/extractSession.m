function destDir = extractSession(tarPath, destDir, opts)
%EXTRACTSESSION Unpack a session archive back into a .mefd directory.
%
%   destDir = mef3io.extractSession(tarPath)
%   destDir = mef3io.extractSession(tarPath, destDir)
%   destDir = mef3io.extractSession(tarPath, destDir, Overwrite=true)
%
% The inverse of mef3io.archiveSession — after extraction the directory is a
% normal writable session again. Omitting destDir strips the ".tar" suffix
% (name.mefd.tar becomes name.mefd next to the archive). The session root
% inside the archive is stripped, so destDir becomes the session directory
% itself; archives from foreign tar tools work too. An existing target is
% refused unless Overwrite=true; a failed extraction is cleaned up. Naming is
% enforced: tarPath must end .mefd.tar, an explicit destDir must end .mefd.
arguments
    tarPath (1, :) char
    destDir (1, :) char = ''
    opts.Overwrite (1, 1) logical = false
end
destDir = mef3io_mex('extract_session', tarPath, destDir, double(opts.Overwrite));
end
