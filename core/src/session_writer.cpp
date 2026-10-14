// mef3io — high-level session writer.
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
  if (overwrite && fs::exists(mefd_path_)) fs::remove_all(mefd_path_);
  fs::create_directories(mefd_path_);
  session_name_ = fs::path(mefd_path_).stem().string();

  // Adopt existing channels/segments so appends continue where the session
  // left off. Metadata is read back lazily (hydrate) on the first write.
  for (const auto& entry : fs::directory_iterator(mefd_path_)) {
    if (!entry.is_directory() || entry.path().extension() != ".timd") continue;
    std::string ch = entry.path().stem().string();
    int nseg = 0;
    for (const auto& s : fs::directory_iterator(entry.path()))
      if (s.is_directory() && s.path().extension() == ".segd") ++nseg;
    channels_[ch].n_segments = nseg;
    channels_[ch].hydrated = (nseg == 0);
  }
}

void SessionWriter::hydrate(const std::string& channel, ChannelState& st) {
  if (st.hydrated) return;
  st.hydrated = true;
  const std::string base = segment_name(channel, st.n_segments - 1);
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
  const si8 rto = segment_rto(md);
  st.sampling_frequency = md.section2.sampling_frequency;
  st.units_conversion_factor = md.section2.units_conversion_factor;
  // start_sample is channel-wide, so the last segment's start + count is the
  // channel total.
  st.total_samples = md.section2.start_sample + md.section2.number_of_samples;
  const si8 end_stored = md.universal_header.end_time;
  st.last_end_uutc = end_stored == fmt::UUTC_NO_ENTRY ? 0 : to_user_time(end_stored, rto);
}

SessionWriter::LevelOffset SessionWriter::level_offset(const std::optional<std::string>& channel) {
  // What the on-disk segments of the level (one channel, or the whole
  // session) say about the recording-time offset — the value the reader
  // resolves that level's times with (Session::read_records).
  //   Agreed   every readable segment has the same offset (0 when none);
  //   Disputed two readable segments differ (`detail` names them);
  //   Unknown  a segment's section 3 is locked (level-1 access only), so its
  //            offset cannot be seen: unknown is NOT 0 (`detail` names it).
  // A .tmet that cannot be read at all is SKIPPED and reported
  // (unreadable_metadata()), never thrown: it belongs to some other segment,
  // and failing here made one damaged file block every unrelated write.
  LevelOffset out;
  std::string agreed_where;
  bool have = false;
  if (!fs::exists(mefd_path_)) return out;
  for (const auto& timd : fs::directory_iterator(mefd_path_)) {
    if (!timd.is_directory() || timd.path().extension() != ".timd") continue;
    if (channel && timd.path().stem().string() != *channel) continue;
    for (const auto& segd : fs::directory_iterator(timd.path())) {
      if (!segd.is_directory() || segd.path().extension() != ".segd") continue;
      const fs::path tmet = segd.path() / (segd.path().stem().string() + ".tmet");
      if (!fs::exists(tmet)) continue;
      TimeSeriesMetadata md;
      try {
        std::ifstream f(tmet, std::ios::binary);
        std::vector<ui1> bytes((std::istreambuf_iterator<char>(f)),
                               std::istreambuf_iterator<char>());
        md = load_time_series_metadata(bytes, password_2_.empty() ? password_1_ : password_2_);
      } catch (const std::exception& e) {
        const std::string what = tmet.string() + ": " + e.what();
        if (std::find(unreadable_metadata_.begin(), unreadable_metadata_.end(), what) ==
            unreadable_metadata_.end())
          unreadable_metadata_.push_back(what);
        continue;
      }
      ++out.readable;
      if (!md.section3_available) {
        out.state = LevelOffset::State::Unknown;
        out.detail = tmet.string();
        return out;  // nothing else can make it known
      }
      const si8 rto = segment_rto(md);
      if (!have) {
        have = true;
        out.rto = rto;
        agreed_where = tmet.string();
      } else if (out.state == LevelOffset::State::Agreed && rto != out.rto) {
        out.state = LevelOffset::State::Disputed;
        out.detail = std::to_string(out.rto) + " in " + agreed_where + ", " +
                     std::to_string(rto) + " in " + tmet.string();
      }
    }
  }
  return out;
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
}  // namespace

si8 SessionWriter::new_segment_rto(const std::string& channel, ChannelState& st) {
  // A NEW segment inherits the offset its neighbours agree on. mef3io never
  // creates an offset itself (its own sessions are 0), but a foreign session
  // that used meflib's anonymization has one, and a new segment written with
  // 0 would make its channel and the session DISAGREE — after which their
  // records could no longer be placed — and would write the real date.
  //   * its own channel's readable segments first (the level the segment
  //     joins, and the cheapest scan: no other channel's metadata is read);
  //   * for a new channel, or one with no readable segment, the session's;
  //   * a locked or disputed offset is refused, never guessed.
  // Cached per channel: the segments this writer adds carry the same value,
  // so the agreement it found still holds, and a months-long recording does
  // not rescan its metadata at every new segment.
  if (st.new_segment_rto) return *st.new_segment_rto;
  const std::string what = "a new segment of channel " + channel;
  if (st.n_segments > 0) {
    const LevelOffset ch = level_offset(channel);
    if (ch.state == LevelOffset::State::Unknown) refuse_unknown_offset(what, ch.detail);
    if (ch.state == LevelOffset::State::Disputed) refuse_disputed_offset(what, ch.detail);
    if (ch.readable > 0) {
      st.new_segment_rto = ch.rto;
      return ch.rto;
    }
  }
  if (!session_offset_) session_offset_ = level_offset(std::nullopt);
  if (session_offset_->state == LevelOffset::State::Unknown)
    refuse_unknown_offset(what, session_offset_->detail);
  if (session_offset_->state == LevelOffset::State::Disputed)
    refuse_disputed_offset(what, session_offset_->detail);
  st.new_segment_rto = session_offset_->rto;
  return session_offset_->rto;
}

void SessionWriter::write_records(std::optional<std::string> channel,
                                  const std::vector<Record>& records) {
  if (records.empty()) return;  // write_records writes nothing for an empty batch
  // Store the times the way the reader resolves this level (time.hpp,
  // to_stored_time): -t at offset 0 (byte-identical to earlier releases),
  // rto - t under an anonymization offset (date stays hidden). A locked or
  // disputed offset, or a time at or before the offset, is refused.
  const std::string what =
      channel ? "channel-level records of " + *channel : std::string("session-level records");
  const LevelOffset level = level_offset(channel);
  if (level.state == LevelOffset::State::Unknown) refuse_unknown_offset(what, level.detail);
  if (level.state == LevelOffset::State::Disputed) refuse_disputed_offset(what, level.detail);
  for (const auto& r : records) require_storable_time(r.time, level.rto, "record time");
  std::string dir, base;
  if (channel) {
    dir = (fs::path(mefd_path_) / (*channel + ".timd")).string();
    fs::create_directories(dir);
    base = *channel;
  } else {
    dir = mefd_path_;
    base = session_name_;
  }
  mef3io::write_records(dir, base, session_name_, channel.value_or(""), 0, records, level.rto,
                        password_1_, password_2_);
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
    summary.segment = (!new_segment && st.n_segments > 0) ? st.n_segments - 1 : st.n_segments;
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
  const int seg = append ? st.n_segments - 1 : st.n_segments;
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
    append_time_series_segment(seg_dir, spec, blocks, n_threads_, &written_difference_bytes,
                               &st.index_cache);
  } else {
    fs::create_directories(seg_dir);
    write_time_series_segment(seg_dir, spec, blocks, n_threads_, &written_difference_bytes);
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
  st.n_segments = seg + 1;
  st.total_samples += stored;
  st.last_end_uutc =
      start_uutc + static_cast<si8>(std::llround(static_cast<sf8>(n) / fs * 1e6));

  summary.samples_written = stored;
  summary.blocks = static_cast<si8>(blocks.size());
  summary.segment = seg;
  return summary;
}

}  // namespace mef3io
