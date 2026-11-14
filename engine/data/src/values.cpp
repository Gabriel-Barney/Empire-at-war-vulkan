#include "alamo/data/values.hpp"

#include <charconv>
#include <cmath>
#include <limits>

#include "alamo/core/hash.hpp"

namespace alamo::data {

namespace {
inline bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f'; }
inline bool is_sep(char c) { return is_ws(c) || c == ','; }

// Parses a float at the start of s (after optional '+'); returns chars consumed (0 = none).
size_t float_prefix(std::string_view s, float& out) {
    size_t i = 0;
    bool neg_plus = false;
    if (i < s.size() && s[i] == '+') {
        ++i;
        neg_plus = true;
    }
    if (i >= s.size()) return 0;
    // from_chars rejects a leading '+' but handles '-'; reject "+-1".
    if (neg_plus && s[i] == '-') return 0;
    float v = 0.0f;
    const char* b = s.data() + i;
    const auto r = std::from_chars(b, s.data() + s.size(), v, std::chars_format::general);
    if (r.ec == std::errc::result_out_of_range) {
        // Over/underflow: clamp the way strtof does (HUGE_VAL / 0) instead of failing.
        const size_t j = static_cast<size_t>(r.ptr - s.data());
        const bool neg = s[i] == '-';
        // Decide over vs under by looking for a negative exponent.
        bool tiny = false;
        for (const char* q = b; q < r.ptr; ++q)
            if ((*q == 'e' || *q == 'E') && q + 1 < r.ptr && q[1] == '-') tiny = true;
        out = tiny ? (neg ? -0.0f : 0.0f) : (neg ? -std::numeric_limits<float>::infinity() : std::numeric_limits<float>::infinity());
        return j;
    }
    if (r.ec != std::errc()) return 0;
    // from_chars accepts "inf"/"nan"; the game data never uses them, treat as not-a-number.
    if (!std::isfinite(v)) return 0;
    out = v;
    return static_cast<size_t>(r.ptr - s.data());
}
}  // namespace

std::string_view trim(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e && is_ws(s[b])) ++b;
    while (e > b && is_ws(s[e - 1])) --e;
    return s.substr(b, e - b);
}

std::optional<float> parse_float(std::string_view s) {
    s = trim(s);
    float v = 0.0f;
    if (float_prefix(s, v) == 0) return std::nullopt;
    return v;
}

std::optional<int32_t> parse_int(std::string_view s) {
    s = trim(s);
    size_t i = 0;
    if (i < s.size() && s[i] == '+') {
        ++i;
        if (i < s.size() && s[i] == '-') return std::nullopt;
    }
    int64_t v = 0;
    const auto r = std::from_chars(s.data() + i, s.data() + s.size(), v, 10);
    if (r.ec != std::errc()) return std::nullopt;
    if (v < std::numeric_limits<int32_t>::min() || v > std::numeric_limits<int32_t>::max()) return std::nullopt;
    return static_cast<int32_t>(v);
}

std::optional<int32_t> parse_int_prefix(std::string_view s) {
    size_t i = 0;
    while (i < s.size() && is_ws(s[i])) ++i;
    bool neg = false;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) neg = s[i++] == '-';
    const size_t first = i;
    int64_t v = 0;
    bool overflow = false;
    for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) {
        if (!overflow) {
            v = v * 10 + (s[i] - '0');
            if (v > 2147483648ll) overflow = true;
        }
    }
    if (i == first) return std::nullopt;
    if (neg) {
        if (overflow || v > 2147483648ll) return std::numeric_limits<int32_t>::min();
        return static_cast<int32_t>(-v);
    }
    if (overflow || v > 2147483647ll) return std::numeric_limits<int32_t>::max();
    return static_cast<int32_t>(v);
}

bool parse_type_bool(std::string_view s) {
    if (s == "1") return true;
    if (s.empty()) return false;
    const char c = s[0];
    return c == 'Y' || c == 'y' || c == 'T' || c == 't';
}

bool parse_flag_char(std::string_view s) {
    if (s.empty()) return false;
    const char c = s[0];
    return c == 'Y' || c == 'y' || c == 'T' || c == 't' || c == '1';
}

std::optional<bool> parse_bool(std::string_view s) {
    s = trim(s);
    if (iequals(s, "yes") || iequals(s, "true") || s == "1" || iequals(s, "on")) return true;
    if (iequals(s, "no") || iequals(s, "false") || s == "0" || iequals(s, "off")) return false;
    return std::nullopt;
}

size_t parse_floats(std::string_view s, float* out, size_t max) {
    size_t n = 0, i = 0;
    while (n < max) {
        while (i < s.size() && is_sep(s[i])) ++i;
        if (i >= s.size()) break;
        float v = 0.0f;
        const size_t used = float_prefix(s.substr(i), v);
        if (used == 0) break;
        out[n++] = v;
        i += used;
        // Skip junk glued to the number ("1.0f") up to the next separator.
        while (i < s.size() && !is_sep(s[i])) ++i;
    }
    return n;
}

std::optional<Vec3> parse_vec3(std::string_view s) {
    float v[3] = {0, 0, 0};
    if (parse_floats(s, v, 3) != 3) return std::nullopt;
    return Vec3{v[0], v[1], v[2]};
}

std::vector<std::string_view> split_list(std::string_view s) {
    std::vector<std::string_view> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && is_sep(s[i])) ++i;
        const size_t b = i;
        while (i < s.size() && !is_sep(s[i])) ++i;
        if (i > b) out.push_back(s.substr(b, i - b));
    }
    return out;
}

std::vector<std::string_view> split_fields(std::string_view s) {
    std::vector<std::string_view> out;
    if (trim(s).empty()) return out;
    size_t b = 0;
    for (;;) {
        const size_t c = s.find(',', b);
        out.push_back(trim(s.substr(b, c == std::string_view::npos ? std::string_view::npos : c - b)));
        if (c == std::string_view::npos) break;
        b = c + 1;
    }
    return out;
}

std::optional<std::pair<std::string_view, std::string_view>> parse_pair(std::string_view s) {
    s = trim(s);
    size_t cut = s.find(',');
    size_t next = cut + 1;
    if (cut == std::string_view::npos) {
        cut = 0;
        while (cut < s.size() && !is_ws(s[cut])) ++cut;
        next = cut;
    }
    if (cut >= s.size()) return std::nullopt;
    const std::string_view a = trim(s.substr(0, cut));
    const std::string_view b = trim(s.substr(next));
    if (a.empty() || b.empty()) return std::nullopt;
    return std::make_pair(a, b);
}

}  // namespace alamo::data
