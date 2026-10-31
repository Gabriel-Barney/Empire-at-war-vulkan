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
//   * Everything runs on the sim thread; the only RNG is the session's synced stream (§10 order).
//
// TARGET IDENTITY [port]: each AI player's target list is kept as append-only SLOTS with tombstones; the live
// slots in slot order are exactly the original's erase-in-place vector (appends at the end, removals keep
// order), and a slot index stays valid for the battle (goal targets, AITarget wrappers). AITargetLocation
// ids = (player << 20) | slot.
//
// A2 tactical: slot 10 = Find_Plan_For_Goal (one synced draw) + the tactical PotentialPlan solve (tactical_pp.hpp:
// the land / space marshaller lists on run_pp_solver; a failed solve rejects the candidate); the selection's free-store
// units / pool entries / pads are reserved (+0x30) until the task force's Produce_Force gathers them (free units join,
// pool entries become pending reinforcements, pads become Build reservations; nothing is bought: AI_GALACTIC_3 §1.11).
// FIRST-CUT PARTS (docs/PHASE4.md A1 open items): blocking-status
// completion rules (open item 3) are this port's: move = every live unit without a movement goal (A2 tactical: an
// Attack_Move to an object completes when the object dies, units re-ordered only when it moved), attack =
// target dead, guard = never (fails when the guarded object dies), reinforce / build = at once; land slot 9
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
#include "alamo/commands/command.hpp"
#include "alamo/commands/events.hpp"
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

// A hard point acquired an opportunity target (STRATEGIC_GAPS §5.3: signal 0x21 OpportunityTargetAcquiredDataClass, sent
// to the hard point's owner, or to the team object for a team member). Object ids; hardpoint_type = HardPointType value.
struct OpportunityTargetEvent {
    int32_t owner = -1;           // the hard point's owner object
    int32_t target = -1;          // the acquired target object
    int32_t hardpoint_type = -1;  // HardPointType of the hard point (5..10 for weapons)
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
    // Active_By_Default re-apply (TACTICAL_GAPS §1.2 [H]): called with the PerceptionObject id when a unit of `player`
    // enters the AI free store (AIFreeStoreClass vf1 0x4d4aa0: every admission, start-up and new units included), and
    // when a task force releases a unit the free store then refuses (TaskForceClass vf49 0x5ff610). The host calls
    // AbilitySystem::apply_active_by_default(id, frame) - skipping a unit whose HUNT is active (0x39ad40) on the
    // task-force path - and un-immobilises it first (0x3a8e50(obj, 0)). Called during the AI update. Default: nothing.
    virtual void unit_released(int32_t /*player*/, int32_t /*object_id*/) {}
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
    // write(): u32 version (1); the shape (AI player pattern, plan library size, plan runtime size, mode: checked
    // against the reader's rebuilt session); now, mode frame, current plan, the plan / task-force / status /
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

// S10 AI land reinforcement - the landing position of a land TF.Reinforce near `at` (0x6eaf60 [M]; drop validity
// TACTICAL_GAPS §4.2 [H radius rule]). With Use_Reinforcement_Points a drop slot is valid only within 2-D <=
// Reinforcement_Enabling_Radius of an allied live object, so [inferred]: the own / allied live object with an enabling
// radius > 0 nearest to `at` (2-D, strict <: the first in snapshot (GOM) order wins a tie), and `at` := that object's
// position moved toward `at` by min(distance, radius / 2) [port: the host lays its formation slots around the point;
// half the radius keeps them inside]. False = no such object (nothing can land; Reinforce fails). Without
// Use_Reinforcement_Points `at` is kept. The host keeps its own drop checks (passability, fog, footprint, prevention
// radius) [port: not repeated here]. `facts` has the same order as `w.objects`. No RNG.
bool reinforce_landing_point(const PerceptionWorld& w, const std::vector<TacticalAiObject>& facts, int32_t player,
                             bool use_reinforcement_points, Vec3f& at);

// DifficultyAdjustments.xml: the tactical fields.
std::vector<TacticalAiDifficulty> load_tactical_ai_difficulties(const FileProvider& files);
// The plan files of one tactical mode's folder (Data\Scripts\AI\LandMode\ or SpaceMode\), sorted [port].
std::vector<std::string> tactical_plan_files(TacticalMode mode, const std::function<std::vector<std::string>(std::string_view)>& list);

}  // namespace alamo::ai::tactical
