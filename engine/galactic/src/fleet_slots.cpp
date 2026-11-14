// Fleet orbit slots: the authoritative state behind the galaxy map's fleet boxes (g_planet_fleet slot 0..2,
// docs/re/GALAXY_MAP_PRESENTATION.md 1.3: a fleet's own slot, fleet +0xc0 -> +0x48). Playtest 3 t07: the slots were HUD-only.
//
// Rules ([M] unless noted; the binary gives the slot field and the display rule, not the assignment rules):
//   - Every fleet in orbit has a slot 0..kOrbitSlots-1, unique among the fleets of its owner at that planet (owners
//     have separate slot spaces; the HUD resolves collisions between owners, GALAXY_MAP_PRESENTATION 1.3).
//   - A fleet entering an orbit (created there, or arriving) takes the owner's lowest free slot, in event order; a fleet
//     leaving frees its slot. An owner with more than kOrbitSlots fleets there leaves the extra ones unslotted (-1) until
//     a slot frees up; settle_slots then hands slots to unslotted fleets in orbit order.
//   - Commands choose slots (FleetManagement op 8 and the `b` slot of ops 0 / 2 / 4 / 6 / 7, campaign.hpp); arrival merging
//     is in GalacticSim::arrive (campaign.cpp).
// CRC: slots are hashed in a "FSL1" tail only once a command chose one (slot_choices_), like the other non-default
// galactic state, so campaigns that never choose a slot keep their CRCs. Until then slots are a function of the
// (hashed) arrival / departure order.
#include "alamo/galactic/campaign.hpp"
#include "alamo/core/sim_crc.hpp"

namespace alamo::galactic {

uint32_t GalacticSim::slot_occupant(PlanetIndex planet, int32_t owner, int32_t slot, uint32_t except) const {
    if (planet >= planets_.size()) return kNoFleet;
    for (uint32_t f : planets_[planet].orbit) {
        const FleetState& fl = fleets_[f];
        if (f != except && fl.alive && !fl.moving && fl.owner == owner && fl.slot == slot) return f;
    }
    return kNoFleet;
}

uint32_t GalacticSim::fleet_in_slot(PlanetIndex planet, int32_t owner, int32_t slot) const {
    return slot_occupant(planet, owner, slot, kNoFleet);
}

int32_t GalacticSim::lowest_free_slot(PlanetIndex planet, int32_t owner, uint32_t except) const {
    for (int32_t s = 0; s < kOrbitSlots; ++s)
        if (slot_occupant(planet, owner, s, except) == kNoFleet) return s;
    return -1;
}

void GalacticSim::settle_slots(PlanetIndex planet) {
    if (planet >= planets_.size()) return;
    for (uint32_t f : planets_[planet].orbit) {
        FleetState& fl = fleets_[f];
        if (!fl.alive || fl.moving || fl.slot >= 0) continue;
        const int32_t s = lowest_free_slot(planet, fl.owner, f);
        if (s >= 0) fl.slot = static_cast<int8_t>(s);
    }
}

bool GalacticSim::place_in_slot(uint32_t fleet, int32_t slot) {
    if (fleet >= fleets_.size()) return false;
    FleetState& fl = fleets_[fleet];
    if (!fl.alive || fl.moving || fl.planet == kInvalidPlanet || slot < 0 || slot >= kOrbitSlots) return false;
    if (fl.slot != slot && slot_occupant(fl.planet, fl.owner, slot, fleet) != kNoFleet) return false;
    slot_choices_ = true;  // a command chose this slot, even when it is the one the fleet already has
    if (fl.slot == slot) return true;
    fl.slot = static_cast<int8_t>(slot);
    settle_slots(fl.planet);  // an unslotted fleet of the owner may take the slot just freed
    return true;
}

void GalacticSim::hash_slots(StateHasher& h) const {
    h.u32(0x46534C31u);  // "FSL1"
    auto id_of = [this](ObjectHandle o) { return store_->exists(o) ? store_->id(o) : objects::kNoObjectId; };
    for (const FleetState& f : fleets_) {
        if (!f.alive) continue;
        h.i32(id_of(f.object));
        h.i32(f.slot);
        h.u32(f.want_planet);
        h.i32(f.want_slot);
    }
}

}  // namespace alamo::galactic
