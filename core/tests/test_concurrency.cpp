// Concurrency: what may run on several threads at once now that the Python
// bindings release the GIL for opens and reads (#79).
//
// These are SMOKE tests: a data race need not crash or corrupt anything on a
// given run. The real check is ThreadSanitizer, which flagged both of the bugs
// pinned here; build with -fsanitize=thread to run them under it:
//   cmake -S core -B build-tsan -DMEF3IO_BUILD_TESTS=ON \
//     -DCMAKE_CXX_FLAGS="-fsanitize=thread -g" && cmake --build build-tsan
//   build-tsan/mef3io_tests "[concurrency]"
#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <filesystem>
#include <thread>
#include <vector>

#include "mef3io/crypto.hpp"
#include "mef3io/reader.hpp"
#include "mef3io/session.hpp"
#include "mef3io/session_writer.hpp"

using namespace mef3io;
namespace fsys = std::filesystem;

namespace {
constexpr si8 START = 1577836800000000;

std::string write_session(const std::string& name, int segments, const std::string& pw1 = "",
                          const std::string& pw2 = "") {
  const auto dir = fsys::temp_directory_path() / name;
  fsys::remove_all(dir);
  SessionWriter w(dir.string(), pw1, pw2);
  std::vector<si4> a(2500);
  for (int i = 0; i < 2500; ++i) a[i] = i;
  for (int s = 0; s < segments; ++s)
    w.write_int32("ch1", a, 1.0, START + s * 20'000'000LL, 250.0, {}, s > 0);
  return dir.string();
}
}  // namespace

TEST_CASE("AES decryption from several threads at once", "[concurrency]") {
  // The inverse S-box was filled lazily behind an unsynchronised flag.
  std::vector<ui1> key(16, 0x2b), plain(16 * 64);
  for (std::size_t i = 0; i < plain.size(); ++i) plain[i] = static_cast<ui1>(i * 7);
  const auto cipher = crypto::aes128_ecb_encrypt(plain, key);
  std::atomic<int> wrong{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; ++t)
    threads.emplace_back([&] {
      for (int i = 0; i < 50; ++i)
        if (crypto::aes128_ecb_decrypt(cipher, key) != plain) ++wrong;
    });
  for (auto& t : threads) t.join();
  REQUIRE(wrong == 0);
}

TEST_CASE("concurrent reads on ONE Session with index eviction", "[concurrency]") {
  // A 1-byte budget evicts every index at the end of every call, while other
  // threads are still reading theirs: a use-after-free before the indices
  // were pinned ("double free or corruption").
  const auto path = write_session("mef3io_concurrent_reads.mefd", 12);
  Session s(path);
  s.set_index_cache_bytes(1);
  const auto expected = s.read_runs("ch1");
  std::atomic<int> wrong{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 6; ++t)
    threads.emplace_back([&] {
      for (int i = 0; i < 20; ++i) {
        const auto runs = s.read_runs("ch1");
        if (runs.size() != expected.size() || runs[0].samples != expected[0].samples) ++wrong;
        if (s.read_index("ch1").size() != 12 * 1) ++wrong;  // one block per segment
        s.segment_map("ch1");
      }
    });
  for (auto& t : threads) t.join();
  REQUIRE(wrong == 0);
}

TEST_CASE("concurrent opens and reads of an encrypted session", "[concurrency]") {
  const auto path = write_session("mef3io_concurrent_enc.mefd", 4, "pw1", "pw2");
  std::atomic<int> wrong{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 6; ++t)
    threads.emplace_back([&] {
      Reader r(path, "pw2", 1);
      if (r.read("ch1").size() < 4 * 2500) ++wrong;
    });
  for (auto& t : threads) t.join();
  REQUIRE(wrong == 0);
}

TEST_CASE("set_threads and set_index_cache_bytes while other threads read", "[concurrency]") {
  // Both setters were plain stores read by every call (#110 item 4); they are
  // atomics now, and a change applies to the NEXT call. Results never depend
  // on the count, so every read must still match.
  const auto path = write_session("mef3io_concurrent_setters.mefd", 8);
  Reader r(path, "", 2);
  const auto expected = r.read_raw("ch1");
  std::atomic<bool> stop{false};
  std::atomic<int> wrong{0};
  std::vector<std::thread> readers;
  for (int t = 0; t < 4; ++t)
    readers.emplace_back([&] {
      while (!stop.load()) {
        const auto got = r.read_raw("ch1");
        if (got.samples != expected.samples || got.valid != expected.valid) ++wrong;
        r.session().read_index("ch1");
      }
    });
  for (int i = 0; i < 400; ++i) {
    r.set_threads(i % 5);
    r.session().set_index_cache_bytes(i % 2 ? 1 : 0);
    (void)r.threads();
  }
  stop = true;
  for (auto& t : readers) t.join();
  REQUIRE(wrong == 0);
}

TEST_CASE("parallel opens of one session on many threads each", "[concurrency]") {
  // The open's own pool (channel listings, file checks, .tmet parses) inside
  // several concurrent opens: no shared state but the immutable source.
  const auto path = write_session("mef3io_concurrent_par_open.mefd", 24, "pw1", "pw2");
  std::atomic<int> wrong{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t)
    threads.emplace_back([&] {
      Reader r(path, "pw2", 8);
      if (r.info("ch1").n_segments != 24) ++wrong;
      if (r.read("ch1").size() < 24 * 2500) ++wrong;
    });
  for (auto& t : threads) t.join();
  REQUIRE(wrong == 0);
}
