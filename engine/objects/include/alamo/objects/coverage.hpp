// Field coverage report over an ObjectTypeDatabase: for every element tag seen below a type
// element -- occurrences, types and files using it, known (schema row) or unknown, typed parse
// results and observed value shapes. Unknown tags are listed, never dropped.
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/objects/object_types.hpp"

namespace alamo::objects {

enum class ValueShape : uint8_t { Empty, BoolWord, Int, Float, NumberList, Name, NameList, Text, Count };
const char* to_string(ValueShape s);
ValueShape shape_of(std::string_view text);  // trimmed text -> shape

struct TagCoverage {
    TagId tag = kNone;
    std::string_view name;
    uint32_t occurrences = 0, top_level = 0, nested = 0, with_children = 0;
    uint32_t types = 0;  // distinct type definitions using the tag
    std::vector<uint32_t> files;  // distinct files (files() indices), first-use order
    uint32_t schema = kNoSchema;  // row used by the first occurrence
    bool ambiguous = false;       // schema rows disagree on the parser code
    bool sub_object = false;      // no schema row, but a known sub-object element (is_sub_object_element)
    std::array<uint32_t, 5> status{};  // indexed by ParseStatus
    std::array<uint32_t, static_cast<size_t>(ValueShape::Count)> shapes{};
    std::vector<std::string> failures;  // first few "file(line): <Tag> 'text' -> status"
    bool known() const { return schema != kNoSchema; }
    bool unknown() const { return !known() && !sub_object; }
};

// One distinct token of the behavior-list tags (is_behavior_list_tag), case-insensitive.
struct BehaviorCoverage {
    std::string_view name;            // first spelling seen
    uint32_t occurrences = 0, types = 0;
    uint32_t tag_mask = 0;            // bit i = i-th behavior-list tag in first-seen order (tags below)
    uint32_t id = 0;                  // behavior_type_id (OBJECT_RUNTIME.md §4.3); 0 = unknown name
};

struct CoverageReport {
    LoadStats stats;
    size_t files = 0;
    std::vector<std::pair<std::string_view, uint32_t>> elements;  // type element -> count, first-seen order
    std::vector<TagCoverage> tags;  // TagId order (first seen); format() sorts for output
    uint32_t tags_known = 0, tags_sub_object = 0, tags_unknown = 0;  // a tag counts in exactly one
    uint64_t occ_known = 0, occ_sub_object = 0, occ_unknown = 0, occ_typed = 0, occ_ok = 0, occ_empty = 0, occ_lossy = 0, occ_failed = 0;
    uint32_t schema_rows = 0, schema_rows_seen = 0;
    std::vector<std::string_view> behavior_tags;   // behavior-list tags seen, first-seen order
    std::vector<BehaviorCoverage> behaviors;       // first-seen order
    uint32_t behaviors_known = 0, behaviors_unknown = 0, behavior_ids_used = 0;  // ids out of kBehaviorTypeCount-1  // field_schema() rows whose tag occurs at least once
};

CoverageReport build_coverage(const ObjectTypeDatabase& db, size_t max_failure_examples = 4);
// Plain-text report: summary, element kinds, unknown tags, parse problems, then every tag.
std::string format_coverage(const CoverageReport& r, const ObjectTypeDatabase& db, std::string_view title);

}  // namespace alamo::objects
