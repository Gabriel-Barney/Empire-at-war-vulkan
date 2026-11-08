// alamo_headless --skirmish-ai (A1 stage 2): see skirmish_run.hpp.
#include "skirmish_run.hpp"
#include "crc_out.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>

#ifdef ALAMO_SIM_HAVE_TACTICAL_AI
#include "alamo/ai/ai_definitions.hpp"
#include "alamo/ai/perception_equations.hpp"
#include "alamo/core/ascii.hpp"
#include "alamo/core/file_provider.hpp"
#include "alamo/data/game_database.hpp"
#include "alamo/data/values.hpp"
#include "alamo/galactic/campaign.hpp"
#include "alamo/galactic/conflicts.hpp"
#include "alamo/galactic/galactic_rules.hpp"
#include "alamo/game/skirmish_rules.hpp"  // q6 t03: AI types, default forces, map planet type (shared with alamo_game)
#include "alamo/jobs/crc_recorder.hpp"
#include "alamo/objects/game_object_types.hpp"
#include "alamo/objects/object_types.hpp"
#include "alamo/sim/battle_ai.hpp"
#include "alamo/sim/campaign_battles.hpp"
#include "alamo/sim/map_spawn.hpp"     // skai: TypeCrcIndex (--ai-log map objects)
#include "alamo/sim/tactical_battle.hpp"
#include "alamo/sim/save_session.hpp"  // N1 stage 2: skirmish_save_*
#include "alamo/sim/sim_anim.hpp"
#include "alamo/sim/sim_host.hpp"
#include "alamo/vfs/vfs.hpp"
#ifdef ALAMO_HEADLESS_HAVE_GALACTIC_AI
#include "alamo/ai/galactic_ai.hpp"         // a1j t07: make_galactic_ai_type_info
#include "alamo/ai/galactic_ai_tokens.hpp"  // a1j t07: GalacticSimTokenResolver
#endif
#else
#include "alamo/objects/game_object_types.hpp"  // N1 stage 2 stubs
#endif

#ifdef ALAMO_SIM_HAVE_TACTICAL_AI
namespace {

using namespace alamo;
namespace fs = std::filesystem;

std::string env_value(const char* name) {
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

// ---- synthetic data (S10 battle test types + tactical evaluator flags / UNIT_AI) ----
const char* kTypes = R"(<A1S>
 <Planet Name="A1S_A"><Behavior>PLANET</Behavior><Terrain>Temperate</Terrain><Planet_Credit_Value>10</Planet_Credit_Value></Planet>
 <Planet Name="A1S_B"><Behavior>PLANET</Behavior><Terrain>Temperate</Terrain><Planet_Credit_Value>10</Planet_Credit_Value>
  <Space_Tactical_Map>A1S_Space_B</Space_Tactical_Map></Planet>
 <Planet Name="A1S_C"><Behavior>PLANET</Behavior><Terrain>Desert</Terrain><Planet_Credit_Value>10</Planet_Credit_Value></Planet>
 <Projectile Name="A1S_Laser_Proj"><Max_Speed>40</Max_Speed><Projectile_Damage>120</Projectile_Damage>
  <Projectile_Max_Flight_Distance>2500</Projectile_Max_Flight_Distance><Damage_Type>Damage_Laser</Damage_Type></Projectile>
 <SpaceUnit Name="A1S_Frigate"><Victory_Relevant>Yes</Victory_Relevant>
  <Tactical_Health>900</Tactical_Health><Max_Speed>3.0</Max_Speed><Max_Rate_Of_Turn>2.0</Max_Rate_Of_Turn>
  <Hyperspace>Yes</Hyperspace><Hyperspace_Speed>1.5</Hyperspace_Speed><AI_Combat_Power>1000</AI_Combat_Power>
  <CategoryMask>Frigate</CategoryMask><Targeting_Max_Attack_Distance>1400</Targeting_Max_Attack_Distance>
  <Population_Value>2</Population_Value><Armor_Type>Armor_Ship</Armor_Type><Has_Space_Evaluator>Yes</Has_Space_Evaluator>
  <HardPoints>HP_A1S_Laser_A, HP_A1S_Laser_B</HardPoints>
  <Behavior>SELECTABLE, UNIT_AI, DAMAGE_TRACKING</Behavior><Collidable_By_Projectile_Living>Yes</Collidable_By_Projectile_Living><SpaceBehavior>STARSHIP_LOCOMOTOR</SpaceBehavior>
  <Custom_Soft_Footprint_Radius>60</Custom_Soft_Footprint_Radius>
 </SpaceUnit>
 <SpaceUnit Name="A1S_Corvette"><Variant_Of_Existing_Type>A1S_Frigate</Variant_Of_Existing_Type>
  <Tactical_Health>400</Tactical_Health><Max_Speed>5.0</Max_Speed><AI_Combat_Power>400</AI_Combat_Power>
  <CategoryMask>Corvette</CategoryMask><Population_Value>1</Population_Value><HardPoints>HP_A1S_Laser_B</HardPoints>
  <Custom_Soft_Footprint_Radius>30</Custom_Soft_Footprint_Radius></SpaceUnit>
 <StarBase Name="A1S_Star_Base_1"><Victory_Relevant>Yes</Victory_Relevant><Tactical_Health>2500</Tactical_Health><Base_Level>1</Base_Level>
  <AI_Combat_Power>600</AI_Combat_Power><CategoryMask>Structure</CategoryMask><HardPoints>HP_A1S_Laser_A</HardPoints>
  <Targeting_Max_Attack_Distance>1500</Targeting_Max_Attack_Distance><Has_Space_Evaluator>Yes</Has_Space_Evaluator>
  <Behavior>SELECTABLE, UNIT_AI, DAMAGE_TRACKING</Behavior><Collidable_By_Projectile_Living>Yes</Collidable_By_Projectile_Living></StarBase>
</A1S>
)";
const char* kHardpoints = R"(<HardPoints>
 <HardPoint Name="HP_A1S_Laser_A"><Type>HARD_POINT_WEAPON_LASER</Type><Health>150</Health>
  <Is_Targetable>Yes</Is_Targetable><Is_Destroyable>Yes</Is_Destroyable>
  <Fire_Projectile_Type>A1S_Laser_Proj</Fire_Projectile_Type><Fire_Min_Recharge_Seconds>2.0</Fire_Min_Recharge_Seconds>
  <Fire_Max_Recharge_Seconds>3.0</Fire_Max_Recharge_Seconds><Fire_Pulse_Count>2</Fire_Pulse_Count>
  <Fire_Pulse_Delay_Seconds>0.2</Fire_Pulse_Delay_Seconds><Fire_Range_Distance>1400</Fire_Range_Distance><Fire_Cone_Width>360</Fire_Cone_Width><Fire_Cone_Height>180</Fire_Cone_Height></HardPoint>
 <HardPoint Name="HP_A1S_Laser_B"><Variant_Of_Existing_Type>HP_A1S_Laser_A</Variant_Of_Existing_Type><Health>100</Health></HardPoint>
</HardPoints>
)";
// [name-ok: synthetic test data of the headless battle host (--skirmish-ai synthetic)]
const char* kFactions = R"(<Factions>
 <Faction Name="Empire"><Is_Playable>Yes</Is_Playable><Basic_AI>BasicEmpire</Basic_AI><Can_Control_Planets>Yes</Can_Control_Planets>
  <Space_Tactical_Unit_Cap>6</Space_Tactical_Unit_Cap><Space_Retreat_Countdown_Seconds>5</Space_Retreat_Countdown_Seconds></Faction>
 <Faction Name="Rebel"><Is_Playable>Yes</Is_Playable><Basic_AI>BasicRebel</Basic_AI><Can_Control_Planets>Yes</Can_Control_Planets>
  <Space_Tactical_Unit_Cap>6</Space_Tactical_Unit_Cap><Space_Retreat_Countdown_Seconds>5</Space_Retreat_Countdown_Seconds></Faction>
 <Faction Name="Neutral"><Is_Neutral>Yes</Is_Neutral></Faction>
</Factions>
)";
const char* kContrastLua = R"(function Set_Contrast_Values()
	EnemyContrastTypes[1] = "Corvette"
	FriendlyContrastTypeNames = {"Frigate", "Corvette"}
	FriendlyContrastWeights =	{1.5, 1.0}
	FriendlyContrastTypes[1].Parse(FriendlyContrastTypeNames, FriendlyContrastWeights)
	EnemyContrastTypes[2] = "Frigate"
	FriendlyContrastTypeNames = {"Frigate", "Corvette"}
	FriendlyContrastWeights =	{1.0, 0.5}
	FriendlyContrastTypes[2].Parse(FriendlyContrastTypeNames, FriendlyContrastWeights)
end
)";
const char* kGoals = R"(<Goals>
  <Skirmish_Destroy_Unit><AIGoalApplicationFlags>Enemy_Unit | Enemy_Structure</AIGoalApplicationFlags><GameMode>Space</GameMode>
    <Category>Tactical_Targeted</Category><Reachability>Any</Reachability><Time_Limit>120</Time_Limit>
    <Tracking_Duration>30</Tracking_Duration><Per_Failure_Desire_Adjust>-0.5</Per_Failure_Desire_Adjust></Skirmish_Destroy_Unit>
  <Skirmish_Sweep_Area><AIGoalApplicationFlags>Tactical_Location</AIGoalApplicationFlags><GameMode>Space</GameMode>
    <Category>Tactical_Targeted</Category><Reachability>Any</Reachability>
    <Tracking_Duration>30</Tracking_Duration><Per_Failure_Desire_Adjust>-0.5</Per_Failure_Desire_Adjust></Skirmish_Sweep_Area>
</Goals>)";
const char* kSet = R"(<FunctionSet>
  <A><Goal>Skirmish_Destroy_Unit</Goal><Function>Skirmish_Destroy_Desire</Function></A>
  <B><Goal>Skirmish_Sweep_Area</Goal><Function>Skirmish_Sweep_Desire</Function></B>
</FunctionSet>)";
const char* kTemplates = R"(<AITemplates>
  <Skirmish_Space><Priority>1</Priority><Trigger>One</Trigger>
    <Turn_On><Goals><Category>Tactical_Targeted</Category></Goals><Plans><Goal_Category>Tactical_Targeted</Goal_Category></Plans></Turn_On>
  </Skirmish_Space>
</AITemplates>)";
const char* kPlayer = R"(<AIPlayerType><Name>%s</Name>
  <SpaceFreeStoreScript>SkirmishFreeStore</SpaceFreeStoreScript>
  <GoalProposalFunctionSets>SkirmishSpaceSet</GoalProposalFunctionSets>
  <Templates><Space>Skirmish_Space</Space></Templates>
  <Difficulty_Adjustments><Easy>Easy_Default</Easy><Normal>Normal_Default</Normal><Hard>Hard_Default</Hard></Difficulty_Adjustments>
</AIPlayerType>)";
const char* kBase = R"(
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
const char* kAICommands = R"(
require("SkirmishBase")
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
function QuickReinforce(tf)
  local home = FindTarget(tf, "One", "Friendly_Structure | Friendly_Unit", 1.0)
  if home then
    local st = tf.Reinforce(home)
    if st then BlockOnCommand(st, 5) end
  end
end
)";
const char* kPlanDestroy = R"(
require("SkirmishAICommands")
ScriptPoolCount = 2
function Definitions()
  Category = "Skirmish_Destroy_Unit"
  TaskForce = { { "MainForce", "Frigate | Corvette = 1,4" } }
end
function MainForce_Thread()
  MainForce.Set_As_Goal_System_Removable(false)
  BlockOnCommand(MainForce.Produce_Force())
  QuickReinforce(MainForce)
  if MainForce.Get_Force_Count() == 0 then ScriptExit() end
  local d = EvaluatePerception("Skirmish_Destroy_Desire", PlayerObject, AITarget)
  if d and d > 0 and TestValid(Target) then
    BlockOnCommand(MainForce.Attack_Target(Target, "Random"), 60)
  end
  MainForce.Set_Plan_Result(true)
  ScriptExit()
end
function MainForce_No_Units_Remaining()
  ScriptExit()
end
)";
const char* kPlanSweep = R"(
require("SkirmishAICommands")
ScriptPoolCount = 2
function Definitions()
  Category = "Skirmish_Sweep_Area"
  TaskForce = { { "SweepForce", "Corvette | Frigate = 1,2" } }
end
function SweepForce_Thread()
  BlockOnCommand(SweepForce.Produce_Force())
  QuickReinforce(SweepForce)
  if SweepForce.Get_Force_Count() == 0 then ScriptExit() end
  BlockOnCommand(SweepForce.Attack_Move(AITarget, SweepForce.Get_Self_Threat_Max()), 40)
  local deadly = FindDeadlyEnemy(SweepForce)
  if TestValid(deadly) then BlockOnCommand(SweepForce.Attack_Target(deadly), 30) end
  SweepForce.Set_Plan_Result(true)
  ScriptExit()
end
)";
const char* kFreeStore = R"(
require("SkirmishBase")
function Base_Definitions()
  ServiceRate = 5
  UnitServiceRate = 2
end
function main()
  while true do
    FreeStoreService()
    PumpEvents()
  end
end
function FreeStoreService()
  enemy_unit = FindTarget.Reachable_Target(PlayerObject, "Skirmish_Destroy_Desire", "Enemy_Unit | Enemy_Structure", "Any", 0.5)
end
function On_Unit_Added(object)
end
function On_Unit_Service(object)
  if not TestValid(object) then return end
  if object.Has_Active_Orders() then return end
  if enemy_unit then
    local o = enemy_unit.Get_Game_Object()
    if TestValid(o) then object.Attack_Move(o) end
  end
end
)";

void write_file(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << s;
}

struct Session {
    std::unique_ptr<FileProvider> files;
    data::GameDatabase db;
    objects::ObjectTypeDatabase raw;
    objects::GameObjectTypes types;
    galactic::GalacticRules rules;
    galactic::ConflictRules crules;
    std::unique_ptr<sim::BattleCatalog> catalog;
    ai::AiDefinitions defs;
    ai::EquationSet eqs;
    std::shared_ptr<sim::TacticalAiSetup> ai;
    galactic::GalacticSetup setup;
    std::string map;
    bool land = false;  // battle host: foc-land - a land invasion instead of a space conflict
    // t48: BattleConfig::skirmish, for the kinds that are skirmishes: synthetic, foc and (t50) foc-land, a land skirmish on an _MP_LAND_
    // map with its pre-built base and MP_Default_Credits. foc-land-campaign keeps the old invasion of a campaign map (the campaign
    // rules); foc-station guards the campaign-only defender retreat rule (t41).
    bool skirmish = true;
    // skai (q2f t04): the skirmish host (foc-land, skirmish): slot 0 Empire, slot 1 Rebel, the battle started on the map
    // (CampaignBattles::start_map_battle) with each slot's default forces; human = the idle human slot's player (-1: none).
    bool map_host = false;
    int32_t human = -1;
};

bool load_game(Session& s, std::vector<std::string>& diag) {
    if (!s.db.load(*s.files, {}, &diag) || !s.raw.load(*s.files, &diag) || !s.types.build(s.raw, s.files.get(), &diag) ||
        !galactic::load_galactic_rules(*s.files, s.db, s.types, s.rules, &diag))
        return false;
    galactic::load_conflict_rules(*s.files, s.db, s.types, s.rules, s.crules, &diag);
    s.catalog = std::make_unique<sim::BattleCatalog>(s.types, s.files.get(), &s.db.constants(), &diag);
    return true;
}

// 3 planets A - B - C; the defender (player 0) holds B's orbit, the attacker (player 1) moves in from C.
// q6 t02: `slots` = the factions of players 0 and 1 (empty = Empire, Rebel).
galactic::GalacticSetup make_galaxy(const Session& s, uint32_t planet_type, const std::vector<uint32_t>& defender,
                                    const std::vector<uint32_t>& attacker, float credits = 0.0f, const std::vector<uint32_t>& slots = {}) {
    galactic::GalacticSetup g;
    GalaxyMap& m = g.map;
    m.campaign_name = "A1_Skirmish";
    for (uint32_t i = 0; i < 3; ++i) {
        m.planets.add(std::string("A1S_P") + std::to_string(i));
        m.planets.position[i] = {static_cast<float>(i) * 60.0f, 0.0f, 0.0f};
        m.planets.flags[i] = static_cast<uint32_t>(kPlanetHasPlanetBehavior);
    }
    for (uint32_t i = 0; i + 1 < 3; ++i) {
        const uint32_t r = m.routes.add("R" + std::to_string(i));
        m.routes.point_a[r] = i;
        m.routes.point_b[r] = i + 1;
        m.routes.hs_speed_factor[r] = 5.0f;
    }
    g.planet_type = {planet_type, planet_type, planet_type};
    galactic::PlayerSetup e;
    // [name-ok-begin: the foc-* / synthetic test scenarios' sides (their goldens); the skirmish host passes `slots`]
    e.faction = slots.size() == 2 ? slots[0] : s.rules.find_faction("Empire");
    e.ai = true;
    e.credits = credits;
    galactic::PlayerSetup r = e;
    r.faction = slots.size() == 2 ? slots[1] : s.rules.find_faction("Rebel");
    // [name-ok-end]
    galactic::PlayerSetup n;
    n.faction = s.rules.neutral_faction();  // modc t03: the first Is_Neutral faction (FoC: Neutral)
    g.players = {e, r, n};
    // S10 map owners [probe 2026-10-29]: a Hostile player besides Neutral (map owners resolve by faction; natives -> Hostile).
    galactic::PlayerSetup h;
    h.faction = s.rules.native_owner_faction();  // modc t03: by data (FoC: Hostile)
    if (h.faction != galactic::kNoIndex && h.faction != e.faction && h.faction != r.faction) g.players.push_back(h);
    for (const uint32_t t : defender) g.forces.push_back({0, 1, t});
    for (const uint32_t t : attacker) g.forces.push_back({1, 2, t});
    return g;
}

bool synthetic_session(Session& s, std::vector<std::string>& diag) {
    const fs::path root = fs::temp_directory_path() / "alamo_a1_skirmish_ai";
    std::error_code ec;
    remove_all_long(root, ec);
    const fs::path x = root / "Data/XML";
    write_file(x / "GameObjectFiles.xml", "<GameObjectFiles><File>A1STypes.xml</File></GameObjectFiles>\n");
    write_file(x / "A1STypes.xml", kTypes);
    write_file(x / "HardPointDataFiles.xml", "<Hard_Point_Files><File>A1SHardPoints.xml</File></Hard_Point_Files>\n");
    write_file(x / "A1SHardPoints.xml", kHardpoints);
    write_file(x / "FactionFiles.xml", "<FactionFiles><File>A1SFactions.xml</File></FactionFiles>\n");
    write_file(x / "A1SFactions.xml", kFactions);
    write_file(x / "TradeRouteFiles.xml", "<TradeRouteFiles></TradeRouteFiles>\n");
    write_file(x / "CampaignFiles.xml", "<CampaignFiles></CampaignFiles>\n");
    write_file(x / "GameConstants.xml",
               "<GameConstants>\n <Damage_Types>Damage_Default, Damage_Laser</Damage_Types>\n"
               " <Armor_Types>Armor_Default, Armor_Ship</Armor_Types>\n"
               " <Damage_To_Armor_Mod>Damage_Laser, Armor_Ship, 0.8</Damage_To_Armor_Mod>\n"
               " <Space_Retreat_Allowed_Countdown_Seconds>5.0</Space_Retreat_Allowed_Countdown_Seconds>\n</GameConstants>\n");
    write_file(root / "Data/Scripts/Library/PGAICommands.lua", kContrastLua);
    write_file(root / "Data/Scripts/Library/SkirmishBase.lua", kBase);
    write_file(root / "Data/Scripts/Library/SkirmishAICommands.lua", kAICommands);
    write_file(root / "Data/Scripts/AI/SpaceMode/SkirmishDestroy.lua", kPlanDestroy);
    write_file(root / "Data/Scripts/AI/SpaceMode/SkirmishSweep.lua", kPlanSweep);
    write_file(root / "Data/Scripts/FreeStore/SkirmishFreeStore.lua", kFreeStore);
    s.files = std::make_unique<DirectoryProvider>(path_to_utf8(root));
    if (!load_game(s, diag)) return false;
    s.defs.add_goals(kGoals, "goals.xml");
    s.defs.link();
    s.defs.add_function_set("SkirmishSpaceSet", kSet, "SkirmishSpaceSet.xml");
    s.defs.add_templates(kTemplates, "templates.xml");
    char buf[1024];
    for (const char* n : {"BasicEmpire", "BasicRebel"}) {
        std::snprintf(buf, sizeof buf, kPlayer, n);
        s.defs.add_players(buf, "players.xml");
    }
    s.eqs.add("One", "1.0");
    s.eqs.add("Skirmish_Destroy_Desire", "2 - Variable_Target.Health");
    s.eqs.add("Skirmish_Sweep_Desire", "(Variable_Target.EnemyForceUnnormalized > 0) * 0.5");
    s.eqs.link();
    s.ai = std::make_shared<sim::TacticalAiSetup>();
    s.ai->space_plans = {"Data\\Scripts\\AI\\SpaceMode\\SkirmishDestroy.lua", "Data\\Scripts\\AI\\SpaceMode\\SkirmishSweep.lua"};
    s.ai->difficulties = {{"Normal_Default", 0.0f, 0.0f, 1.0f, 1.0f}};
    s.ai->constants.load(s.db.constants());  // a2h t05 (the synthetic GameConstants has no tactical AI tag: the defaults)
    auto t =[&s](const char* n) { return s.rules.find_type(n); };
    // Defender: frigate + 2 corvettes + starbase (+ reserves over the cap 6); attacker: 3 frigates + 2 corvettes.
    s.setup = make_galaxy(s, t("A1S_B"), {t("A1S_Frigate"), t("A1S_Corvette"), t("A1S_Corvette"), t("A1S_Star_Base_1"), t("A1S_Frigate"), t("A1S_Frigate")},
                          {t("A1S_Frigate"), t("A1S_Frigate"), t("A1S_Frigate"), t("A1S_Corvette"), t("A1S_Corvette")});
    s.map = "A1S_Space_B";
    return true;
}

// The FoC data (+ MODPATH=) through the VFS: game data, AI definitions, equations, tactical plans and difficulties.
bool foc_load(Session& s, const MapRunOptions& opt, std::vector<std::string>& diag, bool& skipped) {
    const std::string root = opt.game_dir.empty() ? env_value("ALAMO_FOC_DATA") : opt.game_dir;
    std::error_code ec;
    if (root.empty() || !fs::is_directory(path_from_utf8(root) / "Data", ec)) {
        skipped = true;
        return false;
    }
    std::string err;
    std::unique_ptr<vfs::Vfs> v = vfs::mount_game(root, opt.game_args, &err);
    if (!v) {
        diag.push_back(err);
        return false;
    }
    vfs::Vfs* vp = v.get();
    s.files = std::move(v);
    if (!load_game(s, diag)) return false;
    const auto list = [vp](std::string_view glob) { return vp->list(glob); };
    if (!ai::load_ai_definitions(*s.files, ai::list_ai_data(list), s.defs)) return false;
    ai::load_equation_files(*s.files, vp->list(ai::kPerceptionEquationGlob), s.eqs);
    s.ai = std::make_shared<sim::TacticalAiSetup>();
    sim::load_tactical_ai_data(*s.ai, *s.files, &s.db.constants(), list);  // a2h t05: plans, difficulties, GameConstants
    return true;
}

// BM1 t02: --skirmish-ai foc-fleet / --save-session skirmish --campaign foc-fleet, the foc space battle with both fleets this many
// times over (the benchmark's large space battle, tests/benchmark/space_battle.abench).
constexpr int kFleetScale = 4;

// land: a land battle; land_skirmish (t50): that battle is a skirmish (an _MP_LAND_ map, MP_Default_Credits), else the campaign invasion.
// fleet_scale (BM1 t02, kind foc-fleet): the space fleets of both sides repeated that many times (the benchmark's large space battle).
bool foc_session(Session& s, const MapRunOptions& opt, std::vector<std::string>& diag, bool& skipped, bool land, bool station = false,
                 bool land_skirmish = false, int fleet_scale = 1) {
    if (!foc_load(s, opt, diag, skipped)) return false;
    // [name-ok-begin: the foc-* battle scenarios of the test host (kinds named for FoC data; their goldens)]
    std::vector<uint32_t> emp, reb;
    if (land) {  // battle host: --skirmish-ai foc-land - Empire companies hold the planet, Rebel companies invade
        s.land = true;
        s.skirmish = land_skirmish;
        for (const char* n : {"Imperial_Stormtrooper_Squad", "Imperial_Armor_Group", "Imperial_Heavy_Assault_Company", "Imperial_Anti_Infantry_Brigade"})
            if (const uint32_t ty = s.rules.find_type(n); ty != galactic::kNoIndex) emp.push_back(ty);
        for (const char* n : {"Rebel_Infantry_Squad", "Rebel_Light_Tank_Brigade", "Rebel_Heavy_Tank_Brigade", "Rebel_Infantry_Squad"})
            if (const uint32_t ty = s.rules.find_type(n); ty != galactic::kNoIndex) reb.push_back(ty);
    } else if (station) {  // t41: --skirmish-ai foc-station - a Level 2 station and one escort hold against a far larger fleet
        s.skirmish = false;
        for (const char* n : {"Empire_Star_Base_2", "Tartan_Patrol_Cruiser"})
            if (const uint32_t ty = s.rules.find_type(n); ty != galactic::kNoIndex) emp.push_back(ty);
        for (int i = 0; i < 12; ++i)
            for (const char* n : {"Calamari_Cruiser", "Nebulon_B_Frigate", "Alliance_Assault_Frigate"})
                if (const uint32_t ty = s.rules.find_type(n); ty != galactic::kNoIndex) reb.push_back(ty);
    } else {
        for (int i = 0; i < fleet_scale; ++i) {
            for (const char* n : {"Star_Destroyer", "Tartan_Patrol_Cruiser", "Tartan_Patrol_Cruiser", "Acclamator_Assault_Ship", "Broadside_Class_Cruiser"})
                if (const uint32_t ty = s.rules.find_type(n); ty != galactic::kNoIndex) emp.push_back(ty);
            for (const char* n : {"Calamari_Cruiser", "Nebulon_B_Frigate", "Corellian_Corvette", "Corellian_Corvette", "Alliance_Assault_Frigate"})
                if (const uint32_t ty = s.rules.find_type(n); ty != galactic::kNoIndex) reb.push_back(ty);
        }
    }
    // [name-ok-end]
    uint32_t planet = s.rules.find_type(land ? "Utapau" : "Kuat");
    if (planet == galactic::kNoIndex) planet = s.rules.find_type("Coruscant");
    if (emp.empty() || reb.empty() || planet == galactic::kNoIndex) {
        diag.push_back("FoC skirmish: unit / planet types not found");
        return false;
    }
    const float credits = land && land_skirmish ? static_cast<float>(std::max(0, s.db.constants().get_int("MP_Default_Credits", 0))) : 0.0f;
    s.setup = make_galaxy(s, planet, emp, reb, credits);
    // S10 skirmish follow-up: foc-land-campaign = Utapau's Land_Tactical_Map (a campaign map: Defending Forces + Attacker Entry
    // markers, REMAINING_GAPS §3.2). t50: foc-land is a skirmish on _MP_LAND_UTAPAU (explicit faction pads and Player_N_Spawn_Point_Marker).
    s.map = opt.map.empty() ? std::string(!land ? "_MP_SPACE_KUAT" : land_skirmish ? "_MP_LAND_UTAPAU" : "_Land_Planet_Utapau_01") : opt.map;
    return true;
}

std::string trimmed(std::string_view v) { return std::string(data::trim(v)); }  // data::trim = the std::isspace set

// ---- q6 t02 / t03: the skirmish host's data rules for any faction (mods through MODPATH=) ----
// engine/apps/game/rules (skirmish_rules.hpp), shared with alamo_game. This host's deliberate differences are options:
// no base-game lists (it has always started every faction from the data's Space_ / Land_Skirmish_AI_Default_Forces, which
// the checked-in goldens hold) and no hyperspace leader (the battle starts on the map, no galactic hop).

// A slot's starting forces (skirmish_rules skirmish_data_force_types): the faction's list, keeping the types the scaffold
// galaxy can hold (unknown names dropped); none left: [port] the faction's cheapest buildable units. Notes to `diag`.
// Both slots get them (MP_Default_Free_Starting_Units yes): the retail Geonosis trace starts the idle human Empire with 2 TIE
// Interceptor squadrons too.
std::vector<uint32_t> skirmish_slot_forces(const Session& s, const game::SkirmishForceTable& table, const std::string& faction, bool land,
                                           std::vector<std::string>& diag) {
    std::vector<std::string> notes;
    game::SkirmishForceOptions opt;
    opt.hyperspace_leader = false;
    opt.diag = &notes;
    std::vector<uint32_t> out = game::skirmish_data_force_types({s.rules, s.types, table}, faction, land, opt);
    for (const std::string& n : notes) diag.push_back("skirmish host: " + n);
    return out;
}

// The scaffold galaxy's planet type: Utapau, then Coruscant (the stock picks the goldens hold), then the map's planet
// (skirmish_rules skirmish_map_planet_type), then the first planet type.
uint32_t skirmish_host_planet(const Session& s) {
    const galactic::GalacticRules& rules = s.rules;
    uint32_t p = rules.find_type("Utapau");
    if (p == galactic::kNoIndex) p = rules.find_type("Coruscant");
    if (p == galactic::kNoIndex)
        if (const std::string m = game::skirmish_map_planet_type(rules, s.map); !m.empty()) p = rules.find_type(m);
    for (uint32_t t = 0; t < rules.types.size() && p == galactic::kNoIndex; ++t)
        if (rules.types[t].kind == galactic::UnitKind::Planet) p = t;
    return p;
}

std::vector<std::string> split_factions(std::string_view v) {
    std::vector<std::string> out;
    size_t at = 0;
    while (at <= v.size()) {
        size_t end = v.find(',', at);
        if (end == std::string_view::npos) end = v.size();
        if (std::string n = trimmed(v.substr(at, end - at)); !n.empty()) out.push_back(std::move(n));
        at = end + 1;
    }
    return out;
}

// skai (q2f t04): the skirmish host. foc-land: _MP_LAND_UTAPAU, both slots AI; skirmish: --map (default _MP_SPACE_GEONOSIS),
// --human FACTION makes that slot an idle human. Slot 0 = player 0, slot 1 = player 1: Empire / Rebel by default (the retail
// trace's owners), q6 t02: --ai FACTION[,FACTION] (MapRunOptions::ai_factions) for any factions of the data. MP_Default_Credits
// each; the starting forces (skirmish_slot_forces) wait at planets 0 / 2 of the scaffold galaxy (no conflict) and become the
// battle's starting units; the base, pads and stations come from the map's team markers. Each AI slot plays
// game::resolve_ai_type (skirmish: no campaign pairs).
bool skirmish_host_session(Session& s, const MapRunOptions& opt, std::vector<std::string>& diag, bool& skipped, const std::string& kind) {
    MapRunOptions o = opt;
    if (o.map.empty()) o.map = kind == "foc-land" ? "_MP_LAND_UTAPAU" : "_MP_SPACE_GEONOSIS";
    if (!foc_load(s, o, diag, skipped)) return false;
    s.map = o.map;
    terrain::MapData probe;
    std::string map_err;
    if (!terrain::load_map(*s.files, s.map, probe, &map_err)) {
        diag.push_back("skirmish host: map " + s.map + " not loaded (" + map_err + ")");
        return false;
    }
    s.land = !probe.is_space();
    s.skirmish = true;
    s.map_host = true;
    // Slots: Empire, Rebel; --ai A,B: A, B; --human H --ai A: H, A.
    std::vector<std::string> names = split_factions(o.ai_factions);
    if (names.empty()) names = {"Empire", "Rebel"};  // [name-ok: default sides of the retail Geonosis trace (skirmish_ai goldens); --ai picks any]
    else if (names.size() == 1 && !o.human.empty()) names.insert(names.begin(), o.human);
    if (names.size() != 2) {
        diag.push_back("skirmish host: --ai " + o.ai_factions + ": name two factions, or one beside --human");
        return false;
    }
    std::vector<uint32_t> slots;
    for (const std::string& n : names) {
        const uint32_t f = s.rules.find_faction(n);
        if (f == galactic::kNoIndex || s.rules.factions[f].is_neutral) {
            diag.push_back("skirmish host: faction " + n + " unknown or neutral");
            return false;
        }
        slots.push_back(f);
    }
    if (slots[0] == slots[1]) {
        diag.push_back("skirmish host: both slots are " + names[0]);
        return false;
    }
    const uint32_t planet = skirmish_host_planet(s);
    if (planet == galactic::kNoIndex) {
        diag.push_back("skirmish host: the data has no planet type");
        return false;
    }
    const float credits = static_cast<float>(std::max(0, s.db.constants().get_int("MP_Default_Credits", 0)));
    s.setup = make_galaxy(s, planet, {}, {}, credits, slots);
    auto basic_ai = std::make_shared<const std::map<std::string, std::string>>(ai::load_faction_basic_ai(s.rules.factions));
    const game::SkirmishForceTable force_table = game::load_skirmish_forces(s.rules.factions);
    for (int32_t p = 0; p < 2; ++p) {
        galactic::PlayerSetup& ps = s.setup.players[static_cast<size_t>(p)];
        const std::string& faction = s.rules.factions[ps.faction].name;
        if (!o.human.empty() && alamo::ascii_iequals(faction, o.human)) {
            ps.human = true;
            ps.ai = false;
            s.human = p;
        }
        const std::vector<uint32_t> forces = skirmish_slot_forces(s, force_table, faction, s.land, diag);
        if (forces.empty()) {
            diag.push_back("skirmish host: " + faction + " has no starting forces");
            return false;
        }
        std::string line = "skirmish host: slot " + std::to_string(p) + " " + faction + " (" +
                           (ps.human ? std::string("idle human") : "AI " + game::resolve_ai_type(faction, *basic_ai, nullptr, &s.defs)) + "), starts with";
        for (const uint32_t ty : forces) {
            s.setup.forces.push_back({p, static_cast<PlanetIndex>(p == 0 ? 0 : 2), ty});
            line += " " + s.rules.types[ty].name;
        }
        diag.push_back(line);
    }
    if (!o.human.empty() && s.human < 0) {
        diag.push_back("skirmish host: --human " + o.human + " is neither slot's faction (" + names[0] + ", " + names[1] + ")");
        return false;
    }
    diag.push_back("skirmish host: galaxy planet type " + s.rules.types[planet].name + ", map " + s.map);
    const ai::AiDefinitions* defs = &s.defs;
    s.ai->ai_type = [basic_ai, defs](int32_t player, const galactic::GalacticSim& g) -> std::string {
        if (player < 0 || static_cast<size_t>(player) >= g.players().size()) return {};
        const galactic::PlayerState& ps = g.players()[static_cast<size_t>(player)];
        if (ps.human || ps.faction >= g.rules().factions.size() || g.rules().factions[ps.faction].is_neutral) return {};
        return game::resolve_ai_type(g.rules().factions[ps.faction].name, *basic_ai, nullptr, defs);
    };
    return true;
}

// skai: --ai-log diagnostics (never hashed, no sim reads). The map's placements by type and owner.
std::string map_objects_text(const objects::GameObjectTypes& types, const terrain::MapData& map) {
    const sim::TypeCrcIndex index(types);
    std::map<std::pair<uint32_t, std::string>, uint32_t> n;
    for (const terrain::ObjectPlacement& pl : map.objects) {
        const objects::TypeIndex t = index.find(pl.type_crc);
        n[{pl.owner, t == objects::kNone ? std::string("?") : std::string(types.raw().types().name[t])}]++;
    }
    std::string out = "map objects (owner type count):\n";
    for (const auto& [k, c] : n) out += "  " + std::to_string(k.first) + " " + k.second + " " + std::to_string(c) + "\n";
    return out;
}

// skai: one state line of a running battle: credits, live objects and build pads per player (pad: type owner contents).
std::string battle_state_text(uint64_t tick, sim::SimHost& host, const galactic::GalacticSim& gs, const sim::TacticalBattle& tb) {
    sim::TacticalMode* tm = host.tactical();
    if (!tm) return {};
    const objects::ObjectStore& st = tm->primary();
    const objects::GameObjectTypes& types = tm->types();
    static const objects::BehaviorTypeId kPad = objects::behavior_type_id("TACTICAL_BUILD_OBJECTS");
    std::string out = "state t=" + std::to_string(tick) + " battle frame " + std::to_string(tb.stats().frames) + ":";
    char buf[96];
    for (int32_t p = 0; p < static_cast<int32_t>(gs.players().size()); ++p) {
        uint32_t live = 0;
        for (const objects::ObjectHandle h : st.owned_by(p))
            if (st.active(h)) ++live;
        std::snprintf(buf, sizeof buf, " p%d credits %.1f objects %u;", p, static_cast<double>(gs.credits(p)), live);
        out += buf;
    }
    out += "\n  pads:";
    for (const objects::ObjectHandle h : st.all_objects()) {
        if (!st.alive(h) || !st.has_behavior(h, kPad) || st.type(h) >= types.size()) continue;
        const objects::ObjectId c = tb.pad_contents(st.id(h));
        const objects::ObjectHandle ch = c != objects::kNoObjectId ? st.find(c) : objects::ObjectHandle{};
        out += " " + std::to_string(st.id(h)) + ":" + std::string(types.raw().types().name[st.type(h)]) + "/p" + std::to_string(st.owner(h));
        if (st.alive(ch) && st.type(ch) < types.size()) out += "=" + std::string(types.raw().types().name[st.type(ch)]);
    }
    out += "\n  units:";
    for (int32_t p = 0; p < 2; ++p) {
        std::map<std::string, uint32_t> n;
        for (const objects::ObjectHandle h : st.owned_by(p))
            if (st.active(h) && st.type(h) < types.size() && !st.has_behavior(h, kPad)) n[std::string(types.raw().types().name[st.type(h)])]++;
        out += " p" + std::to_string(p) + "{";
        for (const auto& [k, c] : n) out += k + "x" + std::to_string(c) + " ";
        out += "}";
    }
    // q6: the land-victory blockers (DUMMY_LAND_BASE_LEVEL_COMPONENT objects of the slot players): id, type, hull, shield,
    // the enemy units that have it as their current target and the nearest enemy battle unit's distance.
    static const objects::BehaviorTypeId kLandBaseBeh = objects::behavior_type_id("DUMMY_LAND_BASE_LEVEL_COMPONENT");
    if (kLandBaseBeh != objects::kNoBehavior) {
        const auto& c = tb.combat();
        out += "\n  combat shots " + std::to_string(c.stats().shots) + " hits " + std::to_string(c.stats().hits) + " expired " +
               std::to_string(c.stats().expired) + "\n  bases:";
        for (const objects::ObjectHandle h : st.all_objects()) {
            if (!st.active(h) || !st.has_behavior(h, kLandBaseBeh) || st.owner(h) < 0 || st.owner(h) > 1 || st.type(h) >= types.size()) continue;
            const objects::ObjectId id = st.id(h);
            const auto p = st.position(h);
            uint32_t attackers = 0, in_range = 0, firing = 0;
            float nearest = -1.0f;
            for (const objects::ObjectHandle e : st.owned_by(1 - st.owner(h))) {
                if (!st.active(e) || !c.has_unit(st.id(e))) continue;
                if (c.current_target(st.id(e)) == id) {
                    ++attackers;
                    const auto q = st.position(e);
                    const float d = std::sqrt((q.x - p.x) * (q.x - p.x) + (q.y - p.y) * (q.y - p.y));
                    if (d <= c.base_attack_range_of(st.id(e))) ++in_range;
                    if (c.attacking(st.id(e))) ++firing;
                }
                if (!st.has_behavior(e, objects::beh::Locomotor)) continue;
                const auto q = st.position(e);
                const float d = std::sqrt((q.x - p.x) * (q.x - p.x) + (q.y - p.y) * (q.y - p.y));
                if (nearest < 0.0f || d < nearest) nearest = d;
            }
            char b2[256];
            std::snprintf(b2, sizeof b2, " %u:%s/p%d hull %.0f/%.0f shield %.0f attackers %u (in range %u, attacking %u) nearest %.0f",
                          static_cast<unsigned>(id), std::string(types.raw().types().name[st.type(h)]).c_str(), st.owner(h),
                          static_cast<double>(c.has_unit(id) ? c.health(id) : -1.0f), static_cast<double>(c.has_unit(id) ? c.max_health(id) : -1.0f),
                          static_cast<double>(c.has_unit(id) ? c.shield(id) : -1.0f), attackers, in_range, firing, static_cast<double>(nearest));
            out += b2;
        }
    }
    return out + "\n";
}

// q6 land end (--ai-log only, nothing hashed): per AI slot player the free-store size, running plans and the values of
// the equations that gate the base attack (AI_TACTICAL §7.7.5): Is_Initial_Skirmish_State_Established and its inputs,
// Allowed_As_Defender_Land, and Should_Destroy_Structure against each enemy land-victory blocker.
std::string ai_state_text(sim::SimHost& host, const ai::tactical::TacticalAiWorld& w) {
    sim::TacticalMode* tm = host.tactical();
    if (!tm) return {};
    const objects::ObjectStore& st = tm->primary();
    static const objects::BehaviorTypeId kLandBaseBeh = objects::behavior_type_id("DUMMY_LAND_BASE_LEVEL_COMPONENT");
    static const char* const kEquations[] = {"Is_Initial_Skirmish_State_Established", "Number_Resource_Structures_Land",
                                             "Non_Structure_Force_Unnormalized", "Allowed_As_Defender_Land"};
    std::string out;
    char buf[160];
    for (int32_t p = 0; p < 2; ++p) {
        if (!w.is_ai_player(p)) continue;
        out += "  ai p" + std::to_string(p) + " holdings: " + w.diagnostic_holdings(p) + "\n";
        std::snprintf(buf, sizeof buf, "  ai p%d: free store %zu, plans %zu;", p, w.free_store_size(p), w.running_plans());
        out += buf;
        for (const char* e : kEquations) {
            double v = 0.0;
            out += std::string(" ") + e + (w.diagnostic_value(p, e, -1, v) ? "=" + std::to_string(v) : std::string("=n/a"));
        }
        out += "\n    Should_Destroy_Structure:";
        if (kLandBaseBeh != objects::kNoBehavior)
            for (const objects::ObjectHandle h : st.all_objects()) {
                if (!st.active(h) || !st.has_behavior(h, kLandBaseBeh) || st.owner(h) < 0 || st.owner(h) > 1 || st.owner(h) == p) continue;
                double v = 0.0;
                const bool ok = w.diagnostic_value(p, "Should_Destroy_Structure", st.id(h), v);
                std::snprintf(buf, sizeof buf, " %u=%s", static_cast<unsigned>(st.id(h)), ok ? std::to_string(v).c_str() : "n/a");
                out += buf;
            }
        out += "\n";
    }
    return out;
}

// a1j t07 (A1_READINGS §38): the battle AI's parent galaxy (Evaluate_In_Galactic_Context, Release_Credits_For_Tactical).
// These hosts run no galactic AI, so the resolver is a GalacticSimTokenResolver over the galaxy, refreshed at each battle
// start, with no hooks (an AI player's CreditsUnnormalized reads its credits, not a galactic budget) [port]; the galactic
// players with `ai` set count as having a galactic AI (CampaignBattleOptions::has_galactic_ai unset). The option keeps the
// resolver alive (shared_ptr in the callback).
void add_galactic_context([[maybe_unused]] const Session& s, [[maybe_unused]] const galactic::GalacticSim& gs,
                          [[maybe_unused]] sim::CampaignBattleOptions& bo) {
#ifdef ALAMO_HEADLESS_HAVE_GALACTIC_AI
    struct Context {
        std::vector<ai::GalacticAiTypeInfo> types;
        std::unique_ptr<ai::GalacticSimTokenResolver> res;
    };
    auto c = std::make_shared<Context>();
    c->types = ai::make_galactic_ai_type_info(s.types);
    c->res = std::make_unique<ai::GalacticSimTokenResolver>(gs, c->types, nullptr, ai::GalacticSimTokenResolver::Hooks{});
    const galactic::GalacticSim* g = &gs;
    bo.galactic_resolver = [c, g]() -> const ai::TokenResolver* {
        c->res->refresh(g->frame());
        return c->res.get();
    };
#endif
}

}  // namespace

int skirmish_ai_run(const MapRunOptions& opt, const std::string& kind, const std::string& ai_log) {
    Session s;
    std::vector<std::string> diag;
    bool skipped = false;
    const bool host = kind == "foc-land" || kind == "skirmish";  // skai: the skirmish host
    const bool foc = host || kind == "foc" || kind == "foc-land-campaign" || kind == "foc-station" || kind == "foc-fleet";
    const bool ok = host  ? skirmish_host_session(s, opt, diag, skipped, kind)
                    : foc ? foc_session(s, opt, diag, skipped, kind == "foc-land-campaign", kind == "foc-station", false,
                                        kind == "foc-fleet" ? kFleetScale : 1)
                          : synthetic_session(s, diag);
    if (skipped) {
        std::printf("skirmish-ai %s: ALAMO_FOC_DATA / --game not set: skipped\n", kind.c_str());
        return 0;
    }
    if (!ok) {
        std::fprintf(stderr, "skirmish-ai %s: setup failed\n", kind.c_str());
        for (const std::string& d : diag) std::fprintf(stderr, "  %s\n", d.c_str());
        return 2;
    }
    if (host && !opt.quiet)  // q6 t02: the slots, their AI types and starting forces (stdout only, nothing hashed)
        for (const std::string& d : diag)
            if (d.rfind("skirmish host: ", 0) == 0) std::printf("%s\n", d.c_str());
    terrain::MapData map;
    if (foc) {
        if (!terrain::load_map(*s.files, s.map, map) || map.is_space() == s.land) {
            std::fprintf(stderr, "skirmish-ai: %s map %s not loaded\n", s.land ? "land" : "space", s.map.c_str());
            return 2;
        }
    } else {
        map.space.present = true;
        for (int i = 0; i < 3; ++i) {
            map.space.playable_min[i] = -4000.0f;
            map.space.playable_max[i] = 4000.0f;
        }
        map.playable.present = map.playable.has_outer_box = true;
        for (int i = 0; i < 3; ++i) {
            map.playable.outer_min[i] = -4000.0f;
            map.playable.outer_max[i] = 4000.0f;
        }
    }
    s.ai->defs = &s.defs;
    s.ai->equations = &s.eqs;
    s.ai->files = s.files.get();
    if (opt.ai_latency >= 0) s.ai->path_latency = static_cast<uint32_t>(opt.ai_latency);  // OPT2 t01 / t02 parity knobs
    s.ai->stagger = opt.ai_stagger;
    s.ai->pp_streams = opt.pp_streams;  // OPT2 t10 parity knobs
    if (opt.pp_latency >= 0) s.ai->pp_latency = static_cast<uint32_t>(opt.pp_latency);
    const uint64_t max_ticks = opt.ticks_set ? opt.ticks : 30 * 600;
    struct Out {
        CrcLog crc;
        std::string text;
        uint64_t ticks = 0;
        size_t lua_errors = 0;
        // S10 land outcome: from the battle summary (TacticalBattle::summary) - battle frames, winner faction, victory.
        bool ended = false, starbase_lost = false;
        unsigned long battle_frames = 0;
        std::string winner, victory;
        // t50 (land skirmish): the battle's BattleStats as last seen while it ran (the summary only exists once it has ended).
        unsigned long prebuilt = 0, pads = 0, structures = 0, shots = 0;
        uint64_t orders = 0;
    };
    constexpr uint32_t kBattleFrames = 30 * 480;  // [port] the 15-min battle limit (S10)
    std::string log_text;
    auto run_one = [&](int32_t workers, Out& out) -> bool {
        jobs::JobSystem js(jobs::JobSystemConfig{workers});
        sim::SimConfig cfg;
        cfg.seed = opt.seed;
        cfg.players = 4;
        cfg.object_scripts = opt.object_scripts;  // q6
        sim::SimHost host(s.types, js, cfg);
        if (opt.object_scripts) {  // q6: game-object scripts, and the GameScoring script that sets the mod's GlobalValues
            host.set_object_script_files(s.files.get());
            if (const auto scoring = s.db.constants().get("Game_Scoring_Script_Name"))
                host.set_scoring_script(s.files.get(), std::string(*scoring));
        }
        galactic::GalacticSim* gs = host.start_galactic(s.rules, s.setup);
        if (!gs) return false;
        sim::CampaignBattleOptions bo;
        bo.conflict.human_choice = galactic::BattleChoice::Fight;
        bo.conflict.fight_ai_battles = true;
        bo.battle.max_frames = s.map_host ? 0 : kBattleFrames;  // skai: a skirmish has no time limit (it ends at --ticks)
        bo.battle.skirmish = s.skirmish;
        bo.battle.skirmish_map = s.map_host;        // skai: team markers, income, skirmish victory
        bo.battle.skip_human_units = s.human >= 0;  // skai: the idle human gets no stand-in orders
        bo.tactical_ai = s.ai;
        sim::load_tactical_systems(*s.files, s.rules, &s.db.constants(), bo);  // Int-final: weather, retreat, strikes data
        add_galactic_context(s, *gs, bo);  // a1j t07
        const terrain::MapData* mp = &map;
        sim::CampaignBattles battles(host, s.crules, *s.catalog,
                                     [mp](const std::string&, terrain::MapData& m) {
                                         m = *mp;
                                         return true;
                                     },
                                     bo);
        std::vector<std::string> d;
        if (!battles.attach(&d)) return false;
        bool invaded = !s.land;
        if (s.map_host) {  // skai: the skirmish starts on its map; each slot's default forces are its starting units
            galactic::BattleRequest r;
            r.info.kind = s.land ? galactic::ConflictKind::Land : galactic::ConflictKind::Space;
            r.info.planet = 1;
            r.info.defender = 0;
            r.info.attacker = 1;
            r.info.map = r.map = s.map;
            for (int32_t p = 0; p < 2; ++p) {
                galactic::BattleForces f;
                f.player = p;
                const PlanetIndex at = static_cast<PlanetIndex>(p == 0 ? 0 : 2);
                if (s.land) {
                    for (const objects::ObjectHandle h : gs->planets()[at].surface)
                        if (gs->store().alive(h) && gs->store().owner(h) == p) f.start.push_back(h);
                } else {
                    for (const uint32_t fi : gs->fleets_at(at, p))
                        for (const objects::ObjectHandle h : gs->fleets()[fi].contents) f.start.push_back(h);
                }
                r.forces.push_back(std::move(f));
            }
            if (!battles.start_map_battle(std::move(r))) return false;
            invaded = true;
        } else {
            if (s.land) {  // battle host: the invaders lift off from C (Launch_Units into an orbit fleet), then fly to B and invade
                const std::vector<objects::ObjectHandle> up = gs->planets()[2].surface;
                if (gs->launch_units(1, 2, up) != commands::RejectReason::None) return false;
            }
            const std::vector<uint32_t> fleets = gs->fleets_at(2, 1);
            if (fleets.empty() || gs->move_fleet(1, fleets[0], 1) != commands::RejectReason::None) return false;
        }
        const auto t0 = std::chrono::steady_clock::now();
        uint64_t t = 0;
        uint64_t orders = 0, rejected = 0;
        size_t decisions = 0, lua_errors = 0;
        unsigned long prebuilt = 0, pads = 0, structures = 0, shots = 0;
        std::vector<std::string> error_list;
        std::string state_text;  // skai: --ai-log state lines
        std::string script_state;  // q6: --object-scripts, the battle's last state line (units per player)
        // q6: --ai-log diagnostics, damage taken by the slot players' DUMMY_LAND_BASE_LEVEL_COMPONENT objects since the last
        // state line, per victim and source type (never hashed, no sim reads).
        std::map<std::pair<objects::ObjectId, std::string>, std::pair<uint32_t, double>> base_damage;
        bool credits_set = !s.map_host;
        for (; t < max_ticks; ++t) {
            host.tick_once();
            if (!credits_set && battles.battle()) {  // skai: the skirmish starts at MP_Default_Credits (the scaffold galaxy's
                credits_set = true;                  // first tick paid it income before the battle was pushed)
                for (int32_t p = 0; p < 2; ++p) gs->add_credits(p, s.setup.players[static_cast<size_t>(p)].credits - gs->credits(p), false);
            }
            if (!invaded && !gs->fleets_at(1, 1).empty())  // battle host: arrived at B - invade (a land conflict)
                invaded = gs->invade(1, 1, {}) == commands::RejectReason::None;
            if (const sim::BattleAi* a = battles.tactical_ai()) {
                orders = a->world().orders_issued();
                rejected = a->world().orders_rejected();
                decisions = a->world().decision_log().size();
                error_list = a->world().lua_errors();
                lua_errors = error_list.size();
            }
            if (const sim::TacticalBattle* tb = battles.battle()) {
                const sim::BattleStats& bs = tb->stats();
                prebuilt = bs.skirmish_prebuilt;
                pads = bs.skirmish_pads;
                structures = bs.pad_builds;
                shots = static_cast<unsigned long>(bs.shots);
                if (!ai_log.empty())
                    if (sim::TacticalMode* tmode = host.tactical()) {
                        static const objects::BehaviorTypeId kLandBaseDmg = objects::behavior_type_id("DUMMY_LAND_BASE_LEVEL_COMPONENT");
                        const objects::ObjectStore& bst = tmode->primary();
                        for (const auto& e : tb->combat().damage_events()) {
                            const objects::ObjectHandle vh = bst.find(e.victim);
                            if (!bst.exists(vh) || !bst.has_behavior(vh, kLandBaseDmg)) continue;
                            const objects::ObjectHandle sh = bst.find(e.source);
                            const std::string src = bst.exists(sh) && bst.type(sh) < tmode->types().size()
                                                        ? std::string(tmode->types().raw().types().name[bst.type(sh)])
                                                        : std::string("?");
                            auto& acc = base_damage[{e.victim, src + "/k" + std::to_string(e.kind)}];
                            ++acc.first;
                            acc.second += static_cast<double>(e.hull_lost + e.absorbed);
                        }
                    }
                if (!ai_log.empty() && (bs.frames == 1 || bs.frames % 600 == 0)) {
                    state_text += battle_state_text(t + 1, host, *gs, *tb);
                    if (const sim::BattleAi* a = battles.tactical_ai()) state_text += ai_state_text(host, a->world());
                    if (!base_damage.empty()) {
                        state_text += "  base damage:";
                        for (const auto& [k, v] : base_damage)
                            state_text += " " + std::to_string(k.first) + "<-" + k.second + " x" + std::to_string(v.first) + " = " +
                                          std::to_string(static_cast<int>(v.second));
                        state_text += "\n";
                        base_damage.clear();
                    }
                }
                if (opt.object_scripts && (bs.frames == 1 || bs.frames % 600 == 0)) script_state = battle_state_text(t + 1, host, *gs, *tb);
            }
            if (battles.battles_fought() > 0 && !battles.battle() && host.modes().depth() == 1) {
                ++t;
                break;
            }
        }
        out.crc = host.crc_log();
        out.ticks = t;
        char buf[512];
        std::snprintf(buf, sizeof buf, "skirmish-ai %s workers %d: %" PRIu64 " ticks (%.0f ms), AI orders %" PRIu64 " rejected %" PRIu64
                                       ", %zu decisions, %zu Lua errors, final CRC %016" PRIx64 "\n",
                      kind.c_str(), workers, t, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(),
                      orders, rejected, decisions, lua_errors, out.crc.records.empty() ? 0 : out.crc.records.back().combined);
        out.text = buf;
        out.lua_errors = lua_errors;
        out.orders = orders;
        out.prebuilt = prebuilt;
        out.pads = pads;
        out.structures = structures;
        out.shots = shots;
        for (const std::string& e : error_list) out.text += "  Lua error: " + e + "\n";
        if (const sim::ObjectScriptHost* os = host.object_scripts()) {  // q6: diagnostics only (nothing hashed)
            const sim::ObjectScriptHost::Stats& st = os->stats();
            std::snprintf(buf, sizeof buf,
                          "  object scripts: %" PRIu64 " attached, %" PRIu64 " load failures, %" PRIu64 " pumps, %" PRIu64
                          " first-pump draws, released %" PRIu64 " exit / %" PRIu64 " dead / %" PRIu64 " mode end, %" PRIu64
                          " thread errors, %zu live, %" PRIu64 " errors\n",
                          st.attached, st.load_failures, st.pumps, st.first_draws, st.released_exit, st.released_dead,
                          st.released_mode, st.thread_errors, os->live(), os->error_total());
            out.text += buf;
            {  // q6 t19: the §1.4 pools (name size/in use) and the reuse counts
                std::string pl = "  object script pools:";
                for (const sim::ObjectScriptHost::PoolInfo& p : os->pools())
                    pl += " " + p.name + " " + std::to_string(p.size) + "/" + std::to_string(p.in_use);
                pl += "; loaded " + std::to_string(st.loaded) + ", reused " + std::to_string(st.reused) + ", pooled releases " +
                      std::to_string(st.pooled_releases) + ", type instances " + std::to_string(os->type_instances()) + "\n";
                out.text += pl;
            }
            for (const auto& [text, n] : os->error_counts()) out.text += "  object script error x" + std::to_string(n) + ": " + text + "\n";
            if (!opt.quiet) {
                size_t shown_m = 0;
                for (const std::string& m : os->messages())
                    if (shown_m++ < 40) out.text += "  object script: " + m + "\n";
            }
            if (!script_state.empty()) out.text += "  last " + script_state;
            for (const std::string& m : host.scoring_messages()) out.text += "  scoring: " + m + "\n";
        }
        for (const std::string& l : battles.log()) {
            out.text += "  " + l + "\n";
            const size_t c = l.find(": "), w = l.find(", winner "), v = l.find(", victory ");
            if ((l.rfind("land battle ", 0) == 0 || l.rfind("space battle ", 0) == 0) && c != std::string::npos &&
                w != std::string::npos && v != std::string::npos) {
                out.ended = true;
                out.starbase_lost = l.find("+ starbase") != std::string::npos;
                out.battle_frames = std::strtoul(l.c_str() + c + 2, nullptr, 10);
                out.winner = l.substr(w + 9, v - (w + 9));
                out.victory = l.substr(v + 10, l.find(',', v + 10) - (v + 10));
            }
        }
        const bool first_run = log_text.empty();
        if (first_run)
            for (const std::string& l : battles.ai_log()) log_text += l + "\n";
        // skai: a battle still running at the tick limit has no ai_log yet; --ai-log then takes the live AI's decisions and stats.
        if (const sim::BattleAi* a = battles.tactical_ai(); a && !ai_log.empty() && log_text.empty()) {
            log_text = "(battle running at tick " + std::to_string(t) + ") " + a->summary() + "\n";
            for (const ai::tactical::TacticalAiDecision& dec : a->world().decision_log())
                log_text += "  " + std::to_string(dec.frame) + " p" + std::to_string(dec.player) + " " + dec.text + "\n";
            for (const auto& [k, n] : a->world().stats()) log_text += "  stat " + k + " " + std::to_string(n) + "\n";
        }
        if (first_run && !ai_log.empty()) log_text += (foc ? map_objects_text(s.types, map) : std::string()) + state_text;
        size_t shown = 0;
        for (const std::string& l : battles.ai_log())
            if (shown++ < 12) out.text += "  " + l + "\n";
        return true;
    };
    Out first;
    const auto counts = jobs::determinism_worker_counts();
    if (!run_one(opt.check_workers ? counts.front() : opt.workers, first)) {
        std::fprintf(stderr, "skirmish-ai: run failed\n");
        return 1;
    }
    std::printf("%s", first.text.c_str());
    if (!ai_log.empty()) std::ofstream(fs::path(ai_log), std::ios::binary) << log_text;
    bool good = true;
    if (opt.check_workers)
        good = jobs::check_worker_counts(
            first,
            [&](int32_t w) {
                Out other;
                run_one(w, other);
                return other;
            },
            [](const Out& o) -> const CrcLog& { return o.crc; },
            [&](int32_t w, const Out&, const CrcDivergence& dv) {
                std::printf("  workers %d vs %d: %s\n", counts.front(), w, dv.message.c_str());
                return true;
            },
            counts);
    // t48: shipped scripts against the port: a Lua error here is a port bug until shown otherwise. a1j t07: any kind with
    // --fail-on-lua-errors.
    if ((kind == "foc-station" || opt.fail_on_lua_errors) && first.lua_errors != 0) {
        std::printf("Lua errors: %zu (none allowed)\n", first.lua_errors);
        good = false;
    }
    if (opt.expect_battle_end) {  // S10 land outcome: a real result before the frame limit
        // q6 land end chain: a skirmish has no battle time limit (max_frames 0), so its limit is the run's tick count.
        const unsigned long limit = s.map_host ? static_cast<unsigned long>(max_ticks) : static_cast<unsigned long>(kBattleFrames);
        const bool ended_ok = first.ended && first.battle_frames < limit && first.winner != "-";
        std::printf("battle outcome: %s, winner %s, victory %s, %lu of %lu frames\n", ended_ok ? "ended" : "NOT ENDED",
                    first.winner.c_str(), first.victory.c_str(), first.battle_frames, limit);
        if (!ended_ok) good = false;
        // t41: the shipped Want_To_Retreat_From_Space keeps a defender holding a station in the fight, however outnumbered.
        // t48: once the station is dead BaseLevel is 0 and the survivors may leave.
        if (kind == "foc-station" && first.victory == "all-retreated" && !first.starbase_lost) good = false;
        if (s.land && !s.map_host) {  // [q6: the campaign-rules land battle only; a skirmish never proposes PreDefend] Script_ evaluators (AI_GALACTIC_2 §4.4): the goals gated by Script_TargetHasProperty /
                       // Script_GetDistanceToNearestWithProperty must be proposed in the land battle
            for (const char* fn : {"Need_To_Secure_Contestable_Land", "Need_To_Secure_Contestable_Escorted_Land",
                                   "Need_To_PreDefend_Structure"}) {
                const std::string key = std::string("  stat ~ proposed ") + fn + " ";
                unsigned long n = 0;
                for (size_t at = log_text.find(key); at != std::string::npos; at = log_text.find(key, at + 1))
                    n += std::strtoul(log_text.c_str() + at + key.size(), nullptr, 10);
                std::printf("  proposed %s: %lu\n", fn, n);
                if (n == 0) good = false;
            }
        }
    }
    if (opt.expect_skirmish_base) {  // t50: the land skirmish starts from its base and the shipped AI plays from it
        // Both slots' Command Center and Barracks pads start built (4), the AI issues orders, shots are fired, no Lua errors. Whether
        // the battle reaches a result in the frame limit is not asked here: the skirmish AI does not take the enemy base in 15 minutes.
        const bool base_ok = kind == "foc-land" && first.prebuilt >= 4 && first.pads >= first.prebuilt && first.shots > 0 && first.orders > 0 &&
                             first.lua_errors == 0;
        std::printf("skirmish base: %s (%lu pre-built of %lu pads, %lu structures started, %lu shots, %" PRIu64 " AI orders, %zu Lua errors)\n",
                    base_ok ? "ok" : "MISSING", first.prebuilt, first.pads, first.structures, first.shots, first.orders, first.lua_errors);
        if (!base_ok) good = false;
    }
    first.crc.comments = {"A1 skirmish-ai " + kind + " seed " + std::to_string(opt.seed) + " ticks " + std::to_string(first.ticks)};
    std::string err;
    if (!opt.crc_out.empty() && alamo_headless::save_crc_log(first.crc, opt.crc_out, &err)) std::printf("wrote %s\n", opt.crc_out.c_str());
    if (!opt.golden.empty() && alamo_headless::compare_golden(opt.golden, first.crc, good) != 0) return 2;
    std::printf(good ? "skirmish-ai run: OK\n" : "skirmish-ai run: FAILED\n");
    return good ? 0 : 1;
}

// --- N1 stage 2 (owner: n2-host): skirmish_run.hpp skirmish_save_* ---
struct SkirmishSaveData {
    Session s;
    std::string kind;
    terrain::MapData map;
    uint64_t fingerprint = 0;
};

std::shared_ptr<SkirmishSaveData> skirmish_save_load(const std::string& kind, const std::string& game_dir,
                                                     const std::vector<std::string>& game_args, const std::string& map,
                                                     bool& skipped, std::string& err) {
    auto d = std::make_shared<SkirmishSaveData>();
    d->kind = kind;
    Session& s = d->s;
    std::vector<std::string> diag;
    skipped = false;
    const bool foc = kind == "foc" || kind == "foc-land" || kind == "foc-land-campaign" || kind == "foc-fleet";
    if (!foc && kind != "synthetic") {
        err = "unknown skirmish '" + kind + "' (synthetic | foc | foc-fleet | foc-land | foc-land-campaign)";
        return nullptr;
    }
    MapRunOptions mo;
    mo.game_dir = game_dir;
    mo.game_args = game_args;
    mo.map = map;
    const bool ok = foc ? foc_session(s, mo, diag, skipped, kind == "foc-land" || kind == "foc-land-campaign", false, kind == "foc-land",
                                      kind == "foc-fleet" ? kFleetScale : 1)
                        : synthetic_session(s, diag);
    if (skipped) {
        err = "ALAMO_FOC_DATA / --game not set";
        return nullptr;
    }
    if (!ok) {
        err = "skirmish " + kind + ": setup failed (" + std::to_string(diag.size()) + " diagnostics" +
              (diag.empty() ? std::string() : ": " + diag.front()) + ")";
        return nullptr;
    }
    if (foc) {
        if (!terrain::load_map(*s.files, s.map, d->map) || d->map.is_space() == s.land) {
            err = "skirmish: " + std::string(s.land ? "land" : "space") + " map " + s.map + " not loaded";
            return nullptr;
        }
    } else {  // as skirmish_ai_run
        d->map.space.present = true;
        for (int i = 0; i < 3; ++i) {
            d->map.space.playable_min[i] = -4000.0f;
            d->map.space.playable_max[i] = 4000.0f;
        }
        d->map.playable.present = d->map.playable.has_outer_box = true;
        for (int i = 0; i < 3; ++i) {
            d->map.playable.outer_min[i] = -4000.0f;
            d->map.playable.outer_max[i] = 4000.0f;
        }
    }
    s.ai->defs = &s.defs;
    s.ai->equations = &s.eqs;
    s.ai->files = s.files.get();
    StateHasher h;  // the battle inputs beyond types / rules / galaxy
    h.str(kind);
    h.str(s.map);
    h.u32(static_cast<uint32_t>(s.ai->space_plans.size() + s.ai->land_plans.size()));
    h.u32(static_cast<uint32_t>(s.defs.goals.size()));
    h.u32(static_cast<uint32_t>(s.eqs.size()));
    d->fingerprint = sim::combine_fingerprints({sim::fingerprint_types(s.types), sim::fingerprint_galactic(s.rules, s.setup), h.digest()});
    return d;
}

const objects::GameObjectTypes& skirmish_save_types(const SkirmishSaveData& d) { return d.s.types; }
uint64_t skirmish_save_fingerprint(const SkirmishSaveData& d) { return d.fingerprint; }
std::string skirmish_save_map(const SkirmishSaveData& d) { return d.s.map; }

bool skirmish_save_start(const SkirmishSaveData& d, sim::SimHost& host, bool anim, SkirmishSaveRun& run, std::string& err) {
    const Session& s = d.s;
    galactic::GalacticSim* gs = host.start_galactic(s.rules, s.setup);
    if (!gs) {
        err = "skirmish: start_galactic failed";
        return false;
    }
    sim::CampaignBattleOptions bo;  // as skirmish_ai_run
    bo.conflict.human_choice = galactic::BattleChoice::Fight;
    bo.conflict.fight_ai_battles = true;
    bo.battle.max_frames = 30 * 480;
    bo.battle.skirmish = s.skirmish;
    bo.tactical_ai = s.ai;
    sim::load_tactical_systems(*s.files, s.rules, &s.db.constants(), bo);
    add_galactic_context(s, *gs, bo);  // a1j t07
#ifdef ALAMO_SIM_HAVE_ANIM
    if (anim) {
        auto a = std::make_shared<sim::SimAnimation>(s.types, *s.files);
        a->attach(host, true);
        bo.animation = a.get();
        run.anim = a;
    }
#else
    if (anim) {
        err = "skirmish: this build has no animation (engine/anim)";
        return false;
    }
#endif
    const terrain::MapData* mp = &d.map;
    run.battles = std::make_shared<sim::CampaignBattles>(host, s.crules, *s.catalog,
                                                         [mp](const std::string&, terrain::MapData& m) {
                                                             m = *mp;
                                                             return true;
                                                         },
                                                         bo);
    std::vector<std::string> ad;
    if (!run.battles->attach(&ad)) {
        err = "skirmish: CampaignBattles::attach failed";
        return false;
    }
    if (s.land) {
        const std::vector<objects::ObjectHandle> up = gs->planets()[2].surface;
        if (gs->launch_units(1, 2, up) != commands::RejectReason::None) {
            err = "skirmish: launch_units failed";
            return false;
        }
    }
    const std::vector<uint32_t> fleets = gs->fleets_at(2, 1);
    if (fleets.empty() || gs->move_fleet(1, fleets[0], 1) != commands::RejectReason::None) {
        err = "skirmish: move_fleet failed";
        return false;
    }
    auto invaded = std::make_shared<bool>(!s.land);
    run.after_tick = [gs, invaded] {
        if (!*invaded && !gs->fleets_at(1, 1).empty()) *invaded = gs->invade(1, 1, {}) == commands::RejectReason::None;
    };
    sim::SaveParticipant p;  // the harness state across a save: the invasion step done or not
    p.name = "skirmish";
    p.write = [invaded](ByteWriter& w) { w.boolean(*invaded); };
    p.read = [invaded](ByteReader& r) {
        const bool v = r.boolean();
        if (!r.ok()) return false;
        *invaded = v;
        return true;
    };
    host.add_save_participant(std::move(p));
    return true;
}
// --- end N1 stage 2 ---
#else
int skirmish_ai_run(const MapRunOptions&, const std::string&, const std::string&) {
    std::fprintf(stderr, "--skirmish-ai: this build has no tactical AI (engine/ai/tactical + engine/lua)\n");
    return 2;
}
// --- N1 stage 2 (owner: n2-host): no tactical AI in this build ---
struct SkirmishSaveData {
    alamo::objects::GameObjectTypes types;
};
std::shared_ptr<SkirmishSaveData> skirmish_save_load(const std::string&, const std::string&, const std::vector<std::string>&,
                                                     const std::string&, bool& skipped, std::string& err) {
    skipped = true;
    err = "this build has no tactical AI (engine/ai/tactical + engine/lua)";
    return nullptr;
}
const alamo::objects::GameObjectTypes& skirmish_save_types(const SkirmishSaveData& d) { return d.types; }
uint64_t skirmish_save_fingerprint(const SkirmishSaveData&) { return 0; }
std::string skirmish_save_map(const SkirmishSaveData&) { return {}; }
bool skirmish_save_start(const SkirmishSaveData&, alamo::sim::SimHost&, bool, SkirmishSaveRun&, std::string& err) {
    err = "this build has no tactical AI";
    return false;
}
#endif
