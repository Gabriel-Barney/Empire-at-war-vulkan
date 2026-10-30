#include "alamo/objects/field_schema.hpp"

#include <string>
#include <unordered_map>
#include <vector>

#include "alamo/core/hash.hpp"

namespace alamo::objects {

namespace {

const FieldSchema kRows[] = {
#include "field_schema_table.inc"
};

const std::string_view kSubObjects[] = {
#include "sub_object_names.inc"
};

struct Index {
    std::unordered_map<std::string, std::vector<uint32_t>> by_tag;  // lookup only, rows in doc order
    std::unordered_map<std::string, uint8_t> sub_objects;           // lookup only
    Index() {
        for (uint32_t i = 0; i < static_cast<uint32_t>(std::size(kRows)); ++i) {
            by_tag[normalize_name(kRows[i].tag)].push_back(i);
            sub_objects.emplace(normalize_name(kRows[i].owner), uint8_t{1});
        }
        for (std::string_view n : kSubObjects) sub_objects.emplace(normalize_name(n), uint8_t{1});
    }
};

const Index& index() {
    static const Index idx;
    return idx;
}

const std::vector<uint32_t>* rows_of(std::string_view tag) {
    const auto& m = index().by_tag;
    const auto it = m.find(normalize_name(tag));
    return it == m.end() ? nullptr : &it->second;
}

}  // namespace

const char* to_string(FieldKind k) {
    switch (k) {
        case FieldKind::Raw: return "raw";
        case FieldKind::Bool: return "bool";
        case FieldKind::Int: return "int";
        case FieldKind::Float: return "float";
        case FieldKind::Name: return "name";
        case FieldKind::NumberList: return "numbers";
    }
    return "?";
}

FieldKind kind_of_code(uint8_t code) {
    switch (code) {
        case 0x0: return FieldKind::Bool;
        case 0x2: case 0x3: case 0x4: case 0x5: case 0x6: return FieldKind::Int;
        case 0x8: case 0x9: case 0xa: return FieldKind::Float;
        case 0xc: case 0xd: case 0xe: case 0x17: case 0x1c: case 0x1d: case 0x1f: case 0x20: case 0x21:
        case 0x27: case 0x32: case 0x43: case 0x49: case 0x4a: case 0x4b: case 0x52: case 0x53: case 0x54:
            return FieldKind::Name;
        case 0x10: case 0x11: case 0x14: case 0x16: return FieldKind::NumberList;
        default: return FieldKind::Raw;
    }
}

std::span<const FieldSchema> field_schema() { return {kRows, std::size(kRows)}; }

uint32_t find_field_schema(std::string_view tag, std::string_view owner_hint) {
    const std::vector<uint32_t>* rows = rows_of(tag);
    if (!rows) return kNoSchema;
    if (!owner_hint.empty())
        for (uint32_t r : *rows)
            if (iequals(kRows[r].owner, owner_hint)) return r;
    for (uint32_t r : *rows)
        if (kRows[r].owner == "GameObjectType") return r;
    return rows->front();
}

bool is_sub_object_element(std::string_view tag) {
    return index().sub_objects.count(normalize_name(tag)) != 0;
}

bool field_schema_ambiguous(std::string_view tag) {
    const std::vector<uint32_t>* rows = rows_of(tag);
    if (!rows) return false;
    for (uint32_t r : *rows)
        if (kRows[r].code != kRows[rows->front()].code) return true;
    return false;
}

}  // namespace alamo::objects
