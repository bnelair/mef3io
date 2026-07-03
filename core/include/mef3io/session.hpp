// mef3io — session/channel/segment tree and low-level indexed reads.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "mef3io/headers.hpp"
#include "mef3io/metadata.hpp"
#include "mef3io/records.hpp"
#include "mef3io/types.hpp"

namespace mef3io {

// A contiguous run of decoded samples starting at an absolute uUTC time.
// Discontinuities produce separate runs.
struct DataRun {
  si8 start_uutc = 0;      // absolute (recording_time_offset applied)
  si8 start_sample = 0;    // channel-wide sample index of the first sample
  std::vector<si4> samples;
};

struct SegmentReader {
  std::string tmet_path;
  std::string tidx_path;
  std::string tdat_path;
  int segment_number = 0;
  std::optional<TimeSeriesMetadata> metadata;      // loaded lazily
  std::vector<ui1> tidx_bytes;                      // loaded lazily (whole file)
};

// A decode job: where a block lives (index into an owned buffer) and where its
// samples land in absolute time. Used for parallel decoding.
struct BlockJob {
  std::size_t buffer_index = 0;
  std::size_t offset = 0;
  std::size_t block_bytes = 0;
  si8 start_uutc = 0;
  ui4 number_of_samples = 0;
  crypto::AccessKeys keys;
};

struct BlockJobs {
  std::vector<std::vector<ui1>> buffers;  // owns the .tdat images referenced by jobs
  std::vector<BlockJob> jobs;
  sf8 sampling_frequency = 0.0;
  sf8 units_conversion_factor = 1.0;
};

// One RED block's index entry with absolute (user) times resolved.
struct BlockIndexEntry {
  si8 start_uutc = 0;
  si8 start_sample = 0;
  ui4 number_of_samples = 0;
  si4 maximum_sample_value = 0;
  si4 minimum_sample_value = 0;
  bool discontinuity = false;
};

struct ChannelInfo {
  std::string name;
  sf8 sampling_frequency = 0.0;
  sf8 units_conversion_factor = 1.0;
  std::string units_description;
  si8 start_time = 0;      // absolute uUTC
  si8 end_time = 0;        // absolute uUTC
  si8 number_of_samples = 0;
  si8 recording_time_offset = 0;
  int n_segments = 0;
};

class Session {
 public:
  // Discover the session tree and load per-channel basic info (lazy on data).
  Session(const std::string& mefd_path, std::string password = "");

  const std::vector<std::string>& channels() const { return channel_names_; }
  const ChannelInfo& channel_info(const std::string& name) const;

  // Decode all blocks of `channel` overlapping [t0, t1] (absolute uUTC;
  // defaults span the whole channel). Returns contiguous runs split on
  // discontinuities. Samples are the raw int32 stored values (no scaling,
  // no trimming to exact sample yet — that is the high-level Reader's job).
  std::vector<DataRun> read_runs(const std::string& channel,
                                 std::optional<si8> t0 = std::nullopt,
                                 std::optional<si8> t1 = std::nullopt);

  // All RED block index entries for a channel (across segments), with absolute
  // times resolved. Does not decode data. For viewers / seeking.
  std::vector<BlockIndexEntry> read_index(const std::string& channel);

  // Gather (without decoding) all blocks of `channel` overlapping [t0, t1],
  // loading the needed .tdat images into owned buffers. The high-level Reader
  // decodes these in parallel.
  BlockJobs collect_blocks(const std::string& channel, std::optional<si8> t0 = std::nullopt,
                           std::optional<si8> t1 = std::nullopt);

  // Read records (annotations). channel == nullopt -> session-level records;
  // otherwise the given channel's records. Empty if none.
  std::vector<Record> read_records(std::optional<std::string> channel = std::nullopt);

 private:
  struct Channel {
    ChannelInfo info;
    std::vector<SegmentReader> segments;  // sorted by segment number
  };

  void discover();
  void load_channel_basic_info(Channel& ch);
  TimeSeriesMetadata& segment_metadata(SegmentReader& seg);
  std::span<const ui1> segment_index(SegmentReader& seg);

  std::string mefd_path_;
  std::string password_;
  std::vector<std::string> channel_names_;
  std::map<std::string, Channel> channels_;
};

}  // namespace mef3io
