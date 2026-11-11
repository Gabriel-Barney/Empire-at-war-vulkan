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
//   CRC     - the battle host (engine/apps/sim campaign_battles.cpp) hashes hash_units into "battle.movement" and
//             hash_speed into "battle.abilities" (TacticalBattle::hash_abilities); the grid, the space occupancy and
//             the fighter cell grids are not in its CRC. register_crc / register_speed_crc register the same hashes
//             (register_crc adds "<name>", "<name>.grid" and "<name>.space") for the tests and stand-alone runs.
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
//      (s78n t20, MOVEMENT §5.2: "Moving" = not suspended and (speed > 0, or a path record on its stack - a planned order
//      path or a collision-owned record on top of it - or turning in place); no order test, so a unit on a pushed path,
//      a StoppedVsStopped path or a detour is a checker and the moving side);
//   3b. collision driver (TACTICAL_GAPS §6.7; s78n t18: MOVEMENT §5.4.1 / §5.4.2; s78n t20: §5.4.1 "Per-frame driver"):
//      records whose participants are gone or left the layer are dropped; the records that do not take the quota pass
//      (a decided MovingVsMoving, every StoppedVsStopped) are serviced in creation order; the others are sorted ascending
//      by crowd - age ([port] stable by insertion order) and serviced in that order, a record that is not decided yet
//      (state None) only while the DynamicLandComplexityQuota (100 per 5 frames) allows. MovingVsStopped = the decision
//      0x684f90 (Repulsion - also for a short-range unit closing on its target -, the early-arrival cut 0x685e10 with the
//      spare point and the attack-range clause, Path Around 0x5c80e0, a push of the blocker onto a walked path - never
//      of a unit in a hero clash -, the crusher driving on, Try Again Later with a re-decide every 30 frames; no instant
//      displacement) and its per-state service; a record ends by its state only. Path Around (MOVEMENT §1.5.1,
//      path_around.hpp) = a visibility-graph detour from the unit to a rejoin point on its own path - the obstacles are
//      the layer objects' swept rectangles chosen by the relation code, the search runs twice (crossing an obstacle at x
//      DynamicObstacleOverlapPenalty only when the first pass finds no way) - pushed as a record on top of the unit's
//      path stack; the ordered path stays underneath and resumes at the first waypoint line not yet crossed.
//      StoppedVsStopped (§5.4.3) = one of the two walks a path to the first free spot of the ring search around itself
//      (who: 0x687010; Resolving while it holds that path, Try Again Later every 30 frames). MovingVsMoving (s78n t23,
//      §5.4.2 / §5.4.6, every pair) = the chooser 0x67f7b0 (same formation, forced path, Repulse / Speed Slave Only for
//      two Infantry / Bike movers; else overtake -> Pass or Moving With Speed Only, Giant, then by the relation code:
//      Path Around with hold lines, the crusher pair, Moving With Speed Only) and its states, with the synced retry
//      draws of Try Again Later / a failed pass. Each record answers its participants a speed and an "at once" flag,
//      folded per unit before the walk (finished records leave the fold).
//      The layer is hashed (land only);
//   4. walk locomotor step of every unit IN PARALLEL (each unit writes only itself);
//   5. serial commit in ascending id: terrain height, arrivals, candidate tree refit; a failed (blocked) step makes a
//      TerrainCollision record (s78n t24, MOVEMENT §5.4.7: speed 0, decided by the next tick's driver - Path Around
//      kind 3 with the turn in place onto its first leg, else the unit is stopped; no blocked-frame count, no re-plan).
// Tick order (space): 1. space paths planned in parallel against the occupancy snapshot, tracks
//   committed in ascending id; 2. Hermite playback in parallel; 3. serial commit (arrivals become
//   stationary tracks, turn-to-face when a facing was ordered).
// RNG (synced, sim thread): move() - queued orders (0x08): one start delay per unit in the order's unit
// order (no new formation: no ctor draw [M]). cu8j t01 (MOVEMENT §4.6, correcting cu8i): every other move creates its
// formations at the next coordinator update (coordinator_update, retail common tick step 10), not at the order: land
// (§1.6 / §4.2) - one r' = int(0, PathFrameDelay_seed) per staggered seed, then one FormationClass ctor draw int(1, FPS)
// per formation, both in formation order; space (§4.6.4) - one ctor draw per formation of the space composition. The
// ctor's r only sets the next-check timer (+0x1d8 := now + FPS + r: the fighter formations' first check), never a start
// frame. cu8j t02 (§4.7): adopt / stop_and_hold draw one ctor int(1, FPS) at once; a swap to the secondary locomotor
// composes at the next update. The Load_Map-tail pass and the other adopt sites are the host's. tick() (land, OI tactical, §3.5 step 6):
// formation jiggle, two Get_Float(-1, 1) per moving formation member with FormationRaggedness > 0 and
// MaxJiggleDistance > 0, ascending id, before the parallel walk: offset += draw x speed fraction x
// FormationRaggedness, clamped to +-MaxJiggleDistance per axis ([inferred]: k = 1, per axis).
// OI tactical (land): side-error steering for formation members (locomotor WalkExtras); crush (§5.5)
// after the commit - every moving SQUASH unit (ascending id) crushes hostile squashable movers whose
// box overlaps its crusher box (players ascending, then ids): the victim turns to face away and a
// CrushEvent is reported for the host to apply 1,000,000 damage of Squash_Damage_Type ([port]: the
// crusher box is the square of the unit's radius - no CRUSHER bones in the sim; the per-state
// permission 0x397f60 [L] is not modelled); the Path Around obstacle set leaves out the units the mover can crush
// (MOVEMENT §1.5.1 item 3); s78n t20: a crusher takes a MovingVsStopped record with a standing unit it crushes (§5.4.5).
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
//           frame [port]. Squadrons (set_fighter_squad), targets (set_fighter_target; set_attack_target does not drive
//           fighters), XML values (set_fighter_params) and the avoidance hook (set_fighter_avoid_fn) come from
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
// ascending id, after the parallel walk). s78n t23: the MovingVsMoving retry draws - one synced range_int(0, 15) when a
// record goes to Try Again Later (0x680c00) and each time the retry frame of state 10 / 12 passes, in the collision
// driver's service order (serial). No other new draws.
// Orders (s78n t20, MOVEMENT §4.3.1): a land move order without flag 0x20 and without an object goal runs the destination
// check 0x439360 once, at the order - each formation's goal is moved to the first centre whose rectangle is passable, clear
// of the order's other formations and not blocked by units that stand there or are heading there (more than 15 % of its
// area); the order is never refused. A single unit ordered onto its own footprint does not move (the plan's shortcut).
// PROVISIONAL: corridor squeeze
// (corridor.hpp) is a stand-alone pass: the port's formation members path individually (no shared formation path).
// The jiggle draws skip special air units.
#pragma once
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <utility>
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
#include "alamo/tactical/movement/path_around.hpp"
#include "alamo/tactical/movement/space_avoid.hpp"
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
    // S7 t37 / s78f t02 (A1_READINGS §33.4.1, FIGHTER_FLYING §4.11): an object goal (block kind 3); -1 = a position
    // order (kind 1). Space only: each fighter formation the order reaches (a squadron, or a fighter without one) gets a
    // new block stack holding the order's block (FighterFormation), run by the tick. Others ignore it.
    ObjectId goal_object = -1;
    // s78f t02 (MOVEMENT §6.2 waypoint-queue move 0x2a8240 -> coordinator vf +0x30 -> 0x420f70): space fighters whose
    // formation exists append the order's block to it (inserted at the bottom with flag 0x04; FIGHTER_FLYING §4.11 B 1)
    // instead of moving now; the order's other units (and fighters without a formation) move as a plain order.
    bool waypoint = false;
    // s78k t01 (MOVEMENT §6.1 block +0x44 / +0x5c, §3.5.1): the final facing (yaw, degrees) the path's last waypoint
    // carries; kNoFacing = none (+0x5c clear). Land Walk units turn to it on arrival when more than 10 degrees off (rate
    // limited, see tick_land). Not `facing_deg` (the +0x18 formation facing), which never makes a final turn.
    float final_facing_deg = kNoFacing;
    // s78n t20 (MOVEMENT §4.3.1 item 2): the order has an object goal (block kind 3), which retail sends to the attack-arc
    // slot test 0x438c10 instead of the destination check 0x439360. [port] the arc test is not modelled: the host sets
    // this for its attack approaches (and for the garrison approach until its retail form is ported), and the land
    // destination check and the single-member shortcut are skipped.
    bool skip_destination_check = false;
};
// Movement flag bits (P1_DOUBLE_CLICK_MOVE.md §3.3; corrects MOVEMENT.md §6.1: 0x01 is attack-move, 0x02 guard-move).
// UI moves send 0x04 | modifiers (0x84 for a double-click); AI / Lua moves send 0.
// 0x01 / 0x02 select the response chase range (Attack_Move_Response_Range / Guard_Chase_Range, else Idle_Chase_Range,
// 0x4160a0 / 0x4165f0); in this module the fighter formations read them (FIGHTER_FLYING §4.11). 0x04 player order: the
// fighter formations' arrival pop and step 5 read it (the waypoint append adds it).
inline constexpr int32_t kFormationFps = 30;  // FPS of the FormationClass ctor draw int(1, FPS) (cu8i t02)
// cu8j t01: UnitView::start_frame of a unit whose order waits for its formation (the next coordinator_update).
inline constexpr uint32_t kStartPending = 0xFFFFFFFFu;
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
    bool pushed = false;     // a1k t03: walking a collision-owned path on top of its order path (A1_READINGS §40.3)
    // s78n t20: the top record's kind - 0 none, 1 a push (0x6859a0), 2 a Path Around detour, 3 a StoppedVsStopped move;
    // push_flee: it is a crush-flee path (kind 7); form_id: the unit's formation of its last land order (0 none)
    uint8_t push_kind = 0;
    bool push_flee = false;
    uint32_t form_id = 0;
    V2 goal;
    bool space_fallback = false;  // the last space plan failed every try (straight line flown)
    uint32_t start_frame = 0;     // OI tactical: frame the current order starts moving (stagger / queue delay)
    V2 jiggle;                    // OI tactical: formation jiggle offset
    float formation_speed = 3.0e38f;     // land: slowest member's max speed of its formation (3e38 = alone)
    // s78k t03: land team locomotor cap of the last tick (set_team; 3e38 = none): the team's minimum raw Max_Speed x the
    // mode multiplier x the container's speed factor
    float team_speed = 3.0e38f;
    // s78l t02 (MOVEMENT §3.5.1): the Walk arrival turn runs (states 0x0F / 0x10 / 0x11; the path is still on the
    // locomotor's stack in retail, so the IDLE moving gate counts the unit as moving).
    bool arrival_turn = false;
    // s78l t02: the arrival turn ended during the last tick - it popped (0x10 -> 0x11 -> pop, possibly in one frame), or a
    // stop / new order / teleport / locomotor swap dropped a running turn since the tick before (reported on the next
    // tick). False otherwise. The host treats arrival_turn || arrival_turn_ended as "the turn owns the clip".
    bool arrival_turn_ended = false;
    // s78n t20 (FIGHTER_FLYING §4.11 E "stopping"): the land Walk state (LandMotion::state; WalkEndMove = braking for the end)
    LocoState walk_state = LocoState::None;
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
    // s78n t26.1 (MOVEMENT §3.2.1): walking while deployed (pack +0x2b3). deploy_state stays WalkDeployed meanwhile (every
    // 0x39a390 reader sees it deployed); the walk runs with the Deployed_Walk_Speed cap and the host plays clip 115.
    bool walk_deployed = false;
    // S8 t24: the TURRET's current elevation / yaw (degrees, relative to the body; UnitDesc::turret units only).
    float turret_elevation = 0.0f, turret_yaw = 0.0f;
    // s78f t03 (TACTICAL_COMBAT §3.1.1 "Targeting states"): the TURRET's aim toward the host's aim point - 1 the current
    // angles equal the target angles (0x386170), 0 still slewing, -2 the aim yaw is beyond the (deployed) rotate extent
    // (the can-aim test 0x631cc0 fails; s78f t08), -1 no turret model or not aiming. Derived from the turret state.
    int8_t turret_aim = -1;
    // s78n t18 review (FIGHTER_FLYING §7.7 items 5 - 6; squad containers only): team_coast - a new order arrived while
    // it moved and it still brakes straight on before that order's path starts; team_final_turn - it turns in place to
    // the order's final facing before it arrives.
    bool team_coast = false, team_final_turn = false;
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
    // a1k t03 (A1_READINGS §40): pushes started (a spot found and a path to it) and push attempts with no spot / path.
    uint32_t pushes = 0, push_failures = 0;
    // s78n t24 (MOVEMENT §5.4.7): TerrainCollision records made (failed walk steps), detours pushed by their decisions, and
    // units stopped - their own position impassable (decision step 4, or a waiting record's service), Path Around with
    // no base path (no order / formation record on the stack, or past its last waypoint), Path Around failed otherwise.
    // `replans` above stays 0 on land (the blocked-frames re-plan is gone).
    uint32_t terrain_records = 0, terrain_detours = 0, terrain_stops_cell = 0, terrain_stops_nobase = 0, terrain_stops_failed = 0;
    // s78n t26.3 (MOVEMENT §3.5.2 D): the corner-speed pass's corners written with fraction 0 / with a fraction in (0, 1),
    // and its probe step-downs (v /= 1.5).
    uint32_t corner_zero = 0, corner_slowed = 0, corner_stepdowns = 0;
    // s78n t26.4 (MOVEMENT §3.5.2 C): Walk move steps - End Move coast steps (C.4), 0x15 restarts away from the end, waypoints
    // skipped by tau = 0 (C.9) and by a degenerate segment (C.8).
    uint32_t walk_coasts = 0, walk_restarts = 0, walk_tau_skips = 0, walk_degen_skips = 0;
};

// s78n t14 (FIGHTER_FLYING §7.2 Dartboard step 3, 0x241820 / 0x241830): the radical inverse of `index` in `base` (f = 0,
// b_k = (float)b; while i != 0 { f += (float)(i % b) / b_k; i /= b; b_k *= (float)b }). 0 for index 0.
float team_radical_inverse(uint32_t index, uint32_t base);

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
    // s78n t20 review follow-ups (MOVEMENT §4.3.2 item 6, FIGHTER_FLYING §4.11 E): the attack plan's "every member can
    // fire" gate and the land stop clear the path stack as stop() does, but the speed is not zeroed - a land Walk unit or
    // squad container brakes through End Move (brake_to_stop; s78n t26.4: a Walk unit keeps its speed and state and its walk
    // coasts in End Move, MOVEMENT §3.5.2 C.1 / C.4). [port] Other locomotors (and space) stop as stop() does.
    void halt(std::span<const ObjectId> ids, uint32_t frame);
    // cu8i t02 (CU8G_T02_ENV_PICK_RE §7 / §8 item 7): FormationClass ctor 0x40d0d0, one synced int(1, FPS), for a
    // formation the host creates outside move() (the Load_Map-tail GOM post-load pass, TacticalBattle); and whether a unit
    // already has a coordinator ([port]: an order or a formation; unknown ids have none).
    static int32_t formation_ctor_draw(SimRng& rng);
    bool has_coordinator(ObjectId id) const;  // cu8j: also while adopted or waiting for its composition
    // ---- cu8j t01 / t02 (MOVEMENT §4.6 / §4.7): the movement coordinator update and the adopt ----
    // A non-queued move() (and fighter_new_order, and a swap to the secondary locomotor) creates its formations at the
    // next coordinator update (MovementCoordinatorSystem update 0x426eb0, common tick step 10), not at the order: the
    // ordered units wait (start_frame kStartPending) until then. coordinator_update(frame, global_frame, rng) composes the
    // waiting orders in creation order:
    //   land  - the split made at the order [port: positions of the order frame]: for each formation whose seed has
    //           PathFrameDelay > 0 one r' = int(0, PathFrameDelay), start = frame + acc + r' + 1, acc += PathFrameDelay
    //           (0x43c1b0, acc per order); other formations start at frame, or at frame + 61 - global_frame when
    //           global_frame <= 60 (the plan gate 2 FPS < now - F +0x1dc with F +0x1dc = 0 [M]); then one ctor draw
    //           int(1, FPS) per formation (0x43e5b0);
    //   space - the composition 0x5d58f0 (§4.6.4): candidates = the order's units still waiting (a squadron's fighters
    //           as its container); seed = nearest the goal (strict <); one ctor draw per formation; a nonzero role
    //           (Stationary_Space_Layer when not moving and set, else Space_Layer) is alone, a role-0 seed recruits by
    //           the box / speed-ratio rule; each fighter formation of it gets its first check at frame + FPS + r; a seed
    //           with max speed 0 stops the pass, the rest wait for the next update. Start = frame + the port's per-slot
    //           offset (n x SpacePathfindFrameDelayDelta).
    // Units that left the order (another order, stop, teleport, removal) drop out; a composition left empty draws
    // nothing. By default tick() runs it first; a host that calls it at its own step-10 point sets
    // set_deferred_composition(true).
    void set_deferred_composition(bool on) { deferred_compose_ = on; }
    void coordinator_update(uint32_t frame, uint64_t global_frame, SimRng& rng);
    size_t pending_compositions() const { return pending_.size(); }
    // The adopt 0x39bcb0 (§4.7.1) of a unit of this system, past the host's predicate (team member, locomotor kind,
    // builder, hyperspace): a unit without a coordinator gets one holding it where it is (no motion; Unit::adopted) and
    // the ctor draws int(1, FPS) at once. False, no draw: unknown, or it has a coordinator. stop_and_hold = 0x3a9880:
    // stop, leave every coordinator, adopt (one draw for a known unit).
    bool adopt(ObjectId id, SimRng& rng);
    bool stop_and_hold(ObjectId id, SimRng& rng);
    // cu8l t02 (CU8K_T02_T06_RE §2 / §3, A1_READINGS §28.9): the coordinator-system stop 0x4261f0 per unit - stop and leave
    // every coordinator, no draw (stop_and_hold = this + adopt). False: unknown unit.
    bool stop_and_leave(ObjectId id);
    // cu8l t02 (CU8K_T02_T06_RE §3 "movement lock", rule 6; A1_READINGS §28.8 step 2): the first movement lock's
    // Split_From_Formation 0x4f84e0 - a unit in a formation (a member of a formation of > 1 unit, or running a composed
    // move; not waiting for its composition, not an adopt's hold) gets a formation of its own: one ctor draw int(1, FPS)
    // at once, it keeps its current move and stays in the coordinator. False, no draw: unknown or in no formation (the
    // host then runs the adopt predicate).
    bool split_from_formation(ObjectId id, SimRng& rng);
    // cu8l t02 [port]: where a unit stands for the first lock - None (no coordinator: the adopt may take it), NoFormation (in
    // a coordinator without a formation of its own: waiting for its composition or held by an adopt), Formation (the split
    // applies). Unknown ids: None.
    enum class CoordinatorHold : uint8_t { None, NoFormation, Formation };
    CoordinatorHold coordinator_hold(ObjectId id) const;
    // cu8l t01 (CU8K_T02_T06_RE §1.2 / §4 rule 1): a self-registering locomotor's init (LandTeamContainer 0x5abac0,
    // SimpleSpace 0x625090) gives the new object its own coordinator holding a Move_To its own position: no formation and
    // no draw now. The coordinator composes at the next coordinator_update: land - one ctor draw int(1, FPS), no stagger
    // draw (no vanilla team container type has PathFrameDelay) and no start-frame change; space - the 0x5d58f0
    // composition (one draw per formation). `ids` = the object's units in this system (a land team container: its
    // members, one formation). Units that leave it first (another order, stop, teleport, removal) drop out; left empty it
    // composes nothing. False, nothing registered: no id is a unit of this system.
    bool hold_from_birth(std::span<const ObjectId> ids);
    // cu8l t03 (CU8K_T03_T08_T09_RE §1.10, §3 item 1, §4 item 6): "movement locked" 0x39ad40 rule 1 (the lock count obj
    // +0x3a8 > 0), asked by the space composition 0x5d58f0 for a seed candidate (key = a squadron's container id, else the
    // unit id): a locked seed still gets its own formation (one draw) but recruits nobody; candidates are not tested. The
    // host keeps the counts (the start fleet's hyperspace arrival). Unset: nothing is locked. Not saved (host state).
    using LockedFn = std::function<bool(ObjectId key)>;
    void set_locked_fn(LockedFn fn) { locked_ = std::move(fn); }
    // s78n t10 (TACTICAL_COMBAT §5.4.6 port spec item 4, SPACE_AVOIDANCE §3.1): the space composition 0x5d58f0 tests the
    // world AABB 0x3982b0 of the seed and of each candidate - the model's collision bound through the object's yaw and
    // Scale_Factor (avoid_place; no model: [P, P + 0.1]), for a squadron container the union of its fighters' boxes [M:
    // the union rule] - and compares the box CENTRES. The host gives a unit's model and Scale_Factor: true with a null
    // model = the type has no model for the mode; false = no data, and [port] that candidate keeps the older stand-in
    // (the object position with Custom_Hard extents, else the radius). Unset: the stand-in for everyone. Called from the
    // serial coordinator update only. Not saved (host data, like the lock).
    using ModelBoundFn = std::function<bool(ObjectId unit, const AvoidModel*& model, float& scale)>;
    void set_model_bound_fn(ModelBoundFn fn) { model_bound_ = std::move(fn); }
    // OI tactical (crush, §5.5): owner changes (Change_Owner) and the relation test (player +0x370 table == 1 [M];
    // default: different non-negative players are hostile). The test is called from the serial crush pass only.
    bool set_owner(ObjectId id, int32_t owner);
    using HostileFn = std::function<bool(int32_t a, int32_t b)>;
    void set_hostility(HostileFn fn) { hostile_ = std::move(fn); }
    std::span<const CrushEvent> crush_events() const { return crush_; }  // this tick (land)
    // s78n t20 batch 1 (MOVEMENT §5.4.5 item 2): the death-clone obstacles a collision push killed this tick (land; the
    // push 0x6859a0's first test, 0x391310(b, 1)), in the order of the pushes, each once. The host kills them by the
    // normal death after the tick and removes them (remove_unit).
    std::span<const ObjectId> push_kills() const { return push_kills_; }
    // s78n t26.1 review: the units whose walk ended deployed this tick (Walk 0x16 -> 0x19 with the Deploy clip on its last
    // frame, tick_deploy; ascending id). The host sets that clip (AnimBehaviors::deploy_at_end) whatever clip plays then
    // (Deployed_Walk, a turn, an idle). Visual only: read in the same tick, not saved, not hashed.
    std::span<const ObjectId> deploy_pose_events() const { return deploy_pose_; }
    // L1 systems: Teleport (0x3a8f90; Start_Cinematic_Space_Retreat line-up, Lua Teleport): the unit stops (its
    // order is dropped) and is placed at `pos` / `z` with yaw `yaw_deg`; land: the collision tree entry moves; space:
    // a stationary occupancy track from `frame`. No draws. False when the unit is unknown.
    // s78n t18 (FIGHTER_FLYING §7.4, 0x3abb80): a land squad container takes its members along - each steering (or
    // suspended) member is put at `pos` + its old offset from the container, speed 0, its facing kept.
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
    // While in 0x18 / 0x1A the walk does not move the unit (s78n t26.1, MOVEMENT §3.2.1 item 3: 0x1A entered from a walk
    // freezes the position and yaw and keeps the speed; no move, turn or speed step). In 0x19 it walks only with
    // Max_Dist_Walk_While_Deployed (M) > 0 and a path ("walking deployed", below); a 0x19 unit with M = 0 holds its path
    // and stands. s78n t26.1 (§3.2.1 items 1 - 3): Deploy(true) does nothing while walking deployed; 0x1A clears the flag
    // every frame; an idle Walk (no path) with the flag set returns to 0x19 at once with the Deploy clip on its last frame
    // and no event, and an idle `Deploys` unit with M > 0 and no DEPLOY ability (UnitDesc::deploy_ability) deploys
    // (Deploy(true)): the Destroyer Droid deploys at every stop. The locomotor swap (use_secondary_locomotor, both directions)
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
    // s78n t26.1 (MOVEMENT §3.2.1 item 1, 0x3ac100): the walking-deployed cap test with path length d. A deployed or
    // deploying unit (`Deploys` with a Walk) with d >= 0, M > 0 and d < M walks deployed (the flag only: no state change,
    // no clip); every other case is Deploy(false) (0x18 / 0x19 / the flag -> 0x1A; a rolling unit is unchanged). The
    // garrison hooks 0x636090 / 0x6360b0 call it with d = 0 on the host (Deploy(false) for M = 0: the HTT undeploys).
    void deploy_cap(ObjectId id, float d, uint32_t frame);
    // s78n t26.1 (§3.2.1 item 3, Take_Damage 0x3a9e30): a hit on an idle Walk (state 0x16, no path) `Deploys` unit with
    // M > 0 and the flag clear deploys it (Deploy(true)); serial callers only (the host's damage listener).
    void damage_deploy(ObjectId id, uint32_t frame);
    // s78n t26.1 (§3.2.1 item 2, the group move 0x2a7b80 / waypoint queue 0x2a8240): a deployed (0x39a390: 0x19 or walking
    // deployed) `Deploys` unit that is not an auto-deployer (`Auto_Deploys` + `Deploys`, 0x39a320) is skipped by a move
    // order: not moved, not stopped, its orders kept (MovementSystem::move drops it; the host keeps its combat orders).
    bool move_skips(ObjectId id) const;
    bool walking_deployed(ObjectId id) const;  // the flag (pack +0x2b3)
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
    // squadron). An unset target (id < 0, !point) clears it. The only target source of the fighter locomotor (the point
    // target of set_attack_target is not read by fighters).
    bool set_fighter_target(ObjectId id, const FighterTarget& t);
    bool set_fighter_params(ObjectId id, const FighterParams& p);  // Max_Thrust, Minimum_Follow_Distance, attack range
    void set_fighter_avoid_fn(FighterAvoidFn fn) { favoid_ = std::move(fn); }
    // s78f t02: the fighter formations' host data (fog, attack targets, range, non-mover positions; FighterFormationHooks).
    // The fighter formation API itself is below (fighter_formation / fighter_push / fighter_pop).
    // [port] entry to Dead_Stop 0x1F (the retail writer is untraced, "probably the Stop order"): stored orientation =
    // the current one, the path is dropped.
    bool fighter_dead_stop(ObjectId id);
    // S7 t28 (A1_READINGS §33.4, FIGHTER_FLYING §4.10 E): a guard of a moving object. fighter_copy_path gives squadron
    // `squad` (or a solo fighter) `from`'s space path (0x40dbc0) with every member's waypoint index at from's (S7 t35,
    // A1_READINGS §33.4.1 [port: from's next waypoint by playback time]); each member's goal = the path end; idle / moving
    // members fly it (Moving 0x1B), members in combat keep their state. False (nothing changed) when `from` is a fighter
    // or follows no path.
    bool fighter_copy_path(ObjectId squad, ObjectId from, uint32_t frame);
    // ---- s78f t02: fighter formations (FighterFormation block stacks, FIGHTER_FLYING §4.11; fighter_formation.cpp) ----
    // Per tick (space, before the fighter pass; formations in ascending key, serial): state 0 / 5 -> plan the top (a move
    // of the members to the top's goal point), state 1; state 1 -> the arrival 0x423760 (every member done: n = 1 ->
    // state 3; n >= 2 -> an object top stays (state 3), a 0x04 position top pops only when blocks[n-2] has 0x08, any other
    // position top pops), then 0x4192b0 once per FPS frames; state 3 -> with one block +0x240 := 1 and the hidden-goal
    // rule (C 3), then 0x410a60 on the same timer. Details: fighter_formation.cpp.
    const FighterFormation* fighter_formation(ObjectId key) const;  // null without one (key = squadron or solo fighter)
    // Push 0x41fd30 of an auto attack block (LocomotorCommon 0x42d0f0 -> 0x41cf70): {kind 3, `object`, hard point,
    // flags 0, push class "auto", +0x5d = 1} on the formation of `key`, after the pre-pop (B 1). False (nothing changed)
    // without a formation, for a block naming a fighter inside a squadron (D 1) or an unknown object. The formation plans
    // toward it (state 0) on the next tick.
    bool fighter_push(ObjectId key, ObjectId object, int32_t hard_point = -1);
    // Pop 0x4105a0 (n > 1 only; side effects B 1). False when nothing was popped.
    bool fighter_pop(ObjectId key);
    // s78f review (§4.11 B 1 "new group move"): a group move of `order.units` that moves nobody now (an attack order whose
    // fighters are already in range): each fighter formation is replaced by one holding the order's block, as move()
    // does, in state 1 (the members' current paths stand for the plan [port]). No unit moves. cu8j t01: the group move's
    // composition still runs at the next coordinator update (its ctor draws, the fighter formations' first check).
    void fighter_new_order(const MoveOrder& order, uint32_t frame);
    // s78f review: an object that is not a unit of this system (a station, a structure) is gone: blocks naming it are
    // converted / removed as at a unit's removal (listener 0x4f77e0, fform_removed).
    void object_removed(ObjectId id) { fighter_removed(id); }
    void set_fighter_formation_hooks(FighterFormationHooks hooks) { fhooks_ = std::move(hooks); }
    std::span<const FighterEvent> fighter_events() const { return fevents_; }  // this tick
    // Save / load access (the grids' membership and the per-unit / per-squadron fighter state are sim state).
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
    // existing goldens keep their bytes): hash_speed holds it (the battle host's "battle.abilities"; register_speed_crc
    // for the tests and stand-alone runs).
    bool set_speed_factor(ObjectId id, float factor);
    float speed_factor(ObjectId id) const;
    // OI abilities (TACTICAL_GAPS 2.3.2 Berserker): locomotor speed override +0x2a0 with its flag +0x29c. While set
    // (`speed` >= 0) it replaces UnitDesc::max_speed as the base max speed; [port] the mode multiplier and the speed
    // factor still apply. `speed` < 0 clears it. Hashed by hash_speed only while set (format unchanged otherwise).
    bool set_speed_override(ObjectId id, float speed);
    float speed_override(ObjectId id) const;  // -1 = none
    // s78k t03 (MOVEMENT §3.4 / §3.7, FIGHTER_FLYING §4.11 item 4, TACTICAL_COMBAT §4.4): the unit's TEAM container `team`
    // (-1 = none) and that container's speed factor (its Mod_Multiplier SPEED, the squad value). A team's locomotor runs
    // at the container's effective max speed: the minimum raw Max_Speed of the team's units x the mode multiplier x
    // `container_factor` (0x370f00 with the 0x4049e0 cap). [port] There is no container mover, so on land every Walk
    // member (not a jetpack in its airborne kind: 0x371060 has no team cap) walks at most at that speed, formation move
    // or not; in space a squadron's formation speed (0x41b2a0) is that value. Transient: the host sets it every tick
    // before tick() (not saved, not hashed); default -1 / 1 = no team.
    // s78l t01 (MOVEMENT §3.4 "Team cap, exact" / "Full team formula", TACTICAL_COMBAT §4.4 "Team move speed"): the team
    // speed is min(container Max_Speed, min over the members of their raw Max_Speed) x the mode multiplier x
    // `container_factor` (the host's product of the container's chain: team_mod_multiplier SPEED, fleeing, ...).
    // `container_max_speed` = the container type's raw Max_Speed (< 0: unknown, the members' minimum alone). A land team
    // member walks AT that speed: its own speed factor / Mod_Multiplier does not enter (no double count); only a member
    // with speed factor 0 (stunned: its locomotor is suspended) stays put.
    // s78n review: the team link, factor, raw container Max_Speed and the team cap of the last land tick are saved
    // (version 14), so a move ordered before the first tick after a load (host commands run before the host's set_team
    // calls) reads the same team speed as in the saving run. Still not hashed.
    bool set_team(ObjectId id, ObjectId team, float container_factor, float container_max_speed = -1.0f);
    // ---- s78n t03 (TACTICAL_COMBAT §5.4.4 item 2, MOVEMENT §3.7, FIGHTER_FLYING §7): land squad containers ----
    // A land squad container is a unit of this system added with UnitDesc::team_container (land only). add_unit places it
    // with the free-position search 0x440f90 around the given position (forced_spot: the first passable ring sample; the
    // point itself when passable). set_land_team(container, members, slots) makes each member (a Default-locomotor land
    // unit without Uses_Multiple_Locomotors) steer to its slot: `slots[i]` = (along, side) in the container's frame
    // (pack +0x114 / +0x118), members in team index order. A member's own order and pushed move are dropped. False (nothing
    // changed) when the container is not a team container or a member does not qualify.
    // From then on:
    //   - the container walks its own path (the Walk step: start turn, speed control, its own passability test) at the
    //     team speed (set_team(container, container, ...) gives it the team cap) and is the formation unit of its squad:
    //     move / stop / the coordinator calls on a member act on its container (order_id);
    //   - the container is never re-centred on its members and takes no part in crush (as a victim) or arrival turns;
    //     s78n review [port]: a steering member is no collision checker and no collision partner (moving or stopped
    //     checkers skip it: it is neither stopped nor pathing);
    //   - s78n t14 part B (FIGHTER_FLYING §7.5 item 1, 0x504880): the CONTAINER is a land collision checker and partner
    //     (its type's UnitCollisionClass, Is_Squashable; its MovingVsStopped look-ahead is max(MovingVsMovingLookAheadTime,
    //     MovementPredictionInterval)); other movers avoid / push the container, never a member. A crusher (SQUASH) does
    //     not drive through a squad container it crushes (only through the members, which the crush pass kills): a stopped
    //     container in its way is pushed aside (0x6859a0) on a crush-flee path (kind 7), a moving one gets the collision
    //     path away from it (0x683140: the same push, crush-flee only while its owner's faction Scatters_From_Crushers
    //     (set_scatters_fn) and SPREAD_OUT is off). While the container's current path is a crush-flee push (0x39a5b0) the
    //     squad flees: Fleeing_Infantry_Speed_Bonus multiplies the container's team speed, acceleration and turn rate on
    //     that path and each member's max speed, acceleration and turn rate (FIGHTER_FLYING §7.1); it ends with the push
    //     path (arrived, a TerrainCollision claim or stop, a new order, stop or teleport);
    //   - each member steers per frame (LandTeamInfantry 0x62d620) toward container + R(yaw) (slot + jiggle) from the
    //     container's state at the start of the tick [port: members precede their container in service order], with the
    //     speed rule of FIGHTER_FLYING §7 step 4 (v = 0 while the container turns in place, port state WalkStartTurn for
    //     retail 0x10), the heading rule of step 6 and its own turn rate; the step is committed with no passability test
    //     (FIGHTER_FLYING §7 step 8, §7.3 "Recovery in full" item 7: a member walks through impassable cells, which is
    //     what bounds its lag together with the straggler cap). s78n t14 part B: step 6 adds the collision steering
    //     vector 0x509810 (team_member_avoid) from its container's collision objects;
    //   - s78n t18 (FIGHTER_FLYING §7.7 items 3 - 6, the container service 0x5ad690; MovementSystem::team_walk): the step
    //     order of 0x5aaf70 - target = the team speed (the formation speed on the order path), min the collision cap, min
    //     the straggler cap, and while braking the rate itself (replacing all of it); the speed steps toward the target by
    //     the rate, down at once only with the collision "at once" flag (a MovingVsMoving hold; a plain MovingVsStopped
    //     "try again later" brakes at the rate). Braking starts when the rest of the path is <= v^2 / (2 x rate) and ends
    //     when the speed is <= the rate at the start of a tick: speed 0, no step; within 5.0 before the last waypoint's
    //     line (or past it) the container has arrived where it stands (no snap), after a turn in place to the order's
    //     final facing when that is more than 10 degrees off; else the same path restarts on the next tick. A new move
    //     order while moving: the container brakes at the rate, coasting straight along its facing, then starts the new
    //     path from speed 0 (start delays are drawn only when the order finds it at speed 0); Stop and Teleport zero the
    //     speed at once. s78n t26.5: the coast starts at the path set (until then the old order path is walked on), and the
    //     move step is the Walk's C with MOVEMENT §3.5.2 E's differences (the corner fractions and the brake-ahead included);
    //   - s78n t16 (FIGHTER_FLYING §7.7, 0x5aaf70; confirmed by the retail runs in S78N_T14_RUN.md): the container's speed
    //     step. Acceleration = deceleration = 0x5ab540 = the minimum over its steering members of the member's
    //     OverrideDeceleration x (min(member Max_Speed, the members' smallest raw Max_Speed) x the mode multiplier x the
    //     container factor) / member Max_Speed (the bare OverrideDeceleration when its Max_Speed is 0; a member type
    //     without the tag gives that speed itself, MOVEMENT §3.4, so it does not lower the minimum); a minimum not > 0 =
    //     the team speed (full speed in one frame); the container type's own acceleration tags are not used (FoC: 0.05 x 1.56 = 0.078 Stormtrooper squads, 0.07 x 1.56 = 0.109 Rebel
    //     squads per frame). Straggler cap 0x5ab640: the target speed is at most (1 - t) x the team speed, t the largest
    //     clamp(((-HX - 2) - p) / 10, 0, 1) over the members with p < -(HX + 2), p = (member - container) . the
    //     container's facing, HX = the joint slot box half-length (0x404450: the box holds (0, 0) and every slot x +/- the
    //     member's half-extent; set_team_member_shape) + the container's MaxJiggleDistance. Both are computed serially from
    //     the positions at the start of the tick and apply to the order path and a pushed path alike (on a crush-flee
    //     path both x Fleeing_Infantry_Speed_Bonus, with the team speed). Every TEAM entry counts, whatever its state
    //     (0x5ab640 tests nothing else; s78n t18: team_entries - also a member suspended for a hero clash; an absent,
    //     garrisoned one counts in the box and the rate only). The cap is folded in the moving state only (retail 0x13), not while the
    //     container brakes (0x14, port WalkEndMove), turns in place or stands; at speed 0 in the moving state the
    //     container does nothing else that frame (no turn), and its members keep steering to their slots;
    //   - s78n t14 (FIGHTER_FLYING §7.3): the member's step follows the container's walk state (start of the tick): turning
    //     in place (port WalkStartTurn / WalkTurning = retail 0x10) or moving (0x13): a member with a start delay counts it
    //     down and stands; else it steers (v = 0 while turning). Braking (WalkEndMove = 0x14): it steers, delay ignored.
    //     Stopped (0 / 0x15 / 0x16): the recovery decision 0x62bbf0 - only while the container is laid out and has no
    //     target (set_team_target): d = |slot point - P| > 4.0 -> turn toward it (0x29) when the angle to it exceeds
    //     0.5 x the turn rate, else walk (0x2A); d = 4.0 -> walk; d < 4.0 -> keep walking only while already walking, the
    //     next step closes in and the speed is >= the deceleration, else stand (speed 0). The walk turns toward the slot
    //     point by the turn rate, steps its speed toward the max speed (toward 0 inside v^2 / (2 decel)) and moves along
    //     its new yaw; no passability test, no distance limit, no timer and no give-up. (s78n t14: the earlier [port]
    //     turn in place to the container's facing at the slot is gone: the member turns while the container turns.)
    //   - the slots are re-laid by lay_out_land_team (see there); the `slots` given here only stand until the first one.
    // A member's view reports the container's has_goal / arrived / failed / goal (has_goal also while recovering). Removing
    // the container releases its members (ordinary units again).
    bool set_land_team(ObjectId container, std::span<const ObjectId> members, std::span<const V2> slots);
    // The container a member steers to (-1: none) and its slot (along, side); false when `member` is not a steering member.
    bool team_slot(ObjectId member, ObjectId& container, V2& slot) const;
    // The steering members of `container` in team index order with their slots (empty when none).
    std::vector<std::pair<ObjectId, V2>> land_team_slots(ObjectId container) const;
    // s78n review: the same as a view into an index (container, team index, member) kept by the system (derived from the
    // units: rebuilt by set_land_team / rejoin_land_team / remove_unit / read; not saved, not hashed). No allocation, so
    // the host's per-query readers (fog footprint, Bribe's E_T) can use it.
    struct LandTeamSlot {
        ObjectId container = -1;
        uint32_t tindex = 0;
        ObjectId member = -1;
        V2 slot;
        V2 off;  // s78n t14: parked records only - the member's offset from its container when parked
        // s78n t18 (team_entries): 0 a steering member, 1 suspended (a hero clash: its unit is in the system), 2 absent
        // (garrisoned: parked, its unit is not in the system); the member's layout half-extents and its type's Max_Speed /
        // OverrideDeceleration (the joint box and the rate 0x5ab540 read every TEAM entry). land_team() entries: 0.
        uint8_t kind = 0;
        float hx = 0.0f, hy = 0.0f, max_speed = 0.0f, decel = 0.0f;
    };
    std::span<const LandTeamSlot> land_team(ObjectId container) const;
    // s78n t18 (FIGHTER_FLYING §7.7 item 2: every non-null entry of the TEAM list counts, whatever its state): the
    // container's TEAM entries in team index order - the steering members, the members suspended for a hero clash and the
    // absent (parked) ones. The joint slot box and the rate 0x5ab540 read all of them; the straggler cap reads those with
    // a unit in the system (steering and suspended), each at its live position ([port] an absent member has no position
    // here; with the squad garrisoned as one it exists only while the container is hidden and stands).
    std::span<const LandTeamSlot> team_entries(ObjectId container) const;
    bool is_land_team_container(ObjectId id) const;  // a unit added with UnitDesc::team_container
    // s78n t18 (TACTICAL_COMBAT §5.4.4, 0x404450; FIGHTER_FLYING §7.8 item 2): the container's live joint slot box - the box
    // starts at (0, 0, 0, 0), each team entry grows it to hold slot.x +- its half-extent x and slot.y +- its half-extent y
    // (set_team_member_shape); HX = (max x - min x) / 2 + the container's MaxJiggleDistance, HY likewise. It is the
    // container's collision footprint: a rectangle centred on the container and turned by its yaw (live member positions
    // are not read). The container's UnitDesc::radius follows it (sqrt(HX^2 + HY^2): the formation layout, the dynamic
    // repath's circle). False: not a container, or one without team entries ([port] it keeps its last radius as a circle).
    bool team_joint_box(ObjectId container, float& hx, float& hy) const;
    // s78n t18 (MOVEMENT §5.3.2, the occupancy query 0x538200 of the jetpack landing 0x6420e0): the point passes the
    // unit's movement class and no land layer object's footprint rectangle (moving or standing; a squad container's joint
    // box, never a squad member) overlaps the unit's own rectangle placed at `p`. False: unknown unit.
    bool land_spot_clear(ObjectId id, V2 p) const;
    // s78n review (garrisons): a steering member leaving this system for a while (the garrison entry) keeps its team
    // place: park_team_member records (member, container, slot, team index) and removes the unit (as remove_unit);
    // rejoin_land_team, after the host's add_unit of the same id, steers it to that slot again (as set_land_team does for
    // one member). The record is dropped by rejoin_land_team, by remove_unit of the member (it died inside) and by the
    // container's removal. Saved (version 14) and hashed while any exists. False: not a steering member (nothing done) /
    // no record, the container is gone, or the unit does not qualify (set_land_team's member rule).
    bool park_team_member(ObjectId member);
    // s78n t26.1 review (MOVEMENT §3.2.1, FIGHTER_FLYING §7.4): a unit entering a garrison leaves this system, but retail
    // keeps its locomotor pack - a droid deployed on entry walks out deployed. garrison_remove(id) keeps the Walk deploy
    // state of a `Deploys` land unit that is not undeployed (the state, the walking-deployed flag, the deploy clip and its
    // position at `frame`) and then parks (park_team_member) or removes the unit; restore_deploy(id), after the host's
    // add_unit of the same id, puts that state back (the clip resumes at its kept position from `frame`). The record is
    // dropped by restore_deploy and by remove_unit (the unit died inside). Saved (version 21) and hashed while any exists.
    // garrison_remove: false when the unit is unknown. restore_deploy: false when there is no record or no such unit.
    bool garrison_remove(ObjectId id, uint32_t frame);
    bool restore_deploy(ObjectId id, uint32_t frame);
    bool deploy_kept(ObjectId id) const;  // a garrison_remove record exists
    // s78n t06 review (hero clashes): the same record, but the member stays in this system as an ordinary unit where it
    // stands (anchor dropped, no own order: as attach_member leaves it), so move / stop / stop_and_leave act on the member
    // itself instead of its container; rejoin_land_team ends it. False: not a steering member (nothing done).
    bool suspend_team_member(ObjectId member);
    bool rejoin_land_team(ObjectId member);
    bool team_member_parked(ObjectId member) const;
    // s78n t14 (FIGHTER_FLYING §7.4): the parked member's offset from its container when it was parked (member position -
    // container position; zero when the container was gone). False: no record.
    bool parked_offset(ObjectId member, V2& off) const;
    // ---- s78n t14 (FIGHTER_FLYING §7.2 / §7.3): slot layout, re-lay, start delays ----
    // lay_out_land_team = 0x5ac120(container, teleport 0, dir): dir is normalised (sqrtf, x 1 / len); when the container is
    // laid out and dot(dir, its last layout dir) > 0.99 nothing changes (false). Else the container type's
    // ContainerArrangement (UnitDesc::container_arrangement) picks Rows 0x5ac970 (0) or Dartboard 0x5ac2a0 (1) (anything
    // else: false); on success every steering member's slot is rewritten (assigned by where the members stand: a
    // projection sort, not the team index), the container stores dir and its laid-out flag (saved, hashed) and true is
    // returned. Inputs: the members' shapes (set_team_member_shape), their own FormationSpacing (UnitDesc), the
    // container's SPREAD_OUT state (set_team_spread_out: x Spread_Out_Spacing_Coefficient) and, for Dartboard, the Halton
    // seed (int32) container id x `frame` (wrapping uint32). The land tick calls it for each container (serial, ascending
    // id, after the walk, so the members read the new slots from the next tick): at a move start (toward the current
    // waypoint), at every waypoint passed (the next segment), for a pushed path of >= 2 points (wp1 - wp0) and, while the
    // flag is clear, with the container's facing (its first tick after set_land_team, after a SPREAD_OUT toggle).
    bool lay_out_land_team(ObjectId container, V2 dir, uint32_t frame);
    // The container's laid-out flag (pack +0x2a4) and last layout dir (+0xd8); false when not a team container.
    bool land_team_layout(ObjectId container, bool& laid_out, V2& dir) const;
    // A steering member's half-extents (0x396b70 / 0x396a30: the unit's, deployed variants included; Rows) and its type's
    // (0x371be0; Dartboard reads member 0's). Saved, not hashed (static inputs). Default 0. False: not in the system.
    bool set_team_member_shape(ObjectId member, float hx, float hy, float type_hx, float type_hy);
    // SPREAD_OUT active on the container (0x39b950(K, 7, 1)): a change clears the laid-out flag (the handler 0x3a3500
    // clears +0x2a4 on both on and off), so the next tick re-lays with the container's facing. Saved and hashed. The host
    // sets it every tick before tick() [port: polled, so a toggle that does not change the state does not re-lay].
    bool set_team_spread_out(ObjectId container, bool on);
    // ---- s78n t18 (MOVEMENT §5.2 item 5 / §5.3 "Pair gates"): no collision object for a gated pair ----
    // A pair gets no collision object when either side is hidden (+0x3a7: garrisoned; such a unit is no layer object at
    // all), when either one's garrison host is the other (all three tests, from the garrison order until the unit's next
    // order), when a raised hover unit meets a grounded one (MovingVsMoving needs equal raised state; a raised mover's
    // MovingVsStopped needs a raised obstacle; a grounded mover takes a raised standing unit; StoppedVsStopped ignores
    // it). There is no parent, attachment or own-team rule. The gates sit inside the contact tests: a gate that starts
    // later does not by itself cancel a live record; it acts where a state re-runs the test. Not movement units in the
    // port, so not tested here: landing transports (taking off / the units they unloaded) and build pads.
    //   set_pair_gate(id, host, hidden): the unit's garrison host while it walks to it or enters it (GarrisonDataPack +0xc
    //     in states 1 / 2 / 5; -1 none) and whether it is hidden (a squad container whose whole squad is inside). Host-set
    //     every tick before tick(); transient (not saved, not hashed). A hidden unit does not walk and ignores move orders,
    //     so the host also sets it whenever its garrison state changes and right after read() (s78n t18 review: an order
    //     given between a load and the next tick meets the same gates as in the saving run). pair_gate() reads them back
    //     (false: unknown unit).
    //   set_garrison_exit(id, host): the host a unit has just left (state 4 on): kept until the unit's next move order or
    //     stop, or the arrival of its current (walk-out) move if that comes first; Teleport does not clear it. Saved and
    //     hashed while set. False: unknown unit.
    bool set_pair_gate(ObjectId id, ObjectId garrison_host, bool hidden);
    bool pair_gate(ObjectId id, ObjectId& garrison_host, bool& hidden) const;
    bool set_garrison_exit(ObjectId id, ObjectId host);
    // s78n t26.1 (MOVEMENT §3.2.1 item 1): (unit, host) of every walk-out move that finished since the last call (a unit
    // with a set_garrison_exit host arrived); the host runs the garrison host's +0xf8 hook (the cap test, d = 0). Drained
    // every tick, not saved (empty between frames).
    std::vector<std::pair<ObjectId, ObjectId>> take_exit_arrivals();
    // ---- s78n t20 (MOVEMENT §5.4.3 - §5.4.5, §1.5.1): what the land collision decisions read from the host ----
    // The unit's current target 0x397a40 (its formation's, else its own acquired one; -1 none) with its position, the
    // unit's attack range 0x3749d0, the target's radius 0x397780 when the unit has a weapon (else 0), "short-range"
    // 0x372210 (Targeting_Max_Attack_Distance <= Melee_Cutoff_Range) and "in a hero clash" (+0x3b4). Host-set every tick
    // before tick() for every land unit and squad container, like set_pair_gate: transient (not saved, not hashed - the
    // decisions it feeds are), so the host sets it again right after read(). Default: no target, range 0, no clash.
    struct CollisionInfo {
        ObjectId target = -1;
        V2 target_pos;
        float attack_range = 0.0f;
        float target_radius = 0.0f;
        bool short_range = false;
        bool hero_clash = false;
    };
    bool set_collision_info(ObjectId id, const CollisionInfo& info);
    // s78n t20: the info the host set last (false for an unknown id; tests).
    bool collision_info(ObjectId id, CollisionInfo& out) const;
    // The radius 0x397780 of a land unit: sqrt(HX^2 + HY^2) of the joint slot box for a squad container, the soft
    // footprint radius for any other unit; 0 for an unknown id.
    float collision_radius(ObjectId id) const;
    // The unit's top path record (the collision-owned path above its order path): its points (travelled ones included)
    // and the hold tags of its waypoints; empty without one.
    std::span<const V2> top_path(ObjectId id) const;
    std::span<const HoldTag> top_tags(ObjectId id) const;
    // s78n t23 (MOVEMENT §5.3 "When the deployed pair is used"): the footprint rectangle's half extents 0x396850 and the
    // half width across 0x396b70 of a land unit in its current deploy state; (0, 0) / 0 for an unknown id.
    V2 collision_half(ObjectId id) const;
    float collision_half_width(ObjectId id) const;
    // Hover state (pack +0x2a8) of a Walk unit whose type has Hover_Height: 0 grounded (spawn), 2 rising (set by the
    // start of a move while not raised, after any turn in place on the ground: z climbs by the max speed per frame to
    // the ground + Hover_Height, then is 1 raised, standing at the ground + Hover_Height), 3 lowering (hover_lower: a
    // GARRISON_HOVER host whose slot a unit reserves, or that is asked to eject - the host stops it first; z falls back
    // to the ground, then 0; its walk does not run meanwhile and it rises again at its next move). s78n t20 batch 2: z
    // (UnitView::z) is the unit's height. Only "raised" (1) counts in the gates. hover_lower: false for an unknown unit
    // or a type without Hover_Height.
    bool hover_lower(ObjectId id);
    uint8_t hover_state(ObjectId id) const;
    // The container has a current target (0x397a40(K, 0) [M]): its members do not recover to their slots while it idles.
    // Set by the host every tick before tick(); saved and hashed. False: not a team container.
    bool set_team_target(ObjectId container, bool has_target);
    // The member's start delay (pack +0xe4; FIGHTER_FLYING §7.3 item 2): drawn (synced int [0, Max_Move_Frame_Delay], team
    // index order) when its stopped container (speed 0) gets a move order; counts down while the container turns in
    // place or moves (the member stands); braking ignores it. 0 for a non-member.
    uint32_t team_start_delay(ObjectId member) const;
    // ---- s78n t14 part B (FIGHTER_FLYING §7.1 / §7.5): fleeing and the member collision steering vector ----
    // Scatters_From_Crushers (Faction +0x2f88) of an owner's faction, for 0x5c8890 (with the container's SPREAD_OUT off).
    // Called from the serial collision passes only. Unset: every owner scatters (FoC: every faction but the Empire).
    using ScattersFn = std::function<bool(int32_t owner)>;
    void set_scatters_fn(ScattersFn fn) { scatters_ = std::move(fn); }
    // The squad flees (0x39a5b0): the container's current path is a crush-flee push (kind 7). False: not a team container.
    bool team_fleeing(ObjectId container) const;
    // The collision steering vector 0x509810 of steering member `member` for the unnormalised heading `dir` (FIGHTER_FLYING
    // §7 step 6), from the current positions and its container's collision objects in list order: per object the weight
    // (MovingVsMoving 1, MovingVsStopped 2, StoppedVsStopped 3) and the vector - s78n t18 (§7.5 corrections): a
    // MovingVsStopped object gives one on the obstacle's side in every state and on the mover's side only in Repulsion, a
    // MovingVsMoving object on both sides in Repulse only - the side steps 0x5c71a0 away from the other participant, or
    // that participant's members when it is a squad container, x clamp(speed / max speed, 0, 1); objects whose vectors
    // fail the 25-unit passability probes are dropped; best = the largest weight (a tie: the longer vector), plus every
    // other kept vector with dot(best, v) >= -0.5 (retail's index quirk copied). Side step of a against b: t = clamp(1 -
    // |a - b| (3-D) / ((r_a + r_b) x 1.3), 0, 1) with r = the soft footprint radius (0x397780 of a non-container); with L
    // = (r_a + r_b) x 1.3 and left = (-dir.y, dir.x), a side is free when the segment B -> B + L side and the corridor
    // from B + L side - L dir to B + L side + L dir pass a's movement class; b on a's right and left free -> + t left,
    // else right free -> - t left, else 0. The steering tick calls the same code with the start-of-tick positions. A Walk
    // unit that is no squad member adds the same vector (key: itself) to its heading on every leg but the last
    // (MOVEMENT §5.4.1; `member` may be such a unit here). {0, 0}: unknown unit / no objects.
    V2 team_member_avoid(ObjectId member, V2 dir) const;
    void hash_speed(StateHasher& h) const;
    // ---- s78l t02 (MOVEMENT §3.5.1 "Arrival turn, details"): clips of the Walk arrival turn ----
    // Every clip play the arrival turn asks for this tick, ascending id (at most one per unit and tick). The clip data
    // (frame count, fps) comes from set_deploy_clip_fn's hook (any anim id; no data = the model has no such clip, which
    // is then never played); the turn times each clip itself (fps / 30 x speed clip frames per sim frame, "in its last 2
    // frames" = position >= count - 2), so the host only draws: play `anim` with index 0xFFFF (a synced variant draw
    // when the clip has >= 2 variants), loop 0, at `speed` (begin / band / end clips: the blend globals 0x140b398fc /
    // 0x140b39900; the plain TURNL / TURNR clip of a unit without Has_Pre_Turn_Anim: blend min(|e| / r x 0.03333, GC
    // cap) carried in `blend`, speed 1). `idle` = the turn popped the path: play the default idle type (TACTICAL_COMBAT
    // slot 47; index 0, loop Loop_Idle_Anim_00) unless the unit burns or the pack flag +0x2b3 is set (host tests).
    // Drained by take_turn_clip_events() right after each tick (not saved); tick() clears the list first, so events not
    // taken are dropped at the next tick.
    struct TurnClipEvent {
        ObjectId id = 0;
        int32_t anim = -1;  // kAnimNone with `idle`
        float speed = 1.0f;
        float blend = -1.0f;  // >= 0: the TURNL / TURNR blend of a unit without Has_Pre_Turn_Anim; -1: the clip's global
        bool idle = false;
    };
    std::vector<TurnClipEvent> take_turn_clip_events();
    void register_speed_crc(CrcRegistry& registry, const std::string& name) const;

    // ---- CRC (see "CRC" in the host contract at the top)
    void hash_units(StateHasher& h) const;
    void register_crc(CrcRegistry& registry, const std::string& name) const;  // tests / stand-alone runs

    // ---- N1 stage 2 save / load (contract: alamo/core/save_file.hpp; src/movement_save.cpp) ----
    // The loader builds the battle the same way first: MovementSystem(same MovementConfig), set_grid(grid_from_map) +
    // the same map stamps, the same space / air boxes and static entries, every hook (set_height_fn, set_hostility,
    // set_threat_fn, set_anim_length_fn, set_flying_hooks, set_fighter_avoid_fn, set_fighter_formation_hooks,
    // set_scatters_fn); then read() replaces ALL mutable state, so no host call is needed after it (no relink:
    // cross-references are object ids, hooks are not touched). One host input is not saved: set_pair_gate (a unit's
    // garrison host while it walks in, and "hidden") is unset by read(). The host sets it again right after read() (s78n
    // t18 review: move() drops orders for hidden units, so it must hold before the first command, not only before the
    // next tick()), from the same state it sets it from on every tick.
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
    //   are hashed), the squadrons (ascending id), the fighter cell grids, this tick's fighter events and the fighter
    //   formations (s78f t02, version 10: ascending key, each block stack bottom first), the pending compositions and
    //   the last serial (cu8j t01, version 13).
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
        float final_facing = kNoFacing;  // s78k t01: MoveOrder::final_facing_deg of the current land move (saved)
        bool forced = false;             // s78k t01: land flag-0x20 move: straight 2-point path, no passability (saved)
        uint32_t start_frame = 0;
        float formation_speed = 3.0e38f;
        int32_t large_hover_index = -1;  // P1 double-click: formation +0x22c (not hashed; no modelled consumer)
        int32_t check_countdown = 0;
        // s78n t18 (0x509ea0): the lowest speed this tick's collision records answer the unit (3e38: none) and the "at
        // once" flag of the last record that writes one. Rebuilt by every land tick's driver before the walk (transient:
        // not saved, not hashed; the records' own answers are).
        float coll_speed = 3.0e38f;
        bool coll_now = false;
        // s78n t18 (MOVEMENT §5.4.1, save version 17): the units the top path was planned around and the frame of that plan
        // (decision step 3); the frame of the unit's last path push or pop (pack +0x288).
        std::vector<ObjectId> around;
        uint32_t around_frame = 0;
        uint32_t path_change = 0;
        // a1k t03 (A1_READINGS §40.3): the collision-owned record on top of the locomotor's path stack, walked over the
        // order move (m.path / goal stay underneath, untouched, and resume when it ends). s78n t26.2 (MOVEMENT §3.5.2,
        // §5.4.7 item 6; save version 25, replaces t24's push_turn_first): push_frac / push_turn_done = the record's speed
        // fractions and turn-done byte (LandMotion::frac / turn_done of the record, swapped in with push_path / push_next
        // for its walk); a TerrainCollision detour carries {0.0} (waypoint 0 at fraction 0: the unit turns in place onto
        // its first leg before it moves). s78n t20 (MOVEMENT
        // §1.5.1 item 5, §5.4.3; save version 18): one
        // record for every collision path - push_kind 1 a push 0x6859a0, 2 a Path Around detour, 3 a StoppedVsStopped
        // move; push_owner = the collision record that owns it (seq, pack +0x248; kNoDetour none) and push_weight that
        // record's weight (vf +0x30: MovingVsMoving 1, MovingVsStopped 2, StoppedVsStopped 3); push_resume = the order
        // path index the unit resumes at when a detour ends (the end waypoint of the rejoin point's segment; the path
        // size when the detour ends past the path end); push_tags = the hold tags of its waypoints.
        bool pushed = false;
        std::vector<V2> push_path;
        uint32_t push_next = 0;
        std::vector<float> push_frac;
        bool push_turn_done = false;
        uint32_t push_owner = 0xFFFFFFFFu;
        uint8_t push_weight = 0;
        uint8_t push_kind = 0;
        uint32_t push_resume = 0;
        std::vector<HoldTag> push_tags;
        // s78n t20 (MOVEMENT §5.4.4 item 5, §4.3.1; save version 18): the formation of the unit's last land order (0
        // none; unique per formation of an order) and its slot there (x = along the formation's forward axis, y = to
        // its right); self_arrive = a one-member formation whose order has neither flag 0x20 nor an object goal (the
        // single-member shortcut of the plan 0x41c1a0: ordered onto its own footprint it does not move).
        // s78n t20 F5 (save version 19): self_goal = the slot's exact point (before any cell snap), which the shortcut
        // measures to.
        uint32_t form_id = 0;
        V2 form_slot;
        bool self_arrive = false;
        V2 self_goal;
        CollisionInfo ci;  // s78n t20: set_collision_info (transient, host-set every tick)
        // s78n t23 step 0: an early-arrival cut left a path record of the unit on its last waypoint during this tick's
        // collision driver; tick_land finishes it at the walk step (cut_finished). Transient: false between ticks.
        bool cut_pending = false;
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
        // s78k t03 / s78l t01; s78n review: saved (version 14) - the host sets the first three every tick (set_team), but
        // a command before the first tick after a load reads them (move_land's team speed).
        ObjectId team = -1;          // s78k t03: TEAM container (set_team)
        float team_factor = 1.0f;    // s78k t03: the container's speed factor (set_team)
        float team_cap = 3.0e38f;    // s78k t03: land team locomotor cap, rebuilt each land tick (team_caps)
        float team_max = -1.0f;      // s78l t01: the container's raw Max_Speed (set_team; < 0 unknown)
        // s78n t16 (FIGHTER_FLYING §7.7): a squad container's speed step inputs, rebuilt each land tick before the walk
        // (team_speed_steps; transient: not saved, not hashed) - its acceleration = deceleration 0x5ab540 (0: full speed
        // in one frame) and the straggler factor of 0x5ab640 (cap = factor x the team speed; 1 = no straggler).
        float team_accel = 0.0f;
        float straggle = 1.0f;
        // s78l t02 (MOVEMENT §3.5.1, save version 12): the Walk arrival turn state (None, 0x0F begin, 0x10 turn, 0x11 end;
        // the wanted yaw is turn_to), the band rate pack +0xe8 (0 = the stopped rate), the clip the turn last played
        // (kAnimNone before the first: treated as idle-class), its start frame and play speed. tplay = this tick's play
        // request (transient, gathered into turn_events_).
        LocoState aturn = LocoState::None;
        // s78n t26.2 (MOVEMENT §3.5.2 A, save version 25): the turn is the start turn in place before a move (the start
        // rule failed; its wanted yaw is the top path's, re-read every frame) - it ends in Start Move, not in the pop.
        bool aturn_start = false;
        float aturn_rate = 0.0f;
        int32_t tclip = -1;
        uint32_t tclip_start = 0;
        float tclip_speed = 1.0f;
        bool tplay_set = false;
        TurnClipEvent tplay;
        // arrival_turn_ended of the last tick, and a drop outside the turn pass (arrival_reset) waiting for the next tick
        // (saved, version 12).
        bool aturn_ended = false, aturn_end_pending = false;
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
        // s78n t26.1 (MOVEMENT §3.2.1): walking while deployed (pack +0x2b3; save version 21). Set only with
        // UnitDesc::max_walk_deployed > 0; `deploy` stays WalkDeployed meanwhile and `m.state` is a Walk move state.
        bool walk_deployed = false;
        // S8 t24: TURRET state (TD +0x48..+0x5c current / target angles x, elevation, yaw; +0x6c idle time (s); +0x70
        // hold; +0xf6 aiming). Starts at Turret_Rest_Angle [port: the spawn pose is not traced] (save version 7).
        float tcur[3] = {0.0f, 0.0f, 0.0f};
        float ttgt[3] = {0.0f, 0.0f, 0.0f};
        float tidle = 0.0f;
        bool thold = false, taim = false;
        // FIGHTER_FLYING.md §2-§5 (space fighters)
        FighterState fs;
        FighterParams fp;
        // cu8j t01 / t02 (save version 13): the PendingComposition serial this unit waits in (0 none) and the adopt's
        // degenerate coordinator (MOVEMENT §4.7.1).
        uint32_t pend = 0;
        bool adopted = false;
        // s78n t03 (save version 14): a steering land squad member - its container (-1 none), slot (along, side; pack
        // +0x114 / +0x118), team index and recovery state (0x29 / 0x2A).
        ObjectId anchor = -1;
        V2 tslot;
        uint32_t tindex = 0;
        uint8_t recovery = 0;  // s78n t14: the member's recovery state: 0 none, 0x29 turning, 0x2A moving (save version 15)
        // s78n t14 (save version 15): a member's start delay (pack +0xe4) and shape (set_team_member_shape); a container's
        // laid-out flag (+0x2a4) and last layout dir (+0xd8), SPREAD_OUT state and current-target flag (host-set).
        uint32_t start_delay = 0;
        float team_hx = 0.0f, team_hy = 0.0f, team_thx = 0.0f, team_thy = 0.0f;
        bool laid_out = false;
        V2 lay_dir;
        bool spread = false;
        bool team_target = false;
        // s78n t14 part B (FIGHTER_FLYING §7.1, save version 16): the pushed move is a crush-flee path (kind 7; else kind 4).
        bool push_flee = false;
        // s78n t18 (MOVEMENT §5.3 pair gates): gar_host - set_pair_gate's garrison host (transient, host-set every tick;
        // -1 none); exit_host - set_garrison_exit (saved, version 17); hover / hover_off - the hover state (pack +0x2a8)
        // and the height above the ground (s78n t20 batch 2; saved, version 17).
        ObjectId gar_host = -1;
        ObjectId exit_host = -1;
        uint8_t hover = 0;
        float hover_off = 0.0f;
        // s78n t18 (FIGHTER_FLYING §7.7 items 5 - 6, save version 17): a squad container's walk - coast: a new order
        // arrived while it moved (it brakes at its rate, coasting straight on, before the new path starts); fturn: it
        // turns in place to the order's final facing before it arrives; path_started: the order path was started (pack
        // path +0x6d; a pushed path over a started path ends without braking). Hashed for containers.
        bool coast = false, fturn = false, path_started = false;
        // s78n t26.4 (MOVEMENT §3.5.2 C.4, save version 27): the position at the start of the previous land tick (pack
        // +0x194..: the snapshot of the object service); m.last_dir = m.pos - prev_pos is taken at the start of each land
        // tick. Set at add_unit in land mode only (hashed land-only, 0xEB).
        V2 prev_pos;
        // s78n t18 (MOVEMENT §5.3 "+0x3a7"): hidden - garrisoned with its whole squad (host-set every tick, at every
        // garrison change and after a load: set_pair_gate; transient). A hidden unit is no layer object and does not walk.
        bool hidden = false;
        // s78n t18 (MOVEMENT §5.3.1, TACTICAL_COMBAT §5.4.4): a squad container's live joint slot box 0x404450 (half
        // extents; team_boxed: it has team entries) and a member suspended for a hero clash (still a TEAM entry, never a
        // layer object). Derived from the team lists (team_ix_rebuild): not saved, not hashed.
        float team_box_hx = 0.0f, team_box_hy = 0.0f;
        bool team_boxed = false;
        bool team_suspended = false;
        // s78n t14: this tick's re-lay requests of a container (layout dirs in order; written by its own walk or a push,
        // consumed by the serial pass after the walk). Transient: empty between ticks, not saved.
        std::vector<V2> relay;
    };
    // s78n t03: a container's state at the start of the land tick, read by its steering members (tick_land).
    struct TeamSnap {
        V2 pos;
        float z = 0.0f;  // the recovery brake distance is 3-D with the slot point at the container's height (§7.3)
        float yaw = 0.0f;
        float speed = 0.0f;
        LocoState state = LocoState::WalkStopped;
        bool has_goal = false;
        bool laid_out = false;  // s78n t14: the recovery decision needs the laid-out flag and no target
        bool has_target = false;
        bool fleeing = false;  // s78n t14 part B: the container's current path is a crush-flee push (0x39a5b0)
        uint32_t rec_begin = 0, rec_end = 0;  // s78n t14 part B: its collision objects in the tick's SteerRec list
    };
    // s78n t14 part B (FIGHTER_FLYING §7.5): one collision object of a participant (key: a squad container for its
    // members, a Walk unit for itself), as the steering vector 0x509810 reads it: weight (vf +0x30), whether it gives a
    // vector on that side (vf +0x38) and the other participant (units_ index).
    struct SteerRec {
        ObjectId container = -1;
        float w = 0.0f;
        bool vec = false;
        uint32_t other = 0;
    };
    void team_steer_recs(std::vector<SteerRec>& out) const;  // every participant's objects, by key, list order kept
    // s78n t18 review: a unit's position and height as the steering vector reads the OTHER units - the land tick's
    // start-of-tick snapshot (the parallel walk moves them), by units_ index.
    struct SteerPos {
        V2 pos;
        float z = 0.0f;
    };
    // 0x509810 for member `a` at `pa` (speed / max speed for the 0x5c6d40 ratio), `pos` = the snapshot by units_ index.
    V2 member_avoid(const Unit& a, V2 pa, float speed, float vmax, V2 dir, std::span<const SteerRec> recs,
                    std::span<const SteerPos> pos) const;
    V2 member_side_steps(const Unit& a, V2 pa, V2 dir, uint32_t other, std::span<const SteerPos> pos) const;  // 0x5c6d40 sum
    // 0x397780: sqrt(HX^2 + HY^2) of the joint slot box for a squad container, the soft footprint radius for any other unit
    float steer_radius(const Unit& u) const;
    // s78n t14 (FIGHTER_FLYING §7.2): Rows 0x5ac970 / Dartboard 0x5ac2a0 on the container's steering members (no flag test).
    bool lay_out_rows(const Unit& k, V2 dir, std::vector<std::pair<Unit*, V2>>& out);
    bool lay_out_dartboard(const Unit& k, V2 dir, uint32_t frame, std::vector<std::pair<Unit*, V2>>& out);
    void team_relay_pass(uint32_t frame);  // s78n t14: the re-lays of this tick (serial, ascending id)
    WalkResult team_walk(Unit& u, uint32_t frame) const;  // s78n t18: one frame of a squad container's walk (land_team.cpp)
    float team_rate(const Unit& k) const;  // s78n t16: the container's rate 0x5ab540 (0 = none; land_team.cpp)
    ObjectId order_id(ObjectId id) const;  // s78n t03: a steering member's container, else id
    // s78n t03: one member frame (writes only u); s78n t14 part B: + the tick's steering records and start positions
    void steer_member(Unit& u, const TeamSnap& k, std::span<const SteerRec> recs, std::span<const SteerPos> pos0) const;
    // cu8j t01 (MOVEMENT §4.6): an order's formations waiting for the coordinator update (creation order). Land: `forms`
    // = the split's formations, seed first; space: forms[0] = the order's units in order and `offsets` their start
    // offsets. `moves` false: fighter_new_order (nobody moves: draws and fighter timers only).
    struct PendingComposition {
        uint32_t serial = 0;
        bool space = false;
        bool moves = true;
        V2 goal;
        std::vector<std::vector<ObjectId>> forms;
        std::vector<uint32_t> offsets;
    };
    uint32_t compose_register(bool space, bool moves, V2 goal);  // a new PendingComposition (pending_.back()); its serial
    void compose_land(const PendingComposition& pc, uint32_t frame, uint64_t global_frame, SimRng& rng);
    bool compose_space(const PendingComposition& pc, uint32_t frame, SimRng& rng);  // false: stopped, retry next update
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
    // s78n t26.1 (deploy_system.cpp): D(x) = 0x39a390, the auto-deployer test 0x39a320(x, 1), the cap test 0x3ac100, the
    // path start 0x61e570, the idle 0x16 rules and the 3-D length of a land path (heights from the height hook).
    static bool deployed_d(const Unit& u) { return u.d.deploys && (u.walk_deployed || u.deploy == LocoState::WalkDeployed); }
    static bool auto_deployer(const Unit& u) { return u.d.auto_deploys && u.d.deploys; }
    void deploy_cap_unit(Unit& u, float d, uint32_t frame);
    void deploy_path_start(Unit& u, float remaining, uint32_t frame);
    float path_length_3d(std::span<const V2> path) const;
    bool deploy_walks(const Unit& u) const;  // a 0x19 unit with M > 0 walks a path it holds (the 0x19 branch)
    void walk_out_done(Unit& u);  // s78n t26.1: the move finished - the garrison-host gate ends (reported when set)
    static bool deploy_immobile(const Unit& x);  // deployed / deploying with M <= 0 (push, StoppedVsStopped)
    static bool deploy_branch(Unit& u);          // Walk 0x19 with a path: start walking deployed (true = the frame ends)
    bool deploy_gate_holds(const Unit& u) const;  // the plan's gate (every member a deployed / deploying manual deployer)
    void plan_deploy_cap(Unit& u, uint32_t frame);  // the plan's cap test for an auto-deployer
    // s78n t26.3 (MOVEMENT §3.5.2 D): the corner-speed pass 0x5070b0 on a new land order path (u.m.frac); serial.
    void corner_pass(Unit& u);
    std::vector<uint8_t> corner_static_;  // per unit index: Static (not moving) before this plan; scratch, not saved
    void tick_deploy(uint32_t frame);
    void hash_deploy(StateHasher& h) const;
    void hash_turrets(StateHasher& h) const;  // S8 t24
    // S7 t14 (§3.1.1): JetPack vf74 / vf75 - Alternate_Max_Speed / _Rate_Of_Turn (mode multiplier, speed factor /
    // override as for Max_Speed) while the active jetpack is in its airborne kind, else lp.max_speed / lp.turn_rate.
    bool jet_alternates(const Unit& u) const;
    float loco_max_speed(const Unit& u) const;
    // Object_Max_Speed_Multiplier_Land / _Space by the configured mode.
    float mode_mult() const {
        return cfg_.mode == MoveMode::Land ? cfg_.constants.land_speed_multiplier : cfg_.constants.space_speed_multiplier;
    }
    // s78k t03 / s78l t01: raw Max_Speed x the mode multiplier (0x4049e0 operand), capped by the container's Max_Speed x
    // the mode multiplier when set_team gave one.
    float team_base_speed(const Unit& u) const;
    void team_caps();                            // s78k t03: Unit::team_cap of every land unit (serial, units_ order)
    void team_speed_steps();  // s78n t16: Unit::team_accel / straggle of every squad container (serial, after team_caps)
    void land_arrival_turn(Unit& u) const;       // s78k t01: MOVEMENT §3.5.1 state 0x15 test on arrival
    // s78l t02: the arrival turn states 0x0F / 0x10 / 0x11 / 0x15 of one frame (serial), the clip helpers, the reset.
    void arrival_turn_frame(Unit& u, uint32_t frame) const;
    bool turn_clip_data(const Unit& u, int32_t anim, DeployClip& c) const;
    bool turn_clip_near_end(const Unit& u, uint32_t frame) const;  // 0x39a180 on the turn's current clip
    bool turn_clip_play(Unit& u, int32_t anim, float speed, float blend, uint32_t frame) const;
    float turn_clip_speed(const Unit& u) const;  // 0x3736a0
    float stopped_rate(const Unit& u) const;     // 0x374310 at speed 0: Stopped_Rate_Of_Turn, else R
    static void arrival_reset(Unit& u);
    // s78n t26.2 / t26.4 (MOVEMENT §3.5.2 A, §5.4.1 "How Walk combines"): the wanted yaw of a start turn in place - the
    // wanted direction 0x61d570 of the top path (segment direction + lane term + collision vector, walk_wanted_dir with
    // the walk's extras `x`); false: nothing left to walk.
    bool start_turn_want(const Unit& u, uint32_t frame, const WalkExtras& x, float& want) const;
    // s78n t26.4 (MOVEMENT §3.5.2 G): the path set's "started" byte - a Walk unit moving in 0x12 - 0x14 keeps its state and
    // speed and starts the new path (0x410170 "aligned") when waypoint 0's speed fraction is > 0 and the first leg's
    // heading is within R or Close_Enough_Angle_For_Move_Start of its facing; else the path is not started (a moving unit
    // coasts to a stop in End Move, a standing one starts it at its walk's 0x16).
    void set_path_started(Unit& u) const;
    // s78n t26.2 (MOVEMENT §3.5.1 "Arrival turn, details" item 2): the arrival threshold's "current waypoint cap 0" - the
    // speed fraction of the top path's waypoint behind the unit (path +0x60).
    static bool cap_zero(const Unit& u);
    // s78n t26.2 (MOVEMENT §3.5.2 A): a walk whose start rule failed (WalkResult::turn) starts the turn in place 0xF
    // (serial pass, with the arrival turn's states); [port] a non-Walk locomotor (Bike) keeps the plain turn by its rate.
    static void start_turn_mark(Unit& u, const WalkResult& r);
    // s78n t26.2 (MOVEMENT §5.3 "What Moving is", vf +0x1b0 0x42cd30): turning in place (Walk states 0xF / 0x10 / 0x11 -
    // a start turn, an arrival turn - or a container's turn in place).
    static bool in_place_turn(const Unit& u) {
        return u.aturn != LocoState::None || u.fturn || u.m.state == LocoState::WalkStartTurn ||
               u.m.state == LocoState::WalkTurning || u.m.state == LocoState::WalkEndTurn;
    }
    // s78l t02 c (MOVEMENT §6.1 "Forced-path destination check and fallback"): the 0x440f90 ring search around `d`.
    bool forced_spot(const Unit& u, V2 d, V2& out) const;
    float land_wanted_yaw(const Unit& u) const;  // s78k t01: the final facing through Movement_Directions_Count
    float loco_turn_rate(const Unit& u) const;
    bool ground_collider(const Unit& u) const;  // takes part in land collision / crush
    // s78n t18 (MOVEMENT §5.3.2, FIGHTER_FLYING §7.8 item 1): an object of tracking layer 0x10 - a registered mover on the
    // Land layer that is not hidden: squad containers yes, their members never. Every collision candidate, crowd count,
    // occupancy and dynamic-repath query reads this.
    bool land_layer_object(const Unit& u) const;
    // The footprint rectangle 0x38de80 (MOVEMENT §5.3.1): half extents 0x396850 (a squad container: its joint slot box;
    // s78n t23: the deployed pair while deploying / deployed when the deployed X is set; else the hard extents, [port] a
    // circle of the soft radius when the type has none: FootRect::round), its bounding radius 0x3969f0 and the rectangle
    // at the unit's position and yaw ([port] no hard-extent offsets).
    bool deployed_extents(const Unit& u) const {  // the switch of 0x396850 / 0x396b70: Walk state 0x18 or 0x19
        return u.deploy == LocoState::WalkDeploying || u.deploy == LocoState::WalkDeployed;
    }
    V2 foot_half(const Unit& u) const;
    bool foot_boxed(const Unit& u) const;  // a rectangle (else the [port] circle of the soft radius)
    float foot_radius(const Unit& u) const;
    FootRect foot_of(const Unit& u) const;
    FootRect foot_at(const Unit& u, V2 p) const;  // the same rectangle placed at `p`
    float tree_half(const Unit& u) const;  // the collision tree's half box: covers the footprint and the soft radius
    // Box query 0x5d7a00 on layer 0x10: the layer objects whose record box (centre +- its footprint radius) overlaps the
    // box centre +- half (strict), ascending id (units_ index).
    void layer_box_query(V2 centre, float half, std::vector<uint32_t>& out) const;
    void team_box_refresh(Unit& k);  // the joint box of one container from its team entries
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
    const Unit* unit_ptr(ObjectId id) const;
    std::vector<uint32_t> order_indices(const MoveOrder& order) const;  // the order's units_ indices, deduplicated
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
    // s78f t02: fighter formations (fighter_formation.cpp)
    ObjectId fkey(const Unit& u) const { return u.fs.squad >= 0 ? u.fs.squad : u.d.id; }
    FighterFormation* fform_ptr(ObjectId key);
    std::vector<ObjectId> fform_members(ObjectId key);  // fighter units of the formation (team index order)
    bool fform_block_ok(const FighterBlock& b);          // D 1: no fighter inside a squadron; a kind-3 block has an object
    bool fobject_pos(ObjectId id, F3& p);                // a block object's position (mover, squadron box centre, hook)
    bool fblock_goal(const FighterBlock& b, V2& out);    // goal point 0x583680 / 0x583150 (clamped to the playable box)
    bool fform_hidden(ObjectId key, ObjectId object);    // FOW_Object_Hidden for the lead's owner, force 1
    float fform_unit_speed(ObjectId key);                // 0x41b2a0 (min member max speed)
    void fform_waypoint(const MoveOrder& order, std::vector<uint32_t>& idx);  // removes the appended fighters from idx
    // cu8j t01: `serial` != 0 - the order composes at the next update, which sets the first check (frame + FPS + r)
    void fform_order(const MoveOrder& order, const std::vector<uint32_t>& idx, uint32_t frame, uint32_t serial);
    F3 fform_centre(const std::vector<ObjectId>& members);  // L (G 3 / I 9)
    bool fobject_moving(ObjectId id);                       // "G moving" (a current path, not complete)
    FighterCell fobject_cell(ObjectId id);                  // the object's dogfight cell (pack +0x1c4 / +0x1c8)
    float favoid_speed(Unit& u);                            // avoid_speed 0x5ca430
    float fformation_speed(Unit& u);                        // formation_speed_or_max 0x5ca390
    void fform_append(FighterFormation& f, FighterBlock b);  // 0x420f70
    void fform_pop(FighterFormation& f);                     // 0x4105a0
    void fform_stop(std::span<const ObjectId> ids);
    void fform_removed(ObjectId id);
    void fform_lost(FighterFormation& f, size_t block);
    void fform_tick(uint32_t frame, SimRng& rng);
    void fform_plan(FighterFormation& f, uint32_t frame, SimRng& rng);
    void fform_repath(FighterFormation& f, V2 dest, uint32_t frame, SimRng& rng, bool keep_in_range);
    void fform_update(FighterFormation& f, uint32_t frame, SimRng& rng);   // 0x4192b0
    void fform_recheck(FighterFormation& f, uint32_t frame, SimRng& rng);  // 0x410a60
    bool fform_leash(FighterFormation& f);                                  // 0x4165f0
    bool fform_can_hit(FighterFormation& f);                                // 0x40edd0
    float fform_speed_read(Unit& u, bool need_path);  // 0x5ca390 / 0x5ca430: F +0x21c under a guard top, else -1
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
    // s78n t23 step 0 (t21 review item 7): an object without a locomotor (a death-clone obstacle) neither crushes nor is
    // crushed (a crusher's push kills it instead, §5.4.5 item 2).
    bool crushes(const Unit& c, const Unit& v) const {
        return c.d.crusher && !c.d.no_locomotor && !v.d.no_locomotor && hostile(c.d.owner, v.d.owner) &&
               ((v.d.squashable_by_supercrusher && c.d.supercrusher) || v.d.squashable);
    }
    void tick_space(uint32_t frame, const ParallelFor& pf);
    void plan_land(uint32_t frame, const ParallelFor& pf);
    // S7 collision layer (TACTICAL_GAPS §6.7)
    void detect_collisions(uint32_t frame);
    void drive_collisions(uint32_t frame, SimRng& rng);
    void land_candidates(size_t ai, std::vector<uint32_t>& out) const;
    bool mvm_contact(const Unit& a, const Unit& b) const;
    float mvs_contact(const Unit& a, const Unit& b) const;
    float mvs_look_distance(const Unit& a, const Unit& b) const;
    void resolve_collision(CollisionObject& c, bool full, uint32_t frame, SimRng& rng);
    // s78n t14 part B: flee = the path is crush-flee (kind 7); s78n t18: `frame` stamps the blocker's path change; s78n
    // t20: `c` = the record that owns the pushed path
    bool push_aside(const CollisionObject& c, const Unit& a, Unit& b, bool flee, uint32_t frame);
    // s78n t18 (MOVEMENT §5.4.1 / §5.4.2; movement_system.cpp)
    static constexpr uint32_t kNoDetour = 0xFFFFFFFFu;
    bool collision_valid(const CollisionObject& c) const;
    // s78n t23 (vf +0x40): the speed a record answers participant a (else b) - a MovingVsMoving record in state 0
    // answers max speed (kInf) whatever it stores
    static float record_answer(const CollisionObject& c, bool for_a);
    // s78n t20 (MOVEMENT §5.2 "What Moving is", vf +0x1a8 / +0x1b0): not suspended and (speed > 0, or a path record on
    // its stack - a collision-owned one or a planned order path - or turning in place). No order test: an order-less
    // unit on a pushed path is Moving. [port] an order whose path is not planned yet (start delay, composition pending)
    // has no path record: not Moving.
    bool unit_moving(const Unit& u) const;
    // s78n t20 (MOVEMENT §1.5.1 item 5, §5.4.3): the collision-owned record on top of the path stack.
    //   claim (vf +0x178 = 0x42cdd0 with the test 0x42c7d0): accepted when the unit has no owner, the same owner, or an
    //     owner of lower weight; `force` (the push, Path Around) releases any owner. A claim by a new owner removes the
    //     unit's collision-owned record (0x559410) and stamps its path change.
    //   push_top (vf +0x190 -> 0x554310): the path becomes the top record, started, owned by `c`.
    //   pop_top (the Walk step 0x620820): the record is dropped; `walked` (its last waypoint was passed): the order path
    //     below resumes past every waypoint line the unit has already crossed, and true is returned when that leaves no
    //     waypoint (the unit has arrived where it stands).
    bool claim(Unit& u, const CollisionObject& c, bool force, uint32_t frame);
    void push_top(Unit& u, std::vector<V2> path, uint8_t kind, bool flee, const CollisionObject& c, uint32_t frame);
    // s78n t23 (MOVEMENT §5.4.1 "The stamp in full" items 2 - 4, 0x50a020 -> 0x5c7cb0 -> vf +0x78 0x680ef0): a path lands
    // on u - every MovingVsMoving record of u in which u is the held unit (state 1 with u = M, state 4 with u = S) is
    // stamped NOW + 2 (overwritten by a later stamp).
    void stamp_held(const Unit& u, uint32_t frame);
    bool pop_top(Unit& u, bool walked, uint32_t frame);
    bool holds_path(const Unit& u, const CollisionObject& c) const { return u.pushed && u.push_owner == c.seq; }  // vf +0x1c0(id)
    // [port] the path stack as one polyline (stands for "the approximation runs along the path stack"): the top record's
    // remaining points, then the order path from the waypoint it resumes at. pts[next..] remain to be walked.
    struct StackPoly {
        std::span<const V2> pts;
        uint32_t next = 0;
    };
    StackPoly stack_poly(const Unit& u, std::vector<V2>& scratch) const;
    void stack_points(const Unit& u, std::vector<V2>& out) const;  // the unit's position, then the remaining points
    V2 type_half(const Unit& u) const;         // 0x371be0: the type's half extents (UnitDesc::type_hx / type_hy)
    float half_width(const Unit& u) const;     // 0x396b70: the half width across (s78n t23: the deployed Y switch)
    float walk_max_speed(const Unit& u) const;
    V2 wanted_dir(const Unit& u) const;
    float record_cap(const Unit& u) const;
    bool mvs_behind(const Unit& a, const Unit& b) const;
    bool mvm_passed(const Unit& a, const Unit& b) const;
    float slave_speed(const Unit& x, const Unit& y, bool& touch) const;  // 0x67f590: the lane test, then 0x67f130
    float closing_speed(const Unit& x, const Unit& y, bool& touch) const;  // 0x67f130 (touch: the footprints touch)
    // s78n t20 (MOVEMENT §5.4.6 "State 1 slave speed 0x5c69f0"): the formation-follow speed of a unit `s` walking the
    // detour of record `c` - max speed (kInf: no cap), lowered to min(max speed, Vf x L / D) only while s holds c's path,
    // is in a formation of speed Vf > 0 and the path has >= 2 points (L = its length, D = the straight distance from its
    // first to its last point): the formation speed raised by the detour's extra length.
    float follow_speed(const Unit& s, const CollisionObject& c) const;
    // s78n t20 (MOVEMENT §5.4.1 "The impassable-cell rule in full" items 1 - 3, 0x684450): the unit has a locomotor, its
    // top path is not forced and its position fails 0x50e280 for its own class. "Forced" (path +0x5c; no path record:
    // false) = the flag-0x20 order path on top, or [port] the planner's escape leg off a blocked start (the order path's
    // first leg while next < escape_until) and an order still waiting for its plan.
    bool on_blocked_cell(const Unit& u) const;
    // vf +0x140(record id) = 0x5ad320 for Walk and the land team container: the whole path stack is cleared (order /
    // formation path included); the order ends as arrived ([port] the formation's signal 10: the member counts as done).
    // When the path owner is none or `c` (vf +0x170): state 0x16 and speed 0 at once; else brake_to_stop.
    void collision_stop(Unit& u, const CollisionObject& c, uint32_t frame);
    // End Move with no path record left (MOVEMENT §4.3.2 item 6, §5.4.1 impassable-cell item 3, §5.4.4 item 5): a squad
    // container coasts (team_walk's End Move); s78n t26.4 (MOVEMENT §3.5.2 C.1 / C.4): a Walk unit keeps its speed and
    // state - its walk finds no started path, goes to End Move at T = a and coasts along its last frame's displacement
    // to 0x15 (replaces the t20 [port] braking stub record); without a speed or a deceleration it stops at once.
    void brake_to_stop(Unit& u, uint32_t frame);
    // s78n t24 (MOVEMENT §5.4.7): TerrainCollision. terrain_report = the failed-step report 0x509090 (the commit of the land
    // tick, serial, ascending index): speed 0 and a new one-participant record; terrain_service = its decision 0x687ff0
    // (quota pass, state None) and service 0x687ea0.
    void terrain_report(Unit& u, uint32_t frame);
    void terrain_service(CollisionObject& c, bool full, uint32_t frame);
    bool near_path_end(const Unit& a, const Unit& b) const;
    bool destination_blocked(const Unit& a, const Unit& b) const;
    bool side_step_ok(const Unit& a, const Unit& b, const CollisionObject& self) const;
    // s78n t20 (MOVEMENT §5.4.4): the early-arrival cut 0x685e10(a, b, v); `have_v` / `v` = the spare point of 0x5c6ba0.
    // True: b is at or ahead of the stop point (the record resolves with the "at once" flag). False: nothing changed, or
    // the path was cut and b lies behind the stop point (the decision goes on with the shortened path).
    bool early_arrival(Unit& a, const Unit& b, bool have_v, V2 v, uint32_t frame);
    // s78n t20 batch 1 (MOVEMENT §5.4.4 item 5): 0x684100 - the order path (kind-1 record; false: none) or the top record
    // cut by c with 0x54e780 (cut_points), the index untouched; cut_finished then ends a record left on its last waypoint
    bool cut_order_path(Unit& u, float c, uint32_t frame);
    void cut_top_path(Unit& u, float c, uint32_t frame);
    void cut_finished(Unit& u, uint32_t frame);
    // s78n t20 (MOVEMENT §1.5.1): the Path Around call 0x5c80e0(record, u, o, flag, kind, flags) - a visibility-graph
    // detour from u (a point) to a rejoin point on its own current path, pushed as a record on top of its path stack
    // (kind 4; 7 = a crush-flee path). `o` = the obstacle of the record (null: none), forced into the obstacle set when
    // `flag`; `flags` bit 1 / 2 / 4 as in item 1. False: nothing changed (stack, owner, state).
    bool path_around(const CollisionObject& c, Unit& u, const Unit* o, bool flag, uint8_t kind, uint8_t flags, uint32_t frame);
    bool mvs_path_around(const CollisionObject& c, Unit& a, const Unit& b, uint8_t p, uint32_t frame);  // 0x685920: flags p, then p | 4
    // 0x5c6ba0: the first accepted rejoin candidate with D = |a - b| (3-D) + r_a + r_b; no search, no complexity.
    bool spare_point(const Unit& a, const Unit& b, V2& v) const;
    // 0x676c10: the base path of a Path Around / spare point call - the unit's top path record from its current waypoint
    // (`cur`: the start of the segment it walks); a collision-owned top record gets one point 1 unit past its end along
    // the order path's current segment. False: no order path, or no segment left.
    bool around_base(const Unit& u, std::vector<V2>& scratch, std::span<const V2>& base, uint32_t& cur) const;
    // 0x67ad20 / 0x679aa0 / 0x675ed0 (s78n t24, MOVEMENT §5.4.7 "Rejoin candidates of a kind-3 Path Around"): the
    // resumable goal generator - (a) every base path point from progress + D + 2 in steps of 10 to the path end, (b)
    // steps of 10 past the end (a failed path-length test ends (b)), (c) the ring around the end (occupancy and the
    // path-length test only); (a) / (b) candidates pass 0x679aa0. `fn` gets each accepted goal (at most 11) with the
    // order path index to resume at and returns kRejoinNext (the next goal), kRejoinTaken or kRejoinFail (the call
    // fails). True: taken.
    struct Rejoin {
        V2 q;
        uint32_t resume = 0;
        bool on_path = false;
    };
    static constexpr int kRejoinNext = 0, kRejoinTaken = 1, kRejoinFail = -1;
    bool rejoin_search(const Unit& u, std::span<const V2> base, uint32_t cur, float d, const std::function<int(const Rejoin&)>& fn) const;
    bool rejoin_accept(const Unit& u, V2 cand, V2 path_end, V2& q) const;
    // 0x67d580(request, object, 1): the goal-spot obstacle filter of 0x679aa0 (relation mask 0x10)
    bool goal_obstacle(const Unit& u, const Unit& o) const;
    // 0x67d580 + 0x677650: the obstacle set of a Path Around call among the layer objects around u, ascending id, with
    // their swept boxes; `ids` = their units_ indices.
    void around_obstacles(const Unit& u, const Unit* forced, uint8_t flags, float half, std::vector<AroundObstacle>& out,
                          std::vector<uint32_t>& ids) const;
    // 0x679750(p0, p1); `own_clause` false: the "own record in state 0 / 1 / 4" clause is not taken (the chooser re-runs
    // of MovingVsMoving states 2 and 6)
    uint8_t unit_relation(const Unit& p0, const Unit& p1, bool own_clause = true) const;
    FootRect unit_swept_box(const Unit& o, const Unit& u, uint8_t rel) const;  // 0x677650 of o for the pathing unit u
    bool shares_open_record(const Unit& u, const Unit& o) const;        // an undecided / detouring record of the pair (vf +0x58)
    void mvs_decide(CollisionObject& c, Unit& a, Unit& b, uint32_t frame);
    void mvs_service(CollisionObject& c, Unit& a, Unit& b, bool full, uint32_t frame);
    // s78n t23 (MOVEMENT §5.4.2 / §5.4.6): MovingVsMoving for every pair. The chooser 0x67f7b0 gives the state, the
    // master (0 = a, 1 = b), the set-up mode (0 none, 1 Pass 0x682e80, 2 Path Around 0x683140) and swap_ok; `rerun`: the
    // re-runs of states 2 / 6 (the relation code's own-record clause is false).
    struct MvmChoice {
        CollisionState state = CollisionState::Active;
        uint8_t master = 0;
        uint8_t mode = 0;
        bool swap_ok = true;
    };
    MvmChoice mvm_choose(const CollisionObject& c, const Unit& p0, const Unit& p1, bool rerun) const;
    void mvm_service(CollisionObject& c, Unit& a, Unit& b, bool full, uint32_t frame, SimRng& rng);
    bool mvm_decide(CollisionObject& c, Unit& a, Unit& b, uint32_t frame, SimRng& rng);  // false: no service this frame
    bool mvm_valid(CollisionObject& c, Unit& a, Unit& b, uint32_t frame);              // validity 0x680650
    bool mvm_overtake(const Unit& a, const Unit& b) const;                              // 0x683260(rear a, front b)
    void mvm_fail(CollisionObject& c, uint32_t frame, SimRng& rng);                     // 0x680c00
    bool mvm_pass_setup(CollisionObject& c, Unit& m, const Unit& s, uint32_t frame);  // 0x682e80
    bool mvm_detour(CollisionObject& c, Unit& m, Unit& s, bool flag, uint32_t frame); // 0x683140, one attempt
    bool mvm_early_pop(CollisionObject& c, const Unit& m, Unit& s, uint32_t frame);    // 0x6807e0
    bool mvm_starve(CollisionObject& c, const Unit& m, const Unit& s) const;           // 0x681d30 (true: starved)
    void mvm_swap_slots(Unit& f, Unit& b, uint32_t frame);                             // 0x554660 (state 13)
    V2 mvm_facing(const Unit& u) const;  // the facing, or the wanted facing while turning in place (vf +0x1f8)
    // the last hold tag of u's top path naming `obstacle` that u has not passed yet (index >= its waypoint index)
    const HoldTag* pending_tag(const Unit& u, ObjectId obstacle) const;
    float accel_per_frame(const Unit& u) const;  // 0x372fe0 (<= 0: 0.05)
    // s78n t14 part B (0x5c8890): b's owner's faction has Scatters_From_Crushers and b's SPREAD_OUT is off.
    bool scatters(const Unit& b) const { return (!scatters_ || scatters_(b.d.owner)) && !b.spread; }
    // s78n t18 (MOVEMENT §5.3): the garrison-host gate of all three contact tests - either one's garrison host (walking
    // in, entering, or just left) is the other.
    bool pair_gated(const Unit& a, const Unit& b) const {
        return a.gar_host == b.d.id || b.gar_host == a.d.id || a.exit_host == b.d.id || b.exit_host == a.d.id;
    }
    static bool raised(const Unit& u) { return u.hover == 1; }  // the hover gate's "raised" (rising / lowering are not)
    bool svs_overlap(const Unit& a, const Unit& b) const;  // StoppedVsStopped contact 0x686500 (gate + coefficient)
    // The free-position search 0x440f90 around unit b itself (A1_READINGS §40.2): the push's set-up (cone pi / 4, outside
    // radius 0.33) and StoppedVsStopped's (Infantry class pi / 1.0, others 3 pi / 4 / 0.75; MOVEMENT §5.4.3)
    bool push_spot(const Unit& b, V2 dir, float cone, float outside, V2& out) const;
    // s78n t20 (MOVEMENT §5.4.3): StoppedVsStopped 0x687570 / 0x687720 - the service, who moves 0x687010 (0 = a, 1 = b, 2 =
    // nobody), the cost 0x686b20 and the escape direction 0x5c74b0
    void svs_service(CollisionObject& c, Unit& a, Unit& b, uint32_t frame);
    uint8_t svs_pick_mover(const CollisionObject& c, const Unit& p0, const Unit& p1) const;
    float svs_cost(const Unit& x, const Unit& y, V2 d) const;
    V2 svs_escape_dir(const Unit& x, const Unit& o) const;
    static void end_push(Unit& u);
    void update_tree(const Unit& u);
    V2 pick_land_slot(const Unit& u, V2 slot, std::vector<uint64_t>& taken) const;
    // s78n t20 (MOVEMENT §4.3.1 items 2 - 4, 0x439360 / 0x43d6a0): the destination check of a land move order, run once
    // at the order ([port] the split and the layout are made at the order, with the positions of the order frame; retail
    // runs it at the coordinator update that follows). One formation of the order: its members (indices into the
    // order's unit list, seed first), centroid, speed, layout forward axis and slots, and the centre its slots are laid
    // around - the ordered destination until the check moves it.
    struct LandForm {
        std::vector<uint32_t> members;
        V2 centroid;
        float speed = 0.0f;
        V2 fwd;
        std::vector<Slot> slots;
        V2 centre;
        bool checked = false;  // s78n t20 F5: the destination check accepted `centre` (its slots take no cell snap)
        // s78n t20 batch 2 (MOVEMENT §4.2 step 2): `centre` is the seed's zone-validated point V != the ordered point -
        // a fresh block carrying only the order's flags (no formation facing, no final facing)
        bool fresh = false;
    };
    void destination_check(const MoveOrder& order, const std::vector<uint32_t>& idx, std::vector<LandForm>& forms, V2 group_c);
    // s78n t20 batch 2: `forced` = the forced pass after n = 40 (no try accepted: the last try that reached the object
    // test, or Dn, is taken and true returned)
    bool place_formation(const std::vector<uint32_t>& idx, const LandForm& f, V2 dn, V2 v, V2 axis, bool first, bool several,
                         bool forced, std::vector<FootRect>& taken, V2& centre) const;
    void move_land(const MoveOrder& order, const std::vector<uint32_t>& idx, uint32_t frame,
                   const std::vector<uint32_t>& delays, uint32_t serial, SimRng& rng);
    void move_space(const MoveOrder& order, const std::vector<uint32_t>& idx, uint32_t frame,
                    const std::vector<uint32_t>& delays, uint32_t serial);
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
    ScattersFn scatters_;            // s78n t14 part B: Scatters_From_Crushers by owner (host; not saved)
    LockedFn locked_;                // cu8l t03: the space composition's seed lock (host)
    ModelBoundFn model_bound_;       // s78n t10: the space composition's world boxes (host; not saved)
    std::vector<CrushEvent> crush_;  // OI tactical: this tick
    std::vector<ObjectId> push_kills_;  // s78n t20 batch 1: this tick (saved, hashed while any)
    std::vector<ObjectId> deploy_pose_;  // s78n t26.1 review: this tick (not saved, not hashed)
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
    std::vector<FighterFormation> fforms_;  // s78f t02: ascending key
    bool fform_internal_ = false;           // a plan / re-path of the formation tick (keeps the formation)
    std::vector<PendingComposition> pending_;  // cu8j t01: creation order (saved, version 13)
    uint32_t pend_serial_ = 0;                 // cu8j t01: the last PendingComposition serial (saved)
    // s78n review: steering members parked while outside the system (garrison), ascending member id (saved, version 14;
    // hashed while any exists) - LandTeamSlot reused: (container, team index, member, slot).
    std::vector<LandTeamSlot> parked_;
    // s78n t26.1 review: the Walk deploy state of units inside a garrison (garrison_remove), ascending id
    struct DeployKeep {
        ObjectId id = 0;
        LocoState deploy = LocoState::None;
        bool walk_deployed = false;
        int32_t dclip = -1;
        float dclip_pos = 0.0f;
    };
    std::vector<DeployKeep> deploy_kept_;
    // s78n review: the steering members by (container, team index, member) - derived from units_ (team_ix_rebuild).
    std::vector<LandTeamSlot> team_ix_;
    // s78n t18: every TEAM entry (steering, suspended, absent) by (container, team index, member) - derived from units_
    // and parked_ (team_ix_rebuild).
    std::vector<LandTeamSlot> team_all_;
    void team_ix_rebuild();
    bool remove_impl(ObjectId id);  // remove_unit without dropping a parked record
    void attach_member(Unit& u, ObjectId container, V2 slot, uint32_t tindex);  // set_land_team's per-member part
    void park_record(const Unit& u);  // park_team_member / suspend_team_member: the (member, container, slot, index) record
    bool deferred_compose_ = false;           // cu8j t01: the host runs coordinator_update (not saved: host setup)
    uint32_t form_serial_ = 0;                // s78n t20: the last Unit::form_id given (saved, version 18)
    FighterFormationHooks fhooks_;          // s78f t02 (not saved: the loader re-sets them)
    FlyingHooks fly_hooks_;  // FIGHTER_FLYING §6
    // S8 t11
    DeployClipFn deploy_clip_;
    TurretRestFn turret_rest_;
    TurretAimFn turret_aim_;
    std::vector<DeployEvent> devents_;  // since the last take_deploy_events()
    std::vector<std::pair<ObjectId, ObjectId>> exit_arrivals_;  // s78n t26.1: since the last take_exit_arrivals()
    std::vector<TurnClipEvent> tevents_;  // s78l t02: since the last take_turn_clip_events()
};

}  // namespace alamo::tactical::movement
