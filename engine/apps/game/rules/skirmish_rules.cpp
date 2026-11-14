// q6 t03: skirmish rules shared by alamo_game and alamo_headless (skirmish_rules.hpp).
#include "alamo/game/skirmish_rules.hpp"

#include <algorithm>
#include <cstddef>

#include "alamo/core/ascii.hpp"
#include "alamo/core/file_provider.hpp"
#include "alamo/galactic/galactic_rules.hpp"
#include "alamo/objects/game_object_types.hpp"
#ifdef ALAMO_GAME_RULES_HAVE_AI
#include "alamo/ai/ai_definitions.hpp"
#endif

namespace alamo::game {

namespace {

// Comma / whitespace separated names (the list/string field form).
std::vector<std::string> split_names(std::string_view v) {
    std::vector<std::string> out;
    const auto sep = [](char ch) { return ch == ',' || ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n'; };
    size_t at = 0;
    while (at < v.size()) {
        while (at < v.size() && sep(v[at])) ++at;
        size_t end = at;
        while (end < v.size() && !sep(v[end])) ++end;
        if (end > at) out.emplace_back(v.substr(at, end - at));
        at = end;
    }
    return out;
}

bool hop_capable(const galactic::TypeInfo& t) {
    return (t.kind == galactic::UnitKind::SpaceUnit || t.kind == galactic::UnitKind::Squadron) && t.hyperspace && t.hyperspace_speed > 0.0f;
}

// data_fallback_forces' candidates, ordered by (build cost, name); `min_capacity` > 0 keeps ships that hold that many squadrons.
std::vector<uint32_t> buildable_units(const galactic::GalacticRules& rules, const objects::GameObjectTypes& types, uint32_t faction,
                                      bool land, int32_t min_capacity = 0) {
    std::vector<uint32_t> c;
    for (uint32_t i = 0; i < rules.types.size(); ++i) {
        const galactic::TypeInfo& t = rules.types[i];
        if (land ? t.kind != galactic::UnitKind::GroundUnit : (t.kind != galactic::UnitKind::SpaceUnit || !hop_capable(t))) continue;
        if (t.build_cost <= 0 || t.tech_level < 1 || t.named_hero || t.generic_hero || t.cinematic_only || t.company_member ||
            t.squadron_member || t.build_initially_locked || t.limit_lifetime == 0 || t.limit_current == 0 || !t.required_planets.empty() ||
            t.squadron_capacity < min_capacity)
            continue;
        if (!land && (i >= types.size() || !(types.get_float(i, "Max_Speed") > 0.0f))) continue;  // stations
        const bool any = t.affiliation.empty() && !t.data_rules;  // hand-built rules: empty = any
        if (!any && std::find(t.affiliation.begin(), t.affiliation.end(), faction) == t.affiliation.end()) continue;
        c.push_back(i);
    }
    std::sort(c.begin(), c.end(), [&](uint32_t a, uint32_t b) {
        const galactic::TypeInfo &x = rules.types[a], &y = rules.types[b];
        return x.build_cost != y.build_cost ? x.build_cost < y.build_cost : x.name < y.name;
    });
    return c;
}

std::vector<uint32_t> fallback_types(const galactic::GalacticRules& rules, const objects::GameObjectTypes& types, uint32_t fi, bool land) {
    const std::vector<uint32_t> c = buildable_units(rules, types, fi, land);
    std::vector<uint32_t> out;
    for (const size_t k : {size_t{0}, size_t{1}, c.size() / 2})
        if (k < c.size() && std::find(out.begin(), out.end(), c[k]) == out.end()) out.push_back(c[k]);
    return out;
}

}  // namespace

SkirmishForceTable load_skirmish_forces(const std::vector<galactic::FactionRules>& factions) {
    SkirmishForceTable out;
    for (const galactic::FactionRules& f : factions) {
        const std::string key = ascii_upper(ascii_trim(f.name));
        if (key.empty()) continue;
        std::vector<std::string> space = split_names(f.xml_text("Space_Skirmish_AI_Default_Forces"));
        std::vector<std::string> land = split_names(f.xml_text("Land_Skirmish_AI_Default_Forces"));
        if (space.empty() && land.empty()) continue;
        SkirmishForces& e = out[key];
        if (!space.empty()) e.space = std::move(space);
        if (!land.empty()) e.land = std::move(land);
    }
    return out;
}

#ifdef ALAMO_GAME_RULES_HAVE_AI
std::string resolve_ai_type(const std::string& faction, const std::map<std::string, std::string>& basic_ai,
                            const std::vector<std::string>* campaign_control, const ai::AiDefinitions* defs) {
    const auto it = basic_ai.find(ascii_upper(ascii_trim(faction)));
    if (it == basic_ai.end()) return {};
    std::string t = campaign_control ? ai::campaign_ai_type(*campaign_control, basic_ai, faction, false)
                                                       : std::string(ascii_trim(it->second));
    if (ascii_iequals(t, "Human")) t.clear();
    if (!t.empty() && defs && !defs->find_player(t)) t.clear();  // "Unrecognized AI player type": no AI
    return t;
}
#endif

std::vector<std::string> data_fallback_forces(const galactic::GalacticRules& rules, const objects::GameObjectTypes& types,
                                              const std::string& faction, bool land) {
    const uint32_t fi = rules.find_faction(faction);
    if (fi == galactic::kNoIndex) return {};
    std::vector<std::string> out;
    for (const uint32_t t : fallback_types(rules, types, fi, land)) out.push_back(rules.types[t].name);
    return out;
}

std::vector<std::string> skirmish_default_forces(const SkirmishForceData& d, const std::string& faction, bool land,
                                                 const SkirmishForceOptions& o) {
    std::vector<std::string> out;
    for (const uint32_t t : skirmish_data_force_types(d, faction, land, o)) out.push_back(d.rules.types[t].name);
    return out;
}

std::vector<uint32_t> skirmish_data_force_types(const SkirmishForceData& d, const std::string& faction, bool land,
                                                const SkirmishForceOptions& o) {
    const galactic::GalacticRules& rules = d.rules;
    std::vector<uint32_t> list;
    // Only types the skirmish galaxy can hold: space units / squadrons in space, ground units on land (TR's
    // SkirmishSquadron elements have no galactic role in the port, galactic_rules.cpp UnitKind).
    const auto fits = [&](uint32_t t) {
        const galactic::UnitKind k = rules.types[t].kind;
        return land ? k == galactic::UnitKind::GroundUnit : k == galactic::UnitKind::SpaceUnit || k == galactic::UnitKind::Squadron;
    };
    if (const auto it = d.forces.find(ascii_upper(ascii_trim(faction))); it != d.forces.end())
        for (const std::string& n : land ? it->second.land : it->second.space) {
            const uint32_t t = rules.find_type(n);
            if (t != galactic::kNoIndex && fits(t)) list.push_back(t);
            else if (o.diag)
                o.diag->push_back(faction + " default force " + n + (t == galactic::kNoIndex ? " not found" : " has no galactic role") + ", dropped");
        }
    const uint32_t fi = rules.find_faction(faction);
    if (list.empty()) {
        if (fi == galactic::kNoIndex) return {};
        list = fallback_types(rules, d.types, fi, land);
        if (o.diag) {
            std::string names;
            for (const uint32_t t : list) names += " " + rules.types[t].name;
            o.diag->push_back(faction + " starts with its cheapest buildable units [port]:" + names);
        }
    }
    if (land || list.empty() || !o.hyperspace_leader) return list;
    // Space: a hyperspace member leads (make_skirmish_galaxy's land carrier is the front; the human fleet hops).
    if (const auto h = std::find_if(list.begin(), list.end(), [&](uint32_t t) { return hop_capable(rules.types[t]); }); h != list.end()) {
        std::rotate(list.begin(), h, h + 1);
        return list;
    }
    if (const uint32_t leader = skirmish_hyperspace_leader(rules, d.types, fi, list); leader != galactic::kNoIndex)
        list.insert(list.begin(), leader);
    return list;
}

uint32_t skirmish_hyperspace_leader(const galactic::GalacticRules& rules, const objects::GameObjectTypes& types, uint32_t faction,
                                    const std::vector<uint32_t>& fleet) {
    if (fleet.empty()) return galactic::kNoIndex;
    if (std::any_of(fleet.begin(), fleet.end(), [&](uint32_t t) { return t < rules.types.size() && hop_capable(rules.types[t]); }))
        return galactic::kNoIndex;
    int32_t squadrons = 0;
    for (const uint32_t t : fleet) squadrons += t < rules.types.size() && rules.types[t].kind == galactic::UnitKind::Squadron ? 1 : 0;
    std::vector<uint32_t> carriers = buildable_units(rules, types, faction, false, squadrons);
    if (carriers.empty()) carriers = buildable_units(rules, types, faction, false);
    return carriers.empty() ? galactic::kNoIndex : carriers.front();
}

std::vector<uint32_t> skirmish_buy_list(const galactic::GalacticRules& rules, const objects::GameObjectTypes& types, uint32_t faction,
                                        bool land, const SkirmishBuyOptions& options) {
    // SKIRMISH_SETUP §2.7: 0xf0600 (steps 1-4) then the row fill 0xef9f0 (the named-hero rule) and the cost sort 0xf0e80.
    const size_t n = std::min(rules.types.size(), types.size());
    std::vector<uint8_t> in_mp_list;
    if (options.multiplayer_list_only) {  // [port interim rule] named in some type's Tactical_Buildable_Objects_Multiplayer
        in_mp_list.assign(n, 0);
        for (uint32_t t = 0; t < n; ++t)
            for (const std::string_view tok : types.get_list(t, "Tactical_Buildable_Objects_Multiplayer")) {
                const objects::TypeIndex x = types.resolve_name(ascii_trim(tok));  // faction row headers resolve to no type
                if (x != objects::kNone && x < n) in_mp_list[x] = 1;
            }
    }
    std::vector<uint32_t> out;
    for (uint32_t t = 0; t < n; ++t) {
        const galactic::TypeInfo& ti = rules.types[t];
        if (ti.build_cost <= 0) continue;                                                                     // 1.
        if (std::find(ti.affiliation.begin(), ti.affiliation.end(), faction) == ti.affiliation.end()) continue;  // 2.
        if (!types.get_bool(t, "Available_In_Skirmish") && !types.get_bool(t, land ? "Build_Tab_Land_Units" : "Build_Tab_Space_Units"))
            continue;                                                                                         // 3.
        if (ti.cinematic_only) continue;                                                                      // 4.
        if (ti.named_hero && !options.heroes) continue;                                                       // rows: heroes option
        if (options.multiplayer_list_only && !in_mp_list[t]) continue;
        out.push_back(t);
    }
    // Most expensive first; [port] equal costs by type index ascending (stable over the index-ordered list).
    std::stable_sort(out.begin(), out.end(), [&](uint32_t a, uint32_t b) { return rules.types[a].build_cost > rules.types[b].build_cost; });
    return out;
}

std::string skirmish_map_planet_type(const galactic::GalacticRules& rules, std::string_view map) {
    std::string stem(map);
    if (const size_t s = stem.find_last_of("\\/"); s != std::string::npos) stem = stem.substr(s + 1);
    if (const size_t dot = stem.find_last_of('.'); dot != std::string::npos) stem = stem.substr(0, dot);
    ascii_upper_in_place(stem);
    for (const std::string_view prefix : {std::string_view("_MP_SPACE_"), std::string_view("_MP_LAND_")}) {
        if (stem.size() <= prefix.size() || stem.compare(0, prefix.size(), prefix) != 0) continue;
        const uint32_t t = rules.find_type(std::string_view(stem).substr(prefix.size()));
        if (t != galactic::kNoIndex && rules.types[t].kind == galactic::UnitKind::Planet) return rules.types[t].name;
    }
    return {};
}

uint32_t skirmish_galaxy_planet(const galactic::GalacticRules& rules, std::string_view map, bool land) {
    const auto usable = [&](uint32_t t) {
        const galactic::TypeInfo& p = rules.types[t];
        return p.kind == galactic::UnitKind::Planet && p.terrain >= 0 && p.restricted_categories == 0 && p.restricted_types.empty() &&
               (!land || p.surface_accessible);
    };
    if (!map.empty())
        if (const uint32_t t = rules.find_type(skirmish_map_planet_type(rules, map)); t != galactic::kNoIndex && usable(t)) return t;
    uint32_t first = galactic::kNoIndex;
    for (uint32_t t = 0; t < rules.types.size(); ++t) {
        if (rules.types[t].kind != galactic::UnitKind::Planet) continue;
        if (usable(t)) return t;
        if (first == galactic::kNoIndex) first = t;
    }
    return first;
}

}  // namespace alamo::game
