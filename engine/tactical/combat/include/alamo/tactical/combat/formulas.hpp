// S8 - the reference formulas of docs/re/TACTICAL_COMBAT.md sections 2-4 as pure functions, so tests
// can check them against hand-computed values and the CombatSystem uses exactly one implementation.
// Confidence tags follow the doc: [H] read and cross-checked (four of them verified in disassembly,
// marked [H, disasm]), [M] read once / partly inferred, [L] port guess.
//
// Arithmetic: float, evaluated left to right as written (no FMA: /fp:precise, -ffp-contract=off).
// int(x) is C truncation toward zero (cvttss2si). No CRT transcendental (dmath only).
#pragma once
#include <cstdint>

#include "alamo/core/sim_rng.hpp"
#include "alamo/tactical/geometry.hpp"

namespace alamo::tactical::combat {

enum class GameMode : uint8_t { Galactic = 0, Land = 1, Space = 2 };  // section 0.3

inline constexpr uint32_t kDefaultFps = 30;  // logic rate global 0x140b0a340 [M/H]

// ---- targeting (section 2.1) --------------------------------------------------------------------
// Acquire_Target reschedule: now + FPS + Get_Int(0, FPS/2) -> 30 + 0..15 frames. One synced draw [H].
uint32_t next_scan_frame(uint32_t now, uint32_t fps, SimRng& rng);

// ---- weapon recharge (section 3.1 step 12) ------------------------------------------------------
struct Recharge {
    int32_t frames = 0;
    int32_t pulses = 0;
};
// Pulse-delay branch (pulses remain after the decrement) [H]: frames = int(FPS x delay);
// frames = int(frames x W + 0.5) - W applied UNCONDITIONALLY [H, disasm]; F <= 0 -> frames and
// pulses 0; otherwise frames = int(frames / F + 0.5). `pulses_left` is passed through.
Recharge hp_pulse_recharge(float pulse_delay_seconds, int32_t pulses_left, float W, float F, uint32_t fps);
// Full recharge (pulses reached 0) [H]: frames = trunc(Get_Int(trunc(min x 100), trunc(max x 100))
// x FPS) / 100 (bounds TRUNCATED, unsigned integer divide) [H, disasm]; then W only when W < 1
// [H, disasm] as int(frames x W + 0.5) [M: same form as the pulse branch]; F only when 0 < F < 1 as
// int(frames / F + 0.5); pulses = int(Fire_Pulse_Count x max(F, 0) + 0.5). One synced draw.
Recharge hp_full_recharge(float min_seconds, float max_seconds, int32_t pulse_count, float W, float F, uint32_t fps,
                          SimRng& rng);
// HardPoint ctor first cooldown: Get_Int(0, int(FPS x Fire_Max_Recharge_Seconds)) [H]. One draw.
int32_t hp_first_cooldown(float max_seconds, uint32_t fps, SimRng& rng);
// WeaponBehavior reset (non-hardpoint weapons) [H]: frames = int(recharge x FPS) + Get_Int(0, 10);
// W only when W < 1 [M: as int(frames x W + 0.5)]; pulses = Projectile_Fire_Pulse_Count; then
// frames = trunc(frames / F), pulses = trunc(F x pulses) with NO +0.5 rounding [H]. F <= 0 ->
// frames 0, pulses 0 [L]. One synced draw.
Recharge weapon_reset(float recharge_seconds, int32_t pulse_count, float W, float F, uint32_t fps, SimRng& rng);
// WeaponBehavior pulse delay (0x54d8f0) [L]: int(FPS x delay), then trunc(frames / F).
int32_t weapon_pulse_delay(float pulse_delay_seconds, float F, uint32_t fps);

// ---- inaccuracy (section 3.1 step 10) [H, disasm] ----------------------------------------------
// Getter scaling f: 1.0 in space; on land 1.0 - AI-difficulty accuracy (accuracy 1.0 when it does
// not apply). Effective value = v x f + v (mulss then addss): 2x the XML value in space,
// (2 - accuracy) x on land. An ability override radius is returned UNSCALED.
float inaccuracy_f(GameMode mode, float ai_accuracy);
inline float scale_inaccuracy(float v, float f) { return v * f + v; }
// Spread s for the distance branch: d x distance / Get_Fire_Range.
inline float inaccuracy_spread(float d_scaled, float distance, float fire_range) {
    return fire_range > 0.0f ? d_scaled * distance / fire_range : 0.0f;
}
// 3 synced Get_Float(-r, r): X, then Y, then Z.
Vec3 inaccuracy_offset(float r, SimRng& rng);

// ---- range (sections 2.2, 3.1 step 7) -----------------------------------------------------------
// Get_Fire_Range = Fire_Range_Distance x (1 + list 8); Get_Max_Attack_Range = base x (1 + list 8).
inline float fire_range(float base, float list8) { return base * (1.0f + list8); }
// Fire_At range gate, XY only [H]: min <= distance_xy <= fire range + target radius.
inline bool in_fire_range(float dist_xy, float min_range, float range, float target_radius) {
    return min_range <= dist_xy && dist_xy <= range + target_radius;
}
float distance_xy(Vec3 a, Vec3 b);
inline float distance2_xy(Vec3 a, Vec3 b) {
    const float dx = a.x - b.x, dy = a.y - b.y;
    return dx * dx + dy * dy;
}
float distance3(Vec3 a, Vec3 b);
// Lobbed projectiles (Projectile_Category 3) base attack range: Max_Speed^2 / (gravity x lob mod) [H].
inline float lob_range(float max_speed, float gravity, float lob_mod) {
    const float g = gravity * lob_mod;
    return g > 0.0f ? max_speed * max_speed / g : 0.0f;
}

// ---- lead (section 3.1 step 9) [M] --------------------------------------------------------------
// Linear lead (0x399e20): the aim point P + V t for the smallest t >= 0 with |P + V t - M| = S t
// (S = Max_Speed x FPS, V per second). No solution -> P.
Vec3 linear_lead(Vec3 muzzle, Vec3 target, Vec3 target_velocity, float projectile_speed);
// Compute_Lead_Point (0x399450) [M] (OI tactical): the target's last three positions p0 (oldest), p1, p2 (now),
// sampled one logic frame apart, are fitted. A straight track (collinear / repeated points) or a circumcircle of
// radius > 1000 uses linear lead with V = (p2 - p1) x FPS. A curving track moves on its XY circumcircle at the
// angular rate of the last step [port: planar fit, z extrapolated linearly - space motion is planar, MOVEMENT 2.1];
// the flight time t solves |P(t) - M| = S t with the 1-D root finder of 0x7d68e0: bracket [0.1, 1000] s,
// tolerance 0.001, at most 15 iterations ([port]: Illinois false position - ABILITIES_HARDPOINTS 4.11 allows any
// deterministic method with that bracket and stop rule). No sign change: t = 0.1 when the shot is already there at the lower
// bound, else linear lead [port]. `offset` (aim point - unit position, e.g. a hardpoint) is added unrotated [port].
Vec3 circular_lead(Vec3 muzzle, Vec3 p0, Vec3 p1, Vec3 p2, Vec3 offset, float projectile_speed, uint32_t fps);
inline constexpr float kLeadMaxCircleRadius = 1000.0f;
inline constexpr float kLeadRootLo = 0.1f, kLeadRootHi = 1000.0f, kLeadRootTolerance = 0.001f;
inline constexpr int kLeadRootIterations = 15;

// ---- lobbed flight (section 3.3, category 3, 0x4ceea0) [M] (OI tactical) ----------------------------------------
// Gravity per frame^2 = Projectile_Grenade_Gravity x Projectile_Grenade_Gravity_Lob_Mod (the same product as the
// range rule above). Launch direction for a shot from `muzzle` to `aim` at speed v (per frame): the HIGH ballistic
// arc tan(theta) = (v^2 + sqrt(v^4 - g (g d^2 + 2 dz v^2))) / (g d) ([inferred]: "lob"; the flight body was not
// read); out of reach (negative discriminant) -> 45 degrees, the maximum-range angle of the v^2 / g range rule;
// no horizontal distance -> straight at the aim point. Returns a unit vector.
Vec3 lob_direction(Vec3 muzzle, Vec3 aim, float speed, float gravity);

// ---- area damage (section 3.5) [H] --------------------------------------------------------------
// factor = clamp(1 - floor(d x tiers / R) / (tiers + 1), 0, 1); R <= 0 -> 1.
float area_falloff(float d, float R, int32_t tiers);
// delay = d / R x Max_Secs_For_AE_Delayed_Damage when d >= 1, else 0.
float area_delay_seconds(float d, float R, float max_secs);
// Delayed hit (Projectile_Damage_Delay_Secs = d > 0): Get_Float(0.25 d, d), frames max(1, int(FPS x delay)).
int32_t delay_frames(float delay_seconds, uint32_t fps);

// ---- shields (section 3.7) ----------------------------------------------------------------------
// Regeneration cadence int(FPS x SHIELD_REGEN_INTERVAL x 3.0 + 0.5) [M].
int32_t shield_regen_interval_frames(uint32_t fps, float regen_interval_mult, float recharge_interval_secs);
// Depleted lockout (0x380ad0) [M]: (frame - depletion frame) / FPS < Depleted_Shield_Disable_Time.
bool shield_lockout_active(uint32_t frame, uint32_t depletion_frame, uint32_t fps, float disable_time);
// One regeneration step [H, disasm]: new = clamp(current + amount, 0, max); during the lockout
// new = min(new, cap) where the cap is ABSOLUTE shield points (vanilla 0.25), not a fraction.
float shield_regen_step(float current, float amount, float max_shield, bool lockout, float depleted_cap);

// ---- maxima (section 4.3) -----------------------------------------------------------------------
// Max hull = Tactical_Health x difficulty multiplier x faction factor x (1 + list 2).
inline float max_hull(float tactical_health, float difficulty_mult, float faction_factor, float list2) {
    return tactical_health * difficulty_mult * faction_factor * (1.0f + list2);
}
// Max shield = Shield_Points x faction factor x (1 + list 3).
inline float max_shield(float shield_points, float faction_factor, float list3) {
    return shield_points * faction_factor * (1.0f + list3);
}
// Hardpoint max = Health x mode multiplier (Land 1.0 / Space 1.5) x (1 + list 2).
inline float hardpoint_max_health(float health, float mode_mult, float list2) { return health * mode_mult * (1.0f + list2); }
// Get_Damage_Multiplier (0x38e0c0) [H]: (1 + attacker list 1) x (1 - victim list 0) x AI x cover.
inline float damage_multiplier(float attacker_list1, float victim_list0, float ai_mult, float cover) {
    return (1.0f + attacker_list1) * (1.0f - victim_list0) * ai_mult * cover;
}

}  // namespace alamo::tactical::combat
