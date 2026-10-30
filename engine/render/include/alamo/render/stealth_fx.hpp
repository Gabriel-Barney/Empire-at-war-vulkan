// alamo_render - R5 render-only springs: the stealth tint spring and the HideWhenFogged alpha spring
// (docs/re/ABILITY_VISUALS.md 1.1 / 1.2 / 1.3 / 1.4, 6). Presentation only: never in a CRC, stepped by the instance builder
// once per sim tick with dt = 1/30 (as the retail special-ability / behaviour updates run on sim frames).
//
// The spring (0x592e80 / 0x714b40, HideWhenFogged 0x53ddc0) is the usual critically damped "smooth damp" approximation:
// omega = 2 / T, x = omega dt, k = 1 / (1 + x + 0.48 x^2 + 0.235 x^3), c = v - target, t = (vel + omega c) dt,
// vel = (vel - omega t) k, v = target + (c + t) k. About 90 % of the way after 2 T.
#pragma once
#include <cstdint>
#include <span>

#include "alamo/assets/model.hpp"
#include "alamo/materials/constants.hpp"
#include "alamo/render/hardpoints.hpp"

namespace alamo::render {

inline constexpr float kSpringDt = 1.0f / 30.0f;       // global frame time float 0x140b0a344 [M = 1 / FPS]
inline constexpr float kStealthTintMin = 0.01f;        // the model colour callback applies the tint above this (0x3a0c20)
inline constexpr float kFogSpringTime = 0.25f;         // HideWhenFogged smooth time: omega = 2 x 4.0 = 8 / s (ctor 0x53da00)
inline constexpr uint64_t kSpringMaxSteps = 600;       // 20 s of ticks: any longer gap is converged anyway

struct SmoothSpring {
    float value = 0.0f;
    float velocity = 0.0f;
};

// One tick of the spring towards `target` with smooth time T. T <= 0 (or non-finite): instant.
void smooth_damp(SmoothSpring& s, float target, float smooth_time, float dt = kSpringDt);
// `steps` ticks of it (0 = nothing; capped at kSpringMaxSteps).
void smooth_damp_steps(SmoothSpring& s, float target, float smooth_time, uint64_t steps);

// Per-object stealth presentation (SAP +0x290.. in retail): the tint spring plus the shadow-bit state.
class StealthFx {
public:
    // Starts converged at the target (an object that appears already stealthed does not fade in).
    void snap(bool applied, bool cloak);
    // `steps` sim ticks with the fade target 1 (applied) or 0, smooth time T seconds (<= 0: instant). `cloak` = Force_Cloak
    // (restores the shadow bit when the value falls below 0.01), else Stealth_Ability (keeps it lost, 1.3).
    void step(bool applied, bool cloak, float smooth_time, uint64_t steps);
    float value() const { return spring_.value; }
    bool tinted() const { return spring_.value > kStealthTintMin; }
    bool shadow_dropped() const { return shadow_lost_; }
    bool cloak() const { return cloak_; }
    // lerp(white, color, v) per channel while tinted, else white. `color` = R, G, B, A in 0..1.
    materials::float4 tint(const float color[4]) const;

private:
    SmoothSpring spring_;
    bool cloak_ = false;
    bool shadow_lost_ = false;
};

// Marks (1 = hidden) the owner-model sub-objects that stand for hardpoint attachments: every mesh and ALO proxy named like
// a hardpoint's Model_To_Attach (upper-cased, cut at ".ALO"; 1.3, 0x3a8140). `mask` is indexed like HardpointVisibility::mask()
// (meshes, then proxies; sized by the caller). Returns how many sub-objects it newly hid.
uint32_t hide_hardpoint_meshes(const assets::ModelAsset& model, const HardpointLayout& layout, std::span<uint8_t> mask);

}  // namespace alamo::render
