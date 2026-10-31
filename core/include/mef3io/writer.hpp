// mef3io — low-level time-series segment writer. Produces byte-valid .tmet /
// .tidx / .tdat that meflib/pymef read. The high-level Writer (P5) builds the
// block layout (discontinuity splitting, scaling) and calls this.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "mef3io/types.hpp"

namespace mef3io {

// One RED block's worth of already-int32 samples with its absolute start time.
struct BlockSpec {
  std::vector<si4> samples;
  si8 start_uutc = 0;      // absolute (offset not yet applied)
  si8 start_sample = 0;    // channel-wide sample index of first sample
  bool discontinuity = false;
};

// User-settable session/subject/acquisition metadata (MEF section 2 & 3
// descriptive fields). All optional with sane defaults; the writer fills the
// computed fields (counts, extrema, durations) itself. Empty string / NO_ENTRY
// sentinels mean "unset". Subject fields live in the level-2-encrypted
// section 3; the descriptive/acquisition fields in the level-1 section 2.
struct SessionMetadata {
  // --- section 2: descriptive / acquisition ---
  std::string session_description;      // free text; defaults to session name
  std::string channel_description;      // free text; defaults to channel name
  std::string reference_description;    // e.g. montage/reference note
  si8 acquisition_channel_number = 1;   // amplifier channel index
  sf8 low_frequency_filter = -1.0;      // Hz; -1 = not recorded
  sf8 high_frequency_filter = -1.0;     // Hz
  sf8 notch_filter = -1.0;              // Hz
  sf8 line_frequency = -1.0;            // Hz (mains, e.g. 50/60)
  // --- section 3: subject / time zone ---
  std::string subject_name_1;
  std::string subject_name_2;
  std::string subject_id;
  std::string recording_location;
  si4 gmt_offset = 0;                   // seconds
};

/// Everything an append derives by walking the whole `.tidx`, carried across
/// calls so it does not have to walk it again.
///
/// A segment's declarations describe all of its blocks, so the append
/// recomputed them from the full index every time — reading, CRC-ing, walking
/// and rewriting the entire file. That is O(total blocks) per append, which is
/// quadratic over a session, and these sessions run for days to months.
///
/// Holding the summary makes an append O(NEW data): entries are appended in
/// place, the body CRC is extended over just the new bytes (the Koopman CRC is
/// a rolling state with no final inversion, exactly as the `.tdat` path has
/// always done), and the totals are folded in. `file_size` is checked against
/// the file before the fast path is taken, so anything that changed the index
/// behind our back falls back to the full walk.
struct AppendIndexCache {
  bool valid = false;         ///< false => walk the index and populate this
  std::size_t file_size = 0;  ///< .tidx size this summary describes
  std::size_t entries = 0;
  si8 total_samples = 0;
  si8 n_discontinuities = 0;
  ui4 max_block_samples = 0;
  si8 max_block_bytes = 0;
  ui4 body_crc = 0;           ///< rolling CRC over every entry in the file
  si8 run_blocks = 0, run_samples = 0, run_bytes = 0;      ///< open run
  si8 max_blocks = 0, max_samples = 0, max_bytes = 0;      ///< longest run seen
};

struct SegmentSpec {
  std::string session_name;
  std::string channel_name;
  int segment_number = 0;
  sf8 sampling_frequency = 0.0;
  sf8 units_conversion_factor = 1.0;
  std::string units_description = "uV";
  si8 recording_time_offset = 0;
  si4 gmt_offset = 0;
  std::string password_1;  // empty -> unencrypted
  std::string password_2;  // empty -> section 3 not L2-encrypted
  SessionMetadata metadata;  // descriptive/subject fields (see above)
  /// Exact maximum `difference_bytes` over the segment's PRE-EXISTING blocks,
  /// or 0 when unknown. Append only; ignored when writing a fresh segment.
  ///
  /// The old blocks' real values live in .tdat block headers, so an append that
  /// had to discover them would pay a seek per block and lose its O(new data)
  /// cost. With 0 it therefore declares meflib's worst case instead — safe, but
  /// 1.0-1.4x the truth. A caller that ENCODED those blocks itself already
  /// knows the exact value and can pass it here to keep the declaration exact;
  /// it must be a true maximum, since a reader allocates its difference buffer
  /// from the result and an under-declaration truncates that buffer.
  ui4 known_difference_bytes = 0;
  /// Fresh segment only: flush the new `.tmet` before it is renamed into
  /// place. Set by `SessionWriter` for a channel's FIRST segment (and any new
  /// segment in a session that already held data): a short `.tmet` fails the
  /// strict open of the whole session, and a first segment has no previous
  /// `.tmet` recovery could rebuild it from. (An APPEND always flushes its
  /// `.tmet`.)
  bool flush_new_metadata = false;
  /// Every file this write touched (.tdat, .tidx, .tmet) is flushed in ONE
  /// combined wait (`flush_files`) before the `.tmet` is renamed into place,
  /// so the call returns only when its data is on disk.
  bool wait_for_disk = false;
  /// `flush="close"` mode (#166): the `.tmet` is renamed in WITHOUT the
  /// barrier that precedes it. The caller owes one combined flush later
  /// (SessionWriter::flush / close). A power cut before it can leave a short
  /// `.tmet` that fails the strict open of the whole session.
  bool skip_tmet_flush = false;
};

/// A `.tmet` written to its temp file but NOT yet renamed into place: what a
/// segment write leaves behind when the caller asks to publish later
/// (`defer`), so that many channels' barriers can be one combined wait
/// (SessionWriter::write_int32_many, #165). The sequence is
/// flush_pending (before) -> publish_pending (rename) -> flush_pending (after,
/// wait_for_disk only); discard_pending undoes an append that was not published.
struct PendingPublish {
  std::filesystem::path tmp, target;
  /// Default mode: the temp must be durable before the rename (an append, or
  /// a first segment); `flush_with`/`flush_target` are wait_for_disk's.
  bool flush_tmp = false;
  std::vector<std::filesystem::path> flush_with;  ///< wait_for_disk: .tdat, .tidx
  bool flush_target = false;                      ///< wait_for_disk: the published name too
  /// Append only: restores the `.tdat`/`.tidx` the append extended. Returns
  /// an error text ("" = restored). Empty for a fresh segment, whose
  /// caller removes the folder.
  std::function<std::string()> rollback;
};

/// ONE combined flush for the given pendings: before their renames (temps that
/// need it, plus wait_for_disk's data files) or, with @p after_publish, the
/// published names. No-op when nothing needs a wait.
void flush_pending(const std::vector<PendingPublish*>& pending, bool after_publish);
/// Rename the temp over its target.
void publish_pending(PendingPublish& p);
/// Remove an unpublished temp and undo an append's extension. Returns an
/// error text for a rollback that was incomplete ("" = clean).
std::string discard_pending(PendingPublish& p);

/// Largest RED block, in samples, that the format can describe: a block's
/// byte count is a ui4 in both its RED header and its `.tidx` entry, with
/// 0xFFFFFFFF reserved as NO_ENTRY, and meflib's worst case for n samples is
/// RED_MAX_COMPRESSED_BYTES = 5 n + 304 (header) + 7 (pad) bytes. A longer
/// block would wrap those fields and write a header that disagrees with its
/// data, so every write path refuses one up front (#83).
inline constexpr si8 MAX_BLOCK_SAMPLES = (static_cast<si8>(0xFFFFFFFEu) - 304 - 7) / 5;

/// Throws std::invalid_argument, naming the bad value, unless the sampling
/// frequency is finite and > 0 and the units conversion factor finite and
/// non-zero (#83; a negative factor is a polarity inversion and is allowed).
/// A rate of 0, NaN or infinity used to hang the writer, a negative one to
/// throw from deep inside std::vector, and a conversion factor of 0 or NaN
/// to be stored, after which every sample reads back as 0 or NaN — the
/// signal unrecoverable. Every write path calls this before anything exists
/// on disk.
void check_signal_parameters(sf8 sampling_frequency, sf8 units_conversion_factor);

/// Throws std::invalid_argument unless `samples_per_block` is at most
/// MAX_BLOCK_SAMPLES. Values <= 0 are accepted: the writers read them as
/// "derive the block length from the rate".
void check_block_length(si8 samples_per_block);

// Write the three files for one segment into `segment_dir` (which must exist).
// Blocks must be time-ordered. Returns the number of samples written.
// Blocks are RED-encoded in parallel (n_threads: negative throws
// std::invalid_argument; 0 -> hardware concurrency,
// 1 -> serial) then assembled into the .tdat in order, so the output is
// byte-identical regardless of thread count.
/// `out_max_difference_bytes` (optional) receives the exact maximum
/// `difference_bytes` measured over the blocks written here, for a caller that
/// wants to carry it into a later append as `SegmentSpec::known_difference_bytes`.
si8 write_time_series_segment(const std::string& segment_dir, const SegmentSpec& spec,
                              const std::vector<BlockSpec>& blocks, int n_threads = 0,
                              ui4* out_max_difference_bytes = nullptr,
                              PendingPublish* defer = nullptr);

// Append blocks to an EXISTING segment (in-segment append): extends the .tdat
// and .tidx of the segment whose folder is `segment_dir`; its files are named
// after the folder (`<folder stem>.tdat` ...), so a foreign un-padded folder
// such as `ch1-5.segd` is extended under its own name.
//
// and .tidx in place and rewrites the .tmet statistics plus the universal
// headers' end times / entry counts / CRCs. The segment's fs and conversion
// factor are authoritative: a mismatch with `spec`, or a first block starting
// before the segment's stored end time, throws WriteConflictError. Encrypted
// segments need a password granting at least level-1 access (to re-encrypt
// section 2; section 3 bytes are preserved verbatim). File UUIDs and password
// validation fields are preserved. Returns the number of samples appended.
/// `out_max_difference_bytes` (optional) receives the exact maximum
/// `difference_bytes` over the blocks appended here — the NEW blocks only, not
/// the segment's declared maximum.
/// `cache` (optional) carries the index summary across appends. Pass the same
/// object back on every append to the same segment and the index is never
/// walked twice; pass nullptr, or a default-constructed one, for the full walk.
/// `defer` (optional): leave the new `.tmet` as an unpublished temp file and
/// fill this in (see PendingPublish); the `.tdat`/`.tidx` are extended as
/// usual. Same for write_time_series_segment.
si8 append_time_series_segment(const std::string& segment_dir, const SegmentSpec& spec,
                               const std::vector<BlockSpec>& blocks, int n_threads = 0,
                               ui4* out_max_difference_bytes = nullptr,
                               AppendIndexCache* cache = nullptr,
                               PendingPublish* defer = nullptr);

}  // namespace mef3io
