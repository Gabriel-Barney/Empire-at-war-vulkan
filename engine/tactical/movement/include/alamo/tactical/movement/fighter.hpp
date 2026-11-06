// Space fighter locomotor (FIGHTER_LOCOMOTOR; docs/re/FIGHTER_FLYING.md §1-§5 and §9.1 / §9.4, MOVEMENT.md §2.5,
// §3.1): the per-frame primitives, the per-unit state and the two fighter cell grids. The MovementSystem drives the
// states (movement_system.hpp, "fighters").
//
// Fighters steer reactively in every state (§0.1, §9.1): each frame yaw (with bank), pitch and speed turn toward one
// aim point under three per-frame budgets (turn rate, Max_Lift, Max_Thrust), then the fighter moves by P += V. Planned
// space paths are only waypoint positions with a "passed the waypoint" test; there is no Hermite playback.
//
// Units are frames (MOVEMENT §0): speeds units / frame, rates degrees / frame, timers frames. Angles are degrees;
// the orientation is (roll, pitch, yaw) with positive pitch = nose down and yaw counter-clockwise from +x (§1.1);
// fighter orientation angles are kept in [0, 360) (wrap360) as the retail setter does.
//
// Determinism: every function is a fixed sequence of float +, -, *, / , IEEE sqrt and alamo::dmath sin / cos / atan2
// (the retail CRT sinf / cosf / atan2f are replaced by dmath, like the rest of the sim [port]); the operand order follows
// the spec (the (dis) groupings exactly; unmarked sums of three or more terms left to right [M]). No FMA. No RNG: the
// fighter states and grids never draw (spec "RNG summary").
#pragma once
#include <bit>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include "alamo/core/bytes.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/tactical/movement/common.hpp"
#include "alamo/tactical/movement/space_path.hpp"

namespace alamo::objects {
class GameObjectTypes;
}

namespace alamo::tactical::movement {

// ---- 3-D helpers ---------------------------------------------------------------------------------------------------
struct F3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    friend bool operator==(F3 a, F3 b) { return a.x == b.x && a.y == b.y && a.z == b.z; }
};
inline F3 f3_add(F3 a, F3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline F3 f3_sub(F3 a, F3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline F3 f3_scale(F3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
// sqrtf((x^2 + y^2) + z^2) (the order of §3.4 / §3.9).
inline float f3_length(F3 a) { return std::sqrt((a.x * a.x + a.y * a.y) + a.z * a.z); }

// Orientation (obj +0x84 roll, +0x88 pitch, +0x8c yaw), degrees.
struct FAttitude {
    float roll = 0.0f, pitch = 0.0f, yaw = 0.0f;
};

// §1.1 constants: k = 0.017453292f (bits 0x3C8EFA35), 57.295776f (bits 0x42652EE0), 2 pi = 6.2831855f.
inline constexpr float kFighterDegToRad = std::bit_cast<float>(0x3C8EFA35u);
inline constexpr float kFighterRadToDeg = std::bit_cast<float>(0x42652EE0u);
inline constexpr float kFighterTwoPi = 6.2831855f;
// GameConstants SpaceIdleMovementSpeed (GC +0x1554): FoC 0.0 (MOVEMENT §2.7). [port] constant; not in
// MovementConstants.
inline constexpr float kSpaceIdleMovementSpeed = 0.0f;
// lock(x) duration: int(float(FPS) x 10.0) = 300 frames (§4.5).
inline constexpr uint32_t kFighterLockFrames = static_cast<uint32_t>(30.0f * 10.0f);
// Avoidance hold after an evasion (pack +0x2b4 = 15, §3.5).
inline constexpr int32_t kFighterAvoidHold = 15;

// §1.1 wrap180 (0x20b6d0): result in (-180, 180]; wrap360 (0x20b710): [0, 360). The retail while loops;
// [port] guard: non-finite values are returned unchanged and |a| > 1e6 is reduced with fmod first (the retail loop
// would not terminate).
float fighter_wrap180(float a);
float fighter_wrap360(float a);
// §1.1 sin / cos with the tiny-angle shortcut: (bits(x) & 0x7F800000) < 0x1D000001 -> sin x = x, cos x = 1.
float fighter_sin(float rad);
float fighter_cos(float rad);

// §1.2 angles of a vector (0x20acd0), from A to B: yaw in [0, 360) (0 when x and y are equal), pitch =
// -(atan2(dz, sqrt(dy^2 + dx^2)) x 57.295776), computed only when A.z != B.z or A.x != B.x (the retail quirk: a
// vector along y alone gets pitch 0).
struct FAngles {
    float pitch = 0.0f, yaw = 0.0f;
};
FAngles fighter_vector_angles(F3 a, F3 b);
// §1.3 fast normalise (0x16f570): s = (x x + y y) + z z; s > 0 -> v x fast_rsqrt_retail(s) (two Newton steps).
F3 fighter_fast_normalise(F3 v);

// ---- per-unit type values (§1.4 getters) ---------------------------------------------------------------------------
struct FighterKin {
    float turn = 0.0f;       // turn(T): Max_Rate_Of_Turn x mode multiplier (0x372560)
    float roll = 0.0f;       // roll(T): Max_Rate_Of_Roll x mode multiplier (0x3724d0)
    float lift = 0.0f;       // lift(T): Max_Lift x mode multiplier (0x372440) = the pitch rate
    float bank = 70.0f;      // bank(T): Bank_Turn_Angle, raw
    float thrust = 1.0f;     // thrust(T): Max_Thrust, raw (the per-frame speed-change budget)
    float min_speed = 0.0f;  // minspeed(T, obj) (0x3728f0)
    float max_speed = 0.0f;  // maxspeed(T, obj) (0x370f00)
};

// Position, velocity per frame (pack +0x14), orientation and the loop flag (pack +0x1d4).
struct FighterBody {
    F3 p;
    F3 v;
    FAttitude o;
    bool loop = false;
};

// ---- §3 steering primitives ----------------------------------------------------------------------------------------
struct RelAngles {
    float yaw = 0.0f, pitch = 0.0f;  // RelYaw (positive = left), RelPitch (positive = below)
};
// §3.1 steps 2-3: the vector d (target - position) in the body frame of `o`, as wrap180'd relative angles.
RelAngles fighter_body_angles(F3 d, FAttitude o);
// §3.2 yaw with bank (0x5c95a0): budget B by reference.
void fighter_yaw_bank_step(FAttitude& o, const FighterKin& k, float dy, float& budget);
// §3.3 pitch step (0x5c8fb0).
void fighter_pitch_step(FAttitude& o, float dp, float& budget);
// §3.4 speed and velocity (0x5c9360): |V| steps toward v by the budget, V = the forward of the current o x speed.
void fighter_speed_step(FighterBody& b, float v, float& budget);
// §3.1 steer_to (0x5caaf0): returns without changes when a budget is <= 0. `idle_state` = locomotor state 0x1C
// (no loop there). The unread 7th argument is not modelled.
void fighter_steer_to(FighterBody& b, const FighterKin& k, F3 aim, float v, float& turn_budget, float& thrust_budget,
                      float& lift_budget, bool idle_state);
// §3.9 cone (0x5cb1f0): false when |A - from|^2 > r2; else the relative angles (written to `rel`) within the limits.
bool fighter_cone(F3 a, F3 from, FAttitude o, float yaw_limit, float pitch_limit, float r2, RelAngles& rel);
// §2 step 4 pitch fold: p = wrap180(pitch); |p| >= 90 (not > 90) -> roll + 180, pitch = wrap360(((180 - |p|) - lift) x
// sign(p)) (sign + for p >= 0), yaw + 180, all wrap360.
FAttitude fighter_fold_pitch360(FAttitude o, float lift);
// Turn radius ((360 / turn) x la) / 6.2831855 (§3.6).
float fighter_turn_radius(float turn, float la);
// §3.6 step 4 look-ahead (0x5ca860). `path` = the usable path (empty: not the leader, or the container has no
// current path); idx = path +0x60. `solo` = no squadron. The lateral offset pack +0x15c [L] is 0 [port].
F3 fighter_lookahead(std::span<const SpaceWaypoint> path, uint32_t idx, F3 base, F3 dir, bool solo, float la, float turn);
// §3.6 step 5 glide slope: the z goal blended toward P.z inside the level-out distance.
float fighter_glide_z(float z_goal, F3 aim, F3 p, float pitch, float lift_budget, float la);
// §3.6 station_speed (0x5ca4c0), the in-squadron part (the caller returns v outside a squadron).
struct StationInput {
    float v = 0.0f, lo = 0.0f, hi = 0.0f;
    F3 p, leader_p, leader_v;
    F3 slot_leader, slot_self;
    float speed = 0.0f;        // |V| of self
    float thrust = 1.0f;
    float tolerance = 25.0f;   // Squadron_Formation_Error_Tolerance
    float error = 1.0e18f;     // E (team list +0x88)
};
float fighter_station_speed(const StationInput& in);
// §4.1 step 4 waypoint pass test (0x5c5060): W = wp[idx + 1], w = the unit bisector of the incoming and outgoing
// segment directions (2-D, §1.3); passed <=> w . (P - W) >= -0.001. [port] the last waypoint (no outgoing segment)
// uses the incoming direction; the path-kind-1 corner shift [L] is not modelled (the port's paths are never kind 1).
bool fighter_waypoint_passed(std::span<const SpaceWaypoint> path, uint32_t idx, F3 p);
// §4.2 steps 3.3-3.5, a squadron fighter in Idle: returns v (avoid_speed, or (r / rl) x 0.5 within 90 of D) and, for
// the leader inside 90, shifts `d` to the orbit aim (60 units out, 10 / r radians ahead around D).
float fighter_squad_idle(F3 p, F3 leader_p, F3& d, bool leader, float avoid_speed);

// ---- XML-derived parameters the UnitDesc does not carry ----------------------------------------------------------
// s78f t02 (FIGHTER_FLYING §4.10 A, §4.11 E): the response / leash ranges of a formation unit u's type, read by the
// leash test 0x4165f0: Guard_Chase_Range (+0x52c), Idle_Chase_Range (+0x530), Attack_Move_Response_Range (+0x538) and
// Autonomous_Move_Extension_Vs_Attacker (+0x211c). A squadron formation uses the squadron type's (FighterSquadDesc).
struct FighterChaseRanges {
    float guard = 0.0f, idle = 0.0f, attack_move = 0.0f, extension = 0.0f;
};
FighterChaseRanges fighter_chase_ranges_from_type(const objects::GameObjectTypes& types, uint32_t type_index);
struct FighterParams {
    float max_thrust = 1.0f;            // Max_Thrust (+0x384), raw ([port] default: the FoC fighter value 1.0)
    float min_follow_distance = 25.0f;  // Minimum_Follow_Distance (+0x534)
    float attack_range = 450.0f;        // max attack range 0x3749d0 (Targeting_Max_Attack_Distance +0x500) [M]
    FighterChaseRanges chase;           // s78f t02: the fighter type's ranges (a fighter without a squadron)
};
// Reads Max_Thrust, Minimum_Follow_Distance and Targeting_Max_Attack_Distance (a value <= 0 keeps the default) and the
// chase ranges (fighter_chase_ranges_from_type).
FighterParams fighter_params_from_type(const objects::GameObjectTypes& types, uint32_t type_index);
// Squadron_Formation_Error_Tolerance (+0x9b0) of a squadron type (<= 0: 25).
float squadron_tolerance_from_type(const objects::GameObjectTypes& types, uint32_t squadron_type_index);

// ---- §5 fighter cell grids (singleton 0x404950) --------------------------------------------------------------------
struct FighterCell {
    int32_t i = -1, j = -1;
    bool valid() const { return i >= 0 && j >= 0; }
    friend bool operator==(FighterCell a, FighterCell b) { return a.i == b.i && a.j == b.j; }
};
using FighterAlliedFn = std::function<bool(int32_t owner_a, int32_t owner_b)>;

class FighterCellGrid {
public:
    struct Entry {
        ObjectId id = -1;    // a squadron container, or a fighter without one
        int32_t owner = -1;  // owner at join time [port]
    };
    struct Lookup {
        FighterCell cell;     // invalid = nothing
        bool set_dest = false;  // flag 0: D (own and container) = centre(cell)
    };
    // Build (0x4faf40): nx = int(((hx + cx) - (cx - hx)) / size), ny the same in y. `by_position` = flag 1.
    void build(F3 centre, F3 half, float size, uint32_t capacity, uint32_t extra, bool by_position);
    void clear();
    bool built() const { return nx_ > 0 && ny_ > 0; }
    int32_t nx() const { return nx_; }
    int32_t ny() const { return ny_; }
    float size() const { return size_; }
    bool by_position() const { return flag_; }
    // centre (0x4fa6f0): x = ((cx - hx) + i size) + size / 2, + size / 2 more on odd rows; y = ((cy - hy) + j size) +
    // size / 2; z = the object type's Layer_Z_Adjust.
    F3 centre(FighterCell c, float layer_z) const;
    // join (0x4fa540): requires i, j >= 0 and j nx + i < nx ny; flag 0: fails when extra + capacity <= count or the
    // allied count >= capacity; duplicates refused; appended at the tail (join order); `stored` = c.
    bool join(ObjectId id, int32_t owner, FighterCell c, FighterCell& stored, const FighterAlliedFn& allied);
    // leave (0x4f9d50): a valid stored cell is unlinked and stored = (-1, -1).
    void leave(ObjectId id, FighterCell& stored);
    // The home cell of a reference point (lookup steps 2-5).
    FighterCell home(F3 ref) const;
    // lookup (0x4fb160). `ref` = the object position (flag 1) or pack D (flag 0); `stored` = the cell stored in the
    // (container's) pack. Ring search [M for the enumeration]: the 3 x 3 block, then the border of each larger square,
    // rows ascending then columns ascending, cells outside the grid skipped and not counted, at most 64 evaluated; a
    // cell is a candidate on flag 1 or when it has fewer allies than the capacity and no non-allied entry; key = the
    // squared 3-D distance ((dy^2 + dx^2) + dz^2) from its centre (z = layer_z) to ref, 0 for an occupied cell on flag
    // 1; after each completed ring the first strict minimum wins.
    Lookup lookup(ObjectId id, int32_t owner, F3 ref, float layer_z, FighterCell& stored, bool reg,
                  const FighterAlliedFn& allied);
    std::span<const Entry> entries(FighterCell c) const;
    void hash(StateHasher& h) const;
    // N1 stage 2 save / load (src/movement_save.cpp): the build parameters and every non-empty cell's entries in join
    // order (cells ascending). False (unchanged) on bad data.
    void write(ByteWriter& w) const;
    bool read(ByteReader& r);

private:
    int32_t index(FighterCell c) const;  // -1 when invalid / out of range
    std::vector<std::vector<Entry>> cells_;
    int32_t nx_ = 0, ny_ = 0;
    float cx_ = 0.0f, cy_ = 0.0f, cz_ = 0.0f, hx_ = 0.0f, hy_ = 0.0f, hz_ = 0.0f;
    float size_ = 0.0f;
    uint32_t capacity_ = 0, extra_ = 0;
    bool flag_ = false;
};

// G_D (dogfight: size 400, capacity 512, extra 512, flag 1, reference = position, pack +0x1c4) and G_P (parking: size
// 120, capacity 1, extra 0, flag 0, reference = pack D, pack +0x1cc).
struct FighterCellGrids {
    FighterCellGrid dogfight;
    FighterCellGrid parking;
    bool built = false;
    // From the playable box (centre = (min + max) x 0.5, half = (max - min) x 0.5, z 0 [port]).
    void build(V2 box_min, V2 box_max);
    void hash(StateHasher& h) const;
    void write(ByteWriter& w) const;  // N1 stage 2: built flag + both grids
    bool read(ByteReader& r);
};
inline constexpr float kDogfightCellSize = 400.0f;
inline constexpr float kParkingCellSize = 120.0f;

// ---- per-unit state and squadrons ----------------------------------------------------------------------------------
// A formation attack target (0x41a2e0). `id` = the target object: a squadron container registered with
// MovementSystem::set_fighter_squad (a team: dogfight), a movement unit (position / radius read from it), or any other
// object (`pos` / `radius` from the host). `point` = a bare point target (the legacy set_attack_target feed).
struct FighterTarget {
    ObjectId id = -1;
    bool point = false;
    int32_t hard_point = -1;  // hp; >= 0: `pos` is the hard point's position (0x396ce0)
    F3 pos;
    float radius = 0.0f;      // 0x397780; <= 0: the movement unit's radius (0 for other objects)
    bool dying = false;       // target +0x3a0 bit 2
    bool hidden = false;      // fogged for the attacker's player (0x35f470, force 1)
    bool set() const { return id >= 0 || point; }
};

enum class FighterPhase : uint8_t { None = 0, Approach = 1, Pass = 2, BreakOff = 3, Dogfight = 4, Chase = 5 };

struct FighterState {
    FighterBody b;             // P (mirrors the unit position), V, O, loop flag
    F3 dest;                   // pack +0x08 D (Idle station / parking point)
    FAttitude stored;          // pack +0x38 stored orientation (Dead_Stop)
    uint32_t idx = 0;          // path +0x60 (the current waypoint)
    FighterCell dog, park;     // pack +0x1c4 / +0x1cc (a fighter without a squadron; squadrons keep theirs)
    ObjectId lock = -1;        // pack +0x1e0 locked enemy fighter
    uint32_t lock_start = 0;   // pack +0x1d8 (mode frame)
    uint32_t lock_frames = 0;  // pack +0x1dc (expired <=> start + duration <= frame)
    int32_t hold = 0;          // pack +0x2b4 avoidance hold counter
    FighterTarget target;      // the formation attack target of a fighter without a squadron
    ObjectId squad = -1;       // obj +0x2b0 team container (-1 = none)
    FighterPhase phase = FighterPhase::None;  // diagnostics (not hashed)
};

// A squadron (team container C): members in team index order (C +0x110 +0x10; members[0] is the leader L), slot
// offsets (+0x78), the formation error E (+0x88) and spread S (+0x8c) and the container's pack (cells, D, target).
struct FighterSquad {
    ObjectId id = -1;
    std::vector<ObjectId> members;
    std::vector<F3> slots;      // one per member (zero when the host gives none)
    float tolerance = 25.0f;    // Squadron_Formation_Error_Tolerance of the squadron type
    float error = 1.0e18f;      // E; [port] estimate each frame (the writers are untraced)
    F3 spread;                  // S; [port] estimate each frame
    FighterCell dog, park;
    F3 dest;                    // the container's D
    FighterTarget target;
    // S7 t37 / s78f t02 (A1_READINGS §33.4.1 step 3, FIGHTER_FLYING §4.11 D 2): the formation's type T (the type of the
    // first team member, a fighter) has Space_Layer 0 or Disregard_Space_Layer_For_Guard_Attack_Move: the guard follow
    // (path copy, speed cap) applies, a single object block may re-path and step 5 pops instead of 0x4220d0.
    bool guard_follow = true;
    FighterChaseRanges chase;  // s78f t02: the squadron type's ranges (0x4165f0 reads u = the container's type)
};
struct FighterSquadDesc {
    ObjectId id = -1;
    std::vector<ObjectId> members;  // team index order; fighter units of this system
    std::vector<F3> slots;          // slot offsets, parallel to members (missing = zero)
    float tolerance = 25.0f;
    bool guard_follow = true;       // FighterSquad::guard_follow (from the first member's type)
    FighterChaseRanges chase;       // FighterSquad::chase (from the squadron type)
};

// s78f t02 (FIGHTER_FLYING §4.11, A1_READINGS §33.4 / §33.4.1, MOVEMENT §6.1 / §6.2; replaces S7 t37's one-block
// FighterGuard): the move-block stack of a fighter formation - a squadron container or a fighter without one ("key";
// the formation 0x290 bytes, blocks F +0xf8..+0x100). blocks.back() is the TOP (the current block, blocks[n-1]),
// blocks[0] the bottom. Every space move order of the key's fighters builds a new formation with the order's block as
// its only block (a group move makes a fresh coordinator, Coordinator::Move_To 0x4f8200 -> Set_Move 0x423c10); Stop and
// the key's removal drop it. Writers: Push 0x41fd30 (the host's auto attack block, MovementSystem::fighter_push), the
// waypoint append 0x420f70 (MoveOrder::waypoint), Pop 0x4105a0 (fighter_pop, arrival, leash / fog pops), the guard pop of
// 0x4192b0 step 3, the hidden-goal Set_Move of states 3 / 4, and the object listener 0x4f77e0 (the bottom block's object
// gone -> a position block at the last waypoint of the formation's path, flags kept).
// Block flags (MOVEMENT §6.1): kMoveFlag* plus 0x10 space idle movement (never issued with FoC data, §4.11 G 7: not
// modelled) and 0x40 signal 0x2a on pop (no port reader). Bytes kept: +0x50 push class (the port: `pushed`, an auto block
// 0x41cf70 [M: class name]), +0x5d `own_speed`, +0x5e `loop` (patrol). [port] Fighter blocks are kind 1 (position) or
// 3 (object): MoveToRay / kind 4 "stay" / the timed byte +0x61 / +0x62 are not produced for fighters.
inline constexpr uint32_t kFighterGuardInterval = 30;  // FPS: the 0x4192b0 / 0x410a60 timer step (+0x1d8)
inline constexpr uint8_t kFighterBlockPosition = 1;
inline constexpr uint8_t kFighterBlockObject = 3;
struct FighterBlock {
    uint8_t kind = kFighterBlockPosition;  // +0x24
    ObjectId object = -1;                   // +0x28 (kind 3)
    int32_t hard_point = -1;                // +0x30 [port: kept, not read by the formation]
    V2 point;                               // a position block's point (the order's destination, clamped)
    float facing = kNoFacing;               // +0x18 facing of a position block
    float threat = kThreatOff;              // the order's threat threshold
    uint8_t flags = 0;                      // +0x48 (kMoveFlag*: 0x01 attack-move, 0x02 guard, 0x04 player order, 0x08 queued)
    bool pushed = false;                    // +0x50 push class: an auto attack block of Push (0x41cf70) [port marker]
    bool own_speed = false;                 // +0x5d each member closes at its own speed (auto blocks)
    bool loop = false;                      // +0x5e patrol loop
};
// Formation states (+0x1d0) used here: 0 plan (0x41c1a0, then 1), 1 moving (0x423760: 0x4192b0 and the arrival), 3
// arrived (0x423070: the hidden-goal rule and 0x410a60), 5 after popping an object block (0x422e50 not read [M]: the port
// plans as state 0).
inline constexpr uint8_t kFormationPlan = 0, kFormationMoving = 1, kFormationArrived = 3, kFormationPopped = 5;
struct FighterFormation {
    ObjectId key = -1;                  // squadron container id, or the fighter's own id
    std::vector<FighterBlock> blocks;   // bottom first; back() = top (current)
    uint8_t state = kFormationMoving;   // +0x1d0
    V2 goal;                            // recorded goal (+0x210 / +0x214)
    uint32_t next = 0;                  // +0x1d8 next 0x4192b0 / 0x410a60 frame
    float speed = -1.0f;                // +0x21c formation speed (-1 = unset; the guard follow's cap)
    bool single_arrived = false;        // +0x240 arrived on its only block (states 3 / 4 with n = 1)
    bool follow = true;                 // T has Space_Layer 0 (FighterSquad::guard_follow; a solo fighter: its own)
    const FighterBlock* top() const { return blocks.empty() ? nullptr : &blocks.back(); }
};
// Host data for the formation update (all optional; serial calls from the movement tick, must be deterministic).
//   hidden(owner, object)  FOW_Object_Hidden(mode, owner, object, force 1) (0x35f470). Unset: never hidden.
//   attack_target(object)  the object's current attack target (0x397a40(object, 0)); a squadron container: its team
//                          target. Unset: none.
//   in_range(member, object)  member can hit the object from where it stands: not inside its min range (TARGETING slot
//                          46) and inside its max range (slot 43). Unset: 3-D distance <= FighterParams::attack_range.
//   position(object, p)    position of an object that is not a mover of this system (stations, structures). Unset or
//                          false: the object is unknown (its block counts as gone).
struct FighterFormationHooks {
    std::function<bool(int32_t owner, ObjectId object)> hidden;
    std::function<ObjectId(ObjectId object)> attack_target;
    std::function<bool(ObjectId member, ObjectId object)> in_range;
    std::function<bool(ObjectId object, F3& p)> position;
};

// Orders the fighter locomotor issues (the host forwards them to combat as attack orders, 0x38d730):
//   Provoke  - §4.5 provocation: squadron `who` is ordered to attack `target` (the attacker's container, or its leader
//              when solo);
//   Retarget - §4.3 retarget: container `who` is ordered to attack `target` (a dogfight-cell entry).
// The movement system applies them to its own targets at once (set_fighter_target semantics).
//   EnterIdle - S7 t25 (§4.10 D): fighter `who` ran enter_idle (0x5cb750) from one of its four retail call sites
//              (Directed_Combat NoTarget: the solo fighter / the leader's member loop; Moving path complete: the leader's
//              member loop / the solo fighter); one event per fighter, `target` unused. The host drops the squadron's
//              team target (0x5c9ca0 -> 0x406370(team, null), §4.7 step 4). The port's other enter_idle uses (Dead_Stop end,
//              hyperspace exits) raise none.
struct FighterEvent {
    enum Kind : uint8_t { Provoke = 1, Retarget = 2, EnterIdle = 3 };
    Kind kind = Provoke;
    ObjectId who = -1;
    ObjectId target = -1;
};

// Obstacle avoidance host hook (0x5c8b70 steps 2-4; SPACE_AVOIDANCE.md): Query_AABB on the space avoidance tree (GOM
// +0x558) with the box (centre, half-extents), candidates in the tree's result order (TACTICAL_COMBAT §1.6: reverse
// collection order; [port] ascending object id), each tested with the precise model test (0x38f900) against the
// segment a -> b. The first candidate whose test hits decides: when the shared hit-record byte 1 ("the segment starts
// inside a COLLISIONBOX") is clear it returns true with that candidate's position; when set it is skipped, and since
// byte 1 is shared across candidates and never reset, no later candidate can trigger in that call. Fighters are never
// members, so byte 1 is not a self-exclusion. space_avoid.hpp implements the rules (space_avoid()). Called serially on
// the sim thread; must be deterministic. Unset (no tree / no model data): no hit.
using FighterAvoidFn = std::function<bool(ObjectId self, F3 centre, F3 half, F3 a, F3 b, F3& hit_pos)>;

}  // namespace alamo::tactical::movement
