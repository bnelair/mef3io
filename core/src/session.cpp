// mef3io — session tree discovery and low-level indexed reads. All file access
// goes through the SessionSource, so a session reads identically from a .mefd
// directory and from an uncompressed .mefd.tar archive.
#include "mef3io/session.hpp"

#include <algorithm>
#include <cmath>

#include "mef3io/byteio.hpp"
#include "mef3io/errors.hpp"
#include "mef3io/red.hpp"
#include "mef3io/validate.hpp"

namespace mef3io {
namespace {

// Convert an on-disk MEF time to a user-facing absolute uUTC. meflib marks
// "recording-time-offset applied" times by negating them; the true time is
// recovered as (-stored + rto). Non-negative values are already user times
// (offset not applied). Mirrors meflib remove_recording_time_offset.
si8 to_user_time(si8 t, si8 rto) {
  if (t == fmt::UUTC_NO_ENTRY) return t;
  if (t >= 0) return t;
  return -t + rto;
}

int segment_number_from_name(const std::string& stem) {
  // "<channel>-000000" -> 0
  auto dash = stem.rfind('-');
  if (dash == std::string::npos) return 0;
  try {
    return std::stoi(stem.substr(dash + 1));
  } catch (...) {
    return 0;
  }
}

// Entry count of a .tidx image, validating the size first. A truncated or
// empty file (seen on flaky network mounts) must fail loudly — the naive
// (size - 1024) on an unsigned type underflows into billions of entries.
std::size_t index_entry_count(std::span<const ui1> idx, const std::string& path) {
  if (idx.size() < static_cast<std::size_t>(fmt::UNIVERSAL_HEADER_BYTES))
    throw FormatError("index file smaller than its universal header (truncated?): " + path);
  const std::size_t body = idx.size() - fmt::UNIVERSAL_HEADER_BYTES;
  if (body % fmt::TIME_SERIES_INDEX_BYTES != 0)
    throw FormatError("index file body is not a whole number of entries (truncated?): " + path);
  return body / fmt::TIME_SERIES_INDEX_BYTES;
}

}  // namespace

Session::Session(const std::string& mefd_path, std::string password, bool strict)
    : mefd_path_(mefd_path), source_(open_session_source(mefd_path)),
      password_(std::move(password)), strict_(strict) {
  discover();
}

void Session::discover() {
  for (const auto& entry : source_->list_dir("")) {
    if (!entry.is_dir) continue;
    if (entry.name.ends_with(".vidd")) continue;  // video channels are out of scope; skip silently
    if (!entry.name.ends_with(".timd")) continue;

    Channel ch;
    ch.info.name = entry.name.substr(0, entry.name.size() - 5);

    for (const auto& seg_entry : source_->list_dir(entry.name)) {
      if (!seg_entry.is_dir || !seg_entry.name.ends_with(".segd")) continue;
      const std::string seg_stem = seg_entry.name.substr(0, seg_entry.name.size() - 5);
      const std::string seg_dir = entry.name + "/" + seg_entry.name;
      SegmentReader seg;
      seg.segment_number = segment_number_from_name(seg_stem);
      seg.tmet_path = seg_dir + "/" + seg_stem + ".tmet";
      seg.tidx_path = seg_dir + "/" + seg_stem + ".tidx";
      seg.tdat_path = seg_dir + "/" + seg_stem + ".tdat";
      if (source_->exists(seg.tmet_path) && source_->exists(seg.tidx_path) &&
          source_->exists(seg.tdat_path))
        ch.segments.push_back(std::move(seg));
    }
    if (ch.segments.empty()) continue;

    std::sort(ch.segments.begin(), ch.segments.end(),
              [](const SegmentReader& a, const SegmentReader& b) {
                return a.segment_number < b.segment_number;
              });

    load_channel_basic_info(ch.info.name, ch);
    channel_names_.push_back(ch.info.name);
    channels_.emplace(ch.info.name, std::move(ch));
  }
  std::sort(channel_names_.begin(), channel_names_.end());
}

TimeSeriesMetadata& Session::segment_metadata(SegmentReader& seg) {
  if (!seg.metadata) {
    auto bytes = source_->read_all(seg.tmet_path);
    seg.metadata = load_time_series_metadata(bytes, password_);
  }
  return *seg.metadata;
}

std::span<const ui1> Session::segment_index(SegmentReader& seg) {
  if (seg.tidx_bytes.empty()) seg.tidx_bytes = source_->read_all(seg.tidx_path);
  seg.index_last_used = ++index_tick_;
  return seg.tidx_bytes;
}

void Session::load_channel_basic_info(const std::string& name, Channel& ch) {
  ChannelInfo& info = ch.info;
  bool first = true;
  int readable = 0;
  for (auto& seg : ch.segments) {
    // Nothing below can throw once the metadata is in hand, so containing the
    // whole body cannot leave `info` half-updated by a segment that failed.
    const bool ok = with_segment(name, seg, [&] {
    auto& md = segment_metadata(seg);
    si8 rto = md.section3_available ? md.section3.recording_time_offset : 0;
    if (rto == fmt::UUTC_NO_ENTRY) rto = 0;
    si8 seg_start = to_user_time(md.universal_header.start_time, rto);
    si8 seg_end = to_user_time(md.universal_header.end_time, rto);
    if (first) {
      info.sampling_frequency = md.section2.sampling_frequency;
      info.units_conversion_factor = md.section2.units_conversion_factor;
      info.units_description = md.section2.units_description;
      info.recording_time_offset = rto;
      info.start_time = seg_start;
      info.end_time = seg_end;
      info.session_description = md.section2.session_description;
      info.channel_description = md.section2.channel_description;
      info.reference_description = md.section2.reference_description;
      info.acquisition_channel_number = md.section2.acquisition_channel_number;
      info.low_frequency_filter = md.section2.low_frequency_filter_setting;
      info.high_frequency_filter = md.section2.high_frequency_filter_setting;
      info.notch_filter = md.section2.notch_filter_frequency_setting;
      info.line_frequency = md.section2.ac_line_frequency;
      info.section3_available = md.section3_available;
      if (md.section3_available) {
        info.gmt_offset = md.section3.gmt_offset;
        info.subject_name_1 = md.section3.subject_name_1;
        info.subject_name_2 = md.section3.subject_name_2;
        info.subject_id = md.section3.subject_id;
        info.recording_location = md.section3.recording_location;
      }
      first = false;
    } else {
      info.start_time = std::min(info.start_time, seg_start);
      info.end_time = std::max(info.end_time, seg_end);
    }
    info.number_of_samples += md.section2.number_of_samples;
    });
    if (ok) ++readable;
  }
  // Count what can actually be read: segment_map lists only these, and a count
  // that disagreed with it would make the map look truncated.
  info.n_segments = readable;
}

const ChannelInfo& Session::channel_info(const std::string& name) const {
  auto it = channels_.find(name);
  if (it == channels_.end()) throw std::out_of_range("no such channel: " + name);
  return it->second.info;
}

std::vector<DataRun> Session::read_runs(const std::string& channel, std::optional<si8> t0_opt,
                                        std::optional<si8> t1_opt) {
  auto it = channels_.find(channel);
  if (it == channels_.end()) throw std::out_of_range("no such channel: " + channel);
  Channel& ch = it->second;

  const si8 t0 = t0_opt.value_or(ch.info.start_time);
  const si8 t1 = t1_opt.value_or(ch.info.end_time + 1);
  const sf8 fs = ch.info.sampling_frequency;

  std::vector<DataRun> runs;
  DataRun* current = nullptr;
  si8 expected_next_sample = -1;

  for (auto& seg : ch.segments) {
    const std::size_t mark = runs.size();
    if (!with_segment(channel, seg, [&] {
    auto& md = segment_metadata(seg);
    si8 rto = md.section3_available ? md.section3.recording_time_offset : 0;
    if (rto == fmt::UUTC_NO_ENTRY) rto = 0;

    auto idx = segment_index(seg);
    const std::size_t n_entries = index_entry_count(idx, source_->describe(seg.tidx_path));

    crypto::AccessKeys keys = crypto::validate_password(
        password_, md.universal_header.level_1_password_validation_field,
        md.universal_header.level_2_password_validation_field);

    // Select the blocks the window actually needs FIRST, then read exactly the
    // byte extent they span — never the whole .tdat. A segment of a long
    // recording is tens of gigabytes, and reading it whole to serve a
    // one-minute request allocates the entire file. This mirrors what
    // collect_blocks already does for the windowed read path.
    std::vector<fmt::TimeSeriesIndex> hits;
    for (std::size_t i = 0; i < n_entries; ++i) {
      auto e = fmt::TimeSeriesIndex::parse(
          idx.subspan(fmt::UNIVERSAL_HEADER_BYTES + i * fmt::TIME_SERIES_INDEX_BYTES,
                      fmt::TIME_SERIES_INDEX_BYTES));
      if (e.file_offset < 0 || e.number_of_samples == 0) continue;
      const si8 block_start = to_user_time(e.start_time, rto);
      const si8 block_end =
          block_start + static_cast<si8>(std::llround(e.number_of_samples * 1e6 / fs));
      if (block_end <= t0 || block_start >= t1) continue;  // outside requested range
      hits.push_back(e);
    }
    if (hits.empty()) return;  // `return` from the per-segment lambda, not `continue`

    // Do not assume the index is sorted: take the true extent, and check it
    // against the real file size so a damaged index fails loudly rather than
    // reading wild.
    std::size_t range_begin = static_cast<std::size_t>(hits.front().file_offset);
    std::size_t range_end = range_begin;
    for (const auto& e : hits) {
      const std::size_t off = static_cast<std::size_t>(e.file_offset);
      range_begin = std::min(range_begin, off);
      range_end = std::max(range_end, off + e.block_bytes);
    }
    const std::size_t tdat_size = static_cast<std::size_t>(source_->file_size(seg.tdat_path));
    if (range_end > tdat_size)
      throw FormatError("index points past end of .tdat: " + source_->describe(seg.tdat_path));
    const std::vector<ui1> tdat = source_->read_range(seg.tdat_path, range_begin,
                                                      range_end - range_begin);

    for (const auto& e : hits) {
      std::span<const ui1> block(tdat.data() + (static_cast<std::size_t>(e.file_offset) -
                                                range_begin),
                                 e.block_bytes);
      auto decoded = red::decode_block(block, keys);

      // Start a new run on discontinuity or a sample-index gap.
      bool contiguous = current != nullptr && !decoded.discontinuity &&
                        e.start_sample == expected_next_sample;
      if (!contiguous) {
        runs.push_back(DataRun{to_user_time(e.start_time, rto), e.start_sample, {}});
        current = &runs.back();
      }
      current->samples.insert(current->samples.end(), decoded.samples.begin(),
                              decoded.samples.end());
      expected_next_sample = e.start_sample + e.number_of_samples;
    }
    })) {
      runs.resize(mark);
      // `current` pointed into `runs` and the resize may have moved it. Drop
      // it: the next segment must start a new run anyway, because the one that
      // failed leaves a gap where its samples should have been.
      current = nullptr;
      expected_next_sample = -1;
    }
  }
  // Safe point: the operation is finished, so nothing holds a span into a
  // cached index. The index is ~2% of the data, so an unbounded cache grows
  // to several GB on a session of a few hundred gigabytes.
  trim_index_cache();
  return runs;
}

std::vector<Record> Session::read_records(std::optional<std::string> channel) {
  // Locate the .rdat file: channel-level lives in <channel>.timd/, session-
  // level in the .mefd root. rto comes from the channel (0 for session-level).
  std::string rdat_path;
  si8 rto = 0;
  if (channel) {
    auto it = channels_.find(*channel);
    if (it == channels_.end()) throw std::out_of_range("no such channel: " + *channel);
    rto = it->second.info.recording_time_offset;
    std::string cand = *channel + ".timd/" + *channel + ".rdat";
    if (source_->exists(cand)) rdat_path = cand;
  } else {
    for (const auto& e : source_->list_dir(""))
      if (!e.is_dir && e.name.ends_with(".rdat")) {
        rdat_path = e.name;
        break;
      }
  }
  if (rdat_path.empty()) return {};

  auto bytes = source_->read_all(rdat_path);
  auto uh = fmt::UniversalHeader::parse(bytes);
  crypto::AccessKeys keys = crypto::validate_password(
      password_, uh.level_1_password_validation_field, uh.level_2_password_validation_field);
  return parse_records(bytes, rto, keys);
}

BlockJobs Session::collect_blocks(const std::string& channel, std::optional<si8> t0_opt,
                                  std::optional<si8> t1_opt) {
  auto it = channels_.find(channel);
  if (it == channels_.end()) throw std::out_of_range("no such channel: " + channel);
  Channel& ch = it->second;

  BlockJobs out;
  out.sampling_frequency = ch.info.sampling_frequency;
  out.units_conversion_factor = ch.info.units_conversion_factor;
  const si8 t0 = t0_opt.value_or(ch.info.start_time);
  const si8 t1 = t1_opt.value_or(ch.info.end_time + 1);
  const sf8 fs = ch.info.sampling_frequency;

  for (auto& seg : ch.segments) {
    const std::size_t mark_buf = out.buffers.size(), mark_job = out.jobs.size(),
                      mark_mm = out.mismatches.size();
    if (!with_segment(channel, seg, [&] {
    auto& md = segment_metadata(seg);
    si8 rto = md.section3_available ? md.section3.recording_time_offset : 0;
    if (rto == fmt::UUTC_NO_ENTRY) rto = 0;
    crypto::AccessKeys keys = crypto::validate_password(
        password_, md.universal_header.level_1_password_validation_field,
        md.universal_header.level_2_password_validation_field);

    auto idx = segment_index(seg);
    const std::size_t n_entries = index_entry_count(idx, source_->describe(seg.tidx_path));

    // Determine whether any block in this segment is in range before loading
    // the (potentially large) .tdat.
    std::vector<fmt::TimeSeriesIndex> hits;
    for (std::size_t i = 0; i < n_entries; ++i) {
      auto e = fmt::TimeSeriesIndex::parse(
          idx.subspan(fmt::UNIVERSAL_HEADER_BYTES + i * fmt::TIME_SERIES_INDEX_BYTES,
                      fmt::TIME_SERIES_INDEX_BYTES));
      if (e.file_offset < 0 || e.number_of_samples == 0) continue;
      si8 bstart = to_user_time(e.start_time, rto);
      si8 bend = bstart + static_cast<si8>(std::llround(e.number_of_samples * 1e6 / fs));
      if (bend <= t0 || bstart >= t1) continue;
      hits.push_back(e);
    }
    if (hits.empty()) return;  // `return` from the per-segment lambda, not `continue`

    // Needed blocks are normally contiguous in the file, so read one byte
    // range covering them rather than the whole (huge) .tdat — but do NOT
    // trust the index to be sorted: compute the true min/max span and check
    // every block against it and against the actual file size. Damaged or
    // foreign indices must fail with an exception, never with a wild read.
    std::size_t range_begin = static_cast<std::size_t>(hits.front().file_offset);
    std::size_t range_end = range_begin;
    for (const auto& e : hits) {
      const std::size_t off = static_cast<std::size_t>(e.file_offset);
      range_begin = std::min(range_begin, off);
      range_end = std::max(range_end, off + e.block_bytes);
    }
    const std::size_t tdat_size = static_cast<std::size_t>(source_->file_size(seg.tdat_path));
    if (range_end > tdat_size)
      throw FormatError("index points past end of .tdat: " + source_->describe(seg.tdat_path));
    std::size_t buf_index = out.buffers.size();
    out.buffers.push_back(
        source_->read_range(seg.tdat_path, range_begin, range_end - range_begin));
    const std::vector<ui1>& buf = out.buffers[buf_index];
    for (std::size_t hi = 0; hi < hits.size(); ++hi) {
      const auto& e = hits[hi];
      BlockJob job;
      job.buffer_index = buf_index;
      job.offset = static_cast<std::size_t>(e.file_offset) - range_begin;
      job.block_bytes = e.block_bytes;
      job.index_start_uutc = to_user_time(e.start_time, rto);
      job.index_number_of_samples = e.number_of_samples;
      job.keys = keys;

      // The RED block header is the authority for WHERE this block's samples
      // go and HOW MANY there are; the index only chose the byte range. Both
      // values are read straight out of the bytes just fetched — two field
      // reads at fixed offsets, not RedBlockHeader::parse, which would copy
      // the 256-byte statistics table for every block on the read path.
      if (job.offset + static_cast<std::size_t>(fmt::RED_BLOCK_HEADER_BYTES) > buf.size())
        throw FormatError("block is smaller than a RED header: " +
                          source_->describe(seg.tdat_path));
      const std::span<const ui1> hdr(buf.data() + job.offset, fmt::RED_BLOCK_HEADER_BYTES);
      job.start_uutc = to_user_time(byteio::read<si8>(hdr, 40), rto);
      job.number_of_samples = byteio::read<ui4>(hdr, 32);

      if (job.start_uutc != job.index_start_uutc ||
          job.number_of_samples != job.index_number_of_samples) {
        out.mismatches.push_back(BlockCopyMismatch{
            source_->describe(seg.tdat_path), hi, job.index_start_uutc, job.start_uutc,
            job.index_number_of_samples, job.number_of_samples});
      }
      out.jobs.push_back(job);
    }
    })) {
      // Partial output from a segment that threw is worse than none: the
      // samples it did place would sit next to a gap it never finished.
      out.buffers.resize(mark_buf);
      out.jobs.resize(mark_job);
      out.mismatches.resize(mark_mm);
    }
  }
  // Safe point: the operation is finished, so nothing holds a span into a
  // cached index. The index is ~2% of the data, so an unbounded cache grows
  // to several GB on a session of a few hundred gigabytes.
  trim_index_cache();
  return out;
}

std::size_t Session::index_cache_bytes() const {
  std::size_t total = 0;
  for (const auto& name : channel_names_)
    for (const auto& seg : channels_.at(name).segments) total += seg.tidx_bytes.capacity();
  return total;
}

void Session::trim_index_cache() {
  if (index_budget_ == 0) return;
  std::size_t total = index_cache_bytes();
  if (total <= index_budget_) return;

  // Oldest first. Only called once a public operation has finished, so no
  // caller is holding a span into any of these buffers — evicting one while it
  // was in use would be a use-after-free, which is why this is never done
  // mid-read.
  struct Victim {
    std::uint64_t used;
    SegmentReader* seg;
  };
  std::vector<Victim> victims;
  for (const auto& name : channel_names_)
    for (auto& seg : channels_.at(name).segments)
      if (!seg.tidx_bytes.empty()) victims.push_back({seg.index_last_used, &seg});
  std::sort(victims.begin(), victims.end(),
            [](const Victim& a, const Victim& b) { return a.used < b.used; });

  for (const auto& v : victims) {
    if (total <= index_budget_) break;
    total -= v.seg->tidx_bytes.capacity();
    std::vector<ui1>().swap(v.seg->tidx_bytes);  // actually release the memory
    v.seg->index_last_used = 0;
  }
}

std::vector<DeclarationIssue> Session::declaration_issues() const {
  std::vector<DeclarationIssue> out;
  for (const auto& name : channel_names_) {
    const Channel& ch = channels_.at(name);
    for (const auto& seg : ch.segments) {
      if (!seg.metadata) continue;  // not loaded (should not happen after open)
      for (const auto& field : unset_declarations(seg.metadata->section2))
        out.push_back({name, seg.segment_number, field});
    }
  }
  return out;
}

std::vector<SegmentInfo> Session::segment_map(const std::string& channel) {
  auto it = channels_.find(channel);
  if (it == channels_.end()) throw std::out_of_range("no such channel: " + channel);
  Channel& ch = it->second;

  std::vector<SegmentInfo> out;
  out.reserve(ch.segments.size());
  for (auto& seg : ch.segments) {
    const std::size_t mark = out.size();
    if (!with_segment(channel, seg, [&] {
    auto& md = segment_metadata(seg);
    si8 rto = md.section3_available ? md.section3.recording_time_offset : 0;
    if (rto == fmt::UUTC_NO_ENTRY) rto = 0;
    SegmentInfo si;
    si.segment_number = seg.segment_number;
    si.path = source_->describe(seg.tmet_path.substr(0, seg.tmet_path.rfind('/')));
    si.start_time = to_user_time(md.universal_header.start_time, rto);
    si.end_time = to_user_time(md.universal_header.end_time, rto);
    si.start_sample = md.section2.start_sample;
    si.number_of_samples = md.section2.number_of_samples;
    si.number_of_blocks = md.section2.number_of_blocks;
    out.push_back(std::move(si));
    }))
      out.resize(mark);  // a segment that threw part-way contributes nothing
  }
  // Safe point: the operation is finished, so nothing holds a span into a
  // cached index. The index is ~2% of the data, so an unbounded cache grows
  // to several GB on a session of a few hundred gigabytes.
  trim_index_cache();
  return out;
}

std::vector<BlockIndexEntry> Session::read_index(const std::string& channel) {
  auto it = channels_.find(channel);
  if (it == channels_.end()) throw std::out_of_range("no such channel: " + channel);
  Channel& ch = it->second;

  std::vector<BlockIndexEntry> out;
  for (auto& seg : ch.segments) {
    const std::size_t mark = out.size();
    if (!with_segment(channel, seg, [&] {
    auto& md = segment_metadata(seg);
    si8 rto = md.section3_available ? md.section3.recording_time_offset : 0;
    if (rto == fmt::UUTC_NO_ENTRY) rto = 0;
    auto idx = segment_index(seg);
    const std::size_t n = index_entry_count(idx, source_->describe(seg.tidx_path));
    for (std::size_t i = 0; i < n; ++i) {
      auto e = fmt::TimeSeriesIndex::parse(
          idx.subspan(fmt::UNIVERSAL_HEADER_BYTES + i * fmt::TIME_SERIES_INDEX_BYTES,
                      fmt::TIME_SERIES_INDEX_BYTES));
      if (e.file_offset < 0 || e.number_of_samples == 0) continue;
      BlockIndexEntry b;
      b.start_uutc = to_user_time(e.start_time, rto);
      b.start_sample = e.start_sample;
      b.number_of_samples = e.number_of_samples;
      b.maximum_sample_value = e.maximum_sample_value;
      b.minimum_sample_value = e.minimum_sample_value;
      b.discontinuity = (e.red_block_flags & fmt::RedBlockHeader::DISCONTINUITY_MASK) != 0;
      out.push_back(b);
    }
    }))
      out.resize(mark);
  }
  // Safe point: the operation is finished, so nothing holds a span into a
  // cached index. The index is ~2% of the data, so an unbounded cache grows
  // to several GB on a session of a few hundred gigabytes.
  trim_index_cache();
  return out;
}

}  // namespace mef3io
