// A1 stage 2 - the tactical AI mode (land and space) on the A2 framework (docs/re/AI_TACTICAL.md;
// framework AI_GALACTIC.md §1). Library alamo_ai_tactical_world (needs engine/lua: alamo_ai_lua).
//
// TacticalAiWorld owns, for one battle: the AI players' per-mode contexts (ai_context.hpp) whose
// GoalModeHooks / ContextModeServices are implemented here, the shared tactical perception (threat grid,
// evaluator handles, leaf cache: tactical_perception.hpp), each player's target slots, free store and
// free-store script, the plan runtime (pooled Lua plan scripts of the mode's plan folder), task forces and
// blocking statuses, and the Lua hosts of both AI binding families (lua::AiScriptHost = A2's plan-generic
// family, lua::AiTacticalScriptHost = the tactical family).
//
// HOST CONTRACT (TacticalAiHost, implemented by the battle host - engine/apps/sim battle_ai.cpp)
//   * update(now, mode_frame) once per frame after the mode tick (the original's T7 PlayerList update; the
//     battle host calls it at the end of its T5 battle hook, T6 is a stub [port]). It first refreshes the
//     PerceptionWorld snapshot through TacticalAiHost::fill (objects in GOM order), then derives the object
//     signals by comparing with the previous snapshot [port: the original raises them at the GOM add hook /
//     destroyed / owner-changed signals; here they are applied at the next AI frame, in GOM order]:
//       added    -> threat grid register (UNIT_AI objects), then per initialised AI player (index order):
//                   unit target appended (target_list.hpp rule) + free-store admission + On_Unit_Added;
//       removed  -> threat grid / perception release, unit targets tombstoned, free store removal;
//       owner    -> threat grid re-owner, free-store re-check;
//     then UNIT_AI service (threat grid service_all), then every AI player's context (player index order).
//   * AI orders are EventData executed DIRECTLY through the world's own CommandPipeline (execute_direct,
//     source AI) whose handler is TacticalAiHost::execute: MoveToPositionFacing (Move_To), Attack
//     (Attack_Target; ref2 = 1 + HardPointType of an ordered hard point, 0 none [port encoding]),
//     PortExtension LuaOrder AttackMove / Guard / MoveTo (Attack_Move, Guard_Target, Explore_Area: the L1
//     object-order path), Reinforce (unit = pool key), Retreat, TacticalBuild. The same pipeline is the AI
//     VMs' ObjectBridge pipeline (object orders of free-store / plan scripts).
//     refixf t06 - ONE group order per task force (MOVEMENT §6.3 [H]: a task force's Attack_Move / Move_To builds one
//     parameter block and one coordinator for all its units; AI_TACTICAL §7.3): a task-force Attack_Move / Guard_Target
//     (and their follow re-orders) is one PortExtension LuaOrder whose payload is the 24-byte LuaOrder block (object =
//     the first movable unit) followed by one u32 per further movable unit, task-force list order [port encoding; at
//     most 4096 ids]. The host moves them as one formation.
//     refixf t06 - Attack_Move on an object (TACTICAL_COMBAT §2.8): an AttackMove LuaOrder with a target object makes the
//     object the units' formation attack goal (path B: the host locks it as their target once it is in weapon range).
//     The task-force order (not its follow re-orders) sets payload arg bit kTfAttackMoveIssueAttack: the host also
//     issues Order_Attack (ordered 1) to each unit at once (path A); a unit that refuses (non-enemy, stealthed, garrisoned
//     or dead target) is dropped from the move, and when no unit is left the order is rejected (InvalidTarget). An
//     object's own Lua Attack_Move (arg 0) has path B only.
//     refixf t06 - space TF.Reinforce (AI_TACTICAL §7.4): the drop point is searched in rings around the target, one
//     attempt per Lua poll of the block status (BlockOnCommand -> status IsFinished), one unit per poll; the host answers
//     space_drop_frame / space_prevention_hit / space_drop_ok (below) and executes the Reinforce event at the point found.
//   * Everything runs on the sim thread; the only RNG is the session's synced stream (§10 order).
//
// TARGET IDENTITY [port]: each AI player's target list is kept as append-only SLOTS with tombstones; the live
// slots in slot order are exactly the original's erase-in-place vector (appends at the end, removals keep
// order), and a slot index stays valid for the battle (goal targets, AITarget wrappers). AITargetLocation
// ids = (player << 20) | slot.
//
// A2 tactical: slot 10 = Find_Plan_For_Goal (one synced draw) + the tactical PotentialPlan solve (tactical_pp.hpp:
// the land / space marshaller lists on run_pp_solver; a failed solve rejects the candidate). Reservations follow
// A1_READINGS §28.6 (a1g t01): the proposal's solve ignores free-store reservations and reserves nothing; at accept in
// Resolve_Category the PP is re-solved honouring them, then its free-store units (entry +0x30), pool entries and pads
// are reserved; every Commit suspends the +0x30 entries and kept goals re-assert them; a plan whose unit another PP
// reserved meanwhile is invalid; a unit a task force releases leaves its PP. The task force's Produce_Force gathers the
// selection (free units join, pool entries become pending reinforcements, pads become Build reservations; nothing is
// bought: AI_GALACTIC_3 §1.11).
// FIRST-CUT PARTS (docs/PHASE4.md A1 open items): blocking-status
// completion rules (open item 3) are this port's: move = every live unit without a movement goal (A2 tactical: an
// Attack_Move to an object completes when the object dies, units re-ordered only when it moved), attack =
// target dead, guard = never (fails when the guarded object dies), land reinforce / build = at once (space reinforce:
// AI_TACTICAL §7.4 ring search, refixf t06); land slot 9
// reachability source = the zone of the player's first live object; Has_Active_Orders / order locks are
// cleared when the unit is idle (no movement goal, no attack target).
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "alamo/ai/ai_definitions.hpp"
#include "alamo/ai/ai_plans.hpp"
#include "alamo/ai/tactical/applies.hpp"
#include "alamo/ai/tactical/damage_tracking.hpp"
#include "alamo/ai/tactical/free_store.hpp"
#include "alamo/ai/tactical/perception_world.hpp"
#include "alamo/ai/tactical/tactical_pp.hpp"  // a1g t04: TacticalPpPick (reinforce_join_pp)
#include "alamo/commands/command.hpp"
#include "alamo/commands/events.hpp"
#include "alamo/core/parallel.hpp"  // OPT2 t04: lua_gc_pf
#include "alamo/lua/players.hpp"
#include "alamo/objects/object_store.hpp"

namespace alamo {
class FileProvider;
class GameRandom;
class CrcRegistry;
class StateHasher;
class ByteWriter;
class ByteReader;
}  // namespace alamo
namespace alamo::objects {
class GameObjectTypes;
}
namespace alamo::galactic {
class ContrastTable;
}
namespace alamo::lua {
class ObjectBridge;
class GlobalValueStore;  // Int-final
}  // namespace alamo::lua

namespace alamo::ai {
class EquationSet;
class AiContext;
}  // namespace alamo::ai

namespace alamo::ai::tactical {

// Per-object facts the AI needs beyond PerceptionObject (same index as PerceptionWorld::objects).
struct TacticalAiObject {
    objects::ObjectHandle handle;
    TargetObjectInfo target;              // id, dead flags, evaluator flags (target_list.hpp)
    UnitAppliesInfo applies;              // behaviours / fog flags; has_owner + owner_allied are set per AI player
    FreeStoreObjectInfo free_store;       // admission facts; owned_by_player is set per AI player
    std::vector<int32_t> team_members;    // TEAM container: member ids (FindDeadlyEnemy)
    bool moving = false;                  // has a movement goal
    int32_t attack_target = -1;           // object id it is attacking (ordered or automatic), -1 none
    bool damage_tracking = false;         // DAMAGE_TRACKING (FindDeadlyEnemy radius form)
    // UnitAI data-pack re-projection stamp (+0x20, A1_READINGS §14): UnitAiStamps::reprojection_check drawn by the host
    // when the UNIT_AI object was created (draw_unit_ai_stamps). kNoReprojectionStamp: the host does not carry it, the
    // grid uses its [port] legacy registration + 300.
    static constexpr int32_t kNoReprojectionStamp = INT32_MIN;
    int32_t reprojection_stamp = kNoReprojectionStamp;
};

// One entry of a player's reinforcement pool (campaign: units in orbit = the battle's reserves; skirmish: the
// tactical reinforcement pool).
struct TacticalReinforcement {
    int32_t key = -1;          // pool key (the Reinforce event's "unit" field)
    int32_t type = -1;         // PerceptionWorld type index
    float population = 0.0f;   // population cost (0x373500)
    uint32_t units = 1;        // objects it spawns (team members)
    // --- A2 tactical: the Reinforcement marshaller's entry facts (AI_GALACTIC_3 §1.3) ---
    int32_t member_type = -1;          // first Company_Units member type (-1: the type has no Company_Units)
    bool member_create_team = false;   // that member type has Create_Team
    int32_t count = 1;                 // count(E): selections the entry serves
    // --- end A2 tactical ---
};

enum class PadCheck : uint8_t { Ok = 0, NotFriendlyOrVisible = 1, Blocked = 2, RebuildCooldown = 3 };

// refixf t06: payload arg bit of a task-force AttackMove LuaOrder: issue Order_Attack per unit (HOST CONTRACT above).
inline constexpr uint32_t kTfAttackMoveIssueAttack = 1u;

// refixf t06 (AI_TACTICAL §7.4 space): what the space reinforcement ring search needs from the mode.
struct SpaceDropFrame {
    float entry_facing_deg = 0.0f;  // the player's reinforcement entry facing (mode +0x288 record, field +8), degrees
    bool has_box = false;           // the playable box (mode vf 0x208): ring points are clamped into it
    float min_x = 0.0f, min_y = 0.0f, max_x = 0.0f, max_y = 0.0f;
};

// A hard point acquired an opportunity target (STRATEGIC_GAPS §5.3: signal 0x21 OpportunityTargetAcquiredDataClass, sent
// to the hard point's owner, or to the team object for a team member). Object ids; hardpoint_type = HardPointType value.
struct OpportunityTargetEvent {
    int32_t owner = -1;           // the hard point's owner object
    int32_t target = -1;          // the acquired target object
    int32_t hardpoint_type = -1;  // HardPointType of the hard point (5..10 for weapons)
};

// a1j t01 - signal 0x19 "reinforcements unloaded" (ReinforcementsUnloadedDataClass, A1_READINGS §32.1 / §36): a land
// landing object finished its unload (0x6438a0) or was removed while unloading (detach vf15 0x6442d0). `key` = the
// Reinforce event's unit field (the pool entry); `units` = the unloaded object ids in spawn order.
struct ReinforcementsUnloaded {
    int32_t player = -1;
    int32_t key = -1;
    std::vector<int32_t> units;
};

// --- A2 tactical: a build pad the player can build on (the TacticalBuild marshaller's pool, AI_TACTICAL §5.1
// side table 0x4d29a0): visible, empty, friendly or neutral [M], and the UC_ types its pad type lists for the
// player's faction (Tactical_Buildable_Objects_Multiplayer in skirmish, _Campaign otherwise). ---
struct TacticalPad {
    int32_t id = -1;
    Vec3f position;
    std::vector<int32_t> accepts;  // PerceptionWorld type indices
};
// --- end A2 tactical ---

class TacticalAiHost {
public:
    virtual ~TacticalAiHost() = default;
    // The snapshot of this AI frame (objects in GOM order) + the per-object facts (same order).
    virtual void fill(PerceptionWorld& world, std::vector<TacticalAiObject>& objects) = 0;
    // Direct execution of one AI order (the AI pipeline's handler).
    virtual commands::RejectReason execute(uint8_t player, const commands::EventData& ev) = 0;
    virtual std::vector<TacticalReinforcement> reinforcement_pool(int32_t player) = 0;
    virtual bool can_reinforce(int32_t player) = 0;   // 0x2b03e0
    virtual float population_cap(int32_t player) = 0;  // < 0: no cap
    virtual float population_used(int32_t player) = 0;
    virtual const DamageTrackingStore* damage_tracking() = 0;
    virtual const galactic::ContrastTable* contrast() = 0;
    // Build (§7.5): the three refusal checks for (pad, type) of `player`; type = PerceptionWorld type index.
    virtual PadCheck build_pad_check(int32_t /*player*/, int32_t /*pad*/, int32_t /*type*/) { return PadCheck::NotFriendlyOrVisible; }
    // A2 tactical: the pads `player` can build on now (ascending id) with their buildable types (TacticalBuild
    // marshaller). Default: none (battles without build pads).
    virtual std::vector<TacticalPad> build_pads(int32_t /*player*/) { return {}; }
    // A2 tactical: TacticalBuild for types without TACTICAL_UNDER_CONSTRUCTION - a builder of the player can build
    // `type` now (0x288b80 + 0x497140). Default false (no tactical production in the host: campaign battles never buy
    // units through the AI, AI_GALACTIC_3 §1.11).
    virtual bool can_build_now(int32_t /*player*/, int32_t /*type*/) { return false; }
    // A2 tactical: the free store's builder list (free store +0x120, AI_GALACTIC_3 §1.8 / §1.11 [M]) for `type`: ids of
    // the live objects of `player` whose type lists `type` (PerceptionWorld index) for the owner's faction
    // (Tactical_Buildable_Objects_Multiplayer). Order = the order Produce_Force tries them [port: ascending object id].
    // A skirmish build plan's Produce_Force issues Production {type_index, location = builder} to each in turn until the
    // host accepts one. Default: none (no tactical production in the host).
    virtual std::vector<int32_t> tactical_builders(int32_t /*player*/, int32_t /*type*/) { return {}; }
    // Opportunity-fire acquisitions since the last AI frame, in the order they happened (STRATEGIC_GAPS §5.3). A task
    // force holding the owner (or its team container) raises plan event 17 Hardpoint_Opportunity_Target_Acquired
    // (unit, target) when it subscribed that HardPointType. Default: none (the combat side does not report them).
    virtual std::vector<OpportunityTargetEvent> opportunity_targets() { return {}; }
    // MagicPlan HIJACK_UNIT, tactical with a parent galactic mode (AI_GALACTIC_3 §4.3 steps 1-5): spawn `type`
    // (PerceptionWorld type index) for `player` at the battle planet in the galactic store (steal = MagicPlanStealing or a
    // named hero: the player's first galactic object of the type elsewhere is killed silently; pending respawns of the
    // type cancelled), launch + form a fleet for a surface unit (fails when blockaded), take the fleet's first member, put
    // it in the player's galactic free store and append it to the battle's reinforcement record. Returns the new pool
    // entry (key = the Reinforce event's unit field), or an entry with key < 0 = FAILED (Production_Failed). No RNG.
    // Default: always fails (no parent galactic mode / host without the hook).
    virtual TacticalReinforcement hijack_unit(int32_t /*player*/, int32_t /*type*/, bool /*steal*/) { return {}; }
    // Active_By_Default re-apply (TACTICAL_GAPS §1.2, corrected by A1_READINGS §28.8 [H]): called with the
    // PerceptionObject id of a unit of `player`
    //   * OrderLockDone - the free store's signal 10/11 handler (AIFreeStoreClass vf1 0x4d4aa0) for a unit a script
    //     locked with Lock_Current_Orders: its order finished [port: the unit went idle]. The AI has already released
    //     the lock (0x3a8e50(obj, 0) on the script's share: orders_locked cleared); the host applies
    //     AbilitySystem::apply_active_by_default(id, frame) unconditionally. Free-store admission itself does not call.
    //   * TaskForce - a task force releases the unit (TaskForceClass vf49 0x5ff610), after it went back to the free store
    //     or was refused. The host applies Active_By_Default unless the unit's movement is locked (0x39ad40: the
    //     ability share of obj +0x3a8, a script lock, a suspended locomotor [port: HUNT active stands in for the
    //     locomotor term]). No lock is released on this path.
    // Called during the AI update. Default: nothing.
    enum class UnitRelease : uint8_t { OrderLockDone, TaskForce };
    virtual void unit_released(int32_t /*player*/, int32_t /*object_id*/, UnitRelease /*path*/) {}
    // refixf t06 - space TF.Reinforce ring search (AI_TACTICAL §7.4). space_drop_frame: the player's entry facing and the
    // playable box (false: facing 0, no box). space_prevention_hit (0x4d8d10): `p` lies inside the
    // Reinforcement_Prevention_Radius (2-D, strict) of a live object whose owner is not allied with `player`.
    // space_drop_ok (SpaceMode vf80 0x4d6fd0, force 0): `p` is inside the playable box, not fogged for `player` (force 1),
    // outside every non-allied prevention radius, and the occupancy sweep of pool entry `key`'s type from
    // p - sweep x (cos, sin)(facing) to p over frames [now, now + 115] on its Space_Layer (default 4) and layer 8 is
    // clear. Defaults: no frame, no prevention, every point valid (hosts without the hooks drop at the target).
    virtual bool space_drop_frame(int32_t /*player*/, SpaceDropFrame& /*out*/) { return false; }
    virtual bool space_prevention_hit(int32_t /*player*/, const Vec3f& /*p*/) { return false; }
    virtual bool space_drop_ok(int32_t /*player*/, int32_t /*key*/, const Vec3f& /*p*/, float /*facing_deg*/, float /*sweep*/) {
        return true;
    }
    // S7 t15 A1 - "unit ability finished" (object signal 0x29: every ability end, ABILITIES_HARDPOINTS §2.6, and the
    // jetpack's swap back to the primary locomotor, MOVEMENT §3.1.1 step 6): the object ids that raised it since the last
    // AI frame, in the order they happened (an id may repeat). The AI feeds its scripts' object bridge (blocking
    // GameObjectWrapper.Activate_Ability), finishes the task-force AbilityBlockStatus counts and raises plan event 19
    // <TF>_Unit_Ability_Finished(tf, unit) for task-force members that are not dying. Default: none.
    virtual std::vector<int32_t> ability_finished() { return {}; }
    // S7 t21 - the unit's movement lock (0x39ad40: ability share, script lock, suspended locomotor; A1_READINGS §28.9):
    // a locked member does not join a SpaceAmbushBlockStatus (AI_TACTICAL §7.3.1). Default: never locked.
    virtual bool movement_locked(int32_t /*object_id*/) { return false; }
    // S7 t27 - the land sight factor 0x3722a0 of land Prepare_Ambush (AI_TACTICAL §7.3.3 item 5): the weather
    // Sight_Range_Modifier for the Weather_Category of `type` (the target's company type), for the object `object_id`
    // (its owner decides the weather exemption); 1.0 without strong weather / no entry. Default: 1.0 (no weather).
    virtual float sight_weather_factor(int32_t /*type*/, int32_t /*object_id*/) { return 1.0f; }
    // a1g t04 follow-up - land TF.Reinforce drop validity (LandMode vf80 0x3b5f60 -> 0x3b6170, TACTICAL_GAPS
    // §4.2.1, A1_READINGS §30.7): can pool entry `key` of `player` land at spot `p` (the company layout's centre)? The host
    // refuses unless every formation slot is passable for its member, not fogged for `player`, clear of blocking footprints
    // and inside the reinforcement radii (enabling / prevention). A refused spot sends the unit to the next ring spot. Default:
    // every spot valid (hosts without the hook).
    virtual bool land_drop_ok(int32_t /*player*/, int32_t /*key*/, const Vec3f& /*p*/) { return true; }
    // a1j t01 - land TF.Reinforce with a timed landing (ReinforceBlockStatus, A1_READINGS §36). reinforce_in_flight: the
    // Reinforce of pool entry `key` just executed for `player` put the company in a landing object that has not finished
    // its unload (the units come later, by reinforcements_unloaded). reinforcements_unloaded: the signal 0x19 records of
    // the tick just run, in the order they were sent. Defaults: never in flight, none (the units exist at the order).
    virtual bool reinforce_in_flight(int32_t /*player*/, int32_t /*key*/) { return false; }
    virtual std::vector<ReinforcementsUnloaded> reinforcements_unloaded() { return {}; }
};

struct TacticalAiPlayerConfig {
    int32_t player = -1;
    std::string ai_type;     // AIPlayerType ("BasicEmpire")
    int32_t difficulty = 1;  // 0 Easy, 1 Normal, 2 Hard
};

struct TacticalAiDifficulty {  // DifficultyAdjustments fields of the tactical AI (§4.5)
    std::string name;
    float land_goal_sleep = 0.0f, space_goal_sleep = 0.0f;   // *_AI_Goal_Cycle_Sleep_Duration (s)
    float land_contrast = 1.0f, space_contrast = 1.0f;       // *_AI_Contrast_Multiplier
};

struct TacticalAiConfig {
    TacticalMode mode = TacticalMode::Land;
    std::vector<TacticalAiPlayerConfig> players;
    std::vector<TacticalAiDifficulty> difficulties;
    std::vector<lua::LuaPlayerSetup> lua_players;   // players of the Lua player world (PlayerObject, ...)
    int32_t attacker = -1, defender = -1;           // conflict record (opponent preference, §1.2)
    bool campaign = false;                          // galactic parent (campaign battle)
    TacticalAiConstants constants;
    Rect2 map;                                      // playable box: the threat grid (and the region grid if no region_map)
    // Space region grid bounds (A1_READINGS §8 [H]: mode vf65 = the space CAMERA box, .ted 0x103 minis 07/08; default the
    // playable box shrunk by 400 in X and Y). Empty (all 0) = use `map`.
    Rect2 region_map;
    std::vector<HintZone> zones;                    // land hint zones
    std::vector<std::string> plan_files;            // load order (the mode's plan folder)
    bool log_decisions = true;
    uint32_t game_seed = 0;                         // A2 tactical: session seed (perception '#' reseed, AI_GALACTIC_3 §7.3)
    // OPT2 t04: fan-out for the per-update scheduled Lua collections of this world's VMs (lua/gc_schedule.hpp); OPT3
    // t09: also for load_plans (one job per plan file). Any value gives identical results.
    ParallelFor lua_gc_pf = serial_for;
};

struct TacticalAiDecision {
    int64_t frame = 0;
    int32_t player = -1;
    std::string text;
};

class TacticalAiWorld {
public:
    TacticalAiWorld(objects::ObjectStore& store, const objects::GameObjectTypes* types, const FileProvider* files,
                    GameRandom& rng, const AiDefinitions& defs, const EquationSet& equations, TacticalAiHost& host,
                    TacticalAiConfig cfg);
    ~TacticalAiWorld();
    TacticalAiWorld(const TacticalAiWorld&) = delete;
    TacticalAiWorld& operator=(const TacticalAiWorld&) = delete;

    // The ObjectBridge attached to every AI VM (pipeline = the AI pipeline). The host fills its hooks
    // (abilities, objects_in_range, is_enemy, ...) before start().
    lua::ObjectBridge& object_bridge();
    lua::LuaPlayerWorld& player_world();
    // Int-final: the session-wide GlobalValue table (L1_S4_REMAINING §1.5) for GlobalValue.Get / Set of every AI VM;
    // null (default) = the world's own map (hashed in its CRC section). Hosted battles attach SimHost::global_values().
    void set_global_values(lua::GlobalValueStore* store);

    // Loads the plan scripts (cfg.plan_files). False when none loaded.
    bool load_plans(std::vector<std::string>* diagnostics = nullptr);
    // Creates the AI players' contexts and free-store VMs. False when no AI player could be created.
    bool start(std::vector<std::string>* diagnostics = nullptr);
    // One AI frame (see the header comment). `now` = global frame (service clocks), `mode_frame` = F.
    void update(int64_t now, uint32_t mode_frame);
    // Battle end (slot 7 / context teardown): every running plan ends (released by the goal system).
    void end_battle();
    void set_suspended(bool suspended);
    // S7 t26 (AI_TACTICAL §7.3.3 item 2) - object signal 0xb "a new order replaces this unit's order", raised by the host
    // when it executes an order on `object_id` (A1_READINGS §33.3: task-force movement collectors, unit Move_To /
    // Attack_Move, MoveToObject, attack orders, ...): the Activate_Ability blocks waiting on the unit (or its team
    // container) end - the object bridge's GameObjectWrapper blocks and the task-force AbilityBlockStatus counts. No plan
    // event. Call at the moment the order executes (a block created after it must not see it).
    void order_replaced(int32_t object_id);
    // FOW hook (docs/re/FOG_OF_WAR.md: a FOW cell of `player` reached state 0): threat-grid refog at the cell
    // centre (AI_TACTICAL_PERCEPTION §4). Fog itself is the FOW system's; perception reads it through the
    // PerceptionWorld object_fogged / point_fogged callbacks the host fills (absent = never fogged).
    void refog(int32_t player, float x, float y);

    // ---- inspection ----
    bool is_ai_player(int32_t player) const;
    int32_t opponent(int32_t player) const;
    const AiContext* context(int32_t player) const;
    size_t free_store_size(int32_t player) const;
    size_t live_targets(int32_t player) const;
    size_t running_plans() const;
    const std::vector<TacticalAiDecision>& decision_log() const;
    std::vector<std::string> lua_errors() const;
    uint64_t orders_issued() const;
    uint64_t orders_rejected() const;
    // Results left on the AI pipeline's event bus: 0 after every update().
    size_t pending_order_results() const;
    const std::map<std::string, uint32_t>& stats() const;  // port diagnostics (never hashed)
    const PlanLibrary& plans() const;
    const PerceptionWorld& world() const;
    // Threat-grid point danger for `player` at (x, y) (0x653ff0; REMAINING_GAPS §4.2 movement threat mode). Const;
    // safe between AI ticks (the battle's movement planning).
    float point_danger(int32_t player, float x, float y) const;

    void hash(StateHasher& h) const;
    void register_crc(CrcRegistry& registry, const std::string& name = "ai.tactical") const;

    // ---- N1 save / load (contract: alamo/core/save_file.hpp; the battle host's AI chunk) ----
    // write(): u32 version (3: the facts carry the six space free-store facts and the re-projection stamp); the shape (AI player pattern, plan
    // library size, plan runtime size, mode: checked against the reader's rebuilt session); now, mode frame, current plan, the plan / task-force / status /
    // PotentialPlan id counters, orders / rejected; the enable gate; the PerceptionWorld of the last update
    // (PerceptionWorld::write: types by count + digest only) and the per-object facts (handles as value + id) and the
    // previous snapshot's (id, owner) list; the shared perception (threat grid, evaluator targets, leaf cache:
    // length-prefixed); the AI's LuaPlayerWorld and ObjectBridge; the GlobalValue map; the '#' reseed name CRCs; per AI
    // player (index order) the target list + slots, AiContext, free store + schedule, free-store VM, pending desire
    // bonuses, the home-zone cache and the land path finder (cadence + node costs; matrices rebuilt and checked); the
    // plans with their Lua VMs; task forces (units as value + id); blocking statuses; PotentialPlans and the three
    // reservation maps; opportunity-fire subscriptions (non-empty lists); the plan runtime's pools (a still-pristine VM
    // by digest) and created counter; the Script_ evaluator VMs and missing names. Lua VMs are length-prefixed
    // LuaVm::write images (a VM whose state cannot be saved makes the save unreadable: LuaVm::save_error()).
    // NOT SAVED: definitions, equations, plan library, config, the land zone graph (rebuilt by the constructor), the
    // equation-name cache, host wiring (pipeline handler, callbacks, attached GlobalValue store) and the AI pipeline's
    // event bus (never read, emptied at the end of every update()), the decision log, Lua error lists and stats (diagnostics).
    // read(): into a world rebuilt like the writer's (constructor, load_plans(), start(); before or after updates) and
    // after the ObjectStore was restored (handles are resolved by id). All-or-nothing: false and unchanged on bad
    // data or another shape. Reads its own fields only (the owner checks done()).
    void write(ByteWriter& w) const;
    bool read(ByteReader& r);
    // N1 stage 2: the host calls relink() after read, once every other battle system is restored (before the next
    // update()). When the reader's PerceptionWorld had no types yet (no update since start()), relink() calls
    // TacticalAiHost::fill once on a scratch world, checks its types against the saved count / digest, takes them
    // and installs the host callbacks (fog, hard points, ...) the scratch fill set. False = the host's types differ
    // from the writer's (the load must be abandoned). Nothing to do (true) otherwise.
    bool relink();

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

// a1g t04 - land TF.Reinforce placement (0x6eaf60, A1_READINGS §30.7 [H]).
// Ranks one reinforcement point for reinforce_points: false drops it, otherwise `key` (lower first; float, as the
// original's multimap key).
using ReinforcePointRank = std::function<bool(const Vec3f& point, float& key)>;

// One allied reinforcement point in landing order.
struct ReinforcePoint {
    Vec3f position;
    float radius = 0.0f;  // the type's Reinforcement_Enabling_Radius (+0x1e20)
    float key = 0.0f;     // sort key (lower first)
};

// The candidate points of a land TF.Reinforce towards `dest` (§30.7 steps 1-3): the own / allied live objects with an
// enabling radius > 0, in snapshot (GOM = land store list) order; `rank` drops and keys them (A1_READINGS §25.3 / §30.7:
// a point whose zone is outside the destination's flood set is dropped, otherwise key = the slot-7 path cost from its zone
// (a failed path = 1e18, kept and sorted last); without a region on either side the 2-D distance). Without `rank` the
// key is the 2-D distance sqrtf(dx² + dy²) to `dest`. Sorted ascending by key, stable (equal keys keep snapshot order).
// `facts` has the same order as `w.objects`. No RNG.
std::vector<ReinforcePoint> reinforce_points(const PerceptionWorld& w, const std::vector<TacticalAiObject>& facts, int32_t player,
                                             const Vec3f& dest, const ReinforcePointRank& rank = {});

// The landing spots around one point, in try order (§30.7 step 4): θ = 0, then θ += 0.7853982f (float) while
// θ < 6.2831855f; spot = (px + (r/2)·cos θ, py + (r/2)·sin θ, pz) in float (cos / sin: dmath, identical on every
// compiler). The float accumulation reaches 6.283185 < 6.2831855 after eight steps, so the loop yields NINE spots, the
// ninth at θ ≈ 2π (≈ the first spot) [H loop bounds; the ninth spot follows from them].
std::vector<Vec3f> reinforce_ring(const ReinforcePoint& point);

// Lands the pending units on the ring spots of `points` (§30.7 steps 4-5). For each point in order, for each of its
// spots: stop when no unit is left or the remaining population `remaining` (the cap read once; +inf = no cap) is < 1;
// else `land` the LAST pending unit at the spot. On success the unit is removed from `pending` and its population taken
// off `remaining`; then trailing units that are null (`is_null`) or do not fit (population > remaining) are popped
// (null ones removed from `pending`, non-fitting ones kept there for a later Reinforce; either clears "all placed").
// A failed spot goes on to the next spot with the same unit. [port: the trailing pop also runs once before the first
// spot, so a non-fitting last unit, which the host would refuse at every spot, does not block the others.]
// Returns true only when every unit was placed and none was popped unplaced.
using ReinforceLand = std::function<bool(const TacticalReinforcement& unit, const Vec3f& spot)>;
using ReinforceIsNull = std::function<bool(const TacticalReinforcement& unit)>;
bool reinforce_place(const std::vector<ReinforcePoint>& points, std::vector<TacticalReinforcement>& pending, float remaining,
                     const ReinforceIsNull& is_null, const ReinforceLand& land);

// a1g t04 + t01 - 0x6bf3d0 (PP, unit) (A1_READINGS §28.4): a unit delivered by a land TF.Reinforce joins the plan's
// PotentialPlan unit list PP+0x108 (the FreeStore picks): it fills the first "null" slot of its type (a pick of another
// source, i.e. not a free-store unit, with the same type: it becomes a FreeStore pick of `unit`, keeping its task force /
// team) or is appended as a FreeStore pick of task force `tf`. Returns true when a null slot was filled; `filled` (if
// given) receives that slot's previous pick, whose pool / pad reservation the caller releases. The entry +0x30 = PP
// part is the caller's (the free store's reserve).
bool reinforce_join_pp(std::vector<TacticalPpPick>& picks, int32_t unit, int32_t type, int32_t tf, TacticalPpPick* filled = nullptr);

// DifficultyAdjustments.xml: the tactical fields.
std::vector<TacticalAiDifficulty> load_tactical_ai_difficulties(const FileProvider& files);

// S7 t27 (AI_TACTICAL §7.3.3 item 5): the walk step of land Prepare_Ambush (0x6ec8b0): the land sight range 0x3722a0 =
// Land_FOW_Reveal_Range x the weather Sight_Range_Modifier of the target's company type; <= 0 -> the target type's max
// attack range (0x3749d0). A result <= 0 means no walk (the first point, no threat / box test).
float land_ambush_step(float land_fow_reveal_range, float weather_sight_factor, float max_attack_range);
// The plan files of a tactical mode: every plan folder in load order, as for the galactic AI (AI_GALACTIC §1.9.1 [H]).
std::vector<std::string> tactical_plan_files(TacticalMode mode, const std::function<std::vector<std::string>(std::string_view)>& list);

}  // namespace alamo::ai::tactical
