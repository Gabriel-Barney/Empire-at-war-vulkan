// A2 tests: AI framework rules (cadence, quota, desire, plan pick, solver, templates, budget, commit) and the
// galactic AI on the Lua bridge: a synthetic AI-vs-AI campaign (no game data; per-frame CRCs identical for
// 1/2/4/15 threads, golden tests/golden/ai_galactic_synthetic_crc.txt) and, with ALAMO_FOC_DATA, the shipped
// FoC AI data + plan scripts.
//
//   alamo_ai_galactic_tests [--write-golden]
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "alamo/ai/ai_context.hpp"
#include "alamo/ai/ai_definitions.hpp"
#include "alamo/ai/ai_free_store_path.hpp"
#include "alamo/ai/ai_goals.hpp"
#include "alamo/ai/ai_lua_runtime.hpp"
#include "alamo/ai/ai_plans.hpp"
#include "alamo/ai/galactic_ai.hpp"
#include "alamo/ai/perception_equations.hpp"
#include "alamo/core/bytes.hpp"
#include "alamo/core/compress.hpp"
#include "alamo/core/file_provider.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/core/sim_rng.hpp"
#include "alamo/galactic/campaign.hpp"
#include "alamo/galactic/conflicts.hpp"
#include "alamo/galactic/fleet_path.hpp"
#include "alamo/lua/lua_vm.hpp"
#include "alamo/lua/players.hpp"
#include "alamo/objects/game_object_types.hpp"
#include "alamo/objects/object_store.hpp"
#ifdef ALAMO_AI_HAVE_VFS
#include "alamo/data/game_database.hpp"
#include "alamo/galactic/galactic_rules.hpp"
#include "alamo/objects/object_types.hpp"
#include "alamo/vfs/vfs.hpp"
#endif

using namespace alamo;
using namespace alamo::ai;
namespace gal = alamo::galactic;

static int g_fail = 0, g_checks = 0;
#define CHECK(c)                                                                  \
    do {                                                                          \
        ++g_checks;                                                               \
        if (!(c)) {                                                               \
            ++g_fail;                                                             \
            std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #c);            \
        }                                                                         \
    } while (0)

static bool g_write_golden = false;

// ================================================================ framework unit tests

static void test_cadence() {
    // Service clock: next = NOW + max(1, (int)(interval x 30)).
    ServiceClock c{0.1f};
    CHECK(c.due(0));
    c.reschedule(0);
    CHECK(c.next == 3);
    ServiceClock g{0.0f};
    g.reschedule(5);
    CHECK(g.next == 6);
    ServiceClock p{10.0f};
    p.reschedule(7);
    CHECK(p.next == 307);

    // Context order and intervals over 600 frames with stub services.
    struct Stub final : ContextModeServices {
        std::vector<std::string> trace;
        void first_service(int64_t) override { trace.push_back("first"); }
        void template_init(int64_t) override { trace.push_back("tinit"); }
        void perception_service(int64_t n) override { trace.push_back("P" + std::to_string(n)); }
        void planning_service(int64_t n) override { trace.push_back("L" + std::to_string(n)); }
        void execution_service(int64_t n) override { trace.push_back("E" + std::to_string(n)); }
        void templates_changed(int64_t) override {}
    } stub;
    AiDefinitions defs;
    PlanLibrary lib;
    AiContext ctx(AiMode::Galactic);
    ctx.templates.init(defs, lib, {});
    struct NoHooks final : GoalModeHooks {
        uint32_t target_count() override { return 0; }
        bool applies(int32_t, uint32_t) override { return false; }
        bool blocked(uint32_t, int32_t) override { return false; }
        bool evaluate(const std::string&, int32_t, double&) override { return false; }
        bool accept(Goal&) override { return false; }
        bool plan_activatable(const Goal&) override { return false; }
        bool plan_invalid(const Goal&) override { return true; }
        bool replaceable(const Goal&) override { return true; }
        void refresh_plan(Goal&) override {}
        bool uses_budget() override { return false; }
        bool can_fund(const Goal&) override { return true; }
        float category_credits(int32_t) override { return 1.0f; }
        void budget_add(Goal&) override {}
        void budget_release(Goal&) override {}
        void budget_weights() override {}
        std::vector<int32_t> category_order() override { return {}; }
        void queue_production(Goal&) override {}
        void instantiate(Goal&) override {}
        void end_plan(Goal&, bool) override {}
        void drop_potential_plan(Goal&) override {}
        float elapsed_eta(const Goal&) override { return 0.0f; }
        int32_t quota_players() override { return 2; }
    } hooks;
    SimRng rng(1);
    LearningSystem dummy;
    ctx.goals.init(defs, {}, &hooks, &ctx.learning, &ctx.templates, &rng, {});
    ctx.attach(&stub, [](const std::string&, double&) { return false; });
    for (int64_t f = 1; f <= 600; ++f) ctx.service(f, true, false);
    CHECK(stub.trace.size() >= 3 && stub.trace[0] == "first" && stub.trace[1] == "tinit" && stub.trace[2] == "P1");
    CHECK(ctx.service_counts[0] == 2);    // perception: frames 1, 301
    CHECK(ctx.service_counts[1] == 600);  // goal: every frame
    CHECK(ctx.service_counts[2] == 200);  // planning: every 3 frames
    CHECK(ctx.service_counts[3] == 200);  // execution
    CHECK(ctx.service_counts[4] == 2);    // learning
    CHECK(ctx.service_counts[5] == 600);  // template
    // Order inside a frame: P before L before E on frame 1.
    const auto pos = [&](const std::string& s) { return std::find(stub.trace.begin(), stub.trace.end(), s) - stub.trace.begin(); };
    CHECK(pos("P1") < pos("L1") && pos("L1") < pos("E1"));
    // Disabled: nothing runs, except galactic Planning while auto-resolve is busy.
    const auto before = ctx.service_counts;
    ctx.service(601, false, false);
    CHECK(ctx.service_counts == before);
    ctx.service(602, false, true);
    CHECK(ctx.service_counts[2] == before[2] + 1 && ctx.service_counts[1] == before[1]);
    std::printf("cadence: %zu trace entries\n", stub.trace.size());
}

static void test_desire_and_hysteresis() {
    Goal g;
    g.desire = 10.0f;
    CHECK(GoalSystem::effective_desire(g) == 10.0f);  // proposals: no bonus
    g.state = GoalState::Active;
    const float d = 10.0f;
    CHECK(GoalSystem::effective_desire(g) == d + (1.0f - (d - 0.01f) / 39.99f) * 1.6f);
    g.desire = 40.0f;
    CHECK(GoalSystem::effective_desire(g) == 40.0f);
    g.desire = 0.005f;
    CHECK(GoalSystem::effective_desire(g) == 0.005f);
    // Learning: recent failures with expiry, permanent records, zero duration ignored.
    LearningSystem l;
    l.record_goal_outcome(3, 7, 1, false, 10.0f, 100);  // expiry 100 + 300
    l.record_goal_outcome(3, 7, 1, true, 10.0f, 100);
    l.record_goal_outcome(3, 7, 1, false, -1.0f, 100);  // permanent
    l.record_goal_outcome(3, 8, 1, false, 0.0f, 100);   // no recent record
    CHECK(l.recent_goal_failures(3, 7, 200) == 2);
    CHECK(l.recent_goal_failures(3, 7, 400) == 1);
    CHECK(l.recent_goal_failures(3, 8, 200) == 0);
    CHECK(l.recent_plan_failures(1, 7, 200) == 2);
    CHECK(l.plan_success_ratio(1) == 0.25);
    CHECK(l.plan_success_ratio(99) == 1.0);
    l.prune(400);
    CHECK(l.recent_goal_failures(3, 7, 400) == 1);
    // Desire formula: Evaluate + (double)((float)c1*w1 + (float)c2*w2).
    const double v = 12.5, w1 = -5.0, w2 = -8.0;
    const double desire = v + static_cast<double>(2.0f * static_cast<float>(w1) + 1.0f * static_cast<float>(w2));
    CHECK(desire == -5.5);
}

static void test_plan_pick_and_solver() {
    // One synced draw: r = range_float(0, total), first cumulative >= r.
    SimRng a(1234), b(1234);
    const double ratios[] = {1.0, 0.0, 0.5};  // weights 2, 1, 1.5 -> cum 2, 3, 4.5
    const uint32_t k = pick_weighted(ratios, a);
    const float r = b.range_float(0.0f, 4.5f);
    const uint32_t want = r <= 2.0f ? 0u : (r <= 3.0f ? 1u : 2u);
    CHECK(k == want && a.state() == b.state() && a.draws() == 1);
    // Solver core (AI_GALACTIC_2 §1.2): strictly highest, ties -> lowest index, 1e18 veto (permanent erases),
    // 50 % same-type rule, setups A/B/C in order, finalize.
    struct M final : PpMarshaller {
        std::vector<float> s;
        std::vector<uint32_t> picked;
        std::string order;
        uint32_t limit = 3;
        void setup_a() override { order += "A"; }
        void setup_b() override { order += "B"; }
        void setup_c() override { order += "C"; }
        float score(uint32_t i) override { return picked.size() >= limit ? kAiBigFloat : s[i]; }
        void commit(uint32_t i) override { picked.push_back(i); }
        void finalize(bool& ok) override { ok = ok && picked.size() == limit; }
    } m;
    m.s = {1.0f, 5.0f, 5.0f, kAiBigFloat};
    const uint32_t types[] = {3, 7, 7, 9};
    PpMarshaller* ms[] = {&m};
    PpSolverIo io;
    io.candidate_count = [] { return size_t{4}; };
    io.type_of = [&types](uint32_t c) { return types[c]; };
    std::vector<uint32_t> picks;
    const bool solved = run_pp_solver(ms, io, picks);
    // Pick 1 (tie 1/2 -> lowest); then type 7 fills 100 % -> 1 and 2 vetoed while 0 remains -> 0; then types 3 and
    // 7 both fill 50 %: 0 and 1 are vetoed in candidate order, 2 is the only viable one left and stays -> 2.
    CHECK(solved && m.order == "ABC" && picks.size() == 3 && picks[0] == 1 && picks[1] == 0 && picks[2] == 2);
    CHECK(pp_marshaller_order(AiMode::Galactic).size() == 9 && pp_marshaller_order(AiMode::Galactic)[1] == PpMarshallerKind::Budget &&
          pp_marshaller_order(AiMode::Land)[2] == PpMarshallerKind::Reinforcement &&
          pp_marshaller_order(AiMode::Space)[1] == PpMarshallerKind::TechTree);
    // Team specs (§1.3): a,0 -> max a; percents renormalised over the plan.
    {
        PlanDefinition pd;
        TaskForceDef ta, tb;
        CHECK(parse_task_force({"A", "Frigate = 3,0", "Corvette = 30%"}, ta) && parse_task_force({"B", "Infantry = 10%"}, tb));
        CHECK(ta.teams[0].min == 3 && ta.teams[0].max == 3 && ta.teams[1].percent && ta.teams[1].min == 0 && ta.teams[1].max == 0);
        pd.task_forces = {ta, tb};
        renormalise_percents(pd);
        CHECK(pd.task_forces[0].teams[1].percent_value == 75.0f && pd.task_forces[1].teams[0].percent_value == 25.0f);
    }
    // Plan events (§5.2).
    PlanEvent ev{};
    CHECK(plan_event_name(PlanEvent::FleetDestroyed) == "Fleet_Destoyed" && plan_event_code("no_units_remaining", ev) &&
          ev == PlanEvent::NoUnitsRemaining && static_cast<int>(PlanEvent::ProductionFailed) == 4);
    // TaskForce entry syntax.
    TaskForceDef tf;
    CHECK(parse_task_force({"SpaceForce", "MinimumTotalSize = 4", "MinimumTotalForce = 2500",
                            "Frigate | Capital | Corvette = 100%", "DenyHeroAttach", "-Death_Star", "Infantry = 0,4"},
                           tf));
    CHECK(tf.name == "SpaceForce" && tf.minimum_total_size == 4 && tf.minimum_total_force == 2500.0f);
    CHECK(tf.teams.size() == 2 && tf.teams[0].percent && tf.teams[0].names.size() == 3 && tf.teams[1].min == 0 &&
          tf.teams[1].max == 4 && tf.deny_hero_attach && tf.excluded_types.size() == 1 && tf.percent_based());
}

static const char* kSynGoals = R"(<Goals>
  <Build_Space_Forces><AIGoalApplicationFlags>Global</AIGoalApplicationFlags><GameMode>Galactic</GameMode>
    <Category>Offensive</Category><Reachability>Any</Reachability><Time_Limit>200</Time_Limit></Build_Space_Forces>
  <Conquer_Planet><AIGoalApplicationFlags>Enemy | Neutral</AIGoalApplicationFlags><GameMode>Galactic</GameMode>
    <Category>Offensive</Category><Reachability>Enemy_destination</Reachability><Time_Limit>240</Time_Limit>
    <Build_Time_Delay_Tolerance>1.3</Build_Time_Delay_Tolerance><Is_Like>Build_Space_Forces</Is_Like>
    <Tracking_Duration>300</Tracking_Duration><Per_Failure_Desire_Adjust>-1.0</Per_Failure_Desire_Adjust>
    <Activation_Tracking_Duration>60</Activation_Tracking_Duration>
    <Per_Activation_Failure_Desire_Adjust>-2.0</Per_Activation_Failure_Desire_Adjust></Conquer_Planet>
  <Upgrade_Starbase><AIGoalApplicationFlags>Friendly</AIGoalApplicationFlags><GameMode>Galactic</GameMode>
    <Category>Infrastructure</Category><Reachability>Friendly_Only</Reachability><Time_Limit>60</Time_Limit></Upgrade_Starbase>
  <Land_Thing><AIGoalApplicationFlags>Enemy</AIGoalApplicationFlags><GameMode>Land</GameMode>
    <Category>Tactical_Targeted</Category></Land_Thing>
</Goals>)";
static const char* kSynSet = R"(<FunctionSet>
  <A><Goal>Build_Space_Forces</Goal><Function>Allow_Space</Function></A>
  <B><Goal>Conquer_Planet</Goal><Function>Conquer_Desire</Function></B>
  <C><Goal>Upgrade_Starbase</Goal><Function>Needs_Starbase_Upgrade</Function></C>
  <D><Goal>Land_Thing</Goal><Function>One</Function></D>
</FunctionSet>)";
static const char* kSynTemplates = R"(<AITemplates>
  <Syn_Default><Priority>1</Priority><Trigger>One</Trigger>
    <Budget><Offensive>Offensive_Budget</Offensive><Infrastructure>Infra_Budget</Infrastructure><Defensive>Zero</Defensive></Budget>
    <Turn_On><Goals><Category>Offensive</Category><Category>Infrastructure</Category></Goals>
      <Plans><Goal_Category>Offensive</Goal_Category><Goal_Category>Infrastructure</Goal_Category></Plans></Turn_On>
  </Syn_Default>
  <Syn_Rich><Priority>3</Priority><Trigger>Is_Rich</Trigger>
    <Turn_Off><Goals><Goal_Type>Upgrade_Starbase</Goal_Type></Goals></Turn_Off></Syn_Rich>
</AITemplates>)";
static const char* kSynPlayer = R"(<AIPlayerType><Name>%s</Name>
  <GalacticFreeStoreScript>SynFreeStore</GalacticFreeStoreScript>
  <GoalProposalFunctionSets>SynSet</GoalProposalFunctionSets>
  <Templates><Galactic>Syn_Default, Syn_Rich</Galactic></Templates>
  <Difficulty_Adjustments><Easy>Easy_Default</Easy><Normal>Normal_Default</Normal><Hard>Hard_Default</Hard></Difficulty_Adjustments>
</AIPlayerType>)";

static void test_definitions_and_templates() {
    AiDefinitions d;
    d.add_goals(kSynGoals, "goals.xml");
    d.link();
    d.add_function_set("SynSet", kSynSet, "SynSet.xml");
    d.add_templates(kSynTemplates, "t.xml");
    char buf[1024];
    std::snprintf(buf, sizeof buf, kSynPlayer, "SynEmpire");
    d.add_players(buf, "p.xml");
    CHECK(d.goals.size() == 4 && d.diagnostics.empty());
    const uint32_t cq = d.find_goal("conquer_planet");
    CHECK(cq == 1 && d.goals[cq].application == (app::Enemy | app::Neutral) && d.goals[cq].time_limit == 240.0f);
    CHECK(d.goals[cq].reachability == Reachability::EnemyDestination && d.goals[cq].is_like.size() == 1);
    CHECK(d.goals[3].mode == AiMode::Land && d.goals[2].reachability == Reachability::FriendlyOnly);
    CHECK(d.find_set("synset") && d.find_set("SynSet")->entries.size() == 4);
    const AiPlayerTypeDef* p = d.find_player("SynEmpire");
    CHECK(p && p->templates[0].size() == 2 && p->free_store_script[0] == "SynFreeStore");
    // Template switches: base turns Offensive + Infrastructure on (priority 1); Syn_Rich (priority 3)
    // turns Upgrade_Starbase off while active.
    PlanLibrary lib;
    TemplateSystem t;
    t.init(d, lib, {0u, 1u});
    bool rich = false;
    auto eval = [&rich](const std::string& eq, double& out) {
        out = eq == "One" ? 1.0 : (eq == "Is_Rich" ? (rich ? 1.0 : 0.0) : 0.0);
        return true;
    };
    t.first_service(eval);
    CHECK(t.goal_value(cq) == 1 && t.goal_value(2) == 1 && t.goal_value(3) == -1);
    CHECK(t.budget_equation(d.find_category("Offensive")) == "Offensive_Budget");
    rich = true;
    t.service(eval);  // count = 1: one template checked per frame (cursor: base)
    t.service(eval);  // Syn_Rich
    CHECK(t.active().size() == 2 && t.goal_value(2) == -3 && t.goal_value(cq) == 1);
    rich = false;
    t.service(eval);
    CHECK(t.active().size() == 1 && t.goal_value(2) == 1);
    // Budget (AI_GALACTIC_2 §3): init, add goal, tick (asymmetric branch, drift), reweight, can fund, release.
    AiBudget b;
    b.reset(3);
    const float raw[] = {3.0f, -1.0f, 1.0f};
    b.init(raw, 1000.0f);
    CHECK(b.weight(0) == 0.75f && b.weight(1) == 0.0f && b.weight(2) == 0.25f);
    CHECK(b.available(0) == 750.0f && b.pool(0) == 750.0f && b.rate(0) == 0.0f);
    CHECK(b.can_fund(0, 750.0f, 10.0f, -1.0f) && !b.can_fund(0, 751.0f, 10.0f, -1.0f));
    CHECK(b.add_goal(0, 7, 500.0f, 30.0f, 20.0f));
    const AiBudget::GoalEntry* e = b.goal(0, 7);
    CHECK(e && e->req == 500.0f && e->h == 20.0f && e->c == 500.0f && b.available(0) == 250.0f && e->q == 0.0f);
    // Tick: income 450 over T = 45 s -> rate 10; I = 0.75 x 10; share 0 (Iprev 0); x = 500/20 = 25 -> a = 1125.
    b.tick(450.0f, 45.0f, 1450.0f);
    e = b.goal(0, 7);
    CHECK(e && e->a == 25.0f * 45.0f && e->h == 0.0f && b.category(0).I == 7.5f);
    // P0 = 250 + (0 - 25) x 45 + 500 + 1125 = 750; Σ = 750 + 0 + 250 = 1000 -> drift (1450 - 1000) x w.
    CHECK(b.pool(0) == 750.0f + 450.0f * 0.75f && b.pool(2) == 250.0f + 450.0f * 0.25f);
    b.reweight(raw);  // A = P - a, c = 0, needs re-add
    e = b.goal(0, 7);
    CHECK(e && e->readd && e->c == 0.0f && b.available(0) == b.pool(0) - 1125.0f && b.rate(0) == 7.5f);
    b.add_goal(0, 7, 0.0f, 0.0f, -1.0f);  // re-add: need = 500 + 0 - 1125 < 0 -> given back
    e = b.goal(0, 7);
    CHECK(e && !e->readd && e->a == 500.0f && e->c == 0.0f && b.available(0) == b.pool(0) - 500.0f);
    CHECK(b.can_fund_goal(0, 7));
    const float before = b.available(0);
    b.release(0, 7, 0.0f);
    CHECK(!b.has_goal(0, 7) && b.available(0) == before + 500.0f);
    const float zero[] = {0.0f, 0.0f, 0.0f};
    b.reweight(zero);
    CHECK(std::fabs(b.weight(1) - 1.0f / 3.0f) < 1e-7f);
}

// ================================================================ synthetic campaign

class MemoryFiles final : public FileProvider {
public:
    void add(const std::string& path, const std::string& text) { files_[normalize_path(path)] = text; }
    bool read(std::string_view path, std::vector<uint8_t>& out) const override {
        const auto it = files_.find(normalize_path(path));
        out.clear();
        if (it == files_.end()) return false;
        out.assign(it->second.begin(), it->second.end());
        return true;
    }
    bool exists(std::string_view path) const override { return files_.count(normalize_path(path)) != 0; }
    std::string origin(std::string_view path) const override { return exists(path) ? "memory" : ""; }
    std::vector<std::string> list(std::string_view folder) const {
        std::vector<std::string> out;
        const std::string f = normalize_path(folder);
        for (const auto& [k, v] : files_)
            if (k.size() > f.size() && k.compare(0, f.size(), f) == 0 && k.find('\\', f.size()) == std::string::npos) out.push_back(k);
        return out;
    }

private:
    std::map<std::string, std::string> files_;
};

static const char* kSynBase = R"(
function ScriptExit()
  _ScriptExit()
  if GetThreadID() >= 0 then coroutine.yield(false) end
end
function PumpEvents()
  coroutine.yield(true)
  local e = GetEvent()
  while e do
    local p = GetEvent.Params()
    if p then e(unpack(p)) else e() end
    e = GetEvent()
  end
end
function Sleep(t)
  local s = GetCurrentTime()
  while GetCurrentTime() - s < t do PumpEvents() end
end
function BlockOnCommand(b, max)
  PumpEvents()
  if not b then return nil end
  local s = GetCurrentTime()
  repeat
    PumpEvents()
    if max and GetCurrentTime() - s > max then return nil end
  until b.IsFinished() == true
  return b.Result()
end
function TestValid(w)
  if w == nil then return false end
  if w.Is_Valid == nil then return false end
  return w.Is_Valid()
end
function Flush_G()
  local del = {}
  for k, v in pairs(_G) do
    if type(v) == "table" and k ~= "_LOADED" and k ~= "coroutine" and k ~= "string" and k ~= "table" and k ~= "_G" and k ~= "LuaWrapperMetaTable" then
      table.insert(del, k)
    end
  end
  for i, k in pairs(del) do _G[k] = nil end
end
)";
static const char* kSynAICommands = R"(
require("SynBase")
function Base_Definitions()
  MinContrastScale = 1.1
  MaxContrastScale = 1.5
  PerFailureContrastAdjust = 0.1
  AITarget = nil
  Target = nil
  PlayerObject = nil
  Budget = nil
  if TaskForce and type(TaskForce) == "table" then
    for i, d in pairs(TaskForce) do
      if type(d) == "table" and type(d[1]) == "string" then _G[d[1]] = nil end
    end
  end
  PlanDefinitionLoad = nil
  if Definitions then Definitions() end
end
)";
static const char* kPlanBuildSpace = R"(
require("SynAICommands")
ScriptPoolCount = 2
function Definitions()
  Category = "Build_Space_Forces"
  IgnoreTarget = true
  TaskForce = { { "ReserveForce", "Frigate | Corvette = 1,2" } }
  AllowFreeStoreUnits = false
end
function ReserveForce_Thread()
  ReserveForce.Set_As_Goal_System_Removable(false)
  BlockOnCommand(ReserveForce.Produce_Force())
  ReserveForce.Set_Plan_Result(true)
  ScriptExit()
end
)";
static const char* kPlanConquer = R"(
require("SynAICommands")
ScriptPoolCount = 2
function Definitions()
  Category = "Conquer_Planet"
  MinContrastScale = 1.2
  TaskForce = {
    { "SpaceForce", "MinimumTotalSize = 2", "Frigate | Corvette = 100%" },
    { "GroundForce", "Infantry = 2" }
  }
  RequiredCategories = { "Infantry" }
  SpaceSecured = false
  LandSecured = false
end
function SpaceForce_Thread()
  SpaceForce.Set_Plan_Result(true)
  BlockOnCommand(SpaceForce.Produce_Force(SpaceForce.Get_Stage()))
  if not BlockOnCommand(SpaceForce.Move_To(Target)) then
    SpaceForce.Set_Plan_Result(false)
    ScriptExit()
  end
  SpaceSecured = true
  while not LandSecured do Sleep(5) end
  SpaceForce.Release_Forces(1.0)
end
function GroundForce_Thread()
  BlockOnCommand(GroundForce.Produce_Force(GroundForce.Get_Stage()))
  while not SpaceSecured do Sleep(5) end
  BlockOnCommand(GroundForce.Launch_Units())
  BlockOnCommand(GroundForce.Move_To(Target))
  local st = GroundForce.Invade()
  if st and BlockOnCommand(st) then
    GroundForce.Set_As_Goal_System_Removable(false)
  else
    GroundForce.Set_Plan_Result(false)
  end
  LandSecured = true
  GroundForce.Release_Forces(1.0)
  ScriptExit()
end
function SpaceForce_No_Units_Remaining()
  SpaceForce.Set_Plan_Result(false)
  ScriptExit()
end
function GroundForce_Production_Failed(tf, t)
  ScriptExit()
end
)";
static const char* kPlanUpgrade = R"(
require("SynAICommands")
ScriptPoolCount = 4
function Definitions()
  Category = "Upgrade_Starbase"
  IgnoreTarget = true
  TaskForce = { { "BaseForce", "SB1 = 1" } }
end
function BaseForce_Thread()
  BaseForce.Set_As_Goal_System_Removable(false)
  BlockOnCommand(BaseForce.Produce_Force())
  BaseForce.Set_Plan_Result(true)
  GlobalValue.Set("upgrades", (GlobalValue.Get("upgrades") or 0) + 1)
  ScriptExit()
end
function BaseForce_Production_Failed(tf, t)
  ScriptExit()
end
)";
static const char* kSynFreeStore = R"(
require("SynBase")
function Base_Definitions()
  ServiceRate = 8
  UnitServciceRate = 8
  Added = 0
end
function main()
  while true do
    FreeStoreService()
    PumpEvents()
  end
end
function FreeStoreService()
  Count = FreeStore.Get_Object_Count()
end
function On_Unit_Added(object)
  Added = Added + 1
end
)";

enum SynType : uint32_t { tPlanet, tSB1, tSB2, tFrigate, tCorvette, tInfantry, tBarracks, tTypes };

static gal::GalacticRules syn_rules() {
    gal::GalacticRules r;
    gal::FactionRules emp;
    emp.name = "Empire";
    emp.can_control_planets = emp.is_playable = true;
    gal::FactionRules reb = emp;
    reb.name = "Rebel";
    gal::FactionRules neu;
    neu.name = "Neutral";
    neu.is_neutral = true;
    r.factions = {emp, reb, neu};
    r.difficulties = {{"Normal_Default", 1.0f, 1.0f}};
    r.types.resize(tTypes);
    r.types[tPlanet].name = "SynPlanet";
    r.types[tPlanet].kind = gal::UnitKind::Planet;
    r.types[tPlanet].credit_value = 40;
    auto unit = [&r](uint32_t id, const char* name, gal::UnitKind k, int32_t cost, int32_t secs, float power) -> gal::TypeInfo& {
        gal::TypeInfo& t = r.types[id];
        t.name = name;
        t.kind = k;
        t.build_cost = cost;
        t.build_time_seconds = secs;
        t.ai_combat_power = power;
        return t;
    };
    unit(tSB1, "SB1", gal::UnitKind::StarBase, 900, 10, 600.0f).base_level = 1;
    gal::TypeInfo& sb2 = unit(tSB2, "SB2", gal::UnitKind::StarBase, 1500, 15, 1200.0f);
    sb2.base_level = 2;
    sb2.required_star_base = 1;
    gal::TypeInfo& fr = unit(tFrigate, "Frigate", gal::UnitKind::SpaceUnit, 700, 12, 1000.0f);
    fr.required_star_base = 1;
    fr.hyperspace = true;
    fr.hyperspace_speed = 2.0f;
    gal::TypeInfo& co = unit(tCorvette, "Corvette", gal::UnitKind::SpaceUnit, 400, 8, 500.0f);
    co.required_star_base = 1;
    co.hyperspace = true;
    co.hyperspace_speed = 3.0f;
    gal::TypeInfo& inf = unit(tInfantry, "Infantry", gal::UnitKind::GroundUnit, 250, 6, 200.0f);
    inf.required_ground_base = 1;
    inf.hyperspace = true;  // its own transport (synthetic)
    inf.hyperspace_speed = 1.5f;
    unit(tBarracks, "Barracks", gal::UnitKind::GroundStructure, 500, 10, 0.0f);
    return r;
}

static gal::GalacticSetup syn_setup() {
    gal::GalacticSetup s;
    GalaxyMap& m = s.map;
    m.campaign_name = "SyntheticAI";
    const int n = 8;
    for (int i = 0; i < n; ++i) {
        m.planets.add("P" + std::to_string(i));
        m.planets.position[static_cast<size_t>(i)] = {static_cast<float>(i) * 60.0f, static_cast<float>((i % 2) * 40), 0.0f};
        m.planets.flags[static_cast<size_t>(i)] = kPlanetHasPlanetBehavior;
        m.planets.max_space_base[static_cast<size_t>(i)] = 2;
        m.planets.max_ground_base[static_cast<size_t>(i)] = 2;
    }
    for (int i = 0; i + 1 < n; ++i) {
        const uint32_t r = m.routes.add("R" + std::to_string(i));
        m.routes.point_a[r] = static_cast<PlanetIndex>(i);
        m.routes.point_b[r] = static_cast<PlanetIndex>(i + 1);
        m.routes.hs_speed_factor[r] = 4.0f;
        m.routes.credit_gain_factor[r] = 0.0f;
    }
    s.planet_type.assign(static_cast<size_t>(n), tPlanet);
    gal::PlayerSetup e;
    e.faction = 0;
    e.ai = true;
    e.difficulty = 0;
    e.credits = 6000.0f;
    gal::PlayerSetup r = e;
    r.faction = 1;
    gal::PlayerSetup neu;
    neu.faction = 2;
    s.players = {e, r, neu};
    s.forces = {{0, 0, tSB1}, {0, 0, tBarracks}, {0, 0, tFrigate}, {0, 1, tInfantry}, {1, 7, tSB1}, {1, 7, tBarracks}, {1, 7, tFrigate}, {1, 6, tInfantry}};
    return s;
}

static void syn_data(MemoryFiles& files, AiDefinitions& defs, EquationSet& eqs, std::vector<std::string>& plan_files) {
    files.add("Data\\Scripts\\Library\\SynBase.lua", kSynBase);
    files.add("Data\\Scripts\\Library\\SynAICommands.lua", kSynAICommands);
    files.add("Data\\Scripts\\AI\\BuildSpace.lua", kPlanBuildSpace);
    files.add("Data\\Scripts\\AI\\Conquer.lua", kPlanConquer);
    files.add("Data\\Scripts\\AI\\UpgradeStarbase.lua", kPlanUpgrade);
    files.add("Data\\Scripts\\FreeStore\\SynFreeStore.lua", kSynFreeStore);
    defs.add_goals(kSynGoals, "goals.xml");
    defs.link();
    defs.add_function_set("SynSet", kSynSet, "SynSet.xml");
    defs.add_templates(kSynTemplates, "t.xml");
    char buf[1024];
    for (const char* n : {"SynEmpire", "SynRebel"}) {
        std::snprintf(buf, sizeof buf, kSynPlayer, n);
        defs.add_players(buf, "p.xml");
    }
    eqs.add("One", "1.0");
    eqs.add("Zero", "0.0");
    eqs.add("Is_Rich", "Variable_Self.CreditsUnnormalized > 20000");
    eqs.add("Allow_Space", "(Variable_Self.CreditsUnnormalized >= 1500) * 2 + (Game.EnemyForce.SpaceTotalUnnormalized > Game.FriendlyForce.SpaceTotalUnnormalized)");
    eqs.add("Conquer_Desire",
            "(3 + 2 * (Variable_Target.EnemyForce.HasSpaceForce == 0) + Variable_Target.BaseIncome) * "
            "(Variable_Self.SpaceTotalUnnormalized >= 1500)");
    eqs.add("Needs_Starbase_Upgrade",
            "(Variable_Target.StarbaseLevel < Variable_Target.MaxStarbaseLevel) * (Variable_Self.CreditsUnnormalized >= 1000) * 4");
    eqs.add("Offensive_Budget", "5 + 10 * Variable_Self.PlanetsControlled");
    eqs.add("Infra_Budget", "3");
    eqs.link();
    plan_files = list_plan_files([&files](std::string_view glob) {
        const std::string g(glob);
        return files.list(g.substr(0, g.find('*')));
    });
}

static ParallelFor thread_pf(unsigned threads) {
    return [threads](uint32_t count, uint32_t grain, const RangeFn& fn) {
        if (count == 0) return;
        const uint32_t gr = std::max<uint32_t>(1, grain);
        const uint32_t chunks = (count + gr - 1) / gr;
        std::atomic<uint32_t> next{0};
        auto worker = [&] {
            for (;;) {
                const uint32_t c = next.fetch_add(1);
                if (c >= chunks) break;
                fn(c * gr, std::min(count, c * gr + gr));
            }
        };
        const unsigned extra = std::min<unsigned>(std::max(1u, threads), chunks) - 1;
        std::vector<std::thread> ts;
        for (unsigned t = 0; t < extra; ++t) ts.emplace_back(worker);
        worker();
        for (auto& t : ts) t.join();
    };
}

struct CampaignResult {
    std::vector<uint64_t> crc;  // per frame
    std::string summary;
    std::vector<std::string> decisions;
    std::vector<std::string> errors;
    std::vector<int32_t> owned;
    uint64_t orders = 0, rejected = 0, conflicts = 0;
};

static CampaignResult run_synthetic(unsigned threads, int32_t frames) {
    CampaignResult out;
    MemoryFiles files;
    AiDefinitions defs;
    EquationSet eqs;
    std::vector<std::string> plan_files;
    syn_data(files, defs, eqs, plan_files);
    const gal::GalacticRules rules = syn_rules();
    objects::GameObjectTypes types;
    objects::IdCounter ids;
    objects::ObjectStore store(objects::ModeKind::Galactic, objects::StoreKind::Primary, types, &ids);
    GameRandom rng(4242u, 77u);
    gal::GalacticSim sim(rules, store, rng.synced());
    sim.set_parallel_for(thread_pf(threads));
    std::vector<std::string> diag;
    if (!sim.start(syn_setup(), &diag)) {
        std::printf("synthetic: start failed\n");
        ++g_fail;
        return out;
    }
    gal::ConflictRules crules;
    gal::ConflictManager conflicts(sim, crules, rng.synced());
    conflicts.attach();
    std::vector<GalacticAiTypeInfo> tinfo(tTypes);
    tinfo[tFrigate].categories = {"FRIGATE"};
    tinfo[tCorvette].categories = {"CORVETTE"};
    tinfo[tInfantry].categories = {"INFANTRY"};
    tinfo[tSB1].categories = tinfo[tSB2].categories = {"STRUCTURE"};
    GalacticAiConfig cfg;
    cfg.players = {{0, "SynEmpire", 1}, {1, "SynRebel", 1}};
    cfg.difficulties = {{"Normal_Default", 1.1f, 0.0f}};
    GalacticAiWorld world(sim, store, &types, &files, rng, defs, eqs, tinfo,
                          [&sim](uint8_t p, const commands::EventData& ev) { return sim.execute(p, ev); }, cfg);
    std::vector<std::string> pd;
    CHECK(world.load_plans(plan_files, &pd));
    for (const std::string& d : pd) std::printf("  plans: %s\n", d.c_str());
    CHECK(world.plans().plans.size() == 3);
    CHECK(world.start(&pd));
    for (int32_t f = 1; f <= frames; ++f) {
        sim.update();  // T5: the galactic tick
        for (int32_t p = 0; p < static_cast<int32_t>(sim.players().size()); ++p)
            world.update_player(p, f, true, conflicts.state() == gal::ConflictState::AutoRunning);  // T7
        StateHasher h;
        sim.hash_state(h);
        world.hash(h);
        rng.synced().hash(h);
        conflicts.hash(h);
        out.crc.push_back(h.digest());
    }
    for (const AiDecision& d : world.decision_log())
        out.decisions.push_back(std::to_string(d.frame) + " p" + std::to_string(d.player) + " " + d.text);
    out.errors = world.lua_errors();
    for (size_t p = 0; p < sim.players().size(); ++p) out.owned.push_back(sim.planets_owned(static_cast<int32_t>(p)));
    out.orders = world.orders_issued();
    out.rejected = world.orders_rejected();
    out.conflicts = conflicts.log().size();
    char buf[512];
    std::snprintf(buf, sizeof buf, "frames %d planets E=%d R=%d N=%d credits E=%.0f R=%.0f orders %llu rejected %llu conflicts %llu plans running %zu",
                  frames, out.owned[0], out.owned[1], out.owned[2], static_cast<double>(sim.credits(0)),
                  static_cast<double>(sim.credits(1)), static_cast<unsigned long long>(out.orders),
                  static_cast<unsigned long long>(out.rejected), static_cast<unsigned long long>(out.conflicts), world.running_plans());
    out.summary = buf;
    return out;
}

static std::string golden_text(const CampaignResult& r) {
    std::ostringstream o;
    o << "# A2 synthetic AI-vs-AI campaign: per-frame CRC (every 150th frame + last), decisions\n";
    for (size_t i = 0; i < r.crc.size(); ++i)
        if ((i + 1) % 150 == 0 || i + 1 == r.crc.size()) {
            char b[64];
            std::snprintf(b, sizeof b, "frame %zu %016llx\n", i + 1, static_cast<unsigned long long>(r.crc[i]));
            o << b;
        }
    o << r.summary << "\n";
    for (const std::string& d : r.decisions) o << d << "\n";
    return o.str();
}

static void test_synthetic_campaign() {
    const int32_t frames = 30 * 900;  // 15 minutes of galactic time
    const CampaignResult base = run_synthetic(1, frames);
    std::printf("synthetic campaign (1 thread): %s, %zu decisions, %zu Lua errors\n", base.summary.c_str(),
                base.decisions.size(), base.errors.size());
    for (size_t i = 0; i < base.errors.size() && i < 10; ++i) std::printf("  lua error: %s\n", base.errors[i].c_str());
    CHECK(base.errors.empty());
    CHECK(base.orders > 0 && base.rejected * 4 < base.orders + 1);
    CHECK(base.owned.size() == 3 && base.owned[0] + base.owned[1] > 4);  // the AIs expanded
    for (const unsigned t : {2u, 4u, 15u}) {
        const CampaignResult r = run_synthetic(t, frames);
        size_t first = r.crc.size() == base.crc.size() ? r.crc.size() : 0;
        for (size_t i = 0; i < std::min(r.crc.size(), base.crc.size()); ++i)
            if (r.crc[i] != base.crc[i]) {
                first = i;
                break;
            }
        std::printf("  threads %u vs 1: %s\n", t, first == r.crc.size() ? "identical" : ("DIVERGED at frame " + std::to_string(first + 1)).c_str());
        CHECK(first == r.crc.size() && r.decisions == base.decisions);
    }
    const std::string text = golden_text(base);
    const std::string path = std::string(ALAMO_AI_GOLDEN_DIR) + "/ai_galactic_synthetic_crc.txt";
    if (g_write_golden) {
        std::ofstream(std::filesystem::path(path), std::ios::binary) << text;
        std::printf("  wrote %s\n", path.c_str());
    } else {
        std::ifstream in(std::filesystem::path(path), std::ios::binary);
        std::string want((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        want.erase(std::remove(want.begin(), want.end(), '\r'), want.end());
        const bool same = want == text;
        std::printf("  golden %s: %s\n", path.c_str(), same ? "MATCH" : (want.empty() ? "MISSING" : "DIFFERS"));
        if (!same) {
            const std::string actual = std::string(ALAMO_AI_LOG_DIR) + "/ai_galactic_synthetic_actual.txt";
            std::ofstream(std::filesystem::path(actual), std::ios::binary) << text;
            std::printf("  actual written to %s\n", actual.c_str());
        }
        CHECK(same);
    }
}

// AI execution path finder in the world (GALACTIC.md §4, ai_path_finder.hpp): lazy creation by a query before the
// first service, creation at the first service (stagger), the 90-frame recompute in the Execution service, Get_Path
// answers, non-AI players rejected, and the "ai.paths" state identical for any thread count.
static std::vector<uint64_t> run_path_world(unsigned threads, int32_t frames, lua::LuaPlayerWorld* story = nullptr) {
    std::vector<uint64_t> crc;
    MemoryFiles files;
    AiDefinitions defs;
    EquationSet eqs;
    std::vector<std::string> plan_files;
    syn_data(files, defs, eqs, plan_files);
    const gal::GalacticRules rules = syn_rules();
    objects::GameObjectTypes types;
    objects::IdCounter ids;
    objects::ObjectStore store(objects::ModeKind::Galactic, objects::StoreKind::Primary, types, &ids);
    GameRandom rng(4242u, 77u);
    gal::GalacticSim sim(rules, store, rng.synced());
    sim.set_parallel_for(thread_pf(threads));
    std::vector<std::string> diag;
    if (!sim.start(syn_setup(), &diag)) {
        ++g_fail;
        return crc;
    }
    GalacticAiConfig cfg;
    cfg.players = {{0, "SynEmpire", 1}, {1, "SynRebel", 1}};
    cfg.difficulties = {{"Normal_Default", 1.1f, 0.0f}};
    cfg.path_pf = thread_pf(threads);
    std::vector<GalacticAiTypeInfo> tinfo(tTypes);
    GalacticAiWorld world(sim, store, &types, &files, rng, defs, eqs, tinfo,
                          [&sim](uint8_t p, const commands::EventData& ev) { return sim.execute(p, ev); }, cfg);
    std::vector<std::string> pd;
    CHECK(world.load_plans(plan_files, &pd) && world.start(&pd));
    {   // GRC 2 visibility in the S2 snapshot: AI rows only with fog on (presence level, -1 on an owned planet).
        const GalacticSnapshot live = snapshot_from_sim(sim);
        const GalacticSnapshot fog = snapshot_from_sim(sim, true);
        CHECK(live.fog_disabled && live.visibility.empty() && !fog.fog_disabled && fog.visibility.size() == 3);
        CHECK(fog.visibility[0].size() == 8 && fog.visibility[2].empty());
        CHECK(sim.planets()[0].owner != 0 || fog.visibility[0][0] == 0xFFFFFFFFu);
        CHECK(fog.visibility[0][7] == sim.visibility_sources(7, 0).presence && fog.validate().empty());
        CHECK(sim.local_player() >= 0 || fog.ownership_visible.empty());  // all-AI galaxy: no local mask
    }
    if (story) {  // Lua Find_Path of a story world answers from the AI path finders while attached
        world.attach_story_world(story);
        CHECK(story->ai_paths() == &world.path_query());
    }
    // Before any service: no finder yet. The Lua-side query does not create it; path() creates it lazily (next = 0)
    // and answers on the 8-planet chain.
    CHECK(world.path_finder(0) && !world.path_finder(0)->created() && !world.path_finder(2));
    std::vector<PlanetIndex> early;
    CHECK(!world.path_query().ai_path(0, 0, 7, early) && early.empty() && !world.path_finder(0)->created());
    CHECK(world.path(0, 0, 7) == (std::vector<PlanetIndex>{0, 1, 2, 3, 4, 5, 6, 7}));
    CHECK(world.path_finder(0)->created() && world.path_finder(0)->next_recompute() == 0);
    std::vector<PlanetIndex> out;
    CHECK(!world.path_query().ai_path(2, 0, 7, out) && out.empty());  // the neutral player has no AI path finder
    CHECK(world.path(2, 0, 7).empty() && world.path(1, 3, 3) == (std::vector<PlanetIndex>{3, 3}));
    CHECK(world.path(1, 0, 99).empty());
    CrcRegistry reg;
    world.register_path_crc(reg);
    for (int32_t f = 1; f <= frames; ++f) {
        sim.update();
        for (int32_t p = 0; p < static_cast<int32_t>(sim.players().size()); ++p) world.update_player(p, f, true, false);
        crc.push_back(reg.capture(static_cast<uint64_t>(f)).combined);
    }
    for (int32_t p = 0; p < 2; ++p) {
        const AiPathFinder* pf = world.path_finder(p);
        CHECK(pf && pf->created());
        // Created at the first service, then a recompute every 90 mode frames (serviced every 3 frames).
        CHECK(pf->recomputes() >= static_cast<uint32_t>(frames / 90) && pf->recomputes() <= static_cast<uint32_t>(frames / 90 + 4));
        CHECK(pf->next_recompute() > sim.frame() - 3 && pf->next_recompute() <= sim.frame() + 90);
        const std::vector<PlanetIndex> fw = world.path(p, 7, 0);
        CHECK(fw.size() >= 2 && fw.front() == 7 && fw.back() == 0);
    }
    return crc;
}

static void test_path_finder_world() {
    const int32_t frames = 30 * 40;
    lua::LuaPlayerWorld story;  // outlives the AI world: detached by its destructor
    const std::vector<uint64_t> base = run_path_world(1, frames, &story);
    CHECK(base.size() == static_cast<size_t>(frames) && story.ai_paths() == nullptr);
    for (const unsigned t : {2u, 4u, 15u}) CHECK(run_path_world(t, frames) == base);
}

// A2 galactic: the GalacticSim hooks the galactic AI uses. The §3.1 budget tick comes from the sim's income-tick
// listener (rate = gain / T of that cycle); the world may be destroyed before the sim (the listener goes quiet); fleet
// launches are FleetManagement events through the order sink.
static void test_sim_hooks() {
    MemoryFiles files;
    AiDefinitions defs;
    EquationSet eqs;
    std::vector<std::string> plan_files;
    syn_data(files, defs, eqs, plan_files);
    const gal::GalacticRules rules = syn_rules();
    objects::GameObjectTypes types;
    objects::IdCounter ids;
    objects::ObjectStore store(objects::ModeKind::Galactic, objects::StoreKind::Primary, types, &ids);
    GameRandom rng(4242u, 77u);
    gal::GalacticSim sim(rules, store, rng.synced());
    CHECK(sim.start(syn_setup()));
    std::vector<GalacticAiTypeInfo> tinfo(tTypes);
    tinfo[tFrigate].categories = {"FRIGATE"};
    tinfo[tCorvette].categories = {"CORVETTE"};
    tinfo[tInfantry].categories = {"INFANTRY"};
    GalacticAiConfig cfg;
    cfg.players = {{0, "SynEmpire", 1}, {1, "SynRebel", 1}};
    cfg.difficulties = {{"Normal_Default", 1.1f, 0.0f}};
    uint32_t fleet_events = 0, fleet_rejected = 0, ticks_seen = 0;
    int32_t f = 1;
    {
        GalacticAiWorld world(sim, store, &types, &files, rng, defs, eqs, tinfo,
                              [&](uint8_t p, const commands::EventData& ev) {
                                  const commands::RejectReason r = sim.execute(p, ev);
                                  if (ev.type() == commands::EventType::FleetManagement) {
                                      ++fleet_events;
                                      fleet_rejected += r == commands::RejectReason::None ? 0u : 1u;
                                  }
                                  return r;
                              },
                              cfg);
        CHECK(world.load_plans(plan_files) && world.start());
        int32_t day = -1;
        for (; f <= 30 * 300; ++f) {
            sim.update();
            for (int32_t p = 0; p < 2; ++p) world.update_player(p, f, true, false);
            const AiContext* c = world.context(0);
            if (!c || !c->budget.initialised()) continue;
            if (day < 0) day = sim.galactic_day();
            if (sim.galactic_day() != day) {  // an income tick after the budget's init
                day = sim.galactic_day();
                ++ticks_seen;
                CHECK(c->budget.period() > 0.0f);
                CHECK(c->budget.income_per_second() == sim.last_income(0) / c->budget.period());
            }
        }
    }
    for (int32_t k = 0; k < 30 * 120; ++k) sim.update();  // listener of the destroyed world: silent, no crash
    std::printf("sim hooks: %u income ticks checked, %u FleetManagement orders (%u rejected)\n", ticks_seen, fleet_events, fleet_rejected);
    CHECK(ticks_seen >= 2);
    CHECK(fleet_events > 0 && fleet_rejected == 0);
}

// A2 t03 (Conquer_Opponent): the starbase's own AI_Combat_Power belongs in the untyped force totals only (GALACTIC_ECONOMY
// 6.2 Node_Weight helper 0x5f2430, mask "all"); a typed row holds the starbase's per-tech spawned members by category
// (AI_GALACTIC_TOKENS 2: "not the starbase's own power"). Shared by the PotentialPlan Contrast rows and the token leaf.
static void test_starbase_force_split() {
    MemoryFiles files;
    AiDefinitions defs;
    EquationSet eqs;
    std::vector<std::string> plan_files;
    syn_data(files, defs, eqs, plan_files);
    eqs.add("Space_All", "Variable_Target.EnemyForce.SpaceTotalUnnormalized");
    eqs.add("Space_Capital", "Variable_Target.EnemyForce.SpaceTotalUnnormalized {Parameter_Category = \"Capital\"}");
    eqs.add("Space_Frigate", "Variable_Target.EnemyForce.SpaceTotalUnnormalized {Parameter_Category = \"Frigate\"}");
    eqs.add("Space_Corvette", "Variable_Target.EnemyForce.SpaceTotalUnnormalized {Parameter_Category = \"Corvette\"}");
    eqs.add("Space_FrigCorv", "Variable_Target.EnemyForce.SpaceTotalUnnormalized {Parameter_Category = \"Frigate\", Parameter_Category = \"Corvette\"}");
    eqs.add("Ground_All", "Variable_Target.EnemyForce.GroundTotalUnnormalized");
    eqs.link();
    const gal::GalacticRules rules = syn_rules();  // SB1 600, Frigate 1000, Corvette 500
    objects::GameObjectTypes types;
    objects::IdCounter ids;
    objects::ObjectStore store(objects::ModeKind::Galactic, objects::StoreKind::Primary, types, &ids);
    GameRandom rng(4242u, 77u);
    gal::GalacticSim sim(rules, store, rng.synced());
    gal::GalacticSetup setup = syn_setup();
    setup.forces = {{0, 0, tSB1}, {1, 7, tSB1}};  // the Empire's base at P0, the (hostile) Rebel base at P7, nothing else
    CHECK(sim.start(setup));
    std::vector<GalacticAiTypeInfo> tinfo(tTypes);
    tinfo[tFrigate].categories = {"FRIGATE"};
    tinfo[tCorvette].categories = {"CORVETTE"};
    tinfo[tSB1].categories = tinfo[tSB2].categories = {"CAPITAL"};
    tinfo[tSB1].spawn_tech[0] = {{tCorvette, 2}, {tFrigate, 1}};  // 2 x 500 + 1 x 1000
    tinfo[tSB1].spawn_tech[3] = {{tFrigate, 9}};                  // above the owner's tech level: never read
    fill_category_masks(tinfo);
    const std::span<const GalacticAiTypeInfo> ti(tinfo);
    const uint32_t cap = static_cast<uint32_t>(ai_category_bit("Capital")), frig = static_cast<uint32_t>(ai_category_bit("Frigate")),
                   corv = static_cast<uint32_t>(ai_category_bit("Corvette"));
    CHECK(cap != 0 && frig != 0 && corv != 0 && sim.planets()[7].starbase && sim.planets()[7].owner == 1);
    CHECK(sim.players()[1].tech_level == 1);  // slot 1 empty: the fallback reads slot 0
    // The shared helper (PotentialPlan enemy_strength + token leaf): untyped = members + own power, typed = members only.
    CHECK(galactic_ai_structure_force(sim, ti, tSB1, 1, kAllCategories) == 2600.0);
    CHECK(galactic_ai_structure_force(sim, ti, tSB1, 1, cap) == 0.0);  // the starbase is a Capital, its own power is NOT in the row
    CHECK(galactic_ai_structure_force(sim, ti, tSB1, 1, frig) == 1000.0);
    CHECK(galactic_ai_structure_force(sim, ti, tSB1, 1, corv) == 1000.0);
    CHECK(galactic_ai_structure_force(sim, ti, tSB1, 1, frig | corv) == 2000.0);
    CHECK(galactic_ai_structure_force(sim, ti, tSB2, 1, kAllCategories) == 1200.0);  // no members: the untyped total is its power
    CHECK(galactic_ai_structure_force(sim, ti, tSB2, 1, cap) == 0.0);
    // The token leaf on the live sim (Self = Empire, Target = P7).
    GalacticAiConfig cfg;
    cfg.players = {{0, "SynEmpire", 1}, {1, "SynRebel", 1}};
    cfg.difficulties = {{"Normal_Default", 1.1f, 0.0f}};
    GalacticAiWorld world(sim, store, &types, &files, rng, defs, eqs, tinfo,
                          [&sim](uint8_t p, const commands::EventData& ev) { return sim.execute(p, ev); }, cfg);
    CHECK(world.load_plans(plan_files) && world.start());
    sim.update();
    world.update_player(0, 1, true, false);
    EvalContext c;
    c.self = 0;
    c.target = 7;
    auto value = [&](const char* name) {
        const EvalResult r = evaluate(eqs, eqs.find(name), c, world.resolver());
        CHECK(r.ok);
        return r.value;
    };
    CHECK(value("Space_All") == 2600.0);
    CHECK(value("Space_Capital") == 0.0);
    CHECK(value("Space_Frigate") == 1000.0);
    CHECK(value("Space_Corvette") == 1000.0);
    CHECK(value("Space_FrigCorv") == 2000.0);
    CHECK(value("Ground_All") == 0.0);
    // The Empire's own base is not enemy force from its point of view.
    c.target = 0;
    CHECK(value("Space_All") == 0.0);
    std::printf("starbase force split: untyped 2600 (members 2000 + own 600), Capital row 0, Frigate row 1000\n");
}

// A2 galactic, AI_GALACTIC_3 rules: heroes never attach through their AI script (§2.4), the TerrainType veto (§6.3),
// the galactic population cap when AI_Uses_Galactic_Pop_Cap is set (§3.5), MagicPlan spawns without production or
// charge (§4), and the perception DLL's division / clamp failures (§7.1).
static const char* kPlanHeroAttach = R"(
require("SynAICommands")
function Definitions()
  Category = "Conquer_Planet"
  TaskForce = { { "MainForce", "HeroShip | Corvette = 1,1" } }
end
function MainForce_Thread() ScriptExit() end
)";
static const char* kPlanHeroDeny = R"(
require("SynAICommands")
function Definitions()
  Category = "Conquer_Planet"
  TaskForce = { { "MainForce", "DenyHeroAttach", "HeroShip | Corvette = 1,1" } }
end
function MainForce_Thread() ScriptExit() end
)";
static const char* kPlanMagic = R"(
require("SynAICommands")
ScriptPoolCount = 1
function Definitions()
  Category = "Build_Space_Forces"
  IgnoreTarget = true
  MagicPlan = true
  MagicPlanStealing = false
  TaskForce = { { "MagicForce", "Corvette = 1" } }
end
function MagicForce_Thread()
  MagicForce.Set_As_Goal_System_Removable(false)
  BlockOnCommand(MagicForce.Produce_Force())
  MagicForce.Set_Plan_Result(true)
  ScriptExit()
end
)";

struct ParityWorld {
    MemoryFiles files;
    AiDefinitions defs;
    EquationSet eqs;
    std::vector<std::string> plan_files;
    gal::GalacticRules rules;
    objects::GameObjectTypes types;
    objects::IdCounter ids;
    std::unique_ptr<objects::ObjectStore> store;
    std::unique_ptr<GameRandom> rng;
    std::unique_ptr<gal::GalacticSim> sim;
    std::unique_ptr<GalacticAiWorld> world;
    uint32_t hero = 0, planet_r = 0;
    std::vector<std::string> orders;

    // only: the plan files to load (by base name), empty = all.
    ParityWorld(bool pop_cap, const std::vector<std::string>& only,
                const std::function<void(gal::GalacticSetup&)>& tweak = {}) {
        syn_data(files, defs, eqs, plan_files);
        files.add("Data\\Scripts\\AI\\HeroAttach.lua", kPlanHeroAttach);
        files.add("Data\\Scripts\\AI\\HeroDeny.lua", kPlanHeroDeny);
        files.add("Data\\Scripts\\AI\\MagicSpace.lua", kPlanMagic);
        plan_files = list_plan_files([this](std::string_view glob) {
            const std::string g(glob);
            return files.list(g.substr(0, g.find('*')));
        });
        if (!only.empty())
            plan_files.erase(std::remove_if(plan_files.begin(), plan_files.end(),
                                            [&only](const std::string& f) {
                                                for (const std::string& o : only)
                                                    if (ai_iequal(f.substr(f.find_last_of('\\') + 1), o + ".lua")) return false;
                                                return true;
                                            }),
                             plan_files.end());
        rules = syn_rules();
        hero = static_cast<uint32_t>(rules.types.size());
        gal::TypeInfo h;
        h.name = "HeroShip";
        h.kind = gal::UnitKind::SpaceUnit;
        h.ai_combat_power = 3000.0f;
        h.hyperspace = true;
        h.hyperspace_speed = 2.0f;
        rules.types.push_back(h);
        planet_r = static_cast<uint32_t>(rules.types.size());
        gal::TypeInfo pr = rules.types[tPlanet];
        pr.name = "SynPlanetRestricted";
        pr.restricted_types = {tCorvette};
        rules.types.push_back(pr);
        if (pop_cap) {
            rules.constants.ai_uses_galactic_pop_cap = true;
            rules.types[tCorvette].population = 50;  // more than any capacity of this map
        }
        store = std::make_unique<objects::ObjectStore>(objects::ModeKind::Galactic, objects::StoreKind::Primary, types, &ids);
        rng = std::make_unique<GameRandom>(4242u, 77u);
        sim = std::make_unique<gal::GalacticSim>(rules, *store, rng->synced());
        gal::GalacticSetup s = syn_setup();
        s.forces.push_back({0, 0, hero});  // the Empire's hero on its free store
        s.planet_type[2] = planet_r;       // P2 restricts Corvettes
        if (tweak) tweak(s);
        CHECK(sim->start(s));
        std::vector<GalacticAiTypeInfo> tinfo(rules.types.size());
        tinfo[tFrigate].categories = {"FRIGATE"};
        tinfo[tCorvette].categories = {"CORVETTE"};
        tinfo[tInfantry].categories = {"INFANTRY"};
        tinfo[hero].hero = true;
        tinfo[hero].lua_script = "HeroShipScript";  // a hero AI script: a hero record (§2.1)
        tinfo[tCorvette].unit_ai = true;            // STRATEGIC_GAPS §1.1 removal feed filter
        tinfo[tInfantry].company_first = tCorvette; // (synthetic) company-leader substitution for the survival key
        GalacticAiConfig cfg;
        cfg.players = {{0, "SynEmpire", 1}};
        cfg.difficulties = {{"Normal_Default", 1.1f, 0.0f}};
        world = std::make_unique<GalacticAiWorld>(
            *sim, *store, &types, &files, *rng, defs, eqs, tinfo,
            [this](uint8_t p, const commands::EventData& ev) {
                orders.push_back(std::to_string(static_cast<int>(ev.type_id())));
                return sim->execute(p, ev);
            },
            cfg);
        CHECK(world->load_plans(plan_files) && world->start());
    }
    void run(int32_t frames) {
        for (int32_t f = 1; f <= frames; ++f) {
            sim->update();
            world->update_player(0, f, true, false);
        }
    }
    bool has(const std::vector<uint32_t>& sel, uint32_t t) const { return std::find(sel.begin(), sel.end(), t) != sel.end(); }
};

static void test_parity_rules() {
    {
        ParityWorld pw(false, {"HeroAttach", "HeroDeny"});
        pw.run(2);  // first service: templates, budget
        bool ok = false;
        // §2.4: the hero record is vetoed in a plan that allows hero attach; the corvette is chosen instead.
        const std::vector<uint32_t> a = pw.world->solve_potential_plan(0, "HeroAttach", "Conquer_Planet", 3, &ok);
        std::printf("parity: hero-attach plan -> %zu selected (hero %s)\n", a.size(), pw.has(a, pw.hero) ? "yes" : "no");
        CHECK(ok && !pw.has(a, pw.hero) && pw.has(a, tCorvette));
        // Every TF DenyHeroAttach: the hero is a plain free-store unit and wins (FreeStore 2+).
        const std::vector<uint32_t> d = pw.world->solve_potential_plan(0, "HeroDeny", "Conquer_Planet", 3, &ok);
        CHECK(ok && pw.has(d, pw.hero));
        // §6.3: P2's planet type restricts Corvettes -> TerrainType vetoes it; the hero is vetoed too -> the solve fails.
        const std::vector<uint32_t> r = pw.world->solve_potential_plan(0, "HeroAttach", "Conquer_Planet", 2, &ok);
        CHECK(!ok && r.empty());
        // §7.1: x / 0 fails, 0 / 0 = 0, clamp with lo > hi fails.
        EquationSet e;
        e.add("ZeroOverZero", "0 / 0");
        e.add("OneOverZero", "1 / 0");
        e.add("ClampBad", "clamp(5, 3, 1)");
        e.add("ClampOk", "clamp(5, 1, 3)");
        e.link();
        EvalContext c;
        c.self = 0;
        const EvalResult z = evaluate(e, e.find("ZeroOverZero"), c, pw.world->resolver());
        CHECK(z.ok && z.value == 0.0);
        CHECK(!evaluate(e, e.find("OneOverZero"), c, pw.world->resolver()).ok);
        CHECK(!evaluate(e, e.find("ClampBad"), c, pw.world->resolver()).ok);
        const EvalResult k = evaluate(e, e.find("ClampOk"), c, pw.world->resolver());
        CHECK(k.ok && k.value == 3.0);
        // STRATEGIC_GAPS §1.1: per-type survival ratio (1.0 unseen), fed by the auto-resolve roll and object removal
        // (UNIT_AI types only, AI owners only), keyed by the type-name CRC with the company-leader substitution.
        CHECK(pw.world->survival_ratio(0, tCorvette) == 1.0);
        pw.world->record_auto_resolve_survival(0, tCorvette, false);
        pw.world->record_auto_resolve_survival(0, tCorvette, true);
        CHECK(pw.world->survival_ratio(0, tCorvette) == 0.5);
        pw.world->record_object_removal(0, tCorvette, tCorvette, false);
        pw.world->record_object_removal(0, tFrigate, tFrigate, false);  // no UNIT_AI: skipped
        pw.world->record_object_removal(1, tCorvette, tCorvette, true);  // player 1 has no AI: skipped
        CHECK(std::fabs(pw.world->survival_ratio(0, tCorvette) - 1.0 / 3.0) < 1e-12 && pw.world->survival_ratio(0, tFrigate) == 1.0);
        CHECK(pw.world->survival_ratio(0, tInfantry) == pw.world->survival_ratio(0, tCorvette));  // company leader key
        CHECK(pw.world->survival_ratio(1, tCorvette) == 1.0);
        const std::vector<uint32_t> s2 = pw.world->solve_potential_plan(0, "HeroAttach", "Conquer_Planet", 3, &ok);
        CHECK(ok && pw.has(s2, tCorvette));  // the ratio is a 0..1 score term, not a veto
        LearningSystem ls;
        ls.record_survival(7u, true);
        ls.record_survival(7u, false);
        ls.record_survival(7u, false);
        CHECK(ls.survival_ratio(7u) * 3.0 == 1.0 && ls.survival_ratio(8u) == 1.0 && ls.survival_entries() == 1);
    }
    {
        // §3.5: with AI_Uses_Galactic_Pop_Cap a produced Corvette (pop 50) exceeds the remaining population.
        ParityWorld off(false, {"HeroAttach"}), on(true, {"HeroAttach"});
        off.run(2);
        on.run(2);
        bool ok_off = false, ok_on = false;
        off.world->solve_potential_plan(0, "HeroAttach", "Conquer_Planet", 3, &ok_off);
        on.world->solve_potential_plan(0, "HeroAttach", "Conquer_Planet", 3, &ok_on);
        std::printf("parity: pop cap off -> %s, on -> %s\n", ok_off ? "solved" : "failed", ok_on ? "solved" : "failed");
        CHECK(ok_off && !ok_on);
    }
    {
        // §4: a MagicPlan spawns its unit at the destination (no Production event, no budget charge).
        ParityWorld pw(false, {"MagicSpace"});
        pw.run(30 * 60);
        bool spawned = false, failed = false;
        for (const AiDecision& d : pw.world->decision_log()) {
            spawned = spawned || d.text.find("magic spawn Corvette") != std::string::npos;
            failed = failed || d.text.find("failed") != std::string::npos;
        }
        const bool produced = std::find(pw.orders.begin(), pw.orders.end(), std::to_string(static_cast<int>(commands::EventType::Production))) !=
                              pw.orders.end();
        std::printf("parity: magic plan spawned %s, production orders %s\n", spawned ? "yes" : "no", produced ? "yes" : "no");
        CHECK(spawned && !failed && !produced);
    }
}

// OI2 ai: production-disabled types (story DISABLE_BUILDABLE, REMAINING_GAPS §1.5) are never picked by AI production -
// neither by a normal plan's Production marshaller nor by a MagicPlan spawn nor by the team picks; and the build
// market's delivery path class (REMAINING_GAPS §6.2): 2 by default, 3 with +0xDA, 5 for Is_Stealth_Company.
static void test_production_disabled_and_delivery() {
    {
        ParityWorld pw(false, {"HeroAttach"});
        pw.run(2);
        bool ok = false;
        const std::vector<uint32_t> a = pw.world->solve_potential_plan(0, "HeroAttach", "Conquer_Planet", 3, &ok);
        CHECK(ok && pw.has(a, tCorvette));
        CHECK(galactic_ai_player_can_build(*pw.sim, 0, tCorvette));
        pw.sim->set_production_disabled(0, tCorvette, true);
        CHECK(!galactic_ai_player_can_build(*pw.sim, 0, tCorvette));
        CHECK(galactic_ai_player_can_build(*pw.sim, 0, tFrigate));
        const std::vector<uint32_t> d = pw.world->solve_potential_plan(0, "HeroAttach", "Conquer_Planet", 3, &ok);
        std::printf("production disabled: Corvette %s -> %zu selected, corvette %s\n", ok ? "solved" : "failed", d.size(),
                    pw.has(d, tCorvette) ? "yes" : "no");
        CHECK(!pw.has(d, tCorvette));
        pw.sim->set_production_disabled(0, tCorvette, false);  // ENABLE_BUILDABLE
        const std::vector<uint32_t> e = pw.world->solve_potential_plan(0, "HeroAttach", "Conquer_Planet", 3, &ok);
        CHECK(ok && pw.has(e, tCorvette));
    }
    {
        // MagicPlan: the production-disabled list (0x282510) and the locked list (0x282550) veto the spawn [H].
        for (int variant = 0; variant < 2; ++variant) {
            ParityWorld pw(false, {"MagicSpace"});
            if (variant == 0) pw.sim->set_production_disabled(0, tCorvette, true);
            else pw.sim->lock_type(0, tCorvette, true);
            pw.run(30 * 60);
            bool spawned = false;
            for (const AiDecision& d : pw.world->decision_log()) spawned = spawned || d.text.find("magic spawn Corvette") != std::string::npos;
            std::printf("production disabled: magic plan with Corvette %s -> spawned %s\n", variant == 0 ? "disabled" : "locked",
                        spawned ? "yes" : "no");
            CHECK(!spawned);
        }
    }
    {
        CHECK(galactic_ai_delivery_mode(false, false) == static_cast<int>(Reachability::EnemyDestination));
        CHECK(galactic_ai_delivery_mode(false, true) == static_cast<int>(Reachability::EnemyUndefended));
        CHECK(galactic_ai_delivery_mode(true, false) == static_cast<int>(Reachability::Any));
        CHECK(galactic_ai_delivery_mode(true, true) == static_cast<int>(Reachability::Any));
        // The chain P0..P7: the Empire holds P0 / P1, the Rebels P6 / P7, P2..P5 nobody. Class 2 stops at the first
        // unowned planet (reached, not expanded); class 5 (stealth companies) crosses unowned planets too.
        ParityWorld pw(false, {"HeroAttach"});
        pw.run(2);
        const std::vector<uint8_t> m2 = galactic_ai_delivery_flood(*pw.sim, 0, 0, galactic_ai_delivery_mode(false, false));
        const std::vector<uint8_t> m5 = galactic_ai_delivery_flood(*pw.sim, 0, 0, galactic_ai_delivery_mode(true, false));
        std::string a, b;
        for (size_t i = 0; i < m2.size(); ++i) {
            a += m2[i] ? '1' : '0';
            b += m5[i] ? '1' : '0';
        }
        std::printf("delivery floods from P0: class 2 %s, class 5 %s\n", a.c_str(), b.c_str());
        CHECK(m2.size() == 8 && m5.size() == 8);
        CHECK(m2[0] && m2[1] && !m2[4] && !m2[7]);
        for (size_t i = 0; i < m5.size(); ++i) CHECK(m5[i] != 0);
        CHECK(galactic_ai_delivery_flood(*pw.sim, 0, kInvalidPlanet, 2) == std::vector<uint8_t>(8, 0));
        // Request flag 1 is (m == 5) (FIXPASS_GALACTIC §1.4): in class 2 the Rebels' flood from P0 reaches the Empire's P1
        // (flag 4, destination) but does not cross it; class 5 does.
        const std::vector<uint8_t> r2 = galactic_ai_delivery_flood(*pw.sim, 1, 0, galactic_ai_delivery_mode(false, false));
        CHECK(r2.size() == 8 && r2[0] && r2[1] && !r2[2] && !r2[3]);
        const std::vector<uint8_t> r5 = galactic_ai_delivery_flood(*pw.sim, 1, 0, galactic_ai_delivery_mode(true, false));
        CHECK(r5.size() == 8 && r5[1] && r5[2] && r5[3]);
    }
}

// ================================================================ FIXPASS_GALACTIC §1: free-store moves on Friendly_Only
//
// Pure parts: the corrected AI request flags (§1.4), the hop-weight sum / high-16-bit test (§1.3 step 4), the orbit owner
// (step 6), Get_Next_Hop on an AiPathFinder matrix (steps 1-7) and the FindTarget pick (§1.5: at most one draw).
static void test_free_store_path_rules() {
    for (int m = 0; m <= 5; ++m) {
        const gal::PathRequest r = ai_path_request(3, 4, m);
        CHECK(r.source == 3 && r.destination == 4 && r.fog_rule);
        CHECK(r.allow_unknown_or_neutral == (m == 5) && r.allow_enemy_owned == (m == 5));
        CHECK(r.skip_danger_check == (m == 1 || m == 5));
        CHECK(r.allow_impassable == (m > 2) && r.always_accept_destination == (m > 1));
    }
    // 0x691250 / 0x6913a0.
    CHECK(free_store_path_sum({}, 0) == -1 && !free_store_path_passes(-1));
    const std::vector<uint32_t> w3{500u, 0xFFFFu - 500u, 0u};
    CHECK(free_store_path_sum(w3, 3) == 0xFFFF && free_store_path_passes(0xFFFF));
    const std::vector<uint32_t> carry{500u, 0xFFFFu - 499u, 0u};  // distance terms carrying past 0xFFFF fail
    CHECK(free_store_path_sum(carry, 3) == 0x10000 && !free_store_path_passes(0x10000));
    const std::vector<uint32_t> cost{gal::pack_edge_weight(1u, 7u), 0u};  // node cost 1 on the destination
    CHECK(!free_store_path_passes(free_store_path_sum(cost, 2)));
    const std::vector<uint32_t> wrap{0xFFFFFFFFu, 2u, 0u};  // wrapping int32 sum: -1 + 2 = 1
    CHECK(free_store_path_sum(wrap, 3) == 1 && free_store_path_passes(1));
    CHECK(free_store_path_sum(std::vector<uint32_t>{0u, 0u}, 2) == 0 && free_store_path_passes(0));  // src == dst
    std::vector<uint32_t> long_path(1500, 1u);
    CHECK(free_store_path_sum(long_path, 1500) == 1000);  // at most 1000 hops summed
    // 0x3f9270.
    const OrbitFleet tr{1, true}, other{2, false}, mine{0, false};
    CHECK(orbit_owner(true, 5, std::vector<OrbitFleet>{mine}) == 5);
    CHECK(orbit_owner(false, 5, {}) == -1);
    CHECK(orbit_owner(false, 5, std::vector<OrbitFleet>{tr, other, mine}) == 2);
    CHECK(orbit_owner(false, 5, std::vector<OrbitFleet>{tr, OrbitFleet{3, true}}) == 1);

    // Get_Next_Hop on the diamond A(0)-B(1)-C(2), A-D(3)-C (free hops of 50 / 58.3 units).
    GalaxyMap map;
    map.campaign_name = "fs_paths";
    map.max_fleet_move_distance = 60.0f;
    const float xy[4][2] = {{0.0f, 0.0f}, {50.0f, 0.0f}, {100.0f, 0.0f}, {50.0f, 30.0f}};
    for (uint32_t i = 0; i < 4; ++i) {
        const auto pl = map.planets.add(std::string(1, static_cast<char>('A' + i)));
        map.planets.flags[pl] = kPlanetHasPlanetBehavior;
        map.planets.position[pl] = {xy[i][0], xy[i][1], 0.0f};
    }
    const gal::PlanetGraph g = gal::PlanetGraph::build(map);
    const PlanetIndex A = 0, B = 1, C = 2, D = 3;
    auto finder = [&g](const std::vector<double>& nv) {
        AiPathFinder f;
        f.create(g, ai_path_input(nv, nullptr, true, false), 0, 0, 1);
        return f;
    };
    std::vector<int32_t> owner_of(4, -1);
    const std::function<int32_t(PlanetIndex)> oo = [&owner_of](PlanetIndex p) { return owner_of[p]; };
    {
        const AiPathFinder f = finder(std::vector<double>(4, 0.0));
        FreeStoreHopResult r = free_store_next_hop(f, 0, A, C, false, oo);
        CHECK(r.kind == FreeStoreHop::Issue && r.hop == B && (static_cast<uint32_t>(r.sum) >> 16) == 0u);
        CHECK(free_store_next_hop(f, 0, A, C, true, oo).kind == FreeStoreHop::Wait);
        CHECK(free_store_next_hop(f, 0, kInvalidPlanet, C, false, oo).kind == FreeStoreHop::NotOnPlanet);
        CHECK(free_store_next_hop(f, 0, C, C, false, oo).kind == FreeStoreHop::Arrived);
        CHECK(free_store_next_hop(f, 0, A, 9, false, oo).kind == FreeStoreHop::NoPath);  // not a node: empty path
        owner_of[B] = 1;  // another player's fleet in B's orbit (an ally counts as other)
        CHECK(free_store_next_hop(f, 0, A, C, false, oo).kind == FreeStoreHop::OrbitBlocked);
        owner_of[B] = 0;
        CHECK(free_store_next_hop(f, 0, A, C, false, oo).kind == FreeStoreHop::Issue);
        owner_of[B] = -1;
        CHECK(free_store_next_hop(AiPathFinder{}, 0, A, C, false, oo).kind == FreeStoreHop::NoPath);  // no matrix
    }
    {   // Node cost on B: the matrix routes A -> D -> C, which is free -> pass, first hop D.
        std::vector<double> nv(4, 0.0);
        nv[B] = 1234.9;
        const FreeStoreHopResult r = free_store_next_hop(finder(nv), 0, A, C, false, oo);
        CHECK(r.kind == FreeStoreHop::Issue && r.hop == D);
    }
    {   // Node costs on B and D: every path crosses a costly planet -> no path, no fallback.
        std::vector<double> nv(4, 0.0);
        nv[B] = nv[D] = 150.0;
        const FreeStoreHopResult r = free_store_next_hop(finder(nv), 0, A, C, false, oo);
        CHECK(r.kind == FreeStoreHop::NoPath && r.sum > 0xFFFF);
    }
    {   // The destination counts, the start does not.
        std::vector<double> nv(4, 0.0);
        nv[C] = 100.0;
        CHECK(free_store_next_hop(finder(nv), 0, A, C, false, oo).kind == FreeStoreHop::NoPath);
        CHECK(free_store_next_hop(finder(nv), 0, C, A, false, oo).kind == FreeStoreHop::Issue);
        nv[C] = 99.9;  // node cost trunc(99.9) / 100 = 0
        CHECK(free_store_next_hop(finder(nv), 0, A, C, false, oo).kind == FreeStoreHop::Issue);
    }

    // FindTarget pick: draws counted.
    uint32_t draws = 0;
    float next = 0.0f;
    const std::function<float(float)> draw = [&](float total) {
        ++draws;
        CHECK(total > 0.0f);
        return next;
    };
    CHECK(find_target_pick({}, 0.5, draw) == -1 && draws == 0);
    CHECK(find_target_pick(std::vector<double>{0.0, -1.0}, 0.5, draw) == -1 && draws == 0);
    CHECK(find_target_pick(std::vector<double>{0.0, 4.0, -1.0}, 0.1, draw) == 1 && draws == 0);  // one positive: no draw
    CHECK(find_target_pick(std::vector<double>{2.0, 3.0}, 1.0, draw) == 1 && draws == 0);      // p = 1: the best
    // [2, 3], p = 0.5 (w = 1): list (0: 2), best 1 joins with 2 / 1 -> cumulative [2, 4], one draw in [0, 4].
    next = 2.0f;
    CHECK(find_target_pick(std::vector<double>{2.0, 3.0}, 0.5, draw) == 0 && draws == 1);
    next = 2.5f;
    CHECK(find_target_pick(std::vector<double>{2.0, 3.0}, 0.5, draw) == 1 && draws == 2);
    // Equal scores: the earlier stays best, the later joins the list first.
    next = 1.0f;
    CHECK(find_target_pick(std::vector<double>{3.0, 3.0}, 0.5, draw) == 1 && draws == 3);
    next = 3.5f;
    CHECK(find_target_pick(std::vector<double>{3.0, 3.0}, 0.5, draw) == 0 && draws == 4);
}

// A synthetic galactic world (ParityWorld: the Empire AI holds P0 / P1; P2..P5 unowned; the Rebels P6 / P7 with a
// starbase at P7) plus a neutral frigate at P3: Move_Object on the AI matrix, re-queried per hop; the orbit check;
// no fallback; the Friendly_Only Reachable_Target filter and its draw.
static void test_free_store_world() {
    ParityWorld pw(false, {"MagicSpace"}, [](gal::GalacticSetup& s) { s.forces.push_back({2, 3, tFrigate}); });
    pw.run(2);  // first service: the AI path finder exists
    gal::GalacticSim& sim = *pw.sim;
    GalacticAiWorld& world = *pw.world;
    const objects::ObjectStore& st = *pw.store;
    auto unit_of = [&](uint32_t type) {
        for (const gal::FleetState& f : sim.fleets())
            if (f.alive && f.owner == 0)
                for (const objects::ObjectHandle u : f.contents)
                    if (st.alive(u) && st.type(u) == type) return u;
        return objects::ObjectHandle{};
    };
    auto fleet_of = [&](objects::ObjectHandle u) -> const gal::FleetState* {
        for (const gal::FleetState& f : sim.fleets())
            if (f.alive && std::find(f.contents.begin(), f.contents.end(), u) != f.contents.end()) return &f;
        return nullptr;
    };
    int32_t frame = 2;
    auto run_until_idle = [&](int32_t max_frames) {
        int32_t f = 0;
        for (; f < max_frames && world.pending_free_store_moves() > 0; ++f) {
            sim.update();
            world.update_player(0, ++frame, true, false);
        }
        return f;
    };
    auto logged = [&](const std::string& text) {
        for (const AiDecision& d : world.decision_log())
            if (d.text.find(text) != std::string::npos) return true;
        return false;
    };
    const objects::ObjectHandle frigate = unit_of(tFrigate), hero = unit_of(pw.hero);
    CHECK(frigate && hero && world.path_finder(0) && world.path_finder(0)->created());
    {
        const gal::FleetState* f = fleet_of(frigate);
        CHECK(f && f->planet == 0);
    }
    // Refusals and "already there".
    CHECK(!world.free_store_move(0, frigate, 99));
    CHECK(world.free_store_move(0, frigate, 0) && world.pending_free_store_moves() == 0);
    // P0 -> P1 (own planet, node cost 0): the frigate splits off and moves.
    CHECK(world.free_store_move(0, frigate, 1) && world.pending_free_store_moves() == 1);
    CHECK(!world.free_store_move(0, frigate, 2));  // in transit (a pending move)
    const int32_t t1 = run_until_idle(30 * 120);
    const gal::FleetState* ff = fleet_of(frigate);
    std::printf("free store world: frigate P0 -> P1 in %d frames, at P%u, fleet of %zu\n", t1,
                ff ? ff->planet : 99u, ff ? ff->contents.size() : 0u);
    CHECK(ff && ff->planet == 1 && !ff->moving && ff->contents.size() == 1);
    // P0 -> P7: the Rebel starbase at P7 gives it a node cost -> no path; logged; no fallback; the hero stays at P0.
    CHECK(world.free_store_move(0, hero, 7) && world.pending_free_store_moves() == 0);
    CHECK(logged("Unable to find a path from P0 to P7"));
    const gal::FleetState* hf = fleet_of(hero);
    CHECK(hf && hf->planet == 0 && !hf->moving);
    // P0 -> P3: the matrix passes (no node cost on the way: the neutral frigate is no enemy force), the hero hops
    // P1, P2; at P2 the next hop P3 has an orbit owner (the neutral player) that is not this player -> the move ends.
    const uint64_t moves_before = world.orders_issued();
    CHECK(world.free_store_move(0, hero, 3));
    const int32_t t3 = run_until_idle(30 * 300);
    hf = fleet_of(hero);
    std::printf("free store world: hero P0 -> P3 ended after %d frames at P%u (%zu orders)\n", t3, hf ? hf->planet : 99u,
                static_cast<size_t>(world.orders_issued() - moves_before));
    CHECK(world.pending_free_store_moves() == 0 && hf && hf->planet == 2 && !hf->moving);
    CHECK(world.path(0, 0, 3).size() == 4);  // P0 P1 P2 P3

    // Reachable_Target's Friendly_Only filter (§1.4): only planets held by the player or an ally.
    const std::vector<PlanetIndex> r0 = world.reach_planets(0, 0);
    const std::vector<PlanetIndex> r2 = world.reach_planets(0, 2);
    const std::vector<PlanetIndex> r5 = world.reach_planets(0, 5);
    std::printf("free store world: reach level 0 %zu planets, level 2 %zu, level 5 %zu\n", r0.size(), r2.size(), r5.size());
    CHECK(r0 == (std::vector<PlanetIndex>{0, 1}));
    CHECK(world.reach_planets(0, 1) == r0);
    CHECK(r2.size() == 3 && std::find(r2.begin(), r2.end(), PlanetIndex{2}) != r2.end());  // P2 reached (flag 4), not crossed
    CHECK(r5.size() == 8);
    const uint64_t d0 = pw.rng->synced().draws();
    CHECK(world.reachable_target(0, "One", "Friendly", "Friendly_Only", 1.0) == 0);  // ties keep the first, no draw
    CHECK(pw.rng->synced().draws() == d0);
    CHECK(world.reachable_target(0, "One", "Neutral", "Friendly_Only", 0.1) == -1);  // unowned: not reachable
    CHECK(world.reachable_target(0, "One", "Neutral", "Enemy_Destination", 0.1) == 2);
    CHECK(pw.rng->synced().draws() == d0);  // one candidate: no draw
    const int32_t pick = world.reachable_target(0, "One", "Friendly", "Friendly_Only", 0.1);
    CHECK((pick == 0 || pick == 1) && pw.rng->synced().draws() == d0 + 1);  // two candidates, p < 1: one draw
}

// ================================================================ P4 ai: Underworld special systems
//
// A synthetic Underworld AI against a passive Empire, with the four shipped Underworld hero plans (their Lua verbatim,
// require line aside) and the shipped Underworld goal equations (the save / planet-value terms reduced). The plans act
// through GalacticSim::ground_activated_trigger (Activate_Ability / Land_Units): the saboteur corrupts a clean planet
// (corruption_ai_pick) and sabotages a corrupted one, Tyber Zann buys on the black market of the planet he corrupted,
// IG-88 hacks the Death Star II orbiting a corrupted planet. Per-frame CRCs identical for 0 / 1 / 4 / 15 workers and
// against tests/golden/ai_underworld_crc.txt (MSVC and clang check the same file).
static const char* kUwLibrary = R"(
require("SynAICommands")
function AssembleForce(taskforce)
  BlockOnCommand(taskforce.Produce_Force(taskforce.Get_Stage()))
end
function LandUnits(taskforce)
  BlockOnCommand(taskforce.Land_Units())
end
)";
static const char* kUwPlanBlackMarket = R"(
require("SynUnderworld")
function Definitions()
	Category = "Black_Market_Purchase"
	IgnoreTarget = true
	TaskForce = {
		{
			"MainForce",
			"DenyHeroAttach",
			"Silri_Team | Tyber_Zann_Team | Urai_Fen_Team = 1"
		}
	}
end
function MainForce_Thread()
	BlockOnCommand(MainForce.Produce_Force(Target))
	BlockOnCommand(MainForce.Move_To(Target))
	MainForce.Activate_Ability()
	MainForce.Set_Plan_Result(true)
end
function MainForce_Production_Failed(tf, failed_object_type)
	ScriptExit()
end
)";
static const char* kUwPlanSaboteur = R"(
require("SynUnderworld")
function Definitions()
	Category = "Corrupt_Planet | Sabotage_Planet"
	IgnoreTarget = true
	TaskForce = {
		{
			"MainForce",
			"DenyHeroAttach",
			"Underworld_Saboteur_Team = 1"
		}
	}
end
function MainForce_Thread()
	AssembleForce(MainForce)
	BlockOnCommand(MainForce.Move_To(Target))
	MainForce.Activate_Ability()
	Sleep(10)
	if MainForce.Get_Goal_Type_Name() == "Corrupt_Planet" then
		if Target.Is_Corrupted() then
			MainForce.Set_Plan_Result(true)
		end
	end
end
function MainForce_Production_Failed(tf, failed_object_type)
	ScriptExit()
end
)";
static const char* kUwPlanHack = R"(
require("SynUnderworld")
function Definitions()
	Category = "Hack_Super_Weapon"
	IgnoreTarget = true
	TaskForce = {
		{
			"MainForce",
			"DenyHeroAttach",
			"IG88_Team = 1"
		}
	}
end
function MainForce_Thread()
	BlockOnCommand(MainForce.Produce_Force(Target))
	BlockOnCommand(MainForce.Move_To(Target))
	BlockOnCommand(LandUnits(MainForce))
	MainForce.Set_Plan_Result(true)
end
function MainForce_Production_Failed(tf, failed_object_type)
	ScriptExit()
end
)";
static const char* kUwGoals = R"(<Goals>
  <Corrupt_Planet><AIGoalApplicationFlags> Enemy </AIGoalApplicationFlags><GameMode>Galactic</GameMode>
    <Category>Infrastructure</Category><Reachability>Any</Reachability><Time_Limit>60.0</Time_Limit>
    <Build_Time_Delay_Tolerance>1.25</Build_Time_Delay_Tolerance><Tracking_Duration>300.0</Tracking_Duration>
    <Per_Failure_Desire_Adjust>-20.0</Per_Failure_Desire_Adjust></Corrupt_Planet>
  <Black_Market_Purchase><AIGoalApplicationFlags> Enemy </AIGoalApplicationFlags><GameMode>Galactic</GameMode>
    <Category>Hero</Category><Reachability>Any</Reachability><Time_Limit>0.0</Time_Limit></Black_Market_Purchase>
  <Hack_Super_Weapon><AIGoalApplicationFlags> Enemy </AIGoalApplicationFlags><GameMode>Galactic</GameMode>
    <Category>Hero</Category><Reachability>Any</Reachability><Time_Limit>0.0</Time_Limit></Hack_Super_Weapon>
  <Sabotage_Planet><AIGoalApplicationFlags> Enemy </AIGoalApplicationFlags><GameMode>Galactic</GameMode>
    <Category>Offensive</Category><Reachability>Any</Reachability><Time_Limit>90.0</Time_Limit>
    <Build_Time_Delay_Tolerance>1.5</Build_Time_Delay_Tolerance><Tracking_Duration>300.0</Tracking_Duration>
    <Per_Failure_Desire_Adjust>-20.0</Per_Failure_Desire_Adjust></Sabotage_Planet>
</Goals>)";
static const char* kUwSet = R"(<FunctionSet>
  <Corrupt_Planet><Goal>Corrupt_Planet</Goal><Function>Should_Corrupt_Planet</Function></Corrupt_Planet>
  <Black_Market_Purchase><Goal>Black_Market_Purchase</Goal><Function>Should_Make_Black_Market_Purchase</Function></Black_Market_Purchase>
  <Hack_Super_Weapon><Goal>Hack_Super_Weapon</Goal><Function>Should_Hack_Super_Weapon</Function></Hack_Super_Weapon>
  <Sabotage_Planet><Goal>Sabotage_Planet</Goal><Function>Should_Sabotage_Planet</Function></Sabotage_Planet>
</FunctionSet>)";
static const char* kUwTemplates = R"(<AITemplates>
  <Uw_Default><Priority>1</Priority><Trigger>One</Trigger>
    <Budget><Offensive>Offensive_Budget</Offensive><Infrastructure>Infra_Budget</Infrastructure><Hero>Zero</Hero></Budget>
    <Turn_On><Goals><Category>Offensive</Category><Category>Infrastructure</Category><Category>Hero</Category></Goals>
      <Plans><Goal_Category>Offensive</Goal_Category><Goal_Category>Infrastructure</Goal_Category><Goal_Category>Hero</Goal_Category></Plans></Turn_On>
  </Uw_Default>
</AITemplates>)";
static const char* kUwPlayer = R"(<AIPlayerType><Name>SynUnderworld</Name>
  <GalacticFreeStoreScript>SynFreeStore</GalacticFreeStoreScript>
  <GoalProposalFunctionSets>UwSet</GoalProposalFunctionSets>
  <Templates><Galactic>Uw_Default</Galactic></Templates>
  <Difficulty_Adjustments><Easy>Normal_Default</Easy><Normal>Normal_Default</Normal><Hard>Normal_Default</Hard></Difficulty_Adjustments>
</AIPlayerType>)";

enum UwType : uint32_t { uPlanet, uStruct, uDeathStar, uTyber, uSaboteur, uIG88, uTypes };
constexpr PlanetIndex kUwPlanets = 6;  // P0 Underworld home; P1..P5 Empire; P2, P3 corrupted at start; DS II over P3

static gal::GalacticRules uw_rules() {
    gal::GalacticRules r;
    gal::FactionRules emp;
    emp.name = "Empire";
    emp.can_control_planets = emp.is_playable = true;
    gal::FactionRules uw = emp;
    uw.name = "Underworld";
    gal::FactionRules neu;
    neu.name = "Neutral";
    neu.is_neutral = true;
    r.factions = {emp, uw, neu};
    r.difficulties = {{"Normal_Default", 1.0f, 1.0f}};
    r.types.resize(uTypes);
    r.types[uPlanet].name = "UwPlanet";
    r.types[uPlanet].kind = gal::UnitKind::Planet;
    r.types[uPlanet].credit_value = 60;
    r.types[uStruct].name = "E_Ground_Research_Facility";
    r.types[uStruct].kind = gal::UnitKind::GroundStructure;
    r.types[uStruct].build_cost = 1200;
    r.types[uDeathStar].name = "Death_Star_II";
    r.types[uDeathStar].kind = gal::UnitKind::SpaceUnit;
    r.types[uDeathStar].ai_combat_power = 20000.0f;
    for (const auto& [t, name] : {std::pair{uTyber, "Tyber_Zann_Team"}, std::pair{uSaboteur, "Underworld_Saboteur_Team"},
                                  std::pair{uIG88, "IG88_Team"}}) {
        gal::TypeInfo& h = r.types[t];
        h.name = name;
        h.kind = gal::UnitKind::GroundUnit;  // heroes with their own transport (synthetic)
        h.hyperspace = true;
        h.hyperspace_speed = 2.0f;
        h.ai_combat_power = 300.0f;
    }
    r.types[uSaboteur].build_cost = 600;  // the generic hero is buildable (vanilla: the Underworld builds saboteurs)
    r.types[uSaboteur].build_time_seconds = 15;
    r.types[uSaboteur].affiliation = {1};
    r.types[uSaboteur].required_ground_base = 1;
    gal::SpecialRules& s = r.special;
    auto ability = [&s](gal::SpecialAbilityClass cls, const char* name, bool despawn) -> uint32_t {
        gal::SpecialAbilityDef a;
        a.cls = cls;
        a.name = name;
        a.name_crc = gal::special_name_crc(name);
        a.style = gal::ActivationStyle::GroundActivated;
        a.causes_despawn = despawn;
        s.abilities.push_back(a);
        return static_cast<uint32_t>(s.abilities.size() - 1);
    };
    const uint32_t bm = ability(gal::SpecialAbilityClass::BlackMarket, "Tyber_Black_Market", true);
    s.abilities[bm].bm_min_respawn = {20.0f, 20.0f, 20.0f, 20.0f, 20.0f};
    s.abilities[bm].bm_max_respawn = {40.0f, 40.0f, 40.0f, 40.0f, 40.0f};
    const uint32_t sab = ability(gal::SpecialAbilityClass::Sabotage, "Saboteur_Galactic_Sabotage", true);
    s.abilities[sab].sabotage_can[gal::kSabotageSpecialStructure] = true;  // the vanilla saboteur: only this option
    const uint32_t cor = ability(gal::SpecialAbilityClass::Corruption, "Saboteur_Corruption", true);
    const uint32_t hack = ability(gal::SpecialAbilityClass::Hack, "IG88_Hack_Death_Star", false);
    s.activated_sabotage = {"Saboteur_Galactic_Sabotage"};
    s.types.resize(uTypes);
    s.types[uTyber].abilities = {bm};
    s.types[uTyber].named_hero = true;
    s.types[uSaboteur].abilities = {sab, cor};
    s.types[uSaboteur].generic_hero = true;
    s.types[uIG88].abilities = {hack};
    s.types[uIG88].named_hero = true;
    s.types[uStruct].sabotage_cost = 1000;
    gal::CorruptionChoiceDef bmc;  // Corruption_1_Type = Black_Market (6), the only choice
    bmc.slot = 1;
    bmc.type = 6;
    bmc.credit_cost = 500.0f;
    s.types[uPlanet].corruption = {bmc};
    s.types[uPlanet].black_market_modifier = 100.0f;
    for (const auto& [name, tech, cost] : {std::tuple{"Uw_Item_Low", 1, 1000}, std::tuple{"Uw_Item_High", 3, 2000}}) {
        gal::BlackMarketItemDef it;
        it.name = name;
        it.name_crc = gal::special_name_crc(name);
        it.faction = 0;  // Empire items: offered on Empire planets
        it.tech_level = tech;
        it.base_cost = cost;
        it.uncommon_price_multiplier = 5.5f;
        s.items.push_back(it);
    }
    s.default_hero_respawn_time = 30.0f;
    s.hack_required_type = uDeathStar;
    return r;
}

static gal::GalacticSetup uw_setup() {
    gal::GalacticSetup s;
    GalaxyMap& m = s.map;
    m.campaign_name = "SyntheticUnderworld";
    for (PlanetIndex i = 0; i < kUwPlanets; ++i) {
        m.planets.add("U" + std::to_string(i));
        m.planets.position[i] = {static_cast<float>(i) * 50.0f, 0.0f, 0.0f};
        m.planets.flags[i] = kPlanetHasPlanetBehavior;
        m.planets.max_space_base[i] = 2;
        m.planets.max_ground_base[i] = 3;
    }
    for (PlanetIndex i = 0; i + 1 < kUwPlanets; ++i) {
        const uint32_t r = m.routes.add("UR" + std::to_string(i));
        m.routes.point_a[r] = i;
        m.routes.point_b[r] = i + 1;
        m.routes.hs_speed_factor[r] = 4.0f;
        m.routes.credit_gain_factor[r] = 0.0f;
    }
    s.planet_type.assign(kUwPlanets, uPlanet);
    gal::PlayerSetup emp;  // AI-controlled, no galactic AI attached: passive
    emp.faction = 0;
    emp.ai = true;
    emp.difficulty = 0;
    emp.credits = 5000.0f;
    gal::PlayerSetup uw = emp;
    uw.faction = 1;
    uw.credits = 20000.0f;
    gal::PlayerSetup neu;
    neu.faction = 2;
    s.players = {emp, uw, neu};
    s.forces = {{1, 0, uTyber}, {1, 0, uSaboteur}, {1, 0, uIG88}, {1, 0, uStruct}};  // a ground base at home
    for (PlanetIndex p = 1; p < kUwPlanets; ++p) s.forces.push_back({0, p, uStruct});
    s.forces.push_back({0, 3, uDeathStar});
    return s;
}

struct UwResult {
    std::vector<uint64_t> crc;
    std::vector<std::string> decisions, errors;
    std::string summary;
    int8_t corruption_p1 = -1;
    int32_t completed_bm = 0, sabotage_p2 = 0;
    size_t purchased = 0;
    bool p3_destroyed = false, ds_locked = false, ds_alive = true;
    float uw_credits = 0.0f;
};

static UwResult run_underworld(unsigned threads, int32_t frames) {
    UwResult out;
    MemoryFiles files;
    AiDefinitions defs;
    EquationSet eqs;
    files.add("Data\\Scripts\\Library\\SynBase.lua", kSynBase);
    files.add("Data\\Scripts\\Library\\SynAICommands.lua", kSynAICommands);
    files.add("Data\\Scripts\\Library\\SynUnderworld.lua", kUwLibrary);
    files.add("Data\\Scripts\\AI\\AI_Plan_Underworld_BlackMarket.lua", kUwPlanBlackMarket);
    files.add("Data\\Scripts\\AI\\AI_Plan_Underworld_DeploySaboteur.lua", kUwPlanSaboteur);
    files.add("Data\\Scripts\\AI\\AI_Plan_Underworld_HackSuperWeapon.lua", kUwPlanHack);
    files.add("Data\\Scripts\\FreeStore\\SynFreeStore.lua", kSynFreeStore);
    defs.add_goals(kUwGoals, "goals.xml");
    defs.link();
    defs.add_function_set("UwSet", kUwSet, "UwSet.xml");
    defs.add_templates(kUwTemplates, "t.xml");
    defs.add_players(kUwPlayer, "p.xml");
    eqs.add("One", "1.0");
    eqs.add("Zero", "0.0");
    eqs.add("Offensive_Budget", "5");
    eqs.add("Infra_Budget", "5");
    // AI_EQUATIONS_UNDERWORLD_GALACTIC.XML (Can_* / Should_* verbatim except the reduced terms noted).
    eqs.add("Can_Corrupt_Planet",
            "Variable_Target.RetainsResidualInfluence * (Variable_Target.IsCorrupted == 0.0) * "
            "(Variable_Target.IsCorruptionTransitionActive == 0.0) * "
            "((Variable_Target.IsConnectedToCorruption + (Game.Age > Variable_Target.TimeSinceCorruptionChange)) > 0)");
    eqs.add("Should_Corrupt_Planet",
            "Function_Can_Corrupt_Planet.Evaluate * (13.0 + 5.0 * Variable_Target.IsConnectedToCorruption + "
            "5.0 * Variable_Target.IncomeUnnormalized / 75)");
    eqs.add("Can_Make_Black_Market_Purchase",
            "Variable_Target.IsCorrupted * Variable_Target.BlackMarketAbilitiesAvailable {Parameter_Faction = \"Underworld\"}");
    eqs.add("Should_Make_Black_Market_Purchase",
            "Function_Can_Make_Black_Market_Purchase.Evaluate * "
            "(Variable_Self.CreditsUnnormalized > Variable_Target.BlackMarketMinimumAbilityPrice {Parameter_Faction = \"Underworld\"}) * "
            "(10.0 - Variable_Target.BlackMarketPriceModifier / 300.0)");
    eqs.add("Should_Hack_Super_Weapon",  // the Palpatine / planets-controlled weights reduced to a constant
            "Variable_Target.IsCorrupted * (Variable_Target.EnemyForce.HasSpaceUnitsBitfield {Parameter_Type = \"Death_Star_II\"} > 0.0) * 10.0");
    eqs.add("Can_Sabotage_Planet", "Variable_Target.IsCorrupted * (Variable_Target.GroundbaseLevelUnnormalized > 0)");
    eqs.add("Should_Sabotage_Planet",  // GenericPlanetValue / Should_Save_For_Corruption terms dropped
            "Function_Can_Sabotage_Planet.Evaluate * (50.0 + 10.0 * Variable_Target.HasStructure {Parameter_Type = \"E_Ground_Research_Facility\"})");
    eqs.link();
    const std::vector<std::string> plan_files = list_plan_files([&files](std::string_view glob) {
        const std::string g(glob);
        return files.list(g.substr(0, g.find('*')));
    });
    const gal::GalacticRules rules = uw_rules();
    objects::GameObjectTypes types;
    objects::IdCounter ids;
    objects::ObjectStore store(objects::ModeKind::Galactic, objects::StoreKind::Primary, types, &ids);
    GameRandom rng(5150u, 3u);
    gal::GalacticSim sim(rules, store, rng.synced());
    if (threads > 0) sim.set_parallel_for(thread_pf(threads));
    std::vector<std::string> diag;
    if (!sim.start(uw_setup(), &diag)) {
        std::printf("underworld: start failed\n");
        ++g_fail;
        return out;
    }
    sim.set_corruption(2, 3, 1);  // Racketeering by the Underworld: a sabotage target
    sim.set_corruption(3, 3, 1);  // the Death Star II planet: a hack target
    std::vector<GalacticAiTypeInfo> tinfo(uTypes);
    tinfo[uTyber].hero = tinfo[uIG88].hero = true;
    tinfo[uSaboteur].generic_hero = true;
    tinfo[uStruct].categories = {"STRUCTURE"};
    tinfo[uDeathStar].categories = {"CAPITAL"};
    for (const uint32_t h : {uint32_t{uTyber}, uint32_t{uSaboteur}, uint32_t{uIG88}}) tinfo[h].categories = {"INFANTRY"};
    GalacticAiConfig cfg;
    cfg.players = {{1, "SynUnderworld", 1}};
    cfg.difficulties = {{"Normal_Default", 1.1f, 0.0f}};
    GalacticAiWorld world(sim, store, &types, &files, rng, defs, eqs, tinfo,
                          [&sim](uint8_t p, const commands::EventData& ev) { return sim.execute(p, ev); }, cfg);
    std::vector<std::string> pd;
    CHECK(world.load_plans(plan_files, &pd));
    for (const std::string& d : pd) std::printf("  underworld plans: %s\n", d.c_str());
    CHECK(world.plans().plans.size() == 3);
    CHECK(world.start(&pd));
    for (int32_t f = 1; f <= frames; ++f) {
        sim.update();
        for (int32_t p = 0; p < static_cast<int32_t>(sim.players().size()); ++p) world.update_player(p, f, true, false);
        StateHasher h;
        sim.hash_state(h);
        world.hash(h);
        rng.synced().hash(h);
        out.crc.push_back(h.digest());
    }
    for (const AiDecision& d : world.decision_log())
        out.decisions.push_back(std::to_string(d.frame) + " p" + std::to_string(d.player) + " " + d.text);
    out.errors = world.lua_errors();
    out.corruption_p1 = sim.planets()[1].corruption;
    out.completed_bm = sim.corruptions_completed(1, 6);
    out.sabotage_p2 = sim.sabotage_count(2);
    out.purchased = sim.bm_purchased(1).size();
    out.p3_destroyed = sim.planets()[3].destroyed;
    out.ds_locked = sim.type_locked(0, uDeathStar);
    out.ds_alive = false;
    for (const gal::FleetState& fl : sim.fleets())
        if (fl.alive)
            for (const objects::ObjectHandle u : fl.contents) out.ds_alive = out.ds_alive || (store.alive(u) && store.type(u) == uDeathStar);
    out.uw_credits = sim.credits(1);
    char buf[512];
    std::snprintf(buf, sizeof buf,
                  "frames %d P1 corruption %d (black-market corruptions %d) P2 sabotages %d purchased %zu P3 destroyed %d "
                  "DS locked %d DS alive %d credits U=%.0f orders %llu",
                  frames, static_cast<int>(out.corruption_p1), out.completed_bm, out.sabotage_p2, out.purchased,
                  out.p3_destroyed ? 1 : 0, out.ds_locked ? 1 : 0, out.ds_alive ? 1 : 0, static_cast<double>(out.uw_credits),
                  static_cast<unsigned long long>(world.orders_issued()));
    out.summary = buf;
    return out;
}

static void test_underworld_special() {
    const int32_t frames = 30 * 600;  // 10 minutes of galactic time
    const UwResult base = run_underworld(0, frames);
    std::printf("underworld (0 workers): %s, %zu decisions, %zu Lua errors\n", base.summary.c_str(), base.decisions.size(),
                base.errors.size());
    for (size_t i = 0; i < base.errors.size() && i < 10; ++i) std::printf("  lua error: %s\n", base.errors[i].c_str());
    for (const std::string& d : base.decisions)
        if (d.find("ability") != std::string::npos) std::printf("  %s\n", d.c_str());
    CHECK(base.errors.empty());
    CHECK(base.corruption_p1 == 6 && base.completed_bm >= 1);  // Corrupt_Planet: corruption_ai_pick on a clean planet
    CHECK(base.sabotage_p2 >= 1);                              // Sabotage_Planet: a structure of the corrupted P2
    CHECK(base.purchased >= 1);                                // Black_Market_Purchase at the Black_Market planet
    CHECK(base.p3_destroyed && base.ds_locked && !base.ds_alive);  // Hack_Super_Weapon through Land_Units
    for (const unsigned t : {1u, 4u, 15u}) {
        const UwResult r = run_underworld(t, frames);
        size_t first = r.crc.size() == base.crc.size() ? r.crc.size() : 0;
        for (size_t i = 0; i < std::min(r.crc.size(), base.crc.size()); ++i)
            if (r.crc[i] != base.crc[i]) {
                first = i;
                break;
            }
        std::printf("  underworld workers %u vs 0: %s\n", t, first == r.crc.size() ? "identical" : ("DIVERGED at frame " + std::to_string(first + 1)).c_str());
        CHECK(first == r.crc.size() && r.decisions == base.decisions);
    }
    std::ostringstream o;
    o << "# P4 ai synthetic Underworld special systems: per-frame CRC (every 300th frame + last), decisions\n";
    for (size_t i = 0; i < base.crc.size(); ++i)
        if ((i + 1) % 300 == 0 || i + 1 == base.crc.size()) {
            char b[64];
            std::snprintf(b, sizeof b, "frame %zu %016llx\n", i + 1, static_cast<unsigned long long>(base.crc[i]));
            o << b;
        }
    o << base.summary << "\n";
    for (const std::string& d : base.decisions) o << d << "\n";
    const std::string text = o.str();
    const std::string path = std::string(ALAMO_AI_GOLDEN_DIR) + "/ai_underworld_crc.txt";
    if (g_write_golden) {
        std::ofstream(std::filesystem::path(path), std::ios::binary) << text;
        std::printf("  wrote %s\n", path.c_str());
    } else {
        std::ifstream in(std::filesystem::path(path), std::ios::binary);
        std::string want((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        want.erase(std::remove(want.begin(), want.end(), '\r'), want.end());
        const bool same = want == text;
        std::printf("  golden %s: %s\n", path.c_str(), same ? "MATCH" : (want.empty() ? "MISSING" : "DIFFERS"));
        if (!same) {
            const std::string actual = std::string(ALAMO_AI_LOG_DIR) + "/ai_underworld_actual.txt";
            std::ofstream(std::filesystem::path(actual), std::ios::binary) << text;
            std::printf("  actual written to %s\n", actual.c_str());
        }
        CHECK(same);
    }
}

// ================================================================ N1 save / load
//
// Two identical rigs (sim + store + RNG + conflicts + AI) run in lockstep. At the save point rig A's AI world is
// written (B's bytes must be equal), A then rejects broken streams without changing, B's AI world is destroyed and
// rebuilt as a loader would (load_plans + start on the same sim) and reads A's bytes. B's sim and store are not
// reloaded: they are A's twin, so only the AI state crosses the save. With Lua state persistence (LuaVm::write /
// read) both rigs then continue in lockstep comparing the per-tick CRC (sim, AI hash, path finders, RNG, conflicts)
// and every order each AI issued; without it (the n1-lua stubs) the Lua-free part is checked and the test reports
// PENDING.

struct OrderRecord {
    uint8_t player = 0;
    commands::EventData ev;
    commands::RejectReason why{};
    friend bool operator==(const OrderRecord&, const OrderRecord&) = default;
};

struct SaveRigInputs {
    const gal::GalacticRules* rules = nullptr;
    const gal::GalacticSetup* setup = nullptr;
    const objects::GameObjectTypes* types = nullptr;
    const FileProvider* files = nullptr;
    const AiDefinitions* defs = nullptr;
    const EquationSet* eqs = nullptr;
    std::vector<GalacticAiTypeInfo> tinfo;
    GalacticAiConfig cfg;
    std::vector<std::string> plan_files;
    uint32_t seed_synced = 1, seed_free = 1;
    bool conflicts = false;
};

class SaveRig {
public:
    explicit SaveRig(const SaveRigInputs& in) : rng(in.seed_synced, in.seed_free), in_(in) {}
    SaveRig(const SaveRig&) = delete;
    SaveRig& operator=(const SaveRig&) = delete;

    bool start() {
        store = std::make_unique<objects::ObjectStore>(objects::ModeKind::Galactic, objects::StoreKind::Primary, *in_.types, &ids_);
        sim = std::make_unique<gal::GalacticSim>(*in_.rules, *store, rng.synced());
        std::vector<std::string> diag;
        if (!sim->start(*in_.setup, &diag)) return false;
        if (in_.conflicts) {
            conflicts = std::make_unique<gal::ConflictManager>(*sim, crules_, rng.synced());
            conflicts->attach();
        }
        return make_world();
    }
    // A new AI world on this rig's sim, as a loader builds it before reading: load_plans + start.
    bool make_world() {
        world.reset();
        world = std::make_unique<GalacticAiWorld>(
            *sim, *store, in_.types, in_.files, rng, *in_.defs, *in_.eqs, in_.tinfo,
            [this](uint8_t p, const commands::EventData& ev) {
                const commands::RejectReason why = sim->execute(p, ev);
                orders.push_back({p, ev, why});
                return why;
            },
            in_.cfg);
        std::vector<std::string> pd;
        return world->load_plans(in_.plan_files, &pd) && world->start(&pd);
    }
    void tick(int32_t frame) {
        orders.clear();
        sim->update();
        const bool busy = conflicts && conflicts->state() == gal::ConflictState::AutoRunning;
        for (int32_t p = 0; p < static_cast<int32_t>(sim->players().size()); ++p) world->update_player(p, frame, true, busy);
    }
    uint64_t crc() const {
        StateHasher h;
        sim->hash_state(h);
        world->hash(h);
        world->hash_paths(h);
        rng.synced().hash(h);
        if (conflicts) conflicts->hash(h);
        return h.digest();
    }
    uint64_t ai_digest() const {
        StateHasher h;
        world->hash(h);
        world->hash_paths(h);
        return h.digest();
    }
    std::vector<uint8_t> save() const {
        ByteWriter w;
        world->write(w);
        return w.bytes();
    }
    bool load(const std::vector<uint8_t>& bytes) {
        ByteReader r(bytes);
        return world->read(r);
    }

    std::unique_ptr<objects::ObjectStore> store;
    GameRandom rng;
    std::unique_ptr<gal::GalacticSim> sim;
    std::unique_ptr<gal::ConflictManager> conflicts;
    std::unique_ptr<GalacticAiWorld> world;
    std::vector<OrderRecord> orders;  // this frame's AI orders, issue order

private:
    const SaveRigInputs& in_;
    objects::IdCounter ids_;
    gal::ConflictRules crules_;
};

// LuaVm::write / read implemented (false while engine/lua still has the N1 stubs).
static bool lua_state_persists() {
    lua::LuaVm a{lua::LuaHost{}};
    lua::LuaVm b{lua::LuaHost{}};
    ByteWriter w;
    a.write(w);
    ByteReader r(w.bytes());
    return b.read(r) && r.done();
}

// Ticks `a` and every rig of `others` together; false at the first frame whose CRC or AI orders differ from a's.
static bool save_lockstep(SaveRig& a, std::initializer_list<SaveRig*> others, int32_t first, int32_t frames, const char* label) {
    for (int32_t f = first; f < first + frames; ++f) {
        a.tick(f);
        const uint64_t ca = a.crc();
        for (SaveRig* o : others) {
            o->tick(f);
            const uint64_t co = o->crc();
            if (ca != co || a.orders != o->orders) {
                std::printf("  %s: rigs differ at frame %d (crc %016llx / %016llx, %zu / %zu orders)\n", label, f,
                            static_cast<unsigned long long>(ca), static_cast<unsigned long long>(co), a.orders.size(),
                            o->orders.size());
                return false;
            }
        }
    }
    return true;
}

template <class T>
static std::vector<uint8_t> save_chunk(const T& x) {
    ByteWriter w;
    x.write(w);
    return w.bytes();
}
template <class T>
static bool load_chunk(T& x, const std::vector<uint8_t>& bytes) {
    ByteReader r(bytes);
    return x.read(r) && r.done();
}

// Broken streams: read() fails and leaves the running world (and the shared RNG) exactly as it was.
static void save_rejects(SaveRig& a, const std::vector<uint8_t>& bytes, int cuts, const char* label) {
    const uint64_t digest = a.ai_digest();
    const GameRandom rng = a.rng;
    const std::vector<uint8_t> before = a.save();
    int accepted = 0;
    auto reject = [&](const std::vector<uint8_t>& d, const char* what) {
        if (a.load(d)) {
            ++accepted;
            std::printf("  %s: %s (%zu bytes) accepted\n", label, what, d.size());
        }
        CHECK(a.ai_digest() == digest && a.rng == rng);
    };
    reject({}, "empty stream");
    std::vector<uint8_t> v = bytes;
    v[0] = static_cast<uint8_t>(v[0] ^ 0xFFu);
    reject(v, "bad version");
    v = bytes;
    v[4] = static_cast<uint8_t>(v[4] ^ 1u);
    reject(v, "AI player count");
    v = bytes;
    v.push_back(0);
    reject(v, "trailing byte");
    reject(std::vector<uint8_t>(bytes.begin(), bytes.end() - 1), "truncated by one byte");
    for (int k = 1; k < cuts; ++k) {
        const auto cut = static_cast<std::ptrdiff_t>(bytes.size() * static_cast<size_t>(k) / static_cast<size_t>(cuts));
        reject(std::vector<uint8_t>(bytes.begin(), bytes.begin() + cut), "truncated stream");
    }
    CHECK(accepted == 0);
    CHECK(a.save() == before);
}

// `full_session`: also load the whole session (store, sim, conflicts, AI) into a third, fresh rig C. Needs object types
// from a GameObjectTypes database (ObjectStore::read rejects type indices it does not know).
// `max_bytes` / `max_packed` (0: unchecked): size guards (N1 save size) on the AI save, as written and as a save file
// packs it (lz_compress).
static void check_save_load(const SaveRigInputs& in, const char* label, int32_t warm, int32_t after, int cuts, bool full_session,
                            size_t max_bytes = 0, size_t max_packed = 0) {
    SaveRig a(in), b(in), c(in);
    const bool started = a.start() && b.start() && (!full_session || c.start());
    CHECK(started);
    if (!started) return;
    // Save at the first frame from `warm` on with running plans (task forces, statuses, build tasks live).
    int32_t frame = warm;
    bool twins = save_lockstep(a, {&b}, 1, warm, label);
    while (twins && a.world->running_plans() == 0 && frame < warm + 30 * 600) twins = save_lockstep(a, {&b}, ++frame, 1, label);
    CHECK(twins);  // the rigs are deterministic twins
    if (!twins) return;
    const bool lua = lua_state_persists();
    const std::vector<uint8_t> bytes = a.save();
    if (bytes != b.save()) {
        // Twin worlds must save the same bytes; tell the AI's own sections from the Lua VM images.
        a.world->set_save_lua_state(false);
        b.world->set_save_lua_state(false);
        const bool core = a.save() == b.save();
        a.world->set_save_lua_state(true);
        b.world->set_save_lua_state(true);
        CHECK(core && lua);
        std::printf("  %s: twin saves differ %s\n", label,
                    core ? "only inside the Lua VM images (LuaVm::write of twin VMs not byte-identical)" : "in the AI sections");
    }
    const uint64_t digest = a.ai_digest();
    const size_t packed = lz_compress(bytes).size();
    std::printf("N1 save %s: frame %d, %zu bytes (%zu packed), %zu running plans, %llu orders so far\n", label, frame,
                bytes.size(), packed, a.world->running_plans(), static_cast<unsigned long long>(a.world->orders_issued()));
    CHECK(a.world->running_plans() > 0);
    if (max_bytes && bytes.size() > max_bytes) std::printf("  %s: AI save larger than its %zu-byte guard\n", label, max_bytes);
    if (max_packed && packed > max_packed) std::printf("  %s: packed AI save larger than its %zu-byte guard\n", label, max_packed);
    CHECK(!max_bytes || bytes.size() <= max_bytes);
    CHECK(!max_packed || packed <= max_packed);
    // The rest of the session at the save point (n1-objects / n1-galactic), for the full load into rig C.
    const std::vector<uint8_t> s_rng = save_chunk(a.rng), s_store = save_chunk(*a.store), s_sim = save_chunk(*a.sim);
    const std::vector<uint8_t> s_conflicts = a.conflicts ? save_chunk(*a.conflicts) : std::vector<uint8_t>{};

    save_rejects(a, bytes, cuts, label);
    a.world->set_save_lua_state(false);  // the same with the deeper sections reached (fresh VMs staged, then dropped)
    save_rejects(a, bytes, cuts, label);
    a.world->set_save_lua_state(true);
    CHECK(a.ai_digest() == digest && a.crc() == b.crc());

    // B: the loader's rebuild of the AI world on B's (twin) sim. B's RNG as it was: building a world keeps no draw.
    const GameRandom rng_before = b.rng;
    const bool rebuilt = b.make_world();
    CHECK(rebuilt);
    if (!rebuilt) return;
    b.rng = rng_before;
    // C: a whole session loaded into a fresh rig (started from the setup, never ticked): RNG, objects, galactic sim,
    // conflicts, then the AI (objects resolve by id in the restored store).
    const char* failed = !full_session                                           ? nullptr
                         : !load_chunk(c.rng, s_rng)                               ? "RNG"
                         : !load_chunk(*c.store, s_store)                         ? "ObjectStore"
                         : !load_chunk(*c.sim, s_sim)                             ? "GalacticSim"
                         : c.conflicts && !load_chunk(*c.conflicts, s_conflicts) ? "ConflictManager"
                                                                                  : nullptr;
    const bool session = failed == nullptr;
    if (failed) std::printf("  %s: full session: %s read failed\n", label, failed);
    CHECK(session);
    if (!lua) {
        b.world->set_save_lua_state(false);
        if (full_session) c.world->set_save_lua_state(false);
    }
    const bool loaded_b = b.load(bytes), loaded_c = !full_session || (session && c.load(bytes));
    CHECK(loaded_b && loaded_c);
    if (!loaded_b || !loaded_c) {
        std::printf("  %s: read() failed (rebuilt world %s, full session %s)\n", label, loaded_b ? "ok" : "FAILED",
                    !session ? "not loaded" : loaded_c ? "ok" : "FAILED");
        return;
    }
    // Without Lua state (stubs) A's images are empty too, so the byte comparisons hold in both cases.
    const bool hashes = b.ai_digest() == digest && b.crc() == a.crc() &&
                        (!full_session || (c.ai_digest() == digest && c.crc() == a.crc()));
    const bool resave = b.save() == bytes && (!full_session || c.save() == bytes);  // save -> load -> save
    CHECK(hashes && resave);
    if (!lua) {
        std::printf("N1 save %s: PENDING n1-lua (LuaVm::write / read are stubs): rebuilt world%s loaded without the Lua VM "
                    "images, AI hash + paths + CRC %s, re-save %s; lockstep continuation not run\n", label,
                    full_session ? " + full session" : "", hashes ? "identical" : "DIFFER", resave ? "byte-identical" : "DIFFERS");
        return;
    }
    const size_t mark = a.world->decision_log().size();
    const bool same = full_session ? save_lockstep(a, {&b, &c}, frame + 1, after, label)
                                   : save_lockstep(a, {&b}, frame + 1, after, label);
    CHECK(same);
    const auto& la = a.world->decision_log();
    bool logs = true;
    for (const SaveRig* r : {&b, &c}) {
        if (r == &c && !full_session) continue;
        const auto& lr = r->world->decision_log();
        logs = logs && la.size() - mark == lr.size();
        for (size_t i = 0; logs && i < lr.size(); ++i)
            logs = la[mark + i].frame == lr[i].frame && la[mark + i].player == lr[i].player && la[mark + i].text == lr[i].text;
    }
    CHECK(logs);
    std::printf("N1 save %s: loaded into a rebuilt world%s, %d lockstep frames %s (per-tick CRC + orders), %zu decisions "
                "after the save %s, final crc %016llx\n", label, full_session ? " and a fully loaded session" : "", after,
                same ? "identical" : "DIFFER",
                la.size() - mark, logs ? "identical" : "DIFFER", static_cast<unsigned long long>(a.crc()));
}

static void test_save_synthetic() {
    MemoryFiles files;
    AiDefinitions defs;
    EquationSet eqs;
    SaveRigInputs in;
    syn_data(files, defs, eqs, in.plan_files);
    const gal::GalacticRules rules = syn_rules();
    const gal::GalacticSetup setup = syn_setup();
    objects::GameObjectTypes types;
    in.rules = &rules;
    in.setup = &setup;
    in.types = &types;
    in.files = &files;
    in.defs = &defs;
    in.eqs = &eqs;
    in.tinfo.resize(tTypes);
    in.tinfo[tFrigate].categories = {"FRIGATE"};
    in.tinfo[tCorvette].categories = {"CORVETTE"};
    in.tinfo[tInfantry].categories = {"INFANTRY"};
    in.tinfo[tSB1].categories = in.tinfo[tSB2].categories = {"STRUCTURE"};
    in.cfg.players = {{0, "SynEmpire", 1}, {1, "SynRebel", 1}};
    in.cfg.difficulties = {{"Normal_Default", 1.1f, 0.0f}};
    in.seed_synced = 4242u;
    in.seed_free = 77u;
    in.conflicts = true;
    check_save_load(in, "synthetic", 30 * 90, 30 * 240, 48, false);
}

// ================================================================ FoC data

#ifdef ALAMO_AI_HAVE_VFS
static std::string env(const char* name) {
#ifdef _MSC_VER
    char* v = nullptr;
    size_t len = 0;
    std::string out;
    if (_dupenv_s(&v, &len, name) == 0 && v) {
        out = v;
        std::free(v);
    }
    return out;
#else
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
#endif
}

static void test_foc_data() {
    const std::string root = env("ALAMO_FOC_DATA");
    std::error_code ec;
    if (root.empty() || !std::filesystem::is_directory(path_from_utf8(root) / "Data", ec)) {
        std::printf("FoC data: ALAMO_FOC_DATA not set: skipped\n");
        return;
    }
    vfs::VfsConfig cfg;
    cfg.foc_root = root;
    std::string err;
    auto v = vfs::Vfs::create(cfg, &err);
    if (!v) {
        std::printf("FoC data: VFS failed (%s): skipped\n", err.c_str());
        return;
    }
    AiDefinitions defs;
    const AiDataPaths paths = list_ai_data([&v](std::string_view g) { return v->list(g); });
    CHECK(load_ai_definitions(*v, paths, defs));
    std::printf("FoC AI data: %zu categories, %zu goal types, %zu function sets, %zu player types, %zu templates, %zu diagnostics\n",
                defs.categories.size(), defs.goals.size(), defs.sets.size(), defs.players.size(), defs.templates.size(),
                defs.diagnostics.size());
    for (size_t i = 0; i < defs.diagnostics.size() && i < 12; ++i) std::printf("  %s\n", defs.diagnostics[i].c_str());
    CHECK(defs.goals.size() > 100 && defs.find_player("BasicEmpire") && defs.find_template("Basic_Empire_Default"));
    const uint32_t cq = defs.find_goal("Conquer_Opponent");
    CHECK(cq != kAiNone && defs.goals[cq].time_limit == 240.0f && defs.goals[cq].category == defs.find_category("Offensive"));

    // Every shipped plan script runs Base_Definitions on the Lua bridge and yields a definition.
    data::GameDatabase db;
    objects::ObjectTypeDatabase raw;
    objects::GameObjectTypes types;
    gal::GalacticRules rules;
    std::vector<std::string> diag;
    if (!db.load(*v, {}, &diag) || !raw.load(*v, &diag) || !types.build(raw, v.get(), &diag) ||
        !gal::load_galactic_rules(*v, db, types, rules, &diag)) {
        std::printf("FoC data: game database incomplete: skipped\n");
        return;
    }
    EquationSet eqs;
    load_equation_files(*v, v->list(kPerceptionEquationGlob), eqs);
    const gal::GalacticSetup setup = gal::build_campaign_setup(db, rules, "Sandbox_Heart_Of_The_Maw_Underworld", {}, &diag);
    objects::IdCounter ids;
    objects::ObjectStore store(objects::ModeKind::Galactic, objects::StoreKind::Primary, types, &ids);
    GameRandom rng(99u, 1u);
    gal::GalacticSim sim(rules, store, rng.synced());
    CHECK(sim.start(setup, &diag));
    GalacticAiConfig gcfg;
    for (size_t p = 0; p < sim.players().size(); ++p)
        if (sim.players()[p].ai) {
            const std::string fac = rules.factions[sim.players()[p].faction].name;
            gcfg.players.push_back({static_cast<int32_t>(p),
                                    ai_iequal(fac, "Empire") ? "BasicEmpire" : (ai_iequal(fac, "Rebel") ? "BasicRebel" : "AI_Player_Underworld"), 1});
        }
    gcfg.difficulties = load_ai_difficulties(*v);
    // A2 t03: the starbases' Starting_Spawned_Units_Tech_0 lists (type, count; repeated tags appended) reach the AI type
    // info, so the typed force rows hold squadrons instead of the starbase's own AI_Combat_Power.
    const std::vector<GalacticAiTypeInfo> foc_tinfo = make_galactic_ai_type_info(types);
    {
        const uint32_t sb1 = rules.find_type("Rebel_Star_Base_1"), xw = rules.find_type("Rebel_X-Wing_Squadron"), yw = rules.find_type("Y-Wing_Squadron");
        CHECK(sb1 != gal::kNoIndex && sb1 < foc_tinfo.size() && xw != gal::kNoIndex && yw != gal::kNoIndex);
        if (sb1 < foc_tinfo.size()) {
            const auto& l = foc_tinfo[sb1].spawn_tech[0];
            CHECK(l.size() == 2 && l[0].type == xw && l[0].count == 2 && l[1].type == yw && l[1].count == 2);
            CHECK(foc_tinfo[sb1].spawn_tech[1].empty() && rules.types[sb1].ai_combat_power == 5000.0f);
        }
    }
    GalacticAiWorld world(sim, store, &types, v.get(), rng, defs, eqs, foc_tinfo,
                          [&sim](uint8_t p, const commands::EventData& ev) { return sim.execute(p, ev); }, gcfg);
    std::vector<std::string> pd;
    const std::vector<std::string> plan_files = list_plan_files([&v](std::string_view g) { return v->list(g); });
    CHECK(world.load_plans(plan_files, &pd));
    std::printf("FoC plans: %zu files, %zu definitions loaded, %zu diagnostics\n", plan_files.size(), world.plans().plans.size(), pd.size());
    for (size_t i = 0; i < pd.size() && i < 15; ++i) std::printf("  %s\n", pd[i].c_str());
    CHECK(world.plans().find("ConquerOpponentPlan") != kAiNone && world.plans().find("BuildSpaceForcesPlan") != kAiNone);
    const uint32_t cop = world.plans().find("ConquerOpponentPlan");
    if (cop != kAiNone) {
        const PlanDefinition& d = world.plans().plans[cop];
        CHECK(d.task_forces.size() == 2 && d.task_forces[0].name == "SpaceForce" && d.task_forces[0].minimum_total_size == 4 &&
              d.min_contrast_scale == 1.25f && d.per_failure_contrast_adjust == 0.5f && d.required_categories.size() == 2);
    }
    // Token coverage of the galactic goal / budget / template equations.
    CHECK(world.start(&pd));
    for (int32_t f = 1; f <= 30 * 120; ++f) {
        sim.update();
        for (int32_t p = 0; p < static_cast<int32_t>(sim.players().size()); ++p) world.update_player(p, f, true, false);
    }
    // Which goal functions evaluate on this galaxy (per AI player, every planet / global).
    for (size_t p = 0; p < sim.players().size(); ++p) {
        const AiContext* c = world.context(static_cast<int32_t>(p));
        if (!c) continue;
        uint32_t ok = 0, bad = 0, positive = 0;
        std::map<std::string, std::string> failing;
        for (const GoalSource& s : c->goals.sources()) {
            const uint32_t e = eqs.find(s.function);
            const bool global = defs.goals[s.goal_type].global();
            for (int32_t t = global ? -1 : 0; t < (global ? 0 : static_cast<int32_t>(sim.planets().size())); ++t) {
                EvalContext ctx;
                ctx.self = static_cast<int32_t>(p);
                ctx.target = ctx.location = t;
                ctx.enemy = c->opponent;
                ctx.human = c->opponent;
                SimRng scratch(1);
                ctx.rng = &scratch;
                const EvalResult r = e == kUnresolved ? EvalResult{} : evaluate(eqs, e, ctx, world.resolver());
                if (r.ok) {
                    ++ok;
                    positive += r.value > 0.0 ? 1u : 0u;
                } else {
                    ++bad;
                    failing[s.function] = e == kUnresolved ? "missing" : eval_error_name(r.error);
                }
            }
        }
        std::printf("FoC player %zu goal functions: %u evaluations ok (%u > 0), %u failed:", p, ok, positive, bad);
        for (const auto& [n, why] : failing) std::printf(" %s(%s)", n.c_str(), why.c_str());
        std::printf("\n");
    }
    size_t active = 0;
    for (size_t p = 0; p < sim.players().size(); ++p)
        if (const AiContext* c = world.context(static_cast<int32_t>(p)))
            for (const auto& cat : c->goals.active()) active += cat.size();
    std::printf("FoC HotM 120 s: %zu decisions, %zu running plans, %zu active goals, orders %llu (rejected %llu), %zu Lua errors\n",
                world.decision_log().size(), world.running_plans(), active, static_cast<unsigned long long>(world.orders_issued()),
                static_cast<unsigned long long>(world.orders_rejected()), world.lua_errors().size());
    for (const auto& [k, n] : world.proposal_stats()) std::printf("  proposals: %s x%u\n", k.c_str(), n);
    const std::vector<std::string> le = world.lua_errors();
    for (size_t i = 0; i < le.size() && i < 10; ++i) std::printf("  lua: %s\n", le[i].c_str());
    for (size_t i = 0; i < world.decision_log().size() && i < 25; ++i)
        std::printf("  %lld p%d %s\n", static_cast<long long>(world.decision_log()[i].frame), world.decision_log()[i].player,
                    world.decision_log()[i].text.c_str());
    CHECK(world.running_plans() + active > 0);
    // AI execution path finder on the real galaxy: every AI player's finder exists and is on the 90-frame cadence;
    // its Get_Path answers vs the former Lua Find_Path stand-in (fleet A*, all request flags) for every planet pair.
    for (size_t p = 0; p < sim.players().size(); ++p) {
        const auto pi = static_cast<int32_t>(p);
        const AiPathFinder* f = world.path_finder(pi);
        if (!f) continue;
        CHECK(f->created() && f->recomputes() >= 3600u / 90u - 1u);
        uint32_t pairs = 0, reachable = 0, astar_reachable = 0, same = 0;
        gal::PlayerView view = sim.player_view(pi);
        if (view.is_human) view.revealed = gal::Bitset{};
        gal::PathScratch scratch;
        std::vector<PlanetIndex> ap;
        for (PlanetIndex a = 0; a < sim.planets().size(); ++a)
            for (PlanetIndex b = 0; b < sim.planets().size(); ++b) {
                if (a == b || !sim.planets()[a].is_planet || !sim.planets()[b].is_planet) continue;
                ++pairs;
                const std::vector<PlanetIndex> fw = world.path(pi, a, b);
                gal::PathRequest req;
                req.source = a;
                req.destination = b;
                const bool ok = gal::find_fleet_path(sim.graph(), view, req, scratch, ap);
                reachable += fw.empty() ? 0u : 1u;
                astar_reachable += ok ? 1u : 0u;
                same += (ok ? ap : std::vector<PlanetIndex>{}) == fw ? 1u : 0u;
                if (!fw.empty()) CHECK(fw.front() == a && fw.back() == b);
            }
        std::printf("FoC player %zu AI path finder: %u recomputes, %u pairs, %u reachable (A* stand-in %u), %u identical to A*\n",
                    p, f->recomputes(), pairs, reachable, astar_reachable, same);
        CHECK(reachable > 0);
    }
    // FIXPASS_GALACTIC §1 on the real galaxy (shipped GalacticFreeStore.lua drives the moves):
    //   * the Friendly_Only source list holds only planets held by the player or an ally (§1.4);
    //   * the free-store Reachable_Target calls return such a planet or nil;
    //   * Get_Next_Hop passes a pair exactly when every planet after the start has node cost 0 on the matrix.
    for (size_t p = 0; p < sim.players().size(); ++p) {
        const auto pi = static_cast<int32_t>(p);
        const AiPathFinder* f = world.path_finder(pi);
        if (!f) continue;
        const gal::PlayerView view = sim.player_view(pi);
        const std::vector<PlanetIndex> r0 = world.reach_planets(pi, 0);
        for (const PlanetIndex k : r0) CHECK(gal::PlayerView::bit(view.held_by_ally, k, false));
        CHECK(world.reach_planets(pi, 5).size() >= r0.size());
        for (const char* fn : {"Space_Priority_Defense_Score", "Low_Space_Defense_Score", "Ground_Priority_Defense_Score", "One"}) {
            const int32_t t = world.reachable_target(pi, fn, "Friendly", "Friendly_Only", 0.1);
            CHECK(t == -1 || std::find(r0.begin(), r0.end(), static_cast<PlanetIndex>(t)) != r0.end());
        }
        uint32_t pairs = 0, pass = 0, blocked = 0;
        std::vector<PlanetIndex> path;
        std::vector<uint32_t> w;
        for (PlanetIndex a = 0; a < sim.planets().size(); ++a)
            for (PlanetIndex b = 0; b < sim.planets().size(); ++b) {
                if (a == b || !sim.planets()[a].is_planet || !sim.planets()[b].is_planet) continue;
                ++pairs;
                const FreeStoreHopResult r = free_store_next_hop(*f, pi, a, b, false, {});
                f->get_path(a, b, path, &w);
                bool free_nodes = !path.empty();
                for (size_t k = 0; k + 1 < path.size(); ++k) free_nodes = free_nodes && (w[k] >> 16) == 0u;
                CHECK((r.kind == FreeStoreHop::Issue) == (free_nodes && path.size() >= 2));
                if (r.kind == FreeStoreHop::Issue) {
                    ++pass;
                    CHECK(r.hop == path[1]);
                } else {
                    ++blocked;
                }
            }
        size_t no_path = 0;
        for (const AiDecision& d : world.decision_log())
            if (d.player == pi && d.text.find("Unable to find a path") != std::string::npos) ++no_path;
        std::printf("FoC player %zu free store: Friendly_Only reach %zu planets, Get_Next_Hop %u of %u pairs pass (%u fail), "
                    "%zu moves ended without a path\n", p, r0.size(), pass, pairs, blocked, no_path);
        CHECK((!r0.empty() || sim.planets_owned(pi) == 0) && pass > 0);
    }
}

// N1 save / load on the shipped scripts (HotM, as test_foc_data).
static void test_save_foc() {
    const std::string root = env("ALAMO_FOC_DATA");
    std::error_code ec;
    if (root.empty() || !std::filesystem::is_directory(path_from_utf8(root) / "Data", ec)) {
        std::printf("N1 save FoC: ALAMO_FOC_DATA not set: skipped\n");
        return;
    }
    vfs::VfsConfig vcfg;
    vcfg.foc_root = root;
    std::string err;
    auto v = vfs::Vfs::create(vcfg, &err);
    if (!v) {
        std::printf("N1 save FoC: VFS failed (%s): skipped\n", err.c_str());
        return;
    }
    AiDefinitions defs;
    CHECK(load_ai_definitions(*v, list_ai_data([&v](std::string_view g) { return v->list(g); }), defs));
    data::GameDatabase db;
    objects::ObjectTypeDatabase raw;
    objects::GameObjectTypes types;
    gal::GalacticRules rules;
    std::vector<std::string> diag;
    if (!db.load(*v, {}, &diag) || !raw.load(*v, &diag) || !types.build(raw, v.get(), &diag) ||
        !gal::load_galactic_rules(*v, db, types, rules, &diag)) {
        std::printf("N1 save FoC: game database incomplete: skipped\n");
        return;
    }
    EquationSet eqs;
    load_equation_files(*v, v->list(kPerceptionEquationGlob), eqs);
    const gal::GalacticSetup setup = gal::build_campaign_setup(db, rules, "Sandbox_Heart_Of_The_Maw_Underworld", {}, &diag);
    SaveRigInputs in;
    in.rules = &rules;
    in.setup = &setup;
    in.types = &types;
    in.files = v.get();
    in.defs = &defs;
    in.eqs = &eqs;
    in.tinfo = make_galactic_ai_type_info(types);
    for (size_t p = 0; p < setup.players.size(); ++p)
        if (setup.players[p].ai) {
            const std::string fac = rules.factions[setup.players[p].faction].name;
            in.cfg.players.push_back({static_cast<int32_t>(p),
                                      ai_iequal(fac, "Empire") ? "BasicEmpire" : (ai_iequal(fac, "Rebel") ? "BasicRebel" : "AI_Player_Underworld"), 1});
        }
    in.cfg.difficulties = load_ai_difficulties(*v);
    in.plan_files = list_plan_files([&v](std::string_view g) { return v->list(g); });
    in.seed_synced = 99u;
    in.seed_free = 1u;
    // Size guards (N1 save size), about twice the 2026-10-29 sizes: 1.72 MB, 141 KB packed. Before pristine pooled VMs
    // were saved by reference the AI chunk held every pooled VM's image (15.3 MB in the headless HotM save at 1 min).
    check_save_load(in, "FoC HotM", 30 * 120, 30 * 60, 12, true, 4000000, 300000);
}
#endif

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // keep the progress lines when a test crashes
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--write-golden") == 0) g_write_golden = true;
    test_cadence();
    test_desire_and_hysteresis();
    test_plan_pick_and_solver();
    test_definitions_and_templates();
    test_synthetic_campaign();
    test_path_finder_world();
    test_sim_hooks();
    test_starbase_force_split();
    test_parity_rules();
    test_production_disabled_and_delivery();
    test_free_store_path_rules();
    test_free_store_world();
    test_underworld_special();
    test_save_synthetic();
#ifdef ALAMO_AI_HAVE_VFS
    test_foc_data();
    test_save_foc();
#endif
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
