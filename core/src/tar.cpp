// mef3io — uncompressed tar session archives: TOC parsing, random-access
// source, and deterministic ustar creation.
#include "mef3io/tar.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "mef3io/errors.hpp"

namespace fs = std::filesystem;

namespace mef3io {
namespace {

constexpr std::size_t BLOCK = 512;

// ustar header field offsets (all within one 512-byte block).
constexpr std::size_t H_NAME = 0, H_MODE = 100, H_UID = 108, H_GID = 116, H_SIZE = 124,
                      H_MTIME = 136, H_CHKSUM = 148, H_TYPE = 156, H_MAGIC = 257, H_DEVMAJOR = 329,
                      H_DEVMINOR = 337, H_PREFIX = 345;
constexpr std::size_t H_NAME_LEN = 100, H_PREFIX_LEN = 155, H_SIZE_LEN = 12, H_CHKSUM_LEN = 8;

std::uint64_t round_up_block(std::uint64_t n) { return (n + BLOCK - 1) / BLOCK * BLOCK; }

std::string field_str(const unsigned char* h, std::size_t off, std::size_t len) {
  const char* p = reinterpret_cast<const char*>(h + off);
  return std::string(p, strnlen(p, len));
}

std::uint64_t parse_octal(const unsigned char* p, std::size_t len) {
  std::size_t i = 0;
  while (i < len && (p[i] == ' ' || p[i] == '\0')) ++i;
  std::uint64_t v = 0;
  for (; i < len && p[i] >= '0' && p[i] <= '7'; ++i) v = v * 8 + (p[i] - '0');
  return v;
}

// Size field: octal, or base-256 (high bit of the first byte set) for
// members >= 8 GiB — GNU/bsdtar/star all emit and accept this.
std::uint64_t parse_size(const unsigned char* p) {
  if (p[0] & 0x80) {
    std::uint64_t v = p[0] & 0x7F;
    for (std::size_t i = 1; i < H_SIZE_LEN; ++i) v = (v << 8) | p[i];
    return v;
  }
  return parse_octal(p, H_SIZE_LEN);
}

// Verify the header checksum: the chksum field counts as spaces. Historic
// implementations summed signed bytes; accept either.
bool checksum_ok(const unsigned char* h) {
  const std::uint64_t stored = parse_octal(h + H_CHKSUM, H_CHKSUM_LEN);
  std::int64_t sum_unsigned = 0, sum_signed = 0;
  for (std::size_t i = 0; i < BLOCK; ++i) {
    const unsigned char b = (i >= H_CHKSUM && i < H_CHKSUM + H_CHKSUM_LEN) ? ' ' : h[i];
    sum_unsigned += b;
    sum_signed += static_cast<signed char>((i >= H_CHKSUM && i < H_CHKSUM + H_CHKSUM_LEN) ? ' '
                                                                                          : h[i]);
  }
  return stored == static_cast<std::uint64_t>(sum_unsigned) ||
         static_cast<std::int64_t>(stored) == sum_signed;
}

void reject_compressed(const unsigned char* p, const std::string& path) {
  const char* kind = nullptr;
  if (p[0] == 0x1F && p[1] == 0x8B) kind = "gzip";
  else if (p[0] == 'B' && p[1] == 'Z' && p[2] == 'h') kind = "bzip2";
  else if (memcmp(p, "\xFD" "7zXZ", 5) == 0) kind = "xz";
  else if (p[0] == 0x28 && p[1] == 0xB5 && p[2] == 0x2F && p[3] == 0xFD) kind = "zstd";
  else if (p[0] == 'P' && p[1] == 'K') kind = "zip";
  if (kind)
    throw FormatError(std::string(kind) + "-compressed archive not supported (mef3io reads only "
                      "uncompressed tar; decompress it first): " + path);
}

// "path/", "./path" and "/path" all mean "path" as a member key.
std::string normalize_member_name(std::string s) {
  while (s.rfind("./", 0) == 0) s.erase(0, 2);
  while (!s.empty() && s.front() == '/') s.erase(0, 1);
  while (!s.empty() && s.back() == '/') s.pop_back();
  return s;
}

// PAX extended header body: repeated "<len> <key>=<value>\n" records where
// <len> is the decimal byte length of the whole record.
void parse_pax(const std::vector<char>& data, std::string& path, bool& have_path,
               std::uint64_t& size, bool& have_size) {
  std::size_t pos = 0;
  while (pos < data.size()) {
    std::size_t len = 0, i = pos;
    while (i < data.size() && data[i] >= '0' && data[i] <= '9') len = len * 10 + (data[i++] - '0');
    if (i >= data.size() || data[i] != ' ' || len < (i - pos) + 2 || pos + len > data.size()) break;
    const std::string record(data.data() + i + 1, len - (i - pos) - 2);  // strip "<len> " and "\n"
    const auto eq = record.find('=');
    if (eq != std::string::npos) {
      const std::string key = record.substr(0, eq), value = record.substr(eq + 1);
      if (key == "path") {
        path = value;
        have_path = true;
      } else if (key == "size") {
        size = std::stoull(value);
        have_size = true;
      }
    }
    pos += len;
  }
}

// Session root = the archive's single top-level directory (the usual
// "name.mefd/..." layout). Archives tarred from inside the session dir have
// ".timd" trees (or .rdat files) at top level — then the root is "".
std::string detect_session_root(const TarIndex& idx) {
  std::set<std::string> tops;
  bool top_level_file = false;
  for (const auto& [p, m] : idx.files()) {
    const auto slash = p.find('/');
    if (slash == std::string::npos)
      top_level_file = true;
    else
      tops.insert(p.substr(0, slash));
  }
  if (!top_level_file && tops.size() == 1 && !tops.begin()->ends_with(".timd"))
    return *tops.begin();
  return "";
}

// Extraction writes member paths to the real filesystem; refuse traversal.
void check_member_safe(const std::string& rel, const std::string& archive) {
  std::size_t pos = 0;
  while (pos <= rel.size()) {
    const auto next = rel.find('/', pos);
    const std::string part = rel.substr(pos, next == std::string::npos ? next : next - pos);
    if (part == "..") throw FormatError("unsafe member path in archive " + archive + ": " + rel);
    if (next == std::string::npos) break;
    pos = next + 1;
  }
}

}  // namespace

TarIndex::TarIndex(std::string tar_path) : path_(std::move(tar_path)) {
  std::ifstream f(path_, std::ios::binary | std::ios::ate);
  if (!f) throw IoError("cannot open archive: " + path_);
  const auto archive_size = static_cast<std::uint64_t>(f.tellg());
  f.seekg(0);

  std::array<unsigned char, BLOCK> h{};
  std::string gnu_longname;
  std::string pax_path;
  std::uint64_t pax_size = 0;
  bool have_pax_path = false, have_pax_size = false;
  bool first = true;

  std::uint64_t pos = 0;
  while (pos + BLOCK <= archive_size) {
    f.seekg(static_cast<std::streamoff>(pos));
    if (!f.read(reinterpret_cast<char*>(h.data()), BLOCK))
      throw FormatError("truncated tar archive: " + path_);
    if (std::all_of(h.begin(), h.end(), [](unsigned char b) { return b == 0; })) break;

    if (first) reject_compressed(h.data(), path_);
    if (memcmp(h.data() + H_MAGIC, "ustar", 5) != 0)
      throw FormatError(first ? "not a tar archive: " + path_
                              : "bad tar member header at offset " + std::to_string(pos) + ": " +
                                    path_);
    if (!checksum_ok(h.data()))
      throw FormatError("tar header checksum mismatch at offset " + std::to_string(pos) + ": " +
                        path_);
    first = false;

    const std::uint64_t header_size = parse_size(h.data() + H_SIZE);
    const char type = static_cast<char>(h[H_TYPE]);
    const std::uint64_t data_off = pos + BLOCK;

    // Metadata members carry payload for the NEXT member; consume and go on.
    if (type == 'L' || type == 'K' || type == 'x' || type == 'g') {
      if (data_off + header_size > archive_size)
        throw FormatError("truncated tar archive: " + path_);
      if (type == 'L' || type == 'x') {
        std::vector<char> data(static_cast<std::size_t>(header_size));
        f.seekg(static_cast<std::streamoff>(data_off));
        if (header_size && !f.read(data.data(), static_cast<std::streamsize>(header_size)))
          throw FormatError("truncated tar archive: " + path_);
        if (type == 'L')
          gnu_longname.assign(data.data(), strnlen(data.data(), data.size()));
        else
          parse_pax(data, pax_path, have_pax_path, pax_size, have_pax_size);
      }
      pos = data_off + round_up_block(header_size);
      continue;
    }

    std::string name = field_str(h.data(), H_NAME, H_NAME_LEN);
    const std::string prefix = field_str(h.data(), H_PREFIX, H_PREFIX_LEN);
    if (!prefix.empty()) name = prefix + "/" + name;
    if (!gnu_longname.empty()) name = gnu_longname;
    if (have_pax_path) name = pax_path;
    const std::uint64_t size = have_pax_size ? pax_size : header_size;
    gnu_longname.clear();
    have_pax_path = have_pax_size = false;

    const bool is_dir = (type == '5') || (!name.empty() && name.back() == '/');
    name = normalize_member_name(name);
    if (!name.empty() && name != ".") {
      if (is_dir) {
        dirs_.insert(name);
      } else if (type == '0' || type == '\0' || type == '7') {
        if (data_off + size > archive_size) throw FormatError("truncated tar archive: " + path_);
        files_[name] = TarMember{data_off, size};
      }
      // links, fifos, devices: skipped
    }
    pos = data_off + round_up_block(is_dir ? 0 : size);
  }
}

TarSource::TarSource(const std::string& tar_path) : tar_path_(tar_path) {
  TarIndex idx(tar_path);
  if (idx.files().empty()) throw FormatError("tar archive contains no files: " + tar_path);

  root_ = detect_session_root(idx);

  const std::string prefix = root_.empty() ? std::string() : root_ + "/";
  for (const auto& [p, m] : idx.files()) {
    if (!prefix.empty() && p.compare(0, prefix.size(), prefix) != 0) continue;
    files_.emplace(p.substr(prefix.size()), m);
  }
  for (const auto& d : idx.dirs()) {
    if (d == root_) continue;
    if (prefix.empty())
      dirs_.insert(d);
    else if (d.compare(0, prefix.size(), prefix) == 0)
      dirs_.insert(d.substr(prefix.size()));
  }
  // Directory entries are optional in a tar; imply parents from file paths.
  dirs_.insert("");
  for (const auto& [p, m] : files_)
    for (auto slash = p.find('/'); slash != std::string::npos; slash = p.find('/', slash + 1))
      dirs_.insert(p.substr(0, slash));
}

std::vector<SourceDirEntry> TarSource::list_dir(const std::string& rel) const {
  if (!rel.empty() && dirs_.find(rel) == dirs_.end())
    throw IoError("no such directory in archive: " + describe(rel));
  const std::string prefix = rel.empty() ? std::string() : rel + "/";
  std::map<std::string, bool> children;  // leaf name -> is_dir
  for (const auto& d : dirs_) {
    if (d.empty() || d.compare(0, prefix.size(), prefix) != 0) continue;
    const std::string leaf = d.substr(prefix.size());
    if (!leaf.empty() && leaf.find('/') == std::string::npos) children[leaf] = true;
  }
  for (const auto& [p, m] : files_) {
    if (p.compare(0, prefix.size(), prefix) != 0) continue;
    const std::string leaf = p.substr(prefix.size());
    if (!leaf.empty() && leaf.find('/') == std::string::npos) children.emplace(leaf, false);
  }
  std::vector<SourceDirEntry> out;
  out.reserve(children.size());
  for (const auto& [name, is_dir] : children) out.push_back({name, is_dir});
  return out;
}

bool TarSource::exists(const std::string& rel) const {
  return rel.empty() || files_.count(rel) != 0 || dirs_.count(rel) != 0;
}

const TarMember& TarSource::member(const std::string& rel) const {
  auto it = files_.find(rel);
  if (it == files_.end()) throw IoError("no such file in archive: " + describe(rel));
  return it->second;
}

std::uint64_t TarSource::file_size(const std::string& rel) const { return member(rel).size; }

std::vector<ui1> TarSource::read_all(const std::string& rel) const {
  return read_range(rel, 0, static_cast<std::size_t>(member(rel).size));
}

std::vector<ui1> TarSource::read_range(const std::string& rel, std::size_t offset,
                                       std::size_t length) const {
  const TarMember& m = member(rel);
  if (offset + length > m.size) throw IoError("read past end of archived file: " + describe(rel));
  std::ifstream f(tar_path_, std::ios::binary);
  if (!f) throw IoError("cannot open archive: " + tar_path_);
  f.seekg(static_cast<std::streamoff>(m.offset + offset));
  std::vector<ui1> buf(length);
  if (length && !f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(length)))
    throw IoError("short read: " + describe(rel));
  return buf;
}

std::string TarSource::describe(const std::string& rel) const {
  std::string member_path = root_;
  if (!rel.empty()) member_path += member_path.empty() ? rel : "/" + rel;
  return member_path.empty() ? tar_path_ : tar_path_ + "::" + member_path;
}

// ---------------------------------------------------------------------------
// Archive creation
// ---------------------------------------------------------------------------

namespace {

// Write v as (len-1) zero-padded octal digits + NUL.
void put_octal(char* dst, std::size_t len, std::uint64_t v) {
  dst[len - 1] = '\0';
  for (std::size_t i = len - 1; i-- > 0;) {
    dst[i] = static_cast<char>('0' + (v & 7));
    v >>= 3;
  }
}

std::array<char, BLOCK> make_header(const std::string& member_path, std::uint64_t size,
                                    bool is_dir) {
  std::array<char, BLOCK> h{};
  std::string name = member_path;
  if (is_dir) name += '/';

  std::string prefix;
  if (name.size() > H_NAME_LEN) {
    // ustar name+prefix split: prefix "/" name with prefix <= 155, name <= 100.
    std::size_t split = std::string::npos;
    for (auto slash = name.find('/'); slash != std::string::npos; slash = name.find('/', slash + 1))
      if (slash <= H_PREFIX_LEN && name.size() - slash - 1 <= H_NAME_LEN) {
        split = slash;
        break;
      }
    if (split == std::string::npos)
      throw IoError("path too long for a ustar archive member: " + member_path);
    prefix = name.substr(0, split);
    name = name.substr(split + 1);
  }
  memcpy(h.data() + H_NAME, name.data(), name.size());
  memcpy(h.data() + H_PREFIX, prefix.data(), prefix.size());

  put_octal(h.data() + H_MODE, 8, is_dir ? 0755 : 0644);
  put_octal(h.data() + H_UID, 8, 0);
  put_octal(h.data() + H_GID, 8, 0);
  if (size < (1ULL << 33)) {
    put_octal(h.data() + H_SIZE, H_SIZE_LEN, size);
  } else {
    // base-256 for members >= 8 GiB (large .tdat files are realistic).
    auto* p = reinterpret_cast<unsigned char*>(h.data() + H_SIZE);
    p[0] = 0x80;
    for (std::size_t i = H_SIZE_LEN; i-- > 1;) {
      p[i] = static_cast<unsigned char>(size & 0xFF);
      size >>= 8;
    }
  }
  put_octal(h.data() + H_MTIME, 12, 0);  // zeroed: deterministic archives
  h[H_TYPE] = is_dir ? '5' : '0';
  memcpy(h.data() + H_MAGIC, "ustar", 6);
  h[H_MAGIC + 6] = '0';
  h[H_MAGIC + 7] = '0';
  put_octal(h.data() + H_DEVMAJOR, 8, 0);
  put_octal(h.data() + H_DEVMINOR, 8, 0);

  unsigned sum = 0;
  memset(h.data() + H_CHKSUM, ' ', H_CHKSUM_LEN);
  for (char c : h) sum += static_cast<unsigned char>(c);
  put_octal(h.data() + H_CHKSUM, 7, sum);  // 6 digits + NUL, then a space
  h[H_CHKSUM + 7] = ' ';
  return h;
}

void write_member_data(std::ofstream& out, const std::string& file_path, std::uint64_t size) {
  std::ifstream in(file_path, std::ios::binary);
  if (!in) throw IoError("cannot open file: " + file_path);
  std::vector<char> buf(1 << 20);
  std::uint64_t remaining = size;
  while (remaining) {
    const auto chunk = static_cast<std::streamsize>(std::min<std::uint64_t>(buf.size(), remaining));
    if (!in.read(buf.data(), chunk))
      throw IoError("file changed while archiving (short read): " + file_path);
    out.write(buf.data(), chunk);
    remaining -= static_cast<std::uint64_t>(chunk);
  }
  const std::uint64_t pad = round_up_block(size) - size;
  static const std::array<char, BLOCK> zeros{};
  out.write(zeros.data(), static_cast<std::streamsize>(pad));
}

}  // namespace

std::string archive_session(const std::string& session_dir, const std::string& tar_path,
                            bool overwrite) {
  fs::path src(session_dir);
  if (src.filename().empty()) src = src.parent_path();  // tolerate a trailing '/'
  if (!fs::is_directory(src)) throw IoError("session path is not a directory: " + session_dir);
  if (!path_has_suffix(src.string(), ".mefd"))
    throw IoError("session directory must end with .mefd: " + session_dir);
  if (!tar_path.empty() && !path_has_suffix(tar_path, ".mefd.tar"))
    throw IoError("session archive must end with .mefd.tar: " + tar_path);

  const std::string out_path = tar_path.empty() ? src.string() + ".tar" : tar_path;
  if (fs::exists(out_path)) {
    if (!overwrite) throw IoError("archive target already exists (pass overwrite=true): " + out_path);
    if (fs::is_directory(out_path)) throw IoError("archive target is a directory: " + out_path);
  }
  {
    // A target inside the source would archive (a growing) itself.
    const std::string s = fs::weakly_canonical(src).generic_string() + "/";
    const std::string o = fs::weakly_canonical(out_path).generic_string();
    if (o.compare(0, s.size(), s) == 0)
      throw IoError("archive target lies inside the session directory: " + out_path);
  }

  const std::string root_name = src.filename().generic_string();
  std::vector<std::pair<std::string, bool>> entries;  // rel path, is_dir
  for (const auto& e : fs::recursive_directory_iterator(src)) {
    if (!e.is_directory() && !e.is_regular_file()) continue;
    entries.emplace_back(fs::relative(e.path(), src).generic_string(), e.is_directory());
  }
  std::sort(entries.begin(), entries.end());

  const std::string tmp_path = out_path + ".tmp";
  try {
    std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
    if (!out) throw IoError("cannot create archive: " + tmp_path);

    auto root_header = make_header(root_name, 0, true);
    out.write(root_header.data(), BLOCK);
    for (const auto& [rel, is_dir] : entries) {
      const std::string member = root_name + "/" + rel;
      const std::string abs = (src / rel).string();
      const std::uint64_t size = is_dir ? 0 : static_cast<std::uint64_t>(fs::file_size(abs));
      auto header = make_header(member, size, is_dir);
      out.write(header.data(), BLOCK);
      if (!is_dir) write_member_data(out, abs, size);
    }
    static const std::array<char, BLOCK> zeros{};
    out.write(zeros.data(), BLOCK);
    out.write(zeros.data(), BLOCK);
    out.flush();
    if (!out) throw IoError("write failed: " + tmp_path);
  } catch (...) {
    std::error_code ec;
    fs::remove(tmp_path, ec);
    throw;
  }
  if (overwrite && fs::exists(out_path)) fs::remove(out_path);  // Windows rename won't replace
  fs::rename(tmp_path, out_path);
  return out_path;
}

std::string extract_session(const std::string& tar_path, const std::string& dest_dir,
                            bool overwrite) {
  if (!path_has_suffix(tar_path, ".mefd.tar"))
    throw IoError("session archive must end with .mefd.tar: " + tar_path);
  if (!dest_dir.empty() && !path_has_suffix(dest_dir, ".mefd"))
    throw IoError("session directory must end with .mefd: " + dest_dir);

  TarIndex idx(tar_path);
  if (idx.files().empty()) throw FormatError("tar archive contains no files: " + tar_path);

  // Default target strips ".tar": "name.mefd.tar" -> "name.mefd".
  const std::string dest =
      dest_dir.empty() ? tar_path.substr(0, tar_path.size() - 4) : dest_dir;
  if (fs::exists(dest)) {
    if (!overwrite)
      throw IoError("extraction target already exists (pass overwrite=true): " + dest);
    if (!fs::is_directory(dest))
      throw IoError("extraction target exists and is not a directory: " + dest);
    fs::remove_all(dest);
  }

  // Members are written under `dest` with the session root stripped, so
  // extract_session("name.mefd.tar") reproduces "name.mefd" regardless of how
  // the archive was rooted.
  const std::string root = detect_session_root(idx);
  const std::string prefix = root.empty() ? std::string() : root + "/";
  auto rel_of = [&](const std::string& p) -> std::string {
    if (prefix.empty()) return p;
    if (p == root || p.compare(0, prefix.size(), prefix) != 0) return "";  // skip
    return p.substr(prefix.size());
  };

  std::ifstream in(tar_path, std::ios::binary);
  if (!in) throw IoError("cannot open archive: " + tar_path);

  try {
    fs::create_directories(dest);
    for (const auto& d : idx.dirs()) {
      const std::string rel = rel_of(d);
      if (rel.empty()) continue;
      check_member_safe(rel, tar_path);
      fs::create_directories(fs::path(dest) / fs::path(rel));
    }
    std::vector<char> buf(1 << 20);
    for (const auto& [p, m] : idx.files()) {
      const std::string rel = rel_of(p);
      if (rel.empty()) continue;
      check_member_safe(rel, tar_path);
      const fs::path out_path = fs::path(dest) / fs::path(rel);
      fs::create_directories(out_path.parent_path());
      std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
      if (!out) throw IoError("cannot create file: " + out_path.string());
      in.seekg(static_cast<std::streamoff>(m.offset));
      std::uint64_t remaining = m.size;
      while (remaining) {
        const auto chunk =
            static_cast<std::streamsize>(std::min<std::uint64_t>(buf.size(), remaining));
        if (!in.read(buf.data(), chunk)) throw IoError("truncated tar archive: " + tar_path);
        out.write(buf.data(), chunk);
        remaining -= static_cast<std::uint64_t>(chunk);
      }
      out.flush();
      if (!out) throw IoError("write failed: " + out_path.string());
    }
  } catch (...) {
    std::error_code ec;
    fs::remove_all(dest, ec);  // never leave a half-extracted session behind
    throw;
  }
  return dest;
}

}  // namespace mef3io
