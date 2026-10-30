#include "alamo/terrain/chunk.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace alamo::terrain {

bool read_chunks(std::span<const uint8_t> b, std::vector<ChunkView>& out) {
    out.clear();
    size_t pos = 0;
    while (pos < b.size()) {
        if (b.size() - pos < 8) return false;
        uint32_t id = 0, sz = 0;
        std::memcpy(&id, b.data() + pos, 4);
        std::memcpy(&sz, b.data() + pos + 4, 4);
        const uint32_t size = sz & 0x7FFFFFFFu;
        if (b.size() - pos - 8 < size) return false;
        out.push_back({id, (sz & 0x80000000u) != 0, b.subspan(pos + 8, size)});
        pos += 8 + static_cast<size_t>(size);
    }
    return true;
}

bool read_minis(std::span<const uint8_t> leaf, std::vector<MiniView>& out) {
    out.clear();
    size_t pos = 0;
    while (pos < leaf.size()) {
        if (leaf.size() - pos < 2) return false;
        const uint8_t id = leaf[pos], size = leaf[pos + 1];
        if (leaf.size() - pos - 2 < size) return false;
        out.push_back({id, leaf.subspan(pos + 2, size)});
        pos += 2 + static_cast<size_t>(size);
    }
    return true;
}

bool mini_f32(const MiniView& m, float& v) {
    if (m.payload.size() < 4) return false;
    float t = 0;
    std::memcpy(&t, m.payload.data(), 4);
    if (!std::isfinite(t)) return false;
    v = t;
    return true;
}

bool mini_f32x3(const MiniView& m, float (&v)[3]) {
    if (m.payload.size() < 12) return false;
    float t[3];
    std::memcpy(t, m.payload.data(), 12);
    if (!std::isfinite(t[0]) || !std::isfinite(t[1]) || !std::isfinite(t[2])) return false;
    std::copy(t, t + 3, v);
    return true;
}

bool mini_u32(const MiniView& m, uint32_t& v) {
    if (m.payload.size() >= 4) {
        std::memcpy(&v, m.payload.data(), 4);
    } else if (m.payload.size() >= 2) {
        uint16_t s = 0;
        std::memcpy(&s, m.payload.data(), 2);
        v = s;
    } else if (m.payload.size() == 1) {
        v = m.payload[0];
    } else {
        return false;
    }
    return true;
}

uint32_t mini_u32_or(const MiniView& m, uint32_t fallback) {
    uint32_t v = fallback;
    return mini_u32(m, v) ? v : fallback;
}

std::string mini_string(const MiniView& m) {
    const auto* p = reinterpret_cast<const char*>(m.payload.data());
    size_t n = 0;
    while (n < m.payload.size() && p[n] != '\0') ++n;
    return std::string(p, n);
}

namespace {
void put_utf8(std::string& s, uint32_t cp) {
    if (cp < 0x80) {
        s += static_cast<char>(cp);
    } else if (cp < 0x800) {
        s += static_cast<char>(0xC0 | (cp >> 6));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        s += static_cast<char>(0xE0 | (cp >> 12));
        s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        s += static_cast<char>(0xF0 | (cp >> 18));
        s += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    }
}
}  // namespace

std::string mini_utf16(const MiniView& m) {
    std::string s;
    const size_t units = m.payload.size() / 2;
    for (size_t i = 0; i < units; ++i) {
        const uint32_t u = static_cast<uint32_t>(m.payload[2 * i]) | (static_cast<uint32_t>(m.payload[2 * i + 1]) << 8);
        if (u == 0) break;
        if (u >= 0xD800 && u < 0xDC00 && i + 1 < units) {
            const uint32_t lo = static_cast<uint32_t>(m.payload[2 * i + 2]) | (static_cast<uint32_t>(m.payload[2 * i + 3]) << 8);
            if (lo >= 0xDC00 && lo < 0xE000) {
                put_utf8(s, 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00));
                ++i;
                continue;
            }
        }
        if (u >= 0xD800 && u < 0xE000) put_utf8(s, 0xFFFD);
        else put_utf8(s, u);
    }
    return s;
}

std::vector<RawMini> copy_minis(std::span<const MiniView> minis) {
    std::vector<RawMini> r;
    r.reserve(minis.size());
    for (const MiniView& m : minis) r.push_back({m.id, std::vector<uint8_t>(m.payload.begin(), m.payload.end())});
    return r;
}

const RawMini* find_mini(const std::vector<RawMini>& minis, uint8_t id) {
    for (const RawMini& m : minis)
        if (m.id == id) return &m;
    return nullptr;
}

bool raw_f32(const std::vector<RawMini>& minis, uint8_t id, float& v) {
    const RawMini* m = find_mini(minis, id);
    return m && mini_f32(MiniView{m->id, m->data}, v);
}

bool raw_f32x3(const std::vector<RawMini>& minis, uint8_t id, float (&v)[3]) {
    const RawMini* m = find_mini(minis, id);
    return m && mini_f32x3(MiniView{m->id, m->data}, v);
}

// ---- writer ----
namespace {
void put_u32(std::vector<uint8_t>& o, uint32_t v) {
    uint8_t b[4];
    std::memcpy(b, &v, 4);
    o.insert(o.end(), b, b + 4);
}
}  // namespace

void ChunkWriter::begin(uint32_t id) {
    put_u32(out_, id);
    open_.push_back(out_.size());
    put_u32(out_, 0);
}

void ChunkWriter::end() {
    if (open_.empty()) return;
    const size_t at = open_.back();
    open_.pop_back();
    const uint32_t size = static_cast<uint32_t>(out_.size() - at - 4) | 0x80000000u;
    std::memcpy(out_.data() + at, &size, 4);
}

void ChunkWriter::leaf(uint32_t id, std::span<const uint8_t> payload) {
    put_u32(out_, id);
    put_u32(out_, static_cast<uint32_t>(payload.size()) & 0x7FFFFFFFu);
    out_.insert(out_.end(), payload.begin(), payload.end());
}

void ChunkWriter::mini_leaf(uint32_t id, std::span<const RawMini> minis) {
    std::vector<uint8_t> p;
    for (const RawMini& m : minis) {
        const size_t n = std::min<size_t>(m.data.size(), 255);
        p.push_back(m.id);
        p.push_back(static_cast<uint8_t>(n));
        p.insert(p.end(), m.data.begin(), m.data.begin() + static_cast<std::ptrdiff_t>(n));
    }
    leaf(id, p);
}

std::vector<uint8_t> ChunkWriter::take() {
    while (!open_.empty()) end();
    return std::move(out_);
}

RawMini mk_f32(uint8_t id, float v) {
    RawMini m{id, std::vector<uint8_t>(4)};
    std::memcpy(m.data.data(), &v, 4);
    return m;
}
RawMini mk_f32x3(uint8_t id, const float (&v)[3]) {
    RawMini m{id, std::vector<uint8_t>(12)};
    std::memcpy(m.data.data(), v, 12);
    return m;
}
RawMini mk_u32(uint8_t id, uint32_t v) {
    RawMini m{id, std::vector<uint8_t>(4)};
    std::memcpy(m.data.data(), &v, 4);
    return m;
}
RawMini mk_u8(uint8_t id, uint8_t v) { return RawMini{id, std::vector<uint8_t>{v}}; }
RawMini mk_string(uint8_t id, const std::string& s) {
    const size_t n = std::min<size_t>(s.size(), 254);
    RawMini m{id, std::vector<uint8_t>(s.begin(), s.begin() + static_cast<std::ptrdiff_t>(n))};
    m.data.push_back(0);
    return m;
}
RawMini mk_utf16(uint8_t id, const std::string& s) {
    RawMini m{id, {}};
    // Decode UTF-8 (BMP only; anything else becomes '?').
    size_t i = 0;
    while (i < s.size() && m.data.size() < 252) {
        uint32_t cp = static_cast<uint8_t>(s[i]);
        size_t len = 1;
        if (cp >= 0xE0 && cp < 0xF0 && i + 2 < s.size()) {
            cp = ((cp & 0x0F) << 12) | ((static_cast<uint8_t>(s[i + 1]) & 0x3Fu) << 6) | (static_cast<uint8_t>(s[i + 2]) & 0x3Fu);
            len = 3;
        } else if (cp >= 0xC0 && cp < 0xE0 && i + 1 < s.size()) {
            cp = ((cp & 0x1F) << 6) | (static_cast<uint8_t>(s[i + 1]) & 0x3Fu);
            len = 2;
        } else if (cp >= 0x80) {
            cp = '?';
        }
        m.data.push_back(static_cast<uint8_t>(cp & 0xFF));
        m.data.push_back(static_cast<uint8_t>(cp >> 8));
        i += len;
    }
    m.data.push_back(0);
    m.data.push_back(0);
    return m;
}

}  // namespace alamo::terrain
