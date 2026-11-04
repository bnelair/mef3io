// mef3io — session data source abstraction. A Session reads its tree through
// a SessionSource, so the same code serves a .mefd directory on disk and an
/// uncompressed .mefd.tar archive (random access into member byte ranges).
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "mef3io/types.hpp"

namespace mef3io {

/// One entry of a source directory listing (leaf name, not a path).
struct SourceDirEntry {
  std::string name;
  bool is_dir = false;
};

/// Read-only view of a session tree. Paths are source-relative, always
/// '/'-separated; "" names the session root. Implementations must be safe for
/// concurrent reads (the parallel decoder fetches block ranges from worker
/// threads).
class SessionSource {
 public:
  virtual ~SessionSource() = default;

  /// Immediate children of a directory ("" = session root).
  virtual std::vector<SourceDirEntry> list_dir(const std::string& rel) const = 0;
  virtual bool exists(const std::string& rel) const = 0;
  virtual std::uint64_t file_size(const std::string& rel) const = 0;
  virtual std::vector<ui1> read_all(const std::string& rel) const = 0;
  /// The first min(file size, max_bytes) bytes of a file, in ONE request:
  /// a short file returns what it has (not an error), so a caller's own size
  /// check reports it exactly as it would after read_all. Used for the
  /// fixed-length .tmet record, whose loader never looks past
  /// METADATA_FILE_BYTES — on a network share every request is a round trip
  /// (#141). The default reads the whole file and truncates.
  virtual std::vector<ui1> read_head(const std::string& rel, std::size_t max_bytes) const {
    auto bytes = read_all(rel);
    if (bytes.size() > max_bytes) bytes.resize(max_bytes);
    return bytes;
  }
  /// Read exactly [offset, offset+length) of a file. Used to fetch only the
  /// RED blocks a windowed read needs, rather than the whole .tdat.
  virtual std::vector<ui1> read_range(const std::string& rel, std::size_t offset,
                                      std::size_t length) const = 0;
  /// Human-readable absolute location of `rel` (error messages,
  /// SegmentInfo.path). Directories: the on-disk path; tar members:
  /// "<archive>::<member>".
  virtual std::string describe(const std::string& rel) const = 0;
  /// The session's name, as meflib names its session-level files
  /// (<name>.mefd/<name>.rdat, meflib.c:6335): the session directory's name
  /// without ".mefd". For an archive that is the folder INSIDE it, not the
  /// archive's own file name, which can be renamed freely.
  virtual std::string session_name() const = 0;
};

namespace detail {
class HandleCache;
}

/// Plain directory tree on the local filesystem (the classic .mefd layout).
class DirectorySource : public SessionSource {
 public:
  explicit DirectorySource(std::string root);
  /// A source that reads through `cache` (see ReadHandleScope; only the copy
  /// engine builds one).
  DirectorySource(std::string root, std::shared_ptr<detail::HandleCache> cache);

  std::vector<SourceDirEntry> list_dir(const std::string& rel) const override;
  bool exists(const std::string& rel) const override;
  std::uint64_t file_size(const std::string& rel) const override;
  std::vector<ui1> read_all(const std::string& rel) const override;
  std::vector<ui1> read_head(const std::string& rel, std::size_t max_bytes) const override;
  std::vector<ui1> read_range(const std::string& rel, std::size_t offset,
                              std::size_t length) const override;
  std::string describe(const std::string& rel) const override;
  std::string session_name() const override;

 private:
  std::string join(const std::string& rel) const;
  std::string root_;
  std::shared_ptr<detail::HandleCache> cache_;  // null = plain reads
};

namespace detail {

/// A path text from the API (UTF-8) as a path object: on Windows a wide path,
/// with no ANSI code-page conversion in between.
std::filesystem::path utf8_path(const std::string& s);
/// A path object passes through (lets generic call sites wrap either kind).
inline std::filesystem::path utf8_path(const std::filesystem::path& p) { return p; }
inline std::filesystem::path utf8_path(const char* s) { return utf8_path(std::string(s)); }
/// The reverse: a path as UTF-8 text.
std::string path_utf8(const std::filesystem::path& p);
/// Same, with '/' separators (archive member names, relative names kept in notes).
std::string path_generic_utf8(const std::filesystem::path& p);

/// Reuse of open read handles for ONE operation (network drives: every open is
/// a round trip). A copy builds a scope and opens ITS sources through it
/// (`open`): those DirectorySources read through the scope's own bounded cache
/// of open file descriptors (LRU, `pread`, one open per file instead of one per
/// call) and remember what they were told about existence, sizes and listings.
/// Nothing is shared with any other Reader, Session or writer, in this thread or
/// another: a source opened elsewhere (`open_session_source`) never sees the
/// scope's cache, so it always reads fresh state. The bytes are still fetched
/// on each call; only the open/close pair is saved. A file that cannot be
/// opened falls through to the plain path, so errors are unchanged. A read
/// error on a cached handle (a stale network handle) evicts it and retries once
/// through a fresh open.
///
/// Rule: the files a scope's sources show must not change while the scope
/// lives (a copy's SOURCES; its destination only once written, for the
/// verification: open it through the scope after the write). `release_under`
/// closes the handles (Windows: so a folder can be removed or renamed).
class ReadHandleScope {
 public:
  ReadHandleScope();
  ~ReadHandleScope();
  ReadHandleScope(const ReadHandleScope&) = delete;
  ReadHandleScope& operator=(const ReadHandleScope&) = delete;
  /// open_session_source, with a directory source bound to this scope.
  std::shared_ptr<const SessionSource> open(const std::string& path) const;
  /// Close the handles and forget the answers under `prefix`.
  void release_under(const std::string& prefix) const;

 private:
  std::shared_ptr<HandleCache> cache_;
};

/// TEST HOOK: called with the path of every file a DirectorySource really
/// opens (cached or not). Must be thread-safe. Pins "each source file is
/// opened at most N times in a merge".
inline void (*source_open_observer)(const std::string& path) = nullptr;

/// TEST HOOK: called before each positional read through a cached handle;
/// a non-zero return is raised as that errno (POSIX) in place of the read.
inline int (*read_fault_hook)(const std::string& path) = nullptr;

}  // namespace detail

/// Case-insensitive suffix test on a path, ignoring trailing separators
/// ("S.MEFD/" has suffix ".mefd"). Backs the session naming rules.
bool path_has_suffix(const std::string& path, const std::string& suffix);

/// "…/name.mefd", "…/name.mefd/" or "…/name.mefd.tar" -> "name" (suffixes
/// matched case-insensitively).
std::string session_name_from_path(std::string path);

/// Open `path` as a session source: a directory becomes a DirectorySource, a
/// regular file is treated as an uncompressed tar archive of the session
/// (TarSource; compressed archives are rejected with a clear message).
/// Naming is enforced: directories must end ".mefd", archives ".mefd.tar".
std::shared_ptr<const SessionSource> open_session_source(const std::string& path);

}  // namespace mef3io
