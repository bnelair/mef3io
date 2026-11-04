// mef3io — several recordings into ONE new session (#103), in one pass.
//
// merge_sessions() is the same copy engine as transform_session and
// anonymize_session (session_copy.cpp). It can, in ONE call: leave channels
// out, trim each source to its own time range, make the result anonymous,
// leave the annotations out, and write it all as one new session. Each
// source byte is read once and written once by the copy pass; the
// verification then reads the sources and the result again.
//
// The types shared by every copy operation are in transform.hpp; the
// anonymization options are anonymize.hpp's.
#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "mef3io/anonymize.hpp"
#include "mef3io/control.hpp"
#include "mef3io/transform.hpp"
#include "mef3io/types.hpp"

namespace mef3io {

/// Merging several recordings into one (#103). The transform's rules apply
/// to every source (times and samples verbatim; channels, passwords and
/// metadata edits as asked), `rename` is implied (the result is named after
/// `dst`).
struct MergeOptions : TransformOptions {
  /// The password that opens each source, in the order of `sources`. Empty =
  /// CopyOptions::password for every source.
  std::vector<std::string> passwords;

  /// One cut per source, in the order of `sources` ([first, second) in uUTC
  /// as that source stores it; nullopt = the whole source). Empty = no cut.
  /// Otherwise exactly one entry per source (`merge-time-ranges-count`). The
  /// cut rules of CopyOptions::time_range apply to each source (`exact` is
  /// shared); overlap between sources is judged on the TRIMMED data. A source
  /// with no sample in its range is refused (`time-range-empty`). The
  /// single CopyOptions::time_range must stay unset (`merge-time-range`).
  std::vector<std::optional<std::pair<si8, si8>>> time_ranges;

  /// Make the result anonymous in the same pass. Only the anonymization
  /// fields are read (shift_us, pseudonym, clear_*, text, text_fn); the
  /// CopyOptions members of this object are ignored (the merge's own are
  /// used), and `rename` is always true. ONE shift applies to every source
  /// (the sources must share the recording-time offset, as always), checked
  /// against the span of ALL of them. The identity of EVERY source (subject
  /// names and ID, location, session names, earlier pseudonyms) is removed
  /// and scanned for in the result; a subject field that is cleared may
  /// differ between sources. Combined with `metadata` edits it is refused
  /// (`merge-anonymize-metadata`).
  std::optional<AnonymizeOptions> anonymize;

  /// Threads for the whole operation (docs/threads.md; the sanctioned form
  /// of the thread rule for a one-call operation): 0 = automatic, 1 = no
  /// extra thread, negative = error. Covers the open of the sources, the
  /// copy of channels, and the verification. The result never depends on
  /// it. A text callback (anonymize.text == Callback) is called serially, so
  /// the copy pass then runs on one thread.
  int n_threads = 0;
};

struct MergeReport : TransformReport {
  std::vector<std::string> sources;  ///< as given
  /// Source segments split in two or more pieces because another source's
  /// data lies in one of their gaps (each piece is its own segment).
  si8 segments_split = 0;
  /// anonymize only (empty / unset otherwise): the pseudonym written, the
  /// shift applied (unset = the times were kept) and what the text policy
  /// changed.
  std::string pseudonym;
  std::optional<si8> shift_us;
  si8 source_recording_time_offset = 0;
  si8 record_text_fields_changed = 0;
  si8 record_bodies_cleared = 0;
  /// anonymize only. THE RE-IDENTIFICATION KEY over all sources (earliest
  /// start, latest end, and per kept channel): in memory only, as in
  /// AnonymizeReport.
  std::optional<si8> source_start_us, source_end_us;
  std::vector<SourceChannelTimes> source_channel_times;
};

/// Merge `sources` (`.mefd` directories or `.mefd.tar` archives, at least
/// two) into ONE new session `dst`. Refused before anything is written
/// (check_merge lists everything):
///   * `merge-overlap`: two sources hold samples of the same channel at the
///     same moment (compared on the RECORDED data, from the indexes, after
///     any per-source cut, so recordings that take turns merge; a source's
///     own overlapping blocks never count);
///   * `merge-rate-differs`: a channel has different sampling rates;
///   * `merge-rto-differs`: the sources use different recording-time
///     offsets (times are copied verbatim);
///   * `merge-identity-differs`: the sources' subject fields differ and
///     neither `metadata` sets them nor `anonymize` clears them;
/// plus every refusal a transform (or, with `anonymize`, an anonymization)
/// of each source would meet. Each source segment becomes one segment of the
/// result, split at gaps where another source's data lies so that the
/// result's segments never overlap; they are ordered by time and renumbered
/// per channel. Annotations are merged by time. The result is verified
/// against the sources before it is published.
MergeReport merge_sessions(const std::vector<std::string>& sources, const std::string& dst,
                           const MergeOptions& options, const Control& ctl = {});

/// Every check merge_sessions makes before it writes, without writing.
CopyCheck check_merge(const std::vector<std::string>& sources, const std::string& dst,
                      const MergeOptions& options, const Control& ctl = {});

/// Check a merged session against its sources. Reads all; writes nothing.
VerifyReport verify_merge(const std::vector<std::string>& sources, const std::string& dst,
                          const MergeOptions& options, const Control& ctl = {});

}  // namespace mef3io
