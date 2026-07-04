/* mef3io — flat C ABI over the C++ core, for MATLAB MEX (and any FFI).
 *
 * Conventions:
 *  - Every fallible function returns MEF3IO_OK (0) or an error code; the
 *    human-readable message for the last failure on the calling thread is
 *    available via mef3io_last_error() (valid until the next failing call).
 *  - Handles are opaque; close/free functions accept NULL.
 *  - Times are uUTC (microseconds since the Unix epoch), int64.
 *  - Pass MEF3IO_TIME_UNSET for an optional t0/t1 to mean "whole channel".
 *  - Strings are UTF-8, copied into fixed-size, null-terminated fields
 *    (truncated if longer).
 */
#ifndef MEF3IO_C_API_H
#define MEF3IO_C_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MEF3IO_TIME_UNSET INT64_MIN

typedef enum {
  MEF3IO_OK = 0,
  MEF3IO_ERR_IO = 1,
  MEF3IO_ERR_FORMAT = 2,
  MEF3IO_ERR_CRC = 3,
  MEF3IO_ERR_PASSWORD = 4,
  MEF3IO_ERR_CONFLICT = 5,   /* append fs/ufact/time conflict */
  MEF3IO_ERR_ARGUMENT = 6,   /* bad argument, unknown channel, buffer too small */
  MEF3IO_ERR_OTHER = 99
} mef3io_error;

typedef struct mef3io_reader mef3io_reader;
typedef struct mef3io_writer mef3io_writer;
typedef struct mef3io_records mef3io_records; /* record list builder (write side) */

const char* mef3io_version(void);
/* Message for the last failing call on this thread ("" if none). */
const char* mef3io_last_error(void);

/* ----- reader ------------------------------------------------------------ */

int mef3io_reader_open(const char* mefd_path, const char* password, int n_threads,
                       mef3io_reader** out);
void mef3io_reader_close(mef3io_reader* r);

int mef3io_reader_n_channels(mef3io_reader* r, int32_t* out);
int mef3io_reader_channel_name(mef3io_reader* r, int32_t index, char* buf, size_t buf_bytes);

typedef struct {
  double sampling_frequency;
  double units_conversion_factor;
  int64_t start_time;
  int64_t end_time;
  int64_t number_of_samples;   /* stored samples (gaps excluded) */
  int64_t recording_time_offset;
  int32_t n_segments;
  int32_t section3_available;  /* 0 with only level-1 access */
  char units_description[128];
  /* section-2 descriptive / acquisition */
  int64_t acquisition_channel_number;
  double low_frequency_filter;   /* Hz; -1 = not recorded */
  double high_frequency_filter;
  double notch_filter;
  double line_frequency;
  int32_t gmt_offset;            /* seconds (valid only with section3_available) */
  char session_description[512];
  char channel_description[512];
  char reference_description[512];
  /* section-3 subject (valid only with section3_available) */
  char subject_name_1[128];
  char subject_name_2[128];
  char subject_id[128];
  char recording_location[512];
} mef3io_channel_info;

int mef3io_reader_info(mef3io_reader* r, const char* channel, mef3io_channel_info* out);

/* Session-wide metadata (subject/acquisition) written to every channel. Set
 * before writing. String fields "" and numeric fields per struct defaults mean
 * "unset". Subject fields are stored in the level-2 section. */
typedef struct {
  int64_t acquisition_channel_number;  /* default 1 */
  double low_frequency_filter;         /* Hz; -1 = unset */
  double high_frequency_filter;
  double notch_filter;
  double line_frequency;
  int32_t gmt_offset;                  /* seconds */
  char session_description[512];
  char channel_description[512];
  char reference_description[512];
  char subject_name_1[128];
  char subject_name_2[128];
  char subject_id[128];
  char recording_location[512];
} mef3io_metadata;

/* Number of grid samples a read over [t0, t1) returns (no decoding). */
int mef3io_reader_read_size(mef3io_reader* r, const char* channel, int64_t t0, int64_t t1,
                            int64_t* out_n);
/* Float64 read: gaps are NaN, values scaled by the conversion factor.
 * `buf` must hold at least the value from mef3io_reader_read_size. */
int mef3io_reader_read(mef3io_reader* r, const char* channel, int64_t t0, int64_t t1,
                       double* buf, int64_t buf_len, int64_t* out_n);
/* Raw read: int32 counts + validity mask (0 in gaps). Same sizing rule. */
int mef3io_reader_read_raw(mef3io_reader* r, const char* channel, int64_t t0, int64_t t1,
                           int32_t* samples, uint8_t* valid, int64_t buf_len, int64_t* out_n,
                           int64_t* out_start_uutc);

typedef struct {
  int32_t segment_number;
  int64_t start_time;
  int64_t end_time;
  int64_t start_sample;
  int64_t number_of_samples;
  int64_t number_of_blocks;
  char path[1024];
} mef3io_segment_info;

int mef3io_reader_n_segments(mef3io_reader* r, const char* channel, int32_t* out);
int mef3io_reader_segment(mef3io_reader* r, const char* channel, int32_t index,
                          mef3io_segment_info* out);

typedef struct {
  int64_t start_uutc;
  int64_t start_sample;
  int64_t number_of_samples;
  int32_t maximum_sample_value;
  int32_t minimum_sample_value;
  int32_t discontinuity;
} mef3io_block_info;

int mef3io_reader_n_blocks(mef3io_reader* r, const char* channel, int64_t* out);
int mef3io_reader_block(mef3io_reader* r, const char* channel, int64_t index,
                        mef3io_block_info* out);

typedef struct {
  char type[8];
  int64_t time;
  int64_t duration;      /* -1 when absent */
  char text[1024];       /* truncated if longer */
} mef3io_record_info;

/* channel == NULL -> session-level records. */
int mef3io_reader_n_records(mef3io_reader* r, const char* channel, int32_t* out);
int mef3io_reader_record(mef3io_reader* r, const char* channel, int32_t index,
                         mef3io_record_info* out);

/* ----- writer ------------------------------------------------------------ */

int mef3io_writer_open(const char* mefd_path, int overwrite, const char* password_1,
                       const char* password_2, mef3io_writer** out);
void mef3io_writer_close(mef3io_writer* w);

int mef3io_writer_set_units(mef3io_writer* w, const char* units);
int mef3io_writer_set_metadata(mef3io_writer* w, const mef3io_metadata* md);
int mef3io_writer_set_block_length(mef3io_writer* w, int64_t samples_per_block);
int mef3io_writer_set_threads(mef3io_writer* w, int n_threads);

typedef struct {
  int64_t samples_written;
  int64_t blocks;
  int64_t gaps_skipped;
  int32_t segment;
} mef3io_write_summary;

/* NaN = discontinuity gap; precision < 0 -> inferred (or reused on append). */
int mef3io_writer_write_float(mef3io_writer* w, const char* channel, const double* data,
                              int64_t n, int64_t start_uutc, double fs, int precision,
                              int new_segment, mef3io_write_summary* out);
/* Verbatim int32 counts + conversion factor; `valid` may be NULL (all valid). */
int mef3io_writer_write_int32(mef3io_writer* w, const char* channel, const int32_t* data,
                              const uint8_t* valid, int64_t n, double ufact, int64_t start_uutc,
                              double fs, int new_segment, mef3io_write_summary* out);

/* Records are collected into a list, then written (replacing that level). */
mef3io_records* mef3io_records_create(void);
void mef3io_records_free(mef3io_records* list);
/* duration < 0 means "no duration". type e.g. "Note", "EDFA", "SyLg". */
int mef3io_records_add(mef3io_records* list, const char* type, int64_t time, const char* text,
                       int64_t duration);
/* channel == NULL -> session-level records. */
int mef3io_writer_write_records(mef3io_writer* w, const char* channel,
                                const mef3io_records* list);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MEF3IO_C_API_H */
