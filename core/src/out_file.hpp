// Internal: the file a copy writes (anonymize, transform, merge).
//
// One open per file, whatever happens to it: created EXCLUSIVELY (O_EXCL: no
// separate "does it exist" probe, and a file that appeared meanwhile is never
// overwritten), written through a large buffer (a block at a time becomes
// 1 MiB writes, not 8 KiB ones), its universal header patched through the SAME
// handle (pwrite at 0, no reopen), and flushed to stable storage through the
// same handle before it is closed (no reopen to fsync it). On a network share
// every open is a round trip, so each of these is a request saved.
//
// Durability is the copy's own rule, unchanged: every file is fdatasync'd
// (macOS F_FULLFSYNC; Windows FlushFileBuffers, which needs the GENERIC_WRITE
// handle this class holds, see durability.hpp) before the copy is verified and
// renamed. Windows: CreateFileW(CREATE_NEW) is the exclusive create, WriteFile
// the writes, an OVERLAPPED offset the header patch.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <span>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

#include "durability.hpp"
#include "mef3io/errors.hpp"
#include "mef3io/types.hpp"

namespace mef3io::detail {

/// Every output file is flushed through its own handle on every platform.
inline constexpr bool synced_by_handle = true;

class OutputFile {
 public:
  /// Creates `path`; it must not exist (IoError "output file already exists").
  explicit OutputFile(std::string path, std::size_t buffer_bytes = 1u << 20)
      : path_(std::move(path)), cap_(buffer_bytes) {
#ifndef _WIN32
    fd_ = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
    if (fd_ < 0) {
      if (errno == EEXIST) throw IoError("output file already exists: " + path_);
      throw IoError("cannot create file: " + path_ + " (" +
                    std::error_code(errno, std::generic_category()).message() + ")");
    }
#else
    const std::wstring wide = std::filesystem::path(path_).wstring();
    h_ = CreateFileW(wide.c_str(), GENERIC_WRITE,
                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_NEW,
                     FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h_ == INVALID_HANDLE_VALUE) {
      const DWORD e = GetLastError();
      if (e == ERROR_FILE_EXISTS || e == ERROR_ALREADY_EXISTS)
        throw IoError("output file already exists: " + path_);
      throw IoError("cannot create file: " + path_ + " (" +
                    std::error_code(static_cast<int>(e), std::system_category()).message() + ")");
    }
#endif
    buf_.reserve(cap_);
  }
  OutputFile(const OutputFile&) = delete;
  OutputFile& operator=(const OutputFile&) = delete;
  ~OutputFile() {
#ifndef _WIN32
    if (fd_ >= 0) ::close(fd_);
#else
    if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
#endif
  }

  /// Append bytes.
  void write(std::span<const ui1> b) {
    if (b.size() >= cap_) {  // a large piece goes straight out
      drain();
      raw_write(b.data(), b.size());
    } else {
      if (buf_.size() + b.size() > cap_) drain();
      buf_.insert(buf_.end(), b.begin(), b.end());
    }
    size_ += b.size();
  }
  std::uint64_t size() const { return size_; }

  /// Overwrite the first `b.size()` bytes (the universal header) through the same handle.
  void patch_front(std::span<const ui1> b) {
    drain();
#ifndef _WIN32
    std::size_t done = 0;
    while (done < b.size()) {
      const ssize_t n = ::pwrite(fd_, b.data() + done, b.size() - done, static_cast<off_t>(done));
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) throw IoError("write failed: " + path_);
      done += static_cast<std::size_t>(n);
    }
#else
    // An explicit offset still moves a synchronous handle's file pointer, so
    // it is put back at the end (the next append continues there).
    OVERLAPPED ov{};
    DWORD put = 0;
    if (!WriteFile(h_, b.data(), static_cast<DWORD>(b.size()), &put, &ov) || put != b.size())
      throw IoError("write failed: " + path_);
    LARGE_INTEGER end{};
    end.QuadPart = static_cast<LONGLONG>(size_);
    if (!SetFilePointerEx(h_, end, nullptr, FILE_BEGIN)) throw IoError("write failed: " + path_);
#endif
  }

  /// Flush the buffer and close. `sync`: make the file durable first, through
  /// this handle (POSIX only; Windows callers flush by path afterwards).
  void finish(bool sync) {
    drain();
#ifndef _WIN32
    if (sync) {
      if (fsync_observer) fsync_observer(path_);
#if defined(__APPLE__)
      bool ok = ::fcntl(fd_, F_FULLFSYNC) != -1;
      if (!ok && (errno == ENOTSUP || errno == EOPNOTSUPP || errno == EINVAL)) ok = ::fsync(fd_) == 0;
#else
      const bool ok = ::fdatasync(fd_) == 0;
#endif
      if (!ok)
        throw IoError("could not flush to disk, the write is not durable: " + path_ + " (fsync: " +
                      std::error_code(errno, std::generic_category()).message() + ")");
    }
    const int fd = fd_;
    fd_ = -1;
    if (::close(fd) != 0) throw IoError("close failed, data may not have reached disk: " + path_);
#else
    if (sync) {
      if (fsync_observer) fsync_observer(path_);
      if (!FlushFileBuffers(h_)) {
        const DWORD e = GetLastError();
        throw IoError("could not flush to disk, the write is not durable: " + path_ +
                      " (FlushFileBuffers: " +
                      std::error_code(static_cast<int>(e), std::system_category()).message() + ")");
      }
    }
    const HANDLE h = h_;
    h_ = INVALID_HANDLE_VALUE;
    if (!CloseHandle(h)) throw IoError("close failed, data may not have reached disk: " + path_);
#endif
  }

 private:
  void drain() {
    if (buf_.empty()) return;
    raw_write(buf_.data(), buf_.size());
    buf_.clear();
  }
  void raw_write(const ui1* p, std::size_t n) {
#ifndef _WIN32
    std::size_t done = 0;
    while (done < n) {
      const ssize_t w = ::write(fd_, p + done, n - done);
      if (w < 0 && errno == EINTR) continue;
      if (w <= 0) throw IoError("write failed (disk full?): " + path_);
      done += static_cast<std::size_t>(w);
    }
#else
    std::size_t done = 0;
    while (done < n) {
      const DWORD want = static_cast<DWORD>(std::min<std::size_t>(n - done, 1u << 30));
      DWORD put = 0;
      if (!WriteFile(h_, p + done, want, &put, nullptr) || put == 0)
        throw IoError("write failed (disk full?): " + path_);
      done += put;
    }
#endif
  }

  std::string path_;
  std::size_t cap_;
  std::vector<ui1> buf_;
  std::uint64_t size_ = 0;
#ifndef _WIN32
  int fd_ = -1;
#else
  HANDLE h_ = INVALID_HANDLE_VALUE;
#endif
};

}  // namespace mef3io::detail
