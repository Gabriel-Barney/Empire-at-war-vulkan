// docs/re/GALACTIC_REVEAL_CORRUPTION.md (GRC) on GalacticSim (declared in campaign.hpp): the planet revealed byte
// D +0x2c9 (campaign-start home reveal, the galactic pre-step 0x4b2600, story rewards, Is_Planet_Revealed 0x3fa420) and
// the corruption transition D +0x2e2..+0x2f8 (begin 0x4b72e0 / 0x4b7260, complete 0x4b7a50 via 0x3fc5b0, cancel
// 0x4b6c50, fraction 0x4b7e20, particle pairs 0x3ff230).
//
// Determinism: serial code; planets walked in row order (= the category-3 list order), orbit lists in arrival order;
// no RNG. The transition fraction is presentation-only: no sim code branches on it (completion is an integer frame
// test).
#include <algorithm>

#include "alamo/galactic/campaign.hpp"

namespace alamo::galactic {

namespace {

// 0x2c7c90 / 0x2c7cf0 / 0x2c7cc0: type -1 -> [0], 0..7 -> [1], anything else 1.0; a missing element reads 1.0 [L].
float level_value(const std::vector<float>& list, int8_t type) {
    size_t i = 0;
    if (type == kCorruptionNone) i = 0;
    else if (type >= 0 && type <= 7) i = 1;
    else return 1.0f;
    return i < list.size() ? list[i] : 1.0f;
}

// (int)((float)FPS x secs), truncation (cvttss2si).
uint32_t transition_frames(float seconds) {
    return static_cast<uint32_t>(static_cast<int32_t>(static_cast<float>(econ::kFramesPerSecond) * seconds));
}

}  // namespace

// ---- reveal --------------------------------------------------------------------------------------------------------

bool GalacticSim::is_planet_revealed(PlanetIndex planet) const {
    if (!valid_planet(planet) || !planets_[planet].is_planet || planets_[planet].hidden) return false;
    if (auto_reveal_) return true;
    return planets_[planet].revealed;
}

bool GalacticSim::planet_revealed_raw(PlanetIndex planet) const {
    return valid_planet(planet) && planets_[planet].revealed;
}

void GalacticSim::set_planet_revealed(PlanetIndex planet, bool revealed) {
    if (valid_planet(planet) && planets_[planet].is_planet) planets_[planet].revealed = revealed;
}

void GalacticSim::reveal_planets_of_type(uint32_t planet_type, bool on) {
    for (PlanetState& p : planets_)
        if (p.is_planet && p.type == planet_type) p.revealed = on;
}

void GalacticSim::reveal_all_planets() {
    for (PlanetState& p : planets_)
        if (p.is_planet) p.revealed = true;
}

bool GalacticSim::consume_reveal_sfx() {
    const bool r = reveal_sfx_;
    reveal_sfx_ = false;
    return r;
}

bool GalacticSim::reveal_one(PlanetIndex planet) {
    PlanetState& d = planets_[planet];
    if (!d.is_planet || d.revealed) return false;
    d.revealed = true;  // + highlight / visibility refresh (presentation; the masks are not modelled)
    return true;
}

void GalacticSim::reveal_prestep() {
    // GRC 1.3 [H]. Nothing without a local player.
    if (!valid_player(local_player_)) return;
    const int32_t local = local_player_;
    // threshold = (float)FPS x Planet_Reveal_Delay_Time (float, no FMA).
    const float threshold = static_cast<float>(econ::kFramesPerSecond) * rules_->constants.planet_reveal_delay_seconds;
    const double r = static_cast<double>(map_.max_fleet_move_distance);
    const double r2 = r * r;
    bool newly = false;
    for (PlanetIndex i = 0; i < planets_.size(); ++i) {
        PlanetState& d = planets_[i];
        if (!d.is_planet) continue;
        bool source = false;
        // 1. held by me or an ally (0x4ad9f0): owner != -1, relation 0, control > 0.
        if (valid_player(d.owner) && allied(local, d.owner) && d.control > 0.0f) {
            source = true;
        } else {
            // 2. orbit presence with delay (counter D +0x2cc).
            int32_t total = 0, hid = 0;
            for (uint32_t f : d.orbit) {
                const FleetState& fl = fleets_[f];
                if (!fl.alive || fl.owner != local) continue;
                for (ObjectHandle m : fl.contents) {
                    ++total;
                    hid += hidden(m) ? 1 : 0;
                }
            }
            if (total < 1 || total <= hid || paused() || d.orbit.empty()) d.reveal_delay = 0;
            else if (static_cast<float>(d.reveal_delay) <= threshold) ++d.reveal_delay;
            else source = true;
            // 3. corruption (any type, whoever made it) and the local faction benefits.
            if (d.corruption != kCorruptionNone && benefits(local)) source = true;
        }
        if (!source) continue;
        newly = reveal_one(i) || newly;  // 4.
        if (!auto_reveal_) continue;
        // 6. auto-reveal spread: planets not owned by the local player within MaximumFleetMovementDistance (squared,
        // float products summed in double, strict <), then the trade-route neighbours.
        const Vec3 c = d.position;
        for (PlanetIndex j = 0; j < planets_.size(); ++j) {
            const PlanetState& o = planets_[j];
            if (!o.is_planet || o.owner == local) continue;
            const float dx = o.position.x - c.x, dy = o.position.y - c.y, dz = o.position.z - c.z;
            const float xx = dx * dx, yy = dy * dy, zz = dz * dz;
            const double dist2 = static_cast<double>(xx) + static_cast<double>(yy) + static_cast<double>(zz);
            if (dist2 < r2) newly = reveal_one(j) || newly;
        }
        for (uint32_t rt : planets_[i].links) {
            const PlanetIndex nb = map_.routes.point_a[rt] == i ? map_.routes.point_b[rt] : map_.routes.point_a[rt];
            if (valid_planet(nb)) newly = reveal_one(nb) || newly;
        }
    }
    if (newly && frame_ > 0) reveal_sfx_ = true;  // SFXEvent_Planet_Revealed (frameArg > 0 [L])
}

// ---- corruption transitions ------------------------------------------------------------------------------------------

float GalacticSim::corruption_transition_fraction(PlanetIndex planet, uint32_t now) const {
    if (!valid_planet(planet)) return 1.0f;
    const PlanetState& p = planets_[planet];
    const uint32_t end = p.corruption_end, start = p.corruption_start;
    if (end == 0 || start == end) return 1.0f;
    if (start > end) return 0.0f;
    const uint32_t n = now != 0 ? now : static_cast<uint32_t>(frame_);
    // u32 zero-extended through a 64-bit signed convert: exact unsigned; now < start wraps to a large value -> 1.0.
    const float t = static_cast<float>(static_cast<int64_t>(n - start)) / static_cast<float>(static_cast<int64_t>(end - start));
    return std::min(1.0f, t);
}

int8_t GalacticSim::effective_corruption_type(PlanetIndex planet) const {
    if (!valid_planet(planet)) return kCorruptionNone;
    const PlanetState& p = planets_[planet];
    return (p.corruption_protected || p.corruption_removing) ? p.corruption_pending : p.corruption;
}

bool GalacticSim::corruption_transition_active(PlanetIndex planet) const {
    return valid_planet(planet) && (planets_[planet].corruption_protected || planets_[planet].corruption_removing);
}

GalacticSim::CorruptionParticlePairs GalacticSim::corruption_particle_pairs(PlanetIndex planet) const {
    CorruptionParticlePairs out;
    if (!valid_planet(planet)) return out;
    const PlanetState& p = planets_[planet];
    out.present = p.corruption_particles;
    out.brightness_start = p.corruption_pairs[0];
    out.brightness_end = p.corruption_pairs[1];
    out.scale_start = p.corruption_pairs[2];
    out.scale_end = p.corruption_pairs[3];
    out.energy_start = p.corruption_pairs[4];
    out.energy_end = p.corruption_pairs[5];
    return out;
}

bool GalacticSim::begin_gain_raw(PlanetIndex planet, float seconds, int8_t type, int32_t player) {
    PlanetState& p = planets_[planet];
    if (p.corruption_protected || p.corruption_removing || p.corruption != kCorruptionNone) return false;
    p.corruption_protected = true;
    p.corruption_pending = type;
    p.corruption_pending_player = player;
    p.corruption_start = static_cast<uint32_t>(frame_);
    p.corruption_end = p.corruption_start + transition_frames(seconds);
    return true;
}

bool GalacticSim::begin_removal_raw(PlanetIndex planet, float seconds) {
    PlanetState& p = planets_[planet];
    if (p.corruption_protected || p.corruption_removing || p.corruption == kCorruptionNone) return false;
    p.corruption_removing = true;
    p.corruption_pending = kCorruptionNone;
    p.corruption_pending_player = kNoPlayer;
    p.corruption_start = static_cast<uint32_t>(frame_);
    p.corruption_end = p.corruption_start + transition_frames(seconds == 0.0f ? 0.1f : seconds);
    return true;
}

void GalacticSim::cancel_corruption_transition(PlanetIndex planet) {
    if (!valid_planet(planet)) return;
    PlanetState& p = planets_[planet];
    p.corruption_protected = p.corruption_removing = false;
    p.corruption_start = p.corruption_end = 0;
    p.corruption_pending = kCorruptionNone;
    p.corruption_pending_player = kNoPlayer;
}

bool GalacticSim::begin_corruption_gain(PlanetIndex planet, int8_t type, int32_t corrupting_player, float seconds) {
    if (!valid_planet(planet) || !planets_[planet].is_planet) return false;
    if (!begin_gain_raw(planet, seconds, type, corrupting_player)) return false;
    rebuild_corruption_pairs(planet);
    return true;
}

bool GalacticSim::begin_corruption_removal(PlanetIndex planet, float seconds) {
    if (!valid_planet(planet) || !planets_[planet].is_planet) return false;
    if (!begin_removal_raw(planet, seconds)) return false;
    rebuild_corruption_pairs(planet);
    return true;
}

void GalacticSim::complete_corruption(PlanetIndex planet) {
    // 0x3fc5b0: only with a flag set and frame >= end (unsigned).
    PlanetState& p = planets_[planet];
    if (!p.corruption_protected && !p.corruption_removing) return;
    if (static_cast<uint32_t>(frame_) < p.corruption_end) return;
    const int8_t old = p.corruption;
    const int32_t old_player = p.corrupting_player;
    // 1. complete 0x4b7a50.
    p.corruption = p.corruption_pending;
    p.corrupting_player = p.corruption == kCorruptionNone ? kNoPlayer : p.corruption_pending_player;
    p.corruption_start = p.corruption_end = 0;
    p.corruption_protected = p.corruption_removing = false;
    p.corruption_pending = kCorruptionNone;
    p.corruption_pending_player = kNoPlayer;
    p.corruption_changed = static_cast<uint32_t>(frame_);
    rebuild_corruption_pairs(planet);  // 2.
    if (old != p.corruption) {          // 3. corruption-changed notification, 5. 0x3f9690 (unlocks)
        corruption_unlocks(planet, old, old_player);
        GalacticEvent ev;
        ev.kind = GalacticEventKind::CorruptionChanged;
        ev.planet = planet;
        ev.player = p.corrupting_player;
        ev.value = p.corruption;
        ev.other_player = old;
        emit(ev);
    }
    // 4. the gained / removed SFX is presentation.
}

void GalacticSim::rebuild_corruption_pairs(PlanetIndex planet) {
    // 0x3ff230: the particle object exists while corrupted or gaining; the pairs are written only while it exists.
    PlanetState& p = planets_[planet];
    p.corruption_particles = p.corruption != kCorruptionNone || p.corruption_protected;
    if (!p.corruption_particles) return;
    const EconomyConstants& c = rules_->constants;
    const int8_t cur = p.corruption, eff = effective_corruption_type(planet);
    p.corruption_pairs = {level_value(c.particle_brightness_per_level, cur), level_value(c.particle_brightness_per_level, eff),
                          level_value(c.particle_scale_per_level, cur),      level_value(c.particle_scale_per_level, eff),
                          level_value(c.particle_energy_per_level, cur),     level_value(c.particle_energy_per_level, eff)};
}

// ---- CRC ---------------------------------------------------------------------------------------------------------------

bool GalacticSim::reveal_state_default() const {
    if (local_player_ != kNoPlayer || !auto_reveal_) return false;
    for (const PlanetState& p : planets_)
        if (p.revealed || p.corruption_removing || p.corruption_start != 0 || p.corruption_end != 0 ||
            p.corruption_changed != 0 || p.corruption_pending != kCorruptionNone || p.corruption_pending_player != kNoPlayer)
            return false;
    return true;
}

void GalacticSim::hash_reveal(StateHasher& h) const {
    h.u32(0x52564331u);  // "RVC1"
    h.i32(local_player_);
    h.boolean(auto_reveal_);
    for (const PlanetState& p : planets_) {
        if (!p.is_planet) continue;
        h.boolean(p.revealed);
        h.boolean(p.corruption_removing);
        h.u32(p.corruption_start);
        h.u32(p.corruption_end);
        h.u32(p.corruption_changed);
        h.value(p.corruption_pending);
        h.i32(p.corruption_pending_player);
    }
}

}  // namespace alamo::galactic
