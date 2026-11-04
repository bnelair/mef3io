// mef3io — filesystem-backed session source + source dispatch.
#include "mef3io/source.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <list>
#include <mutex>
#include <unordered_map>
#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "mef3io/errors.hpp"
#include "mef3io/tar.hpp"

namespace fs = std::filesystem;

namespace mef3io {

namespace detail {
namespace {

#ifndef _WIN32
/// One open descriptor; closed when the last user lets go (an eviction while
/// another thread is reading from it never closes it under that thread).
struct Handle {
  int fd = -1;
  std::uint64_t size = 0;
  ~Handle() {
    if (fd >= 0) ::close(fd);
  }
};

struct HandleCache {
  std::mutex mu;
  std::unordered_map<std::string, int> roots;  // root text -> scopes holding it
  std::list<std::pair<std::string, std::shared_ptr<Handle>>> lru;  // front = newest
  std::unordered_map<std::string, decltype(lru)::iterator> index;
  std::size_t cap = 0;
  // What the registered roots answered already (their files do not change
  // while registered): whether a path exists, a file's size, a folder listing.
  std::unordered_map<std::string, bool> exists_memo;
  std::unordered_map<std::string, std::uint64_t> size_memo;
  std::unordered_map<std::string, std::vector<SourceDirEntry>> dir_memo;

  HandleCache() {
    // Half the process's descriptors, at most 4096: the rest stay for the
    // writers and whatever the application holds. (A session of thousands of
    // segment files read in several passes reopens what the cap evicted.)
    cap = 4096;
    struct rlimit rl {};
    if (::getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY)
      cap = std::max<std::size_t>(8, std::min<std::size_t>(4096, rl.rlim_cur / 2));
  }
  bool covered_locked(const std::string& path) const {
    for (const auto& [root, n] : roots) {
      (void)n;
      if (path.size() > root.size() && path.compare(0, root.size(), root) == 0 &&
          (root.back() == '/' || path[root.size()] == '/' || path[root.size()] == '\\'))
        return true;
    }
    return false;
  }
};

HandleCache& hc() {
  static HandleCache* c = new HandleCache;  // never destroyed: threads may outlive statics
  return *c;
}
#endif

}  // namespace

ReadHandleScope::ReadHandleScope() = default;

ReadHandleScope::~ReadHandleScope() {
  const auto roots = roots_;
  for (const auto& r : roots) remove_root(r);
}

void ReadHandleScope::add_root(const std::string& root) {
#ifndef _WIN32
  if (root.empty()) return;
  auto& c = hc();
  std::lock_guard<std::mutex> lk(c.mu);
  ++c.roots[root];
  roots_.push_back(root);
#else
  (void)root;
#endif
}

void ReadHandleScope::remove_root(const std::string& root) {
#ifndef _WIN32
  const auto at = std::find(roots_.begin(), roots_.end(), root);
  if (at == roots_.end()) return;
  roots_.erase(at);
  auto& c = hc();
  std::vector<std::shared_ptr<Handle>> doomed;  // closed outside the lock
  {
    std::lock_guard<std::mutex> lk(c.mu);
    auto it = c.roots.find(root);
    if (it == c.roots.end() || --it->second > 0) return;
    c.roots.erase(it);
    const auto under = [&](const std::string& k) {
      return k.size() >= root.size() && k.compare(0, root.size(), root) == 0;
    };
    std::erase_if(c.exists_memo, [&](const auto& kv) { return under(kv.first); });
    std::erase_if(c.size_memo, [&](const auto& kv) { return under(kv.first); });
    std::erase_if(c.dir_memo, [&](const auto& kv) { return under(kv.first); });
    for (auto l = c.lru.begin(); l != c.lru.end();) {
      if (l->first.size() > root.size() && l->first.compare(0, root.size(), root) == 0) {
        doomed.push_back(std::move(l->second));
        c.index.erase(l->first);
        l = c.lru.erase(l);
      } else {
        ++l;
      }
    }
  }
#else
  (void)root;
#endif
}

}  // namespace detail

#ifndef _WIN32
namespace {

using detail::Handle;

/// The cached handle of `path`, opening it if its root is registered and it is
/// not open yet; null = not covered or cannot be opened (the caller takes the
/// plain path, which raises its usual error).
std::shared_ptr<Handle> acquire(const std::string& path) {
  auto& c = detail::hc();
  {
    std::lock_guard<std::mutex> lk(c.mu);
    if (c.roots.empty()) return nullptr;
    auto it = c.index.find(path);
    if (it != c.index.end()) {
      c.lru.splice(c.lru.begin(), c.lru, it->second);
      return it->second->second;
    }
    if (!c.covered_locked(path)) return nullptr;
  }
  if (detail::source_open_observer) detail::source_open_observer(path);
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return nullptr;
  auto h = std::make_shared<Handle>();
  h->fd = fd;
  {
    // exists() already stat'ed a regular file: its size is not asked again.
    std::lock_guard<std::mutex> lk(c.mu);
    const auto known = c.size_memo.find(path);
    if (known != c.size_memo.end()) h->size = known->second;
    else h->size = UINT64_MAX;
  }
  if (h->size == UINT64_MAX) {
    struct stat st {};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) return nullptr;  // ~Handle closes
    h->size = static_cast<std::uint64_t>(st.st_size);
  }
  std::vector<std::shared_ptr<Handle>> doomed;
  {
    std::lock_guard<std::mutex> lk(c.mu);
    auto it = c.index.find(path);  // another thread may have opened it meanwhile
    if (it != c.index.end()) {
      c.lru.splice(c.lru.begin(), c.lru, it->second);
      return it->second->second;
    }
    if (!c.covered_locked(path)) return nullptr;  // the root went away meanwhile
    c.lru.emplace_front(path, h);
    c.index[path] = c.lru.begin();
    while (c.lru.size() > c.cap) {
      doomed.push_back(std::move(c.lru.back().second));
      c.index.erase(c.lru.back().first);
      c.lru.pop_back();
    }
  }
  return h;
}

/// The memo of `path` when its root is registered. `fill` runs outside the
/// lock; its answer is remembered only if the root is still registered.
template <class Map, class F>
auto memoized(Map& (*pick)(detail::HandleCache&), const std::string& path, F&& fill)
    -> decltype(fill()) {
  auto& c = detail::hc();
  bool covered = false;
  {
    std::lock_guard<std::mutex> lk(c.mu);
    if (!c.roots.empty() && c.covered_locked(path)) {
      covered = true;
      auto& m = pick(c);
      auto it = m.find(path);
      if (it != m.end()) return it->second;
    }
  }
  if (!covered) return fill();  // (outside the lock: fill may take it)
  auto value = fill();
  std::lock_guard<std::mutex> lk(c.mu);
  if (c.covered_locked(path)) pick(c).emplace(path, value);
  return value;
}

/// Exactly [offset, offset+length) or false when the file is shorter.
bool pread_full(const Handle& h, std::size_t offset, std::size_t length, ui1* out) {
  std::size_t done = 0;
  while (done < length) {
    const ssize_t n = ::pread(h.fd, out + done, length - done, static_cast<off_t>(offset + done));
    if (n < 0) {
      if (errno == EINTR) continue;
      throw IoError(std::string("read failed: ") + std::error_code(errno, std::generic_category()).message());
    }
    if (n == 0) return false;
    done += static_cast<std::size_t>(n);
  }
  return true;
}

}  // namespace
#endif

DirectorySource::DirectorySource(std::string root) : root_(std::move(root)) {}

std::string DirectorySource::join(const std::string& rel) const {
  if (rel.empty()) return root_;
  return (fs::path(root_) / fs::path(rel)).string();
}

std::vector<SourceDirEntry> DirectorySource::list_dir(const std::string& rel) const {
  const auto list = [&] {
    std::vector<SourceDirEntry> out;
    for (const auto& entry : fs::directory_iterator(join(rel)))
      out.push_back({entry.path().filename().string(), entry.is_directory()});
    return out;
  };
#ifndef _WIN32
  return memoized<decltype(detail::HandleCache::dir_memo)>(
      [](detail::HandleCache& c) -> auto& { return c.dir_memo; }, join(rel), list);
#else
  return list();
#endif
}

bool DirectorySource::exists(const std::string& rel) const {
#ifndef _WIN32
  const std::string path = join(rel);
  return memoized<decltype(detail::HandleCache::exists_memo)>(
      [](detail::HandleCache& c) -> auto& { return c.exists_memo; }, path, [&] {
        // ONE stat answers both "does it exist" and "how big is it".
        struct stat st {};
        if (::stat(path.c_str(), &st) != 0)
          return errno == ENOENT || errno == ENOTDIR ? false : fs::exists(path);
        if (S_ISREG(st.st_mode)) {
          auto& c = detail::hc();
          std::lock_guard<std::mutex> lk(c.mu);
          if (c.covered_locked(path)) c.size_memo.emplace(path, static_cast<std::uint64_t>(st.st_size));
        }
        return true;
      });
#else
  return fs::exists(join(rel));
#endif
}

std::uint64_t DirectorySource::file_size(const std::string& rel) const {
#ifndef _WIN32
  return memoized<decltype(detail::HandleCache::size_memo)>(
      [](detail::HandleCache& c) -> auto& { return c.size_memo; }, join(rel),
      [&] { return static_cast<std::uint64_t>(fs::file_size(join(rel))); });
#else
  return static_cast<std::uint64_t>(fs::file_size(join(rel)));
#endif
}

std::vector<ui1> DirectorySource::read_all(const std::string& rel) const {
  const std::string path = join(rel);
#ifndef _WIN32
  if (auto h = acquire(path)) {
    std::vector<ui1> buf(static_cast<std::size_t>(h->size));
    if (!pread_full(*h, 0, buf.size(), buf.data())) throw IoError("short read: " + path);
    return buf;
  }
#endif
  if (detail::source_open_observer) detail::source_open_observer(path);
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) throw IoError("cannot open file: " + path);
  auto size = f.tellg();
  f.seekg(0);
  std::vector<ui1> buf(static_cast<std::size_t>(size));
  if (!f.read(reinterpret_cast<char*>(buf.data()), size)) throw IoError("short read: " + path);
  return buf;
}

std::vector<ui1> DirectorySource::read_head(const std::string& rel, std::size_t max_bytes) const {
  // Open + one read: no seek to the end for the size (read_all's tellg), so
  // the fixed-length .tmet costs one request whatever padding follows it.
  const std::string path = join(rel);
#ifndef _WIN32
  if (auto h = acquire(path)) {
    std::vector<ui1> buf(static_cast<std::size_t>(std::min<std::uint64_t>(h->size, max_bytes)));
    if (!pread_full(*h, 0, buf.size(), buf.data())) buf.clear();  // shrank: what it has
    return buf;
  }
#endif
  if (detail::source_open_observer) detail::source_open_observer(path);
  std::ifstream f(path, std::ios::binary);
  if (!f) throw IoError("cannot open file: " + path);
  std::vector<ui1> buf(max_bytes);
  f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(max_bytes));
  if (f.bad()) throw IoError("read failed: " + path);
  buf.resize(static_cast<std::size_t>(f.gcount()));  // short file: what it has
  return buf;
}

std::vector<ui1> DirectorySource::read_range(const std::string& rel, std::size_t offset,
                                             std::size_t length) const {
  const std::string path = join(rel);
#ifndef _WIN32
  if (auto h = acquire(path)) {
    std::vector<ui1> buf(length);
    if (length && !pread_full(*h, offset, length, buf.data())) throw IoError("short read: " + path);
    return buf;
  }
#endif
  if (detail::source_open_observer) detail::source_open_observer(path);
  std::ifstream f(path, std::ios::binary);
  if (!f) throw IoError("cannot open file: " + path);
  f.seekg(static_cast<std::streamoff>(offset));
  std::vector<ui1> buf(length);
  if (length && !f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(length)))
    throw IoError("short read: " + path);
  return buf;
}

std::string DirectorySource::describe(const std::string& rel) const { return join(rel); }

std::string DirectorySource::session_name() const { return session_name_from_path(root_); }

std::string session_name_from_path(std::string path) {
  while (!path.empty() && (path.back() == '/' || path.back() == '\\')) path.pop_back();
  const auto slash = path.find_last_of("/\\");
  std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
  for (const std::string suffix : {".tar", ".mefd"}) {
    if (name.size() >= suffix.size()) {
      std::string tail = name.substr(name.size() - suffix.size());
      std::transform(tail.begin(), tail.end(), tail.begin(),
                     [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      if (tail == suffix) name.resize(name.size() - suffix.size());
    }
  }
  return name;
}

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
