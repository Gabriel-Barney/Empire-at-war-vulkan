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
    LocomotorKind locomotor = LocomotorKind::Default;
    LocoState loco_state = LocoState::None;  // special kinds only
    int32_t anim = kAnimNone;                // requested animation (special kinds)
    bool airborne = false;
    float roll = 0.0f;                       // bike lean / flying / fighter roll (degrees, wrap180) - attitude()
    float pitch = 0.0f;                      // flying / fighter pitch (degrees, wrap180; positive = nose down) - attitude()
    FighterPhase fighter_phase = FighterPhase::None;  // fighter: last combat steering phase (diagnostics)
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
};

class MovementSystem {
public:
    explicit MovementSystem(MovementConfig cfg = {});

    MoveMode mode() const { return cfg_.mode; }
    const MovementConstants& constants() const { return cfg_.constants; }
    const MovementClassTable& classes() const { return cfg_.classes; }

    // ---- land world
    void set_grid(PassabilityGrid grid);
    PassabilityGrid& grid() { return grid_; }  // stamping / unstamping; regions rebuild next tick
    const PassabilityGrid& grid() const { return grid_; }
    const RegionMap& regions() const { return regions_; }
    void rebuild_regions_if_dirty();
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

    // ---- REMAINING_GAPS §4.2-4.7
    // Threat at a point for an owner (TacticalPerceptionGrid point query 0x653ff0); const and thread-safe (planning
    // runs in parallel). Unset: threat factor 1.
    using OwnerThreatFn = std::function<float(int32_t owner, V2 p)>;
    void set_threat_fn(OwnerThreatFn fn) { threat_ = std::move(fn); }
    void set_anim_length_fn(AnimLengthFn fn) { anim_len_ = std::move(fn); }
    // Land air units: the planning box (set_box = the playable box) and the map floor (0x136650; default none).
    SpaceOccupancy& air() { return air_; }
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
    //   speed factor / override / lag, jiggle, threat, special motion + params, fighter state + params), the collision
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
        // FIGHTER_FLYING.md §2-§5 (space fighters)
        FighterState fs;
        FighterParams fp;
    };
    bool is_air(const Unit& u) const;           // land Jetpack / Flying / LandBomber
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
};

}  // namespace alamo::tactical::movement
