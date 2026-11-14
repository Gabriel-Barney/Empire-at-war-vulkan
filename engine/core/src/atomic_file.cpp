#include "alamo/core/atomic_file.hpp"

#include <algorithm>
#include <atomic>
#include <string>

#include "alamo/core/file_provider.hpp"
#include "alamo/core/host_fs.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace alamo {

std::filesystem::path atomic_temp_path(const std::filesystem::path& target) {
    static std::atomic<uint64_t> counter{0};
    std::filesystem::path tmp = target;
    tmp += "." + std::to_string(GetCurrentProcessId()) + "." + std::to_string(GetCurrentThreadId()) + "." +
           std::to_string(counter.fetch_add(1, std::memory_order_relaxed)) + ".tmp";
    return tmp;
}

namespace {

bool write_whole(const std::filesystem::path& path, std::span<const uint8_t> data, bool flush) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    size_t done = 0;
    while (ok && done < data.size()) {
        const DWORD want = static_cast<DWORD>(std::min<size_t>(data.size() - done, size_t{1} << 30));
        DWORD put = 0;
        ok = WriteFile(h, data.data() + done, want, &put, nullptr) && put == want;
        done += put;
    }
    if (ok && flush) ok = FlushFileBuffers(h) != 0;
    ok = CloseHandle(h) != 0 && ok;
    return ok;
}

}  // namespace

AtomicWriteResult write_file_atomic(const std::filesystem::path& target, std::span<const uint8_t> data,
                                    const AtomicWriteOptions& options) {
    const std::filesystem::path final_path = long_path(target);
    const std::filesystem::path tmp = atomic_temp_path(final_path);
    if (!write_whole(tmp, data, options.flush_to_disk)) {
        DeleteFileW(tmp.c_str());
        return AtomicWriteResult::WriteFailed;
    }
    const int attempts = std::max(options.attempts, 1);
    for (int attempt = 0; attempt < attempts; ++attempt) {
        if (attempt > 0) {
            // A concurrent writer, or a reader without FILE_SHARE_DELETE, may hold the target briefly.
            const int shift = std::min(attempt - 1, 30);
            Sleep(static_cast<DWORD>(std::max(std::min(1 << shift, options.max_backoff_ms), 0)));
        }
        if (MoveFileExW(tmp.c_str(), final_path.c_str(), MOVEFILE_REPLACE_EXISTING)) return AtomicWriteResult::Ok;
    }
    DeleteFileW(tmp.c_str());
    return AtomicWriteResult::ReplaceFailed;
}

}  // namespace alamo
