// q6 t03: skirmish rules shared by alamo_game and alamo_headless (skirmish_rules.hpp).
#include "alamo/game/skirmish_rules.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>

#include "alamo/core/file_provider.hpp"
#include "alamo/data/xml.hpp"
#include "alamo/galactic/galactic_rules.hpp"
#include "alamo/objects/game_object_types.hpp"
#ifdef ALAMO_GAME_RULES_HAVE_AI
#include "alamo/ai/ai_definitions.hpp"
#endif

namespace alamo::game {

namespace {

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

bool iequal(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::toupper(static_cast<unsigned char>(x)) == std::toupper(static_cast<unsigned char>(y));
           });
}

std::string_view trim(std::string_view s) {
    const auto ws = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!s.empty() && ws(s.front())) s.remove_prefix(1);
    while (!s.empty() && ws(s.back())) s.remove_suffix(1);
    return s;
}

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

// A stock list is kept when the data defines at least one of its names (FoC: all of them).
bool any_known(const galactic::GalacticRules& rules, const std::vector<std::string>& names) {
    return std::any_of(names.begin(), names.end(), [&](const std::string& n) { return rules.find_type(n) != galactic::kNoIndex; });
}

}  // namespace

SkirmishForceTable load_skirmish_forces(const FileProvider& files) {
    SkirmishForceTable out;
    std::vector<uint8_t> bytes;
    if (!files.read("Data\\XML\\FactionFiles.xml", bytes)) return out;
    std::vector<std::string> list;
    {
        const data::XmlDocument doc = data::XmlDocument::parse(bytes, "FactionFiles.xml");
        if (!doc.ok()) return out;
        for (const data::XmlNode n : doc.root().children())
            if (iequal(n.name(), "File")) list.emplace_back(n.text_trimmed());
    }
    for (const std::string& f : list) {
        if (!files.read("Data\\XML\\" + f, bytes)) continue;
        const data::XmlDocument doc = data::XmlDocument::parse(bytes, f);
        if (!doc.ok()) continue;
        for (const data::XmlNode n : doc.root().children()) {
            if (!iequal(n.name(), "Faction")) continue;
            const std::string key = upper(std::string(trim(n.attribute("Name", ""))));
            if (key.empty()) continue;
            std::vector<std::string> space = split_names(n.child_text("Space_Skirmish_AI_Default_Forces"));
            std::vector<std::string> land = split_names(n.child_text("Land_Skirmish_AI_Default_Forces"));
            if (space.empty() && land.empty()) continue;
            SkirmishForces& e = out[key];
            if (!space.empty()) e.space = std::move(space);
            if (!land.empty()) e.land = std::move(land);
        }
    }
    return out;
}

std::string ai_type_for(const std::string& faction) {
    const std::string f = upper(faction);
    if (f == "EMPIRE") return "BasicEmpire";
    if (f == "REBEL") return "BasicRebel";
    if (f == "UNDERWORLD") return "AI_Player_Underworld";
    return {};
}

bool is_stock_faction(const std::string& faction, const std::map<std::string, std::string>& basic_ai) {
    const std::string stock = ai_type_for(faction);
    if (stock.empty()) return false;
    const auto it = basic_ai.find(upper(std::string(trim(faction))));
    return it == basic_ai.end() || iequal(trim(it->second), stock);
}

#ifdef ALAMO_GAME_RULES_HAVE_AI
std::string resolve_ai_type(const std::string& faction, const std::map<std::string, std::string>& basic_ai,
                            const std::vector<std::string>* campaign_control, const ai::AiDefinitions* defs) {
    if (is_stock_faction(faction, basic_ai)) return ai_type_for(faction);
    const auto it = basic_ai.find(upper(std::string(trim(faction))));
    if (it == basic_ai.end()) return {};
    std::string t = campaign_control ? ai::campaign_ai_type(*campaign_control, basic_ai, faction, false) : std::string(trim(it->second));
    if (iequal(t, "Human")) t.clear();
    if (!t.empty() && defs && !defs->find_player(t)) t.clear();  // "Unrecognized AI player type": no AI
    return t;
}
#endif

std::vector<std::string> default_skirmish_fleet(const std::string& faction) {
    const std::string f = upper(faction);
    if (f == "EMPIRE")
        return {"Star_Destroyer", "Tartan_Patrol_Cruiser", "Tartan_Patrol_Cruiser", "Acclamator_Assault_Ship", "Broadside_Class_Cruiser"};
    if (f == "REBEL")
        return {"Calamari_Cruiser", "Nebulon_B_Frigate", "Corellian_Corvette", "Corellian_Corvette", "Alliance_Assault_Frigate"};
    if (f == "UNDERWORLD")
        return {"Vengeance_Frigate", "Kedalbe_Battleship", "Interceptor4_Frigate", "Crusader_Gunship", "Crusader_Gunship"};
    return {};
}

std::vector<std::string> default_skirmish_ground(const std::string& faction) {
    const std::string f = upper(faction);
    if (f == "EMPIRE") return {"Imperial_Stormtrooper_Squad", "Imperial_Stormtrooper_Squad", "Imperial_Armor_Group", "Imperial_Anti_Infantry_Brigade"};
    if (f == "REBEL") return {"Rebel_Infantry_Squad", "Rebel_Infantry_Squad", "Rebel_Light_Tank_Brigade", "Rebel_Tank_Buster_Squad"};
    if (f == "UNDERWORLD") return {"Underworld_Merc_Squad", "Underworld_Merc_Squad", "Underworld_Disruptor_Merc_Squad", "MZ8_Pulse_Cannon_Tank_Company"};
    return {};
}

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
    if (o.stock_lists && is_stock_faction(faction, d.basic_ai))
        if (std::vector<std::string> s = land ? default_skirmish_ground(faction) : default_skirmish_fleet(faction); any_known(d.rules, s)) return s;
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
    if (const auto it = d.forces.find(upper(std::string(trim(faction)))); it != d.forces.end())
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

std::string skirmish_map_planet_type(const galactic::GalacticRules& rules, std::string_view map) {
    std::string stem(map);
    if (const size_t s = stem.find_last_of("\\/"); s != std::string::npos) stem = stem.substr(s + 1);
    if (const size_t dot = stem.find_last_of('.'); dot != std::string::npos) stem = stem.substr(0, dot);
    stem = upper(stem);
    for (const std::string_view prefix : {std::string_view("_MP_SPACE_"), std::string_view("_MP_LAND_")}) {
        if (stem.size() <= prefix.size() || stem.compare(0, prefix.size(), prefix) != 0) continue;
        const uint32_t t = rules.find_type(std::string_view(stem).substr(prefix.size()));
        if (t != galactic::kNoIndex && rules.types[t].kind == galactic::UnitKind::Planet) return rules.types[t].name;
    }
    return {};
}

}  // namespace alamo::game
