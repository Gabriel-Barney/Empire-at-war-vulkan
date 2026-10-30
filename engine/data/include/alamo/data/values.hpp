// Value helpers for XML text. All of them ignore surrounding whitespace (the game files are full
// of "<Tag> 18 </Tag>"). Number parsing uses std::from_chars (locale-free, correctly rounded),
// so a given string produces bit-identical values on every platform.
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "alamo/core/galaxy.hpp"

namespace alamo::data {

std::string_view trim(std::string_view s);

// atof-style: leading whitespace and '+' accepted, the longest numeric prefix is used and any
// trailing junk ("5.0f") is ignored. nullopt if no number starts the string.
std::optional<float> parse_float(std::string_view s);
// atoi-style: optional sign, decimal digits; a fractional part is truncated ("3.0" -> 3).
// nullopt if no digits or the value does not fit in int32.
std::optional<int32_t> parse_int(std::string_view s);
// yes/no, true/false, 1/0, on/off (ASCII case-insensitive). nullopt otherwise.
std::optional<bool> parse_bool(std::string_view s);

inline float parse_float_or(std::string_view s, float fallback) { return parse_float(s).value_or(fallback); }
inline int32_t parse_int_or(std::string_view s, int32_t fallback) { return parse_int(s).value_or(fallback); }
inline bool parse_bool_or(std::string_view s, bool fallback) { return parse_bool(s).value_or(fallback); }

// Up to `max` numbers separated by commas and/or whitespace ("1, 2, 3", "1 2 3"). Returns how
// many were read; stops at the first token that is not a number.
size_t parse_floats(std::string_view s, float* out, size_t max);
// "x, y, z": exactly three numbers required.
std::optional<Vec3> parse_vec3(std::string_view s);

// Name list separated by commas and/or whitespace (<Locations>, <Trade_Routes>); empty tokens
// are dropped. Views point into `s`.
std::vector<std::string_view> split_list(std::string_view s);
// Comma-separated fields, each trimmed; empty fields kept ("Empire, Hoth, Ground_X" -> 3).
std::vector<std::string_view> split_fields(std::string_view s);
// "Rebel, Hoth" -> {"Rebel", "Hoth"}. Splits at the first comma (or, without a comma, the first
// whitespace run). nullopt unless both halves are non-empty.
std::optional<std::pair<std::string_view, std::string_view>> parse_pair(std::string_view s);

}  // namespace alamo::data
