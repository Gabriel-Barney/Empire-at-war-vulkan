// Atomic whole-file replace for on-disk caches (CU2 t11: shaderxlat ShaderCache, materials FxCache,
// rhi PipelineCache). The data goes to a unique temp file beside the target, which is then renamed over
// the target, so a reader sees the old file or the new one, never a partial write. Readers should use
// read_host_file (file_provider.hpp): it opens with FILE_SHARE_DELETE, so it never blocks a replace.
//
// What to do when the replace loses (another writer or a foreign reader holds the target past the retry
// budget) is the caller's policy; the result says which step failed. Tool and render caches only: no sim
// code reads these files.
#pragma once
#include <cstdint>
#include <filesystem>
#include <span>

namespace alamo {

struct AtomicWriteOptions {
    // Rename attempts. Before retry k (k = 1, 2, ...) the writer sleeps min(2^(k-1), max_backoff_ms) ms,
    // so the default (16 attempts, 1 ms) retries every millisecond and {10, 50} backs off 1, 2, 4 ... 50 ms.
    int attempts = 16;
    int max_backoff_ms = 1;
    // Flush the temp file to the device (FlushFileBuffers) before the rename.
    bool flush_to_disk = false;
};

enum class AtomicWriteResult : uint8_t {
    Ok,
    WriteFailed,    // the temp file could not be created or written (missing directory, disk full, ...)
    ReplaceFailed,  // written, but every rename over the target failed; the target is unchanged
};

// "<target>.<pid>.<tid>.<n>.tmp": unique per process, thread and call (n is a process-wide counter).
std::filesystem::path atomic_temp_path(const std::filesystem::path& target);

// Writes `data` to atomic_temp_path(target) and renames it over `target` (MoveFileExW with
// MOVEFILE_REPLACE_EXISTING). The parent directory must exist. The path goes through long_path, so it
// may exceed MAX_PATH. On any failure the temp file is removed.
AtomicWriteResult write_file_atomic(const std::filesystem::path& target, std::span<const uint8_t> data,
                                    const AtomicWriteOptions& options = {});

}  // namespace alamo
