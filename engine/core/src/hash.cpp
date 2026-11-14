#include "alamo/core/hash.hpp"

#include "alamo/core/sim_crc.hpp"  // crc32_raw: the one CRC-32 table

namespace alamo {
namespace {
inline char up(char c) {
    if (c >= 'a' && c <= 'z') return static_cast<char>(c - 'a' + 'A');
    if (c == '/') return '\\';
    return c;
}
}  // namespace

uint32_t crc32(const void* data, size_t len, uint32_t seed) {
    return ~crc32_raw(data, len, ~seed);
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
    return fnv1a64(std::span<const uint8_t>(static_cast<const uint8_t*>(data), len), seed);
}

}  // namespace alamo
