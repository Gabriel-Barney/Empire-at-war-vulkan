#include "alamo/objects/type_fields.hpp"

#include <charconv>
#include <cmath>
#include <limits>
#include <string>
#include <unordered_map>

#include "alamo/core/hash.hpp"
#include "alamo/core/text.hpp"
#include "alamo/objects/field_schema.hpp"

namespace alamo::objects {

namespace {

bool is_type_segment(const FieldSchema& r) {
    return r.table == 0 && (r.owner == "GameObjectType" || r.owner == "SpaceProp" || r.owner == "HeroUnit");
}

struct FieldIndexTable {
    std::vector<TypeField> fields;
    std::unordered_map<std::string, uint32_t> by_tag;  // lookup only
    FieldIndexTable() {
        const auto rows = field_schema();
        for (uint32_t i = 0; i < static_cast<uint32_t>(rows.size()); ++i) {
            if (!is_type_segment(rows[i])) continue;
            const auto [it, inserted] = by_tag.emplace(normalize_name(rows[i].tag), static_cast<uint32_t>(fields.size()));
            (void)it;
            if (!inserted) continue;  // a tag listed twice keeps its first row
            fields.push_back({rows[i].tag, rows[i].code, i, conv_of_code(rows[i].code), merge_of_code(rows[i].code)});
        }
    }
};

const FieldIndexTable& table() {
    static const FieldIndexTable t;
    return t;
}

bool c_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r'; }
bool digit(char c) { return c >= '0' && c <= '9'; }
bool hexdigit(char c) { return digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
bool starts_ci(std::string_view s, std::string_view p) {
    if (s.size() < p.size()) return false;
    for (size_t i = 0; i < p.size(); ++i)
        if (ascii_lower(s[i]) != p[i]) return false;
    return true;
}

}  // namespace

Conv conv_of_code(uint8_t code) {
    switch (code) {
        case 0x0: return Conv::Bool;
        case 0x1: case 0x5: case 0x6: return Conv::Int32;
        case 0x2: case 0x3: return Conv::Int8;
        case 0x4: case 0x7: return Conv::UIntClamp100;
        case 0x8: return Conv::Float;
        case 0x9: return Conv::Float01;
        case 0xa: return Conv::Double;
        case 0xb: return Conv::Double01;
        case 0xf: return Conv::Float2;
        case 0x10: return Conv::Vec3;
        case 0x11: return Conv::Float4;
        case 0x16: return Conv::Rgba;
        case 0x1c: return Conv::StringUpper;
        case 0xe: case 0x32: case 0x43: case 0x4a: case 0x52: return Conv::Enum;
        case 0x54: case 0x56: return Conv::Mask;
        case 0x1d: return Conv::TypeRef;
        case 0x38: case 0x39: return Conv::SubObjects;
        default: break;
    }
    return merge_of_code(code) == Merge::Scalar ? Conv::String : Conv::Tokens;
}

Merge merge_of_code(uint8_t code) {
    switch (code) {
        // §3.3 clearing list kinds; 0x42 (SFXEvent_Hardpoint_Destroyed: "only the last line is
        // effective" in variants, §4.4).
        case 0x12: case 0x13: case 0x14: case 0x15: case 0x18: case 0x19: case 0x1a: case 0x1b: case 0x2a:
        case 0x2b: case 0x2d: case 0x34: case 0x35: case 0x37: case 0x38: case 0x39: case 0x3a: case 0x3d:
        case 0x44: case 0x45: case 0x48: case 0x4f: case 0x51: case 0x55: case 0x42:
            return Merge::ClearList;
        case 0x1e: case 0x3b: return Merge::TypeRefList;
        // 0x22 / 0x3c (§9.6): one pair per occurrence, never cleared (variants append to the copy).
        case 0x46: case 0x47: case 0x53: case 0x22: case 0x3c: return Merge::AppendList;
        default: return Merge::Scalar;
    }
}

std::span<const TypeField> type_fields() { return table().fields; }

uint32_t find_type_field(std::string_view tag) {
    const auto& m = table().by_tag;
    const auto it = m.find(normalize_name(tag));
    return it == m.end() ? 0xFFFFFFFFu : it->second;
}

int32_t c_atoi(std::string_view s) {
    size_t i = 0;
    while (i < s.size() && c_space(s[i])) ++i;
    bool neg = false;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) neg = s[i++] == '-';
    int64_t v = 0;
    bool overflow = false;
    for (; i < s.size() && digit(s[i]); ++i) {
        if (!overflow) {
            v = v * 10 + (s[i] - '0');
            if (v > 2147483648ll) overflow = true;
        }
    }
    if (neg) {
        if (overflow || v > 2147483648ll) return std::numeric_limits<int32_t>::min();
        return static_cast<int32_t>(-v);
    }
    if (overflow || v > 2147483647ll) return std::numeric_limits<int32_t>::max();
    return static_cast<int32_t>(v);
}

double c_atof(std::string_view s) {
    size_t i = 0;
    while (i < s.size() && c_space(s[i])) ++i;
    bool neg = false;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) neg = s[i++] == '-';
    const std::string_view r = s.substr(i);
    const double sign = neg ? -1.0 : 1.0;
    if (starts_ci(r, "inf")) return sign * std::numeric_limits<double>::infinity();
    if (starts_ci(r, "nan")) return std::numeric_limits<double>::quiet_NaN();
    // Hex: "0x" followed by a hex digit (or '.' + hex digit).
    if (r.size() > 2 && r[0] == '0' && ascii_lower(r[1]) == 'x' &&
        (hexdigit(r[2]) || (r[2] == '.' && r.size() > 3 && hexdigit(r[3])))) {
        double v = 0;
        const auto res = std::from_chars(r.data() + 2, r.data() + r.size(), v, std::chars_format::hex);
        if (res.ec == std::errc::result_out_of_range) return 0.0 * sign;  // rare; treated as underflow
        return res.ec == std::errc() ? sign * v : 0.0;
    }
    // Decimal prefix: digits [. digits] [e [+-] digits]; at least one mantissa digit.
    size_t j = 0, mant = 0, int_digits = 0;
    bool seen_nonzero = false;
    while (j < r.size() && digit(r[j])) {
        if (r[j] != '0') seen_nonzero = true;
        if (seen_nonzero) ++int_digits;
        ++j, ++mant;
    }
    int lead_frac_zeros = 0;
    if (j < r.size() && r[j] == '.') {
        ++j;
        while (j < r.size() && digit(r[j])) {
            if (!seen_nonzero) {
                if (r[j] == '0') ++lead_frac_zeros;
                else seen_nonzero = true;
            }
            ++j, ++mant;
        }
    }
    if (mant == 0) return 0.0;
    int64_t exp10 = 0;
    if (j < r.size() && ascii_lower(r[j]) == 'e') {
        size_t k = j + 1;
        bool eneg = false;
        if (k < r.size() && (r[k] == '+' || r[k] == '-')) eneg = r[k++] == '-';
        if (k < r.size() && digit(r[k])) {
            for (; k < r.size() && digit(r[k]); ++k)
                if (exp10 < 100000) exp10 = exp10 * 10 + (r[k] - '0');
            if (eneg) exp10 = -exp10;
            j = k;
        }
    }
    double v = 0;
    const auto res = std::from_chars(r.data(), r.data() + j, v, std::chars_format::general);
    if (res.ec == std::errc::result_out_of_range) {
        // Order of magnitude decides overflow (+-HUGE_VAL = inf) versus underflow (+-0).
        const int64_t mag = int_digits > 0 ? static_cast<int64_t>(int_digits) + exp10 : exp10 - lead_frac_zeros;
        return mag > 0 ? sign * std::numeric_limits<double>::infinity() : sign * 0.0;
    }
    return res.ec == std::errc() ? sign * v : 0.0;
}

bool parse_type_bool(std::string_view s) {
    if (s == "1") return true;
    if (s.empty()) return false;
    const char c = s[0];
    return c == 'Y' || c == 'y' || c == 'T' || c == 't';
}

std::vector<std::string_view> split_type_list(std::string_view s, bool keep_bar, bool split_bar) {
    std::vector<std::string_view> out;
    size_t i = 0;
    auto delim = [&](char c) {
        return c == ' ' || c == ',' || c == '\t' || c == '\r' || c == '\n' || ((keep_bar || split_bar) && c == '|');
    };
    while (i < s.size()) {
        while (i < s.size() && delim(s[i])) {
            if (keep_bar && s[i] == '|') out.push_back(s.substr(i, 1));
            ++i;
        }
        const size_t b = i;
        while (i < s.size() && !delim(s[i])) ++i;
        if (i > b) out.push_back(s.substr(b, i - b));
    }
    return out;
}

}  // namespace alamo::objects
