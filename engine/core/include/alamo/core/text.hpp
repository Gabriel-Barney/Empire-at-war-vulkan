// ASCII text helpers shared by every module: locale-free, so identical on every build and thread.
// Not for path comparison: alamo::iequals (hash.hpp) additionally treats '/' and '\' as equal.
#pragma once
#include <string>
#include <string_view>

namespace alamo {

constexpr char ascii_upper(char c) noexcept { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c; }
constexpr char ascii_lower(char c) noexcept { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

inline std::string ascii_upper(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = ascii_upper(c);
    return out;
}

inline std::string ascii_lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = ascii_lower(c);
    return out;
}

inline bool ascii_iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (ascii_lower(a[i]) != ascii_lower(b[i])) return false;
    return true;
}

// Strips leading and trailing space, tab, CR and LF.
inline std::string_view ascii_trim(std::string_view s) noexcept {
    constexpr std::string_view kWs = " \t\r\n";
    const size_t a = s.find_first_not_of(kWs);
    if (a == std::string_view::npos) return {};
    return s.substr(a, s.find_last_not_of(kWs) - a + 1);
}

}  // namespace alamo
