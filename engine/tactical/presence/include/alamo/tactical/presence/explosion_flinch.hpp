// R3 - flinch from EXPLOSION objects (docs/re/ANIM_PARTICLES.md §A.12, 0x62e040, called from ExplosionBehavior
// Initialise vf9 0x622440 after the push-out 0x621bb0; Initialise does not run on load).
//
// Victims: every object with IDLE in a GOM-wide cube of half-extent Induced_Flinch_Range (explosion type +0x20e0,
// default 100) around the explosion. No owner, dying or self filter. Direction D = victim position - explosion
// position (3D, not normalised; the flinch picks its clip from yaw(D) - facing). Intensity (written to the victim's
// flinch +0x58) = clamp(I x (1 - 0.5 x d²_xy / R²), 0.25, 1) with I = Induced_Flinch_Intensity (+0x20e4, default
// 1.0) and d_xy the horizontal distance. The remaining gates (flinch timer, no flinch in progress, not Deploys,
// current clip idle- or attack-like, the model has the clip) and the play itself are the anim side's
// (anim::AnimBehaviors::on_hit path).
//
// Pure functions; results ascending by victim id (the port's query order), duplicates removed.
#pragma once
#include <cstdint>
#include <span>
#include <vector>

#include "alamo/tactical/presence/presence_system.hpp"

namespace alamo::tactical::presence {

inline constexpr float kDefaultInducedFlinchRange = 100.0f;     // Induced_Flinch_Range (+0x20e0)
inline constexpr float kDefaultInducedFlinchIntensity = 1.0f;   // Induced_Flinch_Intensity (+0x20e4)
inline constexpr float kMinExplosionFlinchIntensity = 0.25f;

struct FlinchCandidate {
    ObjectId id = kNoObject;
    float x = 0.0f, y = 0.0f, z = 0.0f;
    bool has_idle = false;
};

struct ExplosionFlinch {
    ObjectId victim = kNoObject;
    float dx = 0.0f, dy = 0.0f, dz = 0.0f;  // victim - explosion
    float intensity = 0.0f;
};

// clamp(intensity x (1 - 0.5 x d2_xy / range²), 0.25, 1). range <= 0 or NaN: kMinExplosionFlinchIntensity [port].
float explosion_flinch_intensity(float intensity, float d2_xy, float range);

// Candidates from the host's box query (a superset is fine: the cube is re-tested, inclusive). range <= 0 or NaN
// gives no victims [port: the original's zero-size query was not traced].
std::vector<ExplosionFlinch> explosion_flinches(float ex, float ey, float ez, float range, float intensity,
                                                std::span<const FlinchCandidate> candidates);
// Same through the PresenceWorld callbacks (box_query + unit(): has_idle and the position).
std::vector<ExplosionFlinch> explosion_flinches(float ex, float ey, float ez, float range, float intensity,
                                                const PresenceWorld& world);

}  // namespace alamo::tactical::presence
