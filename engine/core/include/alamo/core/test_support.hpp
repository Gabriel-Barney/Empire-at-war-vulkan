// Shared by the test executables only (header-only; nothing in a shipped library includes it).
// CHECK counts failures in g_fail; a test main returns g_fail ? 1 : 0.
#pragma once
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/core/file_provider.hpp"

inline int g_fail = 0;

#define CHECK(c)                                                     \
    do {                                                             \
        if (!(c)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++g_fail;                                                \
        }                                                            \
    } while (0)

namespace alamo::testing {

// Value of an environment variable, "" when unset.
inline std::string env_or_empty(const char* name) {
    char* v = nullptr;
    size_t len = 0;
    std::string out;
    if (_dupenv_s(&v, &len, name) == 0 && v) {
        out = v;
        std::free(v);
    }
    return out;
}

// Creates parent folders, writes `content` in binary mode.
inline void write_file(const std::filesystem::path& p, std::string_view content) {
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f.write(content.data(), static_cast<std::streamsize>(content.size()));
}

// In-memory FileProvider keyed by normalize_path().
class MemoryProvider final : public FileProvider {
public:
    void add(std::string_view path, std::string_view text) {
        files_[normalize_path(path)] = std::vector<uint8_t>(text.begin(), text.end());
    }
    void add(std::string_view path, std::vector<uint8_t> bytes) { files_[normalize_path(path)] = std::move(bytes); }
    bool read(std::string_view path, std::vector<uint8_t>& out) const override {
        const auto it = files_.find(normalize_path(path));
        out.clear();
        if (it == files_.end()) return false;
        out = it->second;
        return true;
    }
    bool exists(std::string_view path) const override { return files_.count(normalize_path(path)) != 0; }
    std::string origin(std::string_view path) const override { return exists(path) ? "memory" : ""; }

private:
    std::map<std::string, std::vector<uint8_t>> files_;
};

}  // namespace alamo::testing
