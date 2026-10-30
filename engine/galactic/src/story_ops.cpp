// L1/S4 additions to GalacticSim (declared in campaign.hpp, "L1/S4 Lua + story operations"): the
// operations Lua story scripts call directly (Change_Owner, Assemble_Fleet) and the containment queries
// the Lua object bindings need for galactic objects (Get_Parent_Object, Get_Planet_Location,
// Contains_Hero, ...). Kept out of campaign.cpp so the S3 / S10 owners of that file are not disturbed.
//
// Determinism: serial code only; every walk is over vectors in index order (fleets in creation order,
// orbit lists in arrival order, contents in content order). No RNG.
#include <algorithm>

#include "alamo/galactic/campaign.hpp"

namespace alamo::galactic {

uint32_t GalacticSim::fleet_containing(ObjectHandle unit) const {
    if (!unit) return kNoFleet;
    for (uint32_t i = 0; i < fleets_.size(); ++i)
        if (fleets_[i].alive && std::find(fleets_[i].contents.begin(), fleets_[i].contents.end(), unit) != fleets_[i].contents.end())
            return i;
    return kNoFleet;
}

ObjectHandle GalacticSim::container_of(ObjectHandle object) const {
    if (!store_->exists(object)) return {};
    const uint32_t own = fleet_of(object);
    if (own != kNoFleet) {  // a fleet: its planet while stationary in orbit
        const FleetState& f = fleets_[own];
        return (!f.moving && valid_planet(f.planet)) ? planets_[f.planet].object : ObjectHandle{};
    }
    const uint32_t fl = fleet_containing(object);
    if (fl != kNoFleet) return fleets_[fl].object;
    for (const PlanetState& p : planets_) {
        if (!p.is_planet) continue;
        if (p.starbase == object || std::find(p.structures.begin(), p.structures.end(), object) != p.structures.end() ||
            std::find(p.surface.begin(), p.surface.end(), object) != p.surface.end())
            return p.object;
    }
    return store_->container(object);
}

std::vector<ObjectHandle> GalacticSim::contents_of(ObjectHandle object) const {
    std::vector<ObjectHandle> out;
    if (!store_->exists(object)) return out;
    const uint32_t fl = fleet_of(object);
    if (fl != kNoFleet) {
        for (ObjectHandle m : fleets_[fl].contents)
            if (store_->alive(m)) out.push_back(m);
        return out;
    }
    const PlanetIndex pi = planet_of(object);
    if (pi == kInvalidPlanet || !valid_planet(pi)) {  // anything else: the store's container links
        for (ObjectHandle m : store_->all_objects())
            if (store_->alive(m) && store_->container(m) == object) out.push_back(m);
        return out;
    }
    const PlanetState& p = planets_[pi];
    for (uint32_t f : p.orbit)
        if (fleets_[f].alive && store_->alive(fleets_[f].object)) out.push_back(fleets_[f].object);
    for (ObjectHandle u : p.surface)
        if (store_->alive(u)) out.push_back(u);
    for (ObjectHandle s : p.structures)
        if (store_->alive(s)) out.push_back(s);
    if (store_->alive(p.starbase)) out.push_back(p.starbase);
    return out;
}

// Change_Owner. Planets: GE 4.2 "Forced transfers" (0x3f7280) in its documented order: the conflict is
// cleared, the starbase is cleared (0x4bda40: destroyed here [M]), the space queue is re-evaluated at
// level 0, the starbase owner reset, the planet's surface forces handled (0x4b7670 / 0x4b7860 not traced:
// [L] every structure and surface unit not owned by the new owner is converted to it, so the derived owner
// (GE 4.1: structures, then surface units) keeps the new owner), then the transfer is applied with
// control 1.0 through the common owner-change steps (on_owner_change: story/store event, credit caps,
// capture bonus, corruption, siphon, queues). The political-control breakdown is recomputed every frame
// here, so "clears the political breakdown" has nothing to clear.
// Other objects [port]: the store owner changes (ObjectStore::set_owner); a fleet's members follow their
// fleet and a stationary fleet then merges into the new owner's stationary fleet in orbit (as on
// arrival); a member of a stationary fleet leaves it (an emptied fleet dies) and joins the new owner's
// fleet in orbit there (found or created); a member of a moving fleet is refused; a starbase also moves
// the planet's starbase owner.
bool GalacticSim::change_owner(ObjectHandle object, int32_t player) {
    if (!store_->alive(object) || !valid_player(player)) return false;
    const PlanetIndex pi = planet_of(object);
    if (pi != kInvalidPlanet && valid_planet(pi)) {
        PlanetState& p = planets_[pi];
        if (p.owner == player) return false;
        p.in_battle = false;
        if (store_->alive(p.starbase)) destroy_object(p.starbase);
        reevaluate_queue(pi, kSpaceQueue, 0);
        p.starbase_owner = kNoPlayer;
        for (ObjectHandle s : p.structures)
            if (store_->alive(s)) store_->set_owner(s, player);
        for (ObjectHandle u : p.surface)
            if (store_->alive(u)) store_->set_owner(u, player);
        const int32_t old = p.owner;
        p.owner = player;
        p.control = 1.0f;
        on_owner_change(pi, old);
        return true;
    }
    const uint32_t own = fleet_of(object);
    if (own != kNoFleet) {
        if (!fleets_[own].alive || fleets_[own].owner == player) return false;
        fleets_[own].owner = player;
        store_->set_owner(object, player);
        for (ObjectHandle m : fleets_[own].contents)
            if (store_->alive(m)) store_->set_owner(m, player);
        const FleetState& f = fleets_[own];
        if (!f.moving && valid_planet(f.planet)) {
            const std::vector<uint32_t> orbit = planets_[f.planet].orbit;
            for (uint32_t g : orbit)
                if (g != own && fleets_[g].alive && fleets_[g].owner == player && !fleets_[g].moving) {
                    merge_fleet(own, g);
                    break;
                }
        }
        return true;
    }
    if (store_->owner(object) == player) return false;
    const uint32_t fl = fleet_containing(object);
    if (fl != kNoFleet) {
        if (fleets_[fl].moving || !valid_planet(fleets_[fl].planet)) return false;
        const PlanetIndex at = fleets_[fl].planet;
        FleetState& src = fleets_[fl];
        for (size_t k = 0; k < src.contents.size(); ++k)
            if (src.contents[k] == object) {
                src.contents.erase(src.contents.begin() + static_cast<std::ptrdiff_t>(k));
                src.carried.erase(src.carried.begin() + static_cast<std::ptrdiff_t>(k));
                break;
            }
        if (src.contents.empty()) kill_fleet(fl);
        store_->set_owner(object, player);
        const uint32_t dst = orbit_fleet(player, at);  // may grow fleets_: no FleetState reference held
        fleets_[dst].contents.push_back(object);
        fleets_[dst].carried.push_back(0);
        return true;
    }
    for (PlanetState& p : planets_)
        if (p.is_planet && p.starbase == object) p.starbase_owner = player;
    return store_->set_owner(object, player);
}

uint32_t GalacticSim::assemble_fleet(std::span<const ObjectHandle> units) {
    std::vector<ObjectHandle> picked;
    std::vector<uint32_t> source;
    int32_t owner = kNoPlayer;
    PlanetIndex at = kInvalidPlanet;
    for (ObjectHandle u : units) {
        if (!store_->alive(u) || fleet_of(u) != kNoFleet || planet_of(u) != kInvalidPlanet) continue;
        if (std::find(picked.begin(), picked.end(), u) != picked.end()) continue;
        const uint32_t fl = fleet_containing(u);
        if (fl == kNoFleet || fleets_[fl].moving || !valid_planet(fleets_[fl].planet)) continue;
        if (picked.empty()) {
            owner = fleets_[fl].owner;
            at = fleets_[fl].planet;
        } else if (fleets_[fl].owner != owner || fleets_[fl].planet != at) {
            continue;
        }
        picked.push_back(u);
        source.push_back(fl);
    }
    if (picked.empty()) return kNoFleet;
    // One fleet holding exactly these units: it is the assembled fleet.
    const uint32_t first = source[0];
    if (std::all_of(source.begin(), source.end(), [first](uint32_t s) { return s == first; })) {
        size_t live = 0;
        for (ObjectHandle m : fleets_[first].contents) live += store_->alive(m) ? 1u : 0u;
        if (live == picked.size()) return first;
    }
    const uint32_t out = create_fleet(owner, at);
    for (size_t i = 0; i < picked.size(); ++i) {
        FleetState& src = fleets_[source[i]];
        for (size_t k = 0; k < src.contents.size(); ++k)
            if (src.contents[k] == picked[i]) {
                src.contents.erase(src.contents.begin() + static_cast<std::ptrdiff_t>(k));
                src.carried.erase(src.carried.begin() + static_cast<std::ptrdiff_t>(k));
                break;
            }
        fleets_[out].contents.push_back(picked[i]);
        fleets_[out].carried.push_back(0);
    }
    for (uint32_t s : source)
        if (fleets_[s].alive && fleets_[s].contents.empty()) kill_fleet(s);
    return out;
}

}  // namespace alamo::galactic
