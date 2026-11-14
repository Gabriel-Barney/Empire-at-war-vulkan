// CU2 t12: the one JSON string escaper of alamo_game_core (internal header; mod_check.cpp's report and benchmark.cpp's
// bench_report_json). Other module-local escapers (apps/sim bench_stats.cpp, the materials tools, tools/formats
// json_writer.hpp) are candidates for one core helper, which would be a core header change.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace alamo::game::json_text {

// Length of the valid UTF-8 sequence at s[i] (1-4), or 0 when the bytes there are not one.
inline size_t utf8_length(std::string_view s, size_t i) {
    const auto b = [&](size_t k) { return static_cast<unsigned char>(s[k]); };
    const unsigned char c = b(i);
    size_t n = 0;
    uint32_t cp = 0;
    if (c < 0x80) return 1;
    if (c >= 0xC2 && c <= 0xDF) n = 2, cp = c & 0x1Fu;
    else if (c >= 0xE0 && c <= 0xEF) n = 3, cp = c & 0x0Fu;
    else if (c >= 0xF0 && c <= 0xF4) n = 4, cp = c & 0x07u;
    else return 0;
    if (i + n > s.size()) return 0;
    for (size_t k = 1; k < n; ++k) {
        if ((b(i + k) & 0xC0u) != 0x80u) return 0;
        cp = (cp << 6) | (b(i + k) & 0x3Fu);
    }
    if ((n == 3 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) || (n == 4 && (cp < 0x10000 || cp > 0x10FFFF))) return 0;
    return n;
}

// Appends `s` as a JSON string. Mod data and file names are not always UTF-8 (a mod's XML in a Windows code page): a byte
// that does not start a valid UTF-8 sequence is written as the Latin-1 character of that value (\u00XX), so the output is
// always valid JSON. '"', '\\' and control characters are escaped; valid UTF-8 passes through unchanged.
inline void json_str(std::string& o, std::string_view s) {
    o.push_back('"');
    for (size_t i = 0; i < s.size();) {
        const char c = s[i];
        const auto u = static_cast<unsigned char>(c);
        const size_t n = utf8_length(s, i);
        if (c == '"' || c == '\\') {
            o.push_back('\\');
            o.push_back(c);
        } else if (u < 0x20 || n == 0) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(u));
            o += buf;
        } else {
            o.append(s.substr(i, n));
            i += n;
            continue;
        }
        ++i;
    }
    o.push_back('"');
}

}  // namespace alamo::game::json_text
