// Jetpack, flying, bike, land-bomber and fighter locomotors (docs/re/REMAINING_GAPS.md §4.3-4.7; MOVEMENT.md §3.1,
// §3.2). The MovementSystem drives them (movement_system.hpp: "special locomotors"); this header holds the kinds, the
// per-unit state, the animation ids they request and the pure per-frame rules (unit-tested on their own).
//
// Animations: the sim does not play animations, but several transitions wait for one to finish (Jump, FlyLand,
// FlyLandDrop, the turn end anims, MoveStart). The locomotor keeps the requested anim and counts its frames; the
// host supplies the lengths (AnimLengthFn, frames, const and thread-safe; unset or 0 = the anim finishes the frame
// it starts, so a host without animation data still runs every state machine to the end).
//
// Units without a special kind (LocomotorKind::Default: WALK_LOCOMOTOR on land, SIMPLE_SPACE_LOCOMOTOR / the Hermite
// playback in space) are untouched; their CRC bytes do not change.
#pragma once
#include <cstdint>
#include <functional>
#include <span>
#include <string_view>

#include "alamo/core/sim_rng.hpp"
#include "alamo/tactical/movement/common.hpp"
#include "alamo/tactical/movement/locomotor.hpp"
#include "alamo/tactical/movement/space_path.hpp"

namespace alamo::objects {
class GameObjectTypes;
}

namespace alamo::tactical::movement {

enum class LocomotorKind : uint8_t {
    Default = 0,  // WALK_LOCOMOTOR (land) / SIMPLE_SPACE_LOCOMOTOR and the rest (space)
    Bike = 1,        // BIKE_LOCOMOTOR (§4.5)
    Jetpack = 2,     // JETPACK_LOCOMOTOR (§4.3, land only)
    Flying = 3,      // FLYING_LOCOMOTOR (§4.4, land only)
    LandBomber = 4,  // LANDBOMBER_LOCOMOTOR (§4.6)
    Fighter = 5,     // FIGHTER_LOCOMOTOR (§4.7 [M], space)
};
// XML token -> kind (case-insensitive); Default for every other token.
LocomotorKind locomotor_kind_from_token(std::string_view token);
// The type's locomotor for a mode: the first locomotor token of LandBehavior / SpaceBehavior (by mode), then of the
// generic Behavior list (MOVEMENT §3.1: the locomotor occupies behaviour slot 1). This is the primary locomotor of a
// Uses_Multiple_Locomotors type; the swap to Secondary_Locomotor_Name and back is MovementSystem's
// (use_secondary_locomotor, MoveOrder::switch_locomotor; UnitDesc::secondary_locomotor).
LocomotorKind locomotor_kind_from_type(const objects::GameObjectTypes& types, uint32_t type_index, MoveMode mode);

// ModelAnimType ids requested by these locomotors (docs/re/ANIM_PARTICLES.md §A.1).
enum AnimId : int32_t {
    kAnimNone = -1,
    kAnimMove = 2, kAnimTurnLeft = 3, kAnimTurnRight = 4,
    kAnimTurnLeftBegin = 0x0C, kAnimTurnLeftEnd = 0x0D, kAnimTurnRightBegin = 0x0E, kAnimTurnRightEnd = 0x0F,
    kAnimMoveStart = 0x10, kAnimMoveEnd = 0x11,
    kAnimTurnLeftHalf = 0x15, kAnimTurnLeftQuarter = 0x16, kAnimTurnRightHalf = 0x17, kAnimTurnRightQuarter = 0x18,
    kAnimJump = 0x30, kAnimFlyIdle = 0x31, kAnimFlyLand = 0x32, kAnimFlyLandIdle = 0x33, kAnimFlyLandDrop = 0x34,
};
using AnimLengthFn = std::function<uint32_t(ObjectId id, int32_t anim)>;  // frames; const, thread-safe

struct AnimPlay {
    int32_t id = kAnimNone;
    bool loop = false;
    uint32_t frames = 0;  // frames played
    uint32_t length = 0;  // frames (AnimLengthFn)
    float rate = 1.0f;    // presentation rate (turn anims)
    bool done() const { return !loop && frames >= length; }
    bool playing(int32_t a) const { return id == a; }
};

// Per-unit state of a special locomotor (the parts of LocomotorDataPack these locomotors use).
struct SpecialMotion {
    LocoState state = LocoState::None;
    AnimPlay anim;
    bool airborne = false;      // 0x3a7e30 (jetpack)
    bool turn_left = true;      // jetpack: side of the last turn (end anim 0xD / 0xF)
    float lean = 0.0f;          // bike +0x1f8 (degrees; added to the roll); flying: the roll O +0x84
    uint32_t idle_frames = 0;   // flying: formation idle counter (+0x1dc)
    bool turn_back = false;     // flying +0x240
    V2 dest;                    // flying loiter / bomber pass: the formation destination
    bool circling = false;      // flying 0x26 around a fixed point (circle()): centre, radii +0x280 / +0x284
    V2 circle_c;
    float circle_z = 0.0f, circle_rmin = 0.0f, circle_rmax = 0.0f;
    ObjectId circle_target = -1;  // flying 0x26 target object +0x278 (start_circling); -1 = none
    bool has_target = false;    // attack target (flying strafe, fighter 0x1E)
    V2 target;
    float target_z = 0.0f;
    float target_radius = 0.0f;  // flying strafe radius(tgt) (0x397780) [port: 0, set_attack_target carries none]
    // FIGHTER_FLYING §6 (flying): pitch O +0x88 (positive = nose down), velocity V (pack +0x14..+0x1c), current
    // waypoint index of the timed path (path +0x60).
    float pitch = 0.0f;
    V2 vel;
    float vel_z = 0.0f;
    uint32_t path_idx = 0;
    float clock = 0.0f;         // bomber +0x1e8 / timed-path time (frames)
    float pass_z = 0.0f;        // bomber pass height
};

// Type values (raw XML; the system applies the mode multiplier where the getters do).
struct SpecialParams {
    float hover_offset = 0.0f;           // Hover_Offset (+0x1b68)
    float max_lift = 0.8f;               // Max_Lift (+0x3a0) x mode multiplier (0x372440)
    float roll_rate = 2.0f;              // Max_Rate_Of_Roll x mode multiplier (0x3724d0)
    float bank_turn_angle = 70.0f;       // Bank_Turn_Angle (+0x39c)
    float min_speed = 0.0f;              // Min_Speed x mode multiplier (0x370fd0)
    float strafe_distance = 0.0f;        // Strafe_Distance (+0x2118)
    float prepare_strafe_height = 0.0f;  // Prepare_Strafe_Height (+0x1e24)
    bool has_pre_turn_anim = false;      // Has_Pre_Turn_Anim (+0x36)
};

// Land bomber clock (+0x1e8) step per frame: GameConstants 0x140b16c4c (GC +0x144c), value unread [M] -> 1 frame.
inline constexpr float kLandBomberClockStep = 1.0f;
// Jetpack landing test window (0x140a44334): last waypoint time - now <= 7 frames.
inline constexpr float kJetpackLandingFrames = 7.0f;
inline constexpr float kJetpackRelandRadius = 50.0f;  // re-issued move (0x640880, radius 50)

// ---- pure rules -----------------------------------------------------------------------------------------------

// Jetpack Turning (0x10): the turn animation for heading error `err_deg` (positive = left) and turn rate r.
// Has_Pre_Turn_Anim: |e| < r none; |e| < 22.5 quarter (0x16 / 0x18) at r x 0.25 x |e| / 11.25; |e| < 45 half
// (0x15 / 0x17) at r x 0.5; else 3 / 4 at r. Without: 3 / 4 at (|e| / r) x 0.03333 (the GC cap is unread [M]).
struct TurnAnim {
    int32_t anim = kAnimNone;
    float rate = 0.0f;
};
TurnAnim jetpack_turn_anim(float err_deg, float turn_rate, bool has_pre_turn_anim);

// Jetpack altitude (0x6404e0). `track(i)` = the unit's position i frames ahead (path playback, or position +
// facing x speed x i); `terrain(p)`; `inside(p)` = inside the map box. base = slot z offset (0 [port]) + Hover_Offset;
// for i in [0, 60) inside the box: h = max(terrain + Hover_Offset, floor), h > base -> (h - base)(1 - i/60) + base;
// target = the maximum h (z itself when no sample is inside). z steps toward it by at most +-max_lift, then is
// clamped to >= terrain(here) and >= floor.
using TrackFn = std::function<V2(uint32_t frames_ahead)>;
using TerrainFn = std::function<float(V2 p)>;
using InsideFn = std::function<bool(V2 p)>;
float jetpack_altitude_target(const TrackFn& track, const TerrainFn& terrain, const InsideFn& inside, float hover_offset,
                              float map_floor, float current_z);
float step_altitude(float z, float target, float max_lift, float terrain_here, float map_floor);

// Bike lean (§4.5): turn anim 3 -> lean -= rate; 4 -> lean += rate; otherwise decays toward 0 by rate; clamped to
// +-bank_turn_angle.
float bike_lean(float lean, int32_t turn_anim, float roll_rate, float bank_turn_angle);

// ---- FLYING_LOCOMOTOR (docs/re/FIGHTER_FLYING.md §6; corrections §9.2 / §9.3) ---------------------------------
// Angles in degrees; positive pitch is nose down; forward = (cos Y cos Pi, sin Y cos Pi, -sin Pi) (§1.1). Every
// sin / cos below takes the radian argument (+-angle) x k (k = 0x3C8EFA35) with the tiny-angle shortcut (|x| < 2^-69:
// sin x = x, cos x = 1) and dmath in place of the CRT sinf / cosf / atan2f (the port's determinism rule).
struct FlyVec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    friend bool operator==(FlyVec3 a, FlyVec3 b) { return a.x == b.x && a.y == b.y && a.z == b.z; }
};
inline FlyVec3 operator-(FlyVec3 a, FlyVec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
struct FlyOrient {  // O = obj +0x84 roll, +0x88 pitch, +0x8c yaw
    float roll = 0.0f, pitch = 0.0f, yaw = 0.0f;
};
struct FlyBody {  // the flyer: P (z included), O, V (pack +0x14) and the scalar speed s (pack +0xec)
    FlyVec3 pos;
    FlyOrient o;
    FlyVec3 vel;
    float speed = 0.0f;
};
struct FlyRates {  // type getters (§1.4): mode multiplier applied where the getters apply it
    float turn = 0.0f;           // turn(T): Max_Rate_Of_Turn (yaw budget)
    float roll = 0.0f;           // roll(T): Max_Rate_Of_Roll
    float lift = 0.0f;           // lift(T): Max_Lift (pitch budget)
    float bank = 70.0f;          // bank(T): Bank_Turn_Angle (raw)
    float max_speed = 0.0f;      // maxspeed(T, obj)
    float hover_offset = 0.0f;   // Hover_Offset (+0x1b68)
    float prepare_strafe_height = 0.0f;  // Prepare_Strafe_Height (+0x1e24)
};
// Tracker 0x538830 / 0x538510: the position and facing of the flyer `frames_ahead` frames from now; false = none.
using FlyTrackFn = std::function<bool(uint32_t frames_ahead, FlyVec3& pos, V2& facing)>;
// Clearance 0x63b640 [M]: raises the minimum height M over objects along from -> to; returns the new M.
using FlyClearanceFn = std::function<float(FlyVec3 from, FlyVec3 to, float min_height)>;
struct FlyWorld {
    TerrainFn terrain;            // bare terrain height (unset = 0)
    bool has_box = false;         // the map / playable box ([port]: one 2-D box for both, SpaceOccupancy::air())
    V2 box_min, box_max;
    float floor = -3.0e38f;       // map floor (0x136650)
    FlyTrackFn track;             // unset = no tracker position (the look-ahead extrapolates)
    FlyClearanceFn clearance;     // unset = M unchanged (headless: no model boxes)
    bool inside(V2 p) const {
        return !has_box || (p.x >= box_min.x && p.x <= box_max.x && p.y >= box_min.y && p.y <= box_max.y);
    }
    float height(V2 p) const { return terrain ? terrain(p) : 0.0f; }
};

// §1.1 / §1.2 helpers: wrap180 (-180, 180], wrap360 [0, 360) (the retail subtract / add loops); angles(A -> B) =
// (roll 0, pitch, yaw) with yaw in [0, 360) and the "pitch only when z or x differ" quirk.
float fly_wrap180(float a);
float fly_wrap360(float a);
FlyOrient fly_angles(FlyVec3 from, FlyVec3 to);

// §6.1 timed follower (vf +0x1d8, 0x63c340): the SimpleSpace Hermite (basis rows a0 (1,0,0,0), a1 (0,0,1,0),
// a2 (-3,3,-2,-1), a3 (2,-2,1,1), [H]) evaluated in the spec's operand order, with STRICT bracketing (advance while
// wp[idx+1].time < T) and a bank from the yaw change; pitch and z are untouched (§6.3 owns them). `idx` is the path's
// current index (path +0x60), advanced in place. T = path time now (frames). Port waypoints are planar: the facing
// vector is (cos h, sin h, 0) from the heading [port]. complete = idx + 1 >= count (the caller pops the path).
// NOTE: the space playback (eval_space_path) keeps its own [M] operand order and non-strict bracketing; it is not
// this function (the space golden is unchanged).
struct FlyFollow {
    FlyVec3 pos;
    FlyOrient o;
    float speed = 0.0f;
    bool complete = false;
};
FlyFollow flying_follow(std::span<const SpaceWaypoint> wps, uint32_t& idx, float T, const FlyBody& b, const FlyRates& r);
// [port] headless tracker: the unit's own reservation, i.e. its timed waypoints interpolated LINEARLY at path time t
// (the tracker interpolates swept segments by time, MOVEMENT §2.2), held at the ends; z = `z`; facing = the segment
// chord direction (the waypoint heading on a zero-length segment) [L]. False without waypoints.
bool flying_track_linear(std::span<const SpaceWaypoint> wps, float t, float z, FlyVec3& pos, V2& facing);

// §6.2 attitude step (0x63d530): yaw toward want.yaw by at most `yaw_budget` (gate: only once the roll is on the
// turning side, s(R) == -s(dy)); roll toward -5 x dy by at most roll(T), clamped to +-bank(T); pitch toward
// want.pitch by at most `pitch_budget`. Budgets are consumed. Out = (R, wrap180(p), wrap180(Y)).
FlyOrient flying_attitude_step(FlyOrient cur, FlyOrient want, float& yaw_budget, float& pitch_budget, bool gate,
                               float roll_rate, float bank);

// §6.3 altitude and pitch step (0x63b920): `dest` = the formation destination (x, y, z) or null; pitch target from a
// 60-sample terrain look-ahead (tracker when use_track, else facing x s), limited to +-30 (a dive also to the
// destination's depression angle); pitch steps by lift(T); z += s x tan(-pitch); z clamped >= terrain and floor.
void flying_altitude_step(FlyVec3& pos, FlyOrient& o, float speed, bool use_track, const FlyVec3* dest,
                          const FlyRates& r, const FlyWorld& w);

// §6.6 loiter step 1: P' = P + (cos Y, sin Y) x s, attitude toward (0, pitch, yaw) with the gate, altitude without the
// tracker; V = P' - P when P' != P.
void flying_loiter_move(FlyBody& b, const FlyVec3* dest, const FlyRates& r, const FlyWorld& w);
// §6.6 re-issue without an attack target (§9.2): L = 1.1 x XYExpansionDistanceLand, D = the formation destination.
// |P - D| >= 0.3 L: dir = normalise(D - P) rotated by ONE synced float(-90, +90) drawn here; otherwise dir = -(cos Y,
// sin Y) and turn_back = true (no draw). Returns D + dir x L.
V2 flying_loiter_reissue(V2 pos, V2 dest, float yaw_deg, float L, SimRng& rng, bool& turn_back);
// §6.6 re-issue with an attack target (§9.2): only when the 2-D distance unit -> aim < radius + 0.3 x Strafe_Distance
// is the aim moved by (cos Y, sin Y) x Strafe_Distance (turn_back = true); otherwise the target itself.
V2 flying_strafe_aim(V2 pos, V2 aim, float yaw_deg, float target_radius, float strafe_distance, bool& turn_back);
inline constexpr float kLoiterDistanceCoefficient = 1.1f;  // 0x140a4432c
inline constexpr uint32_t kLoiterIdleFrames = 2 * 30 + 1;  // re-issue when idle > 2 FPS + 1 frames

// §6.5 circling (0x63db30, §9.3). Height H = max(terrain(P + V), target z, terrain(P + k V) k = 2..11) + Hover_Offset.
float flying_circle_height(const FlyWorld& w, FlyVec3 pos, FlyVec3 vel, float target_z, float hover_offset);
// Vertical step: clamp(H - z, +-vmax); a descent is scaled by -(H - z) / max(Prepare_Strafe_Height - Hover_Offset,
// 1) with NO second clamp.
float flying_circle_dz(float H, float z, float max_speed, float prepare_strafe_height, float hover_offset);
// One circling frame around `target` (null: P += V only): heading away inside rmin, toward beyond rmax, else the
// tangent on the velocity's side; attitude (no gate); V = forward x vmax; up to 3 terrain-clear retries; the
// orientation takes the attitude roll and angles(P -> Q).
void flying_circle_step(FlyBody& b, const FlyVec3* target, float rmin, float rmax, const FlyRates& r, const FlyWorld& w);

// Host hooks of the flying locomotor (MovementSystem::set_flying_hooks). All optional; const and called from the
// serial special-locomotor pass.
struct FlyingHooks {
    // Position of a circling target that is not a mover of this MovementSystem (structures, other modes' objects);
    // false = no such object (circling then coasts on V). Movers are read from the system itself.
    std::function<bool(ObjectId id, FlyVec3& pos)> object_position;
    // Tracker override (0x538830 / 0x538510) for `id` at absolute frame `frame`; unset = the [port] own-path tracker.
    std::function<bool(ObjectId id, uint32_t frame, FlyVec3& pos, V2& facing)> track;
    // Clearance 0x63b640 (§6.3.1): M raised over the Corvette-layer (4) objects the flyer's swept footprint from -> to
    // touches; unset = none. The battle host implements it with MovementSystem::clearance_query.
    std::function<float(ObjectId id, FlyVec3 from, FlyVec3 to, float min_height)> clearance;
};

// ---- §6.3.1 clearance query (0x63b640) ------------------------------------------------------------------------------
// Locomotor kind vf +0x238 of the active locomotor (vf +0x240) == 2 ("airborne"): Flying and LandBomber always; JetPack
// with pack state 0 or 0x0F..0x13 (None, the turn / jump states and Moving); every other kind / state is kind 1.
bool locomotor_airborne_kind(LocomotorKind kind, LocoState state);
// Object-tracking layer 0x397900(obj, moving): kLayerCorvette when Uses_Multiple_Locomotors and the active locomotor is
// airborne; else Space_Layer when moving or when Stationary_Space_Layer is 0; else Stationary_Space_Layer. The object
// is in the clearance layer when (layer & kLayerCorvette) != 0 (0x537e60).
uint32_t object_tracking_layer(bool uses_multiple_locomotors, bool airborne_kind, bool moving, uint32_t space_layer,
                               uint32_t stationary_space_layer);
// Query half-extents 0x396850: Custom_Hard_XExtent / _YExtent x Scale_Factor, each falling back ([port]: when <= 0) to
// |model bbox max x / y| x Scale_Factor (0x371be0); 0 without a model.
V2 clearance_half_extents(float custom_x, float custom_y, float scale_factor, bool has_model, float model_max_x,
                          float model_max_y);
// One tracking entry (0x6b3b00 [M]): the object's tracked position at the slice start / end, its facing at the slice
// end, its half-extents, and the slice's frame range.
struct ClearanceEntry {
    V2 s, e;
    V2 facing;
    float ex = 0.0f, ey = 0.0f;
    float t0 = 0.0f, t1 = 0.0f;
};
// Candidate test (search 0x5d7c00): the entry AABB ([M]: {s, e} grown by max(ex, ey)) overlaps, strictly, the AABB of
// {from, to} grown by max(qx, qy) on every side.
bool clearance_candidate(V2 from, V2 to, float qx, float qy, const ClearanceEntry& e);
// Hit test 0x6b24f0 at frame t, in double (§6.3.1 step 5): the flyer's point from + r (to - from), r in [0, 1], comes
// within R(r) of A = s + (e - s) a, R lerping the two oriented-box radii from the direction toward s (at r = 0) to the
// direction toward e (at r = 1). Heights are not compared.
bool clearance_hit(V2 from, V2 to, float qx, float qy, const ClearanceEntry& e, float t);
// Per-hit value: (bbox.min.z + bbox.max.z) x Scale_Factor + object z (a sum: the scaled top of a box starting at 0).
inline float clearance_top(float box_lo_z, float box_hi_z, float scale_factor, float obj_z) {
    return (box_lo_z + box_hi_z) * scale_factor + obj_z;
}
// Per-object host data for MovementSystem::clearance_query (type values; [port] the land model of the type).
struct ClearanceInfo {
    bool uses_multiple_locomotors = false;  // Uses_Multiple_Locomotors (+0x74)
    uint32_t stationary_space_layer = 0;    // Stationary_Space_Layer (+0x16e0); Space_Layer is UnitDesc::space_layer_type
    V2 half;                                // clearance_half_extents (the flyer's (qx, qy); an entry's (ex, ey) [M])
    bool has_box = false;                   // the land model (type +0x2a0) has a bounding box; no box = no hit value
    float box_lo_z = 0.0f, box_hi_z = 0.0f; // model space, before Scale_Factor
    float scale_factor = 1.0f;              // Scale_Factor (+0x658)
};
using ClearanceInfoFn = std::function<bool(ObjectId id, ClearanceInfo& out)>;  // false = unknown object (skipped)
// The space fighter's pure rules (pitch fold >= 90, steering, cell grids) live in fighter.hpp (FIGHTER_FLYING §1-§5).

}  // namespace alamo::tactical::movement
