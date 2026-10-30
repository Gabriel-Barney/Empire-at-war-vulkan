#include "alamo/core/hash.hpp"

#include <array>

namespace alamo {
namespace {
constexpr std::array<uint32_t, 256> make_table() {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        t[i] = c;
    }
    return t;
}
constexpr auto kTable = make_table();

inline char up(char c) {
    if (c >= 'a' && c <= 'z') return static_cast<char>(c - 'a' + 'A');
    if (c == '/') return '\\';
    return c;
}
}  // namespace

uint32_t crc32(const void* data, size_t len, uint32_t seed) {
    const auto* p = static_cast<const uint8_t*>(data);
    uint32_t c = ~seed;
    for (size_t i = 0; i < len; ++i) c = kTable[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return ~c;
}

std::string normalize_name(std::string_view name) {
    std::string s(name);
    for (auto& c : s) c = up(c);
    return s;
}

uint32_t name_crc(std::string_view name) {
    const std::string n = normalize_name(name);
    return crc32(n.data(), n.size());
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (up(a[i]) != up(b[i])) return false;
    return true;
}

uint64_t fnv1a64(const void* data, size_t len, uint64_t seed) {
    const auto* p = static_cast<const uint8_t*>(data);
    uint64_t h = seed;
    for (size_t i = 0; i < len; ++i) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

}  // namespace alamo
