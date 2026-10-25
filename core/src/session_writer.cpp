// mef3io — high-level session writer.
#include "operation_marker.hpp"
#include "mef3io/session_writer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <limits>

#include <fstream>

#include "mef3io/errors.hpp"
#include "mef3io/metadata.hpp"
#include "mef3io/source.hpp"
#include "mef3io/writer.hpp"
#include "mef3io/time.hpp"
#include "last_segment.hpp"

namespace fs = std::filesystem;

namespace mef3io {
namespace {

bool int32_range_ok(sf8 xmin, sf8 xmax, sf8 alpha) {
  // Matches mef_tools check_int32_dynamic_range: bad only if BOTH ends overflow.
  return !(xmin * alpha < std::numeric_limits<si4>::min() &&
           xmax * alpha > std::numeric_limits<si4>::max());
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
  for (sf8 v : data) {
    if (!std::isnan(v) && !std::isnan(prev)) {
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
    if (!std::isnan(v)) {
      dmax = std::max(dmax, v);
      dmin = std::min(dmin, v);
    }
  if (!std::isfinite(dmax)) return 0;  // all NaN

  sf8 alpha = std::pow(10.0, precision);
  while (!int32_range_ok(dmin, dmax, alpha) && precision != 0) {
    --precision;
    alpha = std::pow(10.0, precision);
  }
  return precision;
}

std::vector<si4> quantize_to_int32(std::span<const sf8> data, int precision) {
  const sf8 scale = std::pow(10.0, precision);
  std::vector<si4> out(data.size());
  for (std::size_t i = 0; i < data.size(); ++i) {
    if (std::isnan(data[i])) {
      out[i] = 0;  // gap position, not stored
      continue;
    }
    sf8 rounded = std::round(data[i] * scale);  // round(data,prec)*10^prec == round(data*10^prec)
    out[i] = static_cast<si4>(std::llround(rounded));
  }
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
  fs::create_directories(mefd_path_);
  session_name_ = fs::path(mefd_path_).stem().string();

  // Adopt existing channels/segments so appends continue where the session
  // left off. Metadata is read back lazily (hydrate) on the first write.
  for (const auto& entry : fs::directory_iterator(mefd_path_)) {
    if (!entry.is_directory() || entry.path().extension() != ".timd") continue;
    std::string ch = entry.path().stem().string();
    int nseg = 0;
    long long last = -1;
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
      last = std::max(last, detail::segment_number_of(s.path().stem().string()));
    }
    // Segments are addressed by the HIGHEST real segment NUMBER, never by
    // how many there are: with a directory without metadata in the middle
    // (0 good, 1 empty, 2 good) the count is 2 but the last segment is 2, and
    // using the count appended to the empty one. A new segment is last + 1,
    // so it never collides with or reorders a real one; an empty directory
    // past every real segment is simply reused.
    channels_[ch].n_segments = nseg;
    channels_[ch].last_segment = static_cast<int>(last);
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
}

void SessionWriter::hydrate(const std::string& channel, ChannelState& st) {
  if (st.hydrated) return;
  st.hydrated = true;
  const std::string base = segment_name(channel, st.last_segment);
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
}

si8 SessionWriter::block_length_for(sf8 fs) const {
  if (block_length_override_ > 0) return block_length_override_;
  if (fs >= 5000.0) return static_cast<si8>(fs);
  return static_cast<si8>(fs * 10.0);
}

WriteSummary SessionWriter::write_int32(const std::string& channel, std::span<const si4> samples,
                                        sf8 ufact, si8 start_uutc, sf8 fs,
                                        std::span<const ui1> valid, bool new_segment) {
  std::vector<si4> buf(samples.begin(), samples.end());
  return write_blocks(channel, buf, valid, ufact, start_uutc, fs, new_segment);
}

WriteSummary SessionWriter::write_float(const std::string& channel, std::span<const sf8> data,
                                        si8 start_uutc, sf8 fs, int precision, bool new_segment) {
  if (precision < 0) {
    // In-segment appends must keep the segment's conversion factor, so reuse
    // its precision instead of re-inferring (which could differ and conflict).
    auto it = channels_.find(channel);
    if (!new_segment && it != channels_.end() && it->second.n_segments > 0) {
      hydrate(channel, it->second);
      precision =
          static_cast<int>(std::llround(-std::log10(it->second.units_conversion_factor)));
    } else {
      precision = infer_precision(data);
    }
  }
  sf8 ufact = std::pow(10.0, -precision);
  auto samples = quantize_to_int32(data, precision);
  std::vector<ui1> valid(data.size());
  for (std::size_t i = 0; i < data.size(); ++i) valid[i] = std::isnan(data[i]) ? 0 : 1;
  return write_blocks(channel, samples, valid, ufact, start_uutc, fs, new_segment);
}

WriteSummary SessionWriter::write_blocks(const std::string& channel,
                                         const std::vector<si4>& samples,
                                         std::span<const ui1> valid, sf8 ufact, si8 start_uutc,
                                         sf8 fs, bool new_segment) {
  check_still_writable();
  ChannelState& st = channels_[channel];
  hydrate(channel, st);
  const si8 n = static_cast<si8>(samples.size());
  const si8 block_len = block_length_for(fs);

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

  WriteSummary summary;
  summary.gaps_skipped = static_cast<si8>(runs.size()) > 0 ? static_cast<si8>(runs.size()) - 1 : 0;
  if (runs.empty()) {
    // All-NaN / no valid data: no-op on disk, but report it explicitly.
    summary.segment = (!new_segment && st.n_segments > 0) ? st.last_segment
                                                          : st.last_segment + 1;
    return summary;
  }

  // Build blocks. start_sample is channel-wide (continues across segments);
  // start_uutc reflects each run's actual time so gaps appear on uutc reads.
  std::vector<BlockSpec> blocks;
  si8 stored = 0;
  const si8 seg_base_sample = st.total_samples;
  for (const Run& run : runs) {
    bool first_block_of_run = true;
    for (si8 s = run.begin; s < run.end; s += block_len) {
      si8 len = std::min(block_len, run.end - s);
      BlockSpec b;
      b.samples.assign(samples.begin() + s, samples.begin() + s + len);
      b.start_sample = seg_base_sample + stored;
      b.start_uutc = start_uutc + static_cast<si8>(std::llround(static_cast<sf8>(s) / fs * 1e6));
      b.discontinuity = first_block_of_run;  // first block of each run (and of segment)
      blocks.push_back(std::move(b));
      stored += len;
      first_block_of_run = false;
    }
  }

  const bool append = !new_segment && st.n_segments > 0;
  const int seg = append ? st.last_segment : st.last_segment + 1;
  std::string seg_dir =
      (fs::path(mefd_path_) / (channel + ".timd") / (segment_name(channel, seg) + ".segd")).string();

  SegmentSpec spec;
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
  spec.durable = durable_;
  // Only a NEW segment takes the session's offset; an append keeps the one
  // already in the segment it extends (append_time_series_segment).
  if (!append) spec.recording_time_offset = new_segment_rto(channel, st);

  // Only pass on a maximum this writer measured itself. A segment adopted from
  // disk leaves the flag false, so the append bounds the blocks it cannot see
  // rather than trusting a declaration it has no way to verify.
  if (append && st.difference_bytes_exact) spec.known_difference_bytes = st.max_difference_bytes;

  ui4 written_difference_bytes = 0;
  if (append) {
    // fs / conversion-factor / start-time conflicts are validated against the
    // on-disk metadata inside append_time_series_segment.
    append_time_series_segment(seg_dir, spec, blocks, n_threads_.load(), &written_difference_bytes,
                               &st.index_cache);
  } else {
    // Everything that can refuse runs BEFORE a directory exists: a refused
    // write must leave the tree exactly as it was. A directory left behind
    // would be adopted as a segment by the next Writer.
    for (const auto& b : blocks)
      require_storable_time(b.start_uutc, spec.recording_time_offset, "block start time");
    const fs::path timd = fs::path(mefd_path_) / (channel + ".timd");
    const bool created_timd = !fs::exists(timd);
    const bool created_segd = !fs::exists(seg_dir);
    fs::create_directories(seg_dir);
    try {
      write_time_series_segment(seg_dir, spec, blocks, n_threads_.load(), &written_difference_bytes);
    } catch (...) {
      std::error_code ec;
      if (created_timd)
        fs::remove_all(timd, ec);
      else if (created_segd)
        fs::remove_all(seg_dir, ec);
      throw;
    }
    // A fresh segment holds nothing but the blocks just encoded, so from here
    // the running maximum is exact — and the index summary from the previous
    // segment describes a file this channel is no longer appending to.
    st.max_difference_bytes = 0;
    st.difference_bytes_exact = true;
    st.index_cache = AppendIndexCache{};
  }
  st.max_difference_bytes = std::max(st.max_difference_bytes, written_difference_bytes);

  st.sampling_frequency = fs;
  st.units_conversion_factor = ufact;
  if (!append) {
    ++st.n_segments;
    note_new_segment(channel, (fs::path(seg_dir) / (segment_name(channel, seg) + ".tmet")).string(),
                     spec.recording_time_offset);
    // The slot is a real segment now: drop the stale "ignored" report.
    const auto e = empty_segment_dirs_.find(seg_dir);
    if (e != empty_segment_dirs_.end()) {
      unreadable_metadata_.erase(
          std::remove(unreadable_metadata_.begin(), unreadable_metadata_.end(), e->second),
          unreadable_metadata_.end());
      empty_segment_dirs_.erase(e);
    }
  }
  st.last_segment = seg;
  st.total_samples += stored;
  st.last_end_uutc =
      start_uutc + static_cast<si8>(std::llround(static_cast<sf8>(n) / fs * 1e6));

  summary.samples_written = stored;
  summary.blocks = static_cast<si8>(blocks.size());
  summary.segment = seg;
  return summary;
}

}  // namespace mef3io
