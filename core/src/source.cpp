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

std::string path_generic_utf8(const std::filesystem::path& p) {
  const std::u8string u = p.generic_u8string();
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
  // Which file this handle is (POSIX: device + inode; Windows: creation time),
  // taken from the open handle; only set for a Reader's cache.
  std::uint64_t id1 = 0, id2 = 0;
  std::uint64_t stamp_size = 0;  // Windows: the path's size when it was opened (part of the identity)
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
  // A Reader's cache (make_reader_cache): handles only, for .tdat files and a
  // tar archive; no memo of any answer; every use is validated against the path.
  bool reader_mode = false;
  // What the bound sources were told already (their files do not change while
  // the scope lives): whether a path exists, a file's size, a folder listing.
  std::unordered_map<std::string, bool> exists_memo;
  std::unordered_map<std::string, std::uint64_t> size_memo;
  std::unordered_map<std::string, std::vector<SourceDirEntry>> dir_memo;
  // Whole contents of the SMALL metadata files (.tmet/.tidx/.ridx, at most
  // MEMO_FILE_BYTES each, detail::memo_total_budget together) read by read_all: a file
  // read again later (the validator, the planning, the verification) costs no
  // open even when the handle cap evicted its handle. Forgotten with release_under.
  static constexpr std::size_t MEMO_FILE_BYTES = 1u << 20;  // the total: detail::memo_total_budget
  std::unordered_map<std::string, std::shared_ptr<const std::vector<ui1>>> bytes_memo;
  std::size_t bytes_memo_total = 0;

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
  for (auto it = c.bytes_memo.begin(); it != c.bytes_memo.end();) {
    if (under_prefix(it->first, prefix)) {
      c.bytes_memo_total -= it->second->size();
      it = c.bytes_memo.erase(it);
    } else {
      ++it;
    }
  }
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

std::shared_ptr<Handle> open_handle(const std::string& path, std::uint64_t known_size,
                                    bool want_ids = false);

/// What a path (or an open handle) says about the file: its size, and which
/// file it is. POSIX: device + inode. Windows: the creation time (a path query
/// has no file index); a file recreated or replaced has another one.
struct Stamp {
  bool ok = false;
  std::uint64_t size = 0, id1 = 0, id2 = 0;
};

/// Whether the open handle is still the file the path shows now. POSIX: device
/// + inode (the kept descriptor pins the inode, so it cannot be recycled).
/// Windows: the path query has no file id and creation time alone is restored
/// by NTFS tunnelling (delete + recreate under a name within ~15 s), so the
/// identity is creation time + last-write time + size, all from the PATH, as
/// recorded when the handle was opened. An in-place write changes mtime/size, so
/// it costs one reopen (correct, and cheap next to a request); what it cannot
/// see is a replacement that keeps ALL THREE (a copy that preserves times and
/// has the same size, made while nothing wrote to the old file).
bool same_file(const Handle& have, const Stamp& now) {
#ifdef _WIN32
  return have.id1 == now.id1 && have.id2 == now.id2 && have.stamp_size == now.size;
#else
  return have.id1 == now.id1 && have.id2 == now.id2;
#endif
}

/// errno / GetLastError of the last failed open_handle on this thread.
thread_local int last_open_error = 0;
bool out_of_descriptors(int e) {
#ifdef _WIN32
  return e == ERROR_TOO_MANY_OPEN_FILES;
#else
  return e == EMFILE || e == ENFILE;
#endif
}

/// The cached handle of `path`, opening it if it is not open yet; null =
/// cannot be opened (the caller takes the plain path, which raises its usual
/// error). One thread opens a given path at a time; the others wait for it
/// and share the handle, so a file is opened once however many threads ask.
/// With `expect` (a Reader's cache): a cached handle whose file is not the one
/// the path shows now is dropped and the path opened afresh.
std::shared_ptr<Handle> acquire(HandleCache& c, const std::string& path,
                                const Stamp* expect = nullptr) {
  std::uint64_t known_size = UINT64_MAX;  // exists() already stat'ed a regular file
  std::shared_ptr<Handle> replaced;       // closed after the lock is released
  {
    std::unique_lock<std::mutex> lk(c.mu);
    for (;;) {
      auto it = c.index.find(path);
      if (it != c.index.end()) {
        const Handle& have = *it->second->second;
        if (!expect || same_file(have, *expect)) {
          c.lru.splice(c.lru.begin(), c.lru, it->second);
          return it->second->second;
        }
        replaced = std::move(it->second->second);  // another file now: forget this one
        c.lru.erase(it->second);
        c.index.erase(it);
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
  auto h = open_handle(path, known_size, expect != nullptr);
  if (!h) return nullptr;
  if (expect) {
    h->stamp_size = expect->size;
#ifdef _WIN32
    h->id1 = expect->id1;  // the identity IS the path's stamp (see same_file)
    h->id2 = expect->id2;
#endif
  }
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
  if (!c || c->reader_mode) return fill();
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
  if (length && detail::source_read_observer) detail::source_read_observer(h.path);
  detail::source_read_bytes.fetch_add(length, std::memory_order_relaxed);
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
    if (!ov.hEvent) throw IoError("could not prepare a read of " + h.path);
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
std::shared_ptr<Handle> open_handle(const std::string& text, std::uint64_t known_size,
                                    bool want_ids) {
  auto h = std::make_shared<Handle>();
  h->path = text;
  const fs::path path = detail::utf8_path(text);
#ifdef _WIN32
  h->fd = CreateFileW(path.c_str(), GENERIC_READ,
                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
                      nullptr);  // (a folder fails here)
  if (h->fd == INVALID_HANDLE_VALUE) {
    last_open_error = static_cast<int>(GetLastError());
    return nullptr;
  }
  if (known_size != UINT64_MAX) {
    h->size = known_size;
  } else {
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h->fd, &sz)) return nullptr;  // ~Handle closes
    h->size = static_cast<std::uint64_t>(sz.QuadPart);
  }
#else
  h->fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (h->fd < 0) {
    last_open_error = errno;
    return nullptr;
  }
  if (known_size != UINT64_MAX && !want_ids) {
    h->size = known_size;
  } else {
    struct stat st {};
    if (::fstat(h->fd, &st) != 0 || !S_ISREG(st.st_mode)) return nullptr;  // ~Handle closes
    h->size = static_cast<std::uint64_t>(st.st_size);
    h->id1 = static_cast<std::uint64_t>(st.st_dev);
    h->id2 = static_cast<std::uint64_t>(st.st_ino);
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

/// The stamp of the regular file at `path` (not ok: missing, or not a regular
/// file). ONE query.
Stamp stat_stamp(const fs::path& path) {
  Stamp s;
#ifdef _WIN32
  WIN32_FILE_ATTRIBUTE_DATA d{};
  if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d)) return s;
  if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return s;
  s.size = (static_cast<std::uint64_t>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
  s.id1 = (static_cast<std::uint64_t>(d.ftCreationTime.dwHighDateTime) << 32) |
          d.ftCreationTime.dwLowDateTime;
  s.id2 = (static_cast<std::uint64_t>(d.ftLastWriteTime.dwHighDateTime) << 32) |
          d.ftLastWriteTime.dwLowDateTime;
  s.ok = true;
#else
  struct stat st {};
  if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return s;
  s.size = static_cast<std::uint64_t>(st.st_size);
  s.id1 = static_cast<std::uint64_t>(st.st_dev);
  s.id2 = static_cast<std::uint64_t>(st.st_ino);
  s.ok = true;
#endif
  return s;
}

/// Close a cache's handle for `path` (a file that is gone).
void drop(HandleCache& c, const std::string& path) {
  std::shared_ptr<Handle> doomed;
  std::lock_guard<std::mutex> lk(c.mu);
  auto it = c.index.find(path);
  if (it == c.index.end()) return;
  doomed = std::move(it->second->second);
  c.lru.erase(it->second);
  c.index.erase(it);
}

}  // namespace

namespace detail {

std::size_t default_open_files() { return 0; }

std::size_t suggested_open_files() {
#ifdef _WIN32
  return 128;
#else
  std::size_t cap = 128;
  struct rlimit rl {};
  if (::getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY)
    cap = std::min<std::size_t>(cap, static_cast<std::size_t>(rl.rlim_cur) / 4);
  return cap;
#endif
}

namespace {
std::mutex g_registry_mu;
std::vector<std::weak_ptr<HandleCache>> g_reader_caches;  // every live Reader cache
}  // namespace

std::shared_ptr<HandleCache> make_reader_cache(std::size_t cap) {
  auto c = std::make_shared<HandleCache>();
  c->reader_mode = true;
  c->cap = cap;
  std::lock_guard<std::mutex> lk(g_registry_mu);
  std::erase_if(g_reader_caches, [](const auto& w) { return w.expired(); });
  g_reader_caches.push_back(c);
  return c;
}

void release_all_reader_caches() {
  std::vector<std::shared_ptr<HandleCache>> live;
  {
    std::lock_guard<std::mutex> lk(g_registry_mu);
    for (const auto& w : g_reader_caches)
      if (auto c = w.lock()) live.push_back(std::move(c));
  }
  for (auto& c : live) release_cache(*c);  // no registry lock held
}

void set_cache_cap(HandleCache& c, std::size_t cap) {
  std::vector<std::shared_ptr<Handle>> doomed;  // closed outside the lock
  std::lock_guard<std::mutex> lk(c.mu);
  c.cap = cap;
  while (c.lru.size() > c.cap) {
    doomed.push_back(std::move(c.lru.back().second));
    c.index.erase(c.lru.back().first);
    c.lru.pop_back();
  }
}

std::size_t cache_cap(HandleCache& c) {
  std::lock_guard<std::mutex> lk(c.mu);
  return c.cap;
}

void release_cache(HandleCache& c) {
  std::vector<std::shared_ptr<Handle>> doomed;
  std::lock_guard<std::mutex> lk(c.mu);
  for (auto& e : c.lru) doomed.push_back(std::move(e.second));
  c.lru.clear();
  c.index.clear();
}

std::size_t cache_open_count(HandleCache& c) {
  std::lock_guard<std::mutex> lk(c.mu);
  return c.lru.size();
}

bool kept_pread(HandleCache& c, const std::string& path, std::size_t offset, std::size_t length,
                std::vector<ui1>& out, bool& past_end) {
  {
    std::lock_guard<std::mutex> lk(c.mu);
    if (c.cap == 0) return false;
  }
  const fs::path native = utf8_path(path);
  for (int attempt = 0;; ++attempt) {
    // ONE query: the file's size now and which file the path names. It replaces
    // the plain path's size query and proves the kept handle is still this file.
    const Stamp now = stat_stamp(native);
    if (!now.ok) {
      drop(c, path);
      return false;  // the plain read raises its usual error
    }
    auto h = acquire(c, path, &now);
    if (!h && out_of_descriptors(last_open_error)) {
      // Out of descriptors: let go of what kept files hold (this Session's, then
      // any other Reader's in the process) and try once more; never leave the
      // process unable to open files because of handles this cache keeps.
      release_cache(c);
      h = acquire(c, path, &now);
      if (!h && out_of_descriptors(last_open_error)) {
        release_all_reader_caches();
        h = acquire(c, path, &now);
      }
    }
    if (!h) return false;
    // The size is the PATH's, from the same query as the identity (a handle's
    // size can differ from the file the path names). Too short: the plain read
    // decides (it raises the old error, or reads what a lagging directory entry
    // does not show yet).
    const std::uint64_t size = now.size;
    if (offset > size || length > size - offset) return false;
    try {
      out.assign(length, 0);
      if (length && !pread_full(*h, offset, length, out.data())) {
        evict(c, path, h);  // shrank since the size was taken: the plain read decides
        out.clear();
        return false;
      }
      past_end = false;
      return true;
    } catch (const StaleRead& e) {
      evict(c, path, h);
      if (attempt == 1) throw IoError(e.what + " (also after reopening)");
    }
  }
}

}  // namespace detail

DirectorySource::DirectorySource(std::string root) : root_(std::move(root)) {}

DirectorySource::DirectorySource(std::string root, std::shared_ptr<detail::HandleCache> cache)
    : root_(std::move(root)), cache_(std::move(cache)) {}

namespace {
/// Run a plain (open-per-call) read of a Reader's source; when it fails while
/// this Session or another Reader keeps files open, the failure may be the
/// process being out of descriptors: let go of every kept file and try once more.
template <class F>
auto healing(const std::shared_ptr<detail::HandleCache>& c, F&& fn) -> decltype(fn()) {
  if (!c || !c->reader_mode || detail::cache_cap(*c) == 0) return fn();
  try {
    return fn();
  } catch (const IoError&) {
    detail::release_all_reader_caches();
    return fn();  // a second failure is the real one
  }
}
}  // namespace

detail::HandleCache* DirectorySource::memo_cache() const {
  return cache_ && !cache_->reader_mode ? cache_.get() : nullptr;
}

std::vector<ui1> DirectorySource::read_extent(const std::string& rel, std::size_t offset,
                                              std::size_t length, bool& past_end) const {
  if (cache_ && cache_->reader_mode) {
    const std::string path = join(rel);
    if (path.size() > 5 && path.compare(path.size() - 5, 5, ".tdat") == 0) {
      std::vector<ui1> out;
      if (detail::kept_pread(*cache_, path, offset, length, out, past_end)) return out;
    }
  }
  return SessionSource::read_extent(rel, offset, length, past_end);
}

void DirectorySource::set_open_files(std::size_t n) const {
  if (cache_ && cache_->reader_mode) detail::set_cache_cap(*cache_, n);
}

std::size_t DirectorySource::open_files() const {
  return cache_ && cache_->reader_mode ? detail::cache_cap(*cache_) : 0;
}

void DirectorySource::release_open_files() const {
  if (cache_ && cache_->reader_mode) detail::release_cache(*cache_);
}

std::size_t DirectorySource::open_file_count() const {
  return cache_ && cache_->reader_mode ? detail::cache_open_count(*cache_) : 0;
}

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
      memo_cache(), [](HandleCache& c) -> auto& { return c.dir_memo; }, path, list);
}

bool DirectorySource::exists(const std::string& rel) const {
  const std::string path = join(rel);
  HandleCache* const c = memo_cache();
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
      memo_cache(), [](HandleCache& c) -> auto& { return c.size_memo; }, path,
      [&] { return static_cast<std::uint64_t>(fs::file_size(detail::utf8_path(path))); });
}

namespace {
bool memo_path(const std::string& path) {
  for (const char* e : {".tmet", ".tidx", ".ridx"})
    if (path.size() > 5 && path.compare(path.size() - 5, 5, e) == 0) return true;
  return false;
}
}  // namespace

/// The cached whole contents of a small metadata file, or null. With `fill`,
/// read it once now (when small enough and the memo has room) so every later
/// read of any part of it costs no request.
std::shared_ptr<const std::vector<ui1>> DirectorySource::memo_get(const std::string& path,
                                                                    const std::string& rel,
                                                                    bool fill) const {
  HandleCache* const c = memo_cache();
  if (!c || !memo_path(path)) return nullptr;
  {
    std::lock_guard<std::mutex> lk(c->mu);
    const auto it = c->bytes_memo.find(path);
    if (it != c->bytes_memo.end()) return it->second;
  }
  if (!fill) return nullptr;
  const std::uint64_t size = file_size(rel);
  if (size > HandleCache::MEMO_FILE_BYTES) return nullptr;
  {
    // Over budget: no whole-file read that read_all could not keep; the
    // caller does its normal small read.
    std::lock_guard<std::mutex> lk(c->mu);
    if (c->bytes_memo_total + size > detail::memo_total_budget) return nullptr;
  }
  read_all(rel);  // memoizes when the budget allows
  std::lock_guard<std::mutex> lk(c->mu);
  const auto it = c->bytes_memo.find(path);
  return it == c->bytes_memo.end() ? nullptr : it->second;
}

std::vector<ui1> DirectorySource::read_all(const std::string& rel) const {
  return healing(cache_, [&] { return read_all_once(rel); });
}

std::vector<ui1> DirectorySource::read_all_once(const std::string& rel) const {
  const std::string path = join(rel);
  std::vector<ui1> buf;
  HandleCache* const mc = memo_cache();
  const auto ends = [&](const char* e) { return path.size() > 5 && path.compare(path.size() - 5, 5, e) == 0; };
  const bool memoizable = mc && (ends(".tmet") || ends(".tidx") || ends(".ridx"));
  if (memoizable) {
    std::lock_guard<std::mutex> lk(mc->mu);
    const auto it = mc->bytes_memo.find(path);
    if (it != mc->bytes_memo.end()) return *it->second;
  }
  struct Remember {
    HandleCache* c;
    const std::string& path;
    const std::vector<ui1>& buf;
    bool on;
    ~Remember() {
      if (!on || buf.size() > HandleCache::MEMO_FILE_BYTES) return;
      std::lock_guard<std::mutex> lk(c->mu);
      if (c->bytes_memo_total + buf.size() > detail::memo_total_budget) return;
      if (c->bytes_memo.emplace(path, std::make_shared<const std::vector<ui1>>(buf)).second)
        c->bytes_memo_total += buf.size();
    }
  };
  if (with_handle(memo_cache(), path, [&](const Handle& h) {
        buf.assign(static_cast<std::size_t>(h.size), 0);
        if (!pread_full(h, 0, buf.size(), buf.data())) throw IoError("short read: " + path);
      })) {
    Remember{mc, path, buf, memoizable};
    return buf;
  }
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
  return healing(cache_, [&] { return read_head_once(rel, max_bytes); });
}

std::vector<ui1> DirectorySource::read_head_once(const std::string& rel, std::size_t max_bytes) const {
  // Open + one read: no seek to the end for the size (read_all's tellg), so
  // the fixed-length .tmet costs one request whatever padding follows it.
  const std::string path = join(rel);
  std::vector<ui1> buf;
  if (const auto m = memo_get(path, rel, /*fill=*/false))
    return std::vector<ui1>(m->begin(), m->begin() + std::min(m->size(), max_bytes));
  if (with_handle(memo_cache(), path, [&](const Handle& h) {
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
  return healing(cache_, [&] { return read_range_once(rel, offset, length); });
}

std::vector<ui1> DirectorySource::read_range_once(const std::string& rel, std::size_t offset,
                                                  std::size_t length) const {
  const std::string path = join(rel);
  std::vector<ui1> buf;
  if (const auto m = memo_get(path, rel, /*fill=*/true)) {
    if (offset > m->size() || length > m->size() - offset) throw IoError("short read: " + path);
    return std::vector<ui1>(m->begin() + offset, m->begin() + offset + length);
  }
  if (with_handle(memo_cache(), path, [&](const Handle& h) {
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

namespace {
std::shared_ptr<const SessionSource> open_source_with(const std::string& path,
                                                      std::shared_ptr<detail::HandleCache> cache) {
  // Session naming is a safety contract: only clearly-marked paths are ever
  // opened, so a stray directory or file cannot be misread as a session.
  const fs::path native = detail::utf8_path(path);  // UTF-8 in, wide on Windows
  if (fs::is_directory(native)) {
    if (!path_has_suffix(path, ".mefd"))
      throw IoError("session directory must end with .mefd: " + path);
    return cache ? std::make_shared<DirectorySource>(path, std::move(cache))
                 : std::make_shared<DirectorySource>(path);
  }
  if (fs::is_regular_file(native)) {
    if (!path_has_suffix(path, ".mefd.tar"))
      throw IoError("session archive must end with .mefd.tar: " + path);
    return std::make_shared<TarSource>(path, std::move(cache));
  }
  throw IoError("session path is not a directory or tar archive: " + path);
}
}  // namespace

std::shared_ptr<const SessionSource> open_session_source(const std::string& path) {
  return open_source_with(path, nullptr);
}

std::shared_ptr<const SessionSource> open_session_source(const std::string& path,
                                                         std::size_t open_files) {
  return open_source_with(path, detail::make_reader_cache(open_files));
}

}  // namespace mef3io
