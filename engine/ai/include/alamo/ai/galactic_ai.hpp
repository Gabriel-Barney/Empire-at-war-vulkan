// Galactic AI (roadmap A2; docs/re/AI_GALACTIC.md §2, framework §1). Library alamo_ai_lua (needs engine/lua).
//
// GalacticAiWorld owns, for one galactic session: the AI players' per-mode galactic contexts
// (ai_context.hpp), the shared AI data (definitions, plan library), the plan runtime (pooled Lua plan
// scripts, ai_lua_runtime.hpp), task forces, blocking statuses, the galactic free stores and the perception
// resolver over the S3 GalacticSim (galactic_ai_tokens.hpp).
//
// HOST CONTRACT
//   * update_player(p, now, ...) from the PlayerList update (T7) in player index order, every frame; it
//     services the player's galactic context only when the galactic mode is on top (§1.3: the galactic AI
//     does not run during a tactical battle). The first call of a frame also refreshes perception.
//   * AI orders are EventData handed to the order sink, which must execute them DIRECTLY at the current
//     frame (SimHost::execute_direct / GalacticSim::execute; COMMANDS.md §5, AI_GALACTIC.md §1.12). Fleet
//     launch / land / form / detach / merge are FleetManagement events (A2 galactic, AI_GALACTIC_2 §5.4).
//   * The budget's income ticks come from GalacticSim's income-tick listener (registered by start()).
//   * Everything runs on the sim thread; the only RNG is the session's synced stream, drawn in the
//     documented order (§1.13). AI state is hashed by hash() ("ai.goals" CRC system, port-only diagnostic).
//
// STAGE 2 (docs/re/AI_GALACTIC_2.md): the PotentialPlan solver runs the nine galactic marshallers in the
// recovered order and rules (§1, galactic_pp.cpp), with the ETA / queue reservations of §2.1-2.2; Produce_Force
// creates AIBuildTasks bid on by the per-planet AIPlanetBuildQueue market (§2.3, galactic_build.cpp); the budget
// is §3 verbatim (ai_budget.hpp); perception tokens follow AI_GALACTIC_TOKENS.md. Remaining port stand-ins are
// marked [port] in the sources and listed in docs/PHASE4.md (A2 open items). STAGE 3 (docs/re/AI_GALACTIC_3.md):
// hero records vetoed as the binary does, galactic pop cap (AI only with AI_Uses_Galactic_Pop_Cap), MagicPlan spawn,
// the market inside GalacticSim's production-service hook, TerrainType, K = 1, FleetManagement orders.
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/core/bytes.hpp"
#include "alamo/ai/ai_definitions.hpp"
#include "alamo/ai/ai_path_finder.hpp"
#include "alamo/ai/latency_job.hpp"
#include "alamo/ai/ai_plans.hpp"
#include "alamo/ai/galactic_ai_tokens.hpp"
#include "alamo/commands/command.hpp"
#include "alamo/commands/events.hpp"
#include "alamo/objects/object_store.hpp"

namespace alamo {
class FileProvider;
class GameRandom;
class CrcRegistry;
class StateHasher;
}  // namespace alamo
namespace alamo::galactic {
class GalacticSim;
class ContrastTable;
}
namespace alamo::lua {
class LuaPlayerWorld;
class GlobalValueStore;  // Int-final
}
namespace alamo::objects {
class GameObjectTypes;
class ObjectStore;
}  // namespace alamo::objects

namespace alamo::ai {

class AiContext;
class EquationSet;

struct GalacticAiPlayerConfig {
    int32_t player = -1;
    std::string ai_type;     // AIPlayerType name ("BasicEmpire")
    int32_t difficulty = 1;  // 0 Easy, 1 Normal, 2 Hard (GetAdj index)
};

struct GalacticAiDifficulty {  // DifficultyAdjustments fields the AI reads (§1.2)
    std::string name;
    float galactic_contrast = 1.0f;     // Galactic_AI_Contrast_Multiplier
    float galactic_goal_sleep = 0.0f;   // Galactic_AI_Goal_Cycle_Sleep_Duration (s)
};

struct GalacticAiConfig {
    std::vector<GalacticAiPlayerConfig> players;
    std::vector<GalacticAiDifficulty> difficulties;  // by DifficultyAdjustments name (lookup by player type's names)
    bool story_campaign = false;    // Game.IsStoryCampaign
    uint32_t solver_max_picks = 200; // PotentialPlan solver iteration guard [port]
    bool log_decisions = true;      // keep the decision log (decision_log())
    // The AI contrast table (PGAICommands EnemyContrastTypes / FriendlyContrastTypes; S10 ConflictRules::contrast).
    // Null = no typed contrast entries (only the space / ground totals).
    const galactic::ContrastTable* contrast = nullptr;
    // Fan-out for the per-AI all-pairs recompute (AiPathFinder); any value gives identical matrices.
    ParallelFor path_pf = serial_for;
    // OPT2 t02: the path finders' cadence recomputes run on `background` (latency_job.hpp; empty = inline at the kick,
    // same results) and are applied `path_latency` galactic frames after the kick (0 = the original timing).
    uint32_t path_latency = galactic::kDefaultAllPairsLatency;
    BackgroundStart background;
    // OPT2 t01: each AI player's Perception / Goal / Planning / Execution / Learning clocks start on a fixed per-player
    // phase (AiContext::set_stagger). false = every player on the same frames (the original timing).
    bool stagger = true;
    // OPT2 t04: fan-out for the per-frame scheduled Lua collections of this world's VMs (lua/gc_schedule.hpp); any
    // value gives identical results.
    ParallelFor lua_gc_pf = serial_for;
    // GameConstants AIUsesFogOfWarGalactic (GC +0x2EC, shipped False). On: the S2 perception's remembered values follow
    // each AI's GRC 2 presence visibility (snapshot_from_sim). The A2 token resolver stays live either way [port].
    bool ai_uses_fog_galactic = false;
    // (A2 galactic: terrain, population cap and AI_Uses_Galactic_Pop_Cap come from GalacticSim / GalacticRules.)
};

using AiOrderSink = std::function<commands::RejectReason(uint8_t player, const commands::EventData& ev)>;

// One line per AI decision (activation, plan start / end, order) for logs and goldens.
struct AiDecision {
    int64_t frame = 0;
    int32_t player = -1;
    std::string text;
};

class GalacticAiWorld {
public:
    GalacticAiWorld(galactic::GalacticSim& sim, objects::ObjectStore& store, const objects::GameObjectTypes* types,
                    const FileProvider* files, GameRandom& rng, const AiDefinitions& defs, const EquationSet& equations,
                    std::vector<GalacticAiTypeInfo> type_info, AiOrderSink sink, GalacticAiConfig cfg);
    ~GalacticAiWorld();
    GalacticAiWorld(const GalacticAiWorld&) = delete;
    GalacticAiWorld& operator=(const GalacticAiWorld&) = delete;

    // Loads every plan script (kPlanFolders order, sorted within a folder [port]) and the free-store
    // scripts. Plan files are listed by `list` (Vfs::list) or given directly. False when no plan loaded.
    bool load_plans(const std::vector<std::string>& plan_files, std::vector<std::string>* diagnostics = nullptr);
    // Creates the AI players' galactic contexts (call after load_plans).
    bool start(std::vector<std::string>* diagnostics = nullptr);

    // T7 entry point (see the header comment). `galactic_on_top`: the galactic mode is the top mode;
    // `auto_resolve_busy`: an auto-resolve runs (Planning-only exception).
    void update_player(int32_t player, int64_t now, bool galactic_on_top, bool auto_resolve_busy);
    // Suspend_AI / galactic pause (TheAI gate).
    void set_suspended(bool suspended);
    void set_galactic_paused(bool paused);
    // Story requests: the story session's LuaPlayerWorld (StorySession::player_world()) is drained every frame
    // like the AI's own: GiveDesireBonus, Purge_Goals, Budget.Flush_Category and Suspend_AI reach the galactic AI.
    // Null detaches. The world must outlive this object (or be detached first).
    void attach_story_world(lua::LuaPlayerWorld* world);
    // Int-final: the session-wide GlobalValue table (L1_S4_REMAINING §1.5) for every AI VM's GlobalValue.Get / Set;
    // null (default) = the world's own map (its CRC section). Hosts attach SimHost::global_values().
    void set_global_values(lua::GlobalValueStore* store);

    // The player's galactic budget categories as the S10 / A1 land-battle credit transfer reads them
    // (template Tactical_Budget_Allowance, available, pool; category list order). Empty for non-AI players.
    // [port] Not written back: the next per-tick budget update re-derives pools from the player's credits.
    struct TacticalBudgetRow {
        float allowance = 0.0f, available = 0.0f, pool = 0.0f;
    };
    // [port] Read-only: the S10 transfer does not write back into the §3 budget (the next tick's drift term
    // re-balances the pools against the player's credits).
    std::vector<TacticalBudgetRow> tactical_budget_rows(int32_t player) const;

    // Survival learning (STRATEGIC_GAPS §1.1, the Survival marshaller's per-type table) - feeds for hosts:
    //   * object removal (0x3a8380: every object leaving a mode, alive = health > 0 and not damage-tracked dead):
    //     record_object_removal(owner, object type, original type, survived). Skipped for non-AI owners, object types
    //     without a UNIT_AI behaviour or with a TRANSPORT behaviour. `original_type` = team / transport / container
    //     type (0x3973b0), else the object's own type. Hosts skip it in multiplayer.
    //   * the auto-resolve survival roll (0x60a140, BATTLE_TRANSITIONS §4.4 step 7): record_auto_resolve_survival.
    // Both apply the company-leader substitution (a company whose first Company_Units member is not Create_Team counts
    // as that member) and key by the type-name CRC. survival_ratio = the marshaller score (1.0 when never recorded).
    void record_object_removal(int32_t owner, uint32_t object_type, uint32_t original_type, bool survived);
    void record_auto_resolve_survival(int32_t owner, uint32_t type, bool survived);
    double survival_ratio(int32_t player, uint32_t type) const;

    // ---- inspection ----
    const PlanLibrary& plans() const;
    bool is_ai_player(int32_t player) const;
    const AiContext* context(int32_t player) const;
    size_t running_plans() const;
    // OPT2 t02: background path recomputes in flight (save tests: --save-when scheduled).
    size_t background_jobs_in_flight() const;
    size_t running_plans(int32_t player) const;
    const std::vector<AiDecision>& decision_log() const;
    std::vector<std::string> lua_errors() const;  // plan / free-store script errors (since start)
    uint64_t orders_issued() const;
    // Port diagnostics: proposal outcomes per goal type (accepted / no plan / no potential plan).
    const std::map<std::string, uint32_t>& proposal_stats() const;
    uint64_t orders_rejected() const;
    const GalacticSimTokenResolver& resolver() const;

    // A2 galactic, tests / diagnostics: solves the PotentialPlan of `plan` for (goal type, target) for an AI player as a
    // proposal would (it draws the synced RNG the same way) and returns the selected types in selection order; empty
    // with *ok = false when the solve fails; *eta = the PotentialPlan's ETA (s) when solved. No AI state changes besides
    // the RNG.
    std::vector<uint32_t> solve_potential_plan(int32_t player, std::string_view plan, std::string_view goal, int32_t target,
                                               bool* ok = nullptr, float* eta = nullptr);
    // A2f t02, tests / diagnostics: the goal maintenance's production-time term (0x525200 stand-in [M]) of a running plan:
    // the largest time its unfinished build tasks still need (0 when none or no such plan).
    float plan_production_time(int32_t running) const;
    // A2f t09, tests / diagnostics: a live PotentialPlan's selected types (selection order) and its nominal time 0x6c0450
    // (the goal's AllowedTime base, Goal::nominal_time); empty / -1 for an unknown handle.
    std::vector<uint32_t> potential_plan_selection(int32_t pp_handle) const;
    float potential_plan_nominal_time(int32_t pp_handle) const;

    // Free store (docs/re/FIXPASS_GALACTIC.md §1), tests / diagnostics. These act like the player's free-store script:
    //   free_store_move   = FreeStore.Move_Object(unit, planet): false when refused (§1.2); a true return with no
    //                       pending move means the move finished at once (already there, no path, orbit check);
    //   reachable_target  = FindTarget.Reachable_Target(PlayerObject, perception, application, reachability, p): -1 for
    //                       nil, else the planet; draws the synced RNG as the Lua call does (at most once);
    //   reach_planets     = the level's source-planet list (0x665160 -> reachability flood, cached 10 x FPS frames).
    bool free_store_move(int32_t player, objects::ObjectHandle unit, PlanetIndex dest);
    size_t pending_free_store_moves() const;
    // a1h t07, tests / diagnostics: free_store_supply = the free-store test 0x4d2bd0 a build task of an AllowFreeStoreUnits
    // plan takes its unit from (null = none; the entry's +0x30 owner is not read, A1_READINGS §28.5); free_store_owner =
    // a unit's entry +0x30 (the PotentialPlan handle, -1 = none); set_free_store_owner stands in for an accept's
    // reservation (0x6c2b20; -1 clears).
    objects::ObjectHandle free_store_supply(int32_t player, uint32_t type) const;
    int32_t free_store_owner(objects::ObjectHandle unit) const;
    void set_free_store_owner(objects::ObjectHandle unit, int32_t pp);
    int32_t reachable_target(int32_t player, std::string_view perception, std::string_view application,
                             std::string_view reachability, double probability);
    std::vector<PlanetIndex> reach_planets(int32_t player, int level);

    // ---- AI execution path finder (GALACTIC.md §4, ai_path_finder.hpp) ----
    // One AiPathFinder per AI player: created at the context's first service (Execution slot 6: one recompute, then
    // the Q11 stagger with slot = player index and K = players - humans), recomputed by the Execution service when
    // the galactic mode frame (GalacticSim::frame) reaches next (then next = frame + 90, before the free store).
    // Inputs at each recompute: Node_Weight = this world's perception node values, Layer_Z_Adjust of each planet's
    // type, and the Is_Planet_Revealed fog gate (auto-reveal honoured) only for a non-human AI player.
    // path(): Get_Path (Q10) on that player's matrices; a query before creation creates the finder lazily (0x524c10,
    // next = 0). Empty for a non-AI player, an unknown planet or no path; src == dst gives [src, src].
    std::vector<PlanetIndex> path(int32_t player, PlanetIndex from, PlanetIndex to);
    const AiPathFinder* path_finder(int32_t player) const;  // null for non-AI players
    // Lua _FindStageArea(player, planet) (A2k t02): the player's own planet starting its cheapest all-pairs path to
    // `target` (cheapest_stage); the fewest-hops planet while the finder is not created. kInvalidPlanet when none.
    PlanetIndex stage_area(int32_t player, PlanetIndex target) const;
    // The same queries for Lua Find_Path / hosts (lives as long as this object), but WITHOUT lazy creation: false for a
    // player whose finder is not created. Attached to this world's own LuaPlayerWorld at construction and to the story
    // world by attach_story_world (detached again on replacement and in the destructor).
    AiPathQuery& path_query();
    // Path finder state (per AI player in index order: created, next, recompute count, matrix digest). Kept out of
    // hash() / "ai.goals" so existing goldens do not move; hosts register it as its own system.
    void hash_paths(StateHasher& h) const;
    void register_path_crc(CrcRegistry& registry, const std::string& name = "ai.paths") const;

    void hash(StateHasher& h) const;
    // N1 save / load (contract: alamo/core/save_file.hpp; layout in galactic_ai_save.cpp). write(): every piece of AI
    // state that hash() / hash_paths() cover and that later frames read (perception, contexts, plans + their Lua VMs,
    // task forces, statuses, PotentialPlans, the build market, free stores, pooled / evaluator VMs, id counters).
    // read(): on a world built from the same data and start()ed for the same session, AFTER the ObjectStore and the
    // GalacticSim were restored (objects are matched by id); all-or-nothing (false leaves this world unchanged).
    // Definitions, host attachments and diagnostics (decision log, script errors, statistics) are not saved.
    void write(ByteWriter& w) const;
    bool read(ByteReader& r);
    // Tests only: false leaves the Lua VMs out of the stream (write() stores empty VM images; read() ignores the images
    // and builds each VM fresh from its script), so the rest of the AI state can be checked on its own. Default true.
    void set_save_lua_state(bool on);
    // Registers "ai.goals" (hash()).
    void register_crc(CrcRegistry& registry, const std::string& name = "ai.goals") const;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

// DifficultyAdjustments.xml (Data\XML\DifficultyAdjustments.xml): the AI-side fields.
std::vector<GalacticAiDifficulty> load_ai_difficulties(const FileProvider& files);// Per-type AI facts (CategoryMask tokens, Max_Space_Base / Max_Ground_Base, Is_Named_Hero, transports)
// from the typed object database.
std::vector<GalacticAiTypeInfo> make_galactic_ai_type_info(const objects::GameObjectTypes& types);

}  // namespace alamo::ai
