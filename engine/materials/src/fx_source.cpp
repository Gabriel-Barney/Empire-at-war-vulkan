// Effect sources and the compiled-source cache (fx_source.hpp, roadmap modc t23).
// Cache file layout, little-endian:
//   "AFXC" u32 format | u32 len + key | u8 ok | u32 len + compiler | u32 len + log | u32 len + fxo |
//   u64 FNV-1a of everything before it
#include "alamo/materials/fx_source.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <set>

#include "alamo/core/atomic_file.hpp"
#include "alamo/core/file_provider.hpp"
#include "alamo/core/host_fs.hpp"
#include "alamo/core/hash.hpp"

#include <windows.h>

namespace alamo::materials {

namespace {

constexpr uint32_t kMagic = 0x43584641u;  // "AFXC"
constexpr uint32_t kFormat = 1;
constexpr size_t kMaxFile = 64u << 20;
constexpr size_t kMaxIncludes = 256;  // a cycle or a runaway include chain stops here

// "a\\b\\..\\c" -> "a\\c"; '/' -> '\\'. Case is kept (the VFS folds it).
std::string join_game_path(std::string_view dir, std::string_view name) {
    std::string full = dir.empty() ? std::string(name) : std::string(dir) + "\\" + std::string(name);
    for (char& c : full)
        if (c == '/') c = '\\';
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= full.size()) {
        const size_t end = std::min(full.find('\\', start), full.size());
        const std::string seg = full.substr(start, end - start);
        if (seg == "..") {
            if (!parts.empty()) parts.pop_back();
        } else if (!seg.empty() && seg != ".") {
            parts.push_back(seg);
        }
        start = end + 1;
    }
    std::string out;
    for (const std::string& p : parts) out += (out.empty() ? "" : "\\") + p;
    return out;
}

std::string dir_of(std::string_view path) {
    const size_t s = path.find_last_of("\\/");
    return s == std::string_view::npos ? std::string() : std::string(path.substr(0, s));
}

// #include names in source order (comments skipped, conditionals not evaluated).
std::vector<std::string> include_names(const std::vector<uint8_t>& bytes) {
    std::vector<std::string> out;
    const std::string_view s(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    bool block = false;
    size_t i = 0;
    while (i < s.size()) {
        const size_t eol = std::min(s.find('\n', i), s.size());
        std::string line;  // the line with comments removed
        for (size_t j = i; j < eol; ++j) {
            if (block) {
                if (s[j] == '*' && j + 1 < eol && s[j + 1] == '/') {
                    block = false;
                    ++j;
                }
                continue;
            }
            if (s[j] == '/' && j + 1 < eol && s[j + 1] == '/') break;
            if (s[j] == '/' && j + 1 < eol && s[j + 1] == '*') {
                block = true;
                ++j;
                continue;
            }
            line.push_back(s[j]);
        }
        i = eol + 1;
        size_t p = line.find_first_not_of(" \t\r");
        if (p == std::string::npos || line[p] != '#') continue;
        p = line.find_first_not_of(" \t", p + 1);
        if (p == std::string::npos || line.compare(p, 7, "include") != 0) continue;
        p = line.find_first_not_of(" \t", p + 7);
        if (p == std::string::npos || (line[p] != '"' && line[p] != '<')) continue;
        const char close = line[p] == '"' ? '"' : '>';
        const size_t q = line.find(close, p + 1);
        if (q == std::string::npos || q == p + 1) continue;
        out.push_back(line.substr(p + 1, q - p - 1));
    }
    return out;
}

void put32(std::vector<uint8_t>& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
void put_bytes(std::vector<uint8_t>& b, const void* p, size_t n) {
    put32(b, static_cast<uint32_t>(n));
    const auto* c = static_cast<const uint8_t*>(p);
    b.insert(b.end(), c, c + n);
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
    template <class C>
    bool bytes(C& out) {
        uint32_t n = 0;
        if (!u32(n) || n > b.size() - pos) return false;
        out.assign(b.begin() + static_cast<std::ptrdiff_t>(pos), b.begin() + static_cast<std::ptrdiff_t>(pos + n));
        pos += n;
        return true;
    }
};

std::string getenv_str(const char* n) {
    char buf[4096];
    const DWORD r = GetEnvironmentVariableA(n, buf, sizeof(buf));
    return (r > 0 && r < sizeof(buf)) ? std::string(buf, r) : std::string();
}

}  // namespace

std::string FxSource::key() const {
    // Length-prefixed fields, so ("ab","c") and ("a","bc") differ.
    uint64_t a = alamo::kFnv1a64Offset, b = 0x84222325cbf29ce4ull;  // b: a second, independent seed
    auto field = [&](const void* p, size_t n) {
        const uint64_t len = n;
        a = alamo::fnv1a64(&len, sizeof len, a);
        a = alamo::fnv1a64(p, n, a);
        b = alamo::fnv1a64(&len, sizeof len, b);
        b = alamo::fnv1a64(p, n, b);
        b ^= b >> 29;
    };
    auto text = [&](std::string_view s) { field(s.data(), s.size()); };
    char head[64];
    std::snprintf(head, sizeof head, "alamo-fx %u %s", kFxKeyVersion, kFxProfile);
    text(head);
    for (const FxSourceFile& f : files) {
        text(alamo::normalize_path(f.path));
        field(f.bytes.data(), f.bytes.size());
    }
    text("missing");
    for (const std::string& m : missing) text(m);
    char buf[40];
    std::snprintf(buf, sizeof buf, "%016llx%016llx", static_cast<unsigned long long>(a), static_cast<unsigned long long>(b));
    return buf;
}

bool collect_fx_source(const FileProvider& files, std::string_view path, FxSource& out, std::string* err) {
    out = FxSource{};
    out.path = std::string(path);
    FxSourceFile root;
    root.path = std::string(path);
    if (!files.read(path, root.bytes)) {
        if (err) *err = "cannot read " + std::string(path);
        return false;
    }
    out.files.push_back(std::move(root));
    std::set<std::string> seen{alamo::normalize_path(path)};
    std::set<std::string> missing_seen;
    for (size_t i = 0; i < out.files.size() && out.files.size() < kMaxIncludes; ++i) {
        const std::vector<std::string> names = include_names(out.files[i].bytes);
        const std::string dir = dir_of(out.files[i].path);
        for (const std::string& name : names) {
            std::string found;
            for (const std::string& cand : {join_game_path(dir, name), join_game_path("Data\\Art\\Shaders", name)}) {
                if (files.exists(cand)) {
                    found = cand;
                    break;
                }
            }
            if (found.empty()) {
                if (missing_seen.insert(name).second) out.missing.push_back(name);
                continue;
            }
            if (!seen.insert(alamo::normalize_path(found)).second) continue;
            FxSourceFile f;
            f.path = found;
            if (!files.read(found, f.bytes)) {
                if (missing_seen.insert(name).second) out.missing.push_back(name);
                continue;
            }
            out.files.push_back(std::move(f));
            if (out.files.size() >= kMaxIncludes) break;
        }
    }
    return true;
}

FxCache::FxCache() : dir_(default_directory()) {}
FxCache::FxCache(std::filesystem::path directory) : dir_(std::move(directory)) {}

std::filesystem::path FxCache::default_directory() {
    const std::string over = getenv_str("ALAMO_FX_CACHE");
    if (!over.empty()) return alamo::path_from_utf8(over);
    const std::string la = getenv_str("LOCALAPPDATA");
    if (!la.empty()) return std::filesystem::path(la) / "alamo" / "fx";
    std::error_code ec;
    return std::filesystem::temp_directory_path(ec) / "alamo" / "fx";
}

bool FxCache::load(const std::string& key, FxCacheEntry& out) const {
    out = FxCacheEntry{};
    std::vector<uint8_t> b;
    // read_host_file opens with FILE_SHARE_DELETE (never blocks a concurrent replace) and applies long_path.
    // The old local reader also rejected files under 16 bytes; the 24-byte check below covers that.
    if (key.empty() || !alamo::read_host_file(dir_ / (key + ".afx"), b) || b.size() < 24 || b.size() > kMaxFile)
        return false;
    const uint64_t want = alamo::fnv1a64(b.data(), b.size() - 8);
    uint64_t got = 0;
    for (int i = 0; i < 8; ++i) got |= static_cast<uint64_t>(b[b.size() - 8 + static_cast<size_t>(i)]) << (8 * i);
    if (want != got) return false;
    b.resize(b.size() - 8);
    In in{b};
    uint32_t magic = 0, fmt = 0;
    std::string k;
    if (!in.u32(magic) || !in.u32(fmt) || magic != kMagic || fmt != kFormat) return false;
    if (!in.bytes(k) || k != key || in.pos >= b.size()) return false;
    const bool ok = b[in.pos++] != 0;
    FxCacheEntry e;
    if (!in.bytes(e.compiler) || !in.bytes(e.log) || !in.bytes(e.fxo) || in.pos != b.size()) return false;
    e.ok = ok && !e.fxo.empty();
    out = std::move(e);
    return true;
}

bool FxCache::store(const std::string& key, const FxCacheEntry& entry) const {
    if (key.empty()) return false;
    std::error_code ec;
    const std::filesystem::path dir = alamo::long_path(dir_);
    std::filesystem::create_directories(dir, ec);
    std::vector<uint8_t> b;
    put32(b, kMagic);
    put32(b, kFormat);
    put_bytes(b, key.data(), key.size());
    b.push_back(static_cast<uint8_t>(entry.ok ? 1 : 0));
    put_bytes(b, entry.compiler.data(), entry.compiler.size());
    put_bytes(b, entry.log.data(), entry.log.size());
    put_bytes(b, entry.fxo.data(), entry.fxo.size());
    const uint64_t sum = alamo::fnv1a64(b.data(), b.size());
    for (int i = 0; i < 8; ++i) b.push_back(static_cast<uint8_t>(sum >> (8 * i)));

    const alamo::AtomicWriteResult r = alamo::write_file_atomic(dir / (key + ".afx"), b);  // 16 tries, 1 ms apart
    if (r == alamo::AtomicWriteResult::ReplaceFailed) {
        // Lost a race against another writer of the same key (CU2 t11: same policy as ShaderCache; this
        // returned false before). The key covers every source byte, so the other entry is equivalent.
        FxCacheEntry other;
        return load(key, other);
    }
    return r == alamo::AtomicWriteResult::Ok;
}

}  // namespace alamo::materials
