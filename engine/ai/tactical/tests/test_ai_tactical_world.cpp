// alamo_ai_tactical_world_tests: A1 stage 2 - the tactical AI mode on the A2 framework (tactical_ai.hpp) against
// a scripted mock battle host (no game data):
//   * synthetic AI data (space goals, a template, two AI player types, three SpaceMode plans, a free-store
//     script) and a 2-player space skirmish whose units move / fight in the mock host;
//   * checks: opponent pick, target slots (units + CELL grid), free-store admission + service, FindTarget /
//     FindDeadlyEnemy / Get_Most_Defended_Position / EvaluatePerception(AITarget) from Lua, Produce_Force on the
//     tactical PotentialPlan (A2 tactical: test_potential_plan + the skirmish), Attack_Target "Random"
//     and Guard_Target(task force) draws, Build_All refusal, Withdraw_Units, legal orders only (0 rejected), the
//     Easy 15 s goal sleep, two identical runs and the golden (decision log + per-150-frame CRC; written by MSVC
//     with --write-golden, clang / gcc must match).
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <tuple>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include "alamo/ai/ai_context.hpp"
#include "alamo/ai/ai_lua_runtime.hpp"
#include "alamo/ai/perception_equations.hpp"
#include "alamo/ai/tactical/tactical_ai.hpp"
#include "alamo/ai/tactical/tactical_perception.hpp"
#include "alamo/ai/tactical/tactical_pp.hpp"
#include "alamo/core/bytes.hpp"
#include "alamo/core/file_provider.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/core/sim_rng.hpp"
#include "alamo/galactic/auto_resolve.hpp"
#include "alamo/lua/object_bridge.hpp"
#include "alamo/objects/behaviors.hpp"
#include "alamo/objects/game_object_types.hpp"

using namespace alamo;
using namespace alamo::ai;
using namespace alamo::ai::tactical;

static int g_fail = 0;
static bool g_write_golden = false;
#define CHECK(c)                                                     \
    do {                                                             \
        if (!(c)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++g_fail;                                                \
        }                                                            \
    } while (0)

namespace {

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

const char* kGoals = R"(<Goals>
  <Syn_Destroy_Unit><AIGoalApplicationFlags>Enemy_Unit | Enemy_Structure</AIGoalApplicationFlags><GameMode>Space</GameMode>
    <Category>Tactical_Targeted</Category><Reachability>Any</Reachability><Time_Limit>120</Time_Limit><Tracking_Duration>60</Tracking_Duration><Per_Failure_Desire_Adjust>-0.5</Per_Failure_Desire_Adjust></Syn_Destroy_Unit>
  <Syn_Sweep_Area><AIGoalApplicationFlags>Tactical_Location</AIGoalApplicationFlags><GameMode>Space</GameMode>
    <Category>Tactical_Targeted</Category><Reachability>Any</Reachability><Tracking_Duration>60</Tracking_Duration><Per_Failure_Desire_Adjust>-0.5</Per_Failure_Desire_Adjust></Syn_Sweep_Area>
  <Syn_Escort><AIGoalApplicationFlags>Global</AIGoalApplicationFlags><GameMode>Space</GameMode><Category>Defensive</Category></Syn_Escort>
  <Syn_Build><AIGoalApplicationFlags>Friendly_Build_Pad</AIGoalApplicationFlags><GameMode>Space</GameMode><Category>Tactical_Untargeted</Category><Tracking_Duration>300</Tracking_Duration><Per_Failure_Desire_Adjust>-2.0</Per_Failure_Desire_Adjust></Syn_Build>
  <Syn_Retreat><AIGoalApplicationFlags>Global</AIGoalApplicationFlags><GameMode>Space</GameMode><Category>Defensive</Category></Syn_Retreat>
  <Syn_Land_Only><AIGoalApplicationFlags>Enemy_Unit</AIGoalApplicationFlags><GameMode>Land</GameMode><Category>Tactical_Targeted</Category></Syn_Land_Only>
</Goals>)";
const char* kSet = R"(<FunctionSet>
  <A><Goal>Syn_Destroy_Unit</Goal><Function>Syn_Destroy_Desire</Function></A>
  <B><Goal>Syn_Sweep_Area</Goal><Function>Syn_Sweep_Desire</Function></B>
  <C><Goal>Syn_Escort</Goal><Function>One</Function></C>
  <D><Goal>Syn_Build</Goal><Function>One</Function></D>
  <E><Goal>Syn_Retreat</Goal><Function>Syn_Want_Retreat</Function></E>
  <F><Goal>Syn_Land_Only</Goal><Function>One</Function></F>
</FunctionSet>)";
const char* kTemplates = R"(<AITemplates>
  <Syn_Space><Priority>1</Priority><Trigger>One</Trigger>
    <Turn_On><Goals><Category>Tactical_Targeted</Category><Category>Defensive</Category><Category>Tactical_Untargeted</Category></Goals>
      <Plans><Goal_Category>Tactical_Targeted</Goal_Category><Goal_Category>Defensive</Goal_Category>
             <Goal_Category>Tactical_Untargeted</Goal_Category></Plans></Turn_On>
  </Syn_Space>
</AITemplates>)";
const char* kPlayer = R"(<AIPlayerType><Name>%s</Name>
  <SpaceFreeStoreScript>SynTacticalFreeStore</SpaceFreeStoreScript>
  <GoalProposalFunctionSets>SynSpaceSet</GoalProposalFunctionSets>
  <Templates><Space>Syn_Space</Space></Templates>
  <Difficulty_Adjustments><Easy>Easy_Default</Easy><Normal>Normal_Default</Normal><Hard>Hard_Default</Hard></Difficulty_Adjustments>
</AIPlayerType>)";

const char* kSynBase = R"(
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
const char* kSynAICommands = R"(
require("SynBase")
function Base_Definitions()
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
function QuickReinforce(tf, target)
  local st = tf.Reinforce(target)
  if st then BlockOnCommand(st, 5) end
end
)";
// Destroy a unit: free-store units + pool reinforcements, the goal equation re-checked with the AI target, then
// Attack_Target(Target, "Random") (space: one Get_Int(0, 13) draw).
const char* kPlanDestroy = R"(
require("SynAICommands")
ScriptPoolCount = 2
function Definitions()
  Category = "Syn_Destroy_Unit"
  TaskForce = { { "MainForce", "Frigate | Corvette = 1,3" } }
end
function MainForce_Thread()
  MainForce.Set_As_Goal_System_Removable(false)
  BlockOnCommand(MainForce.Produce_Force())
  local home = FindTarget(MainForce, "One", "Friendly_Structure", 1.0)
  if home then QuickReinforce(MainForce, home) end
  if MainForce.Get_Force_Count() == 0 then
    ScriptExit()
  end
  local d = EvaluatePerception("Syn_Destroy_Desire", PlayerObject, AITarget)
  if d and d > 0 and TestValid(Target) then
    BlockOnCommand(MainForce.Attack_Target(Target, "Random"), 40)
  end
  MainForce.Set_Plan_Result(true)
  ScriptExit()
end
function MainForce_No_Units_Remaining()
  ScriptExit()
end
)";
// Sweep a CELL: attack-move there with the force's threat, then FindDeadlyEnemy and attack it.
const char* kPlanSweep = R"(
require("SynAICommands")
ScriptPoolCount = 2
function Definitions()
  Category = "Syn_Sweep_Area"
  TaskForce = { { "SweepForce", "Corvette = 1,2" } }
end
function SweepForce_Thread()
  BlockOnCommand(SweepForce.Produce_Force())
  local home = FindTarget(SweepForce, "One", "Friendly_Structure", 1.0)
  if home then QuickReinforce(SweepForce, home) end
  if SweepForce.Get_Force_Count() == 0 then ScriptExit() end
  local threat = SweepForce.Get_Self_Threat_Max()
  BlockOnCommand(SweepForce.Attack_Move(AITarget, threat), 30)
  local deadly = FindDeadlyEnemy(SweepForce)
  if TestValid(deadly) then
    BlockOnCommand(SweepForce.Attack_Target(deadly), 20)
  end
  local unit = FindTarget(SweepForce, "Syn_Destroy_Desire", "Enemy_Unit", 0.5)
  if TestValid(unit) then
    BlockOnCommand(SweepForce.Attack_Target(unit), 20)
  end
  SweepForce.Set_Plan_Result(true)
  ScriptExit()
end
)";
// Escort: Lead moves to the most defended position; Escort guards the Lead task force (one Get_Int(0, n-1)).
const char* kPlanEscort = R"(
require("SynAICommands")
ScriptPoolCount = 1
function Definitions()
  Category = "Syn_Escort"
  TaskForce = { { "Lead", "Frigate = 1,1" }, { "Escort", "Corvette = 1,2" } }
end
function Lead_Thread()
  Lead.Set_As_Goal_System_Removable(false)
  BlockOnCommand(Lead.Produce_Force())
  local pos = Get_Most_Defended_Position(Lead, PlayerObject)
  if pos then BlockOnCommand(Lead.Move_To(pos), 20) end
  Sleep(20)
  Lead.Set_Plan_Result(true)
  ScriptExit()
end
function Escort_Thread()
  BlockOnCommand(Escort.Produce_Force())
  Sleep(1)
  if Escort.Get_Force_Count() > 0 and Lead.Get_Force_Count() > 0 then
    Escort.Guard_Target(Lead)
  end
  Sleep(30)
end
)";
// Build: a UC_ team reserves a pad; Build_All is refused by the host (pad occupied / enemies too close).
const char* kPlanBuild = R"(
require("SynAICommands")
ScriptPoolCount = 1
function Definitions()
  Category = "Syn_Build"
  AllowFreeStoreUnits = false
  TaskForce = { { "Builder", "UC_Syn_Turret = 1" } }
end
function Builder_Thread()
  BlockOnCommand(Builder.Produce_Force())
  local st = Builder.Build_All()
  if st then BlockOnCommand(st, 5) end
  Builder.Set_Plan_Result(false)
  ScriptExit()
end
)";
// AI_GALACTIC_3 §1.11 (variant 2): a skirmish build plan of a non-UC type - Produce_Force issues Production {type,
// builder} to the host's builders in order until one starts; the built unit joins the task force.
const char* kPlanProduce = R"(
require("SynAICommands")
ScriptPoolCount = 1
function Definitions()
  Category = "Syn_Build"
  AllowFreeStoreUnits = false
  TaskForce = { { "Builder", "Syn_Corvette = 1" } }
end
function Builder_Thread()
  BlockOnCommand(Builder.Produce_Force())
  Builder.Set_Plan_Result(Builder.Get_Force_Count() > 0)
  ScriptExit()
end
)";
// Retreat: Withdraw_Units, then Purge_Goals and Block_Goal_Proposal (RetreatPlan shape, Â§9).
const char* kPlanRetreat = R"(
require("SynAICommands")
ScriptPoolCount = 1
function Definitions()
  Category = "Syn_Retreat"
  TaskForce = { { "RetreatForce", "TaskForceRequired", "Frigate | Corvette = 0,10" } }
end
function RetreatForce_Thread()
  RetreatForce.Set_As_Goal_System_Removable(false)
  if not RetreatForce.Withdraw_Units() then ScriptExit() end
  Purge_Goals(PlayerObject)
  RetreatForce.Block_Goal_Proposal()
  RetreatForce.Collect_All_Free_Units()
  while true do Sleep(10) end
end
)";
// STRATEGIC_GAPS §5.3: a TF subscribed to laser opportunity fire gets Hardpoint_Opportunity_Target_Acquired(tf, unit,
// target); torpedo acquisitions are not subscribed.
// refixf t06 (variant 4): a task force of two or more units attack-moves onto the target object (ONE group order with
// every movable unit, tactical_ai.hpp HOST CONTRACT), then attacks it at a hard-point type named in the short form.
const char* kPlanGroupAttack = R"(
require("SynAICommands")
ScriptPoolCount = 1
function Definitions()
  Category = "Syn_Destroy_Unit"
  TaskForce = { { "MainForce", "Frigate | Corvette = 2,3" } }
end
function MainForce_Thread()
  BlockOnCommand(MainForce.Produce_Force())
  local home = FindTarget(MainForce, "One", "Friendly_Structure", 1.0)
  if home then QuickReinforce(MainForce, home) end
  if MainForce.Get_Force_Count() < 2 or not TestValid(Target) then
    ScriptExit()
  end
  BlockOnCommand(MainForce.Attack_Move(Target), 5)
  if TestValid(Target) then
    BlockOnCommand(MainForce.Attack_Target(Target, "Shield_Generator"), 5)
  end
  MainForce.Set_Plan_Result(true)
  ScriptExit()
end
function MainForce_No_Units_Remaining()
  ScriptExit()
end
)";
const char* kPlanOpportunity = R"(
require("SynAICommands")
ScriptPoolCount = 1
function Definitions()
  Category = "Syn_Sweep_Area"
  TaskForce = { { "SweepForce", "Corvette = 1,2" } }
end
function SweepForce_Thread()
  SweepForce.Add_Opportunity_Fire_Event_Subscription("HARD_POINT_WEAPON_LASER")
  BlockOnCommand(SweepForce.Produce_Force())
  while (OppHits or 0) < 2 do Sleep(1) end
  SweepForce.Set_Plan_Result(true)
  ScriptExit()
end
function SweepForce_Hardpoint_Opportunity_Target_Acquired(tf, unit, target)
  if TestValid(unit) and TestValid(target) then OppHits = (OppHits or 0) + 1 end
end
)";
// AI_GALACTIC_3 §4.3 tactical HIJACK_UNIT: a MagicPlan's Produce_Force spawns through the host into the reinforcement
// pool; the TF then drops it with Reinforce.
const char* kPlanMagic = R"(
require("SynAICommands")
ScriptPoolCount = 1
function Definitions()
  Category = "Syn_Destroy_Unit"
  MagicPlan = true
  MagicPlanStealing = false
  TaskForce = { { "MagicForce", "Corvette = 1" } }
end
function MagicForce_Thread()
  BlockOnCommand(MagicForce.Produce_Force())
  local home = FindTarget(MagicForce, "One", "Friendly_Structure", 1.0)
  if home then QuickReinforce(MagicForce, home) end
  MagicForce.Set_Plan_Result(true)
  ScriptExit()
end
)";
const char* kFreeStore = R"(
require("SynBase")
function Base_Definitions()
  ServiceRate = 5
  UnitServiceRate = 2
  Added = 0
  Serviced = 0
end
function main()
  while true do
    FreeStoreService()
    PumpEvents()
  end
end
function FreeStoreService()
  enemy_location = FindTarget.Reachable_Target(PlayerObject, "Current_Enemy_Location", "Tactical_Location", "Any_Threat", 0.5)
  enemy_unit = FindTarget.Reachable_Target(PlayerObject, "Syn_Destroy_Desire", "Enemy_Unit", "Any", 0.5)
end
function On_Unit_Added(object)
  Added = Added + 1
end
function On_Unit_Service(object)
  if not TestValid(object) then return end
  Serviced = Serviced + 1
  if object.Has_Active_Orders() then return end
  if enemy_unit then
    local o = enemy_unit.Get_Game_Object()
    if TestValid(o) then object.Attack_Move(o) end
  end
end
)";

// ---------------------------------------------------------------- mock battle host

enum SynType : int32_t { tFrigate = 0, tCorvette = 1, tStation = 2, tPad = 3, tTurret = 4, tTypes };

struct MockUnit {
    int32_t id = -1;
    objects::ObjectHandle h;
    int32_t owner = -1;
    int32_t type = 0;
    Vec3f pos, goal;
    bool has_goal = false;
    int32_t ordered = -1;  // ordered attack target
    int32_t target = -1;   // current target
    float hp = 0.0f, max_hp = 1.0f;
};

class MockHost final : public TacticalAiHost {
public:
    MockHost(objects::ObjectStore& s) : store_(s) {
        const char* names[] = {"Syn_Frigate", "Syn_Corvette", "Syn_Station", "Syn_Pad", "UC_Syn_Turret"};
        const float power[] = {1000.0f, 400.0f, 2000.0f, 0.0f, 0.0f};
        const char* cats[] = {"Frigate", "Corvette", "Structure", "Structure", "Structure"};
        types_.resize(tTypes);
        for (int i = 0; i < tTypes; ++i) {
            types_[static_cast<size_t>(i)].name = names[i];
            types_[static_cast<size_t>(i)].combat_power = power[i];
            types_[static_cast<size_t>(i)].category = category(cats[i]);
            types_[static_cast<size_t>(i)].under_construction = i == tTurret;  // A2 tactical: a pad structure
            types_[static_cast<size_t>(i)].tactical_cost = i == tTurret ? 150.0f : 0.0f;
        }
        contrast_ = make_contrast();
    }
    static uint64_t category(const char* n) {
        // GameObjectCategoryType bits as the default perception table resolves them.
        TacticalPerception p;
        return p.config().category_mask(n);
    }
    static galactic::ContrastTable make_contrast() {
        galactic::ContrastTable t;
        galactic::ContrastRow r;
        r.enemy = "Corvette";
        r.enemy_mask = category("Corvette");
        r.friendly.push_back({"Frigate", category("Frigate"), 1.5f});
        t.rows.push_back(r);
        return t;
    }
    int32_t spawn(int32_t owner, int32_t type, float x, float y) {
        objects::SpawnParams sp;
        sp.type = static_cast<objects::TypeIndex>(type);
        sp.owner = owner;
        sp.position = {x, y, 0.0f};
        const objects::ObjectHandle h = store_.create(sp);
        if (type <= tCorvette) store_.add_behavior(h, objects::beh::Locomotor);
        MockUnit u;
        u.id = store_.id(h);
        u.h = h;
        u.owner = owner;
        u.type = type;
        u.pos = {x, y, 0.0f};
        u.max_hp = u.hp = type == tFrigate ? 900.0f : type == tCorvette ? 400.0f : type == tStation ? 4000.0f : 1.0e9f;
        units_.push_back(u);
        if (type != tPad) dt_.enable(u.id, u.max_hp, 0.0f);
        return u.id;
    }
    void add_pool(int32_t player, int32_t key, int32_t type, float pop) { pool_[player].push_back({key, type, pop, 1}); }
    void enable_production(int32_t builder) {  // variant 2: player 0 buys corvettes at `builder`
        produce_ = true;
        builder_ = builder;
        types_[tCorvette].tactical_cost = 100.0f;
    }
    bool can_build_now(int32_t player, int32_t type) override { return produce_ && player == 0 && type == tCorvette; }
    std::vector<int32_t> tactical_builders(int32_t player, int32_t type) override {
        if (!can_build_now(player, type)) return {};
        return {pad_, builder_};  // the pad does not build corvettes: the host refuses it, the station accepts
    }
    MockUnit* unit(int32_t id) {
        for (MockUnit& u : units_)
            if (u.id == id) return &u;
        return nullptr;
    }
    static float range_of(int32_t t) { return t == tFrigate ? 1000.0f : t == tCorvette ? 600.0f : t == tStation ? 1200.0f : 0.0f; }
    static float speed_of(int32_t t) { return t == tFrigate ? 15.0f : t == tCorvette ? 25.0f : 0.0f; }

    // One battle frame: movement, targeting (ordered, else nearest enemy in range, ties lowest id), damage, deaths.
    void tick(uint32_t frame) {
        build_frame_ = frame;
        for (size_t i = 0; i < builds_.size();) {
            if (builds_[i] > frame) {
                ++i;
                continue;
            }
            builds_.erase(builds_.begin() + static_cast<std::ptrdiff_t>(i));
            if (const MockUnit* b = unit(builder_)) spawn(0, tCorvette, b->pos.x + 100.0f, b->pos.y);
            ++built;
        }
        for (MockUnit& u : units_) {
            if (!u.has_goal) continue;
            const float dx = u.goal.x - u.pos.x, dy = u.goal.y - u.pos.y;
            const float d = std::sqrt(dx * dx + dy * dy), s = speed_of(u.type);
            if (d <= s) {
                u.pos = u.goal;
                u.has_goal = false;
            } else {
                u.pos.x += dx / d * s;
                u.pos.y += dy / d * s;
            }
            store_.set_position(u.h, {u.pos.x, u.pos.y, 0.0f});
        }
        for (MockUnit& u : units_) {
            u.target = -1;
            if (u.type == tPad || range_of(u.type) <= 0.0f) continue;
            const MockUnit* o = u.ordered >= 0 ? unit(u.ordered) : nullptr;
            if (o && dist(u, *o) <= range_of(u.type)) {
                u.target = o->id;
                continue;
            }
            if (u.ordered >= 0 && !o) u.ordered = -1;
            float best = 0.0f;
            for (const MockUnit& e : units_) {
                if (e.owner == u.owner || e.owner < 0 || e.type == tPad) continue;
                const float d = dist(u, e);
                if (d <= range_of(u.type) && (u.target < 0 || d < best)) {
                    u.target = e.id;
                    best = d;
                }
            }
        }
        for (const MockUnit& u : units_)
            if (u.target >= 0)
                if (MockUnit* t = unit(u.target)) {
                    const float dmg = types_[static_cast<size_t>(u.type)].combat_power / 200.0f;
                    t->hp -= dmg;
                    dt_.record(t->id, u.id, dmg);
                }
        for (size_t i = 0; i < units_.size();) {
            if (units_[i].hp > 0.0f) {
                ++i;
                continue;
            }
            ++deaths_[units_[i].owner];
            dt_.disable(units_[i].id);
            store_.destroy(units_[i].h);
            units_.erase(units_.begin() + static_cast<std::ptrdiff_t>(i));
        }
        if (frame % 30 == 0) dt_.service_all(0.05f);
        store_.flush();
    }
    static float dist(const MockUnit& a, const MockUnit& b) {
        const float dx = a.pos.x - b.pos.x, dy = a.pos.y - b.pos.y;
        return std::sqrt(dx * dx + dy * dy);
    }

    // ---- TacticalAiHost ----
    void fill(PerceptionWorld& w, std::vector<TacticalAiObject>& f) override {
        if (w.types.empty()) {
            w.mode = TacticalMode::Space;
            w.map = {-4000.0f, -4000.0f, 4000.0f, 4000.0f};
            w.types = types_;
            w.players.resize(2);
            w.players[0].id = 0;
            w.players[0].faction = "Empire";
            w.players[1].id = 1;
            w.players[1].faction = "Rebel";
            w.players[1].attacker = true;
            w.players[0].credits = w.players[1].credits = 1000.0f;  // A2 tactical: TacticalBuild affordability
            w.relation = {0, 1, 1, 0};
        }
        w.objects.clear();
        f.clear();
        for (const MockUnit& u : units_) {
            PerceptionObject o;
            o.id = u.id;
            o.owner = u.owner;
            o.type = u.type;
            o.has_unit_ai = u.type != tPad;
            o.mobile = u.type <= tCorvette;
            o.speed = u.has_goal ? speed_of(u.type) * 30.0f : 0.0f;
            o.position = u.pos;
            o.transform[3] = u.pos.x;
            o.transform[7] = u.pos.y;
            o.radius = 40.0f;
            o.health = u.hp / u.max_hp;
            o.combat_power = types_[static_cast<size_t>(u.type)].combat_power;
            o.max_attack_range = range_of(u.type);
            o.has_damage_tracking = u.type != tPad;
            o.is_build_pad = u.type == tPad;
            w.objects.push_back(o);
            TacticalAiObject a;
            a.handle = u.h;
            a.target.id = u.id;
            a.target.rep_has_space_evaluator = a.target.own_has_space_evaluator = true;
            a.applies.has_locomotor = o.mobile;
            a.applies.has_build_objects = u.type == tPad;
            a.free_store.has_locomotor = o.mobile;
            a.free_store.is_starship_combatant = o.mobile;
            a.moving = u.has_goal;
            a.attack_target = u.target;
            a.damage_tracking = u.type != tPad;
            f.push_back(a);
        }
        w.players[0].pop_used = static_cast<int32_t>(population_used(0));
        w.players[1].pop_used = static_cast<int32_t>(population_used(1));
    }
    commands::RejectReason execute(uint8_t player, const commands::EventData& ev) override {
        using R = commands::RejectReason;
        using T = commands::EventType;
        auto own = [&](uint32_t id) -> MockUnit* {
            MockUnit* u = unit(static_cast<int32_t>(id));
            return (u && u->owner == player) ? u : nullptr;
        };
        std::string line = "p" + std::to_string(player) + " " + std::string(ev.def().name);
        R r = R::None;
        switch (ev.type()) {
            case T::MoveToPositionFacing: {
                uint32_t n = 0;
                for (const uint32_t id : ev.ids("units"))
                    if (MockUnit* u = own(id); u && speed_of(u->type) > 0.0f) {
                        u->goal = {ev.f("x"), ev.f("y"), 0.0f};
                        u->has_goal = true;
                        u->ordered = -1;
                        ++n;
                    }
                r = n ? R::None : R::InvalidUnit;
                break;
            }
            case T::Attack: {
                MockUnit* t = unit(static_cast<int32_t>(ev.u("target")));
                if (!t || t->owner == player) {
                    r = R::InvalidTarget;
                    break;
                }
                uint32_t n = 0;
                for (const uint32_t id : ev.ids("attackers"))
                    if (MockUnit* u = own(id)) {
                        u->ordered = t->id;
                        u->goal = t->pos;
                        u->has_goal = speed_of(u->type) > 0.0f;
                        ++n;
                    }
                line += " ref2 " + std::to_string(ev.u("ref2"));
                r = n ? R::None : R::InvalidUnit;
                break;
            }
            case T::PortExtension: {
                const auto b = ev.bytes_view("data");
                if (b.size() < 24) {
                    r = R::Malformed;
                    break;
                }
                auto u32 = [&b](size_t o) {
                    return static_cast<uint32_t>(b[o]) | (static_cast<uint32_t>(b[o + 1]) << 8) | (static_cast<uint32_t>(b[o + 2]) << 16) |
                           (static_cast<uint32_t>(b[o + 3]) << 24);
                };
                // refixf t06: a task-force group order lists its further units after the 24-byte block (tactical_ai.hpp).
                std::vector<MockUnit*> movers;
                for (size_t o = 0; o + 4 <= b.size(); o = o == 0 ? 24 : o + 4)
                    if (MockUnit* u = own(u32(o)); u && speed_of(u->type) > 0.0f) movers.push_back(u);
                if (movers.empty()) {
                    r = R::InvalidUnit;
                    break;
                }
                const uint32_t tid = u32(4);
                const MockUnit* t = tid != 0x3FFFFFu ? unit(static_cast<int32_t>(tid)) : nullptr;
                for (MockUnit* u : movers) {
                    u->goal = t ? t->pos : Vec3f{std::bit_cast<float>(u32(8)), std::bit_cast<float>(u32(12)), 0.0f};
                    u->has_goal = true;
                }
                line += " sub " + std::to_string(ev.u("subtype") - 0x4C00u) + " units " + std::to_string(movers.size());
                break;
            }
            case T::Reinforce: {
                auto& pool = pool_[player];
                const auto it = std::find_if(pool.begin(), pool.end(), [&](const TacticalReinforcement& e) {
                    return static_cast<uint32_t>(e.key) == ev.u("unit");
                });
                if (it == pool.end()) {
                    r = R::InvalidUnit;
                    break;
                }
                line += " key " + std::to_string(it->key);
                if (ring_test) line += " at " + std::to_string(static_cast<int>(ev.f("x"))) + "," + std::to_string(static_cast<int>(ev.f("y")));
                spawn(player, it->type, ev.f("x"), ev.f("y"));
                pool.erase(it);
                break;
            }
            case T::Retreat: retreat_[player] = true; break;
            case T::Production: {
                const int32_t at = static_cast<int32_t>(ev.u("location"));
                line += " type " + std::to_string(ev.u("type_index")) + " at " + std::to_string(at);
                if (!produce_ || at != builder_ || ev.u("type_index") != static_cast<uint32_t>(tCorvette) || !own(static_cast<uint32_t>(at))) {
                    r = R::NotAllowed;
                    break;
                }
                builds_.push_back(build_frame_ + 30);  // built 1 s later next to the station
                break;
            }
            default: r = R::Unhandled; break;
        }
        executed.push_back(line + (r == R::None ? "" : " REJECTED"));
        return r;
    }
    std::vector<TacticalReinforcement> reinforcement_pool(int32_t player) override { return pool_[player]; }
    bool can_reinforce(int32_t) override { return true; }
    float population_cap(int32_t) override { return 6.0f; }
    float population_used(int32_t player) override {
        float n = 0.0f;
        for (const MockUnit& u : units_)
            if (u.owner == player) n += u.type == tFrigate ? 2.0f : u.type == tCorvette ? 1.0f : 0.0f;
        return n;
    }
    const DamageTrackingStore* damage_tracking() override { return &dt_; }
    const galactic::ContrastTable* contrast() override { return &contrast_; }
    PadCheck build_pad_check(int32_t, int32_t, int32_t) override { return PadCheck::Blocked; }
    std::vector<TacticalPad> build_pads(int32_t player) override {
        if (player != 0) return {};  // the Empire's pad
        TacticalPad p;
        p.id = pad_;
        if (const MockUnit* u = unit(pad_)) p.position = u->pos;
        p.accepts = {tTurret};
        return {p};
    }
    // STRATEGIC_GAPS §5.3 variant: every 30 frames each Empire corvette reports a laser (5) and a torpedo (7)
    // acquisition of the nearest Rebel unit.
    std::vector<OpportunityTargetEvent> opportunity_targets() override {
        std::vector<OpportunityTargetEvent> out;
        if (!emit_opportunity || ++opp_frame % 30 != 0) return out;
        for (const MockUnit& u : units_) {
            if (u.owner != 0 || u.type != tCorvette) continue;
            const MockUnit* best = nullptr;
            for (const MockUnit& e : units_)
                if (e.owner == 1 && e.type != tPad && (!best || dist(u, e) < dist(u, *best))) best = &e;
            if (!best) continue;
            out.push_back({u.id, best->id, 5});
            out.push_back({u.id, best->id, 7});
        }
        return out;
    }
    // AI_GALACTIC_3 §4.3 tactical HIJACK: the "parent galactic mode" spawns into the pool (keys from 5001).
    TacticalReinforcement hijack_unit(int32_t player, int32_t type, bool steal) override {
        hijacks.push_back("p" + std::to_string(player) + " type " + std::to_string(type) + (steal ? " steal" : ""));
        if (!allow_hijack) return {};
        const int32_t key = 5001 + static_cast<int32_t>(hijacks.size());
        add_pool(player, key, type, 1.0f);
        return pool_[player].back();
    }
    // A1_READINGS §28.8: Active_By_Default re-apply requests (script lock's order done / task-force release), in call order.
    void unit_released(int32_t player, int32_t object_id, UnitRelease path) override {
        released.emplace_back(player, object_id, static_cast<int32_t>(path));
    }
    // refixf t06 (AI_TACTICAL §7.4): with ring_test the space drop test refuses every point with |x| > 3200 (the strips
    // behind both stations), so a reinforcement at a station needs the ring search; entry facing 0 -> first direction -x.
    bool space_drop_frame(int32_t, SpaceDropFrame& f) override {
        if (!ring_test) return false;
        f = SpaceDropFrame{};
        f.has_box = true;
        f.min_x = f.min_y = -4000.0f;
        f.max_x = f.max_y = 4000.0f;
        return true;
    }
    bool space_drop_ok(int32_t, int32_t, const Vec3f& p, float, float sweep) override {
        ++drop_tests;
        if (ring_test && sweep != 200.0f) return false;  // the GameConstants default reaches the host
        return !ring_test || (p.x >= -3200.0f && p.x <= 3200.0f);
    }
    // N1 save / load test support: the host's own battle state as a save restores it (units keep their handles: the
    // ObjectStore comes back with identical handle values); the damage-tracking store goes through its save format.
    bool copy_state_from(const MockHost& o) {
        ByteWriter w;
        o.dt_.write(w);
        ByteReader r(w.bytes());
        if (!dt_.read(r) || !r.done()) return false;
        units_ = o.units_;
        executed = o.executed;
        deaths_ = o.deaths_;
        retreat_[0] = o.retreat_[0];
        retreat_[1] = o.retreat_[1];
        pad_ = o.pad_;
        emit_opportunity = o.emit_opportunity;
        allow_hijack = o.allow_hijack;
        ring_test = o.ring_test;
        drop_tests = o.drop_tests;
        built = o.built;
        opp_frame = o.opp_frame;
        hijacks = o.hijacks;
        released = o.released;
        types_ = o.types_;
        pool_ = o.pool_;
        produce_ = o.produce_;
        builder_ = o.builder_;
        build_frame_ = o.build_frame_;
        builds_ = o.builds_;
        return true;
    }
    const DamageTrackingStore& damage_store() const { return dt_; }
    bool emit_opportunity = false, allow_hijack = false;
    bool ring_test = false;   // refixf t06
    uint32_t drop_tests = 0;
    uint32_t built = 0;
    uint32_t opp_frame = 0;
    std::vector<std::string> hijacks;
    std::vector<std::tuple<int32_t, int32_t, int32_t>> released;  // (player, object id, UnitRelease)

    void hash(StateHasher& h) const {
        h.u32(static_cast<uint32_t>(units_.size()));
        for (const MockUnit& u : units_) {
            h.i32(u.id);
            h.f32(u.pos.x);
            h.f32(u.pos.y);
            h.f32(u.hp);
            h.i32(u.target);
        }
    }

    std::vector<std::string> executed;
    std::map<int32_t, int32_t> deaths_;
    bool retreat_[2] = {false, false};
    int32_t pad_ = -1;
    std::vector<MockUnit> units_;

private:
    objects::ObjectStore& store_;
    std::vector<PerceptionType> types_;
    std::map<int32_t, std::vector<TacticalReinforcement>> pool_;
    DamageTrackingStore dt_;
    bool produce_ = false;
    int32_t builder_ = -1;
    uint32_t build_frame_ = 0;
    std::vector<uint32_t> builds_;
    galactic::ContrastTable contrast_;
};

// Script_ evaluators (variant 3, AI_GALACTIC_2 §4.4): SynProbe returns 1 only when its arguments are the reference's
// Parameter_Script_String values then the Parameter_Script_Number, PlayerObject and Target (an AI target whose game
// object is at distance 0) are set, and Evaluator_Clean_Up ran after every earlier call (globals persist); SynNil
// returns nil (fails); SynMissing has no file.
const char* kEvalProbe = R"(
Calls = 0
Cleaned = 0
function Evaluate(a, b, n)
  Calls = Calls + 1
  if a ~= "INBASE" or b ~= "FRIENDLY" or n ~= 3 then return -100 end
  if PlayerObject == nil or Target == nil then return -200 end
  local o = Target.Get_Game_Object()
  if o == nil then return -300 end
  if Target.Get_Distance(o) > 1 then return -400 end
  if Cleaned + 1 ~= Calls then return -500 end
  return 1
end
function Evaluator_Clean_Up()
  Cleaned = Cleaned + 1
  Target = nil
  PlayerObject = nil
end
)";
const char* kEvalNil = R"(
function Evaluate() return nil end
function Evaluator_Clean_Up() Target = nil PlayerObject = nil end
)";

void syn_data(MemoryFiles& files, AiDefinitions& defs, EquationSet& eqs, bool scripts = false) {
    files.add("Data\\Scripts\\Library\\SynBase.lua", kSynBase);
    files.add("Data\\Scripts\\Library\\SynAICommands.lua", kSynAICommands);
    files.add("Data\\Scripts\\AI\\SpaceMode\\SynDestroy.lua", kPlanDestroy);
    files.add("Data\\Scripts\\AI\\SpaceMode\\SynSweep.lua", kPlanSweep);
    files.add("Data\\Scripts\\AI\\SpaceMode\\SynEscort.lua", kPlanEscort);
    files.add("Data\\Scripts\\AI\\SpaceMode\\SynBuild.lua", kPlanBuild);
    files.add("Data\\Scripts\\AI\\SpaceMode\\SynRetreat.lua", kPlanRetreat);
    files.add("Data\\Scripts\\AI\\LandMode\\SynLandOnly.lua", kPlanDestroy);  // not loaded in space
    files.add("Data\\Scripts\\FreeStore\\SynTacticalFreeStore.lua", kFreeStore);
    defs.add_goals(kGoals, "goals.xml");
    defs.link();
    defs.add_function_set("SynSpaceSet", kSet, "SynSpaceSet.xml");
    defs.add_templates(kTemplates, "t.xml");
    char buf[1024];
    for (const char* n : {"SynEmpire", "SynRebel"}) {
        std::snprintf(buf, sizeof buf, kPlayer, n);
        defs.add_players(buf, "p.xml");
    }
    eqs.add("One", "1.0");
    eqs.add("Zero", "0.0");
    if (scripts) {
        files.add("Data\\Scripts\\Evaluators\\SynProbe.lua", kEvalProbe);
        files.add("Data\\Scripts\\Evaluators\\SynNil.lua", kEvalNil);
        eqs.add("Syn_Destroy_Desire",
                "(2 - Variable_Target.Health) * Script_SynProbe.Evaluate{Parameter_Script_Number = 3, "
                "Parameter_Script_String = \"INBASE\", Parameter_Script_String = \"FRIENDLY\"}");
        eqs.add("Syn_Sweep_Desire", "(Variable_Target.EnemyForceUnnormalized > 0) * 0.5 + 0 * Script_SynMissing.Evaluate");
        eqs.add("Syn_Want_Retreat", "(Game.Age > 75) * 3 + 0 * Script_SynNil.Evaluate");
    } else {
        eqs.add("Syn_Destroy_Desire", "2 - Variable_Target.Health");
        eqs.add("Syn_Sweep_Desire", "(Variable_Target.EnemyForceUnnormalized > 0) * 0.5");
        eqs.add("Syn_Want_Retreat", "(Game.Age > 75) * 3");
    }
    eqs.add("Current_Enemy_Location", "Variable_Target.EnemyForceUnnormalized");
    eqs.link();
}

struct RunResult {
    std::vector<uint64_t> crc;
    std::vector<std::string> decisions, executed, errors;
    uint64_t orders = 0, rejected = 0;
    size_t targets0 = 0, fs0 = 0;
    int32_t opponent0 = -1, opponent1 = -1;
    int32_t countdown_easy = -1;
    bool retreat1 = false;
    std::map<int32_t, int32_t> deaths;
    std::string summary;
    std::vector<std::string> pp_stats;  // A2 tactical
    std::vector<std::tuple<int32_t, int32_t, int32_t>> released;  // host unit_released calls (player, id, path)
    std::map<std::string, uint32_t> stats;              // world.stats() at the end
    size_t released_at_start = 0;                       // ... after frame 1
    uint32_t built = 0;                                 // variant 2: units the host built
    int32_t lock_probe = -1;                            // the station a script lock was put on at frame 1 (-1: none)
    bool lock_kept = false;                             // ... still locked when frame 2 starts
    int32_t lock_released_frame = -1;                   // ... frame of its OrderLockDone call (-1: none)
};

// variant 0: the golden skirmish; 1: only the opportunity-fire and MagicPlan plans, the host emits acquisitions and
// accepts HIJACK spawns (STRATEGIC_GAPS §5.3, AI_GALACTIC_3 §4.3); 2: production; 3: Script_ evaluators (kEvalProbe);
// 4: refixf t06 group orders (kPlanGroupAttack).
RunResult run(int32_t frames, int32_t difficulty0, int variant = 0, std::vector<std::string>* hijacks = nullptr,
              bool lock_probe = false) {
    RunResult out;
    MemoryFiles files;
    AiDefinitions defs;
    EquationSet eqs;
    syn_data(files, defs, eqs, variant == 3);
    if (variant == 2) files.add("Data\\Scripts\\AI\\SpaceMode\\SynBuild.lua", kPlanProduce);
    if (variant == 4) files.add("Data\\Scripts\\AI\\SpaceMode\\SynDestroy.lua", kPlanGroupAttack);
    if (variant == 1) {
        files.add("Data\\Scripts\\AI\\SpaceMode\\SynDestroy.lua", kPlanMagic);
        files.add("Data\\Scripts\\AI\\SpaceMode\\SynSweep.lua", kPlanOpportunity);
    }
    objects::GameObjectTypes types;
    objects::IdCounter ids;
    objects::ObjectStore store(objects::ModeKind::Space, objects::StoreKind::Primary, types, &ids);
    GameRandom rng(2027u, 5u);
    MockHost host(store);
    host.emit_opportunity = host.allow_hijack = variant == 1;
    host.ring_test = variant == 4;  // refixf t06
    // Player 0 (Empire) at -x, player 1 (Rebel) at +x; a pad; pools.
    const int32_t station0 = host.spawn(0, tStation, -3500.0f, 0.0f);
    host.spawn(0, tFrigate, -3000.0f, -200.0f);
    host.spawn(0, tCorvette, -3000.0f, 200.0f);
    host.spawn(0, tCorvette, -2900.0f, 0.0f);
    host.spawn(1, tStation, 3500.0f, 0.0f);
    host.spawn(1, tFrigate, 3000.0f, 0.0f);
    host.spawn(1, tFrigate, 3000.0f, 300.0f);
    host.spawn(1, tCorvette, 2900.0f, -300.0f);
    host.pad_ = host.spawn(0, tPad, 0.0f, 2500.0f);  // A2 tactical: an Empire pad (Friendly_Build_Pad of Syn_Build)
    if (variant == 2) host.enable_production(station0);
    host.add_pool(0, 1001, tCorvette, 1.0f);
    host.add_pool(0, 1002, tFrigate, 2.0f);
    host.add_pool(0, 1003, tCorvette, 1.0f);
    host.add_pool(0, 1004, tCorvette, 1.0f);  // A2 tactical: enough pool units for the escort plan too
    host.add_pool(0, 1005, tFrigate, 2.0f);
    host.add_pool(1, 2001, tCorvette, 1.0f);
    host.add_pool(1, 2002, tFrigate, 2.0f);
    TacticalAiConfig cfg;
    cfg.mode = TacticalMode::Space;
    cfg.players = {{0, "SynEmpire", difficulty0}, {1, "SynRebel", 1}};
    cfg.difficulties = {{"Easy_Default", 15.0f, 15.0f, 0.67f, 0.75f}, {"Normal_Default", 0.0f, 0.0f, 1.0f, 1.0f}};
    lua::LuaPlayerSetup a, b;
    a.faction = "Empire";
    a.ai = true;
    b.faction = "Rebel";
    b.ai = true;
    cfg.lua_players = {a, b};
    cfg.attacker = 1;
    cfg.defender = 0;
    cfg.map = {-4000.0f, -4000.0f, 4000.0f, 4000.0f};
    cfg.plan_files = tactical_plan_files(TacticalMode::Space, [&files](std::string_view glob) {
        const std::string g(glob);
        return files.list(g.substr(0, g.find('*')));
    });
    TacticalAiWorld world(store, &types, &files, rng, defs, eqs, host, cfg);
    std::vector<std::string> diag;
    CHECK(world.load_plans(&diag));
    CHECK(world.plans().plans.size() == 5);  // the LandMode plan is not loaded in space
    if (hijacks) hijacks->clear();
    CHECK(world.start(&diag));
    for (const std::string& d : diag) std::printf("  diag: %s\n", d.c_str());
    for (int32_t f = 1; f <= frames; ++f) {
        host.tick(static_cast<uint32_t>(f));
        if (f == 2 && out.lock_probe >= 0)
            out.lock_kept = world.object_bridge().state(static_cast<objects::ObjectId>(station0)).orders_locked;
        world.update(f, static_cast<uint32_t>(f));
        if (f == 1) {
            out.opponent0 = world.opponent(0);
            out.opponent1 = world.opponent(1);
            out.targets0 = world.live_targets(0);
            out.fs0 = world.free_store_size(0);
            out.released_at_start = host.released.size();
            if (lock_probe) {  // Lock_Current_Orders on the (idle) station, as a script would
                world.object_bridge().state(static_cast<objects::ObjectId>(station0)).orders_locked = true;
                out.lock_probe = station0;
            }
        } else if (out.lock_probe >= 0 && out.lock_released_frame < 0) {
            for (const auto& [pl, id, path] : host.released)
                if (id == station0 && path == static_cast<int32_t>(TacticalAiHost::UnitRelease::OrderLockDone)) {
                    CHECK(pl == 0);
                    CHECK(!world.object_bridge().state(static_cast<objects::ObjectId>(station0)).orders_locked);
                    out.lock_released_frame = f;
                }
        }
        if (difficulty0 == 0 && out.countdown_easy < 0 && world.context(0) && world.context(0)->goals.commits() > 0)
            out.countdown_easy = world.context(0)->goals.countdown();
        StateHasher h;
        world.hash(h);
        host.hash(h);
        rng.synced().hash(h);
        out.crc.push_back(h.digest());
    }
    for (const TacticalAiDecision& d : world.decision_log())
        out.decisions.push_back(std::to_string(d.frame) + " p" + std::to_string(d.player) + " " + d.text);
    out.executed = host.executed;
    out.errors = world.lua_errors();
    out.orders = world.orders_issued();
    out.rejected = world.orders_rejected();
    out.retreat1 = host.retreat_[1] || host.retreat_[0];
    out.deaths = host.deaths_;
    char buf[256];
    std::snprintf(buf, sizeof buf, "frames %d orders %llu rejected %llu deaths E=%d R=%d units left %zu plans %zu", frames,
                  static_cast<unsigned long long>(out.orders), static_cast<unsigned long long>(out.rejected), out.deaths[0],
                  out.deaths[1], host.units_.size(), world.running_plans());
    out.summary = buf;
    out.stats = world.stats();
    for (const auto& [k, v] : world.stats())  // A2 tactical: PotentialPlan outcomes (diagnostics)
        if (k.find("potential plan") != std::string::npos || k.find("accepted") != std::string::npos) out.pp_stats.push_back(k + " x" + std::to_string(v));
    world.end_battle();
    if (hijacks) *hijacks = host.hijacks;
    out.built = host.built;
    out.released = host.released;
    return out;
}

void test_opportunity_and_hijack() {
    std::vector<std::string> hj;
    const RunResult r = run(30 * 60, 1, 1, &hj);
    auto count = [&r](const std::string& needle) {
        size_t n = 0;
        for (const std::string& d : r.decisions) n += d.find(needle) != std::string::npos ? 1u : 0u;
        return n;
    };
    const size_t opp = count("event Hardpoint_Opportunity_Target_Acquired");
    const size_t hijacked = count("hijack Syn_Corvette -> pool");
    std::printf("opportunity / hijack: %zu opportunity events, %zu hijacks (%zu host calls), %zu Lua errors\n", opp, hijacked,
                hj.size(), r.errors.size());
    for (size_t i = 0; i < r.errors.size() && i < 5; ++i) std::printf("  lua error: %s\n", r.errors[i].c_str());
    CHECK(r.errors.empty());
    CHECK(opp > 0);       // laser acquisitions of TF members raise plan event 17 (torpedo ones are not subscribed)
    CHECK(hijacked > 0);  // the MagicPlan's Corvette came through the host's HIJACK hook
    bool reinforced = false;
    for (const std::string& e : r.executed) reinforced = reinforced || (e.find("p0 Reinforce key 50") != std::string::npos);
    CHECK(reinforced);    // ... and was dropped by TF.Reinforce from the pool
    const RunResult again = run(30 * 60, 1, 1);
    CHECK(again.crc == r.crc && again.decisions == r.decisions);
    // A1_READINGS §28.8 (corrects TACTICAL_GAPS §1.2): free-store admission does not re-apply Active_By_Default (none
    // on frame 1 although both stores filled); task forces releasing units do; no script lock in this run, so no
    // OrderLockDone.
    size_t tf = 0;
    for (const auto& [pl, id, path] : r.released) tf += path == static_cast<int32_t>(TacticalAiHost::UnitRelease::TaskForce) ? 1u : 0u;
    std::printf("active-by-default requests: %zu at start (free store %zu), %zu total, %zu task-force\n", r.released_at_start,
                r.fs0, r.released.size(), tf);
    CHECK(r.fs0 > 0 && r.released_at_start == 0);
    CHECK(tf > 0 && tf == r.released.size());
    CHECK(again.released == r.released);
    // A script lock (Lock_Current_Orders) on an idle unit is released when the AI sees it idle (port stand-in for
    // signal 10/11): orders_locked cleared, then one OrderLockDone call for it.
    const RunResult lk = run(60, 1, 1, nullptr, true);
    std::printf("order lock probe: station %d, released at frame %d\n", lk.lock_probe, lk.lock_released_frame);
    CHECK(lk.lock_probe >= 0 && lk.lock_kept);
    CHECK(lk.lock_released_frame == 2);
}

std::string golden_text(const RunResult& r) {
    std::ostringstream o;
    o << "# A1 stage 2 synthetic space skirmish (mock battle host): per-frame CRC (every 150th + last), decisions, orders\n";
    for (size_t i = 0; i < r.crc.size(); ++i)
        if ((i + 1) % 150 == 0 || i + 1 == r.crc.size()) {
            char b[64];
            std::snprintf(b, sizeof b, "frame %zu %016llx\n", i + 1, static_cast<unsigned long long>(r.crc[i]));
            o << b;
        }
    o << r.summary << "\n";
    for (const std::string& d : r.decisions) o << d << "\n";
    for (const std::string& d : r.executed) o << "exec " << d << "\n";
    return o.str();
}

// ---------------------------------------------------------------- A2 tactical: the tactical PotentialPlan

struct PpFixture {
    std::vector<PerceptionType> types;
    std::function<uint64_t(std::string_view)> mask;
    PlanDefinition def;
    SimRng rng{77u};
    PpFixture() {
        TacticalPerception p;
        mask = p.config().category_mask;
        const char* names[] = {"Syn_Frigate", "Syn_Corvette", "Syn_Station", "Syn_Pad", "UC_Syn_Turret"};
        const char* cats[] = {"Frigate", "Corvette", "Structure", "Structure", "Structure"};
        const float power[] = {1000.0f, 400.0f, 2000.0f, 0.0f, 0.0f};
        for (int i = 0; i < 5; ++i) {
            PerceptionType t;
            t.name = names[i];
            t.category = mask(cats[i]);
            t.combat_power = power[i];
            t.population = i == 0 ? 2.0f : 1.0f;
            t.tech_level = i == 0 ? 2 : 1;
            types.push_back(t);
        }
    }
    void plan(std::vector<std::vector<std::string>> tfs) {
        def = PlanDefinition{};
        def.name = "PP_TEST";
        for (const auto& s : tfs) {
            TaskForceDef d;
            CHECK(parse_task_force(s, d));
            def.task_forces.push_back(d);
        }
        renormalise_percents(def);
    }
    TacticalPpInput input(TacticalMode mode) {
        TacticalPpInput in;
        in.mode = mode;
        in.def = &def;
        in.player = 0;
        in.faction = "Empire";
        in.types = &types;
        in.category_mask = mask;
        in.rng = &rng;
        return in;
    }
};

void test_potential_plan() {
    // 1. Reinforcement (AI_GALACTIC_3 §1.4): a pool unit scores 2.0 and replaces a free unit of its type that is not at
    //    the stage (key >= 300); the 50 % rule; a used entry no longer matches; then the remaining free unit.
    {
        PpFixture f;
        f.plan({{"MainForce", "Frigate | Corvette = 1,3"}});
        TacticalPpInput in = f.input(TacticalMode::Space);
        in.has_target = true;
        in.target_position = {1000.0f, 0.0f, 0.0f};
        in.free_units = {{11, 0, {-3000.0f, 0.0f, 0.0f}, 1.0f, false}, {12, 0, {500.0f, 0.0f, 0.0f}, 1.0f, false},
                         {13, 1, {0.0f, 0.0f, 0.0f}, 1.0f, true}};  // 13 reserved by another PP
        in.pool = {{2001, 1, 1.0f, 1, -1, false, 1, false}, {2002, 0, 2.0f, 1, -1, false, 1, false}};
        TacticalPpResult r;
        const uint64_t d0 = f.rng.draws();
        CHECK(solve_tactical_pp(in, r));
        CHECK(f.rng.draws() == d0);  // one team per candidate: Setup C draws nothing; the marshallers never draw
        CHECK(r.candidates == 2);
        CHECK(r.picks.size() == 3);
        if (r.picks.size() == 3) {
            CHECK(r.picks[0].source == PpSource::Reinforcement && r.picks[0].ref == 2002 && r.picks[0].type == 0);
            CHECK(r.picks[1].source == PpSource::Reinforcement && r.picks[1].ref == 2001 && r.picks[1].type == 1);
            CHECK(r.picks[2].source == PpSource::FreeStore && r.picks[2].ref == 12);  // nearest free frigate
        }
        for (const TacticalPpPick& k : r.picks) CHECK(k.tf == 0 && k.team == 0);
        TacticalPpResult again;
        f.rng = SimRng{77u};
        CHECK(solve_tactical_pp(in, again) && again.picks.size() == r.picks.size());
        // A free unit already at the stage (key < 300) is kept; a full pop cap disables the pool (veto without a free unit).
        in.free_units = {{12, 0, {1010.0f, 0.0f, 0.0f}, 1.0f, false}};
        CHECK(solve_tactical_pp(in, r));
        // The frigate selection keeps unit 12; only once 12 is taken does the frigate pool entry 2002 serve.
        size_t at12 = r.picks.size(), at2002 = r.picks.size();
        for (size_t k = 0; k < r.picks.size(); ++k) {
            if (r.picks[k].source == PpSource::FreeStore && r.picks[k].ref == 12) at12 = std::min(at12, k);
            if (r.picks[k].ref == 2002) at2002 = std::min(at2002, k);
        }
        CHECK(at12 < r.picks.size() && at12 < at2002);
        in.free_units.clear();
        in.pop_cap = 5.0f;
        in.pop_used = 4.5f;  // room for one unit is 1.0 > 0.5
        CHECK(!solve_tactical_pp(in, r));
    }
    // 2. A company entry serves count(E) selections of its member type in one TF, charged once; minimum not met fails;
    //    Setup C draws one int per candidate with >= 2 teams.
    {
        PpFixture f;
        f.plan({{"MainForce", "Corvette = 2,3"}});
        TacticalPpInput in = f.input(TacticalMode::Land);
        in.pool = {{3001, 2, 1.0f, 2, 1, false, 2, false}};  // a "station" company of 2 corvettes
        TacticalPpResult r;
        CHECK(solve_tactical_pp(in, r) && r.picks.size() == 2 && r.picks[0].ref == 3001 && r.picks[1].ref == 3001);
        f.plan({{"MainForce", "Frigate = 2,2", "Frigate | Corvette = 0,1"}});
        in = f.input(TacticalMode::Land);
        in.free_units = {{11, 0, {}, 1.0f, false}};
        const uint64_t d0 = f.rng.draws();
        CHECK(!solve_tactical_pp(in, r) && r.last_veto == "Plan");
        CHECK(f.rng.draws() >= d0 + 1);  // Syn_Frigate sits in 2 teams
    }
    // 3. TacticalBuild (§1.8-1.9): a pad structure needs a goal target, an open pad that accepts it (the nearest to the
    //    target is taken) and the credits; vetoes are permanent.
    {
        PpFixture f;
        f.types[4].under_construction = true;
        f.types[4].tactical_cost = 100.0f;
        f.plan({{"Builder", "UC_Syn_Turret = 1"}});
        f.def.allow_free_store_units = false;
        TacticalPpInput in = f.input(TacticalMode::Land);
        in.has_target = true;
        in.target_position = {100.0f, 0.0f, 0.0f};
        in.credits = 150.0f;
        in.pads = {{40, {0.0f, 0.0f, 0.0f}, {1}, false}, {41, {900.0f, 0.0f, 0.0f}, {4}, false}, {42, {120.0f, 0.0f, 0.0f}, {4}, false}};
        TacticalPpResult r;
        CHECK(solve_tactical_pp(in, r) && r.picks.size() == 1 && r.picks[0].source == PpSource::Build && r.picks[0].ref == 42);
        in.credits = 50.0f;
        CHECK(!solve_tactical_pp(in, r));
        in.credits = 150.0f;
        in.has_target = false;
        CHECK(!solve_tactical_pp(in, r));
        in.has_target = true;
        in.pads[1].reserved = in.pads[2].reserved = true;
        CHECK(!solve_tactical_pp(in, r));
        // A normal unit in a build plan: only when a builder can build it now (none in S10 battles).
        f.plan({{"Builder", "Corvette = 1"}});
        f.def.allow_free_store_units = false;
        f.types[1].tactical_cost = 100.0f;
        in = f.input(TacticalMode::Land);
        in.credits = 150.0f;
        CHECK(!solve_tactical_pp(in, r));
        in.can_build_now = [](int32_t t) { return t == 1; };
        CHECK(solve_tactical_pp(in, r) && r.picks.size() == 1 && r.picks[0].source == PpSource::Produce);
    }
    // 4. IgnoreTarget: one synced contrast float per viable candidate per iteration.
    {
        PpFixture f;
        f.plan({{"MainForce", "Corvette = 1,1"}});
        f.def.ignore_target = true;
        TacticalPpInput in = f.input(TacticalMode::Space);
        in.pool = {{1, 1, 1.0f, 1, -1, false, 1, false}};
        TacticalPpResult r;
        const uint64_t d0 = f.rng.draws();
        CHECK(solve_tactical_pp(in, r) && r.picks.size() == 1);
        CHECK(f.rng.draws() == d0 + 1);  // iteration 2: the team is full, Plan's (permanent) veto comes before Contrast
    }
    // 5. Hero records (§2): a scripted hero never attaches in a plan that allows hero attach; with DenyHeroAttach on every
    //    TF it is a plain unit.
    {
        PpFixture f;
        f.types[1].is_hero = true;
        f.types[1].has_ai_script = true;
        TacticalPpInput in;
        TacticalPpResult r;
        f.plan({{"MainForce", "Corvette = 1,1"}});
        in = f.input(TacticalMode::Space);
        in.pool = {{1, 1, 1.0f, 1, -1, false, 1, false}};
        CHECK(!solve_tactical_pp(in, r));
        f.plan({{"MainForce", "DenyHeroAttach", "Corvette = 1,1"}});
        in = f.input(TacticalMode::Space);
        in.pool = {{1, 1, 1.0f, 1, -1, false, 1, false}};
        CHECK(solve_tactical_pp(in, r) && r.picks.size() == 1);
    }
    // 6. Team expansion: category masks over types (mask overlap), names, exclusions.
    {
        PpFixture f;
        TaskForceDef d;
        CHECK(parse_task_force({"T", "Frigate | Corvette = 1,2", "Syn_Station = 1", "-Syn_Corvette"}, d));
        const std::vector<int32_t> a = pp_team_types(d, d.teams[0], f.types, f.mask);
        const std::vector<int32_t> b = pp_team_types(d, d.teams[1], f.types, f.mask);
        CHECK(a == std::vector<int32_t>{0});
        CHECK(b == std::vector<int32_t>{2});
    }
    std::printf("tactical potential plan: OK\n");
}

void test_skirmish() {
    const int32_t frames = 30 * 120;
    const RunResult a = run(frames, 1);
    std::printf("synthetic skirmish: %s, %zu decisions, %zu Lua errors\n", a.summary.c_str(), a.decisions.size(), a.errors.size());
    for (size_t i = 0; i < a.errors.size() && i < 10; ++i) std::printf("  lua error: %s\n", a.errors[i].c_str());
    for (const std::string& s : a.pp_stats) std::printf("  %s\n", s.c_str());
    CHECK(a.errors.empty());
    CHECK(a.opponent0 == 1 && a.opponent1 == 0);
    // Targets: 9 objects (8 units / stations + the pad) + the 4 x 4 CELL grid of AI_SpaceEvaluatorRegionSize 2000.
    CHECK(a.targets0 == 9 + 16);
    CHECK(a.fs0 == 3);  // the station and the pad are not admitted
    CHECK(a.orders > 10 && a.rejected == 0);
    auto has = [&a](const std::string& needle) {
        for (const std::string& d : a.decisions)
            if (d.find(needle) != std::string::npos) return true;
        return false;
    };
    auto exec_has = [&a](const std::string& needle) {
        for (const std::string& d : a.executed)
            if (d.find(needle) != std::string::npos) return true;
        return false;
    };
    CHECK(has("plan ") && has("SYNDESTROY") && has("SYNSWEEP") && has("SYNESCORT"));
    CHECK(has("refused: pad may be occupied"));  // Build_All refusal (host check), no TacticalBuild issued
    CHECK(!exec_has("TacticalBuild"));
    CHECK(exec_has("Reinforce"));
    CHECK(exec_has("Attack") && exec_has("PortExtension"));
    CHECK(a.retreat1);  // Syn_Retreat after 75 s: Withdraw_Units -> Retreat
    // A2 tactical: the first sweep takes the first corvette entry of the pool in pool order (Reinforcement 2.0 beats the
    // free corvettes, AI_GALACTIC_3 §1.4) and drops it with QuickReinforce.
    size_t first_reinforce = a.executed.size();
    for (size_t i = 0; i < a.executed.size() && first_reinforce == a.executed.size(); ++i)
        if (a.executed[i].find("p0 Reinforce") != std::string::npos) first_reinforce = i;
    CHECK(first_reinforce < a.executed.size() && a.executed[first_reinforce].find("key 1001") != std::string::npos);
    const RunResult b = run(frames, 1);
    CHECK(b.crc == a.crc && b.decisions == a.decisions && b.executed == a.executed);
    std::printf("  second run: %s\n", b.crc == a.crc ? "identical" : "DIVERGED");
    // Easy: Space_AI_Goal_Cycle_Sleep_Duration 15 s after the first commit (slot 11).
    const RunResult e = run(300, 0);
    std::printf("  easy: countdown after the first commit %d\n", e.countdown_easy);
    CHECK(e.countdown_easy == 15 * 30);
    const std::string text = golden_text(a);
    const std::string path = std::string(ALAMO_AI_TACTICAL_GOLDEN_DIR) + "/ai_tactical_world_crc.txt";
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
            std::ofstream(std::filesystem::path(path + ".actual"), std::ios::binary) << text;
            std::printf("  actual written to %s.actual\n", path.c_str());
        }
        CHECK(same);
    }
}

}  // namespace

namespace {
// AI_GALACTIC_3 §1.11: a skirmish build plan buys its non-UC type through Production at a builder (first the host
// accepts, in the host's order); the built unit joins the task force and Produce_Force completes.
void test_tactical_production() {
    const RunResult r = run(30 * 60, 1, 2);
    auto count = [](const std::vector<std::string>& v, const std::string& needle) {
        size_t n = 0;
        for (const std::string& d : v) n += d.find(needle) != std::string::npos ? 1u : 0u;
        return n;
    };
    size_t accepted = 0, refused = 0;
    for (const std::string& e : r.executed)
        if (e.find("p0 Production type 1") != std::string::npos) ++(e.find("REJECTED") == std::string::npos ? accepted : refused);
    const size_t joined = count(r.decisions, "tf Builder produced Syn_Corvette");
    std::printf("tactical production: %zu Production orders accepted, %zu refused at the pad, %u built, %zu joined a task force, "
                "%zu Lua errors\n", accepted, refused, r.built, joined, r.errors.size());
    for (size_t i = 0; i < r.errors.size() && i < 5; ++i) std::printf("  lua error: %s\n", r.errors[i].c_str());
    CHECK(r.errors.empty());
    CHECK(accepted > 0 && refused == accepted);  // the pad (lower id in the list) is tried first every time
    CHECK(r.built > 0 && joined > 0 && joined <= r.built);
    CHECK(count(r.decisions, "in production") > 0);
    const RunResult again = run(30 * 60, 1, 2);
    CHECK(again.crc == r.crc && again.decisions == r.decisions && again.executed == r.executed);
}

// a1g t04 - land TF.Reinforce placement (0x6eaf60, A1_READINGS §30.7 [H]): own / allied live reinforcement points sorted
// stably by their float key, the units (LAST first) one per accepted spot on the r/2 ring of each point in turn.
void test_reinforce_landing() {
    PerceptionWorld w;
    w.players.resize(3);
    w.relation = {0, 1, 0, 1, 0, 1, 0, 1, 0};  // players 0 and 2 allied, 1 hostile to both
    std::vector<TacticalAiObject> facts;
    auto add = [&](int32_t id, int32_t owner, float x, float y, float radius, bool alive) {
        PerceptionObject o;
        o.id = id;
        o.owner = owner;
        o.alive = alive;
        o.position = {x, y, 0.0f};
        w.objects.push_back(o);
        TacticalAiObject a;
        a.applies.reinforcement_enabling_radius = radius;
        facts.push_back(a);
    };
    add(1, 1, 900.0f, 0.0f, 250.0f, true);    // enemy point next to the target: never used
    add(2, 0, 0.0f, 0.0f, 250.0f, true);      // own point, 1000 from the target
    add(3, 2, 500.0f, 400.0f, 250.0f, true);  // allied point, 640 from the target
    add(4, 0, 950.0f, 0.0f, 250.0f, false);   // dead own point
    add(5, 0, 990.0f, 0.0f, 0.0f, true);      // own unit without an enabling radius
    const Vec3f target{1000.0f, 0.0f, 0.0f};
    {
        // Steps 1-3: candidates = own / allied live objects with a radius; default key = 2-D distance (float).
        const std::vector<ReinforcePoint> pts = reinforce_points(w, facts, 0, target);
        CHECK(pts.size() == 2);
        CHECK(pts[0].position.x == 500.0f && pts[0].radius == 250.0f && pts[0].key == std::sqrt(500.0f * 500.0f + 400.0f * 400.0f));
        CHECK(pts[1].position.x == 0.0f && pts[1].key == 1000.0f);
        // Rank: drops the allied point; equal keys keep snapshot order; a failed path (1e18) sorts last.
        int asked = 0;
        const std::vector<ReinforcePoint> ranked = reinforce_points(w, facts, 0, target, [&asked](const Vec3f& pt, float& key) {
            ++asked;
            key = 3.0f;
            return pt.x != 500.0f;
        });
        CHECK(asked == 2 && ranked.size() == 1 && ranked[0].position.x == 0.0f);
        CHECK(reinforce_points(w, facts, 0, target, [](const Vec3f&, float&) { return false; }).empty());
        add(6, 2, -50.0f, 0.0f, 100.0f, true);   // allied: tie with point 3 (key 3) -> after it (snapshot order)
        add(7, 0, 2000.0f, 0.0f, 100.0f, true);  // own: unreachable (1e18) -> last
        const std::vector<ReinforcePoint> mixed = reinforce_points(w, facts, 0, target, [](const Vec3f& pt, float& key) {
            key = pt.x == 2000.0f ? 1.0e18f : pt.x == 0.0f ? 7.0f : 3.0f;
            return true;
        });
        CHECK(mixed.size() == 4 && mixed[0].position.x == 500.0f && mixed[1].position.x == -50.0f && mixed[2].position.x == 0.0f &&
              mixed[3].position.x == 2000.0f);
        w.objects.resize(5);
        facts.resize(5);
    }
    {
        // Step 4 ring: θ += 0.7853982f while θ < 6.2831855f -> nine float steps (the ninth θ = 6.283185 ≈ 2π).
        const std::vector<Vec3f> ring = reinforce_ring({{100.0f, 200.0f, 7.0f}, 40.0f, 0.0f});
        CHECK(ring.size() == 9);
        CHECK(ring[0].x == 120.0f && ring[0].y == 200.0f && ring[0].z == 7.0f);
        CHECK(std::fabs(ring[1].x - (100.0f + 20.0f * 0.70710678f)) < 1e-4f && std::fabs(ring[1].y - (200.0f + 20.0f * 0.70710678f)) < 1e-4f);
        CHECK(std::fabs(ring[2].x - 100.0f) < 1e-4f && std::fabs(ring[2].y - 220.0f) < 1e-4f);
        CHECK(std::fabs(ring[4].x - 80.0f) < 1e-4f && std::fabs(ring[4].y - 200.0f) < 1e-4f);
        CHECK(std::fabs(ring[8].x - 120.0f) < 1e-4f && std::fabs(ring[8].y - 200.0f) < 1e-4f);
    }
    auto unit = [](int32_t key, float pop) {
        TacticalReinforcement r;
        r.key = key;
        r.type = 0;
        r.population = pop;
        return r;
    };
    struct Landed {
        int32_t key;
        Vec3f at;
    };
    const float inf = HUGE_VALF;
    {
        // Spec test (§30.7): two allied points, three pending units -> p1 + r/2 (1, 0), p1 + r/2 (0.7071, 0.7071),
        // p1 + r/2 (0, 1), the LAST unit first.
        const std::vector<ReinforcePoint> pts = reinforce_points(w, facts, 0, target);
        std::vector<TacticalReinforcement> pending = {unit(11, 1.0f), unit(12, 1.0f), unit(13, 1.0f)};
        std::vector<Landed> landed;
        CHECK(reinforce_place(pts, pending, inf, {}, [&landed](const TacticalReinforcement& r, const Vec3f& at) {
            landed.push_back({r.key, at});
            return true;
        }));
        CHECK(pending.empty() && landed.size() == 3);
        const Vec3f p1 = pts[0].position;
        const float h = pts[0].radius * 0.5f;
        CHECK(landed[0].key == 13 && landed[0].at.x == p1.x + h && landed[0].at.y == p1.y);
        CHECK(landed[1].key == 12 && std::fabs(landed[1].at.x - (p1.x + h * 0.70710678f)) < 1e-3f &&
              std::fabs(landed[1].at.y - (p1.y + h * 0.70710678f)) < 1e-3f);
        CHECK(landed[2].key == 11 && std::fabs(landed[2].at.x - p1.x) < 1e-3f && std::fabs(landed[2].at.y - (p1.y + h)) < 1e-3f);
        // Same inputs, same bits (no RNG, no state).
        std::vector<TacticalReinforcement> again = {unit(11, 1.0f), unit(12, 1.0f), unit(13, 1.0f)};
        std::vector<Landed> landed2;
        CHECK(reinforce_place(pts, again, inf, {}, [&landed2](const TacticalReinforcement& r, const Vec3f& at) {
            landed2.push_back({r.key, at});
            return true;
        }));
        CHECK(landed2.size() == 3 && std::memcmp(&landed2[2].at, &landed[2].at, sizeof(Vec3f)) == 0);
    }
    {
        // Cascade: refused spots go on with the same unit; the first point refuses everything -> all land on point 2.
        const std::vector<ReinforcePoint> pts = reinforce_points(w, facts, 0, target);
        std::vector<TacticalReinforcement> pending = {unit(21, 1.0f), unit(22, 1.0f)};
        std::vector<Landed> landed;
        int tries = 0;
        CHECK(reinforce_place(pts, pending, inf, {}, [&](const TacticalReinforcement& r, const Vec3f& at) {
            ++tries;
            if (std::fabs(at.x - pts[0].position.x) <= pts[0].radius) return false;
            landed.push_back({r.key, at});
            return true;
        }));
        CHECK(tries == 9 + 2 && landed.size() == 2 && landed[0].key == 22 && landed[0].at.x == pts[1].position.x + pts[1].radius * 0.5f);
        // A refused spot (the host's land_drop_ok) sends the same unit to the next ring spot: spot 0 refused -> unit 22 at
        // spot 1, unit 21 at spot 2 (one unit per accepted spot).
        const std::vector<Vec3f> ring0 = reinforce_ring(pts[0]);
        std::vector<TacticalReinforcement> two = {unit(23, 1.0f), unit(24, 1.0f)};
        std::vector<Landed> next;
        std::vector<Vec3f> asked_at;
        CHECK(reinforce_place(pts, two, inf, {}, [&](const TacticalReinforcement& r, const Vec3f& at) {
            asked_at.push_back(at);
            if (std::memcmp(&at, &ring0[0], sizeof(Vec3f)) == 0) return false;
            next.push_back({r.key, at});
            return true;
        }));
        CHECK(asked_at.size() == 3 && next.size() == 2);
        CHECK(next[0].key == 24 && std::memcmp(&next[0].at, &ring0[1], sizeof(Vec3f)) == 0);
        CHECK(next[1].key == 23 && std::memcmp(&next[1].at, &ring0[2], sizeof(Vec3f)) == 0);
        // Ten units on one point of nine spots: nine placed, one left -> false.
        std::vector<ReinforcePoint> one = {pts[0]};
        std::vector<TacticalReinforcement> many;
        for (int32_t k = 0; k < 10; ++k) many.push_back(unit(30 + k, 1.0f));
        CHECK(!reinforce_place(one, many, inf, {}, [](const TacticalReinforcement&, const Vec3f&) { return true; }));
        CHECK(many.size() == 1 && many[0].key == 30);
    }
    {
        // Cap read once (remaining 3): unit 44 (pop 1) lands, then the trailing null 43 (popped, removed) and non-fitting
        // 42 (pop 5, popped, kept pending) go; 41 (pop 2) lands; remaining 0 < 1 stops. Not all placed -> false.
        const std::vector<ReinforcePoint> pts = reinforce_points(w, facts, 0, target);
        std::vector<TacticalReinforcement> pending = {unit(40, 1.0f), unit(41, 2.0f), unit(42, 5.0f), unit(43, 1.0f), unit(44, 1.0f)};
        std::vector<int32_t> keys;
        CHECK(!reinforce_place(pts, pending, 3.0f, [](const TacticalReinforcement& r) { return r.key == 43; },
                               [&keys](const TacticalReinforcement& r, const Vec3f&) {
                                   keys.push_back(r.key);
                                   return true;
                               }));
        CHECK(keys == std::vector<int32_t>({44, 41}));
        CHECK(pending.size() == 2 && pending[0].key == 40 && pending[1].key == 42);
        // [port] the trailing pop also runs before the first spot: a non-fitting last unit does not block the others.
        std::vector<TacticalReinforcement> front = {unit(50, 1.0f), unit(51, 9.0f)};
        keys.clear();
        CHECK(!reinforce_place(pts, front, 2.0f, {}, [&keys](const TacticalReinforcement& r, const Vec3f&) {
            keys.push_back(r.key);
            return true;
        }));
        CHECK(keys == std::vector<int32_t>({50}) && front.size() == 1 && front[0].key == 51);
    }
    CHECK(reinforce_points(w, facts, 1, target).size() == 1);  // player 1 owns point 1
    w.objects[0].alive = false;
    CHECK(reinforce_points(w, facts, 1, target).empty());  // no live point of its own or an ally's
    {
        // 0x6bf3d0 (A1_READINGS §28.4): a delivered unit fills the first null slot (a pick of another source) of its type,
        // keeping that slot's task force / team, else it is appended as a FreeStore pick of the given task force.
        auto pick = [](int32_t type, PpSource src, int32_t ref, int32_t tf, int32_t team) {
            TacticalPpPick k;
            k.type = type;
            k.source = src;
            k.ref = ref;
            k.tf = tf;
            k.team = team;
            return k;
        };
        std::vector<TacticalPpPick> picks = {pick(3, PpSource::FreeStore, 10, 0, 0), pick(3, PpSource::Reinforcement, 77, 1, 2),
                                             pick(3, PpSource::Build, 5, 1, 3), pick(4, PpSource::Produce, -1, 0, 1)};
        TacticalPpPick old;
        CHECK(reinforce_join_pp(picks, 900, 3, 1, &old));  // the FreeStore unit 10 is not a null slot; the pool slot is
        CHECK(old.source == PpSource::Reinforcement && old.ref == 77 && old.tf == 1 && old.team == 2);
        CHECK(picks.size() == 4 && picks[1].source == PpSource::FreeStore && picks[1].ref == 900 && picks[1].tf == 1 &&
              picks[1].team == 2 && picks[1].type == 3);
        CHECK(reinforce_join_pp(picks, 901, 3, 0, &old));  // next null slot of type 3: the build pad pick
        CHECK(old.source == PpSource::Build && old.ref == 5 && picks[2].ref == 901 && picks[2].source == PpSource::FreeStore);
        CHECK(!reinforce_join_pp(picks, 902, 3, 0));  // no null slot of type 3 left: appended
        CHECK(picks.size() == 5 && picks[4].type == 3 && picks[4].source == PpSource::FreeStore && picks[4].ref == 902 &&
              picks[4].tf == 0 && picks[4].team == -1);
        CHECK(reinforce_join_pp(picks, 903, 4, 0));  // the type-4 "to be produced" slot
        CHECK(picks[3].ref == 903 && picks[3].source == PpSource::FreeStore && picks[3].team == 1);
        CHECK(!reinforce_join_pp(picks, 904, 9, 2));  // another type: appended
        CHECK(picks.size() == 6 && picks[5].type == 9 && picks[5].tf == 2);
    }
    std::printf("reinforce landing: OK\n");
}
}  // namespace

namespace {
// Script_<File>.Evaluate in the tactical world (AI_GALACTIC_2 §4.4): the shared Data/Scripts/Evaluators VM gets
// PlayerObject / Target (the goal's AI target) and the parameters, Evaluator_Clean_Up runs after each call, a non-number
// result or a missing file fails the equation (so its goal is never proposed), and the run stays deterministic.
void test_script_evaluators() {
    const RunResult r = run(30 * 120, 1, 3);
    auto stat = [&r](const std::string& k) {
        const auto it = r.stats.find(k);
        return it == r.stats.end() ? 0u : it->second;
    };
    std::printf("script evaluators: SynProbe ok %u fail %u, SynNil fail %u, SynMissing missing %u; proposed destroy %u, "
                "retreat %u, sweep %u; %zu Lua errors\n", stat("script evaluator ok SynProbe"), stat("script evaluator FAIL SynProbe"),
                stat("script evaluator FAIL SynNil"), stat("script evaluator missing SynMissing"), stat("~ proposed Syn_Destroy_Desire"),
                stat("~ proposed Syn_Want_Retreat"), stat("~ proposed Syn_Sweep_Desire"), r.errors.size());
    for (size_t i = 0; i < r.errors.size() && i < 5; ++i) std::printf("  lua error: %s\n", r.errors[i].c_str());
    CHECK(r.errors.empty());
    CHECK(stat("script evaluator ok SynProbe") > 0 && stat("script evaluator FAIL SynProbe") == 0);
    CHECK(stat("~ proposed Syn_Destroy_Desire") > 0);  // the probe returned 1: arguments, globals and Target all as specified
    CHECK(stat("script evaluator FAIL SynNil") > 0 && stat("~ proposed Syn_Want_Retreat") == 0);
    CHECK(stat("script evaluator missing SynMissing") > 0 && stat("~ proposed Syn_Sweep_Desire") == 0);
    CHECK(!r.retreat1);
    const RunResult again = run(30 * 120, 1, 3);
    CHECK(again.crc == r.crc && again.decisions == r.decisions && again.executed == r.executed);
}
}  // namespace

namespace {
// ---------------------------------------------------------------- N1 stage 2: save / load mid-battle

// One session of run()'s battle (same data, units, pools and config), stepped by the caller.
struct SaveSession {
    MemoryFiles files;
    AiDefinitions defs;
    EquationSet eqs;
    MemoryFiles type_files;
    objects::ObjectTypeDatabase raw_types;
    objects::GameObjectTypes types;
    objects::IdCounter ids;
    objects::ObjectStore store{objects::ModeKind::Space, objects::StoreKind::Primary, types, &ids};
    GameRandom rng{2027u, 5u};
    MockHost host{store};
    std::unique_ptr<TacticalAiWorld> world;

    explicit SaveSession(int variant) {
        // The store saves type indices guarded by the type count: one bare SpaceUnit per SynType.
        std::string xml = "<SpaceUnits>";
        for (int32_t t = 0; t < tTypes; ++t) xml += "<SpaceUnit Name=\"Syn_Type_" + std::to_string(t) + "\"/>";
        type_files.add("Data\\XML\\GameObjectFiles.xml", "<L><File>SynTypes.xml</File></L>");
        type_files.add("Data\\XML\\SynTypes.xml", xml + "</SpaceUnits>");
        CHECK(raw_types.load(type_files) && types.build(raw_types) && types.size() == static_cast<size_t>(tTypes));
        syn_data(files, defs, eqs, variant == 3);
        if (variant == 1) {
            files.add("Data\\Scripts\\AI\\SpaceMode\\SynDestroy.lua", kPlanMagic);
            files.add("Data\\Scripts\\AI\\SpaceMode\\SynSweep.lua", kPlanOpportunity);
        }
        host.emit_opportunity = host.allow_hijack = variant == 1;
        host.ring_test = variant == 4;  // refixf t06
        host.spawn(0, tStation, -3500.0f, 0.0f);
        host.spawn(0, tFrigate, -3000.0f, -200.0f);
        host.spawn(0, tCorvette, -3000.0f, 200.0f);
        host.spawn(0, tCorvette, -2900.0f, 0.0f);
        host.spawn(1, tStation, 3500.0f, 0.0f);
        host.spawn(1, tFrigate, 3000.0f, 0.0f);
        host.spawn(1, tFrigate, 3000.0f, 300.0f);
        host.spawn(1, tCorvette, 2900.0f, -300.0f);
        host.pad_ = host.spawn(0, tPad, 0.0f, 2500.0f);
        host.add_pool(0, 1001, tCorvette, 1.0f);
        host.add_pool(0, 1002, tFrigate, 2.0f);
        host.add_pool(0, 1003, tCorvette, 1.0f);
        host.add_pool(0, 1004, tCorvette, 1.0f);
        host.add_pool(0, 1005, tFrigate, 2.0f);
        host.add_pool(1, 2001, tCorvette, 1.0f);
        host.add_pool(1, 2002, tFrigate, 2.0f);
        TacticalAiConfig cfg;
        cfg.mode = TacticalMode::Space;
        cfg.players = {{0, "SynEmpire", 1}, {1, "SynRebel", 1}};
        cfg.difficulties = {{"Easy_Default", 15.0f, 15.0f, 0.67f, 0.75f}, {"Normal_Default", 0.0f, 0.0f, 1.0f, 1.0f}};
        lua::LuaPlayerSetup a, b;
        a.faction = "Empire";
        a.ai = true;
        b.faction = "Rebel";
        b.ai = true;
        cfg.lua_players = {a, b};
        cfg.attacker = 1;
        cfg.defender = 0;
        cfg.map = {-4000.0f, -4000.0f, 4000.0f, 4000.0f};
        cfg.plan_files = tactical_plan_files(TacticalMode::Space, [this](std::string_view glob) {
            const std::string g(glob);
            return files.list(g.substr(0, g.find('*')));
        });
        world = std::make_unique<TacticalAiWorld>(store, &types, &files, rng, defs, eqs, host, cfg);
        CHECK(world->load_plans());
        CHECK(world->start());
    }
    ~SaveSession() {
        if (world) world->end_battle();
    }
    void step(int32_t f) {
        host.tick(static_cast<uint32_t>(f));
        world->update(f, static_cast<uint32_t>(f));
    }
    uint64_t crc() {
        StateHasher h;
        world->hash(h);
        host.hash(h);
        rng.synced().hash(h);
        return h.digest();
    }
    std::vector<uint8_t> ai_bytes() const {
        ByteWriter w;
        world->write(w);
        return w.bytes();
    }
    // A load as the battle host does it: the object store, the RNG and the host's own state first (its chunks), then
    // the AI chunk + relink().
    bool restore_host(const SaveSession& from) {
        ByteWriter ws, wr;
        from.store.write(ws);
        from.rng.write(wr);
        ByteReader rs(ws.bytes()), rr(wr.bytes());
        const bool s = store.read(rs) && rs.done();
        const bool g = rng.read(rr) && rr.done();
        const bool m = host.copy_state_from(from.host);
        if (!(s && g && m)) std::printf("  restore_host: store %d rng %d host %d\n", s, g, m);
        return s && g && m;
    }
    bool load_ai(std::span<const uint8_t> ai, bool quiet = false) {
        ByteReader ra(ai);
        const bool rd = world->read(ra);
        const bool dn = rd && ra.done();
        const bool rl = dn && world->relink();
        if (!rl && !quiet) std::printf("  load_ai: read %d done %d relink %d (at %zu of %zu)\n", rd, dn, rl, ai.size() - ra.remaining(), ai.size());
        return rl;
    }
};

std::string decision_line(const TacticalAiDecision& d) { return std::to_string(d.frame) + " p" + std::to_string(d.player) + " " + d.text; }

// A battle saved mid-fight (plans running with Lua threads suspended in their task-force loops, task forces holding
// units, free stores, PotentialPlan reservations, the threat grid and leaf cache filled; variant 1 adds opportunity-fire
// subscriptions and HIJACK spawns, variant 3 the Script_ evaluator VMs) is restored into a session rebuilt from the same
// data and continues identically; write -> read -> write gives the same bytes; truncated or corrupted AI chunks are
// refused and leave the reader as it was.
void test_save_load(int variant, int32_t save_at, int32_t frames) {
    SaveSession a(variant);
    for (int32_t f = 1; f <= save_at; ++f) a.step(f);
    const std::vector<uint8_t> image = a.ai_bytes();
    const size_t plans_at_save = a.world->running_plans();
    CHECK(plans_at_save > 0);

    SaveSession b(variant);
    CHECK(b.restore_host(a) && b.load_ai(image));
    const uint64_t crc_at_save = a.crc();
    CHECK(b.crc() == crc_at_save);
    CHECK(b.ai_bytes() == image);  // write -> read -> write
    CHECK(b.world->running_plans() == plans_at_save && b.world->opponent(0) == a.world->opponent(0) &&
          b.world->free_store_size(0) == a.world->free_store_size(0) && b.world->live_targets(1) == a.world->live_targets(1));
    for (int32_t p = 0; p < 2; ++p)
        for (const float x : {-3000.0f, 0.0f, 2900.0f}) CHECK(b.world->point_danger(p, x, 0.0f) == a.world->point_danger(p, x, 0.0f));
    // The rejection reader (variant 0): the other chunks restored, the AI not loaded yet.
    std::unique_ptr<SaveSession> c;
    if (variant == 0) {
        c = std::make_unique<SaveSession>(variant);
        CHECK(c->restore_host(a));
    }

    const size_t exec_at_save = a.host.executed.size();
    const size_t dec_a = a.world->decision_log().size(), dec_b = b.world->decision_log().size();
    int32_t first_diff = -1;
    for (int32_t f = save_at + 1; f <= frames; ++f) {
        a.step(f);
        b.step(f);
        if (first_diff < 0 && a.crc() != b.crc()) first_diff = f;
    }
    CHECK(first_diff < 0);
    CHECK(b.host.executed == a.host.executed && a.host.executed.size() > exec_at_save);
    CHECK(b.host.released == a.host.released && b.host.hijacks == a.host.hijacks && b.host.deaths_ == a.host.deaths_);
    std::vector<std::string> da, db;
    for (size_t i = dec_a; i < a.world->decision_log().size(); ++i) da.push_back(decision_line(a.world->decision_log()[i]));
    for (size_t i = dec_b; i < b.world->decision_log().size(); ++i) db.push_back(decision_line(b.world->decision_log()[i]));
    CHECK(!da.empty() && da == db);
    CHECK(a.world->lua_errors().empty() && b.world->lua_errors().empty());
    CHECK(b.ai_bytes() == a.ai_bytes());
    std::printf("save / load variant %d: AI chunk %zu bytes at frame %d (%zu plans running); %d frames after the load %s "
                "(%zu orders, %zu decisions)\n", variant, image.size(), save_at, plans_at_save, frames - save_at,
                first_diff < 0 ? "identical" : "DIVERGED", a.host.executed.size() - exec_at_save, da.size());
    if (first_diff >= 0) std::printf("  first CRC difference at frame %d\n", first_diff);
    if (variant != 0) return;

    // Rejection (variant 0): `c` (rebuilt and restored like `b`, AI chunk not read) refuses every truncation and the
    // corruptions below and stays a freshly started world (same CRC and bytes); then the intact image loads.
    const uint64_t fresh_crc = c->crc();
    const std::vector<uint8_t> fresh = c->ai_bytes();
    size_t tried = 0;
    auto rejects = [&](std::span<const uint8_t> data, bool check_bytes) {
        ++tried;
        ByteReader r(data);
        const bool ok = c->world->read(r);
        return !ok && c->crc() == fresh_crc && (!check_bytes || c->ai_bytes() == fresh);
    };
    const size_t n = image.size();
    const size_t stride = std::max<size_t>(1, n / 192);
    bool all = true;
    for (size_t cut = 0; cut < n; cut += cut < 256 ? 1 : stride) all = all && rejects(std::span<const uint8_t>(image.data(), cut), cut % 64 == 0);
    for (size_t cut = n > 64 ? n - 64 : 0; cut < n; ++cut) all = all && rejects(std::span<const uint8_t>(image.data(), cut), false);
    CHECK(all);
    // Header: u32 version, u32 AI player count + one bool each, u32 plan library size, u32 runtime size, u8 mode, i64
    // now, u32 mode frame, i32 current plan, i32 x 4 id counters (next plan first), u64 orders / rejected, the gate.
    const size_t lib_at = 8 + 2, mode_at = lib_at + 8, next_plan_at = mode_at + 1 + 8 + 4 + 4;
    ByteWriter gw;
    AiEnableGate().write(gw);
    const size_t perception_world_at = next_plan_at + 16 + 16 + gw.size();
    auto corrupt = [&](size_t at, std::initializer_list<uint8_t> v) {
        std::vector<uint8_t> bad = image;
        std::copy(v.begin(), v.end(), bad.begin() + static_cast<std::ptrdiff_t>(at));
        return rejects(bad, true);
    };
    CHECK(corrupt(0, {9}));                                  // version
    CHECK(corrupt(4, {3}));                                  // AI player count
    CHECK(corrupt(8, {7}));                                  // the pattern's first bool
    CHECK(corrupt(9, {0}));                                  // player 1 not an AI player
    CHECK(corrupt(lib_at, {static_cast<uint8_t>(image[lib_at] + 1)}));  // plan library size
    CHECK(corrupt(mode_at, {1}));                            // land instead of space
    CHECK(corrupt(next_plan_at, {0, 0, 0, 0}));              // next plan id 0 (plan ids start at 1)
    CHECK(corrupt(perception_world_at, {9}));                // PerceptionWorld version (2 since a1g t03)
    CHECK(corrupt(perception_world_at + 4, {7}));            // PerceptionWorld mode
    CHECK(corrupt(n - 1, {1}));                              // the last count (missing evaluators) past the data
    std::printf("  rejection: %zu truncated / corrupted AI chunks refused, reader unchanged\n", tried);
    CHECK(c->load_ai(image) && c->crc() == crc_at_save && c->ai_bytes() == image);  // control: the intact image loads
}

// The damage-tracking store (the host saves it, TacticalAiHost::damage_tracking): round trip, write -> read -> write,
// refused truncations / corruptions leave the store unchanged.
void test_damage_tracking_save() {
    DamageTrackingStore s;
    s.enable(7, 900.0f, 100.0f);
    s.enable(3, 400.0f, 0.0f);
    s.enable(12, 50.0f, 0.0f);
    s.record(7, 20, 15.5f);
    s.record(7, 4, 2.0f);
    s.record(3, 9, 30.0f);
    ByteWriter w;
    s.write(w);
    DamageTrackingStore t;
    t.enable(99, 1.0f, 1.0f);
    ByteReader r(w.bytes());
    CHECK(t.read(r) && r.done());
    StateHasher hs, ht;
    s.hash(hs);
    t.hash(ht);
    CHECK(hs.digest() == ht.digest() && !t.tracks(99) && t.tracks(12));
    ByteWriter again;
    t.write(again);
    CHECK(again.bytes() == w.bytes());
    const std::vector<uint8_t>& img = w.bytes();
    auto rejects = [&t, &ht](std::span<const uint8_t> data) {
        ByteReader rr(data);
        StateHasher h;
        const bool ok = t.read(rr);
        t.hash(h);
        return !ok && h.digest() == ht.digest();
    };
    bool all = true;
    for (size_t cut = 0; cut < img.size(); ++cut) all = all && rejects(std::span<const uint8_t>(img.data(), cut));
    CHECK(all);
    std::vector<uint8_t> bad = img;
    bad[0] = 2;  // version
    CHECK(rejects(bad));
    bad = img;
    bad[8] = 8;  // first victim id 3 -> 8 (> the next one, 7): not ascending
    CHECK(rejects(bad));
    std::printf("save: damage tracking store (%zu bytes) round trip, %zu truncations refused\n", img.size(), img.size());
}

// The AI pipeline's event bus gets a CommandResult per direct order, and nothing subscribes to it or reads it: it must be
// empty after every AI frame however many orders the battle gives. Together with test_save_load (a loaded world, whose
// bus was never saved, continues identically) this is the whole contract of the bus.
// refixf t06: a task force's Attack_Move is ONE PortExtension order carrying all its movable units (MOVEMENT §6.3: one
// coordinator), and Attack_Target(x, "Shield_Generator") orders hard-point type 1 (ref2 = 1 + type) [M: short name form].
void test_group_orders() {
    const RunResult r = run(30 * 120, 1, 4);
    size_t groups = 0, single = 0, shield = 0;
    for (const std::string& e : r.executed) {
        if (e.find("PortExtension sub 2 units ") != std::string::npos) {
            const int n = std::atoi(e.c_str() + e.find(" units ") + 7);
            (n >= 2 ? groups : single) += 1;
        }
        if (e.find(" Attack ref2 2") != std::string::npos) ++shield;
    }
    // refixf t06 (AI_TACTICAL §7.4): every space drop passed the host's drop test - a ring point, never behind a station.
    size_t drops = 0, bad_drops = 0, ring_drops = 0;
    for (const std::string& e : r.executed) {
        if (e.find(" Reinforce key ") == std::string::npos || e.find("REJECTED") != std::string::npos) continue;
        const size_t p = e.find(" at ");
        if (p == std::string::npos) continue;
        const int x = std::atoi(e.c_str() + p + 4);
        if (drops++ < 6) std::printf("  %s\n", e.c_str());
        bad_drops += (x < -3200 || x > 3200) ? 1u : 0u;
        ring_drops += ((x >= -3200 && x <= -2900) || (x >= 2900 && x <= 3200)) ? 1u : 0u;  // ring 500 around a station at +-3500
    }
    std::printf("space reinforce ring search: %zu drops (%zu on ring 500 at a station), %zu outside the allowed strip\n", drops, ring_drops,
                bad_drops);
    CHECK(drops > 0 && bad_drops == 0 && ring_drops > 0);
    // a1h t05 (A1_READINGS §32.1): a space delivery only joins the task force. SpaceReinforceBlockStatus's handler
    // (0x723ac0) never calls 0x6bf3d0, so no delivered unit joins a plan's PP (the land path logs "reinforce unit").
    size_t pp_joins = 0;
    for (const std::string& d : r.decisions) pp_joins += d.find(": reinforce unit ") != std::string::npos ? 1u : 0u;
    std::printf("space reinforce: %zu PP joins (0x6bf3d0 is land only)\n", pp_joins);
    CHECK(pp_joins == 0);
    std::printf("group orders: %zu attack-move group orders (2+ units), %zu one-unit, %zu Shield_Generator attacks, %zu Lua errors\n",
                groups, single, shield, r.errors.size());
    for (size_t i = 0; i < r.errors.size() && i < 5; ++i) std::printf("  lua error: %s\n", r.errors[i].c_str());
    CHECK(r.errors.empty());
    CHECK(groups > 0);
    CHECK(shield > 0);
}

void test_order_results_bounded(int variant, int32_t frames) {
    SaveSession a(variant);
    size_t worst = 0;
    for (int32_t f = 1; f <= frames; ++f) {
        a.step(f);
        worst = std::max(worst, a.world->pending_order_results());
    }
    CHECK(a.world->orders_issued() > 0 && !a.host.executed.empty());
    CHECK(worst == 0);
    std::printf("order results: variant %d, %d frames, %llu orders, %zu executed, at most %zu results left on the bus after a frame\n",
                variant, frames, static_cast<unsigned long long>(a.world->orders_issued()), a.host.executed.size(), worst);
}
}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--write-golden") == 0) g_write_golden = true;
    test_potential_plan();
    test_skirmish();
    test_opportunity_and_hijack();
    test_group_orders();
    test_tactical_production();
    test_reinforce_landing();
    test_script_evaluators();
    // N1 stage 2: save / load mid-battle
    test_damage_tracking_save();
    test_save_load(0, 30 * 35, 30 * 120);  // the golden skirmish (2 plans running at frame 1050)
    test_save_load(1, 30 * 30, 30 * 60);   // opportunity-fire subscriptions, HIJACK spawns
    test_save_load(3, 30 * 40, 30 * 80);   // Script_ evaluator VMs
    test_order_results_bounded(0, 30 * 120);
    test_order_results_bounded(1, 30 * 60);  // with the opportunity-fire and HIJACK plans
    std::printf(g_fail ? "alamo_ai_tactical_world_tests: %d FAILED\n" : "alamo_ai_tactical_world_tests: OK\n", g_fail);
    return g_fail ? 1 : 0;
}
