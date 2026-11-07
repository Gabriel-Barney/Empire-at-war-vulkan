// Galactic AI world: private implementation shared by galactic_ai.cpp (world, contexts, plans, task forces,
// statuses, free store), galactic_pp.cpp (PotentialPlan marshallers + ETA, AI_GALACTIC_2 §1 / §2.1-2.2) and
// galactic_build.cpp (AIBuildTask + AIPlanetBuildQueue market, §2.3). Not installed.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <tuple>
#include <vector>

#include "alamo/ai/ai_context.hpp"
#include "alamo/ai/ai_free_store_path.hpp"
#include "alamo/ai/ai_lua_runtime.hpp"
#include "alamo/ai/galactic_ai.hpp"
#include "alamo/core/file_provider.hpp"
#include "alamo/core/hash.hpp"  // name_crc (survival key)
#include "alamo/core/sim_crc.hpp"
#include "alamo/core/sim_rng.hpp"
#include "alamo/galactic/campaign.hpp"
#include "alamo/galactic/conflicts.hpp"  // q6 t03: the player conflict stamps
#include "alamo/galactic/fleet_path.hpp"
#include "alamo/lua/ai_bridge.hpp"
#include "alamo/lua/players.hpp"
#include "alamo/lua/script_gaps.hpp"
#include "alamo/lua/script_remaining.hpp"  // L1 final
#include "galactic_goal_time.hpp"
#include "galactic_slots.hpp"
#include "alamo/objects/game_object_types.hpp"

namespace alamo::ai {

namespace gal = alamo::galactic;
namespace cmd = alamo::commands;
using objects::ObjectHandle;

inline constexpr int32_t kFreeStoreScriptBase = 1000000;  // script ids of free stores: base + player
inline constexpr double kPpInfinite = 1.0e18;
inline constexpr uint32_t kAloneFleet = 0xFFFFFFFEu;  // launch_and_form: a fleet of its own

inline double now_seconds(const gal::GalacticSim& sim) {
    return static_cast<double>(sim.frame()) / static_cast<double>(kAiFps);
}
inline bool space_kind(gal::UnitKind k) { return k == gal::UnitKind::SpaceUnit || k == gal::UnitKind::Squadron; }
inline bool ground_kind(gal::UnitKind k) { return k == gal::UnitKind::GroundUnit; }
inline bool structure_kind(gal::UnitKind k) { return k == gal::UnitKind::GroundStructure || k == gal::UnitKind::StarBase; }

// ================================================================ state records

struct TaskForceRec {
    int32_t id = -1;
    int32_t plan = -1;
    int32_t player = -1;
    uint32_t def = 0;  // index into the plan's task forces
    std::string name;
    std::vector<ObjectHandle> units;
    std::vector<uint32_t> wanted;          // types to produce (PotentialPlan selections without a free unit)
    std::vector<PlanetIndex> wanted_source;  // ETA source planet per wanted type (PP+0x180)
    std::vector<int32_t> builds;           // AIBuildTask ids (Produce_Force)
    int32_t stage = -1;                    // target (planet) id
    bool had_units = false;
    bool no_units_sent = false;
    bool alive = true;
};

// AIBuildTask (0x524ee0, FSM 0x64aec0) + its AIPlanetBuildTask (0x6954b0) [H; port states].
enum class BuildState : uint8_t { Queued = 0, Producing = 1, Finished = 2, Failed = 3 };
struct BuildTask {
    int32_t id = -1;
    int32_t plan = -1;
    int32_t tf = -1;  // -1 = the plan ended (a produced object goes to the free store)
    int32_t player = -1;
    uint32_t type = gal::kNoIndex;
    PlanetIndex dest = kInvalidPlanet;  // task +0x98
    uint8_t queue = 1;                  // 0 ground / structures, 1 the rest
    BuildState state = BuildState::Queued;
    bool bound = false;                 // structure / generic: build planet = goal target (0x6956f0)
    PlanetIndex bid_planet = kInvalidPlanet;
    double bid = kPpInfinite;
    int64_t created = 0;
    int64_t next_poll = 0;              // PRODUCTION_QUEUED polls every 3 frames
    bool charged = false;
    float charge = 0.0f;
    int32_t queue_id = -1;              // global counter 0x140a436b4 at production start
    PlanetIndex producing_at = kInvalidPlanet;
    int32_t queue_entries_before = 0;   // the sim queue's entries of this type/player when production started
    uint32_t goal_key = 0;
    int32_t category = -1;
    bool listed = false;                // on the global pending list
    bool reclaimed = false;             // +0xD9: taken by a PAYG reclaim (AI_GALACTIC_3 §8)
};

// AIPlanetBuildQueue (per planet x queue index), §2.2-2.3.
struct QueueReservation {
    int32_t pp = -1;  // PotentialPlan handle
    uint32_t type = gal::kNoIndex;
    float time = 0.0f;
};
// PlanetBuildQueue::current while the production of a task whose plan ended goes on (current_queue_id >= 0): the
// original keeps the ended task (+0xA8) on the queue; the port keeps only whether the PAYG reclaim took it (+0xD9), which
// BudgetFractionToBuild reads (A2H_GALACTIC_AI_RE §2 step 4) [port].
inline constexpr int32_t kQueueTaskEnded = -1;
inline constexpr int32_t kQueueTaskEndedReclaimed = -2;
struct PlanetBuildQueue {
    int64_t last_run = 0;
    std::vector<int32_t> tasks;  // claimed build tasks (+0x10), claim order
    int32_t current = -1;        // +0x20 producing task (< 0 none: kQueueTaskEnded / kQueueTaskEndedReclaimed)
    int32_t current_queue_id = -1;
    uint32_t current_type = gal::kNoIndex;
    std::vector<QueueReservation> reservations;
};

enum class StatusKind : uint8_t { Done, ProduceForce, MoveTo, Invade, BudgetWait };

struct StatusRec {
    int32_t id = -1;
    int32_t plan = -1;
    int32_t tf = -1;
    StatusKind kind = StatusKind::Done;
    bool finished = false;
    lua::AiStatusResult result = lua::AiStatusResult::Nil;
    PlanetIndex dest = kInvalidPlanet;
    uint8_t mode = 5;
    int64_t started = 0;
    lua::AiBudgetOp op = lua::AiBudgetOp::WaitSpendable;
    double amount = 0.0;
};

// A solved PotentialPlanClass (0x210 B): the selection and the results Init copies to the goal.
struct PpSelection {
    uint32_t type = gal::kNoIndex;
    ObjectHandle unit;                    // free-store unit, null = to be produced (PP+0x108)
    int32_t tf = 0;                       // task force of the slot the unit was placed in
    PlanetIndex source = kInvalidPlanet;  // ETA source planet (PP+0x180 / +0x168 reservation)
    uint8_t queue = 1;
    float btime = 0.0f;
};
struct PotentialPlanRec {
    int32_t player = -1;
    uint32_t plan = kAiNone;
    uint32_t goal_type = kAiNone;
    int32_t target = -1;
    std::vector<PpSelection> sel;
    float gross = 0.0f, net = 0.0f, eta = 0.0f;
    bool contrast_ok = true;              // PP+0x4D (Test_Target_Contrast)
    bool queues_reserved = false;         // PP+0x4E (0x6c0be0 ran)
    double pop = 0.0;                     // PP+0x1A8: population of the units to produce (AI_GALACTIC_3 §3.5)
    // PP+0x108: the selection's free-store units (filled by the solve). 0x6c2d60 reserves those still on the free
    // store (+0x30 = this PP, unit_reserved); 0x6c27d0 removes a unit that left the plan (A1_READINGS §28.4).
    std::vector<objects::ObjectId> reserved_units;
};

struct PlanInstance {
    int32_t id = -1;
    int32_t player = -1;
    uint32_t plan = kAiNone;
    uint32_t goal_type = kAiNone;
    uint32_t goal_key = 0;   // goal serial (budget struct, block list)
    int32_t category = -1;
    int32_t target = -1;
    int32_t target_owner = -1;  // for Original_Target_Owner_Changed
    int32_t pp = -1;            // PotentialPlan handle (released with the goal)
    std::unique_ptr<lua::LuaVm> vm;
    std::vector<int32_t> tfs;
    bool result = false;         // Set_Plan_Result (plan+0xa8), default false [L]
    bool non_removable = false;  // Set_As_Goal_System_Removable(false)
    bool test_contrast = true;   // Test_Target_Contrast (PP+0x4D)
    bool blocking = false;       // Block_Goal_Proposal entry present
    bool ended = false;
    uint32_t live_threads = 0;  // after the last service
};

struct PendingBonus {
    uint32_t goal_type = kAiNone;
    int32_t target = -1;
    float bonus = 0.0f;
    int64_t deadline = 0;
};

// A pending free-store Move_Object (AI_GALACTIC §2.8): the unit's own fleet moves hop by hop toward dest.
struct FreeStoreMove {
    ObjectHandle unit;
    int32_t player = -1;
    PlanetIndex dest = kInvalidPlanet;
    bool done = false;
};

struct PlayerAi;

// ================================================================ Impl

struct GalacticAiWorld::Impl final : lua::AiScriptHost, lua::ScriptMarkupHost, AiPathQuery {
    Impl(gal::GalacticSim& s, objects::ObjectStore& st, const objects::GameObjectTypes* ty, const FileProvider* fp,
         GameRandom& r, const AiDefinitions& d, const EquationSet& e, std::vector<GalacticAiTypeInfo> ti, AiOrderSink snk,
         GalacticAiConfig c);
    GalacticSimTokenResolver::Hooks make_hooks();
    bool active_goals(int32_t p, std::string_view category, int32_t& out) const;  // <root>.ActiveGoals
    // BudgetFractionToBuild's view of a planet queue's AI build task (A2H_GALACTIC_AI_RE §2 step 4).
    GalacticSimTokenResolver::QueueTask queue_task(PlanetIndex planet, uint32_t queue) const;

    gal::GalacticSim& sim;
    objects::ObjectStore& store;
    const objects::GameObjectTypes* types;
    const FileProvider* files;
    GameRandom& rng;
    const AiDefinitions& defs;
    const EquationSet& eqs;
    std::vector<GalacticAiTypeInfo> tinfo;
    AiOrderSink sink;
    GalacticAiConfig cfg;
    // q6 t03: the loaded GameObjectCategoryType enum (null = the vanilla table) and the host's conflict manager.
    const objects::EnumConverter* cat_enum = nullptr;
    const gal::ConflictManager* conflicts = nullptr;
    uint64_t category_bit(std::string_view name) const { return ai_category_bit(name, cat_enum); }

    // perception
    GalacticSnapshot snap;
    GalacticPerception perc;
    std::vector<PerceptionMemory> mem;
    std::unique_ptr<GalacticTokenResolver> s2;
    GalacticSimTokenResolver res;
    int64_t perception_frame = -1;
    // Script_<File> evaluators: one shared Lua instance per file, loaded on first use (AI_GALACTIC_2 §4.4).
    ScriptEvaluators scripts;
    // Apply_Markup accumulators per (player, planet) (+0x80 of the player's AI target).
    std::map<std::pair<int32_t, int32_t>, double> markup;

    // Lua
    lua::LuaPlayerWorld world;
    lua::LuaPlayerWorld* story_world = nullptr;  // the story session's world, drained each frame (attach_story_world)
    PlanLibrary library;
    std::unique_ptr<PlanScriptRuntime> runtime;
    std::map<std::string, lua::AiGlobalValue> globals;
    lua::GlobalValueStore* gv_store = nullptr;  // Int-final: attached session-wide GlobalValue table (globals unused then)

    AiEnableGate gate;
    std::vector<std::unique_ptr<PlayerAi>> ai;  // by player index

    std::map<int32_t, PlanInstance> plans;
    std::map<int32_t, TaskForceRec> tfs;
    std::map<int32_t, StatusRec> statuses;
    std::map<int32_t, PotentialPlanRec> pps;
    std::map<int32_t, BuildTask> builds;
    std::vector<int32_t> pending_builds;  // global pending list 0x140b39330 (push order)
    std::vector<std::array<PlanetBuildQueue, 2>> planet_queues;  // per planet row
    // Market delivery floods (0x6955a0 / 0x695c90 path planet -> destination), per frame, key (owner, source, AI mode).
    int64_t delivery_frame = -1;
    std::map<std::tuple<int32_t, PlanetIndex, int>, std::vector<uint8_t>> delivery_reach;
    // Free-store entry +0x30: unit -> PotentialPlan handle (A1_READINGS §28). The galactic free store is derived (the
    // player's units no task force holds), so a task force taking a unit erases its entry here (off_free_store).
    std::map<objects::ObjectId, int32_t> unit_reserved;
    std::vector<FreeStoreMove> fs_moves;  // pending free-store moves (issue order)
    int32_t next_plan = 1, next_tf = 0, next_status = 0, next_pp = 0, next_build = 0, next_queue_id = 0;
    int32_t current_plan = -1;
    int64_t now = 0;
    int32_t market_sim_frame = -1;  // galactic mode frame of the last market call
    std::vector<uint64_t> type_mask;  // GameObjectCategoryType bits per type

    std::vector<AiDecision> log;
    std::vector<std::string> errors;
    std::map<std::string, uint32_t> stats;  // port diagnostics (never hashed)
    // (plan, task force, team) -> expanded type list (static per data, lookup cache)
    std::map<std::tuple<uint32_t, uint32_t, uint32_t>, std::vector<uint32_t>> team_types;
    int64_t eval_count = 0;  // perception evaluations (diagnostic)
    uint64_t orders = 0, rejected = 0;
    // A2 galactic: GalacticSim income-tick listener (§3.1). The sim may outlive or predecease this object, so the
    // listener holds a weak token instead of being removed in the destructor.
    uint32_t income_hook = 0;
    uint32_t credit_hook = 0;  // a2k t10: Add_Credits notify (0x4b0310)
    std::shared_ptr<int> alive = std::make_shared<int>(0);
    void on_income(const gal::IncomeTick& t);

    // ---- N1 save / load (galactic_ai_save.cpp) ----
    bool lua_state = true;  // set_save_lua_state (tests)
    // Points a restored VM's host hooks (players world, AI host with `script_id`, markup host when `with_markup`) at
    // this world, writing only the ones that differ.
    void rebind_vm(lua::LuaVm& vm, int32_t script_id, bool with_markup);

    // ---- helpers ----
    void decide(int32_t player, std::string text) {
        if (cfg.log_decisions) log.push_back({now, player, std::move(text)});
    }
    PlayerAi* player_ai(int32_t p) {
        return p >= 0 && static_cast<size_t>(p) < ai.size() ? ai[static_cast<size_t>(p)].get() : nullptr;
    }
    const PlayerAi* player_ai(int32_t p) const {
        return p >= 0 && static_cast<size_t>(p) < ai.size() ? ai[static_cast<size_t>(p)].get() : nullptr;
    }
    // q6 t07: one AI player of `type` (the start() body per player; also the AI type change and a load that rebuilds a
    // changed player). The stagger ordinal comes from cfg.players, which must already list the player.
    std::unique_ptr<PlayerAi> make_player(int32_t player, const AiPlayerTypeDef* type, int32_t difficulty,
                                          std::vector<std::string>* diagnostics);
    void install_sim_hooks();          // the income / production-service / installment hooks (once, first AI player)
    void retire_player(int32_t player);  // q6 t07: ends the player's plans, PotentialPlans and free-store moves
    const gal::TypeInfo* type_info(uint32_t t) const { return t < sim.rules().types.size() ? &sim.rules().types[t] : nullptr; }
    std::string type_name(uint32_t t) const { return t < sim.rules().types.size() ? sim.rules().types[t].name : "?"; }
    // Hyperspace type of a unit TYPE (STRATEGIC_GAPS §1.7, the AIBuildTask travel estimate 0x648e80): a ground company's
    // Company_Transport_Unit, else the type itself (the type form of GalacticSim::travel_type).
    uint32_t travel_type_of(uint32_t t) const {
        const gal::TypeInfo* x = type_info(t);
        if (x && x->company_transport < sim.rules().types.size() &&
            (x->dummy == gal::kDummyGroundCompany || x->kind == gal::UnitKind::GroundUnit))
            return x->company_transport;
        return t;
    }
    // Survival table key (STRATEGIC_GAPS §1.1) [H]: the type, or the first Company_Units member of a company whose
    // first member is not Create_Team; the key is that type's name CRC.
    uint32_t survival_key(uint32_t t) const {
        if (t < tinfo.size() && tinfo[t].company_first != 0xFFFFFFFFu && !tinfo[t].company_first_create_team)
            t = tinfo[t].company_first;
        return name_crc(type_name(t));
    }
    std::string planet_name(PlanetIndex p) const {
        return p < sim.map().planets.name.size() ? sim.map().planets.name[p] : std::string("-");
    }
    bool valid_planet(PlanetIndex p) const { return p < sim.planets().size() && sim.planets()[p].is_planet; }
    uint64_t mask_of(uint32_t t) const { return t < type_mask.size() ? type_mask[t] : 0u; }
    double pop_of(uint32_t t) const {  // pop(T, 0) = Population_Value (AI_GALACTIC_3 §3)
        const gal::TypeInfo* ti = type_info(t);
        return ti ? static_cast<double>(ti->population) : 0.0;
    }
    double ai_remaining_pop(int32_t player) const;  // 0x5f5080 (§3.5)
    float power_of(uint32_t t) const {
        const gal::TypeInfo* ti = type_info(t);
        return ti ? ti->ai_combat_power : 0.0f;
    }
    float cost_none(uint32_t t) const {  // 0x3711c0 without a planet: type +0x86C
        const gal::TypeInfo* ti = type_info(t);
        return ti ? static_cast<float>(ti->build_cost) : 0.0f;
    }
    // 0x371230 without a planet (GALACTIC_ECONOMY §2.3 T0) [H]: float(uint32(float(+0x890) x 0x140b15920)), the type's
    // Build_Time_Seconds x GameConstants Production_Speed_Factor, truncated (a2g t03; was factor 1).
    float btime_none(uint32_t t) const {
        const gal::TypeInfo* ti = type_info(t);
        return ti ? gal::econ::build_time_t0(ti->build_time_seconds, sim.rules().constants.production_speed_factor) : 0.0f;
    }
    float cost_at(int32_t player, uint32_t t, PlanetIndex p) const {
        return valid_planet(p) ? static_cast<float>(sim.build_cost_for(player, p, t)) : cost_none(t);
    }
    float btime_at(int32_t player, uint32_t t, PlanetIndex p) const {
        return valid_planet(p) ? sim.build_time_for(player, p, t) : btime_none(t);
    }
    // 0x6c0450(PP) (galactic_goal_time.hpp, A2f t09): the goal's AllowedTime base, Goal::nominal_time.
    float pp_nominal_time(const PotentialPlanRec& pp) const {
        std::vector<float> bt;
        bt.reserve(pp.sel.size());
        for (const PpSelection& s : pp.sel) bt.push_back(btime_none(s.type));
        const auto ps = sim.players();
        const uint32_t d = pp.player >= 0 && static_cast<size_t>(pp.player) < ps.size() ? ps[static_cast<size_t>(pp.player)].difficulty
                                                                                         : gal::kNoIndex;
        return galactic_nominal_time(bt, galactic_build_time_multiplier(sim.rules().difficulties, d));
    }
    bool hostile(int32_t a, int32_t b) const { return sim.is_hostile(a, b); }
    uint32_t equation(const std::string& name) const;
    int32_t first_human() const {
        const auto players = sim.players();
        for (size_t p = 0; p < players.size(); ++p)
            if (players[p].human) return static_cast<int32_t>(p);
        return -1;
    }
    bool evaluate(int32_t self, int32_t opponent, const std::string& fn, int32_t target, double& out);
    bool script_evaluate(const EvalContext& c, std::string_view name, std::span<const Param> params, double& out);
    std::vector<ObjectHandle> player_units(int32_t p) const;
    uint32_t fleet_of_unit(ObjectHandle u) const;
    PlanetIndex planet_of_unit(ObjectHandle u) const;
    bool on_surface(ObjectHandle u) const { return fleet_of_unit(u) == gal::kNoFleet && planet_of_unit(u) != kInvalidPlanet; }
    bool tf_owns(ObjectHandle u) const;
    std::vector<ObjectHandle> free_units(int32_t p) const;
    ObjectHandle free_store_supply(int32_t player, uint32_t type) const;  // 0x4d2bd0 (galactic_build.cpp)
    PlanetIndex planet_arg(const lua::AiArg& a) const;
    cmd::RejectReason order(int32_t player, const cmd::EventData& ev, const std::string& what);
    cmd::RejectReason note_order(int32_t player, cmd::RejectReason r, const std::string& what);
    bool move_fleet(int32_t player, uint32_t fleet, PlanetIndex to);
    cmd::RejectReason fleet_op(int32_t player, gal::FleetOp op, uint32_t target, uint32_t fleet, uint32_t unit, const std::string& what);
    bool launch_and_form(int32_t player, PlanetIndex p, ObjectHandle unit, uint32_t fleet);
    // REMAINING_GAPS §6.2 / AI_GALACTIC_2 §2.3: the AI path class of a produced unit's delivery (2, 3 with the task
    // flag +0xDA, 5 for an Is_Stealth_Company type) and the market's path test planet `from` -> b.dest with it.
    int delivery_mode(const BuildTask& b) const;
    bool delivery_reachable(int32_t owner, const BuildTask& b, PlanetIndex from);
    // The AI's Is_Planet_Dangerous mask for `player` (GalacticPerception::fill_view on this frame's snapshot); empty
    // (= none) before the first perception update.
    gal::Bitset ai_dangerous(int32_t player) const;
    PlanetIndex nearest_owned(int32_t player) const;
    // A2k t02: the path finder's node order as a rank per planet row (galactic_ai.cpp); rebuilt lazily (static data).
    std::vector<uint32_t> node_rank;
    std::span<const uint32_t> planet_node_rank();
    bool can_build_at(int32_t player, uint32_t type, PlanetIndex p) const { return galactic_ai_can_build(sim, player, type, p); }
    // Ground structure slots (A2 f t01): the slot max 0x372740 = planet data +0x230, seeded from the planet type's
    // Special_Structures_Land (+0x92C) (AI_GALACTIC_TOKENS MaxStructureSlots, STRATEGIC_GAPS §1 step 18) [H];
    // -1 = tag absent = no cap [port, as GalacticSim::build_check]. Used 0x3f8af0 = the ground structures on the
    // surface, 0 when the planet has no ground base (D+0x130 > 0 ? D+0x160 : 0, OpenStructureSlotsUnnormalized) [H].
    // Not the legacy <Max_Ground_Base> tag (set only on Galaxy_Core_Art_Model, so 0 on every real planet).
    int32_t ground_slot_max(PlanetIndex p) const { return galactic_ai_ground_slot_max(sim, p); }
    int32_t ground_slots_used(PlanetIndex p) const { return galactic_ai_ground_slots_used(sim, p); }
    uint32_t next_starbase_type(int32_t player, PlanetIndex p) const;
    bool applies(int32_t self, int32_t target, uint32_t flags) const;
    int32_t script_player(int32_t script) const;
    PlanInstance* plan_of_script(int32_t script) {
        const auto it = plans.find(script);
        return it == plans.end() ? nullptr : &it->second;
    }

    // ---- PotentialPlan (galactic_pp.cpp) ----
    const std::vector<uint32_t>& team_type_list(uint32_t plan, uint32_t tf, uint32_t team);
    // first_solve: Init at proposal (PP+0x104 == 0, FreeStore Setup A ignores +0x30); false = the re-solve at accept.
    bool solve_potential_plan(PlayerAi& pa, uint32_t plan, uint32_t goal_type, int32_t target, PotentialPlanRec& out,
                              bool first_solve = true);
    bool resolve_pp(PlayerAi& pa, const Goal& g);  // 0x6c4a50: re-solve at accept (A1_READINGS §28.6 step 3)
    void reserve_pp_units(int32_t handle);         // 0x6c2d60's +0x30 part (0x6c2b20)
    void suspend_pp_units(int32_t handle);         // 0x6c0be0's +0x30 part (Commit step 4)
    bool pp_stolen(int32_t handle) const;          // 0x6c45e0
    void pp_unit_left(int32_t plan_id, ObjectHandle u);  // 0x6c27d0 with the plan's PP: a task force released u
    void off_free_store(ObjectHandle u) { unit_reserved.erase(store.id(u)); }  // a task force took u (node erased)
    bool on_free_store(int32_t player, objects::ObjectId id) const;
    void release_pp(int32_t handle);         // drop: free-store entries and queue reservations (0x6c2320)
    void reserve_pp_queues(int32_t handle);  // commit step 4 (0x6c0be0)
    float planet_queue_time(PlanetIndex p, uint32_t q) const;

    // ---- build tasks + market (galactic_build.cpp) ----
    void create_build_tasks(TaskForceRec& tf);
    void market_planet(PlanetIndex p);       // one planet's queues (> 30 frames since their last run), sim hook
    // 0x525200 [H] (A2F_RE_READS §1, galactic_goal_time.hpp): the time the running plan's unfinished build tasks still need.
    float goal_production_time(int32_t running) const;
    void service_build_tasks(int32_t player);  // PRODUCTION_QUEUED polls, claims, failures
    float installment(PlanetIndex p, uint32_t q, const gal::BuildEntry& head, float a);  // PAYG installment (q2f t01)
    void fail_build(BuildTask& b, const char* why);
    void finish_build(BuildTask& b);
    void end_plan_builds(int32_t plan);
    float funds_time(const BuildTask& b) const;
    const Goal* goal_of_plan(int32_t player, int32_t plan) const;  // the active goal running `plan` (null: none)
    void init_queue_phases();  // each planet queue's first last-run frame (A2J_RE_BUILD_MARKET §4)
    void remove_pending(int32_t id);
    void planet_lost(PlanetIndex p);
    float payg_reclaim(int32_t plan, float amount);
    std::map<uint32_t, float> reclaim;  // goal serial -> PAYG reclaim computed at plan end, used by the release

    // ---- plans ----
    int32_t instantiate(PlayerAi& p, Goal& g);
    void end_plan(int32_t id, bool by_goal_system);
    void service_plans(int32_t player);
    void drain_world_requests(lua::LuaPlayerWorld& w, int32_t player, int32_t plan);
    void tf_event(const TaskForceRec& tf, PlanEvent event, const std::vector<lua::AiEventArg>& args);

    // ---- execution ----
    void claim_units(int32_t player);
    void service_statuses(int32_t player);
    bool step_status(StatusRec& s);
    int32_t new_status(int32_t plan, int32_t tf, StatusKind kind);
    int32_t plan_of_tf(int32_t tf) const {
        const auto it = tfs.find(tf);
        return it == tfs.end() ? -1 : it->second.plan;
    }
    void prune_tf(TaskForceRec& tf);
    std::vector<uint32_t> tf_fleets(const TaskForceRec& tf) const;
    bool movable_fleet(uint32_t f) const;
    bool unit_jumps(ObjectHandle u) const;  // its type can jump alone (a fleet of its own can move)
    uint32_t movable_tf_fleet_at(const TaskForceRec& tf, PlanetIndex p) const;
    // P4 ai: the Ground_Activated hero trigger for task-force units (Activate_Ability / Land_Units).
    void ground_trigger(TaskForceRec& tf, const std::vector<std::pair<ObjectHandle, PlanetIndex>>& at);

    // ---- AI execution path finder (ai_path_finder.hpp) ----
    std::vector<float> planet_zoff;  // Layer_Z_Adjust of each planet row's type (sized lazily)
    gal::AllPairsInput path_input(int32_t player);
    AiPathFinder* ensure_path_finder(int32_t player);  // lazy creation (0x524c10) when missing
    bool ai_path(int32_t player, PlanetIndex from, PlanetIndex to, std::vector<PlanetIndex>& out) override;

    // ---- free store ----
    void service_free_store(int32_t player);
    void step_free_store_move(FreeStoreMove& m);
    int32_t planet_orbit_owner(PlanetIndex p) const;  // 0x3f9270
    void step_free_store_moves(int32_t player);
    void auto_land_free_units(int32_t player);

    // ================= lua::AiScriptHost =================
    bool tf_valid(int32_t tf) override {
        const auto it = tfs.find(tf);
        return it != tfs.end() && it->second.alive;
    }
    int32_t tf_force_count(int32_t tf) override;
    void tf_set_flag(int32_t tf, lua::AiTfFlag flag, bool value) override;
    int32_t tf_produce_force(int32_t tf, const lua::AiArg& stage, bool) override;
    int32_t tf_get_stage(int32_t tf) override {
        const auto it = tfs.find(tf);
        return it == tfs.end() ? -1 : it->second.stage;
    }
    bool tf_all_on_free_store(int32_t tf) override;
    int32_t tf_order(int32_t tf, lua::AiTfOrder order, const lua::AiArg& arg) override;
    // L1P4_LUA_GAPS 1: `ability` = the object type name the galactic binding validated ("" = every member).
    int32_t tf_activate_ability(int32_t tf, std::string_view ability, bool on, const lua::AiArg& arg) override;
    uint32_t activate_filter = gal::kNoIndex;  // tf_order(ActivateAbility): members of this type only
    bool tf_is_raid_capable(int32_t tf) override { return tf_valid(tf); }
    void tf_release_forces(int32_t tf, double fraction) override;
    void tf_release_unit(int32_t tf, ObjectHandle unit) override;
    std::vector<ObjectHandle> tf_units(int32_t tf) override;
    std::string tf_goal_type_name(int32_t tf) override;
    bool tf_collect_free_units(int32_t tf, uint64_t category_mask) override;
    bool status_finished(int32_t status) override {
        const auto it = statuses.find(status);
        return it == statuses.end() || it->second.finished;
    }
    lua::AiStatusResult status_result(int32_t status) override {
        const auto it = statuses.find(status);
        return it == statuses.end() ? lua::AiStatusResult::Nil : it->second.result;
    }
    bool target_valid(int32_t target) override { return target >= 0 && valid_planet(static_cast<PlanetIndex>(target)); }
    ObjectHandle target_object(int32_t target) override {
        return target_valid(target) ? sim.planets()[static_cast<size_t>(target)].object : ObjectHandle{};
    }
    double target_distance(int32_t target, const lua::AiArg& other) override;
    double budget_op(int32_t script, lua::AiBudgetOp op, double amount, std::string_view goal_type) override;
    int32_t budget_wait(int32_t script, lua::AiBudgetOp op, double amount) override {
        const int32_t id = new_status(script, -1, StatusKind::BudgetWait);
        statuses[id].op = op;
        statuses[id].amount = amount;
        return id;
    }
    int32_t free_store_count(int32_t script, int which) override;
    bool free_store_has(int32_t script, ObjectHandle unit) override {
        const int32_t p = script >= kFreeStoreScriptBase ? script - kFreeStoreScriptBase : -1;
        return store.alive(unit) && (p < 0 || store.owner(unit) == p) && !tf_owns(unit);
    }
    bool unit_in_transit(ObjectHandle unit) override {  // 0x68efb0: also with a pending free-store move
        if (planet_of_unit(unit) == kInvalidPlanet) return true;
        for (const FreeStoreMove& m : fs_moves)
            if (!m.done && m.unit == unit) return true;
        return false;
    }
    bool unit_safe(ObjectHandle unit) override;
    bool free_store_move(int32_t script, ObjectHandle unit, const lua::AiArg& destination) override;
    int32_t find_target(int32_t script, const lua::AiFindTargetRequest& r) override;
    int32_t find_stage_area(int32_t, const lua::AiArg& player, const lua::AiArg& target, const lua::AiArg&) override;
    // A2k t02: the Lua _FindStageArea planet at AI level `level` (galactic_ai.cpp); kInvalidPlanet = nil.
    PlanetIndex lua_stage_area(int32_t player, PlanetIndex target, int level);
    int tf_reachability(int32_t tf) const;  // the task force's plan goal Reachability (0 when unknown)
    lua::AiGlobalValue global_value(std::string_view name) override {
        if (gv_store) return gv_store->get(name);  // Int-final: the session-wide table
        const auto it = globals.find(std::string(name));
        return it == globals.end() ? lua::AiGlobalValue{} : it->second;
    }
    void set_global_value(std::string_view name, const lua::AiGlobalValue& v) override {
        if (gv_store) return gv_store->set(name, v);  // Int-final
        if (v.kind == lua::AiGlobalValue::Nil) globals.erase(std::string(name));
        else globals[std::string(name)] = v;
    }
    // --- L1 final (script_remaining.hpp): galactic_ai.cpp ---
    bool tf_add_unit(int32_t tf, ObjectHandle unit) override;
    int32_t tf_refit(int32_t tf, ObjectHandle planet, double budget) override;
    int32_t best_of(int32_t script, int32_t tf, const std::vector<lua::AiArg>& targets, std::string_view perception) override;
    // --- end L1 final ---

    // ================= lua::ScriptMarkupHost (Apply_Markup, SCRIPT_GAPS §5.2) =================
    int32_t markup_target_of_object(int32_t player, ObjectHandle object) override;
    int32_t markup_target_of_player(int32_t caller, int32_t player) override;
    void add_markup(int32_t target, double delta) override;
};

// ================================================================ per-player AI (goal hooks + services)

struct PlayerAi final : GoalModeHooks, ContextModeServices {
    GalacticAiWorld::Impl* w;
    int32_t player;
    const AiPlayerTypeDef* type;
    int32_t difficulty;
    float contrast_mult = 1.0f;
    AiContext ctx{AiMode::Galactic};
    // free store
    std::unique_ptr<lua::LuaVm> fs_vm;
    std::set<objects::ObjectId> known;
    std::vector<PendingBonus> bonuses;
    // reachability cache (per frame, per AI mode 0..5)
    int64_t reach_frame = -1;
    std::vector<std::vector<uint8_t>> reach;
    // AI execution path finder (GALACTIC.md §4): created at first service, serviced by execution_service.
    AiPathFinder paths;
    PlayerAi(GalacticAiWorld::Impl* world, int32_t p, const AiPlayerTypeDef* t, int32_t d);

    // ---- GoalModeHooks ----
    uint32_t target_count() override { return static_cast<uint32_t>(w->sim.planets().size()); }
    bool applies(int32_t target, uint32_t flags) override { return w->applies(player, target, flags); }
    bool blocked(uint32_t goal_type, int32_t target) override;
    bool reachable(int mode, PlanetIndex target);
    // 0x665160 (AI_GALACTIC_3 §8): the reachability flood's planets for a level, flood output order, cached 10 s.
    struct SourceList {
        int64_t frame = -1;
        std::vector<PlanetIndex> planets;
    };
    std::array<SourceList, 6> sources;
    const std::vector<PlanetIndex>& source_planets(int level);
    // A2k t02: the per-target lists of the target planets' PlanetReachabilityClass (0x665160 with destination = the
    // target, ai_stage_flood): one cache per (this AI player, target planet row) and level 0..5, rebuilt when more than
    // 10 x FPS frames old (stamp written before the build). Empty until first used; sized to the planet count.
    std::vector<std::array<SourceList, 6>> stage_lists;
    const std::vector<PlanetIndex>& stage_list(PlanetIndex target, int level);
    // Slot 6 (0x664450) with no exclude / include list: level 4 -> the target; level > 5 or an empty list -> none
    // (kInvalidPlanet); else list[0].
    PlanetIndex stage_of(PlanetIndex target, int level);
    // FreeStore Setup A's per-TF stage (0x7287a0, A2K_T08_RE_ANSWERS Q3.1) [H]: slot 6 with exclude = `chosen` (the
    // stages of the earlier TFs) when RequireUniqueStage, else include = `chosen` when RequireMatchingStage; no result ->
    // the target itself. IgnoreTarget is not read. kInvalidPlanet for an invalid target.
    PlanetIndex setup_stage(PlanetIndex target, int level, const TaskForceDef& tf, std::span<const PlanetIndex> chosen);
    std::vector<PlanetIndex> flood(int mode, std::vector<uint8_t>* reached);
    bool evaluate(const std::string& function, int32_t target, double& out) override {
        return w->evaluate(player, ctx.opponent, function, target, out);
    }
    bool accept(Goal& g) override;
    bool plan_activatable(Goal& g) override {
        if (!(g.pp_valid && g.plan != kAiNone && ctx.templates.plan_value(g.plan) > 0 && w->resolve_pp(*this, g))) return false;
        // [M] the re-solve rewrites the PP's selection (+0x58), which AllowedTime (0x692860 -> 0x6c0450) reads live.
        // a2k t01: 0x6c4a50 also copies the re-solved net cost and ETA to the goal (G +0x78 := PP +0x44, G +0x7C := PP +0x48,
        // A2H_T06_TACTICAL_ETA §2.1) [H], so Resolve's fundable test and budget add (0x610d30) see the new selection's cost.
        if (const auto it = w->pps.find(g.pp_handle); it != w->pps.end()) {
            g.cost = it->second.net;
            g.eta = it->second.eta;
            g.nominal_time = w->pp_nominal_time(it->second);
        }
        return true;
    }
    bool plan_invalid(const Goal& g) override;
    bool replaceable(const Goal& g) override;
    void refresh_plan(Goal& g) override { w->reserve_pp_units(g.pp_handle); }
    void suspend_reservations(Goal& g) override { w->suspend_pp_units(g.pp_handle); }
    bool uses_budget() override { return true; }
    int32_t category_of(const Goal& g) const { return w->defs.goals[g.type].category; }
    float time_limit(const Goal& g) const { return w->defs.goals[g.type].time_limit; }
    bool can_fund(const Goal& g) override;
    float category_credits(int32_t category) override { return ctx.budget.available(category); }
    void budget_add(Goal& g) override;
    void budget_release(Goal& g) override;
    void budget_weights() override;
    void apply_template_budget();  // §9.5 members + reset flag after a template pass
    std::vector<int32_t> category_order() override;
    void queue_production(Goal& g) override;
    void instantiate(Goal& g) override { g.running = w->instantiate(*this, g); }
    void end_plan(Goal& g, bool by_goal_system) override {
        if (g.running >= 0) w->end_plan(g.running, by_goal_system);
    }
    void drop_potential_plan(Goal& g) override;
    float elapsed_eta(const Goal& g) override {  // §1.6: 0x525200 [H] + elapsed when > 0 [H] (galactic_goal_time.hpp)
        return galactic_maintenance_eta(w->goal_production_time(g.running), g.created, w->now);
    }
    int32_t quota_players() override;

    // ---- ContextModeServices ----
    void first_service(int64_t) override;
    void template_init(int64_t) override;
    void planning_service(int64_t) override { w->service_plans(player); }
    void execution_service(int64_t) override;
    void templates_changed(int64_t) override;
};

}  // namespace alamo::ai
