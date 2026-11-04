// mef3io — filesystem-backed session source + source dispatch.
#include "mef3io/source.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <condition_variable>
#include <list>
#include <set>
#include <mutex>
#include <unordered_map>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
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

std::filesystem::path utf8_path(const std::string& s) {
  return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

std::string path_utf8(const std::filesystem::path& p) {
  const std::u8string u = p.u8string();
  return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

namespace {

/// A read that failed in a way a fresh open may cure (a stale network handle).
struct StaleRead {
  std::string what;
};

bool retryable_errno(int e) {
#ifndef _WIN32
  return e == ESTALE || e == EIO || e == EBADF;
#else
  (void)e;
  return false;
#endif
}

#ifdef _WIN32
bool retryable_win(DWORD e) {
  return e == ERROR_NETNAME_DELETED || e == ERROR_UNEXP_NET_ERR || e == ERROR_BAD_NETPATH ||
         e == ERROR_SEM_TIMEOUT;
}
#endif

}  // namespace

/// One open file; closed when the last user lets go (an eviction while
/// another thread is reading from it never closes it under that thread).
/// POSIX: a descriptor read with pread. Windows: a HANDLE opened with all
/// three share modes (so the file can still be replaced or deleted by
/// others, like the stream the plain path uses) and FILE_FLAG_OVERLAPPED
/// (every read carries its own offset and event, so threads reading the same
/// file are not serialized by a shared file pointer).
struct Handle {
  std::string path;
#ifdef _WIN32
  HANDLE fd = INVALID_HANDLE_VALUE;
  ~Handle() {
    if (fd != INVALID_HANDLE_VALUE) CloseHandle(fd);
  }
#else
  int fd = -1;
  ~Handle() {
    if (fd >= 0) ::close(fd);
  }
#endif
  std::uint64_t size = 0;
};

/// The open handles and the answers of the sources bound to one scope.
class HandleCache {
 public:
  std::mutex mu;
  std::condition_variable opened;  // an in-flight open finished
  std::set<std::string> opening;   // paths being opened right now (one opener each)
  std::list<std::pair<std::string, std::shared_ptr<Handle>>> lru;  // front = newest
  std::unordered_map<std::string, decltype(lru)::iterator> index;
  std::size_t cap = 0;
  // What the bound sources were told already (their files do not change while
  // the scope lives): whether a path exists, a file's size, a folder listing.
  std::unordered_map<std::string, bool> exists_memo;
  std::unordered_map<std::string, std::uint64_t> size_memo;
  std::unordered_map<std::string, std::vector<SourceDirEntry>> dir_memo;

  HandleCache() {
    // Half the process's descriptors, at most 4096: the rest stay for the
    // writers and whatever the application holds. (A session of thousands of
    // segment files read in several passes reopens what the cap evicted.)
    cap = 4096;
#ifdef _WIN32
    cap = 1024;  // no descriptor limit to read; a few hundred open files are plenty
#else
    struct rlimit rl {};
    if (::getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY)
      cap = std::max<std::size_t>(8, std::min<std::size_t>(4096, rl.rlim_cur / 2));
#endif
  }
};

namespace {

bool under_prefix(const std::string& k, const std::string& prefix) {
  return k.size() >= prefix.size() && k.compare(0, prefix.size(), prefix) == 0;
}

}  // namespace

ReadHandleScope::ReadHandleScope() : cache_(std::make_shared<HandleCache>()) {}

ReadHandleScope::~ReadHandleScope() = default;  // sources still holding the cache keep it; the last one closes the handles

std::shared_ptr<const SessionSource> ReadHandleScope::open(const std::string& path) const {
  // Same naming rules as open_session_source; a directory is bound to the cache.
  auto plain = open_session_source(path);
  if (dynamic_cast<const DirectorySource*>(plain.get()))
    return std::make_shared<DirectorySource>(path, cache_);
  return plain;
}

void ReadHandleScope::release_under(const std::string& prefix) const {
  auto& c = *cache_;
  std::vector<std::shared_ptr<Handle>> doomed;  // closed outside the lock
  std::lock_guard<std::mutex> lk(c.mu);
  std::erase_if(c.exists_memo, [&](const auto& kv) { return under_prefix(kv.first, prefix); });
  std::erase_if(c.size_memo, [&](const auto& kv) { return under_prefix(kv.first, prefix); });
  std::erase_if(c.dir_memo, [&](const auto& kv) { return under_prefix(kv.first, prefix); });
  for (auto l = c.lru.begin(); l != c.lru.end();) {
    if (under_prefix(l->first, prefix)) {
      doomed.push_back(std::move(l->second));
      c.index.erase(l->first);
      l = c.lru.erase(l);
    } else {
      ++l;
    }
  }
  // (`doomed` goes out of scope after `lk`... declared first, destroyed last:
  // the handles close once the lock is released.)
}

}  // namespace detail

namespace {

using detail::Handle;
using detail::HandleCache;

std::shared_ptr<Handle> open_handle(const std::string& path, std::uint64_t known_size);

/// The cached handle of `path`, opening it if it is not open yet; null =
/// cannot be opened (the caller takes the plain path, which raises its usual
/// error). One thread opens a given path at a time; the others wait for it
/// and share the handle, so a file is opened once however many threads ask.
std::shared_ptr<Handle> acquire(HandleCache& c, const std::string& path) {
  std::uint64_t known_size = UINT64_MAX;  // exists() already stat'ed a regular file
  {
    std::unique_lock<std::mutex> lk(c.mu);
    for (;;) {
      auto it = c.index.find(path);
      if (it != c.index.end()) {
        c.lru.splice(c.lru.begin(), c.lru, it->second);
        return it->second->second;
      }
      if (!c.opening.count(path)) break;
      c.opened.wait(lk);
    }
    c.opening.insert(path);
    const auto known = c.size_memo.find(path);
    if (known != c.size_memo.end()) known_size = known->second;
  }
  struct Done {  // always releases the claim, so a waiter never hangs
    HandleCache& c;
    const std::string& path;
    ~Done() {
      std::lock_guard<std::mutex> lk(c.mu);
      c.opening.erase(path);
      c.opened.notify_all();
    }
  } done{c, path};
  if (detail::source_open_observer) detail::source_open_observer(path);
  auto h = open_handle(path, known_size);
  if (!h) return nullptr;
  std::vector<std::shared_ptr<Handle>> doomed;
  {
    std::lock_guard<std::mutex> lk(c.mu);
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

/// Drop `h` from the cache (a stale handle), if it is still the cached one.
void evict(HandleCache& c, const std::string& path, const std::shared_ptr<Handle>& h) {
  std::shared_ptr<Handle> doomed;
  std::lock_guard<std::mutex> lk(c.mu);
  auto it = c.index.find(path);
  if (it == c.index.end() || it->second->second != h) return;
  doomed = std::move(it->second->second);
  c.lru.erase(it->second);
  c.index.erase(it);
}

/// The memo of `path`; `fill` runs outside the lock.
template <class Map, class F>
auto memoized(HandleCache* c, Map& (*pick)(HandleCache&), const std::string& path, F&& fill)
    -> decltype(fill()) {
  if (!c) return fill();
  {
    std::lock_guard<std::mutex> lk(c->mu);
    auto& m = pick(*c);
    auto it = m.find(path);
    if (it != m.end()) return it->second;
  }
  auto value = fill();
  std::lock_guard<std::mutex> lk(c->mu);
  pick(*c).emplace(path, value);
  return value;
}

/// Exactly [offset, offset+length) or false when the file is shorter; with
/// `partial`, the bytes read so far are counted there (a shrunk file).
/// A stale-handle error is raised as StaleRead, any other as IoError naming
/// the file.
bool pread_full(const Handle& h, std::size_t offset, std::size_t length, ui1* out,
                std::size_t* partial = nullptr) {
  std::size_t done = 0;
  if (partial) *partial = 0;
  if (length && detail::read_fault_hook) {
    if (const int e = detail::read_fault_hook(h.path)) {
      const std::string what = "read failed: " + h.path + " (" +
                               std::error_code(e, std::generic_category()).message() + ")";
      if (detail::retryable_errno(e)) throw detail::StaleRead{what};
      throw IoError(what);
    }
  }
  while (done < length) {
#ifdef _WIN32
    OVERLAPPED ov{};
    const std::uint64_t at = static_cast<std::uint64_t>(offset) + done;
    ov.Offset = static_cast<DWORD>(at & 0xFFFFFFFFu);
    ov.OffsetHigh = static_cast<DWORD>(at >> 32);
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);  // per call: no shared state
    const DWORD want = static_cast<DWORD>(std::min<std::size_t>(length - done, 1u << 30));
    DWORD got = 0;
    DWORD e = 0;
    if (!ReadFile(h.fd, out + done, want, &got, &ov)) {
      e = GetLastError();
      if (e == ERROR_IO_PENDING) {  // wait for THIS read only
        e = GetOverlappedResult(h.fd, &ov, &got, TRUE) ? 0 : GetLastError();
      }
    }
    if (ov.hEvent) CloseHandle(ov.hEvent);
    if (e == ERROR_HANDLE_EOF) {
      if (partial) *partial = done;
      return false;
    }
    if (e != 0) {
      const std::string what = "read failed: " + h.path + " (" +
                               std::error_code(static_cast<int>(e), std::system_category()).message() + ")";
      if (detail::retryable_win(e)) throw detail::StaleRead{what};
      throw IoError(what);
    }
    if (got == 0) {
      if (partial) *partial = done;
      return false;
    }
    done += got;
#else
    const ssize_t n = ::pread(h.fd, out + done, length - done, static_cast<off_t>(offset + done));
    if (n < 0) {
      if (errno == EINTR) continue;
      const int e = errno;
      const std::string what =
          "read failed: " + h.path + " (" + std::error_code(e, std::generic_category()).message() + ")";
      if (detail::retryable_errno(e)) throw detail::StaleRead{what};
      throw IoError(what);
    }
    if (n == 0) {
      if (partial) *partial = done;
      return false;
    }
    done += static_cast<std::size_t>(n);
#endif
  }
  if (partial) *partial = done;
  return true;
}

/// Run `fn(handle)` on the cached handle of `path`; a stale-handle error evicts
/// it and runs `fn` once more on a fresh open. False = no handle (not cached,
/// or cannot be opened): the caller reads the plain way.
template <class F>
bool with_handle(HandleCache* c, const std::string& path, F&& fn) {
  if (!c) return false;
  for (int attempt = 0;; ++attempt) {
    auto h = acquire(*c, path);
    if (!h) return false;
    try {
      fn(*h);
      return true;
    } catch (const detail::StaleRead& e) {
      evict(*c, path, h);
      if (attempt == 1) throw IoError(e.what + " (also after reopening)");
    }
  }
}

/// Open `path` for positional reads; null if it cannot be opened or is not a
/// regular file. `known_size` (UINT64_MAX = unknown) saves the size query.
std::shared_ptr<Handle> open_handle(const std::string& text, std::uint64_t known_size) {
  auto h = std::make_shared<Handle>();
  h->path = text;
  const fs::path path = detail::utf8_path(text);
#ifdef _WIN32
  h->fd = CreateFileW(path.c_str(), GENERIC_READ,
                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
                      nullptr);  // (a folder fails here)
  if (h->fd == INVALID_HANDLE_VALUE) return nullptr;
  if (known_size != UINT64_MAX) {
    h->size = known_size;
  } else {
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h->fd, &sz)) return nullptr;  // ~Handle closes
    h->size = static_cast<std::uint64_t>(sz.QuadPart);
  }
#else
  h->fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (h->fd < 0) return nullptr;
  if (known_size != UINT64_MAX) {
    h->size = known_size;
  } else {
    struct stat st {};
    if (::fstat(h->fd, &st) != 0 || !S_ISREG(st.st_mode)) return nullptr;  // ~Handle closes
    h->size = static_cast<std::uint64_t>(st.st_size);
  }
#endif
  return h;
}

/// Existence of `path` and, for a regular file, its size (`size` stays
/// UINT64_MAX otherwise) from ONE query.
bool stat_exists(const fs::path& path, std::uint64_t& size) {
#ifdef _WIN32
  WIN32_FILE_ATTRIBUTE_DATA d{};
  if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d)) {
    const DWORD e = GetLastError();
    if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) return false;
    return fs::exists(path);
  }
  if (!(d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
    size = (static_cast<std::uint64_t>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
  return true;
#else
  struct stat st {};
  if (::stat(path.c_str(), &st) != 0)
    return errno == ENOENT || errno == ENOTDIR ? false : fs::exists(path);
  if (S_ISREG(st.st_mode)) size = static_cast<std::uint64_t>(st.st_size);
  return true;
#endif
}

}  // namespace

DirectorySource::DirectorySource(std::string root) : root_(std::move(root)) {}

DirectorySource::DirectorySource(std::string root, std::shared_ptr<detail::HandleCache> cache)
    : root_(std::move(root)), cache_(std::move(cache)) {}

std::string DirectorySource::join(const std::string& rel) const {
  if (rel.empty()) return root_;
  return detail::path_utf8(detail::utf8_path(root_) / detail::utf8_path(rel));
}

std::vector<SourceDirEntry> DirectorySource::list_dir(const std::string& rel) const {
  const std::string path = join(rel);
  const auto list = [&] {
    std::vector<SourceDirEntry> out;
    for (const auto& entry : fs::directory_iterator(detail::utf8_path(path)))
      out.push_back({detail::path_utf8(entry.path().filename()), entry.is_directory()});
    return out;
  };
  return memoized<decltype(HandleCache::dir_memo)>(
      cache_.get(), [](HandleCache& c) -> auto& { return c.dir_memo; }, path, list);
}

bool DirectorySource::exists(const std::string& rel) const {
  const std::string path = join(rel);
  HandleCache* const c = cache_.get();
  return memoized<decltype(HandleCache::exists_memo)>(
      c, [](HandleCache& k) -> auto& { return k.exists_memo; }, path, [&] {
        // ONE query answers both "does it exist" and "how big is it".
        std::uint64_t size = UINT64_MAX;
        if (!stat_exists(detail::utf8_path(path), size)) return false;
        if (size != UINT64_MAX && c) {
          std::lock_guard<std::mutex> lk(c->mu);
          c->size_memo.emplace(path, size);
        }
        return true;
      });
}

std::uint64_t DirectorySource::file_size(const std::string& rel) const {
  const std::string path = join(rel);
  return memoized<decltype(HandleCache::size_memo)>(
      cache_.get(), [](HandleCache& c) -> auto& { return c.size_memo; }, path,
      [&] { return static_cast<std::uint64_t>(fs::file_size(detail::utf8_path(path))); });
}

std::vector<ui1> DirectorySource::read_all(const std::string& rel) const {
  const std::string path = join(rel);
  std::vector<ui1> buf;
  if (with_handle(cache_.get(), path, [&](const Handle& h) {
        buf.assign(static_cast<std::size_t>(h.size), 0);
        if (!pread_full(h, 0, buf.size(), buf.data())) throw IoError("short read: " + path);
      }))
    return buf;
  if (detail::source_open_observer) detail::source_open_observer(path);
  std::ifstream f(detail::utf8_path(path), std::ios::binary | std::ios::ate);
  if (!f) throw IoError("cannot open file: " + path);
  auto size = f.tellg();
  f.seekg(0);
  buf.assign(static_cast<std::size_t>(size), 0);
  if (size > 0 && !f.read(reinterpret_cast<char*>(buf.data()), size)) throw IoError("short read: " + path);
  return buf;
}

std::vector<ui1> DirectorySource::read_head(const std::string& rel, std::size_t max_bytes) const {
  // Open + one read: no seek to the end for the size (read_all's tellg), so
  // the fixed-length .tmet costs one request whatever padding follows it.
  const std::string path = join(rel);
  std::vector<ui1> buf;
  if (with_handle(cache_.get(), path, [&](const Handle& h) {
        buf.assign(static_cast<std::size_t>(std::min<std::uint64_t>(h.size, max_bytes)), 0);
        std::size_t got = 0;
        if (!pread_full(h, 0, buf.size(), buf.data(), &got)) buf.resize(got);  // shrank: what it has
      }))
    return buf;
  if (detail::source_open_observer) detail::source_open_observer(path);
  std::ifstream f(detail::utf8_path(path), std::ios::binary);
  if (!f) throw IoError("cannot open file: " + path);
  buf.assign(max_bytes, 0);
  f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(max_bytes));
  if (f.bad()) throw IoError("read failed: " + path);
  buf.resize(static_cast<std::size_t>(f.gcount()));  // short file: what it has
  return buf;
}

std::vector<ui1> DirectorySource::read_range(const std::string& rel, std::size_t offset,
                                             std::size_t length) const {
  const std::string path = join(rel);
  std::vector<ui1> buf;
  if (with_handle(cache_.get(), path, [&](const Handle& h) {
        buf.assign(length, 0);
        if (length && !pread_full(h, offset, length, buf.data())) throw IoError("short read: " + path);
      }))
    return buf;
  if (detail::source_open_observer) detail::source_open_observer(path);
  std::ifstream f(detail::utf8_path(path), std::ios::binary);
  if (!f) throw IoError("cannot open file: " + path);
  f.seekg(static_cast<std::streamoff>(offset));
  buf.assign(length, 0);
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
