// Test helper: a SessionSource decorator that counts requests by kind and can
// add a fixed delay to each one, to model a network share where every request
// is a round trip (#141). Thread-safe (atomics), like the sources it wraps.
#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "mef3io/source.hpp"

namespace mef3io::test {

struct SourceCalls {
  long list_dir = 0, exists = 0, file_size = 0, read_all = 0, read_head = 0, read_range = 0;
  long reads() const { return read_all + read_head + read_range; }
  long total() const { return list_dir + exists + file_size + reads(); }
};

class CountingSource : public SessionSource {
 public:
  explicit CountingSource(std::shared_ptr<const SessionSource> inner,
                          std::chrono::microseconds delay = std::chrono::microseconds(0))
      : inner_(std::move(inner)), delay_(delay) {}

  std::vector<SourceDirEntry> list_dir(const std::string& rel) const override {
    hit(list_dir_);
    return inner_->list_dir(rel);
  }
  bool exists(const std::string& rel) const override {
    hit(exists_);
    return inner_->exists(rel);
  }
  std::uint64_t file_size(const std::string& rel) const override {
    hit(file_size_);
    return inner_->file_size(rel);
  }
  std::vector<ui1> read_all(const std::string& rel) const override {
    hit(read_all_);
    return inner_->read_all(rel);
  }
  std::vector<ui1> read_head(const std::string& rel, std::size_t max_bytes) const override {
    hit(read_head_);
    return inner_->read_head(rel, max_bytes);
  }
  std::vector<ui1> read_range(const std::string& rel, std::size_t offset,
                              std::size_t length) const override {
    hit(read_range_);
    return inner_->read_range(rel, offset, length);
  }
  // Local bookkeeping, not requests: never counted or delayed.
  std::string describe(const std::string& rel) const override { return inner_->describe(rel); }
  std::string session_name() const override { return inner_->session_name(); }

  SourceCalls calls() const {
    return {list_dir_.load(), exists_.load(), file_size_.load(),
            read_all_.load(), read_head_.load(), read_range_.load()};
  }
  void reset() const {
    for (auto* c : {&list_dir_, &exists_, &file_size_, &read_all_, &read_head_, &read_range_})
      c->store(0);
  }

 private:
  void hit(std::atomic<long>& c) const {
    ++c;
    if (delay_.count() > 0) std::this_thread::sleep_for(delay_);
  }
  std::shared_ptr<const SessionSource> inner_;
  std::chrono::microseconds delay_;
  mutable std::atomic<long> list_dir_{0}, exists_{0}, file_size_{0}, read_all_{0},
      read_head_{0}, read_range_{0};
};

}  // namespace mef3io::test
