// S7 MovementSystem - the per-tick entry point the sim host calls (docs/re/MOVEMENT.md).
//
// Host contract (engine/apps SimHost):
//   setup   - land: set_grid(grid_from_map(map)) (+ stamp_map_obstacles), optional set_height_fn;
//             space: space() box / static obstacles (space_from_map). add_unit() per mover
//             (unit_desc_from_type + id / position / yaw), remove_unit() on death / flush.
//   orders  - move() when a MoveToPosition-like command executes (positions already dequantised;
//             flags = the move parameter flags, 0x08 queue = synced random start delay), stop().
//   tick    - tick(frame, synced rng, pf) once per logic frame, then commit(store) writes position
//             and the yaw as the store facing direction (cos yaw, sin yaw, 0) of every unit by
//             ascending object id.
//   CRC     - register_crc(registry, "movement") adds "movement" (units), "movement.grid" and
//             "movement.space".
//
// Tick order (land):
//   1. region map rebuild when the grid changed ("regions dirty");
//   2. path requests due this frame, planned IN PARALLEL through `pf` (per-request outputs,
//      per-range scratch), committed in ascending unit id;
//   3. collision checks, serial in ascending id (§5.2; ascending id replaces the retail registration
//      list and object-grid order): every unit is checked every FramesPerCollisionCheck frames
//      against the 160 x 160 square of candidates (SpatialTree query, ascending id); a qualifying
//      pair without a collision object gets one (CollisionLayer, collision.hpp: MovingVsMoving /
//      MovingVsStopped / StoppedVsStopped, creation frame recorded);
//   3b. collision driver (TACTICAL_GAPS §6.7): collisions that no longer apply are cancelled, the
//      rest sorted ascending by crowd - age ([port] stable by insertion order) and resolved in
//      that order - full resolution while the DynamicLandComplexityQuota (100 per 5 frames)
//      allows, else the fallback. Per-type resolutions (the pre-layer port behaviours):
//      MovingVsMoving = speed slaving / yield / side-step for each participant (untraced; same
//      full and fallback); MovingVsStopped = arrival-at-blocked-destination
//      (LandDestinationProximity), the dynamic local pathfinder (full only), else try again later
//      (FramesPerCollisionCheck) and a push after repeated waits; StoppedVsStopped = push both
//      apart. [port] Each unit acts on its own collision-check frames (one blocker resolution / one
//      push / its MovingVsMoving response per check, effects lasting to its next check), so the
//      per-unit cadence stays FramesPerCollisionCheck. The layer is hashed (land only);
//   4. walk locomotor step of every unit IN PARALLEL (each unit writes only itself);
//   5. serial commit in ascending id: terrain height, arrivals, blocked -> re-plan after 10
//      blocked frames (§1.6 re-path trigger), candidate tree refit.
// Tick order (space): 1. space paths planned in parallel against the occupancy snapshot, tracks
//   committed in ascending id; 2. Hermite playback in parallel; 3. serial commit (arrivals become
//   stationary tracks, turn-to-face when a facing was ordered).
// RNG (synced, sim thread): move() - queued orders (0x08): one start delay per unit in the order's unit
// order; plain group moves (OI tactical, §1.6 / §4.2 step 3): one stagger draw per formation in formation
// order, start = now + accumulated PathFrameDelay of the earlier formations' members + Get_Int(0,
// Max_Move_Frame_Delay) ([M] formula: the doc's "- 2 fps" is read as the re-plan gate stamp, so the
// first plan happens at that frame; [inferred] draw range). tick() (land, OI tactical, §3.5 step 6):
// formation jiggle, two Get_Float(-1, 1) per moving formation member with FormationRaggedness > 0 and
// MaxJiggleDistance > 0, ascending id, before the parallel walk: offset += draw x speed fraction x
// FormationRaggedness, clamped to +-MaxJiggleDistance per axis ([inferred]: k = 1, per axis).
// OI tactical (land): side-error steering for formation members (locomotor WalkExtras); crush (§5.5)
// after the commit - every moving SQUASH unit (ascending id) crushes hostile squashable movers whose
// box overlaps its crusher box (players ascending, then ids): the victim turns to face away and a
// CrushEvent is reported for the host to apply 1,000,000 damage of Squash_Damage_Type ([port]: the
// crusher box is the square of the unit's radius - no CRUSHER bones in the sim; the per-state
// permission 0x397f60 [L] is not modelled); a crusher's collision checks and local repaths ignore the
// units it can crush (§5.4 case 3 "push/crush attempt", [port] reading).
// REMAINING_GAPS §4.2: threat-aware planning. MoveOrder::threat_threshold (Lua Move_To(target, T); 1e18 = off) is kept
// per unit; with set_threat_fn() the space planner runs threat mode (ops 25-32, only for units whose raw Space_Layer
// is 0) and the land region search multiplies its edge costs (§4.2 cost rule). Without a threat fn the threat factor
// is 1 (space: compass moves still replace the normal operator set).
// REMAINING_GAPS §4.3-4.7: special locomotors by UnitDesc::locomotor (special_locomotors.hpp):
//   land  - Bike: the walk step (parallel) plus the 0x20-0x25 states and the lean; Jetpack / Flying / LandBomber: timed
//           paths from the kinematic planner with the land constants (XYExpansionDistanceLand, LandWaitOperatorSpeed-
//           Coefficient, MinObstacleCostLand) against air() (box only: flyers ignore ground passability [M]), followed by
//           the Hermite playback (the timed follower vf +0x1d8 [M]); their state machines run SERIALLY in ascending id
//           after the parallel walk (the jetpack landing test reads other units, the flying loiter draws). Air units
//           (jetpack while airborne / in flight states, flying, bomber) take no part in land collision or crush.
//           A land bomber reaching its path end is removed and reported by despawned() (the host kills it silently,
//           0x391310 flag 1).
//   space - Fighter (FIGHTER_FLYING.md §2-§5, fighter.hpp): reactive steering, SERIAL in ascending id after the parallel
//           playback of the other ships (the states read other fighters, lock them and edit the cell grids): pitch fold,
//           squadron grid bookkeeping, then Moving 0x1B (waypoint pass test + follow; the planned path gives only
//           waypoint positions), Idle 0x1C (solo: brake and level; squadron: leader orbit radius 60 around the parking
//           point, members on their slot), Directed_Combat 0x1E (target resolution / retarget, approach, attack run
//           pass / break-off, dogfight with cell travel, provocation and the 300-frame lock), Dead_Stop 0x1F
//           (fighter_dead_stop). Each fighter's occupancy track is its current position (stationary), re-set every
//           frame [port]. Squadrons (set_fighter_squad), targets (set_fighter_target, or the legacy point target of
//           set_attack_target), XML values (set_fighter_params) and the avoidance hook (set_fighter_avoid_fn) come from
//           the host. The fighter cell grids are built from the playable box at the first space tick with a fighter;
//           their membership is hashed into "<name>.space", the fighter state into "<name>" (both only once fighters
//           exist). No draws.
//   multi - S7 t06 / t14, Uses_Multiple_Locomotors (MOVEMENT §3.1.1, 0x3a9910; land only): UnitDesc::locomotor is the
//           ACTIVE locomotor. use_secondary_locomotor(id, true) (the JET_PACK ability's call, 0x3a38d0) or a move with
//           MoveOrder::switch_locomotor (the MoveToPosition 1-bit, §6.2; no FoC sender sets it) swaps a unit whose type
//           has a JETPACK_LOCOMOTOR Secondary_Locomotor_Name to it. Before the swap the unit's current order is
//           re-issued to it alone (goal / facing / threat kept, formation dropped, re-planned; [port] no stagger draw)
//           or, without an order, it holds its position; the jetpack starts in pack state 0 (None), so with no order it
//           reverts one service later. The swap back runs in the jetpack's state 0x16 (every service in it): in the
//           same service when 0x16 is reached from 0x13 with no current path or from the landing (0x14), one service
//           later from None or from a 0x13 frame that left position and yaw unchanged ([port] not while a re-plan is
//           pending, need_path). It keeps the order (goal, facing, formation; the walk re-plans a land path to a kept
//           goal [port]) and starts the primary stopped (Walk 0x16) on the terrain. Alternate_Max_Speed / _Rate_Of_Turn
//           apply only to the jetpack in its airborne kind (0 / 0x0F..0x13): the timed path request and the formation
//           speed (loco_max_speed / loco_turn_rate; UnitView::max_speed); the take-off turn 0x10, the landing 0x14 and
//           0x16 use Max_Speed / Max_Rate_Of_Turn. In the same kind the unit is in the Corvette tracking layer
//           (object_tracking_layer), so the flying clearance query (clearance_query) lifts flyers over it. The host
//           suspends TARGETING / WEAPON / SPECIAL_WEAPON while UnitView::secondary_locomotor is set (§3.1.1 step 6).
//           No draws.
// RNG: FLYING loiter re-issue - one synced range_float(-90, 90) per re-issue without an attack target (serial,
// ascending id, after the parallel walk). No other new draws.
// PROVISIONAL: MovingVsMoving retail resolution states are not modelled (MOVEMENT.md §8); corridor squeeze
// (corridor.hpp) is a stand-alone pass: the port's formation members path individually (no shared formation path).
// The jiggle draws skip special air units.
#pragma once
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "alamo/core/bytes.hpp"
#include "alamo/core/parallel.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/core/sim_rng.hpp"
#include "alamo/tactical/movement/collision.hpp"
#include "alamo/tactical/movement/constants.hpp"
#include "alamo/tactical/movement/fighter.hpp"
#include "alamo/tactical/movement/formation.hpp"
#include "alamo/tactical/movement/land_path.hpp"
#include "alamo/tactical/movement/locomotor.hpp"
#include "alamo/tactical/movement/passability.hpp"
#include "alamo/tactical/movement/space_path.hpp"
#include "alamo/tactical/movement/unit_params.hpp"
#include "alamo/tactical/spatial_tree.hpp"

namespace alamo::objects {
class ObjectStore;
}

namespace alamo::tactical::movement {

struct MovementConfig {
    MoveMode mode = MoveMode::Land;
    MovementConstants constants;
    MovementClassTable classes;
};

struct MoveOrder {
    std::vector<ObjectId> units;  // selection order
    V2 destination;
    float facing_deg = kNoFacing;
    uint8_t flags = 0;            // kMoveFlag* below (the MoveToPosition event's u8 flags, copied unchanged)
    float threat_threshold = kThreatOff;  // REMAINING_GAPS §4.2: threshold T (off = 1e18)
    // MOVEMENT §6.1 / §6.2: the MoveToPosition "switch locomotor" bit - multi-locomotor units swap to their secondary
    // locomotor before the move (MovementSystem::use_secondary_locomotor); others ignore it.
    bool switch_locomotor = false;
};
// Movement flag bits (P1_DOUBLE_CLICK_MOVE.md §3.3; corrects MOVEMENT.md §6.1: 0x01 is attack-move, 0x02 guard-move).
// UI moves send 0x04 | modifiers (0x84 for a double-click); AI / Lua moves send 0.
// 0x01 / 0x02 select the response chase range (Attack_Move_Response_Range / Guard_Chase_Range, else Idle_Chase_Range,
// 0x4160a0); no reader in this module (the chase range lives with the combat side). 0x04 has no modelled reader [L].
inline constexpr uint8_t kMoveFlagAttackMove = 0x01;
inline constexpr uint8_t kMoveFlagGuardMove = 0x02;
inline constexpr uint8_t kMoveFlagPlayerOrder = 0x04;
inline constexpr uint8_t kMoveFlagQueue = 0x08;
inline constexpr uint8_t kMoveFlagSkipValidation = 0x20;
// Double-click "fast" move (land only; space ignores it): formation recruitment rejects a candidate whose speed ratio
// to the seed exceeds DoubleClickMoveMaxSpeedRatio (0x43c1b0), and LargeHover formations are indexed fastest-first
// instead of front-to-back (0x439360).
inline constexpr uint8_t kMoveFlagDoubleClick = 0x80;

struct UnitView {
    ObjectId id = 0;
    V2 pos;
    float z = 0.0f;
    float yaw = 0.0f;
    float speed = 0.0f;
    float max_speed = 0.0f;  // S10-int: effective max speed (mode multiplier and speed factor applied)
    bool has_goal = false;   // moving or waiting to move
    bool arrived = false;    // reached its last ordered destination
    bool failed = false;     // the last order could not be pathed
    V2 goal;
    uint32_t replans = 0;
    bool space_fallback = false;  // the last space plan failed every try (straight line flown)
    uint32_t start_frame = 0;     // OI tactical: frame the current order starts moving (stagger / queue delay)
    V2 jiggle;                    // OI tactical: formation jiggle offset
    float formation_speed = 3.0e38f;     // land: slowest member's max speed of its formation (3e38 = alone)
    int32_t large_hover_index = -1;      // land: index (+0x22c) of its LargeHover formation in the last order, else -1
    // REMAINING_GAPS §4.3-4.7: special locomotors
    LocomotorKind locomotor = LocomotorKind::Default;  // the active locomotor
    bool secondary_locomotor = false;                  // S7 t06: the Uses_Multiple_Locomotors secondary is active
    LocoState loco_state = LocoState::None;  // special kinds only
    int32_t anim = kAnimNone;                // requested animation (special kinds)
    bool airborne = false;
    float roll = 0.0f;                       // bike lean / flying / fighter roll (degrees, wrap180) - attitude()
    float pitch = 0.0f;                      // flying / fighter pitch (degrees, wrap180; positive = nose down) - attitude()
    FighterPhase fighter_phase = FighterPhase::None;  // fighter: last combat steering phase (diagnostics)
    LocoState deploy_state = LocoState::None;  // S8 t11: Walk deploy state (None / 0x18 / 0x19 / 0x1A)
    // S8 t11 follow-up: the deploy clip the locomotor is timing (0x19 Deploy / 0x1A Undeploy; -1 none, also while 0x18 /
    // 0x1A wait for the turret). The host draws it (SimAnimation::deploy) when it is not the current animation.
    int32_t deploy_clip = -1;
    // S8 t24: the TURRET's current elevation / yaw (degrees, relative to the body; UnitDesc::turret units only).
    float turret_elevation = 0.0f, turret_yaw = 0.0f;
};

// OI tactical: one crush this tick (§5.5). The host applies kCrushDamage of the crusher type's Squash_Damage_Type
// from the crusher to the victim (combat Take_Damage) and removes the victim on death.
struct CrushEvent {
    ObjectId crusher = 0;
    ObjectId victim = 0;
};

struct MovementStats {
    uint32_t paths_planned = 0, path_failures = 0, replans = 0;
    uint32_t collision_checks = 0, dynamic_repaths = 0, quota_deferrals = 0, early_arrivals = 0;
    uint32_t blocked_steps = 0, space_fallbacks = 0;
    uint32_t collisions_created = 0, collisions_cancelled = 0, collisions_resolved = 0;  // S7 collision layer
    // S8 t09 (PR #19 review): jetpack 0x13 frames whose follower step left position and orientation unchanged (the
    // §3.1.1 drop to 0x16, possibly in the air). Expected 0 for planned flights (the planner makes no zero-length steps).
    uint32_t jetpack_still_frames = 0;
};

class MovementSystem {
public:
    explicit MovementSystem(MovementConfig cfg = {});

    MoveMode mode() const { return cfg_.mode; }
    const MovementConstants& constants() const { return cfg_.constants; }
    const MovementClassTable& classes() const { return cfg_.classes; }

    // ---- land world
    void set_grid(PassabilityGrid grid, const ParallelFor& pf = serial_for);  // pf: the region build (OPT3 t05)
    PassabilityGrid& grid() { return grid_; }  // stamping / unstamping; regions rebuild next tick
    const PassabilityGrid& grid() const { return grid_; }
    const RegionMap& regions() const { return regions_; }
    void rebuild_regions_if_dirty(const ParallelFor& pf = serial_for);
    using HeightFn = std::function<float(float x, float y)>;  // const and thread-safe
    void set_height_fn(HeightFn fn) { height_ = std::move(fn); }

    // ---- space world
    SpaceOccupancy& space() { return space_; }
    const SpaceOccupancy& space() const { return space_; }

    // ---- units
    bool add_unit(const UnitDesc& d);
    bool remove_unit(ObjectId id);
    bool has_unit(ObjectId id) const { return find(id) >= 0; }
    std::vector<ObjectId> unit_ids() const;
    UnitView view(ObjectId id) const;
    // Drawn attitude (read-only; FIGHTER_FLYING §1.1 O = (roll, pitch, yaw), degrees): space fighters - the fighter
    // orientation's roll and pitch (wrap180; may exceed +-90 between pitch folds); land flyers - the roll (lean) and
    // pitch of the flying locomotor; bikes - the lean as roll (pitch 0); every other unit 0 / 0. Roll is right-handed
    // about the forward axis (a left turn banks negative), positive pitch is nose down; both apply to the level frame
    // of the yaw (the store facing), pitch first. False (both 0) when the unit is unknown.
    bool attitude(ObjectId id, float& roll_deg, float& pitch_deg) const;
    std::span<const V2> land_path(ObjectId id) const;            // remaining + travelled points
    std::span<const SpaceWaypoint> space_path(ObjectId id) const;
    uint32_t mask_of(ObjectId id) const;

    // ---- orders
    void move(const MoveOrder& order, SimRng& rng, uint32_t frame);
    void stop(std::span<const ObjectId> ids);
    // OI tactical (crush, §5.5): owner changes (Change_Owner) and the relation test (player +0x370 table == 1 [M];
    // default: different non-negative players are hostile). The test is called from the serial crush pass only.
    bool set_owner(ObjectId id, int32_t owner);
    using HostileFn = std::function<bool(int32_t a, int32_t b)>;
    void set_hostility(HostileFn fn) { hostile_ = std::move(fn); }
    std::span<const CrushEvent> crush_events() const { return crush_; }  // this tick (land)
    // L1 systems: Teleport (0x3a8f90; Start_Cinematic_Space_Retreat line-up, Lua Teleport): the unit stops (its
    // order is dropped) and is placed at `pos` / `z` with yaw `yaw_deg`; land: the collision tree entry moves; space:
    // a stationary occupancy track from `frame`. No draws. False when the unit is unknown.
    bool teleport(ObjectId id, V2 pos, float z, float yaw_deg, uint32_t frame);
    // S7 t06 / t14 (MOVEMENT §3.1.1, 0x3a9910): swap a Uses_Multiple_Locomotors unit to its secondary locomotor (true)
    // or back to its primary (false); see "multi" in the header comment. Returns false when the unit is unknown, the mode
    // is space, or the type has no Uses_Multiple_Locomotors / no JETPACK Secondary_Locomotor_Name
    // (UnitDesc::has_secondary_locomotor); true otherwise (also when that locomotor is already active, which changes
    // nothing). The host calls it before the JET_PACK ability's move. No draws.
    bool use_secondary_locomotor(ObjectId id, bool secondary);

    // ---- S8 t11 / t12: Walk locomotor deploy states (MOVEMENT §3.2 "Deploy states", "A locomotor swap undeploys") ----
    // Only the Walk locomotor (land, LocomotorKind::Default) deploys. deploy(id, true) = Walk vf +0x118 Deploy(true)
    // (0x61d300): in 0x18 / 0x19 nothing; out of 0x1A with the turret not at rest (TurretRestFn) straight to 0x19 and
    // "ability finished" at once (ReverseBlocked); else the unit stops (its order is dropped) and enters 0x18 Deploying.
    // deploy(id, false): 0x18 / 0x19 -> 0x1A Undeploying ([port] the slot +0x35b blocker and the pending flag +0x2b3,
    // never set in vanilla, are not modelled). Per tick (serial, ascending id, after the walk): 0x18 / 0x1A start their
    // clip (anim 0x19 Deploy / 0x1A Undeploy) only once the turret is at rest (until then the turret recentres: the host's),
    // at Deployment_Anim_Rate; a clip's position after n frames is start + n x step, step = fps / 30 x rate (ABILITIES_
    // HARDPOINTS §4.13), and it has ended when position >= count - 2 - step (0x39a180). End of the Deploy clip: 0x19
    // Deployed; end of the Undeploy clip: back to the walk (state None, the walk stopped). Each clip end raises "ability
    // finished" (signal 0x29). Reversing: Deploy(true) out of 0x1A starts the Deploy clip at count - cur - 1 (min 0), and
    // 0x1A out of a running Deploy clip starts the Undeploy clip at count - cur - 1 - step (min 0), cur = the reversed
    // clip's position. No clip data (DeployClipFn unset / false): count 0, so a clip ends on the frame it starts.
    // While in 0x18 / 0x19 / 0x1A the walk does not move the unit [port: the Walk vf6 deploy states have no move branch;
    // an order given meanwhile waits for the undeploy]. The locomotor swap (use_secondary_locomotor, both directions)
    // leaves the deploy states (JetPack answers "not deployed"; the Walk rebuilt on landing starts in 0x16) without a
    // signal. No draws.
    enum class DeployResult : uint8_t {
        NoWalk,          // unknown unit, space, or the active locomotor is not Walk: nothing (retail 0x1fa90 / 0x13b7d0)
        Ignored,         // the request does not apply in the current state
        Deploying,       // -> 0x18
        Undeploying,     // -> 0x1A
        ReverseBlocked,  // 0x1A -> 0x19 at once (signal 0x29 raised)
    };
    DeployResult deploy(ObjectId id, bool on, uint32_t frame);
    // The deploy state: None (also every non-Walk unit), WalkDeploying, WalkDeployed or WalkUndeploying.
    LocoState deploy_state(ObjectId id) const;
    // One deploy-state change (requests, clip ends and swaps), in the order they happen. `finished` = the change raises
    // signal 0x29 "unit ability finished" (each clip end and the blocked reverse). take_deploy_events() hands them over
    // and clears the list (the host drains it after the movement tick and after the ability pass; not saved: it is
    // empty between frames when the host drains each frame).
    struct DeployEvent {
        ObjectId id = 0;
        LocoState from = LocoState::None, to = LocoState::None;
        bool finished = false;
    };
    std::vector<DeployEvent> take_deploy_events();
    // Clip data for the deploy clips (frame count and fps of anim 0x19 / 0x1A on the unit's model); const, deterministic.
    using DeployClipFn = std::function<bool(ObjectId id, int32_t anim, uint32_t& frame_count, float& fps)>;
    void set_deploy_clip_fn(DeployClipFn fn) { deploy_clip_ = std::move(fn); }
    // ---- S8 t24 (MOVEMENT §3.2 "Turret recentre around deploy"): the TURRET of a Deploys unit (UnitDesc::turret) ----
    // Per frame after the deploy pass (serial, ascending id; TURRET service 0x632270 + motion 0x631210): while aiming the
    // target angles are the aim point's; while not aiming and off the rest angle R the first frame holds (target :=
    // current, idle time := now), and once now - idle > Turret_Rotate_To_Rest_Delay_Sec the target becomes R; at R the
    // hold clears. R = Turret_Deployed_Rest_Angle in 0x18 / 0x19, else Turret_Rest_Angle. Motion: elevation linear,
    // yaw by the shortest wrapped difference, at most Turret_Rotate_Speed degrees per frame each, then the yaw is
    // clamped to +-(Deployed_)Turret_Rotate_Extent_Degrees. "At rest" (0x386170) = current == Turret_Rest_Angle (always
    // the undeployed one) within 0.0001. The Walk deploy states force the recentre (0x6319d0: not aiming, target := R,
    // no delay) every frame they wait.
    // Aim point of the turret this frame (the unit's current target; 0x632050 aims at it): true + world position, false =
    // not aiming. Unset: never aims.
    using TurretAimFn = std::function<bool(ObjectId id, float& x, float& y, float& z)>;
    void set_turret_aim_fn(TurretAimFn fn) { turret_aim_ = std::move(fn); }
    // Test override of the at-rest test (0x386170) for every unit; unset: the turret model above.
    using TurretRestFn = std::function<bool(ObjectId id)>;
    void set_turret_rest_fn(TurretRestFn fn) { turret_rest_ = std::move(fn); }
    // Teleport-free facing snap (0x3a8710; LOB_SOLVER §3.5 "Facing at apply"): the unit's yaw := yaw_deg at once (no
    // turn rate); position and order unchanged. False when the unit is unknown.
    bool snap_yaw(ObjectId id, float yaw_deg);

    // ---- REMAINING_GAPS §4.2-4.7
    // Threat at a point for an owner (TacticalPerceptionGrid point query 0x653ff0); const and thread-safe (planning
    // runs in parallel). Unset: threat factor 1.
    using OwnerThreatFn = std::function<float(int32_t owner, V2 p)>;
    void set_threat_fn(OwnerThreatFn fn) { threat_ = std::move(fn); }
    void set_anim_length_fn(AnimLengthFn fn) { anim_len_ = std::move(fn); }
    // Land air units: the planning box (set_box = the playable box) and the map floor (0x136650; default none).
    SpaceOccupancy& air() { return air_; }
    // S7 t15: the planner request (MOVEMENT §2.1 request record) plan_land would build for land air unit `id` at `frame`
    // (threat hook left null). False when the unit is unknown or not a land air unit.
    bool air_request(ObjectId id, uint32_t frame, SpaceRequest& out) const;
    void set_map_floor(float z) { map_floor_ = z; }
    // Attack target of a flying unit (strafe re-issue) or fighter (0x1E). False when the unit is unknown.
    bool set_attack_target(ObjectId id, bool has, V2 pos = {}, float z = 0.0f);
    // Flying 0x26 Circling around `centre` (radii +0x280 / +0x284) until the next move order.
    bool circle(ObjectId id, V2 centre, float z, float rmin, float rmax);
    // FIGHTER_FLYING §6.5: Start_Circling (vf +0x278) around a target OBJECT (a mover of this system, else
    // FlyingHooks::object_position), radii rmin / rmax > 0. Called by the CableAttackAbilityClass start. No draws.
    bool start_circling(ObjectId id, ObjectId target, float rmin, float rmax);
    // FIGHTER_FLYING §6: flying host hooks (special_locomotors.hpp FlyingHooks; all optional).
    void set_flying_hooks(FlyingHooks hooks) { fly_hooks_ = std::move(hooks); }
    // FIGHTER_FLYING §6.3.1 clearance query (0x63b640) over this system's movers, for FlyingHooks::clearance: returns
    // max(M, clearance_top of every Corvette-layer (4) mover whose entry the flyer's footprint from -> to hits).
    // `info` supplies the type data per object (the flyer's half-extents included). Layer: object_tracking_layer with
    // the mover's locomotor kind / state, moving = speed > 0 or a goal, Space_Layer = UnitDesc::space_layer_type. The
    // flyer is not excluded. Movers in ascending id (the result is a max: order-independent). Const, no draws.
    // [port] the tracking record of every mover exists (retail: made at its first path change); the entry is the
    // mover's current position (s = e, no ring slice: the tracked positions at the slice bounds are not modelled) and
    // facing; entry half-extents = the type's query half-extents [M].
    float clearance_query(ObjectId flyer, FlyVec3 from, FlyVec3 to, float min_height, const ClearanceInfoFn& info) const;
    std::span<const ObjectId> despawned() const { return despawned_; }  // this tick: land bombers at path end

    // ---- space fighters (FIGHTER_FLYING.md §2-§5; fighter.hpp)
    // Squadron (team container C): members in team index order (members[0] = the leader), slot offsets, tolerance.
    // Creates or replaces (cells, D and the target of an existing squadron are kept). Members must be fighter units;
    // others are skipped; a fighter leaves its previous squadron. False when no member qualifies.
    bool set_fighter_squad(const FighterSquadDesc& desc);
    bool remove_fighter_squad(ObjectId id);
    // The formation attack target of a squadron container or of a fighter without one (a member id addresses its
    // squadron). An unset target (id < 0, !point) clears it. Takes precedence over the legacy point target of
    // set_attack_target (which still drives a fighter whose squadron / own target is unset: the leader's for a squadron).
    bool set_fighter_target(ObjectId id, const FighterTarget& t);
    bool set_fighter_params(ObjectId id, const FighterParams& p);  // Max_Thrust, Minimum_Follow_Distance, attack range
    void set_fighter_avoid_fn(FighterAvoidFn fn) { favoid_ = std::move(fn); }
    // [port] entry to Dead_Stop 0x1F (the retail writer is untraced, "probably the Stop order"): stored orientation =
    // the current one, the path is dropped.
    bool fighter_dead_stop(ObjectId id);
    // S7 t28 (A1_READINGS §33.4, FIGHTER_FLYING §4.10 E): a guard of a moving object. fighter_copy_path gives squadron
    // `squad` (or a solo fighter) `from`'s space path (0x40dbc0) with every member's waypoint index at from's (S7 t35,
    // A1_READINGS §33.4.1 [port: from's next waypoint by playback time]); each member's goal = the path end; idle / moving members fly it (Moving 0x1B), members in combat keep their
    // state. False (nothing changed) when `from` is a fighter or follows no path. set_fighter_speed_cap sets
    // FighterSquad::speed_cap (kFighterNoSpeedCap clears it); false without that squadron.
    bool fighter_copy_path(ObjectId squad, ObjectId from, uint32_t frame);
    bool set_fighter_speed_cap(ObjectId squad, float cap);
    std::span<const FighterEvent> fighter_events() const { return fevents_; }  // this tick
    // Save / load access (the grids' membership and the per-unit / per-squadron fighter state are sim state).
    void build_fighter_grids();  // (re)build from the playable box; membership is cleared
    const FighterCellGrids& fighter_grids() const { return fgrids_; }
    FighterCellGrids& fighter_grids() { return fgrids_; }
    const FighterState* fighter_state(ObjectId id) const;
    FighterState* fighter_state(ObjectId id);
    std::span<const FighterSquad> fighter_squads() const { return squads_; }
    FighterSquad* fighter_squad(ObjectId id);

    // ---- tick
    void tick(uint32_t frame, SimRng& rng, const ParallelFor& pf = serial_for);
    void commit(objects::ObjectStore& store) const;
    const MovementStats& stats() const { return stats_; }
    const CollisionLayer& collisions() const { return coll_; }  // land: live collision objects, last driver order

    // ---- S10-int: effective-speed factor from the host (MOVEMENT.md 3.4: Mod_Multiplier Speed x
    // Engines_Disabled_Speed_Modifier x (1 + modifier list 5); 0 while immobilised). Scales max speed and
    // acceleration / deceleration (land) and the space path playback rate ([port]: a slowed ship lags its
    // reserved occupancy track). Default 1 = unchanged behaviour (bit-identical). Not part of hash_units (so
    // existing goldens keep their bytes): hosts that set factors register "<name>" via register_speed_crc.
    bool set_speed_factor(ObjectId id, float factor);
    float speed_factor(ObjectId id) const;
    // OI abilities (TACTICAL_GAPS 2.3.2 Berserker): locomotor speed override +0x2a0 with its flag +0x29c. While set
    // (`speed` >= 0) it replaces UnitDesc::max_speed as the base max speed; [port] the mode multiplier and the speed
    // factor still apply. `speed` < 0 clears it. Hashed by hash_speed only while set (format unchanged otherwise).
    bool set_speed_override(ObjectId id, float speed);
    float speed_override(ObjectId id) const;  // -1 = none
    void hash_speed(StateHasher& h) const;
    void register_speed_crc(CrcRegistry& registry, const std::string& name) const;

    // ---- CRC
    void hash_units(StateHasher& h) const;
    void register_crc(CrcRegistry& registry, const std::string& name) const;

    // ---- N1 stage 2 save / load (contract: alamo/core/save_file.hpp; src/movement_save.cpp) ----
    // The loader builds the battle the same way first: MovementSystem(same MovementConfig), set_grid(grid_from_map) +
    // the same map stamps, the same space / air boxes and static entries, every hook (set_height_fn, set_hostility,
    // set_threat_fn, set_anim_length_fn, set_flying_hooks, set_fighter_avoid_fn); then read() replaces ALL mutable
    // state, so no host call is needed after it (no relink: cross-references are object ids, hooks are not touched).
    // Units added by the rebuild are replaced by the saved ones (read() is not additive).
    // Layout: u32 version, u8 mode (must equal mode()), then sections (u32 length + bytes):
    //   grid    PassabilityGrid::write (stamps + version; the map-derived part is checked by digest, not restored)
    //   bool regions built; the RegionMap and the unit mask slots are REBUILT from the restored grid (a pure function
    //           of the effective values; every reader calls rebuild_regions_if_dirty() first, so the loaded map equals
    //           the one the saving run reads next)
    //   space / air   SpaceOccupancy::write (statics, tracks, box, window)
    //   tree    the land collision SpatialTree as its entry set (SpatialTree::write: rebuilt on load; its queries are
    //           shape-independent)
    //   u32 quota, f32 map floor, MovementStats, units (ascending id, every Unit field: UnitDesc incl. the type name,
    //   locomotor params, land motion + path, goal / order state, collision timers, space waypoints / playback / turn,
    //   speed factor / override / lag, jiggle, threat, special motion + params, the multi-locomotor primary kind and
    //   active flag (version 2), fighter state + params), the collision
    //   layer, this tick's crush / despawn / fighter events (the host may read them after a load; crush and despawn
    //   are hashed), the squadrons (ascending id) and the fighter cell grids.
    // Not saved: MovementConfig (definitions), the hooks, the query scratch. read() returns false and leaves the system
    // unchanged on a version / mode / map mismatch, an invalid value (enum, order, count) or truncated data.
    // HyperspaceDepartures / EngineControl (hyperspace.hpp) have their own write / read for the battle host.
    void write(ByteWriter& w) const;
    bool read(ByteReader& r);

private:
    // N1: every field below (and of UnitDesc, SpecialMotion, FighterState, ...) is saved field by field by
    // src/movement_save.cpp; a new mutable field must be added there (write_unit / read_unit) too.
    struct Unit {
        UnitDesc d;
        LocomotorParams lp;
        int32_t slot = -1;  // region-map mask slot
        LandMotion m;
        float z = 0.0f;
        bool has_goal = false, need_path = false, arrived = false, failed = false;
        V2 goal;
        float facing = kNoFacing;
        uint32_t start_frame = 0;
        float formation_speed = 3.0e38f;
        int32_t large_hover_index = -1;  // P1 double-click: formation +0x22c (not hashed; no modelled consumer)
        uint32_t blocked_frames = 0, replans = 0, waits = 0;
        int32_t check_countdown = 0;
        float speed_cap = 3.0e38f;
        uint32_t hold_until = 0;  // try again later: speed 0 until this frame
        V2 nudge;
        uint32_t nudge_frames = 0;
        // space
        std::vector<SpaceWaypoint> wps;
        uint32_t path_start = 0;
        bool following = false;
        bool turning = false;
        bool fallback = false;
        float turn_from = 0.0f, turn_to = 0.0f;
        uint32_t turn_start = 0, turn_frames = 0;
        float speed_factor = 1.0f;  // S10-int (set_speed_factor)
        float speed_override = -1.0f;  // OI abilities: locomotor +0x2a0 when >= 0 (flag +0x29c)
        float path_lag = 0.0f;      // S10-int: space playback time offset (frames) from speed factors != 1
        // OI tactical
        V2 jiggle;                  // formation jiggle offset (data pack +0x12c/+0x130)
        bool in_formation = false;  // member of a formation of > 1 unit (jiggle, side-error steering)
        // REMAINING_GAPS §4.2-4.7
        float threat = kThreatOff;  // order threshold T
        SpecialMotion sm;
        SpecialParams sp;
        // S7 t06: Uses_Multiple_Locomotors (d.locomotor is the active one)
        LocomotorKind primary_locomotor = LocomotorKind::Default;  // the spawn locomotor (behaviour slot 1)
        bool on_secondary = false;
        // S8 t11: Walk deploy state (None / 0x18 / 0x19 / 0x1A) and its clip: dclip = the clip playing (kAnimNone, 0x19
        // Deploy, 0x1A Undeploy), started at frame dclip_start at clip position dclip_pos0 (save version 5).
        LocoState deploy = LocoState::None;
        int32_t dclip = -1;
        uint32_t dclip_start = 0;
        float dclip_pos0 = 0.0f;
        // S8 t24: TURRET state (TD +0x48..+0x5c current / target angles x, elevation, yaw; +0x6c idle time (s); +0x70
        // hold; +0xf6 aiming). Starts at Turret_Rest_Angle [port: the spawn pose is not traced] (save version 7).
        float tcur[3] = {0.0f, 0.0f, 0.0f};
        float ttgt[3] = {0.0f, 0.0f, 0.0f};
        float tidle = 0.0f;
        bool thold = false, taim = false;
        // FIGHTER_FLYING.md §2-§5 (space fighters)
        FighterState fs;
        FighterParams fp;
    };
    bool is_air(const Unit& u) const;           // land Jetpack / Flying / LandBomber
    void fill_air_request(const Unit& u, uint32_t frame, SpaceRequest& q) const;  // plan_land's request (no threat)
    static void special_idle(Unit& u);          // §4.3-4.7: the idle state of the active special locomotor
    bool swap_locomotor(Unit& u, bool secondary);  // S7 t06 / t14 (0x3a9910)
    // S8 t11 (deploy_system.cpp): clip data, clip position, clip start, the per-tick deploy pass, the state change.
    struct DeployClip {
        float count = 0.0f;  // frame count (0: no clip data)
        float step = 1.0f;   // clip frames per sim frame (fps / 30 x Deployment_Anim_Rate)
    };
    DeployClip deploy_clip(const Unit& u, int32_t anim) const;
    float deploy_clip_pos(const Unit& u, uint32_t frame) const;
    bool turret_at_rest(const Unit& u) const;  // 0x386170 (or the test override)
    // S8 t24 (deploy_system.cpp): the rest angle R, the forced recentre 0x6319d0, the per-frame TURRET pass.
    const float* turret_rest_angle(const Unit& u) const;
    static void turret_recentre(Unit& u, const float* rest);
    void tick_turrets(uint32_t frame);
    void deploy_start_clip(Unit& u, int32_t anim, float pos0, uint32_t frame);
    void deploy_begin_clip(Unit& u, int32_t anim, uint32_t frame);  // start `anim`, reversing the current clip
    void deploy_set(Unit& u, LocoState to, bool finished);
    void tick_deploy(uint32_t frame);
    void hash_deploy(StateHasher& h) const;
    void hash_turrets(StateHasher& h) const;  // S8 t24
    // S7 t14 (§3.1.1): JetPack vf74 / vf75 - Alternate_Max_Speed / _Rate_Of_Turn (mode multiplier, speed factor /
    // override as for Max_Speed) while the active jetpack is in its airborne kind, else lp.max_speed / lp.turn_rate.
    bool jet_alternates(const Unit& u) const;
    float loco_max_speed(const Unit& u) const;
    float loco_turn_rate(const Unit& u) const;
    bool ground_collider(const Unit& u) const;  // takes part in land collision / crush
    void tick_special(uint32_t frame, SimRng& rng);
    void step_jetpack(Unit& u, uint32_t frame);
    void step_flying(Unit& u, uint32_t frame, SimRng& rng);
    void step_bomber(Unit& u);
    void bike_frame(Unit& u, float yaw0, bool walked) const;
    SpacePose air_follow(Unit& u, uint32_t frame) const;
    float air_altitude(const Unit& u, uint32_t frame) const;
    float terrain(V2 p) const { return height_ ? height_(p.x, p.y) : 0.0f; }
    void anim_play(Unit& u, int32_t anim, bool loop) const;
    bool land_spot_free(const Unit& u, V2 p) const;
    void on_air_path(Unit& u, uint32_t frame);
    ThreatFn owner_threat(const Unit& u) const;
    void hash_special(StateHasher& h) const;
    // space fighters (movement_system.cpp)
    bool is_fighter(const Unit& u) const { return u.d.locomotor == LocomotorKind::Fighter && cfg_.mode == MoveMode::Space; }
    Unit* unit_ptr(ObjectId id);
    FighterSquad* squad_ptr(ObjectId id);
    FighterSquad* fsquad(const Unit& u) { return u.fs.squad >= 0 ? squad_ptr(u.fs.squad) : nullptr; }
    Unit& fleader(Unit& u);
    FighterKin fkin(const Unit& u) const;
    static F3 fslot(const FighterSquad* c, ObjectId id);
    bool fallied(int32_t a, int32_t b) const { return !hostile(a, b); }
    FighterTarget* ftarget_slot(Unit& u);
    bool ftarget_get(Unit& u, FighterTarget& out);
    struct FTgt {
        ObjectId id = -1;
        bool team = false;
        F3 p;
        float radius = 0.0f;
        bool dying = false, hidden = false;
    };
    FTgt fresolve(const FighterTarget& t);
    FighterCell fcell_of_target(const FTgt& t);
    bool fpath_open(Unit& u);  // F (the container = the leader, else own) has a current path that is not complete
    void freset_path(Unit& u);
    void fleave(FighterCellGrid& g, Unit& u, bool dogfight);
    bool fjoin(FighterCellGrid& g, Unit& u, FighterCell c, bool dogfight);
    void tick_fighters(uint32_t frame);
    void fighter_squad_estimates();
    void fighter_step(Unit& u, uint32_t frame);
    void f_moving(Unit& u);
    void f_idle(Unit& u);
    void f_combat(Unit& u, uint32_t frame);
    void f_attack_run(Unit& u, const FTgt& t);
    void f_dogfight(Unit& u, const FTgt& t, uint32_t frame);
    void f_dead_stop(Unit& u);
    void f_enter_idle(Unit& u);
    void f_follow(Unit& u, float v_req, float z_goal);
    float f_station_speed(Unit& u, float v, float lo, float hi);
    bool f_avoid(Unit& u, F3& aim, float& tb, float& thb, float& lb);
    void f_steer(Unit& u, F3 aim, float v, float& tb, float& thb, float& lb);
    void f_chase(Unit& u, const Unit& x, RelAngles rel, float& tb, float& thb, float& lb);
    static void f_commit(Unit& u) { u.fs.b.p = f3_add(u.fs.b.p, u.fs.b.v); }
    void fighter_removed(ObjectId id);
    void fighter_order(Unit& u);
    void fighter_stop(Unit& u);
    void hash_fighters(StateHasher& h) const;
    // N1 stage 2 (movement_save.cpp)
    static void write_unit(ByteWriter& w, const Unit& u);
    static bool read_unit(ByteReader& r, Unit& u);

    int32_t find(ObjectId id) const;
    void configure(Unit& u) const;
    void tick_land(uint32_t frame, SimRng& rng, const ParallelFor& pf);
    void crush_pass(const std::vector<WalkResult>& res);
    bool hostile(int32_t a, int32_t b) const { return hostile_ ? hostile_(a, b) : (a >= 0 && b >= 0 && a != b); }
    // §5.5 victim rule: hostile, and (squashable by a supercrusher and the crusher is one) or squashable.
    bool crushes(const Unit& c, const Unit& v) const {
        return c.d.crusher && hostile(c.d.owner, v.d.owner) &&
               ((v.d.squashable_by_supercrusher && c.d.supercrusher) || v.d.squashable);
    }
    void tick_space(uint32_t frame, const ParallelFor& pf);
    void plan_land(uint32_t frame, const ParallelFor& pf);
    // S7 collision layer (TACTICAL_GAPS §6.7)
    void detect_collisions(uint32_t frame);
    void drive_collisions(uint32_t frame);
    void land_candidates(size_t ai, std::vector<uint32_t>& out) const;
    bool checker_moving(const Unit& u, uint32_t frame) const;
    static bool other_moving(const Unit& u);
    bool mvm_contact(const Unit& a, const Unit& b) const;
    float mvs_contact(const Unit& a, const Unit& b) const;
    bool collision_applies(const CollisionObject& c, uint32_t frame) const;
    void resolve_collision(CollisionObject& c, bool full, uint32_t frame, std::vector<uint8_t>& acted);
    void mvm_response(Unit& x, const Unit& y, uint32_t frame);
    bool resolve_blocker(Unit& a, uint32_t blocker, float contact, uint32_t frame, bool full);
    void update_tree(const Unit& u);
    V2 pick_land_slot(const Unit& u, V2 slot, std::vector<uint64_t>& taken) const;
    void move_land(const MoveOrder& order, const std::vector<uint32_t>& idx, uint32_t frame,
                   const std::vector<uint32_t>& delays, SimRng& rng);
    void move_space(const MoveOrder& order, const std::vector<uint32_t>& idx, uint32_t frame,
                    const std::vector<uint32_t>& delays);
    void park_space(Unit& u, uint32_t frame);

    MovementConfig cfg_;
    PassabilityGrid grid_;
    RegionMap regions_;
    bool regions_built_ = false;
    HeightFn height_;
    SpaceOccupancy space_;
    std::vector<Unit> units_;  // ascending id
    SpatialTree tree_;
    mutable QueryContext qctx_;
    uint32_t quota_used_ = 0;
    CollisionLayer coll_;  // S7: persistent land collision objects (hashed)
    MovementStats stats_;
    HostileFn hostile_;              // OI tactical
    std::vector<CrushEvent> crush_;  // OI tactical: this tick
    OwnerThreatFn threat_;           // REMAINING_GAPS §4.2
    AnimLengthFn anim_len_;          // §4.3-4.7
    SpaceOccupancy air_;
    float map_floor_ = -3.0e38f;
    std::vector<ObjectId> despawned_;
    // space fighters
    std::vector<FighterSquad> squads_;  // ascending container id
    FighterCellGrids fgrids_;
    FighterAvoidFn favoid_;
    std::vector<FighterEvent> fevents_;  // this tick
    FlyingHooks fly_hooks_;  // FIGHTER_FLYING §6
    // S8 t11
    DeployClipFn deploy_clip_;
    TurretRestFn turret_rest_;
    TurretAimFn turret_aim_;
    std::vector<DeployEvent> devents_;  // since the last take_deploy_events()
};

}  // namespace alamo::tactical::movement
