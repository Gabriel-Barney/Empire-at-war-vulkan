#include "alamo/objects/coverage.hpp"

#include <algorithm>
#include <cstdio>

#include "alamo/core/hash.hpp"
#include "alamo/data/values.hpp"
#include "alamo/objects/behaviors.hpp"
#include <unordered_map>

namespace alamo::objects {

namespace {

bool is_bool_word(std::string_view t) {
    static constexpr std::string_view kWords[] = {"yes", "no", "true", "false", "on", "off"};
    for (std::string_view w : kWords)
        if (t.size() == w.size()) {
            bool eq = true;
            for (size_t i = 0; i < t.size(); ++i) {
                char c = t[i];
                if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
                if (c != w[i]) eq = false;
            }
            if (eq) return true;
        }
    return false;
}

bool is_name_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
           c == '.' || c == '\\' || c == '/' || c == ':' || c == '\'' || c == '!' || c == '#' || c == '+';
}

bool is_name_token(std::string_view t) {
    for (char c : t)
        if (!is_name_char(c)) return false;
    return true;
}

template <class... A>
std::string format(const char* f, A... a) {
    char buf[1024];
    const int n = std::snprintf(buf, sizeof buf, f, a...);
    if (n < 0) return {};
    if (static_cast<size_t>(n) < sizeof buf) return std::string(buf, static_cast<size_t>(n));
    std::string s(static_cast<size_t>(n) + 1, '\0');
    std::snprintf(s.data(), s.size(), f, a...);
    s.resize(static_cast<size_t>(n));
    return s;
}

std::string clip(std::string_view s, size_t n) {
    std::string out;
    for (char c : s.substr(0, std::min(s.size(), n))) out += (c == '\r' || c == '\n' || c == '\t') ? ' ' : c;
    if (s.size() > n) out += "...";
    return out;
}

std::string shapes_string(const TagCoverage& c) {
    std::string out;
    for (size_t i = 0; i < c.shapes.size(); ++i) {
        if (!c.shapes[i]) continue;
        if (!out.empty()) out += ' ';
        out += format("%s:%u", to_string(static_cast<ValueShape>(i)), c.shapes[i]);
    }
    return out;
}

std::string files_string(const TagCoverage& c, const ObjectTypeDatabase& db, size_t max) {
    std::string out;
    for (size_t i = 0; i < c.files.size() && i < max; ++i) {
        if (!out.empty()) out += ", ";
        out += db.files()[c.files[i]].path.substr(9);  // drop "Data\XML\"
    }
    if (c.files.size() > max) out += format(", +%zu more", c.files.size() - max);
    return out;
}

}  // namespace

const char* to_string(ValueShape s) {
    switch (s) {
        case ValueShape::Empty: return "empty";
        case ValueShape::BoolWord: return "bool";
        case ValueShape::Int: return "int";
        case ValueShape::Float: return "float";
        case ValueShape::NumberList: return "numbers";
        case ValueShape::Name: return "name";
        case ValueShape::NameList: return "names";
        case ValueShape::Text: return "text";
        case ValueShape::Count: break;
    }
    return "?";
}

ValueShape shape_of(std::string_view raw) {
    const std::string_view t = data::trim(raw);
    if (t.empty()) return ValueShape::Empty;
    const std::vector<std::string_view> toks = data::split_list(t);
    if (toks.size() == 1) {
        bool frac = false;
        if (is_strict_number(t, &frac)) return frac ? ValueShape::Float : ValueShape::Int;
        if (is_bool_word(t)) return ValueShape::BoolWord;
        return is_name_token(t) ? ValueShape::Name : ValueShape::Text;
    }
    bool all_numbers = true, all_names = true;
    for (std::string_view tok : toks) {
        if (!is_strict_number(tok)) all_numbers = false;
        if (!is_name_token(tok)) all_names = false;
    }
    if (all_numbers) return ValueShape::NumberList;
    return all_names ? ValueShape::NameList : ValueShape::Text;
}

CoverageReport build_coverage(const ObjectTypeDatabase& db, size_t max_failure_examples) {
    CoverageReport r;
    r.stats = db.stats();
    r.files = db.files().size();
    const TagTable& tags = db.tags();
    const TypeColumns& ty = db.types();
    const FieldColumns& fc = db.fields();

    std::vector<uint32_t> element_count(tags.size(), 0), element_order;
    for (TypeIndex t = 0; t < ty.size(); ++t)
        if (element_count[ty.element[t]]++ == 0) element_order.push_back(ty.element[t]);
    for (TagId e : element_order) r.elements.emplace_back(tags.name[e], element_count[e]);

    r.tags.resize(tags.size());
    std::vector<TypeIndex> last_type(tags.size(), kNone);
    for (TagId i = 0; i < tags.size(); ++i) {
        r.tags[i].tag = i;
        r.tags[i].name = tags.name[i];
        r.tags[i].ambiguous = field_schema_ambiguous(tags.name[i]);
        r.tags[i].sub_object = is_sub_object_element(tags.name[i]);
    }
    std::vector<uint8_t> is_field_tag(tags.size(), 0);
    for (FieldIndex f = 0; f < fc.size(); ++f) {
        TagCoverage& c = r.tags[fc.tag[f]];
        is_field_tag[fc.tag[f]] = 1;
        ++c.occurrences;
        (fc.depth[f] == 1 ? c.top_level : c.nested)++;
        if (fc.child_count[f]) ++c.with_children;
        const TypeIndex t = fc.type[f];
        if (last_type[fc.tag[f]] != t) {
            last_type[fc.tag[f]] = t;
            ++c.types;
        }
        const uint32_t file = ty.file[t];
        if (std::find(c.files.begin(), c.files.end(), file) == c.files.end()) c.files.push_back(file);
        if (c.schema == kNoSchema) c.schema = fc.schema[f];
        const ParseStatus st = fc.status[f];
        ++c.status[static_cast<size_t>(st)];
        if (fc.child_count[f] == 0) ++c.shapes[static_cast<size_t>(shape_of(fc.text[f]))];
        if ((st == ParseStatus::Failed || st == ParseStatus::Lossy) && c.failures.size() < max_failure_examples)
            c.failures.push_back(db.files()[file].path + "(" + std::to_string(fc.line[f]) + "): <" +
                                 std::string(tags.name[fc.tag[f]]) + "> '" + clip(fc.text[f], 60) + "' -> " +
                                 to_string(st));
        if (fc.schema[f] != kNoSchema) ++r.occ_known;
        else if (c.sub_object) ++r.occ_sub_object;
        else ++r.occ_unknown;
        switch (st) {
            case ParseStatus::Untyped: break;
            case ParseStatus::Ok: ++r.occ_typed, ++r.occ_ok; break;
            case ParseStatus::Empty: ++r.occ_typed, ++r.occ_empty; break;
            case ParseStatus::Lossy: ++r.occ_typed, ++r.occ_lossy; break;
            case ParseStatus::Failed: ++r.occ_typed, ++r.occ_failed; break;
        }
    }
    // Keep only tags that occur as fields (type element names are reported in `elements`).
    std::vector<TagCoverage> kept;
    for (TagCoverage& c : r.tags)
        if (is_field_tag[c.tag]) {
            if (c.known()) ++r.tags_known;
            else if (c.sub_object) ++r.tags_sub_object;
            else ++r.tags_unknown;
            kept.push_back(std::move(c));
        }
    r.tags = std::move(kept);

    // Behavior tokens.
    std::unordered_map<std::string, uint32_t> beh_index;  // normalize_name -> behaviors index (lookup only)
    std::vector<TypeIndex> beh_last_type;
    for (FieldIndex f = 0; f < fc.size(); ++f) {
        const std::string_view tag = tags.name[fc.tag[f]];
        if (fc.depth[f] != 1 || !is_behavior_list_tag(tag)) continue;
        uint32_t bit = 0;
        while (bit < r.behavior_tags.size() && !iequals(r.behavior_tags[bit], tag)) ++bit;
        if (bit == r.behavior_tags.size()) r.behavior_tags.push_back(tag);
        for (std::string_view tok : data::split_list(fc.text[f])) {
            const auto [it, inserted] = beh_index.emplace(normalize_name(tok), static_cast<uint32_t>(r.behaviors.size()));
            if (inserted) {
                BehaviorCoverage b;
                b.name = tok;
                b.id = behavior_type_id(tok);
                r.behaviors.push_back(b);
                beh_last_type.push_back(kNone);
            }
            BehaviorCoverage& b = r.behaviors[it->second];
            ++b.occurrences;
            b.tag_mask |= 1u << bit;
            if (beh_last_type[it->second] != fc.type[f]) {
                beh_last_type[it->second] = fc.type[f];
                ++b.types;
            }
        }
    }
    std::vector<uint8_t> id_seen(kBehaviorTypeCount, 0);
    for (const BehaviorCoverage& b : r.behaviors) {
        if (b.id == kNoBehavior) {
            ++r.behaviors_unknown;
            continue;
        }
        ++r.behaviors_known;
        if (!id_seen[b.id]++) ++r.behavior_ids_used;
    }

    const auto rows = field_schema();
    r.schema_rows = static_cast<uint32_t>(rows.size());
    for (const FieldSchema& row : rows) {
        const TagId id = tags.find(row.tag);
        if (id != kNone && is_field_tag[id]) ++r.schema_rows_seen;
    }
    return r;
}

std::string format_coverage(const CoverageReport& r, const ObjectTypeDatabase& db, std::string_view title) {
    std::string o;
    const LoadStats& s = r.stats;
    o += format("# GameObjectType field coverage: %.*s\n\n", static_cast<int>(title.size()), title.data());
    o += format("list entries %u, files walked %u (distinct %zu), missing %u, XML recoveries %u\n", s.list_entries,
                s.files_walked, r.files, s.files_missing, s.xml_warnings);
    o += format("type definitions %u (unnamed %u, duplicate names %u), Variant_Of_Existing_Type %u (unresolved %u)\n",
                s.types, s.unnamed, s.duplicates, s.variants, s.variants_unresolved);
    o += format("field elements %u; distinct tags %zu: known field %u, sub-object element %u, unknown %u\n", s.fields,
                r.tags.size(), r.tags_known, r.tags_sub_object, r.tags_unknown);
    o += format("occurrences: known field %llu, sub-object element %llu, unknown %llu; typed %llu: ok %llu, empty %llu, lossy %llu, failed %llu\n",
                static_cast<unsigned long long>(r.occ_known), static_cast<unsigned long long>(r.occ_sub_object),
                static_cast<unsigned long long>(r.occ_unknown), static_cast<unsigned long long>(r.occ_typed), static_cast<unsigned long long>(r.occ_ok),
                static_cast<unsigned long long>(r.occ_empty), static_cast<unsigned long long>(r.occ_lossy),
                static_cast<unsigned long long>(r.occ_failed));
    o += format("schema rows (docs/re/xml_field_tables.md) whose tag occurs: %u / %u\n\n", r.schema_rows_seen,
                r.schema_rows);

    o += "## Type elements\n\n";
    for (const auto& [name, n] : r.elements)
        o += format("  %-40.*s %6u\n", static_cast<int>(name.size()), name.data(), n);

    // Deterministic output orders: by occurrences desc, then name, then TagId.
    std::vector<const TagCoverage*> sorted;
    for (const TagCoverage& c : r.tags) sorted.push_back(&c);
    std::sort(sorted.begin(), sorted.end(), [](const TagCoverage* a, const TagCoverage* b) {
        if (a->occurrences != b->occurrences) return a->occurrences > b->occurrences;
        if (a->name != b->name) return a->name < b->name;
        return a->tag < b->tag;
    });

    o += format("\n## Unknown tags (%u): no row in xml_field_tables.md; kept as raw text\n\n", r.tags_unknown);
    o += "  tag                                       occ   top nested types  shapes | files\n";
    for (const TagCoverage* c : sorted) {
        if (!c->unknown()) continue;
        o += format("  %-40.*s %5u %5u %6u %5u  %s | %s\n", static_cast<int>(c->name.size()), c->name.data(),
                    c->occurrences, c->top_level, c->nested, c->types, shapes_string(*c).c_str(),
                    files_string(*c, db, 3).c_str());
    }

    o += format("\n## Sub-object elements (%u): ability / action containers, children are the owner's fields\n\n",
                r.tags_sub_object);
    for (const TagCoverage* c : sorted)
        if (!c->known() && c->sub_object)
            o += format("  %-40.*s %5u %5u %5zu\n", static_cast<int>(c->name.size()), c->name.data(), c->occurrences,
                        c->types, c->files.size());

    o += format("\n## Behavior names (%zu distinct): known %u, unknown %u (ignored by the engine); type ids used %u / %u\n",
                r.behaviors.size(), r.behaviors_known, r.behaviors_unknown, r.behavior_ids_used, kBehaviorTypeCount - 1);
    o += "   (XML name -> runtime behaviour type id: docs/re/OBJECT_RUNTIME.md section 4.3)\n\n";
    {
        std::vector<const BehaviorCoverage*> bs;
        for (const BehaviorCoverage& b : r.behaviors) bs.push_back(&b);
        std::sort(bs.begin(), bs.end(), [](const BehaviorCoverage* a, const BehaviorCoverage* b) {
            if (a->occurrences != b->occurrences) return a->occurrences > b->occurrences;
            return a->name < b->name;
        });
        for (const BehaviorCoverage* b : bs) {
            std::string in;
            for (size_t i = 0; i < r.behavior_tags.size(); ++i)
                if (b->tag_mask & (1u << i)) in += (in.empty() ? "" : ",") + std::string(r.behavior_tags[i]);
            const std::string id = b->id == kNoBehavior ? std::string("UNKNOWN") : format("0x%02x", b->id);
            o += format("  %-40.*s %5u %5u  %-8s %s\n", static_cast<int>(b->name.size()), b->name.data(), b->occurrences,
                        b->types, id.c_str(), in.c_str());
        }
        std::vector<uint8_t> seen(kBehaviorTypeCount, 0);
        for (const BehaviorCoverage& b : r.behaviors) seen[b.id] = 1;
        o += "\n  behaviour type ids with an XML name but unused here:";
        for (uint32_t i = 1; i < kBehaviorTypeCount; ++i)
            if (!seen[i] && !behavior_type_name(static_cast<BehaviorTypeId>(i)).empty())
                o += " " + std::string(behavior_type_name(static_cast<BehaviorTypeId>(i)));
        o += "\n";
    }
    o += "\n## Parse problems (typed known fields with failed or lossy values)\n\n";
    for (const TagCoverage* c : sorted) {
        const uint32_t bad = c->status[static_cast<size_t>(ParseStatus::Failed)] +
                             c->status[static_cast<size_t>(ParseStatus::Lossy)];
        if (!bad) continue;
        const FieldSchema& row = field_schema()[c->schema];
        o += format("  %-40.*s %s(0x%x, %.*s)%s occ %u: ok %u empty %u lossy %u failed %u; shapes %s\n",
                    static_cast<int>(c->name.size()), c->name.data(), to_string(row.kind()), row.code,
                    static_cast<int>(row.owner.size()), row.owner.data(), c->ambiguous ? " ambiguous" : "",
                    c->occurrences, c->status[1], c->status[2], c->status[3], c->status[4], shapes_string(*c).c_str());
        for (const std::string& ex : c->failures) o += "      " + ex + "\n";
    }

    o += "\n## All tags\n\n";
    o += "  tag                                       known kind    code owner                      occ types files  status(ok/empty/lossy/failed)  shapes\n";
    for (const TagCoverage* c : sorted) {
        std::string kind = "-", code = "-", owner = "-";
        if (c->known()) {
            const FieldSchema& row = field_schema()[c->schema];
            kind = to_string(row.kind());
            code = format("0x%x", row.code);
            owner = std::string(row.owner);
            if (c->ambiguous) owner += "*";
        }
        o += format("  %-40.*s %-5s %-7s %-4s %-26s %5u %5u %5zu  %u/%u/%u/%u  %s\n", static_cast<int>(c->name.size()),
                    c->name.data(), c->known() ? "yes" : (c->sub_object ? "sub" : "NO"), kind.c_str(), code.c_str(), owner.c_str(),
                    c->occurrences, c->types, c->files.size(), c->status[1], c->status[2], c->status[3], c->status[4],
                    shapes_string(*c).c_str());
    }
    o += "\n(* = schema rows for this tag disagree on the parser code; the row shown is the one used)\n";
    return o;
}

}  // namespace alamo::objects
