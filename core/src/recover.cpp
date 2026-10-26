// mef3io — crash recovery. See recover.hpp for what this is and is not.
#include "operation_marker.hpp"
#include "mef3io/recover.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <optional>
#include <limits>
#include <random>
#include <map>
#include <set>

#include "durability.hpp"
#include "last_segment.hpp"
#include "mef3io/byteio.hpp"
#include "mef3io/crc.hpp"
#include "mef3io/crypto.hpp"
#include "mef3io/errors.hpp"
#include "mef3io/headers.hpp"
#include "mef3io/metadata.hpp"
#include "mef3io/records.hpp"
#include "mef3io/source.hpp"
#include "mef3io/time.hpp"
#include "mef3io/validate.hpp"

namespace fsys = std::filesystem;

namespace mef3io {
namespace {

/// Read one byte range of a file. `.tdat` runs to tens of gigabytes, so it is
/// NEVER read whole: recovery needs its SIZE, plus a 304-byte RED header (and,
/// to verify it, the one block that header describes) at a handful of offsets.
std::vector<ui1> read_range(std::ifstream& f, std::uint64_t offset, std::size_t count) {
  std::vector<ui1> buf(count);
  f.clear();
  f.seekg(static_cast<std::streamoff>(offset));
  if (!f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(count)))
    buf.resize(static_cast<std::size_t>(f.gcount()));
  return buf;
}

std::vector<ui1> read_all(const fsys::path& p) {
  std::ifstream f(p, std::ios::binary | std::ios::ate);
  if (!f) throw IoError("cannot open file: " + p.string());
  const auto size = f.tellg();
  f.seekg(0);
  std::vector<ui1> buf(static_cast<std::size_t>(size));
  if (size && !f.read(reinterpret_cast<char*>(buf.data()), size))
    throw IoError("short read: " + p.string());
  return buf;
}

void write_all(const fsys::path& target, std::span<const ui1> bytes) {
  const fsys::path tmp = target.parent_path() / (target.filename().string() + ".mef3io-rec-tmp");
  std::error_code ignored;
  fsys::remove(tmp, ignored);
  try {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) throw IoError("cannot open for write: " + tmp.string());
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    f.flush();
    if (!f) throw IoError("write failed: " + tmp.string());
    f.close();
    detail::copy_file_identity(target, tmp);
    // Recovery runs once, on a session that is already damaged. Pay for the
    // barrier: a crash DURING recovery must not compound the problem.
    detail::fsync_file_or_throw(tmp.string());
    detail::replace_file(tmp, target);
  } catch (...) {
    fsys::remove(tmp, ignored);
    throw;
  }
  detail::fsync_directory(target.parent_path());
}

void back_up_file(const fsys::path& file, const fsys::path& root, const std::string& rel) {
  const fsys::path dest = root / rel;
  std::error_code ec;
  fsys::create_directories(dest.parent_path(), ec);
  if (fsys::exists(dest)) return;  // never overwrite a pristine backup
  fsys::copy_file(file, dest, fsys::copy_options::overwrite_existing, ec);
  if (ec) throw IoError("cannot back up " + file.string() + ": " + ec.message());
  (void)detail::fsync_file(dest.string());
}

void back_up_bytes(const fsys::path& root, const std::string& rel, std::span<const ui1> bytes) {
  const fsys::path dest = root / rel;
  std::error_code ec;
  fsys::create_directories(dest.parent_path(), ec);
  if (fsys::exists(dest)) return;  // never overwrite a pristine backup
  std::ofstream f(dest, std::ios::binary | std::ios::trunc);
  if (!f) throw IoError("cannot back up to " + dest.string());
  f.write(reinterpret_cast<const char*>(bytes.data()),
          static_cast<std::streamsize>(bytes.size()));
  f.flush();
  if (!f) throw IoError("backup write failed: " + dest.string());
  f.close();
  (void)detail::fsync_file(dest.string());
}

/// Is there a plausible, CRC-valid RED block at `offset`?
///
/// Reads the 304-byte header, then exactly the one block it describes — never
/// the whole file. A block whose CRC does not verify is a torn write, not a
/// block, and must not be indexed as if it held samples.
bool block_looks_real(std::ifstream& f, std::uint64_t size, std::uint64_t offset,
                      fmt::RedBlockHeader& out) {
  if (offset + fmt::RED_BLOCK_HEADER_BYTES > size) return false;
  const auto head = read_range(f, offset, fmt::RED_BLOCK_HEADER_BYTES);
  if (head.size() < fmt::RED_BLOCK_HEADER_BYTES) return false;
  out = fmt::RedBlockHeader::parse(head);
  if (out.block_bytes < fmt::RED_BLOCK_HEADER_BYTES) return false;
  if (out.block_bytes == fmt::UI4_NO_ENTRY) return false;
  if (offset + out.block_bytes > size) return false;   // truncated tail
  if (out.number_of_samples == 0 || out.number_of_samples == fmt::UI4_NO_ENTRY) return false;
  // Each byte read ONCE: the CRC runs over the header already in hand, then
  // over the rest of the block (the Koopman CRC is a rolling state).
  const ui4 head_crc = crc::calculate(std::span<const ui1>(head).subspan(4));
  const auto rest = read_range(f, offset + fmt::RED_BLOCK_HEADER_BYTES,
                               static_cast<std::size_t>(out.block_bytes) -
                                   fmt::RED_BLOCK_HEADER_BYTES);
  if (rest.size() + fmt::RED_BLOCK_HEADER_BYTES != out.block_bytes) return false;
  return out.crc == crc::calculate(rest, head_crc);
}

/// What recovery does about a segment's END TIME (#83). Index 0/1/2 =
/// .tmet/.tidx/.tdat universal header; a set value is the new stored end.
struct EndPlan {
  bool rto_nonzero = false;  ///< the offset is known and non-zero
  std::array<std::optional<si8>, 3> new_end;
  int stale() const {
    return static_cast<int>(std::count_if(new_end.begin(), new_end.end(),
                                          [](const auto& v) { return v.has_value(); }));
  }
};

/// Decide the end times at a NON-ZERO recording-time offset. At offset 0 the
/// validator's times.segment-bounds repair rewrites the bounds after recovery
/// (the CLI runs it), so recovery keeps to the index and the data there. At a
/// non-zero offset every time check stands down (writers disagree on the
/// sign convention), which left `info()`'s end time — the .tmet universal
/// header's — describing the data from BEFORE the recovery.
///
/// Rules (review of #145):
///  * compared in USER time (to_user_time with the segment's offset);
///  * a header's end is stale only if it lies before the last kept block's
///    START plus half a sample — no end convention (exclusive, inclusive,
///    truncated, rounded) puts it there — or, when recovery REBUILT blocks,
///    before the computed end minus half a sample;
///  * an end is never moved earlier (the new value is always later);
///  * each header keeps its OWN convention: only an end stored in the
///    offset-applied (negated) form is rewritten (an unset one if its start
///    is negated); a plain one is left alone with a note, since at a
///    non-zero offset a positive value is ambiguous;
///  * the last block's stored time must be negated too, else nothing.
/// `note` is set when something stale is left alone.
EndPlan plan_end_times(const TimeSeriesMetadata& md, const std::array<const ui1*, 3>& uh,
                       si8 last_start_stored, ui4 last_samples, bool rebuilt,
                       std::string& note) {
  EndPlan out;
  const si8 rto = segment_rto(md);
  if (rto == 0) return out;  // repair_session's job (see above)
  out.rto_nonzero = true;
  const sf8 fs_hz = md.section2.sampling_frequency;
  const sf8 duration = static_cast<sf8>(last_samples) * 1e6 / fs_hz;
  if (!std::isfinite(fs_hz) || !(fs_hz > 0.0) || !std::isfinite(duration) || duration > 9e18) {
    note = "its sampling frequency is not a usable number";
    return out;
  }
  if (last_start_stored >= 0 || last_start_stored == fmt::UUTC_NO_ENTRY) {
    note = "the last block's time is not stored in the offset-applied (negated) form, so "
           "the end cannot be placed without guessing the convention";
    return out;
  }
  const si8 half = static_cast<si8>(std::llround(std::min(0.5e6 / fs_hz, 1e18)));
  const si8 start_user = to_user_time(last_start_stored, rto);
  const si8 computed = start_user + static_cast<si8>(std::llround(duration));
  const si8 limit = rebuilt ? computed - half : start_user + half;
  static const char* const names[] = {".tmet", ".tidx", ".tdat"};
  for (int i = 0; i < 3; ++i) {
    const si8 end = byteio::read<si8>(std::span<const ui1>(uh[i], fmt::UNIVERSAL_HEADER_BYTES), 24);
    const si8 start =
        byteio::read<si8>(std::span<const ui1>(uh[i], fmt::UNIVERSAL_HEADER_BYTES), 16);
    if (end != fmt::UUTC_NO_ENTRY && to_user_time(end, rto) >= limit) continue;  // fine
    const bool negated = end != fmt::UUTC_NO_ENTRY ? end < 0
                                                   : (start < 0 && start != fmt::UUTC_NO_ENTRY);
    if (!negated || !time_is_storable(computed, rto)) {
      if (!note.empty()) note += "; ";
      note += std::string("the ") + names[i] +
              " end time is not stored in the offset-applied (negated) form";
      continue;
    }
    out.new_end[i] = to_stored_time(computed, rto);
  }
  return out;
}

}  // namespace

namespace {

/// One segment's recovery, decided during the scan and carried out after it.
///
/// Recovery runs in two phases so that a cancellation can never leave a
/// session half-recovered: the SCAN (all the reading, and nearly all the time)
/// is cancellable and writes nothing; the WRITE phase is not. Only what the
/// write phase needs is kept — not the index entries that survive unchanged,
/// which for a month-long segment are megabytes per channel; those are re-read
/// after checking that the files are still the ones that were scanned.
struct PlannedRecovery {
  fsys::path channel_dir, tidx, tdat;
  std::string channel, base, description;
  int segment_number = 0;
  std::size_t report_index = 0;               ///< into RecoveryReport::segments
  std::uint64_t index_size = 0;               ///< .tidx size when scanned
  std::vector<ui1> index_header;              ///< .tidx universal header when scanned (or less)
  std::vector<ui1> index_header_override;     ///< written instead of a lost/damaged .tidx header
  std::vector<ui1> data_header_override;      ///< written instead of a lost .tdat header
  std::uint64_t data_size = 0;                ///< .tdat size when scanned
  std::size_t kept_from_index = 0;            ///< leading .tidx entries that stay
  std::vector<fmt::TimeSeriesIndex> rebuilt;  ///< entries recovered from .tdat
  std::uint64_t data_end = 0;                 ///< end of the last whole block
  si8 tail = 0;                               ///< trailing bytes to drop
  /// The end-time decision (plan_end_times) and the .tmet header it was
  /// decided from (re-checked before writing).
  EndPlan end;
  bool tdat_header_current = false;  ///< .tdat entry count already right
  fsys::path tmet;
  std::vector<ui1> tmet_header;
};

/// A .tmet rebuilt during the scan, written first in the write phase.
struct TmetRebuild {
  fsys::path path;
  std::vector<ui1> bytes;
  bool existed = false;  ///< a (short) file was there: backed up before replacing
  fsys::path adopted_from;  ///< a complete `.tmet.mef3io-tmp` published as the .tmet: removed after
};

/// A complete, CRC-valid, loadable `<name>.tmet.mef3io-tmp` — what a
/// wait_for_disk write flushed before renaming it into place, left behind when
/// the rename did not survive a power cut. Returns its bytes (record length,
/// provenance stamped Recovery) or empty. Never throws.
std::vector<ui1> adoptable_tmet_temp(const fsys::path& tmet, const std::string& password) {
  try {
    const fsys::path tmp = tmet.parent_path() / (tmet.filename().string() + ".mef3io-tmp");
    std::error_code ec;
    if (!fsys::exists(tmp, ec)) return {};
    std::vector<ui1> file = read_all(tmp);
    if (file.size() < fmt::METADATA_FILE_BYTES) return {};
    file.resize(fmt::METADATA_FILE_BYTES);
    (void)load_time_series_metadata(file, password);  // CRC + access must hold
    auto uh = fmt::UniversalHeader::parse(file);
    fmt::provenance::stamp_modified(uh, fmt::provenance::Operation::Recovery);
    uh.serialize(file);
    byteio::write<ui4>(file, 0,
                       crc::calculate(std::span<const ui1>(file).subspan(
                           4, fmt::UNIVERSAL_HEADER_BYTES - 4)));
    (void)load_time_series_metadata(file, password);
    return file;
  } catch (const std::exception&) {
    return {};
  }
}

/// Rebuild a NON-FIRST segment's lost .tmet (missing, empty or short — a new
/// segment whose .tmet rename landed before its bytes, after a power cut).
///
/// What is constant along a channel comes from the previous segment's .tmet,
/// byte for byte: sections 1-3 (rate, conversion factor, units, names,
/// descriptions, subject, encryption levels, the protected and discretionary
/// regions) and the universal header's password fields, names and entry
/// sizes. What belongs to THIS segment comes from its own files: the segment
/// number, start/end times and level UUID from its .tdat universal header,
/// the start sample from its first index entry; the file UUID is new and the
/// provenance says Recovery. Every other derived field (counts, maxima,
/// durations, bounds) is left to the declaration step, i.e. repair_session,
/// right after. Throws when the previous .tmet does not load or this
/// segment's headers are not usable.
/// A universal header whose own CRC holds (or is NO_ENTRY).
bool header_crc_ok(const std::vector<ui1>& uh) {
  if (uh.size() < fmt::UNIVERSAL_HEADER_BYTES) return false;
  const ui4 stored = byteio::read<ui4>(uh, 0);
  return stored == fmt::CRC_NO_ENTRY ||
         stored == crc::calculate(std::span<const ui1>(uh).subspan(4, fmt::UNIVERSAL_HEADER_BYTES - 4));
}

/// A header for a file recovery has to write whole (its own header was lost):
/// another header of the SAME segment (names, segment number, level UUID,
/// password fields) with this file's type, no entries yet and a new file
/// UUID. Counts, times and CRCs are set where the file is written.
std::vector<ui1> synthesize_header(const std::vector<ui1>& source, const char* type) {
  auto uh = fmt::UniversalHeader::parse(source);
  uh.file_type_string = type;
  uh.number_of_entries = 0;
  uh.maximum_entry_size = std::string(type) == "tidx" ? fmt::TIME_SERIES_INDEX_BYTES : 0;
  std::random_device rd;
  for (auto& b : uh.file_uuid) b = static_cast<ui1>(rd() & 0xFF);
  std::vector<ui1> out(fmt::UNIVERSAL_HEADER_BYTES);
  uh.serialize(out);
  return out;
}

std::vector<ui1> rebuild_tmet(const fsys::path& prev_tmet, const fsys::path& tdat,
                              const fsys::path& tidx, int segment_number,
                              const std::string& password) {
  std::vector<ui1> file = read_all(prev_tmet);
  if (file.size() < fmt::METADATA_FILE_BYTES)
    throw FormatError("the previous segment's .tmet is short too");
  file.resize(fmt::METADATA_FILE_BYTES);  // no foreign padding in a rebuilt record
  const TimeSeriesMetadata md = load_time_series_metadata(file, password);  // CRC + access
  std::vector<ui1> tdat_uh, first_block;
  {
    std::ifstream in(tdat, std::ios::binary);
    tdat_uh = read_range(in, 0, fmt::UNIVERSAL_HEADER_BYTES);
    first_block = read_range(in, fmt::UNIVERSAL_HEADER_BYTES, fmt::RED_BLOCK_HEADER_BYTES);
  }
  // The index (56 bytes a block, small) gives the start sample and the
  // native extremes: each entry carries its block's count extremes. When it
  // was lost too (a short .tidx, rebuilt from the data right after), the
  // start sample continues the previous segment and the extremes stay unset.
  std::vector<ui1> index;
  {
    std::error_code ec;
    if (fsys::exists(tidx, ec)) index = read_all(tidx);
  }
  const bool index_usable = header_crc_ok(index) &&
                            index.size() >= fmt::UNIVERSAL_HEADER_BYTES + fmt::TIME_SERIES_INDEX_BYTES;
  si8 start_sample = md.section2.start_sample + md.section2.number_of_samples;
  std::optional<si4> hi, lo;
  if (index_usable) {
    start_sample = fmt::TimeSeriesIndex::parse(std::span<const ui1>(index).subspan(
                                                   fmt::UNIVERSAL_HEADER_BYTES,
                                                   fmt::TIME_SERIES_INDEX_BYTES))
                       .start_sample;
    for (std::size_t off = fmt::UNIVERSAL_HEADER_BYTES;
         off + fmt::TIME_SERIES_INDEX_BYTES <= index.size(); off += fmt::TIME_SERIES_INDEX_BYTES) {
      const auto e = fmt::TimeSeriesIndex::parse(
          std::span<const ui1>(index).subspan(off, fmt::TIME_SERIES_INDEX_BYTES));
      if (e.maximum_sample_value != fmt::RED_NAN)
        hi = hi ? std::max(*hi, e.maximum_sample_value) : e.maximum_sample_value;
      if (e.minimum_sample_value != fmt::RED_NAN)
        lo = lo ? std::min(*lo, e.minimum_sample_value) : e.minimum_sample_value;
    }
  }
  struct {
    si4 segment_number;
    si8 start_time, end_time;
    std::array<ui1, 16> level_uuid;
  } data_uh{};
  if (header_crc_ok(tdat_uh) && tdat_uh.size() >= fmt::UNIVERSAL_HEADER_BYTES) {
    const auto d = fmt::UniversalHeader::parse(tdat_uh);
    data_uh = {d.segment_number, d.start_time, d.end_time, d.level_uuid};
  } else {
    // The data file's header was lost too: the folder gives the number, the
    // first block its start (the declaration step sets the end).
    if (first_block.size() < fmt::RED_BLOCK_HEADER_BYTES)
      throw FormatError("this segment's data holds no block");
    data_uh.segment_number = segment_number;
    data_uh.start_time = fmt::RedBlockHeader::parse(first_block).start_time;
    data_uh.end_time = fmt::UUTC_NO_ENTRY;
    std::random_device rd;
    for (auto& b : data_uh.level_uuid) b = static_cast<ui1>(rd() & 0xFF);
  }

  auto uh = fmt::UniversalHeader::parse(file);
  uh.segment_number = data_uh.segment_number;
  uh.start_time = data_uh.start_time;
  uh.end_time = data_uh.end_time;
  uh.level_uuid = data_uh.level_uuid;
  std::random_device rd;
  for (auto& b : uh.file_uuid) b = static_cast<ui1>(rd() & 0xFF);
  fmt::provenance::stamp_modified(uh, fmt::provenance::Operation::Recovery);
  uh.serialize(file);

  // Section 2's start sample, in the stored image (decrypted and re-encrypted
  // under the key section 1 names when section 2 is encrypted).
  std::span<ui1> s2(file.data() + fmt::METADATA_SECTION_2_OFFSET,
                    fmt::TIME_SERIES_METADATA_SECTION_2_BYTES);
  const si1 s2_enc = md.section1.section_2_encryption;
  std::optional<std::array<ui1, fmt::PASSWORD_BYTES>> key;
  if (s2_enc > 0) {
    const auto keys = crypto::validate_password(password, uh.level_1_password_validation_field,
                                                uh.level_2_password_validation_field);
    key = (s2_enc == fmt::LEVEL_2_ENCRYPTION) ? keys.level2_key : keys.level1_key;
    if (!key) throw PasswordError("section 2 is encrypted; pass the session's password");
    const auto plain = crypto::aes128_ecb_decrypt(std::span<const ui1>(s2.data(), s2.size()), *key);
    std::copy(plain.begin(), plain.end(), s2.begin());
  }
  byteio::write<si8>(s2, 6352, start_sample);  // TimeSeriesMetadataSection2::start_sample
  {
    // Native extremes (6336/6344) as the writer stores them: counts times the
    // factor, the pair swapped for a negative factor; NaN (meflib's NO_ENTRY)
    // when no block carries one.
    const sf8 ufact = md.section2.units_conversion_factor;
    const sf8 nan = std::numeric_limits<sf8>::quiet_NaN();
    const sf8 h = hi ? static_cast<sf8>(*hi) * ufact : nan;
    const sf8 l = lo ? static_cast<sf8>(*lo) * ufact : nan;
    byteio::write<sf8>(s2, 6336, ufact >= 0.0 ? h : l);
    byteio::write<sf8>(s2, 6344, ufact >= 0.0 ? l : h);
  }
  if (s2_enc > 0) {
    const auto enc = crypto::aes128_ecb_encrypt(std::span<const ui1>(s2.data(), s2.size()), *key);
    std::copy(enc.begin(), enc.end(), s2.begin());
  }
  const std::size_t body = fmt::METADATA_FILE_BYTES - fmt::UNIVERSAL_HEADER_BYTES;
  byteio::write<ui4>(file, 4,
                     crc::calculate(std::span<const ui1>(file).subspan(fmt::UNIVERSAL_HEADER_BYTES, body)));
  byteio::write<ui4>(file, 0,
                     crc::calculate(std::span<const ui1>(file).subspan(4, fmt::UNIVERSAL_HEADER_BYTES - 4)));
  (void)load_time_series_metadata(file, password);  // must load as written
  return file;
}

struct SegmentDir {
  fsys::path channel_dir, dir;
  std::string channel;
  bool is_last = false;  ///< the channel's last segment (last_segment_dir's rule)
  fsys::path prev;       ///< the channel's previous segment (same rule); empty: first
};

/// After the index and the data agree, bring each target segment's
/// DECLARATIONS (.tmet section 2 + the three universal headers) back to what
/// they hold — through repair_session, so it is exactly the validator's own
/// repair (stored images edited, never re-serialized; backups; provenance
/// HeaderRepair; the last-segment stamp). Every repairable finding of the
/// segment is fixed: once the index changed, all derived declarations follow
/// it. Exact difference bytes: blocks the old .tmet never described may need
/// a larger buffer than it declares (an allocation contract).
/// The repairable check ids the validator reports for ONE segment — exactly
/// what rederive_declarations hands to repair_session for it (the dry run
/// previews it with this same call).
std::vector<std::string> segment_repairable_ids(const std::string& path,
                                                const std::string& password,
                                                const std::pair<std::string, int>& key,
                                                std::vector<std::string>* skipped = nullptr) {
  ValidateOptions vo;
  vo.password = password;
  vo.channels = {key.first};
  vo.segments = {key.second};
  const Report v = validate_session(path, vo);
  std::vector<std::string> ids;
  for (const auto& f : v.findings)
    if (f.repairable && std::find(ids.begin(), ids.end(), f.check_id) == ids.end())
      ids.push_back(f.check_id);
  if (skipped)
    for (const auto& sk : v.skipped) skipped->push_back(sk.reason);
  return ids;
}

void rederive_declarations(const std::string& path, const std::string& password, bool backup,
                           const std::vector<std::pair<std::string, int>>& targets,
                           RecoveryReport& report) {
  std::set<std::pair<std::string, int>> done;
  for (const auto& key : targets) {
    if (!done.insert(key).second) continue;
    ValidateOptions vo;
    vo.password = password;
    vo.channels = {key.first};
    vo.segments = {key.second};
    const std::string where = key.first + " segment " + std::to_string(key.second);
    try {
      std::vector<std::string> skipped;
      const std::vector<std::string> ids = segment_repairable_ids(path, password, key, &skipped);
      for (const auto& reason : skipped)
        report.notes.push_back(where + ": declarations not re-derived: " + reason);
      if (ids.empty()) continue;
      RepairSelection sel;
      sel.check_ids = ids;
      sel.channels = {key.first};
      sel.segments = {key.second};
      sel.backup = backup;
      const Report r = repair_session(path, sel, vo);
      std::vector<std::string> repaired;
      for (const auto& f : r.findings)
        if (f.repaired && std::find(repaired.begin(), repaired.end(), f.check_id) == repaired.end())
          repaired.push_back(f.check_id);
      for (auto& seg : report.segments)
        if (seg.channel == key.first && seg.segment_number == key.second)
          seg.declarations = repaired;
      for (const auto& n : r.findings)
        if (n.check_id == "provenance.last-segment") report.notes.push_back(where + ": " + n.message);
      if (backup && r.segments_repaired > 0)
        report.notes.push_back(where + ": declarations re-derived (" + std::to_string(repaired.size()) +
                               " check(s)); originals under " + path + ".repair-backup");
    } catch (const std::exception& e) {
      report.notes.push_back(where + ": declarations not re-derived: " + e.what());
    }
  }
}

}  // namespace

namespace {

// ---- annotation files (.rdat + .ridx) ---------------------------------------
// An annotation append writes the record bytes, then the index entries, then
// the two headers (records.cpp). A power cut can therefore leave: a torn last
// record, whole records the header does not count, or an index that is short
// or torn. Recovery keeps EVERY whole record, drops only the torn fragment
// AFTER the last whole record (backed up), and rebuilds the .ridx and the
// headers from the .rdat. A whole record whose CRC fails (bit rot, foreign
// damage: NOT an interrupted append) is never removed and never ends the walk:
// it stays in the file and the index, is counted and reported, and every
// record after it stays too.
struct RecordFilePlan {
  fsys::path rdat, ridx;
  std::string where;
  std::vector<ui1> rdat_head, ridx_head;  // original 1024-byte headers (ridx may be empty)
  std::uintmax_t rdat_size = 0;
  std::uint64_t keep_bytes = 0;           // rdat prefix kept (whole records)
  std::vector<ui1> new_ridx_body;
  si8 n_before = 0, n_after = 0, max_entry = 0;
  std::vector<si8> times;                 // stored header times of the kept records
  ui4 body_crc = 0;
  bool header_fix = false, ridx_rebuild = false;
  std::string action;
  si8 dropped = 0;
  si8 damaged = 0;                        // whole records failing their CRC (kept)
  bool no_change = false;                 // nothing to repair; only damage to report
};

std::string damage_text(si8 n) {
  return std::to_string(n) + " record(s) fail their CRC (damage, not an interrupted write): kept "
         "exactly as they are, nothing removed; a strict reader reports them";
}

std::optional<RecordFilePlan> plan_record_file(const fsys::path& rdat) {
  constexpr std::uint64_t UH = fmt::UNIVERSAL_HEADER_BYTES;
  RecordFilePlan p;
  p.rdat = rdat;
  p.ridx = rdat;
  p.ridx.replace_extension(".ridx");
  p.where = rdat.string();
  std::error_code ec;
  p.rdat_size = fsys::file_size(rdat, ec);
  if (ec || p.rdat_size < UH) return std::nullopt;  // not a record file we can reason about
  const auto bytes = read_all(rdat);
  p.rdat_head.assign(bytes.begin(), bytes.begin() + UH);
  auto uh = fmt::UniversalHeader::parse(p.rdat_head);
  if (uh.file_type_string != fmt::FILE_TYPE_RECORD_DATA) return std::nullopt;
  const bool has_ridx = fsys::exists(p.ridx);
  std::vector<ui1> ridx;
  if (has_ridx) {
    ridx = read_all(p.ridx);
    if (ridx.size() >= UH) p.ridx_head.assign(ridx.begin(), ridx.begin() + UH);
  }
  // Walk the whole records. A record is whole when its header fits and its
  // body fits; the walk stops at the first that does not (a torn tail). A
  // record failing its CRC is counted as damaged but stays: only the bytes
  // after the last whole record may ever be dropped.
  std::vector<si8> offsets;
  std::uint64_t off = UH;
  bool last_valid = true;  // the last walked record is whole AND CRC-valid (vacuous with none)
  while (off + fmt::RECORD_HEADER_BYTES <= bytes.size()) {
    const std::string type = byteio::read_string(bytes, off + 4, 4);
    if (type.empty()) break;
    const ui4 nb = byteio::read<ui4>(bytes, off + 12);
    if (nb > bytes.size() - off - fmt::RECORD_HEADER_BYTES) break;
    const ui4 stored = byteio::read<ui4>(bytes, off);
    if (stored != 0 &&
        crc::calculate(std::span<const ui1>(bytes).subspan(off + 4, fmt::RECORD_HEADER_BYTES - 4 + nb)) != stored)
      ++p.damaged;
    last_valid = stored != 0 && crc::calculate(std::span<const ui1>(bytes).subspan(
                                    off + 4, fmt::RECORD_HEADER_BYTES - 4 + nb)) == stored;
    offsets.push_back(static_cast<si8>(off));
    p.times.push_back(byteio::read<si8>(bytes, off + 16));
    p.max_entry = std::max<si8>(p.max_entry, fmt::RECORD_HEADER_BYTES + nb);
    off += fmt::RECORD_HEADER_BYTES + nb;
  }
  p.keep_bytes = off;
  p.n_after = static_cast<si8>(offsets.size());
  p.dropped = static_cast<si8>(bytes.size() - off);  // a torn record, or zeros an unfinished write left
  p.n_before = has_ridx && ridx.size() >= UH ? static_cast<si8>((ridx.size() - UH) / fmt::RECORD_INDEX_BYTES) : 0;

  // Is the part after the last whole record PROVABLY a torn append? Bytes are
  // dropped ONLY when ALL hold: (a) the .ridx exists and is readable, (b) no
  // .ridx entry points at or beyond the stopping offset (an entry AT it means
  // the record was indexed: its length field is damaged, not torn), (c) no
  // whole CRC-valid record sits at any later position of the remaining bytes
  // (the rest of the file is read byte by byte: records are 8-byte aligned in
  // files mef3io writes, but a foreign writer's need not be), and the rest is
  // not itself one complete record with a damaged length, and (d) the remaining
  // bytes are shorter than a header or fail as a record (true by construction:
  // the walk above stopped there). Otherwise both files stay byte for byte as
  // they are. (The header's entry count is no evidence: without barriers it
  // can land before the data.) Without a readable index nothing is ever
  // dropped: a tear cannot be told from damage.
  if (p.dropped > 0) {
    std::string why;
    if (!has_ridx || ridx.size() < UH) {
      why = "there is no readable index (.ridx), so nothing can show these bytes are an interrupted write";
    } else {
      // An index entry at or beyond the stop is TORN (the index landed ahead
      // of the data) only if its bytes provably never arrived: they run past
      // the end of the file, or every byte from there to EOF is zero. Bytes
      // that are present and non-zero are DAMAGE. A length field that merely
      // looks too long is not enough: if the bytes are one record whose CRC
      // matches with the length restored (the remaining size, or the stored
      // length with one bit flipped), it is damage too.
      auto entry_is_damage = [&](std::uint64_t u) {
        if (u + fmt::RECORD_HEADER_BYTES > bytes.size()) return false;  // header cut: torn
        if (std::all_of(bytes.begin() + static_cast<std::ptrdiff_t>(u), bytes.end(), [](ui1 c) { return c == 0; }))
          return false;  // zero-extended: torn
        const ui4 stored = byteio::read<ui4>(bytes, u);
        const ui4 nb = byteio::read<ui4>(bytes, u + 12);
        const std::uint64_t room = bytes.size() - u - fmt::RECORD_HEADER_BYTES;
        if (nb <= room) return true;  // present in full (or longer file): damage
        if (stored == 0) return false;
        std::vector<std::uint64_t> cands = {room};
        for (int k = 0; k < 32; ++k) cands.push_back(nb ^ (std::uint64_t{1} << k));
        for (auto c : cands) {
          if (c > room) continue;
          std::vector<ui1> tmp(bytes.begin() + static_cast<std::ptrdiff_t>(u + 4),
                               bytes.begin() + static_cast<std::ptrdiff_t>(u + fmt::RECORD_HEADER_BYTES + c));
          byteio::write<ui4>(tmp, 8, static_cast<ui4>(c));
          if (crc::calculate(tmp) == stored) return true;
        }
        return false;  // runs past the end of the file: torn
      };
      // A torn append follows INTACT data. If the walk stopped right after a
      // record that fails its CRC, the stop is inside that record's own bytes
      // (a length flipped down): damage. Likewise a stop strictly inside the
      // span two consecutive index entries delimit.
      bool inside_indexed_span = false;
      for (std::uint64_t o = UH; o + 2 * fmt::RECORD_INDEX_BYTES <= ridx.size(); o += fmt::RECORD_INDEX_BYTES) {
        const auto v = byteio::read<si8>(ridx, o + 8);
        const auto w = byteio::read<si8>(ridx, o + 8 + fmt::RECORD_INDEX_BYTES);
        if (v >= 0 && static_cast<std::uint64_t>(v) < off && w > static_cast<si8>(off)) inside_indexed_span = true;
      }
      si8 at_or_beyond = 0;
      for (std::uint64_t o = UH; o + fmt::RECORD_INDEX_BYTES <= ridx.size(); o += fmt::RECORD_INDEX_BYTES) {
        const auto v = byteio::read<si8>(ridx, o + 8);
        if (v >= 0 && static_cast<std::uint64_t>(v) >= off && entry_is_damage(static_cast<std::uint64_t>(v))) ++at_or_beyond;
      }
      auto whole_at = [&](std::uint64_t q) {  // a CRC-valid record starting at q
        if (q + fmt::RECORD_HEADER_BYTES > bytes.size()) return false;
        const ui4 stored = byteio::read<ui4>(bytes, q);
        if (stored == 0) return false;
        for (int k = 4; k < 8; ++k)  // record types are four printable ASCII bytes
          if (bytes[q + k] < 0x20 || bytes[q + k] >= 0x7f) return false;
        const ui4 nb = byteio::read<ui4>(bytes, q + 12);
        if (nb > bytes.size() - q - fmt::RECORD_HEADER_BYTES) return false;
        return crc::calculate(std::span<const ui1>(bytes).subspan(q + 4, fmt::RECORD_HEADER_BYTES - 4 + nb)) == stored;
      };
      bool later_whole = false;
      for (std::uint64_t q = off + 1; q + fmt::RECORD_HEADER_BYTES <= bytes.size() && !later_whole; ++q)
        later_whole = whole_at(q);
      const bool tail_whole = entry_is_damage(off);  // present, non-zero bytes at the stop
      if (!last_valid)
        why = "the record just before it fails its CRC, so these bytes may be its own damaged body, not an interrupted write";
      else if (inside_indexed_span)
        why = "the stop lies inside the span of an indexed record";
      else if (at_or_beyond > 0)
        why = std::to_string(at_or_beyond) + " index entr" + (at_or_beyond == 1 ? "y points" : "ies point") +
              " at or beyond the damage at bytes that are present and not zero, so the record is damaged, not torn";
      else if (later_whole)
        why = "an intact record follows the damage";
      else if (tail_whole)
        why = "the bytes at the stop are present and not zero (a record with a damaged field)";
    }
    if (!why.empty()) {
      p.no_change = true;
      p.dropped = 0;
      p.action = "record file damaged, not modified: the records stop being readable at byte " +
                 std::to_string(off) + " (" + std::to_string(p.n_after) + " intact before it), and " + why +
                 "; both files were left exactly as they are (nothing is removed on a guess). A strict reader "
                 "refuses the file; a lenient one reads the intact records";
      if (p.damaged) p.action += "; " + damage_text(p.damaged);
      return p;
    }
  }

  // Does the index describe exactly these records?
  bool index_ok = has_ridx && ridx.size() >= UH && (ridx.size() - UH) % fmt::RECORD_INDEX_BYTES == 0 &&
                  p.n_before == p.n_after;
  if (index_ok)
    for (si8 i = 0; i < p.n_after && index_ok; ++i)
      index_ok = byteio::read<si8>(ridx, UH + static_cast<std::uint64_t>(i) * fmt::RECORD_INDEX_BYTES + 8) ==
                 offsets[static_cast<std::size_t>(i)];
  if (index_ok) {
    auto ri = fmt::UniversalHeader::parse(p.ridx_head);
    index_ok = ri.file_type_string == fmt::FILE_TYPE_RECORD_INDICES && ri.header_crc_valid(p.ridx_head) &&
               (ri.number_of_entries == p.n_after || ri.number_of_entries == -1);
  }
  const bool count_ok = uh.number_of_entries == p.n_after || uh.number_of_entries == -1;
  const bool head_ok = uh.header_crc_valid(p.rdat_head);
  if (index_ok && count_ok && head_ok && p.dropped == 0) {
    if (p.damaged == 0) return std::nullopt;  // healthy
    p.no_change = true;                       // only damage to report; nothing to write
    p.action = damage_text(p.damaged);
    return p;
  }

  p.ridx_rebuild = !index_ok;
  p.header_fix = !count_ok || !head_ok || p.dropped != 0 || !index_ok;
  p.body_crc = crc::calculate(std::span<const ui1>(bytes).subspan(UH, p.keep_bytes - UH));
  // Index entries come from the record headers: type, version, encryption,
  // offset, time.
  for (si8 i = 0; i < p.n_after; ++i) {
    const auto o = static_cast<std::uint64_t>(offsets[static_cast<std::size_t>(i)]);
    std::array<ui1, fmt::RECORD_INDEX_BYTES> e{};
    std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(o + 4), 4, e.begin());
    e[5] = bytes[o + 9];
    e[6] = bytes[o + 10];
    e[7] = bytes[o + 11];
    byteio::write<si8>(e, 8, offsets[static_cast<std::size_t>(i)]);
    byteio::write<si8>(e, 16, p.times[static_cast<std::size_t>(i)]);
    p.new_ridx_body.insert(p.new_ridx_body.end(), e.begin(), e.end());
  }
  std::string what;
  if (p.dropped) what += "drops a torn fragment after the last whole record: the index does not reach it and nothing intact follows (" + std::to_string(p.dropped) + " byte(s))";
  if (p.n_after != p.n_before || !has_ridx || p.ridx_rebuild)
    what += std::string(what.empty() ? "" : ", ") + "rebuilds the index from the records (" +
            std::to_string(p.n_before) + " -> " + std::to_string(p.n_after) + " entries)";
  if (what.empty()) what = "repairs the file header";
  if (p.damaged) what += "; " + damage_text(p.damaged);
  p.action = what;
  return p;
}

void apply_record_file(const RecordFilePlan& p, bool backup, const fsys::path& backup_root,
                       const fsys::path& session_root) {
  constexpr std::uint64_t UH = fmt::UNIVERSAL_HEADER_BYTES;
  const std::string rel = fsys::relative(p.rdat, session_root).generic_string();
  if (backup) {
    back_up_bytes(backup_root, rel + ".header", p.rdat_head);
    if (p.rdat_size > p.keep_bytes) {
      std::ifstream f(p.rdat, std::ios::binary);
      auto tail = read_range(f, p.keep_bytes, static_cast<std::size_t>(p.rdat_size - p.keep_bytes));
      back_up_bytes(backup_root, rel + ".dropped", tail);
    }
    if (fsys::exists(p.ridx)) back_up_file(p.ridx, backup_root, fsys::relative(p.ridx, session_root).generic_string());
  }
  // Times of the UH span, from the kept records' stored times, in whichever
  // convention they use (negated = offset applied: later time = smaller value).
  si8 start = fmt::UUTC_NO_ENTRY, end = fmt::UUTC_NO_ENTRY;
  if (!p.times.empty()) {
    const auto mm = std::minmax_element(p.times.begin(), p.times.end());
    const bool all_neg = *mm.second < 0, all_pos = *mm.first >= 0;
    if (all_neg) { start = *mm.second; end = *mm.first; }
    else if (all_pos) { start = *mm.first; end = *mm.second; }
  }
  auto uh = fmt::UniversalHeader::parse(p.rdat_head);
  auto fix = [&](fmt::UniversalHeader h, std::vector<ui1> head, ui4 body_crc, si8 max_size) {
    h.number_of_entries = p.n_after;
    h.maximum_entry_size = max_size;
    if (start != fmt::UUTC_NO_ENTRY) { h.start_time = start; h.end_time = end; }
    h.body_crc = body_crc;
    fmt::provenance::stamp_modified(h, fmt::provenance::Operation::Recovery);
    h.serialize(head);
    h.update_header_crc(head);
    return head;
  };
  // Truncate first, then the headers, then the replaced index: a crash in
  // between leaves a state this same recovery handles again.
  if (p.rdat_size > p.keep_bytes) {
    std::error_code ec;
    fsys::resize_file(p.rdat, p.keep_bytes, ec);
    if (ec) throw IoError("cannot truncate " + p.rdat.string() + ": " + ec.message());
  }
  {
    const auto head = fix(uh, p.rdat_head, p.body_crc, p.max_entry);
    std::fstream f(p.rdat, std::ios::binary | std::ios::in | std::ios::out);
    if (!f) throw IoError("cannot open for header update: " + p.rdat.string());
    f.write(reinterpret_cast<const char*>(head.data()), static_cast<std::streamsize>(head.size()));
    f.flush();
    if (!f) throw IoError("header update failed: " + p.rdat.string());
    f.close();
    (void)detail::fsync_file(p.rdat.string());
  }
  // The index: a new file (header from the rdat's, file type ridx), replaced atomically.
  fmt::UniversalHeader ih = p.ridx_head.empty() ? uh : fmt::UniversalHeader::parse(p.ridx_head);
  if (p.ridx_head.empty()) {
    ih.file_type_string = fmt::FILE_TYPE_RECORD_INDICES;
    ih.file_uuid = {};  // a new file
  }
  std::vector<ui1> ihead(UH, 0);
  if (!p.ridx_head.empty()) ihead = p.ridx_head;
  ih.file_type_string = fmt::FILE_TYPE_RECORD_INDICES;
  const ui4 ibody = crc::calculate(p.new_ridx_body);
  std::vector<ui1> file = fix(ih, ihead, ibody, fmt::RECORD_INDEX_BYTES);
  file.insert(file.end(), p.new_ridx_body.begin(), p.new_ridx_body.end());
  write_all(p.ridx, file);
}

void recover_record_files(const fsys::path& root, bool apply, bool backup,
                          const fsys::path& backup_root, RecoveryReport& report, const Control& ctl) {
  std::vector<fsys::path> files;
  auto add_dir = [&](const fsys::path& d) {
    for (const auto& e : fsys::directory_iterator(d))
      if (e.is_regular_file() && e.path().extension() == ".rdat") files.push_back(e.path());
  };
  add_dir(root);
  for (const auto& ch : fsys::directory_iterator(root)) {
    if (!ch.is_directory() || ch.path().extension() != ".timd") continue;
    add_dir(ch.path());
    for (const auto& sg : fsys::directory_iterator(ch.path()))
      if (sg.is_directory() && sg.path().extension() == ".segd") add_dir(sg.path());
  }
  std::sort(files.begin(), files.end());
  // A .ridx whose .rdat is missing: nothing can be rebuilt from an index
  // alone, and it is never deleted or replaced. Reported only.
  {
    std::vector<fsys::path> dirs = {root};
    for (const auto& ch : fsys::directory_iterator(root)) {
      if (!ch.is_directory() || ch.path().extension() != ".timd") continue;
      dirs.push_back(ch.path());
      for (const auto& sg : fsys::directory_iterator(ch.path()))
        if (sg.is_directory() && sg.path().extension() == ".segd") dirs.push_back(sg.path());
    }
    for (const auto& d : dirs)
      for (const auto& e : fsys::directory_iterator(d)) {
        if (!e.is_regular_file() || e.path().extension() != ".ridx") continue;
        auto sibling = e.path();
        sibling.replace_extension(".rdat");
        if (!fsys::exists(sibling))
          report.notes.push_back(e.path().string() + ": record index without its record file (.rdat "
                                 "missing); kept as is, nothing can be rebuilt from an index alone, "
                                 "and writing annotations here is refused until the .rdat is restored");
      }
  }
  for (const auto& f : files) {
    ctl.check();
    std::optional<RecordFilePlan> plan;
    try {
      plan = plan_record_file(f);
    } catch (const std::exception& e) {
      report.skipped.push_back(f.string() + ": annotation file not examined: " + e.what());
      continue;
    }
    if (!plan) continue;
    if (plan->no_change) {
      report.notes.push_back(f.string() + ": " + plan->action);
      continue;
    }
    RecoveredRecordFile r;
    r.path = f.string();
    r.records_before = plan->n_before;
    r.records_after = plan->n_after;
    r.bytes_dropped = plan->dropped;
    r.index_rebuilt = plan->ridx_rebuild;
    r.records_damaged = plan->damaged;
    r.action = (apply ? "" : "would be repaired: ") + plan->action;
    report.record_files.push_back(r);
    if (!apply) continue;
    try {
      report.backup_root = backup ? backup_root.string() : report.backup_root;
      apply_record_file(*plan, backup, backup_root, root);
      report.record_files.back().action = plan->action;
    } catch (const std::exception& e) {
      report.skipped.push_back(f.string() + ": annotation repair failed: " + e.what());
      report.record_files.pop_back();
    }
  }
}

}  // namespace

RecoveryReport recover_session(const std::string& path, bool apply, bool backup,
                               const std::string& password, const Control& ctl) {
  // The password is used only to read a segment's offset and rate when its
  // end time has to be decided (plan_end_times).
  if (path_has_suffix(path, ".tar"))
    throw IoError("cannot recover a tar session archive in place: " + path +
                  " (extract it first with extract_session)");
  if (!path_has_suffix(path, ".mefd"))
    throw IoError("session directory must end with .mefd: " + path);
  if (!fsys::is_directory(path)) throw IoError("no such session directory: " + path);
  if (apply) detail::refuse_if_mid_operation(path);

  const fsys::path root(path);
  RecoveryReport report;
  std::set<fsys::path> channels_to_stamp;
  const fsys::path backup_root =
      root.parent_path() / (root.filename().string() + ".recover-backup");

  std::vector<SegmentDir> all_segments;
  for (const auto& ch_entry : fsys::directory_iterator(root)) {
    if (!ch_entry.is_directory() || ch_entry.path().extension() != ".timd") continue;
    std::vector<fsys::path> segs;
    for (const auto& s : fsys::directory_iterator(ch_entry.path()))
      if (s.is_directory() && s.path().extension() == ".segd") segs.push_back(s.path());
    // Ordered by the SAME rule as detail::last_segment_dir (number, then
    // folder name), so the last one is last and each has its predecessor.
    const auto key = [](const fsys::path& p) {
      return std::make_pair(detail::segment_number_of(p.stem().string()), p.filename().string());
    };
    std::sort(segs.begin(), segs.end(),
              [&](const fsys::path& a, const fsys::path& b) { return key(a) < key(b); });
    for (std::size_t i = 0; i < segs.size(); ++i)
      all_segments.push_back({ch_entry.path(), segs[i], ch_entry.path().stem().string(),
                              i + 1 == segs.size(), i ? segs[i - 1] : fsys::path()});
  }

  // Progress counts each segment's .tidx + .tdat bytes: the scan reads the
  // whole index and walks whatever data lies past it. Sized only for a
  // listener — it costs two stats per segment.
  std::vector<std::uint64_t> seg_bytes;
  std::uint64_t total = 0;
  if (ctl.progress) {
    seg_bytes.reserve(all_segments.size());
    for (const auto& sd : all_segments) {
      const std::string base = sd.dir.stem().string();
      std::uint64_t b = 0;
      std::error_code ec;
      const auto a = fsys::file_size(sd.dir / (base + ".tidx"), ec);
      if (!ec) b += static_cast<std::uint64_t>(a);
      const auto d = fsys::file_size(sd.dir / (base + ".tdat"), ec);
      if (!ec) b += static_cast<std::uint64_t>(d);
      seg_bytes.push_back(b);
      total += b;
    }
  }
  detail::ProgressReporter progress(ctl, total);
  progress.start();

  std::vector<PlannedRecovery> plan;
  // Channels' last segments whose index and data agree: their DECLARATIONS
  // are checked after the scan (the .tmet may be newer or older than the
  // index after a power cut — see declaration_checks below).
  std::vector<std::pair<std::string, int>> last_segments;
  struct Removal {
    fsys::path channel_dir, dir;
    std::string channel;
    std::size_t report_index;
  };
  std::vector<Removal> removals;
  std::vector<TmetRebuild> tmet_rebuilds;
  std::vector<std::pair<std::string, int>> rebuilt_tmet_segments;
  std::uint64_t seg_base = 0;  // progress at the start of the current segment

  // ---- phase 1: scan (reads only; cancellable) ----------------------------
  for (std::size_t si = 0; si < all_segments.size(); ++si) {
    ctl.check();
    if (si > 0 && progress.active()) seg_base += seg_bytes[si - 1];
    progress.advance_to(seg_base);

    const SegmentDir& sd = all_segments[si];
    const fsys::path& seg = sd.dir;
    const std::string& channel = sd.channel;
    const std::string base = seg.stem().string();
    const fsys::path tidx = seg / (base + ".tidx");
    const fsys::path tdat = seg / (base + ".tdat");
    int segment_number = 0;
    try {
      segment_number = std::stoi(base.substr(base.rfind('-') + 1));
    } catch (...) { /* leave 0 */ }
    const std::string description = channel + "/" + base;
    ++report.segments_examined;

    std::string pre_action;              // .tmet/.tidx rebuilds planned for this segment
    std::optional<TmetRebuild> tmet_rebuild;
    std::vector<ui1> index_header_override;  // the .tidx header was lost: write this one
    std::vector<ui1> data_header_override;   // the .tdat header was lost: write this one
    bool index_lost = false;                 // the .tidx is shorter than its header
    // A NEW segment (or channel) cut off by a power cut: one of its files is
    // missing, empty or shorter than its header — the classic outcome of
    // renaming an unflushed file to a new name — and a strict open of the
    // whole session fails on it (a missing .tmet only hides the segment).
    //  * Its .tdat holds no whole, CRC-valid block: it holds no sample, so a
    //    channel's LAST such segment is REMOVED (moved to the backup).
    //  * Its .tmet is lost: rebuilt from the previous segment's .tmet plus
    //    this segment's own files (rebuild_tmet); a channel's FIRST segment
    //    has no previous one (its .tmet is flushed when written for that
    //    reason) — skipped, with what to do.
    //  * Its .tidx is lost: rebuilt from the CRC-checked blocks in .tdat
    //    (the placement authority), under a header taken from this segment's
    //    .tmet (or .tdat), as for any index that fails its CRC below.
    {
      const fsys::path tmet_path = seg / (base + ".tmet");
      std::error_code e1, e2, e3;
      const auto sz_tidx = fsys::exists(tidx) ? fsys::file_size(tidx, e1) : 0;
      const auto sz_tdat = fsys::exists(tdat) ? fsys::file_size(tdat, e2) : 0;
      const auto sz_tmet = fsys::exists(tmet_path) ? fsys::file_size(tmet_path, e3) : 0;
      const bool incomplete = sz_tidx < fmt::UNIVERSAL_HEADER_BYTES ||
                              sz_tdat < fmt::UNIVERSAL_HEADER_BYTES ||
                              sz_tmet < fmt::METADATA_FILE_BYTES;
      if (incomplete) {
        bool has_block = false;
        if (sz_tdat >= fmt::UNIVERSAL_HEADER_BYTES + fmt::RED_BLOCK_HEADER_BYTES) {
          std::ifstream in(tdat, std::ios::binary);
          fmt::RedBlockHeader h;
          has_block = in && block_looks_real(in, static_cast<std::uint64_t>(sz_tdat),
                                             fmt::UNIVERSAL_HEADER_BYTES, h);
        }
        std::string problem;
        if (!has_block) {
          problem = "its data holds no block";
        } else {
          std::vector<std::string> done;
          try {
            if (sz_tmet < fmt::METADATA_FILE_BYTES) {
              if (auto adopted = adoptable_tmet_temp(tmet_path, password); !adopted.empty()) {
                tmet_rebuild = TmetRebuild{tmet_path, std::move(adopted),
                                           sz_tmet > 0 || fsys::exists(tmet_path),
                                           tmet_path.parent_path() /
                                               (tmet_path.filename().string() + ".mef3io-tmp")};
                done.push_back(std::string("the .tmet was ") +
                               (sz_tmet ? "shorter than a metadata record"
                                        : (fsys::exists(tmet_path) ? "empty" : "missing")) +
                               ": " + (apply ? "published" : "would be published") +
                               " from its complete, CRC-valid .tmet.mef3io-tmp (a write whose "
                               "rename did not reach the disk)");
              } else {
                if (sd.prev.empty())
                throw FormatError("the .tmet of a channel's first segment is lost, and there is "
                                  "no previous segment's .tmet to rebuild it from");
              auto rebuilt = rebuild_tmet(sd.prev / (sd.prev.stem().string() + ".tmet"), tdat,
                                          tidx, segment_number, password);
              tmet_rebuild = TmetRebuild{tmet_path, std::move(rebuilt),
                                         sz_tmet > 0 || fsys::exists(tmet_path), {}};
              done.push_back(std::string("the .tmet was ") +
                             (sz_tmet ? "shorter than a metadata record"
                                      : (fsys::exists(tmet_path) ? "empty" : "missing")) +
                             ": " + (apply ? "rebuilt" : "would be rebuilt") +
                             " from the previous segment's .tmet and this segment's own files");
              }
            }
            // A header source of THIS segment for whatever else is lost.
            std::vector<ui1> source;
            if (tmet_rebuild) {
              source.assign(tmet_rebuild->bytes.begin(),
                            tmet_rebuild->bytes.begin() + fmt::UNIVERSAL_HEADER_BYTES);
            } else {
              std::ifstream in(tmet_path, std::ios::binary);
              source = read_range(in, 0, fmt::UNIVERSAL_HEADER_BYTES);
              if (!header_crc_ok(source)) source.clear();
            }
            if (sz_tidx < fmt::UNIVERSAL_HEADER_BYTES) {
              if (source.empty()) throw FormatError("no intact header to rebuild the index under");
              index_header_override = synthesize_header(source, "tidx");
              index_lost = true;
              done.push_back(std::string("the .tidx was ") + (sz_tidx ? "short" : "empty or missing") +
                             ": " + (apply ? "rebuilt" : "would be rebuilt") +
                             " from the CRC-checked blocks in .tdat");
            }
            if (!source.empty()) {
              std::ifstream in(tdat, std::ios::binary);
              if (!header_crc_ok(read_range(in, 0, fmt::UNIVERSAL_HEADER_BYTES)))
                data_header_override = synthesize_header(source, "tdat");
            }
          } catch (const std::exception& e) {
            problem = e.what();
            tmet_rebuild.reset();
            index_header_override.clear();
            data_header_override.clear();
            index_lost = false;
          }
          for (std::size_t i = 0; i < done.size(); ++i) pre_action += (i ? "; " : "") + done[i];
        }
        if (pre_action.empty()) {
          if (sd.is_last && !has_block) {
            RecoveredSegment out;
            out.channel = channel;
            out.segment_number = segment_number;
            out.path = description;
            out.action = "the segment's files are missing, empty or shorter than their headers "
                         "and its data holds no block (a new segment cut off before its data "
                         "reached the disk): the segment is removed";
            report.segments.push_back(std::move(out));
            if (apply)
              removals.push_back({sd.channel_dir, seg, channel, report.segments.size() - 1});
          } else {
            report.skipped.push_back(
                description + ": segment is incomplete (a file is missing or shorter than its "
                              "header); " + problem +
                "; it cannot be rebuilt. Open with strict=False to read the rest of the "
                "session" + (has_block ? "" : ", and remove this segment folder"));
          }
          continue;
        }
      }
    }

    // The .tidx is read whole (56 bytes per block — megabytes at most). The
    // .tdat is NOT: it is the file that runs to tens of gigabytes, and all
    // recovery needs from it is its size plus a few block headers.
    std::vector<ui1> index_bytes;
    std::uint64_t data_size = 0;
    std::ifstream data_in;
    try {
      std::error_code ec;
      if (fsys::exists(tidx, ec)) index_bytes = read_all(tidx);  // a lost one is rebuilt
      data_size = static_cast<std::uint64_t>(fsys::file_size(tdat, ec));
      if (ec) throw IoError("cannot stat " + tdat.string() + ": " + ec.message());
      // Unbuffered: every read is exactly the bytes asked for (a buffered
      // stream refills its buffer after each seek, reading past the block).
      data_in.rdbuf()->pubsetbuf(nullptr, 0);
      data_in.open(tdat, std::ios::binary);
      if (!data_in) throw IoError("cannot open file: " + tdat.string());
    } catch (const std::exception& e) {
      report.skipped.push_back(description + ": " + e.what());
      continue;
    }
    const std::size_t index_size_on_disk = index_bytes.size();
    std::vector<ui1> index_head_on_disk(
        index_bytes.begin(),
        index_bytes.begin() + std::min<std::size_t>(index_bytes.size(), fmt::UNIVERSAL_HEADER_BYTES));
    if (index_lost) index_bytes = index_header_override;  // no entries: all rebuilt
    if (index_bytes.size() < fmt::UNIVERSAL_HEADER_BYTES ||
        data_size < fmt::UNIVERSAL_HEADER_BYTES) {
      report.skipped.push_back(description + ": file shorter than a universal header");
      continue;
    }
    progress.advance_to(seg_base + index_bytes.size());

    // Recovery DECIDES what to keep from these bytes, and then truncates —
    // unlike declaration repair, which never touches the index or the data.
    // When the index passes its own CRC its entries are trusted as written.
    //
    // When it does NOT, the header and the entries came from different
    // writes — the normal outcome of a power cut, since an append extends the
    // entries and patches the header in place with no flush between (2.0):
    // entries without their header, a header without its entries, entries
    // zero-extended. Refusing there left intact samples unreadable. Instead
    // no entry is taken on trust beyond what a CRC vouches for: the longest
    // prefix the stored body CRC covers (an old header over new entries), and
    // past it, each entry only if the RED block header at its offset — the
    // placement authority, covered by the block CRC — says the same thing.
    // From the first entry that does not, the index is rebuilt from the
    // CRC-checked blocks in .tdat, as for any unindexed tail. Streaming: one
    // 304-byte header per unverified entry, never the data file whole.
    std::size_t crc_trusted = static_cast<std::size_t>(-1);  // all: CRC passed
    bool index_crc_bad = false;
    {
      const ui4 stored_header = byteio::read<ui4>(index_bytes, 0);
      const ui4 stored_body = byteio::read<ui4>(index_bytes, 4);
      const ui4 real_header = crc::calculate(std::span<const ui1>(index_bytes)
                                                 .subspan(4, fmt::UNIVERSAL_HEADER_BYTES - 4));
      const std::size_t body_len = index_bytes.size() - fmt::UNIVERSAL_HEADER_BYTES;
      // Bound by whole entries: a torn trailing fragment is exactly what an
      // interrupted write leaves, and it must not make the whole index look
      // corrupt. The fragment is dropped further down.
      const std::size_t whole = body_len - (body_len % fmt::TIME_SERIES_INDEX_BYTES);
      const ui4 real_body = crc::calculate(
          std::span<const ui1>(index_bytes).subspan(fmt::UNIVERSAL_HEADER_BYTES, whole));
      const ui4 real_body_to_eof = crc::calculate(
          std::span<const ui1>(index_bytes).subspan(fmt::UNIVERSAL_HEADER_BYTES));
      const bool header_ok =
          stored_header == real_header || stored_header == fmt::CRC_NO_ENTRY;
      const bool body_ok = stored_body == real_body || stored_body == real_body_to_eof ||
                           stored_body == fmt::CRC_NO_ENTRY;
      if (index_lost || !header_ok || !body_ok) {
        index_crc_bad = true;
        crc_trusted = 0;
        // A lost or damaged HEADER is not written back: one is taken from
        // this segment's .tmet (or .tdat) instead.
        if (!header_ok && !index_lost && index_header_override.empty()) {
          std::ifstream in(seg / (base + ".tmet"), std::ios::binary);
          auto source = read_range(in, 0, fmt::UNIVERSAL_HEADER_BYTES);
          if (header_crc_ok(source)) index_header_override = synthesize_header(source, "tidx");
        }
        if (header_ok) {
          // The longest whole-entry prefix the stored body CRC was taken
          // over (the rolling CRC needs one pass over the in-memory index).
          ui4 rolling = fmt::CRC_START_VALUE;
          const std::size_t n = whole / fmt::TIME_SERIES_INDEX_BYTES;
          for (std::size_t i = 0; i < n; ++i) {
            rolling = crc::calculate(
                std::span<const ui1>(index_bytes)
                    .subspan(fmt::UNIVERSAL_HEADER_BYTES + i * fmt::TIME_SERIES_INDEX_BYTES,
                             fmt::TIME_SERIES_INDEX_BYTES),
                rolling);
            if (rolling == stored_body) crc_trusted = i + 1;
          }
        }
      }
    }

    const std::size_t entry_bytes = index_bytes.size() - fmt::UNIVERSAL_HEADER_BYTES;
    const std::size_t n_entries = entry_bytes / fmt::TIME_SERIES_INDEX_BYTES;
    std::span<const ui1> entries =
        std::span<const ui1>(index_bytes).subspan(fmt::UNIVERSAL_HEADER_BYTES);

    // --- how much of the index does the data actually support? ---
    // Only the last surviving entry is needed to carry on from; the survivors
    // themselves are re-read from the .tidx if the segment is written.
    std::size_t kept = 0;
    fmt::TimeSeriesIndex last_kept;
    si8 previous_end = fmt::UNIVERSAL_HEADER_BYTES;
    bool index_ahead = false;
    for (std::size_t i = 0; i < n_entries; ++i) {
      auto e = fmt::TimeSeriesIndex::parse(
          entries.subspan(i * fmt::TIME_SERIES_INDEX_BYTES, fmt::TIME_SERIES_INDEX_BYTES));
      bool sane = e.file_offset >= fmt::UNIVERSAL_HEADER_BYTES &&
                  e.file_offset >= previous_end - 1 &&
                  e.number_of_samples != fmt::UI4_NO_ENTRY &&
                  e.block_bytes != fmt::UI4_NO_ENTRY &&
                  static_cast<std::uint64_t>(e.file_offset) + e.block_bytes <= data_size;
      if (sane && i >= crc_trusted) {
        // No CRC vouches for this entry: keep it only if the block header at
        // its offset agrees (the header's own CRC is checked by the decoder).
        ctl.check();
        const auto head = read_range(data_in, static_cast<std::uint64_t>(e.file_offset),
                                     fmt::RED_BLOCK_HEADER_BYTES);
        sane = head.size() == fmt::RED_BLOCK_HEADER_BYTES;
        if (sane) {
          const auto h = fmt::RedBlockHeader::parse(head);
          sane = h.start_time == e.start_time && h.number_of_samples == e.number_of_samples &&
                 h.block_bytes == e.block_bytes;
        }
        if (sane && i > 0 && e.start_sample != last_kept.start_sample +
                                                   static_cast<si8>(last_kept.number_of_samples))
          sane = false;
      }
      if (!sane) {                       // this entry and everything after it
        index_ahead = true;              // describes bytes that are not there
        break;
      }
      previous_end = e.file_offset + e.block_bytes;
      last_kept = e;
      ++kept;
    }
    // The LAST append of a channel's last segment is the one a power cut can
    // tear without the index noticing: its entries landed, but some of the
    // .tdat pages they point at did not (they read back as zeros, or as old
    // bytes past a size the file system already extended). So the blocks of
    // that append are CRC-checked against their entries, walking back from
    // the end to the append's first block (the writer flags it as a
    // discontinuity), at most 64 MiB of data — the bytes just written, not
    // the file. Everything from the first bad block on is treated as never
    // written: its entries are dropped, and the data from there on is a torn
    // tail (backed up, then truncated), as after any interrupted append.
    if (sd.is_last && kept > 0) {
      constexpr std::uint64_t kVerifyBytes = 64ull << 20;
      std::uint64_t verified = 0;
      std::size_t first_bad = kept;
      for (std::size_t j = kept; j-- > 0;) {
        ctl.check();
        const auto e = fmt::TimeSeriesIndex::parse(
            entries.subspan(j * fmt::TIME_SERIES_INDEX_BYTES, fmt::TIME_SERIES_INDEX_BYTES));
        fmt::RedBlockHeader h;
        const bool ok = block_looks_real(data_in, data_size,
                                         static_cast<std::uint64_t>(e.file_offset), h) &&
                        h.start_time == e.start_time &&
                        h.number_of_samples == e.number_of_samples &&
                        h.block_bytes == e.block_bytes;
        if (!ok) first_bad = j;
        verified += e.block_bytes;
        if ((e.red_block_flags & fmt::RedBlockHeader::DISCONTINUITY_MASK) || verified >= kVerifyBytes)
          break;
      }
      if (first_bad < kept) {
        kept = first_bad;
        index_ahead = true;
        if (kept > 0)
          last_kept = fmt::TimeSeriesIndex::parse(entries.subspan(
              (kept - 1) * fmt::TIME_SERIES_INDEX_BYTES, fmt::TIME_SERIES_INDEX_BYTES));
      }
    }
    const si8 dropped = static_cast<si8>(n_entries) - static_cast<si8>(kept);

    // --- does the data run past the index? recover those blocks ---
    std::vector<fmt::TimeSeriesIndex> rebuilt;
    si8 recovered = 0;
    std::uint64_t offset = kept == 0 ? fmt::UNIVERSAL_HEADER_BYTES
                                     : static_cast<std::uint64_t>(last_kept.file_offset +
                                                                  last_kept.block_bytes);
    // Sample numbering is CHANNEL-wide (the writer numbers across segments): a
    // segment whose index is rebuilt from its first block continues from the
    // start sample its own .tmet declares (the previous segment's end for a
    // later segment), not from 0 — an append after it, or recovery of a later
    // torn append, would otherwise meet a numbering seam.
    si8 next_sample = 0;
    if (kept > 0) {
      next_sample = last_kept.start_sample + last_kept.number_of_samples;
    } else if (offset < data_size) {
      try {
        std::vector<ui1> record;
        if (tmet_rebuild) {
          record = tmet_rebuild->bytes;
        } else {
          std::ifstream in(seg / (base + ".tmet"), std::ios::binary);
          record = read_range(in, 0, fmt::METADATA_FILE_BYTES);
        }
        const si8 declared = load_time_series_metadata(record, password).section2.start_sample;
        if (declared > 0) next_sample = declared;
      } catch (const std::exception&) { /* unreadable: number from 0 as before */ }
    }
    fmt::RedBlockHeader header;
    while (offset < data_size) {
      // Each candidate block is read whole to verify its CRC, so this is the
      // loop that runs long on a big unindexed tail: stop between blocks.
      ctl.check();
      if (!block_looks_real(data_in, data_size, offset, header)) break;
      fmt::TimeSeriesIndex e;
      e.file_offset = static_cast<si8>(offset);
      e.start_time = header.start_time;
      e.start_sample = next_sample;
      e.number_of_samples = header.number_of_samples;
      e.block_bytes = header.block_bytes;
      // The extrema are per-block statistics a reader recomputes anyway; the
      // index copy is informational. Leaving them at the sentinel would trip
      // index.entry-counts, so use the block's own conservative bounds.
      e.maximum_sample_value = 0;
      e.minimum_sample_value = 0;
      e.red_block_flags = header.flags;
      rebuilt.push_back(e);
      next_sample += header.number_of_samples;
      offset += header.block_bytes;
      ++recovered;
      progress.advance_to(seg_base + index_bytes.size() + offset);
    }
    const si8 tail = static_cast<si8>(data_size) - static_cast<si8>(offset);
    const std::size_t n_after = kept + rebuilt.size();

    // The blocks can line up perfectly and the segment still be mid-update: a
    // crash after the .tidx header was published but before the .tdat header
    // was patched leaves the .tdat declaring the OLD count. meflib clamps the
    // block count down to that field, so the segment reads short — and
    // looking only at block alignment would report "nothing to do" and skip
    // the repair that fixes it.
    si8 stale_headers = 0;
    const si8 index_count = byteio::read<si8>(index_bytes, 32);
    std::vector<ui1> data_head;
    try {
      data_head = read_range(data_in, 0, fmt::UNIVERSAL_HEADER_BYTES);
    } catch (const std::exception&) { /* handled by the size check above */ }
    // A .tdat header that is not a .tdat header at all (zeroed pages of a
    // full-size file: the header page never reached the disk) is REPLACED by
    // one taken from this segment's .tmet — type, names, segment number, level
    // UUID — not just re-sealed, or the validator would call a header of
    // zeros "ok". A header that only fails its CRC but still says "tdat" keeps
    // its fields and is re-sealed as before.
    if (data_header_override.empty() && data_head.size() >= fmt::UNIVERSAL_HEADER_BYTES &&
        fmt::UniversalHeader::parse(data_head).file_type_string != "tdat") {
      std::vector<ui1> source;
      if (tmet_rebuild) {
        source.assign(tmet_rebuild->bytes.begin(),
                      tmet_rebuild->bytes.begin() + fmt::UNIVERSAL_HEADER_BYTES);
      } else {
        std::ifstream in(seg / (base + ".tmet"), std::ios::binary);
        source = read_range(in, 0, fmt::UNIVERSAL_HEADER_BYTES);
        if (!header_crc_ok(source)) source.clear();
      }
      if (!source.empty()) {
        data_header_override = synthesize_header(source, "tdat");
        data_head = data_header_override;
        pre_action += std::string(pre_action.empty() ? "" : "; ") +
                      "the .tdat header was not a data-file header (zeroed or foreign): " +
                      (apply ? "rebuilt" : "would be rebuilt") + " from this segment's .tmet";
      }
    }
    const si8 data_count = data_head.size() >= fmt::UNIVERSAL_HEADER_BYTES
                               ? byteio::read<si8>(data_head, 32)
                               : 0;
    if (index_count != static_cast<si8>(n_after)) ++stale_headers;
    if (data_count != static_cast<si8>(n_after)) ++stale_headers;

    // The end time (plan_end_times), decided in the scan so a dry run reports
    // it. The .tmet is read only where it can matter, so a healthy session
    // costs no metadata reads beyond one 1 KB header per CHANNEL:
    //  * a segment recovery writes anyway — its offset decides whether the
    //    end may be touched at all;
    //  * a channel's LAST segment, the only one an append extends, whose
    //    .tmet update a crash can lose: its header (1 KB) is read, and the
    //    whole record only if the three files' end times disagree.
    EndPlan end_plan;
    std::vector<ui1> tmet_header;
    std::string end_note;
    const fsys::path tmet = seg / (base + ".tmet");
    const bool needs_work =
        dropped != 0 || recovered != 0 || tail != 0 || stale_headers != 0 || index_crc_bad;
    const bool is_last = sd.is_last;
    if (!tmet_rebuild && n_after > 0 && (needs_work || is_last) &&
        data_head.size() >= fmt::UNIVERSAL_HEADER_BYTES) {
      try {
        std::ifstream tmet_in(tmet, std::ios::binary);
        if (!tmet_in) throw IoError("cannot open " + tmet.string());
        tmet_header = read_range(tmet_in, 0, fmt::UNIVERSAL_HEADER_BYTES);
        if (tmet_header.size() < fmt::UNIVERSAL_HEADER_BYTES)
          throw FormatError("shorter than a universal header");
        const si8 tmet_end = byteio::read<si8>(tmet_header, 24);
        const bool ends_disagree = tmet_end != byteio::read<si8>(index_bytes, 24) ||
                                   tmet_end != byteio::read<si8>(data_head, 24);
        if (needs_work || ends_disagree) {
          const std::vector<ui1> record = read_range(tmet_in, 0, fmt::METADATA_FILE_BYTES);
          const TimeSeriesMetadata md = load_time_series_metadata(record, password);
          const si8 last_start = rebuilt.empty() ? last_kept.start_time : rebuilt.back().start_time;
          const ui4 last_samples =
              rebuilt.empty() ? last_kept.number_of_samples : rebuilt.back().number_of_samples;
          if (!md.section3_available) {
            end_note = "if this session has a recording-time offset, its end time could not be "
                       "checked: section 3 is locked (pass the level-2 password); without one, "
                       "repair_session updates it";
          } else {
            end_plan = plan_end_times(md, {tmet_header.data(), index_bytes.data(), data_head.data()},
                                      last_start, last_samples, recovered != 0, end_note);
          }
        }
      } catch (const std::exception& e) {
        if (needs_work)
          end_note = std::string("the segment metadata could not be read (") + e.what() + ")";
      }
    }
    const si8 stale_end = end_plan.stale();

    if (!end_note.empty())
      report.notes.push_back(description + ": end time left as is: " + end_note);
    if (tmet_rebuild) {
      if (apply) tmet_rebuilds.push_back(std::move(*tmet_rebuild));
      rebuilt_tmet_segments.push_back({channel, segment_number});
    }
    if (!needs_work && stale_end == 0) {
      if (!pre_action.empty()) {
        RecoveredSegment out;
        out.channel = channel;
        out.segment_number = segment_number;
        out.path = description;
        out.action = pre_action + "; its declarations are then re-derived from the index";
        report.segments.push_back(std::move(out));
        continue;
      }
      if (is_last) last_segments.push_back({channel, segment_number});
      continue;  // segment is fine (its declarations are checked below)
    }

    RecoveredSegment out;
    out.channel = channel;
    out.segment_number = segment_number;
    out.path = description;
    out.blocks_before = static_cast<si8>(n_entries);
    out.blocks_after = static_cast<si8>(n_after);
    out.blocks_recovered = recovered;
    out.blocks_dropped = dropped;
    out.tdat_bytes_dropped = tail;
    std::string what;
    if (!pre_action.empty()) what += pre_action + "; ";
    if (index_crc_bad)
      what += "the block index failed its CRC (its header and entries are from different "
              "writes); kept " + std::to_string(kept) +
              " entr(ies) that match their RED block headers; ";
    if (recovered)
      what += "recovered " + std::to_string(recovered) +
              " block(s) that reached .tdat but were never indexed";
    if (dropped) {
      if (!what.empty()) what += "; ";
      what += "dropped " + std::to_string(dropped) +
              " index entr(ies) pointing past the end of .tdat or at blocks that never "
              "fully reached it";
    }
    if (tail) {
      if (!what.empty()) what += "; ";
      what += "dropped " + std::to_string(tail) +
              " trailing byte(s) that do not form a whole block";
    }
    if (index_ahead && !dropped) what += " (index stops short of the data)";
    if (stale_headers) {
      if (!what.empty()) what += "; ";
      what += "the universal header entry count was stale in " +
              std::to_string(stale_headers) + " file(s) (index " + std::to_string(index_count) +
              ", data " + std::to_string(data_count) + ", really " + std::to_string(n_after) +
              ") — a reader clamps the block count down to it and would read the segment short";
    }
    if (stale_end) {
      if (!what.empty()) what += "; ";
      what += "the end time was stale in " + std::to_string(stale_end) +
              " universal header(s) and is set from the last block (the segment has a "
              "recording-time offset, where repair_session leaves times alone)";
    }
    while (what.size() >= 2 && what.compare(what.size() - 2, 2, "; ") == 0)
      what.resize(what.size() - 2);
    what += apply ? "; then its declarations are re-derived from the recovered index "
                    "(repair_session, this segment only)"
                  : "; then its declarations would be re-derived from the recovered index "
                    "(repair_session, this segment only — which checks, depends on the "
                    "recovered index)";
    out.action = what;
    report.segments.push_back(std::move(out));

    if (!apply) continue;
    PlannedRecovery p;
    p.channel_dir = sd.channel_dir;
    p.tidx = tidx;
    p.tdat = tdat;
    p.channel = channel;
    p.base = base;
    p.description = description;
    p.segment_number = segment_number;
    p.report_index = report.segments.size() - 1;
    p.index_size = index_size_on_disk;
    p.index_header = index_head_on_disk;
    p.index_header_override = index_header_override;
    p.data_header_override = data_header_override;
    p.data_size = data_size;
    p.kept_from_index = kept;
    p.rebuilt = std::move(rebuilt);
    p.data_end = offset;
    p.tail = tail;
    p.end = end_plan;
    p.tdat_header_current = data_count == static_cast<si8>(n_after);
    p.tmet = tmet;
    p.tmet_header = std::move(tmet_header);
    plan.push_back(std::move(p));
  }
  // ---- declarations of the segments whose index and data agree ------------
  // 2.0 flushes only the .tmet, so after a power cut the published .tmet can
  // be NEWER than the index and data it describes (the common case on ext4:
  // the rename is committed within seconds, the in-place extensions later),
  // or OLDER (the rename itself was lost). Either way the index and the data
  // agree, the loop above sees nothing, and the segment declares blocks and
  // samples it does not hold (readers report an end with NaN behind it, and
  // a recorder re-sending its buffer is refused as overlapping) or fails to
  // declare ones it does (meflib reads the segment short). Only an append
  // extends a segment, so only each channel's LAST segment is checked: the
  // validator's count checks, metadata + index only (no block reads).
  std::vector<std::pair<std::string, int>> declaration_targets;
  if (!last_segments.empty()) {
    ValidateOptions vo;
    vo.password = password;
    vo.exact_difference_bytes = false;
    vo.check_ids = {"index.block-count", "index.sample-count", "header.entry-count"};
    std::set<std::string> chs;
    std::set<int> nums;
    for (const auto& [c, n] : last_segments) {
      chs.insert(c);
      nums.insert(n);
    }
    vo.channels.assign(chs.begin(), chs.end());
    vo.segments.assign(nums.begin(), nums.end());
    Control inner;
    inner.cancel = ctl.cancel;  // cancel only: progress is this call's own
    const Report v = validate_session(path, vo, inner);
    std::map<std::pair<std::string, int>, std::vector<const Finding*>> by_seg;
    for (const auto& f : v.findings) {
      const auto key = std::make_pair(f.channel, f.segment_number);
      if (std::find(last_segments.begin(), last_segments.end(), key) == last_segments.end())
        continue;
      if (f.check_id.rfind("crc.", 0) == 0) continue;  // not ours to fix
      by_seg[key].push_back(&f);
    }
    for (const auto& [key, fs] : by_seg) {
      RecoveredSegment out;
      out.channel = key.first;
      out.segment_number = key.second;
      out.path = fs.front()->path;
      std::string what =
          "the metadata (.tmet) does not describe what the index and data hold — the "
          "segment's last append was published without its data, or the other way round: ";
      for (std::size_t i = 0; i < fs.size(); ++i) what += (i ? "; " : "") + fs[i]->message;
      // The preview is the exact list the apply passes to repair_session:
      // nothing changes this segment's index before it.
      out.declarations = segment_repairable_ids(path, password, key);
      what += std::string(apply ? " — re-derived" : " — would be re-derived") +
              " from the index (repair_session, this segment only: ";
      for (std::size_t i = 0; i < out.declarations.size(); ++i)
        what += (i ? ", " : "") + out.declarations[i];
      what += ")";
      out.action = what;
      report.segments.push_back(std::move(out));
      declaration_targets.push_back(key);
    }
  }

  // The last point at which a cancel is honoured. Nothing has been written.
  ctl.check();
  // finish() belongs HERE, before anything is written: progress measures the
  // pass, and a callback that raises at (total, total) must stop the call
  // while there is still nothing to lose. Called after the write phase, a
  // raise there discarded the report of what had just been written.
  progress.finish();

  // ---- phase 2: write. Rebuilt .tmet files go first (the index/data
  // recovery and the declaration step below read them). -----------------
  for (const auto& tr : tmet_rebuilds) {
    if (backup && tr.existed) {
      report.backup_root = backup_root.string();
      const auto rel = tr.path.parent_path().parent_path().stem().string() + "/" +
                       tr.path.filename().string();
      back_up_file(tr.path, backup_root, rel);
    }
    write_all(tr.path, tr.bytes);
    if (!tr.adopted_from.empty()) {
      std::error_code ignored;
      fsys::remove(tr.adopted_from, ignored);  // its content is the .tmet now
    }
    const fsys::path channel_dir = tr.path.parent_path().parent_path();
    if (const auto last = detail::last_segment_dir(channel_dir);
        last && *last != tr.path.parent_path())
      channels_to_stamp.insert(channel_dir);
  }

  // ---- phase 2: write. Not cancellable: each write is a whole file replaced
  // by rename or a 1024-byte header patch, and stopping between segments would
  // leave the session half-recovered. ---------------------------------------
  std::vector<bool> withdrawn(report.segments.size(), false);
  for (const auto& p : plan) {
    const fsys::path& tidx = p.tidx;
    const fsys::path& tdat = p.tdat;
    const std::string& channel = p.channel;
    const std::string& base = p.base;

    // The scan and the write are further apart than they once were, so check
    // that the files are still the ones that were scanned. A writer appending
    // in between moves the sizes; acting on the stale plan would then drop
    // what it had just written.
    std::vector<ui1> index_bytes;
    std::ifstream data_in;
    bool unchanged = false;
    {
      std::error_code ec;
      const auto now_data = fsys::file_size(tdat, ec);
      if (!ec && static_cast<std::uint64_t>(now_data) == p.data_size) {
        if (fsys::exists(tidx, ec)) index_bytes = read_all(tidx);
        unchanged = index_bytes.size() == p.index_size &&
                    std::equal(p.index_header.begin(), p.index_header.end(),
                               index_bytes.begin());
      }
    }
    if (!unchanged) {
      report.skipped.push_back(p.description +
                               ": changed while recovery was scanning (is a writer running?); "
                               "nothing was written to this segment — run recovery again");
      withdrawn[p.report_index] = true;
      continue;
    }
    data_in.rdbuf()->pubsetbuf(nullptr, 0);
    data_in.open(tdat, std::ios::binary);
    if (!data_in) throw IoError("cannot open file: " + tdat.string());

    std::vector<fmt::TimeSeriesIndex> keep;
    keep.reserve(p.kept_from_index + p.rebuilt.size());
    {
      std::span<const ui1> entries =
          index_bytes.size() >= fmt::UNIVERSAL_HEADER_BYTES
              ? std::span<const ui1>(index_bytes).subspan(fmt::UNIVERSAL_HEADER_BYTES)
              : std::span<const ui1>();
      for (std::size_t i = 0; i < p.kept_from_index && index_bytes.size() >= fmt::UNIVERSAL_HEADER_BYTES;
           ++i)
        keep.push_back(fmt::TimeSeriesIndex::parse(
            entries.subspan(i * fmt::TIME_SERIES_INDEX_BYTES, fmt::TIME_SERIES_INDEX_BYTES)));
      keep.insert(keep.end(), p.rebuilt.begin(), p.rebuilt.end());
    }
    const si8 tail = p.tail;
    const std::uint64_t offset = p.data_end;

    if (backup) {
      // Back up what CHANGES, not the whole file. The .tidx is small. The
      // .tdat may be tens of gigabytes and recovery only ever rewrites its
      // 1024-byte header and drops a trailing fragment shorter than one
      // block — so those are what get saved. Copying 30 GB to undo 200 bytes
      // would make the tool unusable on exactly the files it is for.
      report.backup_root = backup_root.string();
      if (fsys::exists(tidx)) back_up_file(tidx, backup_root, channel + "/" + base + ".tidx");
      const auto head = read_range(data_in, 0, fmt::UNIVERSAL_HEADER_BYTES);
      back_up_bytes(backup_root, channel + "/" + base + ".tdat.header", head);
      if (tail > 0) {
        const auto frag = read_range(data_in, offset, static_cast<std::size_t>(tail));
        back_up_bytes(backup_root, channel + "/" + base + ".tdat.tail", frag);
      }
    }

    std::vector<ui1> new_index = !p.index_header_override.empty()
                                     ? p.index_header_override
                                     : std::vector<ui1>(index_bytes.begin(),
                                                        index_bytes.begin() +
                                                            fmt::UNIVERSAL_HEADER_BYTES);
    std::vector<ui1> entry(fmt::TIME_SERIES_INDEX_BYTES);
    for (const auto& e : keep) {
      e.serialize(entry);
      new_index.insert(new_index.end(), entry.begin(), entry.end());
    }
    {
      auto uh = fmt::UniversalHeader::parse(new_index);
      uh.number_of_entries = static_cast<si8>(keep.size());
      uh.maximum_entry_size = fmt::TIME_SERIES_INDEX_BYTES;
      // At a known non-zero offset only plan_end_times moves an end (never
      // earlier, in the header's own convention). Otherwise as before: the
      // last block's start, which the follow-up repair corrects at offset 0.
      if (p.end.rto_nonzero) {
        if (p.end.new_end[1]) uh.end_time = *p.end.new_end[1];
      } else if (!keep.empty()) {
        uh.end_time = keep.back().start_time;
      }
      fmt::provenance::stamp_modified(uh, fmt::provenance::Operation::Recovery);
      uh.serialize(new_index);
      const ui4 body = crc::calculate(
          std::span<const ui1>(new_index).subspan(fmt::UNIVERSAL_HEADER_BYTES));
      byteio::write<ui4>(new_index, 4, body);
      byteio::write<ui4>(new_index, 0,
                         crc::calculate(std::span<const ui1>(new_index)
                                            .subspan(4, fmt::UNIVERSAL_HEADER_BYTES - 4)));
    }
    write_all(tidx, new_index);

    if (tail > 0) {
      // Truncate only a fragment that is not a whole, CRC-valid block. Real
      // blocks were indexed above rather than discarded.
      std::error_code ec;
      fsys::resize_file(tdat, static_cast<std::uintmax_t>(offset), ec);
      if (ec) throw IoError("cannot truncate " + tdat.string() + ": " + ec.message());
    }
    {
      // The .tdat header's entry count must match the index it is paired with.
      std::vector<ui1> head = !p.data_header_override.empty()
                                  ? p.data_header_override
                                  : read_range(data_in, 0, fmt::UNIVERSAL_HEADER_BYTES);
      auto uh = fmt::UniversalHeader::parse(head);
      uh.number_of_entries = static_cast<si8>(keep.size());
      if (p.end.rto_nonzero) {
        if (p.end.new_end[2]) uh.end_time = *p.end.new_end[2];
      } else if (!keep.empty()) {
        uh.end_time = keep.back().start_time;
      }
      fmt::provenance::stamp_modified(uh, fmt::provenance::Operation::Recovery);
      uh.serialize(head);
      // Truncated, blocks recovered past what the stored CRC was taken over,
      // or a header older than the last append (its entry count was stale,
      // and the writer patches count and CRC together): the stored CRC no
      // longer describes the body, so say "never computed" rather than
      // leaving a confidently wrong value. Otherwise the fix is header-only
      // (end time, dropped index entries), the body is as it was, and its
      // CRC — on a file that can be tens of GB — is kept.
      if (p.tail > 0 || !p.rebuilt.empty() || !p.tdat_header_current)
        byteio::write<ui4>(head, 4, fmt::CRC_NO_ENTRY);
      byteio::write<ui4>(
          head, 0,
          crc::calculate(std::span<const ui1>(head).subspan(4, fmt::UNIVERSAL_HEADER_BYTES - 4)));
      std::fstream f(tdat, std::ios::binary | std::ios::in | std::ios::out);
      if (!f) throw IoError("cannot open for header update: " + tdat.string());
      f.write(reinterpret_cast<const char*>(head.data()), fmt::UNIVERSAL_HEADER_BYTES);
      f.flush();
      if (!f) throw IoError("header update failed: " + tdat.string());
    }
    (void)detail::fsync_file(tdat.string());
    if (p.end.new_end[0]) {
      // The .tmet universal header: header only (the body CRC is unchanged
      // because the body is), the whole file rewritten by rename like the
      // .tidx, any foreign padding past the fixed-length record kept. Skipped,
      // with a note, if the header moved since the scan (a writer?).
      std::vector<ui1> tmet_bytes = read_all(p.tmet);
      if (tmet_bytes.size() < fmt::METADATA_FILE_BYTES ||
          !std::equal(p.tmet_header.begin(), p.tmet_header.end(), tmet_bytes.begin())) {
        report.notes.push_back(p.description + ": end time left as is: the segment metadata "
                                               "changed while recovery was running");
      } else {
        if (backup)
          back_up_bytes(backup_root, channel + "/" + base + ".tmet.header", p.tmet_header);
        auto uh = fmt::UniversalHeader::parse(tmet_bytes);
        uh.end_time = *p.end.new_end[0];
        fmt::provenance::stamp_modified(uh, fmt::provenance::Operation::Recovery);
        uh.serialize(tmet_bytes);
        byteio::write<ui4>(tmet_bytes, 0,
                           crc::calculate(std::span<const ui1>(tmet_bytes)
                                              .subspan(4, fmt::UNIVERSAL_HEADER_BYTES - 4)));
        write_all(p.tmet, tmet_bytes);
      }
    }
    if (const auto last = detail::last_segment_dir(p.channel_dir);
        last && detail::segment_number_of(last->stem().string()) != p.segment_number)
      channels_to_stamp.insert(p.channel_dir);
  }
  // Dataless segments cut off by a power cut (see the scan): moved to the
  // backup (or deleted without one); a channel folder left without segments
  // goes with them — it held no sample.
  for (const auto& rm : removals) {
    std::error_code ec;
    if (backup) {
      report.backup_root = backup_root.string();
      const fsys::path dest = backup_root / rm.channel / rm.dir.filename();
      fsys::create_directories(dest.parent_path());
      fsys::remove_all(dest, ec);
      fsys::rename(rm.dir, dest, ec);
    } else {
      fsys::remove_all(rm.dir, ec);
    }
    if (ec) {
      report.skipped.push_back(report.segments[rm.report_index].path +
                               ": could not be removed: " + ec.message());
      withdrawn[rm.report_index] = true;
      continue;
    }
    bool any = false;
    for (const auto& e : fsys::directory_iterator(rm.channel_dir, ec))
      if (e.is_directory() && e.path().extension() == ".segd") any = true;
    if (!any && !ec) {
      if (backup) {
        // Keep whatever else the folder held (channel records) in the backup.
        const fsys::path dest = backup_root / rm.channel / "channel-folder";
        fsys::remove_all(dest, ec);
        fsys::rename(rm.channel_dir, dest, ec);
      } else {
        fsys::remove_all(rm.channel_dir, ec);
      }
      if (!ec)
        report.notes.push_back(rm.channel + ": the channel held no other segment, so its "
                                            "folder was removed too");
    }
  }

  for (const auto& p : plan)
    if (!withdrawn[p.report_index])
      declaration_targets.insert(declaration_targets.begin(), {p.channel, p.segment_number});
  for (const auto& k : rebuilt_tmet_segments) declaration_targets.push_back(k);
  if (apply) rederive_declarations(path, password, backup, declaration_targets, report);

  if (std::find(withdrawn.begin(), withdrawn.end(), true) != withdrawn.end()) {
    std::vector<RecoveredSegment> still;
    for (std::size_t i = 0; i < report.segments.size(); ++i)
      if (!withdrawn[i]) still.push_back(std::move(report.segments[i]));
    report.segments = std::move(still);
  }

  // segment-repaired: a NON-last segment changed, so the channel's last
  // segment records it (last_segment.hpp). Same backup and durability rules
  // as recovery's own header writes; a stamp that cannot be applied is a note.
  for (const auto& channel_dir : channels_to_stamp) {
    const auto stamp = detail::prepare_last_segment_stamp(channel_dir);
    const std::string channel = channel_dir.stem().string();
    if (!stamp.ready) {
      report.notes.push_back(channel + ": a segment was recovered, but the last segment could "
                                       "not record it: " + stamp.note +
                             detail::STAMP_SKIPPED_CONSEQUENCE);
      continue;
    }
    if (backup) {
      report.backup_root = backup_root.string();
      back_up_bytes(backup_root, channel + "/" + stamp.segment_stem + ".tmet.header",
                    stamp.old_header);
    }
    detail::write_last_segment_stamp(stamp);
    report.notes.push_back(channel + ": last segment stamped segment-repaired (header only): " +
                           stamp.tmet.string());
  }
  recover_record_files(root, apply, backup, backup_root, report, ctl);
  report.applied = apply;
  return report;
}

}  // namespace mef3io
