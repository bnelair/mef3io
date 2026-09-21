// Internal: durability and file-identity helpers shared by the writer and the
// validator's repair path.
//
// These lived in duplicate in writer.cpp and validate.cpp, and drifted: the
// repair path fsynced and preserved mode/owner while the append — which writes
// the SAMPLES, not just declarations — did neither. One copy, one contract.
//
// Not a public header: it is included by .cpp files inside core/src only.
#pragma once

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "mef3io/errors.hpp"

namespace mef3io::detail {

namespace fsys = std::filesystem;

/// Flush a file's contents all the way to stable storage.
///
/// A rename is ORDERED, not durable: without this a power cut can leave the
/// rename visible and the data behind it missing — and a short `.tmet` throws
/// from the metadata loader, which takes the whole session down, not just that
/// segment. Returns false rather than throwing so a caller can decide whether
/// durability is the point of its call.
[[nodiscard]] inline bool fsync_file(const std::string& path) {
#ifdef _WIN32
  HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  const bool ok = FlushFileBuffers(h) != 0;
  CloseHandle(h);
  return ok;
#else
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) return false;
  // fdatasync, not fsync. Both flush the data and any metadata needed to
  // RETRIEVE it — crucially the file size, which is what grows on an append
  // and what a reader needs to see the new bytes. fsync additionally forces
  // out mtime/atime, which nothing here depends on and which costs an extra
  // journal transaction per call. On a 64-channel append that is ~190 extra
  // commits for timestamps nobody reads.
  const bool ok = ::fdatasync(fd) == 0;
  return ::close(fd) == 0 && ok;
#endif
}

/// Flush, and fail loudly if the flush did not happen.
///
/// Used where the whole point of the call is durability: silently renaming an
/// unflushed file over a good one gives back exactly the guarantee the caller
/// was promised and did not get.
inline void fsync_file_or_throw(const std::string& path) {
  if (!fsync_file(path))
    throw IoError("could not flush to disk, the write is not durable: " + path);
}

/// A rename is only durable once the DIRECTORY entry is flushed too. Windows
/// exposes no directory handle to flush and does not need one.
inline void fsync_directory(const fsys::path& dir) {
#ifndef _WIN32
  const int fd = ::open(dir.string().c_str(), O_RDONLY);
  if (fd < 0) return;
  ::fsync(fd);
  ::close(fd);
#else
  (void)dir;
#endif
}

/// Carry an existing target's permissions — and, where the platform has them,
/// owner and group — onto a replacement.
///
/// A fresh temp file is created under the process umask, so without this a
/// rewrite silently widens access to a `.tmet`, the file holding metadata
/// section 3: subject_name, subject_id, recording location. Run as root over a
/// user-owned tree it would also change ownership, after which the original
/// user's next acquisition write fails. Chowning is best effort — an
/// unprivileged process cannot do it, and that is not a reason to fail a write
/// that has already been computed.
inline void copy_file_identity(const fsys::path& from, const fsys::path& to) {
  std::error_code ec;
  const auto st = fsys::status(from, ec);
  if (ec) return;  // target does not exist yet: nothing to carry
  fsys::permissions(to, st.permissions(), fsys::perm_options::replace, ec);
#ifndef _WIN32
  struct stat s {};
  if (::stat(from.string().c_str(), &s) == 0) {
    if (::chown(to.string().c_str(), s.st_uid, s.st_gid) != 0) { /* best effort */ }
  }
#endif
}

}  // namespace mef3io::detail
