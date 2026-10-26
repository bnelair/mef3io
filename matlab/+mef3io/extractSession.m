function destDir = extractSession(tarPath, destDir, opts)
%EXTRACTSESSION Unpack a session archive back into a .mefd directory.
%
%   destDir = mef3io.extractSession(tarPath)
%   destDir = mef3io.extractSession(tarPath, destDir)
%
% The inverse of mef3io.archiveSession — after extraction the directory is a
% normal writable session again. Omitting destDir strips the ".tar" suffix
% (name.mefd.tar becomes name.mefd next to the archive). The session root
% inside the archive is stripped, so destDir becomes the session directory
% itself; archives from foreign tar tools work too. An existing target is
% refused (mef3io never deletes or replaces data; the Overwrite option was
% removed - remove or rename it yourself); a failed extraction is cleaned up. Naming is
% enforced: tarPath must end .mefd.tar, an explicit destDir must end .mefd.
arguments
    tarPath (1, :) char
    destDir (1, :) char = ''
    opts.Overwrite = []   % removed: refused below with a message
end
if ~isempty(opts.Overwrite)
    error('mef3io:overwriteRemoved', 'mef3io never deletes a session; remove or rename the folder yourself, or write to a new path');
end
destDir = mef3io_mex('extract_session', tarPath, destDir);
end
