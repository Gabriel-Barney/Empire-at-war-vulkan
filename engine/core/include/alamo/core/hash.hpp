// Engine string hashing. Matches the original's name keys: CRC-32/IEEE (reflected 0xEDB88320,
// init/xorout 0xFFFFFFFF) over the upper-cased name with '/' normalised to '\\'.
// Same algorithm as tools/meg (alamo::meg::name_crc), duplicated here so core has no deps.
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "alamo/core/ascii.hpp"  // exact (non-folding) ASCII helpers: ascii_iequals, ascii_upper, ascii_trim

namespace alamo {

// CRC-32/IEEE with init / xorout 0xFFFFFFFF: crc32(p, n, seed) == ~crc32_raw(p, n, ~seed) (sim_crc.hpp; one table).
uint32_t crc32(const void* data, size_t len, uint32_t seed = 0);
// Upper-case ASCII, '/' -> '\\'. Non-ASCII bytes pass through unchanged.
std::string normalize_name(std::string_view name);
uint32_t name_crc(std::string_view name);
// ASCII case-insensitive equality ('/' and '\\' compare equal).
bool iequals(std::string_view a, std::string_view b);

// 64-bit FNV-1a, for content fingerprints (dirty checks, cache keys), not for engine keys.
constexpr uint64_t kFnv1a64Offset = 0xcbf29ce484222325ull;
constexpr uint64_t kFnv1a64Prime = 0x100000001b3ull;
uint64_t fnv1a64(const void* data, size_t len, uint64_t seed = kFnv1a64Offset);
// Same algorithm and values, usable in constant expressions (CU2 t09).
constexpr uint64_t fnv1a64(std::string_view s, uint64_t seed = kFnv1a64Offset) noexcept {
    uint64_t h = seed;
    for (const char c : s) {
        h ^= static_cast<unsigned char>(c);
        h *= kFnv1a64Prime;
    }
    return h;
}
constexpr uint64_t fnv1a64(std::span<const uint8_t> bytes, uint64_t seed = kFnv1a64Offset) noexcept {
    uint64_t h = seed;
    for (const uint8_t b : bytes) {
        h ^= b;
        h *= kFnv1a64Prime;
    }
    return h;
}

}  // namespace alamo
