// Exact ASCII string helpers (CU2). Unlike alamo::iequals / normalize_name (hash.hpp), nothing is
// folded except the letters 'a'..'z' <-> 'A'..'Z': '/' and '\\' stay different, and every other byte
// (including non-ASCII bytes >= 0x80) passes through unchanged. That is what std::toupper /
// std::tolower do in the "C" locale, which the engine never changes (no setlocale anywhere), so these
// replace the module-local toupper / manual-range copies with identical results.
//
// Whitespace here is exactly space, tab, CR and LF (not \v / \f; data::trim also strips those).
#pragma once
#include <cstddef>
#include <string>
#include <string_view>

namespace alamo {

constexpr char ascii_to_upper(char c) noexcept { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c; }
constexpr char ascii_to_lower(char c) noexcept { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }
constexpr bool ascii_is_space(char c) noexcept { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

inline std::string ascii_upper(std::string_view s) {
    std::string r(s);
    for (char& c : r) c = ascii_to_upper(c);
    return r;
}
inline std::string ascii_lower(std::string_view s) {
    std::string r(s);
    for (char& c : r) c = ascii_to_lower(c);
    return r;
}
inline void ascii_upper_in_place(std::string& s) noexcept {
    for (char& c : s) c = ascii_to_upper(c);
}
inline void ascii_lower_in_place(std::string& s) noexcept {
    for (char& c : s) c = ascii_to_lower(c);
}

// Case-insensitive equality; '/' != '\\' (alamo::iequals folds those).
constexpr bool ascii_iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (ascii_to_upper(a[i]) != ascii_to_upper(b[i])) return false;
    return true;
}
constexpr bool ascii_istarts_with(std::string_view s, std::string_view prefix) noexcept {
    return s.size() >= prefix.size() && ascii_iequals(s.substr(0, prefix.size()), prefix);
}
constexpr bool ascii_iends_with(std::string_view s, std::string_view suffix) noexcept {
    return s.size() >= suffix.size() && ascii_iequals(s.substr(s.size() - suffix.size()), suffix);
}
// Case-insensitive substring test; an empty needle is found.
constexpr bool ascii_icontains(std::string_view haystack, std::string_view needle) noexcept {
    if (needle.size() > haystack.size()) return false;
    for (size_t i = 0; i + needle.size() <= haystack.size(); ++i)
        if (ascii_iequals(haystack.substr(i, needle.size()), needle)) return true;
    return false;
}
// Case-insensitive strict weak order (compare upper-cased bytes as unsigned char).
constexpr bool ascii_iless(std::string_view a, std::string_view b) noexcept {
    const size_t n = a.size() < b.size() ? a.size() : b.size();
    for (size_t i = 0; i < n; ++i) {
        const auto x = static_cast<unsigned char>(ascii_to_upper(a[i]));
        const auto y = static_cast<unsigned char>(ascii_to_upper(b[i]));
        if (x != y) return x < y;
    }
    return a.size() < b.size();
}

// Strips space, tab, CR and LF from both ends.
constexpr std::string_view ascii_trim(std::string_view s) noexcept {
    size_t b = 0, e = s.size();
    while (b < e && ascii_is_space(s[b])) ++b;
    while (e > b && ascii_is_space(s[e - 1])) --e;
    return s.substr(b, e - b);
}

// A name that the whitespace-separated text formats can carry (CRC log system / parity names, save
// chunk names): non-empty, no space, tab, CR or LF.
constexpr bool valid_chunk_name(std::string_view n) noexcept {
    if (n.empty()) return false;
    for (const char c : n)
        if (ascii_is_space(c)) return false;
    return true;
}

}  // namespace alamo
