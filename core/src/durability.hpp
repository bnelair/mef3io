// Internal: durability and file-identity helpers shared by the writer and the
// validator's repair path.
//
// These lived in duplicate in writer.cpp and validate.cpp, and drifted: the
// repair path fsynced and preserved mode/owner while the append — which writes
// the SAMPLES, not just declarations — did neither. One copy, one contract.
// (Since 2.0 the writer itself flushes only a replaced `.tmet`; repair,
// rename, recovery and the copies keep their own flush rules.)
//
// Not a public header: it is included by .cpp files inside core/src only.
#pragma once

#include <cctype>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>
#include <filesystem>
#include <span>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

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
#include "mef3io/types.hpp"

namespace mef3io::detail {

namespace fsys = std::filesystem;

/// TEST HOOK: when set, called with the path of every flush `fsync_file`
/// attempts (successful or not). Lets a test pin WHICH files a write path
/// flushes — the writer may flush only the `.tmet` it replaces — without
/// patching the OS. Set it only while no other thread writes; the observer
/// itself must be thread-safe (several channels may be written at once).
inline void (*fsync_observer)(const std::string& path) = nullptr;

/// TEST HOOK: called with the files of every flush_files batch, before the
/// batch's one combined wait (syncfs on Linux; elsewhere the per-file flushes
/// run concurrently, and each also reaches fsync_observer).
inline void (*flush_batch_observer)(const std::vector<fsys::path>& files) = nullptr;

/// TEST HOOK: called with the target of every `.tmet` a multi-channel write
/// is about to rename into place; may throw to simulate a failing rename.
inline void (*publish_observer)(const fsys::path& target) = nullptr;

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
/// Windows a no-op that reported failure: 1.x's `durability="full"` threw on
/// the first append, and the paths that ignore the result — then
/// `durability="fast"`, still the recovery backups — silently had no
/// durability at all. The writer's `.tmet` flush (fsync_file_or_throw) relies
/// on this today. Do not "simplify" this back to GENERIC_READ.
[[nodiscard]] inline bool fsync_file(const std::string& path, std::string* why = nullptr) {
  if (fsync_observer) fsync_observer(path);
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

/// A rename (or a new file/folder) is only durable once the DIRECTORY entry is
/// flushed too. Returns false when the flush failed; `why` gets the OS error.
///  * Linux: fsync on the directory.
///  * macOS: F_FULLFSYNC (the drive's cache too), falling back to fsync where
///    the file system does not implement it.
///  * Windows: FlushFileBuffers on a directory handle (FILE_FLAG_BACKUP_SEMANTICS,
///    GENERIC_WRITE). NTFS commonly refuses it (ERROR_ACCESS_DENIED) because it
///    journals directory metadata itself; the call is made and a refusal is
///    reported as false with an "[ntfs-refused]" marker, which flush_files
///    alone ignores on Windows; every other failure (cannot open the folder,
///    other errors) is a failed flush (documented in long_recordings.md).
inline bool fsync_directory(const fsys::path& dir, std::string* why = nullptr) {
#ifndef _WIN32
  const int fd = ::open(dir.string().c_str(), O_RDONLY);
  if (fd < 0) {
    if (why) *why = "cannot open folder: " + std::error_code(errno, std::generic_category()).message();
    return false;
  }
  bool ok;
#if defined(__APPLE__)
  ok = ::fcntl(fd, F_FULLFSYNC) != -1;
  if (!ok && (errno == ENOTSUP || errno == EOPNOTSUPP || errno == EINVAL)) ok = ::fsync(fd) == 0;
#else
  ok = ::fsync(fd) == 0;
#endif
  const int saved = errno;
  ::close(fd);
  if (!ok && why) *why = "fsync of folder: " + std::error_code(saved, std::generic_category()).message();
  return ok;
#else
  const std::wstring wide = dir.wstring();
  HANDLE h = CreateFileW(wide.c_str(), GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    if (why) *why = "cannot open folder for flushing";
    return false;
  }
  const bool ok = FlushFileBuffers(h) != 0;
  const DWORD err = ok ? 0 : GetLastError();
  if (!ok && why) {
    const std::error_code ec(static_cast<int>(err), std::system_category());
    *why = "FlushFileBuffers on a folder: " + ec.message();
    // Marker for flush_files: NTFS's refusal of a directory handle.
    if (err == ERROR_ACCESS_DENIED) *why += " [ntfs-refused]";
  }
  CloseHandle(h);
  return ok;
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

/// Flush and close a stream, throwing on any failure (a full disk shows up here).
inline void finish_stream(std::ofstream& f, const std::string& path) {
  f.flush();
  if (!f) throw IoError("write failed (disk full?): " + path);
  f.close();
  if (!f) throw IoError("close failed, data may not have reached disk: " + path);
}

// Replace a file's contents atomically: fill a sibling temp file, flush it,
// then rename over the target. A crash or a full disk leaves the original
// untouched instead of a truncated one — which matters because the file being
// replaced is the only record of what the (much larger) .tdat contains.
inline void write_all_atomic(const std::string& path, std::span<const ui1> bytes,
                             bool flush = true) {
  const fsys::path target(path);
  const fsys::path tmp =
      target.parent_path() / (target.filename().string() + ".mef3io-repair-tmp");
  // EVERY failure path removes the temp, not just a failed rename. The temp is
  // a sibling, so it lands INSIDE the .segd directory: a half-written file
  // named like a metadata file, left in a clinical session indefinitely, and
  // `archive_session` packs it into the .mefd.tar as if it were data. The
  // writer's own write_file_atomic already guards its whole body this way.
  try {
    {
      std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
      if (!f) throw IoError("cannot open for write: " + tmp.string());
      f.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
      if (!f) throw IoError("write failed: " + tmp.string());
      finish_stream(f, tmp.string());
    }
  // The target still exists here, so its mode/owner can be carried across
  // before it is replaced. Then flush the data before the rename, and the
  // directory entry after it.
    copy_file_identity(target, tmp);
    // Flush before the rename, and fail loudly if the flush did not happen:
    // renaming an unflushed file over a good one hands back a durability
    // guarantee the caller did not get. replace_file is the portable swap
    // (MoveFileEx + WRITE_THROUGH on Windows).
    if (flush) fsync_file_or_throw(tmp.string());
    replace_file(tmp, target);
  } catch (...) {
    std::error_code ignored;
    fsys::remove(tmp, ignored);
    throw;
  }
  if (flush) fsync_directory(target.parent_path());
}

/// Flush every file CONCURRENTLY (up to 16 at a time), so the waits
/// overlap instead of adding up; throws the first failure. The per-file
/// path of flush_files on systems without syncfs.
inline void flush_each_concurrently(const std::vector<fsys::path>& files) {
  std::vector<fsys::path> unique;
  for (const auto& f : files)
    if (std::find(unique.begin(), unique.end(), f) == unique.end()) unique.push_back(f);
  std::mutex mu;
  std::string first_error;
  std::atomic<std::size_t> next{0};
  const auto worker = [&] {
    for (std::size_t i; (i = next.fetch_add(1)) < unique.size();) {
      std::string why;
      if (!fsync_file(unique[i].string(), &why)) {
        std::lock_guard<std::mutex> lk(mu);
        if (first_error.empty())
          first_error = unique[i].string() + (why.empty() ? "" : " (" + why + ")");
      }
    }
  };
  const std::size_t n = std::min<std::size_t>(16, unique.size());
  std::vector<std::thread> pool;
  for (std::size_t t = 1; t < n; ++t) pool.emplace_back(worker);
  worker();
  for (auto& t : pool) t.join();
  if (!first_error.empty())
    throw IoError("could not flush to disk, the write is not durable: " + first_error);
}

/// Flush many files written without a flush, and their folders, in ONE
/// combined wait. Linux: one syncfs per file system (flushing thousands of
/// headers one fsync at a time took ~20 ms each, 38 minutes for a 64-channel
/// x 600-segment rename). Elsewhere: every file concurrently
/// (flush_each_concurrently), then every folder.
inline void flush_files(const std::vector<fsys::path>& files) {
  if (files.empty()) return;
  if (flush_batch_observer) flush_batch_observer(files);
#if defined(__linux__)
  std::vector<dev_t> done;
  for (const auto& f : files) {
    struct stat s {};
    if (::stat(f.c_str(), &s) != 0) continue;
    if (std::find(done.begin(), done.end(), s.st_dev) != done.end()) continue;
    const int fd = ::open(f.c_str(), O_RDONLY);
    if (fd < 0 || ::syncfs(fd) != 0) {
      const int e = errno;
      if (fd >= 0) ::close(fd);
      throw IoError("could not flush to disk, the write is not durable: " + f.string() + " (" +
                    std::error_code(e, std::generic_category()).message() + ")");
    }
    ::close(fd);
    done.push_back(s.st_dev);
  }
#else
  flush_each_concurrently(files);
  // Every folder a flushed file lives in AND the folders above it that this
  // session may have just created (.segd -> .timd -> .mefd -> the .mefd's
  // parent): a new folder's own entry is in its parent, which no file flush
  // reaches. Four levels cover a segment file; a record file just flushes a
  // level or two more than it needs.
  std::vector<fsys::path> dirs;
  for (const auto& f : files) {
    fsys::path d = f.parent_path();
    for (int level = 0; level < 4 && !d.empty(); ++level, d = d.parent_path()) {
      if (std::find(dirs.begin(), dirs.end(), d) == dirs.end()) dirs.push_back(d);
      // Never above the session folder: its parent is not ours to flush.
      std::string leaf = d.filename().string();
      for (auto& ch : leaf) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
      if (leaf.size() >= 5 && leaf.compare(leaf.size() - 5, 5, ".mefd") == 0) break;
      if (d == d.parent_path()) break;
    }
  }
  for (const auto& d : dirs) {
    std::string why;
#ifdef _WIN32
    // NTFS may refuse a folder flush (ERROR_ACCESS_DENIED): only that one
    // refusal is ignored; anything else is a failed flush.
    if (!fsync_directory(d, &why) && why.find("[ntfs-refused]") == std::string::npos)
      throw IoError("could not flush to disk, the write is not durable: " + d.string() +
                    (why.empty() ? "" : " (" + why + ")"));
#else
    if (!fsync_directory(d, &why))
      throw IoError("could not flush to disk, the write is not durable: " + d.string() +
                    (why.empty() ? "" : " (" + why + ")"));
#endif
  }
#endif
}

}  // namespace mef3io::detail
