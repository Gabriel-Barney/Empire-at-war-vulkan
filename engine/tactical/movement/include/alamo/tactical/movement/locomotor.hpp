// Walk locomotor, one logic frame (docs/re/MOVEMENT.md §3.4-3.6).
//
// Per frame (no dt): start rule (turn in place first when the heading error exceeds both one
// frame's turn and Close_Enough_Angle_For_Move_Start), speed controller (target = effective max
// speed capped by the formation / collision speed cap; v steps toward the target by at most accel up
// or decel down), move step (the distance budget v is spent along the waypoints, several may be
// passed in one frame), yaw steps toward the travel direction by at most the turn rate, then the
// passability check before commit: the move old -> new must stay on cells passing the unit's
// MovementClass mask (conservative grid walk); on failure the unit does not move this frame and
// reports `blocked` (the system re-plans, §1.6 re-path triggers). Directions use the retail fast
// inverse sqrt (§3.5 step 5).
// PROVISIONAL [L]: the brake rule is "target <= sqrt(2 * decel * remaining path)" (the retail brakes
// toward the next waypoint's speed cap; here only the final waypoint has cap 0), and the last
// frame snaps onto the goal when the remaining distance is <= the frame's budget. Movement
// direction = desired direction (Movement_Directions_Count <= 0 behaviour) for every unit.
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
};

struct WalkResult {
    bool moved = false;
    bool arrived = false;
    bool blocked = false;
};

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
struct WalkExtras {
    V2 jiggle;
    bool side_error = false;
    V2 (*avoid)(const void* ctx, V2 dir) = nullptr;
    const void* avoid_ctx = nullptr;
    bool cap_now = false;
    float tail = 0.0f;
};

WalkResult walk_step(LandMotion& m, const LocomotorParams& p, float speed_cap, const PassabilityGrid* grid,
                     const WalkExtras* extras = nullptr);

}  // namespace alamo::tactical::movement
