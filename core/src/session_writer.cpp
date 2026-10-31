// mef3io — high-level session writer.
#include "operation_marker.hpp"
#include "mef3io/session_writer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <stdexcept>

#include <fstream>
#include <memory>

#include "mef3io/errors.hpp"
#include "mef3io/metadata.hpp"
#include "mef3io/source.hpp"
#include "mef3io/writer.hpp"
#include "durability.hpp"
#include "mef3io/time.hpp"
#include "last_segment.hpp"
#include "session_layout.hpp"
#include "clamp.hpp"
#include "number_text.hpp"

namespace fs = std::filesystem;

namespace mef3io {
namespace {

bool int32_range_ok(sf8 xmin, sf8 xmax, sf8 alpha) {
  // BOTH ends must fit (INT32_MIN excluded: RED's NaN). mef_tools'
  // check_int32_dynamic_range called a range bad only when both ends
  // overflowed, so one-sided data wrapped silently; quantize_to_int32 now
  // refuses that, and the inference must not choose it.
  const sf8 lim = static_cast<sf8>(std::numeric_limits<si4>::max());
  return std::abs(std::round(xmin * alpha)) <= lim && std::abs(std::round(xmax * alpha)) <= lim;
}

std::string segment_name(const std::string& channel, int seg) {
  char buf[8];
  std::snprintf(buf, sizeof(buf), "%06d", seg);
  return channel + "-" + buf;
}

}  // namespace

int infer_precision(std::span<const sf8> data) {
  // Mean absolute first difference, ignoring NaN.
  sf8 sum = 0.0;
  si8 count = 0;
  sf8 prev = std::numeric_limits<sf8>::quiet_NaN();
  // +-inf is ignored like NaN: it is clamped on write (quantize_to_int32),
  // and must not drag the precision of the ordinary samples down to 0.
  for (sf8 v : data) {
    if (std::isfinite(v) && std::isfinite(prev)) {
      sum += std::abs(v - prev);
      ++count;
    }
    prev = v;
  }
  sf8 mean_diff = count ? sum / static_cast<sf8>(count) : 0.0;

  int precision = 0;
  while (mean_diff < 1000.0 && mean_diff != 0.0) {
    ++precision;
    mean_diff *= 10.0;
  }

  sf8 dmax = -std::numeric_limits<sf8>::infinity(), dmin = std::numeric_limits<sf8>::infinity();
  for (sf8 v : data)
    if (std::isfinite(v)) {
      dmax = std::max(dmax, v);
      dmin = std::min(dmin, v);
    }
  if (!std::isfinite(dmax)) return 0;  // nothing finite

  sf8 alpha = std::pow(10.0, precision);
  while (!int32_range_ok(dmin, dmax, alpha) && precision != 0) {
    --precision;
    alpha = std::pow(10.0, precision);
  }
  return precision;
}

std::vector<si4> quantize_to_int32(std::span<const sf8> data, int precision,
                                   ClampCount* clamped) {
  const sf8 scale = std::pow(10.0, precision);
  std::vector<si4> out(data.size());
  ClampCount c;
  for (std::size_t i = 0; i < data.size(); ++i) {
    if (std::isnan(data[i])) {
      out[i] = 0;  // gap position, not stored
      continue;
    }
    sf8 rounded = std::round(data[i] * scale);  // round(data,prec)*10^prec == round(data*10^prec)
    // Never wrap (it used to): a count beyond int32, or +-inf, is CLAMPED to
    // the nearest storable count (detail::clamp_count; maintainer decision,
    // #149) and counted, so the caller can say so.
    if (!(std::abs(rounded) <= kMaxStorableCount)) {
      out[i] = detail::clamp_count(rounded, std::isfinite(data[i]), c);
      continue;
    }
    out[i] = static_cast<si4>(rounded);
  }
  if (clamped) *clamped = c;
  return out;
}

SessionWriter::SessionWriter(const std::string& mefd_path, bool overwrite, std::string password_1,
                             std::string password_2)
    : mefd_path_(mefd_path),
      password_1_(std::move(password_1)),
      password_2_(std::move(password_2)) {
  // Tar archives are read-only sessions. Refuse them before the destructive
  // overwrite/remove_all below can ever touch one.
  if (path_has_suffix(mefd_path_, ".tar"))
    throw IoError("cannot write into a tar archive (tar sessions are read-only): " + mefd_path_ +
                  "; write a .mefd directory and pack it with archive_session()");
  // Same naming contract as the reader: only clearly-marked session
  // directories are ever created or overwritten.
  if (!path_has_suffix(mefd_path_, ".mefd"))
    throw IoError("session directory must end with .mefd: " + mefd_path_);
  if (fs::exists(mefd_path_) && !fs::is_directory(mefd_path_))
    throw IoError("session path exists and is not a directory: " + mefd_path_);
  // Mid-rename (#72): no write, not even an overwrite, until it is finished
  // or rolled back — its journal would otherwise restore stale headers.
  detail::refuse_if_mid_operation(mefd_path_);
  if (overwrite && fs::exists(mefd_path_)) fs::remove_all(mefd_path_);
  // Held data before this writer: its new segments/channels are flushed.
  session_preexisted_ = fs::exists(mefd_path_);
  fs::create_directories(mefd_path_);
  session_name_ = fs::path(mefd_path_).stem().string();

  // Adopt existing channels/segments so appends continue where the session
  // left off. Metadata is read back lazily (hydrate) on the first write.
  for (const auto& entry : fs::directory_iterator(mefd_path_)) {
    if (!entry.is_directory() || entry.path().extension() != ".timd") continue;
    std::string ch = entry.path().stem().string();
    int nseg = 0;
    long long last = -1;
    std::string last_stem, last_name;
    for (const auto& s : fs::directory_iterator(entry.path())) {
      if (!s.is_directory() || s.path().extension() != ".segd") continue;
      // A segment directory with no metadata is not a segment: it is what an
      // earlier refused or crashed write left behind, and adopting it would
      // wedge the channel (every later write: "cannot open segment metadata
      // for append"). Ignore it and say so.
      if (!fs::exists(s.path() / (s.path().stem().string() + ".tmet"))) {
        const std::string entry_text =
            s.path().string() + ": segment directory without metadata (.tmet) ignored";
        note_unreadable(entry_text);
        empty_segment_dirs_[s.path().string()] = entry_text;
        continue;
      }
      ++nseg;
      // The last segment by the SAME rule as last_segment_dir (number, then
      // folder name), keeping its real name for the append (#83 item 5).
      const long long number = detail::segment_number_of(s.path().stem().string());
      const std::string name = s.path().filename().string();
      if (last_stem.empty() || std::make_pair(number, name) > std::make_pair(last, last_name)) {
        last = number;
        last_name = name;
        last_stem = s.path().stem().string();
      }
    }
    // Segments are addressed by the HIGHEST real segment NUMBER, never by
    // how many there are: with a directory without metadata in the middle
    // (0 good, 1 empty, 2 good) the count is 2 but the last segment is 2, and
    // using the count appended to the empty one. A new segment is last + 1,
    // so it never collides with or reorders a real one; an empty directory
    // past every real segment is simply reused.
    channels_[ch].n_segments = nseg;
    channels_[ch].last_segment = static_cast<int>(last);
    channels_[ch].last_segment_stem = last_stem;
    channels_[ch].hydrated = (nseg == 0);
  }
}

// A Writer is often held open for a whole acquisition. If the session was
// renamed or moved in the meantime (#72), writing to the old path would
// silently start a NEW session there; and while a rename is running its
// journal would restore headers older than this write. Refuse both. One stat
// per write call.
void SessionWriter::check_still_writable() const {
  if (!fs::is_directory(mefd_path_))
    throw IoError("the session folder is no longer there (renamed or moved since this writer "
                  "opened it?): " + mefd_path_ + "; open a writer on its new path");
  detail::refuse_if_mid_operation(mefd_path_);
  // wait_for_disk promises "returned = on disk": after a failed flush no later
  // write may return normally (the earlier data may be lost).
  if (wait_for_disk_ && flush_failure_)
    throw IoError(*flush_failure_ +
                  " (an earlier flush failed: data written since the last successful flush may "
                  "be lost; " + std::string(flush_mode_ == FlushMode::Close ? "the session must be rewritten (flush=\"close\" sessions cannot be reliably recovered)" : "run recover_session") + ")");
}

void SessionWriter::hydrate(const std::string& channel, ChannelState& st) {
  if (st.hydrated) return;
  st.hydrated = true;
  const std::string base = segment_stem(channel, st, st.last_segment);
  const std::string tmet_path =
      (fs::path(mefd_path_) / (channel + ".timd") / (base + ".segd") / (base + ".tmet")).string();
  std::ifstream f(tmet_path, std::ios::binary);
  if (!f) throw IoError("cannot open segment metadata for append: " + tmet_path);
  std::vector<ui1> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  auto md = load_time_series_metadata(bytes,
                                      password_2_.empty() ? password_1_ : password_2_);
  // Section 3 holds the offset the segment's end time is stored against.
  // Locked (level-1 access only) it is UNKNOWN, not 0: refuse rather than
  // validate and place the appended data against the wrong baseline.
  if (!md.section3_available)
    throw PasswordError("writing to channel " + channel + " needs the full-access (level-2) "
                        "password: section 3 of " + tmet_path + ", which holds the "
                        "recording-time offset, is locked with the level-1 password alone");
  if (md.legacy_password_mapping &&
      std::find(legacy_password_segments_.begin(), legacy_password_segments_.end(), tmet_path) ==
          legacy_password_segments_.end())
    legacy_password_segments_.push_back(tmet_path);
  const si8 rto = segment_rto(md);
  st.sampling_frequency = md.section2.sampling_frequency;
  st.units_conversion_factor = md.section2.units_conversion_factor;
  // start_sample is channel-wide, so the last segment's start + count is the
  // channel total.
  st.total_samples = md.section2.start_sample + md.section2.number_of_samples;
  const si8 end_stored = md.universal_header.end_time;
  st.last_end_uutc = end_stored == fmt::UUTC_NO_ENTRY ? 0 : to_user_time(end_stored, rto);
}

SessionWriter::LevelOffset SessionWriter::scan_channel_offset(const fs::path& timd) {
  // What one channel's on-disk segments say about the recording-time offset:
  //   Agreed   every readable segment has the same offset (0 when none);
  //   Disputed two readable segments differ (`detail` names them);
  //   Unknown  a segment's section 3 is locked (level-1 access only).
  // Each segment's .tmet record is read once (16 KB, CRC-verified like the
  // full loader) via probe_recording_time_offset, which decrypts and parses
  // only section 3; the per-channel memo makes it a once-per-Writer cost.
  // A .tmet that cannot be read at all is counted in `unreadable` and listed
  // in unreadable_metadata(); resolve_offset() decides what that permits.
  LevelOffset out;
  std::string agreed_where;
  bool have = false;
  std::error_code ec;
  for (const auto& segd : fs::directory_iterator(timd, ec)) {
    if (!segd.is_directory() || segd.path().extension() != ".segd") continue;
    const fs::path tmet = segd.path() / (segd.path().stem().string() + ".tmet");
    if (!fs::exists(tmet)) continue;  // a directory without metadata: not a segment
    std::optional<si8> rto;
    try {
      if (fs::file_size(tmet) < static_cast<std::uintmax_t>(fmt::METADATA_FILE_BYTES))
        throw FormatError("tmet file too small (truncated?)");
      // Exactly the fixed-length record (never to EOF), unbuffered: the body
      // CRC must be verified before section 3's offset can be trusted.
      std::ifstream f;
      f.rdbuf()->pubsetbuf(nullptr, 0);
      f.open(tmet, std::ios::binary);
      std::vector<ui1> record(fmt::METADATA_FILE_BYTES);
      f.read(reinterpret_cast<char*>(record.data()), static_cast<std::streamsize>(record.size()));
      if (!f) throw IoError("cannot read " + tmet.string());
      rto = probe_recording_time_offset(record, password_2_.empty() ? password_1_ : password_2_);
    } catch (const std::exception& e) {
      ++out.unreadable;
      if (out.unreadable_example.empty()) out.unreadable_example = tmet.string();
      note_unreadable(tmet.string() + ": " + e.what());
      continue;
    }
    ++out.readable;
    if (!rto) {
      out.state = LevelOffset::State::Unknown;
      out.detail = tmet.string();
      return out;  // nothing else can make it known
    }
    if (!have) {
      have = true;
      out.rto = *rto;
      agreed_where = tmet.string();
    } else if (out.state == LevelOffset::State::Agreed && *rto != out.rto) {
      out.state = LevelOffset::State::Disputed;
      out.detail = std::to_string(out.rto) + " in " + agreed_where + ", " +
                   std::to_string(*rto) + " in " + tmet.string();
    }
  }
  if (have) out.where = agreed_where;
  return out;
}

SessionWriter::LevelOffset SessionWriter::level_offset(const std::optional<std::string>& channel) {
  // One channel's answer is scanned ONCE per Writer and memoized: the
  // segments this writer adds carry the offset resolve_offset() chose, so the
  // answer stays true (written segments are folded in by note_new_segment()).
  // The session's answer is the channels' answers combined — so a session
  // records write or a new channel costs one pass over the session's offsets
  // per Writer, and nothing after that.
  auto channel_answer = [&](const std::string& name) -> const LevelOffset& {
    auto it = channel_offsets_.find(name);
    if (it == channel_offsets_.end())
      it = channel_offsets_
               .emplace(name, scan_channel_offset(fs::path(mefd_path_) / (name + ".timd")))
               .first;
    return it->second;
  };
  if (channel) return channel_answer(*channel);

  LevelOffset out;
  bool have = false;
  std::string agreed_where;
  if (!fs::exists(mefd_path_)) return out;
  std::vector<std::string> names;
  for (const auto& timd : fs::directory_iterator(mefd_path_))
    if (timd.is_directory() && timd.path().extension() == ".timd")
      names.push_back(timd.path().stem().string());
  std::sort(names.begin(), names.end());
  for (const auto& name : names) {
    const LevelOffset& lv = channel_answer(name);
    out.readable += lv.readable;
    out.unreadable += lv.unreadable;
    if (out.unreadable_example.empty()) out.unreadable_example = lv.unreadable_example;
    if (lv.state == LevelOffset::State::Unknown) {
      out.state = LevelOffset::State::Unknown;
      out.detail = lv.detail;
      return out;
    }
    if (out.state != LevelOffset::State::Agreed) continue;
    if (lv.state == LevelOffset::State::Disputed) {
      out.state = LevelOffset::State::Disputed;
      out.detail = lv.detail;
    } else if (lv.readable > 0) {
      if (!have) {
        have = true;
        out.rto = lv.rto;
        agreed_where = lv.where;
      } else if (lv.rto != out.rto) {
        out.state = LevelOffset::State::Disputed;
        out.detail = std::to_string(out.rto) + " in " + agreed_where + ", " +
                     std::to_string(lv.rto) + " in " + lv.where;
      }
    }
  }
  if (have) out.where = agreed_where;
  return out;
}

void SessionWriter::note_new_segment(const std::string& channel, const std::string& tmet,
                                     si8 rto) {
  // Keep a memoized channel answer true after this writer adds a segment.
  const auto it = channel_offsets_.find(channel);
  if (it == channel_offsets_.end()) return;  // never scanned: a later scan sees the file
  LevelOffset& lv = it->second;
  if (lv.readable++ == 0) {
    lv.rto = rto;
    lv.where = tmet;
  }
}

void SessionWriter::note_unreadable(const std::string& what) {
  if (std::find(unreadable_metadata_.begin(), unreadable_metadata_.end(), what) ==
      unreadable_metadata_.end())
    unreadable_metadata_.push_back(what);
}

namespace {
// The errors for an offset a write cannot use, in plain words.
[[noreturn]] void refuse_unknown_offset(const std::string& what, const std::string& where) {
  throw PasswordError("cannot write " + what + ": the session's recording-time offset is in "
                      "section 3 of " + where + ", which is locked with the level-1 password "
                      "alone. Open the Writer with the full-access (level-2) password; with a "
                      "guessed offset the times would be stored years off.");
}
[[noreturn]] void refuse_disputed_offset(const std::string& what, const std::string& detail) {
  throw WriteConflictError("cannot write " + what + ": the segments it belongs to disagree on "
                           "the recording-time offset (" + detail + "), so there is no single "
                           "offset to store the times against without misplacing them or "
                           "revealing the real date.");
}
[[noreturn]] void refuse_unverified_offset(const std::string& what, const std::string& where,
                                           bool none_readable) {
  throw WriteConflictError(
      "cannot write " + what + ": the recording-time offset cannot be verified because " +
      where + (none_readable ? " (and every other segment it would be taken from)" : "") +
      " cannot be read. " +
      (none_readable ? std::string("With no readable segment the offset is unknown, and "
                                   "guessing it would store the times years off or reveal "
                                   "the real date.")
                     : std::string("The readable segments use an anonymization offset, and "
                                   "the unreadable one may not agree: writing against an "
                                   "unverified offset can produce records that later cannot "
                                   "be placed.")) +
      " Repair or restore the damaged metadata first.");
}
}  // namespace

si8 SessionWriter::resolve_offset(const std::optional<std::string>& channel,
                                  const std::string& what) {
  // The ONE place a write decides which offset to store times against. The
  // rule keeps an anonymized session's date hidden and never guesses:
  //   * locked (level-1 access) or disputed: refuse;
  //   * nothing readable but something unreadable: unknown, refuse;
  //   * unreadable metadata beside readable segments that agree on a NON-ZERO
  //     offset: the agreement is unverified, refuse;
  //   * unreadable beside agreement on 0: go ahead (and report it) — a bad
  //     segment in an ordinary session must not block unrelated writes;
  //   * a channel with no segment at all (records before data, a new
  //     channel): the session's offset, under the same rules;
  //   * nothing anywhere: 0, mef3io's own convention.
  auto check = [&](const LevelOffset& lv) {
    if (lv.state == LevelOffset::State::Unknown) refuse_unknown_offset(what, lv.detail);
    if (lv.state == LevelOffset::State::Disputed) refuse_disputed_offset(what, lv.detail);
    if (lv.unreadable > 0 && lv.readable == 0)
      refuse_unverified_offset(what, lv.unreadable_example, true);
    if (lv.unreadable > 0 && lv.rto != 0)
      refuse_unverified_offset(what, lv.unreadable_example, false);
  };
  if (channel) {
    const LevelOffset lv = level_offset(channel);
    check(lv);
    if (lv.readable > 0) return lv.rto;
  }
  // Session scope. Cached once it resolves: the segments this writer adds
  // carry the same offset, so the answer cannot change under it — and a
  // fresh session with many channels does not rescan everything per channel.
  if (session_rto_) return *session_rto_;
  const LevelOffset lv = level_offset(std::nullopt);
  check(lv);
  session_rto_ = lv.rto;
  return lv.rto;
}

si8 SessionWriter::new_segment_rto(const std::string& channel, ChannelState& st) {
  // A NEW segment inherits the offset its neighbours agree on (resolve_offset:
  // its own channel first, else the session). mef3io never creates an offset
  // itself, but a foreign session anonymized by meflib has one, and a segment
  // written with 0 would make its channel and the session disagree — after
  // which their records could no longer be placed — and would write the real
  // date. Cached per channel: a months-long recording does not rescan its
  // metadata at every new segment.
  if (st.new_segment_rto) return *st.new_segment_rto;
  const si8 rto = resolve_offset(channel, "a new segment of channel " + channel);
  st.new_segment_rto = rto;
  return rto;
}

void SessionWriter::write_records(std::optional<std::string> channel,
                                  const std::vector<Record>& records) {
  if (records.empty()) return;  // write_records writes nothing for an empty batch
  check_still_writable();
  // Store the times the way the reader resolves this level (time.hpp,
  // to_stored_time): -t at offset 0 (byte-identical to earlier releases),
  // rto - t under an anonymization offset (date stays hidden). Everything
  // that can refuse runs BEFORE any directory or file is created.
  const std::string what =
      channel ? "channel-level records of " + *channel : std::string("session-level records");
  validate_records_for_write(records);
  const si8 rto = resolve_offset(channel, what);
  for (const auto& r : records) require_storable_time(r.time, rto, "record time");
  std::string dir, base;
  bool created_dir = false;
  if (channel) {
    dir = (fs::path(mefd_path_) / (*channel + ".timd")).string();
    created_dir = !fs::exists(dir);
    fs::create_directories(dir);
    base = *channel;
  } else {
    dir = mefd_path_;
    base = session_name_;
    // Append to the file the READER treats as the session's records (the sole
    // .rdat whatever its name, e.g. after the folder was renamed); never start
    // a second one beside it. Several candidates: refuse, name them.
    std::string ambiguity;
    const std::string existing = detail::session_record_file(DirectorySource(mefd_path_), ambiguity);
    if (!ambiguity.empty())
      throw FormatError(ambiguity + "; annotations were not written (keep one record file at the "
                                    "session root, or run recover_session)");
    if (!existing.empty()) base = existing.substr(0, existing.size() - 5);  // strip ".rdat"
  }
  try {
    mef3io::write_records(dir, base, session_name_, channel.value_or(""), 0, records, rto,
                          password_1_, password_2_);
  } catch (...) {
    // write_records validates before it writes; leave no empty channel
    // directory behind for a write that never happened.
    std::error_code ec;
    if (created_dir) fs::remove_all(dir, ec);
    throw;
  }
  const std::vector<fs::path> files = {fs::path(dir) / (base + ".rdat"),
                                       fs::path(dir) / (base + ".ridx")};
  // wait_for_disk waits now (files and their folders), so nothing is owed
  // later; otherwise flush()/close() owe them.
  if (wait_for_disk_)
    flush_recording_failure(files);
  else
    unflushed_.insert(files.begin(), files.end());
}

SessionWriter::~SessionWriter() {
  // After a failed flush the data may already be lost (the OS reports a
  // writeback error once): a retry in a destructor would only look like
  // success, and the failure was reported by the call that hit it.
  if (flush_failure_) return;
  try {
    flush();
  } catch (...) {
    // A destructor cannot report it; flush() before closing does.
  }
}

void SessionWriter::remember_flush_failure(const IoError& e) {
  // A wait_for_disk write whose flush failed: same stickiness as flush().
  if (!flush_failure_ && std::string(e.what()).find("could not flush to disk") != std::string::npos)
    flush_failure_ = e.what();
}

void SessionWriter::flush_recording_failure(const std::vector<fs::path>& files) {
  try {
    detail::flush_files(files);
  } catch (const IoError& e) {
    if (!flush_failure_) flush_failure_ = e.what();
    throw;
  }
}

void SessionWriter::flush() {
  // STICKY: the first failed flush is remembered and rethrown by every later
  // flush()/close(). The OS reports a writeback error once and marks the pages
  // clean, so a retry that "succeeds" would claim data that is gone.
  if (flush_failure_)
    throw IoError(*flush_failure_ +
                  " (an earlier flush failed: data written since the last successful flush may "
                  "be lost; " + std::string(flush_mode_ == FlushMode::Close ? "the session must be rewritten (flush=\"close\" sessions cannot be reliably recovered)" : "run recover_session") + ")");
  if (unflushed_.empty()) return;
  std::vector<fs::path> files;
  std::error_code ec;
  for (const auto& f : unflushed_)
    if (fs::exists(f, ec)) files.push_back(f);
  flush_recording_failure(files);  // ONE combined wait for all of them
  unflushed_.clear();
}

si8 SessionWriter::block_length_for(sf8 fs) const {
  if (block_length_override_ > 0) return block_length_override_;
  // fs is finite and > 0 here (check_signal_parameters). Decided in double
  // BEFORE the cast: a cast of a value outside si8 range is undefined, and a
  // rate below 0.1 Hz gives less than one sample, which would never advance.
  const sf8 n = fs >= 5000.0 ? fs : fs * 10.0;
  if (n > static_cast<sf8>(MAX_BLOCK_SAMPLES))
    throw std::invalid_argument(
        "sampling frequency " + std::to_string(static_cast<long long>(fs)) +
        " Hz gives a default block of more samples than a MEF 3.0 block can hold (" +
        std::to_string(MAX_BLOCK_SAMPLES) + "); set a block length explicitly");
  return std::max<si8>(1, static_cast<si8>(n));
}

std::string SessionWriter::segment_stem(const std::string& channel, const ChannelState& st,
                                        int seg) const {
  if (seg == st.last_segment && !st.last_segment_stem.empty()) return st.last_segment_stem;
  return segment_name(channel, seg);
}

namespace {
std::string join_names(const std::vector<std::string>& v) {
  std::string out;
  for (const auto& n : v) out += (out.empty() ? "" : ", ") + n;
  return out.empty() ? std::string("none") : out;
}

void check_unique_channels(const std::vector<std::string>& channels) {
  std::set<std::string> seen;
  for (const auto& c : channels)
    if (!seen.insert(c).second)
      throw std::invalid_argument("channel " + c + " is listed more than once; a multi-channel "
                                  "write takes each channel once per call (nothing was written)");
}
}  // namespace

WriteSummary SessionWriter::write_int32(const std::string& channel, std::span<const si4> samples,
                                        sf8 ufact, si8 start_uutc, sf8 fs,
                                        std::span<const ui1> valid, bool new_segment) {
  check_signal_parameters(fs, ufact);
  WriteSummary s = write_blocks(channel, samples, valid, ufact, start_uutc, fs, new_segment);
  s.quantization = Quantization::Counts;
  return s;
}

// What a float write stores: the counts, the gap mask and the factor, decided
// BEFORE anything is written. Reads channels_ but never changes it (the
// channel is hydrated by the caller), so several channels may be prepared at
// once.
struct SessionWriter::FloatPrep {
  std::vector<si4> counts;
  std::vector<ui1> valid;
  sf8 ufact = 1.0;
  Quantization quantization = Quantization::Decimal;
  ClampCount clamped;
  std::string step_fallback;
};

SessionWriter::FloatPrep SessionWriter::prepare_float(const std::string& channel,
                                                      std::span<const sf8> data, sf8 fs,
                                                      Precision precision_arg, bool new_segment) {
  check_signal_parameters(fs, 1.0);  // the factor is checked once it is known
  FloatPrep prep;
  int precision = precision_arg.digits;
  if (precision_arg.step) {
    // #81, opt-in: the data's own step as the factor. Everything below is
    // decided before write_blocks, so a refused write leaves the tree as is.
    auto it = channels_.find(channel);
    if (it != channels_.end() && it->second.n_segments > 0) {
      // A channel's factor is fixed once it has segments: the reader scales
      // the WHOLE channel by one factor, so neither an append nor a new
      // segment may change it (#150 may lift this for new segments). Data on
      // it is stored exactly; anything else is refused — rounding it would
      // be lossier than the caller asked for, and silently so.
      const sf8 u = it->second.units_conversion_factor;
      std::string why;
      if (!quantize_to_step(data, u, prep.counts, prep.valid, &why, &prep.clamped)) {
        throw WriteConflictError(
            "precision='step': the data for channel " + channel + " is not stored on the "
            "channel's conversion factor " + detail::shortest_number(u) + " (" + why + "); nothing "
            "was written. A channel keeps one factor: quantize the data yourself and write the "
            "counts with write_int32 at that factor, or write it to a new channel");
      }
      prep.ufact = u;
      prep.quantization = Quantization::Step;
      return prep;
    } else {
      const StepEstimate est = detect_step(data);
      std::string why = est.reason;
      if (est.step && quantize_to_step(data, *est.step, prep.counts, prep.valid, &why,
                                       &prep.clamped)) {
        prep.ufact = *est.step;
        prep.quantization = Quantization::Step;
        return prep;
      }
      prep.counts.clear();
      prep.valid.clear();
      prep.clamped = {};
      // Nothing but 0 and NaN (no reason given): any factor stores that
      // exactly, so the default path is no fallback.
      if (!why.empty())
        prep.step_fallback = why + "; stored with the default (inferred power-of-ten) precision";
      precision = -1;
    }
  }
  if (precision < 0) {
    // In-segment appends must keep the segment's conversion factor, so reuse
    // its precision instead of re-inferring (which could differ and conflict).
    auto it = channels_.find(channel);
    if (!new_segment && it != channels_.end() && it->second.n_segments > 0) {
      const sf8 u = it->second.units_conversion_factor;
      precision = u > 0.0 ? static_cast<int>(std::llround(-std::log10(u))) : 0;
      // A factor that is no power of ten (precision='step', write_int32)
      // has no precision to reuse: say so instead of the generic conflict.
      if (!(u > 0.0) || std::abs(std::pow(10.0, -precision) - u) > 1e-12 * u)
        throw WriteConflictError(
            "cannot append to channel " + channel + " with an inferred precision: its "
            "conversion factor " + detail::shortest_number(u) + " is not a power of ten (written with "
            "precision='step' or write_int32). Append with precision='step' (data on that "
            "factor is stored exactly) or with write_int32 at that factor");
    } else {
      precision = infer_precision(data);
    }
  }
  prep.ufact = std::pow(10.0, -precision);
  prep.counts = quantize_to_int32(data, precision, &prep.clamped);
  prep.valid.resize(data.size());
  for (std::size_t i = 0; i < data.size(); ++i) prep.valid[i] = std::isnan(data[i]) ? 0 : 1;
  prep.quantization = Quantization::Decimal;
  return prep;
}

WriteSummary SessionWriter::write_float(const std::string& channel, std::span<const sf8> data,
                                        si8 start_uutc, sf8 fs, Precision precision_arg,
                                        bool new_segment) {
  check_signal_parameters(fs, 1.0);
  check_still_writable();  // before any read of the session folder
  {
    // Hydrate first (a channel adopted from disk): prepare_float only reads.
    auto it = channels_.find(channel);
    if (it != channels_.end() && it->second.n_segments > 0) hydrate(channel, it->second);
  }
  FloatPrep prep = prepare_float(channel, data, fs, precision_arg, new_segment);
  WriteSummary s = write_blocks(channel, prep.counts, prep.valid, prep.ufact, start_uutc, fs,
                                new_segment);
  s.quantization = prep.quantization;
  s.clamped = prep.clamped.total();
  s.clamped_infinite = prep.clamped.infinite;
  s.step_fallback = std::move(prep.step_fallback);
  return s;
}

struct SessionWriter::WritePlan {
  std::string channel;
  ChannelState* st = nullptr;
  std::span<const si4> samples;
  std::span<const ui1> valid;
  std::vector<si4> own_counts;  // a float write's counts (samples points here)
  std::vector<ui1> own_valid;
  sf8 ufact = 1.0, fs = 0.0;
  si8 start_uutc = 0;
  bool noop = false;
  bool append = false;
  int seg = 0;
  std::string seg_dir;
  SegmentSpec spec;
  si8 stored = 0, n = 0;
  // One entry per block, without its samples (filled by run_plan, so the
  // copies happen on the worker that encodes them).
  std::vector<BlockSpec> blocks;
  std::vector<si8> block_src, block_len;
  ui4 written_difference_bytes = 0;
  PendingPublish pending;
  bool pending_set = false;     // the segment is written, its .tmet unpublished
  AppendIndexCache cache_before;  // the channel's index summary before run_plan
  bool created_timd = false, created_segd = false;
  fs::path timd;
  WriteSummary summary;
};

std::unique_ptr<SessionWriter::WritePlan> SessionWriter::plan_write(
    const std::string& channel, std::span<const si4> samples, std::span<const ui1> valid,
    sf8 ufact, si8 start_uutc, sf8 fs, bool new_segment) {
  // Everything about the arguments is refused BEFORE anything is read or
  // written (#83): a bad rate or factor, a block the format cannot describe,
  // and data whose times would leave the uUTC range.
  check_signal_parameters(fs, ufact);
  const si8 n = static_cast<si8>(samples.size());
  const si8 block_len = block_length_for(fs);
  if (!valid.empty() && valid.size() != samples.size())
    throw std::invalid_argument("valid mask has " + std::to_string(valid.size()) +
                                " entries for " + std::to_string(samples.size()) + " samples");
  {
    const sf8 span_us = static_cast<sf8>(n) / fs * 1e6;
    if (!(static_cast<sf8>(start_uutc) + span_us < 9.0e18)) {
      char rate[40];
      std::snprintf(rate, sizeof(rate), "%.17g", fs);
      throw std::invalid_argument(std::to_string(n) + " samples at a sampling frequency of " +
                                  rate + " Hz would end beyond the representable time range; "
                                  "is the sampling frequency right?");
    }
  }
  check_still_writable();
  ChannelState& st = channels_[channel];
  hydrate(channel, st);

  auto is_valid = [&](si8 i) { return valid.empty() ? true : valid[static_cast<std::size_t>(i)] != 0; };

  // Split into contiguous valid runs.
  struct Run {
    si8 begin, end;
  };
  std::vector<Run> runs;
  si8 i = 0;
  while (i < n) {
    while (i < n && !is_valid(i)) ++i;
    if (i >= n) break;
    si8 begin = i;
    while (i < n && is_valid(i)) ++i;
    runs.push_back({begin, i});
  }

  auto plan = std::make_unique<WritePlan>();
  plan->channel = channel;
  plan->st = &st;
  plan->samples = samples;
  plan->valid = valid;
  plan->ufact = ufact;
  plan->fs = fs;
  plan->start_uutc = start_uutc;
  plan->n = n;
  WriteSummary& summary = plan->summary;
  summary.units_conversion_factor = ufact;
  summary.gaps_skipped = static_cast<si8>(runs.size()) > 0 ? static_cast<si8>(runs.size()) - 1 : 0;
  if (runs.empty()) {
    // All-NaN / no valid data: no-op on disk, but report it explicitly.
    summary.segment = (!new_segment && st.n_segments > 0) ? st.last_segment
                                                          : st.last_segment + 1;
    plan->noop = true;
    return plan;
  }

  // Describe the blocks. start_sample is channel-wide (continues across
  // segments); start_uutc reflects each run's actual time so gaps appear on
  // uutc reads.
  si8 stored = 0;
  const si8 seg_base_sample = st.total_samples;
  for (const Run& run : runs) {
    bool first_block_of_run = true;
    for (si8 s = run.begin; s < run.end; s += block_len) {
      si8 len = std::min(block_len, run.end - s);
      BlockSpec b;
      b.start_sample = seg_base_sample + stored;
      b.start_uutc = start_uutc + static_cast<si8>(std::llround(static_cast<sf8>(s) / fs * 1e6));
      b.discontinuity = first_block_of_run;  // first block of each run (and of segment)
      plan->blocks.push_back(std::move(b));
      plan->block_src.push_back(s);
      plan->block_len.push_back(len);
      stored += len;
      first_block_of_run = false;
    }
  }
  plan->stored = stored;

  const bool append = !new_segment && st.n_segments > 0;
  const int seg = append ? st.last_segment : st.last_segment + 1;
  plan->append = append;
  plan->seg = seg;
  plan->seg_dir =
      (fs::path(mefd_path_) / (channel + ".timd") / (segment_stem(channel, st, seg) + ".segd"))
          .string();

  SegmentSpec& spec = plan->spec;
  spec.session_name = session_name_;
  spec.channel_name = channel;
  spec.segment_number = seg;
  spec.sampling_frequency = fs;
  spec.units_conversion_factor = ufact;
  spec.units_description = units_;
  spec.gmt_offset = metadata_.gmt_offset;
  spec.password_1 = password_1_;
  spec.password_2 = password_2_;
  spec.metadata = metadata_;
  // Only a NEW segment takes the session's offset; an append keeps the one
  // already in the segment it extends (append_time_series_segment).
  if (!append) spec.recording_time_offset = new_segment_rto(channel, st);
  // A new segment's .tmet is flushed before it is renamed in (its .tdat and
  // .tidx are not: a short one is rebuilt by recovery from the data):
  //  * a channel's FIRST segment, in any session (once per channel): a short
  //    .tmet fails the strict open of the whole session and has no previous
  //    segment's .tmet to be rebuilt from;
  //  * any new segment or channel in a session that held data when this
  //    writer opened it.
  // NOT a later segment in a session this writer created: a conversion
  // writes thousands of those, and recovery rebuilds a lost .tmet there from
  // the previous segment's. flush()/close()/wait_for_disk cover the rest.
  if (!append) spec.flush_new_metadata = session_preexisted_ || st.n_segments == 0;
  spec.wait_for_disk = wait_for_disk_;
  spec.skip_tmet_flush = flush_mode_ == FlushMode::Close;

  // Only pass on a maximum this writer measured itself. A segment adopted from
  // disk leaves the flag false, so the append bounds the blocks it cannot see
  // rather than trusting a declaration it has no way to verify.
  if (append && st.difference_bytes_exact) spec.known_difference_bytes = st.max_difference_bytes;

  if (append) {
    // The conflicts append_time_series_segment would find on disk, decided
    // HERE from the hydrated state so a multi-channel call refuses before any
    // channel has been extended (the check there stays as defence).
    if (std::abs(st.sampling_frequency - fs) > 1e-9 * std::max(1.0, std::abs(st.sampling_frequency)))
      throw WriteConflictError("append fs " + std::to_string(fs) + " != segment fs " +
                               std::to_string(st.sampling_frequency));
    if (std::abs(st.units_conversion_factor - ufact) >
        1e-12 * std::max(1.0, std::abs(st.units_conversion_factor)))
      throw WriteConflictError("append units_conversion_factor " + std::to_string(ufact) +
                               " != segment's " + std::to_string(st.units_conversion_factor));
    const si8 slack = static_cast<si8>(std::llround(std::min(0.5e6 / st.sampling_frequency, 1e18)));
    if (plan->blocks.front().start_uutc < st.last_end_uutc - slack)
      throw WriteConflictError("append starts at " + std::to_string(plan->blocks.front().start_uutc) +
                               " uUTC, before segment end " + std::to_string(st.last_end_uutc));
  }
  if (!append) {
    // Everything that can refuse runs BEFORE a directory exists: a refused
    // write must leave the tree exactly as it was. A directory left behind
    // would be adopted as a segment by the next Writer.
    for (const auto& b : plan->blocks)
      require_storable_time(b.start_uutc, spec.recording_time_offset, "block start time");
  }
  return plan;
}

void SessionWriter::create_plan_dirs(WritePlan& p) {
  if (p.noop || p.append) return;
  p.timd = fs::path(mefd_path_) / (p.channel + ".timd");
  p.created_timd = !fs::exists(p.timd);
  p.created_segd = !fs::exists(p.seg_dir);
  fs::create_directories(p.seg_dir);
}

void SessionWriter::run_plan(WritePlan& p, PendingPublish* defer, int threads) {
  // Touches only the plan and its channel's state: plans of different
  // channels run concurrently.
  p.cache_before = p.st->index_cache;
  for (std::size_t k = 0; k < p.blocks.size(); ++k) {
    const auto first = p.samples.begin() + p.block_src[k];
    p.blocks[k].samples.assign(first, first + p.block_len[k]);
  }
  if (p.append) {
    // fs / conversion-factor / start-time conflicts are validated against the
    // on-disk metadata inside append_time_series_segment.
    append_time_series_segment(p.seg_dir, p.spec, p.blocks, threads, &p.written_difference_bytes,
                               &p.st->index_cache, defer);
  } else {
    write_time_series_segment(p.seg_dir, p.spec, p.blocks, threads, &p.written_difference_bytes,
                              defer);
  }
  p.pending_set = defer != nullptr;
  // Written: nothing below needs the samples (commit uses counts only), so
  // the copies and a float write's counts go now, not at the end of the call.
  for (auto& b : p.blocks) std::vector<si4>().swap(b.samples);
  std::vector<si4>().swap(p.own_counts);
  std::vector<ui1>().swap(p.own_valid);
  p.samples = {};
  p.valid = {};
}

std::string SessionWriter::roll_back_plan(WritePlan& p) {
  // After a failure BEFORE the .tmet was published. A failed append already
  // restored itself; one that finished (pending_set) is undone here. A fresh
  // segment is removed with the folders this write created.
  std::string err;
  if (p.pending_set) {
    err = discard_pending(p.pending);
    p.pending_set = false;
    // A clean rollback leaves the files as the summary described them: keep
    // it, so the next append is not a full .tidx walk. Otherwise forget it.
    if (p.append) {
      if (err.empty())
        p.st->index_cache = p.cache_before;
      else
        p.st->index_cache.valid = false;
    }
  }
  if (!p.append && !p.noop) {
    std::error_code ec;
    if (p.created_timd)
      fs::remove_all(p.timd, ec);
    else if (p.created_segd)
      fs::remove_all(p.seg_dir, ec);
  }
  return err;
}

void SessionWriter::commit_plan(WritePlan& p) {
  ChannelState& st = *p.st;
  const bool append = p.append;
  if (!append) {
    // A fresh segment holds nothing but the blocks just encoded, so from here
    // the running maximum is exact — and the index summary from the previous
    // segment describes a file this channel is no longer appending to.
    st.max_difference_bytes = 0;
    st.difference_bytes_exact = true;
    st.index_cache = AppendIndexCache{};
  }
  st.max_difference_bytes = std::max(st.max_difference_bytes, p.written_difference_bytes);

  {
    // Owed to the next flush() / close(). In wait_for_disk mode this write
    // already flushed them (and their folders): not kept, so close() does not
    // re-flush the whole history (one F_FULLFSYNC per file on macOS).
    if (!wait_for_disk_) {
      const std::string stem = fs::path(p.seg_dir).stem().string();
      for (const char* ext : {".tdat", ".tidx", ".tmet"})
        unflushed_.insert(fs::path(p.seg_dir) / (stem + ext));
    }
  }

  st.sampling_frequency = p.fs;
  st.units_conversion_factor = p.ufact;
  if (!append) {
    ++st.n_segments;
    const std::string stem = fs::path(p.seg_dir).stem().string();
    note_new_segment(p.channel, (fs::path(p.seg_dir) / (stem + ".tmet")).string(),
                     p.spec.recording_time_offset);
    // The slot is a real segment now: drop the stale "ignored" report.
    const auto e = empty_segment_dirs_.find(p.seg_dir);
    if (e != empty_segment_dirs_.end()) {
      unreadable_metadata_.erase(
          std::remove(unreadable_metadata_.begin(), unreadable_metadata_.end(), e->second),
          unreadable_metadata_.end());
      empty_segment_dirs_.erase(e);
    }
  }
  st.last_segment = p.seg;
  st.last_segment_stem = fs::path(p.seg_dir).stem().string();
  st.total_samples += p.stored;
  // Same expression as the writer's segment end (last block's start + its
  // length), so the pre-check in plan_write sees what is on disk.
  st.last_end_uutc = p.blocks.back().start_uutc +
                     static_cast<si8>(std::llround(static_cast<sf8>(p.block_len.back()) * 1e6 / p.fs));

  p.summary.samples_written = p.stored;
  p.summary.blocks = static_cast<si8>(p.blocks.size());
  p.summary.segment = p.seg;
}

WriteSummary SessionWriter::write_blocks(const std::string& channel,
                                         std::span<const si4> samples,
                                         std::span<const ui1> valid, sf8 ufact, si8 start_uutc,
                                         sf8 fs, bool new_segment) {
  auto plan = plan_write(channel, samples, valid, ufact, start_uutc, fs, new_segment);
  if (plan->noop) return plan->summary;
  try {
    create_plan_dirs(*plan);
    run_plan(*plan, nullptr, n_threads_.load());
  } catch (const IoError& e) {
    remember_flush_failure(e);
    roll_back_plan(*plan);
    throw;
  } catch (...) {
    roll_back_plan(*plan);
    throw;
  }
  commit_plan(*plan);
  return plan->summary;
}

std::vector<WriteSummary> SessionWriter::write_many_plans(
    std::vector<std::unique_ptr<WritePlan>>& plans) {
  // The ONE multi-channel write. Everything that can refuse was decided in
  // plan_write (serially, nothing on disk). Then:
  //   A  per channel, in parallel: files written / extended, the .tmet left as
  //      a TEMP file (PendingPublish);
  //   -  ONE combined flush of every temp (and, wait_for_disk, the data);
  //   B  the renames;
  //   C  serial commit of the writer's state.
  // A failure in A or the flush rolls EVERY channel back (nothing written);
  // one in B reports the channels already published (PartialWriteError).
  const int T = static_cast<int>(resolve_threads(n_threads_.load()));
  std::vector<WritePlan*> active;
  for (auto& p : plans)
    if (!p->noop) active.push_back(p.get());

  // Undo plans [from, end); returns "<channel>: <why>" for every rollback
  // that did not complete.
  auto roll_back_from = [&](std::size_t from) {
    std::vector<std::pair<std::string, std::string>> bad;
    for (std::size_t j = from; j < active.size(); ++j) {
      const std::string why = roll_back_plan(*active[j]);
      if (!why.empty()) bad.emplace_back(active[j]->channel, why);
    }
    return bad;
  };
  // Rethrow @p cause after a rollback of everything; an incomplete rollback
  // is reported instead, naming the channels (their files may be torn).
  auto fail_rolled_back = [&](const std::string& cause, std::exception_ptr original) {
    const auto bad = roll_back_from(0);
    if (bad.empty()) std::rethrow_exception(original);
    std::string text = cause + "; rollback was incomplete: ";
    std::vector<std::string> names;
    for (const auto& [ch, why] : bad) {
      text += (names.empty() ? "" : "; ") + ch + ": " + why;
      names.push_back(ch);
    }
    throw RollbackIncompleteError(text, names);
  };

  if (!active.empty()) {
    try {
      for (WritePlan* p : active) create_plan_dirs(*p);
    } catch (const std::exception& e) {
      fail_rolled_back(e.what(), std::current_exception());
    }
    // ONE thread budget: C channels at a time, each encoding its blocks on
    // T / C threads, so no more than ~T threads run whatever the shape.
    const int outer = std::min<int>(T, static_cast<int>(active.size()));
    const int inner = std::max(1, T / std::max(1, outer));
    auto errors = parallel_try_each(active.size(), outer, [&](std::size_t k) {
      run_plan(*active[k], &active[k]->pending, inner);
    });
    for (std::size_t k = 0; k < active.size(); ++k) {
      if (!errors[k]) continue;
      try {
        std::rethrow_exception(errors[k]);
      } catch (const IoError& e) {
        remember_flush_failure(e);
        fail_rolled_back(e.what(), std::current_exception());
      } catch (const std::exception& e) {
        fail_rolled_back(e.what(), std::current_exception());
      }
    }

    std::vector<PendingPublish*> pend;
    for (WritePlan* p : active) pend.push_back(&p->pending);
    try {
      flush_pending(pend, false);
    } catch (const IoError& e) {
      if (!flush_failure_) flush_failure_ = e.what();
      fail_rolled_back(e.what(), std::current_exception());
    } catch (const std::exception& e) {
      fail_rolled_back(e.what(), std::current_exception());
    }

    std::vector<std::string> published;
    std::size_t k = 0;
    try {
      for (; k < active.size(); ++k) {
        publish_pending(active[k]->pending);
        active[k]->pending_set = false;  // published: nothing left to discard
        published.push_back(active[k]->channel);
      }
    } catch (const std::exception& e) {
      // Channels [0, k) are on disk and valid; the rest are undone.
      const std::string why = e.what();
      for (std::size_t j = 0; j < k; ++j) commit_plan(*active[j]);
      const auto bad = roll_back_from(k);
      // wait_for_disk promised "returned = on disk" for what IS written.
      std::string flush_note;
      if (wait_for_disk_ && k > 0) {
        std::vector<PendingPublish*> done(pend.begin(), pend.begin() + static_cast<std::ptrdiff_t>(k));
        try {
          flush_pending(done, true);
        } catch (const IoError& fe) {
          if (!flush_failure_) flush_failure_ = fe.what();
          flush_note = std::string("; flushing the written channels also failed: ") + fe.what();
        }
      }
      std::string text = "writing " + std::to_string(active.size()) + " channels failed while "
                         "publishing channel " + active[k]->channel + " (" + why + "); " +
                         std::to_string(k) + " channel(s) were written, the others were rolled "
                         "back. Written: " + join_names(published) + flush_note;
      std::vector<std::string> names;
      for (const auto& [ch, w] : bad) {
        text += (names.empty() ? "; rollback was incomplete: " : "; ") + ch + ": " + w;
        names.push_back(ch);
      }
      throw PartialWriteError(text, published, names);
    }
    for (WritePlan* p : active) p->pending_set = false;
    try {
      flush_pending(pend, true);  // wait_for_disk: the published names too
    } catch (const IoError& e) {
      for (WritePlan* p : active) commit_plan(*p);
      if (!flush_failure_) flush_failure_ = e.what();
      throw PartialWriteError(std::string("all channels were written but the final flush "
                                          "failed: ") + e.what(),
                              published);
    }
  }
  std::vector<WriteSummary> out;
  for (auto& p : plans) {
    if (!p->noop) commit_plan(*p);
    out.push_back(p->summary);
  }
  return out;
}

std::vector<WriteSummary> SessionWriter::write_int32_many(
    const std::vector<std::string>& channels, std::span<const si4> data, std::span<const sf8> ufacts,
    si8 start_uutc, sf8 fs, bool new_segment) {
  const std::size_t nc = channels.size();
  if (nc == 0) return {};
  check_unique_channels(channels);
  if (data.size() % nc != 0)
    throw std::invalid_argument("data holds " + std::to_string(data.size()) +
                                " values, not a whole number of rows for " + std::to_string(nc) +
                                " channels");
  if (ufacts.size() != 1 && ufacts.size() != nc)
    throw std::invalid_argument("ufact must be one value or one per channel (" +
                                std::to_string(nc) + "), not " + std::to_string(ufacts.size()));
  check_signal_parameters(fs, 1.0);
  for (sf8 u : ufacts) check_signal_parameters(fs, u);
  const std::size_t n = data.size() / nc;
  std::vector<std::unique_ptr<WritePlan>> plans;
  for (std::size_t c = 0; c < nc; ++c)
    plans.push_back(plan_write(channels[c], data.subspan(c * n, n), {},
                               ufacts.size() == 1 ? ufacts[0] : ufacts[c], start_uutc, fs,
                               new_segment));
  auto out = write_many_plans(plans);
  for (auto& s : out) s.quantization = Quantization::Counts;
  return out;
}

std::vector<WriteSummary> SessionWriter::write_float_many(
    const std::vector<std::string>& channels, std::span<const sf8> data, si8 start_uutc, sf8 fs,
    Precision precision, bool new_segment) {
  const std::size_t nc = channels.size();
  if (nc == 0) return {};
  check_unique_channels(channels);
  if (data.size() % nc != 0)
    throw std::invalid_argument("data holds " + std::to_string(data.size()) +
                                " values, not a whole number of rows for " + std::to_string(nc) +
                                " channels");
  check_signal_parameters(fs, 1.0);
  check_still_writable();
  const std::size_t n = data.size() / nc;
  // Serial: adopt channels from disk (prepare_float only reads channels_).
  for (const auto& c : channels) {
    auto it = channels_.find(c);
    if (it != channels_.end() && it->second.n_segments > 0) hydrate(c, it->second);
  }
  // Quantization is the CPU-heavy part of a float write: across channels,
  // on the writer's threads. The error reported is the lowest channel's, as
  // a serial loop would meet it.
  std::vector<FloatPrep> preps(nc);
  const auto errors = parallel_try_each(nc, n_threads_.load(), [&](std::size_t c) {
    preps[c] = prepare_float(channels[c], data.subspan(c * n, n), fs, precision, new_segment);
  });
  for (const auto& e : errors)
    if (e) std::rethrow_exception(e);
  std::vector<std::unique_ptr<WritePlan>> plans;
  for (std::size_t c = 0; c < nc; ++c) {
    auto plan = plan_write(channels[c], preps[c].counts, preps[c].valid, preps[c].ufact,
                           start_uutc, fs, new_segment);
    plan->own_counts = std::move(preps[c].counts);  // the spans stay valid: a move keeps the buffer
    plan->own_valid = std::move(preps[c].valid);
    plan->summary.quantization = preps[c].quantization;
    plan->summary.clamped = preps[c].clamped.total();
    plan->summary.clamped_infinite = preps[c].clamped.infinite;
    plan->summary.step_fallback = std::move(preps[c].step_fallback);
    plans.push_back(std::move(plan));
  }
  return write_many_plans(plans);
}

}  // namespace mef3io
