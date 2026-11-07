// Disk cache (see cache.hpp). File layout, little-endian:
//   "AXSC" u32 format | u32 len + key text | u32 len + entry point | u32 len + interface text |
//   u32 words + SPIR-V | u64 FNV-1a of everything before it
#include "alamo/shaderxlat/cache.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <thread>

#include "alamo/core/file_provider.hpp"
#include "alamo/core/hash.hpp"

#include <windows.h>

namespace alamo::shaderxlat {

namespace {
constexpr uint32_t kMagic = 0x43535841u;  // "AXSC"
constexpr uint32_t kFormat = 2;  // 2: key text carries the source hash
constexpr size_t kMaxFile = 64u << 20;

void put32(std::vector<uint8_t>& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
void put_str(std::vector<uint8_t>& b, const std::string& s) {
    put32(b, static_cast<uint32_t>(s.size()));
    b.insert(b.end(), s.begin(), s.end());
}
struct In {
    const std::vector<uint8_t>& b;
    size_t pos = 0;
    bool u32(uint32_t& v) {
        if (pos + 4 > b.size()) return false;
        v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(b[pos + static_cast<size_t>(i)]) << (8 * i);
        pos += 4;
        return true;
    }
    bool str(std::string& s) {
        uint32_t n = 0;
        if (!u32(n) || n > b.size() - pos) return false;
        s.assign(reinterpret_cast<const char*>(b.data() + pos), n);
        pos += n;
        return true;
    }
};

std::string getenv_str(const char* n) {
    char buf[4096];
    const DWORD r = GetEnvironmentVariableA(n, buf, sizeof(buf));
    return (r > 0 && r < sizeof(buf)) ? std::string(buf, r) : std::string();
}

uint64_t process_id() {
    return GetCurrentProcessId();
}

bool replace_file(const std::filesystem::path& from, const std::filesystem::path& to) {
    for (int attempt = 0; attempt < 16; ++attempt) {
        if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING)) return true;
        Sleep(1);  // another process may hold the target open briefly
    }
    return false;
}

// Whole-file read that never blocks a concurrent replace (Windows: FILE_SHARE_DELETE).
bool read_file(const std::filesystem::path& path, std::vector<uint8_t>& out) {
    out.clear();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart >= 32 && static_cast<uint64_t>(size.QuadPart) <= kMaxFile;
    if (ok) {
        out.resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        ok = ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &got, nullptr) && got == out.size();
    }
    CloseHandle(h);
    return ok;
}
}  // namespace

CacheKey CacheKey::make(std::span<const uint8_t> bytecode, const TranslateOptions& options, std::string_view compiler,
                        const Translation& t) {
    CacheKey k;
    k.blob_fnv = alamo::fnv1a64(bytecode.data(), bytecode.size());
    k.blob_crc = alamo::crc32(bytecode.data(), bytecode.size());
    k.blob_size = static_cast<uint32_t>(bytecode.size());
    k.options_key = options.key();
    k.translator_version = kTranslatorVersion;
    k.compiler = std::string(compiler);
    const std::vector<std::string> args = dxc_arguments(t);
    k.source_hash = source_hash_of(t.hlsl, args);
    return k;
}

uint64_t CacheKey::source_hash_of(std::string_view hlsl, std::span<const std::string> args) {
    // Length-prefixed fields, so ("ab","c") and ("a","bc") differ.
    uint64_t h = 0xcbf29ce484222325ull;
    auto field = [&](std::string_view s) {
        const uint64_t n = s.size();
        h = alamo::fnv1a64(&n, sizeof n, h);
        h = alamo::fnv1a64(s.data(), s.size(), h);
    };
    field(hlsl);
    const uint64_t argc = args.size();
    h = alamo::fnv1a64(&argc, sizeof argc, h);
    for (const std::string& a : args) field(a);
    return h;
}

std::string CacheKey::describe() const {
    char buf[200];
    std::snprintf(buf, sizeof(buf), "blob %016llx %08x %u opts %016llx xlat %u src %016llx compiler ",
                  static_cast<unsigned long long>(blob_fnv), blob_crc, blob_size, static_cast<unsigned long long>(options_key),
                  translator_version, static_cast<unsigned long long>(source_hash));
    return buf + compiler;
}

std::string CacheKey::file_name() const {
    const std::string d = describe();
    const uint64_t a = alamo::fnv1a64(d.data(), d.size());
    const uint64_t b = alamo::fnv1a64(d.data(), d.size(), 0x84222325cbf29ce4ull) ^ blob_fnv;
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%016llx%016llx.axs", static_cast<unsigned long long>(a), static_cast<unsigned long long>(b));
    return buf;
}

ShaderCache::ShaderCache() : dir_(default_directory()) {}
ShaderCache::ShaderCache(std::filesystem::path directory) : dir_(std::move(directory)) {}

std::filesystem::path ShaderCache::default_directory() {
    const std::string over = getenv_str("ALAMO_SHADER_CACHE");
    if (!over.empty()) return std::filesystem::path(over);
    const std::string la = getenv_str("LOCALAPPDATA");
    if (!la.empty()) return std::filesystem::path(la) / "alamo" / "shaders";
    std::error_code ec;
    return std::filesystem::temp_directory_path(ec) / "alamo" / "shaders";
}

bool ShaderCache::load(const CacheKey& key, CacheEntry& out) const {
    out = CacheEntry{};
    std::vector<uint8_t> b;
    if (!read_file(alamo::long_path(dir_ / key.file_name()), b) || b.size() < 32) return false;
    const uint64_t want = alamo::fnv1a64(b.data(), b.size() - 8);
    uint64_t got = 0;
    for (int i = 0; i < 8; ++i) got |= static_cast<uint64_t>(b[b.size() - 8 + static_cast<size_t>(i)]) << (8 * i);
    if (want != got) return false;
    b.resize(b.size() - 8);
    In in{b};
    uint32_t magic = 0, fmt = 0, words = 0;
    std::string k, entry, iface;
    if (!in.u32(magic) || !in.u32(fmt) || magic != kMagic || fmt != kFormat) return false;
    if (!in.str(k) || k != key.describe()) return false;
    if (!in.str(entry) || !in.str(iface) || !in.u32(words)) return false;
    if (static_cast<size_t>(words) * 4 != b.size() - in.pos || words < 5) return false;
    out.spirv.resize(words);
    std::memcpy(out.spirv.data(), b.data() + in.pos, static_cast<size_t>(words) * 4);
    if (out.spirv[0] != 0x07230203u) return false;
    if (!ShaderInterface::deserialize(iface, out.iface)) return false;
    out.entry = entry;
    return true;
}

bool ShaderCache::store(const CacheKey& key, const CacheEntry& entry) const {
    std::error_code ec;
    const std::filesystem::path dir = alamo::long_path(dir_);  // "\\?\" form on Windows: no MAX_PATH limit
    std::filesystem::create_directories(dir, ec);
    std::vector<uint8_t> b;
    put32(b, kMagic);
    put32(b, kFormat);
    put_str(b, key.describe());
    put_str(b, entry.entry);
    put_str(b, entry.iface.serialize());
    put32(b, static_cast<uint32_t>(entry.spirv.size()));
    const size_t at = b.size();
    b.resize(at + entry.spirv.size() * 4);
    if (!entry.spirv.empty()) std::memcpy(b.data() + at, entry.spirv.data(), entry.spirv.size() * 4);
    const uint64_t sum = alamo::fnv1a64(b.data(), b.size());
    for (int i = 0; i < 8; ++i) b.push_back(static_cast<uint8_t>(sum >> (8 * i)));

    static std::atomic<uint64_t> counter{0};
    const uint64_t tid = std::hash<std::thread::id>{}(std::this_thread::get_id());
    char suffix[96];
    std::snprintf(suffix, sizeof(suffix), ".tmp.%llu.%llx.%llu", static_cast<unsigned long long>(process_id()),
                  static_cast<unsigned long long>(tid), static_cast<unsigned long long>(counter.fetch_add(1)));
    const std::filesystem::path final_path = dir / key.file_name();
    const std::filesystem::path tmp = dir / (key.file_name() + suffix);
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
        f.flush();
        if (!f) {
            f.close();
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }
    if (!replace_file(tmp, final_path)) {
        std::filesystem::remove(tmp, ec);
        // Lost a race against another writer of the same key: its entry is equivalent.
        CacheEntry other;
        return load(key, other);
    }
    return true;
}

}  // namespace alamo::shaderxlat
