// Galactic victory monitor (victory.hpp; docs/re/STRATEGIC_GAPS.md §1.4).
#include "alamo/galactic/victory.hpp"

#include <algorithm>

#include "alamo/data/game_database.hpp"
#include "alamo/data/values.hpp"

namespace alamo::galactic {

namespace {

bool iequal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

VictorySide load_side(const data::CampaignDef& c, std::string_view side, const GalaxyMap& map, std::vector<std::string>* diag) {
    VictorySide s;
    const std::string prefix(side);
    for (std::string_view v : data::split_fields(c.raw_value(prefix + "_Victory_Conditions"))) {
        if (data::trim(v).empty()) continue;
        const VictoryCondition k = parse_victory_condition(data::trim(v));
        if (k == VictoryCondition::Unknown && diag) diag->push_back("galactic: unknown victory condition " + std::string(v));
        s.conditions.push_back(k);
    }
    for (std::string_view v : data::split_fields(c.raw_value(prefix + "_Victory_Planet_Names"))) {
        if (data::trim(v).empty()) continue;
        const PlanetIndex p = map.planets.find(data::trim(v));
        if (p == kInvalidPlanet) {
            if (diag) diag->push_back("galactic: victory planet not in the map: " + std::string(v));
            continue;
        }
        s.planets.push_back(p);
    }
    for (std::string_view v : data::split_fields(c.raw_value(prefix + "_Victory_Faction_Names")))
        if (!data::trim(v).empty()) s.faction_names.emplace_back(data::trim(v));
    s.cycle_limit = data::parse_int_or(c.raw_value(prefix + "_Victory_Cycle_Limit"), 0);
    s.credit_target = data::parse_int_or(c.raw_value(prefix + "_Victory_Credit_Target"), 0);
    s.galactic_control = data::parse_float_or(c.raw_value(prefix + "_Victory_Galactic_Control"), 0.0f);
    return s;
}

const FactionRules* faction(const GalacticSim& sim, int32_t player) {
    if (player < 0 || static_cast<size_t>(player) >= sim.players().size()) return nullptr;
    const uint32_t f = sim.players()[static_cast<size_t>(player)].faction;
    return f < sim.rules().factions.size() ? &sim.rules().factions[f] : nullptr;
}

bool playable(const GalacticSim& sim, int32_t player) {
    const FactionRules* r = faction(sim, player);
    return r && r->is_playable && !r->is_neutral;
}

// 0x3fa300 [port reading]: a live object of `type` (or a company listing it) in some planet's orbit or on its surface.
bool type_on_a_planet(const GalacticSim& sim, uint32_t type) {
    if (type == kNoIndex) return false;
    const objects::ObjectStore& st = sim.store();
    const auto is = [&](ObjectHandle h) {
        if (!st.alive(h)) return false;
        const uint32_t t = static_cast<uint32_t>(st.type(h));
        if (t == type) return true;
        if (t >= sim.rules().types.size()) return false;
        const std::vector<uint32_t>& m = sim.rules().types[t].company_units;
        return std::find(m.begin(), m.end(), type) != m.end();
    };
    for (const PlanetState& p : sim.planets()) {
        if (!p.is_planet) continue;
        for (uint32_t f : p.orbit)
            if (sim.fleets()[f].alive)
                for (ObjectHandle m : sim.fleets()[f].contents)
                    if (is(m)) return true;
        for (ObjectHandle u : p.surface)
            if (is(u)) return true;
    }
    return false;
}

}  // namespace

bool victory_condition_met(const GalacticSim& sim, const VictorySide& side, VictoryCondition k, int32_t player) {
    if (!playable(sim, player)) return false;
    switch (k) {
    case VictoryCondition::AllPlanetsControlled: {
        bool any = false;
        for (const PlanetState& p : sim.planets()) {
            if (!p.is_planet) continue;
            if (!sim.allied(player, p.owner)) return false;
            any = true;
        }
        return any;
    }
    case VictoryCondition::OpponentControlsNoPlanets:
        for (const PlanetState& p : sim.planets()) {
            if (!p.is_planet || !sim.is_hostile(player, p.owner) || !playable(sim, p.owner)) continue;
            if (!side.faction_names.empty()) {
                const FactionRules* f = faction(sim, p.owner);
                if (!f || std::none_of(side.faction_names.begin(), side.faction_names.end(),
                                       [f](const std::string& n) { return iequal(n, f->name); }))
                    continue;
            }
            return false;
        }
        return true;
    case VictoryCondition::CreditsAccrued:
        return sim.credits(player) >= static_cast<float>(side.credit_target);
    case VictoryCondition::PercentageControl: {
        int32_t all = 0;
        for (const PlanetState& p : sim.planets()) all += p.is_planet ? 1 : 0;
        if (all == 0) return false;
        return static_cast<float>(sim.planets_owned(player)) / static_cast<float>(all) >= side.galactic_control;
    }
    case VictoryCondition::ConquerEnemyHomeworldAndLeader: {
        const FactionRules* f = faction(sim, player);
        if (!f || f->primary_enemy >= sim.rules().factions.size()) return false;
        const FactionRules& e = sim.rules().factions[f->primary_enemy];
        if (e.home_planet_type == kNoIndex) return false;
        bool home = false;
        for (const PlanetState& p : sim.planets())
            if (p.is_planet && p.type == e.home_planet_type) home = home || p.owner == player;
        return home && !type_on_a_planet(sim, e.leader_company);
    }
    case VictoryCondition::KillEnemyLeader: {
        bool any = false;
        for (size_t q = 0; q < sim.players().size(); ++q) {
            const int32_t pq = static_cast<int32_t>(q);
            if (!sim.is_hostile(player, pq) || !playable(sim, pq)) continue;
            const FactionRules* f = faction(sim, pq);
            if (!f || f->leader_company >= sim.rules().types.size()) continue;
            const std::vector<uint32_t>& m = sim.rules().types[f->leader_company].company_units;
            const uint32_t leader = m.empty() ? f->leader_company : m.front();
            any = true;
            if (type_on_a_planet(sim, leader) || type_on_a_planet(sim, f->leader_company)) return false;
        }
        return any;
    }
    case VictoryCondition::ControlNamedPlanets:
        if (side.planets.empty()) return false;
        return std::all_of(side.planets.begin(), side.planets.end(), [&](PlanetIndex p) {
            return p < sim.planets().size() && sim.planets()[p].is_planet && sim.planets()[p].owner == player;
        });
    case VictoryCondition::CyclesElapsed:
        return side.cycle_limit > 0 && sim.galactic_day() > side.cycle_limit;
    case VictoryCondition::SuperWeaponDestruction:
    case VictoryCondition::SuperWeaponDestroysLastEnemyPlanet:
    case VictoryCondition::SuperWeaponKillsEnemyLeader:
        for (const VictoryClaim& c : sim.victory_claims())
            if (c.player == player && c.type == static_cast<uint8_t>(k) && sim.frame() >= c.frame + c.countdown) return true;
        return false;
    case VictoryCondition::Unknown:
        break;
    }
    return false;
}

VictoryCondition parse_victory_condition(std::string_view name) {
    static constexpr const char* kNames[] = {
        "Galactic_All_Planets_Controlled", "Galactic_Opponent_Controls_No_Planets", "Galactic_Credits_Accrued",
        "Galactic_Super_Weapon_Destruction", "Galactic_Percentage_Control", "Galactic_Conquer_Enemy_Homeworld_And_Leader",
        "Galactic_Super_Weapon_Destroys_Last_Enemy_Planet", "Galactic_Super_Weapon_Kills_Enemy_Leader",
        "Galactic_Kill_Enemy_Leader", "Galactic_Control_Named_Planets", "Galactic_Cycles_Elapsed"};
    for (uint8_t i = 0; i < 11; ++i)
        if (iequal(name, kNames[i])) return static_cast<VictoryCondition>(i);
    return VictoryCondition::Unknown;
}

VictoryRules load_victory_rules(const data::GameDatabase& db, std::string_view campaign, const GalaxyMap& map,
                                std::vector<std::string>* diagnostics) {
    VictoryRules r;
    const data::CampaignDef* c = db.find_campaign(campaign);
    if (!c) return r;
    r.human = load_side(*c, "Human", map, diagnostics);
    r.ai = load_side(*c, "AI", map, diagnostics);
    return r;
}

bool player_eliminated(const GalacticSim& sim, int32_t player) {
    if (player < 0 || static_cast<size_t>(player) >= sim.players().size()) return false;
    if (sim.planets_owned(player) > 0) return false;
    const objects::ObjectStore& st = sim.store();
    for (const FleetState& f : sim.fleets())
        if (f.alive)
            for (ObjectHandle m : f.contents)
                if (st.alive(m) && st.owner(m) == player) return false;
    for (const PlanetState& p : sim.planets()) {
        if (!p.is_planet) continue;
        for (ObjectHandle u : p.surface)
            if (st.alive(u) && st.owner(u) == player) return false;
        for (ObjectHandle s : p.structures)
            if (st.alive(s) && st.owner(s) == player) return false;
        if (st.alive(p.starbase) && st.owner(p.starbase) == player) return false;
    }
    return true;
}

VictoryResult check_victory(const GalacticSim& sim, const VictoryRules& rules) {
    VictoryResult out;
    for (size_t i = 0; i < sim.players().size(); ++i) {
        const int32_t p = static_cast<int32_t>(i);
        if (!playable(sim, p)) continue;
        if (player_eliminated(sim, p)) {
            out.eliminated.push_back(p);
            continue;
        }
        if (out.winner != kNoPlayer) continue;
        const VictorySide& side = sim.players()[i].human ? rules.human : rules.ai;
        for (VictoryCondition k : side.conditions)
            if (victory_condition_met(sim, side, k, p)) {
                out.winner = p;
                out.how = k;
                break;
            }
    }
    return out;
}

int32_t victory_outcome(const GalacticSim& sim, const VictoryRules& rules, int32_t player) {
    const VictoryResult r = check_victory(sim, rules);
    if (r.winner == player) return 1;
    if (r.finished() || std::find(r.eliminated.begin(), r.eliminated.end(), player) != r.eliminated.end()) return -1;
    return 0;
}

}  // namespace alamo::galactic
