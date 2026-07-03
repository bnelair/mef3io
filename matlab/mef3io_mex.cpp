// mef3io — MATLAB MEX gateway. One MEX function dispatching on a command
// string, built over the flat C ABI (mef3io/c_api.h) so exceptions never
// cross the MEX boundary. The user-facing API is the +mef3io package
// (mef3io.Reader / mef3io.Writer); this gateway is an implementation detail.
//
// Build: run matlab/build_mex.m (requires a C++20 compiler via `mex -setup`).
#include <mex.h>
#include <matrix.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

#include "mef3io/c_api.h"

namespace {

// ---- handle registry: validates handles and keeps the MEX locked while ----
// ---- any native object is alive (so `clear mex` cannot dangle them).   ----
std::unordered_set<std::uint64_t> g_readers;
std::unordered_set<std::uint64_t> g_writers;

void at_exit() {
  for (auto h : g_readers) mef3io_reader_close(reinterpret_cast<mef3io_reader*>(h));
  for (auto h : g_writers) mef3io_writer_close(reinterpret_cast<mef3io_writer*>(h));
  g_readers.clear();
  g_writers.clear();
}

void track(std::unordered_set<std::uint64_t>& set, std::uint64_t h) {
  if (g_readers.empty() && g_writers.empty()) {
    mexAtExit(at_exit);
    mexLock();
  }
  set.insert(h);
}

void untrack(std::unordered_set<std::uint64_t>& set, std::uint64_t h) {
  set.erase(h);
  if (g_readers.empty() && g_writers.empty()) mexUnlock();
}

[[noreturn]] void fail(const char* fmt, const char* detail = "") {
  mexErrMsgIdAndTxt("mef3io:error", fmt, detail);
  throw 0;  // unreachable; mexErrMsgIdAndTxt does not return
}

void check(int code) {
  if (code != MEF3IO_OK) fail("%s", mef3io_last_error());
}

std::string get_string(const mxArray* a, const char* what) {
  char* s = mxArrayToUTF8String(a);
  if (!s) fail("expected a string for %s", what);
  std::string out(s);
  mxFree(s);
  return out;
}

double get_scalar(const mxArray* a, const char* what) {
  if (!mxIsNumeric(a) || mxGetNumberOfElements(a) != 1) fail("expected a numeric scalar for %s", what);
  return mxGetScalar(a);
}

std::int64_t get_int64(const mxArray* a, const char* what) {
  if (mxIsInt64(a) && mxGetNumberOfElements(a) == 1) return *mxGetInt64s(a);
  return static_cast<std::int64_t>(get_scalar(a, what));
}

// [] -> "unset"; otherwise an int64/double uUTC.
std::int64_t get_opt_time(const mxArray* a, const char* what) {
  if (mxIsEmpty(a)) return MEF3IO_TIME_UNSET;
  return get_int64(a, what);
}

mef3io_reader* get_reader(const mxArray* a) {
  if (!mxIsUint64(a) || mxGetNumberOfElements(a) != 1) fail("bad reader handle");
  std::uint64_t h = *mxGetUint64s(a);
  if (g_readers.find(h) == g_readers.end()) fail("invalid or closed reader handle");
  return reinterpret_cast<mef3io_reader*>(h);
}

mef3io_writer* get_writer(const mxArray* a) {
  if (!mxIsUint64(a) || mxGetNumberOfElements(a) != 1) fail("bad writer handle");
  std::uint64_t h = *mxGetUint64s(a);
  if (g_writers.find(h) == g_writers.end()) fail("invalid or closed writer handle");
  return reinterpret_cast<mef3io_writer*>(h);
}

mxArray* make_handle(void* p) {
  mxArray* out = mxCreateNumericMatrix(1, 1, mxUINT64_CLASS, mxREAL);
  *mxGetUint64s(out) = reinterpret_cast<std::uint64_t>(p);
  return out;
}

mxArray* make_int64(std::int64_t v) {
  mxArray* out = mxCreateNumericMatrix(1, 1, mxINT64_CLASS, mxREAL);
  *mxGetInt64s(out) = v;
  return out;
}

void need_args(int nrhs, int n, const char* cmd) {
  if (nrhs < n) fail("not enough arguments for command %s", cmd);
}

// Optional channel argument: [] -> session level (NULL).
struct OptChannel {
  std::string value;
  bool present = false;
  const char* c_str() const { return present ? value.c_str() : nullptr; }
};

OptChannel get_opt_channel(const mxArray* a) {
  OptChannel ch;
  if (!mxIsEmpty(a)) {
    ch.value = get_string(a, "channel");
    ch.present = true;
  }
  return ch;
}

}  // namespace

void mexFunction(int nlhs, mxArray* plhs[], int nrhs, const mxArray* prhs[]) {
  if (nrhs < 1) fail("usage: mef3io_mex(command, ...)");
  const std::string cmd = get_string(prhs[0], "command");

  // ---- misc -----------------------------------------------------------
  if (cmd == "version") {
    plhs[0] = mxCreateString(mef3io_version());
    return;
  }

  // ---- reader ----------------------------------------------------------
  if (cmd == "reader_open") {
    need_args(nrhs, 4, "reader_open");
    std::string path = get_string(prhs[1], "path");
    std::string password = get_string(prhs[2], "password");
    int n_threads = static_cast<int>(get_scalar(prhs[3], "n_threads"));
    mef3io_reader* r = nullptr;
    check(mef3io_reader_open(path.c_str(), password.c_str(), n_threads, &r));
    track(g_readers, reinterpret_cast<std::uint64_t>(r));
    plhs[0] = make_handle(r);
    return;
  }
  if (cmd == "reader_close") {
    need_args(nrhs, 2, "reader_close");
    mef3io_reader* r = get_reader(prhs[1]);
    untrack(g_readers, reinterpret_cast<std::uint64_t>(r));
    mef3io_reader_close(r);
    return;
  }
  if (cmd == "reader_channels") {
    need_args(nrhs, 2, "reader_channels");
    mef3io_reader* r = get_reader(prhs[1]);
    std::int32_t n = 0;
    check(mef3io_reader_n_channels(r, &n));
    plhs[0] = mxCreateCellMatrix(n, 1);
    char name[512];
    for (std::int32_t i = 0; i < n; ++i) {
      check(mef3io_reader_channel_name(r, i, name, sizeof name));
      mxSetCell(plhs[0], i, mxCreateString(name));
    }
    return;
  }
  if (cmd == "reader_info") {
    need_args(nrhs, 3, "reader_info");
    mef3io_reader* r = get_reader(prhs[1]);
    std::string ch = get_string(prhs[2], "channel");
    mef3io_channel_info ci{};
    check(mef3io_reader_info(r, ch.c_str(), &ci));
    const char* fields[] = {"sampling_frequency", "units_conversion_factor", "start_time",
                            "end_time", "number_of_samples", "recording_time_offset",
                            "n_segments", "section3_available", "units_description",
                            "subject_name_1", "subject_name_2", "subject_id",
                            "recording_location"};
    plhs[0] = mxCreateStructMatrix(1, 1, 13, fields);
    mxSetField(plhs[0], 0, "sampling_frequency", mxCreateDoubleScalar(ci.sampling_frequency));
    mxSetField(plhs[0], 0, "units_conversion_factor",
               mxCreateDoubleScalar(ci.units_conversion_factor));
    mxSetField(plhs[0], 0, "start_time", make_int64(ci.start_time));
    mxSetField(plhs[0], 0, "end_time", make_int64(ci.end_time));
    mxSetField(plhs[0], 0, "number_of_samples", make_int64(ci.number_of_samples));
    mxSetField(plhs[0], 0, "recording_time_offset", make_int64(ci.recording_time_offset));
    mxSetField(plhs[0], 0, "n_segments", mxCreateDoubleScalar(ci.n_segments));
    mxSetField(plhs[0], 0, "section3_available", mxCreateLogicalScalar(ci.section3_available != 0));
    mxSetField(plhs[0], 0, "units_description", mxCreateString(ci.units_description));
    mxSetField(plhs[0], 0, "subject_name_1", mxCreateString(ci.subject_name_1));
    mxSetField(plhs[0], 0, "subject_name_2", mxCreateString(ci.subject_name_2));
    mxSetField(plhs[0], 0, "subject_id", mxCreateString(ci.subject_id));
    mxSetField(plhs[0], 0, "recording_location", mxCreateString(ci.recording_location));
    return;
  }
  if (cmd == "reader_read") {
    need_args(nrhs, 5, "reader_read");
    mef3io_reader* r = get_reader(prhs[1]);
    std::string ch = get_string(prhs[2], "channel");
    std::int64_t t0 = get_opt_time(prhs[3], "t0");
    std::int64_t t1 = get_opt_time(prhs[4], "t1");
    std::int64_t n = 0;
    check(mef3io_reader_read_size(r, ch.c_str(), t0, t1, &n));
    plhs[0] = mxCreateNumericMatrix(static_cast<mwSize>(n), 1, mxDOUBLE_CLASS, mxREAL);
    std::int64_t n_read = 0;
    check(mef3io_reader_read(r, ch.c_str(), t0, t1, mxGetDoubles(plhs[0]), n, &n_read));
    return;
  }
  if (cmd == "reader_read_raw") {
    need_args(nrhs, 5, "reader_read_raw");
    mef3io_reader* r = get_reader(prhs[1]);
    std::string ch = get_string(prhs[2], "channel");
    std::int64_t t0 = get_opt_time(prhs[3], "t0");
    std::int64_t t1 = get_opt_time(prhs[4], "t1");
    std::int64_t n = 0;
    check(mef3io_reader_read_size(r, ch.c_str(), t0, t1, &n));
    mxArray* samples = mxCreateNumericMatrix(static_cast<mwSize>(n), 1, mxINT32_CLASS, mxREAL);
    mxArray* valid = mxCreateLogicalMatrix(static_cast<mwSize>(n), 1);
    std::int64_t n_read = 0, start_uutc = 0;
    static_assert(sizeof(mxLogical) == sizeof(std::uint8_t), "mxLogical must be 1 byte");
    check(mef3io_reader_read_raw(r, ch.c_str(), t0, t1, mxGetInt32s(samples),
                                 reinterpret_cast<std::uint8_t*>(mxGetLogicals(valid)), n,
                                 &n_read, &start_uutc));
    mef3io_channel_info ci{};
    check(mef3io_reader_info(r, ch.c_str(), &ci));
    const char* fields[] = {"samples", "valid", "start_uutc", "sampling_frequency",
                            "units_conversion_factor"};
    plhs[0] = mxCreateStructMatrix(1, 1, 5, fields);
    mxSetField(plhs[0], 0, "samples", samples);
    mxSetField(plhs[0], 0, "valid", valid);
    mxSetField(plhs[0], 0, "start_uutc", make_int64(start_uutc));
    mxSetField(plhs[0], 0, "sampling_frequency", mxCreateDoubleScalar(ci.sampling_frequency));
    mxSetField(plhs[0], 0, "units_conversion_factor",
               mxCreateDoubleScalar(ci.units_conversion_factor));
    return;
  }
  if (cmd == "reader_segments") {
    need_args(nrhs, 3, "reader_segments");
    mef3io_reader* r = get_reader(prhs[1]);
    std::string ch = get_string(prhs[2], "channel");
    std::int32_t n = 0;
    check(mef3io_reader_n_segments(r, ch.c_str(), &n));
    const char* fields[] = {"segment",        "start_time",       "end_time",
                            "start_sample",   "number_of_samples", "number_of_blocks",
                            "path"};
    plhs[0] = mxCreateStructMatrix(n, 1, 7, fields);
    for (std::int32_t i = 0; i < n; ++i) {
      mef3io_segment_info s{};
      check(mef3io_reader_segment(r, ch.c_str(), i, &s));
      mxSetField(plhs[0], i, "segment", mxCreateDoubleScalar(s.segment_number));
      mxSetField(plhs[0], i, "start_time", make_int64(s.start_time));
      mxSetField(plhs[0], i, "end_time", make_int64(s.end_time));
      mxSetField(plhs[0], i, "start_sample", make_int64(s.start_sample));
      mxSetField(plhs[0], i, "number_of_samples", make_int64(s.number_of_samples));
      mxSetField(plhs[0], i, "number_of_blocks", make_int64(s.number_of_blocks));
      mxSetField(plhs[0], i, "path", mxCreateString(s.path));
    }
    return;
  }
  if (cmd == "reader_toc") {
    need_args(nrhs, 3, "reader_toc");
    mef3io_reader* r = get_reader(prhs[1]);
    std::string ch = get_string(prhs[2], "channel");
    std::int64_t n = 0;
    check(mef3io_reader_n_blocks(r, ch.c_str(), &n));
    mxArray* start_uutc = mxCreateNumericMatrix(static_cast<mwSize>(n), 1, mxINT64_CLASS, mxREAL);
    mxArray* start_sample = mxCreateNumericMatrix(static_cast<mwSize>(n), 1, mxINT64_CLASS, mxREAL);
    mxArray* nsamp = mxCreateNumericMatrix(static_cast<mwSize>(n), 1, mxINT64_CLASS, mxREAL);
    mxArray* maxv = mxCreateNumericMatrix(static_cast<mwSize>(n), 1, mxINT32_CLASS, mxREAL);
    mxArray* minv = mxCreateNumericMatrix(static_cast<mwSize>(n), 1, mxINT32_CLASS, mxREAL);
    mxArray* disc = mxCreateLogicalMatrix(static_cast<mwSize>(n), 1);
    for (std::int64_t i = 0; i < n; ++i) {
      mef3io_block_info b{};
      check(mef3io_reader_block(r, ch.c_str(), i, &b));
      mxGetInt64s(start_uutc)[i] = b.start_uutc;
      mxGetInt64s(start_sample)[i] = b.start_sample;
      mxGetInt64s(nsamp)[i] = b.number_of_samples;
      mxGetInt32s(maxv)[i] = b.maximum_sample_value;
      mxGetInt32s(minv)[i] = b.minimum_sample_value;
      mxGetLogicals(disc)[i] = b.discontinuity != 0;
    }
    const char* fields[] = {"start_uutc", "start_sample", "number_of_samples",
                            "maximum_sample_value", "minimum_sample_value", "discontinuity"};
    plhs[0] = mxCreateStructMatrix(1, 1, 6, fields);
    mxSetField(plhs[0], 0, "start_uutc", start_uutc);
    mxSetField(plhs[0], 0, "start_sample", start_sample);
    mxSetField(plhs[0], 0, "number_of_samples", nsamp);
    mxSetField(plhs[0], 0, "maximum_sample_value", maxv);
    mxSetField(plhs[0], 0, "minimum_sample_value", minv);
    mxSetField(plhs[0], 0, "discontinuity", disc);
    return;
  }
  if (cmd == "reader_records") {
    need_args(nrhs, 3, "reader_records");
    mef3io_reader* r = get_reader(prhs[1]);
    OptChannel ch = get_opt_channel(prhs[2]);
    std::int32_t n = 0;
    check(mef3io_reader_n_records(r, ch.c_str(), &n));
    const char* fields[] = {"type", "time", "text", "duration"};
    plhs[0] = mxCreateStructMatrix(n, 1, 4, fields);
    for (std::int32_t i = 0; i < n; ++i) {
      mef3io_record_info rec{};
      check(mef3io_reader_record(r, ch.c_str(), i, &rec));
      mxSetField(plhs[0], i, "type", mxCreateString(rec.type));
      mxSetField(plhs[0], i, "time", make_int64(rec.time));
      mxSetField(plhs[0], i, "text", mxCreateString(rec.text));
      mxSetField(plhs[0], i, "duration",
                 rec.duration >= 0 ? make_int64(rec.duration) : mxCreateDoubleMatrix(0, 0, mxREAL));
    }
    return;
  }

  // ---- writer ----------------------------------------------------------
  if (cmd == "writer_open") {
    need_args(nrhs, 5, "writer_open");
    std::string path = get_string(prhs[1], "path");
    int overwrite = static_cast<int>(get_scalar(prhs[2], "overwrite"));
    std::string p1 = get_string(prhs[3], "password1");
    std::string p2 = get_string(prhs[4], "password2");
    mef3io_writer* w = nullptr;
    check(mef3io_writer_open(path.c_str(), overwrite, p1.c_str(), p2.c_str(), &w));
    track(g_writers, reinterpret_cast<std::uint64_t>(w));
    plhs[0] = make_handle(w);
    return;
  }
  if (cmd == "writer_close") {
    need_args(nrhs, 2, "writer_close");
    mef3io_writer* w = get_writer(prhs[1]);
    untrack(g_writers, reinterpret_cast<std::uint64_t>(w));
    mef3io_writer_close(w);
    return;
  }
  if (cmd == "writer_set_units") {
    need_args(nrhs, 3, "writer_set_units");
    check(mef3io_writer_set_units(get_writer(prhs[1]),
                                  get_string(prhs[2], "units").c_str()));
    return;
  }
  if (cmd == "writer_set_block_length") {
    need_args(nrhs, 3, "writer_set_block_length");
    check(mef3io_writer_set_block_length(get_writer(prhs[1]),
                                         get_int64(prhs[2], "block_length")));
    return;
  }
  if (cmd == "writer_set_threads") {
    need_args(nrhs, 3, "writer_set_threads");
    check(mef3io_writer_set_threads(get_writer(prhs[1]),
                                    static_cast<int>(get_scalar(prhs[2], "n_threads"))));
    return;
  }
  if (cmd == "writer_write" || cmd == "writer_write_int32") {
    const bool is_float = (cmd == "writer_write");
    // writer_write:      h, ch, data(double), start, fs, precision, new_segment
    // writer_write_int32: h, ch, data(int32), valid([]|logical|uint8), ufact, start, fs, new_segment
    need_args(nrhs, is_float ? 7 : 8, cmd.c_str());
    mef3io_writer* w = get_writer(prhs[1]);
    std::string ch = get_string(prhs[2], "channel");
    mef3io_write_summary sum{};
    if (is_float) {
      if (!mxIsDouble(prhs[3]) || mxIsComplex(prhs[3])) fail("data must be a real double vector");
      std::int64_t n = static_cast<std::int64_t>(mxGetNumberOfElements(prhs[3]));
      std::int64_t start = get_int64(prhs[4], "start_uutc");
      double fs = get_scalar(prhs[5], "fs");
      int precision = static_cast<int>(get_scalar(prhs[6], "precision"));
      int new_segment = nrhs > 7 ? static_cast<int>(get_scalar(prhs[7], "new_segment")) : 0;
      check(mef3io_writer_write_float(w, ch.c_str(), mxGetDoubles(prhs[3]), n, start, fs,
                                      precision, new_segment, &sum));
    } else {
      if (!mxIsInt32(prhs[3])) fail("data must be an int32 vector");
      std::int64_t n = static_cast<std::int64_t>(mxGetNumberOfElements(prhs[3]));
      const std::uint8_t* valid = nullptr;
      std::vector<std::uint8_t> valid_copy;
      if (!mxIsEmpty(prhs[4])) {
        if (mxGetNumberOfElements(prhs[4]) != static_cast<size_t>(n))
          fail("valid mask must match data length");
        if (mxIsLogical(prhs[4])) {
          valid = reinterpret_cast<const std::uint8_t*>(mxGetLogicals(prhs[4]));
        } else if (mxIsUint8(prhs[4])) {
          valid = mxGetUint8s(prhs[4]);
        } else if (mxIsDouble(prhs[4]) && !mxIsComplex(prhs[4])) {
          // Double mask: nonzero = valid (the Writer class also coerces).
          valid_copy.resize(static_cast<size_t>(n));
          const double* src = mxGetDoubles(prhs[4]);
          for (std::int64_t i = 0; i < n; ++i)
            valid_copy[static_cast<size_t>(i)] = src[i] != 0.0;
          valid = valid_copy.data();
        } else {
          fail("valid mask must be logical, uint8, or double");
        }
      }
      double ufact = get_scalar(prhs[5], "ufact");
      std::int64_t start = get_int64(prhs[6], "start_uutc");
      double fs = get_scalar(prhs[7], "fs");
      int new_segment = nrhs > 8 ? static_cast<int>(get_scalar(prhs[8], "new_segment")) : 0;
      check(mef3io_writer_write_int32(w, ch.c_str(), mxGetInt32s(prhs[3]), valid, n, ufact,
                                      start, fs, new_segment, &sum));
    }
    const char* fields[] = {"samples_written", "blocks", "gaps_skipped", "segment"};
    plhs[0] = mxCreateStructMatrix(1, 1, 4, fields);
    mxSetField(plhs[0], 0, "samples_written", make_int64(sum.samples_written));
    mxSetField(plhs[0], 0, "blocks", make_int64(sum.blocks));
    mxSetField(plhs[0], 0, "gaps_skipped", make_int64(sum.gaps_skipped));
    mxSetField(plhs[0], 0, "segment", mxCreateDoubleScalar(sum.segment));
    return;
  }
  if (cmd == "writer_write_records") {
    need_args(nrhs, 4, "writer_write_records");
    mef3io_writer* w = get_writer(prhs[1]);
    OptChannel ch = get_opt_channel(prhs[2]);
    const mxArray* recs = prhs[3];
    if (!mxIsStruct(recs)) fail("records must be a struct array");
    mef3io_records* list = mef3io_records_create();
    const size_t n = mxGetNumberOfElements(recs);
    for (size_t i = 0; i < n; ++i) {
      const mxArray* f_time = mxGetField(recs, i, "time");
      if (!f_time) {
        mef3io_records_free(list);
        fail("each record needs a 'time' field");
      }
      std::int64_t time = get_int64(f_time, "record time");
      const mxArray* f_type = mxGetField(recs, i, "type");
      std::string type = f_type && !mxIsEmpty(f_type) ? get_string(f_type, "type") : "Note";
      const mxArray* f_text = mxGetField(recs, i, "text");
      std::string text = f_text && !mxIsEmpty(f_text) ? get_string(f_text, "text") : "";
      const mxArray* f_dur = mxGetField(recs, i, "duration");
      std::int64_t dur = f_dur && !mxIsEmpty(f_dur) ? get_int64(f_dur, "duration") : -1;
      int code = mef3io_records_add(list, type.c_str(), time, text.c_str(), dur);
      if (code != MEF3IO_OK) {
        mef3io_records_free(list);
        fail("%s", mef3io_last_error());
      }
    }
    int code = mef3io_writer_write_records(w, ch.c_str(), list);
    mef3io_records_free(list);
    check(code);
    return;
  }

  fail("unknown command: %s", cmd.c_str());
}
