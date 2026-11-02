// Planet ground-structure slots for the galactic AI (A2 f t01). Private to engine/ai.
//
// Slot max 0x372740 = planet data +0x230, seeded from the planet type's Special_Structures_Land (+0x92C)
// (AI_GALACTIC_TOKENS MaxStructureSlots "D+0x230 (type+0x92C)"; STRATEGIC_GAPS §1 step 18) [H]. -1 = the tag is
// absent = no cap [port: GalacticSim::build_check reads it the same way]. The legacy <Max_Ground_Base> tag
// (core PlanetTable::max_ground_base) is NOT this: shipped data sets it only on Galaxy_Core_Art_Model, so it reads 0
// on every real planet and vetoed every ground structure.
//
// Used slots 0x3f8af0 = the ground structures on the surface, 0 when the planet has no ground base
// (D+0x130 > 0 ? D+0x160 : 0; AI_GALACTIC_TOKENS OpenStructureSlotsUnnormalized) [H]. Port: ground base level =
// structure count (GalacticSim::ground_base_level [M]); a structure counts when its type is of the ground family
// (galactic::is_ground_structure_type, as build_check step 18).
#pragma once
#include <cstdint>

#include "alamo/galactic/campaign.hpp"

namespace alamo::ai {

inline int32_t galactic_ai_ground_slot_max(const galactic::GalacticSim& sim, PlanetIndex p) {
    const auto planets = sim.planets();
    if (p >= planets.size()) return 0;
    const uint32_t t = planets[p].type;
    const auto& types = sim.rules().types;
    return t < types.size() ? types[t].ground_structure_slots : -1;
}

inline int32_t galactic_ai_ground_slots_used(const galactic::GalacticSim& sim, PlanetIndex p) {
    const auto planets = sim.planets();
    if (p >= planets.size() || sim.ground_base_level(p) <= 0) return 0;
    const objects::ObjectStore& st = sim.store();
    const auto& types = sim.rules().types;
    int32_t n = 0;
    for (const auto s : planets[p].structures) {
        if (!st.alive(s)) continue;
        const auto t = static_cast<size_t>(st.type(s));
        n += t < types.size() && galactic::is_ground_structure_type(types[t]) ? 1 : 0;
    }
    return n;
}

}  // namespace alamo::ai
