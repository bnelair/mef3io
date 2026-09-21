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
#include <cerrno>
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
/// durability is the point of its call; `why` receives the OS error when it
/// does fail, because "the write is not durable" without a reason is a bug
/// report nobody can act on.
///
/// WINDOWS NEEDS GENERIC_WRITE HERE. `FlushFileBuffers` requires the handle to
/// carry the write right and fails with ERROR_ACCESS_DENIED without it —
/// unlike POSIX, where fsync on an O_RDONLY descriptor is perfectly legal.
/// Opening for read cost nothing on Linux and macOS and made every flush on
/// Windows a no-op that reported failure: `durability="full"` threw on the
/// first append, and the paths that ignore the result — `durability="fast"`
/// and the recovery backups — silently had no durability at all. Do not
/// "simplify" this back to GENERIC_READ.
[[nodiscard]] inline bool fsync_file(const std::string& path, std::string* why = nullptr) {
#ifdef _WIN32
  const auto fail = [&](const char* what) {
    if (why) {
      const std::error_code ec(static_cast<int>(GetLastError()), std::system_category());
      *why = std::string(what) + ": " + ec.message();
    }
    return false;
  };
  // The wide entry point, like replace_file: CreateFileA goes through the ANSI
  // code page, so a session path outside it cannot even be opened.
  const std::wstring wide = fsys::path(path).wstring();
  HANDLE h = CreateFileW(wide.c_str(), GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return fail("cannot open for flushing");
  if (!FlushFileBuffers(h)) {
    const bool reported = fail("FlushFileBuffers");
    CloseHandle(h);
    return reported;
  }
  CloseHandle(h);
  return true;
#else
  const auto fail = [&](const char* what) {
    if (why) *why = std::string(what) + ": " + std::error_code(errno, std::generic_category()).message();
    return false;
  };
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) return fail("cannot open for flushing");
  bool ok;
#if defined(__APPLE__)
  // macOS has no fdatasync, and its fsync() only pushes the data to the drive
  // — the drive's own write cache can still lose it on a power cut. F_FULLFSYNC
  // is the call that actually forces the platter, and it is what the contract
  // in the docs promises. Some filesystems (and network mounts) do not support
  // it and return ENOTSUP, so fall back to fsync there rather than failing a
  // write outright: a weaker flush beats refusing to write at all, and it is
  // still no worse than what the reference implementation does, which is
  // nothing.
  ok = ::fcntl(fd, F_FULLFSYNC) != -1;
  if (!ok && (errno == ENOTSUP || errno == EOPNOTSUPP || errno == EINVAL)) {
    // The filesystem or mount does not implement a full flush (common on
    // network mounts and some disk images). Falling back is right there.
    ok = ::fsync(fd) == 0;
  }
  // Any OTHER failure is a real I/O error and must stay a failure: silently
  // downgrading it would report a write as durable when the flush did not
  // happen, which is the one thing this function exists to prevent.
#else
  // fdatasync, not fsync. Both flush the data and any metadata needed to
  // RETRIEVE it — crucially the file size, which is what grows on an append
  // and what a reader needs to see the new bytes. fsync additionally forces
  // out mtime/atime, which nothing here depends on and which costs an extra
  // journal transaction per call. On a 64-channel append that is ~190 extra
  // commits for timestamps nobody reads.
  ok = ::fdatasync(fd) == 0;
#endif
  if (!ok) {
    const int saved = errno;
    ::close(fd);
    errno = saved;
    return fail("fsync");
  }
  if (::close(fd) != 0) return fail("close");
  return true;
#endif
}

/// Flush, and fail loudly if the flush did not happen.
///
/// Used where the whole point of the call is durability: silently renaming an
/// unflushed file over a good one gives back exactly the guarantee the caller
/// was promised and did not get.
inline void fsync_file_or_throw(const std::string& path) {
  std::string why;
  if (!fsync_file(path, &why))
    throw IoError("could not flush to disk, the write is not durable: " + path +
                  (why.empty() ? "" : " (" + why + ")"));
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

/// Replace `target` with `tmp` atomically, on every platform.
///
/// `std::filesystem::rename` is specified to overwrite, but the guarantee is
/// thin on Windows and the codebase has always used MoveFileEx there. Recovery
/// and the writer both publish files by rename, so they share this.
inline void replace_file(const fsys::path& tmp, const fsys::path& target) {
#ifdef _WIN32
  if (MoveFileExW(tmp.wstring().c_str(), target.wstring().c_str(),
                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    return;
  const std::error_code ec(static_cast<int>(GetLastError()), std::system_category());
  throw IoError("cannot replace " + target.string() + ": " + ec.message());
#else
  std::error_code ec;
  fsys::rename(tmp, target, ec);
  if (ec) throw IoError("cannot replace " + target.string() + ": " + ec.message());
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
