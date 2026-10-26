// mef3io — MATLAB MEX gateway. One MEX function dispatching on a command
// string, built over the flat C ABI (mef3io/c_api.h) so exceptions never
// cross the MEX boundary. The user-facing API is the +mef3io package
// (mef3io.Reader / mef3io.Writer); this gateway is an implementation detail.
//
// Build: run matlab/build_mex.m (requires a C++20 compiler via `mex -setup`).
#include <mex.h>
#include <matrix.h>

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <memory>
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

// Every argument / library error in this file is a C++ exception of this
// type, thrown from wherever it is detected and turned into a MATLAB error in
// exactly ONE place: mexFunction, after the whole C++ call has unwound.
//
// Calling mexErrMsgIdAndTxt from a helper is NOT equivalent. It raises
// MATLAB's own C++ exception from MATLAB's libstdc++, and on Linux this MEX
// links its own static libstdc++ (see build_mex.m), so unwinding that
// exception through the MEX's frames — any frame with a destructor to run —
// can abort MATLAB inside __cxa_throw. That is what a fractional time did.
struct MexError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

[[noreturn]] void fail(const char* fmt, const char* detail = "") {
  char buf[2048];
  std::snprintf(buf, sizeof buf, fmt, detail);
  throw MexError(buf);
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

// Sparse and complex arrays are refused wherever a buffer or a scalar is read
// (#83): mxGetDoubles/mxGetLogicals on a SPARSE array return its nonzero
// values only, so treating the result as dense read past it (crash) or
// stored the wrong mask; a complex scalar would silently lose its imaginary
// part. The +mef3io classes pass full() arrays; this guards direct calls.
// Buffers only: scalars go through get_scalar, which accepts sparse.
void require_dense_real(const mxArray* a, const char* what) {
  if (mxIsSparse(a)) fail("%s must be a full (non-sparse) array; use full()", what);
  if (mxIsComplex(a)) fail("%s must be real, not complex", what);
}

// A sparse SCALAR is fine (mxGetScalar reads it correctly, and S(i,j) of a
// sparse matrix is one); only buffers need require_dense_real.
double get_scalar(const mxArray* a, const char* what) {
  if (!mxIsNumeric(a) || mxGetNumberOfElements(a) != 1) fail("expected a numeric scalar for %s", what);
  if (mxIsComplex(a)) fail("%s must be real, not complex", what);
  return mxGetScalar(a);
}

// A double -> integer conversion of NaN, Inf or anything out of the target's
// range is undefined behaviour, not an error, so every numeric argument that
// becomes an integer is checked here first: finite, integral, in range.
double checked_integral(const mxArray* a, const char* what, double lo, double hi) {
  const double v = get_scalar(a, what);
  if (!std::isfinite(v) || v != std::floor(v) || v < lo || v > hi)
    fail("expected a finite integer in range for %s", what);
  return v;
}

// [-2^63, 2^63): the upper bound is exclusive because 2^63 itself is a
// double but not an int64.
constexpr double TWO63 = 9223372036854775808.0;

std::int64_t get_int64(const mxArray* a, const char* what) {
  if (mxIsInt64(a) && mxGetNumberOfElements(a) == 1) return *mxGetInt64s(a);
  const double v = checked_integral(a, what, -TWO63, TWO63);
  if (v >= TWO63) fail("expected a finite integer in range for %s", what);
  return static_cast<std::int64_t>(v);
}

// A time or duration in microseconds. Fractions are ROUNDED to the nearest
// microsecond rather than rejected: pure-double MATLAB code computing
// t0 + n/fs*1e6 produces them routinely, and a microsecond is the format's
// resolution. Non-finite and out-of-range values are still errors.
std::int64_t get_time(const mxArray* a, const char* what) {
  if (mxIsInt64(a) && mxGetNumberOfElements(a) == 1) return *mxGetInt64s(a);
  const double v = std::nearbyint(get_scalar(a, what));
  if (!std::isfinite(v) || v < -TWO63 || v >= TWO63)
    fail("expected a finite time in microseconds for %s", what);
  return static_cast<std::int64_t>(v);
}

int get_int(const mxArray* a, const char* what) {
  return static_cast<int>(checked_integral(a, what, static_cast<double>(INT32_MIN),
                                           static_cast<double>(INT32_MAX)));
}

// [] -> "unset"; otherwise an int64/double uUTC.
std::int64_t get_opt_time(const mxArray* a, const char* what) {
  if (mxIsEmpty(a)) return MEF3IO_TIME_UNSET;
  return get_time(a, what);
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

// A MATLAB char array from UTF-8 bytes. mxCreateString interprets its input
// in the user's locale codepage (not UTF-8 on Windows), while this binding
// reads strings with mxArrayToUTF8String — so non-ASCII text (a record, a
// subject name, a path) would not round-trip. Every string this MEX returns
// goes through here: decode to UTF-16 (MATLAB's mxChar) ourselves; an invalid
// byte becomes U+FFFD, so one malformed record cannot fail the whole call.
mxArray* make_utf8_string(const std::string& s) {
  std::u16string out;
  out.reserve(s.size());
  const auto* p = reinterpret_cast<const unsigned char*>(s.data());
  const std::size_t n = s.size();
  std::size_t i = 0;
  while (i < n) {
    std::uint32_t cp = 0xFFFD;
    std::size_t len = 1;
    const unsigned char c = p[i];
    if (c < 0x80) {
      cp = c;
    } else if (c >= 0xC2 && c <= 0xF4) {
      len = c < 0xE0 ? 2 : (c < 0xF0 ? 3 : 4);
      std::uint32_t v = c & (0x7F >> len);
      bool ok = i + len <= n;
      for (std::size_t k = 1; ok && k < len; ++k) {
        if ((p[i + k] & 0xC0) != 0x80) ok = false;
        else v = (v << 6) | (p[i + k] & 0x3F);
      }
      const std::uint32_t min = len == 2 ? 0x80 : (len == 3 ? 0x800 : 0x10000);
      if (ok && v >= min && v <= 0x10FFFF && !(v >= 0xD800 && v <= 0xDFFF)) cp = v;
      else len = 1;  // resynchronise on the next byte
    }
    if (cp >= 0x10000) {
      cp -= 0x10000;
      out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
      out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
    } else {
      out.push_back(static_cast<char16_t>(cp));
    }
    i += len;
  }
  const mwSize dims[2] = {1, static_cast<mwSize>(out.size())};
  mxArray* a = mxCreateCharArray(2, dims);
  if (!out.empty()) std::memcpy(mxGetChars(a), out.data(), out.size() * sizeof(char16_t));
  return a;
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

// [] when the file carries no provenance; otherwise a struct mirroring the
// Python dict. Unknown codes (a newer writer's) read as 'unknown(<code>)'.
static mxArray* provenance_struct(const mef3io_provenance& p) {
  if (!p.present) return mxCreateDoubleMatrix(0, 0, mxREAL);
  auto name = [](int code) {
    const char* n = mef3io_operation_name(code);
    return n ? std::string(n) : "unknown(" + std::to_string(code) + ")";
  };
  const char* f[] = {"layout_version", "created_by", "last_modified_by", "last_operation",
                     "operations", "modification_count"};
  mxArray* s = mxCreateStructMatrix(1, 1, 6, f);
  mxSetField(s, 0, "layout_version", mxCreateDoubleScalar(p.layout_version));
  mxSetField(s, 0, "created_by", make_utf8_string(p.created_by));
  mxSetField(s, 0, "last_modified_by", make_utf8_string(p.last_modified_by));
  mxSetField(s, 0, "last_operation", make_utf8_string(name(p.last_operation).c_str()));
  int n_ops = 0;
  for (int b = 0; b < 32; ++b) n_ops += (p.operations_mask >> b) & 1u;
  mxArray* ops = mxCreateCellMatrix(1, static_cast<mwSize>(n_ops));
  for (int b = 0, k = 0; b < 32; ++b)
    if ((p.operations_mask >> b) & 1u) mxSetCell(ops, k++, make_utf8_string(name(b).c_str()));
  mxSetField(s, 0, "operations", ops);
  mxSetField(s, 0, "modification_count", mxCreateDoubleScalar(p.modification_count));
  return s;
}

static void dispatch(int nlhs, mxArray* plhs[], int nrhs, const mxArray* prhs[]) {
  if (nrhs < 1) fail("usage: mef3io_mex(command, ...)");
  const std::string cmd = get_string(prhs[0], "command");

  // ---- misc -----------------------------------------------------------
  if (cmd == "version") {
    plhs[0] = make_utf8_string(mef3io_version());
    return;
  }
  if (cmd == "archive_session") {
    need_args(nrhs, 4, "archive_session");
    std::string session_dir = get_string(prhs[1], "session_dir");
    std::string tar_path = get_string(prhs[2], "tar_path");
    int overwrite = get_int(prhs[3], "overwrite");
    char out_path[2048] = {0};
    check(mef3io_archive_session(session_dir.c_str(), tar_path.c_str(), overwrite, out_path,
                                 sizeof out_path));
    plhs[0] = make_utf8_string(out_path);
    return;
  }
  if (cmd == "recover_session") {
    need_args(nrhs, 5, "recover_session");
    std::string path = get_string(prhs[1], "path");
    int apply = get_int(prhs[2], "apply");
    int backup = get_int(prhs[3], "backup");
    std::string password = get_string(prhs[4], "password");
    std::vector<char> summary(8192, 0);
    int64_t segments = 0;
    check(mef3io_recover_session(path.c_str(), apply, backup, password.c_str(), summary.data(),
                                 summary.size(), &segments));
    plhs[0] = make_utf8_string(summary.data());
    if (nlhs > 1) plhs[1] = mxCreateDoubleScalar(static_cast<double>(segments));
    return;
  }
  if (cmd == "extract_session") {
    need_args(nrhs, 4, "extract_session");
    std::string tar_path = get_string(prhs[1], "tar_path");
    std::string dest_dir = get_string(prhs[2], "dest_dir");
    int overwrite = get_int(prhs[3], "overwrite");
    char out_path[2048] = {0};
    check(mef3io_extract_session(tar_path.c_str(), dest_dir.c_str(), overwrite, out_path,
                                 sizeof out_path));
    plhs[0] = make_utf8_string(out_path);
    return;
  }

  // ---- reader ----------------------------------------------------------
  if (cmd == "reader_open") {
    // The 5th argument (strict) is optional so an older caller still works.
    if (nrhs < 4) need_args(nrhs, 4, "reader_open");
    std::string path = get_string(prhs[1], "path");
    std::string password = get_string(prhs[2], "password");
    int n_threads = get_int(prhs[3], "n_threads");
    int strict = nrhs >= 5 ? get_int(prhs[4], "strict") : 1;
    mef3io_reader* r = nullptr;
    check(mef3io_reader_open_ex(path.c_str(), password.c_str(), n_threads, strict, &r));
    track(g_readers, reinterpret_cast<std::uint64_t>(r));
    plhs[0] = make_handle(r);
    return;
  }
  if (cmd == "reader_set_threads") {
    need_args(nrhs, 3, "reader_set_threads");
    check(mef3io_reader_set_threads(get_reader(prhs[1]), get_int(prhs[2], "n_threads")));
    return;
  }
  if (cmd == "reader_threads") {
    need_args(nrhs, 2, "reader_threads");
    int n = 0;
    check(mef3io_reader_threads(get_reader(prhs[1]), &n));
    plhs[0] = mxCreateDoubleScalar(static_cast<double>(n));
    return;
  }
  if (cmd == "reader_problems") {
    need_args(nrhs, 2, "reader_problems");
    mef3io_reader* r = get_reader(prhs[1]);
    int32_t n = 0;
    check(mef3io_reader_n_problems(r, &n));
    const char* fields[] = {"channel", "segment", "path", "reason", "kind"};
    plhs[0] = mxCreateStructMatrix(n > 0 ? 1 : 0, n > 0 ? static_cast<mwSize>(n) : 0, 5, fields);
    for (int32_t i = 0; i < n; ++i) {
      mef3io_segment_problem p{};
      check(mef3io_reader_problem(r, i, &p));
      std::int32_t kind = MEF3IO_PROBLEM_SEGMENT;
      check(mef3io_reader_problem_kind(r, i, &kind));
      mxSetField(plhs[0], i, "kind",
                 make_utf8_string(kind == MEF3IO_PROBLEM_RECORDS ? "records" : "segment"));
      mxSetField(plhs[0], i, "channel", make_utf8_string(p.channel));
      mxSetField(plhs[0], i, "segment", mxCreateDoubleScalar(p.segment_number));
      mxSetField(plhs[0], i, "path", make_utf8_string(p.segment));
      mxSetField(plhs[0], i, "reason", make_utf8_string(p.reason));
    }
    return;
  }
  if (cmd == "reader_legacy_password_channels") {
    need_args(nrhs, 2, "reader_legacy_password_channels");
    int32_t n = 0;
    check(mef3io_reader_n_legacy_password_channels(get_reader(prhs[1]), &n));
    plhs[0] = mxCreateDoubleScalar(n);
    return;
  }
  if (cmd == "reader_legacy_password_record_files") {
    need_args(nrhs, 2, "reader_legacy_password_record_files");
    int32_t n = 0;
    check(mef3io_reader_n_legacy_password_record_files(get_reader(prhs[1]), &n));
    plhs[0] = mxCreateDoubleScalar(n);
    return;
  }
  if (cmd == "password_interop_issue") {
    need_args(nrhs, 2, "password_interop_issue");
    std::string pw = get_string(prhs[1], "password");
    char buf[1024] = {0};
    check(mef3io_password_interop_issue(pw.c_str(), buf, sizeof buf));
    plhs[0] = make_utf8_string(buf);
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
      mxSetCell(plhs[0], i, make_utf8_string(name));
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
                            "acquisition_channel_number", "low_frequency_filter",
                            "high_frequency_filter", "notch_filter", "line_frequency",
                            "gmt_offset", "session_description", "channel_description",
                            "reference_description", "subject_name_1", "subject_name_2",
                            "subject_id", "recording_location"};
    plhs[0] = mxCreateStructMatrix(1, 1, 22, fields);
    mxSetField(plhs[0], 0, "sampling_frequency", mxCreateDoubleScalar(ci.sampling_frequency));
    mxSetField(plhs[0], 0, "units_conversion_factor",
               mxCreateDoubleScalar(ci.units_conversion_factor));
    mxSetField(plhs[0], 0, "start_time", make_int64(ci.start_time));
    mxSetField(plhs[0], 0, "end_time", make_int64(ci.end_time));
    mxSetField(plhs[0], 0, "number_of_samples", make_int64(ci.number_of_samples));
    mxSetField(plhs[0], 0, "recording_time_offset", make_int64(ci.recording_time_offset));
    mxSetField(plhs[0], 0, "n_segments", mxCreateDoubleScalar(ci.n_segments));
    mxSetField(plhs[0], 0, "section3_available", mxCreateLogicalScalar(ci.section3_available != 0));
    mxSetField(plhs[0], 0, "units_description", make_utf8_string(ci.units_description));
    mxSetField(plhs[0], 0, "acquisition_channel_number",
               make_int64(ci.acquisition_channel_number));
    mxSetField(plhs[0], 0, "low_frequency_filter", mxCreateDoubleScalar(ci.low_frequency_filter));
    mxSetField(plhs[0], 0, "high_frequency_filter", mxCreateDoubleScalar(ci.high_frequency_filter));
    mxSetField(plhs[0], 0, "notch_filter", mxCreateDoubleScalar(ci.notch_filter));
    mxSetField(plhs[0], 0, "line_frequency", mxCreateDoubleScalar(ci.line_frequency));
    mxSetField(plhs[0], 0, "gmt_offset", mxCreateDoubleScalar(ci.gmt_offset));
    mxSetField(plhs[0], 0, "session_description", make_utf8_string(ci.session_description));
    mxSetField(plhs[0], 0, "channel_description", make_utf8_string(ci.channel_description));
    mxSetField(plhs[0], 0, "reference_description", make_utf8_string(ci.reference_description));
    mxSetField(plhs[0], 0, "subject_name_1", make_utf8_string(ci.subject_name_1));
    mxSetField(plhs[0], 0, "subject_name_2", make_utf8_string(ci.subject_name_2));
    mxSetField(plhs[0], 0, "subject_id", make_utf8_string(ci.subject_id));
    mxSetField(plhs[0], 0, "recording_location", make_utf8_string(ci.recording_location));
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
    // Where each conversion factor applies (segments may store different
    // ones). One span is the usual case; a too-small buffer reports the size
    // it needs, and the read is repeated once with that.
    std::vector<mef3io_factor_span> spans(64);  // ~1.5 KB: the retry is practically never taken
    std::int32_t n_spans = 0;
    int rc = mef3io_reader_read_raw_factors(
        r, ch.c_str(), t0, t1, mxGetInt32s(samples),
        reinterpret_cast<std::uint8_t*>(mxGetLogicals(valid)), n, &n_read, &start_uutc,
        spans.data(), static_cast<std::int32_t>(spans.size()), &n_spans, nullptr, nullptr,
        nullptr);
    if (rc == MEF3IO_ERR_ARGUMENT && n_spans > static_cast<std::int32_t>(spans.size())) {
      spans.resize(static_cast<std::size_t>(n_spans));
      rc = mef3io_reader_read_raw_factors(
          r, ch.c_str(), t0, t1, mxGetInt32s(samples),
          reinterpret_cast<std::uint8_t*>(mxGetLogicals(valid)), n, &n_read, &start_uutc,
          spans.data(), static_cast<std::int32_t>(spans.size()), &n_spans, nullptr, nullptr,
          nullptr);
    }
    check(rc);
    mef3io_channel_info ci{};
    check(mef3io_reader_info(r, ch.c_str(), &ci));
    // One factor for the window when its samples share one; NaN when they
    // come from segments storing different ones; the channel's when empty.
    double uf = ci.units_conversion_factor;
    if (n_spans == 1) uf = spans[0].units_conversion_factor;
    else if (n_spans > 1) uf = std::numeric_limits<double>::quiet_NaN();
    const char* span_fields[] = {"start_index", "end_index", "units_conversion_factor"};
    mxArray* factors = mxCreateStructMatrix(static_cast<mwSize>(n_spans), 1, 3, span_fields);
    for (std::int32_t i = 0; i < n_spans; ++i) {
      // MATLAB indexing: 1-based, inclusive (samples(s.start_index:s.end_index)).
      mxSetField(factors, i, "start_index",
                 mxCreateDoubleScalar(static_cast<double>(spans[i].start_index + 1)));
      mxSetField(factors, i, "end_index",
                 mxCreateDoubleScalar(static_cast<double>(spans[i].end_index)));
      mxSetField(factors, i, "units_conversion_factor",
                 mxCreateDoubleScalar(spans[i].units_conversion_factor));
    }
    const char* fields[] = {"samples", "valid", "start_uutc", "sampling_frequency",
                            "units_conversion_factor", "units_conversion_factors"};
    plhs[0] = mxCreateStructMatrix(1, 1, 6, fields);
    mxSetField(plhs[0], 0, "samples", samples);
    mxSetField(plhs[0], 0, "valid", valid);
    mxSetField(plhs[0], 0, "start_uutc", make_int64(start_uutc));
    mxSetField(plhs[0], 0, "sampling_frequency", mxCreateDoubleScalar(ci.sampling_frequency));
    mxSetField(plhs[0], 0, "units_conversion_factor", mxCreateDoubleScalar(uf));
    mxSetField(plhs[0], 0, "units_conversion_factors", factors);
    return;
  }
  if (cmd == "reader_segments") {
    need_args(nrhs, 3, "reader_segments");
    mef3io_reader* r = get_reader(prhs[1]);
    std::string ch = get_string(prhs[2], "channel");
    // One segment table (one segment map) for all segments: a per-index
    // accessor loop rebuilt the map for every segment, O(n^2).
    std::int32_t n = 0;
    check(mef3io_reader_segment_table(r, ch.c_str(), nullptr, nullptr, nullptr, 0, &n));
    std::vector<mef3io_segment_info> infos(static_cast<std::size_t>(n));
    std::vector<mef3io_provenance> provs(static_cast<std::size_t>(n));
    std::vector<double> factors(static_cast<std::size_t>(n));
    {
      std::int32_t got = 0;
      check(mef3io_reader_segment_table(r, ch.c_str(), infos.data(), provs.data(),
                                        factors.data(), n, &got));
      n = got;  // a lenient session can only lose segments in between
    }
    const char* fields[] = {"segment",        "start_time",       "end_time",
                            "start_sample",   "number_of_samples", "number_of_blocks",
                            "path",           "provenance",       "units_conversion_factor"};
    plhs[0] = mxCreateStructMatrix(n, 1, 9, fields);
    for (std::int32_t i = 0; i < n; ++i) {
      const mef3io_segment_info& s = infos[static_cast<std::size_t>(i)];
      mxSetField(plhs[0], i, "segment", mxCreateDoubleScalar(s.segment_number));
      mxSetField(plhs[0], i, "start_time", make_int64(s.start_time));
      mxSetField(plhs[0], i, "end_time", make_int64(s.end_time));
      mxSetField(plhs[0], i, "start_sample", make_int64(s.start_sample));
      mxSetField(plhs[0], i, "number_of_samples", make_int64(s.number_of_samples));
      mxSetField(plhs[0], i, "number_of_blocks", make_int64(s.number_of_blocks));
      mxSetField(plhs[0], i, "path", make_utf8_string(s.path));
      mxSetField(plhs[0], i, "provenance", provenance_struct(provs[static_cast<std::size_t>(i)]));
      mxSetField(plhs[0], i, "units_conversion_factor",
                 mxCreateDoubleScalar(factors[static_cast<std::size_t>(i)]));
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
    // reader_records(h, channel, [segment], [level]); [] = not given.
    need_args(nrhs, 3, "reader_records");
    mef3io_reader* r = get_reader(prhs[1]);
    OptChannel ch = get_opt_channel(prhs[2]);
    std::int32_t segment = -1;
    if (nrhs >= 4 && !mxIsEmpty(prhs[3])) {
      const double s = get_scalar(prhs[3], "segment");
      // Range-check BEFORE converting: a double -> int conversion of NaN, Inf
      // or anything outside int32 is undefined behaviour, not an error.
      if (!std::isfinite(s) || s < 0 || s > static_cast<double>(INT32_MAX) ||
          s != std::floor(s))
        fail("segment must be a non-negative integer");
      segment = static_cast<std::int32_t>(s);
    }
    std::int32_t level = MEF3IO_RECORD_LEVEL_DEFAULT;
    if (nrhs >= 5 && !mxIsEmpty(prhs[4])) {
      const std::string l = get_string(prhs[4], "level");
      if (l == "session") level = MEF3IO_RECORD_LEVEL_SESSION;
      else if (l == "channel") level = MEF3IO_RECORD_LEVEL_CHANNEL;
      else if (l == "segment") level = MEF3IO_RECORD_LEVEL_SEGMENT;
      else if (l == "all") level = MEF3IO_RECORD_LEVEL_ALL;
      else fail("level must be 'session', 'channel', 'segment' or 'all'; got '%s'", l.c_str());
    }
    mef3io_record_list* list = nullptr;
    check(mef3io_reader_read_records(r, ch.c_str(), segment, level, &list));

    // Copy everything out and free the list BEFORE building mxArrays, so no
    // error path can leak it. Indices below are in range by construction.
    struct Field {
      std::string name;
      int kind = MEF3IO_FIELD_ABSENT;
      std::int64_t i = 0;
      double d = 0;
      std::string s;
      std::vector<std::int32_t> arr;
      std::vector<mef3io_seizure_channel> chans;
    };
    struct Rec {
      mef3io_record_header h{};
      std::vector<Field> fields;
    };
    std::vector<Rec> recs;
    std::int32_t n = 0;
    mef3io_record_list_size(list, &n);
    for (std::int32_t i = 0; i < n; ++i) {
      Rec rec;
      mef3io_record_list_header(list, i, &rec.h);
      for (std::int32_t k = 0; k < rec.h.n_fields; ++k) {
        mef3io_record_field f{};
        mef3io_record_list_field(list, i, k, &f);
        Field out;
        out.name = f.name;
        out.kind = f.kind;
        out.i = f.int_value;
        out.d = f.double_value;
        if (f.kind == MEF3IO_FIELD_STRING) out.s = f.string_value;
        if (f.kind == MEF3IO_FIELD_INT32_ARRAY)
          out.arr.assign(f.int32_values, f.int32_values + f.count);
        for (std::int64_t c = 0; f.kind == MEF3IO_FIELD_SEIZURE_CHANNELS && c < f.count; ++c) {
          mef3io_seizure_channel sc{};
          mef3io_record_list_seizure_channel(list, i, k, c, &sc);
          out.chans.push_back(sc);
        }
        rec.fields.push_back(std::move(out));
      }
      recs.push_back(std::move(rec));
    }
    mef3io_record_list_free(list);

    // A struct array needs one field set: the common fields (text/duration
    // kept for every record, as before), then every body field any record
    // carries, in first-seen order. A field a record does not have is [].
    std::vector<std::string> names = {"type",          "time",          "level", "channel",
                                      "segment",       "version_major", "version_minor",
                                      "text",          "duration"};
    for (const auto& rec : recs)
      for (const auto& f : rec.fields)
        if (std::find(names.begin(), names.end(), f.name) == names.end()) names.push_back(f.name);
    std::vector<const char*> cnames;
    for (const auto& s : names) cnames.push_back(s.c_str());
    plhs[0] = mxCreateStructMatrix(static_cast<mwSize>(recs.size()), 1,
                                   static_cast<int>(cnames.size()), cnames.data());
    auto empty = [] { return mxCreateDoubleMatrix(0, 0, mxREAL); };
    static const char* const level_names[] = {"session", "channel", "segment"};
    for (std::size_t i = 0; i < recs.size(); ++i) {
      const auto& h = recs[i].h;
      const auto idx = static_cast<mwIndex>(i);
      mxSetField(plhs[0], idx, "type", make_utf8_string(h.type));
      mxSetField(plhs[0], idx, "time", make_int64(h.time));
      mxSetField(plhs[0], idx, "level",
                 make_utf8_string(h.level >= 0 && h.level <= 2 ? level_names[h.level] : "session"));
      mxSetField(plhs[0], idx, "channel", make_utf8_string(h.channel));
      mxSetField(plhs[0], idx, "segment",
                 h.segment >= 0 ? mxCreateDoubleScalar(h.segment) : empty());
      mxSetField(plhs[0], idx, "version_major", mxCreateDoubleScalar(h.version_major));
      mxSetField(plhs[0], idx, "version_minor", mxCreateDoubleScalar(h.version_minor));
      for (const auto& f : recs[i].fields) {
        mxArray* v = nullptr;
        switch (f.kind) {
          case MEF3IO_FIELD_INT64: v = make_int64(f.i); break;
          case MEF3IO_FIELD_DOUBLE: v = mxCreateDoubleScalar(f.d); break;
          case MEF3IO_FIELD_STRING: v = make_utf8_string(f.s); break;
          case MEF3IO_FIELD_INT32_ARRAY: {
            v = mxCreateNumericMatrix(static_cast<mwSize>(f.arr.size()), 1, mxINT32_CLASS, mxREAL);
            if (!f.arr.empty())
              std::memcpy(mxGetInt32s(v), f.arr.data(), f.arr.size() * sizeof(std::int32_t));
            break;
          }
          case MEF3IO_FIELD_SEIZURE_CHANNELS: {
            const char* cf[] = {"name", "onset", "offset"};
            v = mxCreateStructMatrix(static_cast<mwSize>(f.chans.size()), 1, 3, cf);
            for (std::size_t c = 0; c < f.chans.size(); ++c) {
              const auto ci = static_cast<mwIndex>(c);
              mxSetField(v, ci, "name", make_utf8_string(f.chans[c].name));
              mxSetField(v, ci, "onset", make_int64(f.chans[c].onset));
              mxSetField(v, ci, "offset", make_int64(f.chans[c].offset));
            }
            break;
          }
          default: v = empty(); break;
        }
        mxSetField(plhs[0], idx, f.name.c_str(), v);
      }
    }
    return;
  }

  // ---- writer ----------------------------------------------------------
  if (cmd == "writer_open") {
    need_args(nrhs, 5, "writer_open");
    std::string path = get_string(prhs[1], "path");
    int overwrite = get_int(prhs[2], "overwrite");
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
  if (cmd == "writer_set_metadata") {
    // h, struct with any of the metadata fields
    need_args(nrhs, 3, "writer_set_metadata");
    mef3io_writer* w = get_writer(prhs[1]);
    const mxArray* s = prhs[2];
    if (!mxIsStruct(s)) fail("metadata must be a struct");
    mef3io_metadata md{};
    md.acquisition_channel_number = 1;
    md.low_frequency_filter = -1.0;
    md.high_frequency_filter = -1.0;
    md.notch_filter = -1.0;
    md.line_frequency = -1.0;
    auto set_str = [&](const char* key, char* dst, size_t cap) {
      const mxArray* f = mxGetField(s, 0, key);
      if (f && !mxIsEmpty(f)) {
        std::string v = get_string(f, key);
        std::strncpy(dst, v.c_str(), cap - 1);
        dst[cap - 1] = '\0';
      }
    };
    auto set_num = [&](const char* key, double& dst) {
      const mxArray* f = mxGetField(s, 0, key);
      if (f && !mxIsEmpty(f)) dst = get_scalar(f, key);
    };
    set_str("session_description", md.session_description, sizeof md.session_description);
    set_str("channel_description", md.channel_description, sizeof md.channel_description);
    set_str("reference_description", md.reference_description, sizeof md.reference_description);
    set_str("subject_name_1", md.subject_name_1, sizeof md.subject_name_1);
    set_str("subject_name_2", md.subject_name_2, sizeof md.subject_name_2);
    set_str("subject_id", md.subject_id, sizeof md.subject_id);
    set_str("recording_location", md.recording_location, sizeof md.recording_location);
    set_num("low_frequency_filter", md.low_frequency_filter);
    set_num("high_frequency_filter", md.high_frequency_filter);
    set_num("notch_filter", md.notch_filter);
    set_num("line_frequency", md.line_frequency);
    const mxArray* fa = mxGetField(s, 0, "acquisition_channel_number");
    if (fa && !mxIsEmpty(fa)) md.acquisition_channel_number = get_int64(fa, "acq num");
    const mxArray* fg = mxGetField(s, 0, "gmt_offset");
    if (fg && !mxIsEmpty(fg)) md.gmt_offset = static_cast<std::int32_t>(get_int(fg, "gmt_offset"));
    check(mef3io_writer_set_metadata(w, &md));
    return;
  }
  if (cmd == "check_block_length") {  // before writer_open (Writer.m), #83
    need_args(nrhs, 2, "check_block_length");
    check(mef3io_check_block_length(get_int64(prhs[1], "block_length")));
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
                                    get_int(prhs[2], "n_threads")));
    return;
  }
  if (cmd == "writer_threads") {
    need_args(nrhs, 2, "writer_threads");
    int n = 0;
    check(mef3io_writer_threads(get_writer(prhs[1]), &n));
    plhs[0] = mxCreateDoubleScalar(static_cast<double>(n));
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
      if (!mxIsDouble(prhs[3])) fail("data must be a real double vector");
      require_dense_real(prhs[3], "data");
      std::int64_t n = static_cast<std::int64_t>(mxGetNumberOfElements(prhs[3]));
      std::int64_t start = get_time(prhs[4], "start_uutc");
      double fs = get_scalar(prhs[5], "fs");
      int precision = get_int(prhs[6], "precision");
      int new_segment = nrhs > 7 ? get_int(prhs[7], "new_segment") : 0;
      check(mef3io_writer_write_float(w, ch.c_str(), mxGetDoubles(prhs[3]), n, start, fs,
                                      precision, new_segment, &sum));
    } else {
      if (!mxIsInt32(prhs[3])) fail("data must be an int32 vector");
      require_dense_real(prhs[3], "data");
      std::int64_t n = static_cast<std::int64_t>(mxGetNumberOfElements(prhs[3]));
      const std::uint8_t* valid = nullptr;
      std::vector<std::uint8_t> valid_copy;
      if (!mxIsEmpty(prhs[4])) {
        require_dense_real(prhs[4], "valid mask");
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
      std::int64_t start = get_time(prhs[6], "start_uutc");
      double fs = get_scalar(prhs[7], "fs");
      int new_segment = nrhs > 8 ? get_int(prhs[8], "new_segment") : 0;
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
  if (cmd == "writer_unreadable_metadata") {
    need_args(nrhs, 2, "writer_unreadable_metadata");
    mef3io_writer* w = get_writer(prhs[1]);
    std::int32_t n = 0;
    check(mef3io_writer_n_unreadable_metadata(w, &n));
    plhs[0] = mxCreateCellMatrix(static_cast<mwSize>(n), 1);
    for (std::int32_t i = 0; i < n; ++i) {
      char buf[4096] = {0};
      check(mef3io_writer_unreadable_metadata(w, i, buf, sizeof buf));
      mxSetCell(plhs[0], static_cast<mwIndex>(i), make_utf8_string(buf));
    }
    return;
  }
  if (cmd == "writer_legacy_password_segments") {
    need_args(nrhs, 2, "writer_legacy_password_segments");
    mef3io_writer* w = get_writer(prhs[1]);
    std::int32_t n = 0;
    check(mef3io_writer_n_legacy_password_segments(w, &n));
    plhs[0] = mxCreateCellMatrix(static_cast<mwSize>(n), 1);
    for (std::int32_t i = 0; i < n; ++i) {
      char buf[4096] = {0};
      check(mef3io_writer_legacy_password_segment(w, i, buf, sizeof buf));
      mxSetCell(plhs[0], static_cast<mwIndex>(i), make_utf8_string(buf));
    }
    return;
  }
  if (cmd == "writer_write_records") {
    need_args(nrhs, 4, "writer_write_records");
    mef3io_writer* w = get_writer(prhs[1]);
    OptChannel ch = get_opt_channel(prhs[2]);
    const mxArray* recs = prhs[3];
    if (!mxIsStruct(recs)) fail("records must be a struct array");
    // Owned by a guard: any argument error below throws, and the list must
    // not leak on the way out.
    std::unique_ptr<mef3io_records, decltype(&mef3io_records_free)> list(
        mef3io_records_create(), &mef3io_records_free);
    const size_t n = mxGetNumberOfElements(recs);
    for (size_t i = 0; i < n; ++i) {
      const mxArray* f_time = mxGetField(recs, i, "time");
      if (!f_time) fail("each record needs a 'time' field");
      std::int64_t time = get_time(f_time, "record time");
      const mxArray* f_type = mxGetField(recs, i, "type");
      std::string type = f_type && !mxIsEmpty(f_type) ? get_string(f_type, "type") : "Note";
      const mxArray* f_text = mxGetField(recs, i, "text");
      std::string text = f_text && !mxIsEmpty(f_text) ? get_string(f_text, "text") : "";
      const mxArray* f_dur = mxGetField(recs, i, "duration");
      std::int64_t dur = f_dur && !mxIsEmpty(f_dur) ? get_time(f_dur, "duration") : -1;
      check(mef3io_records_add(list.get(), type.c_str(), time, text.c_str(), dur));
    }
    int code = mef3io_writer_write_records(w, ch.c_str(), list.get());
    check(code);
    return;
  }

  fail("unknown command: %s", cmd.c_str());
}

// Message of the error dispatch() threw. Static, so that by the time
// mexErrMsgIdAndTxt runs no C++ object with a destructor is alive in this
// MEX's frames (see MexError).
static char g_error_message[4096];

static bool run(int nlhs, mxArray* plhs[], int nrhs, const mxArray* prhs[]) {
  try {
    dispatch(nlhs, plhs, nrhs, prhs);
    return true;
  } catch (const std::exception& e) {
    std::snprintf(g_error_message, sizeof g_error_message, "%s", e.what());
  } catch (...) {
    std::snprintf(g_error_message, sizeof g_error_message, "%s", "unknown error");
  }
  return false;
}

void mexFunction(int nlhs, mxArray* plhs[], int nrhs, const mxArray* prhs[]) {
  if (!run(nlhs, plhs, nrhs, prhs)) mexErrMsgIdAndTxt("mef3io:error", "%s", g_error_message);
}
