// mef3io — uncompressed tar archives of whole sessions (.mefd.tar). One file
// instead of a directory tree: easy to share, hard to corrupt piecemeal.
// Reading needs no extraction — members of an uncompressed tar are contiguous
// byte ranges, so windowed .tdat reads work directly against the archive.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "mef3io/source.hpp"
#include "mef3io/types.hpp"

namespace mef3io {

/// Location of one regular-file member inside a tar archive.
struct TarMember {
  std::uint64_t offset = 0;  // byte offset of the member's data in the archive
  std::uint64_t size = 0;    // data length in bytes
};

/// Parsed table of contents of an uncompressed tar file. Tolerates archives
/// from GNU tar / bsdtar / Python tarfile: ustar name+prefix, "./" prefixes,
/// missing directory entries, GNU 'L' longnames, PAX 'x' (path/size honored)
/// and 'g' headers, signed-checksum variants. Rejects compressed input.
class TarIndex {
 public:
  explicit TarIndex(std::string tar_path);

  const std::string& path() const { return path_; }
  /// Regular-file members keyed by normalized member path ('/'-separated,
  /// no leading "./", no trailing '/').
  const std::map<std::string, TarMember>& files() const { return files_; }
  /// Explicit directory members (normalized, no trailing '/').
  const std::set<std::string>& dirs() const { return dirs_; }

 private:
  std::string path_;
  std::map<std::string, TarMember> files_;
  std::set<std::string> dirs_;
};

/// SessionSource over a tar archive of a session directory. The archive's
/// single top-level directory (e.g. "name.mefd/") is taken as the session
/// root and stripped from member paths; archives tarred from inside the
/// session dir (".timd" trees at top level) work too.
class TarSource : public SessionSource {
 public:
  explicit TarSource(const std::string& tar_path);

  std::vector<SourceDirEntry> list_dir(const std::string& rel) const override;
  bool exists(const std::string& rel) const override;
  std::uint64_t file_size(const std::string& rel) const override;
  std::vector<ui1> read_all(const std::string& rel) const override;
  std::vector<ui1> read_range(const std::string& rel, std::size_t offset,
                              std::size_t length) const override;
  std::string describe(const std::string& rel) const override;

 private:
  const TarMember& member(const std::string& rel) const;

  std::string tar_path_;
  std::string root_;                        // stripped member prefix ("" if none)
  std::map<std::string, TarMember> files_;  // root-relative
  std::set<std::string> dirs_;              // root-relative; always contains ""
};

/// Pack a session directory into a single uncompressed tar archive and return
/// the archive path. `tar_path` empty derives "<session_dir>.tar" (so
/// "name.mefd" becomes "name.mefd.tar"). The source directory is left
/// untouched; members are stored under the directory's name so a plain
/// `tar -x` reproduces the tree. Output is deterministic (sorted entries,
/// zeroed mtimes/owners) — archiving the same session twice yields identical
/// bytes. Refuses an existing target unless `overwrite`.
std::string archive_session(const std::string& session_dir, const std::string& tar_path = "",
                            bool overwrite = false);

/// Inverse of archive_session: unpack a session archive back into a directory
/// and return its path. `dest_dir` empty strips the archive's ".tar" suffix
/// ("name.mefd.tar" becomes "name.mefd" next to it). The session root inside
/// the archive is stripped, so `dest_dir` becomes the session directory
/// itself; foreign archives (GNU/bsdtar/tarfile) work too. Refuses an
/// existing target unless `overwrite`; a failed extraction is cleaned up.
std::string extract_session(const std::string& tar_path, const std::string& dest_dir = "",
                            bool overwrite = false);

}  // namespace mef3io
