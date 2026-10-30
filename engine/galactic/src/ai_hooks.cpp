// A2 galactic: the GalacticSim operations and queries the galactic AI (engine/ai) asked for (declared in
// campaign.hpp, "A2 galactic"): income-tick listeners (AI_GALACTIC_2 §3.1), production queries, and the
// FleetManagement event (COMMANDS.md id 8, AI_GALACTIC_2 §5.4) - the one path for fleet form / detach / merge /
// launch / land, used by the galactic HUD's fleet drag (it replaces the P host block of campaign.cpp, same ops 2-5)
// and by the AI through the command stream (execute_direct).
//
// Determinism: serial code, vectors walked in index order, no RNG. Income listeners are not state.
#include <algorithm>

#include "alamo/galactic/campaign.hpp"

namespace alamo::galactic {

commands::EventData fleet_management_event(FleetOp op, uint32_t target, uint32_t fleet, uint32_t unit) {
    commands::EventData e(commands::EventType::FleetManagement);
    e.set("target", target).set("fleet", fleet).set("unit", unit).set("op", static_cast<uint32_t>(op));
    return e;
}

// ---- income ticks --------------------------------------------------------------------------------------------

uint32_t GalacticSim::add_income_hook(IncomeHook hook) {
    const uint32_t id = next_hook_id_++;
    if (next_hook_id_ == 0) next_hook_id_ = 1;
    income_hooks_.emplace_back(id, std::move(hook));
    return id;
}

void GalacticSim::remove_income_hook(uint32_t id) {
    income_hooks_.erase(std::remove_if(income_hooks_.begin(), income_hooks_.end(),
                                       [id](const std::pair<uint32_t, IncomeHook>& h) { return h.first == id; }),
                        income_hooks_.end());
}

uint32_t GalacticSim::add_production_service_hook(std::function<void(PlanetIndex)> hook) {
    const uint32_t id = next_hook_id_++;
    if (next_hook_id_ == 0) next_hook_id_ = 1;
    production_hooks_.emplace_back(id, std::move(hook));
    return id;
}

void GalacticSim::remove_production_service_hook(uint32_t id) {
    production_hooks_.erase(std::remove_if(production_hooks_.begin(), production_hooks_.end(),
                                           [id](const auto& h) { return h.first == id; }),
                            production_hooks_.end());
}

float GalacticSim::last_income(int32_t player) const {
    return valid_player(player) && static_cast<size_t>(player) < last_income_.size() ? last_income_[static_cast<size_t>(player)]
                                                                                       : 0.0f;
}

// ---- production queries ------------------------------------------------------------------------------------------

int32_t GalacticSim::queue_kind(uint32_t type) const {
    return type < rules_->types.size() ? queue_kind_of(rules_->types[type]) : -1;
}

std::span<const BuildEntry> GalacticSim::queue_of(PlanetIndex planet, uint32_t queue) const {
    if (!valid_planet(planet) || queue > 1) return {};
    return planets_[planet].queue[queue];
}

RejectReason GalacticSim::can_enqueue(int32_t player, PlanetIndex planet, uint32_t type) const {
    return check_enqueue(player, planet, type, nullptr);
}

bool GalacticSim::buildable(PlanetIndex planet, int32_t player, uint32_t type) const {
    if (!valid_player(player) || !valid_planet(planet) || type >= rules_->types.size()) return false;
    const TypeInfo& t = rules_->types[type];
    return t.build_cost >= 1 && queue_kind_of(t) >= 0 && build_check(player, planet, type, false).ok;  // OI galactic: SG 1.3
}

std::vector<uint32_t> GalacticSim::buildable(PlanetIndex planet, int32_t player) const {
    std::vector<uint32_t> out;
    if (!valid_player(player) || !valid_planet(planet) || planets_[planet].owner != player) return out;
    for (uint32_t t = 0; t < rules_->types.size(); ++t)
        if (buildable(planet, player, t)) out.push_back(t);
    return out;
}

// ---- MagicPlan spawn (AI_GALACTIC_3 §4.3)-----------------------------------------------------------------------------------

ObjectHandle GalacticSim::spawn_unit(int32_t player, PlanetIndex planet, uint32_t type) {
    if (!valid_player(player) || !valid_planet(planet) || type >= rules_->types.size()) return {};
    const UnitKind k = rules_->types[type].kind;
    if (k != UnitKind::SpaceUnit && k != UnitKind::Squadron && k != UnitKind::GroundUnit && k != UnitKind::GroundStructure &&
        k != UnitKind::StarBase)
        return {};
    last_spawn_ = {};
    if (!create_unit(player, planet, type)) return {};
    return last_spawn_;
}

bool GalacticSim::steal_unit(int32_t player, uint32_t type, PlanetIndex destination, bool include_destination) {
    if (!valid_player(player) || type >= rules_->types.size()) return false;
    for (ObjectHandle h : store_->all_objects()) {
        if (!store_->alive(h) || store_->owner(h) != player || static_cast<uint32_t>(store_->type(h)) != type) continue;
        if (fleet_of(h) != kNoFleet || planet_of(h) != kInvalidPlanet) continue;
        if (!include_destination && destination != kInvalidPlanet && location_of(h) == destination) continue;
        const bool emit = emit_events_;  // Kill 0x391310 with flag 1: silently
        emit_events_ = false;
        const bool done = destroy_object(h);
        emit_events_ = emit;
        return done;
    }
    return false;
}

ObjectHandle GalacticSim::magic_spawn(int32_t player, PlanetIndex planet, uint32_t type, bool steal, bool hero) {
    if (!valid_player(player) || !valid_planet(planet) || type >= rules_->types.size()) return {};
    if (steal || hero) steal_unit(player, type, planet, hero);
    // Step 3: pending delayed-spawn entries of `type` leave every planet's list (D +0x240, P4 6.2 / STRATEGIC_GAPS 1.6),
    // whatever their owner [H as written: the spec filters by type only].
    for (PlanetState& p : planets_) {
        auto& list = p.respawns;
        list.erase(std::remove_if(list.begin(), list.end(), [type](const RespawnEntry& e) { return e.type == type; }), list.end());
    }
    return spawn_unit(player, planet, type);
}

// ---- population (AI_GALACTIC_3 §3.5) -------------------------------------------------------------------------------

bool GalacticSim::population_capped(int32_t player) const {
    if (!valid_player(player)) return false;
    return players_[static_cast<size_t>(player)].human || rules_->constants.ai_uses_galactic_pop_cap;
}

namespace {
template <class F>
void for_each_owned_object(const std::vector<PlanetState>& planets, const std::vector<FleetState>& fleets,
                           const objects::ObjectStore& store, int32_t player, F&& f) {
    // Planet-located objects in row order (surface, structures, starbase), then fleet members (fleet order).
    for (PlanetIndex i = 0; i < planets.size(); ++i) {
        const PlanetState& p = planets[i];
        if (!p.is_planet) continue;
        for (ObjectHandle u : p.surface)
            if (store.alive(u) && store.owner(u) == player) f(u, i);
        for (ObjectHandle s : p.structures)
            if (store.alive(s) && store.owner(s) == player) f(s, i);
        if (store.alive(p.starbase) && store.owner(p.starbase) == player) f(p.starbase, i);
    }
    for (const FleetState& fl : fleets) {
        if (!fl.alive) continue;
        for (ObjectHandle m : fl.contents)
            if (store.alive(m) && store.owner(m) == player) f(m, fl.planet);
    }
}
}  // namespace

int32_t GalacticSim::population_capacity(int32_t player) const {
    if (!valid_player(player)) return 0;
    const bool human = players_[static_cast<size_t>(player)].human;
    const auto usable = [this, human](PlanetIndex p) {
        return human || p >= planets_.size() || !planets_[p].is_planet || rules_->types[planets_[p].type].surface_accessible;
    };
    int32_t cap = faction_of(player).base_population_capacity;
    for (PlanetIndex i = 0; i < planets_.size(); ++i)
        if (planets_[i].is_planet && planets_[i].owner == player && usable(i)) cap += rules_->types[planets_[i].type].population_capacity;
    for_each_owned_object(planets_, fleets_, *store_, player, [&](ObjectHandle h, PlanetIndex at) {
        if (usable(at)) cap += rules_->types[static_cast<size_t>(store_->type(h))].population_capacity;
    });
    return cap;
}

int32_t GalacticSim::population_used(int32_t player, bool include_queued) const {
    if (!valid_player(player)) return 0;
    const bool human = players_[static_cast<size_t>(player)].human;
    int32_t used = 0;
    for_each_owned_object(planets_, fleets_, *store_, player, [&](ObjectHandle h, PlanetIndex at) {
        if (!human && at < planets_.size() && planets_[at].is_planet && !rules_->types[planets_[at].type].surface_accessible) return;
        used += rules_->types[static_cast<size_t>(store_->type(h))].population;
    });
    if (include_queued)
        for (const PlanetState& p : planets_)
            for (const auto& q : p.queue)
                for (const BuildEntry& e : q)
                    if (e.player == player && e.type < rules_->types.size()) used += rules_->types[e.type].population;
    return used;  // (int)(sum + 0.5): the sum of integer Population_Value is already whole
}

int32_t GalacticSim::population_room(int32_t player) const {
    return std::max(population_capacity(player) - population_used(player, true), 0);
}

// ---- terrain (AI_GALACTIC_3 §6) ----------------------------------------------------------------------------------

int32_t GalacticSim::planet_terrain(PlanetIndex planet) const {
    return valid_planet(planet) ? rules_->types[planets_[planet].type].terrain : -1;
}

bool GalacticSim::planet_restricts(PlanetIndex planet, uint32_t type) const {
    if (!valid_planet(planet) || type >= rules_->types.size()) return false;
    const TypeInfo& pt = rules_->types[planets_[planet].type];
    if ((pt.restricted_categories & rules_->types[type].categories) != 0) return true;
    return std::find(pt.restricted_types.begin(), pt.restricted_types.end(), type) != pt.restricted_types.end();
}

double GalacticSim::terrain_effectiveness(PlanetIndex planet, uint32_t type) const {
    if (type >= rules_->types.size()) return 1.0;
    return rules_->terrain_effectiveness.get(planet_terrain(planet), rules_->types[type].movement_class);
}

// ---- FleetManagement -------------------------------------------------------------------------------------------
// Payload per FleetOp (campaign.hpp). Ops 2-5 keep the P host block's rules (galactic HUD drag, replays recorded with
// it stay valid); op 0 (form, never emitted by the HUD) moves a unit into a given fleet or a new one. Fleets keep their
// indices; a fleet emptied by the change dies like merge_fleet's source.
RejectReason GalacticSim::fleet_management(int32_t player, const commands::EventData& ev) {
    if (paused()) return RejectReason::NotAllowed;  // COMMANDS §6.2: ignored while the galactic pause counter is raised
    const auto find = [this](uint32_t id) { return id == commands::kNoObject ? ObjectHandle{} : store_->find(static_cast<objects::ObjectId>(id)); };
    const auto own_idle = [this, player](uint32_t f) {
        return f != kNoFleet && fleets_[f].alive && fleets_[f].owner == player && !fleets_[f].moving && fleets_[f].planet != kInvalidPlanet;
    };
    const ObjectHandle unit = find(ev.u("unit"));
    const uint32_t src = ev.u("fleet") == commands::kNoObject ? kNoFleet : fleet_of(find(ev.u("fleet")));
    // Takes `unit` (or every member when null) out of fleet `f`, carried flags kept; the emptied fleet dies.
    const auto take = [this](uint32_t f, ObjectHandle u, std::vector<ObjectHandle>& out, std::vector<uint8_t>& carried) {
        FleetState& s = fleets_[f];
        for (size_t i = 0; i < s.contents.size();) {
            if (static_cast<bool>(u) && s.contents[i] != u) {
                ++i;
                continue;
            }
            out.push_back(s.contents[i]);
            carried.push_back(i < s.carried.size() ? s.carried[i] : 0);
            s.contents.erase(s.contents.begin() + static_cast<std::ptrdiff_t>(i));
            if (i < s.carried.size()) s.carried.erase(s.carried.begin() + static_cast<std::ptrdiff_t>(i));
        }
        if (s.contents.empty()) kill_fleet(f);
    };
    const auto member_of = [this](uint32_t f, ObjectHandle u) {
        return std::find(fleets_[f].contents.begin(), fleets_[f].contents.end(), u) != fleets_[f].contents.end();
    };
    // The issuer's idle fleet in orbit of `p` holding `u` (orbit order), kNoFleet if none.
    const auto holder_at = [&](PlanetIndex p, ObjectHandle u) {
        for (uint32_t f : planets_[p].orbit)
            if (own_idle(f) && member_of(f, u)) return f;
        return kNoFleet;
    };
    // Playtest 3 t07: the slot `b` asks for a new fleet at its planet (a taken or invalid slot keeps the automatic one).
    const uint32_t want_slot = ev.u("b");
    const auto slot_new_fleet = [this, want_slot](uint32_t nf) {
        if (want_slot < static_cast<uint32_t>(kOrbitSlots)) place_in_slot(nf, static_cast<int32_t>(want_slot));
    };
    const auto new_fleet_conflict = [this, player](PlanetIndex p) {
        if (conflicts_ && orbit_conflict(p, player)) conflicts_->on_space_conflict(p, player);  // Initiate_Space_Conflict
    };
    // Land (0x4f3bd0, galactic): refused while the surface holds Max_Ground_Forces_On_Planet, unless the planet is in
    // battle (the current battle planet [port: the conflict list]).
    const auto surface_full = [this](PlanetIndex p) {
        return !planets_[p].in_battle && surface_forces(p) >= rules_->constants.max_ground_forces_on_planet;
    };
    const auto surface_planet_of = [this](ObjectHandle u) {
        for (PlanetIndex i = 0; i < planets_.size(); ++i)
            if (std::find(planets_[i].surface.begin(), planets_[i].surface.end(), u) != planets_[i].surface.end()) return i;
        return kInvalidPlanet;
    };
    switch (ev.u("op")) {
    case kFleetJoin: {  // op 1: into the fleet holding `fleet` (a fleet, a fleet member), or land when it is a planet
        if (!static_cast<bool>(unit) || !store_->alive(unit) || store_->owner(unit) != player) return RejectReason::InvalidUnit;
        const ObjectHandle where = find(ev.u("fleet"));
        const PlanetIndex land_at = planet_of(where);
        uint32_t dst = fleet_of(where);
        if (dst == kNoFleet && land_at == kInvalidPlanet && store_->alive(where)) dst = fleet_containing(where);
        if (land_at != kInvalidPlanet) {  // land (op 5 rules) from the issuer's idle orbit fleet there
            if (planets_[land_at].owner != player) return RejectReason::InvalidTarget;
            const uint32_t from = holder_at(land_at, unit);
            if (from == kNoFleet || rules_->types[static_cast<size_t>(store_->type(unit))].kind != UnitKind::GroundUnit)
                return RejectReason::InvalidUnit;
            if (surface_full(land_at)) return RejectReason::NotAllowed;
            std::vector<ObjectHandle> moved;
            std::vector<uint8_t> carried;
            take(from, unit, moved, carried);
            for (const ObjectHandle m : moved) planets_[land_at].surface.push_back(m);
            return RejectReason::None;
        }
        if (!own_idle(dst)) return RejectReason::InvalidTarget;
        const PlanetIndex p = fleets_[dst].planet;
        const uint32_t from = holder_at(p, unit);
        if (from == dst) return RejectReason::None;
        if (from != kNoFleet) {
            std::vector<ObjectHandle> moved;
            std::vector<uint8_t> carried;
            take(from, unit, moved, carried);
            for (size_t i = 0; i < moved.size(); ++i) {
                fleets_[dst].contents.push_back(moved[i]);
                fleets_[dst].carried.push_back(carried[i]);
            }
            return RejectReason::None;
        }
        if (surface_planet_of(unit) != p) return RejectReason::InvalidUnit;  // launch (op 4) + join
        planets_[p].surface.erase(std::remove(planets_[p].surface.begin(), planets_[p].surface.end(), unit), planets_[p].surface.end());
        fleets_[dst].contents.push_back(unit);
        fleets_[dst].carried.push_back(0);
        return RejectReason::None;
    }
    case kFleetMoveNew:
    case kFleetMoveNewFlag: {  // ops 6 / 7: the unit as a new fleet, moving toward planet `a`
        const PlanetIndex p = planet_of(find(ev.u("target")));
        const PlanetIndex dest = planet_of(find(ev.u("a")));
        if (p == kInvalidPlanet || dest == kInvalidPlanet) return RejectReason::InvalidTarget;
        if (!static_cast<bool>(unit) || !store_->alive(unit) || store_->owner(unit) != player) return RejectReason::InvalidUnit;
        std::vector<ObjectHandle> moved;
        std::vector<uint8_t> carried;
        const uint32_t from = holder_at(p, unit);
        if (from != kNoFleet) {
            take(from, unit, moved, carried);
        } else if (surface_planet_of(unit) == p) {  // launch first
            planets_[p].surface.erase(std::remove(planets_[p].surface.begin(), planets_[p].surface.end(), unit), planets_[p].surface.end());
            moved.push_back(unit);
            carried.push_back(0);
        } else {
            return RejectReason::InvalidUnit;
        }
        const uint32_t nf = create_fleet(player, p);
        fleets_[nf].contents = std::move(moved);
        fleets_[nf].carried = std::move(carried);
        if (dest == p) {
            slot_new_fleet(nf);
            return RejectReason::None;
        }
        if (want_slot < static_cast<uint32_t>(kOrbitSlots)) {  // the slot it takes on arrival at `dest`
            fleets_[nf].want_planet = dest;
            fleets_[nf].want_slot = static_cast<int8_t>(want_slot);
            slot_choices_ = true;
        }
        const RejectReason moved_ok = move_fleet(player, nf, dest);
        if (moved_ok != RejectReason::None) {  // stays here: the request is void
            fleets_[nf].want_planet = kInvalidPlanet;
            fleets_[nf].want_slot = -1;
        }
        return moved_ok;
    }
    case kFleetSlot: {  // op 8: orbit slot (campaign.hpp; fleet_slots.cpp)
        if (!own_idle(src)) return RejectReason::InvalidUnit;
        const uint32_t slot = ev.u("slot");
        if (slot >= static_cast<uint32_t>(kOrbitSlots)) return RejectReason::InvalidTarget;
        const ObjectHandle where = find(ev.u("a"));
        const PlanetIndex dest = planet_of(where);
        FleetState& fl = fleets_[src];
        if (dest != kInvalidPlanet && dest != fl.planet) {  // the slot to take on arrival at `dest` (before the move order)
            fl.want_planet = dest;
            fl.want_slot = static_cast<int8_t>(slot);
            slot_choices_ = true;
            return RejectReason::None;
        }
        const uint32_t occupant = slot_occupant(fl.planet, player, static_cast<int32_t>(slot), src);
        if (occupant == kNoFleet) {
            place_in_slot(src, static_cast<int32_t>(slot));
            return RejectReason::None;
        }
        // The slot is taken: the fleet merges into the one in it (the HUD's drop on an occupied slot).
        std::vector<ObjectHandle> moved;
        std::vector<uint8_t> carried;
        take(src, ObjectHandle{}, moved, carried);
        for (size_t i = 0; i < moved.size(); ++i) {
            fleets_[occupant].contents.push_back(moved[i]);
            fleets_[occupant].carried.push_back(carried[i]);
        }
        return RejectReason::None;
    }
    case kFleetMerge: {  // the source fleet (or one of its units) into the target fleet
        const uint32_t dst = fleet_of(find(ev.u("target")));
        if (!own_idle(src)) return RejectReason::InvalidUnit;
        if (!own_idle(dst) || dst == src || fleets_[dst].planet != fleets_[src].planet) return RejectReason::InvalidTarget;
        if (static_cast<bool>(unit) && !member_of(src, unit)) return RejectReason::InvalidUnit;
        std::vector<ObjectHandle> moved;
        std::vector<uint8_t> carried;
        take(src, unit, moved, carried);
        for (size_t i = 0; i < moved.size(); ++i) {
            fleets_[dst].contents.push_back(moved[i]);
            fleets_[dst].carried.push_back(carried[i]);
        }
        return RejectReason::None;
    }
    case kFleetForm: {  // the unit joins the destination fleet at the target planet, or a new fleet
        const PlanetIndex p = planet_of(find(ev.u("target")));
        if (p == kInvalidPlanet) return RejectReason::InvalidTarget;
        if (!static_cast<bool>(unit)) return RejectReason::InvalidUnit;
        const uint32_t from = holder_at(p, unit);
        if (from == kNoFleet) return RejectReason::InvalidUnit;
        if (src == kNoFleet) {
            if (ev.u("fleet") != commands::kNoObject) return RejectReason::InvalidTarget;  // a stale destination id
            if (fleets_[from].contents.size() == 1) return RejectReason::None;  // already a fleet of its own
            std::vector<ObjectHandle> moved;
            std::vector<uint8_t> carried;
            take(from, unit, moved, carried);
            const uint32_t nf = create_fleet(player, p);
            fleets_[nf].contents = std::move(moved);
            fleets_[nf].carried = std::move(carried);
            slot_new_fleet(nf);
            new_fleet_conflict(p);  // OI galactic: a new fleet in a hostile orbit starts a space conflict (SG 1.7 op 0)
            return RejectReason::None;
        }
        if (!own_idle(src) || fleets_[src].planet != p) return RejectReason::InvalidTarget;
        if (src == from) return RejectReason::None;
        std::vector<ObjectHandle> moved;
        std::vector<uint8_t> carried;
        take(from, unit, moved, carried);
        for (size_t i = 0; i < moved.size(); ++i) {
            fleets_[src].contents.push_back(moved[i]);
            fleets_[src].carried.push_back(carried[i]);
        }
        return RejectReason::None;
    }
    case kFleetDetach: {  // one unit of an orbiting fleet at the target planet becomes a fleet of its own
        const PlanetIndex p = planet_of(find(ev.u("target")));
        if (p == kInvalidPlanet) return RejectReason::InvalidTarget;
        uint32_t from = src;
        if (from == kNoFleet && static_cast<bool>(unit))
            for (uint32_t f : planets_[p].orbit)
                if (own_idle(f) && member_of(f, unit)) from = f;
        if (!own_idle(from) || fleets_[from].planet != p || !static_cast<bool>(unit) || !member_of(from, unit)) return RejectReason::InvalidUnit;
        if (fleets_[from].contents.size() == 1) return RejectReason::None;  // already alone
        std::vector<ObjectHandle> moved;
        std::vector<uint8_t> carried;
        take(from, unit, moved, carried);
        const uint32_t nf = create_fleet(player, p);
        fleets_[nf].contents = std::move(moved);
        fleets_[nf].carried = std::move(carried);
        slot_new_fleet(nf);
        return RejectReason::None;
    }
    case kFleetLand: {  // ground units of an orbiting fleet onto the issuer's own planet (hostile surfaces: Invade)
        const PlanetIndex p = planet_of(find(ev.u("target")));
        if (p == kInvalidPlanet || planets_[p].owner != player) return RejectReason::InvalidTarget;
        if (!own_idle(src) || fleets_[src].planet != p) return RejectReason::InvalidUnit;
        if (static_cast<bool>(unit) && !member_of(src, unit)) return RejectReason::InvalidUnit;
        std::vector<ObjectHandle> ground;
        for (const ObjectHandle h : fleets_[src].contents)
            if ((!static_cast<bool>(unit) || h == unit) && store_->alive(h) && rules_->types[static_cast<size_t>(store_->type(h))].kind == UnitKind::GroundUnit)
                ground.push_back(h);
        if (ground.empty()) return RejectReason::InvalidUnit;
        if (surface_full(p)) return RejectReason::NotAllowed;  // OI galactic: Max_Ground_Forces_On_Planet (SG 1.7)
        for (const ObjectHandle h : ground) {
            if (surface_full(p)) break;  // one transport at a time: the rest stays in orbit
            std::vector<ObjectHandle> moved;
            std::vector<uint8_t> carried;
            take(src, h, moved, carried);
            for (const ObjectHandle m : moved) planets_[p].surface.push_back(m);
            if (!fleets_[src].alive) break;
        }
        return RejectReason::None;
    }
    case kFleetLaunch: {  // a ground unit of the issuer's planet into the target fleet in orbit (kNoObject: the orbit fleet)
        if (!static_cast<bool>(unit) || !store_->alive(unit) || store_->owner(unit) != player) return RejectReason::InvalidUnit;
        PlanetIndex p = kInvalidPlanet;
        for (PlanetIndex i = 0; i < planets_.size() && p == kInvalidPlanet; ++i)
            if (std::find(planets_[i].surface.begin(), planets_[i].surface.end(), unit) != planets_[i].surface.end()) p = i;
        if (p == kInvalidPlanet) return RejectReason::InvalidUnit;
        const size_t fleets_before = fleets_.size();
        const uint32_t dst = ev.u("target") == commands::kNoObject ? orbit_fleet(player, p) : fleet_of(find(ev.u("target")));
        if (!own_idle(dst) || fleets_[dst].planet != p) return RejectReason::InvalidTarget;
        planets_[p].surface.erase(std::remove(planets_[p].surface.begin(), planets_[p].surface.end(), unit), planets_[p].surface.end());
        fleets_[dst].contents.push_back(unit);
        fleets_[dst].carried.push_back(0);
        launch_reveal(p, unit);  // REMAINING_GAPS 6.1
        if (fleets_.size() != fleets_before) {
            slot_new_fleet(dst);
            new_fleet_conflict(p);  // OI galactic: launched into a new fleet
        }
        return RejectReason::None;
    }
    default:
        return RejectReason::NotAllowed;
    }
}

}  // namespace alamo::galactic
