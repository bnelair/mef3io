// mef3io — flat C ABI implementation: wraps the C++ API, converts exceptions
// to error codes with a thread-local message.
#include "mef3io/c_api.h"

#include "mef3io/recover.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <variant>
#include <string>
#include <vector>

#include "mef3io/control.hpp"
#include "mef3io/crypto.hpp"
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
  } catch (const mef3io::Cancelled& e) {  // not a std::exception: catch it by name
    g_last_error = e.what();
    return MEF3IO_ERR_CANCELLED;
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
  } catch (const std::invalid_argument& e) {
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
// A snapshot of read records plus each one's body fields, so the pointers
// handed out in mef3io_record_field stay valid until the list is freed.
struct mef3io_record_list {
  std::vector<mef3io::Record> records;
  std::vector<std::vector<mef3io::RecordField>> fields;
};
struct mef3io_cancel_token {
  mef3io::CancelToken impl;
};

namespace {
// The C hooks as a core Control. The lambda captures plain values only.
mef3io::Control make_control(const mef3io_cancel_token* cancel, mef3io_progress_fn progress,
                             void* user_data) {
  mef3io::Control ctl;
  ctl.cancel = cancel ? &cancel->impl : nullptr;
  if (progress)
    ctl.progress = [progress, user_data](std::uint64_t done, std::uint64_t total) {
      progress(done, total, user_data);
    };
  return ctl;
}
}  // namespace

namespace {
const mef3io::RecordField* list_field(const mef3io_record_list* list, int32_t index,
                                      int32_t field) {
  if (index < 0 || static_cast<size_t>(index) >= list->records.size()) return nullptr;
  const auto& fs = list->fields[static_cast<size_t>(index)];
  if (field < 0 || static_cast<size_t>(field) >= fs.size()) return nullptr;
  return &fs[static_cast<size_t>(field)];
}
}  // namespace

extern "C" {

const char* mef3io_version(void) { return mef3io::version(); }

mef3io_cancel_token* mef3io_cancel_token_new(void) {
  return new (std::nothrow) mef3io_cancel_token{};
}
void mef3io_cancel_token_free(mef3io_cancel_token* token) { delete token; }
void mef3io_cancel_token_cancel(mef3io_cancel_token* token) {
  if (token) token->impl.cancel();
}
int mef3io_cancel_token_is_cancelled(const mef3io_cancel_token* token) {
  return token && token->impl.cancelled() ? 1 : 0;
}

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

int mef3io_reader_open_ex(const char* mefd_path, const char* password, int n_threads,
                          int strict, mef3io_reader** out) {
  if (!mefd_path || !out) return fail_argument("mefd_path and out must not be NULL");
  *out = nullptr;
  return guarded([&] {
    *out = new mef3io_reader{mef3io::Reader(mefd_path, password ? std::string(password) : "",
                                            n_threads, strict != 0)};
  });
}

void mef3io_reader_close(mef3io_reader* r) { delete r; }

int mef3io_reader_n_problems(mef3io_reader* r, int32_t* out) {
  if (!r || !out) return fail_argument("NULL argument");
  return guarded([&] { *out = static_cast<int32_t>(r->impl.problems().size()); });
}

int mef3io_reader_n_legacy_password_channels(mef3io_reader* r, int32_t* out) {
  if (!r || !out) return fail_argument("NULL argument");
  return guarded([&] {
    int32_t n = 0;
    for (const auto& ch : r->impl.channels())
      if (r->impl.info(ch).legacy_password_mapping) ++n;
    *out = n;
  });
}

int mef3io_reader_n_legacy_password_record_files(mef3io_reader* r, int32_t* out) {
  if (!r || !out) return fail_argument("NULL argument");
  return guarded(
      [&] { *out = static_cast<int32_t>(r->impl.legacy_password_record_files().size()); });
}

int mef3io_password_interop_issue(const char* password, char* buf, size_t buf_bytes) {
  if (!password || !buf) return fail_argument("NULL argument");
  return guarded([&] { copy_str(buf, buf_bytes, mef3io::crypto::password_interop_issue(password)); });
}

int mef3io_reader_problem(mef3io_reader* r, int32_t index, mef3io_segment_problem* out) {
  if (!r || !out) return fail_argument("NULL argument");
  return guarded([&] {
    const auto& ps = r->impl.problems();
    if (index < 0 || static_cast<size_t>(index) >= ps.size())
      throw std::out_of_range("problem index out of range");
    const auto& p = ps[static_cast<size_t>(index)];
    copy_str(out->channel, sizeof out->channel, p.channel);
    out->segment_number = static_cast<int32_t>(p.segment_number);
    copy_str(out->segment, sizeof out->segment, p.segment);
    copy_str(out->reason, sizeof out->reason, p.reason);
  });
}

int mef3io_reader_problem_kind(mef3io_reader* r, int32_t index, int32_t* out) {
  if (!r || !out) return fail_argument("NULL argument");
  return guarded([&] {
    const auto& ps = r->impl.problems();
    if (index < 0 || static_cast<size_t>(index) >= ps.size())
      throw std::out_of_range("problem index out of range");
    *out = ps[static_cast<size_t>(index)].kind == "records" ? MEF3IO_PROBLEM_RECORDS
                                                            : MEF3IO_PROBLEM_SEGMENT;
  });
}

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
  return mef3io_reader_read_ex(r, channel, t0, t1, buf, buf_len, out_n, nullptr, nullptr,
                               nullptr);
}

int mef3io_reader_read_ex(mef3io_reader* r, const char* channel, int64_t t0, int64_t t1,
                          double* buf, int64_t buf_len, int64_t* out_n,
                          const mef3io_cancel_token* cancel, mef3io_progress_fn progress,
                          void* user_data) {
  // A NULL buffer is fine for a zero-length window (empty reads are legal).
  if (!r || !channel || (!buf && buf_len > 0) || !out_n)
    return fail_argument("NULL argument");
  return guarded([&] {
    const mef3io::Control ctl = make_control(cancel, progress, user_data);
    std::vector<mef3io::sf8> v = r->impl.read(channel, opt_time(t0), opt_time(t1), ctl);
    if (static_cast<int64_t>(v.size()) > buf_len)
      throw std::out_of_range("buffer too small: need " + std::to_string(v.size()));
    std::copy(v.begin(), v.end(), buf);
    *out_n = static_cast<int64_t>(v.size());
  });
}

int mef3io_reader_read_raw(mef3io_reader* r, const char* channel, int64_t t0, int64_t t1,
                           int32_t* samples, uint8_t* valid, int64_t buf_len, int64_t* out_n,
                           int64_t* out_start_uutc) {
  return mef3io_reader_read_raw_ex(r, channel, t0, t1, samples, valid, buf_len, out_n,
                                   out_start_uutc, nullptr, nullptr, nullptr);
}

int mef3io_reader_read_raw_ex(mef3io_reader* r, const char* channel, int64_t t0, int64_t t1,
                              int32_t* samples, uint8_t* valid, int64_t buf_len, int64_t* out_n,
                              int64_t* out_start_uutc, const mef3io_cancel_token* cancel,
                              mef3io_progress_fn progress, void* user_data) {
  if (!r || !channel || ((!samples || !valid) && buf_len > 0) || !out_n)
    return fail_argument("NULL argument");
  return guarded([&] {
    const mef3io::Control ctl = make_control(cancel, progress, user_data);
    mef3io::RawData d = r->impl.read_raw(channel, opt_time(t0), opt_time(t1), ctl);
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

int mef3io_reader_segment_provenance(mef3io_reader* r, const char* channel, int32_t index,
                                     mef3io_provenance* out) {
  if (!r || !channel || !out) return fail_argument("NULL argument");
  return guarded([&] {
    auto segs = r->impl.segments(channel);
    if (index < 0 || static_cast<size_t>(index) >= segs.size())
      throw std::out_of_range("segment index out of range");
    const auto& p = segs[static_cast<size_t>(index)].provenance;
    *out = mef3io_provenance{};
    out->present = p.present ? 1 : 0;
    out->layout_version = p.layout_version;
    out->last_operation = p.last_operation;
    out->operations_mask = p.operations_mask;
    out->modification_count = p.modification_count;
    copy_str(out->created_by, sizeof out->created_by, p.created_by);
    copy_str(out->last_modified_by, sizeof out->last_modified_by, p.last_modified_by);
  });
}

const char* mef3io_operation_name(int32_t code) {
  if (code < 0 || code > 255) return nullptr;
  return mef3io::fmt::provenance::operation_name(static_cast<mef3io::ui1>(code));
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

int mef3io_reader_read_records(mef3io_reader* r, const char* channel, int32_t segment,
                               int32_t level, mef3io_record_list** out) {
  if (!r || !out) return fail_argument("NULL argument");
  *out = nullptr;
  using L = mef3io::Session::RecordLevels;
  L levels;
  switch (level) {
    case MEF3IO_RECORD_LEVEL_DEFAULT: levels = L::Default; break;
    case MEF3IO_RECORD_LEVEL_SESSION: levels = L::Session; break;
    case MEF3IO_RECORD_LEVEL_CHANNEL: levels = L::Channel; break;
    case MEF3IO_RECORD_LEVEL_SEGMENT: levels = L::Segment; break;
    case MEF3IO_RECORD_LEVEL_ALL: levels = L::All; break;
    default: return fail_argument("unknown record level");
  }
  return guarded([&] {
    std::optional<std::string> ch;
    if (channel) ch = channel;
    std::optional<int> seg;
    if (segment >= 0) seg = segment;
    auto list = std::make_unique<mef3io_record_list>();
    list->records = r->impl.records(ch, seg, levels);
    for (const auto& rec : list->records) list->fields.push_back(mef3io::record_fields(rec));
    *out = list.release();
  });
}

void mef3io_record_list_free(mef3io_record_list* list) { delete list; }

int mef3io_record_list_size(const mef3io_record_list* list, int32_t* out) {
  if (!list || !out) return fail_argument("NULL argument");
  *out = static_cast<int32_t>(list->records.size());
  return MEF3IO_OK;
}

int mef3io_record_list_header(const mef3io_record_list* list, int32_t index,
                              mef3io_record_header* out) {
  if (!list || !out) return fail_argument("NULL argument");
  if (index < 0 || static_cast<size_t>(index) >= list->records.size())
    return fail_argument("record index out of range");
  const mef3io::Record& rec = list->records[static_cast<size_t>(index)];
  copy_str(out->type, sizeof out->type, rec.type);
  out->time = rec.time;
  out->level = static_cast<int32_t>(rec.level);
  out->segment = rec.level == mef3io::RecordLevel::Segment ? rec.segment : -1;
  copy_str(out->channel, sizeof out->channel, rec.channel);
  out->version_major = rec.version_major;
  out->version_minor = rec.version_minor;
  out->n_fields = static_cast<int32_t>(list->fields[static_cast<size_t>(index)].size());
  return MEF3IO_OK;
}

int mef3io_record_list_field(const mef3io_record_list* list, int32_t index, int32_t field,
                             mef3io_record_field* out) {
  if (!list || !out) return fail_argument("NULL argument");
  const mef3io::RecordField* f = list_field(list, index, field);
  if (!f) return fail_argument("record or field index out of range");
  *out = mef3io_record_field{};
  copy_str(out->name, sizeof out->name, f->name);
  out->kind = MEF3IO_FIELD_ABSENT;
  if (!f->value) return MEF3IO_OK;
  std::visit(
      [&](const auto& v) {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, mef3io::si8>) {
          out->kind = MEF3IO_FIELD_INT64;
          out->int_value = v;
        } else if constexpr (std::is_same_v<T, mef3io::sf8>) {
          out->kind = MEF3IO_FIELD_DOUBLE;
          out->double_value = v;
        } else if constexpr (std::is_same_v<T, std::string>) {
          out->kind = MEF3IO_FIELD_STRING;
          out->string_value = v.c_str();
        } else if constexpr (std::is_same_v<T, std::vector<mef3io::si4>>) {
          out->kind = MEF3IO_FIELD_INT32_ARRAY;
          out->int32_values = v.data();
          out->count = static_cast<int64_t>(v.size());
        } else {
          out->kind = MEF3IO_FIELD_SEIZURE_CHANNELS;
          out->count = static_cast<int64_t>(v.size());
        }
      },
      *f->value);
  return MEF3IO_OK;
}

int mef3io_record_list_seizure_channel(const mef3io_record_list* list, int32_t index,
                                       int32_t field, int64_t entry,
                                       mef3io_seizure_channel* out) {
  if (!list || !out) return fail_argument("NULL argument");
  const mef3io::RecordField* f = list_field(list, index, field);
  if (!f) return fail_argument("record or field index out of range");
  const auto* chans =
      f->value ? std::get_if<std::vector<mef3io::SeizureChannel>>(&*f->value) : nullptr;
  if (!chans) return fail_argument("field is not a seizure channel list");
  if (entry < 0 || static_cast<size_t>(entry) >= chans->size())
    return fail_argument("seizure channel index out of range");
  const auto& c = (*chans)[static_cast<size_t>(entry)];
  copy_str(out->name, sizeof out->name, c.name);
  out->onset = c.onset;
  out->offset = c.offset;
  return MEF3IO_OK;
}

/* ----- archive ----------------------------------------------------------- */

int mef3io_writer_n_unreadable_metadata(mef3io_writer* w, int32_t* out) {
  if (!w || !out) return fail_argument("NULL argument");
  return guarded([&] { *out = static_cast<int32_t>(w->impl.unreadable_metadata().size()); });
}

int mef3io_writer_unreadable_metadata(mef3io_writer* w, int32_t index, char* buf,
                                      size_t buf_bytes) {
  if (!w || !buf) return fail_argument("NULL argument");
  return guarded([&] {
    const auto& u = w->impl.unreadable_metadata();
    if (index < 0 || static_cast<size_t>(index) >= u.size())
      throw std::out_of_range("unreadable-metadata index out of range");
    copy_str(buf, buf_bytes, u[static_cast<size_t>(index)]);
  });
}

int mef3io_writer_n_legacy_password_segments(mef3io_writer* w, int32_t* out) {
  if (!w || !out) return fail_argument("NULL argument");
  return guarded([&] { *out = static_cast<int32_t>(w->impl.legacy_password_segments().size()); });
}

int mef3io_writer_legacy_password_segment(mef3io_writer* w, int32_t index, char* buf,
                                          size_t buf_bytes) {
  if (!w || !buf) return fail_argument("NULL argument");
  return guarded([&] {
    const auto& u = w->impl.legacy_password_segments();
    if (index < 0 || static_cast<size_t>(index) >= u.size())
      throw std::out_of_range("legacy-password segment index out of range");
    copy_str(buf, buf_bytes, u[static_cast<size_t>(index)]);
  });
}

int mef3io_writer_set_durable(mef3io_writer* w, int durable) {
  if (!w) return fail_argument("writer must not be NULL");
  return guarded([&] { reinterpret_cast<mef3io::SessionWriter*>(w)->set_durable(durable != 0); });
}

int mef3io_recover_session(const char* mefd_path, int apply, int backup, const char* password,
                           char* out_summary, size_t out_summary_bytes, int64_t* out_segments) {
  return mef3io_recover_session_ex(mefd_path, apply, backup, password, out_summary,
                                   out_summary_bytes, out_segments, nullptr, nullptr, nullptr);
}

int mef3io_recover_session_ex(const char* mefd_path, int apply, int backup, const char* password,
                              char* out_summary, size_t out_summary_bytes, int64_t* out_segments,
                              const mef3io_cancel_token* cancel, mef3io_progress_fn progress,
                              void* user_data) {
  if (!mefd_path) return fail_argument("mefd_path must not be NULL");
  return guarded([&] {
    const auto report =
        mef3io::recover_session(mefd_path, apply != 0, backup != 0, password ? password : "",
                                make_control(cancel, progress, user_data));
    if (out_segments) *out_segments = static_cast<int64_t>(report.segments.size());
    if (out_summary && out_summary_bytes) {
      std::string text = std::to_string(report.segments_examined) + " segment(s) examined, " +
                         std::to_string(report.segments.size()) + " needing recovery";
      if (!report.applied) text += "  (DRY RUN - nothing was written)";
      for (const auto& s : report.segments) text += "\n    " + s.path + ": " + s.action;
      for (const auto& s : report.skipped) text += "\n    skipped " + s;
      for (const auto& n : report.notes) text += "\n    note: " + n;
      if (report.applied && !report.backup_root.empty())
        text += "\n  originals backed up under " + report.backup_root;
      copy_str(out_summary, out_summary_bytes, text);
    }
  });
}

int mef3io_archive_session(const char* session_dir, const char* tar_path, int overwrite,
                           char* out_path, size_t out_path_bytes) {
  return mef3io_archive_session_ex(session_dir, tar_path, overwrite, out_path, out_path_bytes,
                                   nullptr, nullptr, nullptr);
}

int mef3io_archive_session_ex(const char* session_dir, const char* tar_path, int overwrite,
                              char* out_path, size_t out_path_bytes,
                              const mef3io_cancel_token* cancel, mef3io_progress_fn progress,
                              void* user_data) {
  if (!session_dir) return fail_argument("session_dir must not be NULL");
  return guarded([&] {
    const std::string result =
        mef3io::archive_session(session_dir, tar_path ? tar_path : "", overwrite != 0,
                                make_control(cancel, progress, user_data));
    if (out_path && out_path_bytes) copy_str(out_path, out_path_bytes, result);
  });
}

int mef3io_extract_session(const char* tar_path, const char* dest_dir, int overwrite,
                           char* out_path, size_t out_path_bytes) {
  return mef3io_extract_session_ex(tar_path, dest_dir, overwrite, out_path, out_path_bytes,
                                   nullptr, nullptr, nullptr);
}

int mef3io_extract_session_ex(const char* tar_path, const char* dest_dir, int overwrite,
                              char* out_path, size_t out_path_bytes,
                              const mef3io_cancel_token* cancel, mef3io_progress_fn progress,
                              void* user_data) {
  if (!tar_path) return fail_argument("tar_path must not be NULL");
  return guarded([&] {
    const std::string result =
        mef3io::extract_session(tar_path, dest_dir ? dest_dir : "", overwrite != 0,
                                make_control(cancel, progress, user_data));
    if (out_path && out_path_bytes) copy_str(out_path, out_path_bytes, result);
  });
}

/* ----- writer ------------------------------------------------------------ */

static_assert(MEF3IO_MAX_BLOCK_SAMPLES == mef3io::MAX_BLOCK_SAMPLES,
              "the C ABI's block cap must be the core's");

int mef3io_check_block_length(int64_t samples_per_block) {
  return guarded([&] { mef3io::check_block_length(samples_per_block); });
}

int mef3io_check_signal_parameters(double sampling_frequency, double units_conversion_factor) {
  return guarded(
      [&] { mef3io::check_signal_parameters(sampling_frequency, units_conversion_factor); });
}

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

int mef3io_reader_set_threads(mef3io_reader* r, int n_threads) {
  if (!r) return fail_argument("NULL argument");
  return guarded([&] { r->impl.set_threads(n_threads); });
}

int mef3io_reader_threads(mef3io_reader* r, int* out) {
  if (!r || !out) return fail_argument("NULL argument");
  return guarded([&] { *out = r->impl.threads(); });
}

int mef3io_writer_set_threads(mef3io_writer* w, int n_threads) {
  if (!w) return fail_argument("NULL argument");
  return guarded([&] { w->impl.set_threads(n_threads); });
}

int mef3io_writer_threads(mef3io_writer* w, int* out) {
  if (!w || !out) return fail_argument("NULL argument");
  return guarded([&] { *out = w->impl.threads(); });
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
