// mef3io — flat C ABI implementation: wraps the C++ API, converts exceptions
// to error codes with a thread-local message.
#include "mef3io/c_api.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "mef3io/errors.hpp"
#include "mef3io/reader.hpp"
#include "mef3io/records.hpp"
#include "mef3io/session_writer.hpp"
#include "mef3io/tar.hpp"
#include "mef3io/version.hpp"

namespace {

thread_local std::string g_last_error;

void copy_str(char* dst, size_t dst_bytes, const std::string& src) {
  if (dst_bytes == 0) return;
  const size_t n = std::min(src.size(), dst_bytes - 1);
  std::memcpy(dst, src.data(), n);
  dst[n] = '\0';
}

// Run `fn`, translating exceptions into error codes + g_last_error.
template <typename Fn>
int guarded(Fn&& fn) {
  try {
    fn();
    return MEF3IO_OK;
  } catch (const mef3io::PasswordError& e) {
    g_last_error = e.what();
    return MEF3IO_ERR_PASSWORD;
  } catch (const mef3io::CrcError& e) {
    g_last_error = e.what();
    return MEF3IO_ERR_CRC;
  } catch (const mef3io::WriteConflictError& e) {
    g_last_error = e.what();
    return MEF3IO_ERR_CONFLICT;
  } catch (const mef3io::FormatError& e) {
    g_last_error = e.what();
    return MEF3IO_ERR_FORMAT;
  } catch (const mef3io::IoError& e) {
    g_last_error = e.what();
    return MEF3IO_ERR_IO;
  } catch (const std::out_of_range& e) {
    g_last_error = e.what();
    return MEF3IO_ERR_ARGUMENT;
  } catch (const std::exception& e) {
    g_last_error = e.what();
    return MEF3IO_ERR_OTHER;
  } catch (...) {
    g_last_error = "unknown error";
    return MEF3IO_ERR_OTHER;
  }
}

int fail_argument(const char* msg) {
  g_last_error = msg;
  return MEF3IO_ERR_ARGUMENT;
}

std::optional<mef3io::si8> opt_time(int64_t t) {
  if (t == MEF3IO_TIME_UNSET) return std::nullopt;
  return t;
}

}  // namespace

struct mef3io_reader {
  mef3io::Reader impl;
};
struct mef3io_writer {
  mef3io::SessionWriter impl;
};
struct mef3io_records {
  std::vector<mef3io::Record> records;
};

extern "C" {

const char* mef3io_version(void) { return mef3io::version(); }

const char* mef3io_last_error(void) { return g_last_error.c_str(); }

/* ----- reader ------------------------------------------------------------ */

int mef3io_reader_open(const char* mefd_path, const char* password, int n_threads,
                       mef3io_reader** out) {
  if (!mefd_path || !out) return fail_argument("mefd_path and out must not be NULL");
  *out = nullptr;
  return guarded([&] {
    *out = new mef3io_reader{
        mef3io::Reader(mefd_path, password ? std::string(password) : "", n_threads)};
  });
}

void mef3io_reader_close(mef3io_reader* r) { delete r; }

int mef3io_reader_n_channels(mef3io_reader* r, int32_t* out) {
  if (!r || !out) return fail_argument("NULL argument");
  return guarded([&] { *out = static_cast<int32_t>(r->impl.channels().size()); });
}

int mef3io_reader_channel_name(mef3io_reader* r, int32_t index, char* buf, size_t buf_bytes) {
  if (!r || !buf) return fail_argument("NULL argument");
  return guarded([&] {
    const auto& chs = r->impl.channels();
    if (index < 0 || static_cast<size_t>(index) >= chs.size())
      throw std::out_of_range("channel index out of range");
    copy_str(buf, buf_bytes, chs[static_cast<size_t>(index)]);
  });
}

int mef3io_reader_info(mef3io_reader* r, const char* channel, mef3io_channel_info* out) {
  if (!r || !channel || !out) return fail_argument("NULL argument");
  return guarded([&] {
    const mef3io::ChannelInfo& ci = r->impl.info(channel);
    out->sampling_frequency = ci.sampling_frequency;
    out->units_conversion_factor = ci.units_conversion_factor;
    out->start_time = ci.start_time;
    out->end_time = ci.end_time;
    out->number_of_samples = ci.number_of_samples;
    out->recording_time_offset = ci.recording_time_offset;
    out->n_segments = ci.n_segments;
    out->section3_available = ci.section3_available ? 1 : 0;
    copy_str(out->units_description, sizeof out->units_description, ci.units_description);
    out->acquisition_channel_number = ci.acquisition_channel_number;
    out->low_frequency_filter = ci.low_frequency_filter;
    out->high_frequency_filter = ci.high_frequency_filter;
    out->notch_filter = ci.notch_filter;
    out->line_frequency = ci.line_frequency;
    out->gmt_offset = ci.gmt_offset;
    copy_str(out->session_description, sizeof out->session_description, ci.session_description);
    copy_str(out->channel_description, sizeof out->channel_description, ci.channel_description);
    copy_str(out->reference_description, sizeof out->reference_description, ci.reference_description);
    copy_str(out->subject_name_1, sizeof out->subject_name_1, ci.subject_name_1);
    copy_str(out->subject_name_2, sizeof out->subject_name_2, ci.subject_name_2);
    copy_str(out->subject_id, sizeof out->subject_id, ci.subject_id);
    copy_str(out->recording_location, sizeof out->recording_location, ci.recording_location);
  });
}

int mef3io_reader_read_size(mef3io_reader* r, const char* channel, int64_t t0, int64_t t1,
                            int64_t* out_n) {
  if (!r || !channel || !out_n) return fail_argument("NULL argument");
  return guarded([&] {
    const mef3io::ChannelInfo& ci = r->impl.info(channel);
    const int64_t a = (t0 == MEF3IO_TIME_UNSET) ? ci.start_time : t0;
    const int64_t b = (t1 == MEF3IO_TIME_UNSET) ? ci.end_time : t1;
    // Mirrors Reader::read_raw's grid sizing exactly.
    int64_t n = static_cast<int64_t>(
        std::llround(static_cast<double>(b - a) * ci.sampling_frequency / 1e6));
    *out_n = n < 0 ? 0 : n;
  });
}

int mef3io_reader_read(mef3io_reader* r, const char* channel, int64_t t0, int64_t t1,
                       double* buf, int64_t buf_len, int64_t* out_n) {
  // A NULL buffer is fine for a zero-length window (empty reads are legal).
  if (!r || !channel || (!buf && buf_len > 0) || !out_n)
    return fail_argument("NULL argument");
  return guarded([&] {
    std::vector<mef3io::sf8> v = r->impl.read(channel, opt_time(t0), opt_time(t1));
    if (static_cast<int64_t>(v.size()) > buf_len)
      throw std::out_of_range("buffer too small: need " + std::to_string(v.size()));
    std::copy(v.begin(), v.end(), buf);
    *out_n = static_cast<int64_t>(v.size());
  });
}

int mef3io_reader_read_raw(mef3io_reader* r, const char* channel, int64_t t0, int64_t t1,
                           int32_t* samples, uint8_t* valid, int64_t buf_len, int64_t* out_n,
                           int64_t* out_start_uutc) {
  if (!r || !channel || ((!samples || !valid) && buf_len > 0) || !out_n)
    return fail_argument("NULL argument");
  return guarded([&] {
    mef3io::RawData d = r->impl.read_raw(channel, opt_time(t0), opt_time(t1));
    if (static_cast<int64_t>(d.samples.size()) > buf_len)
      throw std::out_of_range("buffer too small: need " + std::to_string(d.samples.size()));
    std::copy(d.samples.begin(), d.samples.end(), samples);
    std::copy(d.valid.begin(), d.valid.end(), valid);
    *out_n = static_cast<int64_t>(d.samples.size());
    if (out_start_uutc) *out_start_uutc = d.start_uutc;
  });
}

int mef3io_reader_n_segments(mef3io_reader* r, const char* channel, int32_t* out) {
  if (!r || !channel || !out) return fail_argument("NULL argument");
  return guarded([&] { *out = static_cast<int32_t>(r->impl.segments(channel).size()); });
}

int mef3io_reader_segment(mef3io_reader* r, const char* channel, int32_t index,
                          mef3io_segment_info* out) {
  if (!r || !channel || !out) return fail_argument("NULL argument");
  return guarded([&] {
    auto segs = r->impl.segments(channel);
    if (index < 0 || static_cast<size_t>(index) >= segs.size())
      throw std::out_of_range("segment index out of range");
    const mef3io::SegmentInfo& s = segs[static_cast<size_t>(index)];
    out->segment_number = s.segment_number;
    out->start_time = s.start_time;
    out->end_time = s.end_time;
    out->start_sample = s.start_sample;
    out->number_of_samples = s.number_of_samples;
    out->number_of_blocks = s.number_of_blocks;
    copy_str(out->path, sizeof out->path, s.path);
  });
}

int mef3io_reader_n_blocks(mef3io_reader* r, const char* channel, int64_t* out) {
  if (!r || !channel || !out) return fail_argument("NULL argument");
  return guarded([&] { *out = static_cast<int64_t>(r->impl.toc(channel).size()); });
}

int mef3io_reader_block(mef3io_reader* r, const char* channel, int64_t index,
                        mef3io_block_info* out) {
  if (!r || !channel || !out) return fail_argument("NULL argument");
  return guarded([&] {
    auto toc = r->impl.toc(channel);
    if (index < 0 || static_cast<size_t>(index) >= toc.size())
      throw std::out_of_range("block index out of range");
    const mef3io::BlockIndexEntry& b = toc[static_cast<size_t>(index)];
    out->start_uutc = b.start_uutc;
    out->start_sample = b.start_sample;
    out->number_of_samples = b.number_of_samples;
    out->maximum_sample_value = b.maximum_sample_value;
    out->minimum_sample_value = b.minimum_sample_value;
    out->discontinuity = b.discontinuity ? 1 : 0;
  });
}

int mef3io_reader_n_records(mef3io_reader* r, const char* channel, int32_t* out) {
  if (!r || !out) return fail_argument("NULL argument");
  return guarded([&] {
    std::optional<std::string> ch;
    if (channel) ch = channel;
    *out = static_cast<int32_t>(r->impl.records(ch).size());
  });
}

int mef3io_reader_record(mef3io_reader* r, const char* channel, int32_t index,
                         mef3io_record_info* out) {
  if (!r || !out) return fail_argument("NULL argument");
  return guarded([&] {
    std::optional<std::string> ch;
    if (channel) ch = channel;
    auto recs = r->impl.records(ch);
    if (index < 0 || static_cast<size_t>(index) >= recs.size())
      throw std::out_of_range("record index out of range");
    const mef3io::Record& rec = recs[static_cast<size_t>(index)];
    copy_str(out->type, sizeof out->type, rec.type);
    out->time = rec.time;
    out->duration = rec.duration.value_or(-1);
    copy_str(out->text, sizeof out->text, rec.text.value_or(""));
  });
}

/* ----- archive ----------------------------------------------------------- */

int mef3io_archive_session(const char* session_dir, const char* tar_path, int overwrite,
                           char* out_path, size_t out_path_bytes) {
  if (!session_dir) return fail_argument("session_dir must not be NULL");
  return guarded([&] {
    const std::string result =
        mef3io::archive_session(session_dir, tar_path ? tar_path : "", overwrite != 0);
    if (out_path && out_path_bytes) copy_str(out_path, out_path_bytes, result);
  });
}

int mef3io_extract_session(const char* tar_path, const char* dest_dir, int overwrite,
                           char* out_path, size_t out_path_bytes) {
  if (!tar_path) return fail_argument("tar_path must not be NULL");
  return guarded([&] {
    const std::string result =
        mef3io::extract_session(tar_path, dest_dir ? dest_dir : "", overwrite != 0);
    if (out_path && out_path_bytes) copy_str(out_path, out_path_bytes, result);
  });
}

/* ----- writer ------------------------------------------------------------ */

int mef3io_writer_open(const char* mefd_path, int overwrite, const char* password_1,
                       const char* password_2, mef3io_writer** out) {
  if (!mefd_path || !out) return fail_argument("mefd_path and out must not be NULL");
  *out = nullptr;
  return guarded([&] {
    *out = new mef3io_writer{mef3io::SessionWriter(mefd_path, overwrite != 0,
                                                   password_1 ? password_1 : "",
                                                   password_2 ? password_2 : "")};
  });
}

void mef3io_writer_close(mef3io_writer* w) { delete w; }

int mef3io_writer_set_units(mef3io_writer* w, const char* units) {
  if (!w || !units) return fail_argument("NULL argument");
  return guarded([&] { w->impl.set_units(units); });
}

int mef3io_writer_set_metadata(mef3io_writer* w, const mef3io_metadata* md) {
  if (!w || !md) return fail_argument("NULL argument");
  return guarded([&] {
    mef3io::SessionMetadata m;
    m.session_description = md->session_description;
    m.channel_description = md->channel_description;
    m.reference_description = md->reference_description;
    m.acquisition_channel_number = md->acquisition_channel_number;
    m.low_frequency_filter = md->low_frequency_filter;
    m.high_frequency_filter = md->high_frequency_filter;
    m.notch_filter = md->notch_filter;
    m.line_frequency = md->line_frequency;
    m.subject_name_1 = md->subject_name_1;
    m.subject_name_2 = md->subject_name_2;
    m.subject_id = md->subject_id;
    m.recording_location = md->recording_location;
    m.gmt_offset = md->gmt_offset;
    w->impl.set_metadata(m);
  });
}

int mef3io_writer_set_block_length(mef3io_writer* w, int64_t samples_per_block) {
  if (!w) return fail_argument("NULL argument");
  return guarded([&] { w->impl.set_block_length(samples_per_block); });
}

int mef3io_writer_set_threads(mef3io_writer* w, int n_threads) {
  if (!w) return fail_argument("NULL argument");
  return guarded([&] { w->impl.set_threads(n_threads); });
}

static void copy_summary(const mef3io::WriteSummary& s, mef3io_write_summary* out) {
  if (!out) return;
  out->samples_written = s.samples_written;
  out->blocks = s.blocks;
  out->gaps_skipped = s.gaps_skipped;
  out->segment = s.segment;
}

int mef3io_writer_write_float(mef3io_writer* w, const char* channel, const double* data,
                              int64_t n, int64_t start_uutc, double fs, int precision,
                              int new_segment, mef3io_write_summary* out) {
  if (!w || !channel || (!data && n > 0) || n < 0) return fail_argument("bad argument");
  return guarded([&] {
    auto s = w->impl.write_float(channel, std::span<const mef3io::sf8>(data, static_cast<size_t>(n)),
                                 start_uutc, fs, precision, new_segment != 0);
    copy_summary(s, out);
  });
}

int mef3io_writer_write_int32(mef3io_writer* w, const char* channel, const int32_t* data,
                              const uint8_t* valid, int64_t n, double ufact, int64_t start_uutc,
                              double fs, int new_segment, mef3io_write_summary* out) {
  if (!w || !channel || (!data && n > 0) || n < 0) return fail_argument("bad argument");
  return guarded([&] {
    std::span<const mef3io::ui1> vspan;
    if (valid) vspan = std::span<const mef3io::ui1>(valid, static_cast<size_t>(n));
    auto s = w->impl.write_int32(channel,
                                 std::span<const mef3io::si4>(data, static_cast<size_t>(n)),
                                 ufact, start_uutc, fs, vspan, new_segment != 0);
    copy_summary(s, out);
  });
}

mef3io_records* mef3io_records_create(void) { return new mef3io_records{}; }

void mef3io_records_free(mef3io_records* list) { delete list; }

int mef3io_records_add(mef3io_records* list, const char* type, int64_t time, const char* text,
                       int64_t duration) {
  if (!list || !type) return fail_argument("NULL argument");
  return guarded([&] {
    mef3io::Record rec;
    rec.type = type;
    rec.time = time;
    if (text && *text) rec.text = text;
    if (duration >= 0) rec.duration = duration;
    list->records.push_back(std::move(rec));
  });
}

int mef3io_writer_write_records(mef3io_writer* w, const char* channel,
                                const mef3io_records* list) {
  if (!w || !list) return fail_argument("NULL argument");
  return guarded([&] {
    std::optional<std::string> ch;
    if (channel) ch = channel;
    w->impl.write_records(ch, list->records);
  });
}

} /* extern "C" */
