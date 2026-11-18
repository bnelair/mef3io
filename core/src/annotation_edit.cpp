// mef3io — remove_annotations: take records out of (and put records into) a
// record file pair, crash-safely. See annotation_edit.hpp for the contract and
// for_agents/notes/records-and-time.md ("Removing annotations") for the design
// and the crash table.
//
// THE STEPS, in the order they run (`Steps::point` marks every one; the test
// hook `crash_at` stops at the k-th as a crash would, `fail_at` fails there):
//
//   A. check, select and build everything in memory     (nothing on disk)
//   B. per new file: write `<file>.<id>.new`, flush it  (2 points per file)
//      flush the folders                                (1 point)
//   C. read the new files back, compare with the images (no point: reads)
//   D. marker (carries the plan) written + flushed      (2 points: before, after)
//   E. re-check the old files did not change            (no point)
//   F. swap, per file: old -> `<file>.<id>.old`, new -> `<file>`   (1 point each)
//   G. flush the folders                                (1 point)
//   H. remove the `.old` files                          (1 point each)
//   I. flush, remove the marker                         (2 points: before, after)
//
// A crash before D leaves the old state (plus unused `.new` files). From D on
// the marker is there and settle() (shared with recover_session) finishes the
// swap from the files' own state, or rolls it back.
#include "mef3io/annotation_edit.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

#include "durability.hpp"
#include "operation_marker.hpp"
#include "record_raw.hpp"
#include "session_layout.hpp"
#include "mef3io/byteio.hpp"
#include "mef3io/crc.hpp"
#include "mef3io/crypto.hpp"
#include "mef3io/errors.hpp"
#include "mef3io/headers.hpp"
#include "mef3io/metadata.hpp"
#include "mef3io/naming.hpp"
#include "mef3io/records.hpp"
#include "mef3io/source.hpp"
#include "mef3io/time.hpp"
#include "mef3io/transform.hpp"
#include "mef3io/version.hpp"

namespace fs = std::filesystem;

namespace mef3io {

RecordSelector selector_for(const Record& r) {
  RecordSelector s;
  s.level = r.level;
  s.channel = r.channel;
  s.segment = r.segment;
  s.type = r.type;
  s.time = r.time;
  s.fields = record_fields(r);
  return s;
}

namespace {

using byteio::read;
using byteio::write;
using detail::RawRecordFile;

constexpr std::size_t UH = fmt::UNIVERSAL_HEADER_BYTES;
constexpr std::size_t RHDR = fmt::RECORD_HEADER_BYTES;
constexpr std::size_t RIDX = fmt::RECORD_INDEX_BYTES;
constexpr const char* OP = detail::EDIT_ANNOTATIONS_OPERATION;

// ---- refusals ----------------------------------------------------------------

template <class Base>
class Coded final : public Base, public CopyError {
 public:
  explicit Coded(CopyRefusal r) : Base(r.message), CopyError(std::move(r)) {}
};

[[noreturn]] void refuse(RefusalKind kind, std::string code, std::string message,
                         std::string channel = {}) {
  CopyRefusal r{std::move(code), std::move(message), kind, std::move(channel), {}, {}};
  switch (kind) {
    case RefusalKind::InvalidArgument: throw Coded<std::invalid_argument>(std::move(r));
    case RefusalKind::Io: throw Coded<IoError>(std::move(r));
    case RefusalKind::WriteConflict: throw Coded<WriteConflictError>(std::move(r));
    case RefusalKind::Password: throw Coded<PasswordError>(std::move(r));
    case RefusalKind::Format: throw Coded<FormatError>(std::move(r));
    case RefusalKind::Crc: throw Coded<CrcError>(std::move(r));
  }
  throw std::logic_error("unknown refusal kind");
}

/// Test hook: stops as a crash would. Nothing that sees it may clean up.
struct SimulatedCrash : IoError {
  using IoError::IoError;
};

void note_point();  // defined with the test seam below (it needs `note`)

struct Steps {
  int crash_at = -1, fail_at = -1;
  int n = 0;
  void point(const std::function<void()>& partial = {}) {
    const int k = n++;
    note_point();
    if (fail_at == k) throw IoError("simulated failure at step " + std::to_string(k));
    if (crash_at == k) {
      if (partial) partial();
      throw SimulatedCrash("simulated crash at step " + std::to_string(k));
    }
  }
};

inline void point(Steps* s, const std::function<void()>& partial = {}) {
  if (s) s->point(partial);
}

// ---- small file helpers --------------------------------------------------------

std::string u8(const fs::path& p) { return detail::path_utf8(p); }
fs::path from8(const std::string& s) { return detail::utf8_path(s); }

bool has(const fs::path& p) {
  std::error_code ec;
  return fs::exists(p, ec);
}

std::vector<ui1> slurp(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  if (!f) throw IoError("cannot open for read: " + u8(p));
  return std::vector<ui1>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

void spit(const fs::path& p, std::span<const ui1> bytes) {
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  if (!f) throw IoError("cannot open for write: " + u8(p));
  f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  detail::finish_stream(f, u8(p));
}

std::string to_hex(const std::string& s) {
  static const char* d = "0123456789abcdef";
  std::string o;
  for (unsigned char c : s) {
    o += d[c >> 4];
    o += d[c & 15];
  }
  return o;
}

std::optional<std::string> from_hex(const std::string& h) {
  if (h.size() % 2) return std::nullopt;
  std::string o;
  auto v = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
  };
  for (std::size_t i = 0; i < h.size(); i += 2) {
    const int a = v(h[i]), b = v(h[i + 1]);
    if (a < 0 || b < 0) return std::nullopt;
    o += static_cast<char>(a * 16 + b);
  }
  return o;
}

fs::path sibling(const fs::path& target, const std::string& id, const char* ext) {
  return target.parent_path() / from8(u8(target.filename()) + "." + id + ext);
}

// ---- the test seam: what durable operations ran, in order --------------------------

std::mutex g_obs_mu;
testing::EditObserver g_obs;
bool g_skip_flush = false;

void note(const char* kind, const std::string& path = {}) {
  testing::EditObserver o;
  {
    std::lock_guard<std::mutex> lk(g_obs_mu);
    o = g_obs;
  }
  if (o) o(kind, path);
}

bool skip_flush() {
  std::lock_guard<std::mutex> lk(g_obs_mu);
  return g_skip_flush;
}

/// Flush the folders (their entries: new names, renames, removals). NTFS may
/// refuse a folder flush; only that refusal is ignored (durability.hpp).
void flush_dirs(const std::vector<fs::path>& dirs) {
  if (skip_flush()) return;  // test seam only: the negative control of the order test
  std::set<fs::path> seen;
  for (const auto& d : dirs) {
    if (!seen.insert(d).second) continue;
    note("flush-dir", u8(d));
    std::string why;
    if (!detail::fsync_directory(d, &why) && why.find("[ntfs-refused]") == std::string::npos)
      throw IoError("could not flush to disk, the write is not durable: " + u8(d) +
                    (why.empty() ? "" : " (" + why + ")"));
  }
}

void note_point() { note("point"); }

std::string tool_name() { return std::string("mef3io ") + version(); }

// ---- the plan a marker carries, and the state of each file ----------------------

/// One of the files of an edit: `target`, its new file and the backup of the
/// old one.
struct JFile {
  std::string rel;  ///< session-relative, '/'-separated
  fs::path target, fresh, old;
  bool had_old = true;  ///< the file existed before the edit
  bool has_new = true;  ///< the file exists after it (false: the pair is removed)
  /// CRC of the whole new file (carried by the marker): a target that is the
  /// only file left must be THIS to count as "the swap was done".
  bool has_crc = false;
  ui4 new_crc = 0;
  /// What the old file looked like when it was read (in-process only): checked
  /// again immediately before it is renamed away.
  bool has_snap = false;
  std::uintmax_t snap_size = 0;
  fs::file_time_type snap_mtime{};
};

struct JPlan {
  fs::path root;
  std::string id;
  std::vector<JFile> files;
  bool back = false;  ///< the marker says the edit is being rolled back (decided once, durable)
  std::vector<fs::path> dirs() const {
    std::vector<fs::path> d;
    for (const auto& f : files) d.push_back(f.target.parent_path());
    return d;
  }
};

JFile make_jfile(const fs::path& root, const std::string& rel, const std::string& id, bool had_old,
                 bool has_new) {
  JFile f;
  f.rel = rel;
  f.target = root / from8(rel);
  f.fresh = sibling(f.target, id, ".new");
  f.old = sibling(f.target, id, ".old");
  f.had_old = had_old;
  f.has_new = has_new;
  return f;
}

std::string crc_hex(ui4 v) {
  static const char* d = "0123456789abcdef";
  std::string o(8, '0');
  for (int i = 7; i >= 0; --i, v >>= 4) o[static_cast<std::size_t>(i)] = d[v & 15];
  return o;
}

/// The marker for a plan: the marker IS the journal.
OperationMarker marker_for(const JPlan& p) {
  OperationMarker m{tool_name(), OP, p.id, {}};
  m.details.push_back("edit-format 1");
  if (p.back) m.details.push_back("direction back");
  for (const auto& f : p.files)
    m.details.push_back(std::string("file ") + (f.had_old ? "1" : "0") + " " +
                        (f.has_new ? "1" : "0") + " " + to_hex(f.rel) +
                        (f.has_crc ? " " + crc_hex(f.new_crc) : std::string()));
  return m;
}

std::optional<JPlan> plan_from_marker(const fs::path& root, const OperationMarker& m) {
  if (m.operation != OP) return std::nullopt;
  JPlan p;
  p.root = root;
  p.id = m.id;
  bool format = false;
  for (const auto& line : m.details) {
    if (line == "edit-format 1") {
      format = true;
      continue;
    }
    if (line == "direction back") {
      p.back = true;
      continue;
    }
    if (line.rfind("file ", 0) != 0) continue;
    // file <had_old> <has_new> <hex rel>
    if (line.size() < 10 || line[6] != ' ' || line[8] != ' ' || line[5] < '0' || line[5] > '1' ||
        line[7] < '0' || line[7] > '1')
      refuse(RefusalKind::Format, "edit-journal-inconsistent",
             "the session's operation marker carries a plan this version cannot read");
    std::string hex = line.substr(9);
    std::optional<ui4> crc_of_new;
    if (const auto sp = hex.find(' '); sp != std::string::npos) {
      const std::string c = hex.substr(sp + 1);
      hex = hex.substr(0, sp);
      if (c.size() != 8 || !from_hex(c))
        refuse(RefusalKind::Format, "edit-journal-inconsistent",
               "the session's operation marker carries a plan this version cannot read");
      ui4 v = 0;
      for (char ch : c) v = v * 16 + static_cast<ui4>(ch <= '9' ? ch - '0' : ch - 'a' + 10);
      crc_of_new = v;
    }
    const auto rel = from_hex(hex);
    if (!rel || rel->empty() || rel->find("..") != std::string::npos || (*rel)[0] == '/')
      refuse(RefusalKind::Format, "edit-journal-inconsistent",
             "the session's operation marker carries a plan this version cannot read");
    p.files.push_back(make_jfile(root, *rel, m.id, line[5] == '1', line[7] == '1'));
    if (crc_of_new) {
      p.files.back().has_crc = true;
      p.files.back().new_crc = *crc_of_new;
    }
  }
  if (!format || p.files.empty() || m.id.size() != 32)
    refuse(RefusalKind::Format, "edit-journal-inconsistent",
           "the session's operation marker has no readable plan of the annotation edit");
  return p;
}

enum class St { S0, S1, S2, S3, Bad };

/// Which step of the swap a file stands at, from what exists:
///   replace: S0 (T,N)  S1 (N,O: old moved aside)  S2 (T,O: new in place)  S3 (T)
///   delete:  S0 (T)    S2 (O)                     S3 (nothing)
///   create:  S0 (N)    S3 (T)
St state_of(const JFile& f) {
  const bool t = has(f.target), n = has(f.fresh), o = has(f.old);
  if (f.had_old && f.has_new) {
    if (t && n && !o) return St::S0;
    if (!t && n && o) return St::S1;
    if (t && !n && o) return St::S2;
    if (t && !n && !o) return St::S3;
  } else if (f.had_old) {
    if (t && !n && !o) return St::S0;
    if (!t && !n && o) return St::S2;
    if (!t && !n && !o) return St::S3;
  } else if (f.has_new) {
    if (!t && n && !o) return St::S0;
    if (t && !n && !o) return St::S3;
  }
  return St::Bad;
}

/// Is a staged new record file (rdat or ridx) whole: right type, header CRC,
/// body CRC (a stored 0 = never computed, accepted).
bool new_file_intact(const fs::path& p) {
  try {
    const auto b = slurp(p);
    if (b.size() < UH) return false;
    const auto uh = fmt::UniversalHeader::parse(b);
    if (uh.file_type_string != fmt::FILE_TYPE_RECORD_DATA &&
        uh.file_type_string != fmt::FILE_TYPE_RECORD_INDICES)
      return false;
    if (!uh.header_crc_valid(b)) return false;
    if (uh.body_crc != fmt::CRC_NO_ENTRY &&
        crc::calculate(std::span<const ui1>(b).subspan(UH)) != uh.body_crc)
      return false;
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

enum class Dir { Forward, Back };

struct Decision {
  Dir dir = Dir::Forward;
  std::string why;
};

/// Forward when every new file still to be moved is intact; else back, when
/// every file can still be put back; else the edit cannot be settled.
Decision decide(const JPlan& p) {
  if (p.back) return {Dir::Back, "the edit was already being rolled back"};
  bool forward_ok = true, back_ok = true, lost = false;
  for (const auto& f : p.files) {
    const St s = state_of(f);
    if (s == St::Bad)
      refuse(RefusalKind::Format, "edit-journal-inconsistent",
             "the files of the interrupted annotation edit are in a state no step of it leaves (" +
                 f.rel + "); nothing was touched");
    if ((s == St::S0 || s == St::S1) && f.has_new && !new_file_intact(f.fresh)) forward_ok = false;
    if (s == St::S3 && f.had_old) {
      // "only the target is left" is either a finished swap or a new file that
      // never got in place and was lost: the marker's CRC of the new file tells.
      bool applied = true;
      if (f.has_new && f.has_crc) {
        try {
          applied = crc::calculate(slurp(f.target)) == f.new_crc;
        } catch (const std::exception&) {
          applied = false;
        }
      }
      if (applied) back_ok = false;
      else lost = true;
    }
  }
  Decision d;
  if (forward_ok && !lost) return d;
  if (!back_ok)
    refuse(RefusalKind::Format, "edit-journal-inconsistent",
           "a new annotation file of the interrupted edit is damaged or missing and the old one is "
           "already gone; nothing was touched");
  d.dir = Dir::Back;
  d.why = lost ? "a new annotation file is missing: the removal was never carried out"
               : "a new annotation file is not intact";
  return d;
}

void move_file(const fs::path& from, const fs::path& to, Steps* st) {
  point(st);
  if (has(to)) throw IoError("cannot move " + u8(from) + ": " + u8(to) + " exists");
  note("rename", u8(from) + "|" + u8(to));
  detail::replace_file(from, to);
}

void remove_file(const fs::path& p, Steps* st) {
  point(st);
  note("remove", u8(p));
  std::error_code ec;
  fs::remove(p, ec);
  if (ec) throw IoError("cannot remove " + u8(p) + ": " + ec.message());
}

/// Immediately before an old file is renamed away: is it still what was read?
/// (A writer that was already past its marker check may have appended.)
void require_unchanged(const JFile& f) {
  if (!f.has_snap) return;
  std::error_code ec;
  const auto size = fs::file_size(f.target, ec);
  const auto mtime = fs::last_write_time(f.target, ec);
  if (ec || size != f.snap_size || mtime != f.snap_mtime)
    refuse(RefusalKind::WriteConflict, "records-changed",
           "an annotation file changed while this call was working (another program or writer "
           "added or changed annotations): nothing was changed; try again");
}

/// Replace the marker with `p`'s (atomic: written beside it, flushed, renamed
/// over it, folder flushed). A crash leaves the old marker whole.
void rewrite_marker(const JPlan& p) {
  const OperationMarker m = marker_for(p);
  const fs::path file = p.root / detail::OPERATION_MARKER;
  const fs::path part = p.root / from8(std::string(detail::OPERATION_MARKER) + ".part");
  try {
    {
      std::ofstream out(part, std::ios::binary | std::ios::trunc);
      if (!out) throw IoError("cannot write the operation marker: " + u8(part));
      out << detail::OPERATION_MARKER_MAGIC << "\ntool " << m.tool << "\noperation " << m.operation
          << "\nid " << m.id << "\n";
      for (const auto& d : m.details) out << d << "\n";
      detail::finish_stream(out, u8(part));
    }
    if (!skip_flush()) detail::fsync_file_or_throw(u8(part));
    detail::replace_file(part, file);
  } catch (...) {
    std::error_code ec;
    fs::remove(part, ec);
    throw;
  }
  flush_dirs({p.root});  // the direction must be durable before anything is undone
  note("direction-durable");
}

/// Carry the plan out from whatever state its files are in, in direction
/// `dir`; every step is idempotent. Ends with the marker removed. Used by the
/// edit itself (forward, or back after a failure), by recover_session and by
/// the next edit.
void settle(const JPlan& given, Dir dir, Steps* st) {
  JPlan p = given;
  if (dir == Dir::Back && !p.back) {
    // The direction is decided ONCE and made durable before anything is undone:
    // a half-undone state cannot be told from a half-done one by its files.
    p.back = true;
    point(st);
    rewrite_marker(p);
  }
  for (const auto& f : p.files) {
    const bool t = has(f.target), n = has(f.fresh), o = has(f.old);
    const auto inconsistent = [&] {
      refuse(RefusalKind::Format, "edit-journal-inconsistent",
             "the files of the annotation edit are in a state no step of it leaves (" + f.rel + ")");
    };
    if (dir == Dir::Forward) {
      if (state_of(f) == St::Bad) inconsistent();
      const St s = state_of(f);
      if (s == St::S0) {
        if (f.had_old) {
          require_unchanged(f);
          move_file(f.target, f.old, st);
        }
        if (f.has_new) move_file(f.fresh, f.target, st);
      } else if (s == St::S1) {
        move_file(f.fresh, f.target, st);
      }
      continue;
    }
    if (f.had_old && f.has_new) {
      if (t && n && !o) remove_file(f.fresh, st);
      else if (!t && n && o) { move_file(f.old, f.target, st); remove_file(f.fresh, st); }
      else if (t && !n && o) {
        move_file(f.target, f.fresh, st);  // the new file steps aside
        move_file(f.old, f.target, st);
        remove_file(f.fresh, st);
      } else if (!(t && !n && !o)) inconsistent();
    } else if (f.had_old) {
      if (!t && !n && o) move_file(f.old, f.target, st);
      else if (!(t && !n && !o)) inconsistent();
    } else {
      if (!t && n && !o) remove_file(f.fresh, st);
      else if (t && !n && !o) remove_file(f.target, st);
      else if (!(!t && !n && !o)) inconsistent();
    }
  }
  point(st);
  flush_dirs(p.dirs());
  if (dir == Dir::Forward)
    for (const auto& f : p.files)
      if (has(f.old)) remove_file(f.old, st);
  point(st);
  flush_dirs(p.dirs());
  point(st);
  detail::remove_operation_marker(p.root);
  note("marker-removed");
  point(st);
}

// ---- leftovers -------------------------------------------------------------------

bool is_edit_leftover(const std::string& name) {
  if (name == std::string(detail::OPERATION_MARKER) + ".part") return true;
  for (const char* ext : {".new", ".old"}) {
    const std::size_t el = 4;
    if (name.size() < el + 33 + 5 || name.compare(name.size() - el, el, ext) != 0) continue;
    const std::string rest = name.substr(0, name.size() - el);
    if (rest[rest.size() - 33] != '.') continue;
    const std::string id = rest.substr(rest.size() - 32);
    const bool hex = std::all_of(id.begin(), id.end(), [](char c) {
      return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
    const std::string stem = rest.substr(0, rest.size() - 33);
    if (hex && stem.size() > 5 &&
        (stem.compare(stem.size() - 5, 5, ".rdat") == 0 || stem.compare(stem.size() - 5, 5, ".ridx") == 0))
      return true;
  }
  return false;
}

std::vector<fs::path> leftover_files(const fs::path& root) {
  std::vector<fs::path> out;
  std::error_code ec;
  const auto scan = [&](const fs::path& dir) {
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
      if (it->is_regular_file(ec) && is_edit_leftover(u8(it->path().filename())))
        out.push_back(it->path());
    ec.clear();
  };
  scan(root);
  for (fs::directory_iterator ch(root, ec), end; !ec && ch != end; ch.increment(ec)) {
    if (!ch->is_directory(ec) || ch->path().extension() != ".timd") continue;
    scan(ch->path());
    std::error_code ec2;
    for (fs::directory_iterator sg(ch->path(), ec2), e2; !ec2 && sg != e2; sg.increment(ec2))
      if (sg->is_directory(ec2) && sg->path().extension() == ".segd") scan(sg->path());
  }
  std::sort(out.begin(), out.end());
  return out;
}

fs::path session_root(const std::string& path) {
  if (path_has_suffix(path, ".tar"))
    refuse(RefusalKind::Io, "tar-read-only",
           "a tar session archive is read-only: annotations cannot be removed from " + path +
               " in place (extract it with extract_session, edit the folder, archive it again)");
  if (!path_has_suffix(path, ".mefd"))
    refuse(RefusalKind::Io, "not-a-session", "session directory must end with .mefd: " + path);
  const fs::path root = from8(path);
  std::error_code ec;
  if (!fs::is_directory(root, ec))
    refuse(RefusalKind::Io, "not-a-session", "no such session directory: " + path);
  return root;
}

// ---- selection ------------------------------------------------------------------

using FieldValue = RecordField::Value;

bool empty_list(const FieldValue& v) {
  if (const auto* a = std::get_if<std::vector<si4>>(&v)) return a->empty();
  if (const auto* b = std::get_if<std::vector<SeizureChannel>>(&v)) return b->empty();
  return false;
}

bool number_of(const FieldValue& v, long double& out) {
  if (const auto* i = std::get_if<si8>(&v)) { out = static_cast<long double>(*i); return true; }
  if (const auto* d = std::get_if<sf8>(&v)) { out = static_cast<long double>(*d); return true; }
  return false;
}

bool value_eq(const FieldValue& a, const FieldValue& b) {
  long double x, y;
  if (number_of(a, x) && number_of(b, y)) {
    if (x != x && y != y) return true;  // NaN names itself (a Curs value)
    if (const auto* ia = std::get_if<si8>(&a))
      if (const auto* ib = std::get_if<si8>(&b)) return *ia == *ib;
    return x == y;
  }
  if (empty_list(a) && empty_list(b)) return true;
  if (a.index() != b.index()) return false;
  if (const auto* s = std::get_if<std::string>(&a)) return *s == std::get<std::string>(b);
  if (const auto* v = std::get_if<std::vector<si4>>(&a)) return *v == std::get<std::vector<si4>>(b);
  if (const auto* c = std::get_if<std::vector<SeizureChannel>>(&a)) {
    const auto& d = std::get<std::vector<SeizureChannel>>(b);
    if (c->size() != d.size()) return false;
    for (std::size_t i = 0; i < c->size(); ++i)
      if ((*c)[i].name != d[i].name || (*c)[i].onset != d[i].onset || (*c)[i].offset != d[i].offset)
        return false;
    return true;
  }
  return false;
}

std::vector<std::string> field_names_of(const std::string& type) {
  Record r;
  r.type = type;
  std::vector<std::string> n;
  for (const auto& f : record_fields(r)) n.push_back(f.name);
  return n;
}

enum class Match { No, Yes, Incomplete };

/// Does `stored` match `sel`? Incomplete: the base keys match, but the
/// selector leaves out a field the stored record has a value for (`missing`).
Match match_record(const RecordSelector& sel, const Record& stored, std::string& missing) {
  if (sel.level != stored.level || sel.channel != stored.channel || sel.segment != stored.segment ||
      sel.type != stored.type || sel.time != stored.time)
    return Match::No;
  bool incomplete = false, differs = false;
  for (const auto& sf : record_fields(stored)) {
    const auto it = std::find_if(sel.fields.begin(), sel.fields.end(),
                                 [&](const RecordField& x) { return x.name == sf.name; });
    if (it == sel.fields.end()) {
      if (sf.value) {
        incomplete = true;
        if (missing.empty()) missing = sf.name;
      }
      continue;
    }
    if (!sf.value && !it->value) continue;
    if (!sf.value || !it->value || !value_eq(*sf.value, *it->value)) differs = true;
  }
  if (differs) return Match::No;
  return incomplete ? Match::Incomplete : Match::Yes;
}

bool same_record(const Record& a, const Record& b) {
  if (a.type != b.type || a.time != b.time || a.version_major != b.version_major ||
      a.version_minor != b.version_minor)
    return false;
  const auto fa = record_fields(a), fb = record_fields(b);
  if (fa.size() != fb.size()) return false;
  for (std::size_t i = 0; i < fa.size(); ++i) {
    if (fa[i].name != fb[i].name) return false;
    if (!fa[i].value && !fb[i].value) continue;
    if (!fa[i].value || !fb[i].value || !value_eq(*fa[i].value, *fb[i].value)) return false;
  }
  return true;
}

const char* level_word(RecordLevel l) { return record_level_name(l); }

// ---- where a level's record file is ---------------------------------------------------

struct Target {
  RecordLevel level = RecordLevel::Session;
  std::string channel;
  int segment = -1;
  std::string rel;  ///< session-relative `.rdat` path ("" = there is none)
  bool operator<(const Target& o) const {
    return std::tie(level, channel, segment) < std::tie(o.level, o.channel, o.segment);
  }
};

Target locate(const fs::path& root, const std::string& path, RecordLevel level,
              const std::string& channel, int segment) {
  Target t;
  t.level = level;
  t.channel = channel;
  t.segment = segment;
  if (level == RecordLevel::Session) {
    if (!channel.empty() || segment >= 0)
      refuse(RefusalKind::InvalidArgument, "bad-selector",
             "a session-level record names no channel or segment");
    std::string ambiguity;
    const std::string existing = detail::session_record_file(DirectorySource(path), ambiguity);
    if (!ambiguity.empty())
      refuse(RefusalKind::Format, "ambiguous-session-records", ambiguity);
    t.rel = existing;
    return t;
  }
  if (channel.empty())
    refuse(RefusalKind::InvalidArgument, "bad-selector",
           std::string("a ") + level_word(level) + "-level record names its channel");
  const fs::path timd = root / from8(channel + ".timd");
  if (!has(timd))
    refuse(RefusalKind::InvalidArgument, "unknown-channel", "no such channel: " + channel, channel);
  if (level == RecordLevel::Channel) {
    if (segment >= 0)
      refuse(RefusalKind::InvalidArgument, "bad-selector",
             "a channel-level record names no segment");
    const std::string rel = channel + ".timd/" + channel + ".rdat";
    t.rel = has(root / from8(rel)) ? rel : "";
    return t;
  }
  if (segment < 0)
    refuse(RefusalKind::InvalidArgument, "bad-selector", "a segment-level record names its segment");
  std::error_code ec;
  for (fs::directory_iterator it(timd, ec), end; !ec && it != end; it.increment(ec)) {
    if (!it->is_directory(ec) || it->path().extension() != ".segd") continue;
    const std::string stem = u8(it->path().stem());
    if (detail::layout_segment_number(stem) != segment) continue;
    const std::string rel = channel + ".timd/" + stem + ".segd/" + stem + ".rdat";
    t.rel = has(root / from8(rel)) ? rel : "";
    return t;
  }
  refuse(RefusalKind::InvalidArgument, "unknown-segment",
         "no such segment: " + channel + " segment " + std::to_string(segment), channel);
}

// ---- the recording-time offset a level's record times are read with --------------------

si8 level_offset(const fs::path& root, const Target& t, const std::string& password) {
  std::vector<fs::path> tmets;
  const auto channel_tmets = [&](const std::string& ch, int only_segment) {
    std::error_code ec;
    const fs::path timd = root / from8(ch + ".timd");
    for (fs::directory_iterator it(timd, ec), end; !ec && it != end; it.increment(ec)) {
      if (!it->is_directory(ec) || it->path().extension() != ".segd") continue;
      const std::string stem = u8(it->path().stem());
      if (only_segment >= 0 && detail::layout_segment_number(stem) != only_segment) continue;
      const fs::path tmet = it->path() / from8(stem + ".tmet");
      if (has(tmet)) tmets.push_back(tmet);
    }
  };
  if (t.level == RecordLevel::Segment) {
    channel_tmets(t.channel, t.segment);
  } else if (t.level == RecordLevel::Channel) {
    channel_tmets(t.channel, -1);
  }
  if (tmets.empty()) {  // a record-only channel, or the session: every segment there is
    std::error_code ec;
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
      if (it->is_directory(ec) && it->path().extension() == ".timd")
        channel_tmets(u8(it->path().stem()), -1);
  }
  std::optional<si8> agreed;
  std::string unreadable;
  for (const auto& tmet : tmets) {
    std::optional<si8> rto;
    try {
      std::ifstream f;
      f.rdbuf()->pubsetbuf(nullptr, 0);
      f.open(tmet, std::ios::binary);
      std::vector<ui1> bytes(fmt::METADATA_FILE_BYTES);
      f.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
      if (!f) throw IoError("short metadata");
      rto = probe_recording_time_offset(bytes, password);
      if (!rto) refuse(RefusalKind::WriteConflict, "offset-unknown",
                       "the recording-time offset of this level is not known (section 3 of the "
                       "metadata is locked: both passwords are needed); nothing was changed");
    } catch (const CopyError&) {
      throw;
    } catch (const std::exception&) {
      if (unreadable.empty()) unreadable = u8(tmet.parent_path().filename());
      continue;
    }
    if (!agreed) agreed = *rto;
    else if (*agreed != *rto)
      refuse(RefusalKind::WriteConflict, "offset-unknown",
             "the segments of this level disagree on the recording-time offset, so a record's "
             "time cannot be placed with certainty; nothing was changed");
  }
  if (!agreed) {
    if (!unreadable.empty())
      refuse(RefusalKind::WriteConflict, "offset-unknown",
             "the recording-time offset of this level cannot be read (its metadata is unreadable); "
             "nothing was changed");
    return 0;  // no segments at all: mef3io writes such records with offset 0
  }
  return *agreed;
}

// ---- one record file, read, edited in memory --------------------------------------------

/// What a file looked like when it was read.
struct Snap {
  std::uintmax_t size = 0;
  fs::file_time_type mtime{};
  std::vector<ui1> head;  // the first UH bytes
};

struct FilePlan {
  Target target;
  Snap rdat_snap, ridx_snap;
  std::vector<std::size_t> rec_len;     // stored length of each record (header + body)
  si8 n_before = 0;
  std::size_t old_end = 0;              // where the last record of the old .rdat ends
  std::size_t kept_bytes = 0, removed_bytes = 0;  // record bytes carried over / taken out
  fs::path rdat, ridx;
  std::string rel_rdat, rel_ridx;
  std::vector<ui1> old_rdat, old_ridx;  // whole files as read
  RawRecordFile raw;
  std::vector<Record> decoded;          // file order, absolute times
  si8 rto = 0;
  bool protected_ = false;
  std::vector<bool> drop;
  std::vector<Record> adds;             // normalised, time order
  std::vector<ui1> new_rdat, new_ridx;  // the images; empty when the pair is removed
  bool remove_pair = false;
  std::vector<Record> expected;         // what a reader must find in the new file
};

void raw_refuse(RefusalKind kind, std::string code, std::string message) {
  if (code != "record-locked")
    message += "; nothing was changed (run recover_session, which keeps every whole record, or "
               "validate_session first)";
  refuse(kind, std::move(code), std::move(message));
}

std::string display_for(const Target& t) {
  std::string s = std::string(level_word(t.level)) + "-level annotations";
  if (!t.channel.empty()) s += " of " + t.channel;
  if (t.segment >= 0) s += " segment " + std::to_string(t.segment);
  return s;
}

/// Stamp, count and seal a record file's header over its body.
std::vector<ui1> sealed(const std::vector<ui1>& old_uh_bytes, std::span<const ui1> body, si8 n,
                        si8 max_entry, si8 lo, si8 hi, si8 rto) {
  auto uh = fmt::UniversalHeader::parse(old_uh_bytes);
  uh.number_of_entries = n;
  uh.maximum_entry_size = max_entry;
  if (lo != fmt::UUTC_NO_ENTRY && time_is_storable(lo, rto) && time_is_storable(hi, rto)) {
    uh.start_time = to_stored_time(lo, rto);
    uh.end_time = to_stored_time(hi, rto);
  }
  uh.body_crc = crc::calculate(body);
  fmt::provenance::stamp_modified(uh, fmt::provenance::Operation::RecordsEdit);
  std::vector<ui1> head(old_uh_bytes.begin(), old_uh_bytes.begin() + UH);
  uh.serialize(head);
  uh.update_header_crc(head);
  return head;
}

/// Build the new images of `fp` (drop[] and adds[] decided).
void build_images(FilePlan& fp, const crypto::AccessKeys& keys, bool encrypt_adds) {
  auto& raw = fp.raw;
  std::unordered_map<si8, std::size_t> by_offset;
  for (std::size_t i = 0; i < raw.ridx.size(); ++i) by_offset[read<si8>(raw.ridx[i], 8)] = i;

  std::vector<ui1> rdat_body, ridx_body;
  si8 n = 0, max_size = 0, lo = fmt::UUTC_NO_ENTRY, hi = fmt::UUTC_NO_ENTRY;
  const auto widen = [&](si8 t) {
    if (t == fmt::UUTC_NO_ENTRY) return;
    lo = lo == fmt::UUTC_NO_ENTRY ? t : std::min(lo, t);
    hi = hi == fmt::UUTC_NO_ENTRY ? t : std::max(hi, t);
  };
  fp.expected.clear();
  std::size_t kept_bytes = 0, removed_bytes = 0;
  for (std::size_t i = 0; i < raw.records.size(); ++i) {
    if (fp.drop[i]) {
      removed_bytes += fp.rec_len[i];
      continue;
    }
    const std::size_t off = static_cast<std::size_t>(raw.record_offsets[i]);
    const std::size_t len = fp.rec_len[i];
    kept_bytes += len;
    const auto it = by_offset.find(raw.record_offsets[i]);
    if (it == by_offset.end())
      refuse(RefusalKind::Format, "record-index-mismatch",
             "the record index of the " + display_for(fp.target) + " has no entry for record " +
                 std::to_string(i) + "; which time is right is unknown, so nothing is changed");
    auto entry = raw.ridx[it->second];
    write<si8>(entry, 8, static_cast<si8>(UH + rdat_body.size()));
    rdat_body.insert(rdat_body.end(), fp.old_rdat.begin() + static_cast<std::ptrdiff_t>(off),
                     fp.old_rdat.begin() + static_cast<std::ptrdiff_t>(off + len));
    ridx_body.insert(ridx_body.end(), entry.begin(), entry.end());
    max_size = std::max<si8>(max_size, static_cast<si8>(len));
    widen(fp.decoded[i].time);
    fp.expected.push_back(std::move(fp.decoded[i]));
    ++n;
  }
  // By construction nothing can be lost: every old record byte is either kept
  // or belongs to a record the caller named.
  if (kept_bytes + removed_bytes != fp.old_end - UH)
    refuse(RefusalKind::Io, "verify-failed",
           "internal check failed: the records kept and removed do not add up to the old file; "
           "nothing was changed");
  fp.kept_bytes = kept_bytes;
  fp.removed_bytes = removed_bytes;
  if (!fp.adds.empty()) {
    const auto enc = encode_records_for_file(fp.adds, fp.rto, encrypt_adds, keys);
    for (std::size_t i = 0; i < enc.size(); ++i) {
      auto entry = enc[i].entry;
      write<si8>(entry, 8, static_cast<si8>(UH + rdat_body.size()));
      rdat_body.insert(rdat_body.end(), enc[i].bytes.begin(), enc[i].bytes.end());
      ridx_body.insert(ridx_body.end(), entry.begin(), entry.end());
      max_size = std::max<si8>(max_size, static_cast<si8>(enc[i].bytes.size()));
      widen(enc[i].time);
      ++n;
    }
    // what a reader finds for them: the same order as encoded (time, stable)
    std::vector<Record> sorted = fp.adds;
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const Record& a, const Record& b) { return a.time < b.time; });
    for (auto& r : sorted) fp.expected.push_back(std::move(r));
  }
  fp.remove_pair = n == 0;
  fp.new_rdat.clear();
  fp.new_ridx.clear();
  if (fp.remove_pair) return;

  const auto old_uh = fmt::UniversalHeader::parse(fp.raw.uh);
  const si8 declared_max = old_uh.maximum_entry_size > 0 ? old_uh.maximum_entry_size : 0;
  fp.new_rdat = sealed(fp.raw.uh, rdat_body, n, std::max(declared_max, max_size), lo, hi, fp.rto);
  fp.new_rdat.insert(fp.new_rdat.end(), rdat_body.begin(), rdat_body.end());
  fp.new_ridx = sealed(fp.raw.ridx_uh, ridx_body, n, static_cast<si8>(RIDX), lo, hi, fp.rto);
  fp.new_ridx.insert(fp.new_ridx.end(), ridx_body.begin(), ridx_body.end());
}

/// Read an image the way a reader would and compare it with what the edit
/// meant to produce. Used on the in-memory images before anything is written
/// and on the files read back from disk.
void check_images(const FilePlan& fp, const std::vector<ui1>& rdat, const std::vector<ui1>& ridx,
                  const crypto::AccessKeys& keys, const std::string& code) {
  const auto bad = [&](const std::string& what) {
    refuse(RefusalKind::Io, code,
           "the new annotation files for the " + display_for(fp.target) +
               " do not hold what was intended (" + what + "); nothing was changed");
  };
  if (rdat.size() < UH || ridx.size() < UH || (ridx.size() - UH) % RIDX != 0) bad("size");
  const auto ru = fmt::UniversalHeader::parse(rdat);
  const auto iu = fmt::UniversalHeader::parse(ridx);
  if (ru.file_type_string != fmt::FILE_TYPE_RECORD_DATA ||
      iu.file_type_string != fmt::FILE_TYPE_RECORD_INDICES)
    bad("file type");
  if (!ru.header_crc_valid(rdat) || !iu.header_crc_valid(ridx)) bad("header CRC");
  if (ru.body_crc != crc::calculate(std::span<const ui1>(rdat).subspan(UH)) ||
      iu.body_crc != crc::calculate(std::span<const ui1>(ridx).subspan(UH)))
    bad("body CRC");
  const si8 n = static_cast<si8>((ridx.size() - UH) / RIDX);
  if (ru.number_of_entries != n || iu.number_of_entries != n ||
      n != static_cast<si8>(fp.expected.size()))
    bad("record count");
  std::vector<Record> got;
  try {
    got = parse_records(rdat, fp.rto, keys);
  } catch (const std::exception& e) {
    bad(std::string("a reader cannot read it: ") + e.what());
  }
  if (got.size() != fp.expected.size()) bad("record count");
  si8 off = static_cast<si8>(UH);
  for (std::size_t i = 0; i < got.size(); ++i) {
    if (!same_record(got[i], fp.expected[i])) bad("record " + std::to_string(i) + " differs");
    if (read<si8>(ridx, UH + i * RIDX + 8) != off) bad("index offset " + std::to_string(i));
    if (read<si8>(ridx, UH + i * RIDX + 16) != read<si8>(rdat, static_cast<std::size_t>(off) + 16))
      bad("index time " + std::to_string(i));
    off += static_cast<si8>(RHDR) + static_cast<si8>(read<ui4>(rdat, static_cast<std::size_t>(off) + 12));
  }
  if (static_cast<std::size_t>(off) != rdat.size()) bad("trailing bytes");
}

void write_new(const fs::path& to, const std::vector<ui1>& bytes, const fs::path& like, Steps* st) {
  point(st, [&] { spit(to, std::span<const ui1>(bytes).first(bytes.size() / 2)); });
  spit(to, bytes);
  detail::copy_file_identity(like, to);
  point(st);
  if (!skip_flush()) {
    detail::fsync_file_or_throw(u8(to));
    note("fsync-file", u8(to));
  }
}

Record normalised_add(Record r, const Target& t) {
  r.level = t.level;
  r.channel = t.channel;
  r.segment = t.segment;
  if (r.type == "Note" || r.type == "SyLg") {
    if (!r.text) r.text = std::string();
  } else if (r.type == "EDFA") {
    if (!r.text) r.text = std::string();
    if (!r.duration) r.duration = 0;
  }
  return r;
}

}  // namespace

// ================================================================================
// remove_annotations
// ================================================================================

RemoveAnnotationsReport remove_annotations(const std::string& path,
                                           const std::vector<RecordSelector>& remove,
                                           const std::vector<Record>& add,
                                           const RemoveAnnotationsOptions& opts, const Control& ctl) {
  const fs::path root = session_root(path);
  if (remove.empty() && add.empty())
    refuse(RefusalKind::InvalidArgument, "nothing-to-do",
           "nothing to do: name the records to remove, or records to add");
  if (opts.password_1.empty() != opts.password_2.empty())
    refuse(RefusalKind::Password, "password",
           "a password-protected session needs both passwords (level-1 and level-2), or none; "
           "got only one; nothing was changed");
  for (const auto& s : remove) {
    if (s.expect < 1)
      refuse(RefusalKind::InvalidArgument, "bad-selector", "expect must be at least 1");
    if (s.type.size() != RECORD_TYPE_BYTES)
      refuse(RefusalKind::InvalidArgument, "bad-selector",
             "a selector names its record type (4 characters, e.g. \"Note\")");
    const auto names = field_names_of(s.type);
    std::set<std::string> given;
    for (const auto& f : s.fields) {
      if (std::find(names.begin(), names.end(), f.name) == names.end())
        refuse(RefusalKind::InvalidArgument, "selector-unknown-field",
               "a " + s.type + " record has no field \"" + f.name + "\"");
      if (!given.insert(f.name).second)
        refuse(RefusalKind::InvalidArgument, "bad-selector",
               "the field \"" + f.name + "\" is given twice");
    }
  }

  RemoveAnnotationsReport report;
  report.dry_run = opts.dry_run;
  Steps steps;
  steps.crash_at = opts.crash_at;
  steps.fail_at = opts.fail_at;

  // An edit this session was interrupted in is finished (or rolled back) first.
  if (const auto m = detail::read_operation_marker(root)) {
    const bool ours =
        m->operation == OP || (m->operation == "unknown" && !leftover_files(root).empty());
    if (!ours)
      refuse(RefusalKind::WriteConflict, "mid-operation", detail::mid_operation_message(*m));
    if (opts.dry_run) {
      refuse(RefusalKind::WriteConflict, "edit-interrupted",
             "an earlier annotation edit of this session was interrupted: its annotation files "
             "may be half swapped, so nothing can be previewed. Run recover_session with "
             "apply=True (or a real remove_annotations call, which finishes it first)");
    } else {
      const auto done = finish_interrupted_record_edit(path);
      if (done) {
        report.resumed = done->id;
        report.notes.push_back(
            "an earlier annotation edit was interrupted and has been " +
            std::string(done->action == "finish"      ? "finished"
                        : done->action == "roll back" ? "rolled back (that removal was NOT done)"
                                                      : "settled from the files") +
            " first");
      }
    }
  }
  try {
    detail::refuse_if_being_written(root, root);
  } catch (const WriteConflictError& e) {
    refuse(RefusalKind::WriteConflict, "being-written", e.what());
  }

  const std::string pw = opts.password_2.empty() ? opts.password_1 : opts.password_2;

  // ---- A: where, what, and the passwords -----------------------------------------
  std::map<Target, std::vector<std::size_t>> sel_of;  // selectors per file
  std::map<Target, std::vector<std::size_t>> add_of;
  std::vector<Target> sel_target, add_target;
  const auto level_of_add = [](const Record& r) { return r.level; };
  for (std::size_t i = 0; i < remove.size(); ++i) {
    Target t = locate(root, path, remove[i].level, remove[i].channel, remove[i].segment);
    sel_target.push_back(t);
    sel_of[t].push_back(i);
  }
  for (std::size_t i = 0; i < add.size(); ++i) {
    Target t = locate(root, path, level_of_add(add[i]), add[i].channel, add[i].segment);
    if (t.rel.empty())
      refuse(RefusalKind::InvalidArgument, "add-level-missing",
             "there are no annotations at the " + display_for(t) +
                 " yet: records can only be added where the file exists (use write_annotations to "
                 "create it)",
             t.channel);
    add_target.push_back(t);
    add_of[t].push_back(i);
  }
  validate_records_for_write(add);  // a FormatError: bad type or payload

  RecordAccessCache access;
  std::set<Target> files;
  for (const auto& [t, v] : sel_of) files.insert(t);
  for (const auto& [t, v] : add_of) files.insert(t);

  std::vector<FilePlan> plans;
  std::uint64_t total = 0;
  for (const auto& t : files) {
    FilePlan fp;
    fp.target = t;
    if (t.rel.empty())
      refuse(RefusalKind::InvalidArgument, "no-match",
             "there are no annotations at the " + display_for(t) +
                 ": none of the selectors for it matches",
             t.channel);
    fp.rel_rdat = t.rel;
    fp.rel_ridx = detail::ridx_name_of(t.rel);
    fp.rdat = root / from8(fp.rel_rdat);
    fp.ridx = root / from8(fp.rel_ridx);
    if (!has(fp.ridx))
      refuse(RefusalKind::Format, "record-index-missing",
             "the " + display_for(t) + " have no index file; run recover_session first, nothing was changed",
             t.channel);
    try {
      require_record_write_access(path, t.level == RecordLevel::Session ? std::nullopt
                                                                        : std::optional<std::string>(t.channel),
                                  opts.password_1, opts.password_2, access);
    } catch (const PasswordError& e) {
      std::string m = e.what();
      for (const auto& [from, to] : {std::pair<const char*, const char*>{"to add annotations", "to change annotations"},
                                     {"add annotations", "change annotations"},
                                     {"nothing was written", "nothing was changed"}}) {
        const auto at = m.find(from);
        if (at != std::string::npos) m.replace(at, std::strlen(from), to);
      }
      refuse(RefusalKind::Password, "password", m, t.channel);
    }
    std::error_code ec;
    total += 3 * (fs::file_size(fp.rdat, ec) + fs::file_size(fp.ridx, ec));
    plans.push_back(std::move(fp));
  }
  detail::ProgressReporter progress(ctl, total);
  progress.start();

  std::vector<std::size_t> plan_of_sel(remove.size(), 0);
  for (std::size_t p = 0; p < plans.size(); ++p) {
    for (std::size_t i : sel_of[plans[p].target]) plan_of_sel[i] = p;
  }

  // ---- A: read, decode, select ------------------------------------------------------
  for (auto& fp : plans) {
    ctl.check();
    std::error_code mt_ec;
    fp.rdat_snap.mtime = fs::last_write_time(fp.rdat, mt_ec);
    fp.ridx_snap.mtime = fs::last_write_time(fp.ridx, mt_ec);
    fp.old_rdat = slurp(fp.rdat);
    fp.old_ridx = slurp(fp.ridx);
    progress.add(fp.old_rdat.size() + fp.old_ridx.size());
    fp.rdat_snap.size = fp.old_rdat.size();
    fp.ridx_snap.size = fp.old_ridx.size();
    fp.rdat_snap.head.assign(fp.old_rdat.begin(),
                             fp.old_rdat.begin() + std::min<std::size_t>(UH, fp.old_rdat.size()));
    fp.ridx_snap.head.assign(fp.old_ridx.begin(),
                             fp.old_ridx.begin() + std::min<std::size_t>(UH, fp.old_ridx.size()));
    const std::string where = display_for(fp.target);
    fp.raw = detail::parse_record_file(fp.old_rdat, &fp.old_ridx, where, where + " (index)", pw,
                                       raw_refuse);
    // Both headers must be sound: they are rewritten, not trusted.
    for (int k = 0; k < 2; ++k) {
      const auto& bytes = k == 0 ? fp.old_rdat : fp.old_ridx;
      const auto uh = fmt::UniversalHeader::parse(bytes);
      if (uh.file_type_string != (k == 0 ? fmt::FILE_TYPE_RECORD_DATA : fmt::FILE_TYPE_RECORD_INDICES))
        refuse(RefusalKind::Format, "record-damaged", "not a record file: the " + where, fp.target.channel);
      if (uh.header_crc != fmt::CRC_NO_ENTRY && !uh.header_crc_valid(bytes))
        refuse(RefusalKind::Crc, "record-damaged",
               std::string("the header of the ") + (k ? "index of the " : "") + where +
                   " fails its CRC; run recover_session or validate first, nothing was changed",
               fp.target.channel);
    }
    // EVERY byte of the old files must be carried over or named: a whole record
    // or a fragment after the last record the header counts (what an interrupted
    // append leaves), or an index that disagrees with the data, would otherwise
    // be dropped silently by the rebuild. recover_session keeps such a record.
    {
      fp.old_end = fp.raw.records.empty()
                       ? UH
                       : static_cast<std::size_t>(fp.raw.record_offsets.back()) + RHDR +
                             fp.raw.records.back().body.size();
      const si8 declared = read<si8>(fp.old_rdat, 32);
      const bool zero_tail =
          declared == -1 &&
          detail::all_zero_bytes(std::span<const ui1>(fp.old_rdat).subspan(fp.old_end));
      const auto idx_uh = fmt::UniversalHeader::parse(fp.old_ridx);
      const si8 idx_declared = idx_uh.number_of_entries;
      const char* torn =
          "hold bytes or index entries that the header does not count (a record an interrupted "
          "append left); remove_annotations would not carry them over, so nothing was changed. "
          "Run recover_session first, which keeps every whole record and repairs the files";
      if (fp.old_end != fp.old_rdat.size() && !zero_tail)
        refuse(RefusalKind::Format, "record-torn", "the " + where + " " + torn, fp.target.channel);
      if (idx_declared != -1 && idx_declared != static_cast<si8>(fp.raw.ridx.size()))
        refuse(RefusalKind::Format, "record-torn", "the index of the " + where + " " + torn,
               fp.target.channel);
      std::unordered_set<si8> offs;
      for (const auto& e : fp.raw.ridx) offs.insert(read<si8>(e, 8));
      bool each = offs.size() == fp.raw.ridx.size();
      for (const si8 o : fp.raw.record_offsets) each = each && offs.count(o) == 1;
      if (!each)
        refuse(RefusalKind::Format, "record-index-mismatch",
               "the record index of the " + where +
                   " does not list the records the file holds; which is right is unknown, so "
                   "nothing was changed (run recover_session first)",
               fp.target.channel);
    }
    fp.protected_ = fmt::UniversalHeader::parse(fp.old_rdat).is_password_protected();
    fp.rto = level_offset(root, fp.target, pw);
    try {
      fp.decoded = parse_records(fp.old_rdat, fp.rto, fp.raw.keys);
    } catch (const PasswordError& e) {
      refuse(RefusalKind::Password, "record-locked", e.what(), fp.target.channel);
    }
    if (fp.decoded.size() != fp.raw.records.size())
      refuse(RefusalKind::Format, "record-damaged", "the " + where + " cannot be read in full",
             fp.target.channel);
    for (auto& r : fp.decoded) {
      r.level = fp.target.level;
      r.channel = fp.target.channel;
      r.segment = fp.target.segment;
    }
    fp.drop.assign(fp.decoded.size(), false);
    fp.n_before = static_cast<si8>(fp.decoded.size());
    for (auto& r : fp.raw.records) {  // only the stored lengths are needed from here on
      fp.rec_len.push_back(RHDR + r.body.size());
      std::vector<ui1>().swap(r.body);
    }
  }

  std::vector<std::vector<std::size_t>> hits(remove.size());
  std::vector<si8> removed_per_file(plans.size(), 0);
  for (std::size_t p = 0; p < plans.size(); ++p) {
    auto& fp = plans[p];
    // records by (type, time): a selector only looks at those
    std::unordered_map<std::string, std::vector<std::size_t>> by_key;
    for (std::size_t r = 0; r < fp.decoded.size(); ++r)
      by_key[fp.decoded[r].type + '\0' + std::to_string(fp.decoded[r].time)].push_back(r);
    std::vector<int> owner(fp.decoded.size(), -1);
    for (std::size_t i : sel_of[fp.target]) {
      ctl.check();
      const auto cand = by_key.find(remove[i].type + '\0' + std::to_string(remove[i].time));
      if (cand == by_key.end()) continue;
      for (std::size_t r : cand->second) {
        std::string missing;
        const Match m = match_record(remove[i], fp.decoded[r], missing);
        if (m == Match::Incomplete)
          refuse(RefusalKind::InvalidArgument, "selector-incomplete",
                 "selector " + std::to_string(i) + " (" + remove[i].type + ") leaves out the field \"" +
                     missing + "\" that a stored record of that type and time has: name every field "
                     "of the record as Reader.records() returns it",
                 fp.target.channel);
        if (m != Match::Yes) continue;
        if (owner[r] >= 0 && owner[r] != static_cast<int>(i))
          refuse(RefusalKind::InvalidArgument, "selectors-overlap",
                 "selectors " + std::to_string(owner[r]) + " and " + std::to_string(i) +
                     " name the same stored record",
                 fp.target.channel);
        owner[r] = static_cast<int>(i);
        hits[i].push_back(r);
      }
    }
    for (std::size_t i : sel_of[fp.target]) {
      const auto n = static_cast<si8>(hits[i].size());
      if (n == 0)
        refuse(RefusalKind::InvalidArgument, "no-match",
               "selector " + std::to_string(i) + " (" + remove[i].type + " at the " +
                   display_for(fp.target) +
                   ") matches no stored record; nothing was changed (note text that is not valid "
                   "UTF-8 is shown with replacement characters by Reader.records() and cannot be "
                   "matched by a selector built from it)",
               fp.target.channel);
      if (!remove[i].all_copies && n != remove[i].expect)
        refuse(RefusalKind::InvalidArgument, n > remove[i].expect ? "ambiguous" : "count-mismatch",
               "selector " + std::to_string(i) + " (" + remove[i].type + " at the " +
                   display_for(fp.target) + ") matches " + std::to_string(n) +
                   " stored record(s) but " + std::to_string(remove[i].expect) +
                   " were expected" +
                   (n > remove[i].expect
                        ? " (identical copies: say expect=" + std::to_string(n) + " or all_copies)"
                        : "") +
                   "; nothing was changed",
               fp.target.channel);
      for (std::size_t r : hits[i]) fp.drop[r] = true;
    }
    for (std::size_t r = 0; r < fp.drop.size(); ++r) removed_per_file[p] += fp.drop[r];
  }

  // ---- A: adds, images ---------------------------------------------------------------
  for (std::size_t p = 0; p < plans.size(); ++p) {
    auto& fp = plans[p];
    crypto::AccessKeys keys = fp.raw.keys;
    for (std::size_t i : add_of[fp.target]) {
      if (add[i].time < 0)
        refuse(RefusalKind::WriteConflict, "add-time",
               "a record to add has a negative time: a record time is microseconds since the "
               "Unix epoch and cannot be negative; nothing was changed",
               fp.target.channel);
      try {
        require_storable_time(add[i].time, fp.rto, "record time");
      } catch (const WriteConflictError& e) {
        refuse(RefusalKind::WriteConflict, "add-time", e.what(), fp.target.channel);
      }
      fp.adds.push_back(normalised_add(add[i], fp.target));
    }
    const bool encrypt = fp.protected_;
    if (!fp.adds.empty()) {
      if (encrypt != !opts.password_1.empty())
        refuse(RefusalKind::WriteConflict, "records-encryption-mismatch",
               std::string("the ") + display_for(fp.target) +
                   (encrypt ? " are encrypted but no passwords were given"
                            : " are not encrypted but passwords were given") +
                   "; nothing was changed",
               fp.target.channel);
      if (encrypt && !keys.level2_key)
        refuse(RefusalKind::Password, "password",
               "the level-2 password is needed to add encrypted records", fp.target.channel);
    }
    try {
      build_images(fp, keys, encrypt);
    } catch (const CopyError&) {
      throw;
    } catch (const WriteConflictError& e) {
      refuse(RefusalKind::WriteConflict, "add-time", e.what(), fp.target.channel);
    }
    if (!fp.remove_pair) check_images(fp, fp.new_rdat, fp.new_ridx, keys, "verify-failed");
    std::vector<ui1>().swap(fp.old_rdat);  // only the snapshots are needed from here on
    std::vector<ui1>().swap(fp.old_ridx);
    fp.raw.ridx.clear();
    fp.raw.ridx.shrink_to_fit();
    fp.decoded.clear();
    fp.decoded.shrink_to_fit();

    RecordFileEdit e;
    e.level = fp.target.level;
    e.channel = fp.target.channel;
    e.segment = fp.target.segment;
    e.file = fp.rel_rdat;
    e.records_before = fp.n_before;
    e.removed = removed_per_file[p];
    e.added = static_cast<si8>(fp.adds.size());
    e.records_after = e.records_before - e.removed + e.added;
    e.pair_removed = fp.remove_pair;
    report.files.push_back(e);
    report.removed += e.removed;
    report.added += e.added;
  }
  if (opts.dry_run) {
    progress.finish();
    return report;
  }
  ctl.check();

  // ---- B: stage the new files beside the old ----------------------------------------
  const std::string id = detail::random_token();
  JPlan plan;
  plan.root = root;
  plan.id = id;
  for (const auto& fp : plans) {
    plan.files.push_back(make_jfile(root, fp.rel_rdat, id, true, !fp.remove_pair));
    plan.files.push_back(make_jfile(root, fp.rel_ridx, id, true, !fp.remove_pair));
    for (int k = 0; k < 2; ++k) {
      JFile& f = plan.files[plan.files.size() - 2 + static_cast<std::size_t>(k)];
      const Snap& sn = k == 0 ? fp.rdat_snap : fp.ridx_snap;
      f.has_snap = true;
      f.snap_size = sn.size;
      f.snap_mtime = sn.mtime;
      if (!fp.remove_pair) {
        f.has_crc = true;
        f.new_crc = crc::calculate(k == 0 ? fp.new_rdat : fp.new_ridx);
      }
    }
  }
  const auto cleanup_staged = [&] {
    for (const auto& f : plan.files) {
      std::error_code ec;
      fs::remove(f.fresh, ec);
    }
  };
  try {
    for (std::size_t p = 0; p < plans.size(); ++p) {
      const auto& fp = plans[p];
      if (fp.remove_pair) continue;
      ctl.check();
      write_new(plan.files[2 * p].fresh, fp.new_rdat, fp.rdat, &steps);
      progress.add(fp.new_rdat.size());
      write_new(plan.files[2 * p + 1].fresh, fp.new_ridx, fp.ridx, &steps);
      progress.add(fp.new_ridx.size());
    }
    point(&steps);
    flush_dirs(plan.dirs());
    // ---- C: read back and verify against the intended result -----------------------
    for (std::size_t p = 0; p < plans.size(); ++p) {
      const auto& fp = plans[p];
      if (fp.remove_pair) continue;
      ctl.check();
      const auto got_rdat = slurp(plan.files[2 * p].fresh);
      const auto got_ridx = slurp(plan.files[2 * p + 1].fresh);
      progress.add(got_rdat.size() + got_ridx.size());
      if (got_rdat != fp.new_rdat || got_ridx != fp.new_ridx)
        refuse(RefusalKind::Io, "verify-failed",
               "the new annotation files read back from disk differ from what was written; "
               "nothing was changed");
      check_images(fp, got_rdat, got_ridx, fp.raw.keys, "verify-failed");
    }
    ctl.check();
    progress.finish();  // the last point at which a cancel is honoured
  } catch (const SimulatedCrash&) {
    throw;
  } catch (...) {
    cleanup_staged();
    throw;
  }

  // ---- D: the marker (the journal), durable before anything is moved -----------------
  try {
    point(&steps);
    detail::write_operation_marker(root, marker_for(plan));
    note("marker-durable");
  } catch (const SimulatedCrash&) {
    throw;
  } catch (...) {
    cleanup_staged();
    throw;
  }

  // ---- E..I: from here the edit is finished or rolled back, never abandoned ------------
  try {
    point(&steps);
    // E: the old files are still what was read (a writer or another program
    // may have appended since): the marker is down, so no new write starts.
    for (std::size_t p = 0; p < plans.size(); ++p) {
      for (int k = 0; k < 2; ++k) {
        const auto& f = plan.files[2 * p + static_cast<std::size_t>(k)];
        const Snap& was = k == 0 ? plans[p].rdat_snap : plans[p].ridx_snap;
        std::error_code ec;
        const auto size = fs::file_size(f.target, ec);
        std::vector<ui1> head(UH);
        std::ifstream in(f.target, std::ios::binary);
        in.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(UH));
        if (ec || size != was.size || !std::equal(head.begin(), head.end(), was.head.begin()))
          refuse(RefusalKind::WriteConflict, "records-changed",
                 "the annotation files changed while this call was working (another program or "
                 "writer added or changed annotations): nothing was changed; try again");
      }
    }
    settle(plan, Dir::Forward, &steps);
  } catch (const SimulatedCrash&) {
    throw;
  } catch (...) {
    const std::string why = [] {
      try {
        throw;
      } catch (const std::exception& e) {
        return std::string(e.what());
      } catch (...) {
        return std::string("unknown error");
      }
    }();
    const bool conflict = [] {
      try {
        throw;
      } catch (const CopyError& e) {
        return e.code() == "records-changed";
      } catch (...) {
        return false;
      }
    }();
    // From the removal of the old files on there is no way back: finish.
    const bool can_back = std::none_of(plan.files.begin(), plan.files.end(), [](const JFile& f) {
      return f.had_old && state_of(f) == St::S3;
    });
    if (!can_back) {
      settle(plan, Dir::Forward, nullptr);
      return report;
    }
    try {
      settle(plan, Dir::Back, nullptr);
    } catch (const std::exception& e) {
      refuse(RefusalKind::Io, "edit-failed",
             "the annotation edit failed (" + why + ") and could not be rolled back (" + e.what() +
                 "): the session carries an operation marker; run recover_session with apply=True "
                 "to settle it");
    }
    if (conflict) throw;
    refuse(RefusalKind::Io, "edit-failed",
           "the annotations were not changed: " + why +
               " (on Windows, another program holding a record file open prevents the swap)");
  }
  return report;
}

// ================================================================================
// interrupted edits
// ================================================================================

namespace {

/// A record file pair that is whole and agrees with itself, judged from the
/// files alone (no password: record bodies are not read, only walked): both
/// headers and body CRCs, the counts, every record's CRC, and the index
/// pointing at exactly the records the data holds, to the last byte.
bool pair_consistent(const fs::path& rdat, const fs::path& ridx) {
  try {
    if (!has(rdat) || !has(ridx)) return false;
    const auto d = slurp(rdat), x = slurp(ridx);
    if (d.size() < UH || x.size() < UH || (x.size() - UH) % RIDX != 0) return false;
    const auto du = fmt::UniversalHeader::parse(d), xu = fmt::UniversalHeader::parse(x);
    if (du.file_type_string != fmt::FILE_TYPE_RECORD_DATA ||
        xu.file_type_string != fmt::FILE_TYPE_RECORD_INDICES)
      return false;
    if (du.header_crc != fmt::CRC_NO_ENTRY && !du.header_crc_valid(d)) return false;
    if (xu.header_crc != fmt::CRC_NO_ENTRY && !xu.header_crc_valid(x)) return false;
    if (du.body_crc != fmt::CRC_NO_ENTRY &&
        crc::calculate(std::span<const ui1>(d).subspan(UH)) != du.body_crc)
      return false;
    if (xu.body_crc != fmt::CRC_NO_ENTRY &&
        crc::calculate(std::span<const ui1>(x).subspan(UH)) != xu.body_crc)
      return false;
    const si8 n = static_cast<si8>((x.size() - UH) / RIDX);
    if ((du.number_of_entries != n && du.number_of_entries != -1) ||
        (xu.number_of_entries != n && xu.number_of_entries != -1))
      return false;
    std::size_t off = UH;
    for (si8 i = 0; i < n; ++i) {
      if (off + RHDR > d.size()) return false;
      const ui4 nb = read<ui4>(d, off + 12);
      if (nb > d.size() - off - RHDR) return false;
      const ui4 stored = read<ui4>(d, off);
      if (stored != fmt::CRC_NO_ENTRY &&
          crc::calculate(std::span<const ui1>(d).subspan(off + 4, RHDR - 4 + nb)) != stored)
        return false;
      if (read<si8>(x, UH + static_cast<std::size_t>(i) * RIDX + 8) != static_cast<si8>(off))
        return false;
      off += RHDR + nb;
    }
    return off == d.size();
  } catch (const std::exception&) {
    return false;
  }
}

/// An edit marker whose plan cannot be used: decide from the files alone.
/// Every record pair an edit left traces of (a `.new` / `.old` file beside it)
/// must end whole: a pair that is intact stays; a mixed or missing one is
/// restored from an intact `.old` pair; anything else is refused with the
/// files to look at. NOTHING IS EVER DELETED here: leftovers (and a pair put
/// aside) move to `<session>.edit-leftovers/`, because they may hold the only
/// copy of something. The marker is removed last.
InterruptedRecordEdit settle_from_files(const fs::path& root, const OperationMarker& m, bool apply) {
  struct Group {
    fs::path dir;
    std::string stem;  // "name" of name.rdat / name.ridx
    std::vector<fs::path> leftovers;
    fs::path old_rdat, old_ridx;
  };
  std::map<std::string, Group> groups;
  std::vector<fs::path> loose;  // leftovers that belong to no pair (a marker .part)
  for (const auto& f : leftover_files(root)) {
    const std::string name = u8(f.filename());
    if (name == std::string(detail::OPERATION_MARKER) + ".part") {
      loose.push_back(f);
      continue;
    }
    // <stem>.<rdat|ridx>.<32 hex>.<new|old>
    const std::string rest = name.substr(0, name.size() - 4 - 33);
    const std::string ext = rest.substr(rest.size() - 4);
    const std::string stem = rest.substr(0, rest.size() - 5);
    Group& g = groups[u8(f.parent_path()) + "/" + stem];
    g.dir = f.parent_path();
    g.stem = stem;
    g.leftovers.push_back(f);
    if (name.compare(name.size() - 4, 4, ".old") == 0)
      (ext == "rdat" ? g.old_rdat : g.old_ridx) = f;
  }
  struct Move {
    fs::path from, to;
    bool to_aside = false;
  };
  std::vector<Move> moves;
  std::vector<std::string> kept, restored, problems, files;
  const fs::path aside_root =
      root.parent_path() / from8(u8(root.filename()) + ".edit-leftovers");
  const auto rel = [&](const fs::path& f) { return detail::path_generic_utf8(fs::relative(f, root)); };
  std::set<fs::path> moved_aside;
  const auto aside = [&](const fs::path& f) {
    if (moved_aside.insert(f).second) moves.push_back({f, aside_root / fs::relative(f, root), true});
  };
  for (auto& [key, g] : groups) {
    const fs::path trdat = g.dir / from8(g.stem + ".rdat"), tridx = g.dir / from8(g.stem + ".ridx");
    files.push_back(rel(trdat));
    if (pair_consistent(trdat, tridx)) {
      kept.push_back(rel(trdat));
      for (const auto& l : g.leftovers) aside(l);
    } else if ((!g.old_rdat.empty() || !g.old_ridx.empty()) &&
               pair_consistent(g.old_rdat.empty() ? trdat : g.old_rdat,
                               g.old_ridx.empty() ? tridx : g.old_ridx)) {
      // the old pair: each half is its `.old` file when there is one, else the
      // target itself (a swap that stopped between the two halves)
      restored.push_back(rel(trdat));
      for (const auto& l : g.leftovers)
        if (l != g.old_rdat && l != g.old_ridx) aside(l);
      if (!g.old_rdat.empty()) {
        if (has(trdat)) aside(trdat);
        moves.push_back({g.old_rdat, trdat, false});
      }
      if (!g.old_ridx.empty()) {
        if (has(tridx)) aside(tridx);
        moves.push_back({g.old_ridx, tridx, false});
      }
    } else {
      std::string names;
      for (const auto& l : g.leftovers) names += (names.empty() ? "" : ", ") + rel(l);
      problems.push_back(rel(trdat) + " / " + rel(tridx) + " (leftovers: " + names + ")");
    }
  }
  for (const auto& l : loose) aside(l);
  if (!problems.empty()) {
    std::string list;
    for (const auto& pr : problems) list += (list.empty() ? "" : "; ") + pr;
    refuse(RefusalKind::Format, "edit-unsettled",
           "the marker's plan of the interrupted annotation edit cannot be read, and the files "
           "alone do not say how to finish it: no record pair is whole (and no whole .old pair "
           "exists) for " + list +
               ". Nothing was touched and no file was deleted: look at these files, put a whole "
               "pair (.rdat and .ridx) in place by hand, then run recover_session again");
  }
  InterruptedRecordEdit e;
  e.id = m.id;
  e.files = files;
  e.action = "settle";
  e.reason = "the marker's plan cannot be read, so the files alone decided: " +
             std::to_string(kept.size()) + " record pair(s) whole and kept as they are, " +
             std::to_string(restored.size()) + " restored from their .old files (that removal was NOT done), " +
             std::to_string(moved_aside.size()) + " other file(s) " + (apply ? "moved" : "to be moved") +
             " aside (never deleted) to " + u8(aside_root.filename()) + " beside the session";
  if (!apply) return e;
  std::error_code ec;
  for (const auto& mv : moves) {
    fs::path to = mv.to;
    if (mv.to_aside) {
      fs::create_directories(to.parent_path(), ec);
      for (int n = 1; has(to); ++n)
        to = mv.to.parent_path() / from8(u8(mv.to.filename()) + "." + std::to_string(n));
    } else if (has(to)) {
      throw IoError("cannot restore " + u8(mv.from) + ": " + u8(to) + " is in the way");
    }
    note("rename", u8(mv.from) + "|" + u8(to));
    detail::replace_file(mv.from, to);
  }
  std::vector<fs::path> dirs;
  for (const auto& mv : moves) {
    dirs.push_back(mv.from.parent_path());
    dirs.push_back(mv.to.parent_path());
  }
  flush_dirs(dirs);
  detail::remove_operation_marker(root);
  note("marker-removed");
  e.applied = true;
  return e;
}

/// Is this marker the one of an annotation edit? A marker whose first lines
/// cannot be read still is, when the files of an edit lie beside the records.
bool is_edit_marker(const fs::path& root, const OperationMarker& m) {
  if (m.operation == OP) return true;
  return m.operation == "unknown" && !leftover_files(root).empty();
}

bool is_unreadable_plan(const CopyError& e) { return e.code() == "edit-journal-inconsistent"; }

}  // namespace

std::optional<InterruptedRecordEdit> interrupted_record_edit(const std::string& path) {
  if (path_has_suffix(path, ".tar")) return std::nullopt;
  const fs::path root = from8(path);
  const auto m = detail::read_operation_marker(root);
  if (!m || !is_edit_marker(root, *m)) return std::nullopt;
  try {
    const auto plan = plan_from_marker(root, *m);
    if (plan) {
      InterruptedRecordEdit e;
      e.id = plan->id;
      for (const auto& f : plan->files) e.files.push_back(f.rel);
      const Decision d = decide(*plan);
      e.action = d.dir == Dir::Forward ? "finish" : "roll back";
      e.reason = d.why;
      return e;
    }
  } catch (const CopyError& e) {
    if (!is_unreadable_plan(e)) throw;
  }
  return settle_from_files(root, *m, false);
}

std::optional<InterruptedRecordEdit> finish_interrupted_record_edit(const std::string& path,
                                                                    int crash_at, int fail_at) {
  const fs::path root = session_root(path);
  const auto m = detail::read_operation_marker(root);
  if (!m || !is_edit_marker(root, *m)) return std::nullopt;
  Steps steps;
  steps.crash_at = crash_at;
  steps.fail_at = fail_at;
  try {
    const auto plan = plan_from_marker(root, *m);
    if (plan) {
      InterruptedRecordEdit e;
      e.id = plan->id;
      for (const auto& f : plan->files) e.files.push_back(f.rel);
      const Decision d = decide(*plan);
      e.action = d.dir == Dir::Forward ? "finish" : "roll back";
      e.reason = d.why;
      settle(*plan, d.dir, (crash_at >= 0 || fail_at >= 0) ? &steps : nullptr);
      e.applied = true;
      return e;
    }
  } catch (const CopyError& e) {
    if (!is_unreadable_plan(e)) throw;
  }
  return settle_from_files(root, *m, true);
}

std::vector<std::string> record_edit_leftovers(const std::string& path, bool apply) {
  std::vector<std::string> out;
  if (path_has_suffix(path, ".tar")) return out;
  const fs::path root = from8(path);
  if (!fs::is_directory(root)) return out;
  if (detail::read_operation_marker(root)) return out;
  for (const auto& f : leftover_files(root)) {
    out.push_back(detail::path_generic_utf8(fs::relative(f, root)));
    if (apply) {
      std::error_code ec;
      fs::remove(f, ec);
    }
  }
  return out;
}

namespace testing {
void set_edit_observer(EditObserver observer) {
  std::lock_guard<std::mutex> lk(g_obs_mu);
  g_obs = std::move(observer);
}
void set_edit_skip_flush(bool skip) {
  std::lock_guard<std::mutex> lk(g_obs_mu);
  g_skip_flush = skip;
}
}  // namespace testing

}  // namespace mef3io
