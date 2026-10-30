// Engine string hashing. Matches the original's name keys: CRC-32/IEEE (reflected 0xEDB88320,
// init/xorout 0xFFFFFFFF) over the upper-cased name with '/' normalised to '\\'.
// Same algorithm as tools/meg (alamo::meg::name_crc), duplicated here so core has no deps.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace alamo {

uint32_t crc32(const void* data, size_t len, uint32_t seed = 0);
// Upper-case ASCII, '/' -> '\\'. Non-ASCII bytes pass through unchanged.
std::string normalize_name(std::string_view name);
uint32_t name_crc(std::string_view name);
// ASCII case-insensitive equality ('/' and '\\' compare equal).
bool iequals(std::string_view a, std::string_view b);

// 64-bit FNV-1a, for content fingerprints (dirty checks), not for engine keys.
uint64_t fnv1a64(const void* data, size_t len, uint64_t seed = 0xcbf29ce484222325ull);

}  // namespace alamo
