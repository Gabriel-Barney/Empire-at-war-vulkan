#include "alamo/objects/object_types.hpp"

#include <unordered_map>

#include "alamo/core/hash.hpp"
#include "alamo/core/log.hpp"
#include "alamo/data/values.hpp"

namespace alamo::objects {

namespace {

constexpr std::string_view kXmlDir = "Data\\XML\\";
constexpr std::string_view kVariantTag = "Variant_Of_Existing_Type";

bool is_digit(char c) { return c >= '0' && c <= '9'; }

// Strict decimal number: [+-] digits [. digits] [e [+-] digits], at least one mantissa digit.
// Sets *fraction when a '.' or exponent is present.
}  // namespace

bool is_strict_number(std::string_view s, bool* fraction) {
    size_t i = 0;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
    size_t digits = 0;
    while (i < s.size() && is_digit(s[i])) ++i, ++digits;
    bool frac = false;
    if (i < s.size() && s[i] == '.') {
        ++i;
        frac = true;
        while (i < s.size() && is_digit(s[i])) ++i, ++digits;
    }
    if (digits == 0) return false;
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
        size_t j = i + 1;
        if (j < s.size() && (s[j] == '+' || s[j] == '-')) ++j;
        size_t ed = 0;
        while (j < s.size() && is_digit(s[j])) ++j, ++ed;
        if (ed > 0) {
            i = j;
            frac = true;
        }
    }
    if (fraction) *fraction = frac;
    return i == s.size();
}

namespace {

std::string where(const SourceFile& f, uint32_t line) { return f.path + "(" + std::to_string(line) + ")"; }

}  // namespace

const char* to_string(ParseStatus s) {
    switch (s) {
        case ParseStatus::Untyped: return "untyped";
        case ParseStatus::Ok: return "ok";
        case ParseStatus::Empty: return "empty";
        case ParseStatus::Lossy: return "lossy";
        case ParseStatus::Failed: return "failed";
    }
    return "?";
}

ParseStatus classify_value(FieldKind kind, std::string_view raw) {
    const std::string_view t = data::trim(raw);
    if (kind == FieldKind::Raw) return ParseStatus::Untyped;
    if (t.empty()) return ParseStatus::Empty;
    switch (kind) {
        case FieldKind::Bool: return data::parse_bool(t) ? ParseStatus::Ok : ParseStatus::Failed;
        case FieldKind::Int: {
            if (!data::parse_int(t)) return ParseStatus::Failed;
            bool frac = false;
            return is_strict_number(t, &frac) && !frac ? ParseStatus::Ok : ParseStatus::Lossy;
        }
        case FieldKind::Float:
            if (!data::parse_float(t)) return ParseStatus::Failed;
            return is_strict_number(t) ? ParseStatus::Ok : ParseStatus::Lossy;
        case FieldKind::Name: return ParseStatus::Ok;
        case FieldKind::NumberList: {
            size_t good = 0, bad = 0;
            for (std::string_view tok : data::split_list(t)) (is_strict_number(tok) ? good : bad)++;
            if (good == 0) return ParseStatus::Failed;
            return bad == 0 ? ParseStatus::Ok : ParseStatus::Lossy;
        }
        case FieldKind::Raw: break;
    }
    return ParseStatus::Untyped;
}

TagId TagTable::find(std::string_view tag) const {
    const auto it = index_.find(normalize_name(tag));
    return it == index_.end() ? kNone : it->second;
}

TagId TagTable::intern(std::string_view tag) {
    const auto [it, inserted] = index_.emplace(normalize_name(tag), static_cast<TagId>(name.size()));
    if (inserted) name.push_back(tag);
    return it->second;
}

void ObjectTypeDatabase::warn(std::string message) {
    log::warnf("objects", "%s", message.c_str());
    if (diag_) diag_->push_back(std::move(message));
}

uint32_t ObjectTypeDatabase::add_attrs(data::XmlNode node) {
    const uint32_t begin = static_cast<uint32_t>(attrs_.size());
    for (const data::XmlAttribute* a = node.attributes_begin(); a != node.attributes_end(); ++a)
        attrs_.push_back({a->name, a->value});
    return begin;
}

void ObjectTypeDatabase::walk(TypeIndex t, data::XmlNode node, FieldIndex parent, uint16_t depth,
                              std::string_view owner) {
    for (data::XmlNode c = node.first_child(); c; c = c.next_sibling()) {
        const FieldIndex f = static_cast<FieldIndex>(fields_.size());
        const std::string_view text = data::trim(c.text());
        const uint32_t schema = find_field_schema(c.name(), owner);
        fields_.tag.push_back(tags_.intern(c.name()));
        fields_.type.push_back(t);
        fields_.parent.push_back(parent);
        fields_.depth.push_back(depth);
        fields_.child_count.push_back(static_cast<uint32_t>(c.child_count()));
        fields_.line.push_back(c.line());
        fields_.text.push_back(text);
        fields_.attr_begin.push_back(add_attrs(c));
        fields_.attr_count.push_back(static_cast<uint32_t>(c.attribute_count()));
        fields_.schema.push_back(schema);
        // An element with children is a container: its own text is not a value.
        const FieldKind kind = (schema == kNoSchema || c.child_count() > 0) ? FieldKind::Raw : field_schema()[schema].kind();
        fields_.status.push_back(classify_value(kind, text));
        if (c.child_count() > 0) walk(t, c, f, static_cast<uint16_t>(depth + 1), c.name());
    }
}

bool ObjectTypeDatabase::load(const FileProvider& provider, std::vector<std::string>* diagnostics) {
    diag_ = diagnostics;
    std::vector<uint8_t> bytes;
    if (!provider.read(kListPath, bytes)) {
        warn("error: unable to open '" + std::string(kListPath) + "'");
        diag_ = nullptr;
        return false;
    }
    const data::XmlDocument list = data::XmlDocument::parse(bytes, std::string(kListPath));
    for (const data::XmlDiagnostic& d : list.diagnostics()) warn(d.to_string());
    if (!list.ok()) {
        diag_ = nullptr;
        return false;
    }

    std::unordered_map<std::string, uint32_t> file_by_path;  // normalize_path -> files_ index (lookup only)
    for (data::XmlNode entry : list.root().children("File")) {
        const std::string_view listed = data::trim(entry.text());
        if (listed.empty()) continue;
        ++stats_.list_entries;
        std::string path(kXmlDir);
        path += listed;
        const std::string key = normalize_path(path);
        uint32_t fi;
        if (const auto it = file_by_path.find(key); it != file_by_path.end()) {
            fi = it->second;
            warn(std::string(kListPath) + ": '" + std::string(listed) + "' listed twice; walked twice");
        } else {
            bytes.clear();
            if (!provider.read(path, bytes)) {
                ++stats_.files_missing;
                warn("unable to open XML file '" + path + "' (skipped)");
                continue;
            }
            SourceFile sf;
            sf.path = path;
            sf.origin = provider.origin(path);
            sf.doc = std::make_unique<data::XmlDocument>(data::XmlDocument::parse(bytes, path));
            for (const data::XmlDiagnostic& d : sf.doc->diagnostics()) {
                ++stats_.xml_warnings;
                warn(d.to_string());
            }
            fi = static_cast<uint32_t>(files_.size());
            file_by_path.emplace(key, fi);
            files_.push_back(std::move(sf));
        }
        SourceFile& sf = files_[fi];
        if (!sf.doc->ok()) continue;
        ++sf.walks;
        ++stats_.files_walked;

        for (data::XmlNode e = sf.doc->root().first_child(); e; e = e.next_sibling()) {
            const TypeIndex t = static_cast<TypeIndex>(types_.size());
            const std::string_view name = data::trim(e.attribute("Name"));
            const uint32_t crc = name_crc(name);
            TypeIndex first = t;
            if (name.empty()) {
                ++stats_.unnamed;
                warn(where(sf, e.line()) + ": <" + std::string(e.name()) + "> without a Name (kept, not findable)");
            } else {
                auto& bucket = by_crc_[crc];
                for (TypeIndex k : bucket)
                    if (iequals(types_.name[k], name)) {
                        first = k;
                        break;
                    }
                if (first != t) {
                    ++stats_.duplicates;
                    warn(where(sf, e.line()) + ": duplicate type name '" + std::string(name) + "' (first definition " +
                         where(files_[types_.file[first]], types_.line[first]) + " wins lookups)");
                }
                bucket.push_back(t);
            }
            types_.name.push_back(name);
            types_.name_crc.push_back(crc);
            types_.element.push_back(tags_.intern(e.name()));
            types_.file.push_back(fi);
            types_.walk.push_back(stats_.files_walked - 1);
            types_.line.push_back(e.line());
            types_.attr_begin.push_back(add_attrs(e));
            types_.attr_count.push_back(static_cast<uint32_t>(e.attribute_count()));
            types_.first_definition.push_back(first);
            types_.field_begin.push_back(static_cast<FieldIndex>(fields_.size()));
            walk(t, e, kNone, 1, e.name());
            types_.field_end.push_back(static_cast<FieldIndex>(fields_.size()));
            const FieldIndex v = last_field(t, kVariantTag);
            types_.variant_of.push_back(v == kNone ? std::string_view{} : fields_.text[v]);
            types_.variant_parent.push_back(kNone);
        }
    }

    // Variant links resolve against the whole set (forward references allowed); not merged yet.
    for (TypeIndex t = 0; t < types_.size(); ++t) {
        if (types_.variant_of[t].empty()) continue;
        ++stats_.variants;
        const TypeIndex p = find(types_.variant_of[t]);
        types_.variant_parent[t] = p;
        if (p == kNone) {
            ++stats_.variants_unresolved;
            warn(where(files_[types_.file[t]], types_.line[t]) + ": '" + std::string(types_.name[t]) +
                 "' is a variant of unknown type '" + std::string(types_.variant_of[t]) + "'");
        }
    }
    stats_.types = static_cast<uint32_t>(types_.size());
    stats_.fields = static_cast<uint32_t>(fields_.size());
    diag_ = nullptr;
    return true;
}

TypeIndex ObjectTypeDatabase::find(std::string_view name) const {
    const std::string_view n = data::trim(name);
    if (n.empty()) return kNone;
    const auto it = by_crc_.find(name_crc(n));
    if (it == by_crc_.end()) return kNone;
    for (TypeIndex t : it->second)
        if (iequals(types_.name[t], n)) return t;
    return kNone;
}

std::vector<TypeIndex> ObjectTypeDatabase::find_all(std::string_view name) const {
    std::vector<TypeIndex> out;
    const std::string_view n = data::trim(name);
    if (n.empty()) return out;
    const auto it = by_crc_.find(name_crc(n));
    if (it == by_crc_.end()) return out;
    for (TypeIndex t : it->second)
        if (iequals(types_.name[t], n)) out.push_back(t);
    return out;
}

std::vector<FieldIndex> ObjectTypeDatabase::fields_of(TypeIndex t, std::string_view tag) const {
    std::vector<FieldIndex> out;
    const TagId id = tags_.find(tag);
    if (id == kNone || t >= types_.size()) return out;
    for (FieldIndex f = types_.field_begin[t]; f < types_.field_end[t]; ++f)
        if (fields_.depth[f] == 1 && fields_.tag[f] == id) out.push_back(f);
    return out;
}

FieldIndex ObjectTypeDatabase::last_field(TypeIndex t, std::string_view tag) const {
    const TagId id = tags_.find(tag);
    if (id == kNone || t >= types_.field_end.size()) return kNone;
    for (FieldIndex f = types_.field_end[t]; f-- > types_.field_begin[t];)
        if (fields_.depth[f] == 1 && fields_.tag[f] == id) return f;
    return kNone;
}

FieldIndex ObjectTypeDatabase::first_field(TypeIndex t, std::string_view tag) const {
    const TagId id = tags_.find(tag);
    if (id == kNone || t >= types_.field_end.size()) return kNone;
    for (FieldIndex f = types_.field_begin[t]; f < types_.field_end[t]; ++f)
        if (fields_.depth[f] == 1 && fields_.tag[f] == id) return f;
    return kNone;
}

std::optional<std::string_view> ObjectTypeDatabase::get_text(TypeIndex t, std::string_view tag) const {
    const FieldIndex f = last_field(t, tag);
    if (f == kNone) return std::nullopt;
    return fields_.text[f];
}

std::optional<bool> ObjectTypeDatabase::get_bool(TypeIndex t, std::string_view tag) const {
    const auto v = get_text(t, tag);
    return v ? data::parse_bool(*v) : std::nullopt;
}

std::optional<int32_t> ObjectTypeDatabase::get_int(TypeIndex t, std::string_view tag) const {
    const auto v = get_text(t, tag);
    return v ? data::parse_int(*v) : std::nullopt;
}

std::optional<float> ObjectTypeDatabase::get_float(TypeIndex t, std::string_view tag) const {
    const auto v = get_text(t, tag);
    return v ? data::parse_float(*v) : std::nullopt;
}

std::vector<std::string_view> ObjectTypeDatabase::get_list(TypeIndex t, std::string_view tag) const {
    std::vector<std::string_view> out;
    for (FieldIndex f : fields_of(t, tag))
        for (std::string_view tok : data::split_list(fields_.text[f])) out.push_back(tok);
    return out;
}

}  // namespace alamo::objects
