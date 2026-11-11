// Walk locomotor, one logic frame (docs/re/MOVEMENT.md §3.4-3.6, §3.5.2).
//
// Per frame (no dt), s78n t26.4 (MOVEMENT §3.5.2 A - C, §3.5.1 "Arrival turn, details" item 1): End Move at a speed <= the
// deceleration enters 0x15 (speed 0; heading for the last waypoint within 5 of it along its segment the index steps on; on
// the last waypoint of a started path the unit has arrived where it stands, else the move restarts from state 0; no
// started path: idle); start rule A (turn in place first unless the heading error is within one frame's turn, or within
// Close_Enough_Angle_For_Move_Start with the current waypoint's speed fraction > 0 - the turn itself is the caller's 0xF /
// 0x10 / 0x11, WalkResult::turn); then the move step C: speed controller B (walk_speed), the turn budget R (x v / vmax in
// End Move), and the loop over the waypoints - nothing left to walk: pop into a started path below (no more turning) or
// End Move and coast along the last frame's displacement; a waypoint is passed only by crossing its line (-0.001); turn
// toward the wanted yaw (segment direction + lane term + collision vector) by the budget left; travel along the facing to
// the waypoint line (s' = |D / tau|, tau = 0 or a degenerate segment skips the waypoint); every step is point-tested at its
// new position (0x50e280) unless the path is forced - a refused point reports `blocked` with no commit (s78n t24: the
// system makes a TerrainCollision record, §5.4.7); v^2 / (2a) > the rest of the path -> End Move unless a started path
// lies below. Directions use the retail fast inverse sqrt (§3.5 step 5).
#pragma once
#include <cstdint>
#include <span>
#include <vector>

#include "alamo/tactical/movement/passability.hpp"

namespace alamo::tactical::movement {

// LocomotorStateType values (§3.2) used by the port.
enum class LocoState : uint8_t {
    None = 0,
    WalkStartTurn = 0x0F,
    WalkTurning = 0x10,    // REMAINING_GAPS §4.3 (jetpack)
    WalkEndTurn = 0x11,
    WalkStartMove = 0x12,
    WalkMoving = 0x13,
    WalkEndMove = 0x14,
    WalkStopped = 0x16,
    WalkDeploying = 0x18,    // S8 t11 (§3.2 "Deploy states"): Walk only
    WalkDeployed = 0x19,
    WalkUndeploying = 0x1A,
    FighterMoving = 0x1B,  // §4.7
    FighterIdle = 0x1C,
    FighterIdleCombat = 0x1D,
    FighterDirectedCombat = 0x1E,
    FighterDeadStop = 0x1F,
    BikeTurningInPlace = 0x20,  // §4.5
    BikeStartMove = 0x21,
    BikeMoving = 0x22,
    BikeEndMove = 0x23,
    BikePreStopped = 0x24,
    BikeStopped = 0x25,
    FlyCircling = 0x26,  // §4.4
    FlyNoPath = 0x27,
};

struct LocomotorParams {
    float max_speed = 0.0f;   // effective max speed, units / frame (mode multiplier applied)
    float accel = 0.0f;       // units / frame^2 (<= 0 in the type -> = max_speed, §3.4)
    float decel = 0.0f;
    float turn_rate = 0.0f;   // degrees / frame
    uint32_t mask = kPassClear;
    float close_enough_angle = 180.0f;
};

struct LandMotion {
    V2 pos;
    float yaw = 0.0f;          // degrees
    float speed = 0.0f;
    LocoState state = LocoState::WalkStopped;
    std::vector<V2> path;      // remaining waypoints are path[next..]
    uint32_t next = 0;
    uint32_t escape_until = 0; // skip the passability check until `next` reaches this (blocked start)
    // s78n t26.2 (MOVEMENT §3.5.2 notation, §5.4.7 item 6): the path record's per-waypoint speed fractions (entry +4; the
    // +0 turn factor is always 1.0 and not stored) - frac[i] for waypoint i, 1.0 past the end of the vector or of the
    // path (empty: every waypoint 1.0); turn_done = the record's turn-done byte +0x6c (set by the move step once the turn
    // has rate left over, cleared at every waypoint index step).
    std::vector<float> frac;
    bool turn_done = false;
    // s78n t26.4 (MOVEMENT §3.5.2 C.4, G): last_dir = the unit's whole previous frame's 2-D displacement (pack +0x170 -
    // +0x1a0: P(end of frame N - 1) - P(end of frame N - 2), set by the system at the start of each land tick), the End Move
    // coast direction; started = the record's "started" byte +0x6d (an order path set while the unit moves keeps it only
    // when aligned, else the walk coasts to a stop and starts it from state 0x16).
    V2 last_dir;
    bool started = true;
};

// s78n t26.2: the speed fraction of waypoint i (1.0 past the end; 0x5c4b80 / 0x5c4af0, the static (1.0, 1.0) default).
inline float wp_frac(const LandMotion& m, size_t i) {
    return i < m.frac.size() && i < m.path.size() ? m.frac[i] : 1.0f;
}

struct WalkResult {
    bool moved = false;
    bool arrived = false;
    bool blocked = false;
    // s78n t26.2 (MOVEMENT §3.5.2 A): the start rule failed - the unit turns in place (state 0xF, then 0x10 / 0x11 as the
    // arrival turn of §3.5.1) toward `want` (degrees) before it moves; speed 0, nothing else this frame.
    bool turn = false;
    float want = 0.0f;
    // s78n t26.4: popped = the walk finished the top record and went on along the started path below in the same call
    // (C.4; the caller pops the record); coast = an End Move coast step ran (C.4); restart = 0x15 away from the end (the move
    // restarts from state 0); tau_skips / degen_skips = waypoints skipped by tau = 0 (C.9) / a degenerate segment (C.8).
    bool popped = false;
    bool coast = false;
    bool restart = false;
    uint8_t tau_skips = 0, degen_skips = 0;
};

// s78n t26.2 (MOVEMENT §3.5.2 B, FIGHTER_FLYING §7.7 item 3): the speed controller 0x61ccd0 / the container's 0x5aaf70 -
// a pure step. Items: T = vmax; a formation path T = its formation speed (0 -> End Move); not started -> T = a; the corner
// cap (turn-done clear, the facing not strictly within |R| of the wanted yaw, not Start Move) T = min(T, frac(idx) x vmax),
// 0 -> End Move; the brake-ahead (not Start Move): vn = frac(idx + 1) x vmax < v and D <= (v^2 - vn^2) / (2a) in double ->
// T = min(T, vn), 0 -> End Move; the collision cap (and the straggler cap of a container) T = min(T, cap); End Move ->
// T = a; then v += min(acc, T - v) up, v := T with the "at once" flag else v -= min(a, v - T) down; v := min(v, vmax), 0
// when negative.
struct WalkSpeedIn {
    float v = 0.0f, vmax = 0.0f, acc = 0.0f, a = 0.0f;
    float formation = 3.0e38f;  // B.2: the formation speed on a formation path (3e38: not one)
    float cap = 3.0e38f;        // B.6: the collision cap (min the straggler cap for a container)
    bool now = false;           // B.6: the "at once" flag
    bool started = true;        // B.3
    bool turn_done = false;     // B.4: P +0x6c
    float yaw_err = 0.0f;       // B.4: |wrap(yaw - wanted)|
    float rate = 0.0f;          // B.4: R (the turn rate)
    float frac_cur = 1.0f;      // B.4: frac(idx)
    float frac_next = 1.0f;     // B.5: frac(idx + 1)
    float line_d = 3.0e38f;     // B.5: D, the distance along d to line(idx + 1) (3e38: none)
    LocoState state = LocoState::WalkMoving;
};
struct WalkSpeedOut {
    float v = 0.0f;
    bool end_move = false;  // the controller set state 0x14 (End Move)
};
WalkSpeedOut walk_speed(const WalkSpeedIn& in);

// s78n t26.2 (MOVEMENT §3.5.2 C.9): D = the distance from pos along d (unit) to the waypoint line through l0 with unit normal
// n: mu = cross(l0 - pos, d) / (d . n), X = l0 + mu (-n.y, n.x), D = |pos - X| (2-D). d . n = 0: 3e38 (no line ahead).
float line_distance_along(V2 pos, V2 d, V2 l0, V2 n);

// Remaining path length from pos through path[next..].
float remaining_length(const LandMotion& m);

// Extras of one walk (MOVEMENT §3.5.2 C, §5.4.1 "How Walk combines"):
//   jiggle      - the formation jiggle offset (data pack +0x12c / +0x130, §3.5 step 6): on a formation path its side
//                 component enters the lane term e (the slot's side offset is 0: [port] the member's path already leads to
//                 its slot);
//   lane        - s78n t26.4 (MOVEMENT §5.4.1 "How Walk combines" item 2): the lane term pack +0x15c of the formation
//                 update 0x41dbb0 - k = formation_side_correction(e), e = the jiggle's side (formation path) - the unit's
//                 signed side offset from its current segment; the wanted direction is normalise(d + k (-d.y, d.x));
//   formation_path - the record is the order path of a formation member (the jiggle term, B.2's formation speed);
//   avoid       - s78n t18 (§5.4.1 "How Walk combines" item 3): the collision steering vector 0x509810 for the wanted
//                 direction `dir` (unit length), added to it on every leg until the last line is crossed;
//   cap_now     - s78n t20 (§3.5 step 2, FIGHTER_FLYING §7.7 item 5): the collision records' "at once" flag;
//   formation   - s78n t26.2 (§3.5.2 B.2): the formation speed on a formation path (3e38: none), 0 sets End Move;
//   below       - s78n t26.4 (§3.5.2 C.4 / C.11): Q, the started order path under a collision-owned top record (null: none
//                 or not started) - no End Move for the record's end, and the walk pops into it in the same call;
//                 below_kind / below_resume: the record's kind and rejoin index (kind 2: Q resumes at least there),
//                 below_forced: Q's skip-passability flag;
//   forced      - the top path's skip-passability flag +0x5c (the flag-0x20 order path); the planner's escape leg off a
//                 blocked start (next < escape_until) counts as forced too;
//   directions  - Movement_Directions_Count (C.7: 1 the facing, 2 the facing or its reverse, <= 0 the wanted direction);
//   vmax_u      - C.1 / C.2: the undeployed effective max speed for f = v / vmax_u (<= 0: the controller's vmax).
struct WalkExtras {
    V2 jiggle;
    bool lane = false;
    bool formation_path = false;
    V2 (*avoid)(const void* ctx, V2 dir) = nullptr;
    const void* avoid_ctx = nullptr;
    bool cap_now = false;
    float formation = 3.0e38f;
    LandMotion* below = nullptr;
    uint8_t below_kind = 0;
    uint32_t below_resume = 0;
    bool below_forced = false;
    bool forced = false;
    int32_t directions = 1;
    float vmax_u = 0.0f;
};

// s78n t26.4 (MOVEMENT §5.4.1 "How Walk combines" items 1 - 3, 0x61d570): the wanted direction on the top record m - false
// when there is none (no path, not started, on its last waypoint: m.next >= size).
bool walk_wanted_dir(const LandMotion& m, const WalkExtras* x, V2& dir);
bool walk_wanted_dir(std::span<const V2> path, uint32_t next, V2 pos, const WalkExtras* x, V2& dir);

// s78n t26.4: the index a record resumes at when the record above it (kind, rejoin index) is popped (pop_top's rule):
// max(next, min(resume, size)) for a kind-2 detour, at least 1.
uint32_t resume_index(const LandMotion& q, uint8_t kind, uint32_t resume);

WalkResult walk_step(LandMotion& m, const LocomotorParams& p, float speed_cap, const PassabilityGrid* grid,
                     const WalkExtras* extras = nullptr);

}  // namespace alamo::tactical::movement
