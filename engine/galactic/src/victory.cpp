// Galactic victory monitor (victory.hpp; docs/re/STRATEGIC_GAPS.md §1.4, Q6_T15_RETAIL_RUN.md "ELIM C" / "q6 t25").
#include "alamo/galactic/victory.hpp"

#include <algorithm>
#include <utility>

#include "alamo/data/game_database.hpp"
#include "alamo/core/ascii.hpp"
#include "alamo/data/values.hpp"

namespace alamo::galactic {

namespace {

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

const VictorySide& no_filter_side() {
    static const VictorySide s;
    return s;
}

const VictorySide& side_of(const GalacticSim& sim, const VictoryRecord& r) {
    if (r.implicit) return no_filter_side();  // the implicit type 1: no faction filter
    return r.human_side ? sim.victory_rules().human : sim.victory_rules().ai;
}

bool registered(const GalacticSim& sim, int32_t player, uint8_t type) {
    if (type == GalacticSim::kVictoryStoryClaim) return true;  // the VICTORY reward adds its own record
    for (const VictoryRecord& r : sim.victory_records())
        if (r.player == player && static_cast<uint8_t>(r.type) == type) return true;
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
    case VictoryCondition::OpponentControlsNoPlanets: {
        // q6 t22 (Q6_T15 "ELIM C", 0x2814d0) [H]: a planet whose owner and the tested player are both AI-controlled
        // non-humans does not block; in single player an AI's type 1 means "no human enemy owns a planet".
        const bool p_ai = sim.victory_ai_player(player);
        for (const PlanetState& p : sim.planets()) {
            if (!p.is_planet || !sim.is_hostile(player, p.owner) || !playable(sim, p.owner)) continue;
            if (p_ai && sim.victory_ai_player(p.owner)) continue;
            if (!side.faction_names.empty()) {
                const FactionRules* f = faction(sim, p.owner);
                if (!f || std::none_of(side.faction_names.begin(), side.faction_names.end(),
                                       [f](const std::string& n) { return ascii_iequals(n, f->name); }))
                    continue;
            }
            return false;
        }
        return true;
    }
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
    case VictoryCondition::StoryTriggered:
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
        if (ascii_iequals(name, kNames[i])) return static_cast<VictoryCondition>(i);
    return VictoryCondition::Unknown;
}

VictoryRules load_victory_rules(const data::GameDatabase& db, std::string_view campaign, const GalaxyMap& map,
                                std::vector<std::string>* diagnostics, std::string_view starting_active_faction) {
    VictoryRules r;
    const data::CampaignDef* c = db.find_campaign(campaign);
    if (!c) return r;
    r.human = load_side(*c, "Human", map, diagnostics);
    r.ai = load_side(*c, "AI", map, diagnostics);
    r.starting_active_faction = std::string(data::trim(starting_active_faction.empty() ? std::string_view(c->starting_active_player)
                                                                                         : starting_active_faction));
    return r;
}

VictoryResult check_victory(const GalacticSim& sim) {
    VictoryResult out;
    if (sim.victory_hold() > 0) return out;  // 0x342d80: the result needs hold <= 0
    const VictoryClaim* d = sim.victory_declared();
    if (d && sim.frame() >= d->frame + d->countdown) {
        out.winner = d->player;
        out.how = static_cast<VictoryCondition>(d->type);
    }
    return out;
}

int32_t victory_outcome(const GalacticSim& sim, int32_t player) {
    const VictoryResult r = check_victory(sim);
    if (!r.finished()) return 0;
    return r.winner == player ? 1 : -1;
}

// ---- GalacticSim: the monitor's records and event-driven tests (campaign.hpp, q6 t21 / t22) ------------------------

void GalacticSim::set_victory_rules(VictoryRules rules) {
    victory_rules_ = std::move(rules);
    victory_records_.clear();
    victory_side_.assign(players_.size(), 0);
    for (size_t i = 0; i < players_.size(); ++i) {
        const int32_t p = static_cast<int32_t>(i);
        const uint32_t f = players_[i].faction;
        const FactionRules* fr = f < rules_->factions.size() ? &rules_->factions[f] : nullptr;
        if (!fr || fr->is_neutral) continue;  // 0x289a90: no records for an Is_Neutral faction (+0x108)
        const bool human = victory_rules_.starting_active_faction.empty()
                               ? players_[i].human  // [port] hand-built rules without a Starting_Active_Player
                               : ascii_iequals(fr->name, victory_rules_.starting_active_faction);
        victory_side_[i] = human ? 1 : 2;
        const VictorySide& side = human ? victory_rules_.human : victory_rules_.ai;
        for (VictoryCondition k : side.conditions) {
            if (k == VictoryCondition::Unknown) continue;
            VictoryRecord r;
            r.player = p;
            r.type = k;
            r.human_side = human;
            victory_records_.push_back(r);
        }
        if (!human && !side.conditions.empty()) {  // the implicit type 1, only when the AI list is non-empty
            VictoryRecord r;
            r.player = p;
            r.type = VictoryCondition::OpponentControlsNoPlanets;
            r.implicit = true;
            victory_records_.push_back(r);
        }
    }
}

bool GalacticSim::victory_ai_player(int32_t player) const {
    if (!valid_player(player)) return false;
    const PlayerState& ps = players_[static_cast<size_t>(player)];
    return ps.ai && !ps.human && victory_side(player) != 1;
}

const VictoryClaim* GalacticSim::victory_declared() const {
    for (const VictoryClaim& c : claims_)
        if (registered(*this, c.player, c.type)) return &c;
    return nullptr;
}

bool GalacticSim::victory_test(int32_t player, VictoryCondition type) {
    const uint8_t t = static_cast<uint8_t>(type);
    if (!valid_player(player) || !registered(*this, player, t) || type == VictoryCondition::StoryTriggered) return false;
    if (victory_hold_ > 0) {  // 0x344710 gate: defer {type, player}, no duplicates (0x341f40)
        const std::pair<int32_t, uint8_t> e{player, t};
        if (std::find(victory_deferred_.begin(), victory_deferred_.end(), e) == victory_deferred_.end()) victory_deferred_.push_back(e);
        return false;
    }
    if (const VictoryClaim* d = victory_declared()) return d->player == player && d->type == t;
    // [port] a player with two records of the type (an AI list that names type 1 and the implicit one): any passing
    // record declares, with that record's countdown (both 30).
    for (const VictoryRecord& r : victory_records_) {
        if (r.player != player || r.type != type) continue;
        if (!victory_condition_met(*this, side_of(*this, r), type, player)) continue;
        claim_victory(player, t, r.countdown);
        return true;
    }
    return false;
}

void GalacticSim::victory_tests(std::initializer_list<VictoryCondition> types) {
    if (victory_records_.empty()) return;
    for (size_t i = 0; i < players_.size(); ++i)
        for (VictoryCondition k : types)
            if (victory_test(static_cast<int32_t>(i), k)) return;  // first pass wins
}

void GalacticSim::victory_hold_release() {
    if (victory_hold_ <= 0) return;
    if (--victory_hold_ > 0) return;
    // 0x3440a0 replay 1: the deferred tests, in queue order, against the state now; the first declaration stops.
    const std::vector<std::pair<int32_t, uint8_t>> queue = std::move(victory_deferred_);
    victory_deferred_.clear();
    for (const auto& e : queue)
        if (victory_test(e.first, static_cast<VictoryCondition>(e.second))) break;
}

}  // namespace alamo::galactic
