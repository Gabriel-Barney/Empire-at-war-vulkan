// [port: Gabriel, 2026-11-13] Fleet and ship names (docs/DECISIONS.md "Fleet and ship names"). Retail has no names for
// fleets or regular units; the port lets the player give their own fleets and units a name. Names are presentation only:
// no sim system reads one, so they are saved (galactic.state v7, src/galactic_save.cpp) but not hashed, and a campaign
// without names has the same CRCs as before.
//
// Storage: names_ = {ObjectId, name} id-ascending (never pointer- or hash-keyed). A name stays with its object: a unit
// keeps its name when it changes fleets, lands, launches or fights a battle (the galactic object lives on); a fleet that
// is merged away, or a unit that dies, loses its name (dropped by prune_names on the next rename, skipped by write).
#include <algorithm>
#include <string>

#include "alamo/galactic/campaign.hpp"

namespace alamo::galactic {

std::string_view GalacticSim::object_name(ObjectHandle object) const {
    if (!store_->alive(object)) return {};
    const int32_t id = static_cast<int32_t>(store_->id(object));
    const auto it = std::lower_bound(names_.begin(), names_.end(), id, [](const auto& e, int32_t v) { return e.first < v; });
    return it != names_.end() && it->first == id ? std::string_view(it->second) : std::string_view{};
}

void GalacticSim::prune_names() {
    std::erase_if(names_, [this](const auto& e) { return !store_->alive(store_->find(e.first)); });
}

RejectReason GalacticSim::rename(int32_t player, ObjectHandle object, std::string_view name) {
    if (!valid_player(player)) return RejectReason::InvalidPlayer;
    if (!store_->alive(object) || planet_of(object) != kInvalidPlanet) return RejectReason::InvalidUnit;
    // Which object is it: a fleet (owner = the fleet's), a fleet member, or a surface unit (owner = the object's).
    bool known = false;
    int32_t owner = kNoPlayer;
    if (const uint32_t f = fleet_of(object); f != kNoFleet) {
        known = true;
        owner = fleets_[f].owner;
    } else if (fleet_containing(object) != kNoFleet) {
        known = true;
        owner = store_->owner(object);
    } else {
        for (const PlanetState& p : planets_)
            if (std::find(p.surface.begin(), p.surface.end(), object) != p.surface.end()) {
                known = true;
                owner = store_->owner(object);
                break;
            }
    }
    if (!known) return RejectReason::InvalidUnit;
    if (owner != player) return RejectReason::NotOwner;
    std::string normalized;
    if (!commands::normalize_object_name(name, normalized)) return RejectReason::InvalidTarget;

    prune_names();
    const int32_t id = static_cast<int32_t>(store_->id(object));
    const auto it = std::lower_bound(names_.begin(), names_.end(), id, [](const auto& e, int32_t v) { return e.first < v; });
    const bool found = it != names_.end() && it->first == id;
    if (normalized.empty()) {
        if (found) names_.erase(it);
    } else if (found) {
        it->second = std::move(normalized);
    } else {
        names_.insert(it, {id, std::move(normalized)});
    }
    return RejectReason::None;
}

}  // namespace alamo::galactic
