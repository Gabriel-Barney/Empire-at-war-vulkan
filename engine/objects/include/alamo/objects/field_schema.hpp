// Known XML fields: the descriptor rows recovered in docs/re/xml_field_tables.md (tag -> parser
// code -> member offset, grouped by owning class segment), compiled in from the generated
// src/field_schema_table.inc (engine/objects/tools/gen_field_schema.py).
//
// The doc only characterises parser codes by the value kinds seen in vanilla XML, so the value
// kind below is the MAJORITY kind per code (see kind_of_code). Codes with mixed or list samples
// are kept as Raw text (never a parse failure); the coverage report shows the observed value
// shapes per tag so the mapping can be refined from data.
#pragma once
#include <cstdint>
#include <span>
#include <string_view>

namespace alamo::objects {

enum class FieldKind : uint8_t {
    Raw,         // text kept as written (lists, names lists, structured values, unknown codes)
    Bool,        // code 0x0: yes/no, true/false, 1/0, on/off
    Int,         // codes 0x2..0x6: atoi-style (fraction truncated)
    Float,       // codes 0x8..0xa: atof-style
    Name,        // single name/string codes (0xc..0xe, 0x17, 0x1c, 0x1d, 0x1f, 0x20, 0x21, ...)
    NumberList,  // vector codes 0x10, 0x11, 0x14, 0x16: comma/space separated numbers
};
const char* to_string(FieldKind k);
FieldKind kind_of_code(uint8_t code);

struct FieldSchema {
    std::string_view tag;      // as written in the doc
    uint8_t code = 0;          // parser type code
    uint32_t offset = 0;       // member offset in the owning class (layout hint only)
    std::string_view owner;    // segment owner, e.g. "GameObjectType", "Combat_Bonus_Ability"
    uint8_t table = 0;         // ordinal of the descriptor table in the doc
    std::string_view sample;   // sample kind column of the doc
    FieldKind kind() const { return kind_of_code(code); }
};

inline constexpr uint32_t kNoSchema = 0xFFFFFFFFu;

// Every row, in document order.
std::span<const FieldSchema> field_schema();
// Row for `tag` (ASCII case-insensitive), or kNoSchema. Priority: a row whose segment owner equals
// `owner_hint` (e.g. the enclosing element's name for nested fields), then a row of a
// "GameObjectType" segment, then the first row in document order.
uint32_t find_field_schema(std::string_view tag, std::string_view owner_hint = {});
// True when rows for `tag` disagree on the parser code.
bool field_schema_ambiguous(std::string_view tag);
// Element names of sub-objects (abilities / actions such as <Combat_Bonus_Ability>, <Unit_Ability>):
// the code-referenced names of docs/re/xml_tags.md "AllyEventClass" plus every segment owner of
// field_schema(). Such an element is a container whose children are the owner's fields.
bool is_sub_object_element(std::string_view tag);

}  // namespace alamo::objects
