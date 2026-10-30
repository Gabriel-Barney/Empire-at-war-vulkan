// GameObjectType database: every type definition of every file listed in GameObjectFiles.xml,
// with EVERY element of its subtree kept (tag, trimmed text, attributes, file, line) so nothing
// in the XML is dropped -- known fields get a schema row and a typed parse status, unknown ones
// are still stored and reported.
//
// Loading (stage 1, before docs/re/OBJECT_TYPES.md; rules taken from docs/re/PHASE2_ANSWERS.md Q13
// as implemented by alamo_data):
//   * Data\XML\GameObjectFiles.xml is read through the FileProvider (VFS rules apply); each <File>
//     is "Data\XML\<name>", in LIST ORDER; a file listed twice is parsed (walked) twice; a listed
//     file that cannot be opened is skipped with a diagnostic.
//   * every element child of a file's root element is one type definition (named or not; an
//     unnamed one is kept and reported, never dropped);
//   * duplicate names: every definition gets a TypeIndex; find() returns the FIRST definition and
//     later ones carry `first_definition` != themselves.
//   * Variant_Of_Existing_Type is recorded (text + resolved first definition, forward references
//     allowed) but NOT merged yet: field inheritance waits for OBJECT_TYPES.md.
//
// Stable order: TypeIndex = load order (list order, then document order); TagId = first-seen order;
// FieldIndex = load order, preorder within a type. Nothing depends on hash order.
//
// Storage is SoA. Every std::string_view points into documents owned by the database; the
// database is move-only and views stay valid across moves.
#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "alamo/core/file_provider.hpp"
#include "alamo/data/xml.hpp"
#include "alamo/objects/field_schema.hpp"

namespace alamo::objects {

using TypeIndex = uint32_t;
using FieldIndex = uint32_t;
using TagId = uint32_t;
inline constexpr uint32_t kNone = 0xFFFFFFFFu;

enum class ParseStatus : uint8_t {
    Untyped,  // no schema row, or a Raw-kind row: text kept as is
    Ok,       // the whole text parsed as the schema kind
    Empty,    // typed field with empty text
    Lossy,    // a value was read but some text was ignored ("5.0f", "1, 2" for a float, bad list token)
    Failed,   // no value could be read
};
const char* to_string(ParseStatus s);

struct SourceFile {
    std::string path;    // "Data\\XML\\<name>" as listed
    std::string origin;  // provider origin (MEG / loose path)
    std::unique_ptr<data::XmlDocument> doc;
    uint32_t walks = 0;  // how many list entries walked this file (>1: listed twice)
};

struct XmlAttr {
    std::string_view name, value;
};

// Interned element names, first-seen order. Lookup is ASCII case-insensitive.
struct TagTable {
    std::vector<std::string_view> name;  // first spelling seen
    TagId find(std::string_view tag) const;  // kNone if never seen
    size_t size() const { return name.size(); }

private:
    friend class ObjectTypeDatabase;
    TagId intern(std::string_view tag);
    std::unordered_map<std::string, TagId> index_;  // normalize_name -> id (lookup only)
};

// One row per type definition.
struct TypeColumns {
    std::vector<std::string_view> name;   // Name attribute, trimmed ("" = unnamed)
    std::vector<uint32_t> name_crc;       // alamo::name_crc(name)
    std::vector<TagId> element;           // root element tag: SpaceUnit, GroundInfantry, Planet, ...
    std::vector<uint32_t> file;           // index into files()
    std::vector<uint32_t> walk;           // ordinal of the list walk that created it (a file listed twice walks twice)
    std::vector<uint32_t> line;
    std::vector<FieldIndex> field_begin;  // [field_begin, field_end) = the subtree, preorder
    std::vector<FieldIndex> field_end;
    std::vector<uint32_t> attr_begin, attr_count;  // root element attributes (Name included)
    std::vector<TypeIndex> first_definition;  // == own index unless a later duplicate
    std::vector<std::string_view> variant_of;  // last direct <Variant_Of_Existing_Type> text, trimmed
    std::vector<TypeIndex> variant_parent;     // first definition of variant_of, kNone if absent/unresolved
    size_t size() const { return name.size(); }
};

// One row per element below a type element (all depths).
struct FieldColumns {
    std::vector<TagId> tag;
    std::vector<TypeIndex> type;
    std::vector<FieldIndex> parent;     // enclosing field, kNone for direct children of the type
    std::vector<uint16_t> depth;        // 1 = direct child of the type element
    std::vector<uint32_t> child_count;  // element children
    std::vector<uint32_t> line;         // file = types.file[type]
    std::vector<std::string_view> text; // trimmed character data
    std::vector<uint32_t> attr_begin, attr_count;
    std::vector<uint32_t> schema;       // field_schema() row, kNoSchema if unknown
    std::vector<ParseStatus> status;
    size_t size() const { return tag.size(); }
};

struct LoadStats {
    uint32_t list_entries = 0, files_walked = 0, files_missing = 0, xml_warnings = 0;
    uint32_t types = 0, unnamed = 0, duplicates = 0, fields = 0;
    uint32_t variants = 0, variants_unresolved = 0;
};

class ObjectTypeDatabase {
public:
    ObjectTypeDatabase() = default;
    ObjectTypeDatabase(const ObjectTypeDatabase&) = delete;
    ObjectTypeDatabase& operator=(const ObjectTypeDatabase&) = delete;
    ObjectTypeDatabase(ObjectTypeDatabase&&) = default;
    ObjectTypeDatabase& operator=(ObjectTypeDatabase&&) = default;

    static constexpr std::string_view kListPath = "Data\\XML\\GameObjectFiles.xml";

    // Returns false only when the list file is missing or unparsable. Diagnostics (missing files,
    // XML recoveries, duplicates, unresolved variants) are appended to `diagnostics` if given.
    bool load(const FileProvider& provider, std::vector<std::string>* diagnostics = nullptr);

    const std::vector<SourceFile>& files() const { return files_; }
    const TagTable& tags() const { return tags_; }
    const TypeColumns& types() const { return types_; }
    const FieldColumns& fields() const { return fields_; }
    const std::vector<XmlAttr>& attributes() const { return attrs_; }
    const LoadStats& stats() const { return stats_; }
    size_t type_count() const { return types_.size(); }

    TypeIndex find(std::string_view name) const;                 // first definition, kNone if absent
    std::vector<TypeIndex> find_all(std::string_view name) const;  // every definition, load order
    std::string_view element_name(TypeIndex t) const { return tags_.name[types_.element[t]]; }
    std::string_view tag_name(FieldIndex f) const { return tags_.name[fields_.tag[f]]; }
    const std::string& file_of(TypeIndex t) const { return files_[types_.file[t]].path; }

    // Direct children (depth 1) of type `t` named `tag`, document order.
    std::vector<FieldIndex> fields_of(TypeIndex t, std::string_view tag) const;
    // Last / first direct child named `tag`, kNone if absent. Scalar getters use the LAST
    // occurrence (provisional: the XML parser assigns each occurrence in turn).
    FieldIndex last_field(TypeIndex t, std::string_view tag) const;
    FieldIndex first_field(TypeIndex t, std::string_view tag) const;

    std::optional<std::string_view> get_text(TypeIndex t, std::string_view tag) const;
    std::optional<bool> get_bool(TypeIndex t, std::string_view tag) const;
    std::optional<int32_t> get_int(TypeIndex t, std::string_view tag) const;
    std::optional<float> get_float(TypeIndex t, std::string_view tag) const;
    // Tokens of every direct occurrence of `tag`, appended in document order (list fields such
    // as <Behavior>).
    std::vector<std::string_view> get_list(TypeIndex t, std::string_view tag) const;

private:
    void warn(std::string message);
    void walk(TypeIndex t, data::XmlNode node, FieldIndex parent, uint16_t depth, std::string_view owner);
    uint32_t add_attrs(data::XmlNode node);

    std::vector<SourceFile> files_;
    TagTable tags_;
    TypeColumns types_;
    FieldColumns fields_;
    std::vector<XmlAttr> attrs_;
    LoadStats stats_;
    std::unordered_map<uint32_t, std::vector<TypeIndex>> by_crc_;  // lookup only
    std::vector<std::string>* diag_ = nullptr;
};

// Strict typed parse of `text` as `kind` (whitespace-trimmed). Shared by the loader and tests.
ParseStatus classify_value(FieldKind kind, std::string_view text);
// Whole string is a decimal number: [+-] digits [. digits] [e [+-] digits]. *fraction is set when
// it has a '.' or an exponent.
bool is_strict_number(std::string_view s, bool* fraction = nullptr);

}  // namespace alamo::objects
