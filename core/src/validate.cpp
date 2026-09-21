// mef3io — session validation and targeted repair. See validate.hpp for the
// contract; this file holds the check registry and the repair mechanics.
#include "mef3io/validate.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <set>
#include <stdexcept>
#include <system_error>
#include <tuple>

#include "mef3io/byteio.hpp"
#include "mef3io/crc.hpp"
#include "mef3io/crypto.hpp"
#include "mef3io/errors.hpp"
#include "mef3io/headers.hpp"
#include "mef3io/metadata.hpp"
#include "mef3io/source.hpp"

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fsys = std::filesystem;

namespace mef3io {
namespace {

// --- small helpers -----------------------------------------------------------

std::string num(si8 v) { return std::to_string(v); }
std::string num(ui4 v) { return std::to_string(static_cast<si8>(v)); }

// Render a declaration for a report, naming the sentinels a reader special-cases.
std::string declared_ui4(ui4 v) {
  if (v == fmt::UI4_NO_ENTRY) return "NO_ENTRY";
  return num(v);
}
std::string declared_si8(si8 v) {
  if (v == fmt::SI8_NO_ENTRY) return "NO_ENTRY";
  if (v == fmt::UUTC_NO_ENTRY) return "NO_ENTRY";
  return num(v);
}

int segment_number_from_name(const std::string& stem) {
  const auto dash = stem.rfind('-');
  if (dash == std::string::npos) return 0;
  try {
    return std::stoi(stem.substr(dash + 1));
  } catch (const std::exception&) {
    return 0;
  }
}

bool selected(const std::vector<std::string>& filter, const std::string& value) {
  return filter.empty() || std::find(filter.begin(), filter.end(), value) != filter.end();
}
bool selected(const std::vector<int>& filter, int value) {
  return filter.empty() || std::find(filter.begin(), filter.end(), value) != filter.end();
}

// Times on disk carry meflib's sign convention: a negative value means the
// recording-time offset has been applied (absolute = -stored + rto), a
// non-negative one is already absolute. Foreign writers mix the two within a
// segment — pymef negates block times but not universal-header times — so
// every time comparison here happens in absolute space, exactly as a reader
// does it.
si8 to_user_time(si8 stored, si8 rto) {
  if (stored == fmt::UUTC_NO_ENTRY) return stored;
  return stored >= 0 ? stored : -stored + rto;
}
si8 to_disk_time(si8 absolute, si8 rto) {
  if (absolute == fmt::UUTC_NO_ENTRY) return absolute;
  return rto - absolute;
}

// meflib's RED_MAX_DIFFERENCE_BYTES(x): a full si4 plus one keysample flag
// byte per sample. The worst case a reader can safely allocate from.
ui4 red_max_difference_bytes(ui4 samples) {
  constexpr ui4 kMaxSamples = fmt::UI4_NO_ENTRY / 5u;
  return samples >= kMaxSamples ? fmt::UI4_NO_ENTRY : samples * 5u;
}

// --- one segment, as found on disk -------------------------------------------

struct SegmentFiles {
  std::string channel;
  int segment_number = 0;
  std::string tmet_rel, tidx_rel, tdat_rel;
  std::string description;        // human-readable segment location
  std::vector<std::string> missing;  // of .tmet/.tidx/.tdat, those absent
};

// What the files declare, plus the raw bytes needed to check and rewrite them.
struct SegmentState {
  SegmentFiles files;
  std::vector<ui1> tmet_bytes;
  std::vector<ui1> tidx_bytes;
  std::vector<ui1> tdat_uh;  // universal header only; .tdat body is never read whole
  std::uint64_t tdat_size = 0;
  fmt::UniversalHeader tidx_uh, tdat_uh_parsed;
  TimeSeriesMetadata md;
};

// Everything the same segment's data actually says, recomputed from the index
// (and, for difference_bytes, from the RED block headers).
struct SegmentTruth {
  si8 n_blocks = 0;
  si8 total_samples = 0;
  si8 start_sample = 0;
  si8 max_block_bytes = 0;
  ui4 max_block_samples = 0;
  ui4 max_difference_bytes = 0;
  bool difference_bytes_exact = false;  // false => a bound, not a measurement
  si8 contiguous_blocks = 0;
  si8 contiguous_block_bytes = 0;
  si8 contiguous_samples = 0;
  si8 n_discontinuities = 0;
  si8 rto = 0;
  si8 first_start_uutc = 0;  // absolute, sign convention already resolved
  si8 end_uutc = 0;
  si8 recording_duration = 0;
  si8 block_interval = 0;
  si8 data_bytes = 0;  // sum of block_bytes
  bool has_blocks = false;  // false => the index is empty; derive nothing from it
  bool times_known = true;  // false => an entry carried UUTC_NO_ENTRY
  bool rto_known = true;    // false => section 3 unreadable; rto is UNKNOWN, not 0
  bool times_comparable = true;  // false => rto non-zero or unknown; conventions ambiguous
  // False => sampling_frequency is not a usable number, so end_uutc /
  // recording_duration / block_interval could not be derived at all. Nothing
  // in the registry validates fs itself, and every time expectation divides by
  // it: a denormal-but-positive value makes `n * 1e6 / fs` exceed si8 range,
  // where std::llround is undefined and in practice yields LLONG_MIN. That
  // poisons the comparisons (a negative slack fails `|stored-expected| <=
  // slack` even when the difference is zero, so the report shows an error with
  // stored == expected) and, if repaired, overwrites the one correct record of
  // the segment's times with nonsense.
  bool times_derivable = false;
  bool difference_bytes_suspect = false;  // a block header exceeded the worst case
  bool offsets_sane = true;
  std::string offset_problem;
  // False => the index stops short of the data file, i.e. it describes less
  // than .tdat actually holds. An index that has lost entries still looks
  // internally consistent, so nothing else catches it.
  bool index_covers_data = true;
  si8 unaccounted_tail_bytes = 0;
  // False => some entry left number_of_samples or block_bytes at NO_ENTRY.
  // Those are coerced to 0 so a sentinel cannot inflate a total, which means
  // every total derived here is then an UNDER-estimate.
  bool index_counts_known = true;
  si8 unknown_count_entries = 0;
};

// The mutable declarations a repair may write back.
struct RepairBuffer {
  fmt::TimeSeriesMetadataSection2 s2;
  fmt::UniversalHeader tmet_uh, tidx_uh, tdat_uh;
  bool tmet_dirty = false, tidx_dirty = false, tdat_dirty = false;
};

using DetectFn = std::function<void(const SegmentState&, const SegmentTruth&, Finding&, bool&)>;
// Returns whether it actually changed a declaration. A repair is allowed to
// decline — sizing.difference-bytes will not write 0, the NULL-buffer value it
// exists to remove — and a declined repair must not be reported as one:
// `repaired` on a finding means "this was written", not "a repair was offered".
using RepairFn = std::function<bool(const SegmentTruth&, RepairBuffer&)>;

struct CheckImpl {
  CheckInfo info;
  DetectFn detect;  // sets `hit` and fills the finding's field/stored/expected/message
  RepairFn repair;  // empty when info.repairable is false
};

// --- the registry ------------------------------------------------------------
//
// Order matters: integrity first (if the bytes cannot be trusted nothing
// derived from them can be either), then structure, then the declarations a
// reader allocates from, then the cosmetic-but-wrong time fields.

const std::vector<CheckImpl>& check_impls() {
  static const std::vector<CheckImpl> impls = [] {
    std::vector<CheckImpl> v;

    v.push_back({{"crc.metadata", "Metadata CRCs verify",
                  "The .tmet universal header stores a CRC over the header and one over the "
                  "metadata sections. A mismatch means the declarations cannot be trusted, so "
                  "no repair is attempted on that segment.",
                  Severity::Error, false},
                 [](const SegmentState& s, const SegmentTruth&, Finding& f, bool& hit) {
                   const auto& b = s.tmet_bytes;
                   const ui4 stored_header = byteio::read<ui4>(b, 0);
                   const ui4 stored_body = byteio::read<ui4>(b, 4);
                   const ui4 real_header = crc::calculate(
                       std::span<const ui1>(b).subspan(4, fmt::UNIVERSAL_HEADER_BYTES - 4));
                   // The record is fixed-length; foreign writers pad past its
                   // end, so the body CRC stops at METADATA_FILE_BYTES.
                   const ui4 real_body = crc::calculate(std::span<const ui1>(b).subspan(
                       fmt::UNIVERSAL_HEADER_BYTES,
                       fmt::METADATA_FILE_BYTES - fmt::UNIVERSAL_HEADER_BYTES));
                   // CRC_NO_ENTRY (0) is meflib's "never computed" marker —
                   // meflib.h:234, written at meflib.c:689 and :4614-4615. NOT
                   // CRC_START_VALUE, which is only the register seed
                   // (meflib.h:1214). metadata.cpp accepts a no-entry CRC
                   // rather than rejecting the session, so the validator must
                   // not call such a file corrupt — it reads perfectly.
                   const bool body_no_entry = stored_body == fmt::CRC_NO_ENTRY;
                   const bool header_no_entry = stored_header == fmt::CRC_NO_ENTRY;
                   const bool body_ok = stored_body == real_body || body_no_entry;
                   const bool header_ok = stored_header == real_header || header_no_entry;
                   if (header_ok && body_ok) {
                     // Accepted, but say so: "not computed" is not "verified".
                     // Silence here rendered an UNVERIFIABLE file as a clean
                     // one, and a torn write that zeroes a CRC would then have
                     // switched off the only check that could have caught it.
                     // A warning, not an error — the file may be perfectly
                     // good, and a streaming writer legitimately produces this.
                     if (!header_no_entry && !body_no_entry) return;
                     hit = true;
                     f.severity = Severity::Warning;
                     f.field = header_no_entry ? "header_CRC" : "body_CRC";
                     f.stored = "NO_ENTRY";
                     f.expected = declared_ui4(header_no_entry ? real_header : real_body);
                     f.message =
                         "the writer never computed this CRC (meflib's CRC_NO_ENTRY), so these "
                         "bytes cannot be verified; accepted, but not checked";
                     return;
                   }
                   hit = true;
                   f.field = !header_ok ? "header_CRC" : "body_CRC";
                   f.stored = declared_ui4(!header_ok ? stored_header : stored_body);
                   f.expected = declared_ui4(!header_ok ? real_header : real_body);
                   f.message = "metadata CRC mismatch; the segment's declarations are not "
                               "trustworthy and will not be repaired";
                 },
                 {}});

    v.push_back({{"crc.index", "Index CRCs verify",
                  "The .tidx universal header stores a CRC over the header and one over the "
                  "block index. A mismatch means the block table itself is damaged.",
                  Severity::Error, false},
                 [](const SegmentState& s, const SegmentTruth&, Finding& f, bool& hit) {
                   const auto& b = s.tidx_bytes;
                   const ui4 real_header = crc::calculate(
                       std::span<const ui1>(b).subspan(4, fmt::UNIVERSAL_HEADER_BYTES - 4));
                   // Bound the body by the entries the header declares, not by
                   // EOF: foreign writers pad past the last entry, and hashing
                   // the padding would reject an intact index (the same trap
                   // .tmet already avoids). Read the count straight from the
                   // universal header — this check runs before the index is
                   // parsed, precisely because nothing derived from it can be
                   // trusted until the CRCs pass.
                   const si8 declared_entries =
                       byteio::read<si8>(b, 32);  // UniversalHeader::number_of_entries
                   const std::size_t avail = b.size() - fmt::UNIVERSAL_HEADER_BYTES;
                   const std::size_t entry_bytes =
                       declared_entries > 0
                           ? static_cast<std::size_t>(declared_entries) * fmt::TIME_SERIES_INDEX_BYTES
                           : avail;
                   const std::size_t body_bytes = std::min(avail, entry_bytes);
                   const ui4 real_body = crc::calculate(
                       std::span<const ui1>(b).subspan(fmt::UNIVERSAL_HEADER_BYTES, body_bytes));
                   const ui4 real_body_to_eof =
                       crc::calculate(std::span<const ui1>(b).subspan(fmt::UNIVERSAL_HEADER_BYTES));
                   const ui4 stored_header = byteio::read<ui4>(b, 0);
                   const ui4 stored_body = byteio::read<ui4>(b, 4);
                   // CRC_NO_ENTRY (0) is meflib's "never computed" marker; see
                   // the note on crc.metadata above.
                   const bool header_ok =
                       stored_header == real_header || stored_header == fmt::CRC_NO_ENTRY;
                   const bool body_no_entry = stored_body == fmt::CRC_NO_ENTRY;
                   const bool header_no_entry = stored_header == fmt::CRC_NO_ENTRY;
                   const bool body_ok = stored_body == real_body ||
                                        stored_body == real_body_to_eof || body_no_entry;
                   if (header_ok && body_ok) {
                     // Same rule as crc.metadata: accepted, but reported. An
                     // unverified index must not read as a verified one — the
                     // repairs derive their truth from exactly these bytes.
                     if (!header_no_entry && !body_no_entry) return;
                     hit = true;
                     f.severity = Severity::Warning;
                     f.field = header_no_entry ? "header_CRC" : "body_CRC";
                     f.stored = "NO_ENTRY";
                     f.expected = declared_ui4(header_no_entry ? real_header : real_body);
                     f.message =
                         "the writer never computed this CRC (meflib's CRC_NO_ENTRY), so the "
                         "block table cannot be verified; accepted, but not checked";
                     return;
                   }
                   hit = true;
                   f.field = !header_ok ? "header_CRC" : "body_CRC";
                   f.stored = declared_ui4(!header_ok ? stored_header : stored_body);
                   f.expected = declared_ui4(!header_ok ? real_header : real_body);
                   f.message = "index CRC mismatch; the block table is damaged";
                 },
                 {}});

    v.push_back({{"index.block-offsets", "Block offsets lie inside the data file",
                  "Every .tidx entry points at a byte range of .tdat. Offsets must start at or "
                  "after the universal header, increase, and stay within the file — otherwise a "
                  "reader seeks past the end or decodes the wrong bytes. Not repairable: the "
                  "data file itself is wrong, not a declaration.",
                  Severity::Error, false},
                 [](const SegmentState&, const SegmentTruth& t, Finding& f, bool& hit) {
                   if (t.offsets_sane) return;
                   hit = true;
                   f.field = "file_offset";
                   f.message = t.offset_problem;
                 },
                 {}});

    v.push_back({{"index.data-coverage", "The index describes the whole data file",
                  "The last block the .tidx accounts for should end at the end of .tdat. When it "
                  "stops short, the index has lost entries while the blocks themselves are still "
                  "on disk — a truncated or partially rewritten index. Such an index stays "
                  "internally consistent (monotonic offsets, every block inside the file, its own "
                  "CRC covering what is left), so no other check sees it, and every declaration "
                  "derived from it is SMALLER than the truth. Not repairable, and it blocks "
                  "repairs on the segment: the index is the only cheap description of .tdat, and "
                  "writing a short one back over section 2 destroys the last record of what the "
                  "data file contains.",
                  Severity::Error, false},
                 [](const SegmentState&, const SegmentTruth& t, Finding& f, bool& hit) {
                   if (t.index_covers_data) return;
                   hit = true;
                   f.field = "file_offset";
                   f.stored = num(t.data_bytes);
                   f.expected = num(t.data_bytes + t.unaccounted_tail_bytes);
                   f.message =
                       num(t.unaccounted_tail_bytes) +
                       " bytes of the data file lie past the last block the index describes. "
                       "Either index entries are missing (repair would write declarations "
                       "smaller than the data), or an append was interrupted after .tdat grew "
                       "and before .tidx was rewritten, leaving an unreferenced tail — in which "
                       "case the index is correct and the tail is the thing to remove. The two "
                       "are indistinguishable from here, so nothing is repaired in this segment.";
                 },
                 {}});

    v.push_back({{"index.entry-counts", "Every index entry declares its size",
                  "Each .tidx entry carries the block's sample count and byte count. An entry "
                  "that leaves either at NO_ENTRY does not say how big its block is, and this "
                  "module coerces such a value to 0 so a sentinel cannot inflate a total — "
                  "which makes every total and maximum derived from that index too SMALL. "
                  "Writing those back would under-declare a reader's buffer, the direction that "
                  "truncates rather than merely wastes. Not repairable (the missing sizes are "
                  "only in the .tdat block headers), and it blocks repairs on the segment.",
                  Severity::Error, false},
                 [](const SegmentState&, const SegmentTruth& t, Finding& f, bool& hit) {
                   if (t.index_counts_known) return;
                   hit = true;
                   f.field = "number_of_samples/block_bytes";
                   f.stored = "NO_ENTRY";
                   f.expected = "a real size";
                   f.message = num(t.unknown_count_entries) +
                               " index entr(ies) leave their sample or byte count unset; totals "
                               "derived from this index would be too small, so nothing is "
                               "repaired in this segment";
                 },
                 {}});

    v.push_back({{"index.block-count", "Declared block count matches the index",
                  "number_of_samples aside, a reader loops over number_of_blocks entries; "
                  "declaring more than the index holds walks off the end of the block table.",
                  Severity::Error, true},
                 [](const SegmentState& s, const SegmentTruth& t, Finding& f, bool& hit) {
                   if (s.md.section2.number_of_blocks == t.n_blocks) return;
                   hit = true;
                   f.field = "number_of_blocks";
                   f.stored = declared_si8(s.md.section2.number_of_blocks);
                   f.expected = num(t.n_blocks);
                   f.message = "metadata declares a block count the index does not hold";
                 },
                 [](const SegmentTruth& t, RepairBuffer& r) {
                   r.s2.number_of_blocks = t.n_blocks;
                   r.tmet_dirty = true;
                   return true;
                 }});

    v.push_back({{"index.sample-count", "Declared sample count matches the index",
                  "number_of_samples is the stored sample total (gaps excluded). Readers size "
                  "whole-channel buffers from it.",
                  Severity::Error, true},
                 [](const SegmentState& s, const SegmentTruth& t, Finding& f, bool& hit) {
                   if (s.md.section2.number_of_samples == t.total_samples) return;
                   hit = true;
                   f.field = "number_of_samples";
                   f.stored = declared_si8(s.md.section2.number_of_samples);
                   f.expected = num(t.total_samples);
                   f.message = "metadata sample count disagrees with the sum over the index";
                 },
                 [](const SegmentTruth& t, RepairBuffer& r) {
                   r.s2.number_of_samples = t.total_samples;
                   r.tmet_dirty = true;
                   return true;
                 }});

    v.push_back({{"index.start-sample", "Declared start sample matches the first block",
                  "start_sample places this segment in the channel-wide sample numbering. "
                  "REPORT ONLY, never repaired: writers disagree on what .tidx start_sample "
                  "means. mef3io stores channel-absolute values in both section 2 and the "
                  "index; pymef resets the index to 0 in every segment and keeps section 2 "
                  "channel-cumulative, and its reader depends on exactly that split. Rewriting "
                  "one convention into the other makes a working session unreadable, so this "
                  "check only speaks up when the index itself is channel-absolute and the two "
                  "still disagree.",
                  Severity::Warning, false},
                 [](const SegmentState& s, const SegmentTruth& t, Finding& f, bool& hit) {
                   if (!t.has_blocks) return;
                   // index[0].start_sample == 0 is the per-segment convention;
                   // section 2 being non-zero there is correct, not a defect.
                   if (t.start_sample == 0) return;
                   if (s.md.section2.start_sample == t.start_sample) return;
                   hit = true;
                   f.field = "start_sample";
                   f.stored = declared_si8(s.md.section2.start_sample);
                   f.expected = num(t.start_sample);
                   f.message = "metadata start sample disagrees with the first index entry "
                               "(reported only; the two conventions cannot be told apart "
                               "reliably enough to rewrite)";
                 },
                 {}});

    v.push_back({{"sizing.block-maxima", "Largest block is declared correctly",
                  "maximum_block_bytes and maximum_block_samples size a reader's per-block "
                  "buffers. Under-declaring either overflows them; 0 is not the NO_ENTRY "
                  "sentinel for either, so a reader cannot tell unset from measured.",
                  Severity::Error, true},
                 [](const SegmentState& s, const SegmentTruth& t, Finding& f, bool& hit) {
                   const auto& s2 = s.md.section2;
                   const bool bytes_bad = s2.maximum_block_bytes < t.max_block_bytes;
                   const bool samples_bad = s2.maximum_block_samples == fmt::UI4_NO_ENTRY ||
                                            s2.maximum_block_samples < t.max_block_samples;
                   if (!bytes_bad && !samples_bad) return;
                   hit = true;
                   f.field = bytes_bad ? "maximum_block_bytes" : "maximum_block_samples";
                   f.stored = bytes_bad ? declared_si8(s2.maximum_block_bytes)
                                        : declared_ui4(s2.maximum_block_samples);
                   f.expected = bytes_bad ? num(t.max_block_bytes) : num(t.max_block_samples);
                   f.message = "declared block maximum is smaller than a block on disk";
                 },
                 [](const SegmentTruth& t, RepairBuffer& r) {
                   r.s2.maximum_block_bytes = t.max_block_bytes;
                   r.s2.maximum_block_samples = t.max_block_samples;
                   r.tmet_dirty = true;
                   return true;
                 }});

    v.push_back({{"sizing.difference-bytes", "Difference buffer size is declared",
                  "maximum_difference_bytes sizes the RED difference buffer. meflib's "
                  "RED_allocate_processing_struct skips the allocation entirely for size 0, "
                  "leaving a NULL buffer that RED_decode then writes through — and 0 is not "
                  "this field's NO_ENTRY sentinel (0xFFFFFFFF), so a reader cannot tell it was "
                  "never set. mef3io <= 1.1.2 left it at 0.",
                  Severity::Error, true},
                 [](const SegmentState& s, const SegmentTruth& t, Finding& f, bool& hit) {
                   const ui4 stored = s.md.section2.maximum_difference_bytes;
                   const bool unset = stored == 0 || stored == fmt::UI4_NO_ENTRY;
                   // Without an exact measurement only a clearly-unset value can
                   // be judged; a real number is taken at its word.
                   if (!unset && (!t.difference_bytes_exact || stored >= t.max_difference_bytes))
                     return;
                   hit = true;
                   f.field = "maximum_difference_bytes";
                   f.stored = declared_ui4(stored);
                   f.expected = num(t.max_difference_bytes);
                   f.message = unset ? "difference buffer size is never set; a meflib-based "
                                       "reader allocates nothing and decodes into NULL"
                                     : "declared difference buffer is smaller than a block on disk";
                   // Say which kind of number the expectation is. Substituting a
                   // bound for a measurement without saying so would leave an
                   // operator believing the file was measured exactly.
                   if (!t.difference_bytes_exact)
                     f.message +=
                         t.difference_bytes_suspect
                             ? " (expected value is meflib's worst-case bound, not a "
                               "measurement: a block header was unreadable or implausible, so "
                               "the measured maximum would have been too small)"
                             : " (expected value is meflib's worst-case bound, not a "
                               "measurement)";
                 },
                 [](const SegmentTruth& t, RepairBuffer& r) {
                   // Never write 0 — that is the NULL-buffer value the check
                   // exists to remove. derive_truth guarantees a positive
                   // bound whenever the measurement is unusable, so a 0 here
                   // means the segment carries no blocks worth sizing for and
                   // the repair declines rather than installing the defect.
                   if (t.max_difference_bytes == 0) return false;
                   r.s2.maximum_difference_bytes = t.max_difference_bytes;
                   r.tmet_dirty = true;
                   return true;
                 }});

    v.push_back({{"sizing.contiguous", "Contiguous-run maxima match the index",
                  "The maximum_contiguous_* trio describes the longest run of blocks between "
                  "discontinuities. Severity depends on the direction, because the two are not "
                  "equally dangerous: UNDER-declaring is an ERROR — a reader that allocates a "
                  "run buffer from these fields truncates it, and 0 in "
                  "maximum_contiguous_block_bytes reads as a real zero, not as unset. "
                  "OVER-declaring is a warning: it only wastes memory, though recorders in the "
                  "field over-declare by two to three orders of magnitude and the waste scales "
                  "with channel count (mef3io <= 1.1.2 wrote whole-channel totals). Repaired in "
                  "both directions: the declaration states what the index holds. Do NOT downgrade "
                  "the under-declared case on the grounds that no reader in reference_files "
                  "consumes the trio — that is one meflib build, and a deployed build is known to "
                  "allocate from section-2 sizes that this one ignores.",
                  Severity::Warning, true},
                 [](const SegmentState& s, const SegmentTruth& t, Finding& f, bool& hit) {
                   const auto& s2 = s.md.section2;
                   struct Item {
                     const char* name;
                     si8 stored, expected;
                   };
                   const Item items[] = {
                       {"maximum_contiguous_blocks", s2.maximum_contiguous_blocks,
                        t.contiguous_blocks},
                       {"maximum_contiguous_block_bytes", s2.maximum_contiguous_block_bytes,
                        t.contiguous_block_bytes},
                       {"maximum_contiguous_samples", s2.maximum_contiguous_samples,
                        t.contiguous_samples},
                   };
                   for (const auto& it : items) {
                     if (it.stored == it.expected) continue;
                     hit = true;
                     f.field = it.name;
                     f.stored = declared_si8(it.stored);
                     f.expected = num(it.expected);
                     const bool under = it.stored < it.expected;
                     f.severity = under ? Severity::Error : Severity::Warning;
                     f.message =
                         under ? "declared contiguous maximum is smaller than a run on disk; a "
                                 "reader sizing a run buffer from it truncates"
                               : "declared contiguous maximum exceeds the longest run on disk "
                                 "(wasted allocation)";
                     // Prefer reporting an under-declaration: it is the one that
                     // truncates a reader's buffer, and it must not stay hidden
                     // behind a less urgent over-declaration earlier in the list.
                     if (under) return;
                   }
                 },
                 [](const SegmentTruth& t, RepairBuffer& r) {
                   // Write the measured truth, in whichever direction it lies.
                   // The field's job is to describe the data, and the longest
                   // run between discontinuity flags is what the index holds —
                   // the same quantity mef3io's own writer measures and the same
                   // one an independent third-party patcher writes, so a session
                   // repaired by either tool now agrees with the other.
                   // make_tuple, not tie: tie would alias the very fields the
                   // next lines mutate, so the comparison could never differ.
                   const auto snapshot = std::make_tuple(r.s2.maximum_contiguous_blocks,
                                                         r.s2.maximum_contiguous_block_bytes,
                                                         r.s2.maximum_contiguous_samples);
                   r.s2.maximum_contiguous_blocks = t.contiguous_blocks;
                   r.s2.maximum_contiguous_block_bytes = t.contiguous_block_bytes;
                   r.s2.maximum_contiguous_samples = t.contiguous_samples;
                   if (snapshot == std::make_tuple(r.s2.maximum_contiguous_blocks,
                                                   r.s2.maximum_contiguous_block_bytes,
                                                   r.s2.maximum_contiguous_samples))
                     return false;
                   r.tmet_dirty = true;
                   return true;
                 }});

    v.push_back({{"times.sampling-frequency", "Sampling frequency is a usable number",
                  "Every time expectation in this registry is derived by dividing a sample "
                  "count by sampling_frequency, and nothing else validates it. A value that is "
                  "not finite and positive — or one so small that samples/fs leaves si8 range — "
                  "makes those expectations meaningless, so the time checks stand down and "
                  "report nothing at all. This says so out loud instead. Not repairable: the "
                  "true rate is not recoverable from the file (the block start times imply one, "
                  "but a segment with a single block or a damaged index implies nothing), and "
                  "guessing it would rewrite the segment's whole time base.",
                  Severity::Error, false},
                 [](const SegmentState& s, const SegmentTruth& t, Finding& f, bool& hit) {
                   if (!t.has_blocks) return;
                   const sf8 fs = s.md.section2.sampling_frequency;
                   if (t.times_derivable) return;
                   hit = true;
                   f.field = "sampling_frequency";
                   f.stored = std::isfinite(fs) ? num(static_cast<si8>(fs)) : "not-a-number";
                   f.expected = "a finite positive rate";
                   f.message =
                       "sampling_frequency is unusable, so the segment's times cannot be "
                       "checked; every time-derived declaration is unverified";
                 },
                 {}});

    v.push_back({{"times.segment-bounds", "Universal-header times bracket the data",
                  "Every file of a segment carries the segment's start and end time. A reader "
                  "that seeks by time skips a segment whose declared range does not cover its "
                  "blocks. Compared as absolute uUTC — a stored time may be negated (meflib's "
                  "'offset applied' marker) or not, and both mean the same instant — with one "
                  "sample period of slack for per-block microsecond rounding. An ERROR, not "
                  "a cosmetic one: it fires exactly when the declared range fails to cover the "
                  "blocks, and a reader that seeks by time then returns NOTHING for a segment "
                  "whose samples are all present and intact — verified, a .tmet with corrupted "
                  "universal-header times reads back 0 samples.",
                  Severity::Error, true},
                 [](const SegmentState& s, const SegmentTruth& t, Finding& f, bool& hit) {
                   const sf8 fs_hz = s.md.section2.sampling_frequency;
                   if (!(fs_hz > 0.0)) return;  // times cannot be derived without fs
                   if (!t.has_blocks || !t.times_known || !t.times_comparable || !t.times_derivable)
                     return;
                   const si8 slack = static_cast<si8>(std::llround(1e6 / fs_hz)) + 1;
                   struct Item {
                     const char* name;
                     si8 stored, expected;
                   };
                   const Item items[] = {
                       {"metadata start_time", to_user_time(s.md.universal_header.start_time, t.rto),
                        t.first_start_uutc},
                       {"metadata end_time", to_user_time(s.md.universal_header.end_time, t.rto),
                        t.end_uutc},
                       {"index start_time", to_user_time(s.tidx_uh.start_time, t.rto),
                        t.first_start_uutc},
                       {"index end_time", to_user_time(s.tidx_uh.end_time, t.rto), t.end_uutc},
                       {"data start_time", to_user_time(s.tdat_uh_parsed.start_time, t.rto),
                        t.first_start_uutc},
                       {"data end_time", to_user_time(s.tdat_uh_parsed.end_time, t.rto), t.end_uutc},
                   };
                   for (const auto& it : items) {
                     if (it.stored != fmt::UUTC_NO_ENTRY &&
                         std::abs(it.stored - it.expected) <= slack)
                       continue;
                     hit = true;
                     f.field = it.name;
                     f.stored = declared_si8(it.stored);
                     f.expected = num(it.expected);
                     f.message = "universal-header time does not cover the blocks on disk "
                                 "(absolute uUTC)";
                     return;
                   }
                 },
                 [](const SegmentTruth& t, RepairBuffer& r) {
                   // Written back in meflib's negated form, as mef3io's writer does.
                   for (auto* uh : {&r.tmet_uh, &r.tidx_uh, &r.tdat_uh}) {
                     uh->start_time = to_disk_time(t.first_start_uutc, t.rto);
                     uh->end_time = to_disk_time(t.end_uutc, t.rto);
                   }
                   r.tmet_dirty = r.tidx_dirty = r.tdat_dirty = true;
                   return true;
                 }});

    v.push_back({{"times.recording-duration", "Recording duration spans the segment",
                  "recording_duration is the wall-clock span of the segment including gaps, in "
                  "microseconds — meflib computes ABS(latest_end) - ABS(earliest_start). The "
                  "legacy pymef writer instead stores number_of_samples / fs, which omits the "
                  "gaps and so reports short on any segment with a discontinuity. Compared with "
                  "one sample period of slack.",
                  Severity::Warning, true},
                 [](const SegmentState& s, const SegmentTruth& t, Finding& f, bool& hit) {
                   const sf8 fs_hz = s.md.section2.sampling_frequency;
                   if (!(fs_hz > 0.0)) return;
                   if (!t.has_blocks || !t.times_known || !t.times_comparable || !t.times_derivable)
                     return;
                   const si8 slack = static_cast<si8>(std::llround(1e6 / fs_hz)) + 1;
                   const si8 stored = s.md.section2.recording_duration;
                   // Both sentinels, not just SI8_NO_ENTRY: declared_si8 renders
                   // UUTC_NO_ENTRY here too because meflib writes it into this
                   // si8 field in the channel/session rollup, and INT64_MIN
                   // reaching the subtraction below is signed overflow.
                   if (stored != fmt::SI8_NO_ENTRY && stored != fmt::UUTC_NO_ENTRY &&
                       std::abs(stored - t.recording_duration) <= slack)
                     return;
                   hit = true;
                   f.field = "recording_duration";
                   f.stored = declared_si8(stored);
                   f.expected = num(t.recording_duration);
                   f.message = "declared recording duration does not match the segment's span";
                 },
                 [](const SegmentTruth& t, RepairBuffer& r) {
                   r.s2.recording_duration = t.recording_duration;
                   r.tmet_dirty = true;
                   return true;
                 }});

    v.push_back({{"times.block-interval", "Block interval is set",
                  "block_interval is the nominal microseconds covered by one full block "
                  "(maximum_block_samples / sampling_frequency). The legacy pymef writer leaves "
                  "it at 0. Only a clearly unset or badly wrong value is reported.",
                  Severity::Warning, true},
                 [](const SegmentState& s, const SegmentTruth& t, Finding& f, bool& hit) {
                   if (!t.times_derivable) return;  // sampling_frequency is unusable
                   if (t.block_interval <= 0) return;  // cannot derive an expectation
                   const si8 stored = s.md.section2.block_interval;
                   const bool unset = stored <= 0 || stored == fmt::SI8_NO_ENTRY;
                   // 1% tolerance: writers round the nominal interval differently.
                   const si8 tol = std::max<si8>(1, t.block_interval / 100);
                   if (!unset && std::abs(stored - t.block_interval) <= tol) return;
                   hit = true;
                   f.field = "block_interval";
                   f.stored = declared_si8(stored);
                   f.expected = num(t.block_interval);
                   f.message = unset ? "block interval is never set"
                                     : "declared block interval does not match the block geometry";
                 },
                 [](const SegmentTruth& t, RepairBuffer& r) {
                   r.s2.block_interval = t.block_interval;
                   r.tmet_dirty = true;
                   return true;
                 }});

    v.push_back({{"times.discontinuities", "Discontinuity count matches the index",
                  "number_of_discontinuities should equal the number of blocks flagged "
                  "discontinuous (a segment always begins with one). The legacy mef_tools "
                  "writer leaves it at 0 even when it wrote the flags. meflib's own "
                  "find_discontinuity_indices (meflib.c:3548) mallocs exactly this many "
                  "entries and then writes one per flagged block, so an under-declared count "
                  "is a straight heap overflow in any caller of find_discontinuity_samples. "
                  "Established by reading the C source, not by reproducing a crash — unlike "
                  "sizing.difference-bytes, which has both.",
                  Severity::Error, true},
                 [](const SegmentState& s, const SegmentTruth& t, Finding& f, bool& hit) {
                   const si8 stored = s.md.section2.number_of_discontinuities;
                   if (stored == t.n_discontinuities) return;
                   hit = true;
                   f.field = "number_of_discontinuities";
                   f.stored = declared_si8(stored);
                   f.expected = num(t.n_discontinuities);
                   f.message = "declared discontinuity count disagrees with the index flags";
                 },
                 [](const SegmentTruth& t, RepairBuffer& r) {
                   r.s2.number_of_discontinuities = t.n_discontinuities;
                   r.tmet_dirty = true;
                   return true;
                 }});

    v.push_back({{"header.entry-count", "Universal headers declare the right entry count",
                  "number_of_entries is how many records each file holds: 1 for .tmet, one per "
                  "block for .tidx and .tdat. Readers iterate on it. Severity depends on the "
                  "DIRECTION, because the two are not equally dangerous. UNDER-declaring is an "
                  "ERROR: meflib clamps number_of_blocks DOWN to this field (meflib.c:5983-5984 "
                  "for .tdat, :6005-6006 for .tidx), so the blocks past the count are dropped "
                  "and the segment reads SHORT with no error anywhere — the samples are intact "
                  "on disk and a reader silently returns fewer of them. Over-declaring is a "
                  "warning here (mef3io sizes from the file itself), though meflib iterates "
                  "number_of_entries index entries over an array it allocated from the file "
                  "length (meflib.c:4928-4933, :5776-5779), so it is not harmless there either.",
                  Severity::Warning, true},
                 [](const SegmentState& s, const SegmentTruth& t, Finding& f, bool& hit) {
                   struct Item {
                     const char* name;
                     si8 stored, expected;
                     bool clamps;  // meflib clamps number_of_blocks down to this one
                   };
                   const Item items[] = {
                       {"metadata number_of_entries", s.md.universal_header.number_of_entries, 1,
                        false},
                       {"index number_of_entries", s.tidx_uh.number_of_entries, t.n_blocks, true},
                       {"data number_of_entries", s.tdat_uh_parsed.number_of_entries, t.n_blocks,
                        true},
                   };
                   for (const auto& it : items) {
                     if (it.stored == it.expected) continue;
                     hit = true;
                     f.field = it.name;
                     f.stored = declared_si8(it.stored);
                     f.expected = num(it.expected);
                     const bool truncates = it.clamps && it.stored < it.expected;
                     if (truncates) {
                       f.severity = Severity::Error;
                       f.message =
                           "universal-header entry count is SMALLER than the file holds; meflib "
                           "clamps the block count down to it, so a reader returns a short "
                           "segment and the blocks past it are silently dropped";
                     } else {
                       f.message = "universal-header entry count disagrees with the file contents";
                     }
                     return;
                   }
                 },
                 [](const SegmentTruth& t, RepairBuffer& r) {
                   r.tmet_uh.number_of_entries = 1;
                   r.tidx_uh.number_of_entries = t.n_blocks;
                   r.tdat_uh.number_of_entries = t.n_blocks;
                   r.tmet_dirty = r.tidx_dirty = r.tdat_dirty = true;
                   return true;
                 }});

    v.push_back({{"header.max-entry-size", "Universal headers declare the right entry size",
                  "maximum_entry_size is the largest record in the file: the metadata record "
                  "(16384 B) and one index entry (56 B) are fixed by the format. The .tdat "
                  "value is INFORMATIONAL: meflib never writes it (only NO_ENTRY) and reads it "
                  "for record files alone, and the one reference writer stores a sample count "
                  "there — so 'largest RED block' is mef3io's reading, not a specification.",
                  Severity::Info, true},
                 [](const SegmentState& s, const SegmentTruth& t, Finding& f, bool& hit) {
                   struct Item {
                     const char* name;
                     si8 stored, expected;
                   };
                   const Item items[] = {
                       {"metadata maximum_entry_size", s.md.universal_header.maximum_entry_size,
                        fmt::METADATA_FILE_BYTES},
                       {"index maximum_entry_size", s.tidx_uh.maximum_entry_size,
                        fmt::TIME_SERIES_INDEX_BYTES},
                       {"data maximum_entry_size", s.tdat_uh_parsed.maximum_entry_size,
                        t.max_block_bytes},
                   };
                   for (const auto& it : items) {
                     if (it.stored == it.expected) continue;
                     hit = true;
                     f.field = it.name;
                     f.stored = declared_si8(it.stored);
                     f.expected = num(it.expected);
                     f.message = "universal-header entry size disagrees with the file contents";
                     return;
                   }
                 },
                 [](const SegmentTruth& t, RepairBuffer& r) {
                   r.tmet_uh.maximum_entry_size = fmt::METADATA_FILE_BYTES;
                   r.tidx_uh.maximum_entry_size = fmt::TIME_SERIES_INDEX_BYTES;
                   r.tdat_uh.maximum_entry_size = t.max_block_bytes;
                   r.tmet_dirty = r.tidx_dirty = r.tdat_dirty = true;
                   return true;
                 }});

    return v;
  }();
  return impls;
}

// --- discovery ---------------------------------------------------------------

std::vector<SegmentFiles> discover_segments(const SessionSource& src,
                                            const ValidateOptions& opts) {
  std::vector<SegmentFiles> out;
  for (const auto& entry : src.list_dir("")) {
    if (!entry.is_dir || !entry.name.ends_with(".timd")) continue;
    const std::string channel = entry.name.substr(0, entry.name.size() - 5);
    if (!selected(opts.channels, channel)) continue;

    for (const auto& seg : src.list_dir(entry.name)) {
      if (!seg.is_dir || !seg.name.ends_with(".segd")) continue;
      const std::string stem = seg.name.substr(0, seg.name.size() - 5);
      const std::string dir = entry.name + "/" + seg.name;
      SegmentFiles f;
      f.channel = channel;
      f.segment_number = segment_number_from_name(stem);
      if (!selected(opts.segments, f.segment_number)) continue;
      f.tmet_rel = dir + "/" + stem + ".tmet";
      f.tidx_rel = dir + "/" + stem + ".tidx";
      f.tdat_rel = dir + "/" + stem + ".tdat";
      f.description = src.describe(dir);
      // An incomplete segment is still a segment: a reader traversal skips it
      // silently, which is exactly the kind of thing a validator exists to
      // surface. Record what is missing and report it downstream rather than
      // dropping the segment here and calling the session clean.
      if (!src.exists(f.tmet_rel)) f.missing.push_back(".tmet");
      if (!src.exists(f.tidx_rel)) f.missing.push_back(".tidx");
      if (!src.exists(f.tdat_rel)) f.missing.push_back(".tdat");
      out.push_back(std::move(f));
    }
  }
  std::sort(out.begin(), out.end(), [](const SegmentFiles& a, const SegmentFiles& b) {
    return a.channel != b.channel ? a.channel < b.channel
                                  : a.segment_number < b.segment_number;
  });
  return out;
}

// --- deriving the truth ------------------------------------------------------

SegmentTruth derive_truth(const SessionSource& src, const SegmentState& s,
                          const ValidateOptions& opts) {
  SegmentTruth t;
  const auto& idx = s.tidx_bytes;
  const std::size_t from_size = idx.size() > fmt::UNIVERSAL_HEADER_BYTES
                                    ? (idx.size() - fmt::UNIVERSAL_HEADER_BYTES) /
                                          fmt::TIME_SERIES_INDEX_BYTES
                                    : 0;
  // The count of entries physically present, NOT the universal header's
  // declaration — that declaration is itself one of the things under test
  // (header.entry-count), so deriving the truth from it would make an
  // under-declared count undetectable. Integer division already ignores a
  // partial trailing entry, so padding shorter than one entry is harmless;
  // crc.index bounds its own hash window by the declared count instead.
  const std::size_t n = from_size;
  t.n_blocks = static_cast<si8>(n);
  if (n == 0) {
    // Nothing to derive from. `has_blocks` stays false so every check that
    // would otherwise compare against a fabricated zero stays silent — an
    // empty index is a damaged segment to report, never a truth to write back.
    return t;
  }
  t.has_blocks = true;

  // "Unknown" and "zero" are DIFFERENT states, and conflating them is a
  // data-loss bug. Section 3 holds the recording-time offset and is level-2
  // encrypted by default (meflib.h:405), so `section3_available` is false for
  // any encrypted session opened with a level-1 password — an ordinary, valid
  // way to open a file. Falling back to 0 there and then deriving
  // `times_comparable` from `rto == 0` would declare the times comparable
  // precisely when the offset cannot be seen, and a time repair would then
  // rewrite a correct session's bounds against the wrong baseline.
  t.rto_known = s.md.section3_available &&
                s.md.section3.recording_time_offset != fmt::UUTC_NO_ENTRY;
  t.rto = t.rto_known ? s.md.section3.recording_time_offset : 0;
  // With a non-zero recording-time offset, writers disagree on the stored sign
  // convention (meflib negates; the legacy stack stores a positive delta), and
  // a single file mixes both across its universal headers and its index. The
  // two are indistinguishable from the bytes alone, so the time checks stand
  // down rather than risk rewriting a correct file into an inverted range.
  // They stand down for an unknown offset too, for the same reason: the
  // baseline is unavailable, so nothing derived from it can be trusted.
  t.times_comparable = t.rto_known && t.rto == 0;

  si8 run_blocks = 0, run_bytes = 0, run_samples = 0;
  si8 last_start_uutc = 0;
  ui4 last_samples = 0;
  si8 previous_end_offset = fmt::UNIVERSAL_HEADER_BYTES;

  for (std::size_t i = 0; i < n; ++i) {
    auto e = fmt::TimeSeriesIndex::parse(std::span<const ui1>(idx).subspan(
        fmt::UNIVERSAL_HEADER_BYTES + i * fmt::TIME_SERIES_INDEX_BYTES,
        fmt::TIME_SERIES_INDEX_BYTES));
    // A damaged index may leave counts at NO_ENTRY; treat those as nothing
    // rather than letting 0xFFFFFFFF inflate every total derived here.
    const bool counts_known =
        e.number_of_samples != fmt::UI4_NO_ENTRY && e.block_bytes != fmt::UI4_NO_ENTRY;
    if (!counts_known) {
      t.index_counts_known = false;
      ++t.unknown_count_entries;
    }
    const ui4 samples = e.number_of_samples == fmt::UI4_NO_ENTRY ? 0 : e.number_of_samples;
    const ui4 block_bytes = e.block_bytes == fmt::UI4_NO_ENTRY ? 0 : e.block_bytes;
    const bool discontinuity =
        (e.red_block_flags & fmt::RedBlockHeader::DISCONTINUITY_MASK) != 0;

    // start_time is the one index field with a sentinel that would otherwise
    // reach arithmetic: INT64_MIN makes the later std::abs() difference UB.
    const bool time_known = e.start_time != fmt::UUTC_NO_ENTRY;
    if (!time_known) t.times_known = false;
    if (i == 0) {
      t.first_start_uutc = time_known ? to_user_time(e.start_time, t.rto) : 0;
      t.start_sample = e.start_sample == fmt::SI8_NO_ENTRY ? 0 : e.start_sample;
    }
    if (time_known) last_start_uutc = to_user_time(e.start_time, t.rto);
    last_samples = samples;

    t.total_samples += samples;
    t.data_bytes += block_bytes;
    t.max_block_bytes = std::max<si8>(t.max_block_bytes, block_bytes);
    t.max_block_samples = std::max(t.max_block_samples, samples);
    if (discontinuity) {
      ++t.n_discontinuities;
      run_blocks = run_bytes = run_samples = 0;
    }
    ++run_blocks;
    run_bytes += block_bytes;
    run_samples += samples;
    t.contiguous_blocks = std::max(t.contiguous_blocks, run_blocks);
    t.contiguous_block_bytes = std::max(t.contiguous_block_bytes, run_bytes);
    t.contiguous_samples = std::max(t.contiguous_samples, run_samples);

    if (t.offsets_sane) {
      if (e.file_offset < fmt::UNIVERSAL_HEADER_BYTES) {
        t.offsets_sane = false;
        t.offset_problem = "block " + num(static_cast<si8>(i)) + " starts at offset " +
                           num(e.file_offset) + ", inside the universal header";
      } else if (e.file_offset < previous_end_offset) {
        t.offsets_sane = false;
        t.offset_problem = "block " + num(static_cast<si8>(i)) + " starts at offset " +
                           num(e.file_offset) + ", before the end of the previous block";
      } else if (static_cast<std::uint64_t>(e.file_offset) + block_bytes > s.tdat_size) {
        t.offsets_sane = false;
        t.offset_problem = "block " + num(static_cast<si8>(i)) + " runs past the end of the " +
                           "data file (" + num(static_cast<si8>(s.tdat_size)) + " bytes)";
      }
      previous_end_offset = e.file_offset + block_bytes;
    }

    if (opts.exact_difference_bytes && t.offsets_sane) {
      // Every block must contribute, or the maximum is taken over a subset and
      // is therefore an UNDER-estimate — the direction that truncates a
      // reader's buffer. Each way a block can fail to contribute sets
      // `difference_bytes_suspect`, which downgrades the whole segment from
      // "measured" to "bounded" below. A block too short to hold a RED header
      // cannot be read at all, so it counts as a failure, not as a zero.
      if (block_bytes < fmt::RED_BLOCK_HEADER_BYTES) {
        t.difference_bytes_suspect = true;
      } else {
        auto head = src.read_range(
            s.files.tdat_rel,
            static_cast<std::size_t>(e.file_offset) + fmt::RedBlockHeader::DIFFERENCE_BYTES_OFFSET,
            sizeof(ui4));
        if (head.size() != sizeof(ui4)) {
          t.difference_bytes_suspect = true;
        } else {
          const ui4 measured = byteio::read<ui4>(head, 0);
          // .tdat carries no CRC check anywhere in the registry, so a corrupt
          // block header can present any value. Anything past meflib's own
          // worst case is not a measurement: writing it back would install the
          // very NO_ENTRY sentinel this check exists to remove, and the repair
          // would never converge.
          if (measured != fmt::UI4_NO_ENTRY && measured <= red_max_difference_bytes(samples))
            t.max_difference_bytes = std::max(t.max_difference_bytes, measured);
          else
            t.difference_bytes_suspect = true;
        }
      }
    }
  }

  // Does the index actually describe the whole data file? An index that has
  // lost entries — truncated, or rewritten by a tool that dropped some — stays
  // internally consistent: its offsets are monotonic, every block fits inside
  // .tdat, and its own CRC covers exactly what is left. Nothing else here
  // notices. But the blocks it no longer mentions are still on disk, and the
  // declarations derived from such an index are smaller than the truth, so
  // writing them back destroys the only remaining record of what .tdat holds.
  // `previous_end_offset` is the end of the last block the index accounts for.
  if (t.offsets_sane && static_cast<std::uint64_t>(previous_end_offset) < s.tdat_size) {
    t.index_covers_data = false;
    t.unaccounted_tail_bytes =
        static_cast<si8>(s.tdat_size) - previous_end_offset;
  }

  // A measurement counts as exact only when EVERY block contributed one. If
  // any block was skipped (`difference_bytes_suspect`) the maximum is taken
  // over the survivors and is too small; if none contributed it is 0 — which
  // is the exact NULL-buffer value this check exists to remove, and writing it
  // back would report the defect as repaired while leaving it on disk.
  //
  // The fallback is meflib's own worst case, 5 bytes per sample. It is a
  // bound, so it can over-declare by a few bytes per block; that costs a
  // reader a little memory, where under-declaring corrupts it. We only take
  // that trade when the exact answer is genuinely unavailable — the default
  // path measures, so a healthy segment is declared exactly.
  t.difference_bytes_exact = opts.exact_difference_bytes && t.offsets_sane &&
                             !t.difference_bytes_suspect && t.max_difference_bytes > 0;
  if (!t.difference_bytes_exact)
    t.max_difference_bytes = red_max_difference_bytes(t.max_block_samples);

  const sf8 fs_hz = s.md.section2.sampling_frequency;
  // Convert a sample count to microseconds, refusing anything that is not a
  // finite si8. `samples * 1e6 / fs` is the only place fs is trusted, and an
  // out-of-range double passed to std::llround is undefined behaviour.
  const auto to_us = [&](sf8 samples, si8& out) {
    const sf8 v = samples * 1e6 / fs_hz;
    if (!std::isfinite(v) || std::fabs(v) > 9.0e18) return false;
    out = static_cast<si8>(std::llround(v));
    return true;
  };
  si8 last_duration = 0, block_interval = 0;
  if (std::isfinite(fs_hz) && fs_hz > 0.0 && to_us(static_cast<sf8>(last_samples), last_duration)) {
    t.times_derivable = true;
    t.end_uutc = last_start_uutc + last_duration;
    // meflib defines recording_duration as the span of the segment including
    // gaps (meflib.c: ABS(latest_end) - ABS(earliest_start)). The legacy pymef
    // writer instead stores number_of_samples / fs, which omits the gaps.
    t.recording_duration = t.end_uutc - t.first_start_uutc;
    // NOMINAL, not "the largest block that happens to be present". A segment
    // whose blocks are all shorter than the writer's nominal block — a short
    // final segment, a brief recording, one closed early — would otherwise
    // yield an expectation well below the correct value, and the repair would
    // LOWER a correct declaration. Take the larger of what section 2 declares
    // and what is on disk. (meflib never computes this field at all: it inits
    // to NO_ENTRY at meflib.c:4570 and resets it at :5514-5515 / :6193-6194,
    // so the derivation is mef3io's inference either way.)
    const ui4 declared_block_samples = s.md.section2.maximum_block_samples;
    const ui4 nominal_samples =
        declared_block_samples == fmt::UI4_NO_ENTRY
            ? t.max_block_samples
            : std::max(declared_block_samples, t.max_block_samples);
    if (to_us(static_cast<sf8>(nominal_samples), block_interval)) t.block_interval = block_interval;
  }
  return t;
}

// --- writing repairs ---------------------------------------------------------

// Flush and close explicitly, checking each. A stream's destructor discards a
// failed flush, and most of a write sits in the buffer until then — so testing
// the stream straight after write() only tests that the buffer accepted the
// bytes, not that they reached the disk. An ENOSPC here must not be silent.
// Flush a file's contents all the way to stable storage. A rename is ORDERED,
// not durable: without this, a power cut can leave the rename visible and the
// data behind it missing — and a short .tmet throws from the metadata loader,
// which takes the whole session down, not just that segment.
[[nodiscard]] bool fsync_file(const std::string& path) {
#ifdef _WIN32
  HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  const bool ok = FlushFileBuffers(h) != 0;
  CloseHandle(h);
  return ok;
#else
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) return false;
  const bool ok = ::fsync(fd) == 0;
  return ::close(fd) == 0 && ok;
#endif
}

// Flush, and fail loudly if the flush did not happen. Used where the whole
// point of the call is durability: silently renaming an unflushed file over a
// good one would give back exactly the guarantee the caller was promised and
// did not get.
void fsync_file_or_throw(const std::string& path) {
  if (!fsync_file(path))
    throw IoError("could not flush to disk, the write is not durable: " + path);
}

// A rename is only durable once the DIRECTORY entry is flushed too. Windows
// exposes no directory handle to flush and does not need one.
void fsync_directory(const fsys::path& dir) {
#ifndef _WIN32
  const int fd = ::open(dir.string().c_str(), O_RDONLY);
  if (fd < 0) return;
  ::fsync(fd);
  ::close(fd);
#else
  (void)dir;
#endif
}

// Carry the target's permissions — and, where the platform has them, owner and
// group — onto the replacement. A fresh temp file is created under the process
// umask, so without this a repair silently widens access to a .tmet, which is
// the file holding metadata section 3: subject_name, subject_id, recording
// location. Run as root over a user-owned tree it would also change ownership,
// after which the original user's next acquisition write fails.
void copy_file_identity(const fsys::path& from, const fsys::path& to) {
  std::error_code ec;
  const auto st = fsys::status(from, ec);
  if (!ec) fsys::permissions(to, st.permissions(), fsys::perm_options::replace, ec);
#ifndef _WIN32
  struct stat s {};
  if (::stat(from.string().c_str(), &s) == 0) {
    // Best effort: an unprivileged process cannot chown, and that is not a
    // reason to fail a repair it has already computed.
    if (::chown(to.string().c_str(), s.st_uid, s.st_gid) != 0) { /* ignored */ }
  }
#endif
}

void finish_stream(std::ofstream& f, const std::string& path) {
  f.flush();
  if (!f) throw IoError("write failed (disk full?): " + path);
  f.close();
  if (!f) throw IoError("close failed, data may not have reached disk: " + path);
}

void replace_file(const fsys::path& tmp, const fsys::path& target) {
#ifdef _WIN32
  if (MoveFileExW(tmp.wstring().c_str(), target.wstring().c_str(),
                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    return;
  const std::error_code ec(static_cast<int>(GetLastError()), std::system_category());
  throw IoError("cannot replace " + target.string() + ": " + ec.message());
#else
  std::error_code ec;
  fsys::rename(tmp, target, ec);
  if (!ec) return;
  throw IoError("cannot replace " + target.string() + ": " + ec.message());
#endif
}

// Replace a file's contents atomically: fill a sibling temp file, flush it,
// then rename over the target. A crash or a full disk leaves the original
// untouched instead of a truncated one — which matters because the file being
// replaced is the only record of what the (much larger) .tdat contains.
void write_all_atomic(const std::string& path, std::span<const ui1> bytes) {
  const fsys::path target(path);
  const fsys::path tmp =
      target.parent_path() / (target.filename().string() + ".mef3io-repair-tmp");
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) throw IoError("cannot open for write: " + tmp.string());
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    if (!f) throw IoError("write failed: " + tmp.string());
    finish_stream(f, tmp.string());
  }
  // The target still exists here, so its mode/owner can be carried across
  // before it is replaced. Then flush the data before the rename, and the
  // directory entry after it.
  copy_file_identity(target, tmp);
  // Flush before the rename, and fail loudly if the flush did not happen:
  // renaming an unflushed file over a good one hands back a durability
  // guarantee the caller did not get. replace_file is the portable swap
  // (MoveFileEx + WRITE_THROUGH on Windows).
  fsync_file_or_throw(tmp.string());
  try {
    replace_file(tmp, target);
  } catch (...) {
    std::error_code ignored;
    fsys::remove(tmp, ignored);
    throw;
  }
  fsync_directory(target.parent_path());
}

// Patch the first 1024 bytes of a file in place. Deliberately NOT atomic: the
// alternative is rewriting a multi-gigabyte .tdat to change its header, and
// the body is untouched, so the stored body CRC stays valid — only the header
// CRC (over bytes [4, 1024)) is recomputed.
void overwrite_universal_header(const std::string& path, const fmt::UniversalHeader& uh) {
  std::vector<ui1> head(fmt::UNIVERSAL_HEADER_BYTES);
  {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw IoError("cannot open for read: " + path);
    if (!in.read(reinterpret_cast<char*>(head.data()), fmt::UNIVERSAL_HEADER_BYTES))
      throw IoError("short read: " + path);
  }
  fmt::UniversalHeader patched = uh;
  patched.body_crc = byteio::read<ui4>(head, 4);
  patched.serialize(head);
  patched.update_header_crc(head);
  std::fstream out(path, std::ios::binary | std::ios::in | std::ios::out);
  if (!out) throw IoError("cannot open for header update: " + path);
  out.write(reinterpret_cast<const char*>(head.data()), fmt::UNIVERSAL_HEADER_BYTES);
  out.flush();
  if (!out) throw IoError("header update failed: " + path);
  out.close();
  if (!out) throw IoError("close failed, header may not have reached disk: " + path);
  (void)fsync_file(path);  // best effort: body unchanged, header is backed up
}

// `<session>.repair-backup`, with any trailing separator stripped first —
// otherwise "s.mefd/" yields "s.mefd/.repair-backup", i.e. a backup INSIDE the
// session, which later gets packed into an archive or deleted with it.
fsys::path backup_root_for(const std::string& session_path) {
  std::string s = session_path;
  while (s.size() > 1 && (s.back() == '/' || s.back() == '\\')) s.pop_back();
  return fsys::path(s + ".repair-backup");
}

// Copy `file` into the backup tree before it is modified. `limit_bytes` copies
// only a prefix (the universal header of a .tdat, which is all a header patch
// can damage — copying a whole multi-gigabyte .tdat to protect 1024 bytes
// would fill the volume).
//
// Written to a ".part" file and renamed, so an interrupted copy can never be
// mistaken for a pristine backup by the next run; a completed backup is never
// overwritten.
void back_up(const std::string& file, const fsys::path& backup_root, const std::string& rel,
             std::uintmax_t limit_bytes = 0) {
  const fsys::path dest = backup_root / fsys::path(rel);
  if (fsys::exists(dest)) return;
  fsys::create_directories(dest.parent_path());
  const fsys::path part = dest.string() + ".part";

  std::ifstream in(file, std::ios::binary);
  if (!in) throw IoError("cannot open for backup: " + file);
  const std::uintmax_t total = limit_bytes ? limit_bytes : fsys::file_size(file);
  std::vector<ui1> buf(static_cast<std::size_t>(std::min<std::uintmax_t>(total, 1u << 20)));
  {
    std::ofstream out(part, std::ios::binary | std::ios::trunc);
    if (!out) throw IoError("cannot open backup for write: " + part.string());
    std::uintmax_t left = total;
    while (left > 0) {
      const std::streamsize chunk =
          static_cast<std::streamsize>(std::min<std::uintmax_t>(left, buf.size()));
      if (!in.read(reinterpret_cast<char*>(buf.data()), chunk))
        throw IoError("short read while backing up: " + file);
      out.write(reinterpret_cast<const char*>(buf.data()), chunk);
      if (!out) throw IoError("backup write failed (disk full?): " + part.string());
      left -= static_cast<std::uintmax_t>(chunk);
    }
    finish_stream(out, part.string());
  }
  // The backup exists to survive exactly the crash that fsync guards against;
  // an unflushed one can come back empty from the same power cut.
  fsync_file_or_throw(part.string());
  std::error_code ec;
  fsys::rename(part, dest, ec);
  if (ec) {
    std::error_code ignored;
    fsys::remove(part, ignored);
    throw IoError("cannot finalize backup " + dest.string() + ": " + ec.message());
  }
  fsync_directory(dest.parent_path());
}

}  // namespace

// --- public surface ----------------------------------------------------------

std::string severity_name(Severity s) {
  switch (s) {
    case Severity::Info: return "info";
    case Severity::Warning: return "warning";
    case Severity::Error: return "error";
  }
  return "unknown";
}

std::vector<std::string> unset_declarations(const fmt::TimeSeriesMetadataSection2& s2) {
  std::vector<std::string> out;
  auto check_si8 = [&](const char* name, si8 v) {
    if (v <= 0) out.emplace_back(name);  // covers both 0 and the -1 sentinel
  };
  auto check_ui4 = [&](const char* name, ui4 v) {
    if (v == 0 || v == fmt::UI4_NO_ENTRY) out.emplace_back(name);
  };
  check_si8("maximum_block_bytes", s2.maximum_block_bytes);
  check_ui4("maximum_block_samples", s2.maximum_block_samples);
  check_ui4("maximum_difference_bytes", s2.maximum_difference_bytes);
  check_si8("maximum_contiguous_blocks", s2.maximum_contiguous_blocks);
  check_si8("maximum_contiguous_block_bytes", s2.maximum_contiguous_block_bytes);
  check_si8("maximum_contiguous_samples", s2.maximum_contiguous_samples);
  return out;
}

const std::vector<CheckInfo>& checks() {
  static const std::vector<CheckInfo> infos = [] {
    std::vector<CheckInfo> v;
    for (const auto& impl : check_impls()) v.push_back(impl.info);
    return v;
  }();
  return infos;
}

const CheckInfo* find_check(const std::string& id) {
  for (const auto& c : checks())
    if (c.id == id) return &c;
  return nullptr;
}

bool Report::ok() const {
  // A run that examined nothing is not a clean run. Without this, a typo in a
  // channel filter yields zero findings and zero skips, and the session is
  // reported healthy without a single byte having been read.
  if (segments_checked == 0) return false;
  for (const auto& f : findings)
    if (f.severity == Severity::Error && !f.repaired) return false;
  return skipped.empty();
}

si8 Report::count(Severity s) const {
  si8 n = 0;
  for (const auto& f : findings)
    if (f.severity == s) ++n;
  return n;
}

std::vector<std::string> Report::repairable_check_ids() const {
  std::vector<std::string> out;
  for (const auto& f : findings) {
    if (!f.repairable) continue;
    if (std::find(out.begin(), out.end(), f.check_id) == out.end()) out.push_back(f.check_id);
  }
  return out;
}

namespace {

// The shared body of validate/repair. `repair` empty => read-only.
Report run(const std::string& path, const ValidateOptions& opts, const RepairSelection* repair) {
  auto src = open_session_source(path);
  Report report;

  for (const auto& id : opts.check_ids)
    if (!find_check(id)) throw std::invalid_argument("unknown check id: " + id);

  std::vector<const CheckImpl*> active;
  for (const auto& impl : check_impls())
    if (selected(opts.check_ids, impl.info.id)) {
      active.push_back(&impl);
      report.checks_run.push_back(impl.info.id);
    }

  std::set<std::string> to_repair;
  if (repair) {
    for (const auto& id : repair->check_ids) {
      const CheckInfo* info = find_check(id);
      if (!info) throw std::invalid_argument("unknown check id: " + id);
      if (!info->repairable)
        throw std::invalid_argument("check is not repairable: " + id);
      to_repair.insert(id);
    }
    report.checks_repaired.assign(repair->check_ids.begin(), repair->check_ids.end());
  }

  for (const auto& files : discover_segments(*src, opts)) {
    auto skip = [&](const std::string& reason) {
      report.skipped.push_back({files.channel, files.segment_number, files.description, reason});
    };

    // A finding is marked `repaired` only once the segment's writes have all
    // succeeded, so these are staged out here where the catch below can still
    // reach them. `files_written` records what was already replaced when a
    // later write throws — a segment whose three files disagree is recoverable
    // only if the operator is told which ones moved.
    std::vector<Finding> staged;
    std::vector<std::size_t> pending_repair;  // indices into `staged`
    std::vector<std::string> files_written;

    // One bad segment must not cost the caller the record of what was already
    // rewritten in the ones before it. On a 254-channel session an exception
    // escaping here would discard the whole report, leaving no way to tell
    // which files had been modified.
    try {

    if (!files.missing.empty()) {
      std::string list;
      for (const auto& name : files.missing) list += (list.empty() ? "" : ", ") + name;
      skip("segment is incomplete: missing " + list);
      continue;
    }

    SegmentState state;
    state.files = files;
    try {
      state.tmet_bytes = src->read_all(files.tmet_rel);
      state.tidx_bytes = src->read_all(files.tidx_rel);
      state.tdat_size = src->file_size(files.tdat_rel);
      state.tdat_uh = src->read_range(files.tdat_rel, 0, fmt::UNIVERSAL_HEADER_BYTES);
    } catch (const MefError& e) {
      skip(std::string("cannot read segment files: ") + e.what());
      continue;
    }
    if (state.tmet_bytes.size() < fmt::METADATA_FILE_BYTES ||
        state.tidx_bytes.size() < fmt::UNIVERSAL_HEADER_BYTES ||
        state.tdat_uh.size() < fmt::UNIVERSAL_HEADER_BYTES) {
      skip("segment file is shorter than its fixed-size header");
      continue;
    }

    // CRC checks read the raw bytes, so they run before the metadata loader
    // (which throws on a bad CRC) and can report instead of aborting.
    std::vector<Finding> segment_findings;
    bool integrity_failed = false;
    // Deliberately iterates the whole registry, not `active`: narrowing the
    // run with check_ids must never disable the gate that stops a repair from
    // deriving truth out of bytes that failed their CRC.
    for (const auto& impl_ref : check_impls()) {
      const CheckImpl* impl = &impl_ref;
      if (impl->info.id != "crc.metadata" && impl->info.id != "crc.index") continue;
      Finding f;
      bool hit = false;
      // Seeded before detect, as in the main loop, so an integrity check can
      // lower a particular finding's severity.
      f.severity = impl->info.severity;
      impl->detect(state, SegmentTruth{}, f, hit);
      if (!hit) continue;
      f.check_id = impl->info.id;
      f.repairable = impl->info.repairable;
      f.channel = files.channel;
      f.segment_number = files.segment_number;
      f.path = files.description;
      // Only a genuine MISMATCH disarms the rest of the segment. These checks
      // also report a CRC the writer never computed, which is a warning: the
      // bytes are unverifiable, not known-bad, and treating that as a failure
      // would strip every other check from a legitimate streaming writer's
      // file — including the difference-bytes error this registry exists for.
      if (f.severity == Severity::Error) integrity_failed = true;
      segment_findings.push_back(std::move(f));
    }
    if (integrity_failed) {
      report.findings.insert(report.findings.end(), segment_findings.begin(),
                             segment_findings.end());
      ++report.segments_checked;
      skip("CRC mismatch; remaining checks skipped and nothing repaired");
      continue;
    }
    // Unverifiable-CRC warnings still belong in the report; the segment simply
    // carries on being checked.
    report.findings.insert(report.findings.end(), segment_findings.begin(),
                           segment_findings.end());

    try {
      state.md = load_time_series_metadata(state.tmet_bytes, opts.password);
      state.tidx_uh = fmt::UniversalHeader::parse(state.tidx_bytes);
      state.tdat_uh_parsed = fmt::UniversalHeader::parse(state.tdat_uh);
    } catch (const MefError& e) {
      skip(std::string("cannot read metadata: ") + e.what());
      continue;
    }
    if (state.md.section1.section_2_encryption > 0 &&
        state.md.access_level < fmt::LEVEL_1_ACCESS) {
      skip("section 2 is encrypted and the password does not grant level-1 access");
      continue;
    }

    const SegmentTruth truth = derive_truth(*src, state, opts);
    if (!truth.has_blocks) {
      ++report.segments_checked;
      skip(state.tdat_size > fmt::UNIVERSAL_HEADER_BYTES
               ? "block index is empty but the data file holds " +
                     num(static_cast<si8>(state.tdat_size - fmt::UNIVERSAL_HEADER_BYTES)) +
                     " bytes of blocks; segment is damaged and will not be repaired"
               : "segment contains no blocks");
      continue;
    }
    ++report.segments_checked;

    RepairBuffer buffer;
    buffer.s2 = state.md.section2;
    buffer.tmet_uh = state.md.universal_header;
    buffer.tidx_uh = state.tidx_uh;
    buffer.tdat_uh = state.tdat_uh_parsed;
    bool any_repair = false;

    // Everything a repair writes is derived from the block index, so the index
    // has to be trustworthy before any of it may be written back. A CRC failure
    // already stops the segment earlier; these two are the cases a CRC cannot
    // see, because the damaged index is internally consistent:
    //
    //   * offsets that overlap, run backwards, or point outside .tdat — the
    //     index describes a file that is not the one on disk;
    //   * an index that stops short of the end of .tdat — entries are missing,
    //     so every derived declaration is too small.
    //
    // Both were reported before this gate existed and then repaired FROM
    // ANYWAY, which turned a readable session into an unreadable one. Checks
    // still all run, so the report stays complete; only the writing stops.
    const bool index_trustworthy =
        truth.offsets_sane && truth.index_covers_data && truth.index_counts_known;

    for (const auto* impl : active) {
      if (impl->info.id == "crc.metadata" || impl->info.id == "crc.index") continue;
      Finding f;
      bool hit = false;
      // Seeded before detect so a check can RAISE the severity of a particular
      // finding. Some defects are only dangerous in one direction:
      // under-declaring a buffer size can truncate a reader that allocates from
      // it, while over-declaring the same field only wastes memory. The
      // registry entry carries the ordinary case; detect sharpens it.
      f.severity = impl->info.severity;
      impl->detect(state, truth, f, hit);
      if (!hit) continue;
      f.check_id = impl->info.id;
      f.repairable = impl->info.repairable;
      f.channel = files.channel;
      f.segment_number = files.segment_number;
      f.path = files.description;
      if (repair && impl->repair && index_trustworthy && to_repair.count(impl->info.id) &&
          selected(repair->channels, files.channel) &&
          selected(repair->segments, files.segment_number)) {
        // Only what the repair actually wrote counts, and at this point
        // NOTHING has been written — `repair` has mutated an in-memory buffer
        // and no more. A repair may also decline outright. So the finding is
        // only remembered as pending here; `repaired` is set after the
        // segment's writes succeed. Marking it now would report a defect as
        // fixed when the write later fails, and for an error-severity finding
        // would let Report::ok discount something still on disk.
        const bool changed = impl->repair(truth, buffer);
        if (changed) {
          pending_repair.push_back(staged.size());
          any_repair = true;
        }
      }
      staged.push_back(std::move(f));
    }

    // Say so out loud. A caller that asked for repairs and silently got none
    // would reasonably read the report as "nothing needed fixing".
    if (repair && !index_trustworthy && selected(repair->channels, files.channel) &&
        selected(repair->segments, files.segment_number))
      skip(!truth.offsets_sane
               ? "the block index is structurally unsound; nothing was repaired in this segment"
               : !truth.index_counts_known
                     ? "the block index leaves entry sizes unset; nothing was repaired in this "
                       "segment"
                     : "the block index does not describe the whole data file; nothing was "
                       "repaired in this segment");

    if (!any_repair) {
      report.findings.insert(report.findings.end(), staged.begin(), staged.end());
      continue;
    }

    // Write back. Only declarations move: section 2 and the universal headers.
    const std::string tmet_path = src->describe(files.tmet_rel);
    const std::string tidx_path = src->describe(files.tidx_rel);
    const std::string tdat_path = src->describe(files.tdat_rel);
    if (repair->backup) {
      const fsys::path backup_root = backup_root_for(path);
      if (buffer.tmet_dirty) back_up(tmet_path, backup_root, files.tmet_rel);
      if (buffer.tidx_dirty) back_up(tidx_path, backup_root, files.tidx_rel);
      // Only the universal header of a .tdat is ever rewritten, so only that
      // needs preserving; copying the whole data file would be a multi-gigabyte
      // write to protect 1024 bytes.
      if (buffer.tdat_dirty)
        back_up(tdat_path, backup_root, files.tdat_rel + ".universal-header",
                fmt::UNIVERSAL_HEADER_BYTES);
    }

    if (buffer.tmet_dirty) {
      std::vector<ui1> file = state.tmet_bytes;
      std::span<ui1> s2_image(file.data() + fmt::METADATA_SECTION_2_OFFSET,
                              fmt::TIME_SERIES_METADATA_SECTION_2_BYTES);
      // Section 1 says which key section 2 is under — meflib allows either
      // level, and assuming level 1 re-encrypts a level-2 section with the
      // wrong key, leaving CRC-valid garbage no reader can open.
      const si1 s2_enc = state.md.section1.section_2_encryption;
      std::optional<std::array<ui1, fmt::PASSWORD_BYTES>> s2_key;
      if (s2_enc > 0) {
        auto keys = crypto::validate_password(
            opts.password, state.md.universal_header.level_1_password_validation_field,
            state.md.universal_header.level_2_password_validation_field);
        s2_key = (s2_enc == fmt::LEVEL_2_ENCRYPTION) ? keys.level2_key : keys.level1_key;
        if (!s2_key)
          throw PasswordError("section 2 is encrypted at level " + std::to_string(s2_enc) +
                              "; the supplied password does not yield that key: " + tmet_path);
        auto plain = crypto::aes128_ecb_decrypt(s2_image, *s2_key);
        std::copy(plain.begin(), plain.end(), s2_image.begin());
      }
      // Patch only the fields a repair can change, in place. Re-serializing the
      // whole section would zero the protected and discretionary regions (4320
      // bytes this struct does not model) and could shorten an un-terminated
      // text field.
      buffer.s2.serialize_derived_fields(s2_image);
      if (s2_enc > 0) {
        auto enc = crypto::aes128_ecb_encrypt(s2_image, *s2_key);
        std::copy(enc.begin(), enc.end(), s2_image.begin());
      }
      buffer.tmet_uh.serialize(file);
      const ui4 body_crc = crc::calculate(std::span<const ui1>(file).subspan(
          fmt::UNIVERSAL_HEADER_BYTES,
          fmt::METADATA_FILE_BYTES - fmt::UNIVERSAL_HEADER_BYTES));
      byteio::write<ui4>(file, 4, body_crc);
      const ui4 header_crc = crc::calculate(
          std::span<const ui1>(file).subspan(4, fmt::UNIVERSAL_HEADER_BYTES - 4));
      byteio::write<ui4>(file, 0, header_crc);
      write_all_atomic(tmet_path, file);
      files_written.push_back(files.tmet_rel);
    }
    if (buffer.tidx_dirty) {
      overwrite_universal_header(tidx_path, buffer.tidx_uh);
      files_written.push_back(files.tidx_rel);
    }
    if (buffer.tdat_dirty) {
      overwrite_universal_header(tdat_path, buffer.tdat_uh);
      files_written.push_back(files.tdat_rel);
    }
    // Everything reached disk: only now is a finding a repair.
    for (const auto i : pending_repair) staged[i].repaired = true;
    ++report.segments_repaired;
    report.findings.insert(report.findings.end(), staged.begin(), staged.end());
    } catch (const std::exception& e) {
      // The findings still belong in the report — losing them would hide what
      // was wrong with the segment as well as failing to fix it. None is marked
      // repaired: whatever was staged did not all reach disk.
      report.findings.insert(report.findings.end(), staged.begin(), staged.end());
      std::string reason = std::string("aborted: ") + e.what();
      if (!files_written.empty()) {
        std::string list;
        for (const auto& f : files_written) list += (list.empty() ? "" : ", ") + f;
        reason += " — ALREADY MODIFIED before the failure: " + list +
                  " (this segment's files no longer agree)";
        // Only name a backup that exists. Sending an operator to a directory
        // that was never created, mid-incident, is worse than saying nothing.
        if (repair->backup)
          reason += "; restore from " + backup_root_for(path).string();
        else
          reason += "; no backup was taken (backup=false)";
      }
      skip(reason);
    }
  }

  return report;
}

}  // namespace

Report validate_session(const std::string& path, const ValidateOptions& opts) {
  return run(path, opts, nullptr);
}

Report repair_session(const std::string& path, const RepairSelection& selection,
                      const ValidateOptions& opts) {
  if (selection.check_ids.empty())
    throw std::invalid_argument(
        "repair_session: no checks selected. Repairs are never implicit — pass the check ids to "
        "fix (Report::repairable_check_ids lists the candidates).");
  if (path_has_suffix(path, ".tar"))
    throw IoError("cannot repair a tar session archive in place: " + path +
                  " (extract it first with extract_session)");
  return run(path, opts, &selection);
}

}  // namespace mef3io
