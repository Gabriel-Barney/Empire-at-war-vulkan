#include "alamo/tactical/combat/tables.hpp"

#include <cstdlib>
#include <iterator>
#include <string>

#include "alamo/core/hash.hpp"
#include "alamo/data/game_database.hpp"
#include "alamo/data/xml.hpp"
#include "alamo/objects/enums.hpp"

namespace alamo::tactical::combat {

namespace {

bool is_sep(char c) { return c == ',' || c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// Splits on commas and whitespace (collapsed); empty tokens dropped.
std::vector<std::string_view> tokens(std::string_view s) {
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

// Locale-free decimal float (digits, optional sign / point / exponent). false if not a number.
bool parse_float(std::string_view s, float& out) {
    if (s.empty()) return false;
    const std::string tmp(s);
    char* end = nullptr;
    const double v = std::strtod(tmp.c_str(), &end);
    if (end == tmp.c_str() || *end != '\0') return false;
    out = static_cast<float>(v);
    return true;
}

uint16_t find_name(const std::vector<std::string>& names, std::string_view name) {
    const std::string up = normalize_name(name);
    for (size_t i = 0; i < names.size(); ++i)
        if (names[i] == up) return static_cast<uint16_t>(i);
    return kNoTypeId;
}

}  // namespace

void DamageArmorTable::grow() {
    const auto na = static_cast<uint32_t>(armor_names_.size());
    const auto nd = static_cast<uint32_t>(damage_names_.size());
    if (na <= stride_) {  // only rows were added
        cells_.resize(static_cast<size_t>(nd) * stride_, -1.0f);
        return;
    }
    const uint32_t stride = na < 16 ? 16 : na * 2;
    const uint32_t old_rows = stride_ ? static_cast<uint32_t>(cells_.size() / stride_) : 0;
    std::vector<float> cells(static_cast<size_t>(nd) * stride, -1.0f);
    for (uint32_t d = 0; d < old_rows && d < nd; ++d)
        for (uint32_t a = 0; a < stride_; ++a)
            cells[static_cast<size_t>(d) * stride + a] = cells_[static_cast<size_t>(d) * stride_ + a];
    cells_ = std::move(cells);
    stride_ = stride;
}

uint16_t DamageArmorTable::add_damage_type(std::string_view name) {
    const uint16_t i = find_name(damage_names_, name);
    if (i != kNoTypeId) return i;
    damage_names_.push_back(normalize_name(name));
    grow();
    return static_cast<uint16_t>(damage_names_.size() - 1);
}

uint16_t DamageArmorTable::add_armor_type(std::string_view name) {
    const uint16_t i = find_name(armor_names_, name);
    if (i != kNoTypeId) return i;
    armor_names_.push_back(normalize_name(name));
    grow();
    return static_cast<uint16_t>(armor_names_.size() - 1);
}

uint16_t DamageArmorTable::damage_index(std::string_view name) const { return find_name(damage_names_, name); }
uint16_t DamageArmorTable::armor_index(std::string_view name) const { return find_name(armor_names_, name); }

void DamageArmorTable::set(uint16_t damage, uint16_t armor, float factor_value) {
    if (damage >= damage_names_.size() || armor >= armor_names_.size()) return;
    cells_[static_cast<size_t>(damage) * stride_ + armor] = factor_value;
}

float DamageArmorTable::factor(uint16_t damage, uint16_t armor) const {
    if (damage >= damage_names_.size() || armor >= armor_names_.size()) return 1.0f;
    const float f = cells_[static_cast<size_t>(damage) * stride_ + armor];
    return f < 0.0f ? 1.0f : f;
}

DamageArmorTable DamageArmorTable::from_constants(const data::GameConstants& gc, std::vector<std::string>* diagnostics) {
    DamageArmorTable t;
    for (const auto& [tag, text] : gc.fields) {
        if (iequals(tag, "Damage_Types")) {
            for (std::string_view n : tokens(text)) t.add_damage_type(n);
        } else if (iequals(tag, "Armor_Types")) {
            for (std::string_view n : tokens(text)) t.add_armor_type(n);
        }
    }
    for (const auto& [tag, text] : gc.fields) {
        if (!iequals(tag, "Damage_To_Armor_Mod")) continue;
        const auto tk = tokens(text);
        float f = 0.0f;
        if (tk.size() != 3 || !parse_float(tk[2], f)) {
            if (diagnostics) diagnostics->push_back("Damage_To_Armor_Mod: malformed '" + std::string(text) + "'");
            continue;
        }
        t.set(t.add_damage_type(tk[0]), t.add_armor_type(tk[1]), f);
    }
    return t;
}

float priority_score(const PrioritySet& set, int32_t target_type, uint64_t target_categories, uint64_t target_properties) {
    for (const PriorityEntry& e : set.attack)
        if (e.kind == PriorityEntry::Kind::Type && e.type == target_type) return e.weight;
    float v = 1e18f;
    for (const PriorityEntry& e : set.attack)
        if (e.kind == PriorityEntry::Kind::Category && (e.mask & target_categories) != 0 && e.weight < v) v = e.weight;
    bool property_hit = false;
    for (const PriorityEntry& e : set.attack) {
        if (e.kind != PriorityEntry::Kind::Property || (e.mask & target_properties) == 0) continue;
        if (!property_hit) {
            v = e.weight;  // first property hit overwrites
            property_hit = true;
        } else if (e.weight < v) {
            v = e.weight;
        }
    }
    for (int32_t t : set.unit_exclusions)
        if (t == target_type) return -1.0f;
    if (!property_hit && (set.category_exclusions & target_categories) != 0) return -1.0f;
    if ((set.property_exclusions & target_properties) != 0) return -1.0f;
    return v;
}

float hardpoint_rank(const PrioritySet& set, int32_t hardpoint_type) {
    for (size_t i = 0; i < set.hardpoint_priorities.size(); ++i)
        if (set.hardpoint_priorities[i] == hardpoint_type) return static_cast<float>(i + 1);
    for (int32_t c : set.hardpoint_exclusions)
        if (c == hardpoint_type) return -1.0f;
    return 1e18f;
}

int32_t hardpoint_type_value(std::string_view name) {
    static constexpr std::string_view kNames[] = {
        "ENGINE",          "SHIELD_GENERATOR",     "GRAVITY_WELL",  "FIGHTER_BAY",   "TRACTOR_BEAM",
        "WEAPON_LASER",    "WEAPON_MISSILE",       "WEAPON_TORPEDO", "WEAPON_ION_CANNON", "WEAPON_MASS_DRIVER",
        "WEAPON_SPECIAL",  "ENABLE_SPECIAL_ABILITY", "DUMMY_ART",
    };
    constexpr std::string_view kPrefix = "HARD_POINT_";
    if (name.size() > kPrefix.size() && iequals(name.substr(0, kPrefix.size()), kPrefix)) name.remove_prefix(kPrefix.size());
    for (size_t i = 0; i < std::size(kNames); ++i)
        if (iequals(name, kNames[i])) return static_cast<int32_t>(i);
    return -1;
}

std::vector<PrioritySet> load_priority_sets(const data::XmlNode& root, const objects::EnumRegistry& enums,
                                            const TypeResolver& resolve_type, std::vector<std::string>* diagnostics) {
    std::vector<PrioritySet> out;
    const objects::EnumConverter& cats = enums.get(objects::EnumId::GameObjectCategory);
    const objects::EnumConverter& props = enums.get(objects::EnumId::GameObjectProperties);
    auto warn = [&](std::string m) {
        if (diagnostics) diagnostics->push_back(std::move(m));
    };
    auto mask_of = [&](const objects::EnumConverter& conv, std::string_view text, const std::string& where) {
        uint64_t m = 0;
        for (std::string_view n : tokens(text)) {
            const auto v = conv.lookup(n);
            if (v) {
                m |= static_cast<uint64_t>(*v);
            } else {
                warn(where + ": unknown name '" + std::string(n) + "'");
            }
        }
        return m;
    };
    for (data::XmlNode s : root.children("Priority_Set")) {
        PrioritySet ps;
        ps.name = std::string(s.attribute("Name"));
        for (data::XmlNode c : s.children()) {
            const std::string_view tag = c.name();
            const std::string_view text = c.text_trimmed();
            const std::string where = ps.name + "/" + std::string(tag);
            if (iequals(tag, "Attack_Priorities")) {
                const auto tk = tokens(text);
                for (size_t i = 0; i + 1 < tk.size(); i += 2) {
                    PriorityEntry e;
                    if (!parse_float(tk[i + 1], e.weight)) {
                        warn(where + ": bad weight '" + std::string(tk[i + 1]) + "'");
                        continue;
                    }
                    const int32_t type = resolve_type ? resolve_type(tk[i]) : -1;
                    if (type >= 0) {
                        e.kind = PriorityEntry::Kind::Type;
                        e.type = type;
                    } else if (const auto cv = cats.lookup(tk[i])) {
                        e.kind = PriorityEntry::Kind::Category;
                        e.mask = static_cast<uint64_t>(*cv);
                    } else if (const auto pv = props.lookup(tk[i])) {
                        e.kind = PriorityEntry::Kind::Property;
                        e.mask = static_cast<uint64_t>(*pv);
                    } else {
                        warn(where + ": unknown name '" + std::string(tk[i]) + "'");
                        continue;
                    }
                    ps.attack.push_back(e);
                }
                if (tk.size() % 2 != 0) warn(where + ": odd token count");
            } else if (iequals(tag, "Unit_Exclusions")) {
                for (std::string_view n : tokens(text)) {
                    const int32_t type = resolve_type ? resolve_type(n) : -1;
                    if (type >= 0) {
                        ps.unit_exclusions.push_back(type);
                    } else {
                        warn(where + ": unknown type '" + std::string(n) + "'");
                    }
                }
            } else if (iequals(tag, "Category_Exclusions")) {
                ps.category_exclusions |= mask_of(cats, text, where);
            } else if (iequals(tag, "Property_Exclusions")) {
                ps.property_exclusions |= mask_of(props, text, where);
            } else if (iequals(tag, "Hard_Point_Priorities") || iequals(tag, "Hard_Point_Exclusions")) {
                std::vector<int32_t>& list = iequals(tag, "Hard_Point_Priorities") ? ps.hardpoint_priorities : ps.hardpoint_exclusions;
                for (std::string_view n : tokens(text)) {
                    const int32_t v = hardpoint_type_value(n);
                    if (v >= 0) {
                        list.push_back(v);
                    } else {
                        warn(where + ": unknown hard point type '" + std::string(n) + "'");
                    }
                }
            }
        }
        out.push_back(std::move(ps));
    }
    return out;
}

}  // namespace alamo::tactical::combat
