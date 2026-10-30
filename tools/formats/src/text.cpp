#include "alamo/formats/text.hpp"

#include <algorithm>
#include <cctype>

namespace alamo {

namespace {
bool iequals(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}
}  // namespace

namespace mtd {

Atlas parse(Bytes data) {
    ByteReader r(data);
    const std::uint32_t n = r.u32();
    constexpr std::size_t kEntry = 64 + 16 + 1;
    if (r.remaining() != std::size_t(n) * kEntry)
        throw FormatError("MTD size mismatch: " + std::to_string(n) + " entries need " +
                              std::to_string(std::size_t(n) * kEntry) + " bytes, have " +
                              std::to_string(r.remaining()), 4);
    Atlas a;
    a.names.reserve(n);
    for (auto* v : {&a.x, &a.y, &a.width, &a.height}) v->reserve(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        a.names.push_back(r.fixed_string(64));
        a.x.push_back(r.u32());
        a.y.push_back(r.u32());
        a.width.push_back(r.u32());
        a.height.push_back(r.u32());
        a.alpha.push_back(r.u8());
    }
    return a;
}

Atlas load(const std::filesystem::path& p) {
    auto b = read_file(p);
    return parse(b);
}

std::optional<std::size_t> Atlas::find(std::string_view name) const {
    for (std::size_t i = 0; i < names.size(); ++i)
        if (iequals(names[i], name)) return i;
    return std::nullopt;
}

}  // namespace mtd

namespace dat {

StringTable parse(Bytes data) {
    ByteReader r(data);
    const std::uint32_t n = r.u32();
    if (std::size_t(n) * 12 > r.remaining()) throw FormatError("DAT record table overruns file", 0);
    std::vector<std::uint32_t> vlen(n), klen(n);
    StringTable t;
    t.crcs.resize(n);
    std::size_t total_v = 0, total_k = 0;
    for (std::uint32_t i = 0; i < n; ++i) {
        t.crcs[i] = r.u32();
        vlen[i] = r.u32();
        klen[i] = r.u32();
        total_v += vlen[i];
        total_k += klen[i];
    }
    if (total_v * 2 + total_k != r.remaining())
        throw FormatError("DAT string pool size mismatch (expected " + std::to_string(total_v * 2 + total_k) +
                              ", have " + std::to_string(r.remaining()) + ")", r.abs_pos());
    t.values.resize(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        auto& v = t.values[i];
        v.resize(vlen[i]);
        for (auto& ch : v) ch = static_cast<char16_t>(r.u16());
    }
    t.keys.resize(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        auto b = r.bytes(klen[i]);
        t.keys[i].assign(reinterpret_cast<const char*>(b.data()), b.size());
    }
    t.sorted_by_crc = std::is_sorted(t.crcs.begin(), t.crcs.end());
    return t;
}

StringTable load(const std::filesystem::path& p) {
    auto b = read_file(p);
    return parse(b);
}

const std::u16string* StringTable::find(std::string_view key) const {
    const std::uint32_t c = crc32(key);
    if (sorted_by_crc) {
        auto it = std::lower_bound(crcs.begin(), crcs.end(), c);
        for (; it != crcs.end() && *it == c; ++it) {
            auto i = std::size_t(it - crcs.begin());
            if (keys[i] == key) return &values[i];
        }
    } else {
        for (std::size_t i = 0; i < crcs.size(); ++i)
            if (crcs[i] == c && keys[i] == key) return &values[i];
    }
    for (std::size_t i = 0; i < keys.size(); ++i)
        if (iequals(keys[i], key)) return &values[i];
    return nullptr;
}

std::size_t StringTable::crc_mismatches() const {
    std::size_t n = 0;
    for (std::size_t i = 0; i < keys.size(); ++i)
        if (crc32(keys[i]) != crcs[i]) ++n;
    return n;
}

std::string to_utf8(const std::u16string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        std::uint32_t cp = s[i];
        if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (s[i + 1] - 0xDC00);
            ++i;
        }
        if (cp < 0x80) out += char(cp);
        else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) {
            out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F));
        } else {
            out += char(0xF0 | (cp >> 18)); out += char(0x80 | ((cp >> 12) & 0x3F));
            out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

}  // namespace dat
}  // namespace alamo
