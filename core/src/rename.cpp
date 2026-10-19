// mef3io — rename_session and fix_name_drift (#72, second part). See
// naming.hpp.
//
// One operation, three phases, and a crash in any of them leaves a session
// the next call puts back together:
//
//   1. PREPARE (nothing changes): every file that carries the name is read,
//      its CRCs checked and, for a .tmet, its section 2 decrypted, so a
//      damaged file or a missing password refuses before the first write.
//      The old headers (and whole .tmet files: 16 KB) are copied to the
//      backup folder and a JOURNAL listing the planned steps is written and
//      flushed there.
//   2. MARK: the operation marker goes into the session and is flushed with
//      its directory entry. From here every mef3io write refuses.
//   3. CHANGE: headers, then the session-level record files, then the folder;
//      then the marker is removed, and only then the journal.
//
// Marker + journal found later: the operation was interrupted; it is ROLLED
// BACK (the old bytes restored, the renames undone) before anything else.
// Journal without marker: it never started, or it finished; the journal is
// discarded. Marker without journal: refused (operation_marker.hpp).
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <system_error>

#include "durability.hpp"
#include "naming_rules.hpp"
#include "no_replace.hpp"
#include "operation_marker.hpp"

#include "mef3io/byteio.hpp"
#include "mef3io/crc.hpp"
#include "mef3io/crypto.hpp"
#include "mef3io/errors.hpp"
#include "mef3io/headers.hpp"
#include "mef3io/naming.hpp"
#include "mef3io/version.hpp"

namespace fs = std::filesystem;

namespace mef3io {

namespace {

constexpr std::size_t UH = fmt::UNIVERSAL_HEADER_BYTES;
constexpr std::size_t UH_SESSION_NAME = 308, UH_NAME_BYTES = 256;
constexpr std::size_t S2_SESSION_DESC = 2048, S2_DESC_BYTES = 2048;
/// `<name>.mefd` / `<name>.ridx` must fit a 255-byte file name.
constexpr std::size_t MAX_NAME_BYTES = 250;
constexpr const char* JOURNAL_MAGIC = "mef3io-rename-journal 1";

/// Paths from and to UTF-8 strings. fs::path(std::string) and
/// path::string() use the ANSI code page on Windows, which garbles (or
/// throws on) a non-ASCII name; every name here is UTF-8, as stored.
fs::path u8(const std::string& s) {
  return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}
std::string str(const fs::path& p) {
  const std::u8string u = p.u8string();
  return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

bool ends_with(const std::string& s, const std::string& suffix) {
  return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string hex(const std::string& s) {
  static const char* digits = "0123456789abcdef";
  std::string out;
  for (const unsigned char c : s) {
    out += digits[c >> 4];
    out += digits[c & 0xF];
  }
  return out;
}

std::optional<std::string> unhex(const std::string& s) {
  if (s.size() % 2) return std::nullopt;
  std::string out;
  for (std::size_t i = 0; i < s.size(); i += 2) {
    const auto v = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      return -1;
    };
    const int hi = v(s[i]), lo = v(s[i + 1]);
    if (hi < 0 || lo < 0) return std::nullopt;
    out += static_cast<char>(hi * 16 + lo);
  }
  return out;
}

/// The exact entry `name` is in `dir` (by listing: on a case-insensitive file
/// system fs::exists cannot tell "Patient" from "patient").
bool has_entry(const fs::path& dir, const std::string& name) {
  std::error_code ec;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
    if (str(it->path().filename()) == name) return true;
  return false;
}

std::vector<ui1> read_prefix(const fs::path& file, std::size_t n) {
  std::ifstream in(file, std::ios::binary);
  if (!in) throw IoError("cannot open: " + str(file));
  std::vector<ui1> b(n);
  if (!in.read(reinterpret_cast<char*>(b.data()), static_cast<std::streamsize>(n)))
    throw IoError("short read: " + str(file));
  return b;
}

std::vector<ui1> read_whole(const fs::path& file) {
  return read_prefix(file, static_cast<std::size_t>(fs::file_size(file)));
}

void write_new_file(const fs::path& file, std::span<const ui1> bytes) {
  {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) throw IoError("cannot write: " + str(file));
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    detail::finish_stream(out, str(file));
  }
  detail::fsync_file_or_throw(str(file));
}

/// Patch a file's first bytes in place and flush: a header is rewritten
/// without touching the (possibly multi-gigabyte) body.
/// Not flushed here: the operation flushes everything it wrote in one batch
/// (detail::flush_files) before it is complete. Until then the journal and
/// the backups cover a crash.
void overwrite_prefix(const fs::path& file, std::span<const ui1> bytes) {
  {
    std::fstream f(file, std::ios::binary | std::ios::in | std::ios::out);
    if (!f) throw IoError("cannot open for header update: " + str(file));
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    f.flush();
    if (!f) throw IoError("header update failed: " + str(file));
  }
}

/// Rename an entry of `dir`, never replacing one. A case-only rename (the
/// target "exists" because the file system ignores case) goes through
/// `temp`, a 32-character token name.
void rename_entry(const fs::path& dir, const std::string& from, const std::string& to,
                  const std::string& temp) {
  if (from == to) return;
  const auto step = [&](const std::string& a, const std::string& b) {
    std::string why;
    switch (detail::rename_no_replace(dir / u8(a), dir / u8(b), why)) {
      case detail::NoReplaceResult::Renamed: return;
      case detail::NoReplaceResult::TargetExists:
        throw IoError("the target already exists: " + str(dir / u8(b)));
      case detail::NoReplaceResult::Unsupported:
        throw IoError("this file system cannot rename without the risk of replacing a file (" +
                      why + ")");
      case detail::NoReplaceResult::Failed:
        throw IoError("cannot rename " + str(dir / u8(a)) + " (" + why + ")");
    }
  };
  std::error_code ec;
  if (fs::exists(dir / u8(to), ec) && fs::equivalent(dir / u8(from), dir / u8(to), ec)) {
    step(from, temp);
    step(temp, to);
  } else {
    step(from, to);
  }
  detail::fsync_directory(dir);
}

// --- the journal ------------------------------------------------------------

struct Journal {
  std::string id, operation;
  fs::path parent;                       ///< the folder holding the session
  std::string old_folder, new_folder;    ///< entry names, e.g. "a.mefd"
  std::string temp_folder;               ///< "<token>.mefd"
  struct Rename {
    std::string from, to, temp;          ///< session-root file names
  };
  std::vector<Rename> records;
  std::vector<std::string> files;        ///< session-relative; backed up as <id>/<i>
  fs::path backup_dir;                   ///< where the journal file lives

  fs::path file() const { return backup_dir / (id + ".journal"); }
  /// Every file's backed-up bytes, concatenated in `files` order (one file,
  /// one flush, however many files the session has).
  fs::path backups() const { return backup_dir / (id + ".backup"); }
  std::vector<std::size_t> sizes;        ///< bytes of each file's backup
};

void write_journal(const Journal& j) {
  std::string text = std::string(JOURNAL_MAGIC) + "\nid " + j.id + "\noperation " + j.operation +
                     "\nparent " + hex(str(j.parent)) + "\nold_folder " + hex(j.old_folder) +
                     "\nnew_folder " + hex(j.new_folder) + "\ntemp_folder " + hex(j.temp_folder) +
                     "\n";
  for (const auto& r : j.records)
    text += "record " + hex(r.from) + " " + hex(r.to) + " " + hex(r.temp) + "\n";
  for (std::size_t i = 0; i < j.files.size(); ++i)
    text += "file " + hex(j.files[i]) + " " + std::to_string(j.sizes[i]) + "\n";
  text += "end\n";  // a journal without it was cut short: never acted on
  const fs::path tmp = j.backup_dir / (j.id + ".journal.part");
  write_new_file(tmp, std::span<const ui1>(reinterpret_cast<const ui1*>(text.data()), text.size()));
  std::error_code ec;
  fs::rename(tmp, j.file(), ec);
  if (ec) throw IoError("cannot write the journal " + str(j.file()) + ": " + ec.message());
  detail::fsync_directory(j.backup_dir);
}

std::optional<Journal> read_journal(const fs::path& file) {
  std::ifstream in(file, std::ios::binary);
  std::string line;
  if (!in || !std::getline(in, line) || line != JOURNAL_MAGIC) return std::nullopt;
  Journal j;
  j.backup_dir = file.parent_path();
  bool complete = false;
  while (std::getline(in, line)) {
    if (line == "end") {
      complete = true;
      break;
    }
    std::vector<std::string> w;
    for (std::size_t a = 0; a <= line.size();) {
      const auto b = std::min(line.find(' ', a), line.size());
      w.push_back(line.substr(a, b - a));
      a = b + 1;
    }
    const auto field = [&](std::size_t i) -> std::string {
      if (i >= w.size()) throw std::runtime_error("short line");
      const auto v = unhex(w[i]);
      if (!v) throw std::runtime_error("bad field");
      return *v;
    };
    try {
      if (w[0] == "id" && w.size() == 2) j.id = w[1];
      else if (w[0] == "operation" && w.size() == 2) j.operation = w[1];
      else if (w[0] == "parent") j.parent = u8(field(1));
      else if (w[0] == "old_folder") j.old_folder = field(1);
      else if (w[0] == "new_folder") j.new_folder = field(1);
      else if (w[0] == "temp_folder") j.temp_folder = field(1);
      else if (w[0] == "record") j.records.push_back({field(1), field(2), field(3)});
      else if (w[0] == "file" && w.size() == 3) {
        j.files.push_back(field(1));
        j.sizes.push_back(static_cast<std::size_t>(std::stoull(w[2])));
      }
    } catch (const std::exception&) {
      return std::nullopt;
    }
  }
  if (!complete || j.id.empty() || j.old_folder.empty()) return std::nullopt;
  return j;
}

/// Delete a journal and its backups. `own_dir`: the backup folder is the
/// default one beside the session, removed too once empty (a repair's
/// backups in it stay).
void discard_journal(const Journal& j, bool own_dir) {
  std::error_code ec;
  fs::remove(j.file(), ec);
  fs::remove(j.backups(), ec);
  detail::fsync_directory(j.backup_dir);
  if (own_dir) fs::remove(j.backup_dir, ec);  // only when empty
}

/// Where journals are looked for: the caller's backup folder, or by default
/// every `*.repair-backup` folder beside the session (the session may have
/// been renamed since the journal was written, and the default backup folder
/// is named after the OLD name).
std::vector<fs::path> journal_dirs(const fs::path& parent, const std::string& backup_dir) {
  if (!backup_dir.empty()) return {u8(backup_dir)};
  std::vector<fs::path> out;
  std::error_code ec;
  for (fs::directory_iterator it(parent, ec), end; !ec && it != end; it.increment(ec))
    if (it->is_directory(ec) && ends_with(str(it->path().filename()), ".repair-backup"))
      out.push_back(it->path());
  return out;
}

std::vector<Journal> journals_in(const std::vector<fs::path>& dirs) {
  std::vector<Journal> out;
  for (const auto& d : dirs) {
    std::error_code ec;
    for (fs::directory_iterator it(d, ec), end; !ec && it != end; it.increment(ec))
      if (ends_with(str(it->path().filename()), ".journal"))
        if (auto j = read_journal(it->path())) out.push_back(std::move(*j));
  }
  return out;
}

/// The entry name the journal's session has now, recognised by its marker.
std::optional<std::string> locate(const Journal& j) {
  for (const auto& name : {j.old_folder, j.new_folder, j.temp_folder}) {
    if (name.empty() || !has_entry(j.parent, name)) continue;
    const auto m = detail::read_operation_marker(j.parent / u8(name));
    if (m && m->id == j.id) return name;
  }
  return std::nullopt;
}

/// The byte ranges a rename writes in a file: the CRCs, the stored name, the
/// provenance region and, in a .tmet, the description. Section 2 is
/// AES-ECB, block by block from its start, and the description is
/// block-aligned in it (offset 2048, 2048 bytes), so its ciphertext can be
/// copied back without the password.
bool outside_rename_fields(std::size_t i, bool tmet) {
  constexpr std::size_t DESC = fmt::METADATA_SECTION_2_OFFSET + S2_SESSION_DESC;
  static_assert(S2_SESSION_DESC % 16 == 0 && S2_DESC_BYTES % 16 == 0);
  if (i < (tmet ? 8u : 4u)) return false;
  if (i >= UH_SESSION_NAME && i < UH_SESSION_NAME + UH_NAME_BYTES) return false;
  if (i >= fmt::provenance::REGION_OFFSET && i < UH) return false;
  if (tmet && i >= DESC && i < DESC + S2_DESC_BYTES) return false;
  return true;
}

/// Undo one file's patch. When nothing but the rename touched the file since
/// its backup, the backup goes back exactly. When something else did (an
/// append by a writer that does not know the marker), restoring the backup
/// would erase that write, so only the name and the description are put back
/// and the CRCs recomputed: a logical inverse.
void restore(const fs::path& file, const std::vector<ui1>& saved, bool tmet) {
  std::vector<ui1> cur = tmet ? read_whole(file) : read_prefix(file, UH);
  if (cur == saved) return;
  bool only_ours = cur.size() == saved.size();
  for (std::size_t i = 0; only_ours && i < cur.size(); ++i)
    if (cur[i] != saved[i] && outside_rename_fields(i, tmet)) only_ours = false;
  if (!only_ours) {
    constexpr std::size_t DESC = fmt::METADATA_SECTION_2_OFFSET + S2_SESSION_DESC;
    std::copy_n(saved.begin() + UH_SESSION_NAME, UH_NAME_BYTES, cur.begin() + UH_SESSION_NAME);
    if (tmet) {
      std::copy_n(saved.begin() + DESC, S2_DESC_BYTES, cur.begin() + DESC);
      if (byteio::read<ui4>(cur, 4) != fmt::CRC_NO_ENTRY)
        byteio::write<ui4>(cur, 4, crc::calculate(std::span<const ui1>(cur).subspan(
                                        UH, fmt::METADATA_FILE_BYTES - UH)));
    }
    if (byteio::read<ui4>(cur, 0) != fmt::CRC_NO_ENTRY)
      byteio::write<ui4>(cur, 0, crc::calculate(std::span<const ui1>(cur).subspan(4, UH - 4)));
  }
  const std::vector<ui1>& out = only_ours ? saved : cur;
  if (tmet) detail::write_all_atomic(str(file), out, false);
  else overwrite_prefix(file, std::span<const ui1>(out).first(UH));
}

/// Put everything back as it was before the operation: the record files'
/// names, every saved header (and .tmet), the folder's name. Idempotent, so
/// a rollback interrupted in turn is simply run again.
void roll_back(const Journal& j, bool own_dir) {
  const auto at = locate(j);
  if (!at)
    throw IoError("cannot find the session of the interrupted operation " + j.id + " in " +
                  str(j.parent));
  fs::path dir = j.parent / u8(*at);
  for (const auto& r : j.records) {
    if (has_entry(dir, r.temp)) rename_entry(dir, r.temp, r.from, r.temp);
    else if (!has_entry(dir, r.from) && has_entry(dir, r.to)) rename_entry(dir, r.to, r.from, r.temp);
  }
  const auto blob = read_whole(j.backups());
  std::vector<fs::path> written;
  std::size_t off = 0;
  for (std::size_t i = 0; i < j.files.size(); ++i) {
    if (off + j.sizes[i] > blob.size())
      throw IoError("the backups of operation " + j.id + " are incomplete: " + str(j.backups()));
    const std::vector<ui1> saved(blob.begin() + off, blob.begin() + off + j.sizes[i]);
    off += j.sizes[i];
    written.push_back(dir / u8(j.files[i]));
    restore(written.back(), saved, ends_with(j.files[i], ".tmet"));
  }
  detail::flush_files(written);
  if (*at == j.temp_folder) rename_entry(j.parent, j.temp_folder, j.old_folder, j.temp_folder);
  else if (*at == j.new_folder) rename_entry(j.parent, j.new_folder, j.old_folder, j.temp_folder);
  detail::remove_operation_marker(j.parent / u8(j.old_folder));
  discard_journal(j, own_dir);
}

// --- planning ---------------------------------------------------------------

struct Plan {
  std::string operation;  ///< "rename" / "fix-name-drift"
  fs::path parent;
  std::string folder, new_folder;   ///< entry names
  std::string old_name, new_name;   ///< stored names (descriptions equal to old_name change)
  std::vector<Journal::Rename> records;
  std::vector<std::string> files;   ///< header files to rewrite
  std::vector<std::string> descriptions;  ///< .tmet whose description changes
};

void check_new_name(const std::string& name) {
  detail::check_portable_name(name);
  if (name.size() > MAX_NAME_BYTES)
    throw std::invalid_argument("the new name is " + std::to_string(name.size()) +
                                " bytes; at most 250 fit (<name>.mefd must fit a 255-byte "
                                "file name)");
}

fs::path session_dir(const std::string& path) {
  if (path_has_suffix(path, ".mefd.tar"))
    throw std::invalid_argument(
        "a .mefd.tar archive is renamed with rename_archive (its inside keeps the old name; "
        "extract, rename and archive again to change that): " + path);
  if (!path_has_suffix(path, ".mefd"))
    throw IoError("not a session folder (the name must end in .mefd): " + path);
  fs::path dir = u8(path);
  if (dir.filename().empty()) dir = dir.parent_path();
  dir = fs::absolute(dir);
  if (!fs::is_directory(dir)) throw IoError("not a session folder: " + str(dir));
  // The name AS STORED ON DISK: on a case-insensitive volume "PATIENT.mefd"
  // opens "patient.mefd", and taking the caller's spelling would report
  // drift that is not there, write the wrong name into every header, and
  // never find the folder again by its exact entry name after a crash.
  std::error_code ec;
  for (fs::directory_iterator it(dir.parent_path(), ec), end; !ec && it != end; it.increment(ec))
    if (fs::equivalent(it->path(), dir, ec)) return dir.parent_path() / it->path().filename();
  return dir;
}

/// A session that is being written must not be renamed underneath its
/// writer: a copy's `<dst>.partial` beside it (or beside the target), a
/// writer's or repair's temp file inside it.
void refuse_if_being_written(const fs::path& dir, const fs::path& target) {
  std::error_code ec;
  for (const auto& p : {dir, target})
    if (fs::exists(u8(str(p) + ".partial"), ec))
      throw WriteConflictError("a copy is being written beside the session (" + str(p) +
                               ".partial): finish or remove it first");
  for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    const std::string n = str(it->path().filename());
    if (ends_with(n, ".mef3io-tmp") || ends_with(n, ".mef3io-repair-tmp"))
      throw WriteConflictError("the session is being written (" + str(it->path()) +
                               " exists); if no writer is running, the file can be removed");
  }
}

/// Read every file the plan rewrites and check it can be rewritten: CRCs
/// valid and, for a .tmet with section 2 encrypted, the key. Decides which
/// descriptions change. Nothing is written.
void prepare(Plan& p, const fs::path& dir, const RenameOptions& o) {
  const auto src = open_session_source(str(dir));
  std::vector<std::string> wanted;
  if (o.descriptions) wanted = *o.descriptions;
  for (const auto& rel : detail::header_files(*src)) {
    const fs::path file = dir / u8(rel);
    const bool tmet = ends_with(rel, ".tmet");
    if (fs::file_size(file) < (tmet ? fmt::METADATA_FILE_BYTES : UH))
      throw IoError("too short to carry a header: " + str(file));
    const auto bytes = read_prefix(file, tmet ? fmt::METADATA_FILE_BYTES : UH);
    const auto uh = fmt::UniversalHeader::parse(bytes);
    if (uh.header_crc != fmt::CRC_NO_ENTRY && !uh.header_crc_valid(bytes))
      throw IoError("the header CRC of " + str(file) +
                    " does not match; run validate/repair first, nothing was renamed");
    bool desc = false;
    if (tmet) {
      if (uh.body_crc != fmt::CRC_NO_ENTRY &&
          crc::calculate(std::span<const ui1>(bytes).subspan(UH)) != uh.body_crc)
        throw IoError("the metadata CRC of " + str(file) +
                      " does not match; run validate/repair first, nothing was renamed");
      const bool asked = !o.descriptions ||
                         std::find(wanted.begin(), wanted.end(), rel) != wanted.end();
      if (asked) {
        const auto s1 = fmt::MetadataSection1::parse(
            std::span<const ui1>(bytes).subspan(fmt::METADATA_SECTION_1_OFFSET));
        std::vector<ui1> s2(bytes.begin() + fmt::METADATA_SECTION_2_OFFSET,
                            bytes.begin() + fmt::METADATA_SECTION_2_OFFSET +
                                fmt::TIME_SERIES_METADATA_SECTION_2_BYTES);
        if (s1.section_2_encryption > 0) {
          const auto keys = crypto::validate_password(o.password, uh.level_1_password_validation_field,
                                                      uh.level_2_password_validation_field);
          const auto& key = s1.section_2_encryption == fmt::LEVEL_2_ENCRYPTION ? keys.level2_key
                                                                              : keys.level1_key;
          if (!key)
            throw PasswordError(
                "the session is encrypted: section 2, which holds session_description, needs " +
                std::string(o.password.empty() ? "the level-1 password"
                                               : "a password that opens it") +
                "; nothing was renamed");
          s2 = crypto::aes128_ecb_decrypt(s2, *key);
        }
        desc = p.old_name != p.new_name &&
               byteio::read_string(s2, S2_SESSION_DESC, S2_DESC_BYTES) == p.old_name;
      }
    }
    const bool name_changes = byteio::read_string(bytes, UH_SESSION_NAME, UH_NAME_BYTES) != p.new_name;
    if (desc) p.descriptions.push_back(rel);
    if (name_changes || desc) p.files.push_back(rel);
  }
  for (const auto& d : wanted)
    if (std::find(p.descriptions.begin(), p.descriptions.end(), d) == p.descriptions.end() &&
        !(ends_with(d, ".tmet") && fs::exists(dir / u8(d))))
      throw std::invalid_argument("descriptions: not a .tmet file of this session: " + d);
}

/// Rewrite one file's header: the stored name, the description where
/// planned, the provenance stamp, the CRCs. Every other byte is kept.
void patch(const fs::path& dir, const std::string& rel, bool desc, const Plan& p,
           const RenameOptions& o) {
  const fs::path file = dir / u8(rel);
  const bool tmet = ends_with(rel, ".tmet");
  std::vector<ui1> bytes = tmet ? read_whole(file) : read_prefix(file, UH);
  auto uh = fmt::UniversalHeader::parse(bytes);
  byteio::write_string(bytes, UH_SESSION_NAME, UH_NAME_BYTES, p.new_name);
  if (desc) {
    const auto s1 = fmt::MetadataSection1::parse(
        std::span<const ui1>(bytes).subspan(fmt::METADATA_SECTION_1_OFFSET));
    std::span<ui1> s2(bytes.data() + fmt::METADATA_SECTION_2_OFFSET,
                      fmt::TIME_SERIES_METADATA_SECTION_2_BYTES);
    std::optional<std::array<ui1, fmt::PASSWORD_BYTES>> key;
    if (s1.section_2_encryption > 0) {
      const auto keys = crypto::validate_password(o.password, uh.level_1_password_validation_field,
                                                  uh.level_2_password_validation_field);
      key = s1.section_2_encryption == fmt::LEVEL_2_ENCRYPTION ? keys.level2_key : keys.level1_key;
      if (!key) throw PasswordError("section 2 of " + str(file) + " cannot be opened");
      const auto plain = crypto::aes128_ecb_decrypt(s2, *key);
      std::copy(plain.begin(), plain.end(), s2.begin());
    }
    // The stored image is edited, never re-serialized (CLAUDE.md: a full
    // serialize wipes meflib's protected and discretionary regions).
    byteio::write_string(s2, S2_SESSION_DESC, S2_DESC_BYTES, p.new_name);
    if (key) {
      const auto enc = crypto::aes128_ecb_encrypt(s2, *key);
      std::copy(enc.begin(), enc.end(), s2.begin());
    }
    // Bounded by the fixed record length: a foreign writer's padding past it
    // is not part of the body (CLAUDE.md, ".tmet is a FIXED-length record").
    if (uh.body_crc != fmt::CRC_NO_ENTRY)
      byteio::write<ui4>(bytes, 4,
                         crc::calculate(std::span<const ui1>(bytes).subspan(
                             UH, fmt::METADATA_FILE_BYTES - UH)));
  }
  if (fmt::provenance::stamp_modified(uh, fmt::provenance::Operation::Rename))
    std::copy(uh.discretionary_region.begin(), uh.discretionary_region.end(),
              bytes.begin() + fmt::provenance::REGION_OFFSET);
  if (uh.header_crc != fmt::CRC_NO_ENTRY)  // "never computed" stays so
    byteio::write<ui4>(bytes, 0, crc::calculate(std::span<const ui1>(bytes).subspan(4, UH - 4)));
  if (tmet) detail::write_all_atomic(str(file), bytes, false);
  else overwrite_prefix(file, bytes);
}

std::string tool_name() { return std::string("mef3io ") + version(); }

/// Before planning: roll back an interrupted operation (marker + journal),
/// refuse a marker without its journal, discard journals of operations that
/// never started or already finished. Returns the session folder to work on
/// (after a rollback, the folder is back at its old name).
fs::path settle(fs::path dir, const RenameOptions& o, RenameReport& rep) {
  const fs::path parent = dir.parent_path();
  auto journals = journals_in(journal_dirs(parent, o.backup_dir));
  if (const auto m = detail::read_operation_marker(dir)) {
    const auto it = std::find_if(journals.begin(), journals.end(),
                                 [&](const Journal& j) { return j.id == m->id && !m->id.empty(); });
    if (it == journals.end()) throw WriteConflictError(detail::mid_operation_message(*m));
    // The journal must describe THIS folder. A copy made mid-operation
    // carries the same marker, and rolling back "its" journal would act on
    // the original (and the work that follows would rename the original).
    const auto at = locate(*it);
    std::error_code ec;
    if (!at || !fs::equivalent(it->parent / u8(*at), dir, ec))
      throw WriteConflictError(
          "this folder carries the marker of an interrupted operation whose journal describes "
          "another folder" + (at ? " (" + str(it->parent / u8(*at)) + ")" : std::string()) +
          ": it is probably a copy made while that operation was unfinished. Run the operation "
          "again on the original to roll it back, then copy again; nothing was changed");
    roll_back(*it, o.backup_dir.empty());
    rep.rolled_back = it->id;
    dir = it->parent / u8(it->old_folder);
    journals.erase(it);
  }
  const std::string folder = str(dir.filename());
  for (const auto& j : journals) {
    if (j.parent != parent) continue;
    if (j.old_folder != folder && j.new_folder != folder) continue;
    if (locate(j)) continue;  // another session's interrupted operation: not ours to touch
    discard_journal(j, o.backup_dir.empty());
    ++rep.journals_discarded;
  }
  return dir;
}

/// RenameOptions::crash_at: leaves the disk exactly as a crash there would.
struct SimulatedCrash : IoError {
  using IoError::IoError;
};

RenameReport run(Plan p, const fs::path& dir, const RenameOptions& o, RenameReport rep) {
  rep.old_path = str(dir);
  rep.new_path = str(p.parent / u8(p.new_folder));
  rep.old_name = p.old_name;
  rep.new_name = p.new_name;
  refuse_if_being_written(dir, p.parent / u8(p.new_folder));
  prepare(p, dir, o);
  rep.folder_renamed = p.folder != p.new_folder;
  if (p.files.empty() && p.records.empty() && !rep.folder_renamed) return rep;

  Journal j;
  j.id = detail::random_token();
  j.operation = p.operation;
  j.parent = p.parent;
  j.old_folder = p.folder;
  j.new_folder = p.new_folder;
  j.temp_folder = detail::random_token() + ".mefd";
  j.records = p.records;
  j.files = p.files;
  const bool default_dir = o.backup_dir.empty();
  j.backup_dir = default_dir ? u8(str(dir) + ".repair-backup") : u8(o.backup_dir);
  fs::create_directories(j.backup_dir);
  {
    std::ofstream out(j.backups(), std::ios::binary | std::ios::trunc);
    if (!out) throw IoError("cannot write the backups: " + str(j.backups()));
    for (const auto& rel : p.files) {
      const fs::path file = dir / u8(rel);
      const auto b = ends_with(rel, ".tmet") ? read_whole(file) : read_prefix(file, UH);
      out.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
      j.sizes.push_back(b.size());
    }
    detail::finish_stream(out, str(j.backups()));
  }
  detail::fsync_file_or_throw(str(j.backups()));
  write_journal(j);
  int point = 0;
  const auto reached = [&] {
    if (o.fail_at == point) throw IoError("simulated failure at point " + std::to_string(point));
    if (o.crash_at == point++) throw SimulatedCrash("simulated crash at point " + std::to_string(o.crash_at));
  };
  reached();
  // The journal is durable; only now the marker, and only after the marker
  // the first change.
  detail::write_operation_marker(dir, {tool_name(), p.operation, j.id});

  try {
    reached();
    std::vector<fs::path> written;
    for (const auto& rel : p.files) {
      patch(dir, rel,
            std::find(p.descriptions.begin(), p.descriptions.end(), rel) != p.descriptions.end(),
            p, o);
      written.push_back(dir / u8(rel));
      reached();
    }
    // Every header durable before the names move (and long before the
    // marker goes): one batch, not one flush per file.
    detail::flush_files(written);
    for (const auto& r : p.records) {
      rename_entry(dir, r.from, r.to, r.temp);
      reached();
    }
    if (p.folder != p.new_folder) {
      rename_entry(p.parent, p.folder, p.new_folder, j.temp_folder);
      reached();
    }
  } catch (const SimulatedCrash&) {
    throw;
  } catch (const std::exception& e) {
    try {
      roll_back(j, default_dir);
    } catch (const std::exception& again) {
      throw IoError(std::string(e.what()) + "; rolling back failed too (" + again.what() +
                    "). The session is marked as mid-operation and its journal is at " +
                    str(j.file()) + ": run the same operation again with the same backup "
                    "folder to roll it back.");
    }
    throw;
  }
  const fs::path done = p.parent / u8(p.new_folder);
  // Marker first, then the journal: a journal without a marker is discarded
  // by the next call. If the marker cannot be removed, the operation is
  // still COMPLETE: the journal goes anyway (so no later call rolls back a
  // finished rename), and the leftover marker can be cleared because the
  // name is consistent.
  try {
    detail::remove_operation_marker(done);
  } catch (const std::exception& e) {
    rep.notes.push_back(std::string("the rename is complete, but its marker could not be "
                                    "removed (") + e.what() +
                        "); writes are refused until clear_operation_marker() removes it");
  }
  reached();
  discard_journal(j, default_dir);

  for (auto rel : p.files) {
    for (const auto& r : p.records)
      if (rel == r.from) rel = r.to;
    rep.files_changed.push_back(rel);
  }
  rep.descriptions_changed = p.descriptions;
  for (const auto& r : p.records) rep.records_renamed.emplace_back(r.from, r.to);
  rep.new_path = str(done);
  return rep;
}

/// The session-level record files to rename from stem `from` to stem `to`.
std::vector<Journal::Rename> record_renames(const fs::path& dir, const std::string& from,
                                            const std::string& to) {
  std::vector<Journal::Rename> out;
  if (from == to) return out;
  for (const char* ext : {".rdat", ".ridx"}) {
    if (!has_entry(dir, from + ext)) continue;
    std::error_code ec;
    if (has_entry(dir, to + ext) ||
        (fs::exists(dir / u8(to + ext), ec) && !fs::equivalent(dir / u8(from + ext), dir / u8(to + ext), ec)))
      throw WriteConflictError("the session already has " + to + ext + "; nothing was renamed");
    out.push_back({from + ext, to + ext, detail::random_token() + ext});
  }
  return out;
}

}  // namespace

RenameReport rename_session(const std::string& path, const std::string& new_name,
                            const RenameOptions& opts) {
  check_new_name(new_name);
  RenameReport rep;
  const fs::path dir = settle(session_dir(path), opts, rep);
  Plan p;
  p.operation = "rename";
  p.parent = dir.parent_path();
  p.folder = str(dir.filename());
  p.new_folder = new_name + ".mefd";
  p.old_name = p.folder.substr(0, p.folder.size() - 5);
  p.new_name = new_name;
  if (p.old_name == new_name)
    throw std::invalid_argument("the session is already named " + new_name);
  std::error_code ec;
  const fs::path target = p.parent / u8(p.new_folder);
  if (fs::exists(target, ec) && !fs::equivalent(dir, target, ec))
    throw IoError("the target already exists: " + str(target));
  const auto d = name_drift(str(dir));
  if (!d.consistent)
    throw WriteConflictError(
        "the session's name is already out of step (the folder, the annotation file and the "
        "stored names disagree): run fix_name_drift first, then rename; nothing was renamed");
  p.records = record_renames(dir, p.old_name, new_name);
  return run(std::move(p), dir, opts, std::move(rep));
}

RenameReport fix_name_drift(const std::string& path, const std::string& target,
                            const RenameOptions& opts) {
  RenameReport rep;
  const fs::path dir = settle(session_dir(path), opts, rep);
  const auto d = name_drift(str(dir));
  Plan p;
  p.operation = "fix-name-drift";
  p.parent = dir.parent_path();
  p.folder = p.new_folder = str(dir.filename());  // the folder is never renamed
  p.new_name = target.empty() ? d.folder : target;
  check_new_name(p.new_name);
  if (d.stored_names.size() > 1)
    throw WriteConflictError("the files store " + std::to_string(d.stored_names.size()) +
                             " different session names, so the old name is ambiguous; "
                             "nothing was changed");
  if (d.record_files.size() > 1)
    throw WriteConflictError("the session has " + std::to_string(d.record_files.size()) +
                             " session-level annotation files (.rdat), so which one is the "
                             "session's is ambiguous; nothing was changed");
  p.old_name = !d.stored_names.empty() ? d.stored_names.begin()->first
               : !d.record_files.empty() ? d.record_files.front()
                                         : d.folder;
  if (!d.record_files.empty()) p.records = record_renames(dir, d.record_files.front(), p.new_name);
  return run(std::move(p), dir, opts, std::move(rep));
}

std::optional<OperationMarker> operation_marker(const std::string& path) {
  if (path_has_suffix(path, ".tar")) return std::nullopt;
  return detail::read_operation_marker(fs::path(path));
}

void clear_operation_marker(const std::string& path) {
  const fs::path dir = session_dir(path);
  if (!detail::read_operation_marker(dir))
    throw std::invalid_argument("the session carries no operation marker: " + str(dir));
  if (!name_drift(str(dir)).consistent)
    throw WriteConflictError(
        "the marker stays: the session's name is out of step (name_drift), so the "
        "interrupted operation may have left it half-changed. Roll it back with its journal "
        "(run the same operation again with the backup folder that holds the journal).");
  detail::remove_operation_marker(dir);
}

}  // namespace mef3io
