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
  MEF3IO_ERR_CANCELLED = 7,  /* stopped through a mef3io_cancel_token; nothing left behind */
  MEF3IO_ERR_OTHER = 99
} mef3io_error;

typedef struct mef3io_reader mef3io_reader;
typedef struct mef3io_writer mef3io_writer;
typedef struct mef3io_records mef3io_records; /* record list builder (write side) */

/* ----- cancellation and progress ----------------------------------------
 *
 * The *_ex functions below take two optional hooks (pass NULL for either):
 *
 *  - a cancel token. mef3io_cancel_token_cancel() may be called from ANY
 *    thread while the operation runs on another; the operation notices
 *    between units of work (a decoded block, a 1 MB copy chunk, a segment)
 *    and returns MEF3IO_ERR_CANCELLED. A cancelled call writes no output and
 *    leaves no partial file. Tokens are one-shot (no reset).
 *  - a progress callback `progress(done, total, user_data)`, both in bytes.
 *    Called first with (0, total), then at a bounded rate (about every 1% of
 *    total or 250 ms), with `done` never decreasing, and finally with
 *    (total, total) exactly once when the cancellable work completes (a
 *    short commit step after it — a rename, a header patch — can still
 *    fail with an I/O error). Never called
 *    concurrently with itself, but possibly from a worker thread rather than
 *    the caller's; it must not call back into the same handle.
 */
typedef struct mef3io_cancel_token mef3io_cancel_token;
typedef void (*mef3io_progress_fn)(uint64_t done, uint64_t total, void* user_data);

/* NULL on allocation failure. Free with mef3io_cancel_token_free (NULL ok)
 * only after every call it was passed to has returned. */
mef3io_cancel_token* mef3io_cancel_token_new(void);
void mef3io_cancel_token_free(mef3io_cancel_token* token);
/* Request cancellation. Thread-safe, idempotent; NULL is ignored. */
void mef3io_cancel_token_cancel(mef3io_cancel_token* token);
/* 1 once cancelled, else 0 (also 0 for NULL). */
int mef3io_cancel_token_is_cancelled(const mef3io_cancel_token* token);

const char* mef3io_version(void);
/* Message for the last failing call on this thread ("" if none). */
const char* mef3io_last_error(void);

/* ----- reader ------------------------------------------------------------ */

/* `mefd_path` is a .mefd session directory or an uncompressed tar archive of
 * one (e.g. name.mefd.tar, from mef3io_archive_session) — tar sessions are
 * read in place, without extraction.
 *
 * `n_threads` is how many threads the reader may use for everything it does
 * (opening, file access, decoding, decrypting): 0 = automatic, 1 = no extra
 * threads. Results never depend on it; negative = MEF3IO_ERR_ARGUMENT
 * (since 2.0; before, negative meant automatic). See
 * mef3io_reader_set_threads. */
int mef3io_reader_open(const char* mefd_path, const char* password, int n_threads,
                       mef3io_reader** out);
/* As above, but `strict` 0 CONTAINS a segment-level failure instead of failing
 * the open: the unreadable segment is skipped, its time span reads back as a
 * gap like any other missing data, and it is listed by mef3io_reader_problem.
 * `strict` non-zero behaves exactly like mef3io_reader_open.
 *
 * Strict is the default everywhere because a skipped segment is indistinguish-
 * able, in the returned samples, from a real recording gap. Lenient mode is for
 * salvaging the intact remainder of a damaged archive, and a caller that uses
 * it should read the problem list before trusting the data. */
int mef3io_reader_open_ex(const char* mefd_path, const char* password, int n_threads,
                          int strict, mef3io_reader** out);
void mef3io_reader_close(mef3io_reader* r);
/* Change the reader's thread count (same meaning as at open; negative =
 * MEF3IO_ERR_ARGUMENT). Takes effect on the next call; calls in flight keep
 * theirs. Safe while other threads read. */
int mef3io_reader_set_threads(mef3io_reader* r, int n_threads);
/* The thread count as set (0 = automatic). */
int mef3io_reader_threads(mef3io_reader* r, int* out);

typedef struct {
  char channel[256];      /* "" for a session-level record file */
  int32_t segment_number; /* -1 for a session- or channel-level record file */
  char segment[1024];     /* human-readable location (segment dir or record file) */
  char reason[512];       /* why it could not be read */
} mef3io_segment_problem;

/* What a lenient reader skipped. Always 0 for a reader opened strict, where
 * the same failure raises instead. Two kinds (mef3io_reader_problem_kind):
 * a SEGMENT that could not be read (its span reads back as a gap), and a
 * RECORD (.rdat) file that could not be read or placed (records only; the
 * samples are unaffected). Record-file problems are added by the records
 * calls, so the count can grow after the open. */
#define MEF3IO_PROBLEM_SEGMENT 0
#define MEF3IO_PROBLEM_RECORDS 1
int mef3io_reader_n_problems(mef3io_reader* r, int32_t* out);
int mef3io_reader_problem(mef3io_reader* r, int32_t index, mef3io_segment_problem* out);
/* MEF3IO_PROBLEM_SEGMENT or MEF3IO_PROBLEM_RECORDS for problem `index`. (A
 * separate call so mef3io_segment_problem keeps its size and layout.) */
int mef3io_reader_problem_kind(mef3io_reader* r, int32_t index, int32_t* out);

/* Number of channels the password opened only through the password-to-key
 * mapping mef3io used up to 1.2.0 (issue #84): the session was encrypted by an
 * older mef3io with a non-ASCII password, and meflib/pymef cannot open it with
 * that password. 0 for everything else. Bindings surface it as a warning. */
int mef3io_reader_n_legacy_password_channels(mef3io_reader* r, int32_t* out);
/* The same for record (.rdat) files read so far; grows as records are read. */
int mef3io_reader_n_legacy_password_record_files(mef3io_reader* r, int32_t* out);

/* Why `password` will not behave as expected in other MEF readers (more than
 * 15 characters, more than 15 UTF-8 bytes, or a character above U+00FF),
 * copied into `buf`; an empty
 * string when it is fine. Writers surface it as a warning; they never refuse. */
int mef3io_password_interop_issue(const char* password, char* buf, size_t buf_bytes);

int mef3io_reader_n_channels(mef3io_reader* r, int32_t* out);
int mef3io_reader_channel_name(mef3io_reader* r, int32_t index, char* buf, size_t buf_bytes);

typedef struct {
  double sampling_frequency;
  /* The factor every segment stores; NaN when the channel's segments store
   * different ones (mef3io_reader_segment_units_conversion_factor gives each
   * segment's; mef3io_reader_read scales each sample by its own segment's). */
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
/* Float64 read: gaps are NaN, each value scaled by the conversion factor of
 * the segment it comes from (segments may store different ones).
 * A segment whose factor is not set (0, MEF's "no entry") reads as its raw
 * counts (factor 1), without a warning here. MEF3IO_ERR_FORMAT when a sample
 * in the window comes from a segment whose factor is NaN or infinite; the raw
 * reads still return its counts.
 * `buf` must hold at least the value from mef3io_reader_read_size. */
int mef3io_reader_read(mef3io_reader* r, const char* channel, int64_t t0, int64_t t1,
                       double* buf, int64_t buf_len, int64_t* out_n);
/* Raw read: int32 counts + validity mask (0 in gaps). Same sizing rule.
 * The counts of a channel whose segments store different conversion factors
 * mean different things: use mef3io_reader_read_raw_factors to learn which
 * factor applies where. */
int mef3io_reader_read_raw(mef3io_reader* r, const char* channel, int64_t t0, int64_t t1,
                           int32_t* samples, uint8_t* valid, int64_t buf_len, int64_t* out_n,
                           int64_t* out_start_uutc);
/* The same reads, cancellable and with progress (compressed bytes decoded).
 * On MEF3IO_ERR_CANCELLED the buffers are left untouched. */
int mef3io_reader_read_ex(mef3io_reader* r, const char* channel, int64_t t0, int64_t t1,
                          double* buf, int64_t buf_len, int64_t* out_n,
                          const mef3io_cancel_token* cancel, mef3io_progress_fn progress,
                          void* user_data);
int mef3io_reader_read_raw_ex(mef3io_reader* r, const char* channel, int64_t t0, int64_t t1,
                              int32_t* samples, uint8_t* valid, int64_t buf_len, int64_t* out_n,
                              int64_t* out_start_uutc, const mef3io_cancel_token* cancel,
                              mef3io_progress_fn progress, void* user_data);

/* Output samples [start_index, end_index) (0-based, half-open) whose valid
 * samples come from segments storing `units_conversion_factor` (as stored:
 * 0 = not set, read by mef3io_reader_read as factor 1). */
typedef struct {
  int64_t start_index;
  int64_t end_index;
  double units_conversion_factor;
} mef3io_factor_span;

/* mef3io_reader_read_raw_ex plus where each conversion factor applies: units
 * = samples[i] * the factor of the span holding i. Spans are sorted and
 * disjoint, one for an ordinary window and none when nothing is valid; each
 * runs from its first valid sample to just past its last (Reader::read_raw's
 * factor_spans). `spans` holds `spans_len` entries; `*out_n_spans` is set to
 * the number needed even when that is too many (the call then fails with
 * MEF3IO_ERR_ARGUMENT, "buffer too small", and leaves the buffers untouched). */
int mef3io_reader_read_raw_factors(mef3io_reader* r, const char* channel, int64_t t0,
                                   int64_t t1, int32_t* samples, uint8_t* valid,
                                   int64_t buf_len, int64_t* out_n, int64_t* out_start_uutc,
                                   mef3io_factor_span* spans, int32_t spans_len,
                                   int32_t* out_n_spans, const mef3io_cancel_token* cancel,
                                   mef3io_progress_fn progress, void* user_data);

typedef struct {
  int32_t segment_number;
  int64_t start_time;
  int64_t end_time;
  int64_t start_sample;
  int64_t number_of_samples;
  int64_t number_of_blocks;
  char path[1024];  /* .segd dir; "<archive>::<member>" for tar sessions */
} mef3io_segment_info;

int mef3io_reader_n_segments(mef3io_reader* r, const char* channel, int32_t* out);
/* Provenance of segment `index` (its .tmet's universal-header discretionary
 * region; see docs/mef3_format.md "Provenance region"). Versions and
 * operations only, no times. `present` is 0 for a file from another writer or
 * from mef3io before 1.2, and every other field is then zero/empty. Separate
 * from mef3io_segment_info so that struct keeps its size and layout. */
typedef struct {
  int32_t present;
  int32_t layout_version;
  int32_t last_operation;       /* code; name via mef3io_operation_name */
  uint32_t operations_mask;     /* bit n = operation code n ever applied */
  uint32_t modification_count;  /* operations after creation; saturates */
  char created_by[24];          /* version string, "" if unknown */
  char last_modified_by[24];
} mef3io_provenance;

int mef3io_reader_segment_provenance(mef3io_reader* r, const char* channel, int32_t index,
                                     mef3io_provenance* out);
/* "create", "append", "header-repair", "recovery", "metadata-update",
 * "segment-repaired", "unset";
 * NULL for a code this library does not know (a newer writer's). */
const char* mef3io_operation_name(int32_t code);
int mef3io_reader_segment(mef3io_reader* r, const char* channel, int32_t index,
                          mef3io_segment_info* out);
/* Segment `index`'s section-2 units_conversion_factor, as stored (0 = MEF's
 * "no entry"). Separate from mef3io_segment_info so that struct keeps its
 * size and layout. Each of these per-index calls builds the channel's
 * segment map, so for every segment use mef3io_reader_segment_table. */
int mef3io_reader_segment_units_conversion_factor(mef3io_reader* r, const char* channel,
                                                  int32_t index, double* out);
/* Every segment of `channel` in one call (one segment map): `infos`,
 * `provenances` and `factors` (units_conversion_factor) each hold `len`
 * entries, or are NULL to skip that column. `*out_n` is set to the number of
 * segments even when `len` is too small (the call then fails with
 * MEF3IO_ERR_ARGUMENT, "buffer too small"); all three NULL with len 0 just
 * counts. */
int mef3io_reader_segment_table(mef3io_reader* r, const char* channel,
                                mef3io_segment_info* infos, mef3io_provenance* provenances,
                                double* factors, int32_t len, int32_t* out_n);

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

/* channel == NULL -> session-level records; otherwise the channel's channel-
 * AND segment-level records (the default levels of mef3io_reader_read_records).
 * Kept for compatibility: each call re-reads the record files, and only the
 * fields above are exposed. New code should use mef3io_reader_read_records. */
int mef3io_reader_n_records(mef3io_reader* r, const char* channel, int32_t* out);
int mef3io_reader_record(mef3io_reader* r, const char* channel, int32_t index,
                         mef3io_record_info* out);

/* ----- records: every level, every body field --------------------------- */

/* Record levels. MEF 3.0 keeps record files at the session (.mefd), channel
 * (.timd) and segment (.segd) level. */
#define MEF3IO_RECORD_LEVEL_DEFAULT (-1) /* query only: the scope's natural levels */
#define MEF3IO_RECORD_LEVEL_SESSION 0
#define MEF3IO_RECORD_LEVEL_CHANNEL 1
#define MEF3IO_RECORD_LEVEL_SEGMENT 2
#define MEF3IO_RECORD_LEVEL_ALL 3        /* query only */

typedef struct mef3io_record_list mef3io_record_list; /* a snapshot, read once */

/* Read records into a list the caller frees with mef3io_record_list_free.
 *  channel  NULL -> session scope; else that channel.
 *  segment  < 0 -> none; else that segment number (requires channel).
 *  level    MEF3IO_RECORD_LEVEL_*. DEFAULT: session-level for the session
 *           scope, channel+segment level for a channel, the segment's for a
 *           segment. ALL with no channel: every record in the session.
 * A level outside the scope, a segment without a channel, or an unknown
 * channel/segment -> MEF3IO_ERR_ARGUMENT. Segments of the channel / session
 * disagreeing on the recording-time offset -> MEF3IO_ERR_FORMAT. */
int mef3io_reader_read_records(mef3io_reader* r, const char* channel, int32_t segment,
                               int32_t level, mef3io_record_list** out);
void mef3io_record_list_free(mef3io_record_list* list);
int mef3io_record_list_size(const mef3io_record_list* list, int32_t* out);

typedef struct {
  char type[8];
  int64_t time;          /* absolute uUTC */
  int32_t level;         /* MEF3IO_RECORD_LEVEL_SESSION / _CHANNEL / _SEGMENT */
  int32_t segment;       /* segment number at segment level, else -1 */
  char channel[256];     /* "" at session level */
  int32_t version_major;
  int32_t version_minor;
  int32_t n_fields;      /* body fields; same names/order for every record of a type */
} mef3io_record_header;

int mef3io_record_list_header(const mef3io_record_list* list, int32_t index,
                              mef3io_record_header* out);

/* Kind of a body field's value. ABSENT: the record does not carry it (body
 * too short, or a version meflib does not decode). */
#define MEF3IO_FIELD_ABSENT 0
#define MEF3IO_FIELD_INT64 1
#define MEF3IO_FIELD_DOUBLE 2
#define MEF3IO_FIELD_STRING 3
#define MEF3IO_FIELD_INT32_ARRAY 4      /* LNTP "template" */
#define MEF3IO_FIELD_SEIZURE_CHANNELS 5 /* Seiz "channels"; see below */

typedef struct {
  char name[32];
  int32_t kind;                 /* MEF3IO_FIELD_* */
  int64_t int_value;            /* INT64 (times are absolute uUTC) */
  double double_value;          /* DOUBLE */
  const char* string_value;     /* STRING; owned by the list, valid until it is freed */
  const int32_t* int32_values;  /* INT32_ARRAY; owned by the list */
  int64_t count;                /* INT32_ARRAY elements / SEIZURE_CHANNELS entries */
} mef3io_record_field;

int mef3io_record_list_field(const mef3io_record_list* list, int32_t index, int32_t field,
                             mef3io_record_field* out);

typedef struct {
  char name[256];
  int64_t onset;   /* absolute uUTC; -1 = no entry (mefrec.h) */
  int64_t offset;  /* absolute uUTC; -1 = no entry */
} mef3io_seizure_channel;

/* Entry `entry` of field `field` (a SEIZURE_CHANNELS field) of record `index`. */
int mef3io_record_list_seizure_channel(const mef3io_record_list* list, int32_t index,
                                       int32_t field, int64_t entry,
                                       mef3io_seizure_channel* out);

/* ----- archive ----------------------------------------------------------- */

/* Pack a session directory into a single uncompressed tar archive (readable
 * directly by mef3io_reader_open; the directory is left untouched).
 * tar_path NULL or "" derives "<session_dir>.tar". The resulting archive path
 * is copied into out_path (may be NULL when not wanted). */
int mef3io_archive_session(const char* session_dir, const char* tar_path, int overwrite,
                           char* out_path, size_t out_path_bytes);

/* Inverse: unpack a session archive back into a directory. dest_dir NULL or
 * "" strips the ".tar" suffix ("name.mefd.tar" -> "name.mefd"). Refuses an
 * existing target unless overwrite. The directory path lands in out_path. */
/* ----- recovery ---------------------------------------------------------- */

/* Make each segment's block index and data agree again after an interrupted
 * write. `apply` 0 reports what it WOULD do and writes nothing. `backup` saves
 * what changes (never the whole .tdat, which may be tens of gigabytes) under
 * "<session>.recover-backup". A human-readable summary lands in `out_summary`.
 * `out_segments` receives the number of segments needing work (may be NULL).
 *
 * Unlike a declaration repair this may change the block index, so it is a dry
 * run by default. Re-derive the declarations afterwards. */
int mef3io_recover_session(const char* mefd_path, int apply, int backup, const char* password,
                           char* out_summary, size_t out_summary_bytes, int64_t* out_segments);

int mef3io_extract_session(const char* tar_path, const char* dest_dir, int overwrite,
                           char* out_path, size_t out_path_bytes);

/* Cancellable / progress-reporting versions (see "cancellation and progress").
 * archive/extract: build in a temporary file/directory and rename into place
 * only when complete, so a cancel leaves nothing (and an existing target is
 * untouched). recover: every segment is SCANNED before anything is written; a
 * cancel during the scan writes nothing at all, and the short write phase that
 * follows is not interruptible (a cancel then is ignored). */
int mef3io_archive_session_ex(const char* session_dir, const char* tar_path, int overwrite,
                              char* out_path, size_t out_path_bytes,
                              const mef3io_cancel_token* cancel, mef3io_progress_fn progress,
                              void* user_data);
int mef3io_extract_session_ex(const char* tar_path, const char* dest_dir, int overwrite,
                              char* out_path, size_t out_path_bytes,
                              const mef3io_cancel_token* cancel, mef3io_progress_fn progress,
                              void* user_data);
int mef3io_recover_session_ex(const char* mefd_path, int apply, int backup, const char* password,
                              char* out_summary, size_t out_summary_bytes, int64_t* out_segments,
                              const mef3io_cancel_token* cancel, mef3io_progress_fn progress,
                              void* user_data);

/* ----- writer ------------------------------------------------------------ */

/* The writer's argument rules (#83), for a caller that must refuse BEFORE
 * mef3io_writer_open — which removes an existing session when overwrite is
 * set. MEF3IO_ERR_ARGUMENT (message in mef3io_last_error) when the block
 * length exceeds MEF3IO_MAX_BLOCK_SAMPLES (<= 0 = derive from the rate is
 * fine), or fs is not finite > 0, or ufact is 0 / NaN / infinite (negative
 * is a polarity inversion and allowed). Every write call checks the same. */
#define MEF3IO_MAX_BLOCK_SAMPLES 858993396LL
int mef3io_check_block_length(int64_t samples_per_block);
int mef3io_check_signal_parameters(double sampling_frequency, double units_conversion_factor);

/* Rejects tar archives: tar sessions are read-only. */
int mef3io_writer_open(const char* mefd_path, int overwrite, const char* password_1,
                       const char* password_2, mef3io_writer** out);
void mef3io_writer_close(mef3io_writer* w);

int mef3io_writer_set_units(mef3io_writer* w, const char* units);
int mef3io_writer_set_metadata(mef3io_writer* w, const mef3io_metadata* md);
int mef3io_writer_set_block_length(mef3io_writer* w, int64_t samples_per_block);
/* How many threads the writer may use (encoding): 0 = automatic, 1 = no extra
 * threads, negative = MEF3IO_ERR_ARGUMENT (since 2.0); output is
 * byte-identical whatever the count. Next write on. */
int mef3io_writer_set_threads(mef3io_writer* w, int n_threads);
/* The thread count as set (0 = automatic). */
int mef3io_writer_threads(mef3io_writer* w, int* out);
/* Return only when everything this writer has written so far is on disk, in
 * ONE combined wait (since 2.0). mef3io_writer_close does the same but cannot
 * report a failure: call this first to see one. */
int mef3io_writer_flush(mef3io_writer* w);
/* Opt-in (default 0): non-zero makes every write return only when its own
 * data is on disk (since 2.0). */
int mef3io_writer_set_wait_for_disk(mef3io_writer* w, int on);

/* Segment metadata the writer could not use: .tmet files it could not read
 * while choosing a recording-time offset, and empty segment directories it
 * ignored. Each entry is "path: reason". In a session at offset 0 such a file
 * is skipped so it does not block unrelated writes; with a non-zero offset, or
 * nothing else readable, the write is refused instead. */
int mef3io_writer_n_unreadable_metadata(mef3io_writer* w, int32_t* out);
int mef3io_writer_unreadable_metadata(mef3io_writer* w, int32_t index, char* buf,
                                      size_t buf_bytes);

/* Segments (.tmet paths) this writer appended to whose password matched only
 * through the password mapping of mef3io 1.2.0 and earlier (issue #84). They keep their
 * existing keys; new segments use meflib's mapping. Grows over the writer's
 * life; deduplicated. */
int mef3io_writer_n_legacy_password_segments(mef3io_writer* w, int32_t* out);
int mef3io_writer_legacy_password_segment(mef3io_writer* w, int32_t index, char* buf,
                                          size_t buf_bytes);

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
