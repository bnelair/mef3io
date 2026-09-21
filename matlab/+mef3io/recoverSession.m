function [summary, nSegments] = recoverSession(path, opts)
%RECOVERSESSION Make a session's block index and data agree after a crash.
%
%   summary = mef3io.recoverSession(sessionPath)               % DRY RUN
%   summary = mef3io.recoverSession(sessionPath, Apply=true)
%   [summary, n] = mef3io.recoverSession(sessionPath, Apply=true, Backup=false)
%
%   This is the counterpart to Durability='fast' (the default for
%   mef3io.Writer): without the flush barriers, an unclean shutdown can leave
%   the block index and the data file disagreeing. Two shapes, treated
%   differently because one has lost data and the other has not:
%
%     index ahead of data - entries reference bytes that never landed. Those
%                           samples do not exist, so the entries are dropped.
%     data ahead of index - blocks reached .tdat but the index was not
%                           extended. Those samples DO exist and are
%                           RECOVERED: a RED block header carries the sample
%                           count, byte count, start time and discontinuity
%                           flag, which is everything an index entry needs.
%                           Only blocks whose CRC verifies are indexed.
%
%   It is a DRY RUN unless Apply=true, and backs up what it changes to
%   <session>.recover-backup first - the .tidx, the .tdat's 1024-byte header
%   and any dropped fragment, never the whole data file, which may be tens of
%   gigabytes per channel.
%
%   A session whose block index fails its own CRC is reported and left alone:
%   recovery decides what to keep from those bytes, so it must not act on an
%   index it cannot trust.
%
%   Tar archives are refused - extract one first with mef3io.extractSession.
%
%   Returns the human-readable summary and the number of segments that needed
%   (or would need) work.
%
%   See also mef3io.Writer, mef3io.archiveSession, mef3io.extractSession.
    arguments
        path (1, :) char
        opts.Apply (1, 1) logical = false
        opts.Backup (1, 1) logical = true
        opts.Password (1, :) char = ''
    end
    [summary, nSegments] = mef3io_mex('recover_session', path, ...
        double(opts.Apply), double(opts.Backup), opts.Password);
end
