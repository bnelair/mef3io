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
#include "mef3io/writer.hpp"

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
  si8 rto = md.section3_available ? md.section3.recording_time_offset : 0;
  if (rto == fmt::UUTC_NO_ENTRY) rto = 0;
  st.sampling_frequency = md.section2.sampling_frequency;
  st.units_conversion_factor = md.section2.units_conversion_factor;
  // start_sample is channel-wide, so the last segment's start + count is the
  // channel total.
  st.total_samples = md.section2.start_sample + md.section2.number_of_samples;
  si8 end_stored = md.universal_header.end_time;
  if (end_stored == fmt::UUTC_NO_ENTRY)
    st.last_end_uutc = 0;
  else
    st.last_end_uutc = (end_stored >= 0) ? end_stored : -end_stored + rto;
}

void SessionWriter::write_records(std::optional<std::string> channel,
                                  const std::vector<Record>& records) {
  std::string dir, base;
  if (channel) {
    dir = (fs::path(mefd_path_) / (*channel + ".timd")).string();
    fs::create_directories(dir);
    base = *channel;
  } else {
    dir = mefd_path_;
    base = session_name_;
  }
  mef3io::write_records(dir, base, session_name_, channel.value_or(""), 0, records,
                        /*rto=*/0, password_1_, password_2_);
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
  spec.gmt_offset = -21600;
  spec.password_1 = password_1_;
  spec.password_2 = password_2_;

  if (append) {
    // fs / conversion-factor / start-time conflicts are validated against the
    // on-disk metadata inside append_time_series_segment.
    append_time_series_segment(seg_dir, spec, blocks, n_threads_);
  } else {
    fs::create_directories(seg_dir);
    write_time_series_segment(seg_dir, spec, blocks, n_threads_);
  }

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
