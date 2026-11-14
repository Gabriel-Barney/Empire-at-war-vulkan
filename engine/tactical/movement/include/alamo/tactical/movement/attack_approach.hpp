// s78n t25.1: the attack approach on land, pure geometry (docs/re/MOVEMENT.md §4.3.2 items 2-4, 6 and 7) [H unless tagged].
//
// The attack slots 0x438c10 give every formation that cannot fire yet a polar slot (theta, rho) around the target point
// C: rings laid out from the shortest engagement range R inward (0x43d380), the formations of one ring spread by chord
// steps (0x43b520). At every plan a formation with a slot walks start -> Q' -> G (the slot leg), and the path is then cut
// back from its end in 10.0 steps to the outermost point of the last stretch from which every member can fire
// (0x410e90). This file holds those rules as pure functions: no MovementSystem, no grid, no objects. The slot test
// (0x43a070) and the fire test are callbacks the caller supplies (t25.2).
//
// Conventions: angles in radians, counter-clockwise positive, measured from the ring axis u (= -d1, from the target back
// along the approach), so on an approach heading +x a positive angle lies on the approach's right (-y). Slot depth is
// positive ahead of the formation centre (a rear row has depth -15), lateral offsets follow slot_world
// (formation_right(direction) * lateral, formation.hpp). Determinism (rule 3): float +, -, *, /, IEEE sqrt and
// alamo::dmath only; sorts are stable with an object-id tie-break.
#pragma once
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include "alamo/tactical/movement/common.hpp"

namespace alamo::tactical::movement {

// GC +0x154c Base_Land_Targeting_Arc_Angle_Coefficient (0.2) and the arc step of the middle / neighbour search (5 deg).
inline constexpr float kAttackArcCoefficient = 0.2f;
inline constexpr float kAttackArcStepRad = 5.0f * kDegToRad;
// 0x410e90: the trim steps back from the path end in 10.0 world units.
inline constexpr float kFireTrimStep = 10.0f;

// §4.3.2 item 2: the coefficient tries. f = 0.2, then f -= 0.2 while f > -1.0, in float32: exactly six tries
// 0.2, 0, -0.2, -0.4, -0.6, -0.8 (the seventh, about -1.0000001, fails the test).
std::vector<float> attack_arc_tries(float base = kAttackArcCoefficient);
// §4.3.2 item 3: the half arc A(f) = (1 - f) x 90 deg (radians): 72, 90, 108, 126, 144, 162 deg for the six tries.
inline float attack_half_arc(float f) { return (1.0f - f) * dmath::kHalfPiF; }

// §4.3.2 item 4: step(a, b) = acos(sqrt(rho^2 - (a + b)^2 / 4) / rho), written exactly so (asin of half the chord a + b
// on the ring rho). NaN when a + b > 2 rho: every later |theta| <= A test is then false and the placement fails.
float arc_step(float a, float b, float rho);

// Rotation of v about z by th (counter-clockwise), and the ring point C + rho * rot(u, th).
V2 rot(V2 v, float th);
inline V2 arc_point(V2 c, V2 u, float rho, float th) { return c + rot(u, th) * rho; }

// One formation of the attack list L. `index` is the caller's formation index, `id` its lead's object id (the stable
// sort's tie-break), (hx, hy) its half depth / half width (F +0x250 / +0x254). theta / rho are written by a commit run.
struct RingForm {
    uint32_t index = 0;
    ObjectId id = 0;
    V2 centroid;
    float hx = 0.0f;
    float hy = 0.0f;
    float theta = 0.0f;  // out (commit): block +0x3c
    float rho = 0.0f;    // out (commit): block +0x40 (the ring radius rho_k)
};
// The slot test 0x43a070 (§4.3.2 item 5) for formation `f` at the slot point p.
using SlotTestFn = std::function<bool(const RingForm& f, V2 p)>;

// 0x43b520 (§4.3.2 item 4): one ring. The prefix is sorted (a copy; stable, id tie-break) ascending by the centroid's
// projection on the right-hand normal of d0 (d0.y, -d0.x): left-most first. n formations, middle m = n >> 1 at
// theta_m = 0 (odd n) or 2 step(HY_m, 0) (even n; fail at once when > A), tried at theta_0, -theta_0, theta_0 + 5 deg,
// -(theta_0 + 5 deg), ... (from 0: 0, +5, -5, +10, ...) while |theta| <= A. Left neighbours m - 1 .. 0: the first at
// theta_m - 2 step(HY, HY_m), each further one at the previous accepted angle - 1 step(HY, HY_prev); a failed test moves it
// 5 deg further out, below -A fails. Right neighbours mirrored. Every slot point is P(theta) = C + rho_k rot(u, theta).
// True when every formation of the prefix got an angle; with `commit` the angles and rho_k are written into the prefix
// (in its own order; the prefix itself is not reordered).
bool ring_place(std::span<RingForm> prefix, V2 c, V2 u, V2 d0, float rho_k, float half_arc, const SlotTestFn& test, bool commit);

// 0x43d380 (§4.3.2 item 3): the rings of one coefficient try. L is sorted rear-most first by the caller (ascending
// projection of the centroid on d0 from the reference path's start, 0x43b210). R_min = max(r_T, R f), A = (1 - f) 90 deg,
// rho := R; while formations are left: for k = 1, 2, ...: h = the largest HX of the first k, rho_k = rho - h; the
// extension stops when rho - 2h < r_T or ring_place(first k, rho_k) fails. No k fits -> the try fails. Else the longest
// fitting prefix is placed (commit) and removed, and rho := rho - 2h with h of the LAST prefix examined (the one that
// stopped the extension, or the whole remainder when it all fit); rho < R_min -> stop. True iff nothing is left. Without
// `commit` nothing in L is written.
bool ring_layout(std::span<RingForm> l, V2 c, V2 u, V2 d0, float r, float f, float r_t, const SlotTestFn& test, bool commit);

// A member's slot position on a laid-out path (§4.3.2 item 7): q = s + depth; the path point at arc length q (q < 0: the
// start extended backwards along the first segment; q > L: false - the member is not tested at this s - unless `extend`,
// which extends the end along the last segment), shifted by formation_right(that segment's direction) * lateral.
// Zero-length segments are skipped. False for an empty path. A path without any non-zero segment gives its first point
// (no shift) for q <= 0 or with `extend`.
bool trim_point(std::span<const V2> path, float s, float depth, float lateral, bool extend, V2& out);

// One member of the fire trim: its slot offsets and whether it is tested (it has TARGETING and a locomotor pack).
struct TrimMember {
    uint32_t member = 0;  // the caller's index
    float depth = 0.0f;   // ahead of the formation centre (a rear row: negative)
    float lateral = 0.0f;
    bool tested = true;
};
// "This member can fire at the target from p" (non-guard: not inside min range, in max range, line of fire clear; guard:
// within the guard range and the line of fire clear). The caller supplies the rule.
using MemberFireFn = std::function<bool(const TrimMember& m, V2 p)>;

// 0x410e90 (§4.3.2 item 7): the fire trim. L = the path's length, s := L, last good g := L (guard: g := max(0, L -
// guard_range) [L reading of item 7]). At each s every tested member whose trim point exists (trim_point, no extend)
// must pass can_fire; a failure returns g. All pass -> g := s; s == 0 returns 0; else s := max(0, s - 10.0). Returns the
// keep length: the caller cuts the path with cut_points(path, keep) (path_around.hpp; keep 0 leaves the first node).
// When some member fails at the path end nothing is cut (keep = L).
float fire_trim(std::span<const V2> path, std::span<const TrimMember> members, bool guard, float guard_range,
                const MemberFireFn& can_fire);

// §4.3.2 item 6: the slot leg point Q' = G + q rot(-v, theta) (v = the unit direction of the first path's last segment,
// q = the distance the terrain line test from G toward Q = G + rho rot(-v, theta) gets; rho when clear). False when
// q <= 0 (no leg).
bool slot_leg_point(V2 g, V2 v, float theta, float q, V2& out);

}  // namespace alamo::tactical::movement
