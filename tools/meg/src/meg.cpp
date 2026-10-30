#include "alamo/meg/meg.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <mutex>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace alamo::meg {

// ---------------------------------------------------------------- CRC / names

namespace {
constexpr std::array<uint32_t, 256> make_crc_table() {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        t[i] = c;
    }
    return t;
}
constexpr auto kCrcTable = make_crc_table();

inline char up(char c) { return (c >= 'a' && c <= 'z') ? char(c - 32) : (c == '/' ? '\\' : c); }

inline uint32_t rd32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
inline uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }

constexpr uint32_t kIdPlain = 0xFFFFFFFFu;
constexpr uint32_t kIdEncrypted = 0x8FFFFFFFu;
constexpr uint32_t kMagic = 0x3F7D70A4u;  // float 0.99
}  // namespace

uint32_t crc32(const void* data, size_t len, uint32_t seed) {
    uint32_t c = ~seed;
    auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; ++i) c = kCrcTable[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return ~c;
}

std::string normalize_name(std::string_view name) {
    std::string s(name);
    for (auto& c : s) c = up(c);
    return s;
}

uint32_t name_crc(std::string_view name) {
    std::string n = normalize_name(name);
    return crc32(n.data(), n.size());
}

bool glob_match(std::string_view pat, std::string_view txt) {
    size_t p = 0, t = 0, star = std::string_view::npos, mark = 0;
    while (t < txt.size()) {
        if (p < pat.size() && (pat[p] == '?' || up(pat[p]) == up(txt[t]))) {
            ++p; ++t;
        } else if (p < pat.size() && pat[p] == '*') {
            star = p++; mark = t;
        } else if (star != std::string_view::npos) {
            p = star + 1; t = ++mark;
        } else {
            return false;
        }
    }
    while (p < pat.size() && pat[p] == '*') ++p;
    return p == pat.size();
}

// ---------------------------------------------------------------- Sources

namespace {
class MemorySource final : public Source {
public:
    explicit MemorySource(std::vector<uint8_t> b) : buf_(std::move(b)) {}
    uint64_t size() const override { return buf_.size(); }
    bool read(uint64_t off, void* dst, size_t len) const override {
        if (off > buf_.size() || len > buf_.size() - off) return false;
        std::memcpy(dst, buf_.data() + off, len);
        return true;
    }
    const uint8_t* data() const override { return buf_.data(); }
private:
    std::vector<uint8_t> buf_;
};

class StreamSource final : public Source {
public:
    StreamSource(FILE* f, uint64_t sz) : f_(f), size_(sz) {}
    ~StreamSource() override { if (f_) std::fclose(f_); }
    uint64_t size() const override { return size_; }
    bool read(uint64_t off, void* dst, size_t len) const override {
        if (off > size_ || len > size_ - off) return false;
        std::lock_guard<std::mutex> lk(mu_);
        if (_fseeki64(f_, int64_t(off), SEEK_SET) != 0) return false;
        return std::fread(dst, 1, len, f_) == len;
    }
private:
    FILE* f_;
    uint64_t size_;
    mutable std::mutex mu_;
};

class MappedSource final : public Source {
public:
    ~MappedSource() override {
        if (base_) UnmapViewOfFile(base_);
        if (map_) CloseHandle(map_);
        if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
    }
    static std::unique_ptr<MappedSource> open(const std::string& path) {
        auto m = std::unique_ptr<MappedSource>(new MappedSource());
        int wlen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
        std::wstring w(size_t(wlen > 0 ? wlen : 1), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, w.data(), wlen);
        m->file_ = CreateFileW(w.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (m->file_ == INVALID_HANDLE_VALUE) return nullptr;
        LARGE_INTEGER li;
        if (!GetFileSizeEx(m->file_, &li)) return nullptr;
        m->size_ = uint64_t(li.QuadPart);
        if (m->size_ == 0) return m;
        m->map_ = CreateFileMappingW(m->file_, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!m->map_) return nullptr;
        m->base_ = static_cast<const uint8_t*>(MapViewOfFile(m->map_, FILE_MAP_READ, 0, 0, 0));
        if (!m->base_) return nullptr;
        return m;
    }
    uint64_t size() const override { return size_; }
    bool read(uint64_t off, void* dst, size_t len) const override {
        if (off > size_ || len > size_ - off) return false;
        if (len) std::memcpy(dst, base_ + off, len);
        return true;
    }
    const uint8_t* data() const override { return base_; }
private:
    MappedSource() = default;
    const uint8_t* base_ = nullptr;
    uint64_t size_ = 0;
    HANDLE file_ = INVALID_HANDLE_VALUE;
    HANDLE map_ = nullptr;
};
}  // namespace

std::unique_ptr<Source> Source::open(const std::string& path, Mode mode, std::string* err) {
    if (mode != Mode::Stream) {
        if (auto m = MappedSource::open(path)) return m;
        if (mode == Mode::Mapped) {
            if (err) *err = "cannot map " + path;
            return nullptr;
        }
    }
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") != 0) f = nullptr;
    if (!f) {
        if (err) *err = "cannot open " + path;
        return nullptr;
    }
    _fseeki64(f, 0, SEEK_END);
    uint64_t sz = uint64_t(_ftelli64(f));
    return std::make_unique<StreamSource>(f, sz);
}

std::unique_ptr<Source> Source::from_memory(std::vector<uint8_t> bytes) {
    return std::make_unique<MemorySource>(std::move(bytes));
}

// ---------------------------------------------------------------- Archive

std::unique_ptr<Archive> Archive::open(const std::string& path, Source::Mode mode, std::string* err) {
    auto src = Source::open(path, mode, err);
    if (!src) return nullptr;
    auto a = open(std::move(src), err);
    if (a) a->path_ = path;
    return a;
}

std::unique_ptr<Archive> Archive::open(std::unique_ptr<Source> src, std::string* err) {
    auto a = std::unique_ptr<Archive>(new Archive());
    a->src_ = std::move(src);
    if (!a->parse(err)) return nullptr;
    return a;
}

bool Archive::parse(std::string* err) {
    auto fail = [&](const char* m) { if (err) *err = m; return false; };
    const uint64_t fsize = src_->size();
    uint8_t h[24] = {};
    if (fsize < 8 || !src_->read(0, h, fsize >= 24 ? 24 : 8)) return fail("file too small");

    uint32_t a = rd32(h), b = rd32(h + 4);
    uint64_t names_off = 0;
    uint32_t n_names = 0, n_files = 0, rec_size = 20;
    bool v3_records = false;
    if ((a == kIdPlain || a == kIdEncrypted) && b == kMagic) {
        if (fsize < 24) return fail("truncated v2/v3 header");
        uint32_t data_start = rd32(h + 8);
        n_names = rd32(h + 12);
        n_files = rd32(h + 16);
        uint32_t name_tab_size = rd32(h + 20);
        if (a == kIdEncrypted) {
            version_ = Version::V3;
            encrypted_ = true;
            return true;  // tables are AES-128-CBC encrypted; key not available
        }
        if (uint64_t(24) + name_tab_size + uint64_t(n_files) * 20 == data_start) {
            version_ = Version::V3; names_off = 24; rec_size = 20; v3_records = true;
        } else {
            version_ = Version::V2; names_off = 20;
        }
    } else {
        version_ = Version::V1;
        n_names = a; n_files = b; names_off = 8;
    }
    // Sanity: each name costs >= 2 bytes, each record rec_size bytes.
    if (uint64_t(n_names) * 2 + uint64_t(n_files) * rec_size + names_off > fsize)
        return fail("header counts exceed file size");

    // Read name table.
    std::vector<std::string> names;
    names.reserve(n_names);
    uint64_t off = names_off;
    for (uint32_t i = 0; i < n_names; ++i) {
        uint8_t lb[2];
        if (!src_->read(off, lb, 2)) return fail("truncated name table");
        uint16_t ln = rd16(lb);
        off += 2;
        std::string s(ln, '\0');
        if (ln && !src_->read(off, s.data(), ln)) return fail("truncated name");
        off += ln;
        names.push_back(std::move(s));
    }

    // Read record table.
    std::vector<uint8_t> recs(size_t(n_files) * rec_size);
    if (!recs.empty() && !src_->read(off, recs.data(), recs.size())) return fail("truncated record table");
    entries_.resize(n_files);
    for (uint32_t i = 0; i < n_files; ++i) {
        const uint8_t* r = recs.data() + size_t(i) * rec_size;
        Entry& e = entries_[i];
        if (v3_records) {
            e.flags = rd16(r);
            e.crc = rd32(r + 2); e.index = rd32(r + 6); e.size = rd32(r + 10);
            e.offset = rd32(r + 14); e.name_index = rd16(r + 18);
        } else {
            e.crc = rd32(r); e.index = rd32(r + 4); e.size = rd32(r + 8);
            e.offset = rd32(r + 12); e.name_index = rd32(r + 16);
        }
        if (e.name_index < names.size()) e.name = names[e.name_index];
        else e.name = "<bad name index " + std::to_string(e.name_index) + ">";
    }
    return true;
}

const Entry* Archive::find(std::string_view name) const {
    const std::string n = normalize_name(name);
    const uint32_t c = crc32(n.data(), n.size());
    auto it = std::lower_bound(entries_.begin(), entries_.end(), c,
                               [](const Entry& e, uint32_t v) { return e.crc < v; });
    for (; it != entries_.end() && it->crc == c; ++it)
        if (normalize_name(it->name) == n) return &*it;
    return nullptr;
}

bool Archive::read(const Entry& e, std::vector<uint8_t>& out) const {
    if (e.encrypted()) return false;
    out.resize(e.size);
    return src_->read(e.offset, out.data(), e.size);
}

std::span<const uint8_t> Archive::view(const Entry& e) const {
    const uint8_t* d = src_->data();
    if (!d || e.encrypted() || uint64_t(e.offset) + e.size > src_->size()) return {};
    return {d + e.offset, e.size};
}

std::vector<std::string> Archive::verify() const {
    std::vector<std::string> p;
    uint32_t prev = 0;
    bool first = true;
    for (const auto& e : entries_) {
        if (name_crc(e.name) != e.crc) p.push_back("crc mismatch: " + e.name);
        if (!first && e.crc < prev) p.push_back("unsorted at " + e.name);
        prev = e.crc; first = false;
        if (uint64_t(e.offset) + e.size > src_->size()) p.push_back("oob: " + e.name);
    }
    return p;
}

// ---------------------------------------------------------------- FileSystem

bool FileSystem::mount(const std::string& path, std::string* err) {
    auto a = Archive::open(path, Source::Mode::Auto, err);
    if (!a) return false;
    archives_.push_back(std::move(a));
    return true;
}

bool FileSystem::find(std::string_view name, Hit& out) const {
    for (auto it = archives_.rbegin(); it != archives_.rend(); ++it) {
        if (const Entry* e = (*it)->find(name)) {
            out = {it->get(), e};
            return true;
        }
    }
    return false;
}

bool FileSystem::read(std::string_view name, std::vector<uint8_t>& out) const {
    Hit h{};
    return find(name, h) && h.archive->read(*h.entry, out);
}

}  // namespace alamo::meg
