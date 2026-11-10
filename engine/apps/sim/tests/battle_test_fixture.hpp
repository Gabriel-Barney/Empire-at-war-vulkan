// Shared fixture of the alamo_sim_battle*_tests exes (test_battles*.cpp; split out of test_battles.cpp in WF1b t01):
// synthetic game data (types, hardpoints, projectiles, factions, contrast table), the in-memory maps, the 3-planet galaxy
// setup and run_campaign. Header-only (inline), included by one translation unit per exe; each exe sets g_temp_prefix
// before its first make_data so the exes never share a %TEMP% folder.
#pragma once

#include <algorithm>
#include <bit>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "alamo/core/bytes.hpp"  // FOW t01: fog save round trip
#include "alamo/core/file_provider.hpp"
#include "alamo/core/hash.hpp"  // S4: name_crc for map placements
#include "alamo/data/game_database.hpp"
#include "alamo/galactic/campaign.hpp"
#include "alamo/galactic/conflicts.hpp"
#include "alamo/galactic/galactic_rules.hpp"
#include "alamo/galactic/victory.hpp"  // story t11
#include "alamo/objects/game_object_types.hpp"
#include "alamo/objects/object_types.hpp"
#include "alamo/objects/behaviors.hpp"
#include "alamo/sim/campaign_battles.hpp"
#include "alamo/tactical/abilities/ability_types.hpp"  // s78j t03: SPRINT
#ifdef ALAMO_SIM_HAVE_TACTICAL_AI
#include "alamo/ai/ai_definitions.hpp"        // OI host: BattleAi order test
#include "alamo/ai/perception_equations.hpp"
#include "alamo/lua/lua_vm.hpp"  // refixf t03: Find_All_Objects_Of_Type from a battle VM
#include "alamo/lua/object_bridge.hpp"
#include "alamo/sim/battle_ai.hpp"
#endif
#include "alamo/sim/map_spawn.hpp"
#include "alamo/sim/sim_host.hpp"
#include "alamo/tactical/movement/ted_grid.hpp"  // S7 t07: SPACE_OBSTACLE footprints
#include "alamo/vfs/vfs.hpp"
#include "alamo/tactical/garrison/garrison_types.hpp"  // S4
#include "alamo/test/support.hpp"
#ifdef ALAMO_SIM_HAVE_ANIM
#include "alamo/sim/sim_anim.hpp"
#endif

using namespace alamo;
using namespace alamo::sim;
namespace fs = std::filesystem;

using ::alamo::test::g_fail;
inline bool g_regen = false;
// The temp folder prefix of make_data (one per test exe, so the exes can run in parallel under ctest -j).
inline const char* g_temp_prefix = "alamo_sim_battle_tests";

namespace battle_fixture {

using ::alamo::test::write_file;

inline const char* const kTypes = R"(<S10>
 <Planet Name="S10_A"><Behavior>PLANET</Behavior><Terrain>Temperate</Terrain><Planet_Credit_Value>10</Planet_Credit_Value></Planet>
 <Planet Name="S10_B"><Behavior>PLANET</Behavior><Terrain>Temperate</Terrain><Planet_Credit_Value>10</Planet_Credit_Value>
  <Space_Tactical_Map>S10_Space_B</Space_Tactical_Map><Land_Tactical_Map>S10_Land_B</Land_Tactical_Map></Planet>
 <Planet Name="S10_C"><Behavior>PLANET</Behavior><Terrain>Desert</Terrain><Planet_Credit_Value>10</Planet_Credit_Value></Planet>
 <Projectile Name="S10_Laser_Proj"><Max_Speed>40</Max_Speed><Projectile_Damage>150</Projectile_Damage>
  <Projectile_Max_Flight_Distance>2500</Projectile_Max_Flight_Distance><Damage_Type>Damage_Laser</Damage_Type></Projectile>
 <Projectile Name="S10_Blaster_Proj"><Max_Speed>30</Max_Speed><Projectile_Damage>8</Projectile_Damage>
  <Projectile_Max_Flight_Distance>600</Projectile_Max_Flight_Distance></Projectile>
 <SpaceUnit Name="S10_Frigate"><Victory_Relevant>Yes</Victory_Relevant>
  <Tactical_Health>900</Tactical_Health><Max_Speed>3.0</Max_Speed><Max_Rate_Of_Turn>2.0</Max_Rate_Of_Turn>
  <Hyperspace>Yes</Hyperspace><Hyperspace_Speed>1.5</Hyperspace_Speed><AI_Combat_Power>1000</AI_Combat_Power>
  <CategoryMask>Frigate</CategoryMask><Targeting_Max_Attack_Distance>1400</Targeting_Max_Attack_Distance>
  <Population_Value>2</Population_Value><Armor_Type>Armor_Ship</Armor_Type>
  <HardPoints>HP_S10_Laser_A, HP_S10_Laser_B</HardPoints>
  <Behavior>SELECTABLE, ABILITY_COUNTDOWN</Behavior><Collidable_By_Projectile_Living>Yes</Collidable_By_Projectile_Living><SpaceBehavior>STARSHIP_LOCOMOTOR</SpaceBehavior>
  <Custom_Soft_Footprint_Radius>60</Custom_Soft_Footprint_Radius><Damage_Hit_Particles>S10_Hit_FX</Damage_Hit_Particles>
  <Unit_Abilities_Data SubObjectList="Yes">
   <Unit_Ability><Type>POWER_TO_WEAPONS</Type><Expiration_Seconds>8</Expiration_Seconds><Recharge_Seconds>20</Recharge_Seconds>
    <Mod_Multiplier>WEAPON_DELAY_MULTIPLIER, 0.5</Mod_Multiplier><Mod_Multiplier>SPEED_MULTIPLIER, 0.5f</Mod_Multiplier></Unit_Ability>
  </Unit_Abilities_Data>
 </SpaceUnit>
 <Particle Name="S10_Hit_FX"><Space_Model_Name>P_S10_Hit.alo</Space_Model_Name><Particle_Lifetime_Frames>20</Particle_Lifetime_Frames></Particle>
 <SpaceUnit Name="S10_Corvette"><Variant_Of_Existing_Type>S10_Frigate</Variant_Of_Existing_Type>
  <Tactical_Health>400</Tactical_Health><Max_Speed>5.0</Max_Speed><AI_Combat_Power>400</AI_Combat_Power>
  <CategoryMask>Corvette</CategoryMask><Population_Value>1</Population_Value><HardPoints>HP_S10_Laser_B</HardPoints>
  <Custom_Soft_Footprint_Radius>30</Custom_Soft_Footprint_Radius></SpaceUnit>
 <StarBase Name="S10_Star_Base_1"><Victory_Relevant>Yes</Victory_Relevant><Tactical_Health>2500</Tactical_Health><Base_Level>1</Base_Level>
  <AI_Combat_Power>600</AI_Combat_Power><CategoryMask>Structure</CategoryMask><HardPoints>HP_S10_Laser_A</HardPoints>
  <Targeting_Max_Attack_Distance>1500</Targeting_Max_Attack_Distance><Behavior>SELECTABLE</Behavior><Collidable_By_Projectile_Living>Yes</Collidable_By_Projectile_Living></StarBase>
 <GroundInfantry Name="S10_Trooper"><Victory_Relevant>Yes</Victory_Relevant><Tactical_Health>100</Tactical_Health><Max_Speed>1.0</Max_Speed>
  <AI_Combat_Power>50</AI_Combat_Power><CategoryMask>Infantry</CategoryMask><Targeting_Max_Attack_Distance>250</Targeting_Max_Attack_Distance>
  <Projectile_Types>S10_Blaster_Proj</Projectile_Types><Projectile_Fire_Recharge_Seconds>1.0</Projectile_Fire_Recharge_Seconds>
  <Projectile_Fire_Pulse_Count>1</Projectile_Fire_Pulse_Count><MovementClass>Infantry</MovementClass>
  <Behavior>SELECTABLE, WEAPON</Behavior><Collidable_By_Projectile_Living>Yes</Collidable_By_Projectile_Living><LandBehavior>WALK_LOCOMOTOR</LandBehavior><Custom_Soft_Footprint_Radius>6</Custom_Soft_Footprint_Radius>
  <Custom_Hard_XExtent>6</Custom_Hard_XExtent><Custom_Hard_YExtent>6</Custom_Hard_YExtent>
 </GroundInfantry>
 <GroundCompany Name="S10_Squad"><Company_Units>S10_Trooper, S10_Trooper, S10_Trooper, S10_Trooper</Company_Units>
  <Hyperspace>Yes</Hyperspace><Hyperspace_Speed>1.5</Hyperspace_Speed><Population_Value>1</Population_Value></GroundCompany>
</S10>
)";

inline const char* const kHardpoints = R"(<HardPoints>
 <HardPoint Name="HP_S10_Laser_A"><Type>HARD_POINT_WEAPON_LASER</Type><Health>150</Health>
  <Is_Targetable>Yes</Is_Targetable><Is_Destroyable>Yes</Is_Destroyable>
  <Fire_Projectile_Type>S10_Laser_Proj</Fire_Projectile_Type><Fire_Min_Recharge_Seconds>2.0</Fire_Min_Recharge_Seconds>
  <Fire_Max_Recharge_Seconds>3.0</Fire_Max_Recharge_Seconds><Fire_Pulse_Count>2</Fire_Pulse_Count>
  <Fire_Pulse_Delay_Seconds>0.2</Fire_Pulse_Delay_Seconds><Fire_Range_Distance>1400</Fire_Range_Distance><Fire_Cone_Width>360</Fire_Cone_Width><Fire_Cone_Height>180</Fire_Cone_Height>
  <Fire_Inaccuracy_Distance>Corvette, 5.0</Fire_Inaccuracy_Distance></HardPoint>
 <HardPoint Name="HP_S10_Laser_B"><Variant_Of_Existing_Type>HP_S10_Laser_A</Variant_Of_Existing_Type><Health>100</Health></HardPoint>
</HardPoints>
)";

inline const char* const kFactions = R"(<Factions>
 <Faction Name="Empire"><Is_Playable>Yes</Is_Playable><Basic_AI>BasicEmpire</Basic_AI><Can_Control_Planets>Yes</Can_Control_Planets>
  <Space_Tactical_Unit_Cap>6</Space_Tactical_Unit_Cap><Space_Retreat_Countdown_Seconds>5</Space_Retreat_Countdown_Seconds></Faction>
 <Faction Name="Underworld"><Is_Playable>Yes</Is_Playable><Basic_AI>AI_Player_Underworld</Basic_AI><Can_Control_Planets>Yes</Can_Control_Planets>
  <Space_Tactical_Unit_Cap>6</Space_Tactical_Unit_Cap></Faction>
 <Faction Name="Neutral"><Is_Neutral>Yes</Is_Neutral></Faction>
</Factions>
)";

inline const char* const kLua = R"(function Set_Contrast_Values()
	EnemyContrastTypes[1] = "Corvette"
	FriendlyContrastTypeNames = {"Frigate", "Corvette"}
	FriendlyContrastWeights =	{0.5, 1.0}
	FriendlyContrastTypes[1].Parse(FriendlyContrastTypeNames, FriendlyContrastWeights)
	EnemyContrastTypes[2] = "Frigate"
	FriendlyContrastTypeNames = {"Frigate", "Corvette"}
	FriendlyContrastWeights =	{1.0, 0.5}
	FriendlyContrastTypes[2].Parse(FriendlyContrastTypeNames, FriendlyContrastWeights)
	EnemyContrastTypes[3] = "Infantry"
	FriendlyContrastTypeNames = {"Infantry"}
	FriendlyContrastWeights =	{1.0}
	FriendlyContrastTypes[3].Parse(FriendlyContrastTypeNames, FriendlyContrastWeights)
end
)";

struct Data {
    fs::path root;
    std::unique_ptr<DirectoryProvider> files;
    data::GameDatabase db;
    objects::ObjectTypeDatabase raw;
    objects::GameObjectTypes types;
    galactic::GalacticRules rules;
    galactic::ConflictRules crules;
    std::unique_ptr<BattleCatalog> catalog;
};

// Fog of war test data (owner: fog agent): the same types with REVEAL / HIDE_WHEN_FOGGED and sight ranges.
inline std::string fog_types() {
    std::string t = kTypes;
    auto replace_all = [&t](const std::string& from, const std::string& to) {
        for (size_t p = t.find(from); p != std::string::npos; p = t.find(from, p + to.size())) t.replace(p, from.size(), to);
    };
    replace_all("<SpaceBehavior>STARSHIP_LOCOMOTOR</SpaceBehavior>",
                "<SpaceBehavior>STARSHIP_LOCOMOTOR, REVEAL, HIDE_WHEN_FOGGED</SpaceBehavior><Space_FOW_Reveal_Range>900</Space_FOW_Reveal_Range>");
    replace_all("<LandBehavior>WALK_LOCOMOTOR</LandBehavior>",
                "<LandBehavior>WALK_LOCOMOTOR, REVEAL, HIDE_WHEN_FOGGED</LandBehavior><Land_FOW_Reveal_Range>150</Land_FOW_Reveal_Range>");
    // fog follow-up (S9 fog hooks): FOW_REVEAL_PING on the frigates (spawns a REVEAL projectile), Force Sight on the
    // troopers (2 s of Reveal_All for the owner).
    replace_all("    <Mod_Multiplier>WEAPON_DELAY_MULTIPLIER, 0.5</Mod_Multiplier><Mod_Multiplier>SPEED_MULTIPLIER, 0.5f</Mod_Multiplier></Unit_Ability>\n",
                "    <Mod_Multiplier>WEAPON_DELAY_MULTIPLIER, 0.5</Mod_Multiplier><Mod_Multiplier>SPEED_MULTIPLIER, 0.5f</Mod_Multiplier></Unit_Ability>\n"
                "   <Unit_Ability><Type>FOW_REVEAL_PING</Type><Recharge_Seconds>30</Recharge_Seconds>"
                "<Spawned_Object_Type> S10_Ping_Blast </Spawned_Object_Type></Unit_Ability>\n");
    replace_all("<Custom_Soft_Footprint_Radius>6</Custom_Soft_Footprint_Radius>\n",
                "<Custom_Soft_Footprint_Radius>6</Custom_Soft_Footprint_Radius>\n"
                "  <Unit_Abilities_Data SubObjectList=\"Yes\"><Unit_Ability><Type>FORCE_SIGHT</Type><Recharge_Seconds>50</Recharge_Seconds>"
                "<GUI_Activated_Ability_Name>S10_Force_Sight</GUI_Activated_Ability_Name></Unit_Ability></Unit_Abilities_Data>\n"
                "  <Abilities SubObjectList=\"Yes\"><Force_Sight_Ability Name=\"S10_Force_Sight\"><Activation_Style>User_Input</Activation_Style>"
                "<Force_Sight_Duration_In_Secs>2.0</Force_Sight_Duration_In_Secs></Force_Sight_Ability></Abilities>\n");
    replace_all("</S10>", " <Projectile Name=\"S10_Ping_Blast\"><Max_Speed>0</Max_Speed><Behavior>PROJECTILE, HIDE_WHEN_FOGGED, REVEAL</Behavior>"
                          "<Space_FOW_Reveal_Range>3000</Space_FOW_Reveal_Range><Dense_FOW_Reveal_Range_Multiplier>1.0</Dense_FOW_Reveal_Range_Multiplier>"
                          "</Projectile>\n</S10>");
    return t;
}

// FOW t01: the fog data plus a Space_Automatic Battlefield_Modifier with FOW_Reveal_Range_Multiplier 1.5 on the corvettes
// (Empire has one, the Underworld four), as the shipped Hutt_Field_Commander_FOW_Increase (Ground_Automatic) is built.
inline std::string fog_bm_types() {
    std::string t = fog_types();
    const std::string at = "<CategoryMask>Corvette</CategoryMask>";
    const size_t p = t.find(at);
    if (p != std::string::npos)
        t.insert(p + at.size(), "<Abilities SubObjectList=\"Yes\"><Battlefield_Modifier_Ability Name=\"S10_FOW_Boost\">"
                                "<Activation_Style>Space_Automatic</Activation_Style><FOW_Reveal_Range_Multiplier>1.5</FOW_Reveal_Range_Multiplier>"
                                "<Stacking_Category>0</Stacking_Category></Battlefield_Modifier_Ability></Abilities>");
    return t;
}

// cu8g t05 (CU8F_T04_T06_RE §3.3): Battlefield_Modifier gates. The Empire's star base carries S10_Gate_On (Space_Automatic,
// Enable_Special_Weapons and Enable_Bombing_Runs); every frigate and corvette (both sides) carries S10_Gate_Rev
// (Reverse_Application_Logic, Enable_Special_Weapons, Reinforcement_Time_Multiplier 2.0: the comm array's
// Slower_Reinforcements shape), which pushes its entries only when it is removed.
inline std::string gate_types() {
    std::string t = kTypes;
    auto insert_after = [&t](const std::string& at, const std::string& what) {
        const size_t p = t.find(at);
        if (p != std::string::npos) t.insert(p + at.size(), what);
    };
    insert_after("<StarBase Name=\"S10_Star_Base_1\">",
                 "<Abilities SubObjectList=\"Yes\"><Battlefield_Modifier_Ability Name=\"S10_Gate_On\">"
                 "<Activation_Style>Space_Automatic</Activation_Style><Enable_Special_Weapons>Yes</Enable_Special_Weapons>"
                 "<Enable_Bombing_Runs>Yes</Enable_Bombing_Runs><Stacking_Category>0</Stacking_Category>"
                 "</Battlefield_Modifier_Ability></Abilities>");
    insert_after("<SpaceUnit Name=\"S10_Frigate\">",
                 "<Abilities SubObjectList=\"Yes\"><Battlefield_Modifier_Ability Name=\"S10_Gate_Rev\">"
                 "<Activation_Style>Space_Automatic</Activation_Style><Enable_Special_Weapons>Yes</Enable_Special_Weapons>"
                 "<Reinforcement_Time_Multiplier>2.0</Reinforcement_Time_Multiplier><Reverse_Application_Logic>Yes</Reverse_Application_Logic>"
                 "<Stacking_Category>0</Stacking_Category></Battlefield_Modifier_Ability></Abilities>");
    return t;
}

// R3 anim test data: the same types plus corpse / removal fields and a frigate death clone.
inline std::string anim_types() {
    std::string t = kTypes;
    auto insert_after = [&t](const std::string& at, const std::string& what) {
        const size_t p = t.find(at);
        if (p != std::string::npos) t.insert(p + at.size(), what);
    };
    insert_after("<GroundInfantry Name=\"S10_Trooper\">", "<Death_Persistence_Duration>1.0</Death_Persistence_Duration><Death_Fade_Time>0.5</Death_Fade_Time>");
    // refixf t04: the star base's typed clone (no Remove_Upon_Death, 30 s persistence) is made on the battle's last tick.
    insert_after("<StarBase Name=\"S10_Star_Base_1\">",
                 "<Remove_Upon_Death>Yes</Remove_Upon_Death><Death_Clone>Damage_Normal, S10_Star_Base_Death_Clone</Death_Clone>");
    insert_after("<SpaceUnit Name=\"S10_Frigate\">", "<Death_Clone>Damage_Normal, S10_Frigate_Death_Clone</Death_Clone>");
    const size_t end = t.find("</S10>");
    if (end != std::string::npos)
        t.insert(end, " <SpaceUnit Name=\"S10_Frigate_Death_Clone\"><Death_Persistence_Duration>2</Death_Persistence_Duration>"
                      "<Death_Fade_Time>1</Death_Fade_Time></SpaceUnit>\n"
                      " <SpaceUnit Name=\"S10_Star_Base_Death_Clone\"><Death_Persistence_Duration>30</Death_Persistence_Duration></SpaceUnit>\n");
    return t;
}

// s78l t03 (TACTICAL_COMBAT slot 47 "s78l t03", TACTICAL_GAPS 6.2): the anim data with IDLE and IMPOSING_PRESENCE on the
// trooper, a 5000-unit Presence_Range and Presence_Induced_Animations "Celebrate, S10_Trooper" (every trooper induces
// Celebrate in every other).
inline std::string presence_types() {
    std::string t = anim_types();
    const std::string beh = "<Behavior>SELECTABLE, WEAPON</Behavior><Collidable_By_Projectile_Living>Yes</Collidable_By_Projectile_Living><LandBehavior>WALK_LOCOMOTOR";
    if (const size_t p = t.find(beh); p != std::string::npos)
        t.replace(p, beh.size(),
                  "<Behavior>SELECTABLE, WEAPON, IDLE, IMPOSING_PRESENCE</Behavior><Collidable_By_Projectile_Living>Yes</Collidable_By_Projectile_Living>"
                  "<Presence_Range>5000</Presence_Range><Presence_Induced_Animations>Celebrate, S10_Trooper</Presence_Induced_Animations>"
                  "<LandBehavior>WALK_LOCOMOTOR");
    return t;
}

// refixf t03: TEAM containers in land battles: S10_Trooper has Create_Team and S10_Squad no Create_Team_Type, so each
// squad chunk gets one object of the type named "Team" whose representative type (0x3973b0) is S10_Squad.
inline std::string team_types() {
    std::string t = kTypes;
    const std::string at = "<GroundInfantry Name=\"S10_Trooper\">";
    const size_t p = t.find(at);
    if (p != std::string::npos) t.insert(p + at.size(), "<Create_Team>Yes</Create_Team>");
    const size_t end = t.find("</S10>");
    if (end != std::string::npos) t.insert(end, " <Team Name=\"Team\"><Behavior>TEAM</Behavior></Team>\n");
    return t;
}

// s78n t03 / t09: the team data with the Team type on LAND_TEAM_CONTAINER_LOCOMOTOR (its own mover, members steering to
// slots) and MaxJiggleDistance 0.75; the trooper sized as FoC's Squad_Stormtrooper (Custom_Hard extents 2.0 x Scale_Factor
// 1.5 = half-extent 3.0) with FormationSpacing 0.5. s78n t14: ContainerArrangement Rows (FoC's Stormtrooper_Team).
// s78n review: `garrison` adds a garrison vehicle (S10_Walker: GARRISON_VEHICLE, 8 Infantry slots; company S10_Walker_Co),
// makes the trooper a GARRISON_UNIT and gives troopers and walker 100000 health (nobody dies during the garrison test).
inline std::string squad_types(bool garrison = false) {
    std::string t = team_types();
    if (garrison) {
        const size_t tp = t.find("<GroundInfantry Name=\"S10_Trooper\">");
        if (tp != std::string::npos) {
            const std::string hp = "<Tactical_Health>100</Tactical_Health>", walk = "<LandBehavior>WALK_LOCOMOTOR</LandBehavior>";
            if (const size_t p = t.find(hp, tp); p != std::string::npos) t.replace(p, hp.size(), "<Tactical_Health>100000</Tactical_Health>");
            if (const size_t p = t.find(walk, tp); p != std::string::npos)
                t.replace(p, walk.size(), "<LandBehavior>WALK_LOCOMOTOR, GARRISON_UNIT</LandBehavior><Garrison_Category>Infantry</Garrison_Category>");
        }
        const size_t end = t.find("</S10>");
        if (end != std::string::npos)
            t.insert(end, " <GroundVehicle Name=\"S10_Walker\"><Tactical_Health>100000</Tactical_Health><Max_Speed>1.0</Max_Speed>"
                          "<AI_Combat_Power>100</AI_Combat_Power><CategoryMask>Vehicle</CategoryMask><MovementClass>Tracked</MovementClass>"
                          "<Behavior>SELECTABLE</Behavior><Collidable_By_Projectile_Living>Yes</Collidable_By_Projectile_Living>"
                          "<LandBehavior>WALK_LOCOMOTOR, GARRISON_VEHICLE</LandBehavior><Garrison_Category>Infantry</Garrison_Category>"
                          "<Num_Garrison_Slots>8</Num_Garrison_Slots><Custom_Soft_Footprint_Radius>10</Custom_Soft_Footprint_Radius>"
                          "<Custom_Hard_XExtent>10</Custom_Hard_XExtent><Custom_Hard_YExtent>10</Custom_Hard_YExtent></GroundVehicle>\n"
                          " <GroundCompany Name=\"S10_Walker_Co\"><Company_Units>S10_Walker</Company_Units><Population_Value>1</Population_Value></GroundCompany>\n");
    }
    const std::string team = "<Team Name=\"Team\"><Behavior>TEAM</Behavior>";
    if (const size_t p = t.find(team); p != std::string::npos)
        t.insert(p + team.size(), "<LandBehavior>LAND_TEAM_CONTAINER_LOCOMOTOR</LandBehavior><MaxJiggleDistance>0.75</MaxJiggleDistance>"
                                    "<ContainerArrangement>Rows</ContainerArrangement>");
    const std::string ext = "<Custom_Hard_XExtent>6</Custom_Hard_XExtent><Custom_Hard_YExtent>6</Custom_Hard_YExtent>\n </GroundInfantry>";
    if (const size_t p = t.find(ext); p != std::string::npos)
        t.replace(p, ext.size(), "<Custom_Hard_XExtent>2</Custom_Hard_XExtent><Custom_Hard_YExtent>2</Custom_Hard_YExtent>"
                                 "<Scale_Factor>1.5</Scale_Factor><FormationSpacing>0.5</FormationSpacing>\n </GroundInfantry>");
    return t;
}

// a1i t04: the team data plus a landing transport: S10_Squad (Max_Squad_Size 2: two teams) names Company_Transport_Unit
// S10_Lander, whose Landing_Transport_Variant S10_Lander_Landing (TRANSPORT, Hover_Offset 10 inherited) carries it down.
inline std::string landing_types() {
    std::string t = team_types();
    const std::string at = "<GroundCompany Name=\"S10_Squad\">";
    if (const size_t p = t.find(at); p != std::string::npos)
        t.insert(p + at.size(), "<Company_Transport_Unit>S10_Lander</Company_Transport_Unit><Max_Squad_Size>2</Max_Squad_Size>");
    const size_t end = t.find("</S10>");
    if (end != std::string::npos)
        t.insert(end, " <GroundVehicle Name=\"S10_Lander\"><Behavior>TRANSPORT</Behavior><Hover_Offset>10</Hover_Offset>"
                      "<Landing_Transport_Variant>S10_Lander_Landing</Landing_Transport_Variant>"
                      "<Custom_Hard_XExtent>20</Custom_Hard_XExtent><Custom_Hard_YExtent>15</Custom_Hard_YExtent>"
                      "<Custom_Hard_XExtent_Deployed>30</Custom_Hard_XExtent_Deployed></GroundVehicle>\n"
                      " <GroundVehicle Name=\"S10_Lander_Landing\"><Variant_Of_Existing_Type>S10_Lander</Variant_Of_Existing_Type>"
                      "<Landing_Transport_Variant></Landing_Transport_Variant></GroundVehicle>\n");
    return t;
}

// s78j t03 / t04: the team data plus a SPRINT unit ability (SPEED Mod_Multiplier 1.5, long expiry) on S10_Trooper, so a
// test can activate it on single team members.
inline std::string sprint_types() {
    std::string t = team_types();
    const std::string at = "<GroundInfantry Name=\"S10_Trooper\">";
    if (const size_t p = t.find(at); p != std::string::npos)
        t.insert(p + at.size(), "<Unit_Abilities_Data SubObjectList=\"Yes\"><Unit_Ability><Type>SPRINT</Type><Expiration_Seconds>600</Expiration_Seconds>"
                                "<Recharge_Seconds>1</Recharge_Seconds><Mod_Multiplier>SPEED_MULTIPLIER, 1.5</Mod_Multiplier></Unit_Ability>"
                                "</Unit_Abilities_Data>");
    return t;
}

// s78k t04: the team data with a converting blaster (Projectile_Convert_Enemy_*: every Infantry enemy within 30 of the
// detonation changes owner to the shooter's), so team members convert in a battle.
inline std::string convert_types() {
    std::string t = team_types();
    const std::string at = "<Projectile Name=\"S10_Blaster_Proj\">";
    if (const size_t p = t.find(at); p != std::string::npos)
        t.insert(p + at.size(), "<Projectile_Convert_Enemy_On_Detonation>Yes</Projectile_Convert_Enemy_On_Detonation>"
                                "<Projectile_Convert_Enemy_Radius>30</Projectile_Convert_Enemy_Radius>"
                                "<Projectile_Convert_Enemy_Targets_Category_Mask>Infantry</Projectile_Convert_Enemy_Targets_Category_Mask>");
    return t;
}

// s78l t04 (TACTICAL_COMBAT §3.5 "Convert and its team rule" step 4): the team data plus a non-team slicer company for the
// Empire (S10_Slicer_Co: one S10_Slicer with User_Input Hack_Ability (TARGETED_HACK) and Tactical_Bribe_Ability
// (TACTICAL_BRIBE), range 5000, Applicable_Unit_Categories Infantry) and sturdy troopers (health 5000, so the victim team lives through the hack).
inline std::string hack_types() {
    std::string t = team_types();
    // s78l t05 (TACTICAL_COMBAT 5.4.1): a bribe cost > 1 on the team container (FoC containers carry their own
    // Tactical_Bribe_Cost) and on its member / company types.
    for (const char* at : {"<Team Name=\"Team\">", "<GroundInfantry Name=\"S10_Trooper\">", "<GroundCompany Name=\"S10_Squad\">"}) {
        const std::string a = at;
        if (const size_t p = t.find(a); p != std::string::npos) t.insert(p + a.size(), "<Tactical_Bribe_Cost>40</Tactical_Bribe_Cost>");
    }
    const std::string hp = "<Tactical_Health>100</Tactical_Health><Max_Speed>1.0</Max_Speed>";
    if (const size_t p = t.find(hp); p != std::string::npos) t.replace(p, hp.size(), "<Tactical_Health>5000</Tactical_Health><Max_Speed>1.0</Max_Speed>");
    const size_t end = t.find("</S10>");
    if (end != std::string::npos)
        t.insert(end,
                 " <GroundInfantry Name=\"S10_Slicer\"><Tactical_Health>5000</Tactical_Health><Max_Speed>1.0</Max_Speed>"
                 "<AI_Combat_Power>50</AI_Combat_Power><CategoryMask>Infantry</CategoryMask><MovementClass>Infantry</MovementClass>"
                 "<Behavior>SELECTABLE</Behavior><Collidable_By_Projectile_Living>Yes</Collidable_By_Projectile_Living>"
                 "<LandBehavior>WALK_LOCOMOTOR</LandBehavior><Custom_Soft_Footprint_Radius>6</Custom_Soft_Footprint_Radius>"
                 "<Custom_Hard_XExtent>6</Custom_Hard_XExtent><Custom_Hard_YExtent>6</Custom_Hard_YExtent>"
                 "<Unit_Abilities_Data SubObjectList=\"Yes\"><Unit_Ability><Type>TARGETED_HACK</Type><Recharge_Seconds>1</Recharge_Seconds>"
                 "<GUI_Activated_Ability_Name>S10_Hack</GUI_Activated_Ability_Name></Unit_Ability>"
                 "<Unit_Ability><Type>TACTICAL_BRIBE</Type><Recharge_Seconds>1</Recharge_Seconds>"
                 "<GUI_Activated_Ability_Name>S10_Bribe</GUI_Activated_Ability_Name></Unit_Ability></Unit_Abilities_Data>"
                 "<Abilities SubObjectList=\"Yes\"><Hack_Ability Name=\"S10_Hack\"><Activation_Style>User_Input</Activation_Style>"
                 "<Activation_Max_Range>5000</Activation_Max_Range><Applicable_Unit_Categories>Infantry</Applicable_Unit_Categories>"
                 "</Hack_Ability><Tactical_Bribe_Ability Name=\"S10_Bribe\"><Activation_Style>User_Input</Activation_Style>"
                 "<Activation_Max_Range>5000</Activation_Max_Range>"
                 "</Tactical_Bribe_Ability></Abilities></GroundInfantry>\n"
                 " <GroundCompany Name=\"S10_Slicer_Co\"><Company_Units>S10_Slicer</Company_Units><Population_Value>1</Population_Value></GroundCompany>\n"
                 // s78n t05 (HERO_CLASH §8): a vehicle thief (CAPTURE_VEHICLE + Vehicle_Thief_Ability, range 0-5000, chance
                 // 1) and a stealable speeder whose Vehicle_Thief_Inside_Clone is a one-slot GARRISON_VEHICLE variant; only
                 // test_vehicle_thief puts them into a battle.
                 " <GroundInfantry Name=\"S10_Thief\"><Tactical_Health>5000</Tactical_Health><Max_Speed>1.0</Max_Speed>"
                 "<AI_Combat_Power>50</AI_Combat_Power><CategoryMask>Infantry</CategoryMask><MovementClass>Infantry</MovementClass>"
                 "<Behavior>SELECTABLE</Behavior><Collidable_By_Projectile_Living>Yes</Collidable_By_Projectile_Living>"
                 "<LandBehavior>WALK_LOCOMOTOR, GARRISON_UNIT</LandBehavior><Garrison_Category>Infantry</Garrison_Category>"
                 "<Custom_Soft_Footprint_Radius>6</Custom_Soft_Footprint_Radius>"
                 "<Custom_Hard_XExtent>6</Custom_Hard_XExtent><Custom_Hard_YExtent>6</Custom_Hard_YExtent>"
                 "<Unit_Abilities_Data SubObjectList=\"Yes\"><Unit_Ability><Type>CAPTURE_VEHICLE</Type><Recharge_Seconds>1</Recharge_Seconds>"
                 "<GUI_Activated_Ability_Name>S10_Steal</GUI_Activated_Ability_Name></Unit_Ability></Unit_Abilities_Data>"
                 "<Abilities SubObjectList=\"Yes\"><Vehicle_Thief_Ability Name=\"S10_Steal\"><Activation_Style>User_Input</Activation_Style>"
                 "<Activation_Min_Range>0</Activation_Min_Range><Activation_Max_Range>5000</Activation_Max_Range>"
                 "<Activation_Chance>1.0</Activation_Chance><Applicable_Unit_Categories>Vehicle</Applicable_Unit_Categories>"
                 "</Vehicle_Thief_Ability></Abilities></GroundInfantry>\n"
                 " <GroundCompany Name=\"S10_Thief_Co\"><Company_Units>S10_Thief</Company_Units><Population_Value>1</Population_Value></GroundCompany>\n"
                 " <GroundVehicle Name=\"S10_Speeder\"><Tactical_Health>5000</Tactical_Health><Max_Speed>1.0</Max_Speed>"
                 "<AI_Combat_Power>100</AI_Combat_Power><CategoryMask>Vehicle</CategoryMask><MovementClass>Tracked</MovementClass>"
                 "<Behavior>SELECTABLE</Behavior><Collidable_By_Projectile_Living>Yes</Collidable_By_Projectile_Living>"
                 "<LandBehavior>WALK_LOCOMOTOR</LandBehavior><Custom_Soft_Footprint_Radius>10</Custom_Soft_Footprint_Radius>"
                 "<Custom_Hard_XExtent>10</Custom_Hard_XExtent><Custom_Hard_YExtent>10</Custom_Hard_YExtent>"
                 "<Tactical_Bribe_Cost>40</Tactical_Bribe_Cost><Vehicle_Thief_Inside_Clone>S10_Speeder_Stolen</Vehicle_Thief_Inside_Clone>"
                 "</GroundVehicle>\n"
                 " <GroundVehicle Name=\"S10_Speeder_Stolen\"><Tactical_Health>5000</Tactical_Health><Max_Speed>1.0</Max_Speed>"
                 "<AI_Combat_Power>100</AI_Combat_Power><CategoryMask>Vehicle</CategoryMask><MovementClass>Tracked</MovementClass>"
                 "<Behavior>SELECTABLE</Behavior><Collidable_By_Projectile_Living>Yes</Collidable_By_Projectile_Living>"
                 "<LandBehavior>WALK_LOCOMOTOR, GARRISON_VEHICLE</LandBehavior><Garrison_Category>Infantry</Garrison_Category>"
                 "<Num_Garrison_Slots>1</Num_Garrison_Slots><Custom_Soft_Footprint_Radius>10</Custom_Soft_Footprint_Radius>"
                 "<Custom_Hard_XExtent>10</Custom_Hard_XExtent><Custom_Hard_YExtent>10</Custom_Hard_YExtent>"
                 "<Tactical_Bribe_Cost>40</Tactical_Bribe_Cost></GroundVehicle>\n"
                 " <GroundCompany Name=\"S10_Speeder_Co\"><Company_Units>S10_Speeder</Company_Units><Population_Value>1</Population_Value></GroundCompany>\n"
                 // s78n t06 (HERO_CLASH §1.2): two named heroes (health 500, no weapon) for the hero-clash battle test;
                 // only test_hero_clash_battle puts them into a battle.
                 " <GroundInfantry Name=\"S10_HeroA\"><Tactical_Health>500</Tactical_Health><Max_Speed>1.0</Max_Speed>"
                 "<Is_Named_Hero>Yes</Is_Named_Hero><AI_Combat_Power>50</AI_Combat_Power><CategoryMask>Infantry</CategoryMask>"
                 "<MovementClass>Infantry</MovementClass><Behavior>SELECTABLE</Behavior>"
                 "<Collidable_By_Projectile_Living>Yes</Collidable_By_Projectile_Living><LandBehavior>WALK_LOCOMOTOR</LandBehavior>"
                 "<Custom_Soft_Footprint_Radius>6</Custom_Soft_Footprint_Radius><Custom_Hard_XExtent>6</Custom_Hard_XExtent>"
                 "<Custom_Hard_YExtent>6</Custom_Hard_YExtent></GroundInfantry>\n"
                 " <GroundCompany Name=\"S10_HeroA_Co\"><Company_Units>S10_HeroA</Company_Units><Population_Value>1</Population_Value></GroundCompany>\n"
                 " <GroundInfantry Name=\"S10_HeroB\"><Tactical_Health>500</Tactical_Health><Max_Speed>1.0</Max_Speed>"
                 "<Is_Named_Hero>Yes</Is_Named_Hero><AI_Combat_Power>50</AI_Combat_Power><CategoryMask>Infantry</CategoryMask>"
                 "<MovementClass>Infantry</MovementClass><Behavior>SELECTABLE</Behavior>"
                 "<Collidable_By_Projectile_Living>Yes</Collidable_By_Projectile_Living><LandBehavior>WALK_LOCOMOTOR</LandBehavior>"
                 "<Custom_Soft_Footprint_Radius>6</Custom_Soft_Footprint_Radius><Custom_Hard_XExtent>6</Custom_Hard_XExtent>"
                 "<Custom_Hard_YExtent>6</Custom_Hard_YExtent></GroundInfantry>\n"
                 " <GroundCompany Name=\"S10_HeroB_Co\"><Company_Units>S10_HeroB</Company_Units><Population_Value>1</Population_Value></GroundCompany>\n"
                 // s78n t06 review: a garrisonable named hero (S10_HeroA as a GARRISON_UNIT, like Vader / Obi-Wan) and a
                 // one-slot garrison vehicle company for it (S10_Speeder_Stolen); only test_hero_clash_garrison uses them.
                 " <GroundInfantry Name=\"S10_HeroC\"><Tactical_Health>500</Tactical_Health><Max_Speed>1.0</Max_Speed>"
                 "<Is_Named_Hero>Yes</Is_Named_Hero><AI_Combat_Power>50</AI_Combat_Power><CategoryMask>Infantry</CategoryMask>"
                 "<MovementClass>Infantry</MovementClass><Behavior>SELECTABLE</Behavior>"
                 "<Collidable_By_Projectile_Living>Yes</Collidable_By_Projectile_Living>"
                 "<LandBehavior>WALK_LOCOMOTOR, GARRISON_UNIT</LandBehavior><Garrison_Category>Infantry</Garrison_Category>"
                 "<Custom_Soft_Footprint_Radius>6</Custom_Soft_Footprint_Radius><Custom_Hard_XExtent>6</Custom_Hard_XExtent>"
                 "<Custom_Hard_YExtent>6</Custom_Hard_YExtent></GroundInfantry>\n"
                 " <GroundCompany Name=\"S10_HeroC_Co\"><Company_Units>S10_HeroC</Company_Units><Population_Value>1</Population_Value></GroundCompany>\n"
                 " <GroundCompany Name=\"S10_HeroHold_Co\"><Company_Units>S10_Speeder_Stolen</Company_Units><Population_Value>1</Population_Value></GroundCompany>\n"
                 // s78n t12 (HERO_CLASH §8.4.3): stealable speeders WITHOUT a Vehicle_Thief_Inside_Clone and without a
                 // garrison behaviour - the thief boards the original. S10_Speeder_Eject also has unit ability
                 // EJECT_VEHICLE_THIEF (46), which the boarding turns on only when the boarded object has it. Only
                 // test_vehicle_thief puts them into a battle.
                 " <GroundVehicle Name=\"S10_Speeder_Plain\"><Tactical_Health>5000</Tactical_Health><Max_Speed>1.0</Max_Speed>"
                 "<AI_Combat_Power>100</AI_Combat_Power><CategoryMask>Vehicle</CategoryMask><MovementClass>Tracked</MovementClass>"
                 "<Behavior>SELECTABLE</Behavior><Collidable_By_Projectile_Living>Yes</Collidable_By_Projectile_Living>"
                 "<LandBehavior>WALK_LOCOMOTOR</LandBehavior><Custom_Soft_Footprint_Radius>10</Custom_Soft_Footprint_Radius>"
                 "<Custom_Hard_XExtent>10</Custom_Hard_XExtent><Custom_Hard_YExtent>10</Custom_Hard_YExtent>"
                 "<Tactical_Bribe_Cost>40</Tactical_Bribe_Cost></GroundVehicle>\n"
                 " <GroundCompany Name=\"S10_Speeder_Plain_Co\"><Company_Units>S10_Speeder_Plain</Company_Units><Population_Value>1</Population_Value></GroundCompany>\n"
                 " <GroundVehicle Name=\"S10_Speeder_Eject\"><Tactical_Health>5000</Tactical_Health><Max_Speed>1.0</Max_Speed>"
                 "<AI_Combat_Power>100</AI_Combat_Power><CategoryMask>Vehicle</CategoryMask><MovementClass>Tracked</MovementClass>"
                 "<Behavior>SELECTABLE</Behavior><Collidable_By_Projectile_Living>Yes</Collidable_By_Projectile_Living>"
                 "<LandBehavior>WALK_LOCOMOTOR</LandBehavior><Custom_Soft_Footprint_Radius>10</Custom_Soft_Footprint_Radius>"
                 "<Custom_Hard_XExtent>10</Custom_Hard_XExtent><Custom_Hard_YExtent>10</Custom_Hard_YExtent>"
                 "<Tactical_Bribe_Cost>40</Tactical_Bribe_Cost>"
                 "<Unit_Abilities_Data SubObjectList=\"Yes\"><Unit_Ability><Type>EJECT_VEHICLE_THIEF</Type><Recharge_Seconds>1</Recharge_Seconds>"
                 "</Unit_Ability></Unit_Abilities_Data></GroundVehicle>\n"
                 " <GroundCompany Name=\"S10_Speeder_Eject_Co\"><Company_Units>S10_Speeder_Eject</Company_Units><Population_Value>1</Population_Value></GroundCompany>\n");
    return t;
}

// s78l t04 (TACTICAL_COMBAT §3.1.1 "Units leaving a land landing object", MOVEMENT §3.5.1 / §6.1): the landing data with
// S10_Squad made of three vehicles (S10_Tank: no Create_Team, Walk locomotor, Stopped_Rate_Of_Turn 4, no weapon), so each
// unload step is one vehicle whose exit move carries the landing's yaw as its final facing.
inline std::string vehicle_landing_types() {
    std::string t = landing_types();
    const std::string cu = "<Company_Units>S10_Trooper, S10_Trooper, S10_Trooper, S10_Trooper</Company_Units>";
    if (const size_t p = t.find(cu); p != std::string::npos) t.replace(p, cu.size(), "<Company_Units>S10_Tank, S10_Tank, S10_Tank</Company_Units>");
    const size_t end = t.find("</S10>");
    if (end != std::string::npos)
        t.insert(end,
                 " <GroundVehicle Name=\"S10_Tank\"><Victory_Relevant>Yes</Victory_Relevant><Tactical_Health>2000</Tactical_Health>"
                 "<Max_Speed>1.5</Max_Speed><Max_Rate_Of_Turn>2.0</Max_Rate_Of_Turn><Stopped_Rate_Of_Turn>4.0</Stopped_Rate_Of_Turn>"
                 "<AI_Combat_Power>100</AI_Combat_Power><CategoryMask>Vehicle</CategoryMask><MovementClass>Tracked</MovementClass>"
                 "<Behavior>SELECTABLE</Behavior><Collidable_By_Projectile_Living>Yes</Collidable_By_Projectile_Living>"
                 "<LandBehavior>WALK_LOCOMOTOR</LandBehavior><Custom_Soft_Footprint_Radius>10</Custom_Soft_Footprint_Radius>"
                 "<Custom_Hard_XExtent>10</Custom_Hard_XExtent><Custom_Hard_YExtent>10</Custom_Hard_YExtent></GroundVehicle>\n");
    return t;
}

// --- S4 (owner: S4 agent): garrison / capture / corruption test data: troopers are GARRISON_UNIT (Infantry); a
// community bunker (GARRISON_STRUCTURE, GarrisonCanFire) and a capture post (CAPTURE_POINT, 1 s transition) are
// placed on the land map; planet S10_B offers a Kidnapping corruption mission (land, S10_Land_B, mission plots) and a
// Piracy corruption without a mission.
inline std::string s4_types() {
    std::string t = kTypes;
    auto replace_all = [&t](const std::string& from, const std::string& to) {
        for (size_t p = t.find(from); p != std::string::npos; p = t.find(from, p + to.size())) t.replace(p, from.size(), to);
    };
    replace_all("<LandBehavior>WALK_LOCOMOTOR</LandBehavior>",
                "<LandBehavior>WALK_LOCOMOTOR, GARRISON_UNIT</LandBehavior><Garrison_Category>Infantry</Garrison_Category>");
    replace_all("<Planet_Credit_Value>10</Planet_Credit_Value></Planet>\n <Planet Name=\"S10_B\">",
                "<Planet_Credit_Value>10</Planet_Credit_Value>\n"
                "  <Corruption_1_Type> Corruption_Piracy </Corruption_1_Type><Corruption_1_Credit_Cost>100</Corruption_1_Credit_Cost></Planet>\n"
                " <Planet Name=\"S10_B\">");
    replace_all("<Land_Tactical_Map>S10_Land_B</Land_Tactical_Map>",
                "<Land_Tactical_Map>S10_Land_B</Land_Tactical_Map>\n"
                "  <Corruption_1_Type> Corruption_Kidnapping </Corruption_1_Type><Corruption_1_Credit_Cost>500</Corruption_1_Credit_Cost>\n"
                "  <Corruption_1_Tactical_Game_Mode> Land </Corruption_1_Tactical_Game_Mode>"
                "<Corruption_1_Tactical_Map_Name> S10_Land_B </Corruption_1_Tactical_Map_Name>\n"
                "  <Corruption_1_Tactical_Script_Name> Story_Plots_S4_Mission.xml </Corruption_1_Tactical_Script_Name>\n"
                "  <Corruption_2_Type> Corruption_Piracy </Corruption_2_Type><Corruption_2_Credit_Cost>1000</Corruption_2_Credit_Cost>\n"
                "  <Corruption_2_Required_Mission_Type> Corruption_Kidnapping </Corruption_2_Required_Mission_Type>"
                "<Corruption_2_Required_Mission_Count>1</Corruption_2_Required_Mission_Count>");
    replace_all("</S10>",
                " <SpecialStructure Name=\"S4_Bunker\"><Victory_Relevant>Yes</Victory_Relevant><Behavior>SELECTABLE, CAPTURE_POINT</Behavior><LandBehavior>GARRISON_STRUCTURE</LandBehavior>"
                "<Garrison_Category>Infantry</Garrison_Category><Num_Garrison_Slots>4</Num_Garrison_Slots><Garrison_Exit_Dist>30</Garrison_Exit_Dist>"
                "<Is_Community_Property>Yes</Is_Community_Property><Property_Flags>GarrisonCanFire</Property_Flags>"
                "<Capture_Point_Radius>20</Capture_Point_Radius><Capture_Point_Transition_Time_Seconds>1.0</Capture_Point_Transition_Time_Seconds>"
                "<Tactical_Health>500</Tactical_Health><CategoryMask>Structure</CategoryMask></SpecialStructure>\n"
                " <SpecialStructure Name=\"S4_Barracks\"><Victory_Relevant>Yes</Victory_Relevant><Behavior>SELECTABLE, DUMMY_GROUND_STRUCTURE</Behavior>"  // t48: land BaseLevel
                "<Tactical_Health>500</Tactical_Health><CategoryMask>Structure</CategoryMask></SpecialStructure>\n"
                " <GroundInfantry Name=\"S4_Hero\"><Variant_Of_Existing_Type>S10_Trooper</Variant_Of_Existing_Type>"
                "<Is_Named_Hero>Yes</Is_Named_Hero><Hyperspace>Yes</Hyperspace><Hyperspace_Speed>1.5</Hyperspace_Speed>"
                "<Population_Value>1</Population_Value></GroundInfantry>\n"
                // a1f t07: Secondary_Abandoned_Sensor's combat-relevant tags (a selectable neutral capture structure, no weapon; health
                // 250 instead of 700 so three troopers kill it before the Underworld wins)
                " <SecondaryStructure Name=\"S4_Sensor\"><Behavior>CAPTURE_POINT, SELECTABLE, DUMMY_GROUND_STRUCTURE</Behavior>"
                "<Is_Dummy>Yes</Is_Dummy><Tactical_Health>250</Tactical_Health><CategoryMask>Structure</CategoryMask>"
                "<Collidable_By_Projectile_Living>Yes</Collidable_By_Projectile_Living><Influences_Capture_Point>False</Influences_Capture_Point>"
                "<Capture_Point_Transition_Time_Seconds>10.0</Capture_Point_Transition_Time_Seconds><Victory_Relevant>No</Victory_Relevant>"
                "</SecondaryStructure>\n"
                " <Marker Name=\"S4_Post\"><Behavior>CAPTURE_POINT</Behavior><Capture_Point_Radius>300</Capture_Point_Radius>"
                "<Capture_Point_Transition_Time_Seconds>1.0</Capture_Point_Transition_Time_Seconds>"
                "<Control_Point_Domination_Condition_Relevant>Yes</Control_Point_Domination_Condition_Relevant></Marker>\n"  // ccpgt t02
                // S10 map owners: a landing reinforcement point (REMAINING_GAPS §3.3 probe 2026-10-29)
                " <Marker Name=\"S4_RPoint\"><Behavior>CAPTURE_POINT, REINFORCEMENT_POINT</Behavior><Capture_Point_Radius>100</Capture_Point_Radius>"
                "<Capture_Point_Transition_Time_Seconds>1.0</Capture_Point_Transition_Time_Seconds></Marker>\n"
                "</S10>");
    return t;
}
// --- end S4 ---

// --- P4 tactical: Black Market gate test data: the frigates' (and the corvette variant's) POWER_TO_WEAPONS record is
// Must_Be_Bought_On_Black_Market under the name S10_BM_Power, listed by the one item S10_BM_Item. ---
inline std::string bm_types() {
    std::string t = kTypes;
    const std::string at = "<Unit_Ability><Type>POWER_TO_WEAPONS</Type>";
    const size_t p = t.find(at);
    if (p != std::string::npos)
        t.insert(p + at.size(), "<GUI_Activated_Ability_Name>S10_BM_Power</GUI_Activated_Ability_Name>"
                                "<Must_Be_Bought_On_Black_Market>Yes</Must_Be_Bought_On_Black_Market>");
    return t;
}
inline const char* const kBmItems = R"(<BlackMarketItems>
 <Item Name="S10_BM_Item"><Faction>Empire</Faction><Tech_Level>1</Tech_Level><Base_Cost_Credits>100</Base_Cost_Credits>
  <Ability_Names>S10_BM_Power</Ability_Names></Item>
</BlackMarketItems>
)";
// --- end P4 tactical ---

// Skirmish station production test data: the Empire star base lists S10_Base_Upgrade (DUMMY_UPGRADE with a
// Skirmish_Automatic Starbase_Upgrade_Ability, prerequisite S10_Star_Base_1, limit 1 for all allies) in its Empire row of
// Tactical_Buildable_Objects_Multiplayer, and upgrades to S10_Star_Base_2.
inline std::string prod_types() {
    std::string t = kTypes;
    const std::string at = "<StarBase Name=\"S10_Star_Base_1\">";
    const size_t p = t.find(at);
    if (p != std::string::npos)
        t.insert(p + at.size(), "<SpaceBehavior>DUMMY_STAR_BASE</SpaceBehavior><Tactical_Buildable_Objects_Multiplayer> Empire, S10_Base_Upgrade </Tactical_Buildable_Objects_Multiplayer>"
                                "<Next_Level_Base>S10_Star_Base_2</Next_Level_Base>");
    const size_t end = t.find("</S10>");
    if (end != std::string::npos)
        t.insert(end, " <StarBase Name=\"S10_Star_Base_2\"><Victory_Relevant>Yes</Victory_Relevant><Tactical_Health>3000</Tactical_Health><Base_Level>2</Base_Level>"
                      "<AI_Combat_Power>800</AI_Combat_Power><CategoryMask>Structure</CategoryMask><HardPoints>HP_S10_Laser_A</HardPoints>"
                      "<Targeting_Max_Attack_Distance>1500</Targeting_Max_Attack_Distance><Behavior>SELECTABLE</Behavior>"
                      "<Collidable_By_Projectile_Living>Yes</Collidable_By_Projectile_Living><SpaceBehavior>DUMMY_STAR_BASE</SpaceBehavior></StarBase>\n"
                      " <UpgradeObject Name=\"S10_Base_Upgrade\"><Behavior>DUMMY_UPGRADE</Behavior><Affiliation>Empire</Affiliation>"
                      "<Tactical_Build_Cost_Multiplayer>100</Tactical_Build_Cost_Multiplayer><Tactical_Build_Time_Seconds>1</Tactical_Build_Time_Seconds>"
                      "<Tactical_Build_Prerequisites> S10_Star_Base_1 </Tactical_Build_Prerequisites>"
                      "<Build_Limit_Current_For_All_Allies>1</Build_Limit_Current_For_All_Allies>"
                      "<Abilities SubObjectList=\"Yes\"><Starbase_Upgrade_Ability Name=\"S10_Base_Upgrade_Effect\">"
                      "<Activation_Style>Skirmish_Automatic</Activation_Style></Starbase_Upgrade_Ability></Abilities></UpgradeObject>\n");
    return t;
}

// refixf t02: the star base is a SPACE_OBSTACLE (as every FoC star base: STARBASES.XML) with a 250 footprint.
inline std::string obstacle_types() {
    std::string t = kTypes;
    const std::string at = "<StarBase Name=\"S10_Star_Base_1\">";
    const size_t p = t.find(at);
    if (p != std::string::npos)
        t.insert(p + at.size(), "<SpaceBehavior>SPACE_OBSTACLE</SpaceBehavior><Custom_Soft_Footprint_Radius>250</Custom_Soft_Footprint_Radius>");
    // a weak one, so the attackers kill it inside the test's frames (its static entry must leave with it)
    const std::string hp = "<Tactical_Health>2500</Tactical_Health><Base_Level>1</Base_Level>";
    if (const size_t q = t.find(hp); q != std::string::npos) t.replace(q, hp.size(), "<Tactical_Health>300</Tactical_Health><Base_Level>1</Base_Level>");
    // S8 t02 / S7 t13 (test_runtime_obstacles): a weak LAND_OBSTACLE bunker, a LAND_OBSTACLE rock and a smaller rock
    // placeholder that always spawns the rock (Map_Load_Spawn_Table); a SPACE_OBSTACLE map unit (SIMPLE_SPACE locomotor, so
    // Hyperspace_Away kills it), an asteroid and a smaller asteroid placeholder that always spawns the asteroid.
    // S8 t13: the land obstacles have no Land_Model_Name, so (MOVEMENT §1.3) they stamp only with Fine_Tune off (the
    // explicit XML rectangle); with the default (on) and no proxy retail never stamps them.
    const std::string extra =
        " <GroundStructure Name=\"S10_Obst_Bunker\"><Fine_Tune_Occupied_Passability>No</Fine_Tune_Occupied_Passability><Tactical_Health>40</Tactical_Health><CategoryMask>Structure</CategoryMask>"
        "<Behavior>SELECTABLE</Behavior><LandBehavior>LAND_OBSTACLE</LandBehavior><Collidable_By_Projectile_Living>Yes</Collidable_By_Projectile_Living>"
        "<Obstacle_Width>60</Obstacle_Width><Obstacle_Height>60</Obstacle_Height><Obstacle_X_Offset>-30</Obstacle_X_Offset>"
        "<Obstacle_Y_Offset>-30</Obstacle_Y_Offset></GroundStructure>\n"
        // S8 t09: a structure whose death clone is not an obstacle (Death_Clone_Is_Obstacle false, as Imperial_Prison_Dathomir)
        " <GroundStructure Name=\"S10_Obst_Prison\"><Fine_Tune_Occupied_Passability>No</Fine_Tune_Occupied_Passability><Tactical_Health>40</Tactical_Health><CategoryMask>Structure</CategoryMask>"
        "<Behavior>SELECTABLE</Behavior><LandBehavior>LAND_OBSTACLE</LandBehavior><Death_Clone_Is_Obstacle>No</Death_Clone_Is_Obstacle>"
        "<Obstacle_Width>60</Obstacle_Width><Obstacle_Height>60</Obstacle_Height><Obstacle_X_Offset>-30</Obstacle_X_Offset>"
        "<Obstacle_Y_Offset>-30</Obstacle_Y_Offset></GroundStructure>\n"
        " <GroundProp Name=\"S10_Obst_Rock\"><Fine_Tune_Occupied_Passability>No</Fine_Tune_Occupied_Passability><LandBehavior>LAND_OBSTACLE</LandBehavior><Obstacle_Width>100</Obstacle_Width>"
        "<Obstacle_Height>100</Obstacle_Height><Obstacle_X_Offset>-50</Obstacle_X_Offset><Obstacle_Y_Offset>-50</Obstacle_Y_Offset></GroundProp>\n"
        " <GroundProp Name=\"S10_Obst_Rock_Spawner\"><Fine_Tune_Occupied_Passability>No</Fine_Tune_Occupied_Passability><LandBehavior>LAND_OBSTACLE</LandBehavior><Obstacle_Width>20</Obstacle_Width>"
        "<Obstacle_Height>20</Obstacle_Height><Obstacle_X_Offset>-10</Obstacle_X_Offset><Obstacle_Y_Offset>-10</Obstacle_Y_Offset>"
        "<Map_Load_Spawn_Table>S10_Obst_Rock, 1.0</Map_Load_Spawn_Table></GroundProp>\n"
        " <SpaceUnit Name=\"S10_Obst_Mine\"><Variant_Of_Existing_Type>S10_Frigate</Variant_Of_Existing_Type>"
        "<SpaceBehavior>SIMPLE_SPACE_LOCOMOTOR, SPACE_OBSTACLE</SpaceBehavior><Custom_Soft_Footprint_Radius>120</Custom_Soft_Footprint_Radius></SpaceUnit>\n"
        " <SpaceProp Name=\"S10_Obst_Asteroid\"><Behavior>SPACE_OBSTACLE</Behavior><Space_Obstacle_Radius>400</Space_Obstacle_Radius></SpaceProp>\n"
        " <SpaceProp Name=\"S10_Obst_Asteroid_Spawner\"><Behavior>SPACE_OBSTACLE</Behavior><Space_Obstacle_Radius>50</Space_Obstacle_Radius>"
        "<Map_Load_Spawn_Table>S10_Obst_Asteroid, 1.0</Map_Load_Spawn_Table></SpaceProp>\n";
    t.insert(t.rfind("</S10>"), extra);
    return t;
}

// S7 t12: a TIE-like fighter squadron (FIGHTER_LOCOMOTOR members, Squadron_Units -> one TEAM container in space). Health
// is high so the frigates' lasers do not end the test; the attack range is wide so a frigate next to the route becomes the
// fighters' combat auto target long before they pass it. S7 t22: the squadron's chase ranges are the FoC X-wing squadron's
// (Guard 1000 / Idle 200 / Attack_Move_Response 300); `fog` starts from the fog test types (frigates REVEAL 900 and
// HIDE_WHEN_FOGGED; the fighters reveal nothing).
inline std::string fighter_types(bool fog = false) {
    std::string t = fog ? fog_types() : std::string(kTypes);
    const size_t end = t.find("</S10>");
    if (end != std::string::npos)
        t.insert(end,
                 " <SpaceUnit Name=\"S10_TIE\"><Victory_Relevant>Yes</Victory_Relevant><Tactical_Health>20000</Tactical_Health>"
                 "<Max_Speed>4.5</Max_Speed><Min_Speed>2.5</Min_Speed><Max_Rate_Of_Turn>3.0</Max_Rate_Of_Turn><Max_Rate_Of_Roll>5.0</Max_Rate_Of_Roll>"
                 "<Max_Lift>4.0</Max_Lift><Max_Thrust>1.0</Max_Thrust><Bank_Turn_Angle>40</Bank_Turn_Angle><Strafe_Distance>500</Strafe_Distance>"
                 "<CategoryMask>Fighter</CategoryMask><Targeting_Max_Attack_Distance>900</Targeting_Max_Attack_Distance>"
                 "<Projectile_Types>S10_Blaster_Proj</Projectile_Types><Projectile_Fire_Recharge_Seconds>1.0</Projectile_Fire_Recharge_Seconds>"
                 "<Projectile_Fire_Pulse_Count>1</Projectile_Fire_Pulse_Count><Behavior>SELECTABLE, WEAPON</Behavior>"
                 "<Collidable_By_Projectile_Living>Yes</Collidable_By_Projectile_Living><SpaceBehavior>FIGHTER_LOCOMOTOR</SpaceBehavior>"
                 "<Custom_Soft_Footprint_Radius>10</Custom_Soft_Footprint_Radius></SpaceUnit>\n"
                 " <Squadron Name=\"S10_TIE_Squadron\"><Squadron_Units>S10_TIE, S10_TIE, S10_TIE</Squadron_Units>"
                 "<Hyperspace>Yes</Hyperspace><Hyperspace_Speed>1.5</Hyperspace_Speed><Population_Value>1</Population_Value>"
                 "<Squadron_Formation_Error_Tolerance>25</Squadron_Formation_Error_Tolerance><Guard_Chase_Range>1000.0</Guard_Chase_Range>"
                 "<Idle_Chase_Range>200.0</Idle_Chase_Range><Attack_Move_Response_Range>300.0</Attack_Move_Response_Range></Squadron>\n"
                 " <Team Name=\"Team\"><Behavior>TEAM</Behavior></Team>\n"
                 // S7 t24: a moving carrier (a locomotor, so its launches escort it) with one fighter bay; one squadron at a time
                 " <SpaceUnit Name=\"S10_Carrier\"><Variant_Of_Existing_Type>S10_Frigate</Variant_Of_Existing_Type>"
                 "<Tactical_Health>20000</Tactical_Health><HardPoints>HP_S10_Laser_A, HP_S10_Bay</HardPoints>"
                 "<SpaceBehavior>STARSHIP_LOCOMOTOR, SPAWN_SQUADRON</SpaceBehavior>"
                 "<Starting_Spawned_Units_Tech_0>S10_TIE_Squadron, 1</Starting_Spawned_Units_Tech_0>"
                 "<Spawned_Squadron_Delay_Seconds>1.0</Spawned_Squadron_Delay_Seconds></SpaceUnit>\n");
    // fog: a 2100 attack range, so an ordered attack stops the squadron at 0.7 x 2100 = 1470, outside the frigates' 1400 (no
    // shots at it, so no attacker flash reveals the target).
    if (fog)
        if (const size_t p = t.find("<CategoryMask>Fighter</CategoryMask><Targeting_Max_Attack_Distance>900"); p != std::string::npos)
            t.replace(p, 70, "<CategoryMask>Fighter</CategoryMask><Targeting_Max_Attack_Distance>2100");
    return t;
}

// S8 t10 follow-up (LOB_SOLVER §3.5 "MARKER removal"): the troopers carry Remote_Bomb (PLACE_REMOTE_BOMB 64, Bomb_Type a
// sticky GRENADE, Toss_Anim without clip data: the toss 12 frames after the stop) and the map knows Dummy_Barrage_Target.
inline std::string bomb_types() {
    std::string t = kTypes;
    const std::string at = "<Custom_Hard_YExtent>6</Custom_Hard_YExtent>\n";
    const size_t p = t.find(at);
    if (p != std::string::npos)
        t.insert(p + at.size(),
                 "  <Unit_Abilities_Data SubObjectList=\"Yes\"><Unit_Ability><Type>PLACE_REMOTE_BOMB</Type><Recharge_Seconds>18</Recharge_Seconds>"
                 "<GUI_Activated_Ability_Name>S10_Remote_Bomb</GUI_Activated_Ability_Name></Unit_Ability>"
                 "<Unit_Ability><Type>DETONATE_REMOTE_BOMB</Type></Unit_Ability></Unit_Abilities_Data>\n"
                 "  <Abilities SubObjectList=\"Yes\"><Remote_Bomb_Ability Name=\"S10_Remote_Bomb\"><Activation_Style>User_Input</Activation_Style>"
                 "<Activation_Min_Range>5.0</Activation_Min_Range><Activation_Max_Range>130.0</Activation_Max_Range>"
                 "<Bomb_Type>S10_Remote_Bomb_Proj</Bomb_Type><Toss_Anim>BOMBTOSS</Toss_Anim><Spawn_Frame>12.0</Spawn_Frame>"
                 "</Remote_Bomb_Ability></Abilities>\n");
    t.insert(t.rfind("</S10>"),
             " <Projectile Name=\"S10_Remote_Bomb_Proj\"><Max_Speed>20.0</Max_Speed><Projectile_Grenade_Gravity>1.0</Projectile_Grenade_Gravity>"
             "<Projectile_Grenade_Gravity_Lob_Mod>1.0</Projectile_Grenade_Gravity_Lob_Mod><Projectile_Grenade_Can_Lob_Slower>yes</Projectile_Grenade_Can_Lob_Slower>"
             "<Behavior>PROJECTILE, BOMB</Behavior><Projectile_Category>GRENADE</Projectile_Category>"
             "<Projectile_Grenade_Sticks_On_Collision>yes</Projectile_Grenade_Sticks_On_Collision><Projectile_Max_Lifetime>900.0</Projectile_Max_Lifetime>"
             "<Projectile_Damage>0.0</Projectile_Damage><Projectile_Blast_Area_Damage>50</Projectile_Blast_Area_Damage>"
             "<Projectile_Blast_Area_Range>30</Projectile_Blast_Area_Range><Friendly_Damage_Amount>200</Friendly_Damage_Amount></Projectile>\n"
             " <MiscObject Name=\"Dummy_Barrage_Target\"><LandBehavior>MARKER</LandBehavior><SpaceBehavior>MARKER</SpaceBehavior>"
             "<Is_Decoration>No</Is_Decoration><CategoryMask>Corvette</CategoryMask><Immune_To_Damage>True</Immune_To_Damage></MiscObject>\n");
    return t;
}

inline std::unique_ptr<Data> make_data(bool fog = false, bool anim = false, bool s4 = false, bool bm = false, bool prod = false,
                                bool obstacle = false, bool team = false, bool fog_bm = false, bool fighters = false,
                                bool fog_setup = false, bool bomb = false, bool landing = false, bool sprint = false,
                                bool convert = false, bool hack = false, bool vland = false, bool presence = false,
                                bool gate = false, bool squad = false, bool squad_garrison = false) {
    auto d = std::make_unique<Data>();
    d->root = fs::temp_directory_path() / (std::string(g_temp_prefix) + (squad_garrison ? "_squad_garrison"  // s78n review
                                           : squad ? "_squad"  // s78n t03 / t09
                                           : gate ? "_gate"  // cu8g t05
                                           : presence ? "_presence"  // s78l t03
                                           : vland       ? "_vehicle_landing"  // s78l t04
                                           : hack      ? "_team_hack"  // s78l t04
                                           : convert     ? "_team_convert"  // s78k t04
                                           : sprint      ? "_team_sprint"  // s78j t03 / t04
                                           : landing     ? "_landing"  // a1i t04
                                           : bomb ? "_bomb"  // S8 t10 follow-up
                                           : fighters && fog ? "_fighters_fog"  // S7 t22
                                           : fighters    ? "_fighters"  // S7 t12
                                           : fog_setup ? "_fog_setup"  // FOWF t05
                                           : fog_bm ? "_fog_bm"  // FOW t01
                                           : obstacle ? "_obstacle"  // refixf t02
                                           : team     ? "_team"  // refixf t03: TEAM containers
                                           : prod   ? "_prod"  // station production
                                           : s4   ? "_s4"  // S4
                                           : bm   ? "_bm"  // P4 tactical
                                           : anim ? "_anim"
                                           : fog  ? "_fog"
                                                  : ""));
    std::error_code ec;
    remove_all_long(d->root, ec);
    const fs::path x = d->root / "Data/XML";
    write_file(x / "GameObjectFiles.xml", "<GameObjectFiles><File>S10Types.xml</File></GameObjectFiles>\n");
    write_file(x / "S10Types.xml", squad || squad_garrison ? squad_types(squad_garrison)  // s78n t03 / t09, s78n review
                                   : gate ? gate_types()  // cu8g t05
                                   : presence ? presence_types()  // s78l t03
                                   : vland ? vehicle_landing_types()  // s78l t04
                                   : hack ? hack_types()  // s78l t04
                                   : convert ? convert_types()  // s78k t04
                                   : sprint ? sprint_types()  // s78j t03 / t04
                                   : landing ? landing_types()  // a1i t04
                                   : bomb ? bomb_types()  // S8 t10 follow-up
                                   : fighters ? fighter_types(fog)  // S7 t12 / t22
                                    : fog_setup ? fog_types()
                                   : fog_bm ? fog_bm_types()
                                   : obstacle ? obstacle_types()
                                   : team     ? team_types()
                                   : prod   ? prod_types()
                                   : s4     ? s4_types()
                                   : bm     ? bm_types()
                                   : anim   ? anim_types()
                                   : fog    ? fog_types()
                                            : std::string(kTypes));
    if (bm) write_file(x / "BlackMarketItems.xml", kBmItems);  // P4 tactical
    write_file(x / "HardPointDataFiles.xml", fighters ? "<Hard_Point_Files><File>S10HardPoints.xml</File><File>S10BayHardPoints.xml</File></Hard_Point_Files>\n"
                                                      : "<Hard_Point_Files><File>S10HardPoints.xml</File></Hard_Point_Files>\n");
    write_file(x / "S10HardPoints.xml", kHardpoints);
    if (fighters)  // S7 t24: the carrier's fighter bay
        write_file(x / "S10BayHardPoints.xml", "<HardPoints><HardPoint Name=\"HP_S10_Bay\"><Type>HARD_POINT_FIGHTER_BAY</Type><Health>200</Health>"
                                               "<Fighter_Bay_Flyout_Distance>100</Fighter_Bay_Flyout_Distance></HardPoint></HardPoints>\n");
    write_file(x / "FactionFiles.xml", "<FactionFiles><File>S10Factions.xml</File></FactionFiles>\n");
    std::string factions = kFactions;
    // S10 map owners: Hostile (3), a native faction (4) and a playable faction without a player (5). modc t03: the first
    // native faction (not playable, neutral or planet-controlling) owns the natives, so Hostile comes first as in FoC.
    if (s4)
        factions.insert(factions.find("</Factions>"), " <Faction Name=\"Hostile\"></Faction>\n <Faction Name=\"S10_Natives\"></Faction>\n"
                                                      " <Faction Name=\"S10_Absent\"><Is_Playable>Yes</Is_Playable></Faction>\n");
    write_file(x / "S10Factions.xml", factions);
    if (!s4) {
        write_file(x / "TradeRouteFiles.xml", "<TradeRouteFiles></TradeRouteFiles>\n");
        write_file(x / "CampaignFiles.xml", "<CampaignFiles></CampaignFiles>\n");
    } else {  // S10 map owners: make_setup(land) as a campaign definition (build_campaign_setup adds the Hostile player)
        write_file(x / "TradeRouteFiles.xml", "<TradeRouteFiles><File>S10Routes.xml</File></TradeRouteFiles>\n");
        write_file(x / "S10Routes.xml",
                   "<TradeRoutes><TradeRoute Name=\"A_B\"><Point_A>S10_A</Point_A><Point_B>S10_B</Point_B><HS_Speed_Factor>5.0</HS_Speed_Factor></TradeRoute>"
                   "<TradeRoute Name=\"B_C\"><Point_A>S10_B</Point_A><Point_B>S10_C</Point_B><HS_Speed_Factor>5.0</HS_Speed_Factor></TradeRoute></TradeRoutes>\n");
        write_file(x / "CampaignFiles.xml", "<CampaignFiles><File>S10Campaigns.xml</File></CampaignFiles>\n");
        write_file(x / "S10Campaigns.xml",
                   "<Campaigns><Campaign Name=\"S10_GC\"><Locations>S10_A, S10_B, S10_C</Locations><Trade_Routes>A_B, B_C</Trade_Routes>"
                   "<Starting_Active_Player>Empire</Starting_Active_Player><Starting_Credits>Underworld, 1000</Starting_Credits>"
                   "<AI_Player_Control>Empire, Human</AI_Player_Control><AI_Player_Control>Underworld, None</AI_Player_Control>"
                   "<Starting_Forces>Empire, S10_B, S10_Squad</Starting_Forces><Starting_Forces>Underworld, S10_C, S10_Frigate</Starting_Forces>"
                   "<Starting_Forces>Underworld, S10_C, S10_Squad</Starting_Forces><Starting_Forces>Underworld, S10_C, S10_Squad</Starting_Forces>"
                   "</Campaign></Campaigns>\n");
    }
    write_file(x / "GameConstants.xml",
               "<GameConstants>\n <Damage_Types>Damage_Default, Damage_Laser</Damage_Types>\n"
               " <Armor_Types>Armor_Default, Armor_Ship</Armor_Types>\n"
               " <Damage_To_Armor_Mod>Damage_Laser, Armor_Ship, 0.8</Damage_To_Armor_Mod>\n"
               " <AutoResolveLoserAttrition>0.80</AutoResolveLoserAttrition>\n"
               " <Space_Retreat_Allowed_Countdown_Seconds>5.0</Space_Retreat_Allowed_Countdown_Seconds>\n" +
                   std::string(landing || sprint || convert || hack || vland || squad || squad_garrison ? " <Use_Reinforcement_Points>No</Use_Reinforcement_Points>\n" : "") +  // a1i t04: no points on the map
                   std::string(fog_setup ? " <SetupPhaseEnabled>True</SetupPhaseEnabled>\n"  // FOWF t05: a mod's land setup phase
                                           " <SetupPhaseCountdownSeconds>3</SetupPhaseCountdownSeconds>\n"
                                         : "") +
                   "</GameConstants>\n");
    write_file(d->root / "Data/Scripts/Library/PGAICommands.lua", kLua);
    d->files = std::make_unique<DirectoryProvider>(path_to_utf8(d->root));
    std::vector<std::string> diag;
    CHECK(d->db.load(*d->files, {}, &diag));
    CHECK(d->raw.load(*d->files));
    CHECK(d->types.build(d->raw, d->files.get(), &diag));
    CHECK(galactic::load_galactic_rules(*d->files, d->db, d->types, d->rules, &diag));
    CHECK(galactic::load_conflict_rules(*d->files, d->db, d->types, d->rules, d->crules, &diag));
    d->catalog = std::make_unique<BattleCatalog>(d->types, d->files.get(), &d->db.constants(), &diag);
    CHECK(d->crules.contrast.size() == 3);
    CHECK(d->catalog->hardpoint_records() == (fighters ? 3u : 2u));
    {
        std::vector<uint32_t> idx;
        const auto cd = d->catalog->build({static_cast<uint32_t>(d->types.resolve_name("S10_Frigate")), static_cast<uint32_t>(d->types.resolve_name("S10_Trooper"))}, idx);
        std::printf("  catalog: units %zu hardpoints %zu projectiles %zu; frigate hps %zu range %.0f; hp0 kind %d proj %d range %.0f; trooper weapon %d\n", cd.units.size(), cd.hardpoints.size(), cd.projectiles.size(), cd.units[0].hardpoints.size(), static_cast<double>(cd.units[0].max_attack_distance), cd.hardpoints.empty() ? -1 : static_cast<int>(cd.hardpoints[0].kind), cd.hardpoints.empty() ? -9 : cd.hardpoints[0].projectile, cd.hardpoints.empty() ? 0.0 : static_cast<double>(cd.hardpoints[0].fire_range), cd.units[1].weapon_projectile);
        CHECK(cd.units.size() == 2 && cd.units[0].hardpoints.size() == 2 && cd.hardpoints.size() == 2 && cd.projectiles.size() == 2);
        CHECK(!cd.hardpoints.empty() && cd.hardpoints[0].projectile == 0 && cd.units[1].weapon_projectile == 1);
    }
    return d;
}

inline terrain::MapData space_map() {
    terrain::MapData m;
    m.space.present = true;
    for (int i = 0; i < 3; ++i) {
        m.space.playable_min[i] = -4000.0f;
        m.space.playable_max[i] = 4000.0f;
    }
    m.playable.present = m.playable.has_outer_box = true;
    for (int i = 0; i < 3; ++i) {
        m.playable.outer_min[i] = -4000.0f;
        m.playable.outer_max[i] = 4000.0f;
    }
    return m;
}

inline terrain::MapData land_map() {
    terrain::MapData m;
    m.terrain.present = true;
    m.terrain.width = m.terrain.height = 63;
    m.terrain.heights.assign(63u * 63u, 0);
    m.terrain.material.assign(63u * 63u, 0);
    return m;
}

inline uint32_t type_of(const Data& d, const char* name) { return static_cast<uint32_t>(d.types.resolve_name(name)); }

inline galactic::GalacticSetup make_setup(const Data& d, bool land) {
    galactic::GalacticSetup s;
    GalaxyMap& m = s.map;
    m.campaign_name = "S10";
    const char* names[] = {"S10_A", "S10_B", "S10_C"};
    for (uint32_t i = 0; i < 3; ++i) {
        m.planets.add(names[i]);
        m.planets.position[i] = {static_cast<float>(i) * 60.0f, 0.0f, 0.0f};
        m.planets.flags[i] = static_cast<uint32_t>(kPlanetHasPlanetBehavior);
    }
    auto route = [&m](const char* n, PlanetIndex a, PlanetIndex b) {
        const uint32_t r = m.routes.add(n);
        m.routes.point_a[r] = a;
        m.routes.point_b[r] = b;
        m.routes.hs_speed_factor[r] = 5.0f;
    };
    route("A_B", 0, 1);
    route("B_C", 1, 2);
    s.planet_type = {type_of(d, "S10_A"), type_of(d, "S10_B"), type_of(d, "S10_C")};
    galactic::PlayerSetup e;
    e.faction = d.rules.find_faction("Empire");
    e.human = true;
    galactic::PlayerSetup u;
    u.faction = d.rules.find_faction("Underworld");
    u.ai = true;
    u.credits = 1000.0f;
    galactic::PlayerSetup n;
    n.faction = d.rules.find_faction("Neutral");
    s.players = {e, u, n};
    const uint32_t fr = type_of(d, "S10_Frigate"), co = type_of(d, "S10_Corvette"), sb = type_of(d, "S10_Star_Base_1"),
                   sq = type_of(d, "S10_Squad");
    if (!land) {
        s.forces = {{0, 1, fr}, {0, 1, co}, {0, 1, sb}, {1, 2, fr}, {1, 2, fr}, {1, 2, co}, {1, 2, co}, {1, 2, co}, {1, 2, co}};
    } else {
        s.forces = {{0, 1, sq}, {1, 2, fr}, {1, 2, sq}, {1, 2, sq}};
    }
    return s;
}

struct Run {
    CrcLog crc;
    std::vector<std::string> battles, conflicts;
    uint32_t fought = 0;
    size_t depth = 0;
    int won = 0, lost = 0;
    bool trace_battles = false;
    bool empire_frigate_alive = false;
    uint32_t hold_ticks = 0;  // Playtest QA D1: ticks with the result decided, not yet applied
    std::vector<TacticalBattle::RemovalRecord> losses;
    int under_surface = 0, empire_surface = 0;
    float credits_before = -1.0f, credits_in_battle = -1.0f, credits_after = -1.0f;
    // S10-int
    int ability_command = -1;  // RejectReason of the command 43 sent once the space battle runs
    uint32_t activations = 0, particles = 0, ability_records = 0;
    // Fog of war (owner: fog agent)
    uint64_t fog_hash = 0;  // digest of the per-frame FogOfWar::hash digests
    uint32_t fog_frames = 0, fog_sources_max = 0, fog_visible_max = 0, fog_hidden = 0, fog_shown = 0, fog_refogs = 0;
    bool fog_grids_ok = true, fog_ai_unfogged = true;
    // fog follow-up: S8 targeting filter (human targets acquired while hidden), S9 ping / Force Sight
    std::map<int32_t, int32_t> fog_last_target;  // human unit -> auto target after the previous tick
    uint32_t fog_new_targets = 0, fog_bad_targets = 0, fog_force_frames = 0;
    uint32_t fog_ability_frame = 0;  // battle frame the ping / Force Sight was activated (0 = not used)
    float fog_probe_x = 0.0f, fog_probe_y = 0.0f;  // the user's position mirrored halfway through the map centre
    bool fog_probe_before = false, fog_probe_after = false, fog_force_on = false;
    // OI fog: shots (REVEAL vf26 flashes) seen, host API round trips (FOW-active flag, reinforcement overlay)
    uint64_t fog_shots = 0, fog_flashed = 0;
    bool fog_api_ok = true, fog_api_checked = false;
    // FOW t01: Battlefield_Modifier FOW_Reveal_Range_Multiplier (fog_bm data: S10_FOW_Boost on the corvettes), per player
    uint32_t bm_on_frames[2] = {0, 0}, bm_off_after_on[2] = {0, 0}, bm_bad_frames = 0;
    float bm_r_on = 0.0f, bm_r_off = 0.0f;  // a REVEAL unit's sight radius with / without the bonus
    bool bm_saved = false, bm_save_ok = false, bm_grid_ok = false;
};

#ifdef ALAMO_SIM_HAVE_AI_TACTICAL
inline void battles_budget_hook(CampaignBattleOptions& opt, std::vector<ai::tactical::TacticalBudgetCategory>& budget,
                         const galactic::GalacticSim& gs) {
    opt.tactical_budget = [&budget, &gs](int32_t player) {
        budget[0].allowance = 0.5f;
        budget[0].available = gs.credits(player);
        budget[0].pool = gs.credits(player);
        return std::span<ai::tactical::TacticalBudgetCategory>(budget);
    };
}
#endif

// Fog of war probe after each tick (owner: fog agent): player 0 = human Empire, 1 = AI Underworld, 2 = neutral.
inline void fog_probe(TacticalBattle& b, TacticalMode& tm, Run& out, StateHasher& acc, bool& refog_set) {
    tactical::fog::FogOfWar* f = b.fog();
    if (!f) return;
    if (!refog_set) {
        b.set_fog_refog([&out](int32_t, uint32_t, float, float) { ++out.fog_refogs; });
        refog_set = true;
    }
    ++out.fog_frames;
    out.fog_grids_ok = out.fog_grids_ok && f->grid(0) && f->grid(1) && !f->grid(2);
    out.fog_sources_max = std::max(out.fog_sources_max, static_cast<uint32_t>(f->source_count()));
    if (const tactical::fog::FogGrid* g = f->grid(0)) {
        uint32_t vis = 0;
        for (uint8_t s : g->state()) vis += s != 0 ? 1u : 0u;
        out.fog_visible_max = std::max(out.fog_visible_max, vis);
    }
    const objects::ObjectStore& st = tm.primary();
    for (objects::ObjectHandle h : st.owned_by(1)) {
        if (!st.alive(h)) continue;
        const Vec3 p = st.position(h);
        out.fog_ai_unfogged = out.fog_ai_unfogged && !b.fog_point_fogged(1, p.x + 3000.0f, p.y, false);
        if (b.fog_object_hidden(0, st.id(h), false)) ++out.fog_hidden;
        else ++out.fog_shown;
    }
    // fog follow-up (S8 step 4): a target the human acquired this tick was visible at its scan. Visibility can only
    // grow between the gather phase and the end of the tick (reveals / flashes raise states, removals only uncount),
    // so it must still be visible now.
    const tactical::combat::CombatSystem& c = b.combat();
    for (objects::ObjectHandle h : st.owned_by(0)) {
        const objects::ObjectId id = st.id(h);
        if (!st.alive(h) || !c.has_unit(id)) continue;
        const int32_t t = c.auto_target(id);
        const auto it = out.fog_last_target.find(id);
        const int32_t before = it == out.fog_last_target.end() ? tactical::combat::kNoObject : it->second;
        out.fog_last_target[id] = t;
        if (t == before || t == tactical::combat::kNoObject) continue;
        ++out.fog_new_targets;
        if (b.fog_object_hidden(0, t, false)) ++out.fog_bad_targets;
    }
    // fog follow-up (S9): Force Sight frames (Reveal_All on the human's grid); the ping's reveal one tick after use.
    if (f->grid(0) && f->grid(0)->reveal_all_active()) ++out.fog_force_frames;
    // OI fog: every shot this tick flashed its shooter on the target owner's grid (state != 0 at the shooter, unless the
    // shooter has no REVEAL or the target owner no grid).
    for (const tactical::combat::ShotEvent& e : c.shot_events()) {
        ++out.fog_shots;
        const objects::ObjectHandle sh = st.find(e.shooter);
        if (!st.alive(sh) || !st.has_behavior(sh, objects::behavior_type_id("REVEAL")) || !f->grid(e.target_owner)) continue;
        const Vec3 p = st.position(sh);
        if (f->grid(e.target_owner)->state_at(f->grid(e.target_owner)->index_of(p.x, p.y)) != 0) ++out.fog_flashed;
        else out.fog_api_ok = false;
    }
    // OI fog: host API between ticks at battle frame 100 (restored before the next tick).
    if (b.frame() == 100) {
        out.fog_api_checked = true;
        const bool was = b.fog_active();
        b.set_fog_active(false, false);
        out.fog_api_ok = out.fog_api_ok && was && !b.fog_active() && !b.fog_render_option() && !b.fog_point_fogged(0, 0.0f, 0.0f, true);
        b.set_fog_active(true, true);
        out.fog_api_ok = out.fog_api_ok && b.fog_active() && !b.fog_setup_phase();
        std::vector<uint8_t> ov;
        out.fog_api_ok = out.fog_api_ok && b.fog_reinforce_overlay(0, ov) && f->grid(0) && ov.size() == f->grid(0)->cells() &&
                         !b.fog_reinforce_overlay(2, ov);
    }
    if (out.fog_ability_frame && b.frame() == out.fog_ability_frame + 1)
        out.fog_probe_after = !b.fog_point_fogged(0, out.fog_probe_x, out.fog_probe_y, false);
    StateHasher h;
    f->hash(h);
    acc.u64(h.digest());
}

// FOW t01: the Battlefield_Modifier reveal list end to end. While a player has a live S10_FOW_Boost corvette its +0x1f8
// bonus is 0.5 (MostExtreme in one category: several corvettes count once) and its REVEAL units reveal with sight x 1.5;
// when the last one dies the bonus is 0 again. Once, with the bonus on: the grid reveal of a REVEAL unit covers the cells
// of a 1.5 x sight circle (on a copy read back from FogOfWar::write, so the copy also checks the list survives a save),
// and the battle's Fog save part reads back into the running battle unchanged. Read-only otherwise.
inline void fog_bm_probe(TacticalBattle& b, TacticalMode& tm, Run& out) {
    tactical::fog::FogOfWar* f = b.fog();
    if (!f) return;
    const objects::ObjectStore& st = tm.primary();
    const objects::TypeIndex corvette = tm.types().resolve_name("S10_Corvette");
    for (int32_t p = 0; p < 2; ++p) {
        bool live = false;
        for (objects::ObjectHandle h : st.owned_by(p)) live = live || (st.alive(h) && st.active(h) && st.type(h) == corvette);  // not a death clone
        const float bonus = f->player_reveal_bonus(p);
        if (bonus != (live ? 0.5f : 0.0f)) ++out.bm_bad_frames;
        if (bonus == 0.5f) ++out.bm_on_frames[p];
        else if (out.bm_on_frames[p] > 0) ++out.bm_off_after_on[p];
        for (objects::ObjectHandle h : st.owned_by(p)) {
            tactical::fog::FogObject o;
            if (!st.alive(h) || !f->has_source(st.id(h)) || !b.fog_object(st.id(h), o) || o.object_modifier != 0.0f) continue;
            float cx = 0.0f, cy = 0.0f, r = 0.0f, rd = 0.0f;
            f->sight_radii(p, o, false, cx, cy, r, rd);
            if (r != o.sight_range * (1.0f + bonus)) ++out.bm_bad_frames;
            (bonus == 0.5f ? out.bm_r_on : out.bm_r_off) = r;
            if (out.bm_saved || bonus != 0.5f || p != 0 || b.frame() < 30) continue;
            out.bm_saved = true;
            StateHasher before;
            f->hash(before);
            ByteWriter w;
            f->write(w);
            tactical::fog::FogOfWar copy;
            ByteReader r1(w.bytes());
            StateHasher copied;
            const bool read = copy.read(r1) && r1.done();
            copy.hash(copied);
            tactical::fog::CellList cells;
            copy.reveal_object(0, o, cells, false);
            const tactical::fog::FogCells wide = copy.lua_reveal_position(0, o.x, o.y, 1.5f * o.sight_range, 1.5f * o.sight_range * o.dense_multiplier);
            const tactical::fog::FogCells base = copy.lua_reveal_position(0, o.x, o.y, o.sight_range, o.sight_range * o.dense_multiplier);
            out.bm_grid_ok = read && cells.size() == wide.cells.size() && cells.size() > base.cells.size();
            ByteWriter part;
            b.write_part(TacticalBattle::SavePart::Fog, part);
            ByteReader r2(part.bytes());
            StateHasher after;
            const bool loaded = b.read_part(TacticalBattle::SavePart::Fog, r2);
            f = b.fog();
            if (f) f->hash(after);
            out.bm_save_ok = read && copy.player_reveal_bonus(0) == 0.5f && copied.digest() == before.digest() && loaded && f &&
                             f->player_reveal_bonus(0) == 0.5f && after.digest() == before.digest();
            if (!f) return;
        }
    }
}

// fog follow-up (S9 fog hooks): at battle frame 30 the human's first unit with FOW_REVEAL_PING (space frigates) /
// FORCE_SIGHT (land troopers) uses it through the battle's AbilitySystem (the fog test data has them).
inline void fog_use_ability(TacticalBattle& b, TacticalMode& tm, bool land, Run& out) {
    if (!b.fog()) return;
    const objects::ObjectStore& st = tm.primary();
    const uint8_t type = land ? 52 : 28;  // FORCE_SIGHT / FOW_REVEAL_PING
    for (objects::ObjectHandle h : st.owned_by(0)) {
        const objects::ObjectId id = st.id(h);
        if (!st.alive(h) || !b.abilities().has_ability(id, type)) continue;
        const Vec3 p = st.position(h);
        // Mirrored halfway through the playable box centre: 1.5 x the unit's distance from the centre, inside the box.
        const float cx = 0.5f * (tm.bounds().min.x + tm.bounds().max.x);
        out.fog_probe_x = cx - 0.5f * (p.x - cx);
        out.fog_probe_y = p.y;
        out.fog_probe_before = b.fog_point_fogged(0, out.fog_probe_x, out.fog_probe_y, false);
        const tactical::combat::ObjectId ids[] = {id};
        if (b.abilities().set_unit_ability_mode(ids, type, true, b.frame()) == 1) out.fog_ability_frame = b.frame();
        out.fog_force_on = land && b.fog()->grid(0) && b.fog()->grid(0)->reveal_all_active();
        return;
    }
}

inline Run run_campaign(const Data& d, bool land, bool fight, int32_t workers, uint64_t ticks, float retreat_ratio = 0.0f, bool fog = true,
                 uint32_t end_hold_frames = 0) {
    Run out;
    StateHasher fog_acc;
    bool fog_refog_set = false;
    jobs::JobSystem js(jobs::JobSystemConfig{workers});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    SimHost host(d.types, js, cfg);
    galactic::GalacticSim* gs = host.start_galactic(d.rules, make_setup(d, land));
    CHECK(gs != nullptr);
    if (!gs) return out;
    gs->set_emit_events(true);
    CampaignBattleOptions opt;
    opt.conflict.human_choice = fight ? galactic::BattleChoice::Fight : galactic::BattleChoice::AutoResolve;
    opt.battle.max_frames = 6000;
    opt.battle.retreat_ratio = retreat_ratio;
    opt.battle.end_hold_frames = end_hold_frames;
    opt.battle.fog = fog;  // fog of war (owner: fog agent)
#ifdef ALAMO_SIM_HAVE_AI_TACTICAL
    // A1 credit transfer with a stand-in budget: one category, allowance 0.5 of the AI's credits.
    std::vector<ai::tactical::TacticalBudgetCategory> budget(1);
    battles_budget_hook(opt, budget, *gs);
#endif
    CampaignBattles battles(host, d.crules, *d.catalog,
                            [](const std::string& name, terrain::MapData& m) {
                                if (name == "S10_Space_B") m = space_map();
                                else if (name == "S10_Land_B") m = land_map();
                                else return false;
                                return true;
                            },
                            opt);
    std::vector<std::string> diag;
    CHECK(battles.attach(&diag));
    // Underworld: the fleet at C moves to B (AI direct order before the first tick); on land its squads ride along.
    if (land) {
        std::vector<objects::ObjectHandle> up;
        for (objects::ObjectHandle h : gs->planets()[2].surface)
            if (gs->store().owner(h) == 1) up.push_back(h);
        CHECK(gs->launch_units(1, 2, up) == commands::RejectReason::None);
    }
    const std::vector<uint32_t> uf = gs->fleets_at(2, 1);
    CHECK(!uf.empty());
    if (!uf.empty()) CHECK(gs->move_fleet(1, uf[0], 1) == commands::RejectReason::None);
    std::vector<std::string> trace;
    bool invaded = false;
    std::vector<galactic::GalacticEvent> events;
    for (uint64_t t = 0; t < ticks; ++t) {
        if (t == 5) {
            host.set_trace(&trace);
        } else if (t == 6) {
            host.set_trace(nullptr);
        }
        const size_t depth_before = host.modes().depth();
        const float cr = gs->credits(1);
        host.tick_once();
        if (battles.battle() && battles.battle()->end_hold_active()) ++out.hold_ticks;
        if (depth_before == 1 && host.modes().depth() == 2 && out.credits_in_battle < 0.0f) {
            out.credits_before = cr;
            out.credits_in_battle = gs->credits(1);
        }
        if (depth_before == 2 && host.modes().depth() == 1 && out.credits_after < 0.0f) out.credits_after = gs->credits(1);
        // S10-int: the human Empire activates POWER_TO_WEAPONS (command 43, direct path) on its frigates.
        if (!land && out.ability_command < 0 && battles.battle() && battles.battle()->frame() == 10) {
            TacticalMode* tm = host.tactical();
            commands::EventData ev(commands::EventType::SetUnitAbilityMode);
            std::vector<uint32_t> ids;
            for (objects::ObjectHandle h : tm->primary().owned_by(0))
                if (tm->primary().alive(h)) ids.push_back(static_cast<uint32_t>(tm->primary().id(h)));
            ev.set("ability", 29).set("on", 1).set_ids("units", ids);
            out.ability_command = static_cast<int>(host.execute_direct(0, commands::CommandSource::Player, ev));
            for (const auto& o : battles.battle()->tables().abilities.objects) out.ability_records += static_cast<uint32_t>(o.unit_abilities.size());
        }
        if (fog && !out.fog_ability_frame && battles.battle() && battles.battle()->frame() == 30 && host.tactical())
            fog_use_ability(*battles.battle(), *host.tactical(), land, out);  // fog follow-up (S9)
        if (battles.battle()) {
            out.activations = battles.battle()->stats().activations;
            out.particles = battles.battle()->stats().particles;
            if (TacticalMode* tm = host.tactical()) {
                fog_probe(*battles.battle(), *tm, out, fog_acc, fog_refog_set);
                fog_bm_probe(*battles.battle(), *tm, out);  // FOW t01
            }
        }
        if (std::getenv("S10_DEBUG") && battles.battle() && (t % 300) == 0) {
            const auto& c = battles.battle()->combat();
            const auto& mv = battles.battle()->movement();
            for (int32_t id = 1; id < 200; ++id) {
                if (!c.has_unit(id)) continue;
                const auto v = mv.view(id);
                std::printf("   t%llu id %d hp %.0f pos %.0f,%.0f goal %d tgt %d range %.0f\n", (unsigned long long)t, id, c.health(id), v.pos.x, v.pos.y, v.has_goal ? 1 : 0, c.auto_target(id), 0.0);
            }
        }
        gs->drain_events(events);
        // Land scenario: once in orbit (no defending fleet: no space battle), the invaders go down.
        if (land && !invaded && host.modes().depth() == 1) {
            const std::vector<uint32_t> f = gs->fleets_at(1, 1);
            if (!f.empty() && !gs->fleets()[f[0]].moving) {
                CHECK(gs->invade(1, 1, {}) == commands::RejectReason::None);
                CHECK(battles.conflicts().land_queue().size() == 1);
                invaded = true;
            }
        }
    }
    out.fog_hash = fog_acc.digest();
    for (const std::string& s : trace) out.trace_battles = out.trace_battles || s == "T5.battles";
    if (battles.battle()) std::printf("  (battle still running: %s)\n", battles.battle()->summary().c_str());
    out.crc = host.crc_log();
    out.battles = battles.log();
    out.losses.assign(battles.last_battle_losses().begin(), battles.last_battle_losses().end());
    for (const auto& e : battles.conflicts().log()) out.conflicts.push_back(e.text(*gs));
    out.fought = battles.battles_fought();
    out.depth = host.modes().depth();
    for (const auto& e : events) {
        out.won += e.kind == galactic::GalacticEventKind::BattleWon;
        out.lost += e.kind == galactic::GalacticEventKind::BattleLost;
    }
    for (const galactic::FleetState& f : gs->fleets())
        if (f.alive && f.owner == 0)
            for (objects::ObjectHandle h : f.contents)
                out.empire_frigate_alive = out.empire_frigate_alive || (gs->store().alive(h) && gs->store().type(h) == d.types.resolve_name("S10_Frigate"));
    for (objects::ObjectHandle h : gs->planets()[1].surface) {
        if (!gs->store().alive(h)) continue;
        out.under_surface += gs->store().owner(h) == 1;
        out.empire_surface += gs->store().owner(h) == 0;
    }
    return out;
}

inline std::string golden_path() { return std::string(ALAMO_SIM_TESTDATA) + "/battle_synthetic_golden.crc"; }


}  // namespace battle_fixture

using namespace battle_fixture;
