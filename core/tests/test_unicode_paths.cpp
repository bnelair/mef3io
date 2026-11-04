// mef3io — non-ASCII folder and session names through every file-system call.
// The core takes UTF-8 text; on Windows each call must get a wide path (never
// the ANSI code page): every path goes through detail::utf8_path and comes
// back through detail::path_utf8. On Linux/macOS this pins that the bytes
// survive; on Windows (GitHub's CI) it is the real check.
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <string>
#include <vector>

#include "mef3io/anonymize.hpp"
#include "mef3io/merge.hpp"
#include "mef3io/recover.hpp"
#include "mef3io/session.hpp"
#include "mef3io/session_writer.hpp"
#include "mef3io/source.hpp"
#include "mef3io/tar.hpp"
#include "mef3io/transform.hpp"
#include "mef3io/validate.hpp"

using namespace mef3io;
namespace fsys = std::filesystem;

namespace {

std::string u(const fsys::path& p) { return detail::path_utf8(p); }

fsys::path fresh(const std::string& name) {
  const auto dir = fsys::temp_directory_path() / detail::utf8_path(name);
  fsys::remove_all(dir);
  return dir;
}

constexpr si8 START = 1577836800000000;

void write_some(SessionWriter& w, si8 start, int base, const std::string& note) {
  std::vector<si4> a(1000), b(500);
  for (int i = 0; i < 1000; ++i) a[i] = base + i;
  for (int i = 0; i < 500; ++i) b[i] = base + 3 * i;
  w.write_int32("ch1", a, 1.0, start, 100.0);
  w.write_int32("ch2", b, 1.0, start, 50.0);
  Record r;
  r.type = "Note";
  r.time = start + 1000;
  r.text = note;
  w.write_records(std::nullopt, {r});
}

std::size_t sample_count(const std::string& path, const std::string& ch) {
  Session s(path);
  std::size_t n = 0;
  for (const auto& run : s.read_runs(ch)) n += run.samples.size();
  return n;
}

}  // namespace

TEST_CASE("every file-system call takes a non-ASCII folder and session name") {
  for (const auto& parts : std::vector<std::pair<std::string, std::string>>{
           {"テスト", "患者.mefd"}, {"Ünïcödé dir", "Ünïcödé.mefd"}}) {
    const auto root = fresh("mef3io_unicode_" + std::to_string(parts.first.size()));
    fsys::create_directories(root / detail::utf8_path(parts.first));
    const fsys::path parent = root / detail::utf8_path(parts.first);
    const std::string sess = u(parent / detail::utf8_path(parts.second));

    // write, read, append in place, annotate
    {
      SessionWriter w(sess, true, "", "");
      write_some(w, START, 0, "first");
    }
    CHECK(sample_count(sess, "ch1") == 1000);
    {
      SessionWriter w(sess, false, "", "");
      write_some(w, START + 10 * 1000000, 5000, "second");
    }
    CHECK(sample_count(sess, "ch1") == 2000);
    CHECK(validate_session(sess).ok());

    // archive, read the tar, extract
    const std::string tar = archive_session(sess);
    CHECK(sample_count(tar, "ch1") == 2000);
    const std::string dest = u(parent / detail::utf8_path("抽出 Ünï.mefd"));
    CHECK(extract_session(tar, dest) == dest);
    CHECK(sample_count(dest, "ch2") == 1000);
    CHECK(validate_session(dest).ok());
    (void)recover_session(dest, false);  // a dry run reads every file

    // copies
    AnonymizeOptions ao;
    ao.shift_us = 5000LL * 86400LL * 1000000LL;
    const std::string anon = u(parent / detail::utf8_path("匿名 Çopy.mefd"));
    CHECK(anonymize_session(sess, anon, ao).verification.ok);
    CHECK(sample_count(anon, "ch1") == 2000);

    const std::string tr = u(parent / detail::utf8_path("変換 Çopy.mefd"));
    CHECK(transform_session(tar, tr, TransformOptions{}).verification.ok);
    CHECK(sample_count(tr, "ch2") == 1000);

    const std::string other = u(parent / detail::utf8_path("別 " + parts.second));
    {
      SessionWriter w(other, true, "", "");
      write_some(w, START + 1000LL * 1000000, 0, "later");
    }
    const std::string merged = u(parent / detail::utf8_path("統合 Çopy.mefd"));
    CHECK(merge_sessions({sess, other}, merged, MergeOptions{}).verification.ok);
    CHECK(sample_count(merged, "ch1") == 3000);
    CHECK(validate_session(merged).ok());
    fsys::remove_all(root);
  }
}
