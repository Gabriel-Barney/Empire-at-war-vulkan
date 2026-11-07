#pragma once
// ui-internal text helpers shared by the GUI / HUD sources (not a public header). ASCII case helpers live in
// alamo/core/ascii.hpp (ascii_iequals, ascii_upper, ...) and alamo/core/hash.hpp (iequals).

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace alamo::ui {

// Byte string -> UTF-32, one code point per byte (0..255): for ASCII ids, digits and printf output.
inline std::u32string ascii32(std::string_view s) {
    std::u32string o;
    for (char c : s) o.push_back(static_cast<char32_t>(static_cast<unsigned char>(c)));
    return o;
}

// Decimal text of `v` ("-12") as UTF-32.
inline std::u32string num32(int64_t v) {
    char b[32];
    std::snprintf(b, sizeof b, "%lld", static_cast<long long>(v));
    return ascii32(b);
}

// printf of one int into a control / bone id ("IDC_SLOT_%d", 3 -> "IDC_SLOT_3"); ids are under 48 characters.
inline std::string id_n(const char* fmt, int32_t n) {
    char b[48];
    std::snprintf(b, sizeof b, fmt, n);
    return b;
}

}  // namespace alamo::ui
