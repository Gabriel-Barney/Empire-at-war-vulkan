#pragma once
// Split out of test_ai_galactic.cpp (bo3 t03): shared includes, types and helpers; the tests live in test_ai_galactic_partN.cpp.
// A2 tests: AI framework rules (cadence, quota, desire, plan pick, solver, templates, budget, commit) and the
// galactic AI on the Lua bridge: a synthetic AI-vs-AI campaign (no game data; per-frame CRCs identical for
// 1/2/4/15 threads, golden tests/golden/ai_galactic_synthetic_crc.txt) and, with ALAMO_FOC_DATA, the shipped
// FoC AI data + plan scripts.
//
//   alamo_ai_galactic_tests [--write-golden]
#include <algorithm>
#include <array>
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
#include "alamo/core/host_fs.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/core/sim_rng.hpp"
#include "alamo/galactic/campaign.hpp"
#include "alamo/galactic/conflicts.hpp"
#include "alamo/galactic/fleet_path.hpp"
#include "alamo/lua/lua_vm.hpp"
#include "alamo/lua/players.hpp"
#include "alamo/lua/script_remaining.hpp"
#include "alamo/objects/game_object_types.hpp"
#include "alamo/objects/object_store.hpp"
#include "alamo/objects/object_types.hpp"  // A2k t06: ObjectTypeDatabase (mod-category type table)
#include "alamo/test/support.hpp"
#include "../src/galactic_goal_time.hpp"
#include "../src/galactic_slots.hpp"
#ifdef ALAMO_AI_HAVE_VFS
#include "alamo/data/game_database.hpp"
#include "alamo/galactic/galactic_rules.hpp"
#include "alamo/objects/object_types.hpp"
#include "alamo/vfs/vfs.hpp"
#endif

using namespace alamo;
using namespace alamo::ai;
namespace gal = alamo::galactic;

using ::alamo::test::g_checks;
using ::alamo::test::g_fail;

inline bool g_write_golden = false;
void test_cadence();
void test_goal_pass_cadence();
void test_desire_and_hysteresis();
void test_goal_reservations();
void test_goal_batched_accept();
void test_goal_batched_values();
void test_plan_pick_and_solver();

inline const char* kSynGoals = R"(<Goals>
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
inline const char* kSynSet = R"(<FunctionSet>
  <A><Goal>Build_Space_Forces</Goal><Function>Allow_Space</Function></A>
  <B><Goal>Conquer_Planet</Goal><Function>Conquer_Desire</Function></B>
  <C><Goal>Upgrade_Starbase</Goal><Function>Needs_Starbase_Upgrade</Function></C>
  <D><Goal>Land_Thing</Goal><Function>One</Function></D>
</FunctionSet>)";
inline const char* kSynTemplates = R"(<AITemplates>
  <Syn_Default><Priority>1</Priority><Trigger>One</Trigger>
    <Budget><Offensive>Offensive_Budget</Offensive><Infrastructure>Infra_Budget</Infrastructure><Defensive>Zero</Defensive></Budget>
    <Turn_On><Goals><Category>Offensive</Category><Category>Infrastructure</Category></Goals>
      <Plans><Goal_Category>Offensive</Goal_Category><Goal_Category>Infrastructure</Goal_Category></Plans></Turn_On>
  </Syn_Default>
  <Syn_Rich><Priority>3</Priority><Trigger>Is_Rich</Trigger>
    <Turn_Off><Goals><Goal_Type>Upgrade_Starbase</Goal_Type></Goals></Turn_Off></Syn_Rich>
</AITemplates>)";
inline const char* kSynPlayer = R"(<AIPlayerType><Name>%s</Name>
  <GalacticFreeStoreScript>SynFreeStore</GalacticFreeStoreScript>
  <GoalProposalFunctionSets>SynSet</GoalProposalFunctionSets>
  <Templates><Galactic>Syn_Default, Syn_Rich</Galactic></Templates>
  <Difficulty_Adjustments><Easy>Easy_Default</Easy><Normal>Normal_Default</Normal><Hard>Hard_Default</Hard></Difficulty_Adjustments>
</AIPlayerType>)";
void test_definitions_and_templates();
void test_budget_membership();
void test_contrast_rules();

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

inline const char* kSynBase = R"(
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
inline const char* kSynAICommands = R"(
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
-- a2m t03 (A2M_T03_PRODUCE_FORCE_CLAIMS Q1): a task force starts with no members, so the tests that read members
-- without assembling them first wait here until the test has added the selected units (take_selection_units).
function Wait_For_Members()
  GlobalValue.Set("take_ready", 1)
  while not GlobalValue.Get("take_done") do Sleep(1) end
end
)";
inline const char* kPlanBuildSpace = R"(
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
inline const char* kPlanConquer = R"(
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
inline const char* kPlanUpgrade = R"(
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
inline const char* kSynFreeStore = R"(
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
gal::GalacticRules syn_rules();
gal::GalacticSetup syn_setup();
void syn_data(MemoryFiles& files, AiDefinitions& defs, EquationSet& eqs, std::vector<std::string>& plan_files);
ParallelFor thread_pf(unsigned threads);

struct CampaignResult {
    std::vector<uint64_t> crc;  // per frame
    std::string summary;
    std::vector<std::string> decisions;
    std::vector<std::string> errors;
    std::vector<int32_t> owned;
    uint64_t orders = 0, rejected = 0, conflicts = 0;
};
CampaignResult run_synthetic(unsigned threads, int32_t frames);
std::string golden_text(const CampaignResult& r);
void test_plan_load_parallel();
void test_synthetic_campaign();
std::vector<uint64_t> run_path_world(unsigned threads, int32_t frames, lua::LuaPlayerWorld* story = nullptr);
void test_path_finder_world();
std::vector<std::array<int64_t, 4>> run_clock_phase(bool stagger, int32_t frames);
void test_galactic_clock_phase();
void test_sim_hooks();
void test_starbase_force_split();
void test_leaf_cache();
void test_force_members();

// A2 galactic, AI_GALACTIC_3 rules: heroes never attach through their AI script (§2.4), the TerrainType veto (§6.3),
// the galactic population cap when AI_Uses_Galactic_Pop_Cap is set (§3.5), MagicPlan spawns without production or
// charge (§4), and the perception DLL's division / clamp failures (§7.1).
inline const char* kPlanHeroAttach = R"(
require("SynAICommands")
function Definitions()
  Category = "Conquer_Planet"
  TaskForce = { { "MainForce", "HeroShip | Corvette = 1,1" } }
end
function MainForce_Thread() ScriptExit() end
)";
// a2m t03 (A2M_R2_RE_B §3.2): game-object scripts of the hero. HeroShipScript keeps running in galactic (a HeroService
// loop, as HeroPlanAttach); SpaceOnlyScript exits at its first pump unless the mode is Space (Home_One's
// ObjectScript_PowerToShields State_Init), then reads Object as the shipped script does.
inline const char* kObjHeroShip = R"(
require("SynBase")
function main()
  while 1 do PumpEvents() end
end
)";
inline const char* kObjSpaceOnly = R"(
require("SynBase")
function main()
  if Get_Game_Mode() ~= "Space" then ScriptExit() end
  if Object.Get_Owner().Is_Human() then ScriptExit() end
  while 1 do PumpEvents() end
end
)";
inline const char* kPlanHeroDeny = R"(
require("SynAICommands")
function Definitions()
  Category = "Conquer_Planet"
  TaskForce = { { "MainForce", "DenyHeroAttach", "HeroShip | Corvette = 1,1" } }
end
function MainForce_Thread() ScriptExit() end
)";
inline const char* kPlanMagic = R"(
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
// A2 f t01: a ground-structure plan on a friendly target (Upgrade_Starbase = the synthetic Infrastructure goal).
inline const char* kPlanBuildBarracks = R"(
require("SynAICommands")
function Definitions()
  Category = "Upgrade_Starbase"
  TaskForce = { { "BaseForce", "Barracks = 1" } }
end
function BaseForce_Thread()
  BaseForce.Set_As_Goal_System_Removable(false)
  BlockOnCommand(BaseForce.Produce_Force())
  BaseForce.Set_Plan_Result(true)
  ScriptExit()
end
function BaseForce_Production_Failed(tf, t)
  ScriptExit()
end
)";

// A2f t02: a category-mask team (CORVETTE is a GameObjectCategoryType name, AI_GALACTIC_2 §1.3).
inline const char* kPlanMaskCorvette = R"(
require("SynAICommands")
function Definitions()
  Category = "Conquer_Planet"
  TaskForce = { { "MainForce", "DenyHeroAttach", "Corvette = 1" } }
end
function MainForce_Thread() ScriptExit() end
)";
// q6 t18 (AI_GALACTIC_TOKENS §9.10): synthetic galactic plans with an escort team (no shipped galactic plan has one).
inline const char* kPlanEscortOnly = R"(
require("SynAICommands")
function Definitions()
  Category = "Conquer_Planet"
  TaskForce = { { "EscortForce", "EscortForce", "DenyHeroAttach", "Corvette = 1,1" } }
end
function EscortForce_Thread() ScriptExit() end
)";
inline const char* kPlanMainEscort = R"(
require("SynAICommands")
function Definitions()
  Category = "Conquer_Planet"
  TaskForce = { { "MainForce", "DenyHeroAttach", "Frigate = 0,1" }, { "EscortForce", "EscortForce", "DenyHeroAttach", "Corvette = 1,1" } }
end
function MainForce_Thread() ScriptExit() end
function EscortForce_Thread() ScriptExit() end
)";
// a2l t03 (A2L_T03_RE_ANSWERS Q5): Get_Stage of a goal without a target is nil; plain Produce_Force() has no
// destination, so nothing gathers (the hero stays where it is although the TF's stage is P2).
inline const char* kPlanA2lNoTarget = R"(
require("SynAICommands")
function Definitions()
  Category = "Build_Space_Forces"
  IgnoreTarget = true
  TaskForce = { { "MainForce", "DenyHeroAttach", "Frigate = 1" } }
end
function MainForce_Thread()
  MainForce.Set_As_Goal_System_Removable(false)
  if MainForce.Get_Stage() then GlobalValue.Set("a2l_nt_stage", 1) else GlobalValue.Set("a2l_nt_stage", 0) end
  BlockOnCommand(MainForce.Produce_Force())
  GlobalValue.Set("a2l_nt_done", 1)
  MainForce.Set_Plan_Result(true)
  ScriptExit()
end
)";
inline const char* kPlanA2lPlainProduce = R"(
require("SynAICommands")
function Definitions()
  Category = "Conquer_Planet"
  TaskForce = { { "MainForce", "DenyHeroAttach", "HeroShip = 1" } }
end
function MainForce_Thread()
  MainForce.Set_As_Goal_System_Removable(false)
  if MainForce.Get_Stage() then GlobalValue.Set("a2l_t_stage", 1) else GlobalValue.Set("a2l_t_stage", 0) end
  BlockOnCommand(MainForce.Produce_Force())
  GlobalValue.Set("a2l_t_done", 1)
  ScriptExit()
end
)";

// a2m t03 (A2M_T03_PRODUCE_FORCE_CLAIMS Q1-Q3): the task force starts with no members; Are_All_Units_On_Free_Store is
// false while the PotentialPlan has a unit to produce; plain Produce_Force() hands the claimed hero back to the free store
// and sends the produced corvette there too, so the task force still has no member when it returns.
inline const char* kPlanA2mT03Plain = R"(
require("SynAICommands")
function Definitions()
  Category = "Conquer_Planet"
  TaskForce = { { "MainForce", "DenyHeroAttach", "HeroShip = 1", "Corvette = 1" } }
end
function MainForce_Thread()
  MainForce.Set_As_Goal_System_Removable(false)
  GlobalValue.Set("t03_count0", MainForce.Get_Force_Count())
  if MainForce.Are_All_Units_On_Free_Store() then GlobalValue.Set("t03_all", 1) else GlobalValue.Set("t03_all", 0) end
  BlockOnCommand(MainForce.Produce_Force())
  GlobalValue.Set("t03_count1", MainForce.Get_Force_Count())
  GlobalValue.Set("t03_done", 1)
  while true do Sleep(1) end
end
)";
// a2m t02 (A2L_T01_RE_ANSWERS Q8): a task force whose two members sit in two fleets; Move_To moves only the FleetObject.
inline const char* kPlanA2mMoveTo = R"(
require("SynAICommands")
function Definitions()
  Category = "Conquer_Planet"
  TaskForce = { { "MainForce", "DenyHeroAttach", "HeroShip = 1", "Frigate = 1" } }
end
function MainForce_Thread()
  MainForce.Set_As_Goal_System_Removable(false)
  Wait_For_Members()
  GlobalValue.Set("a2m_count0", MainForce.Get_Force_Count())
  local block = MainForce.Move_To(Target)
  if block then GlobalValue.Set("a2m_block", 1) else GlobalValue.Set("a2m_block", 0) end
  BlockOnCommand(block)
  GlobalValue.Set("a2m_count1", MainForce.Get_Force_Count())
  GlobalValue.Set("a2m_done", 1)
  ScriptExit()
end
)";
// a2m t05 (A2M_T04_RE_ANSWERS Q6): Land_Units lands a transport only at a planet whose ground owner is the player or an
// ally; elsewhere it fires Ground_Activated abilities only. The infantry launches at the Empire's P0, lands back there,
// launches again, flies with the hero to the target and is not landed there.
inline const char* kPlanA2mLand = R"(
require("SynAICommands")
function Definitions()
  Category = "Conquer_Planet"
  TaskForce = { { "MainForce", "DenyHeroAttach", "HeroShip = 1", "Infantry = 1" } }
end
function MainForce_Thread()
  MainForce.Set_As_Goal_System_Removable(false)
  Wait_For_Members()
  BlockOnCommand(MainForce.Launch_Units())
  Sleep(2)
  GlobalValue.Set("a2m_l_launched", 1)
  local st = MainForce.Land_Units()
  if st then GlobalValue.Set("a2m_l_home", 1) else GlobalValue.Set("a2m_l_home", 0) end
  BlockOnCommand(st)
  Sleep(2)
  GlobalValue.Set("a2m_l_landed", 1)
  BlockOnCommand(MainForce.Launch_Units())
  Sleep(2)
  BlockOnCommand(MainForce.Move_To(Target))
  Sleep(2)
  local st2 = MainForce.Land_Units()
  if st2 then GlobalValue.Set("a2m_l_away", 1) else GlobalValue.Set("a2m_l_away", 0) end
  BlockOnCommand(st2)
  Sleep(2)
  GlobalValue.Set("a2m_l_done", 1)
  ScriptExit()
end
)";
// a2m t10 (A2M_T08_T10_T14_RE t10.3): No_Units_Remaining on each member loss that leaves no counted entry. The handler
// counts the events and the thread keeps the plan alive.
inline const char* kPlanA2mNur = R"(
require("SynAICommands")
function Definitions()
  Category = "Conquer_Planet"
  TaskForce = { { "MainForce", "DenyHeroAttach", "HeroShip = 1", "Infantry = 1" } }
end
function MainForce_Thread()
  MainForce.Set_As_Goal_System_Removable(false)
  Wait_For_Members()
  GlobalValue.Set("a2m_n_started", MainForce.Get_Force_Count())
  while true do Sleep(1) end
end
function MainForce_No_Units_Remaining()
  NurCount = (NurCount or 0) + 1
  GlobalValue.Set("a2m_nur", NurCount)
end
)";
// a2m r2 (A2M_R2_RE_B §1): the plan starts the production of a corvette and ends through its own script while the
// corvette is in production: no PAYG reclaim, so the AI queue cancels it with the refund at the next production service.
inline const char* kPlanA2mCancel = R"(
require("SynAICommands")
function Definitions()
  Category = "Conquer_Planet"
  TaskForce = { { "MainForce", "DenyHeroAttach", "HeroShip = 1", "Corvette = 1" } }
end
function MainForce_Thread()
  MainForce.Set_As_Goal_System_Removable(false)
  MainForce.Produce_Force()
  Sleep(4)
  GlobalValue.Set("a2m_c_exit", 1)
  ScriptExit()
end
)";
// a2m t02 (A2L_T01_RE_ANSWERS Q8.2): the thread waits for the test to remove a member, then calls Move_To. The test
// destroys the member in the FleetObject (TF +0x120); Move_To must move the re-elected fleet. The thread then stays
// alive, so the task force can still be read.
inline const char* kPlanA2mReelect = R"(
require("SynAICommands")
function Definitions()
  Category = "Conquer_Planet"
  TaskForce = { { "MainForce", "DenyHeroAttach", "HeroShip = 1", "Frigate = 1" } }
end
function MainForce_Thread()
  MainForce.Set_As_Goal_System_Removable(false)
  Wait_For_Members()
  GlobalValue.Set("a2m_r_ready", 1)
  while not GlobalValue.Get("a2m_r_go") do Sleep(1) end
  GlobalValue.Set("a2m_r_count", MainForce.Get_Force_Count())
  local block = MainForce.Move_To(Target)
  if block then GlobalValue.Set("a2m_r_block", 1) else GlobalValue.Set("a2m_r_block", 0) end
  BlockOnCommand(block)
  GlobalValue.Set("a2m_r_done", 1)
  while true do Sleep(1) end
end
function MainForce_Unit_Destroyed(tf, unit)
  local n = GlobalValue.Get("a2m_r_destroyed") or 0
  GlobalValue.Set("a2m_r_destroyed", n + 1)
  if unit then GlobalValue.Set("a2m_r_destroyed_arg", 1) end
end
)";
// a2m t02 (A2K_T08_RE_ANSWERS Q2.3 step 4, A2L_T01_RE_ANSWERS Q8.3): Produce_Force to the stage launches a member that
// sits on the stage's surface and merges it into the FleetObject, so the following Move_To takes it along. The thread
// waits for the test between the two.
inline const char* kPlanA2mStage = R"(
require("SynAICommands")
function Definitions()
  Category = "Conquer_Planet"
  TaskForce = { { "MainForce", "DenyHeroAttach", "HeroShip = 1", "Infantry = 1" } }
end
function MainForce_Thread()
  MainForce.Set_As_Goal_System_Removable(false)
  BlockOnCommand(MainForce.Produce_Force(MainForce.Get_Stage()))
  GlobalValue.Set("a2m_s_produced", 1)
  while not GlobalValue.Get("a2m_s_go") do Sleep(1) end
  BlockOnCommand(MainForce.Move_To(Target))
  GlobalValue.Set("a2m_s_done", 1)
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

    // only: the plan files to load (by base name), empty = all. rules_tweak: applied to the rules before the sim starts;
    // eq_first: equations added before the synthetic ones (a name added here wins: the first definition is kept);
    // tinfo_tweak: applied to the AI type info before the world is built.
    ParityWorld(bool pop_cap, const std::vector<std::string>& only,
                const std::function<void(gal::GalacticSetup&)>& tweak = {},
                const std::function<void(gal::GalacticRules&)>& rules_tweak = {},
                const std::function<void(EquationSet&)>& eq_first = {},
                const std::function<void(std::vector<GalacticAiTypeInfo>&)>& tinfo_tweak = {}) {
        if (eq_first) eq_first(eqs);
        syn_data(files, defs, eqs, plan_files);
        files.add("Data\\Scripts\\AI\\HeroAttach.lua", kPlanHeroAttach);
        files.add("Data\\Scripts\\AI\\HeroDeny.lua", kPlanHeroDeny);
        files.add("Data\\Scripts\\GameObject\\HeroShipScript.lua", kObjHeroShip);
        files.add("Data\\Scripts\\GameObject\\SpaceOnlyScript.lua", kObjSpaceOnly);
        files.add("Data\\Scripts\\AI\\MagicSpace.lua", kPlanMagic);
        files.add("Data\\Scripts\\AI\\BuildBarracks.lua", kPlanBuildBarracks);
        files.add("Data\\Scripts\\AI\\MaskCorvette.lua", kPlanMaskCorvette);
        files.add("Data\\Scripts\\AI\\EscortOnly.lua", kPlanEscortOnly);
        files.add("Data\\Scripts\\AI\\MainEscort.lua", kPlanMainEscort);
        files.add("Data\\Scripts\\AI\\A2lNoTarget.lua", kPlanA2lNoTarget);
        files.add("Data\\Scripts\\AI\\A2lPlainProduce.lua", kPlanA2lPlainProduce);
        files.add("Data\\Scripts\\AI\\A2mMoveTo.lua", kPlanA2mMoveTo);
        files.add("Data\\Scripts\\AI\\A2mLand.lua", kPlanA2mLand);
        files.add("Data\\Scripts\\AI\\A2mNur.lua", kPlanA2mNur);
        files.add("Data\\Scripts\\AI\\A2mReelect.lua", kPlanA2mReelect);
        files.add("Data\\Scripts\\AI\\A2mStage.lua", kPlanA2mStage);
        files.add("Data\\Scripts\\AI\\A2mCancel.lua", kPlanA2mCancel);
        files.add("Data\\Scripts\\AI\\A2mT03Plain.lua", kPlanA2mT03Plain);
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
        if (rules_tweak) rules_tweak(rules);
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
        if (tinfo_tweak) tinfo_tweak(tinfo);
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
void take_when_ready(ParityWorld& pw, lua::GlobalValueStore& gv);
void test_free_store_supply();
void own_p2(gal::GalacticSetup& s);
void test_escort_flag();
void test_parity_rules();
void test_ai_clock_precision();
void test_stage_and_setup_a();
void test_null_stage_and_plain_produce();
void test_move_to_fleet_object();
void test_fleet_object_reelection();
void test_produce_force_stage_launch();
void test_land_units_owner_rule();
void test_plain_produce_hands_back();
void test_ended_production_cancel();
void test_no_units_remaining_counted_entries();
void test_production_disabled_and_delivery();
void test_ground_structure_slots();
void test_structure_eta_queue_wait();
void test_queue_slot_lifetime();
void test_budget_fraction_before_init();
void test_production_unbuildable();

// A2f t02 R2: maintenance drops a replaceable goal when AllowedTime < ETA, ETA = 0x525200 + (NOW - created) / FPS
// (AI_GALACTIC §1.6) [H; 0x525200 = the time the goal's build tasks still need, M]. Was "ETA only": never late.
inline const char* kLateGoals = R"(<Goals>
  <Late_Goal><AIGoalApplicationFlags>Friendly</AIGoalApplicationFlags><GameMode>Galactic</GameMode>
    <Category>Offensive</Category><Reachability>Any</Reachability><Time_Limit>240</Time_Limit>
    <Build_Time_Delay_Tolerance>1.3</Build_Time_Delay_Tolerance></Late_Goal>
</Goals>)";

enum class LateMode { Stalled, OnSchedule };

struct LateHooks final : GoalModeHooks {
    int64_t now = 0, ended = -1, created = -1;
    bool removable = true;
    float eta = 10.0f;
    float nominal = -1.0f;  // Goal::nominal_time (A2f t09); < 0 = the ETA
    LateMode mode = LateMode::Stalled;
    uint32_t target_count() override { return 1; }
    bool applies(int32_t, uint32_t) override { return true; }
    bool blocked(uint32_t, int32_t) override { return false; }
    bool evaluate(const std::string&, int32_t, double& out) override {
        out = 10.0;
        return true;
    }
    bool accept(Goal& g) override {
        g.plan = 0;
        g.pp_valid = true;
        g.eta = eta;
        g.nominal_time = nominal < 0.0f ? eta : nominal;
        return true;
    }
    bool plan_activatable(Goal&) override { return true; }
    bool plan_invalid(const Goal& g) override { return g.running < 0; }
    bool replaceable(const Goal&) override { return removable; }
    void refresh_plan(Goal&) override {}
    bool uses_budget() override { return true; }
    bool can_fund(const Goal&) override { return true; }
    float category_credits(int32_t) override { return 1.0f; }
    void budget_add(Goal&) override {}
    void budget_release(Goal&) override {}
    void budget_weights() override {}
    std::vector<int32_t> category_order() override { return {}; }
    void queue_production(Goal&) override {}
    void instantiate(Goal& g) override {
        g.running = 1;
        created = g.created;
    }
    void end_plan(Goal&, bool) override {
        if (ended < 0) ended = now;
    }
    void drop_potential_plan(Goal&) override {}
    void suspend_reservations(Goal&) override {}
    float elapsed_eta(const Goal& g) override {
        // Stalled: the build still needs its whole time; on schedule: the remaining time falls with the clock.
        const float elapsed = static_cast<float>(now - g.created) / static_cast<float>(kAiFps);
        const float production = mode == LateMode::Stalled ? g.eta : std::max(g.eta - elapsed, 0.0f);
        return galactic_maintenance_eta(production, g.created, now);
    }
    int32_t quota_players() override { return 1; }
};
GoalSystemConfig galactic_goal_config();
int64_t late_goal_released_after(bool removable, float eta, LateMode mode, float nominal = -1.0f, int64_t* created = nullptr);
void test_goal_time_drop();
const Goal* last_active(const GoalSystem& gs);
void test_goal_allowed_time_base();
void test_btime_none_speed_factor();
void test_goal_nominal_time_resolve();

// ================================================================ a2g t05 (b): Resolve's time_ok (0x6c8970) on its own
//
// Resolve_Category (AI_GALACTIC §1.6): a candidate passes plan_activatable (0x6c4a50) and fundable, then time_ok: AllowedTime
// (nominal_time x Build_Time_Delay_Tolerance, A2f t09) >= ETA, or AllowedTime <= 0. A failure in the greedy phase records an
// activation failure and stops it; the weighted phase then draws the goal, fails it again and releases it. Tolerance 2.0
// keeps the products exact; ETA 10.
inline const char* kResolveGoals = R"(<Goals>
  <Resolve_Goal><AIGoalApplicationFlags>Friendly</AIGoalApplicationFlags><GameMode>Galactic</GameMode>
    <Category>Offensive</Category><Reachability>Any</Reachability><Time_Limit>240</Time_Limit>
    <Build_Time_Delay_Tolerance>2.0</Build_Time_Delay_Tolerance>
    <Activation_Tracking_Duration>60</Activation_Tracking_Duration></Resolve_Goal>
</Goals>)";

struct ResolveHooks final : GoalModeHooks {
    float eta = 10.0f, nominal = 20.0f;  // set at accept
    float resolved = -1.0f;              // >= 0: the re-solve's new nominal_time
    bool budget = true;
    int32_t activatable_calls = 0, drops = 0, instantiated = 0;
    uint32_t target_count() override { return 1; }
    bool applies(int32_t, uint32_t) override { return true; }
    bool blocked(uint32_t, int32_t) override { return false; }
    bool evaluate(const std::string&, int32_t, double& out) override {
        out = 10.0;
        return true;
    }
    bool accept(Goal& g) override {
        g.plan = 0;
        g.pp_valid = true;
        g.eta = eta;
        g.nominal_time = nominal;
        return true;
    }
    bool plan_activatable(Goal& g) override {
        ++activatable_calls;
        if (resolved >= 0.0f) g.nominal_time = resolved;
        return true;
    }
    bool plan_invalid(const Goal& g) override { return g.running < 0; }
    bool replaceable(const Goal&) override { return true; }
    void refresh_plan(Goal&) override {}
    bool uses_budget() override { return budget; }
    bool can_fund(const Goal&) override { return true; }
    float category_credits(int32_t) override { return 1.0f; }
    void budget_add(Goal&) override {}
    void budget_release(Goal&) override {}
    void budget_weights() override {}
    std::vector<int32_t> category_order() override { return {}; }
    void queue_production(Goal&) override {}
    void instantiate(Goal& g) override {
        g.running = 1;
        ++instantiated;
    }
    void end_plan(Goal&, bool) override {}
    void drop_potential_plan(Goal&) override { ++drops; }
    void suspend_reservations(Goal&) override {}
    float elapsed_eta(const Goal& g) override { return g.eta; }
    int32_t quota_players() override { return 1; }
};

struct ResolveOutcome {
    bool activated = false;
    int32_t activatable_calls = 0, activation_failures = 0, drops = 0;
};
ResolveOutcome resolve_once(float nominal, float resolved, bool budget, bool galactic = true);
void test_goal_resolve_time_ok();
void test_active_goals_category();
void test_maintenance_tokens();
void test_force_maintenance_tokens();
void test_budget_fraction_to_build();
bool patch_queue_head(gal::GalacticSim& sim, PlanetIndex planet, uint32_t q, float build_time, int32_t start,
                             int32_t duration, float remaining);
void test_budget_fraction_in_progress();
void test_budget_reset_on_template_switch();
void test_free_store_path_rules();
void test_free_store_world();
void test_prefer_ground_auto_land();

// ================================================================ P4 ai: Underworld special systems
//
// A synthetic Underworld AI against a passive Empire, with the four shipped Underworld hero plans (their Lua verbatim,
// require line aside) and the shipped Underworld goal equations (the save / planet-value terms reduced). The plans act
// through GalacticSim::ground_activated_trigger (Activate_Ability / Land_Units): the saboteur corrupts a clean planet
// (corruption_ai_pick) and sabotages a corrupted one, Tyber Zann buys on the black market of the planet he corrupted,
// IG-88 hacks the Death Star II orbiting a corrupted planet. Per-frame CRCs identical for 0 / 1 / 4 / 15 workers and
// against tests/golden/ai_underworld_crc.txt (MSVC and clang check the same file).
inline const char* kUwLibrary = R"(
require("SynAICommands")
function AssembleForce(taskforce)
  BlockOnCommand(taskforce.Produce_Force(taskforce.Get_Stage()))
end
function LandUnits(taskforce)
  BlockOnCommand(taskforce.Land_Units())
end
)";
inline const char* kUwPlanBlackMarket = R"(
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
inline const char* kUwPlanSaboteur = R"(
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
inline const char* kUwPlanHack = R"(
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
inline const char* kUwGoals = R"(<Goals>
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
inline const char* kUwSet = R"(<FunctionSet>
  <Corrupt_Planet><Goal>Corrupt_Planet</Goal><Function>Should_Corrupt_Planet</Function></Corrupt_Planet>
  <Black_Market_Purchase><Goal>Black_Market_Purchase</Goal><Function>Should_Make_Black_Market_Purchase</Function></Black_Market_Purchase>
  <Hack_Super_Weapon><Goal>Hack_Super_Weapon</Goal><Function>Should_Hack_Super_Weapon</Function></Hack_Super_Weapon>
  <Sabotage_Planet><Goal>Sabotage_Planet</Goal><Function>Should_Sabotage_Planet</Function></Sabotage_Planet>
</FunctionSet>)";
inline const char* kUwTemplates = R"(<AITemplates>
  <Uw_Default><Priority>1</Priority><Trigger>One</Trigger>
    <Budget><Offensive>Offensive_Budget</Offensive><Infrastructure>Infra_Budget</Infrastructure><Hero>Zero</Hero></Budget>
    <Turn_On><Goals><Category>Offensive</Category><Category>Infrastructure</Category><Category>Hero</Category></Goals>
      <Plans><Goal_Category>Offensive</Goal_Category><Goal_Category>Infrastructure</Goal_Category><Goal_Category>Hero</Goal_Category></Plans></Turn_On>
  </Uw_Default>
</AITemplates>)";
inline const char* kUwPlayer = R"(<AIPlayerType><Name>SynUnderworld</Name>
  <GalacticFreeStoreScript>SynFreeStore</GalacticFreeStoreScript>
  <GoalProposalFunctionSets>UwSet</GoalProposalFunctionSets>
  <Templates><Galactic>Uw_Default</Galactic></Templates>
  <Difficulty_Adjustments><Easy>Normal_Default</Easy><Normal>Normal_Default</Normal><Hard>Normal_Default</Hard></Difficulty_Adjustments>
</AIPlayerType>)";

enum UwType : uint32_t { uPlanet, uStruct, uDeathStar, uTyber, uSaboteur, uIG88, uTypes };
constexpr PlanetIndex kUwPlanets = 6;  // P0 Underworld home; P1..P5 Empire; P2, P3 corrupted at start; DS II over P3
gal::GalacticRules uw_rules();
gal::GalacticSetup uw_setup();

struct UwResult {
    std::vector<uint64_t> crc;
    std::vector<std::string> decisions, errors;
    std::string summary;
    int8_t corruption_p1 = -1;
    int32_t completed_bm = 0, sabotage_p2 = 0;
    size_t purchased = 0;
    bool p3_destroyed = false, ds_locked = false, ds_alive = true;
    float uw_credits = 0.0f;
    // a2k t08: the AI update in which the first corruption applied: credits and Σ category pools / available before
    // and after it (frame -1: no corruption applied).
    int32_t corr_frame = -1;
    float corr_credits_before = 0.0f, corr_credits_after = 0.0f;
    double corr_pool_before = 0.0, corr_pool_after = 0.0, corr_avail_before = 0.0, corr_avail_after = 0.0;
    // a2k t08 Q2 (shared_fleet runs): frames on which a moving fleet held more than one hero, and the "split" orders.
    int32_t shared_moves = 0, first_shared = -1;
    size_t splits = 0;
};
UwResult run_underworld(unsigned threads, int32_t frames, bool shared_fleet = false);
void test_underworld_special();

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
bool lua_state_persists();
bool save_lockstep(SaveRig& a, std::initializer_list<SaveRig*> others, int32_t first, int32_t frames, const char* label);

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
void save_rejects(SaveRig& a, const std::vector<uint8_t>& bytes, int cuts, const char* label);
void check_save_load(const SaveRigInputs& in, const char* label, int32_t warm, int32_t after, int cuts, bool full_session,
                            size_t max_bytes = 0, size_t max_packed = 0);
void test_save_synthetic();
void test_planet_type_is_type();
void test_player_conflict_tokens();
void test_budget_allocation_token();
void test_loaded_category_enum();
void test_ai_type_switch();

// ================================================================ FoC data

#ifdef ALAMO_AI_HAVE_VFS
using ::alamo::test::env_or_empty;
#ifdef ALAMO_AI_HAVE_VFS
size_t starbase_candidates_check(const gal::GalacticSim& sim, const gal::GalacticRules& rules, const GalacticAiWorld& world,
                                        const char* when);
#endif
#ifdef ALAMO_AI_HAVE_VFS
void test_foc_data();
#endif
#ifdef ALAMO_AI_HAVE_VFS
void test_save_foc();
#endif
#endif
void test_category_masks_from_enum_files();
void test_type_info_mod_categories();

// Test functions (defined in the part files)
