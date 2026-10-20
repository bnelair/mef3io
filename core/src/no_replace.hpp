// Internal: a rename that never replaces an existing file (#72).
//
// Linux renameat2(RENAME_NOREPLACE), macOS renamex_np(RENAME_EXCL), Windows
// MoveFileExW without MOVEFILE_REPLACE_EXISTING. Where the file system does
// not support it (some network and FAT file systems answer EINVAL / ENOTSUP),
// the rename is REFUSED: there is no fallback to a plain rename, which could
// silently overwrite a file that appeared in between.
#pragma once

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <stdio.h>
#else
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>
#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1 << 0)
#endif
#endif

namespace mef3io::detail {

enum class NoReplaceResult {
  Renamed,
  TargetExists,  ///< nothing was changed
  Unsupported,   ///< the file system has no no-replace rename: nothing was changed
  Failed,        ///< another error (`why` says which): nothing was changed
};

/// Rename `from` to `to` only if `to` does not exist, atomically.
inline NoReplaceResult rename_no_replace(const std::filesystem::path& from,
                                         const std::filesystem::path& to, std::string& why) {
#if defined(_WIN32)
  if (MoveFileExW(from.wstring().c_str(), to.wstring().c_str(), MOVEFILE_WRITE_THROUGH))
    return NoReplaceResult::Renamed;
  const DWORD e = GetLastError();
  if (e == ERROR_ALREADY_EXISTS || e == ERROR_FILE_EXISTS) return NoReplaceResult::TargetExists;
  why = "MoveFileExW failed (Windows error " + std::to_string(e) + ")";
  return e == ERROR_NOT_SUPPORTED ? NoReplaceResult::Unsupported : NoReplaceResult::Failed;
#else
#if defined(__APPLE__)
  const int rc = renamex_np(from.c_str(), to.c_str(), RENAME_EXCL);
#else
  const int rc = static_cast<int>(
      syscall(SYS_renameat2, AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), RENAME_NOREPLACE));
#endif
  if (rc == 0) return NoReplaceResult::Renamed;
  const int e = errno;
  if (e == EEXIST || e == ENOTEMPTY) return NoReplaceResult::TargetExists;
  why = std::strerror(e);
  if (e == EINVAL || e == ENOTSUP || e == ENOSYS
#if defined(EOPNOTSUPP) && EOPNOTSUPP != ENOTSUP
      || e == EOPNOTSUPP
#endif
  )
    return NoReplaceResult::Unsupported;
  return NoReplaceResult::Failed;
#endif
}

}  // namespace mef3io::detail
