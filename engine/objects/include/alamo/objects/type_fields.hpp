// GameObjectType field set and converters (docs/re/OBJECT_TYPES.md §4).
//
// The type's descriptor table = the rows of xml_field_tables.md table 0 whose segment owner is
// GameObjectType, SpaceProp or HeroUnit (894 rows, offsets 0x10..0x2948 inside the 0x2950-byte
// type; the doc's "~880 fields"). A tag listed twice keeps its first row. Every other tag inside a
// type element is "not handled" and ignored by the typed view (it stays in the raw store).
//
// Number parsing reproduces the MSVC C runtime the original links, deterministically and
// locale-free on every compiler: c_atoi / c_atof below.
#pragma once
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace alamo::objects {

// How a code converts its text (§4.5).
enum class Conv : uint8_t {
    Bool,          // 0x0: true iff exactly "1" or first char Y/y/T/t
    Int32,         // 0x1, 0x5, 0x6: atoi
    Int8,          // 0x2, 0x3: atoi truncated to int8
    UIntClamp100,  // 0x4, 0x7: atoi, clamped to <= 100 (unsigned compare)
    Float,         // 0x8: atof -> double -> float
    Float01,       // 0x9: as Float, clamped to [0,1] (logged)
    Double,        // 0xa
    Double01,      // 0xb
    Float2,        // 0xf: first two tokens; missing tokens leave the old values
    Vec3,          // 0x10: three tokens (partial update as Float2)
    Float4,        // 0x11: colour
    Rgba,          // 0x16: four ints
    String,        // 0x17, 0xc, 0xd, events 0x1f-0x21, unknown scalar codes: text kept
    StringUpper,   // 0x1c: compared case-insensitively (kept as written)
    Enum,          // 0xe, 0x32, 0x43, 0x4a, 0x52: enums.hpp converter (name kept; 0x43/0x52/anim: name only)
    Mask,          // 0x54, 0x56: whole value through a bitfield converter; tokens split at '|', ',', whitespace
    TypeRef,       // 0x1d: lazy type name ("None" / unknown -> null)
    Tokens,        // list codes: tokens appended (see Merge)
    SubObjects,    // 0x38, 0x39: sub-object list built from the child elements
};

// What repeated tags / variants do (§3.3, §4.4).
enum class Merge : uint8_t {
    Scalar,       // last occurrence wins; a variant overwrites the inherited value
    ClearList,    // normal: append; variant: every occurrence clears first (last one survives)
    TypeRefList,  // 0x1e, 0x3b: append; the first append after a variant copy clears (all child occurrences kept)
    AppendList,   // 0x46, 0x47, 0x53, 0x22, 0x3c: always append, also onto the inherited list
};

struct TypeField {
    std::string_view tag;
    uint8_t code = 0;
    uint32_t schema_row = 0;  // field_schema() row
    Conv conv = Conv::String;
    Merge merge = Merge::Scalar;
};

std::span<const TypeField> type_fields();
uint32_t find_type_field(std::string_view tag);  // case-insensitive; 0xFFFFFFFF if not a type field
Conv conv_of_code(uint8_t code);
Merge merge_of_code(uint8_t code);

// MSVC CRT semantics, locale-free: leading C whitespace, sign, digits; the rest is ignored;
// garbage -> 0; atoi clamps to the int32 range on overflow (strtol); atof accepts decimal with
// exponent, "inf"/"infinity"/"nan" and hex ("0x1.8p3"); overflow -> +-inf, underflow -> +-0.
int32_t c_atoi(std::string_view s);
double c_atof(std::string_view s);
bool parse_type_bool(std::string_view s);  // §4.5 code 0x0
// List tokeniser of §4.5: delimiters space, comma, tab, CR, LF (collapsed). With keep_bar, '|' is
// a token of its own (0x53 OR groups); with split_bar, '|' is a delimiter (masks).
std::vector<std::string_view> split_type_list(std::string_view s, bool keep_bar = false, bool split_bar = false);

}  // namespace alamo::objects
