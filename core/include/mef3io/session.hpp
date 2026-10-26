// mef3io — session/channel/segment tree and low-level indexed reads.
#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "mef3io/control.hpp"
#include "mef3io/headers.hpp"
#include "mef3io/metadata.hpp"
#include "mef3io/parallel.hpp"
#include "mef3io/records.hpp"
#include "mef3io/source.hpp"
#include "mef3io/types.hpp"

namespace mef3io {

// A contiguous run of decoded samples starting at an absolute uUTC time.
// Discontinuities produce separate runs, and so does a change of conversion
// factor between segments (#150): every sample of a run has the same one.
struct DataRun {
  si8 start_uutc = 0;      // absolute (recording_time_offset applied)
  si8 start_sample = 0;    // channel-wide sample index of the first sample
  /// The section-2 units_conversion_factor of the segment(s) these samples
  /// come from, as stored (0 = MEF's "no entry"): units = samples * factor.
  sf8 units_conversion_factor = 1.0;
  std::vector<si4> samples;
};

/// True when two stored conversion factors are the same value (NaN equals
/// NaN here: a factor is a stored value, not a measurement).
inline bool same_units_conversion_factor(sf8 a, sf8 b) {
  return a == b || (std::isnan(a) && std::isnan(b));
}

/// A conversion factor that turns counts into values: finite and non-zero
/// (negative = a polarity inversion, allowed). 0 is meflib's NO_ENTRY
/// (meflib.h:439, TIME_SERIES_METADATA_UNITS_CONVERSION_FACTOR_NO_ENTRY).
inline bool usable_units_conversion_factor(sf8 f) {
  return f != 0.0 && std::isfinite(f);
}

namespace detail {
// read()'s grid (reader.cpp), here so block_envelope can apply the SAME
// rule when it decides what a window touches.
// Number of output samples of a window [t0, t1) (the C ABI's
// mef3io_reader_n_samples mirrors it). Its argument is never negative.
inline si8 grid_count(si8 t1, si8 t0, sf8 fs) {
  return static_cast<si8>(std::llround(static_cast<sf8>(t1 - t0) * fs / 1e6));
}

// Output slot of a sample (or block start) at absolute uUTC `t`: the nearest
// slot of the [t0, fs) grid, a TIE going to the EARLIER slot whatever the
// sign of t - t0 (#129). So slot 0 holds the first sample at or after t0.
// std::llround rounds halves away from zero: the block holding t0 (negative
// offset) went one way and every later block (positive offset) the other,
// leaving one slot no block claimed -- a NaN at the first block boundary of
// any window starting exactly half a sample off the grid (at 500 Hz, every
// millisecond time ending in 1 ms). One rule for every block keeps
// consecutive blocks exactly their length apart.
inline si8 grid_slot(si8 t, si8 t0, sf8 fs) {
  return static_cast<si8>(std::ceil(static_cast<sf8>(t - t0) * fs / 1e6 - 0.5));
}

}  // namespace detail

/// Why a read in units refuses samples whose segment stores an unusable
/// factor (see usable_units_conversion_factor). Never names a segment, path
/// or time: the same text is raised by Python's MultiReader, whose segment
/// numbering differs. `alternative` names the call that returns the counts.
std::string unusable_factor_message(const std::string& channel, sf8 factor,
                                    const std::string& alternative);

struct SegmentReader {
  // Source-relative '/'-separated paths (resolved through the SessionSource,
  // which may be a directory tree or a tar archive).
  std::string tmet_path;
  std::string tidx_path;
  std::string tdat_path;
  int segment_number = 0;
  /// Loaded by the open and IMMUTABLE afterwards, so later calls read it
  /// without a lock (#110). Empty only for a segment that failed to load, and
  /// such a segment is marked failed (load_error) before the open returns.
  std::optional<TimeSeriesMetadata> metadata;
  /// The block index, loaded lazily (whole file) and shared: a call holds its
  /// own reference (a PIN) for as long as it reads it, so evicting it from the
  /// cache (another thread's trim) never frees bytes in use. Guarded by
  /// Session::index_mu_.
  std::shared_ptr<const std::vector<ui1>> tidx;
  std::uint64_t index_last_used = 0;                // for cache eviction (index_mu_)
  /// The time span the CACHED block index (`tidx`) covers, learnt when a
  /// read walks it and DROPPED WITH IT on eviction, so it is never older than
  /// what a walk would read: a windowed read skips walking a cached index
  /// that holds nothing in the window, and nothing else. No .tmet time is ever
  /// used to skip (#146: a stale or late start/end silently lost samples).
  /// `span_open` = an entry has no usable count, so the span is unbounded.
  /// Guarded by Session::index_mu_.
  bool span_known = false;
  bool span_open = false;
  si8 span_lo = 0, span_hi = 0;
  /// Why this segment could not be read, in lenient mode. Empty means fine.
  /// Set once: a segment that failed is not retried on every later call.
  /// Guarded by Session::state_mu_.
  std::string load_error;
};

/// One segment that could not be read, and why.
///
/// Reported rather than thrown only when the Session was opened lenient. The
/// segment's time span then reads back as a gap, exactly like any other
/// missing data — which is why the list matters: silently returning NaN where
/// data should be is its own hazard.
struct SegmentProblem {
  std::string channel;   ///< "" for a session-level record file
  /// The segment's number; -1 for a session- or channel-level record file.
  int segment_number = 0;
  std::string segment;   ///< human-readable location (segment dir or record file)
  std::string reason;    ///< the exception that was contained
  /// "segment": the segment could not be read and its span reads back as a
  /// gap. "records": one record (.rdat) file was skipped; samples unaffected.
  std::string kind = "segment";
};

// A decode job: where a block lives (index into an owned buffer) and where its
// samples land in absolute time. Used for parallel decoding.
struct BlockJob {
  std::size_t buffer_index = 0;
  std::size_t offset = 0;
  std::size_t block_bytes = 0;
  /// Where the block goes and how much of it there is — taken from the RED
  /// BLOCK HEADER, not the index. See BlockCopyMismatch for why.
  si8 start_uutc = 0;
  ui4 number_of_samples = 0;
  /// The `.tidx` entry's copies of the same two values, kept for comparison.
  si8 index_start_uutc = 0;
  ui4 index_number_of_samples = 0;
  /// The block's OWN segment's section-2 conversion factor (#150): segments
  /// of one channel may store different ones, so it is never the channel's.
  sf8 units_conversion_factor = 1.0;
  int segment_number = 0;
  crypto::AccessKeys keys;
};

/// A block whose two stored copies of a value disagree.
///
/// A block's start time and sample count each exist TWICE: in the `.tidx`
/// entry and in the RED block header at the head of the block itself. Nothing
/// in the format keeps them in step, and the two are not equally trustworthy:
///
///   * the header copy is covered by the per-block CRC, which is verified on
///     every decode; the index copy is covered only by the `.tidx` body CRC,
///     which the read path does not check at all;
///   * the header copy is the one meflib and pymef place data by
///     (`pymef3_file.c`, the `times_specified` path), so following it is
///     parity by construction rather than by agreement.
///
/// mef3io therefore places by the header and uses the index only to SELECT
/// blocks — which is what meflib does too. On every file mef3io or the legacy
/// stack has ever written the copies are identical, so this changes nothing;
/// it only decides what happens on a file where they have drifted apart, and
/// there it reports rather than silently picking one.
struct BlockCopyMismatch {
  std::string segment;            ///< human-readable segment location
  std::size_t block_index = 0;    ///< position within the segment's index
  si8 index_start_uutc = 0;
  si8 header_start_uutc = 0;
  ui4 index_number_of_samples = 0;
  ui4 header_number_of_samples = 0;

  bool times_differ() const { return index_start_uutc != header_start_uutc; }
  bool counts_differ() const { return index_number_of_samples != header_number_of_samples; }
};

struct BlockJobs {
  std::vector<std::vector<ui1>> buffers;  // owns the .tdat images referenced by jobs
  std::vector<BlockJob> jobs;
  sf8 sampling_frequency = 0.0;
  /// Blocks whose index entry and RED header disagree. Empty for every
  /// well-formed file; non-empty means the file is internally inconsistent.
  std::vector<BlockCopyMismatch> mismatches;
};

// One RED block's index entry with absolute (user) times resolved.
struct BlockIndexEntry {
  si8 start_uutc = 0;
  si8 start_sample = 0;
  ui4 number_of_samples = 0;
  si4 maximum_sample_value = 0;
  si4 minimum_sample_value = 0;
  bool discontinuity = false;
};

// Per-block extremes of a channel over a time window, from the block index
// alone (block_envelope): one row per data block, nothing decoded. Values are
// scaled like read(): by the conversion factor of the block's own segment
// (swapped for a negative one); a writer that left a block's min/max unset
// (RED_NAN) gives NaN there.
struct BlockEnvelope {
  std::vector<si8> start_uutc;  // first sample of the block
  std::vector<si8> end_uutc;    // just past its last sample: start + n / fs
  std::vector<sf8> minimum;
  std::vector<sf8> maximum;
};

// Per-segment map entry: where one segment's data lies in time and in
// channel-wide sample indices. Metadata only — nothing is decoded — so it is
// cheap even for huge sessions, and it locates data across arbitrary gaps.
struct SegmentInfo {
  int segment_number = 0;
  std::string path;             // the .segd directory ("<archive>::<member>" for tar sessions)
  si8 start_time = 0;           // absolute uUTC of the first sample
  si8 end_time = 0;             // absolute uUTC just past the last sample
  si8 start_sample = 0;         // channel-wide index of the first sample
  si8 number_of_samples = 0;    // samples stored in this segment (gaps excluded)
  si8 number_of_blocks = 0;
  /// This segment's section-2 units_conversion_factor, as stored (0 = MEF's
  /// "no entry"). Segments of one channel may differ (#150).
  sf8 units_conversion_factor = 1.0;
  /// Provenance of this segment's .tmet (see fmt::provenance): versions and
  /// operations only, no times. `present` is false for a file from another
  /// writer, or from mef3io before 1.2.
  fmt::provenance::Provenance provenance;
};

struct ChannelInfo {
  std::string name;
  sf8 sampling_frequency = 0.0;
  /// The factor every readable segment stores; nullopt when they differ
  /// (#150; meflib then sets the channel's to NO_ENTRY, meflib.c:5494). The
  /// reads scale each block by its own segment's factor either way, and
  /// SegmentInfo::units_conversion_factor holds each segment's.
  std::optional<sf8> units_conversion_factor = 1.0;
  std::string units_description;
  si8 start_time = 0;      // absolute uUTC
  si8 end_time = 0;        // absolute uUTC
  si8 number_of_samples = 0;
  si8 recording_time_offset = 0;
  int n_segments = 0;
  si4 gmt_offset = 0;
  // Section-2 (level-1) descriptive / acquisition metadata.
  std::string session_description;
  std::string channel_description;
  std::string reference_description;
  si8 acquisition_channel_number = 1;
  sf8 low_frequency_filter = -1.0;
  sf8 high_frequency_filter = -1.0;
  sf8 notch_filter = -1.0;
  sf8 line_frequency = -1.0;
  // Section-3 (level-2) subject metadata: populated only when the password
  // grants L2 access (or the session is unencrypted); section3_available says.
  bool section3_available = false;
  // Any segment carries password validation fields (metadata is encrypted at
  // some level). The warm-start cache never snapshots such a session.
  bool password_protected = false;
  // The password opened at least one segment only through the byte mapping
  // mef3io used up to 1.2.0 (issue #84): meflib/pymef cannot open those files
  // with this password. Readers surface it as a warning.
  bool legacy_password_mapping = false;
  std::string subject_name_1;
  std::string subject_name_2;
  std::string subject_id;
  std::string recording_location;
};

// One allocation-relevant declaration a segment leaves unset. Cheap enough to
// collect at open time (the metadata is already parsed); a reader can surface
// it as a warning without slowing anything down.
struct DeclarationIssue {
  std::string channel;
  int segment_number = 0;
  std::string field;
};

class Session {
 public:
  // Discover the session tree and load per-channel basic info (lazy on data).
  // `mefd_path` is either a .mefd directory or an uncompressed tar archive of
  // one (e.g. name.mefd.tar) — tar sessions are read without extraction.
  /// @param strict  true (the default) propagates a segment-level failure, so
  ///                 one damaged file fails the whole session — the safe
  ///                 behaviour when an unreported CRC failure would otherwise
  ///                 reach an analysis unnoticed. false contains the failure to
  ///                 the segment, leaves its span reading back as a gap, and
  ///                 records it in problems().
  /// @param n_threads  how many threads this object may use (docs/threads.md):
  ///                 0 = automatic (resolve_threads), 1 = no extra threads.
  ///                 The open lists channel folders, checks segment files and
  ///                 reads + parses every .tmet on that many threads; its
  ///                 result, its error (strict) and problems() (lenient) do
  ///                 not depend on the count.
  Session(const std::string& mefd_path, std::string password = "", bool strict = true,
          int n_threads = 0);
  /// INTERNAL / TESTING — not part of the supported API. Opens a session
  /// through an existing source (a decorator that counts or delays requests,
  /// as the open-cost tests do). It skips the session NAMING rule (`.mefd` /
  /// `.mefd.tar`) that every public path enforces, because the source decides
  /// what it serves; use the path constructor in applications. The source must
  /// be safe for concurrent calls.
  Session(std::shared_ptr<const SessionSource> source, std::string password = "",
          bool strict = true, int n_threads = 0);

  /// Thread count for the NEXT call (0 = automatic, 1 = no extra threads);
  /// calls in flight keep theirs. Safe while other threads read.
  /// @throws std::invalid_argument for a negative count.
  void set_threads(int n) { n_threads_.store(check_threads(n), std::memory_order_relaxed); }
  /// The thread count as set (0 = automatic), not resolved.
  int threads() const { return n_threads_.load(std::memory_order_relaxed); }

  /// Segments that failed to load, in lenient mode. Always empty when strict.
  /// A copy: lenient reads on other threads may append while you look.
  std::vector<SegmentProblem> problems() const {
    std::lock_guard<std::mutex> lock(state_mu_);
    return problems_;
  }

  /// Record (.rdat) files read so far whose password matched only through
  /// the byte mapping mef3io used up to 1.2.0 (issue #84). Grows as records
  /// are read; deduplicated. (Metadata is reported per channel instead, via
  /// ChannelInfo::legacy_password_mapping, because it is read at open.)
  std::vector<std::string> legacy_password_record_files() const {
    std::lock_guard<std::mutex> lock(state_mu_);
    return legacy_password_record_files_;
  }
  bool strict() const { return strict_; }

  const std::vector<std::string>& channels() const { return channel_names_; }
  /// The session's name (SessionSource::session_name: for an archive, the
  /// folder INSIDE it, which survives a rename of the archive file).
  std::string session_name() const { return source_->session_name(); }
  /// Where the session root is, as error messages and problems() print it:
  /// the directory, or "<archive>::<root folder>" for a tar.
  std::string location() const { return source_->describe(""); }
  const ChannelInfo& channel_info(const std::string& name) const;

  // Decode all blocks of `channel` overlapping [t0, t1] (absolute uUTC;
  // defaults span the whole channel). Returns contiguous runs split on
  // discontinuities. Samples are the raw int32 stored values (no scaling,
  // no trimming to exact sample yet — that is the high-level Reader's job).
  std::vector<DataRun> read_runs(const std::string& channel,
                                 std::optional<si8> t0 = std::nullopt,
                                 std::optional<si8> t1 = std::nullopt);

  // All RED block index entries for a channel (across segments), with absolute
  // times resolved. Does not decode data. For viewers / seeking.
  std::vector<BlockIndexEntry> read_index(const std::string& channel);

  // Min/max of every data block overlapping [t0, t1) (unset ends = the whole
  // channel), from the block index only: no .tdat byte is read. Segments
  // outside the window are skipped without reading their index. Times come
  // from the index entries (a block's RED header copy is not read here).
  BlockEnvelope block_envelope(const std::string& channel, std::optional<si8> t0 = std::nullopt,
                               std::optional<si8> t1 = std::nullopt);

  // Per-segment map of a channel, sorted by segment number: time range, sample
  // range and block count of each segment, from metadata only. Use this to see
  // what data is where (e.g. across huge recording gaps); read_index gives the
  // finer block-level view including in-segment discontinuities.
  std::vector<SegmentInfo> segment_map(const std::string& channel);

  // Gather (without decoding) all blocks of `channel` overlapping [t0, t1],
  // loading the needed .tdat images into owned buffers. The high-level Reader
  // decodes these in parallel. `ctl.cancel` is checked before each segment;
  // a cancelled call throws Cancelled and returns nothing.
  BlockJobs collect_blocks(const std::string& channel, std::optional<si8> t0 = std::nullopt,
                           std::optional<si8> t1 = std::nullopt, const Control& ctl = {});

  // Section-2 size declarations the session leaves unset, across every channel
  // and segment. Free at this point: every segment's metadata was parsed when
  // the session was opened. Nothing here affects mef3io's own reads — it sizes
  // from each block's own header — but meflib-based readers allocate from
  // these fields, so it is worth telling the user. `validate_session` is the
  // thorough version; this only sees what is missing, not what is wrong.
  std::vector<DeclarationIssue> declaration_issues() const;

  /// Cap on cached block indices, in bytes (default 256 MB). 0 = unlimited.
  /// Lowering it trades re-reads for resident memory; the indices are read
  /// sequentially, so a re-read is cheap next to decoding.
  /// Safe while other threads read: takes effect at the next trim.
  void set_index_cache_bytes(std::size_t n) {
    index_budget_.store(n, std::memory_order_relaxed);
  }
  std::size_t index_cache_bytes() const;

  /// Which record levels read_records returns.
  ///
  /// `Default` keeps the historical meaning of each scope: no channel ->
  /// session-level records only; a channel -> that channel's channel-level
  /// records AND those of its segments; a channel + segment -> that segment's.
  /// `All` widens the no-channel scope to every record in the session.
  enum class RecordLevels { Default, Session, Channel, Segment, All };

  /// Read records (annotations), each tagged with the level, channel and
  /// segment it was found at, grouped session -> channel -> its segments
  /// (channels by name, segments by number), each file in its own order.
  ///
  /// Times are resolved with the offset of what the file belongs to: a
  /// segment's own section 3; for a channel or the session, the offset ALL of
  /// its readable segments agree on. When they disagree AND the file stores
  /// an offset-dependent (negated) time, there is no correct answer, so that
  /// file fails with FormatError rather than guess (a deliberate divergence
  /// from meflib, which uses whichever offset it read last — see
  /// read_record_file). Other files, including the segment-level ones, are
  /// unaffected by that.
  ///
  /// A record file that cannot be read throws when strict; when lenient it is
  /// skipped and reported in problems() (segment_number -1 for a session- or
  /// channel-level file) without marking any segment's data unreadable.
  ///
  /// @throws std::invalid_argument  a segment without a channel, or a level
  ///         outside the scope (Session within a channel; Session/Channel
  ///         within a segment).
  /// @throws std::out_of_range      unknown channel or segment number.
  std::vector<Record> read_records(std::optional<std::string> channel = std::nullopt,
                                   std::optional<int> segment = std::nullopt,
                                   RecordLevels levels = RecordLevels::Default);

 private:
  struct Channel {
    ChannelInfo info;
    std::vector<SegmentReader> segments;  // sorted by segment number
  };

  std::uint64_t index_tick_ = 0;
  std::atomic<std::size_t> index_budget_{256u * 1024u * 1024u};
  std::atomic<int> n_threads_{0};

  void discover();
  /// `errors[k]` is what loading segment k's metadata threw (null = loaded).
  void load_channel_basic_info(const std::string& name, Channel& ch,
                               const std::exception_ptr* errors);
  /// The offset every readable segment of `channel` (or of the whole session
  /// when null) agrees on; nullopt, with `why` set, when they disagree.
  std::optional<si8> agreed_rto(const std::string* channel, std::string& why);
  /// Parse one record file and append its records, tagged with where they
  /// came from. `rto` nullopt = disputed (see agreed_rto): the file is still
  /// read if none of its times depends on the offset. Throws when strict;
  /// when lenient a failure is reported in problems() and the file skipped.
  void read_record_file(std::vector<Record>& out, const std::string& rdat_path,
                        std::optional<si8> rto, const std::string& why, RecordLevel level,
                        const std::string& channel, int segment);
  /// The segment's metadata, loaded at open. Lock-free: it never changes
  /// after the open (#110). Only for a segment that did not fail.
  const TimeSeriesMetadata& segment_metadata(const SegmentReader& seg) const;
  /// Run `fn` for one segment, containing its failure when lenient.
  ///
  /// Returns false when the segment is unusable — already known bad, or `fn`
  /// threw and the failure was contained. Strict mode never contains anything,
  /// so the exception propagates as before. The caller is responsible for
  /// rolling back anything `fn` appended before it threw; a half-written
  /// segment is worse than a skipped one.
  template <class F>
  bool with_segment(const std::string& channel, SegmentReader& seg, F&& fn) {
    if (segment_failed(seg)) return false;
    if (strict_) {
      fn();
      return true;
    }
    try {
      fn();
      return true;
    } catch (const std::exception& e) {
      std::lock_guard<std::mutex> lock(state_mu_);
      if (seg.load_error.empty()) {  // two threads can fail the same segment at once
        seg.load_error = e.what();
        problems_.push_back(SegmentProblem{channel, seg.segment_number,
                                           source_->describe(seg.tmet_path), e.what()});
      }
      return false;
    }
  }
  bool segment_failed(const SegmentReader& seg) const {
    std::lock_guard<std::mutex> lock(state_mu_);
    return !seg.load_error.empty();
  }
  /// The segment's block index, PINNED: the returned reference keeps the bytes
  /// alive even if another thread evicts them from the cache meanwhile.
  std::shared_ptr<const std::vector<ui1>> segment_index(SegmentReader& seg);
  /// Whether `seg`'s CACHED index certainly holds no block overlapping
  /// [lo, hi) (its learnt span; false when not cached or not learnt).
  bool skip_segment(const SegmentReader& seg, si8 lo, si8 hi) const;
  /// Record the span of `pin` if it is still the cached index and none is
  /// known yet.
  void learn_span(SegmentReader& seg, const std::shared_ptr<const std::vector<ui1>>& pin,
                  std::size_t n_entries, si8 rto, sf8 fs);
  /// Drop the least recently used block indices until the cache fits its
  /// budget.
  ///
  /// The index is ~2% of the data, so a session of a few hundred gigabytes
  /// holds several GB of it — and a reader that touches every channel used to
  /// keep all of it resident for the life of the Session. Called only at the
  /// END of a public operation. Concurrent calls on one Session are safe: a
  /// call that is still reading an evicted index holds its own pin.
  void trim_index_cache();

  std::string mefd_path_;
  std::shared_ptr<const SessionSource> source_;
  std::string password_;
  std::vector<std::string> channel_names_;
  std::map<std::string, Channel> channels_;
  bool strict_ = true;
  // Concurrent calls on ONE Session (several threads reading one Reader, now
  // that the bindings release the GIL) share this mutable state, so it is
  // locked. Two mutexes, never held together and never across file I/O or a
  // decode: state_mu_ for what lenient reads and records append (problems,
  // failed segments, legacy record files, metadata), index_mu_ for the block
  // index cache. Uncontended locks cost nanoseconds against a file read.
  mutable std::mutex state_mu_;
  mutable std::mutex index_mu_;
  std::vector<SegmentProblem> problems_;
  std::vector<std::string> legacy_password_record_files_;
  std::set<std::string> reported_record_files_;  // record files already in problems_
  std::vector<std::string> record_only_channels_;  // records but no segments (yet)
};

}  // namespace mef3io
