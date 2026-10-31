#include "alamo/galactic/conflicts.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

#include "alamo/data/game_database.hpp"
#include "alamo/data/xml.hpp"
#include "alamo/objects/game_object_types.hpp"
#include "alamo/objects/type_fields.hpp"

namespace alamo::galactic {

namespace {

char upper(char c) { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c; }

bool iequal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (upper(a[i]) != upper(b[i])) return false;
    return true;
}

std::string trimmed(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.remove_suffix(1);
    return std::string(s);
}

void note(std::vector<std::string>* d, std::string m) {
    if (d) d->push_back(std::move(m));
}

bool space_kind(UnitKind k) { return k == UnitKind::SpaceUnit || k == UnitKind::Squadron; }

int32_t terrain_index(std::string_view s) {
    static const char* const kNames[] = {"TEMPERATE", "ARCTIC", "DESERT", "FOREST", "SWAMP", "VOLCANIC", "URBAN", "SPACE"};
    for (int32_t i = 0; i < 8; ++i)
        if (iequal(s, kNames[i])) return i;
    return -1;
}

int32_t map_faction_index(std::string_view name) {
    if (iequal(name, "Rebel")) return 0;
    if (iequal(name, "Empire")) return 1;
    if (iequal(name, "Pirates")) return 2;
    if (iequal(name, "Underworld")) return 4;
    if (iequal(name, "Hutts")) return 5;
    return 0;
}

}  // namespace

// ---- rules ----------------------------------------------------------------------------------------------------

const ConflictTypeInfo& ConflictRules::type(uint32_t t) const {
    static const ConflictTypeInfo kDefault{};
    return t < types.size() ? types[t] : kDefault;
}

const ConflictFactionInfo& ConflictRules::faction(uint32_t f) const {
    static const ConflictFactionInfo kDefault{};
    return f < factions.size() ? factions[f] : kDefault;
}

bool load_conflict_rules(const FileProvider& provider, const data::GameDatabase& db, const objects::GameObjectTypes& types,
                         const GalacticRules& galactic, ConflictRules& out, std::vector<std::string>* diag) {
    const data::GameConstants& g = db.constants();
    AutoResolveConstants& a = out.auto_resolve;
    a.attrition_allowance = g.get_float("AutoResolveAttritionAllowanceFactor", a.attrition_allowance);
    a.transport_losses = g.get_float("AutoResolveTransportLosses", a.transport_losses);
    a.loser_attrition = g.get_float("AutoResolveLoserAttrition", a.loser_attrition);
    a.winner_attrition = g.get_float("AutoResolveWinnerAttrition", a.winner_attrition);
    a.retreat_loser_attrition = g.get_float("RetreatAutoResolveLoserAttrition", a.retreat_loser_attrition);
    a.retreat_winner_attrition = g.get_float("RetreatAutoResolveWinnerAttrition", a.retreat_winner_attrition);
    a.tactical_multiplier = g.get_float("Auto_Resolve_Tactical_Multiplier", a.tactical_multiplier);
    a.display_time = g.get_float("AutoResolveDisplayTime", a.display_time);
    out.overrun_multiple = g.get_float("Overrun_Auto_Resolve_Multiple", 0.0f);
    out.queue_tactical_battles = g.get_bool("Strategic_Queue_Tactical_Battles", true);
    out.space_auto_resolve_delay = g.get_float("Space_Auto_Resolve_Delay_Seconds", out.space_auto_resolve_delay);
    out.land_auto_resolve_delay = g.get_float("Land_Auto_Resolve_Delay_Seconds", out.land_auto_resolve_delay);
    out.space_retreat_allowed = g.get_float("Space_Retreat_Allowed_Countdown_Seconds", out.space_retreat_allowed);
    out.land_retreat_allowed = g.get_float("Land_Retreat_Allowed_Countdown_Seconds", out.land_retreat_allowed);
    out.land_base_destruction_forces_retreat = g.get_bool("Land_Base_Destruction_Forces_Retreat", false);
    out.space_station_destruction_forces_retreat = g.get_bool("Space_Station_Destruction_Forces_Retreat", false);

    const objects::EnumConverter& cat = types.enums().get(objects::EnumId::GameObjectCategory);
    auto category_mask = [&cat](std::string_view name) -> uint64_t {
        const auto v = cat.lookup(name);
        return v ? static_cast<uint64_t>(*v) : 0u;
    };
    out.super_category = category_mask("Super");
    // OI galactic (TACTICAL_GAPS 4.2): land split constants (same reading as the battle host's BattleCatalog).
    {
        auto words = [](std::string_view s, bool bars) {
            std::vector<std::string> w;
            std::string cur;
            for (const char c : s) {
                const bool sep = c == ',' || c == ' ' || c == '\t' || c == '\r' || c == '\n' || (bars && c == '|');
                if (!sep) cur.push_back(c);
                else if (!cur.empty()) w.push_back(std::exchange(cur, {}));
            }
            if (!cur.empty()) w.push_back(cur);
            return w;
        };
        if (const auto v = g.get("Preferred_Pathfinder_Types")) out.preferred_pathfinder_types = words(*v, false);
        if (const auto v = g.get("Raid_Force_Required_Faction")) out.raid_required_faction = trimmed(*v);
        auto mask = [&](std::string_view tag) {
            uint64_t m = 0;
            if (const auto v = g.get(tag))
                for (const std::string& w : words(*v, true)) m |= category_mask(w);
            return m;
        };
        out.raid_free_mask = mask("Raid_Force_Free_Object_Category_Mask");
        out.raid_limited_mask = mask("Raid_Force_Limited_Object_Category_Mask");
        out.raid_max_limited = g.get_int("Raid_Force_Max_Limited_Objects", 0);
        out.raid_max_heroes = g.get_int("Raid_Force_Max_Heros", 0);
    }

    // Per type.
    out.types.assign(types.size(), ConflictTypeInfo{});
    for (objects::TypeIndex t = 0; t < types.size(); ++t) {
        ConflictTypeInfo& i = out.types[t];
        i.categories = types.get_mask(t, "CategoryMask");
        i.population = types.columns().population_value[t];
        if (!types.get_string(t, "Space_Override_Population_Value").empty())
            i.space_population_override = types.get_int(t, "Space_Override_Population_Value");
        i.hero = types.get_bool(t, "Is_Named_Hero");
        i.super_killer = types.get_bool(t, "Is_Super_Weapon_Killer");
        for (const char* tag : {"Squadron_Units", "Company_Units"})
            for (std::string_view m : types.get_list(t, tag)) {
                const objects::TypeIndex mt = types.resolve_name(trimmed(m));
                if (mt == objects::kNone) continue;
                bool found = false;
                for (auto& [ty, n] : i.members)
                    if (ty == mt) {
                        ++n;
                        found = true;
                    }
                if (!found) i.members.emplace_back(static_cast<uint32_t>(mt), 1);
            }
        i.space_map = trimmed(types.get_string(t, "Space_Tactical_Map"));
        i.land_map = trimmed(types.get_string(t, "Land_Tactical_Map"));
        const std::string terrain = trimmed(types.get_string(t, "Terrain"));
        i.terrain = terrain.empty() ? -1 : terrain_index(terrain);
        if (t < galactic.types.size() && galactic.types[t].kind == UnitKind::StarBase) {
            std::string name = galactic.types[t].name;
            if (!name.empty() && name.back() > '1' && name.back() <= '9') {
                name.back() = static_cast<char>(name.back() - 1);
                const uint32_t lower = galactic.find_type(name);
                if (lower < galactic.types.size() && galactic.types[lower].kind == UnitKind::StarBase) i.lower_starbase = lower;
            }
        }
    }

    // Factions (FactionFiles.xml list order; matched to GalacticRules by name).
    out.factions.assign(galactic.factions.size(), ConflictFactionInfo{});
    for (size_t f = 0; f < galactic.factions.size(); ++f) out.factions[f].map_faction = map_faction_index(galactic.factions[f].name);
    std::vector<uint8_t> bytes;
    if (provider.read("Data\\XML\\FactionFiles.xml", bytes)) {
        const data::XmlDocument list = data::XmlDocument::parse(bytes, "FactionFiles.xml");
        for (data::XmlNode file : list.root().children("File")) {
            const std::string path = "Data\\XML\\" + std::string(file.text_trimmed());
            std::vector<uint8_t> fb;
            if (!provider.read(path, fb)) {
                note(diag, "conflicts: " + path + " not found");
                continue;
            }
            const data::XmlDocument doc = data::XmlDocument::parse(fb, path);
            for (data::XmlNode n : doc.root().children("Faction")) {
                const uint32_t fi = galactic.find_faction(n.attribute("Name"));
                if (fi >= out.factions.size()) continue;
                ConflictFactionInfo& ci = out.factions[fi];
                auto num = [&n](const char* tag, float fallback) {
                    const std::string_view v = n.child_text(tag);
                    return v.empty() ? fallback : static_cast<float>(objects::c_atof(v));
                };
                ci.space_unit_cap = num("Space_Tactical_Unit_Cap", ci.space_unit_cap);
                ci.space_retreat_countdown = num("Space_Retreat_Countdown_Seconds", ci.space_retreat_countdown);
                ci.land_retreat_countdown = num("Land_Retreat_Countdown_Seconds", ci.land_retreat_countdown);
            }
        }
    } else {
        note(diag, "conflicts: FactionFiles.xml not found (default unit caps)");
    }

    // AI contrast table.
    bool ok = true;
    bytes.clear();
    if (provider.read("Data\\Scripts\\Library\\PGAICommands.lua", bytes)) {
        const std::string_view src(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        if (!out.contrast.parse_lua(src, category_mask, diag)) ok = false;
    } else {
        note(diag, "conflicts: Data\\Scripts\\Library\\PGAICommands.lua not found (no contrast table)");
        ok = false;
    }
    return ok;
}

// ---- log ---------------------------------------------------------------------------------------------------------

std::string ConflictLogEntry::text(const GalacticSim& sim) const {
    auto planet_name = [&sim](PlanetIndex p) {
        return p < sim.map().planets.name.size() ? std::string(sim.map().planets.name[p]) : std::string("?");
    };
    auto faction = [&sim](int32_t p) -> std::string {
        if (p < 0 || static_cast<size_t>(p) >= sim.players().size()) return "-";
        const uint32_t f = sim.players()[static_cast<size_t>(p)].faction;
        return f < sim.rules().factions.size() ? sim.rules().factions[f].name : "?";
    };
    std::string s = "tick " + std::to_string(tick) + " gframe " + std::to_string(galactic_frame) +
                    (kind == ConflictKind::Space ? " space " : " land ") + planet_name(planet) + " " + faction(attacker) +
                    " vs " + faction(defender) + ": ";
    if (how == 0) return s + "dropped";
    s += how == 1 ? "auto-resolved" : "tactical " + map;
    s += ", winner " + faction(winner) + ", loser " + faction(loser) + ", retreat " + faction(retreating) +
         ", losses att " + std::to_string(attacker_losses) + " def " + std::to_string(defender_losses);
    if (how == 1) s += ", draws " + std::to_string(rng_draws);
    return s;
}

// ---- manager -----------------------------------------------------------------------------------------------------

ConflictManager::ConflictManager(GalacticSim& sim, const ConflictRules& rules, SimRng& rng, ConflictConfig cfg)
    : sim_(&sim), rules_(&rules), rng_(&rng), cfg_(cfg) {
    planet_map_.assign(sim.planets().size(), std::string());
}

ConflictManager::~ConflictManager() { detach(); }

void ConflictManager::detach() {
    if (sim_->conflict_hooks() == this) sim_->set_conflict_hooks(nullptr);
}

bool ConflictManager::human(int32_t player) const {
    return player >= 0 && static_cast<size_t>(player) < sim_->players().size() && sim_->players()[static_cast<size_t>(player)].human;
}

void ConflictManager::raise_pause() {
    if (pause_held_ > 0) return;  // 0x28bb80: only if not raised yet
    pause_held_ = 1;
    if (pause) pause(true);
    else sim_->push_pause();
}

void ConflictManager::release_pause() {
    if (pause_held_ == 0) return;
    pause_held_ = 0;
    if (pause) pause(false);
    else sim_->pop_pause();
}

// ---- participants ----

std::vector<ObjectHandle> ConflictManager::orbit_units(PlanetIndex planet, int32_t player, bool space_only) const {
    std::vector<ObjectHandle> out;
    if (planet >= sim_->planets().size()) return out;
    const auto& store = sim_->store();
    for (uint32_t fi : sim_->planets()[planet].orbit) {
        const FleetState& f = sim_->fleets()[fi];
        if (!f.alive || f.moving || f.owner != player) continue;
        for (ObjectHandle m : f.contents) {
            if (!store.alive(m)) continue;
            const uint32_t t = static_cast<uint32_t>(store.type(m));
            const bool sk = t < sim_->rules().types.size() && space_kind(sim_->rules().types[t].kind);
            if (space_only && !sk) continue;
            out.push_back(m);
        }
    }
    return out;
}

std::vector<ObjectHandle> ConflictManager::orbit_cargo(PlanetIndex planet, int32_t player) const {
    std::vector<ObjectHandle> out;
    if (planet >= sim_->planets().size()) return out;
    const auto& store = sim_->store();
    for (uint32_t fi : sim_->planets()[planet].orbit) {
        const FleetState& f = sim_->fleets()[fi];
        if (!f.alive || f.moving || f.owner != player) continue;
        for (ObjectHandle m : f.contents) {
            if (!store.alive(m)) continue;
            const uint32_t t = static_cast<uint32_t>(store.type(m));
            if (t < sim_->rules().types.size() && sim_->rules().types[t].kind == UnitKind::GroundUnit) out.push_back(m);
        }
    }
    return out;
}

std::vector<ObjectHandle> ConflictManager::surface_units(PlanetIndex planet, int32_t player) const {
    std::vector<ObjectHandle> out;
    if (planet >= sim_->planets().size()) return out;
    for (ObjectHandle u : sim_->planets()[planet].surface)
        if (sim_->store().alive(u) && sim_->store().owner(u) == player) out.push_back(u);
    return out;
}

float ConflictManager::unit_power(ObjectHandle h) const {
    const uint32_t t = static_cast<uint32_t>(sim_->store().type(h));
    const auto& ti = sim_->rules().types;
    float p = t < ti.size() ? ti[t].ai_combat_power : 0.0f;
    for (const auto& [mt, n] : rules_->type(t).members)
        if (mt < ti.size()) p += static_cast<float>(n) * ti[mt].ai_combat_power;
    return p;
}

bool ConflictManager::can_fight_space(ObjectHandle h) const { return unit_power(h) > 0.0f; }

bool ConflictManager::has_hero(const std::vector<ObjectHandle>& units) const {
    for (ObjectHandle h : units) {
        const uint32_t t = static_cast<uint32_t>(sim_->store().type(h));
        if (rules_->type(t).hero) return true;
        for (const auto& [mt, n] : rules_->type(t).members)
            if (rules_->type(mt).hero) return true;
    }
    return false;
}

// ---- queueing ----

bool ConflictManager::initiate_space_conflict(PlanetIndex planet, int32_t attacker, int32_t defender, std::string map,
                                              bool at_front) {
    if (planet >= sim_->planets().size() || !sim_->planets()[planet].is_planet) return false;
    const PlanetState& p = sim_->planets()[planet];
    if (defender == kNoPlayer) {
        for (uint32_t fi : p.orbit) {
            const FleetState& f = sim_->fleets()[fi];
            if (f.alive && !f.moving && f.owner != attacker && sim_->is_hostile(f.owner, attacker)) {
                defender = f.owner;
                break;
            }
        }
        if (defender == kNoPlayer && p.starbase_level > 0 && !sim_->allied(p.starbase_owner, attacker) && p.starbase_owner != attacker)
            defender = p.starbase_owner;
    }
    if (defender == kNoPlayer || defender == attacker) return false;
    for (const ConflictInfo& c : space_q_)
        if (c.planet == planet && c.attacker == attacker) return false;  // [port] already queued
    ConflictInfo c;
    c.kind = ConflictKind::Space;
    c.planet = planet;
    c.attacker = attacker;
    c.defender = defender;
    c.map = std::move(map);
    if (at_front) space_q_.insert(space_q_.begin(), std::move(c));
    else space_q_.push_back(std::move(c));
    sim_->set_in_battle(planet, true);
    return true;
}

bool ConflictManager::land_invade(PlanetIndex planet, std::vector<ObjectHandle> units, int32_t attacker, int32_t defender,
                                  std::string map, bool at_front) {
    if (planet >= sim_->planets().size() || !sim_->planets()[planet].is_planet) return false;
    const PlanetState& p = sim_->planets()[planet];
    const auto& store = sim_->store();
    if (defender == kNoPlayer) {
        for (ObjectHandle s : p.structures)  // ground-base owner when the base level > 0
            if (store.alive(s)) {
                defender = store.owner(s);
                break;
            }
        if (defender == kNoPlayer && p.owner != kNoPlayer && p.owner != sim_->neutral_player() &&
            (p.owner == attacker || !sim_->allied(p.owner, attacker)))  // own planet: no conflict below; ally: not a defender
            defender = p.owner;
        if (defender == kNoPlayer)
            for (ObjectHandle u : p.surface)
                if (store.alive(u)) {
                    defender = store.owner(u);
                    break;
                }
    }
    if (defender == kNoPlayer || defender == attacker) return false;
    ConflictInfo c;
    c.kind = ConflictKind::Land;
    c.planet = planet;
    c.attacker = attacker;
    c.defender = defender;
    c.units = std::move(units);
    c.map = std::move(map);
    if (at_front) land_q_.insert(land_q_.begin(), std::move(c));
    else land_q_.push_back(std::move(c));
    sim_->set_in_battle(planet, true);
    return true;
}

void ConflictManager::on_space_conflict(PlanetIndex planet, int32_t attacker) { initiate_space_conflict(planet, attacker); }

bool ConflictManager::on_invade(int32_t attacker, PlanetIndex planet, std::span<const ObjectHandle> units) {
    return land_invade(planet, std::vector<ObjectHandle>(units.begin(), units.end()), attacker);
}

void ConflictManager::refresh_in_battle(PlanetIndex planet) {
    bool on = false;
    for (const ConflictInfo& c : space_q_) on = on || c.planet == planet;
    for (const ConflictInfo& c : land_q_) on = on || c.planet == planet;
    sim_->set_in_battle(planet, on);
}

// ---- state machine ----

bool ConflictManager::validate(const ConflictInfo& c) const {
    if (c.attacker == c.defender) return false;
    const PlanetState& p = sim_->planets()[c.planet];
    if (c.kind == ConflictKind::Space) {
        bool att = false, def = false;
        for (ObjectHandle h : orbit_units(c.planet, c.attacker, true)) att = att || can_fight_space(h);
        for (ObjectHandle h : orbit_units(c.planet, c.defender, true)) def = def || can_fight_space(h);
        const bool starbase = p.starbase_level > 0 && p.starbase_owner == c.defender && sim_->store().alive(p.starbase);
        return att && (def || starbase);
    }
    bool att = false, def = false;
    const std::vector<ObjectHandle> cargo = orbit_cargo(c.planet, c.attacker);
    for (ObjectHandle u : c.units) att = att || std::find(cargo.begin(), cargo.end(), u) != cargo.end();
    def = !surface_units(c.planet, c.defender).empty();
    for (ObjectHandle s : p.structures) def = def || (sim_->store().alive(s) && sim_->store().owner(s) == c.defender);
    return att && def;
}

void ConflictManager::pop_head(ConflictKind kind) {
    std::vector<ConflictInfo>& q = queue(kind);
    if (!q.empty()) {
        const PlanetIndex planet = q.front().planet;
        q.erase(q.begin());
        refresh_in_battle(planet);
    }
    state_ = ConflictState::Idle;
    force_ = false;
    retreating_ = kNoPlayer;
    has_choice_ = false;
    choice_ = BattleChoice::Pending;
    ar_in_ = AutoResolveInput{};
    ar_objects_.clear();
    ar_cargo_.clear();
}

void ConflictManager::drop_head(ConflictKind kind, bool log_it) {
    const std::vector<ConflictInfo>& q = queue(kind);
    if (q.empty()) return;
    if (log_it) {
        ConflictLogEntry e;
        e.tick = tick_;
        e.galactic_frame = sim_->frame();
        e.kind = kind;
        e.planet = q.front().planet;
        e.attacker = q.front().attacker;
        e.defender = q.front().defender;
        e.how = 0;
        log_.push_back(std::move(e));
    }
    release_pause();
    pop_head(kind);
}

void ConflictManager::choose(BattleChoice c) {
    if (state_ != ConflictState::Vote || c == BattleChoice::Pending) return;
    has_choice_ = true;
    choice_ = c;
}

void ConflictManager::apply_choice(BattleChoice c, const ConflictInfo& head) {
    switch (c) {
        case BattleChoice::Fight: state_ = ConflictState::StartBattle; break;
        case BattleChoice::Retreat:
            retreating_ = human(head.attacker) ? head.attacker : head.defender;
            force_ = false;
            state_ = ConflictState::AutoSetup;
            break;
        default:  // Auto-Resolve (0x347850): state 6 with the force flag (GOM+0x634), resolves this tick
            force_ = true;  // (GALACTIC_DIALOGS §1.5 / §3.1)
            state_ = ConflictState::AutoSetup;
            break;
    }
}

void ConflictManager::on_service_tail() {
    ++tick_;
    // S10-int (L1/S4 ask): GalacticSim::change_owner on a planet (Lua / story forced transfer, GE 4.2) clears the
    // planet's in_battle flag. Every queued conflict sets it (refresh_in_battle), so a queued conflict on a planet
    // whose flag is clear was cancelled by the transfer: drop it (logged as dropped), except the active head.
    std::vector<PlanetIndex> dropped;
    for (ConflictKind kind : {ConflictKind::Space, ConflictKind::Land}) {
        std::vector<ConflictInfo>& q = queue(kind);
        const size_t keep = (state_ != ConflictState::Idle && active_ == kind) ? 1u : 0u;
        for (size_t i = keep; i < q.size();) {
            const PlanetIndex planet = q[i].planet;
            if (planet >= sim_->planets().size() || sim_->planets()[planet].in_battle) {
                ++i;
                continue;
            }
            ConflictLogEntry e;
            e.tick = tick_;
            e.galactic_frame = sim_->frame();
            e.kind = kind;
            e.planet = planet;
            e.attacker = q[i].attacker;
            e.defender = q[i].defender;
            e.how = 0;
            log_.push_back(std::move(e));
            q.erase(q.begin() + static_cast<std::ptrdiff_t>(i));
            dropped.push_back(planet);
        }
    }
    for (PlanetIndex planet : dropped) refresh_in_battle(planet);  // the active head (if any) keeps its flag
    // Space head first, then the land head; one conflict is active at a time (shared state).
    if (!space_q_.empty() && (state_ == ConflictState::Idle || active_ == ConflictKind::Space)) run(ConflictKind::Space);
    if (!land_q_.empty() && (state_ == ConflictState::Idle || active_ == ConflictKind::Land)) run(ConflictKind::Land);
}

void ConflictManager::run(ConflictKind kind) {
    std::vector<ConflictInfo>& q = queue(kind);
    if (q.empty()) return;
    if (state_ != ConflictState::AutoRunning && state_ != ConflictState::InBattle && !validate(q.front())) {
        drop_head(kind, true);
        return;
    }
    for (int guard = 0; guard < 16; ++guard) {
        const ConflictInfo& head = q.front();
        switch (state_) {
            case ConflictState::Idle:
                active_ = kind;
                state_ = ConflictState::Decide;
                continue;
            case ConflictState::Decide: {
                const bool hum = human(head.attacker) || human(head.defender);
                int32_t overrun_loser = kNoPlayer;
                if (rules_->overrun_multiple != 0.0f) {
                    const bool space = kind == ConflictKind::Space;
                    std::vector<ObjectHandle> au = space ? orbit_units(head.planet, head.attacker, true) : head.units;
                    std::vector<ObjectHandle> du = space ? orbit_units(head.planet, head.defender, true)
                                                         : surface_units(head.planet, head.defender);
                    float ap = 0.0f, dp = 0.0f;
                    for (ObjectHandle h : au)
                        if (sim_->store().alive(h)) ap += unit_power(h);
                    for (ObjectHandle h : du) dp += unit_power(h);
                    const PlanetState& p = sim_->planets()[head.planet];
                    if (space && p.starbase_owner == head.defender && sim_->store().alive(p.starbase))
                        dp += unit_power(p.starbase);
                    const float mx = std::max(ap, dp), mn = std::min(ap, dp);
                    if (mx > mn * rules_->overrun_multiple) {
                        const bool att_weaker = ap < dp;
                        if (!has_hero(att_weaker ? au : du)) overrun_loser = att_weaker ? head.attacker : head.defender;
                    }
                }
                if ((!hum && !cfg_.fight_ai_battles) || overrun_loser != kNoPlayer) {
                    force_ = true;
                    retreating_ = overrun_loser;
                    state_ = ConflictState::AutoSetup;
                    continue;
                }
                if (!hum) {
                    state_ = ConflictState::StartBattle;
                    continue;
                }
                raise_pause();  // the pending-battle vote / dialog (state 0)
                const BattleChoice c = decide ? decide(head) : cfg_.human_choice;
                if (c == BattleChoice::Pending) {
                    has_choice_ = false;
                    state_ = ConflictState::Vote;
                    return;
                }
                apply_choice(c, head);
                continue;
            }
            case ConflictState::Vote:
                if (!has_choice_) return;
                apply_choice(choice_, head);
                continue;
            case ConflictState::Dialog: return;
            case ConflictState::AutoSetup:
                raise_pause();
                auto_setup(head);
                if (ar_in_.combatants.empty()) {
                    drop_head(kind, true);
                    return;
                }
                timer_start_ = tick_;
                state_ = ConflictState::AutoRunning;
                continue;
            case ConflictState::AutoRunning: {
                const double elapsed = static_cast<double>(tick_ - timer_start_) / 30.0;
                if (!force_ && retreating_ == kNoPlayer && elapsed < static_cast<double>(rules_->auto_resolve.display_time)) return;
                ar_in_.retreating = retreating_;
                last_ar_ = auto_resolve(ar_in_, rules_->contrast, rules_->auto_resolve, *rng_);
                auto_finish(ConflictInfo(head));
                return;
            }
            case ConflictState::StartBattle:
                release_pause();
                if (begin_battle(head)) {
                    state_ = ConflictState::InBattle;
                    return;
                }
                force_ = true;  // no tactical host (or refused): auto-resolve instead [port]
                state_ = ConflictState::AutoSetup;
                continue;
            case ConflictState::InBattle: return;
        }
    }
}

// ---- auto-resolve ----

ArCombatant ConflictManager::combatant_of(ObjectHandle h, bool space) const {
    const auto& store = sim_->store();
    const uint32_t t = static_cast<uint32_t>(store.type(h));
    const auto& ti = sim_->rules().types;
    const ConflictTypeInfo& ct = rules_->type(t);
    ArCombatant c;
    c.owner = store.owner(h);
    c.own_power = t < ti.size() ? ti[t].ai_combat_power : 0.0f;
    c.hero = ct.hero;
    c.super_killer = ct.super_killer;
    uint64_t first_cats = ct.categories;
    bool first = true;
    for (const auto& [mt, n] : ct.members) {
        const ConflictTypeInfo& mi = rules_->type(mt);
        ArPiece p;
        p.type = mt;
        p.name = mt < ti.size() ? std::string_view(ti[mt].name) : std::string_view();
        p.categories = mi.categories;
        p.power = static_cast<float>(n) * (mt < ti.size() ? ti[mt].ai_combat_power : 0.0f);
        c.pieces.push_back(p);
        c.hero = c.hero || mi.hero;
        c.super_killer = c.super_killer || mi.super_killer;
        if (first) first_cats = mi.categories;
        first = false;
    }
    if (!(space && !ct.members.empty())) {  // property 5 [M: the container itself in a space battle]
        ArPiece p;
        p.type = t;
        p.name = t < ti.size() ? std::string_view(ti[t].name) : std::string_view();
        p.categories = ct.categories;
        p.power = c.own_power;
        c.pieces.push_back(p);
    }
    c.super = rules_->super_category != 0 && (first_cats & rules_->super_category) != 0;
    return c;
}

void ConflictManager::auto_setup(const ConflictInfo& head) {
    ar_in_ = AutoResolveInput{};
    ar_objects_.clear();
    ar_cargo_.clear();
    const bool space = head.kind == ConflictKind::Space;
    ar_in_.space = space;
    ar_in_.attacker = head.attacker;
    for (int32_t p : {head.defender, head.attacker}) {
        ArSideInfo s;
        s.player = p;
        s.ai = !human(p);
        const uint32_t f = p >= 0 && static_cast<size_t>(p) < sim_->players().size() ? sim_->players()[static_cast<size_t>(p)].faction : kNoIndex;
        s.playable = f < sim_->rules().factions.size() && sim_->rules().factions[f].is_playable;
        ar_in_.players.push_back(s);
    }
    const PlanetState& planet = sim_->planets()[head.planet];
    auto add = [this, space](ObjectHandle h) {
        ar_in_.combatants.push_back(combatant_of(h, space));
        ar_objects_.push_back(h);
    };
    if (space) {
        for (int32_t p : {head.defender, head.attacker})
            for (ObjectHandle h : orbit_units(head.planet, p, true)) add(h);
        if (sim_->store().alive(planet.starbase) && planet.starbase_level > 0 &&
            (planet.starbase_owner == head.defender || planet.starbase_owner == head.attacker)) {
            ArCombatant c = combatant_of(planet.starbase, true);
            uint32_t t = static_cast<uint32_t>(sim_->store().type(planet.starbase));
            for (int guard = 0; guard < 16 && t < sim_->rules().types.size(); ++guard) {
                c.starbase.push_back({t, sim_->rules().types[t].ai_combat_power});
                t = rules_->type(t).lower_starbase;
            }
            ar_in_.combatants.push_back(std::move(c));
            ar_objects_.push_back(planet.starbase);
        }
        for (int32_t p : {head.defender, head.attacker})
            for (ObjectHandle h : orbit_cargo(head.planet, p)) {
                AutoResolveInput::Cargo cg;
                cg.owner = p;
                cg.hero = rules_->type(static_cast<uint32_t>(sim_->store().type(h))).hero;
                ar_in_.cargo.push_back(cg);
                ar_cargo_.push_back(h);
            }
    } else {
        for (ObjectHandle h : surface_units(head.planet, head.defender)) add(h);
        const std::vector<ObjectHandle> cargo = orbit_cargo(head.planet, head.attacker);
        for (ObjectHandle h : head.units)
            if (std::find(cargo.begin(), cargo.end(), h) != cargo.end()) add(h);
        for (ObjectHandle s : planet.structures)  // the planet's base combatant: ground structures
            if (sim_->store().alive(s) && sim_->store().owner(s) == head.defender) add(s);
    }
}

void ConflictManager::auto_finish(const ConflictInfo& head) {
    const AutoResolveResult& r = last_ar_;
    const bool space = head.kind == ConflictKind::Space;
    const PlanetIndex planet = head.planet;
    ConflictLogEntry e;
    e.tick = tick_;
    e.galactic_frame = sim_->frame();
    e.kind = head.kind;
    e.planet = planet;
    e.attacker = head.attacker;
    e.defender = head.defender;
    e.how = 1;
    e.winner = r.winner;
    e.loser = r.loser;
    e.retreating = r.retreating;
    e.rng_draws = r.draws;
    // OI galactic: survival roll listener (STRATEGIC_GAPS 1.1), before the objects are removed.
    if (on_survival_roll)
        for (size_t i = 0; i < ar_objects_.size() && i < r.survives.size(); ++i)
            if (sim_->store().exists(ar_objects_[i]))
                on_survival_roll(ar_in_.combatants[i].owner, static_cast<uint32_t>(sim_->store().type(ar_objects_[i])), r.survives[i] != 0);
    // Destroyed combatants, starbase level.
    for (size_t i = 0; i < ar_objects_.size(); ++i) {
        const ObjectHandle h = ar_objects_[i];
        const bool is_starbase = !ar_in_.combatants[i].starbase.empty();
        if (!r.survives[i]) {
            (ar_in_.combatants[i].owner == head.attacker ? e.attacker_losses : e.defender_losses)++;
            if (is_starbase) sim_->replace_starbase(planet, kNoIndex);
            else sim_->destroy_object(h);
        } else if (is_starbase && sim_->store().alive(h) && r.starbase_type[i] != static_cast<uint32_t>(sim_->store().type(h))) {
            sim_->replace_starbase(planet, r.starbase_type[i]);
        }
    }
    // Loss records (BT §4.4 "Loss records for carried units", 0x60da50): a destroyed transport records one entry
    // per carried unit and none for its hull. The port has no transport objects (auto_resolve.hpp: each carried
    // ground unit is its own one-unit transport, ar_cargo_), and no space combatant carries cargo, so the
    // survival-roll kills above count 1 each and every cargo unit killed by the transport roll counts 1 here.
    for (size_t i = 0; i < ar_cargo_.size(); ++i)
        if (!r.cargo_survives[i]) {
            (ar_in_.cargo[i].owner == head.attacker ? e.attacker_losses : e.defender_losses)++;
            sim_->destroy_object(ar_cargo_[i]);
        }
    if (r.valid && r.winner != kNoPlayer && r.loser != kNoPlayer) {
        if (r.retreating != kNoPlayer) {
            if (!space && r.retreating == head.defender) retreat_ground(r.retreating, planet);
            else retreat_fleets(r.retreating, planet);
        }
        if (r.winner == head.attacker) {
            sim_->clear_queue(planet, space ? kSpaceQueue : kGroundQueue);
            if (!space) land_survivors(head.attacker, planet, head.units);
        }
        post_results(head, r.winner, r.loser, r.retreating, kVictoryAllDestroyed);
        rewrite_defenders(r.loser, r.winner);
    }
    log_.push_back(std::move(e));
    release_pause();
    pop_head(head.kind);
}

// ---- tactical battle ----

void ConflictManager::split_space_forces(int32_t player, const std::vector<ObjectHandle>& units, BattleForces& out) const {
    out.player = player;
    out.start.clear();
    out.reserve.clear();
    const auto& store = sim_->store();
    const uint32_t f = player >= 0 && static_cast<size_t>(player) < sim_->players().size() ? sim_->players()[static_cast<size_t>(player)].faction : kNoIndex;
    float cap = rules_->faction(f).space_unit_cap;
    std::vector<uint8_t> start(units.size(), 0);
    // Containers (squadron / company holders) take one cap slot each, list order [M: slot = 1].
    std::vector<uint32_t> plain;
    for (uint32_t i = 0; i < units.size(); ++i) {
        const uint32_t t = static_cast<uint32_t>(store.type(units[i]));
        if (!rules_->type(t).members.empty()) {
            if (cap >= 1.0f) {
                start[i] = 1;
                cap -= 1.0f;
            }
        } else {
            plain.push_back(i);
        }
    }
    struct Group {
        uint32_t type;
        int32_t pop;
        std::vector<uint32_t> idx;
        size_t left;  // untaken = idx[0, left)
    };
    std::vector<Group> groups;
    for (uint32_t i : plain) {
        const uint32_t t = static_cast<uint32_t>(store.type(units[i]));
        Group* g = nullptr;
        for (Group& x : groups)
            if (x.type == t) g = &x;
        if (!g) {
            const ConflictTypeInfo& ti = rules_->type(t);
            groups.push_back({t, ti.space_population_override >= 0 ? ti.space_population_override : ti.population, {}, 0});
            g = &groups.back();
        }
        g->idx.push_back(i);
        g->left = g->idx.size();
    }
    auto take = [&start](Group& g) {
        --g.left;
        start[g.idx[g.left]] = 1;
    };
    // Pass A.
    for (Group& g : groups) {
        if (g.pop < 1) {
            while (g.left > 0) take(g);
        } else if (static_cast<float>(g.pop) <= cap) {
            take(g);
            cap -= static_cast<float>(g.pop);
        }
    }
    // Pass B.
    size_t n = 0;
    for (const Group& g : groups) n += g.left;
    if (n > 0) {
        int32_t used = 0;
        for (Group& g : groups) {
            if (g.left == 0) continue;
            const float share = static_cast<float>(g.left) / static_cast<float>(n) * cap + 0.5f;
            int32_t k = static_cast<int32_t>(std::floor(share)) / g.pop;
            k = std::min(k, static_cast<int32_t>(g.left));
            for (int32_t j = 0; j < k; ++j) take(g);
            used += k * g.pop;
        }
        cap -= static_cast<float>(used);
    }
    // Pass C.
    for (Group& g : groups)
        while (g.left > 0 && static_cast<float>(g.pop) <= cap) {
            take(g);
            cap -= static_cast<float>(g.pop);
        }
    for (uint32_t i = 0; i < units.size(); ++i) (start[i] ? out.start : out.reserve).push_back(units[i]);
}

// OI galactic: Raid_Force_* test 0x432e20 (TACTICAL_GAPS 4.2), the battle host's reading: required faction, every
// member free or limited (free mask tested first [M]), 1 <= limited <= max, heroes <= max.
bool ConflictManager::raid_force(int32_t player, const std::vector<ObjectHandle>& units) const {
    if (rules_->raid_limited_mask == 0 && rules_->raid_free_mask == 0) return false;  // TR: never a raid
    if (!rules_->raid_required_faction.empty()) {
        if (player < 0 || static_cast<size_t>(player) >= sim_->players().size()) return false;
        const uint32_t f = sim_->players()[static_cast<size_t>(player)].faction;
        if (f >= sim_->rules().factions.size() || !iequal(sim_->rules().factions[f].name, rules_->raid_required_faction)) return false;
    }
    int32_t limited = 0, heroes = 0;
    for (ObjectHandle h : units) {
        const ConflictTypeInfo& ti = rules_->type(static_cast<uint32_t>(sim_->store().type(h)));
        if (ti.hero) ++heroes;
        if ((ti.categories & rules_->raid_free_mask) != 0) continue;
        if ((ti.categories & rules_->raid_limited_mask) != 0) ++limited;
        else return false;
    }
    return limited >= 1 && limited <= rules_->raid_max_limited && heroes <= rules_->raid_max_heroes;
}

void ConflictManager::split_land_forces(const ConflictInfo& head, BattleForces& d, BattleForces& a) const {
    d.player = head.defender;
    d.start = surface_units(head.planet, head.defender);  // every surface object starts, no cap
    d.reserve.clear();
    a.player = head.attacker;
    a.start.clear();
    a.reserve.clear();
    const std::vector<ObjectHandle> cargo = orbit_cargo(head.planet, head.attacker);
    std::vector<ObjectHandle> rec;  // the record's units that are still in orbit, record order
    for (ObjectHandle h : head.units)
        if (std::find(cargo.begin(), cargo.end(), h) != cargo.end()) rec.push_back(h);
    if (rec.empty()) return;
    if (raid_force(head.attacker, rec)) {
        a.start = rec;
        return;
    }
    const auto& gt = sim_->rules().types;
    const auto hero = [&](ObjectHandle h) { return rules_->type(static_cast<uint32_t>(sim_->store().type(h))).hero; };
    const auto name = [&](ObjectHandle h) {
        const uint32_t t = static_cast<uint32_t>(sim_->store().type(h));
        return t < gt.size() ? std::string_view(gt[t].name) : std::string_view();
    };
    ObjectHandle lead;
    for (const std::string& want : rules_->preferred_pathfinder_types) {
        for (int pass = 0; pass < 2 && !lead; ++pass)
            for (ObjectHandle h : rec)
                if (hero(h) == (pass == 1) && iequal(name(h), want)) {
                    lead = h;
                    break;
                }
        if (lead) break;
    }
    for (int pass = 0; pass < 2 && !lead; ++pass)
        for (ObjectHandle h : rec)
            if (hero(h) == (pass == 1)) {
                lead = h;
                break;
            }
    a.start.push_back(lead);
    for (ObjectHandle h : rec)  // the other record units first (record order), then the other transports in orbit
        if (h != lead) a.reserve.push_back(h);
    for (ObjectHandle h : cargo)
        if (std::find(rec.begin(), rec.end(), h) == rec.end()) a.reserve.push_back(h);
}

std::string ConflictManager::choose_map(const ConflictInfo& head, int32_t base_level, bool* ok) {
    *ok = true;
    const bool space = head.kind == ConflictKind::Space;
    const PlanetState& p = sim_->planets()[head.planet];
    std::string name = head.planet < planet_map_.size() ? planet_map_[head.planet] : std::string();
    const ConflictTypeInfo& pt = rules_->type(p.type);
    const std::string& field = space ? pt.space_map : pt.land_map;
    if (!field.empty()) {
        name = field;
    } else if (name.empty()) {
        if (pt.terrain < 0) {
            *ok = false;  // unknown Terrain: the start fails
            return {};
        }
        const uint32_t f = head.defender >= 0 && static_cast<size_t>(head.defender) < sim_->players().size()
                               ? sim_->players()[static_cast<size_t>(head.defender)].faction
                               : kNoIndex;
        if (pick_map) name = pick_map(space, pt.terrain, rules_->faction(f).map_faction, base_level);
        if (head.planet < planet_map_.size()) planet_map_[head.planet] = name;
    }
    if (!head.map.empty()) name = head.map;
    if (name.empty()) name = space ? "_Space_Temperate1.ted" : "_Desert_L5_01.ted";
    return name;
}

bool ConflictManager::start_linked(LinkedBattle b) {
    if (state_ != ConflictState::Idle) return false;
    const bool space = b.kind == ConflictKind::Space;
    const bool queued = space ? initiate_space_conflict(b.planet, b.attacker, b.defender, std::move(b.map), true)
                              : land_invade(b.planet, std::move(b.units), b.attacker, b.defender, std::move(b.map), true);
    if (!queued) return false;
    active_ = b.kind;
    if (begin_battle(queue(b.kind).front(), b.sandbox ? Linked::Sandbox : Linked::Plain)) {
        state_ = ConflictState::InBattle;
        return true;
    }
    drop_head(b.kind, true);
    return false;
}

bool ConflictManager::begin_battle(const ConflictInfo& head, Linked linked) {
    BattleRequest req;
    req.info = head;
    const bool space = head.kind == ConflictKind::Space;
    const PlanetState& p = sim_->planets()[head.planet];
    const auto& store = sim_->store();
    if (space) {
        req.base_level = p.starbase_level;
        if (store.alive(p.starbase) && (p.starbase_owner == head.defender || p.starbase_owner == head.attacker))
            req.starbase = p.starbase;
    } else {
        req.base_level = sim_->ground_base_level(head.planet);
        for (ObjectHandle s : p.structures)
            if (store.alive(s) && store.owner(s) == head.defender) req.structures.push_back(s);
    }
    bool ok = true;
    req.map = choose_map(head, req.base_level, &ok);
    if (!ok) return false;
    if (linked == Linked::Plain) {  // BT §2.9.5: both force maps cleared, no galactic unit enters the battle
        for (int32_t pl : {head.defender, head.attacker}) {
            BattleForces f;
            f.player = pl;
            req.forces.push_back(std::move(f));
        }
    } else if (space) {
        for (int32_t pl : {head.defender, head.attacker}) {
            BattleForces f;
            if (linked == Linked::Sandbox && !human(pl)) f.player = pl;  // §2.9.5 sandbox: AI sides get nothing
            else split_space_forces(pl, orbit_units(head.planet, pl, true), f);
            req.forces.push_back(std::move(f));
        }
    } else {  // OI galactic: TACTICAL_GAPS 4.2 land split (sandbox, BT §2.9.5: humanOnly = 1)
        BattleForces d, a;
        split_land_forces(head, d, a);
        for (BattleForces* f : {&d, &a})
            if (linked == Linked::Sandbox && !human(f->player)) {
                f->start.clear();
                f->reserve.clear();
            }
        req.forces.push_back(std::move(d));
        req.forces.push_back(std::move(a));
    }
    req.land_base_destruction_forces_retreat = rules_->land_base_destruction_forces_retreat;
    req.space_station_destruction_forces_retreat = rules_->space_station_destruction_forces_retreat;
    request_ = std::move(req);
    return start_battle && start_battle(request_);
}

void ConflictManager::finish_battle(const BattleResult& result) {
    if (state_ != ConflictState::InBattle || queue(active_).empty()) return;
    const ConflictInfo head = queue(active_).front();
    const bool space = head.kind == ConflictKind::Space;
    const PlanetIndex planet = head.planet;
    const int32_t A = head.attacker, D = head.defender;
    const int32_t W = result.winner;
    const int32_t L = W == D ? A : D;
    const auto& store = sim_->store();
    ConflictLogEntry e;
    e.tick = tick_;
    e.galactic_frame = sim_->frame();
    e.kind = head.kind;
    e.planet = planet;
    e.attacker = A;
    e.defender = D;
    e.how = 2;
    e.winner = W;
    e.loser = L;
    e.map = request_.map;
    auto player_of = [this](int32_t p) -> const FactionRules* {
        if (p < 0 || static_cast<size_t>(p) >= sim_->players().size()) return nullptr;
        const uint32_t f = sim_->players()[static_cast<size_t>(p)].faction;
        return f < sim_->rules().factions.size() ? &sim_->rules().factions[f] : nullptr;
    };
    // Objects killed in the battle.
    for (ObjectHandle h : result.destroyed) {
        if (!store.alive(h)) continue;
        (store.owner(h) == A ? e.attacker_losses : e.defender_losses)++;
        if (h == sim_->planets()[planet].starbase) sim_->replace_starbase(planet, kNoIndex);
        else sim_->destroy_object(h);
    }
    if (result.starbase_destroyed && store.alive(sim_->planets()[planet].starbase)) {
        (sim_->planets()[planet].starbase_owner == A ? e.attacker_losses : e.defender_losses)++;
        sim_->replace_starbase(planet, kNoIndex);
    }
    // §3.1 step 2: a non-playable loser is wiped from the galaxy.
    const FactionRules* lf = player_of(L);
    if (lf && !lf->is_playable) wipe_player(L);
    // Step 3: retreat player.
    int32_t R = result.retreated;
    if (result.victory == kVictoryEnemyBaseDestroyed) {
        R = D;
    } else if (result.victory == kVictoryStory) {
        R = L;
    } else if (result.victory == kVictoryStarbaseDestroyed) {
        const FactionRules* df = player_of(D);
        if (df && !df->is_playable && !df->is_neutral) {
            wipe_player(D);
            R = kNoPlayer;
        } else {
            R = D;
        }
    }
    // OI galactic (TACTICAL_GAPS 4.8 step 4): a super weapon the winner cannot kill makes the winner retreat.
    if (sim_->super_weapon_forces_retreat(planet, W)) R = W;
    e.retreating = R;
    // §3.2 planet write-back.
    const PlanetState& p = sim_->planets()[planet];
    if (!space && W == A) {
        if (p.starbase_level > 0 && p.starbase_owner != W && store.alive(p.starbase)) sim_->replace_starbase(planet, kNoIndex);
        sim_->reevaluate_queue(planet, kGroundQueue, sim_->ground_base_level(planet));
        sim_->reevaluate_queue(planet, kSpaceQueue, sim_->planets()[planet].starbase_level);
    }
    if (W == A) sim_->clear_queue(planet, space ? kSpaceQueue : kGroundQueue);
    // §3.3 fleets / ground.
    if (!space) {
        if (R != kNoPlayer) {
            if (R == D) retreat_ground(R, planet);
            else retreat_fleets(R, planet);
        }
        if (W == A) land_survivors(A, planet, head.units);
    } else {
        for (int32_t side : {D, A}) {
            if ((R == kNoPlayer && side != W) || (R != kNoPlayer && side == R)) {
                retreat_fleets(side, planet);
                const PlanetState& pp = sim_->planets()[planet];
                if (pp.starbase_owner == side && store.alive(pp.starbase)) sim_->replace_starbase(planet, kNoIndex);
            }
        }
    }
    post_results(head, W, L, R, result.victory);
    rewrite_defenders(L, W);
    log_.push_back(std::move(e));
    pop_head(head.kind);
}

// OI galactic: corruption-mission battle end (TACTICAL_GAPS 4.8, 0x28bf10).
void ConflictManager::finish_corruption_mission(const BattleResult& result) {
    if (state_ != ConflictState::InBattle || queue(active_).empty()) return;
    const ConflictInfo head = queue(active_).front();
    const auto& store = sim_->store();
    ConflictLogEntry e;
    e.tick = tick_;
    e.galactic_frame = sim_->frame();
    e.kind = head.kind;
    e.planet = head.planet;
    e.attacker = head.attacker;
    e.defender = head.defender;
    e.how = 2;
    e.winner = result.winner;
    e.loser = result.winner == head.defender ? head.attacker : head.defender;
    e.map = request_.map;
    for (ObjectHandle h : result.destroyed) {  // [M] the battle's deaths still reach the galaxy
        if (!store.alive(h)) continue;
        (store.owner(h) == head.attacker ? e.attacker_losses : e.defender_losses)++;
        if (h == sim_->planets()[head.planet].starbase) sim_->replace_starbase(head.planet, kNoIndex);
        else sim_->destroy_object(h);
    }
    log_.push_back(std::move(e));
    pop_head(head.kind);
}

// --- P4 sim (owner: galactic agent): Set_Retreating and the tactical auto-resolve (conflicts.hpp) ---
bool ConflictManager::set_retreating(int32_t player) {
    if (queue(active_).empty() || player == kNoPlayer) return false;
    const ConflictInfo& head = queue(active_).front();
    // [port] a player outside the two sides is refused (vf9's early withdrawal only acts for a side, BT §4.6 [M]).
    if (player != head.attacker && player != head.defender) return false;
    // 0x80670003 before Begin, 0x8067000A when a retreat is already set (P4_SIM_SYSTEMS §9.2) [H].
    if (state_ != ConflictState::AutoRunning || retreating_ != kNoPlayer) return false;
    retreating_ = player;
    return true;
}

int32_t ConflictManager::resolve_battle(const BattleResult& so_far) {
    if (state_ != ConflictState::InBattle || queue(active_).empty()) return kNoPlayer;
    const ConflictInfo head = queue(active_).front();
    const auto& store = sim_->store();
    for (ObjectHandle h : so_far.destroyed) {  // the battle's deaths so far
        if (!store.alive(h)) continue;
        if (h == sim_->planets()[head.planet].starbase) sim_->replace_starbase(head.planet, kNoIndex);
        else sim_->destroy_object(h);
    }
    if (so_far.starbase_destroyed && store.alive(sim_->planets()[head.planet].starbase)) sim_->replace_starbase(head.planet, kNoIndex);
    auto_setup(head);
    ar_in_.from_tactical = true;  // +0x3e
    force_ = true;
    ar_in_.retreating = retreating_;
    last_ar_ = auto_resolve(ar_in_, rules_->contrast, rules_->auto_resolve, *rng_);
    const int32_t winner = last_ar_.valid ? last_ar_.winner : head.defender;  // [port] no two sides: the defender holds
    auto_finish(head);
    return winner;
}
// --- end P4 sim ---

// ---- write-back helpers ----

// OI galactic: TACTICAL_GAPS 4.5 finders. filter 0 = 0x2a9dd0 orbit controller, 1 = 0x2aa400 ground-base / starbase owner,
// 2 = 0x2a9ee0 neutral owner [M], 3 = 0x2a8dd0 any planet.
PlanetIndex ConflictManager::nearest_planet(PlanetIndex from, PlanetIndex exclude, int32_t filter, int32_t player) const {
    const auto& planets = sim_->planets();
    const auto& store = sim_->store();
    if (from >= planets.size()) return kInvalidPlanet;
    const Vec3 o = planets[from].position;
    const auto controller = [&](const PlanetState& p) {  // 0x3f9270
        if (p.starbase_level > 0) return p.starbase_owner;
        for (uint32_t f : p.orbit) {
            const FleetState& fl = sim_->fleets()[f];
            if (!fl.alive || fl.moving) continue;
            for (ObjectHandle m : fl.contents)
                if (store.alive(m) && unit_power(m) > 0.0f) return fl.owner;
        }
        return kNoPlayer;  // fleets in transit are not in an orbit list in the port
    };
    const auto ground_owner = [&](const PlanetState& p) {
        for (ObjectHandle s : p.structures)
            if (store.alive(s)) return store.owner(s);
        return kNoPlayer;
    };
    PlanetIndex best = kInvalidPlanet;
    double best_d = 0.0;
    for (size_t k = planets.size(); k-- > 0;) {  // the all-objects list, newest first = reverse creation (row) order
        const PlanetIndex i = static_cast<PlanetIndex>(k);
        const PlanetState& p = planets[i];
        if (!p.is_planet || i == exclude || p.owner == kNoPlayer) continue;
        bool pass = true;
        switch (filter) {
        case 0: pass = controller(p) == player; break;
        case 1: pass = ground_owner(p) == player || p.starbase_owner == player; break;
        case 2: {
            const uint32_t f = sim_->players()[static_cast<size_t>(p.owner)].faction;
            pass = f < sim_->rules().factions.size() && sim_->rules().factions[f].is_neutral;
            break;
        }
        default: break;
        }
        if (!pass) continue;
        const double dx = static_cast<double>(p.position.x) - static_cast<double>(o.x);
        const double dy = static_cast<double>(p.position.y) - static_cast<double>(o.y);
        const double dz = static_cast<double>(p.position.z) - static_cast<double>(o.z);
        const double d = dx * dx + dy * dy + dz * dz;
        if (best == kInvalidPlanet || d < best_d) {
            best = i;
            best_d = d;
        }
    }
    return best;
}

PlanetIndex ConflictManager::retreat_destination(int32_t player, PlanetIndex from, PlanetIndex /*came_from*/) const {
    for (int32_t f = 0; f < 4; ++f) {  // 0x2ba6c0: dd0 -> 400 -> ee0 -> 8dd0, excluding the battle planet
        const PlanetIndex p = nearest_planet(from, from, f, player);
        if (p != kInvalidPlanet) return p;
    }
    return kInvalidPlanet;
}

PlanetIndex ConflictManager::retreat_destination_ground(int32_t player, PlanetIndex from, bool flag) const {
    const int32_t order[4] = {flag ? 1 : 0, flag ? 0 : 1, 2, 3};
    for (int32_t k = 0; k < 4; ++k) {
        const PlanetIndex p = nearest_planet(from, k < 2 ? kInvalidPlanet : from, order[k], player);
        if (p != kInvalidPlanet) return p;
    }
    return kInvalidPlanet;
}

void ConflictManager::retreat_fleets(int32_t player, PlanetIndex planet) {
    const PlanetIndex dest = retreat_destination(player, planet);
    if (dest == kInvalidPlanet) return;
    const std::vector<uint32_t> fleets = sim_->fleets_at(planet, player);
    for (uint32_t f : fleets) {
        if (f >= sim_->fleets().size() || !sim_->fleets()[f].alive || sim_->fleets()[f].moving) continue;
        sim_->move_fleet(player, f, dest);  // stays when it cannot jump / no path [port]
    }
}

void ConflictManager::retreat_ground(int32_t player, PlanetIndex planet) {
    const std::vector<ObjectHandle> units = surface_units(planet, player);
    if (units.empty()) return;
    sim_->launch_units(player, planet, units);  // up into orbit (transports)
    const PlanetIndex dest = retreat_destination_ground(player, planet);
    if (dest == kInvalidPlanet || dest == planet) return;  // the player controls the orbit: the units stay up there
    const uint32_t f = sim_->fleet_containing(units.front());
    if (f != kNoFleet && !sim_->fleets()[f].moving) sim_->move_fleet(player, f, dest);
}

void ConflictManager::wipe_player(int32_t player) {
    const auto& store = sim_->store();
    for (uint32_t f = 0; f < sim_->fleets().size(); ++f)
        if (sim_->fleets()[f].alive && sim_->fleets()[f].owner == player) sim_->destroy_object(sim_->fleets()[f].object);
    for (PlanetIndex pi = 0; pi < sim_->planets().size(); ++pi) {
        const PlanetState& p = sim_->planets()[pi];
        if (!p.is_planet) continue;
        if (store.alive(p.starbase) && p.starbase_owner == player) sim_->replace_starbase(pi, kNoIndex);
        std::vector<ObjectHandle> kill;
        for (ObjectHandle s : p.structures)
            if (store.alive(s) && store.owner(s) == player) kill.push_back(s);
        for (ObjectHandle s : p.surface)
            if (store.alive(s) && store.owner(s) == player) kill.push_back(s);
        for (ObjectHandle s : kill) sim_->destroy_object(s);
    }
}

void ConflictManager::land_survivors(int32_t attacker, PlanetIndex planet, const std::vector<ObjectHandle>& units) {
    const auto& store = sim_->store();
    const PlanetState& p = sim_->planets()[planet];
    for (ObjectHandle s : p.structures)
        if (store.alive(s) && sim_->is_hostile(store.owner(s), attacker)) return;
    for (ObjectHandle s : p.surface)
        if (store.alive(s) && sim_->is_hostile(store.owner(s), attacker)) return;
    std::vector<ObjectHandle> alive;
    for (ObjectHandle u : units)
        if (store.alive(u)) alive.push_back(u);
    if (!alive.empty()) sim_->invade(attacker, planet, alive);
}

void ConflictManager::post_results(const ConflictInfo& head, int32_t winner, int32_t loser, int32_t retreat, uint8_t victory) {
    const int32_t kind = head.kind == ConflictKind::Space ? 2 : 1;
    GalacticEvent w;
    w.kind = GalacticEventKind::BattleWon;
    w.planet = head.planet;
    w.player = winner;
    w.other_player = loser;
    w.value = kind;
    w.type = victory;
    sim_->post_event(w);
    GalacticEvent l;
    l.kind = GalacticEventKind::BattleLost;
    l.planet = head.planet;
    l.player = loser;
    l.other_player = winner;
    l.value = kind;
    sim_->post_event(l);
    if (retreat != kNoPlayer) {
        GalacticEvent r;
        r.kind = GalacticEventKind::BattleRetreat;
        r.planet = head.planet;
        r.player = retreat;
        r.value = kind;
        sim_->post_event(r);
    }
    // OI galactic (STRATEGIC_GAPS 1.4): the battle result reaches the mode tick's super-weapon test (victory 3 / 6).
    if (winner != kNoPlayer) sim_->resolve_battle_super_weapon(head.planet, winner);
}

void ConflictManager::rewrite_defenders(int32_t loser, int32_t winner) {
    for (std::vector<ConflictInfo>* q : {&space_q_, &land_q_})
        for (size_t i = 1; i < q->size(); ++i)  // [0] is the conflict being finished
            if ((*q)[i].defender == loser) (*q)[i].defender = winner;
    // The other queue's head is not the active one.
    std::vector<ConflictInfo>& other = active_ == ConflictKind::Space ? land_q_ : space_q_;
    if (!other.empty() && other.front().defender == loser) other.front().defender = winner;
}

// ---- CRC ----

void ConflictManager::hash(StateHasher& h) const {
    const auto& store = sim_->store();
    auto id_of = [&store](ObjectHandle o) { return store.exists(o) ? store.id(o) : objects::kNoObjectId; };
    for (const std::vector<ConflictInfo>* q : {&space_q_, &land_q_}) {
        h.u32(static_cast<uint32_t>(q->size()));
        for (const ConflictInfo& c : *q) {
            h.u8(static_cast<uint8_t>(c.kind));
            h.u32(c.planet);
            h.i32(c.attacker);
            h.i32(c.defender);
            h.u32(static_cast<uint32_t>(c.units.size()));
            for (ObjectHandle u : c.units) h.i32(id_of(u));
            h.str(c.map);
        }
    }
    h.value(static_cast<int8_t>(state_));
    h.u8(static_cast<uint8_t>(active_));
    h.u64(tick_);
    h.u64(timer_start_);
    h.boolean(force_);
    h.i32(retreating_);
    h.i32(pause_held_);
    h.boolean(has_choice_);
    h.u8(static_cast<uint8_t>(choice_));
    h.u32(static_cast<uint32_t>(planet_map_.size()));
    for (const std::string& m : planet_map_) h.str(m);
    h.u32(static_cast<uint32_t>(log_.size()));
    for (const ConflictLogEntry& e : log_) {
        h.u64(e.tick);
        h.u32(e.planet);
        h.u8(e.how);
        h.i32(e.winner);
        h.i32(e.loser);
        h.i32(e.retreating);
        h.u32(e.attacker_losses);
        h.u32(e.defender_losses);
    }
    last_ar_.hash(h);
}

void ConflictManager::register_crc(CrcRegistry& registry, const std::string& name) const {
    registry.add(name, [this](StateHasher& h) { hash(h); });
}

}  // namespace alamo::galactic
