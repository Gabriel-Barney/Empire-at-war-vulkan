// Non-chunked formats: MTD texture atlas definitions and DAT string tables.
#pragma once

#include "alamo/formats/common.hpp"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace alamo {

// ---- MTD ----------------------------------------------------------------------------------------
//   u32 count
//   count x { char name[64]; u32 x, y, width, height; u8 alpha; }   (81 bytes, packed)
namespace mtd {
struct Atlas {
    std::vector<std::string> names;
    std::vector<std::uint32_t> x, y, width, height;
    std::vector<std::uint8_t> alpha;   // 1 = entry uses alpha (observed 0/1)
    std::size_t size() const { return names.size(); }
    std::optional<std::size_t> find(std::string_view name) const;  // case-insensitive
};
Atlas parse(Bytes data);
Atlas load(const std::filesystem::path& path);
}  // namespace mtd

// ---- DAT (string table) ---------------------------------------------------------------------------
//   u32 count
//   count x { u32 crc; u32 value_chars; u32 key_chars; }
//   values: concatenated UTF-16LE strings (no terminators), in record order
//   keys:   concatenated ASCII strings  (no terminators), in record order
// crc = CRC-32 of the key bytes *as stored* (case-sensitive; most keys are upper-case).
// Localised text files are sorted by crc; credits files are not and contain duplicate keys.
namespace dat {
struct StringTable {
    std::vector<std::uint32_t> crcs;
    std::vector<std::string> keys;
    std::vector<std::u16string> values;
    bool sorted_by_crc = false;
    std::size_t size() const { return keys.size(); }
    // Lookup by key (exact CRC, falling back to a case-insensitive linear scan).
    const std::u16string* find(std::string_view key) const;
    std::size_t crc_mismatches() const;  // records whose crc != crc32(key)
};
StringTable parse(Bytes data);
StringTable load(const std::filesystem::path& path);
std::string to_utf8(const std::u16string& s);
}  // namespace dat

}  // namespace alamo
