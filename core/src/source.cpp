// mef3io — filesystem-backed session source + source dispatch.
#include "mef3io/source.hpp"

#include <cctype>
#include <filesystem>
#include <fstream>

#include "mef3io/errors.hpp"
#include "mef3io/tar.hpp"

namespace fs = std::filesystem;

namespace mef3io {

DirectorySource::DirectorySource(std::string root) : root_(std::move(root)) {}

std::string DirectorySource::join(const std::string& rel) const {
  if (rel.empty()) return root_;
  return (fs::path(root_) / fs::path(rel)).string();
}

std::vector<SourceDirEntry> DirectorySource::list_dir(const std::string& rel) const {
  std::vector<SourceDirEntry> out;
  for (const auto& entry : fs::directory_iterator(join(rel)))
    out.push_back({entry.path().filename().string(), entry.is_directory()});
  return out;
}

bool DirectorySource::exists(const std::string& rel) const { return fs::exists(join(rel)); }

std::uint64_t DirectorySource::file_size(const std::string& rel) const {
  return static_cast<std::uint64_t>(fs::file_size(join(rel)));
}

std::vector<ui1> DirectorySource::read_all(const std::string& rel) const {
  const std::string path = join(rel);
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) throw IoError("cannot open file: " + path);
  auto size = f.tellg();
  f.seekg(0);
  std::vector<ui1> buf(static_cast<std::size_t>(size));
  if (!f.read(reinterpret_cast<char*>(buf.data()), size)) throw IoError("short read: " + path);
  return buf;
}

std::vector<ui1> DirectorySource::read_range(const std::string& rel, std::size_t offset,
                                             std::size_t length) const {
  const std::string path = join(rel);
  std::ifstream f(path, std::ios::binary);
  if (!f) throw IoError("cannot open file: " + path);
  f.seekg(static_cast<std::streamoff>(offset));
  std::vector<ui1> buf(length);
  if (length && !f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(length)))
    throw IoError("short read: " + path);
  return buf;
}

std::string DirectorySource::describe(const std::string& rel) const { return join(rel); }

bool path_has_suffix(const std::string& path, const std::string& suffix) {
  std::size_t end = path.size();
  while (end > 0 && (path[end - 1] == '/' || path[end - 1] == '\\')) --end;
  if (end < suffix.size()) return false;
  for (std::size_t i = 0; i < suffix.size(); ++i) {
    const auto a = static_cast<unsigned char>(path[end - suffix.size() + i]);
    if (std::tolower(a) != static_cast<unsigned char>(suffix[i])) return false;
  }
  return true;
}

std::shared_ptr<const SessionSource> open_session_source(const std::string& path) {
  // Session naming is a safety contract: only clearly-marked paths are ever
  // opened, so a stray directory or file cannot be misread as a session.
  if (fs::is_directory(path)) {
    if (!path_has_suffix(path, ".mefd"))
      throw IoError("session directory must end with .mefd: " + path);
    return std::make_shared<DirectorySource>(path);
  }
  if (fs::is_regular_file(path)) {
    if (!path_has_suffix(path, ".mefd.tar"))
      throw IoError("session archive must end with .mefd.tar: " + path);
    return std::make_shared<TarSource>(path);
  }
  throw IoError("session path is not a directory or tar archive: " + path);
}

}  // namespace mef3io
