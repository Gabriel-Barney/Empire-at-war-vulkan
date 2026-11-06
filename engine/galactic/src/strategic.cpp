// OI galactic: the strategic gaps of docs/re/STRATEGIC_GAPS.md (SG) on GalacticSim (declared in campaign.hpp, "OI
// galactic"): fleet speed multiplier M (SG 2.1), company transports (SG 1.7), buildability 0x2804d0 (SG 1.3), galactic
// stealth and detection (SG 2.2) and the event-driven victory claims (SG 1.4).
//
// Determinism: serial code; vectors walked in index order; the store's object list only for order-independent counts;
// the only RNG consumer is detect_stealthed (one synced draw per qualifying candidate, candidate order).
#include <algorithm>
#include <iterator>

#include "alamo/galactic/campaign.hpp"

namespace alamo::galactic {

namespace {

bool contains(const std::vector<uint32_t>& v, uint32_t x) { return std::find(v.begin(), v.end(), x) != v.end(); }

}  // namespace

commands::EventData fleet_management_event(FleetOp op, uint32_t target, uint32_t fleet, uint32_t unit, uint32_t a,
                                           uint32_t slot) {
    commands::EventData e = fleet_management_event(op, target, fleet, unit);
    e.set("a", a).set("slot", slot);
    return e;
}

// ---- SG 2.1 ----------------------------------------------------------------------------------------------------------

float GalacticSim::fleet_speed_multiplier() const {
    // M = D x mode+0x234 x GMM+0x98: D (debug game speed) and GMM+0x98 are 1.0 in release play [H]; mode+0x234 is
    // Object_Max_Speed_Multiplier_Galactic (FoC 5.0).
    // Measured 2026-10-29 (G1 hop timing, tools/probe_mod/g1_hops/): the original's fleets (Q2 reference trace of retail FoC
    // 1.121) move 0.525 units per tick on an unrouted hop and 2.625 on an HS 5 route (Hyperspace_Speed 1.5 x 0.07 x M x R), so
    // M = 5.0, and the original's Corellia -> Kuat hop (72.1 units) takes 26 ticks (0.87 s at 30 ticks/s). M = 1.0, which
    // this port has used since the 2026-10-28 playtest ("fleets far too fast"), gives 136 ticks (4.5 s).
    // Gabriel (2026-10-29): parity with the base game at 30 ticks/s, so the mode multiplier is applied (reverses the
    // 2026-10-28 playtest tweak). The shared goldens (alamo_game_smoke_*_golden, alamo_headless_galactic_ai*,
    // alamo_headless_skirmish_ai_foc*, galactic_foc_crc) moved with it: fleet positions / AI travel estimates.
    constexpr bool kApplyModeSpeedMultiplier = true;
    const float debug_speed = 1.0f, mode_scale = kApplyModeSpeedMultiplier ? rules_->constants.fleet_speed_multiplier : 1.0f,
                manager_scale = 1.0f;
    return debug_speed * mode_scale * manager_scale;
}

// ---- SG 1.7: company transports ------------------------------------------------------------------------------------------

uint32_t GalacticSim::travel_type(ObjectHandle unit) const {
    // The slot's type even for a stale handle (as ObjectStore::type), like the former direct type reads.
    const uint32_t t = static_cast<uint32_t>(store_->type(unit));
    if (t >= rules_->types.size()) return t;
    const TypeInfo& ti = rules_->types[t];
    if (ti.company_transport < rules_->types.size() && (ti.dummy == kDummyGroundCompany || ti.kind == UnitKind::GroundUnit))
        return ti.company_transport;
    return t;
}

uint32_t GalacticSim::representative_type(ObjectHandle object) const {
    // Port model (campaign.hpp): squadron / company objects carry the squadron / company type, a fleet object its own.
    return store_->exists(object) ? static_cast<uint32_t>(store_->type(object)) : kNoIndex;
}

bool GalacticSim::is_company_transport(ObjectHandle unit) const {
    if (!store_->exists(unit)) return false;
    const uint32_t t = static_cast<uint32_t>(store_->type(unit));
    return t < rules_->types.size() && travel_type(unit) != t;
}

int32_t GalacticSim::surface_forces(PlanetIndex planet) const {
    if (!valid_planet(planet)) return 0;
    int32_t n = 0;
    for (ObjectHandle u : planets_[planet].surface) n += store_->alive(u) ? 1 : 0;
    return n;
}

bool GalacticSim::hostile_visible_in_orbit(PlanetIndex planet, int32_t player) const {
    for (uint32_t f : planets_[planet].orbit)
        if (fleets_[f].alive && hostile(fleets_[f].owner, player) && !hidden(fleets_[f].object)) return true;
    return false;
}

// 0x400f30 for a company with a Company_Transport_Unit: the transport lands when it can, else a new Galactic_Fleet.
bool GalacticSim::place_company(int32_t player, PlanetIndex planet, uint32_t type) {
    PlanetState& p = planets_[planet];
    const TypeInfo& t = rules_->types[type];
    const bool accessible = surface_accessible(planet);  // 0x375220 (destroyed -> false)
    const bool room = surface_forces(planet) < rules_->constants.max_ground_forces_on_planet;
    bool orbit_member = t.always_spawn_in_orbit;
    for (uint32_t m : t.company_units) orbit_member = orbit_member || (m < rules_->types.size() && rules_->types[m].always_spawn_in_orbit);
    const bool stealthed = special().type(type).galactic_stealth;
    const bool force_orbit = orbit_member && (stealthed || !hostile_visible_in_orbit(planet, player));  // [M]
    if (accessible && room && !force_orbit) {
        p.surface.push_back(spawn(player, planet, type, {}));
        return true;
    }
    const ObjectHandle h = spawn(player, planet, type, {});  // the transport first, then its fleet
    const uint32_t f = create_fleet(player, planet);
    fleets_[f].contents.push_back(h);
    fleets_[f].carried.push_back(0);
    last_spawn_ = h;
    return true;
}

// ---- SG 1.3: buildability --------------------------------------------------------------------------------------------------

int32_t GalacticSim::queued_count(int32_t player, uint32_t type) const {
    int32_t n = 0;
    for (const PlanetState& p : planets_)
        for (const auto& q : p.queue)
            for (const BuildEntry& e : q) n += (e.player == player && e.type == type) ? 1 : 0;
    return n;
}

void GalacticSim::count_build(int32_t player, uint32_t type) {
    if (!valid_player(player) || type >= rules_->types.size()) return;
    // 0x2821b0 / 0x281c70: the type and its company members, recursively (depth-limited) [M for the member walk].
    std::vector<uint32_t> stack{type};
    for (int32_t guard = 0; !stack.empty() && guard < 64; ++guard) {
        const uint32_t t = stack.back();
        stack.pop_back();
        auto& built = players_[static_cast<size_t>(player)].built;
        auto it = std::find_if(built.begin(), built.end(), [t](const auto& e) { return e.first == t; });
        if (it == built.end()) built.emplace_back(t, 1);
        else ++it->second;
        if (t < rules_->types.size())
            for (size_t k = rules_->types[t].company_units.size(); k-- > 0;) stack.push_back(rules_->types[t].company_units[k]);
    }
}

int32_t GalacticSim::built_count(int32_t player, uint32_t type) const {
    if (!valid_player(player)) return 0;
    for (const auto& e : players_[static_cast<size_t>(player)].built)
        if (e.first == type) return e.second;
    return 0;
}

int32_t GalacticSim::live_count(int32_t player, uint32_t type) const {
    if (!valid_player(player)) return 0;
    int32_t n = 0;
    for (ObjectHandle h : store_->all_objects()) {
        if (!store_->alive(h) || store_->owner(h) != player) continue;
        const uint32_t t = static_cast<uint32_t>(store_->type(h));
        if (t == type) ++n;
        else if (t < rules_->types.size() && contains(rules_->types[t].company_units, type)) ++n;
    }
    return n;
}

int32_t GalacticSim::max_tech_level(int32_t player) const {
    return valid_player(player) ? players_[static_cast<size_t>(player)].max_tech_level : 0;
}

void GalacticSim::set_max_tech_level(int32_t player, int32_t level) {
    if (valid_player(player)) players_[static_cast<size_t>(player)].max_tech_level = level;
}

BuildCheck GalacticSim::build_check(int32_t player, PlanetIndex planet, uint32_t type, bool check_credits) const {
    BuildCheck r;
    const auto fail = [&r](int32_t step, int32_t reason = kBuildOk, uint32_t detail = kNoIndex) {
        r.ok = false;
        r.step = step;
        r.reason = reason;
        r.detail = detail;
        return r;
    };
    // 1. type and planet exist, the planet produces, not a cinematic object.
    if (!valid_player(player) || !valid_planet(planet) || type >= rules_->types.size()) return fail(1);
    const TypeInfo& t = rules_->types[type];
    const PlanetState& p = planets_[planet];
    const TypeInfo& pt = rules_->types[p.type];
    const PlayerState& ps = players_[static_cast<size_t>(player)];
    if (t.cinematic_only) return fail(1);
    // 2. Affiliation lists the player's faction (hand-built rules: an empty list = any faction [port]).
    if ((t.data_rules || !t.affiliation.empty()) && !contains(t.affiliation, ps.faction)) return fail(2);
    // 3. planet-adjusted cost >= 1 and build time >= 0.
    const int32_t cost = build_cost_for(player, planet, type);
    if (cost < 1 || build_time_for(player, planet, type) < 0.0f) return fail(3);
    // 4. not a generic ground base.
    if (t.dummy == kDummyGroundBase) return fail(4);
    // 5. ground- or space-buildable.
    const bool ground_ok = t.required_ground_base != 0 || is_ground_structure_type(t);
    const bool space_ok = t.required_star_base != 0 || (is_star_base_type(t) && t.base_level == 1);
    if (!ground_ok && !space_ok) return fail(5);
    // 6. credits (Is_Human players; the free-build cheat global is not modelled).
    if (check_credits && ps.human && ps.account.credits < static_cast<float>(cost)) return fail(6);
    // 7. a tech upgrade needs the player at exactly its Tech_Level and a max tech covering the advance.
    if (t.advances_tech != 0 && (ps.tech_level != t.tech_level || ps.max_tech_level < t.advances_tech)) return fail(7);
    // 8. Lock_Tech list. 9. Build_Initially_Locked.
    if (type_locked(player, type)) return fail(8);
    if (initial_build_locks_ && t.build_initially_locked && !type_unlocked(player, type)) return fail(9, kBuildInitiallyLocked);
    // 10. the planet is the player's.
    if (p.owner != player) return fail(10);
    // 11. not a company / squadron member, not a transport.
    if (t.company_member || t.squadron_member || t.transport_behavior) return fail(11);
    // 12. planet restrictions.
    if (planet_restricts(planet, type)) return fail(12, kBuildPlanetRestricted);
    // 13. space-buildable: the starbase is nobody's or the player's.
    if (space_ok && p.starbase_owner != kNoPlayer && p.starbase_owner != player) return fail(13);
    // 14. Required_Special_Structures OR chains, on this planet (any chain member present; unresolvable passes).
    const auto present = [&](uint32_t req) {
        if (req == kNoIndex) return true;
        for (ObjectHandle s : p.structures)
            if (store_->alive(s) && static_cast<uint32_t>(store_->type(s)) == req) return true;
        return false;
    };
    if (t.data_rules || !t.required_special_groups.empty()) {
        for (const std::vector<uint32_t>& chain : t.required_special_groups)
            if (!chain.empty() && !std::any_of(chain.begin(), chain.end(), present))
                return fail(14, kBuildRequiredStructure, chain.front());
    } else {
        for (uint32_t req : t.required_special_structures)  // hand-built rules: a plain AND list
            if (!present(req)) return fail(14, kBuildRequiredStructure, req);
    }
    // 15. Required_Orbiting_Units: alive among the units in the planet's orbit (fleet members; transports and squadron
    // containers are single objects in the port).
    for (uint32_t req : t.required_orbiting_units) {
        bool found = false;
        for (uint32_t f : p.orbit) {
            if (!fleets_[f].alive) continue;
            for (ObjectHandle m : fleets_[f].contents) found = found || (store_->alive(m) && static_cast<uint32_t>(store_->type(m)) == req);
        }
        if (!found) return fail(15, kBuildRequiredStructure, req);
    }
    // 16. tech level (plain >=). 17. base levels.
    if (ps.tech_level < t.tech_level) return fail(16, kBuildTechLevel, static_cast<uint32_t>(t.tech_level));
    if (ground_base_level(planet) < t.required_ground_base)
        return fail(17, kBuildGroundBase, static_cast<uint32_t>(t.required_ground_base));
    if (p.starbase_level < t.required_star_base) return fail(17, kBuildStarBase, static_cast<uint32_t>(t.required_star_base));
    // 18. structure slots: structures on the surface (0 when there is no ground base; orbital: 0 when there is no starbase)
    // + queued items of the same family (0x5F / 0x60, both queues) < the planet type's slots (STRATEGIC_GAPS §1.3 step 18).
    const auto family_count = [&](bool orbital) {
        int32_t n = 0;
        if (orbital ? p.starbase_level > 0 : ground_base_level(planet) > 0)
            for (ObjectHandle s : p.structures)
                if (store_->alive(s)) {
                    const TypeInfo& st = rules_->types[static_cast<size_t>(store_->type(s))];
                    n += (orbital ? is_orbital_structure_type(st) : is_ground_structure_type(st)) ? 1 : 0;
                }
        for (const auto& q : p.queue)
            for (const BuildEntry& e : q)
                n += (orbital ? is_orbital_structure_type(rules_->types[e.type]) : is_ground_structure_type(rules_->types[e.type])) ? 1 : 0;
        return n;
    };
    if (is_ground_structure_type(t) && pt.ground_structure_slots >= 0 && family_count(false) >= pt.ground_structure_slots) return fail(18);
    if (is_orbital_structure_type(t) && pt.orbital_structure_slots >= 0 && family_count(true) >= pt.orbital_structure_slots) return fail(18);
    // 19. Required_Planets.
    if (!t.required_planets.empty() && !contains(t.required_planets, p.type)) return fail(19, kBuildRequiredPlanet);
    // 20. starbases: exactly the next level within Max_Space_Base, and no hostile visible fleet in orbit.
    if (is_star_base_type(t)) {
        const int32_t cur = p.starbase_level, max_level = pt.max_space_base;
        const int32_t next = cur == 0 ? (max_level != 0 ? 1 : 0) : (cur != 5 && cur <= max_level ? cur + 1 : 0);
        if (t.base_level != next || max_level < next) return fail(20);
        for (uint32_t f : p.orbit)
            if (fleets_[f].alive && fleets_[f].owner != player && hostile(fleets_[f].owner, player) && !hidden(fleets_[f].object))
                return fail(20, kBuildHostileFleet);
    }
    // 21. Build_Max_Instances_Per_Planet (structures): on the planet + queued here; orbital ones need a starbase.
    if ((is_ground_structure_type(t) || is_orbital_structure_type(t)) && t.max_instances_per_planet >= 0) {
        int32_t n = 0;
        for (ObjectHandle s : p.structures) n += (store_->alive(s) && static_cast<uint32_t>(store_->type(s)) == type) ? 1 : 0;
        for (const auto& q : p.queue)
            for (const BuildEntry& e : q) n += e.type == type ? 1 : 0;
        if (n >= t.max_instances_per_planet) return fail(21, kBuildLimit);
    }
    if (is_orbital_structure_type(t) && p.starbase_level < 1) return fail(21);
    // 22. lifetime limit. 23. current limit (live + queued).
    if (t.limit_lifetime == 0 || (t.limit_lifetime > 0 && built_count(player, type) >= t.limit_lifetime)) return fail(22);
    if (t.limit_current == 0) return fail(23);
    if (t.limit_current > 0 && live_count(player, type) + queued_count(player, type) >= t.limit_current) return fail(23, kBuildLimit);
    // 24. surface access for ground bases / companies / ground structures (generic heroes excepted), initial placement
    // (the planet's initial-placement list +0x110 is not modelled: such types are never buildable [port]).
    const bool accessible = surface_accessible(planet);  // 0x375220 [H]: planet data +0x2C8 (destroyed) -> false
    const bool needs_surface = t.dummy != 0 ? (t.dummy == kDummyGroundBase || t.dummy == kDummyGroundCompany || t.dummy == kDummyGroundStructure)
                                            : (t.kind == UnitKind::GroundUnit || t.kind == UnitKind::GroundStructure);
    if (!(accessible || !needs_surface || t.generic_hero)) return fail(24);
    if (t.requires_initial_placement) return fail(24);
    r.ok = true;
    return r;
}

// ---- SG 2.2: galactic stealth --------------------------------------------------------------------------------------------

void GalacticSim::oi_on_spawn(ObjectHandle h) {
    if (!store_->exists(h)) return;
    if (special().type(static_cast<uint32_t>(store_->type(h))).galactic_stealth) set_galactic_stealth(h, true);
}

void GalacticSim::set_galactic_stealth(ObjectHandle object, bool on) {
    if (!store_->exists(object)) return;
    const int32_t id = static_cast<int32_t>(store_->id(object));
    const auto it = std::lower_bound(stealthed_.begin(), stealthed_.end(), id);
    const bool has = it != stealthed_.end() && *it == id;
    if (on && !has) stealthed_.insert(it, id);
    if (!on && has) stealthed_.erase(it);
}

bool GalacticSim::galactic_stealth(ObjectHandle object) const {
    if (!store_->exists(object)) return false;
    const int32_t id = static_cast<int32_t>(store_->id(object));
    return std::binary_search(stealthed_.begin(), stealthed_.end(), id);
}

bool GalacticSim::hidden(ObjectHandle object) const {
    if (!store_->alive(object)) return true;  // being removed
    const uint32_t f = fleet_of(object);
    if (f != kNoFleet) {
        for (ObjectHandle m : fleets_[f].contents)
            if (!hidden(m)) return false;
        return true;
    }
    return galactic_stealth(object);
}

int32_t GalacticSim::detect_stealthed(ObjectHandle detector, float chance) {
    if (!store_->alive(detector)) return 0;
    const PlanetIndex at = location_of(detector);
    if (!valid_planet(at)) return 0;
    const int32_t owner = store_->owner(detector);
    const TypeSpecial& dt = special().type(static_cast<uint32_t>(store_->type(detector)));
    std::vector<ObjectHandle> cand;
    for (uint32_t f : planets_[at].orbit)
        if (fleets_[f].alive)
            for (ObjectHandle m : fleets_[f].contents)
                if (store_->alive(m) && galactic_stealth(m)) cand.push_back(m);
    for (ObjectHandle u : planets_[at].surface)
        if (store_->alive(u) && galactic_stealth(u)) cand.push_back(u);
    int32_t found = 0;
    for (ObjectHandle c : cand) {
        if (allied(owner, store_->owner(c))) continue;
        const TypeSpecial& ct = special().type(static_cast<uint32_t>(store_->type(c)));
        if (dt.generic_hero && ct.named_hero) continue;
        const bool discovered = (dt.named_hero && ct.generic_hero) || rng_->range_float(0.0f, 1.0f) < chance;
        if (!discovered) continue;
        set_galactic_stealth(c, false);
        ++found;
    }
    return found;
}

// ---- REMAINING_GAPS 6.3 (corrected 2026-10-28): planet-info display queries (pure) ------------------------------------

namespace {
enum InfoGate : uint8_t { kGateNone = 0, kGateStarbase = 1, kGateGround = 2, kGateBoth = 3 };
struct InfoQuery {
    bool bypass;    // step 1 corruption bypass
    uint8_t gate;   // step 2 stealth gate(s)
    uint32_t bits;  // step 3: any of these bits
};
// Indexed by GalacticSim::PlanetInfo.
constexpr InfoQuery kInfoQueries[] = {
    {true, kGateStarbase, 0x2},          // StarbaseLevel 0x50c420 (+ fallback)
    {true, kGateGround, 0x4},            // GroundBaseLevel 0x50b840 (+ fallback)
    {true, kGateStarbase, 0x40},         // FleetContents 0x50b3f0
    {true, kGateStarbase, 0x10 | 0x20},  // FleetPresence 0x50b4c0 (0x10, or NumFleets)
    {true, kGateStarbase, 0x20},         // NumFleets 0x50b5a0 (rest not traced)
    {true, kGateGround, 0x100},          // GroundCompanyContents 0x50b970
    {true, kGateGround, 0x80 | 0x100},   // NumGroundCompanies 0x50ba40
    {false, kGateStarbase, 0x10000},     // MostPowerfulShip 0x50bb10
    {true, kGateBoth, 0x200},            // SpecialStructures 0x50c300
    {true, kGateNone, 0x400 | 0x800},    // PoliticalControl 0x50c270
    {false, kGateNone, 0x8 | 0x8000},    // CreditIncome 0x50b2a0
    {false, kGateNone, 0x8000},          // CreditIncomeBreakdown 0x50b2d0
    {true, kGateNone, 0x2000},           // MinorStealthHeroes 0x50b370
    {true, kGateNone, 0x4000},           // MajorStealthHeroes 0x50b2f0
    {false, kGateNone, 0x20000},         // ForceSensitiveHeroes 0x50b820
};
}  // namespace

int32_t GalacticSim::groundbase_owner(PlanetIndex planet) const {
    for (ObjectHandle s : planets_[planet].structures)
        if (store_->alive(s)) return store_->owner(s);
    return kNoPlayer;
}

bool GalacticSim::can_see_planet_info(int32_t player, PlanetIndex planet, PlanetInfo what) const {
    const auto qi = static_cast<size_t>(what);
    if (!valid_player(player) || !valid_planet(planet) || qi >= std::size(kInfoQueries)) return false;
    const InfoQuery& q = kInfoQueries[qi];
    const PlanetState& p = planets_[planet];
    if (q.bypass && p.corruption != kCorruptionNone && benefits(player)) return true;  // 1.
    const TypeInfo* pt = p.type < rules_->types.size() ? &rules_->types[p.type] : nullptr;
    // 2. stealth gates (0x2823c0 [M]: allied with the base owner).
    if ((q.gate & kGateStarbase) != 0 && pt && pt->starbase_stealth && p.starbase_owner != kNoPlayer &&
        !allied(player, p.starbase_owner))
        return false;
    if ((q.gate & kGateGround) != 0 && pt && pt->groundbase_stealth) {
        const int32_t go = groundbase_owner(planet);
        if (go != kNoPlayer && !allied(player, go)) return false;
    }
    if ((visibility_mask(planet, player) & q.bits) != 0) return true;  // 3. (0x50c550)
    // Base-level fallback (0x50c420 / 0x50b840): allied with the base owner, or faction +0x20ac <= the base level [M].
    if (what == PlanetInfo::StarbaseLevel || what == PlanetInfo::GroundBaseLevel) {
        const bool sb = what == PlanetInfo::StarbaseLevel;
        const int32_t owner = sb ? p.starbase_owner : groundbase_owner(planet);
        const int32_t level = sb ? p.starbase_level : ground_base_level(planet);
        if (owner != kNoPlayer && allied(player, owner)) return true;
        if (faction_of(player).info_base_level_threshold <= level) return true;
    }
    return false;
}

bool GalacticSim::can_see_starbase_info(int32_t player, PlanetIndex planet) const {
    return can_see_planet_info(player, planet, PlanetInfo::StarbaseLevel);
}

bool GalacticSim::can_see_groundbase_info(int32_t player, PlanetIndex planet) const {
    return can_see_planet_info(player, planet, PlanetInfo::GroundBaseLevel);
}

// ---- SG 1.4: event-driven victories ------------------------------------------------------------------------------------

void GalacticSim::claim_victory(int32_t player, uint8_t type, int32_t countdown) {
    if (!valid_player(player)) return;
    for (const VictoryClaim& c : claims_)
        if (c.player == player && c.type == type) return;
    VictoryClaim c;
    c.player = player;
    c.type = type;
    c.frame = frame_;
    c.countdown = countdown;
    claims_.push_back(c);
}

bool GalacticSim::story_victory(int32_t player, bool short_silent) {
    if (!valid_player(player)) return false;
    victory_hold_ = 0;  // 0x343f80(..., 0): every level, no replay
    claim_victory(player, kVictoryStoryClaim, short_silent ? 30 : 210);
    return true;
}

ObjectHandle GalacticSim::super_weapon_at(PlanetIndex planet) const {
    if (!valid_planet(planet)) return {};
    for (uint32_t f : planets_[planet].orbit) {
        if (!fleets_[f].alive) continue;
        for (ObjectHandle m : fleets_[f].contents)
            if (store_->alive(m) && rules_->types[static_cast<size_t>(store_->type(m))].planet_destruction) return m;
    }
    return {};
}

bool GalacticSim::owns_super_weapon_killer(int32_t player) const {
    if (!valid_player(player)) return false;
    for (ObjectHandle h : store_->all_objects()) {
        if (!store_->alive(h) || store_->owner(h) != player) continue;
        const uint32_t t = static_cast<uint32_t>(store_->type(h));  // fleets / planets: no type
        if (t < rules_->types.size() && rules_->types[t].super_weapon_killer) return true;
    }
    return false;
}

bool GalacticSim::super_weapon_forces_retreat(PlanetIndex planet, int32_t winner) const {
    const ObjectHandle w = super_weapon_at(planet);
    return store_->alive(w) && valid_player(winner) && store_->owner(w) != winner && !owns_super_weapon_killer(winner);
}

bool GalacticSim::resolve_battle_super_weapon(PlanetIndex planet, int32_t winner) {
    const ObjectHandle w = super_weapon_at(planet);
    if (!store_->alive(w)) return false;
    const int32_t owner = store_->owner(w);
    if (valid_player(winner) && owner != winner) {
        if (owns_super_weapon_killer(winner)) {
            destroy_object(w);         // Kill 0x391310
            claim_victory(winner, 3, 5);  // Galactic_Super_Weapon_Destruction, countdown 5
            return true;
        }
    }
    // 6: the weapon's owner's playable enemies own no undestroyed planet.
    if (!valid_player(owner)) return false;
    bool enemies = false, holds = false;
    for (size_t q = 0; q < players_.size(); ++q) {
        const int32_t pq = static_cast<int32_t>(q);
        if (!hostile(owner, pq) || !faction_of(pq).is_playable) continue;
        enemies = true;
        for (const PlanetState& p : planets_) holds = holds || (p.is_planet && p.owner == pq && !p.destroyed);
    }
    if (enemies && !holds) claim_victory(owner, 6, 30);
    return false;
}

bool GalacticSim::super_weapon_leader_hit(PlanetIndex planet) const {
    // TSW_PLANET_DESTROY §6 steps 2-4: victim = D +0x6c (the owner); its Faction_Leader among the team members
    // (0x397bf0, object +0xc8 = Company_Units here) of an object in D +0xe0 (the surface list). Members only.
    // refix t06 [port]: a bare ground unit on the surface (a type without Company_Units) stands for a one-unit transport
    // whose cargo is itself (as the auto-resolve cargo model, conflicts.cpp), so its own type is its member list. A
    // company object's own type is still not compared (the original compares members only).
    // [M] kept: members one level deep (no nested teams); orbit fleets do not count; exact type match only (a variant
    // such as Tyber_Zann_Passenger in Tyber_Zann_Team_No_Peacebringer is not the leader Tyber_Zann); the condition's
    // optional leader-name list (§6 step 3) is not modelled.
    if (!valid_planet(planet)) return false;
    const int32_t victim = planets_[planet].owner;
    if (!valid_player(victim)) return false;
    const uint32_t leader = faction_of(victim).leader;
    if (leader == kNoIndex) return false;
    for (ObjectHandle u : planets_[planet].surface) {
        if (!store_->alive(u)) continue;
        const uint32_t t = static_cast<uint32_t>(store_->type(u));
        if (t >= rules_->types.size()) continue;
        const std::vector<uint32_t>& members = rules_->types[t].company_units;
        if (members.empty() ? t == leader : contains(members, leader)) return true;
    }
    return false;
}

bool GalacticSim::super_weapon_destroy_planet(PlanetIndex planet, int32_t attacker) {
    // 0x707670: leader test (no name list) -> claim 7, then the shared routine only (+ the visual refresh, presentation).
    if (!valid_planet(planet) || planets_[planet].destroyed) return false;
    if (valid_player(attacker) && super_weapon_leader_hit(planet)) claim_victory(attacker, 7, 30);
    return planet_destroy_core(planet, attacker);
}

bool GalacticSim::tsw_destroy_planet(PlanetIndex planet, int32_t tsw_owner, bool* leader_hit) {
    // TSW_PLANET_DESTROY §4 steps 7-9: the victory test reads the surface list before the destroy empties it.
    if (leader_hit) *leader_hit = false;
    if (!valid_planet(planet) || planets_[planet].destroyed || !planets_[planet].is_planet) return false;
    const bool hit = valid_player(tsw_owner) && super_weapon_leader_hit(planet);
    if (hit) claim_victory(tsw_owner, 7, 30);  // 0x343b90 on the parent (galactic) monitor
    if (leader_hit) *leader_hit = hit;
    return planet_destroy_core(planet, tsw_owner, 1);  // value 1: tactical kill, the galactic plots do not hear it (§7)
}

// ---- CRC tail --------------------------------------------------------------------------------------------------------------

bool GalacticSim::oi_state_default() const {
    if (!claims_.empty() || linked_battle_pending_ || victory_hold_ != 0) return false;
    for (int32_t id : stealthed_)
        if (store_->alive(store_->find(static_cast<objects::ObjectId>(id)))) return false;
    for (const PlayerState& p : players_)
        if (p.max_tech_level != 5 || !p.built.empty()) return false;
    return true;
}

void GalacticSim::hash_oi(StateHasher& h) const {
    h.u32(0x4F494731u);  // "OIG1"
    h.boolean(linked_battle_pending_);
    uint32_t live = 0;
    for (int32_t id : stealthed_) live += store_->alive(store_->find(static_cast<objects::ObjectId>(id))) ? 1u : 0u;
    h.u32(live);
    for (int32_t id : stealthed_)
        if (store_->alive(store_->find(static_cast<objects::ObjectId>(id)))) h.i32(id);
    h.u32(static_cast<uint32_t>(claims_.size()));
    for (const VictoryClaim& c : claims_) {
        h.i32(c.player);
        h.u8(c.type);
        h.i32(c.frame);
        h.i32(c.countdown);
    }
    if (victory_hold_ != 0) {  // story t11 (only once used, so older captures keep their CRC)
        h.u32(0x56484C44u);  // "VHLD"
        h.i32(victory_hold_);
    }
    for (const PlayerState& p : players_) {
        h.i32(p.max_tech_level);
        h.u32(static_cast<uint32_t>(p.built.size()));
        for (const auto& e : p.built) {
            h.u32(e.first);
            h.i32(e.second);
        }
    }
}

}  // namespace alamo::galactic
