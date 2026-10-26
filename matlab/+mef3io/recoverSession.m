function [summary, nSegments] = recoverSession(path, opts)
%RECOVERSESSION Make a session's block index and data agree after a crash.
%
%   summary = mef3io.recoverSession(sessionPath)               % DRY RUN
%   summary = mef3io.recoverSession(sessionPath, Apply=true)
%   [summary, n] = mef3io.recoverSession(sessionPath, Apply=true, Backup=false)
%
%   This is the counterpart to mef3io.Writer's write path, which flushes
%   only each segment's .tmet: a power cut during an append can leave the
%   block index and the data file disagreeing. Two shapes, treated
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
%   With Apply=true it also re-derives the declarations (.tmet section 2,
%   universal headers) of every segment it changed, and of a channel's last
%   segment whose .tmet is newer or older than its index, through the same
%   repairs as repair_session - so recovery alone leaves a valid session. An
%   index that fails its own CRC is rebuilt from the CRC-checked blocks.
%
%   Backup=false is accepted but IGNORED since 2.0.1: the backup is always kept
%   (warning mef3io:backupAlwaysKept).
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
    if ~opts.Backup
        warning('mef3io:backupAlwaysKept', ...
            'Backup=false is ignored: a backup is always kept in <session>.recover-backup.');
    end
    [summary, nSegments] = mef3io_mex('recover_session', path, ...
        double(opts.Apply), 1, opts.Password);
end
