// mef3io — session tree discovery and low-level indexed reads. All file access
// goes through the SessionSource, so a session reads identically from a .mefd
// directory and from an uncompressed .mefd.tar archive.
#include "mef3io/session.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <cstring>
#include <stdexcept>

#include "mef3io/byteio.hpp"
#include "mef3io/errors.hpp"
#include "mef3io/parallel.hpp"
#include "mef3io/red.hpp"
#include "mef3io/validate.hpp"
#include "mef3io/time.hpp"
#include "session_layout.hpp"

namespace mef3io {
namespace {

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

Session::Session(const std::string& mefd_path, std::string password, bool strict, int n_threads)
    : mefd_path_(mefd_path), source_(open_session_source(mefd_path)),
      password_(std::move(password)), strict_(strict) {
  n_threads_.store(check_threads(n_threads));
  discover();
}

Session::Session(std::shared_ptr<const SessionSource> source, std::string password, bool strict,
                 int n_threads)
    : mefd_path_(source ? source->describe("") : std::string()), source_(std::move(source)),
      password_(std::move(password)), strict_(strict) {
  if (!source_) throw std::invalid_argument("Session: null source");
  n_threads_.store(check_threads(n_threads));
  discover();
}

void Session::discover() {
  // The open is mostly waiting on the file system (on a network share every
  // request is a round trip), so it runs on the object's thread budget
  // resolved for FILE ACCESS. Three parallel passes, each assembled in order:
  // the layout (session_layout.hpp: channel listings, segment file checks),
  // then every segment's .tmet read + parse, then — serially, in the order a
  // one-thread open uses — the channel infos. A failure is therefore raised
  // (strict) or recorded in problems() (lenient) exactly as a serial open
  // would: same error, same order, whatever the thread count.
  const int threads = n_threads_.load();
  auto layout = detail::discover_layout(*source_, threads);
  record_only_channels_ = std::move(layout.record_only_channels);

  std::vector<Channel> chans(layout.channels.size());
  std::vector<SegmentReader*> flat;
  std::vector<std::size_t> first(layout.channels.size(), 0);
  for (std::size_t c = 0; c < layout.channels.size(); ++c) {
    auto& lc = layout.channels[c];
    chans[c].info.name = lc.name;
    for (auto& ls : lc.segments) {
      SegmentReader seg;
      seg.segment_number = ls.number;
      seg.tmet_path = std::move(ls.tmet);
      seg.tidx_path = std::move(ls.tidx);
      seg.tdat_path = std::move(ls.tdat);
      chans[c].segments.push_back(std::move(seg));
    }
  }
  // Pointers only once every vector has its final size.
  for (std::size_t c = 0; c < chans.size(); ++c) {
    first[c] = flat.size();
    for (auto& seg : chans[c].segments) flat.push_back(&seg);
  }
  // Each item writes only its own segment: nothing is shared but the source,
  // which is safe for concurrent calls (DirectorySource and TarSource open
  // their own stream per request; TarSource's tables are read-only after
  // construction).
  const auto errors = parallel_try_each(
      flat.size(), threads,
      [&](std::size_t i) {
        // ONE bounded request: .tmet is a fixed-length record and the loader
        // never looks past METADATA_FILE_BYTES (foreign padding after it is
        // ignored, as the bounded body CRC requires). A short file comes back
        // short and fails the loader's size check exactly as before (#141).
        auto bytes = source_->read_head(flat[i]->tmet_path, fmt::METADATA_FILE_BYTES);
        flat[i]->metadata = load_time_series_metadata(bytes, password_);
      },
      Work::Io);

  for (std::size_t c = 0; c < chans.size(); ++c) {
    Channel& ch = chans[c];
    const std::string name = ch.info.name;
    load_channel_basic_info(name, ch, errors.data() + first[c]);
    channel_names_.push_back(name);
    channels_.emplace(name, std::move(ch));
  }
  std::sort(channel_names_.begin(), channel_names_.end());
}

const TimeSeriesMetadata& Session::segment_metadata(const SegmentReader& seg) const {
  // Written by the open, read-only after it: no lock (#110). Every caller runs
  // inside with_segment, which never reaches here for a failed segment.
  if (!seg.metadata)
    throw std::logic_error("segment metadata not loaded: " + source_->describe(seg.tmet_path));
  return *seg.metadata;
}

bool Session::skip_segment(const SegmentReader& seg, si8 lo, si8 hi) const {
  // ONLY the span of the index that is cached right now (learn_span): it is
  // dropped with that index, so it can never be older than what a walk would
  // read. No metadata time is used — a .tmet start or end can be stale or
  // wrong (#146 review rounds 1-2) and a skip on it is silent data loss.
  std::lock_guard<std::mutex> lock(index_mu_);
  if (!seg.span_known || seg.span_open || !seg.tidx) return false;
  return seg.span_hi <= lo || seg.span_lo >= hi;
}

void Session::learn_span(SegmentReader& seg, const std::shared_ptr<const std::vector<ui1>>& pin,
                         std::size_t n_entries, si8 rto, sf8 fs) {
  {
    std::lock_guard<std::mutex> lock(index_mu_);
    if (seg.span_known || seg.tidx != pin) return;
  }
  const std::span<const ui1> idx(*pin);
  // A SUPERSET of what any windowed walk can select: every entry is counted,
  // also the ones a walk drops for their byte range; one without a usable
  // count (0 = damaged, kept by collect_blocks; NO_ENTRY) makes the span
  // unbounded. Times and lengths by the walks' own expressions.
  bool open = !(fs > 0) || !std::isfinite(fs);
  si8 lo = std::numeric_limits<si8>::max(), hi = std::numeric_limits<si8>::min();
  for (std::size_t i = 0; i < n_entries && !open; ++i) {
    const auto e = fmt::TimeSeriesIndex::parse(
        idx.subspan(fmt::UNIVERSAL_HEADER_BYTES + i * fmt::TIME_SERIES_INDEX_BYTES,
                    fmt::TIME_SERIES_INDEX_BYTES));
    if (e.number_of_samples == 0 || e.number_of_samples == fmt::UI4_NO_ENTRY) {
      open = true;
      break;
    }
    const si8 b0 = to_user_time(e.start_time, rto);
    const si8 b1 = b0 + static_cast<si8>(
                            std::llround(static_cast<sf8>(e.number_of_samples) * 1e6 / fs));
    lo = std::min(lo, b0);
    hi = std::max(hi, b1);
  }
  std::lock_guard<std::mutex> lock(index_mu_);
  // Only for the index still cached: if another thread's trim evicted (and a
  // call re-read) it meanwhile, this span describes bytes nobody walks now.
  if (seg.span_known || seg.tidx != pin) return;
  seg.span_known = true;
  seg.span_open = open;
  seg.span_lo = lo;
  seg.span_hi = hi;
}

std::shared_ptr<const std::vector<ui1>> Session::segment_index(SegmentReader& seg) {
  {
    std::lock_guard<std::mutex> lock(index_mu_);
    if (seg.tidx) {
      seg.index_last_used = ++index_tick_;
      return seg.tidx;  // the copy is the caller's pin
    }
  }
  // Read OUTSIDE the lock: other threads keep using the cache meanwhile. Two
  // threads missing the same segment both read it; the first to finish wins.
  auto loaded = std::make_shared<const std::vector<ui1>>(source_->read_all(seg.tidx_path));
  std::lock_guard<std::mutex> lock(index_mu_);
  if (!seg.tidx) seg.tidx = std::move(loaded);
  seg.index_last_used = ++index_tick_;
  return seg.tidx;
}

void Session::load_channel_basic_info(const std::string& name, Channel& ch,
                                      const std::exception_ptr* errors) {
  ChannelInfo& info = ch.info;
  bool first = true;
  sf8 first_factor = 1.0;
  int readable = 0;
  for (std::size_t k = 0; k < ch.segments.size(); ++k) {
    auto& seg = ch.segments[k];
    // Nothing below can throw once the metadata is in hand, so containing the
    // whole body cannot leave `info` half-updated by a segment that failed.
    const bool ok = with_segment(name, seg, [&] {
    // The load ran in parallel; its failure surfaces HERE, in segment order,
    // so strict raises and lenient records exactly what a serial open did.
    if (errors[k]) std::rethrow_exception(errors[k]);
    auto& md = segment_metadata(seg);
    const si8 rto = segment_rto(md);
    si8 seg_start = to_user_time(md.universal_header.start_time, rto);
    si8 seg_end = to_user_time(md.universal_header.end_time, rto);
    if (first) {
      info.sampling_frequency = md.section2.sampling_frequency;
      info.units_conversion_factor = md.section2.units_conversion_factor;
      first_factor = md.section2.units_conversion_factor;
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
      // Segments may store different factors (#150): the channel then has
      // none of its own, as in meflib (meflib.c:5494, NO_ENTRY there).
      if (!same_units_conversion_factor(first_factor, md.section2.units_conversion_factor))
        info.units_conversion_factor.reset();
    }
    info.number_of_samples += md.section2.number_of_samples;
    if (md.universal_header.is_password_protected()) info.password_protected = true;
    if (md.legacy_password_mapping) info.legacy_password_mapping = true;
    });
    if (ok) ++readable;
  }
  // Count what can actually be read: segment_map lists only these, and a count
  // that disagreed with it would make the map look truncated.
  info.n_segments = readable;
}

std::string unusable_factor_message(const std::string& channel, sf8 factor,
                                    const std::string& alternative) {
  // Spelled like Python's "{:g}" for the values that can get here, so
  // MultiReader can raise the same text.
  const std::string value = std::isnan(factor)  ? "nan"
                            : std::isinf(factor) ? (factor < 0 ? "-inf" : "inf")
                                                 : "0";
  return "channel " + channel + ": samples in this window come from a segment that stores " +
         "units_conversion_factor " + value +
         (factor == 0.0 ? " (MEF's \"no entry\")" : "") +
         ", so they have no value in units; " + alternative +
         " returns the stored counts, and segments() lists each segment's factor";
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
    const si8 rto = segment_rto(md);

    const auto idx_pin = segment_index(seg);  // keeps the bytes alive for this call
    const std::span<const ui1> idx(*idx_pin);
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

    const sf8 factor = md.section2.units_conversion_factor;
    for (const auto& e : hits) {
      std::span<const ui1> block(tdat.data() + (static_cast<std::size_t>(e.file_offset) -
                                                range_begin),
                                 e.block_bytes);
      auto decoded = red::decode_block(block, keys);

      // Start a new run on discontinuity, a sample-index gap, or a segment
      // storing another conversion factor (#150: one factor per run).
      bool contiguous = current != nullptr && !decoded.discontinuity &&
                        e.start_sample == expected_next_sample &&
                        same_units_conversion_factor(current->units_conversion_factor, factor);
      if (!contiguous) {
        runs.push_back(DataRun{to_user_time(e.start_time, rto), e.start_sample, factor, {}});
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

std::optional<si8> Session::agreed_rto(const std::string* channel, std::string& why) {
  // The offset the DATA of this scope is read with, so the records land
  // where the data does: the one every readable segment's section 3 agrees
  // on. When they disagree there is no single right value; `why` says which
  // two differ and read_record_file decides whether that matters.
  std::optional<si8> agreed;
  std::string agreed_where;
  for (const auto& name : channel_names_) {
    if (channel && name != *channel) continue;
    for (auto& seg : channels_.at(name).segments) {
      if (segment_failed(seg)) continue;  // lenient: already reported, has no say
      const si8 rto = segment_rto(segment_metadata(seg));
      if (!agreed) {
        agreed = rto;
        agreed_where = source_->describe(seg.tmet_path);
      } else if (*agreed != rto) {
        why = "segments disagree on recording_time_offset (" + std::to_string(*agreed) + " in " +
              agreed_where + ", " + std::to_string(rto) + " in " +
              source_->describe(seg.tmet_path) + ")";
        return std::nullopt;
      }
    }
  }
  // A channel with no readable segment has no offset of its own: its records
  // were written with the SESSION's (SessionWriter::resolve_offset), so they
  // are read with it too.
  if (channel && !agreed) return agreed_rto(nullptr, why);
  return agreed.value_or(0);
}

void Session::read_record_file(std::vector<Record>& out, const std::string& rdat_path,
                               std::optional<si8> rto, const std::string& why,
                               RecordLevel level, const std::string& channel, int segment) {
  auto run = [&] {
    auto bytes = source_->read_all(rdat_path);
    auto uh = fmt::UniversalHeader::parse(bytes);
    crypto::AccessKeys keys = crypto::validate_password(
        password_, uh.level_1_password_validation_field, uh.level_2_password_validation_field);
    bool offset_dependent = false;
    auto recs = parse_records(bytes, rto.value_or(0), keys, &offset_dependent);
    // A disputed offset only matters to times stored negated (offset
    // applied); non-negative times are absolute whatever the offset is.
    // DELIBERATE DIVERGENCE: meflib, in its default RTO_REMOVE_ON_INPUT mode
    // (meflib.h:278), resolves record times with MEF_globals->
    // recording_time_offset -- whichever .tmet it happened to read last
    // (meflib.c:1806 -> 1846). The NO_ENTRY at meflib.c:5529 / 6208 is only
    // the METADATA aggregate. Following the read order would misplace the
    // records by the difference without a word, so this refuses instead.
    if (!rto && offset_dependent)
      throw FormatError(std::string(record_level_name(level)) +
                        "-level records cannot be placed: " + why +
                        " and the file stores offset-dependent (negated) times, so any "
                        "choice would misplace them by the difference: " +
                        source_->describe(rdat_path) +
                        (level == RecordLevel::Channel
                             ? ". records(\"" + channel + "\", level=\"segment\") still "
                               "returns this channel's segment-level records"
                             : std::string(level == RecordLevel::Session
                                               ? ". records(level=\"channel\") and "
                                                 "records(level=\"segment\") still read"
                                               : "")));
    // Only a file that was actually read: one that failed above is a problem,
    // not a successful read through the old password mapping.
    if (keys.legacy_password_mapping) {
      const auto where = source_->describe(rdat_path);
      std::lock_guard<std::mutex> lock(state_mu_);
      if (std::find(legacy_password_record_files_.begin(), legacy_password_record_files_.end(),
                    where) == legacy_password_record_files_.end())
        legacy_password_record_files_.push_back(where);
    }
    for (auto& rec : recs) {
      rec.level = level;
      rec.channel = channel;
      rec.segment = segment;
    }
    // Nothing is appended until the whole file parsed: a half-read file is
    // worse than a skipped one.
    out.insert(out.end(), std::make_move_iterator(recs.begin()),
               std::make_move_iterator(recs.end()));
  };
  if (strict_) {
    run();
    return;
  }
  // Lenient: one bad record file is CONTAINED, like a bad segment -- reported
  // in problems() (once per file), and the other files still read. It does
  // NOT mark the segment failed: an unreadable annotation file says nothing
  // about whether the segment's samples are intact.
  try {
    run();
  } catch (const std::exception& e) {
    std::lock_guard<std::mutex> lock(state_mu_);
    if (reported_record_files_.insert(rdat_path).second)
      problems_.push_back(SegmentProblem{channel, segment, source_->describe(rdat_path),
                                         std::string("records: ") + e.what(), "records"});
  }
}

std::vector<Record> Session::read_records(std::optional<std::string> channel,
                                          std::optional<int> segment, RecordLevels levels) {
  // Validate the query first, so a bad one fails the same whether or not any
  // record files happen to exist.
  if (segment && !channel)
    throw std::invalid_argument("a record segment needs its channel");
  const bool record_only =
      channel && std::find(record_only_channels_.begin(), record_only_channels_.end(),
                           *channel) != record_only_channels_.end();
  if (channel && !record_only && channels_.find(*channel) == channels_.end())
    throw std::out_of_range("no such channel: " + *channel);
  if (channel && levels == RecordLevels::Session)
    throw std::invalid_argument("session-level records do not belong to a channel");
  if (segment && levels == RecordLevels::Channel)
    throw std::invalid_argument("channel-level records do not belong to a segment");
  if (segment) {
    if (record_only)
      throw std::out_of_range("no such segment: " + *channel + " has records but no segments");
    const auto& segs = channels_.at(*channel).segments;
    if (std::none_of(segs.begin(), segs.end(),
                     [&](const SegmentReader& s) { return s.segment_number == *segment; }))
      throw std::out_of_range("no such segment: " + *channel + " segment " +
                              std::to_string(*segment));
  }

  // Which levels this call returns (see RecordLevels).
  bool want_session = false, want_channel = false, want_segment = false;
  switch (levels) {
    case RecordLevels::Default:
      want_session = !channel;
      want_channel = channel && !segment;
      want_segment = channel.has_value();
      break;
    case RecordLevels::Session: want_session = true; break;
    case RecordLevels::Channel: want_channel = true; break;
    case RecordLevels::Segment: want_segment = true; break;
    case RecordLevels::All:
      want_session = !channel;
      want_channel = !segment;
      want_segment = true;
      break;
  }

  std::vector<Record> out;

  // Session level: <session>.mefd/<session>.rdat (meflib.c:6343). Any .rdat at
  // the root is accepted, as before.
  if (want_session) {
    // The naming rule (session_layout.hpp) is shared with inspect_session.
    std::string reason;
    const std::string rdat_path = detail::session_record_file(*source_, reason);
    if (!reason.empty()) {
      if (strict_) throw FormatError(reason);
      std::lock_guard<std::mutex> lock(state_mu_);
      if (reported_record_files_.insert("<session-root>").second)
        problems_.push_back(SegmentProblem{"", -1, source_->describe(""),
                                           "records: " + reason, "records"});
    }
    if (!rdat_path.empty()) {
      std::string why;
      const auto rto = agreed_rto(nullptr, why);
      read_record_file(out, rdat_path, rto, why, RecordLevel::Session, "", -1);
    }
  }

  // Data channels plus channels that have records but no segments yet, by name.
  std::vector<std::string> record_channels = channel_names_;
  record_channels.insert(record_channels.end(), record_only_channels_.begin(),
                         record_only_channels_.end());
  std::sort(record_channels.begin(), record_channels.end());
  for (const auto& name : record_channels) {
    if (channel && name != *channel) continue;
    // Channel level: <ch>.timd/<ch>.rdat (meflib.c:5660).
    if (want_channel) {
      const std::string cand = name + ".timd/" + name + ".rdat";
      if (source_->exists(cand)) {
        std::string why;
        const auto rto = agreed_rto(&name, why);
        read_record_file(out, cand, rto, why, RecordLevel::Channel, name, -1);
      }
    }
    // Segment level: <seg>.segd/<seg>.rdat, next to the segment's .tmet
    // (meflib.c:6030). Its offset is the segment's own, so it is never
    // disputed, and a dispute at the channel level above never costs these.
    if (want_segment && channels_.count(name)) {
      for (auto& seg : channels_.at(name).segments) {
        if (segment && seg.segment_number != *segment) continue;
        if (segment_failed(seg)) continue;  // lenient: reported in problems()
        const std::string cand =
            seg.tmet_path.substr(0, seg.tmet_path.size() - 5) + ".rdat";  // ".tmet"
        if (source_->exists(cand))
          read_record_file(out, cand, segment_rto(segment_metadata(seg)), "",
                           RecordLevel::Segment, name, seg.segment_number);
      }
    }
  }
  return out;
}

BlockJobs Session::collect_blocks(const std::string& channel, std::optional<si8> t0_opt,
                                  std::optional<si8> t1_opt, const Control& ctl) {
  auto it = channels_.find(channel);
  if (it == channels_.end()) throw std::out_of_range("no such channel: " + channel);
  Channel& ch = it->second;

  BlockJobs out;
  out.sampling_frequency = ch.info.sampling_frequency;
  const si8 t0 = t0_opt.value_or(ch.info.start_time);
  const si8 t1 = t1_opt.value_or(ch.info.end_time + 1);
  const sf8 fs = ch.info.sampling_frequency;

  for (auto& seg : ch.segments) {
    // Between segments, OUTSIDE with_segment: a cancellation is not a damaged
    // segment, and (Cancelled being no std::exception) a lenient session could
    // not contain it anyway. Nothing has been appended for this segment yet, so
    // there is nothing to roll back; the caller discards `out` whole. No span
    // into the index cache is held here, so trimming on the way out is safe.
    if (ctl.cancel && ctl.cancel->cancelled()) {
      trim_index_cache();
      throw Cancelled();
    }
    const std::size_t mark_buf = out.buffers.size(), mark_job = out.jobs.size(),
                      mark_mm = out.mismatches.size();
    if (!with_segment(channel, seg, [&] {
    auto& md = segment_metadata(seg);
    // #110 item 3: a segment whose CACHED index holds nothing in the window is
    // not walked again (no I/O either way: the index is in memory).
    if (skip_segment(seg, t0, t1)) return;
    const si8 rto = segment_rto(md);
    crypto::AccessKeys keys = crypto::validate_password(
        password_, md.universal_header.level_1_password_validation_field,
        md.universal_header.level_2_password_validation_field);

    const auto idx_pin = segment_index(seg);  // keeps the bytes alive for this call
    const std::span<const ui1> idx(*idx_pin);
    const std::size_t n_entries = index_entry_count(idx, source_->describe(seg.tidx_path));
    learn_span(seg, idx_pin, n_entries, rto, fs);

    // Determine whether any block in this segment is in range before loading
    // the (potentially large) .tdat.
    std::vector<fmt::TimeSeriesIndex> hits;
    for (std::size_t i = 0; i < n_entries; ++i) {
      auto e = fmt::TimeSeriesIndex::parse(
          idx.subspan(fmt::UNIVERSAL_HEADER_BYTES + i * fmt::TIME_SERIES_INDEX_BYTES,
                      fmt::TIME_SERIES_INDEX_BYTES));
      // Selection is the ONE thing the index must be trusted for: finding a
      // block without it means scanning the whole .tdat, and one channel of a
      // long recording is tens of gigabytes. So reject an entry only on its
      // BYTE RANGE, which is the part nothing else can supply. A real block
      // starts at or after the universal header and is at least a RED header
      // long; anything else is index padding, which foreign writers do leave
      // behind (NO_ENTRY offsets and zero-filled tails alike).
      if (e.file_offset < static_cast<si8>(fmt::UNIVERSAL_HEADER_BYTES) ||
          e.block_bytes < static_cast<ui4>(fmt::RED_BLOCK_HEADER_BYTES))
        continue;
      si8 bstart = to_user_time(e.start_time, rto);
      if (e.number_of_samples == 0) {
        // A usable byte range with a zero sample count is a DAMAGED entry, not
        // an empty block. Dropping it here would discard real samples on the
        // index's word alone, before the header — the copy that is actually
        // CRC-protected — could contradict it. Keep it and let the header
        // decide; the disagreement is reported like any other.
        hits.push_back(e);
        continue;
      }
      si8 bend = bstart + static_cast<si8>(std::llround(e.number_of_samples * 1e6 / fs));
      if (bend <= t0 || bstart >= t1) continue;
      hits.push_back(e);
    }
    // KNOWN LIMIT: a WINDOWED read still tests the window against the index's
    // time and count, because a block has to be fetched before its header can
    // be read and fetching every block to check is the whole-file read this
    // design exists to avoid. So on a file whose index times are wrong, a
    // windowed read can still miss a block the index misplaced. It is reported
    // whenever such a block IS fetched, which is what tells a caller the index
    // is untrustworthy; a full pass (`Validator`, or a read with no window) is
    // the way to see all of it.
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
      job.units_conversion_factor = md.section2.units_conversion_factor;
      job.segment_number = seg.segment_number;
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

namespace {
std::size_t cached_bytes(const SegmentReader& seg) { return seg.tidx ? seg.tidx->capacity() : 0; }
}  // namespace

std::size_t Session::index_cache_bytes() const {
  std::lock_guard<std::mutex> lock(index_mu_);
  std::size_t total = 0;
  for (const auto& name : channel_names_)
    for (const auto& seg : channels_.at(name).segments) total += cached_bytes(seg);
  return total;
}

void Session::trim_index_cache() {
  const std::size_t budget = index_budget_.load(std::memory_order_relaxed);
  if (budget == 0) return;
  std::lock_guard<std::mutex> lock(index_mu_);
  std::size_t total = 0;
  for (const auto& name : channel_names_)
    for (const auto& seg : channels_.at(name).segments) total += cached_bytes(seg);
  if (total <= budget) return;

  // Oldest first. Called at the end of a public operation, so THIS call holds
  // no index. Another thread's call may: it holds its own pin (segment_index
  // returns a shared_ptr), so dropping the cache's reference here frees the
  // bytes only once that call is done with them. Without the pin, evicting an
  // index in use was a use-after-free ("double free or corruption" from four
  // threads reading one Session with a tiny budget).
  struct Victim {
    std::uint64_t used;
    SegmentReader* seg;
  };
  std::vector<Victim> victims;
  for (const auto& name : channel_names_)
    for (auto& seg : channels_.at(name).segments)
      if (seg.tidx) victims.push_back({seg.index_last_used, &seg});
  std::sort(victims.begin(), victims.end(),
            [](const Victim& a, const Victim& b) { return a.used < b.used; });

  for (const auto& v : victims) {
    if (total <= budget) break;
    total -= cached_bytes(*v.seg);
    v.seg->tidx.reset();  // the memory goes with the last pin
    v.seg->index_last_used = 0;
    // The span describes THAT index: once it is gone, the next walk re-reads
    // the file (which may have grown since) and learns the span again.
    v.seg->span_known = false;
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
    const si8 rto = segment_rto(md);
    SegmentInfo si;
    si.segment_number = seg.segment_number;
    si.path = source_->describe(seg.tmet_path.substr(0, seg.tmet_path.rfind('/')));
    si.start_time = to_user_time(md.universal_header.start_time, rto);
    si.end_time = to_user_time(md.universal_header.end_time, rto);
    si.start_sample = md.section2.start_sample;
    si.number_of_samples = md.section2.number_of_samples;
    si.number_of_blocks = md.section2.number_of_blocks;
    si.units_conversion_factor = md.section2.units_conversion_factor;
    si.provenance = fmt::provenance::read(md.universal_header);
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
    const si8 rto = segment_rto(md);
    const auto idx_pin = segment_index(seg);  // keeps the bytes alive for this call
    const std::span<const ui1> idx(*idx_pin);
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

BlockEnvelope Session::block_envelope(const std::string& channel, std::optional<si8> t0,
                                     std::optional<si8> t1) {
  auto it = channels_.find(channel);
  if (it == channels_.end()) throw std::out_of_range("no such channel: " + channel);
  Channel& ch = it->second;
  const sf8 fs = ch.info.sampling_frequency;
  const sf8 nan = std::numeric_limits<sf8>::quiet_NaN();
  if (!(fs > 0) || !std::isfinite(fs))
    throw FormatError("channel " + channel + " has no valid sampling frequency");
  const si8 lo = t0.value_or(std::numeric_limits<si8>::min());
  const si8 hi = t1.value_or(std::numeric_limits<si8>::max());

  BlockEnvelope out;
  // A factor that cannot scale (0 = NO_ENTRY, NaN, inf) is refused like
  // read() refuses it -- but OUTSIDE with_segment: it is not a damaged
  // segment, and containing it would mark the segment unreadable for reads
  // of its counts too.
  std::optional<sf8> unusable;
  for (auto& seg : ch.segments) {
    const std::size_t mark = out.start_uutc.size();
    if (!with_segment(channel, seg, [&] {
          auto& md = segment_metadata(seg);
          const si8 rto = segment_rto(md);
          // Each block is scaled by ITS segment's factor (#150).
          const sf8 uf = md.section2.units_conversion_factor;
          // A segment whose cached index holds nothing in the window is not
          // walked again (a day-long session has thousands of them).
          if (skip_segment(seg, lo, hi)) return;
          const auto idx_pin = segment_index(seg);  // keeps the bytes alive for this call
          const std::span<const ui1> idx(*idx_pin);
          const std::size_t n = index_entry_count(idx, source_->describe(seg.tidx_path));
          learn_span(seg, idx_pin, n, rto, fs);
          for (std::size_t i = 0; i < n; ++i) {
            const auto e = fmt::TimeSeriesIndex::parse(
                idx.subspan(fmt::UNIVERSAL_HEADER_BYTES + i * fmt::TIME_SERIES_INDEX_BYTES,
                            fmt::TIME_SERIES_INDEX_BYTES));
            if (e.file_offset < 0 || e.number_of_samples == 0 || e.number_of_samples == fmt::UI4_NO_ENTRY)
              continue;
            const si8 b0 = to_user_time(e.start_time, rto);
            const si8 b1 = b0 + static_cast<si8>(std::llround(
                                    static_cast<sf8>(e.number_of_samples) * 1e6 / fs));
            if (b1 <= lo || b0 >= hi) continue;
            if (!usable_units_conversion_factor(uf)) {
              unusable = uf;
              return;
            }
            out.start_uutc.push_back(b0);
            out.end_uutc.push_back(b1);
            const sf8 smin = e.minimum_sample_value == fmt::RED_NAN
                                 ? nan
                                 : static_cast<sf8>(e.minimum_sample_value) * uf;
            const sf8 smax = e.maximum_sample_value == fmt::RED_NAN
                                 ? nan
                                 : static_cast<sf8>(e.maximum_sample_value) * uf;
            // A negative factor (a polarity inversion) turns the largest
            // count into the smallest value (#83; pymef3_file.c:972-978).
            out.minimum.push_back(uf < 0.0 ? smax : smin);
            out.maximum.push_back(uf < 0.0 ? smin : smax);
          }
        })) {
      out.start_uutc.resize(mark);
      out.end_uutc.resize(mark);
      out.minimum.resize(mark);
      out.maximum.resize(mark);
    }
    if (unusable) break;
  }
  trim_index_cache();  // safe point: nothing holds a span into a cached index
  if (unusable) throw FormatError(unusable_factor_message(channel, *unusable, "toc()"));
  return out;
}

}  // namespace mef3io
