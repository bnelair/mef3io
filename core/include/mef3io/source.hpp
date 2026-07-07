// mef3io — session data source abstraction. A Session reads its tree through
// a SessionSource, so the same code serves a .mefd directory on disk and an
/// uncompressed .mefd.tar archive (random access into member byte ranges).
#pragma once

#include <cstdint>
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
  /// Read exactly [offset, offset+length) of a file. Used to fetch only the
  /// RED blocks a windowed read needs, rather than the whole .tdat.
  virtual std::vector<ui1> read_range(const std::string& rel, std::size_t offset,
                                      std::size_t length) const = 0;
  /// Human-readable absolute location of `rel` (error messages,
  /// SegmentInfo.path). Directories: the on-disk path; tar members:
  /// "<archive>::<member>".
  virtual std::string describe(const std::string& rel) const = 0;
};

/// Plain directory tree on the local filesystem (the classic .mefd layout).
class DirectorySource : public SessionSource {
 public:
  explicit DirectorySource(std::string root);

  std::vector<SourceDirEntry> list_dir(const std::string& rel) const override;
  bool exists(const std::string& rel) const override;
  std::uint64_t file_size(const std::string& rel) const override;
  std::vector<ui1> read_all(const std::string& rel) const override;
  std::vector<ui1> read_range(const std::string& rel, std::size_t offset,
                              std::size_t length) const override;
  std::string describe(const std::string& rel) const override;

 private:
  std::string join(const std::string& rel) const;
  std::string root_;
};

/// Case-insensitive suffix test on a path, ignoring trailing separators
/// ("S.MEFD/" has suffix ".mefd"). Backs the session naming rules.
bool path_has_suffix(const std::string& path, const std::string& suffix);

/// Open `path` as a session source: a directory becomes a DirectorySource, a
/// regular file is treated as an uncompressed tar archive of the session
/// (TarSource; compressed archives are rejected with a clear message).
/// Naming is enforced: directories must end ".mefd", archives ".mefd.tar".
std::shared_ptr<const SessionSource> open_session_source(const std::string& path);

}  // namespace mef3io
