// Walk locomotor, one logic frame (docs/re/MOVEMENT.md §3.4-3.6, §3.5.2).
//
// Per frame (no dt): End Move at a speed <= the deceleration ends away from the end (0x15: speed 0 and the move
// restarts; on the last leg within 5 of the last point the unit still crawls at a to the snap - s78n t26.2 [port] until
// t26.4 takes the full 0x15 arrival), start rule A (MOVEMENT §3.5.2 A: turn in place first unless the heading error is within one frame's
// turn, or within Close_Enough_Angle_For_Move_Start with the current waypoint's speed fraction > 0 - the turn itself is
// the caller's 0xF / 0x10 / 0x11, WalkResult::turn), speed controller B (walk_speed: the corner cap, the brake-ahead toward
// the next waypoint's fraction, the collision cap, End Move T = a), move step (the distance budget v is spent along the
// waypoints, several may be passed in one frame), yaw steps toward the travel direction by at most the turn rate (leftover
// > 0 sets the path's turn-done byte), then the passability check before commit: the move old -> new must stay on cells
// passing the unit's MovementClass mask (conservative grid walk); on failure the unit does not move this frame and reports
// `blocked` (s78n t24: the system makes a TerrainCollision record, §5.4.7). Directions use the retail fast inverse sqrt
// (§3.5 step 5).
// PROVISIONAL [L] (until s78n t26.4, the move step C): the last frame snaps onto the goal when the remaining distance is
// <= the frame's budget, and the travel direction is the direction to the waypoint (not the facing).
#pragma once
#include <cstdint>
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

// OI tactical: formation-member extras (MOVEMENT.md §3.5 step 6, §4.4).
//   jiggle      - the formation jiggle offset (data pack +0x12c/+0x130): added to every waypoint except the last
//                 while it is steered toward ([port]: the offset waypoint is used only when its cell passes the
//                 unit's mask, so the jiggle never steers into an obstacle);
//   side_error  - side-error correction (LocomotorCommon 0x42d440): on a leg that does not reach its waypoint this
//                 frame, the direction is the segment direction pulled along the path normal by
//                 formation_side_correction(error), error = the slot's lateral offset (0: the member's path already
//                 leads to its slot [port]) - the member's signed lateral distance from the segment.
//   avoid       - s78n t18 (MOVEMENT §5.4.1 "How Walk combines the vector"): the collision steering vector 0x509810
//                 for the unit's heading `dir` (unit length), asked on every leg but the last. With a non-zero vector the
//                 unit moves along normalise(dir + vector), and a waypoint it passes abeam - it crosses the waypoint's
//                 line, the bisector of the two segments - counts as passed (it does not turn back for it).
//   cap_now     - s78n t20 (MOVEMENT §3.5 step 2, FIGHTER_FLYING §7.7 item 5): the collision records' "at once" flag - a
//                 target speed below the current speed is taken in one frame instead of at the deceleration.
//   tail        - s78n t20 (MOVEMENT §1.5.1 item 5, A1_READINGS §40.3): the path is a collision-owned record on top of a
//                 started order path of this remaining length (> 0): the walk does not brake for the record's end and
//                 reaching it reports `arrived` with the speed kept (the record is popped and the path below goes on).
//   formation   - s78n t26.2 (MOVEMENT §3.5.2 B.2): the formation speed when the path is a formation path (the order
//                 path of a formation member; 3e38: none). 0 sets End Move.
struct WalkExtras {
    V2 jiggle;
    bool side_error = false;
    V2 (*avoid)(const void* ctx, V2 dir) = nullptr;
    const void* avoid_ctx = nullptr;
    bool cap_now = false;
    float tail = 0.0f;
    float formation = 3.0e38f;
};

WalkResult walk_step(LandMotion& m, const LocomotorParams& p, float speed_cap, const PassabilityGrid* grid,
                     const WalkExtras* extras = nullptr);

}  // namespace alamo::tactical::movement
