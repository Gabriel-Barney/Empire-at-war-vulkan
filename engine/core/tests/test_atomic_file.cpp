// CU2 t11: write_file_atomic / atomic_temp_path (atomic_file.hpp). Every run works in its own temp
// directory (pid + clock), so parallel ctest runs and other trees never share a path.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "alamo/core/atomic_file.hpp"
#include "alamo/core/file_provider.hpp"

#include <windows.h>

namespace fs = std::filesystem;
using alamo::AtomicWriteOptions;
using alamo::AtomicWriteResult;
using alamo::write_file_atomic;

static int g_fail = 0;
#define CHECK(c)                                                     \
    do {                                                             \
        if (!(c)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++g_fail;                                                \
        }                                                            \
    } while (0)

namespace {

fs::path g_dir;

std::vector<uint8_t> bytes_of(std::string_view s) { return std::vector<uint8_t>(s.begin(), s.end()); }

std::vector<uint8_t> read(const fs::path& p) {
    std::vector<uint8_t> out;
    if (!alamo::read_host_file(p, out)) out.assign(1, 0xEE);  // a marker no test writes
    return out;
}

// Leftover temp files (".tmp") in g_dir.
size_t temp_files() {
    std::error_code ec;
    size_t n = 0;
    for (const fs::path& e : alamo::list_directory(g_dir, ec))
        if (e.extension() == ".tmp") ++n;
    return ec ? 9999 : n;
}

void test_temp_path() {
    const fs::path t = g_dir / "a.bin";
    const fs::path x = alamo::atomic_temp_path(t), y = alamo::atomic_temp_path(t);
    CHECK(x != y);
    CHECK(x.extension() == ".tmp" && x.parent_path() == t.parent_path());
    CHECK(x.filename().string().rfind("a.bin.", 0) == 0);
}

void test_write_and_replace() {
    const fs::path t = g_dir / "cache.bin";
    CHECK(write_file_atomic(t, bytes_of("first version")) == AtomicWriteResult::Ok);
    CHECK(read(t) == bytes_of("first version"));
    CHECK(write_file_atomic(t, bytes_of("2nd")) == AtomicWriteResult::Ok);  // shorter: no stale tail
    CHECK(read(t) == bytes_of("2nd"));
    AtomicWriteOptions synced;
    synced.attempts = 10;
    synced.max_backoff_ms = 50;
    synced.flush_to_disk = true;
    std::vector<uint8_t> big(3u << 20);
    for (size_t i = 0; i < big.size(); ++i) big[i] = static_cast<uint8_t>(i * 7u + (i >> 11));
    CHECK(write_file_atomic(t, big, synced) == AtomicWriteResult::Ok);
    CHECK(read(t) == big);
    CHECK(write_file_atomic(t, {}) == AtomicWriteResult::Ok);
    std::vector<uint8_t> empty;
    CHECK(alamo::read_host_file(t, empty) && empty.empty());
    CHECK(temp_files() == 0);
    // A long path (over MAX_PATH) works: write_file_atomic applies long_path itself.
    fs::path deep = g_dir;
    for (int i = 0; i < 6; ++i) deep /= std::string(48, static_cast<char>('a' + i));
    std::error_code ec;
    fs::create_directories(alamo::long_path(deep), ec);
    CHECK(!ec);
    const fs::path deep_file = deep / "deep.bin";
    CHECK(deep_file.native().size() > MAX_PATH);
    CHECK(write_file_atomic(deep_file, bytes_of("deep")) == AtomicWriteResult::Ok);
    CHECK(read(deep_file) == bytes_of("deep"));
    CHECK(alamo::remove_all_long(g_dir / std::string(48, 'a'), ec) != static_cast<std::uintmax_t>(-1) && !ec);
}

void test_failures() {
    // Missing directory: the temp file cannot be created.
    const fs::path missing = g_dir / "no_such_dir" / "x.bin";
    CHECK(write_file_atomic(missing, bytes_of("x")) == AtomicWriteResult::WriteFailed);
    std::error_code ec;
    CHECK(!fs::exists(missing, ec));
    // The target is a directory: the rename can never succeed, and the temp file is cleaned up.
    const fs::path as_dir = g_dir / "is_a_dir";
    fs::create_directories(as_dir, ec);
    AtomicWriteOptions quick;
    quick.attempts = 2;
    CHECK(write_file_atomic(as_dir, bytes_of("x"), quick) == AtomicWriteResult::ReplaceFailed);
    CHECK(fs::is_directory(as_dir, ec));
    CHECK(temp_files() == 0);
    fs::remove(as_dir, ec);
}

void test_open_target() {
    const fs::path t = g_dir / "held.bin";
    CHECK(write_file_atomic(t, bytes_of("old")) == AtomicWriteResult::Ok);
    // A foreign reader without FILE_SHARE_DELETE blocks the replace for as long as it holds the file;
    // the target keeps its old content and the temp file is removed.
    HANDLE h = CreateFileW(alamo::long_path(t).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(h != INVALID_HANDLE_VALUE);
    AtomicWriteOptions quick;
    quick.attempts = 3;
    CHECK(write_file_atomic(t, bytes_of("new"), quick) == AtomicWriteResult::ReplaceFailed);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    CHECK(read(t) == bytes_of("old"));
    CHECK(temp_files() == 0);
    // Released within the retry budget (16 attempts, 1 ms apart = ~15 ms; 10 with back-off = ~0.26 s).
    h = CreateFileW(alamo::long_path(t).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(h != INVALID_HANDLE_VALUE);
    std::thread release([h] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    });
    AtomicWriteOptions patient;
    patient.attempts = 10;
    patient.max_backoff_ms = 50;
    CHECK(write_file_atomic(t, bytes_of("new"), patient) == AtomicWriteResult::Ok);
    release.join();
    CHECK(read(t) == bytes_of("new"));
    CHECK(temp_files() == 0);
}

// 8 writers replace one file while 2 readers read it: every read is one complete version, never a torn
// mix, and no temp file is left behind. Versions are 64 KiB of one repeated byte.
void test_concurrent() {
    const fs::path t = g_dir / "shared.bin";
    constexpr size_t kSize = 64u << 10;
    CHECK(write_file_atomic(t, std::vector<uint8_t>(kSize, 1)) == AtomicWriteResult::Ok);
    std::atomic<int> write_failed{0}, replace_failed{0}, torn{0}, reads{0};
    std::atomic<bool> stop{false};
    std::vector<std::thread> readers;
    for (int r = 0; r < 2; ++r)
        readers.emplace_back([&] {
            std::vector<uint8_t> b;
            while (!stop.load()) {
                if (!alamo::read_host_file(t, b)) continue;  // between the old file's removal and the rename
                ++reads;
                bool same = b.size() == kSize;
                for (size_t i = 1; same && i < b.size(); ++i) same = b[i] == b[0];
                if (!same) ++torn;
            }
        });
    std::vector<std::thread> writers;
    for (int w = 0; w < 8; ++w)
        writers.emplace_back([&, w] {
            for (int i = 0; i < 40; ++i) {
                const std::vector<uint8_t> v(kSize, static_cast<uint8_t>(2 + w * 40 + i));
                const AtomicWriteResult r = write_file_atomic(t, v);
                if (r == AtomicWriteResult::WriteFailed) ++write_failed;
                if (r == AtomicWriteResult::ReplaceFailed) ++replace_failed;
            }
        });
    for (auto& th : writers) th.join();
    stop = true;
    for (auto& th : readers) th.join();
    const std::vector<uint8_t> last = read(t);
    bool whole = last.size() == kSize;
    for (size_t i = 1; whole && i < last.size(); ++i) whole = last[i] == last[0];
    std::printf("  concurrent: %d reads, %d torn, %d write / %d replace failures\n", reads.load(), torn.load(),
                write_failed.load(), replace_failed.load());
    CHECK(whole);
    CHECK(torn.load() == 0);
    CHECK(write_failed.load() == 0);
    CHECK(temp_files() == 0);
}

}  // namespace

int main() {
    std::error_code ec;
    g_dir = fs::temp_directory_path(ec) /
            ("alamo_atomic_file_test_" + std::to_string(GetCurrentProcessId()) + "_" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(g_dir, ec);
    if (ec) {
        std::printf("FAIL cannot create %s\n", g_dir.string().c_str());
        return 1;
    }
    test_temp_path();
    test_write_and_replace();
    test_failures();
    test_open_target();
    test_concurrent();
    alamo::remove_all_long(g_dir, ec);
    if (g_fail) {
        std::printf("%d check(s) failed\n", g_fail);
        return 1;
    }
    std::printf("atomic_file tests passed\n");
    return 0;
}
