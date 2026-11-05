// alamo_sim_battle_tests: S10 battle transitions, host side (docs/re/BATTLE_TRANSITIONS.md).
//   * synthetic game data (types, hardpoints, projectiles, factions, contrast table) + a 3-planet galaxy;
//   * space: a human-defended orbit is attacked, the conflict is FOUGHT: tactical Space mode pushed over the
//     suspended galaxy, MovementSystem + CombatSystem run the battle on an in-memory map, the result is
//     written back (losses killed in the galaxy, loser retreats), mode popped, story events posted;
//   * land: an invasion against a defended surface is fought on an in-memory land map; a winning
//     attacker lands its survivors;
//   * the same campaign with auto-resolve only;
//   * per-tick CRC logs identical for 0/1/4/N workers and equal to testdata/battle_synthetic_golden.crc
//     (MSVC writes it with --regen; clang / gcc must reproduce it).
// With ALAMO_FOC_DATA (FoC "corruption" folder) a real FoC space map battle runs as well.
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
#include "alamo/galactic/corruption.hpp"  // S4
#include "alamo/galactic/galactic_rules.hpp"
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
#ifdef ALAMO_SIM_HAVE_GARRISON
#include "alamo/tactical/garrison/garrison_types.hpp"  // S4
#endif
#ifdef ALAMO_SIM_HAVE_ANIM
#include "alamo/sim/sim_anim.hpp"
#endif

using namespace alamo;
using namespace alamo::sim;
namespace fs = std::filesystem;

static int g_fail = 0;
static bool g_regen = false;
#define CHECK(c)                                                     \
    do {                                                             \
        if (!(c)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++g_fail;                                                \
        }                                                            \
    } while (0)

namespace {

void write_file(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << s;
}

const char* kTypes = R"(<S10>
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

const char* kHardpoints = R"(<HardPoints>
 <HardPoint Name="HP_S10_Laser_A"><Type>HARD_POINT_WEAPON_LASER</Type><Health>150</Health>
  <Is_Targetable>Yes</Is_Targetable><Is_Destroyable>Yes</Is_Destroyable>
  <Fire_Projectile_Type>S10_Laser_Proj</Fire_Projectile_Type><Fire_Min_Recharge_Seconds>2.0</Fire_Min_Recharge_Seconds>
  <Fire_Max_Recharge_Seconds>3.0</Fire_Max_Recharge_Seconds><Fire_Pulse_Count>2</Fire_Pulse_Count>
  <Fire_Pulse_Delay_Seconds>0.2</Fire_Pulse_Delay_Seconds><Fire_Range_Distance>1400</Fire_Range_Distance><Fire_Cone_Width>360</Fire_Cone_Width><Fire_Cone_Height>180</Fire_Cone_Height>
  <Fire_Inaccuracy_Distance>Corvette, 5.0</Fire_Inaccuracy_Distance></HardPoint>
 <HardPoint Name="HP_S10_Laser_B"><Variant_Of_Existing_Type>HP_S10_Laser_A</Variant_Of_Existing_Type><Health>100</Health></HardPoint>
</HardPoints>
)";

const char* kFactions = R"(<Factions>
 <Faction Name="Empire"><Is_Playable>Yes</Is_Playable><Can_Control_Planets>Yes</Can_Control_Planets>
  <Space_Tactical_Unit_Cap>6</Space_Tactical_Unit_Cap><Space_Retreat_Countdown_Seconds>5</Space_Retreat_Countdown_Seconds></Faction>
 <Faction Name="Underworld"><Is_Playable>Yes</Is_Playable><Can_Control_Planets>Yes</Can_Control_Planets>
  <Space_Tactical_Unit_Cap>6</Space_Tactical_Unit_Cap></Faction>
 <Faction Name="Neutral"><Is_Neutral>Yes</Is_Neutral></Faction>
</Factions>
)";

const char* kLua = R"(function Set_Contrast_Values()
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
std::string fog_types() {
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
std::string fog_bm_types() {
    std::string t = fog_types();
    const std::string at = "<CategoryMask>Corvette</CategoryMask>";
    const size_t p = t.find(at);
    if (p != std::string::npos)
        t.insert(p + at.size(), "<Abilities SubObjectList=\"Yes\"><Battlefield_Modifier_Ability Name=\"S10_FOW_Boost\">"
                                "<Activation_Style>Space_Automatic</Activation_Style><FOW_Reveal_Range_Multiplier>1.5</FOW_Reveal_Range_Multiplier>"
                                "<Stacking_Category>0</Stacking_Category></Battlefield_Modifier_Ability></Abilities>");
    return t;
}

// R3 anim test data: the same types plus corpse / removal fields and a frigate death clone.
std::string anim_types() {
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

// refixf t03: TEAM containers in land battles: S10_Trooper has Create_Team and S10_Squad no Create_Team_Type, so each
// squad chunk gets one object of the type named "Team" whose representative type (0x3973b0) is S10_Squad.
std::string team_types() {
    std::string t = kTypes;
    const std::string at = "<GroundInfantry Name=\"S10_Trooper\">";
    const size_t p = t.find(at);
    if (p != std::string::npos) t.insert(p + at.size(), "<Create_Team>Yes</Create_Team>");
    const size_t end = t.find("</S10>");
    if (end != std::string::npos) t.insert(end, " <Team Name=\"Team\"><Behavior>TEAM</Behavior></Team>\n");
    return t;
}

// a1i t04: the team data plus a landing transport: S10_Squad (Max_Squad_Size 2: two teams) names Company_Transport_Unit
// S10_Lander, whose Landing_Transport_Variant S10_Lander_Landing (TRANSPORT, Hover_Offset 10 inherited) carries it down.
std::string landing_types() {
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
std::string sprint_types() {
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
std::string convert_types() {
    std::string t = team_types();
    const std::string at = "<Projectile Name=\"S10_Blaster_Proj\">";
    if (const size_t p = t.find(at); p != std::string::npos)
        t.insert(p + at.size(), "<Projectile_Convert_Enemy_On_Detonation>Yes</Projectile_Convert_Enemy_On_Detonation>"
                                "<Projectile_Convert_Enemy_Radius>30</Projectile_Convert_Enemy_Radius>"
                                "<Projectile_Convert_Enemy_Targets_Category_Mask>Infantry</Projectile_Convert_Enemy_Targets_Category_Mask>");
    return t;
}

// --- S4 (owner: S4 agent): garrison / capture / corruption test data: troopers are GARRISON_UNIT (Infantry); a
// community bunker (GARRISON_STRUCTURE, GarrisonCanFire) and a capture post (CAPTURE_POINT, 1 s transition) are
// placed on the land map; planet S10_B offers a Kidnapping corruption mission (land, S10_Land_B, mission plots) and a
// Piracy corruption without a mission.
std::string s4_types() {
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
                "<Capture_Point_Transition_Time_Seconds>1.0</Capture_Point_Transition_Time_Seconds></Marker>\n"
                // S10 map owners: a landing reinforcement point (REMAINING_GAPS §3.3 probe 2026-10-29)
                " <Marker Name=\"S4_RPoint\"><Behavior>CAPTURE_POINT, REINFORCEMENT_POINT</Behavior><Capture_Point_Radius>100</Capture_Point_Radius>"
                "<Capture_Point_Transition_Time_Seconds>1.0</Capture_Point_Transition_Time_Seconds></Marker>\n"
                "</S10>");
    return t;
}
// --- end S4 ---

// --- P4 tactical: Black Market gate test data: the frigates' (and the corvette variant's) POWER_TO_WEAPONS record is
// Must_Be_Bought_On_Black_Market under the name S10_BM_Power, listed by the one item S10_BM_Item. ---
std::string bm_types() {
    std::string t = kTypes;
    const std::string at = "<Unit_Ability><Type>POWER_TO_WEAPONS</Type>";
    const size_t p = t.find(at);
    if (p != std::string::npos)
        t.insert(p + at.size(), "<GUI_Activated_Ability_Name>S10_BM_Power</GUI_Activated_Ability_Name>"
                                "<Must_Be_Bought_On_Black_Market>Yes</Must_Be_Bought_On_Black_Market>");
    return t;
}
const char* kBmItems = R"(<BlackMarketItems>
 <Item Name="S10_BM_Item"><Faction>Empire</Faction><Tech_Level>1</Tech_Level><Base_Cost_Credits>100</Base_Cost_Credits>
  <Ability_Names>S10_BM_Power</Ability_Names></Item>
</BlackMarketItems>
)";
// --- end P4 tactical ---

// Skirmish station production test data: the Empire star base lists S10_Base_Upgrade (DUMMY_UPGRADE with a
// Skirmish_Automatic Starbase_Upgrade_Ability, prerequisite S10_Star_Base_1, limit 1 for all allies) in its Empire row of
// Tactical_Buildable_Objects_Multiplayer, and upgrades to S10_Star_Base_2.
std::string prod_types() {
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
std::string obstacle_types() {
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
std::string fighter_types(bool fog = false) {
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
std::string bomb_types() {
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

std::unique_ptr<Data> make_data(bool fog = false, bool anim = false, bool s4 = false, bool bm = false, bool prod = false,
                                bool obstacle = false, bool team = false, bool fog_bm = false, bool fighters = false,
                                bool fog_setup = false, bool bomb = false, bool landing = false, bool sprint = false,
                                bool convert = false) {
    auto d = std::make_unique<Data>();
    d->root = fs::temp_directory_path() / (convert     ? "alamo_sim_battle_tests_team_convert"  // s78k t04
                                           : sprint      ? "alamo_sim_battle_tests_team_sprint"  // s78j t03 / t04
                                           : landing     ? "alamo_sim_battle_tests_landing"  // a1i t04
                                           : bomb ? "alamo_sim_battle_tests_bomb"  // S8 t10 follow-up
                                           : fighters && fog ? "alamo_sim_battle_tests_fighters_fog"  // S7 t22
                                           : fighters    ? "alamo_sim_battle_tests_fighters"  // S7 t12
                                           : fog_setup ? "alamo_sim_battle_tests_fog_setup"  // FOWF t05
                                           : fog_bm ? "alamo_sim_battle_tests_fog_bm"  // FOW t01
                                           : obstacle ? "alamo_sim_battle_tests_obstacle"  // refixf t02
                                           : team     ? "alamo_sim_battle_tests_team"  // refixf t03: TEAM containers
                                           : prod   ? "alamo_sim_battle_tests_prod"  // station production
                                           : s4   ? "alamo_sim_battle_tests_s4"  // S4
                                           : bm   ? "alamo_sim_battle_tests_bm"  // P4 tactical
                                           : anim ? "alamo_sim_battle_tests_anim"
                                           : fog  ? "alamo_sim_battle_tests_fog"
                                                  : "alamo_sim_battle_tests");
    std::error_code ec;
    remove_all_long(d->root, ec);
    const fs::path x = d->root / "Data/XML";
    write_file(x / "GameObjectFiles.xml", "<GameObjectFiles><File>S10Types.xml</File></GameObjectFiles>\n");
    write_file(x / "S10Types.xml", convert ? convert_types()  // s78k t04
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
    if (s4)  // S10 map owners: a native faction (3), Hostile (4) and a playable faction without a player (5)
        factions.insert(factions.find("</Factions>"), " <Faction Name=\"S10_Natives\"></Faction>\n <Faction Name=\"Hostile\"></Faction>\n"
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
                   std::string(landing || sprint || convert ? " <Use_Reinforcement_Points>No</Use_Reinforcement_Points>\n" : "") +  // a1i t04: no points on the map
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

terrain::MapData space_map() {
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

terrain::MapData land_map() {
    terrain::MapData m;
    m.terrain.present = true;
    m.terrain.width = m.terrain.height = 63;
    m.terrain.heights.assign(63u * 63u, 0);
    m.terrain.material.assign(63u * 63u, 0);
    return m;
}

uint32_t type_of(const Data& d, const char* name) { return static_cast<uint32_t>(d.types.resolve_name(name)); }

galactic::GalacticSetup make_setup(const Data& d, bool land) {
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
void battles_budget_hook(CampaignBattleOptions& opt, std::vector<ai::tactical::TacticalBudgetCategory>& budget,
                         const galactic::GalacticSim& gs) {
    opt.tactical_budget = [&budget, &gs](int32_t player) {
        budget[0].allowance = 0.5f;
        budget[0].available = gs.credits(player);
        budget[0].pool = gs.credits(player);
        return std::span<ai::tactical::TacticalBudgetCategory>(budget);
    };
}
#endif

#ifdef ALAMO_SIM_HAVE_FOG
// Fog of war probe after each tick (owner: fog agent): player 0 = human Empire, 1 = AI Underworld, 2 = neutral.
void fog_probe(TacticalBattle& b, TacticalMode& tm, Run& out, StateHasher& acc, bool& refog_set) {
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
void fog_bm_probe(TacticalBattle& b, TacticalMode& tm, Run& out) {
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
void fog_use_ability(TacticalBattle& b, TacticalMode& tm, bool land, Run& out) {
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
#endif

Run run_campaign(const Data& d, bool land, bool fight, int32_t workers, uint64_t ticks, float retreat_ratio = 0.0f, bool fog = true,
                 uint32_t end_hold_frames = 0) {
    Run out;
#ifdef ALAMO_SIM_HAVE_FOG
    StateHasher fog_acc;
    bool fog_refog_set = false;
#endif
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
#ifdef ALAMO_SIM_HAVE_FOG
        if (fog && !out.fog_ability_frame && battles.battle() && battles.battle()->frame() == 30 && host.tactical())
            fog_use_ability(*battles.battle(), *host.tactical(), land, out);  // fog follow-up (S9)
#endif
        if (battles.battle()) {
            out.activations = battles.battle()->stats().activations;
            out.particles = battles.battle()->stats().particles;
#ifdef ALAMO_SIM_HAVE_FOG
            if (TacticalMode* tm = host.tactical()) {
                fog_probe(*battles.battle(), *tm, out, fog_acc, fog_refog_set);
                fog_bm_probe(*battles.battle(), *tm, out);  // FOW t01
            }
#endif
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
#ifdef ALAMO_SIM_HAVE_FOG
    out.fog_hash = fog_acc.digest();
#endif
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

std::string golden_path() { return std::string(ALAMO_SIM_TESTDATA) + "/battle_synthetic_golden.crc"; }

void test_space_battle(const Data& d) {
    const uint64_t ticks = 2500;
    const Run a = run_campaign(d, false, true, 0, ticks);
    for (const std::string& s : a.battles) std::printf("  %s\n", s.c_str());
    for (const std::string& s : a.conflicts) std::printf("  %s\n", s.c_str());
    CHECK(a.fought == 1 && a.battles.size() == 1);
    CHECK(a.conflicts.size() == 1 && a.conflicts[0].find("tactical S10_Space_B") != std::string::npos);
    CHECK(a.depth == 1);  // the tactical mode was popped
    CHECK(a.won == 1 && a.lost == 1);
    CHECK(a.trace_battles);
    CHECK(!a.crc.records.empty() && a.crc.records.front().systems.size() == 12);  // 7 host + 5 battle systems
    // S10-int: ability data parsed from Unit_Abilities_Data (frigate + its corvette variant), command 43 accepted
    // and activated, hit particles spawned as PARTICLE objects.
    std::printf("  abilities: records %u, command 43 -> %d, activations %u, particle objects %u\n", a.ability_records,
                a.ability_command, a.activations, a.particles);
    CHECK(a.ability_records >= 2 && a.ability_command == 0 && a.activations > 0 && a.particles > 0);
    for (int32_t w : jobs::determinism_worker_counts()) {
        if (w == 0) continue;
        const Run b = run_campaign(d, false, true, w, ticks);
        const auto dv = first_divergence(a.crc, b.crc);
        std::printf("  space battle workers %2d: %s\n", w, dv.message.c_str());
        CHECK(!dv.diverged && b.battles == a.battles);
    }
    CrcLog log = a.crc;
    log.comments = {"S10 synthetic campaign: Underworld attacks the Empire orbit at S10_B, fought on S10_Space_B, seed 2026, " +
                    std::to_string(ticks) + " ticks"};
    std::string err;
    if (g_regen) {
        CHECK(log.save(golden_path(), &err));
        std::printf("  wrote %s\n", golden_path().c_str());
    } else {
        CrcLog g;
        if (!CrcLog::load(golden_path(), g, &err)) {
            std::printf("FAIL golden %s: %s (run with --regen)\n", golden_path().c_str(), err.c_str());
            ++g_fail;
        } else {
            const auto dv = first_divergence(g, log);
            std::printf("  golden battle_synthetic_golden.crc: %s\n", dv.message.c_str());
            CHECK(!dv.diverged);
        }
    }
}

void test_land_battle(const Data& d) {
    const Run a = run_campaign(d, true, true, 0, 3000);
    for (const std::string& s : a.battles) std::printf("  %s\n", s.c_str());
    for (const std::string& s : a.conflicts) std::printf("  %s\n", s.c_str());
    CHECK(a.fought == 1 && a.depth == 1);
    bool land_logged = false;
    for (const std::string& s : a.conflicts) land_logged = land_logged || s.find(" land S10_B ") != std::string::npos;
    CHECK(land_logged);
    // Whoever won holds the surface alone.
    CHECK((a.under_surface > 0) != (a.empire_surface > 0));
#ifdef ALAMO_SIM_HAVE_AI_TACTICAL
    // AI attacker (Underworld): credits halved for the land battle, the leftovers returned afterwards.
    std::printf("  credits: before %.1f, in battle %.1f, after %.1f\n", static_cast<double>(a.credits_before),
                static_cast<double>(a.credits_in_battle), static_cast<double>(a.credits_after));
    CHECK(a.credits_in_battle == 0.5f * a.credits_before && a.credits_after == a.credits_before);
#endif
    const Run b = run_campaign(d, true, true, 4, 3000);
    CHECK(!first_divergence(a.crc, b.crc).diverged);
}

// Stand-in retreat: the weaker Empire leaves after the 5 s retreat lock (test GameConstants) + its 5 s countdown; its ships survive in the
// galaxy and retreat to a neighbouring planet; the Underworld wins by "all capable enemies retreated".
void test_retreat(const Data& d) {
    const Run a = run_campaign(d, false, true, 0, 2500, 0.95f);
    for (const std::string& s : a.battles) std::printf("  %s\n", s.c_str());
    for (const std::string& s : a.conflicts) std::printf("  %s\n", s.c_str());
    CHECK(a.battles.size() == 1 && a.battles[0].find("all-retreated") != std::string::npos);
    CHECK(a.conflicts.size() == 1 && a.conflicts[0].find("retreat Empire") != std::string::npos);
    CHECK(a.empire_frigate_alive);
    // Playtest QA D1: the station cannot jump out, so it dies at the end of the retreat (BT §3.4), counts as a galactic loss
    // and is in the battle's loss records; the ships that left are not.
    CHECK(a.battles.size() == 1 && a.battles[0].find("+ starbase") != std::string::npos);
    const uint32_t base = type_of(d, "S10_Star_Base_1");
    const auto count = [](const Run& r, uint32_t t, int32_t owner) {
        return std::count_if(r.losses.begin(), r.losses.end(), [&](const auto& l) { return l.object_type == t && l.owner == owner; });
    };
    CHECK(count(a, base, 0) == 1);
    // story t06 (BT §3.1a): 0xc is declared when the retreat completes and the battle ends after the record's 210-frame
    // countdown (the world keeps ticking).
    CHECK(a.hold_ticks == 210);
    // A longer end hold (BattleConfig::end_hold_frames) is a minimum on that countdown: same winner and write-back;
    // deterministic across runs.
    const Run b = run_campaign(d, false, true, 0, 2700, 0.95f, true, 300);
    CHECK(b.hold_ticks == 300);
    CHECK(b.battles.size() == 1 && b.battles[0].find("all-retreated") != std::string::npos && b.battles[0].find("+ starbase") != std::string::npos);
    CHECK(b.conflicts.size() == 1 && b.conflicts[0].find("retreat Empire") != std::string::npos);
    CHECK(b.empire_frigate_alive && count(b, base, 0) == 1);
    const Run c = run_campaign(d, false, true, 4, 2700, 0.95f, true, 300);
    CHECK(!first_divergence(b.crc, c.crc).diverged);
}

// ---- integration fixes: human order API + skip_human_units; [port: Gabriel] queueable unit actions ----------------
// The space campaign with BattleConfig::skip_human_units: the stand-in never orders the human Empire's ships. At
// battle frame 3 the human orders its frigate F: plain move to P1 (with the corvette C), then queued: move to P2,
// attack the first Underworld frigate's hard point 0, POWER_TO_WEAPONS (command 43 with EventData::queued through
// the host's direct path). The queue must run in that order; plain orders / Stop clear C's queue. Per-tick queue
// trace + CRC log identical for 0/1/4/15 workers and equal to testdata/battle_queue_golden.crc.
struct QueueRun {
    CrcLog crc;
    uint64_t trace = 0;  // digest of (frame, F queue size, F goal, F ordered target) per tick
    int stage = -1;      // F's queue: 3 entries after the commands, then each completed stage
    bool order_ok = true, skip_ok = false, ai_moved = false, clears_ok = false;
    bool move2_ok = false, attack_ok = false, ability_ok = false;
    std::vector<int> stages;  // queue sizes in the order seen
};

std::string queue_golden_path() { return std::string(ALAMO_SIM_TESTDATA) + "/battle_queue_golden.crc"; }

QueueRun run_orders(const Data& d, int32_t workers, uint64_t ticks) {
    QueueRun out;
    jobs::JobSystem js(jobs::JobSystemConfig{workers});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    SimHost host(d.types, js, cfg);
    galactic::GalacticSim* gs = host.start_galactic(d.rules, make_setup(d, false));
    CHECK(gs != nullptr);
    if (!gs) return out;
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 6000;
    opt.battle.skip_human_units = true;
    CampaignBattles battles(host, d.crules, *d.catalog,
                            [](const std::string& name, terrain::MapData& m) {
                                if (name != "S10_Space_B") return false;
                                m = space_map();
                                return true;
                            },
                            opt);
    CHECK(battles.attach(nullptr));
    const std::vector<uint32_t> uf = gs->fleets_at(2, 1);
    if (!uf.empty()) CHECK(gs->move_fleet(1, uf[0], 1) == commands::RejectReason::None);
    using R = commands::RejectReason;
    using QO = TacticalBattle::QueuedOrder;
    objects::ObjectId fid = objects::kNoObjectId, eid = objects::kNoObjectId;
    float p2x = 0.0f, p2y = 0.0f;
    StateHasher trace;
    for (uint64_t t = 0; t < ticks; ++t) {
        host.tick_once();
        TacticalBattle* b = battles.battle();
        TacticalMode* tm = host.tactical();
        if (!b || !tm) continue;
        const objects::ObjectStore& st = tm->primary();
        if (b->frame() == 3 && fid == objects::kNoObjectId) {
            objects::ObjectId cid = objects::kNoObjectId;
            out.skip_ok = true;
            for (objects::ObjectHandle h : st.owned_by(0)) {
                const objects::ObjectId id = st.id(h);
                if (!st.alive(h) || !b->movement().has_unit(id)) continue;
                out.skip_ok = out.skip_ok && !b->movement().view(id).has_goal;  // the stand-in left them alone
                if (st.type(h) == d.types.resolve_name("S10_Frigate") && fid == objects::kNoObjectId) fid = id;
                if (st.type(h) == d.types.resolve_name("S10_Corvette") && cid == objects::kNoObjectId) cid = id;
            }
            for (objects::ObjectHandle h : st.owned_by(1)) {
                const objects::ObjectId id = st.id(h);
                if (!st.alive(h) || !b->movement().has_unit(id)) continue;
                out.ai_moved = out.ai_moved || b->movement().view(id).has_goal;
            }
            // The Underworld corvette nearest to F (one HP_S10_Laser_B hard point, 100 health).
            float best = 0.0f;
            const Vec3 fpos = fid != objects::kNoObjectId ? st.position(st.find(fid)) : Vec3{};
            for (objects::ObjectHandle h : st.owned_by(1)) {
                if (!st.alive(h) || st.type(h) != d.types.resolve_name("S10_Corvette")) continue;
                const Vec3 p = st.position(h);
                const float d2 = (p.x - fpos.x) * (p.x - fpos.x) + (p.y - fpos.y) * (p.y - fpos.y);
                if (eid == objects::kNoObjectId || d2 < best) {
                    eid = st.id(h);
                    best = d2;
                }
            }
            CHECK(fid != objects::kNoObjectId && cid != objects::kNoObjectId && eid != objects::kNoObjectId);
            if (fid == objects::kNoObjectId || cid == objects::kNoObjectId || eid == objects::kNoObjectId) break;
            SimRng& rng = host.random().synced();
            const Vec3 p0 = st.position(st.find(fid));
            const Vec3 e0 = st.position(st.find(eid));
            const float p1x = p0.x, p1y = p0.y + 100.0f;  // short hops, then towards the enemy
            p2x = p0.x + 0.05f * (e0.x - p0.x);
            p2y = p0.y + 0.05f * (e0.y - p0.y);
            const objects::ObjectId fc[] = {fid, cid}, f1[] = {fid}, c1[] = {cid};
            CHECK(b->order_move(*tm, 0, fc, p1x, p1y, rng) == R::None);
            CHECK(b->order_move(*tm, 1, fc, p1x, p1y, rng) == R::InvalidUnit);  // not player 1's units
            CHECK(b->order_move(*tm, 0, f1, p2x, p2y, rng, tactical::movement::kNoFacing, false, true) == R::None);
            CHECK(b->order_attack(*tm, 0, f1, fid, rng, 0, true) == R::InvalidTarget);  // own unit
            CHECK(b->order_attack(*tm, 0, f1, eid, rng, 0, true) == R::None);
            commands::EventData ev(commands::EventType::SetUnitAbilityMode);
            ev.set("ability", 29).set("on", 1).set_ids("units", {static_cast<uint32_t>(fid)});
            ev.set_queued(true);
            CHECK(host.execute_direct(0, commands::CommandSource::Player, ev) == R::None);
            const auto q = b->queued_orders(fid);
            CHECK(q.size() == 3 && q[0].kind == QO::Kind::Move && q[1].kind == QO::Kind::Attack && q[1].target == eid &&
                  q[1].hardpoint == 0 && q[2].kind == QO::Kind::Ability && q[0].group < q[1].group && q[1].group < q[2].group);
            // C: queued behind its move, cleared by Stop; an idle unit runs a queued order at once; a plain order clears.
            CHECK(b->order_move(*tm, 0, c1, p2x, p2y, rng, tactical::movement::kNoFacing, false, true) == R::None);
            const bool queued1 = b->queued_orders(cid).size() == 1;
            CHECK(b->order_stop(*tm, 0, c1) == R::None);
            const bool stopped = b->queued_orders(cid).empty() && !b->movement().view(cid).has_goal;
            CHECK(b->order_move(*tm, 0, c1, p2x, p2y, rng, tactical::movement::kNoFacing, false, true) == R::None);
            const bool ran_now = b->queued_orders(cid).empty() && b->movement().view(cid).has_goal;
            CHECK(b->order_move(*tm, 0, c1, p1x, p1y, rng, tactical::movement::kNoFacing, false, true) == R::None);
            const bool queued2 = b->queued_orders(cid).size() == 1;
            CHECK(b->order_move(*tm, 0, c1, p1x, p1y, rng) == R::None);
            out.clears_ok = queued1 && stopped && ran_now && queued2 && b->queued_orders(cid).empty();
            out.stage = 3;
            out.stages.push_back(3);
        }
        if (fid == objects::kNoObjectId) continue;
        const int n = static_cast<int>(b->queued_orders(fid).size());
        const bool alive = st.alive(st.find(fid));
        if (alive && n != out.stage) {
            if (n != out.stage - 1) out.order_ok = false;  // one entry at a time, in order
            out.stage = n;
            out.stages.push_back(n);
            if (n == 2) {  // the queued move started
                const auto v = b->movement().view(fid);
                const float dx = v.goal.x - p2x, dy = v.goal.y - p2y;
                out.move2_ok = v.has_goal && dx * dx + dy * dy < 300.0f * 300.0f;
            } else if (n == 1) {  // the attack on hard point 0
                out.attack_ok = b->combat().ordered_target(fid) == eid && b->combat().ordered_hardpoint(fid) == 0;
            } else if (n == 0) {  // hard point 0 (or the target) gone: POWER_TO_WEAPONS
                out.ability_ok = b->abilities().is_active(fid, 29);
            }
        }
        trace.u64(b->frame());
        trace.i32(n);
        if (alive && b->movement().has_unit(fid)) {
            const auto v = b->movement().view(fid);
            trace.f32(v.goal.x);
            trace.f32(v.goal.y);
            trace.i32(b->combat().ordered_target(fid));
        }
    }
    out.trace = trace.digest();
    out.crc = host.crc_log();
    return out;
}

void test_orders_and_queue(const Data& d) {
    const uint64_t ticks = 1500;
    const QueueRun a = run_orders(d, 0, ticks);
    std::string seq;
    for (int s : a.stages) seq += std::to_string(s) + " ";
    std::printf("  skip %d ai_moved %d clears %d order %d stages [%s] move2 %d attack %d ability %d\n", a.skip_ok, a.ai_moved,
                a.clears_ok, a.order_ok, seq.c_str(), a.move2_ok, a.attack_ok, a.ability_ok);
    CHECK(a.skip_ok && a.ai_moved && a.clears_ok && a.order_ok);
    CHECK(a.stages.size() == 4 && a.move2_ok && a.attack_ok && a.ability_ok);
    for (int32_t w : jobs::determinism_worker_counts()) {
        if (w == 0) continue;
        const QueueRun b = run_orders(d, w, ticks);
        const auto dv = first_divergence(a.crc, b.crc);
        std::printf("  order queue workers %2d: %s\n", w, dv.message.c_str());
        CHECK(!dv.diverged && b.trace == a.trace && b.stages == a.stages);
    }
    CrcLog log = a.crc;
    log.comments = {"S10 synthetic campaign, skip_human_units, human orders + queued move / hard point attack / ability at battle "
                    "frame 3, seed 2026, " + std::to_string(ticks) + " ticks"};
    std::string err;
    if (g_regen) {
        CHECK(log.save(queue_golden_path(), &err));
        std::printf("  wrote %s\n", queue_golden_path().c_str());
    } else {
        CrcLog g;
        if (!CrcLog::load(queue_golden_path(), g, &err)) {
            std::printf("FAIL golden %s: %s (run with --regen)\n", queue_golden_path().c_str(), err.c_str());
            ++g_fail;
        } else {
            const auto dv = first_divergence(g, log);
            std::printf("  golden battle_queue_golden.crc: %s\n", dv.message.c_str());
            CHECK(!dv.diverged);
        }
    }
}

void test_auto_only(const Data& d) {
    const Run a = run_campaign(d, false, false, 0, 600);
    for (const std::string& s : a.conflicts) std::printf("  %s\n", s.c_str());
    CHECK(a.fought == 0 && a.conflicts.size() == 1 && a.conflicts[0].find("auto-resolved") != std::string::npos);
    CHECK(a.won == 1 && a.lost == 1 && a.depth == 1);
}

#ifdef ALAMO_SIM_HAVE_STORY
// L1/S4 wave 2a story hookup: a galactic plot whose STORY_SPACE_TACTICAL names tactical plots; CampaignBattles
// attaches them at the push (StorySession::begin_battle with the battle's bridge), feeds deaths, ends with
// VICTORY. The tactical script activates POWER_TO_WEAPONS through the battle's AbilitySystem (no command 43 is
// sent here, so every activation counted by the battle comes from Lua). Logs identical for 0 / 4 workers.
struct StoryRun {
    std::string log;
    std::vector<std::string> lua;
    uint32_t activations = 0;
    bool fog_on_seen = false, fog_off_seen = false;  // fog follow-up: the battle's FOW-active flag over the run
};
StoryRun run_story_battle(const Data& d, const FileProvider& files, int32_t workers) {
    StoryRun out;
    jobs::JobSystem js(jobs::JobSystemConfig{workers});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    cfg.story_crc = true;
    SimHost host(d.types, js, cfg);
    galactic::GalacticSim* gs = host.start_galactic(d.rules, make_setup(d, false));
    CHECK(gs != nullptr);
    if (!gs) return out;
    StoryStart st;
    st.files = &files;  // a provider created after the story files were written
    st.plot_sets = {{0, "Story_Plots_S10.xml"}};
    std::vector<std::string> diag;
    CHECK(host.start_story(st, &diag));
    for (const auto& m : diag) std::printf("  story diag: %s\n", m.c_str());
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 6000;
    CampaignBattles battles(host, d.crules, *d.catalog,
                            [](const std::string& name, terrain::MapData& m) {
                                if (name != "S10_Space_B") return false;
                                m = space_map();
                                return true;
                            },
                            opt);
    CHECK(battles.attach());
    const std::vector<uint32_t> uf = gs->fleets_at(2, 1);
    CHECK(!uf.empty());
    if (!uf.empty()) CHECK(gs->move_fleet(1, uf[0], 1) == commands::RejectReason::None);
    for (uint64_t t = 0; t < 2500; ++t) {
        host.tick_once();
        if (battles.battle()) out.activations = battles.battle()->stats().activations;
#ifdef ALAMO_SIM_HAVE_FOG
        if (battles.battle())
            if (const tactical::fog::FogOfWar* f = battles.battle()->fog()) {
                out.fog_on_seen = out.fog_on_seen || f->fow_active();
                out.fog_off_seen = out.fog_off_seen || !f->fow_active();
            }
#endif
    }
    if (host.story()) {
        out.log = host.story()->log_text("S10 story battle");
        out.lua = host.story()->lua_messages();
    }
    return out;
}

// BT §2.9 / REMAINING_GAPS §6.4: a galactic plot's LINK_TACTICAL (STORY_ELAPSED 1 s) starts its battle at the battle
// hook right after the reward (ConflictManager::start_linked: queued at the front, no validation / decide). The story flag
// (GalacticSim::linked_battle_pending; paused() refuses FleetManagement while the local player is in a queued conflict)
// is set by the record and cleared at the start, so no tick ends with it set. Cases: space at S10_B (an Underworld
// frigate already orbits it); land at S10_B with no Underworld unit anywhere near (the record has no units: the validation used to drop it); land sandbox (Param8 1: the
// human defender's surface units start, the AI attacker gets nothing).
struct LinkedRun {
    bool started = false, land = false, flag_after_tick = false;
    uint64_t reward_tick = 0, start_tick = 0;
    size_t forces = 0, def_start = 0, att_start = 0, att_reserve = 0;
    uint32_t fought = 0;
    std::string log, conflicts;
    // story t07: the record's P9-P14 as the battle sees them, and the P13 banner (ask = the battle dialog answers Pending).
    CampaignBattles::LinkedPresentation presentation;
    galactic::LinkedResolution resolution = galactic::LinkedResolution::None, resolution_after = galactic::LinkedResolution::None;
    bool skipped_dialog = false;
    uint64_t banner_tick = 0, begin_tick = 0;
    bool paused_in_banner = true, begin_refused_early = false;
    // story t10: `post` puts an Underworld (attacker) S10_Trooper (Is_Branched_Map_Discardable by default) on the land map;
    // -1 = not checked, else whether it is in the battle at its first frame; and the request's linked flag.
    int post_kept = -1;
    bool request_linked = false;
};

// `params9`: Reward_Param9.. XML appended after Param8 (story t07). `ask`: human_choice Pending (the game's battle dialog), so
// a P13 banner waits for ResumeGame, sent 20 ticks after it went up. `post`: see LinkedRun::post_kept.
LinkedRun run_linked(const Data& d, bool space, bool sandbox, const std::string& params9 = {}, bool ask = false, bool post = false) {
    LinkedRun out;
    const fs::path x = d.root / "Data/XML";
    write_file(x / "Story_Plots_Link.xml", "<Story_Mode_Plots><Active_Plot>Story_Link.xml</Active_Plot></Story_Mode_Plots>\n");
    write_file(x / "Story_Link.xml",
               std::string("<Story>\n"
                           " <Event Name=\"Link\"><Event_Type>STORY_ELAPSED</Event_Type><Event_Param1>1</Event_Param1>"
                           "<Reward_Type>LINK_TACTICAL</Reward_Type><Reward_Param1>S10_B</Reward_Param1><Reward_Param2>") +
                   (space ? "SPACE" : "LAND") + "</Reward_Param2><Reward_Param3>Underworld</Reward_Param3><Reward_Param4> " +
                   (space ? "S10_Space_B" : "S10_Land_B") + " </Reward_Param4><Reward_Param5>Empire</Reward_Param5>"
                   "<Reward_Param6></Reward_Param6><Reward_Param7></Reward_Param7><Reward_Param8>" + (sandbox ? "1" : "0") +
                   "</Reward_Param8>" + params9 + "</Event>\n</Story>\n");
    DirectoryProvider files(path_to_utf8(d.root));
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    SimHost host(d.types, js, cfg);
    galactic::GalacticSetup setup = make_setup(d, !space);
    if (space) setup.forces.push_back({1, 1, type_of(d, "S10_Frigate")});  // an Underworld frigate already orbits S10_B
    galactic::GalacticSim* gs = host.start_galactic(d.rules, std::move(setup));
    CHECK(gs != nullptr);
    if (!gs) return out;
    StoryStart st;
    st.files = &files;
    st.plot_sets = {{0, "Story_Plots_Link.xml"}};
    std::vector<std::string> diag;
    CHECK(host.start_story(st, &diag));
    CampaignBattleOptions opt;
    opt.conflict.human_choice = ask ? galactic::BattleChoice::Pending : galactic::BattleChoice::Fight;
    opt.battle.max_frames = 300;
    CampaignBattles battles(host, d.crules, *d.catalog,
                            [post](const std::string& name, terrain::MapData& m) {
                                if (name == "S10_Space_B") m = space_map();
                                else if (name == "S10_Land_B") m = land_map();
                                else return false;
                                if (post && name == "S10_Land_B") {
                                    terrain::ObjectPlacement pl;
                                    pl.type_crc = name_crc("S10_Trooper");
                                    pl.owner = 1;  // Underworld (faction index 1)
                                    pl.position[0] = 700.0f;
                                    pl.position[1] = 1800.0f;
                                    m.objects.push_back(pl);
                                }
                                return true;
                            },
                            opt);
    CHECK(battles.attach());
    for (uint64_t t = 0; t < 400 && battles.battles_fought() == 0; ++t) {
        const size_t fired = host.story() ? host.story()->linked_tactical().size() : 0;
        host.tick_once();
        // story t07: the record waits while its banner is up (the flag stays set then, so it is only tracked outside it)
        out.flag_after_tick = out.flag_after_tick || (gs->linked_battle_pending() && !battles.linked_banner_up() && !battles.linked_record());
        if (host.story() && fired == 0 && !host.story()->linked_tactical().empty()) out.reward_tick = host.frame();
        if (battles.linked_banner_up()) {
            if (out.banner_tick == 0) out.banner_tick = host.frame();
            out.paused_in_banner = out.paused_in_banner && gs->paused();
            if (host.frame() == out.banner_tick + 20 && out.begin_tick == 0) {
                commands::EventData ev(commands::EventType::ResumeGame);
                ev.set("flag", 0);  // Begin
                host.submit(0, ev);
                out.begin_tick = host.frame() + 1;  // executed at the next tick's command phase
            }
        }
        if (battles.battle() && !out.started) {
            out.started = true;
            out.start_tick = host.frame();
            out.presentation = battles.linked_presentation();
            out.resolution = battles.linked_resolution();
            out.land = battles.battle()->ground_height(0.0f, 0.0f) > -1e30f;
            const galactic::BattleRequest& r = battles.conflicts().active_request();
            out.forces = r.forces.size();
            if (r.forces.size() == 2) {
                out.def_start = r.forces[0].start.size();
                out.att_start = r.forces[1].start.size();
                out.att_reserve = r.forces[1].reserve.size();
            }
            out.request_linked = r.linked;
        }
    }
    for (int t = 0; t < 400 && battles.battle(); ++t) {  // run the battle out
        host.tick_once();
        if (post && out.post_kept < 0 && battles.battle() && host.tactical() && battles.battle()->frame() >= 1) {
            const objects::ObjectStore& ts = host.tactical()->primary();
            const objects::TypeIndex pt = d.types.resolve_name("S10_Trooper");
            out.post_kept = 0;
            for (const objects::ObjectHandle h : ts.all_objects())
                if (ts.alive(h) && ts.type(h) == pt && ts.owner(h) == battles.battle()->request().info.attacker) out.post_kept = 1;
        }
    }
    out.fought = battles.battles_fought();
    out.resolution_after = battles.linked_resolution();
    out.skipped_dialog = battles.linked_presentation().last_battle_skipped_dialog;
    for (const std::string& l : battles.log()) out.log += l + "; ";
    for (const auto& e : battles.conflicts().log()) out.conflicts += e.text(*gs) + "; ";
    return out;
}

// a1f t08: the S10 PROBE A story sequence (tools/probe_mod/s10_map_owners/s10_story.lua, the shipped
// Story_Campaign_Underworld.lua attack pattern) on the synthetic galaxy: a galactic story script of the human's plot
// assembles the Underworld squads in orbit of S10_C into a fleet, BlockOnCommand(fleet.Move_To(S10_B)) (the Empire's
// planet, an Empire squad on the surface), Sleep(1), fleet.Invade(). The block must last until the fleet is in orbit
// of S10_B (StorySession's order_finished); before a1f t08 it finished at once, Invade() ran in hyperspace and was
// dropped, so the fleet sat in orbit with either human. The squads are spawned at S10_C with Spawn_Unit (the probe's
// SpawnList; a1g t06: registered with the GalacticSim, so Assemble_Fleet flies them). Two runs: Underworld human (the
// probe's fallback: the original invaded at once) and Empire human with an Underworld without galactic AI (the probe's
// preferred variant, AI_Player_Control None: the original's fleet stayed in orbit and never invaded). a1g t05
// (A1_READINGS §31): fleet.Invade() is issued by the LOCAL player with its own ground units only, so with a human
// Empire nothing lands, silently.
struct ProbeInvadeRun {
    std::string conflicts, story;
    std::vector<std::string> lua;
    size_t land_queued = 0;  // most land conflicts queued at once
    uint32_t fought = 0;
    bool fleet_at_target = false;  // an Underworld fleet with ground units in orbit of S10_B at the end
};
ProbeInvadeRun run_probe_invade(const Data& d, const FileProvider& files, bool human_empire) {
    ProbeInvadeRun out;
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    SimHost host(d.types, js, cfg);
    galactic::GalacticSetup setup = make_setup(d, true);
    setup.players[0].human = human_empire;
    setup.players[0].ai = !human_empire;
    setup.players[1].human = !human_empire;
    setup.players[1].ai = false;  // human, or the probe's AI_Player_Control None (no galactic AI)
    galactic::GalacticSim* gs = host.start_galactic(d.rules, std::move(setup));
    CHECK(gs != nullptr);
    if (!gs) return out;
    StoryStart st;
    st.files = &files;
    st.plot_sets = {{human_empire ? 0 : 1, "Story_Plots_Probe.xml"}};
    std::vector<std::string> diag;
    CHECK(host.start_story(st, &diag));
    for (const auto& m : diag) std::printf("  story diag: %s\n", m.c_str());
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 300;
    CampaignBattles battles(host, d.crules, *d.catalog,
                            [](const std::string& name, terrain::MapData& m) {
                                if (name != "S10_Land_B") return false;
                                m = land_map();
                                return true;
                            },
                            opt);
    CHECK(battles.attach());
    for (uint64_t t = 0; t < 900; ++t) {
        host.tick_once();
        out.land_queued = std::max(out.land_queued, battles.conflicts().land_queue().size());
    }
    out.fought = battles.battles_fought();
    for (const uint32_t f : gs->fleets_at(1, 1))
        for (const objects::ObjectHandle m : gs->fleets()[f].contents)
            if (gs->store().alive(m) && gs->rules().types[gs->store().type(m)].kind == galactic::UnitKind::GroundUnit)
                out.fleet_at_target = true;
    if (host.story()) {
        out.lua = host.story()->lua_messages();
        out.story = host.story()->log_text("probe");
    }
    for (const auto& e : battles.conflicts().log()) out.conflicts += e.text(*gs) + "; ";
    return out;
}

void test_probe_invade(const Data& d) {
    const fs::path x = d.root / "Data/XML";
    write_file(x / "Story_Plots_Probe.xml",
               "<Story_Mode_Plots><Lua_Script>Story_Probe</Lua_Script><Active_Plot>Story_Probe.xml</Active_Plot></Story_Mode_Plots>\n");
    std::string plot = "<Story>\n";
    for (const char* s : {"FLEET", "ARRIVED", "NOT_THERE", "INVADED"})
        plot += std::string(" <Event Name=\"P_") + s + "\"><Event_Type>STORY_AI_NOTIFICATION</Event_Type><Event_Param2>" + s +
                "</Event_Param2></Event>\n";
    plot += "</Story>\n";
    write_file(x / "Story_Probe.xml", plot);
    // PGBase's BlockOnCommand / Sleep inlined as polling stages (no PG library in the synthetic data).
    write_file(d.root / "Data/Scripts/Story/Story_Probe.lua",
               "function Base_Definitions() ServiceRate = 0.1 stage = 0 end\n"
               "function main() while true do\n"
               "  if stage == 0 and GetCurrentTime() >= 1 then\n"
               "    uw = Find_Player('Underworld') tgt = FindPlanet('S10_B') src = FindPlanet('S10_C') units = {}\n"
               "    for _, t in ipairs({'S10_Squad', 'S10_Squad'}) do\n"
               "      for _, u in pairs(Spawn_Unit(Find_Object_Type(t), src, uw)) do table.insert(units, u) end end\n"
               "    fleet = Assemble_Fleet(units)\n"
               "    if fleet then Story_Event('FLEET') block = fleet.Move_To(tgt) stage = 1 else stage = 9 end\n"
               "  elseif stage == 1 and block.IsFinished() then\n"
               "    if fleet.Get_Planet_Location() == tgt then Story_Event('ARRIVED') else Story_Event('NOT_THERE') end\n"
               "    t1 = GetCurrentTime() stage = 2\n"
               "  elseif stage == 2 and GetCurrentTime() >= t1 + 1 then\n"
               "    fleet.Invade() Story_Event('INVADED') stage = 3\n"
               "  end\n"
               "  coroutine.yield(true) end end\n");
    DirectoryProvider files(path_to_utf8(d.root));
    for (const bool human_empire : {false, true}) {
        const ProbeInvadeRun r = run_probe_invade(d, files, human_empire);
        auto fired = [&r](const char* ev) { return r.story.find(std::string(" P_") + ev + " ") != std::string::npos; };
        std::printf("  human %s: fleet %d, block ended in orbit %d (elsewhere %d), invade issued %d, land conflicts queued %zu, "
                    "fought %u, fleet in orbit of S10_B at the end %d\n    %s\n",
                    human_empire ? "Empire (Underworld AI None)" : "Underworld", fired("FLEET") ? 1 : 0, fired("ARRIVED") ? 1 : 0,
                    fired("NOT_THERE") ? 1 : 0, fired("INVADED") ? 1 : 0, r.land_queued, r.fought, r.fleet_at_target ? 1 : 0,
                    r.conflicts.c_str());
        for (const std::string& m : r.lua) std::printf("    lua: %s\n", m.c_str());
        CHECK(r.lua.empty());
        CHECK(fired("FLEET") && fired("ARRIVED") && !fired("NOT_THERE") && fired("INVADED"));
        if (human_empire) {
            // The original (probe 2026-10-31, A1_READINGS §31): Invade() issued by the local Empire keeps no Underworld
            // unit, nothing lands, the fleet stays in orbit.
            CHECK(r.land_queued == 0 && r.fought == 0 && r.fleet_at_target);
        } else {
            CHECK(r.land_queued == 1 && r.fought == 1);
            CHECK(r.conflicts.find(" land S10_B Underworld vs Empire") != std::string::npos);
        }
    }
}

// a1h t04 (A1_READINGS §31.6): story reward INVADE_PLANET. A galactic story script brings two Underworld squads into orbit
// of S10_B (Empire) and raises GO; the plot's GO event has the reward INVADE_PLANET S10_B [company]. The reward picks the
// first ground unit of a fleet in orbit friendly to the plot's player (Underworld) that matches the company filter and
// issues ONE Invade event with that unit as the LOCAL player: with a human Underworld one squad lands (a land conflict
// with one unit); with a human Empire (an AI plot) the executor keeps no unit and nothing happens, silently; a filter
// that matches nothing logs the mismatch and issues nothing.
struct InvadeRewardRun {
    size_t land_queued = 0, landing_units = 0;  // most land conflicts queued at once; units of the first one seen
    std::vector<std::string> lua;
    std::string story;
};
InvadeRewardRun run_invade_reward(const Data& d, const FileProvider& files, bool human_empire) {
    InvadeRewardRun out;
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    SimHost host(d.types, js, cfg);
    galactic::GalacticSetup setup = make_setup(d, true);
    setup.players[0].human = human_empire;
    setup.players[0].ai = false;
    setup.players[1].human = !human_empire;
    setup.players[1].ai = false;
    galactic::GalacticSim* gs = host.start_galactic(d.rules, std::move(setup));
    CHECK(gs != nullptr);
    if (!gs) return out;
    StoryStart st;
    st.files = &files;
    st.plot_sets = {{1, "Story_Plots_Reward.xml"}};  // the Underworld's plot
    std::vector<std::string> diag;
    CHECK(host.start_story(st, &diag));
    for (const auto& m : diag) std::printf("  story diag: %s\n", m.c_str());
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 300;
    CampaignBattles battles(host, d.crules, *d.catalog,
                            [](const std::string& name, terrain::MapData& m) {
                                if (name != "S10_Land_B") return false;
                                m = land_map();
                                return true;
                            },
                            opt);
    CHECK(battles.attach());
    for (uint64_t t = 0; t < 900; ++t) {
        host.tick_once();
        const auto& q = battles.conflicts().land_queue();
        out.land_queued = std::max(out.land_queued, q.size());
        if (!q.empty() && out.landing_units == 0) out.landing_units = q[0].units.size();
    }
    if (host.story()) {
        out.lua = host.story()->lua_messages();
        out.story = host.story()->log_text("reward");
    }
    return out;
}

void test_invade_planet_reward(const Data& d) {
    const fs::path x = d.root / "Data/XML";
    write_file(x / "Story_Plots_Reward.xml",
               "<Story_Mode_Plots><Lua_Script>Story_Reward</Lua_Script><Active_Plot>Story_Reward.xml</Active_Plot></Story_Mode_Plots>\n");
    write_file(d.root / "Data/Scripts/Story/Story_Reward.lua",
               "function Base_Definitions() ServiceRate = 0.1 stage = 0 end\n"
               "function main() while true do\n"
               "  if stage == 0 and GetCurrentTime() >= 1 then\n"
               "    uw = Find_Player('Underworld') units = {}\n"
               "    for _, t in ipairs({'S10_Squad', 'S10_Squad'}) do\n"
               "      for _, u in pairs(Spawn_Unit(Find_Object_Type(t), FindPlanet('S10_C'), uw)) do table.insert(units, u) end end\n"
               "    fleet = Assemble_Fleet(units) block = fleet.Move_To(FindPlanet('S10_B')) stage = 1\n"
               "  elseif stage == 1 and block.IsFinished() then Story_Event('GO') stage = 2 end\n"
               "  coroutine.yield(true) end end\n");
    struct Case {
        bool human_empire;
        const char* company;
        size_t land_queued, landing_units;
        bool mismatch_logged;
    };
    for (const Case& c : {Case{false, "S10_SQUAD", 1, 1, false}, Case{false, "", 1, 1, false}, Case{true, "S10_SQUAD", 0, 0, false},
                          Case{false, "S10_Squad", 0, 0, true}}) {
        write_file(x / "Story_Reward.xml",
                   std::string("<Story>\n <Event Name=\"P_GO\"><Event_Type>STORY_AI_NOTIFICATION</Event_Type><Event_Param2>GO</Event_Param2>"
                               "<Reward_Type>INVADE_PLANET</Reward_Type><Reward_Param1>S10_B</Reward_Param1>") +
                       (*c.company ? std::string("<Reward_Param2>") + c.company + "</Reward_Param2>" : std::string()) + "</Event>\n</Story>\n");
        DirectoryProvider files(path_to_utf8(d.root));  // after the plot file is written
        const InvadeRewardRun r = run_invade_reward(d, files, c.human_empire);
        bool mismatch = false;
        for (const std::string& m : r.lua) mismatch = mismatch || m.find("INVADE PLANET - S10_SQUAD does not match S10_Squad") != std::string::npos;
        std::printf("  INVADE_PLANET human %s, company '%s': land conflicts %zu with %zu unit(s), mismatch logged %d\n",
                    c.human_empire ? "Empire" : "Underworld", c.company, r.land_queued, r.landing_units, mismatch ? 1 : 0);
        CHECK(r.story.find(" P_GO ") != std::string::npos);
        CHECK(r.land_queued == c.land_queued && r.landing_units == c.landing_units && mismatch == c.mismatch_logged);
        CHECK(r.lua.size() == (c.mismatch_logged ? 2u : 0u));  // both squads mismatch
    }
}

void test_linked_battle(const Data& d) {
    for (int c = 0; c < 3; ++c) {
        const bool space = c == 0, sandbox = c == 2;
        const LinkedRun r = run_linked(d, space, sandbox);
        std::printf("  linked %s%s: reward tick %" PRIu64 ", battle tick %" PRIu64 " (%s), flag after a tick %d, forces %zu (defender start %zu, "
                    "attacker start %zu reserve %zu), fought %u\n    battles: %s\n    conflicts: %s\n",
                    space ? "space" : "land", sandbox ? " sandbox" : "", r.reward_tick, r.start_tick, r.land ? "land" : "space",
                    r.flag_after_tick ? 1 : 0, r.forces, r.def_start, r.att_start, r.att_reserve, r.fought, r.log.c_str(), r.conflicts.c_str());
        CHECK(r.started && r.land == !space);
        // §2.9.3 (Param13 0): started by the reward itself. [port] the reward is recorded in the story service, which runs
        // after the battle hook, so the battle starts at the next hook (the retail load also spans frames).
        CHECK(r.reward_tick != 0 && r.start_tick == r.reward_tick + 1);
        CHECK(!r.flag_after_tick);
        CHECK(r.forces == 2 && r.att_start == 0 && r.att_reserve == 0);
        CHECK(r.def_start == (sandbox ? 1u : 0u));  // not sandbox: no galactic unit; sandbox: the human defender's squad
        CHECK(r.fought == 1 && r.log.find("LINK_TACTICAL S10_B") != std::string::npos && r.log.find(" started") != std::string::npos);
        CHECK(r.conflicts.find(space ? " space S10_B " : " land S10_B ") != std::string::npos);
        // story t07: defaults (P9 1, P12 DESTROY_AI, P13 on but answered at once headless); the dialog skip is taken at the end.
        CHECK(r.resolution == galactic::LinkedResolution::DestroyAi && r.resolution_after == galactic::LinkedResolution::None);
        CHECK(r.presentation.arrival == 1 && r.presentation.arrival_cinematic && !r.presentation.hyperspace_hold);
        CHECK(!r.presentation.presentation_a && !r.presentation.presentation_b && r.skipped_dialog && r.banner_tick == 0);
    }
}

// story t07 (BT §2.9.1 / §2.9.3): LINK_TACTICAL Param9-14. (1) P9 2 / P10 1 / P11 1 / P12 RETREAT_PLAYER / P13 0 / P14 0: the
// arrival hold and presentation flags are set at the start, GOM+0x61c = RETREAT_PLAYER during the battle and cleared by its
// end; a bad P12 keyword is logged and falls back to DESTROY_AI. (2) The P13 banner with the battle dialog (Pending): the
// record waits with the galaxy paused until ResumeGame (Begin), then starts at the next hook; before it, no battle.
void test_linked_params(const Data& d) {
    const auto p = [](int n, const char* v) { return "<Reward_Param" + std::to_string(n) + ">" + v + "</Reward_Param" + std::to_string(n) + ">"; };
    const LinkedRun a = run_linked(d, true, false, p(9, "2") + p(10, "1") + p(11, "1") + p(12, "RETREAT_PLAYER") + p(13, "0") + p(14, "0"));
    std::printf("  P9-P14: started tick %" PRIu64 " (reward %" PRIu64 "), arrival %d hold %d A %d letterbox %d B %d, resolution %d -> %d, "
                "dialog skipped %d\n",
                a.start_tick, a.reward_tick, a.presentation.arrival, a.presentation.hyperspace_hold ? 1 : 0, a.presentation.presentation_a ? 1 : 0,
                a.presentation.letterbox ? 1 : 0, a.presentation.presentation_b ? 1 : 0, static_cast<int>(a.resolution),
                static_cast<int>(a.resolution_after), a.skipped_dialog ? 1 : 0);
    CHECK(a.started && a.start_tick == a.reward_tick + 1 && a.fought == 1);
    CHECK(a.presentation.arrival == 2 && a.presentation.arrival_cinematic && a.presentation.hyperspace_hold);
    CHECK(a.presentation.presentation_a && a.presentation.letterbox && a.presentation.presentation_b);
    CHECK(a.resolution == galactic::LinkedResolution::RetreatPlayer && a.resolution_after == galactic::LinkedResolution::None);
    CHECK(a.skipped_dialog);
    const LinkedRun u = run_linked(d, false, false, p(9, "0") + p(12, "RETREAT_EVERYONE"));
    CHECK(u.started && u.resolution == galactic::LinkedResolution::DestroyAi && !u.presentation.arrival_cinematic && !u.presentation.hyperspace_hold);
    CHECK(u.log.find("Unknown resolution RETREAT_EVERYONE") != std::string::npos);
    const LinkedRun b = run_linked(d, true, false, {}, true);  // P13 default on, the dialog answers Pending
    std::printf("  P13 banner: reward tick %" PRIu64 ", banner %" PRIu64 ", Begin %" PRIu64 ", battle %" PRIu64 ", paused %d\n    battles: %s\n",
                b.reward_tick, b.banner_tick, b.begin_tick, b.start_tick, b.paused_in_banner ? 1 : 0, b.log.c_str());
    CHECK(b.banner_tick == b.reward_tick + 1 && b.paused_in_banner && b.begin_tick == b.banner_tick + 21);
    CHECK(b.started && b.start_tick == b.begin_tick && b.fought == 1);  // Begin at the command phase, pushed at that tick's hook
    CHECK(b.log.find("LINK_TACTICAL S10_B space banner") != std::string::npos && !b.flag_after_tick);
}

// story t10 (REMAINING_GAPS §3.1 item 4, [M] global 0x140b155d8; BT §3.1a item 4): a linked battle keeps the attacker's
// Is_Branched_Map_Discardable map objects (Kessel UM01's jailed UMP_TYBER_ZANN): an Underworld S10_Trooper placed on the
// map, which a normal invade drops (as the S4_Post of the map owners test: owner = attacker), is in the linked land battle.
void test_linked_map_objects(const Data& d) {
    const LinkedRun r = run_linked(d, false, false, {}, false, true);
    std::printf("  linked land: attacker's branched-discardable trooper kept %d, request linked %d\n", r.post_kept, r.request_linked ? 1 : 0);
    CHECK(r.started && r.request_linked && r.post_kept == 1);
}

// a1j t05 (A1_READINGS §31.7, BT §1.4 step 2): 0x2be640 skips the space/land state machines while the story block holds
// (a linked battle record waits and the local player is in a queued conflict); the paused query 0x4b2360 freezes the
// galactic frame. The record is raised by hand here (no story), the Underworld fleet flies C -> B against the human
// Empire: its conflict stays queued, nothing is voted, fought or auto-resolved and the frame stands still until the flag
// is cleared; then the conflict runs as usual.
void test_story_block_host(const Data& d) {
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    SimHost host(d.types, js, cfg);
    galactic::GalacticSim* gs = host.start_galactic(d.rules, make_setup(d, false));
    CHECK(gs != nullptr);
    if (!gs) return;
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 300;
    CampaignBattles battles(host, d.crules, *d.catalog,
                            [](const std::string& name, terrain::MapData& m) {
                                if (name != "S10_Space_B") return false;
                                m = space_map();
                                return true;
                            },
                            opt);
    CHECK(battles.attach());
    const std::vector<uint32_t> uf = gs->fleets_at(2, 1);
    CHECK(!uf.empty());
    if (uf.empty()) return;
    CHECK(gs->move_fleet(1, uf[0], 1) == commands::RejectReason::None);
    uint64_t t = 0;
    // [port] the host releases a flag that has no conflict to wait for at its battle hook, so it is raised again before
    // every tick until the fleet's conflict holds it.
    for (; t < 2000 && !gs->story_block(); ++t) {
        if (!gs->linked_battle_pending()) gs->queue_linked_battle();
        host.tick_once();
    }
    CHECK(gs->story_block() && battles.conflicts().space_queue().size() == 1);
    const int32_t frozen = gs->frame();
    for (int i = 0; i < 200; ++i) host.tick_once();
    CHECK(gs->frame() == frozen && gs->paused());
    CHECK(battles.conflicts().state() == galactic::ConflictState::Idle && battles.conflicts().space_queue().size() == 1);
    CHECK(!battles.battle() && battles.battles_fought() == 0 && battles.conflicts().log().empty());
    gs->clear_linked_battle();
    bool started = false;
    for (int i = 0; i < 600 && !started; ++i) {
        host.tick_once();
        started = battles.battle() != nullptr;
    }
    CHECK(started && gs->frame() > frozen);
    std::printf("  story block: queued at tick %" PRIu64 ", frame %d frozen for 200 ticks, battle started after the flag cleared\n",
                t, frozen);
}

void test_story_battle(const Data& d) {
    const fs::path x = d.root / "Data/XML";
    write_file(x / "Story_Plots_S10.xml", "<Story_Mode_Plots><Active_Plot>Story_S10.xml</Active_Plot></Story_Mode_Plots>\n");
    write_file(x / "Story_S10.xml",
               "<Story>\n"
               " <Event Name=\"Go_Space\"><Event_Type>STORY_SPACE_TACTICAL</Event_Type><Event_Param1>Story_Plots_S10_Tac.xml</Event_Param1></Event>\n"
               " <Event Name=\"Won_Space\"><Event_Type>STORY_WIN_BATTLES</Event_Type><Event_Param1>1</Event_Param1><Event_Param2>SPACE</Event_Param2></Event>\n"
               " <Event Name=\"Lost_Space\"><Event_Type>STORY_LOSE_BATTLES</Event_Type><Event_Param1>1</Event_Param1><Event_Param2>SPACE</Event_Param2></Event>\n"
               "</Story>\n");
    write_file(x / "Story_Plots_S10_Tac.xml",
               "<Story_Mode_Plots><Lua_Script>Story_S10_Tac</Lua_Script><Active_Plot>Story_S10_Tac.xml</Active_Plot></Story_Mode_Plots>\n");
    write_file(x / "Story_S10_Tac.xml",
               "<Story>\n"
               // L1/S4 wave 2b (SCRIPT_GAPS.md §4.2 / §4.5): VICTORY names one faction (empty never fires);
               // TACTICAL_DESTROY needs the dead type in Param1.
               " <Event Name=\"Victory_E\"><Event_Type>STORY_VICTORY</Event_Type><Event_Param1>Empire</Event_Param1></Event>\n"
               " <Event Name=\"Victory_U\"><Event_Type>STORY_VICTORY</Event_Type><Event_Param1>Underworld</Event_Param1></Event>\n"
               " <Event Name=\"Victory_Never\"><Event_Type>STORY_VICTORY</Event_Type></Event>\n"
               " <Event Name=\"First_Kill\"><Event_Type>STORY_TACTICAL_DESTROY</Event_Type>"
               "<Event_Param1>S10_Frigate, S10_Corvette, S10_Star_Base_1</Event_Param1></Event>\n"
               " <Event Name=\"Script_Saw\"><Event_Type>STORY_AI_NOTIFICATION</Event_Type><Event_Param2>TAC_SCRIPT</Event_Param2></Event>\n"
               // fog follow-up (FOG_OF_WAR.md §6): the map centre is fogged for the Empire (no REVEAL units in this
               // data) until the script's FogOfWar.Reveal; that fires Fog_Point, whose ENABLE_FOW 0 reward switches
               // the battle's fog off. Units without TEAM / HIDE_WHEN_FOGGED are never hidden: Fog_Object fires at once.
               " <Event Name=\"Script_Fog\"><Event_Type>STORY_AI_NOTIFICATION</Event_Type><Event_Param2>FOG_CELLS</Event_Param2></Event>\n"
               " <Event Name=\"Fog_Point\"><Event_Type>STORY_FOG_POSITION_REVEAL</Event_Type><Event_Param1>0, 0, 0</Event_Param1>"
               "<Reward_Type>ENABLE_FOW</Reward_Type><Reward_Param1>0</Reward_Param1></Event>\n"
               " <Event Name=\"Fog_Object\"><Event_Type>STORY_FOG_OBJECT_REVEAL</Event_Type><Event_Param1>S10_Corvette</Event_Param1></Event>\n"
               "</Story>\n");
    write_file(d.root / "Data/Scripts/Story/Story_S10_Tac.lua",
               "function Base_Definitions() ServiceRate = 0.1 done = false revealed = false end\n"
               "function main() while true do\n"
               "  if not done then local f = Find_First_Object('S10_Frigate')\n"
               "    if f then local h = f.Get_Hull() if h > 0 and h <= 1 then Story_Event('TAC_SCRIPT') end\n"
               "      f.Activate_Ability('POWER_TO_WEAPONS') done = true end\n"
               "  elseif not revealed then local f = Find_First_Object('S10_Frigate') local p = Find_Player('Empire')\n"
               "    if f and p then cells = FogOfWar.Reveal(p, f, 20000) revealed = true\n"
               "      if cells then Story_Event('FOG_CELLS') end end end\n"
               "  coroutine.yield(true) end end\n");
    DirectoryProvider files(path_to_utf8(d.root));
    const StoryRun a = run_story_battle(d, files, 0);
    const StoryRun b = run_story_battle(d, files, 4);
    for (const auto& m : a.lua) std::printf("  lua: %s\n", m.c_str());
    auto fired = [&a](const char* ev) { return a.log.find(std::string(" ") + ev + " ") != std::string::npos; };
    std::printf("  story battle: %zu log bytes, Lua activations %u, identical for 0 / 4 workers: %s\n", a.log.size(), a.activations,
                a.log == b.log ? "yes" : "NO");
    CHECK(a.lua.empty() && a.activations > 0);
    // L1/S4 wave 2b feeds: movement transitions (UNIT_ARRIVED / FULL_STOP) and reserve deployments (REINFORCE).
    auto count = [&a](const char* k) {
        size_t n = 0;
        for (size_t p = a.log.find(k); p != std::string::npos; p = a.log.find(k, p + 1)) ++n;
        return n;
    };
    std::printf("  wave 2b feeds: TacUnitArrived %zu, TacFullStop %zu, TacReinforced %zu, TacBaseDestroyed %zu\n", count("TacUnitArrived"),
                count("TacFullStop"), count("TacReinforced"), count("TacBaseDestroyed"));
    CHECK(count("TacUnitArrived") + count("TacFullStop") > 0);
    CHECK(fired("Go_Space") && (fired("Victory_E") != fired("Victory_U")) && !fired("Victory_Never") && fired("First_Kill") &&
          fired("Script_Saw") && (fired("Won_Space") || fired("Lost_Space")));
    CHECK(a.log.find("BattleStarted") != std::string::npos && a.log.find("BattleEnded") != std::string::npos &&
          a.log.find("TacDestroyed") != std::string::npos);
    CHECK(!a.log.empty() && a.log == b.log);
#if defined(ALAMO_SIM_HAVE_FOG) && defined(ALAMO_LUA_HAVE_FOG)
    // fog follow-up: FogOfWar.Reveal from the tactical script -> STORY_FOG_POSITION_REVEAL -> ENABLE_FOW 0.
    std::printf("  fog: Script_Fog %d, Fog_Point %d, Fog_Object %d, FOW active seen %d / off seen %d\n", fired("Script_Fog") ? 1 : 0,
                fired("Fog_Point") ? 1 : 0, fired("Fog_Object") ? 1 : 0, a.fog_on_seen ? 1 : 0, a.fog_off_seen ? 1 : 0);
    CHECK(fired("Script_Fog") && fired("Fog_Point") && fired("Fog_Object"));
    CHECK(a.fog_on_seen && a.fog_off_seen);
#endif
}

// --- S4 (owner: S4 agent): the remaining S4 event sources in story land battles (s4_types data; docs/re/
// L1_S4_REMAINING.md §3.5, §4). The human Empire defends S10_B (its galactic plot's STORY_LAND_TACTICAL loads
// Story_Plots_S4_Tac.xml, so the Empire is the tactical plot player = the "local player"); commands go through SimHost's
// order hook (a stand-in for apps/game's router):
//   battle-load screen: STORY_LOAD_TACTICAL_MAP (galactic, planet S10_B, land) -> SET_TACTICAL_MAP S4_Land_Alt replaces
//                    the battle's map (the loader records the name it was asked for)
//   battle frame 5   Select the Empire troopers + MoveToPosition onto the capture post -> STORY_COMMAND_UNIT
//   battle frame 40  Select one trooper + Escort another                              -> STORY_GUARD_UNIT
//   battle frame 60  Garrison (52) of trooper 0 into the neutral bunker (also a capture point) -> approach, 30-frame
//                    entering, STORY_GARRISON_UNIT; rule (b) makes the bunker the Empire's -> STORY_CAPTURE_STRUCTURE
//   60 frames inside: Garrison (52) with garrison = 0x3FFFFF (leave)
//   battle frame 80  battles.change_owner on the Underworld hero: STORY_CAPTURE_HERO still never fires (§4.3)
//   the troopers on the post capture it (1 s, countdown 0)                -> STORY_CAPTURE_STRUCTURE (Param2 Empire)
// The corruption runs make the Underworld human (a mission needs a non-AI corrupter) and start its Kidnapping corruption
// at S10_B (CampaignBattles::start_corruption with its orbit ground units, or the hero alone): a land battle with
// Story_Plots_S4_Mission.xml -> STORY_CORRUPTION_TACTICAL_COMPLETE / _FAILED by the winner; the Piracy option of S10_A
// (no mission) then applies at once. In the plain run the AI Underworld's Piracy at S10_A applies directly too.
struct S4Run {
    std::string log;
    std::vector<std::string> lua, battles, maps;
    CrcLog crc;
    bool garrison_flag = false, left = false, readded = false, bunker_taken = false, post_taken = false;
    bool fire_query = false, can_query = false, hero_changed = false, contained = false;
    // a1f t07 (attack_sensor runs): the neutral sensor is a combat unit and a capture point, untouched by the target scan;
    // S7 t30: the attack order on it is refused (relation 2), so it stays alive and a capture point.
    bool sensor_combat = false, sensor_point = false, sensor_untouched = false, sensor_killed = false, sensor_point_removed = false;
    int attack_order = -1;
    float sensor_health = -1.0f;  // last health fraction seen
    uint32_t garrisoned_frames = 0;
    int mission = -1, piracy = -1;
    int32_t winner = -2;
    size_t mission_units = 99;          // story t08: galactic units in the mission battle's request (start + reserve)
    uint64_t mission_tick = 0, battle_tick = 0;
    int32_t corruption_b = -2, corruption_a = -2;
    float credits_before = -1.0f, credits_after = -1.0f;
};

terrain::MapData s4_map(bool sensor = false) {
    terrain::MapData m = land_map();
    auto place = [&m](const char* type, float x, float y, uint32_t owner, float yaw = 0.0f) {
        terrain::ObjectPlacement p;
        p.type_crc = name_crc(type);
        p.owner = owner;
        p.position[0] = x;
        p.position[1] = y;
        p.rotation_deg[2] = yaw;
        m.objects.push_back(p);
    };
    // REMAINING_GAPS §3.3: placements keep their map owner (player id): the post and the bunker belong to the neutral
    // player (2) of this galaxy.
    // Yaw 90 = facing +x (a placement's facing is Rz(yaw) x the model nose -Y): the facings this scenario was laid out
    // with (defenders' row along y, the bunker's door side).
    place("Defending Forces Position", 900.0f, 660.0f, 0, 90.0f);  // the defenders' anchor (marker, no type needed)
    place("S4_Post", 900.0f, 660.0f, 2, 90.0f);                    // on the defender anchor
    place("S4_Bunker", 900.0f, 700.0f, 2, 90.0f);                  // among the defenders' first row
    if (sensor) place("S4_Sensor", 940.0f, 660.0f, 2, 90.0f);      // a1f t07: next to the defenders, neutral
    return m;
}

// corruption: 0 none, 1 mission, 2 hero alone, 3 (story t08) mission with no Underworld unit near S10_B; attack_sensor (a1f t07): the map gets a neutral S4_Sensor and the Empire
// troopers only attack it (frame 60), no other scripted orders.
S4Run run_s4_battle(const Data& d, const FileProvider& files, int32_t workers, int corruption, bool attack_sensor = false) {
    using R = commands::RejectReason;
    using T = commands::EventType;
    S4Run out;
    jobs::JobSystem js(jobs::JobSystemConfig{workers});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    cfg.story_crc = true;
    SimHost host(d.types, js, cfg);
    galactic::GalacticSetup setup = make_setup(d, true);
    setup.forces.push_back({1, 2, type_of(d, "S4_Hero")});  // an Underworld named hero rides along
    if (corruption) {  // a corruption mission needs a non-AI corrupting player (§4.5)
        setup.players[1].human = true;
        setup.players[1].ai = false;
    }
    galactic::GalacticSim* gs = host.start_galactic(d.rules, setup);
    CHECK(gs != nullptr);
    if (!gs) return out;
    StoryStart st;
    st.files = &files;
    st.plot_sets = {{0, "Story_Plots_S4.xml"}};
    std::vector<std::string> diag;
    CHECK(host.start_story(st, &diag));
    for (const auto& m : diag) std::printf("  story diag: %s\n", m.c_str());
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    // story t08 + t06: a corruption mission brings no galactic unit and the victory monitor tests only when a
    // victory-relevant object leaves a player, so the synthetic mission (no plot VICTORY) ends at the time limit, where
    // the defender holds [port]; kept short so the galaxy is back before the piracy step at tick 2700.
    opt.battle.max_frames = corruption ? 1200 : 6000;
    opt.battle.skip_human_units = corruption == 0;  // the corruption runs let the stand-in fight for both sides
    opt.battle.land_capture_countdown = 0.0f;
    std::vector<std::string>* maps = &out.maps;
    CampaignBattles battles(host, d.crules, *d.catalog,
                            [maps, attack_sensor](const std::string& name, terrain::MapData& m) {
                                maps->push_back(name);
                                if (name != "S10_Land_B" && name != "S4_Land_Alt") return false;
                                m = s4_map(attack_sensor);
                                return true;
                            },
                            opt);
    CHECK(battles.attach());
    // apps/game battle_orders.cpp stand-in: MoveToPosition / Escort of the issuer's selection into the battle.
    host.set_order_hook([&battles](SimHost& h, uint8_t p, const commands::EventData& ev, const std::vector<objects::ObjectId>& sel) {
        TacticalBattle* b = battles.battle();
        TacticalMode* tm = h.tactical();
        if (!b || !tm) return R::Unhandled;
        SimRng& rng = h.random().synced();
        if (ev.type() == T::MoveToPosition) return b->order_move(*tm, p, sel, ev.f("x"), ev.f("y"), rng);
        if (ev.type() == T::Escort) return b->order_move_to_object(*tm, p, sel, static_cast<objects::ObjectId>(ev.u("target")), rng);
        return R::Unhandled;
    });
    std::vector<objects::ObjectHandle> up;
    for (objects::ObjectHandle h : gs->planets()[2].surface)
        if (gs->store().owner(h) == 1) up.push_back(h);
    CHECK(gs->launch_units(1, 2, up) == R::None);
    const std::vector<uint32_t> uf = gs->fleets_at(2, 1);
    CHECK(!uf.empty());
    // GRC 1.1: a human's hop needs the target's raw revealed byte - set by the galactic pre-step of the first frame
    // (auto-reveal spread from the held planets).
    if (gs->players()[1].human) {  // the hop gate reads the local player's (0) bytes; S10_B is held by player 0
        for (int i = 0; i < 4 && !gs->planet_revealed_raw(1); ++i) host.tick_once();
        CHECK(gs->planet_revealed_raw(1));
    }
    if (!uf.empty() && corruption != 3) CHECK(gs->move_fleet(1, uf[0], 1) == R::None);
    bool invaded = false;
    std::vector<objects::ObjectId> troopers;
    objects::ObjectId bunker = objects::kNoObjectId, post = objects::kNoObjectId, sensor = objects::kNoObjectId;
    const objects::TypeIndex trooper_t = d.types.resolve_name("S10_Trooper");
    for (uint64_t t = 0; t < 3000; ++t) {
        host.tick_once();
        // GRC 4.3: early enough for the 5 s gain (150 galactic frames) to complete and raise CORRUPTION_CHANGED.
        if (t >= 2700 && host.modes().depth() == 1 && out.piracy < 0) {
            out.corruption_b = gs->planets()[1].corruption;
            // S10_A's Piracy option has no mission: applied directly (AI or human corrupter) as a 5 s timed gain (GRC 4.3).
            out.piracy = static_cast<int>(battles.start_corruption(0, 1, 1));
            out.corruption_a = gs->corruption_transition_active(0) ? gs->effective_corruption_type(0) : int8_t{-2};
        }
        if (corruption == 3 && !invaded && host.modes().depth() == 1) {  // story t08: the linked start needs no unit in orbit
            CHECK(gs->fleets_at(1, 1).empty());
            out.credits_before = gs->credits(1);
            out.mission = static_cast<int>(battles.start_corruption(1, 1, 2));
            out.credits_after = gs->credits(1);
            out.mission_tick = host.frame();
            invaded = true;
        }
        if (!out.battle_tick && battles.battle()) {
            out.battle_tick = host.frame();
            out.mission_units = 0;
            for (const galactic::BattleForces& bf : battles.conflicts().active_request().forces) out.mission_units += bf.start.size() + bf.reserve.size();
        }
        if (!invaded && host.modes().depth() == 1) {
            const std::vector<uint32_t> f = gs->fleets_at(1, 1);
            if (!f.empty() && !gs->fleets()[f[0]].moving) {
                if (corruption) {
                    std::vector<objects::ObjectHandle> ground;
                    for (objects::ObjectHandle h : gs->fleets()[f[0]].contents)
                        if (gs->store().alive(h) && gs->rules().types[gs->store().type(h)].kind == galactic::UnitKind::GroundUnit)
                            ground.push_back(h);
                    if (corruption == 2) {  // only the hero goes in: the defenders should win
                        std::vector<objects::ObjectHandle> hero;
                        for (objects::ObjectHandle h : ground)
                            if (gs->store().type(h) == d.types.resolve_name("S4_Hero")) hero.push_back(h);
                        ground = hero;
                    }
                    CHECK(battles.start_corruption(1, 1, 5) == CampaignBattles::CorruptionStart::Refused);  // no Bribery option
                    CHECK(battles.start_corruption(1, 0, 2) == CampaignBattles::CorruptionStart::Refused);  // own planet
                    out.credits_before = gs->credits(1);
                    out.mission = static_cast<int>(battles.start_corruption(1, 1, 2, ground));
                    out.credits_after = gs->credits(1);
                } else {
                    CHECK(gs->invade(1, 1, {}) == R::None);
                }
                invaded = true;
            }
        }
        TacticalBattle* b = battles.battle();
        TacticalMode* tm = host.tactical();
        if (b && tm) {
            const objects::ObjectStore& s = tm->primary();
            if (troopers.empty()) {
                for (objects::ObjectHandle h : s.owned_by(0))
                    if (s.alive(h) && s.type(h) == trooper_t && b->movement().has_unit(s.id(h))) troopers.push_back(s.id(h));
                for (objects::ObjectHandle h : s.all_objects()) {
                    if (s.type(h) == d.types.resolve_name("S4_Bunker")) bunker = s.id(h);
                    if (s.type(h) == d.types.resolve_name("S4_Post")) post = s.id(h);
                    if (s.type(h) == d.types.resolve_name("S4_Sensor")) sensor = s.id(h);
                }
            }
            const uint32_t f = b->frame();
            if (attack_sensor) {  // a1f t07
                if (troopers.empty() || sensor == objects::kNoObjectId) continue;
                const tactical::combat::CombatSystem& c = b->combat();
                if (f == 2) {
                    out.sensor_combat = c.has_unit(sensor) && c.alive(sensor) && c.max_health(sensor) > 0.0f;
                    out.sensor_point = b->captures() && b->captures()->is_point(sensor);
                }
                if (f == 60) {  // the troopers stood next to it for 60 frames: the target scan never picked it
                    out.sensor_untouched = out.sensor_combat && c.health(sensor) == c.max_health(sensor);
                    out.attack_order = static_cast<int>(b->order_attack(*tm, 0, troopers, sensor, host.random().synced()));
                }
                if (out.sensor_combat && c.alive(sensor)) out.sensor_health = c.health(sensor) / c.max_health(sensor);
                if (out.sensor_combat && !out.sensor_killed && !c.alive(sensor)) out.sensor_killed = true;
                if (out.sensor_killed && b->captures() && !b->captures()->is_point(sensor)) out.sensor_point_removed = true;
                continue;
            }
            if (std::getenv("S4_DEBUG") && (f % 50) == 1 && !troopers.empty()) {
                for (const objects::ObjectId id : troopers) {
                    const objects::ObjectHandle h = s.find(id);
                    if (!s.alive(h)) continue;
                    const Vec3 p = s.position(h);
                    std::printf("   f%u trooper %d at %.0f,%.0f goal %d holder %d pending %d\n", f, id, static_cast<double>(p.x),
                                static_cast<double>(p.y), b->movement().has_unit(id) ? b->movement().view(id).has_goal : -1,
                                b->garrison_holder_of(id), b->garrison().pending_holder(id));
                }
                if (bunker != objects::kNoObjectId && post != objects::kNoObjectId)
                    std::printf("   f%u bunker owner %d; post owner %d progress %.2f\n", f, s.owner(s.find(bunker)), s.owner(s.find(post)),
                                b->captures() ? static_cast<double>(b->captures()->progress(post)) : 0.0);
            }
            auto ids = [](std::initializer_list<objects::ObjectId> l) {
                std::vector<uint32_t> w;
                for (const objects::ObjectId id : l) w.push_back(static_cast<uint32_t>(id));
                return w;
            };
            if (!corruption && troopers.size() >= 3) {
                if (f == 5) {
                    std::vector<uint32_t> all;
                    for (const objects::ObjectId id : troopers) all.push_back(static_cast<uint32_t>(id));
                    commands::EventData sel(T::Select);
                    sel.set_ids("ids", all);
                    host.submit(0, sel);
                    commands::EventData mv(T::MoveToPosition);
                    mv.set_f("x", 900.0f).set_f("y", 660.0f).set_f("z", 0.0f);
                    host.submit(0, mv);
                } else if (f == 40) {
                    commands::EventData sel(T::Select);
                    sel.set_ids("ids", ids({troopers[1]}));
                    host.submit(0, sel);
                    commands::EventData es(T::Escort);
                    es.set("target", static_cast<uint32_t>(troopers[2]));
                    host.submit(0, es);
                } else if (f == 60) {
                    out.can_query = b->can_garrison(troopers[0], bunker) && !b->can_garrison(troopers[0], post) &&
                                    b->can_garrison_fire(bunker) && b->contained_by(troopers[0]) == objects::kNoObjectId;
                    commands::EventData g(T::Garrison);
                    g.set("unit", static_cast<uint32_t>(troopers[0])).set("garrison", static_cast<uint32_t>(bunker));
                    host.submit(0, g);
                }
                // battle host: the hero is no longer the attacker's lead unit (TACTICAL_GAPS §4.2 land split: the lead is the
                // first non-hero, the hero waits in the reserve) - capture it once it is on the map (frame >= 80).
                if (f >= 80 && !out.hero_changed)
                    for (objects::ObjectHandle h : s.owned_by(1))
                        if (s.alive(h) && s.type(h) == d.types.resolve_name("S4_Hero") && !out.hero_changed) out.hero_changed = battles.change_owner(h, 0);
                if (b->garrison_holder_of(troopers[0]) == bunker && bunker != objects::kNoObjectId) {
                    ++out.garrisoned_frames;
                    out.garrison_flag = (b->combat().flags(troopers[0]) & tactical::combat::kUnitGarrisoned) != 0 &&
                                        !b->movement().has_unit(troopers[0]);
                    out.fire_query = b->can_garrison_fire(troopers[0]) && b->garrisoned_units(bunker).size() == 1;
                    out.contained = b->contained_by(troopers[0]) == bunker;
                    out.bunker_taken = out.bunker_taken || s.owner(s.find(bunker)) == 0;
                    if (out.garrisoned_frames == 60) {
                        commands::EventData g(T::Garrison);
                        g.set("unit", static_cast<uint32_t>(troopers[0])).set("garrison", commands::kNoObject);
                        host.submit(0, g);
                    }
                } else if (out.garrisoned_frames >= 60 && !out.left) {
                    out.left = true;
                    out.readded = b->movement().has_unit(troopers[0]) &&
                                  (b->combat().flags(troopers[0]) & tactical::combat::kUnitGarrisoned) == 0 &&
                                  b->contained_by(troopers[0]) == objects::kNoObjectId;
                }
                if (post != objects::kNoObjectId) out.post_taken = out.post_taken || s.owner(s.find(post)) == 0;
            }
        }
    }
    if (corruption)
        for (const galactic::ConflictLogEntry& e : battles.conflicts().log())
            if (e.how == 2) out.winner = e.winner;
    if (host.story()) {
        out.log = host.story()->log_text("S4 story battle");
        out.lua = host.story()->lua_messages();
    }
    out.battles = battles.log();
    out.crc = host.crc_log();
    return out;
}

void test_s4_battle(const Data& d) {
    const fs::path x = d.root / "Data/XML";
    write_file(x / "Story_Plots_S4.xml", "<Story_Mode_Plots><Active_Plot>Story_S4.xml</Active_Plot></Story_Mode_Plots>\n");
    write_file(x / "Story_S4.xml",
               "<Story>\n"
               " <Event Name=\"Go_Land\"><Event_Type>STORY_LAND_TACTICAL</Event_Type><Event_Param1>Story_Plots_S4_Tac.xml</Event_Param1></Event>\n"
               // L1_S4_REMAINING.md §4.4: Param1 planets, Param3 GROUND / SPACE; the reward fills the land map slot.
               " <Event Name=\"Map_Hook\"><Event_Type>STORY_LOAD_TACTICAL_MAP</Event_Type><Event_Param1>S10_B</Event_Param1>"
               "<Event_Param3>GROUND</Event_Param3><Reward_Type>SET_TACTICAL_MAP</Reward_Type><Reward_Param1>S4_Land_Alt</Reward_Param1>"
               "<Reward_Param2>GROUND</Reward_Param2></Event>\n"
               " <Event Name=\"Map_Space_Never\"><Event_Type>STORY_LOAD_TACTICAL_MAP</Event_Type><Event_Param1>S10_B</Event_Param1>"
               "<Event_Param3>SPACE</Event_Param3></Event>\n"
               // Param2 entry 0: the first S4_Hero object of the galactic store must be at the battle planet (it rides in the
               // invading fleet); no S10_Corvette exists in the land setup.
               " <Event Name=\"Map_Hero\"><Event_Type>STORY_LOAD_TACTICAL_MAP</Event_Type><Event_Param1>S10_B</Event_Param1>"
               "<Event_Param2>S4_Hero</Event_Param2></Event>\n"
               " <Event Name=\"Map_Hero_Never\"><Event_Type>STORY_LOAD_TACTICAL_MAP</Event_Type><Event_Param1>S10_B</Event_Param1>"
               "<Event_Param2>S10_Corvette</Event_Param2></Event>\n"
               // SCRIPT_GAPS.md §4.7: Param1 planets, Param2 the corruption keyword (default any).
               " <Event Name=\"Corrupt_Done\"><Event_Type>STORY_CORRUPTION_TACTICAL_COMPLETE</Event_Type><Event_Param1>S10_B</Event_Param1>"
               "<Event_Param2>KIDNAPPING</Event_Param2></Event>\n"
               " <Event Name=\"Corrupt_Failed\"><Event_Type>STORY_CORRUPTION_TACTICAL_FAILED</Event_Type><Event_Param1>S10_B</Event_Param1></Event>\n"
               " <Event Name=\"Corrupt_Wrong_Type\"><Event_Type>STORY_CORRUPTION_TACTICAL_COMPLETE</Event_Type><Event_Param1>S10_B</Event_Param1>"
               "<Event_Param2>BRIBERY</Event_Param2></Event>\n"
               " <Event Name=\"Pirated\"><Event_Type>STORY_CORRUPTION_CHANGED</Event_Type><Event_Param1>S10_A</Event_Param1>"
               "<Event_Param2>PIRACY</Event_Param2></Event>\n"
               "</Story>\n");
    write_file(x / "Story_Plots_S4_Tac.xml", "<Story_Mode_Plots><Active_Plot>Story_S4_Tac.xml</Active_Plot></Story_Mode_Plots>\n");
    write_file(x / "Story_S4_Tac.xml",
               "<Story>\n"
               " <Event Name=\"Commanded\"><Event_Type>STORY_COMMAND_UNIT</Event_Type><Event_Param1>S10_Trooper</Event_Param1></Event>\n"
               " <Event Name=\"Commanded_Near_Post\"><Event_Type>STORY_COMMAND_UNIT</Event_Type><Event_Param1>S10_Trooper</Event_Param1>"
               "<Event_Param2>S4_Post</Event_Param2><Event_Param3>300</Event_Param3></Event>\n"
               " <Event Name=\"Commanded_Far_Never\"><Event_Type>STORY_COMMAND_UNIT</Event_Type><Event_Param1>S10_Trooper</Event_Param1>"
               "<Event_Param2>S4_Bunker</Event_Param2><Event_Param3>10</Event_Param3></Event>\n"
               " <Event Name=\"Commanded_Frigate_Never\"><Event_Type>STORY_COMMAND_UNIT</Event_Type><Event_Param1>S10_Frigate</Event_Param1></Event>\n"
               " <Event Name=\"Guarded\"><Event_Type>STORY_GUARD_UNIT</Event_Type><Event_Param1>S10_Trooper</Event_Param1>"
               "<Event_Param2>S10_Trooper</Event_Param2></Event>\n"
               " <Event Name=\"Guard_Bunker_Never\"><Event_Type>STORY_GUARD_UNIT</Event_Type><Event_Param1>S10_Trooper</Event_Param1>"
               "<Event_Param2>S4_Bunker</Event_Param2></Event>\n"
               " <Event Name=\"Garrisoned\"><Event_Type>STORY_GARRISON_UNIT</Event_Type><Event_Param1>S10_Trooper</Event_Param1></Event>\n"
               " <Event Name=\"Garrison_Enemy_Never\"><Event_Type>STORY_GARRISON_UNIT</Event_Type><Event_Param1>S10_Trooper</Event_Param1>"
               "<Event_Param3>FILTER_ENEMY_ONLY</Event_Param3></Event>\n"
               // §4.2: Param1 structure types AND Param2 the new owner's faction; an empty Param2 never fires.
               " <Event Name=\"Post_Captured\"><Event_Type>STORY_CAPTURE_STRUCTURE</Event_Type><Event_Param1>S4_Post</Event_Param1>"
               "<Event_Param2>Empire</Event_Param2></Event>\n"
               " <Event Name=\"Bunker_Captured\"><Event_Type>STORY_CAPTURE_STRUCTURE</Event_Type><Event_Param1>S4_Bunker</Event_Param1>"
               "<Event_Param2>Empire</Event_Param2></Event>\n"
               " <Event Name=\"Capture_No_Faction_Never\"><Event_Type>STORY_CAPTURE_STRUCTURE</Event_Type><Event_Param1>S4_Post</Event_Param1></Event>\n"
               " <Event Name=\"Capture_Wrong_Faction_Never\"><Event_Type>STORY_CAPTURE_STRUCTURE</Event_Type><Event_Param1>S4_Post</Event_Param1>"
               "<Event_Param2>Underworld</Event_Param2></Event>\n"
               // §4.3: no dispatcher in 1.121.
               " <Event Name=\"Hero_Captured_Never\"><Event_Type>STORY_CAPTURE_HERO</Event_Type><Event_Param1>S4_Hero</Event_Param1>"
               "<Event_Param3>FILTER_NONE</Event_Param3></Event>\n"
               "</Story>\n");
    write_file(x / "Story_Plots_S4_Mission.xml", "<Story_Mode_Plots><Active_Plot>Story_S4_Mission.xml</Active_Plot></Story_Mode_Plots>\n");
    write_file(x / "Story_S4_Mission.xml",
               "<Story>\n"
               " <Event Name=\"Mission_Won_U\"><Event_Type>STORY_VICTORY</Event_Type><Event_Param1>Underworld</Event_Param1></Event>\n"
               " <Event Name=\"Mission_Won_E\"><Event_Type>STORY_VICTORY</Event_Type><Event_Param1>Empire</Event_Param1></Event>\n"
               "</Story>\n");
    DirectoryProvider files(path_to_utf8(d.root));
    const S4Run a = run_s4_battle(d, files, 0, 0);
    for (const auto& m : a.lua) std::printf("  lua: %s\n", m.c_str());
    for (const auto& m : a.battles) std::printf("  %s\n", m.c_str());
    auto fired_in = [](const std::string& log, const char* ev) { return log.find(std::string(" ") + ev + " ") != std::string::npos; };
    auto fired = [&a, &fired_in](const char* ev) { return fired_in(a.log, ev); };
    auto count = [&a](const char* k) {
        size_t n = 0;
        for (size_t p = a.log.find(k); p != std::string::npos; p = a.log.find(k, p + 1)) ++n;
        return n;
    };
    std::printf("  S4 feeds: TacUnitCommanded %zu, TacUnitGuarding %zu, TacGarrisoned %zu, TacStructureCaptured %zu, LoadTacticalMap %zu; maps",
                count("TacUnitCommanded"), count("TacUnitGuarding"), count("TacGarrisoned"), count("TacStructureCaptured"),
                count("LoadTacticalMap"));
    for (const std::string& m : a.maps) std::printf(" %s", m.c_str());
    std::printf("\n  garrison: inside %u frames, flag %d, fire %d, queries %d, contained %d, left %d, re-added %d, bunker taken %d, post taken %d\n",
                a.garrisoned_frames, a.garrison_flag ? 1 : 0, a.fire_query ? 1 : 0, a.can_query ? 1 : 0, a.contained ? 1 : 0,
                a.left ? 1 : 0, a.readded ? 1 : 0, a.bunker_taken ? 1 : 0, a.post_taken ? 1 : 0);
    CHECK(a.lua.empty());
    CHECK(fired("Go_Land") && fired("Map_Hook") && fired("Map_Hero") && !fired("Map_Space_Never") && !fired("Map_Hero_Never"));
    CHECK(a.maps.size() == 1 && a.maps[0] == "S4_Land_Alt");  // SET_TACTICAL_MAP replaced S10_Land_B
    CHECK(fired("Commanded") && fired("Commanded_Near_Post") && !fired("Commanded_Far_Never") && !fired("Commanded_Frigate_Never"));
    CHECK(fired("Guarded") && !fired("Guard_Bunker_Never"));
    CHECK(fired("Garrisoned") && !fired("Garrison_Enemy_Never"));
    CHECK(fired("Post_Captured") && fired("Bunker_Captured") && !fired("Capture_No_Faction_Never") && !fired("Capture_Wrong_Faction_Never"));
    CHECK(a.hero_changed && !fired("Hero_Captured_Never"));
    CHECK(a.garrisoned_frames >= 60 && a.garrison_flag && a.fire_query && a.can_query && a.contained && a.left && a.readded &&
          a.bunker_taken && a.post_taken);
    // The AI Underworld's corruption applies directly (no mission option at S10_A anyway).
    CHECK(a.piracy == static_cast<int>(CampaignBattles::CorruptionStart::Applied) && a.corruption_a == 1 && fired("Pirated"));
    // Determinism: the story log and the per-tick CRCs (story system included) for 0 / 1 / 4 / 15 workers.
    for (const int32_t w : {1, 4, 15}) {
        const S4Run b = run_s4_battle(d, files, w, 0);
        CHECK(b.log == a.log && !first_divergence(a.crc, b.crc).diverged);
    }
    // a1f t07: a map-placed neutral SELECTABLE capture structure (Secondary_Abandoned_Sensor stand-in) is a battle combat
    // unit as well as a capture point; the target scan skips the Is_Neutral player's objects (TACTICAL_COMBAT §2.2), so
    // the Empire troopers next to it leave it alone. S7 t30 (TACTICAL_COMBAT §2.10): the Neutral player is relation 2, so
    // the human attack order is refused too (the attack cursor needs relation 1): the sensor is never damaged and stays a
    // capture point (taken by presence, not by attack).
    {
        const S4Run s = run_s4_battle(d, files, 0, 0, true);
        std::printf("  neutral sensor: combat %d, capture point %d, untouched %d, attack order %d, health %.2f, killed %d, point removed %d\n",
                    s.sensor_combat ? 1 : 0, s.sensor_point ? 1 : 0, s.sensor_untouched ? 1 : 0, s.attack_order,
                    static_cast<double>(s.sensor_health), s.sensor_killed ? 1 : 0, s.sensor_point_removed ? 1 : 0);
        for (const auto& m : s.battles) std::printf("  %s\n", m.c_str());
        CHECK(s.sensor_combat && s.sensor_point && s.sensor_untouched);
        CHECK(s.attack_order == static_cast<int>(commands::RejectReason::InvalidTarget) && !s.sensor_killed && s.sensor_health == 1.0f &&
              !s.sensor_point_removed);
        const S4Run s4 = run_s4_battle(d, files, 4, 0, true);
        CHECK(!first_divergence(s.crc, s4.crc).diverged);
    }
    // Corruption mission (human Underworld).
    const S4Run c = run_s4_battle(d, files, 0, 1);
    for (const auto& m : c.battles) std::printf("  %s\n", m.c_str());
    std::printf("  corruption: mission %d (credits %.0f -> %.0f), winner %d, B corruption %d, A piracy %d; Corrupt_Done %d, Corrupt_Failed %d\n",
                c.mission, static_cast<double>(c.credits_before), static_cast<double>(c.credits_after), c.winner, c.corruption_b, c.piracy,
                fired_in(c.log, "Corrupt_Done") ? 1 : 0, fired_in(c.log, "Corrupt_Failed") ? 1 : 0);
    CHECK(c.mission == static_cast<int>(CampaignBattles::CorruptionStart::MissionQueued) && c.credits_after == c.credits_before - 500.0f);
    CHECK(c.winner == 0 || c.winner == 1);
    CHECK(fired_in(c.log, c.winner == 1 ? "Mission_Won_U" : "Mission_Won_E"));
    CHECK(fired_in(c.log, "Corrupt_Done") == (c.winner == 1) && fired_in(c.log, "Corrupt_Failed") == (c.winner != 1));
    CHECK(!fired_in(c.log, "Corrupt_Wrong_Type"));
    if (c.winner == 1) CHECK(c.corruption_b == 2);
    CHECK(c.log.find("CorruptionMission") != std::string::npos);
    CHECK(c.piracy == static_cast<int>(CampaignBattles::CorruptionStart::Applied) && fired_in(c.log, "Pirated"));
    CHECK(c.mission_units == 0);  // story t08: not sandbox, the map and the mission plots supply every unit
    // story t08: no Underworld unit anywhere near S10_B - the mission starts at once through the linked record (BT §2.9.3,
    // L1_S4_REMAINING §4.5), its battle is pushed at the next hook and the mission's story events follow its winner.
    const S4Run n = run_s4_battle(d, files, 0, 3);
    for (const auto& m : n.battles) std::printf("  %s\n", m.c_str());
    std::printf("  corruption, no units in orbit: mission %d (tick %" PRIu64 ", battle tick %" PRIu64 ", galactic units %zu), winner %d\n",
                n.mission, n.mission_tick, n.battle_tick, n.mission_units, n.winner);
    CHECK(n.mission == static_cast<int>(CampaignBattles::CorruptionStart::MissionQueued) && n.credits_after == n.credits_before - 500.0f);
    CHECK(n.battle_tick == n.mission_tick + 1 && n.mission_units == 0 && (n.winner == 0 || n.winner == 1));
    CHECK(n.log.find("CorruptionMission") != std::string::npos);
    CHECK(fired_in(n.log, "Corrupt_Done") == (n.winner == 1) && fired_in(n.log, "Corrupt_Failed") == (n.winner != 1));
    bool started_line = false;
    for (const std::string& m : n.battles) started_line = started_line || m == "corruption: mission land started";
    CHECK(started_line);
    const S4Run c4 = run_s4_battle(d, files, 4, 1);
    CHECK(c4.log == c.log && !first_divergence(c.crc, c4.crc).diverged);
    // The mission with the hero alone: the defenders win -> _FAILED, no corruption.
    const S4Run f = run_s4_battle(d, files, 0, 2);
    std::printf("  corruption, hero alone: mission %d, winner %d, B corruption %d; Corrupt_Done %d, Corrupt_Failed %d\n", f.mission,
                f.winner, f.corruption_b, fired_in(f.log, "Corrupt_Done") ? 1 : 0, fired_in(f.log, "Corrupt_Failed") ? 1 : 0);
    // story t08: the mission starts through the linked record (not sandbox): no galactic unit enters, so the units the
    // test picks no longer matter; the story events follow the winner.
    CHECK(f.mission == static_cast<int>(CampaignBattles::CorruptionStart::MissionQueued) && (f.winner == 0 || f.winner == 1));
    CHECK(fired_in(f.log, "Corrupt_Failed") == (f.winner != 1) && fired_in(f.log, "Corrupt_Done") == (f.winner == 1) &&
          (f.winner == 1 ? f.corruption_b == 2 : f.corruption_b == -1));
    // Int-final: the human corruption command (SpecialAbility kind 6) through the command stream -> start_corruption.
    {
        uint32_t dd = 0, pp = 0;
        int8_t tt = -1;
        CHECK(read_corruption_command(corruption_command(0x12345u, 0x2468u, 6), dd, pp, tt) && dd == 0x12345u && pp == 0x2468u && tt == 6);
        jobs::JobSystem js(jobs::JobSystemConfig{0});
        SimHost host(d.types, js, SimConfig{});
        galactic::GalacticSetup setup = make_setup(d, true);
        setup.forces.push_back({1, 2, type_of(d, "S4_Hero")});
        galactic::GalacticSim* gs = host.start_galactic(d.rules, setup);
        CHECK(gs != nullptr);
        if (gs) {
            CampaignBattleOptions opt;
            CampaignBattles battles(host, d.crules, *d.catalog, [](const std::string&, terrain::MapData&) { return false; }, opt);
            CHECK(battles.attach());
            objects::ObjectHandle hero{};
            for (objects::ObjectHandle h : gs->planets()[2].surface)
                if (gs->store().alive(h) && gs->store().type(h) == d.types.resolve_name("S4_Hero")) hero = h;
            CHECK(gs->store().alive(hero));
            const auto hid = static_cast<uint32_t>(gs->store().id(hero));
            const auto planet_a = static_cast<uint32_t>(gs->store().id(gs->planets()[0].object));
            const uint64_t rejected0 = host.commands_rejected();
            host.submit(0, corruption_command(hid, planet_a, 1));  // not the defiler's owner: refused
            host.tick_once();
            host.tick_once();
            CHECK(host.commands_rejected() == rejected0 + 1 && gs->planets()[0].corruption == galactic::kCorruptionNone);
            host.submit(1, corruption_command(hid, planet_a, 1));  // Piracy at S10_A (no mission): direct timed gain
            host.tick_once();
            host.tick_once();
            std::printf("  corruption command: S10_A corruption %d, result %d\n", gs->planets()[0].corruption,
                        static_cast<int>(battles.last_corruption_start()));
            CHECK(gs->effective_corruption_type(0) == 1 && gs->corruption_transition_active(0) &&
                  battles.last_corruption_start() == CampaignBattles::CorruptionStart::Applied);
            for (int t = 0; t < 2000 && gs->corruption_transition_active(0); ++t) host.tick_once();  // GRC 4.3: 5 s gain
            CHECK(gs->planets()[0].corruption == 1 && !gs->corruption_transition_active(0));
        }
    }
}
// --- end S4 ---
#endif

std::string env_or_empty(const char* name) {
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

#ifdef ALAMO_SIM_HAVE_FOG
// Fog of war in battles (owner: fog agent; docs/re/FOG_OF_WAR.md): units with REVEAL reveal for their side, the
// human sees part of the map and enemy units hidden / shown over time, the AI player is never fogged (force 0),
// the neutral player has no grid; fog is worker-count independent (identical per-frame fog digests and CRC logs for
// 0 and 4 workers). Fog follow-up: the grids are still not in the CRC, but the human's targeting now reads them
// (S8 filter step 4: no target acquired while hidden), so fog on / off may diverge; Force Sight (land) = Reveal_All
// for 2 s; the ping (space) reveals 2000 units ahead one tick after use.
void test_fog_battle(const Data& d) {
    for (const bool land : {false, true}) {
        const uint64_t ticks = land ? 3000 : 2500;
        const Run a = run_campaign(d, land, true, 0, ticks, 0.0f, true);
        const Run off = run_campaign(d, land, true, 0, ticks, 0.0f, false);
        const Run w4 = run_campaign(d, land, true, 4, ticks, 0.0f, true);
        std::printf("  %s: fog frames %u, sources max %u, visible cells max %u, enemy hidden %u / shown %u, refogs %u, digest %016" PRIx64
                    "\n", land ? "land" : "space", a.fog_frames, a.fog_sources_max, a.fog_visible_max, a.fog_hidden, a.fog_shown, a.fog_refogs,
                    a.fog_hash);
        std::printf("  %s: human targets acquired %u (hidden %u), fog on / off CRC %s; %s at frame %u: probe fogged before %d, "
                    "after %d, Reveal_All frames %u\n",
                    land ? "land" : "space", a.fog_new_targets, a.fog_bad_targets,
                    first_divergence(a.crc, off.crc).diverged ? "diverge (human targeting)" : "identical", land ? "Force Sight" : "ping",
                    a.fog_ability_frame, a.fog_probe_before ? 1 : 0, a.fog_probe_after ? 1 : 0, a.fog_force_frames);
        CHECK(a.fought == 1 && a.fog_frames > 100 && a.fog_grids_ok && a.fog_ai_unfogged);
        CHECK(a.fog_sources_max > 0 && a.fog_visible_max > 0 && a.fog_hidden > 0 && a.fog_shown > 0);
        CHECK(off.fog_frames == 0);
        CHECK(a.fog_new_targets > 0 && a.fog_bad_targets == 0);
        CHECK(a.fog_ability_frame == 30 && a.fog_probe_before && a.fog_probe_after);
        if (land) CHECK(a.fog_force_on && a.fog_force_frames >= 59 && a.fog_force_frames <= 61);
        else CHECK(a.fog_force_frames == 0);
        CHECK(a.fog_hash == w4.fog_hash && !first_divergence(a.crc, w4.crc).diverged && a.battles == w4.battles);
        std::printf("  %s: OI fog shots %" PRIu64 " (shooter flashed on the target owner's grid %" PRIu64 "), host API %s\n",
                    land ? "land" : "space", a.fog_shots, a.fog_flashed, a.fog_api_ok ? "ok" : "FAILED");
        CHECK(a.fog_shots > 0 && a.fog_flashed > 0 && a.fog_api_ok && a.fog_api_checked);
    }
}
#endif

#ifdef ALAMO_SIM_HAVE_FOG
// FOWF t05: a land battle with a mod-style setup phase (SetupPhaseEnabled, SetupPhaseCountdownSeconds 3). During setup
// the human moves an own unit with SetupPhaseMove (31; an enemy unit is refused); SetupPhaseTriggerEnd (32) from the AI arms the 90-frame
// countdown, from the (only) human ends the phase at the next tick; afterwards 31 is refused.
struct SetupRun {
    bool setup_at_start = false, moved = false, enemy_refused = false, refused_after = false;
    int ready_cmd = -1;
    uint32_t ready_frame = 0, end_frame = 0;
};

SetupRun run_setup_battle(const Data& d, uint8_t ready_player, int32_t workers) {
    SetupRun out;
    jobs::JobSystem js(jobs::JobSystemConfig{workers});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    SimHost host(d.types, js, cfg);
    galactic::GalacticSim* gs = host.start_galactic(d.rules, make_setup(d, true));
    CHECK(gs != nullptr);
    if (!gs) return out;
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 6000;
    opt.battle.fog = true;
    CampaignBattles battles(host, d.crules, *d.catalog,
                            [](const std::string& name, terrain::MapData& m) {
                                if (name != "S10_Land_B") return false;
                                m = land_map();
                                return true;
                            },
                            opt);
    std::vector<std::string> diag;
    CHECK(battles.attach(&diag));
    std::vector<objects::ObjectHandle> up;
    for (objects::ObjectHandle h : gs->planets()[2].surface)
        if (gs->store().owner(h) == 1) up.push_back(h);
    CHECK(gs->launch_units(1, 2, up) == commands::RejectReason::None);
    const std::vector<uint32_t> uf = gs->fleets_at(2, 1);
    CHECK(!uf.empty());
    if (!uf.empty()) CHECK(gs->move_fleet(1, uf[0], 1) == commands::RejectReason::None);
    using R = commands::RejectReason;
    auto move_cmd = [](objects::ObjectId id, float x, float y, float angle) {
        commands::EventData ev(commands::EventType::SetupPhaseMove);
        ev.set("object", static_cast<uint32_t>(id)).set_f("x", x).set_f("y", y).set_f("z", 0.0f).set_f("angle", angle);
        return ev;
    };
    bool invaded = false, seen = false;
    for (uint64_t t = 0; t < 4000 && !out.refused_after; ++t) {
        host.tick_once();
        if (!invaded && host.modes().depth() == 1) {
            const std::vector<uint32_t> f = gs->fleets_at(1, 1);
            if (!f.empty() && !gs->fleets()[f[0]].moving) {
                CHECK(gs->invade(1, 1, {}) == R::None);
                invaded = true;
            }
        }
        TacticalBattle* b = battles.battle();
        TacticalMode* tm = host.tactical();
        if (!b || !tm || !b->fog()) continue;
        const objects::ObjectStore& st = tm->primary();
        if (!seen) {
            seen = true;
            out.setup_at_start = b->fog_setup_phase();
        }
        if (b->frame() == 10 && b->fog_setup_phase()) {
            for (objects::ObjectHandle h : st.owned_by(1))
                if (st.active(h)) {
                    out.enemy_refused = host.execute_direct(0, commands::CommandSource::Player, move_cmd(st.id(h), 600.0f, 600.0f, 0.0f)) == R::InvalidUnit;
                    break;
                }
            for (objects::ObjectHandle h : st.owned_by(0)) {
                if (!st.active(h) || !b->combat().has_unit(st.id(h))) continue;
                const Vec3 p = st.position(h);
                const commands::EventData ev = move_cmd(st.id(h), p.x + 40.0f, p.y + 30.0f, 90.0f);
                const R r = host.execute_direct(0, commands::CommandSource::Player, ev);
                const Vec3 q = st.position(h), f = st.facing(h);
                out.moved = r == R::None && q.x == ev.f("x") && q.y == ev.f("y") && std::fabs(q.x - p.x - 40.0f) < 1.0f &&
                            std::fabs(f.x) < 1.0e-3f && std::fabs(f.y - 1.0f) < 1.0e-3f;
                break;
            }
            out.ready_frame = b->frame();
            out.ready_cmd = static_cast<int>(host.execute_direct(ready_player, commands::CommandSource::Player,
                                                                 commands::EventData(commands::EventType::SetupPhaseTriggerEnd)));
        }
        if (out.ready_frame && !out.end_frame && !b->fog_setup_phase()) {
            out.end_frame = b->frame();
            for (objects::ObjectHandle h : st.owned_by(0))
                if (st.active(h)) {
                    out.refused_after = host.execute_direct(0, commands::CommandSource::Player, move_cmd(st.id(h), 600.0f, 600.0f, 0.0f)) == R::NotAllowed;
                    break;
                }
            if (!out.refused_after) break;
        }
    }
    return out;
}

void test_fog_setup_battle(const Data& d) {
    const SetupRun ai = run_setup_battle(d, 1, 0), human = run_setup_battle(d, 0, 0), ai4 = run_setup_battle(d, 1, 4);
    for (const SetupRun* r : {&ai, &human})
        std::printf("  ready from %s: setup at start %d, own move %d, enemy refused %d, ready cmd %d at frame %u, "
                    "phase ended at frame %u, move refused after %d\n",
                    r == &ai ? "AI" : "human", r->setup_at_start ? 1 : 0, r->moved ? 1 : 0,
                    r->enemy_refused ? 1 : 0, r->ready_cmd, r->ready_frame, r->end_frame, r->refused_after ? 1 : 0);
    for (const SetupRun* r : {&ai, &human}) {
        CHECK(r->setup_at_start && r->moved && r->enemy_refused && r->refused_after);
        CHECK(r->ready_cmd == static_cast<int>(commands::RejectReason::None) && r->ready_frame == 10);
    }
    CHECK(ai.end_frame == ai.ready_frame + 90);       // 3 s x 30 fps after the first ready
    CHECK(human.end_frame == human.ready_frame + 1);  // every human ready: the next tick's end test
    CHECK(ai4.end_frame == ai.end_frame && ai4.moved && ai4.refused_after);
}

// FOW t01: Battlefield_Modifier feeds the fog reveal list (space battle, fog_bm data; see fog_bm_probe).
void test_fog_bm_battle(const Data& d) {
    const Run a = run_campaign(d, false, true, 0, 2500, 0.0f, true);
    std::printf("  bonus frames: Empire %u (then off %u), Underworld %u (then off %u); sight radius on %.1f / off %.1f; bad frames %u; "
                "grid %s, save %s\n",
                a.bm_on_frames[0], a.bm_off_after_on[0], a.bm_on_frames[1], a.bm_off_after_on[1], static_cast<double>(a.bm_r_on),
                static_cast<double>(a.bm_r_off), a.bm_bad_frames, a.bm_grid_ok ? "ok" : "FAILED", a.bm_save_ok ? "ok" : "FAILED");
    CHECK(a.fought == 1 && a.fog_frames > 100);
    CHECK(a.bm_on_frames[0] > 0 && a.bm_on_frames[1] > 0 && a.bm_bad_frames == 0);
    CHECK(a.bm_off_after_on[0] > 0 || a.bm_off_after_on[1] > 0);  // a side lost its last corvette: the bonus left with it
    CHECK(a.bm_r_on == 1350.0f && a.bm_r_off == 900.0f);
    CHECK(a.bm_saved && a.bm_grid_ok && a.bm_save_ok);
}
#endif

// S8 t08 (TACTICAL_COMBAT §3.1.1): the units with both weapon sources get their own weapon (WEAPON behaviour +
// Projectile_Types) next to their weapon hardpoints, with the Fire_Weapon_When_* / Fire_When_* flags of the table; a type
// with Projectile_Types but no WEAPON behaviour gets none. S8 t06 B4 (LOB_SOLVER §5.1): with model data the own weapon's
// fire bones MUZZLEA_NN come from the land model.
void test_foc_weapon_sources(const objects::GameObjectTypes& types, const BattleCatalog& catalog) {
    namespace tc = tactical::combat;
    struct Row {
        const char* unit;
        const char* own;  // own weapon projectile
        bool own_dep, own_undep, own_rocket, own_normal;
        const char* hp;  // weapon hardpoint projectile
        bool hp_dep, hp_undep, hp_rocket, hp_normal;
    };
    const Row rows[] = {
        {"SPMAT_Walker", "Proj_Ground_SPMAT_Grenade", true, false, true, true, "Proj_Blaster_Cannon_Green", false, true, true, true},
        {"T4B_Tank", "Proj_Ground_Laser_Cannon_Red_T4B", true, true, false, true, "Proj_T4B_Missile", true, true, true, false},
        {"Dark_Trooper_PhaseIII", "Proj_Ground_Laser_Cannon_Red_DT3", false, true, true, true, "Proj_Dark_Trooper_Missile", true, false, true,
         true},
        {"Destroyer_Droid", "Proj_Destroyer_Droid_Blaster_Cannon", true, false, false, true, "Proj_Destroyer_Droid_Ion_Cannon", true, false,
         true, false},
    };
    for (const Row& r : rows) {
        const objects::TypeIndex t = types.resolve_name(r.unit);
        CHECK(t != objects::kNone);
        if (t == objects::kNone) continue;
        std::vector<uint32_t> idx;
        const BattleTables bt = catalog.build_battle({static_cast<uint32_t>(t)}, idx, false);
        const tc::CombatData& cd = bt.combat;
        CHECK(cd.units.size() == 1);
        if (cd.units.size() != 1) continue;
        const tc::UnitDesc& u = cd.units[0];
        const bool own_ok = u.weapon_projectile >= 0 && cd.projectiles[static_cast<size_t>(u.weapon_projectile)].name == r.own &&
                            u.fire_weapon_when_deployed == r.own_dep && u.fire_weapon_when_undeployed == r.own_undep &&
                            u.fire_weapon_when_rocket_attack == r.own_rocket && u.fire_weapon_when_normal_attack == r.own_normal;
        uint32_t weapon_hps = 0, hp_ok = 0;
        for (const uint32_t h : u.hardpoints) {
            const tc::HardpointDesc& hd = cd.hardpoints[h];
            if (!tc::is_weapon(hd.kind)) continue;
            ++weapon_hps;
            if (hd.projectile >= 0 && cd.projectiles[static_cast<size_t>(hd.projectile)].name == r.hp && hd.fire_when_deployed == r.hp_dep &&
                hd.fire_when_undeployed == r.hp_undep && hd.fire_when_rocket_attack == r.hp_rocket &&
                hd.fire_when_normal_attack == r.hp_normal)
                ++hp_ok;
        }
        std::printf("foc weapon sources: %s own %s (%d), %u weapon hardpoints (%u as the table), muzzle model %d bones %zu\n", r.unit,
                    u.weapon_projectile >= 0 ? cd.projectiles[static_cast<size_t>(u.weapon_projectile)].name.c_str() : "-",
                    own_ok ? 1 : 0, weapon_hps, hp_ok, u.muzzle_model ? 1 : 0, u.muzzle_bones.size());
        CHECK(own_ok && weapon_hps > 0 && hp_ok == weapon_hps);
        if (u.muzzle_model) CHECK(!u.muzzle_bones.empty());  // the vanilla models carry MUZZLEA_00
    }
    {  // Projectile_Types without the WEAPON behaviour (and without weapon hardpoints): no weapon.
        uint32_t listed = 0, armed = 0;
        std::string example;
        for (uint32_t t = 0; t < types.size(); ++t) {
            if (types.get_list(t, "Projectile_Types").empty() ||
                types.has_behavior(t, objects::BehaviorSet::Union, objects::beh::Weapon) || !types.hardpoints(t).empty())
                continue;
            ++listed;
            std::vector<uint32_t> idx;
            const tc::CombatData cd = catalog.build({t}, idx);
            if (!cd.units.empty() && cd.units[0].weapon_projectile >= 0) ++armed;
            if (example.empty()) example = std::string(types.raw().types().name[t]);
        }
        std::printf("foc weapon sources: %u types list Projectile_Types without WEAPON or hardpoints (e.g. %s), %u armed\n", listed,
                    example.c_str(), armed);
        CHECK(listed > 0 && armed == 0);
    }
    {  // Fire bones over every land unit with an own weapon: how many models lack MUZZLEA_00 (those never fire). S8 t12 (a)
       // (LOB_SOLVER §5.1 "Muzzle bones on FoC land models"): WEAPON in Behavior or LandBehavior (the land set the battle host
       // reads) - every vanilla model has the bone. The 8 types the Union set used to list (Moldy_Crow, ..._Landing,
       // UM07_*) carry WEAPON in SpaceBehavior only: no land own weapon.
        uint32_t with_model = 0, no_bone = 0;
        std::string missing;
        for (uint32_t t = 0; t < types.size(); ++t) {
            if (types.get_list(t, "Projectile_Types").empty() ||
                !(types.has_behavior(t, objects::BehaviorSet::Generic, objects::beh::Weapon) ||
                  types.has_behavior(t, objects::BehaviorSet::Land, objects::beh::Weapon)))
                continue;
            std::vector<tactical::Vec3> bones;
            if (!catalog.muzzle_bones(t, false, bones)) continue;
            ++with_model;
            if (bones.empty()) {
                ++no_bone;
                if (missing.size() < 400) missing += std::string(missing.empty() ? "" : ", ") + std::string(types.raw().types().name[t]);
            }
        }
        std::printf("foc fire bones: %u land models of weapon users, %u without MUZZLEA_00: %s\n", with_model, no_bone, missing.c_str());
        CHECK(with_model > 0 && no_bone == 0);
    }
    {  // S8 t10 (LOB_SOLVER §5.1): the garrison gun bone table FP_0<i>_ALT<a> of every Num_Garrison_Guns type with a model.
        uint32_t hosts = 0, with_bones = 0;
        std::string line;
        for (uint32_t t = 0; t < types.size(); ++t) {
            const int32_t guns = types.get_int(t, "Num_Garrison_Guns");
            if (guns <= 0) continue;
            std::vector<std::vector<tc::GarrisonBone>> table;
            if (!catalog.garrison_bones(t, false, guns, table)) continue;
            ++hosts;
            size_t valid = 0;
            for (const auto& e : table)
                for (const tc::GarrisonBone& b : e) valid += b.valid ? 1u : 0u;
            with_bones += valid > 0 ? 1u : 0u;
            if (line.size() < 300)
                line += std::string(line.empty() ? "" : ", ") + std::string(types.raw().types().name[t]) + " " + std::to_string(guns) +
                        " guns x " + std::to_string(table.size()) + " ALT (" + std::to_string(valid) + " bones)";
        }
        std::printf("foc garrison guns: %u host models, %u with gun bones: %s\n", hosts, with_bones, line.c_str());
        CHECK(hosts == 0 || with_bones > 0);
    }
    {  // S8 t27 (TACTICAL_COMBAT §3.1.1 "Bone list"): Target_Bones found on the type's model (missing names dropped).
        uint32_t listed = 0, with_model = 0;
        std::string line;
        for (uint32_t t = 0; t < types.size(); ++t) {
            const auto names = types.get_list(t, "Target_Bones");
            if (names.empty()) continue;
            ++listed;
            const bool space = types.get_string(t, "Land_Model_Name").empty();  // land model when the type has one
            std::vector<tactical::Vec3> bones;
            if (!catalog.target_bones(t, space, bones)) continue;
            ++with_model;
            CHECK(bones.size() <= names.size());
            if (line.size() < 400)
                line += std::string(line.empty() ? "" : ", ") + std::string(types.raw().types().name[t]) + " " +
                        std::to_string(bones.size()) + "/" + std::to_string(names.size());
        }
        std::printf("foc target bones: %u types list Target_Bones, %u with a model: %s\n", listed, with_model, line.c_str());
        CHECK(listed >= 5);
        const objects::TypeIndex atat = types.resolve_name("AT_AT_Walker");
        std::vector<tactical::Vec3> b;
        if (atat != objects::kNone && catalog.target_bones(static_cast<uint32_t>(atat), false, b)) {
            CHECK(b.size() == 3);  // MISSILE_TARGET00-02 on the land model
            for (const tactical::Vec3& v : b) CHECK(v.z > 0.0f);
        }
    }
}

// FoC: a battle request built by hand (two FoC frigate groups at Kuat-like orbit) on a real space map.
// a1g t04 follow-up (TACTICAL_GAPS §4.2.1): the land company drop layout, the oriented-box test and the radius rules.
// The worked example: FoC Rebel_Infantry_Squad (15 x Squad_Rebel_Trooper, Max_Squad_Size 5, team container locomotor,
// trooper half extents 3 x 3, FormationSpacing 0.5).
LandDropShape rebel_squad_shape() {
    LandDropShape s;
    s.members.assign(15, 7u);
    s.hx.assign(15, 3.0f);
    s.hy.assign(15, 3.0f);
    s.create_team = true;
    s.squad = 5;
    s.container_locomotor = true;
    s.formation_spacing = 0.5f;
    return s;
}

bool rebel_squad_slots_ok(const LandDropLayout& lay, float px, float py, float yaw_deg) {
    static const float kSlots[15][2] = {{35.5f, -16.0f}, {35.5f, 0.0f},   {35.5f, 16.0f},  {19.5f, -8.0f}, {19.5f, 8.0f},
                                        {8.0f, -16.0f},  {8.0f, 0.0f},    {8.0f, 16.0f},   {-8.0f, -8.0f}, {-8.0f, 8.0f},
                                        {-35.5f, 16.0f}, {-35.5f, 0.0f},  {-35.5f, -16.0f}, {-19.5f, 8.0f}, {-19.5f, -8.0f}};
    if (!lay.ok || lay.slots.size() != 15) return false;
    if (std::fabs(lay.half_x - 41.25f) > 1e-3f || std::fabs(lay.half_y - 42.0f) > 1e-2f) return false;
    const bool quarter = yaw_deg == 90.0f;
    for (size_t i = 0; i < 15; ++i) {
        const float ox = quarter ? -kSlots[i][1] : kSlots[i][0], oy = quarter ? kSlots[i][0] : kSlots[i][1];  // (x, y) -> (-y, x)
        if (std::fabs(lay.slots[i].x - (px + ox)) > 1e-3f || std::fabs(lay.slots[i].y - (py + oy)) > 1e-3f) {
            std::printf("  slot %zu: (%.4f, %.4f) expected (%.4f, %.4f)\n", i, static_cast<double>(lay.slots[i].x),
                        static_cast<double>(lay.slots[i].y), static_cast<double>(px + ox), static_cast<double>(py + oy));
            return false;
        }
    }
    return true;
}

void test_land_drop_rules() {
    // Layout: yaw 0 and the 90-degree rotation, at a drop point away from the origin.
    const LandDropShape rebel = rebel_squad_shape();
    CHECK(rebel_squad_slots_ok(land_drop_layout(rebel, 1000.0f, 500.0f, 0.0f), 1000.0f, 500.0f, 0.0f));
    CHECK(rebel_squad_slots_ok(land_drop_layout(rebel, 1000.0f, 500.0f, 90.0f), 1000.0f, 500.0f, 90.0f));
    {
        const LandDropLayout lay = land_drop_layout(rebel, 0.0f, 0.0f, 0.0f);
        CHECK(lay.ok && lay.slots[0].type == 7u && lay.slots[0].hx == 3.0f);
        CHECK(std::fabs(lay.slots[6].yaw_deg) < 1e-3f);  // (8, 0): the slot box's axis is the yaw of drop point -> slot
        CHECK(std::fabs(lay.slots[11].yaw_deg - 180.0f) < 1e-3f);
    }
    {  // without the team container locomotor the team uses 0x40f970; a partial last team keeps its first slots
        LandDropShape s = rebel;
        s.container_locomotor = false;
        s.members.resize(12);
        s.hx.resize(12);
        s.hy.resize(12);
        const LandDropLayout lay = land_drop_layout(s, 0.0f, 0.0f, 0.0f);
        CHECK(lay.ok && lay.slots.size() == 12);
    }
    {  // vehicles: one 0x40f970 grid. Cell (2 * 10 / 20 + 0, same) = (1, 1), 4 members: side = sqrt(7.2), 2 per row.
        LandDropShape v;
        v.members.assign(4, 3u);
        v.hx.assign(4, 10.0f);
        v.hy.assign(4, 10.0f);
        const LandDropLayout lay = land_drop_layout(v, 0.0f, 0.0f, 0.0f);
        CHECK(lay.ok && lay.slots.size() == 4);
        if (lay.ok && lay.slots.size() == 4) {
            const float side = std::sqrt(7.2f), gap = (side - 2.0f) / 3.0f;
            CHECK(std::fabs(lay.slots[0].x - 10.0f) < 1e-3f && std::fabs(lay.slots[2].x + 10.0f) < 1e-3f);
            CHECK(std::fabs(lay.slots[0].y - (gap - side / 2.0f + 0.5f) * 20.0f) < 1e-3f);
            CHECK(std::fabs(lay.slots[1].y + lay.slots[0].y) < 1e-3f);  // centred row
            CHECK(std::fabs(lay.half_x - 20.0f) < 1e-3f && std::fabs(lay.half_y - side * 10.0f) < 1e-3f);
        }
        v.hy.assign(4, 0.0f);  // b = 2 hy / 20 + FS = 0 -> no layout
        CHECK(!land_drop_layout(v, 0.0f, 0.0f, 0.0f).ok);
        CHECK(!land_drop_layout(LandDropShape{}, 0.0f, 0.0f, 0.0f).ok);
    }
    // Oriented boxes.
    CHECK((drop_boxes_overlap({0.0f, 0.0f, 3.0f, 3.0f, 0.0f}, {5.0f, 0.0f, 3.0f, 3.0f, 0.0f})));
    CHECK((!drop_boxes_overlap({0.0f, 0.0f, 3.0f, 3.0f, 0.0f}, {6.0f, 0.0f, 3.0f, 3.0f, 0.0f})));  // touching
    CHECK((!drop_boxes_overlap({0.0f, 0.0f, 3.0f, 3.0f, 0.0f}, {7.5f, 0.0f, 3.0f, 3.0f, 0.0f})));
    CHECK((!drop_boxes_overlap({0.0f, 0.0f, 3.0f, 3.0f, 45.0f}, {9.0f, 0.0f, 3.0f, 3.0f, 45.0f})));
    CHECK((drop_boxes_overlap({0.0f, 0.0f, 3.0f, 3.0f, 0.0f}, {6.5f, 0.0f, 3.0f, 3.0f, 45.0f})));  // the corner reaches 2.26
    CHECK((drop_boxes_overlap({0.0f, 0.0f, 20.0f, 1.0f, 90.0f}, {0.0f, 15.0f, 1.0f, 1.0f, 0.0f})));   // long axis along y
    CHECK((!drop_boxes_overlap({0.0f, 0.0f, 20.0f, 1.0f, 0.0f}, {0.0f, 15.0f, 1.0f, 1.0f, 0.0f})));
    // Radii: a POWERED enemy object with prevention radius 100, 60 from a slot, blocks only while its owner has base
    // power; without POWERED it always blocks. Enabling (<=) needs an allied object; prevention is strict (<).
    LandDropRadiusObject turret;
    turret.x = 60.0f;
    turret.prevention = 100.0f;
    turret.powered = true;
    turret.owner_base_power = true;
    LandDropRadiusObject point;
    point.allied = true;
    point.enabling = 500.0f;
    std::vector<LandDropRadiusObject> objs = {point, turret};
    CHECK(!land_drop_radius_ok(0.0f, 0.0f, objs, true));
    objs[1].owner_base_power = false;  // the owner's base-power list is empty
    CHECK(land_drop_radius_ok(0.0f, 0.0f, objs, true));
    objs[1].powered = false;
    CHECK(!land_drop_radius_ok(0.0f, 0.0f, objs, true));
    CHECK(land_drop_radius_ok(-40.0f, 0.0f, objs, true));  // d = 100: not < 100
    objs[1].allied = true;                                  // an allied object never prevents
    CHECK(land_drop_radius_ok(0.0f, 0.0f, objs, true));
    CHECK(land_drop_radius_ok(500.0f, 0.0f, objs, true));   // d = 500 <= 500
    CHECK(!land_drop_radius_ok(500.5f, 0.0f, objs, true));  // outside every enabling radius
    CHECK(land_drop_radius_ok(500.5f, 0.0f, objs, false));  // without Use_Reinforcement_Points
    {  // a1i t04: the unload offsets use 3.0 x the team bounds (the validity 2.5); team members keep their offsets in-team.
        const LandDropLayout v = land_drop_layout(rebel, 0.0f, 0.0f, 0.0f), u = land_drop_layout(rebel, 0.0f, 0.0f, 0.0f, 3.0f);
        CHECK(v.ok && u.ok && v.slots.size() == u.slots.size());
        bool wider = false;
        for (size_t i = 0; v.ok && u.ok && i < v.slots.size(); ++i) {
            const float dv = std::sqrt(v.slots[i].x * v.slots[i].x + v.slots[i].y * v.slots[i].y);
            const float du = std::sqrt(u.slots[i].x * u.slots[i].x + u.slots[i].y * u.slots[i].y);
            wider = wider || du > dv + 1.0f;
        }
        CHECK(wider && u.half_x >= v.half_x && u.half_y >= v.half_y);
        LandDropShape veh;  // vehicles: one member grid, no team factor
        veh.members.assign(4, 3u);
        veh.hx.assign(4, 10.0f);
        veh.hy.assign(4, 10.0f);
        const LandDropLayout a = land_drop_layout(veh, 5.0f, 5.0f, 30.0f), b = land_drop_layout(veh, 5.0f, 5.0f, 30.0f, 3.0f);
        CHECK(a.ok && b.ok && a.slots.size() == b.slots.size());
        for (size_t i = 0; a.ok && b.ok && i < a.slots.size(); ++i) CHECK(a.slots[i].x == b.slots[i].x && a.slots[i].y == b.slots[i].y);
    }
    {  // a1i t05: Find_Free_Placement rings (box 20 x 10 -> step 24, 22.5-degree angles, 360 inclusive).
        const FreePlacementBox box{-10.0f, -5.0f, 10.0f, 5.0f};
        float qx = 0.0f, qy = 0.0f;
        CHECK(find_free_placement(100.0f, 50.0f, box, {}, 2500.0f, qx, qy) && qx == 100.0f && qy == 50.0f);
        std::vector<FreePlacementBox> bl = {{90.0f, 45.0f, 110.0f, 55.0f}};  // covers P
        CHECK(find_free_placement(100.0f, 50.0f, box, bl, 2500.0f, qx, qy));
        CHECK(std::fabs(qx - 124.0f) < 1e-3f && std::fabs(qy - 50.0f) < 1e-3f);  // ring 1, angle 0
        bl.push_back({113.0f, 45.0f, 140.0f, 52.0f});                         // angle 0 of ring 1 taken too
        CHECK(find_free_placement(100.0f, 50.0f, box, bl, 2500.0f, qx, qy));
        CHECK(std::fabs(qx - (100.0f + 24.0f * std::cos(0.39269908f))) < 1e-2f && std::fabs(qy - (50.0f + 24.0f * std::sin(0.39269908f))) < 1e-2f);
        const std::vector<FreePlacementBox> touch = {{110.0f, 45.0f, 130.0f, 55.0f}};  // touches the box at P: free
        CHECK(find_free_placement(100.0f, 50.0f, box, touch, 2500.0f, qx, qy) && qx == 100.0f);
        const std::vector<FreePlacementBox> all = {{-5000.0f, -5000.0f, 5000.0f, 5000.0f}};
        CHECK(!find_free_placement(100.0f, 50.0f, box, all, 2500.0f, qx, qy));
        CHECK(!find_free_placement(0.0f, 0.0f, FreePlacementBox{}, all, 2500.0f, qx, qy));  // zero step: r = 0 only
    }
    std::printf("  land drop rules: OK\n");
}

void test_foc() {
    const std::string root = env_or_empty("ALAMO_FOC_DATA");
    if (root.empty()) {
        std::printf("foc battle: ALAMO_FOC_DATA not set, skipped\n");
        return;
    }
    vfs::VfsConfig vc;
    vc.foc_root = root;
    std::string err;
    auto files = vfs::Vfs::create(vc, &err);
    CHECK(files != nullptr);
    if (!files) return;
    data::GameDatabase db;
    objects::ObjectTypeDatabase raw;
    objects::GameObjectTypes types;
    galactic::GalacticRules rules;
    galactic::ConflictRules crules;
    std::vector<std::string> diag;
    CHECK(db.load(*files, {}, &diag) && raw.load(*files) && types.build(raw, files.get(), &diag));
    CHECK(galactic::load_galactic_rules(*files, db, types, rules, &diag));
    CHECK(galactic::load_conflict_rules(*files, db, types, rules, crules, &diag));
    const BattleCatalog catalog(types, files.get(), &db.constants(), &diag);
    std::printf("foc battle: contrast rows %zu, hardpoint records %zu, priority sets %zu, damage types %u\n", crules.contrast.size(),
                catalog.hardpoint_records(), catalog.priority_sets().size(), catalog.damage().damage_count());
    CHECK(crules.contrast.size() == 11 && catalog.hardpoint_records() > 100 && !catalog.priority_sets().empty());
    {  // a1g t04 follow-up (TACTICAL_GAPS §4.2.1 worked example): the Rebel_Infantry_Squad drop shape and layout
        const objects::TypeIndex rq = types.resolve_name("Rebel_Infantry_Squad");
        CHECK(rq != objects::kNone && rq < types.size());
        if (rq != objects::kNone && rq < types.size()) {
            const LandDropShape s = land_drop_shape(types, &catalog, crules, static_cast<uint32_t>(rq));
            std::printf("foc drop shape Rebel_Infantry_Squad: %zu members, hx %.3f hy %.3f, team %d squad %u container %d fs %.2f\n",
                        s.members.size(), s.hx.empty() ? 0.0 : static_cast<double>(s.hx[0]), s.hy.empty() ? 0.0 : static_cast<double>(s.hy[0]),
                        s.create_team ? 1 : 0, s.squad, s.container_locomotor ? 1 : 0, static_cast<double>(s.formation_spacing));
            CHECK(s.members.size() == 15 && s.create_team && s.squad == 5 && s.container_locomotor && s.formation_spacing == 0.5f);
            CHECK(!s.hx.empty() && std::fabs(s.hx[0] - 3.0f) < 1e-4f && std::fabs(s.hy[0] - 3.0f) < 1e-4f);
            CHECK(rebel_squad_slots_ok(land_drop_layout(s, 250.0f, -125.0f, 0.0f), 250.0f, -125.0f, 0.0f));
            CHECK(rebel_squad_slots_ok(land_drop_layout(s, 250.0f, -125.0f, 90.0f), 250.0f, -125.0f, 90.0f));
        }
    }
    {  // refixf t01: rocket curve fields reach the battle tables (MPTL fires Proj_Ground_Proton_Torpedo: MPTL_Rocket,
       // straight 100, curve 50, offset 7, Target_Point_On_Terrain, Explode_When_Reached_Target_Radius)
        std::vector<uint32_t> idx;
        const BattleTables bt = catalog.build_battle({static_cast<uint32_t>(types.resolve_name("MPTL"))}, idx);
        const tactical::combat::ProjectileDesc* torp = nullptr;
        for (const tactical::combat::ProjectileDesc& pd : bt.combat.projectiles)
            if (pd.name == "Proj_Ground_Proton_Torpedo") torp = &pd;
        CHECK(torp != nullptr);
        if (torp) {
            std::printf("foc rocket: %s cat %d straight %.0f curve %.0f offset %.0f tpot %d explode %d mfd %.0f\n", torp->name.c_str(),
                        static_cast<int>(torp->category), static_cast<double>(torp->rocket_straight_distance),
                        static_cast<double>(torp->rocket_curve_distance), static_cast<double>(torp->rocket_curve_offset),
                        torp->target_point_on_terrain ? 1 : 0, torp->explode_when_reached_target_radius ? 1 : 0,
                        static_cast<double>(torp->max_flight_distance));
            CHECK(torp->category == tactical::combat::ProjectileCategory::MptlRocket && torp->rocket_straight_distance == 100.0f &&
                  torp->rocket_curve_distance == 50.0f && torp->rocket_curve_offset == 7.0f && torp->target_point_on_terrain &&
                  torp->explode_when_reached_target_radius && torp->max_flight_distance == 720.0f);
        }
    }
    {  // S8 t04 (LOB_SOLVER §1.2): Underworld_Merc fires Proj_Merc_Concussion_Grenade through Projectile_Types - GRENADE,
       // v 10, gravity 0.7 x lob mod 0.8 (G 0.56, R = v^2 / G = 178.6), Can_Lob_Slower, not sticky, lifetime 900.
        std::vector<uint32_t> idx;
        const BattleTables bt = catalog.build_battle({static_cast<uint32_t>(types.resolve_name("Underworld_Merc"))}, idx);
        const tactical::combat::ProjectileDesc* g = nullptr;
        for (const tactical::combat::ProjectileDesc& pd : bt.combat.projectiles)
            if (pd.name == "Proj_Merc_Concussion_Grenade") g = &pd;
        CHECK(g != nullptr && bt.combat.units.size() == 1 && bt.combat.units[0].weapon_projectile >= 0 &&
              &bt.combat.projectiles[static_cast<size_t>(bt.combat.units[0].weapon_projectile)] == g);
        if (g) {
            std::printf("foc grenade: %s cat %d v %.1f gravity %.9g lob mod %.3g slower %d sticky %d lifetime %.0f\n", g->name.c_str(),
                        static_cast<int>(g->category), static_cast<double>(g->max_speed), static_cast<double>(g->grenade_gravity),
                        static_cast<double>(g->lob_mod), g->can_lob_slower ? 1 : 0, g->sticks_on_collision ? 1 : 0,
                        static_cast<double>(g->max_lifetime));
            CHECK(g->lobbed && g->category == tactical::combat::ProjectileCategory::Grenade && g->max_speed == 10.0f &&
                  g->grenade_gravity == 0.7f && g->lob_mod == 0.8f && g->can_lob_slower && !g->sticks_on_collision &&
                  g->max_lifetime == 900.0f);
            const float r = tactical::combat::lob_range(g->max_speed, g->grenade_gravity, g->lob_mod);
            CHECK(r > 178.5f && r < 178.6f);
        }
    }
    test_foc_weapon_sources(types, catalog);  // S8 t08 / S8 t06 B4
    {  // --- S4 (owner: S4 agent): shipped corruption options (PLANETS.XML) and garrison / capture descriptors ---
        const std::vector<galactic::CorruptionOption> ae = galactic::corruption_options(types, types.resolve_name("AetenII"));
        size_t missions = 0, options = 0;
        for (size_t t = 0; t < types.size(); ++t)
            for (const galactic::CorruptionOption& o : galactic::corruption_options(types, static_cast<objects::TypeIndex>(t))) {
                ++options;
                missions += o.tactical ? 1u : 0u;
            }
        std::printf("foc corruption: %zu options, %zu with a tactical mission; AetenII %zu options\n", options, missions, ae.size());
        CHECK(ae.size() == 2 && options > 20 && missions > 5);
        if (ae.size() == 2) {
            std::printf("  AetenII 1: type %d tactical %d kind %d cost %d map '%s' plots '%s' unit '%s' x%d\n", ae[0].type, ae[0].tactical ? 1 : 0,
                        static_cast<int>(ae[0].kind), ae[0].credit_cost, ae[0].map.c_str(), ae[0].plots.c_str(), ae[0].required_unit.c_str(),
                        ae[0].required_unit_count);
            CHECK(ae[0].type == 1 && ae[0].tactical && ae[0].kind == galactic::ConflictKind::Space && ae[0].credit_cost == 1000 &&
                  ae[0].map == "_Space_Planet_AetenII_Piracy.ted" && ae[0].plots == "Story_Plots_Underworld_AetenII_Piracy.xml" &&
                  ae[0].required_unit == "Bossk" && ae[0].required_unit_count == 1);
            CHECK(ae[1].type == 3 && !ae[1].tactical && ae[1].required_mission == 0 && ae[1].required_mission_count == 1 &&
                  ae[1].required_unit_count == 2);
        }
#ifdef ALAMO_SIM_HAVE_GARRISON
        tactical::garrison::HolderDesc hd;
        tactical::garrison::OccupantDesc od;
        tactical::garrison::CapturePointDesc cd;
        CHECK(tactical::garrison::holder_desc_from_type(types, types.resolve_name("HAV_Juggernaut"), hd) && hd.slots == 10 &&
              hd.radius == 30.0f && hd.kind == tactical::garrison::HolderKind::Vehicle);
        CHECK(tactical::garrison::holder_desc_from_type(types, types.resolve_name("Garrison_Bunker_Template"), hd) &&
              hd.kind == tactical::garrison::HolderKind::Structure);
        CHECK(tactical::garrison::occupant_desc_from_type(types, types.resolve_name("Dark_Trooper_PhaseIII"), od) && od.value == 2);
        CHECK(tactical::garrison::capture_desc_from_type(types, types.resolve_name("Reinforcement_Point"), cd) && cd.radius == 100.0f &&
              cd.transition_seconds == 10.0f && cd.sticky);
#endif
    }  // --- end S4 ---
    {  // S7 t07 (MOVEMENT §2.2): the FoC construction / capture pods have no radius tag; with model data their footprint is
       // the space model's collision-bound half extent x Scale_Factor (N_Orbital_Construction_Pod: x 1.75)
        namespace mv = tactical::movement;
        mv::SpaceObstacleHooks hooks;
        hooks.model_half_extents = [&catalog](uint32_t t, float& hx, float& hy) { return catalog.space_model_half_extents(t, hx, hy); };
        for (const char* name : {"Orbital_Construction_Pod", "N_Orbital_Construction_Pod", "Skirmish_Merchant_Dock"}) {
            const uint32_t t = static_cast<uint32_t>(types.resolve_name(name));
            CHECK(t < types.size());
            if (t >= types.size()) continue;
            float r = 0.0f;
            uint32_t bits = 0;
            CHECK(!mv::space_obstacle_footprint(types, t, r, bits));  // no radius tag: needs the model
            float hx = 0.0f, hy = 0.0f;
            if (!catalog.space_model_half_extents(t, hx, hy)) {
                std::printf("foc space obstacle: %s: no model data (sim built without assets), skipped\n", name);
                continue;
            }
            const float scale = types.get_float(t, "Scale_Factor") > 0.0f ? types.get_float(t, "Scale_Factor") : 1.0f;
            CHECK(mv::space_obstacle_footprint(types, t, r, bits, &hooks) && bits == mv::kSpaceStatic);
            CHECK(r == std::max(std::fabs(hx), std::fabs(hy)) * scale && r > 10.0f);
            std::printf("foc space obstacle: %s half %.1f x %.1f, scale %.2f -> radius %.1f\n", name, static_cast<double>(hx),
                        static_cast<double>(hy), static_cast<double>(scale), static_cast<double>(r));
        }
    }
#ifdef ALAMO_SIM_HAVE_FOG
    // Fog of war on real data (owner: fog agent): shipped GameConstants; grids, dense mask and REVEAL placements of
    // two real maps.
    const tactical::fog::FogConstants& fc = catalog.fog_constants();
    std::printf("foc fog: cells %.1f / %.1f, regrow %.1f / %.1f s, AI uses fog %d / %d, overrun x%.1f after %.0f s\n",
                static_cast<double>(fc.desired_land_cell_size), static_cast<double>(fc.desired_space_cell_size),
                static_cast<double>(fc.land_regrow_time), static_cast<double>(fc.space_regrow_time), fc.ai_uses_fog_land ? 1 : 0,
                fc.ai_uses_fog_space ? 1 : 0, static_cast<double>(fc.tactical_overrun_multiple), static_cast<double>(fc.min_overrun_seconds));
    CHECK(fc.desired_land_cell_size == 15.0f && fc.desired_space_cell_size == 100.0f && fc.land_regrow_time == 6.0f &&
          fc.space_regrow_time == 6.0f && !fc.ai_uses_fog_land && !fc.ai_uses_fog_space && fc.tactical_overrun_multiple == 3.0f &&
          fc.min_overrun_seconds == 30.0f && !fc.setup_phase_enabled);
    const TypeCrcIndex index(types);
    const objects::BehaviorTypeId reveal = objects::behavior_type_id("REVEAL"), obstacle = objects::behavior_type_id("SPACE_OBSTACLE");
    for (const char* name : {"_SPACE_PLANET_CORELLIA_PIRACY", "_LAND_PLANET_BYSS_KIDNAPPING"}) {
        terrain::MapData map;
        std::string map_err;
        if (!terrain::load_map(*files, name, map, &map_err)) {
            std::printf("FAIL foc fog: map %s: %s\n", name, map_err.c_str());
            ++g_fail;
            continue;
        }
        float mn[2] = {-5000.0f, -5000.0f}, mx[2] = {5000.0f, 5000.0f};
        map.playable_box(mn, mx);
        tactical::fog::FogSetup s;
        s.mode = map.is_space() ? tactical::fog::FogMode::Space : tactical::fog::FogMode::Land;
        s.box = {mn[0], mn[1], mx[0], mx[1]};
        s.constants = fc;
        s.players = {{0, true, true}, {1, true, false}, {2, false, false}};
        tactical::fog::FogOfWar fog;
        fog.start(s);
        std::vector<tactical::fog::FogOfWar::DenseCircle> dense;
        uint32_t sources = 0;
        int32_t id = 1;
        for (const terrain::ObjectPlacement& pl : map.objects) {
            const objects::TypeIndex t = index.find(pl.type_crc);
            if (t == objects::kNone) continue;
            if (obstacle != objects::kNoBehavior && types.has_behavior(t, objects::BehaviorSet::Union, obstacle) &&
                (types.get_bool(t, "Is_Nebula") || types.get_bool(t, "Is_Asteroid_Field") || types.get_bool(t, "Is_Ion_Storm") ||
                 types.get_bool(t, "Is_Impassable_Asteroid"))) {
                const auto off = types.get_vec(t, "Space_Obstacle_Offset");
                dense.push_back({pl.position[0] + off[0], pl.position[1] + off[1], types.get_float(t, "Space_Obstacle_Radius")});
            }
            if (reveal == objects::kNoBehavior || !types.has_behavior(t, objects::BehaviorSet::Union, reveal)) continue;
            tactical::fog::FogObject o;
            o.owner = pl.owner == 0 ? 0 : 1;
            o.x = pl.position[0];
            o.y = pl.position[1];
            o.sight_range = map.is_space() ? types.columns().space_fow_reveal_range[t] : types.columns().land_fow_reveal_range[t];
            o.dense_multiplier = std::clamp(types.get_float(t, "Dense_FOW_Reveal_Range_Multiplier"), 0.0f, 1.0f);
            fog.source_service(id++, o);
            ++sources;
        }
        fog.set_dense_obstacles(dense);
        for (uint32_t f = 1; f <= 64; ++f) fog.decay_step(f);
        const tactical::fog::FogGrid* g = fog.grid(0);
        uint32_t vis = 0, dense_cells = 0;
        if (g) {
            for (uint8_t v : g->state()) vis += v != 0 ? 1u : 0u;
            for (uint8_t m : fog.dense_mask()) dense_cells += m;
        }
        std::printf("foc fog %s: grid %dx%d (cell %.2f), %u REVEAL placements, %zu dense obstacles (%u cells), player 0 sees %u cells\n",
                    name, g ? g->width() : 0, g ? g->height() : 0, g ? static_cast<double>(g->cell_w()) : 0.0, sources, dense.size(), dense_cells, vis);
        CHECK(g != nullptr && g->width() > 0 && g->width() <= 512 && g->height() > 0 && !fog.grid(2));
    }
#endif
}

#ifdef ALAMO_SIM_HAVE_ANIM
// ---- R3 anim: flinch / death clips / corpses in synthetic battles (SimAnimation with synthetic clip models) ----

// One-bone skeleton, clips without tracks (bound: ALA bones 0 + 1): only frame counts / fps matter here.
std::unique_ptr<anim::AnimModel> anim_model(std::initializer_list<std::pair<int32_t, std::pair<uint32_t, uint32_t>>> specs) {
    auto m = std::make_unique<anim::AnimModel>();
    m->model = "S10_SYNTH.ALO";
    m->base = "S10_SYNTH";
    m->skeleton.names = {"Root"};
    m->skeleton.parents = {-1};
    m->skeleton.billboard_mode = {0u};
    m->skeleton.bind_local = {anim::Mat34{}};
    m->has_skeleton = true;
    for (const auto& [type, vf] : specs)
        for (uint32_t v = 0; v < vf.first; ++v) {
            auto c = std::make_shared<anim::Clip>();
            c->name = anim::clip_file_name(m->base, type, v);
            c->frame_count = vf.second;
            c->fps = 30.0f;
            anim::ClipRef r;
            r.clip = c;
            m->clips[static_cast<size_t>(type)].push_back(r);
        }
    anim::bind_clips(*m);
    return m;
}

struct AnimRun {
    CrcLog crc;
    SimAnimStats stats;
    uint32_t max_detached = 0, max_corpses = 0, flinch_seen = 0, muzzles_seen = 0, hp_muzzles_seen = 0, foot_events = 0, impact_events = 0;
    uint64_t events_digest = 0;  // per-tick events (id, name, bone, position bits)
    std::vector<std::string> battles;
    // refix t01 / t03: death clone objects (+0x3a0 |= 0xc0) seen in the tactical store, battle clone count, searches
    uint32_t max_dying = 0, clone_objects = 0, dying_found = 0;
    bool corpse_path_trooper = false, corpse_path_trooper_k4 = true, corpse_path_frigate = true;
};

AnimRun run_anim_campaign(const Data& d, bool land, int32_t workers, uint64_t ticks) {
    AnimRun out;
    const auto trooper = anim_model({{anim::kAnimIdle, {2, 30}}, {anim::kAnimMove, {1, 24}}, {anim::kAnimWalkMove, {1, 24}},
                                     {anim::kAnimAttack, {1, 12}}, {anim::kAnimAttackIdle, {1, 20}}, {anim::kAnimDie, {2, 45}},
                                     {anim::kAnimFlinchLeft, {1, 9}}, {anim::kAnimFlinchRight, {1, 9}}, {anim::kAnimFlinchFront, {2, 9}},
                                     {anim::kAnimFlinchBack, {1, 9}}, {anim::kAnimAttackFlinchLeft, {1, 9}}, {anim::kAnimAttackFlinchRight, {1, 9}},
                                     {anim::kAnimAttackFlinchFront, {1, 9}}, {anim::kAnimAttackFlinchBack, {1, 9}},
                                     {anim::kAnimTurnLeft, {1, 10}}, {anim::kAnimTurnRight, {1, 10}}});
    const auto ship = anim_model({{anim::kAnimIdle, {1, 60}}});
    const auto wreck = anim_model({{anim::kAnimDie, {1, 30}}});
    jobs::JobSystem js(jobs::JobSystemConfig{workers});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    SimHost host(d.types, js, cfg);
    SimAnimation animation(d.types, *d.files);
    animation.set_model(d.types.resolve_name("S10_Trooper"), objects::ModeKind::Land, trooper.get());
    animation.set_model(d.types.resolve_name("S10_Frigate"), objects::ModeKind::Space, ship.get());
    animation.set_model(d.types.resolve_name("S10_Corvette"), objects::ModeKind::Space, ship.get());
    animation.set_model(d.types.resolve_name("S10_Frigate_Death_Clone"), objects::ModeKind::Space, wreck.get());
    {  // AnimationSFXMaps.txt rows for the synthetic clips (foot plants on the move clip, a body impact on DIE)
        anim::AnimEventMap map;
        map.parse("SURFACE\ts10_synth_move_00\t0\tINFANTRY_FOOT_PLANT\tB_Foot_R\nSURFACE\ts10_synth_move_00\t400\tINFANTRY_FOOT_PLANT\tB_Foot_L\n"
                  "SURFACE\ts10_synth_die_00\t500\tTROOPER_IMPACT\tRoot\nSOUND\ts10_synth_attack_00\t0\tUnit_Fire\n");
        animation.set_event_map(std::move(map));
    }
    animation.attach(host, true);
    galactic::GalacticSim* gs = host.start_galactic(d.rules, make_setup(d, land));
    CHECK(gs != nullptr);
    if (!gs) return out;
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 6000;
    opt.animation = &animation;
    CampaignBattles battles(host, d.crules, *d.catalog,
                            [](const std::string& name, terrain::MapData& m) {
                                if (name == "S10_Space_B") m = space_map();
                                else if (name == "S10_Land_B") m = land_map();
                                else return false;
                                return true;
                            },
                            opt);
    CHECK(battles.attach(nullptr));
    if (land) {
        std::vector<objects::ObjectHandle> up;
        for (objects::ObjectHandle h : gs->planets()[2].surface)
            if (gs->store().owner(h) == 1) up.push_back(h);
        CHECK(gs->launch_units(1, 2, up) == commands::RejectReason::None);
    }
    const std::vector<uint32_t> uf = gs->fleets_at(2, 1);
    if (!uf.empty()) CHECK(gs->move_fleet(1, uf[0], 1) == commands::RejectReason::None);
    bool invaded = false;
    StateHasher ev_hash;
    for (uint64_t t = 0; t < ticks; ++t) {
        host.tick_once();
        if (const TacticalBattle* b = battles.battle()) {
            out.clone_objects = std::max(out.clone_objects, b->stats().death_clones);
            const uint32_t trooper_type = type_of(d, "S10_Trooper");
            out.corpse_path_trooper = b->death_clone_corpse_path(trooper_type, static_cast<int32_t>(trooper_type), 1);
            out.corpse_path_trooper_k4 = b->death_clone_corpse_path(trooper_type, static_cast<int32_t>(trooper_type), 4);
            out.corpse_path_frigate =
                b->death_clone_corpse_path(type_of(d, "S10_Frigate"), static_cast<int32_t>(type_of(d, "S10_Frigate_Death_Clone")), 1);
        }
        if (TacticalMode* tm = host.tactical()) {
            {  // death clones: in the store (find by id, owner), dying, never in the active searches
                const objects::ObjectStore& st = tm->primary();
                uint32_t dying = 0;
                for (objects::ObjectHandle h : st.all_objects()) {
                    if (!st.alive(h) || !st.dying(h)) continue;  // (a clone the anim removed is dead until the flush)
                    ++dying;
                    CHECK(!st.active(h) && st.find(st.id(h)) == h);
                    for (objects::ObjectHandle o : st.find_all_of_type(st.type(h))) out.dying_found += o == h ? 1u : 0u;
                }
                out.max_dying = std::max(out.max_dying, dying);
            }
            out.muzzles_seen += static_cast<uint32_t>(animation.muzzles(*tm).size());
            for (const SimAnimation::Muzzle& mz : animation.muzzles(*tm)) out.hp_muzzles_seen += mz.hardpoint >= 0 ? 1u : 0u;
            for (const SimAnimation::FrameEvent& e : animation.events(*tm)) {
                out.foot_events += e.def->name == "INFANTRY_FOOT_PLANT" ? 1u : 0u;
                out.impact_events += e.def->name == "TROOPER_IMPACT" ? 1u : 0u;
                ev_hash.u64(t);
                ev_hash.i32(e.id);
                ev_hash.str(e.def->name);
                ev_hash.i32(e.bone);
                ev_hash.f32(e.position.x);
                ev_hash.f32(e.position.y);
                ev_hash.f32(e.position.z);
            }
            uint32_t detached = 0;
            for (const anim::Corpse& c : animation.corpses(*tm)) detached += c.detached ? 1u : 0u;
            out.max_detached = std::max(out.max_detached, detached);
            out.max_corpses = std::max(out.max_corpses, static_cast<uint32_t>(animation.corpses(*tm).size()));
            if (const anim::AnimWorld* w = animation.find_world(*tm))
                for (ObjectId id : w->ids()) out.flinch_seen += anim::is_flinch(w->current_type(id)) ? 1u : 0u;
        }
        if (land && !invaded && host.modes().depth() == 1) {
            const std::vector<uint32_t> f = gs->fleets_at(1, 1);
            if (!f.empty() && !gs->fleets()[f[0]].moving) {
                CHECK(gs->invade(1, 1, {}) == commands::RejectReason::None);
                invaded = true;
            }
        }
    }
    out.events_digest = ev_hash.digest();
    out.crc = host.crc_log();
    out.battles = battles.log();
    out.stats = animation.stats();
    animation.detach(host);
    return out;
}

// s78k t05 (TACTICAL_COMBAT §3.1.1 "Slot 47", "Order of the tests" [H]; s78j t02): the attack-idle pose hook tests the
// current clip before its Get_Float draw. On a land trooper of a battle with an animation world (the synthetic trooper
// model has clips 5 Attack and 6 Attack Idle), not moving: from Idle the hook draws once (blend in [0.2, 0.75]) and plays
// clip 6; a second service while clip 6 plays draws nothing and keeps it; while clip 5 Attack plays it draws nothing
// either and keeps clip 5. The hook runs with a private generator (TacticalBattle::test_attack_idle), so only its own
// draws are counted.
void test_attack_idle_clip_order() {
    auto d = make_data(false, true);
    const auto trooper = anim_model({{anim::kAnimIdle, {2, 30}}, {anim::kAnimMove, {1, 24}}, {anim::kAnimWalkMove, {1, 24}},
                                     {anim::kAnimAttack, {1, 12}}, {anim::kAnimAttackIdle, {1, 20}}, {anim::kAnimDie, {2, 45}}});
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    SimHost host(d->types, js, cfg);
    SimAnimation animation(d->types, *d->files);
    const objects::TypeIndex trooper_t = d->types.resolve_name("S10_Trooper");
    animation.set_model(trooper_t, objects::ModeKind::Land, trooper.get());
    animation.attach(host, true);
    galactic::GalacticSim* gs = host.start_galactic(d->rules, make_setup(*d, true));
    CHECK(gs != nullptr);
    if (!gs) return;
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 600;
    opt.animation = &animation;
    CampaignBattles battles(host, d->crules, *d->catalog,
                            [](const std::string& name, terrain::MapData& m) {
                                if (name != "S10_Land_B") return false;
                                m = land_map();
                                return true;
                            },
                            opt);
    CHECK(battles.attach(nullptr));
    std::vector<objects::ObjectHandle> up;
    for (objects::ObjectHandle h : gs->planets()[2].surface)
        if (gs->store().owner(h) == 1) up.push_back(h);
    CHECK(gs->launch_units(1, 2, up) == commands::RejectReason::None);
    const std::vector<uint32_t> uf = gs->fleets_at(2, 1);
    if (!uf.empty()) CHECK(gs->move_fleet(1, uf[0], 1) == commands::RejectReason::None);
    namespace tc = alamo::tactical::combat;
    bool invaded = false, ran = false;
    for (uint64_t t = 0; t < 3000 && !ran; ++t) {
        host.tick_once();
        if (!invaded && host.modes().depth() == 1) {
            const std::vector<uint32_t> f = gs->fleets_at(1, 1);
            if (!f.empty() && !gs->fleets()[f[0]].moving) invaded = gs->invade(1, 1, {}) == commands::RejectReason::None;
        }
        TacticalBattle* b = battles.battle();
        TacticalMode* tm = host.tactical();
        if (!b || !tm || b->frame() < 3) continue;
        const anim::AnimWorld* w = animation.find_world(*tm);
        CHECK(w != nullptr);
        if (!w) break;
        const tc::CombatSystem& cs = b->combat();
        const objects::ObjectStore& st = tm->primary();
        // A live, standing trooper with an animation row whose clip can be set (not locked).
        objects::ObjectId id = objects::kNoObjectId;
        for (size_t i = 0; i < cs.unit_count() && id == objects::kNoObjectId; ++i) {
            const auto u = cs.unit_id_at(i);
            if (!cs.alive(u) || (cs.flags(u) & tc::kUnitMoving) || st.type(st.find(u)) != trooper_t) continue;
            if (b->test_play_clip(u, anim::kAnimIdle)) id = u;
        }
        if (id == objects::kNoObjectId) continue;  // every trooper walking this tick: try the next one
        ran = true;
        CHECK(w->current_type(id) == anim::kAnimIdle);
        SimRng r1(77);
        CHECK(b->test_attack_idle(id, false, r1));
        CHECK(r1.draws() == 1u);  // Idle: one Get_Float(0.2, 0.75) for the blend
        CHECK(w->current_type(id) == anim::kAnimAttackIdle);
        SimRng r2(77);
        CHECK(b->test_attack_idle(id, false, r2));
        CHECK(r2.draws() == 0u);  // clip 6 plays: the clip test returns before the draw
        CHECK(w->current_type(id) == anim::kAnimAttackIdle);
        CHECK(b->test_play_clip(id, anim::kAnimAttack));
        SimRng r3(77);
        CHECK(b->test_attack_idle(id, false, r3));
        CHECK(r3.draws() == 0u);  // clip 5 plays: no draw, clip 5 kept
        CHECK(w->current_type(id) == anim::kAnimAttack);
        std::printf("  slot 47 on trooper %d (frame %u): idle 1 draw, attack idle %llu, attack %llu\n", static_cast<int>(id), b->frame(),
                    static_cast<unsigned long long>(r2.draws()), static_cast<unsigned long long>(r3.draws()));
    }
    CHECK(ran);
    animation.detach(host);
}

void test_anim_battles() {
    auto d = make_data(false, true);
    for (const bool land : {true, false}) {
        const uint64_t ticks = land ? 3000 : 2500;
        const AnimRun a = run_anim_campaign(*d, land, 0, ticks);
        for (const std::string& s : a.battles) std::printf("  %s\n", s.c_str());
        const SimAnimStats& s = a.stats;
        StateHasher h;
        for (const CrcRecord& r : a.crc.records) h.u64(r.combined);
        std::printf("  %s + anim: rows created %u removed %u; flinches %u (flinch-frames %u), deaths %u (death clips %u, removed at death %u, "
                    "clone deaths %u), corpses removed %u, max corpses %u (detached %u); CRC log digest %016" PRIx64 "\n",
                    land ? "land" : "space", s.created, s.removed, s.flinches, a.flinch_seen, s.deaths, s.death_clips, s.removed_at_death,
                    s.clone_deaths, s.corpses_removed, a.max_corpses, a.max_detached, h.digest());
        std::printf("  %s + anim: turns %u, muzzle-frames %u (flashes %u), events %llu (foot plants %u, impacts %u), digest %016" PRIx64 "\n",
                    land ? "land" : "space", s.turns, a.muzzles_seen, s.muzzle_flashes, static_cast<unsigned long long>(s.events), a.foot_events,
                    a.impact_events, a.events_digest);
        std::printf("  %s + anim: death clone objects %u, max dying in the store %u\n", land ? "land" : "space", a.clone_objects, a.max_dying);
        CHECK(a.dying_found == 0);  // find_all_of_type skips dying objects
        bool has_anim_system = false;
        for (const std::string& n : a.crc.systems) has_anim_system = has_anim_system || n == "anim";
        CHECK(has_anim_system && !a.battles.empty());
        CHECK(s.deaths > 0);
        if (land) {
            // Troopers flinch on blaster hits, play DIE, persist 1 s + fade 0.5 s as detached corpse rows, then go.
            CHECK(s.flinches > 0 && a.flinch_seen > 0 && s.death_clips > 0 && a.max_detached > 0 && s.corpses_removed > 0);
            // Foot plants from the moving troopers' MOVE clip, impacts from the DIE clip (detached corpses too),
            // muzzle flashes from their blasters.
            CHECK(a.foot_events > 0 && a.impact_events > 0 && a.muzzles_seen > 0 && s.muzzle_flashes > 0);
            // refix t03: S10_Trooper (Infantry, no HIDE_WHEN_FOGGED) clones itself on the corpse path: no clone object,
            // the dying row plays the clone's death (clone_deaths); key 4 (Force lightning) and ships keep the object.
            CHECK(a.corpse_path_trooper && !a.corpse_path_trooper_k4 && !a.corpse_path_frigate);
            CHECK(s.clone_deaths > 0);
        } else {
            // Frigates / corvettes: the death clone plays DIE and is removed (store object destroyed) after 2 s + 1 s.
            // (refix t01: the star base's own-type clone - no model, Remove_Upon_Death: removed at once - is made on the
            // battle's last tick, which ends the battle before the anim sync starts clone deaths: removed_at_death 0.)
            CHECK(s.clone_deaths > 0 && s.corpses_removed > 0);
            // refix t01: the clones are store objects flagged dying / removing until their death sequence removes them.
            CHECK(a.clone_objects > 0 && a.max_dying > 0);
            // Ships fire from hardpoints: their muzzle records carry the HardPoints index.
            CHECK(a.hp_muzzles_seen > 0);
        }
        for (int32_t w : jobs::determinism_worker_counts()) {
            if (w == 0) continue;
            const AnimRun b = run_anim_campaign(*d, land, w, ticks);
            const auto dv = first_divergence(a.crc, b.crc);
            std::printf("  %s + anim workers %2d: %s\n", land ? "land" : "space", w, dv.message.c_str());
            CHECK(!dv.diverged && b.battles == a.battles && b.stats.flinches == s.flinches && b.stats.corpses_removed == s.corpses_removed &&
                  b.events_digest == a.events_digest && b.muzzles_seen == a.muzzles_seen);
        }
    }
}

// ---- refixf t04: death clone removal, headless and with the animation service ----
// The space campaign on the anim data: frigate / corvette clones (S10_Frigate_Death_Clone: 2 s persistence + 1 s fade, no
// Remove_Upon_Death) must leave the store at the planned frame; the star base's typed clone (30 s) is made on the battle's
// last tick and must not outlive the battle. `anim`: the SimAnimation service is attached (no "anim" CRC system, so the
// CRC logs compare); otherwise the same SimAnimation is only the battle's clone-model source. `wreck`: the clone type gets
// a 30-frame DIE clip (both paths), so the plan includes the clip length.
struct CloneRun {
    CrcLog crc;
    std::vector<std::string> battles;
    std::map<ObjectId, std::pair<uint64_t, uint64_t>> life;  // clone id -> (frame first alive, frame first gone; 0 = never)
    std::map<ObjectId, uint32_t> type;
    uint64_t end_frame = 0;      // frame of the battle's end (object_removed callback)
    uint32_t alive_at_end = ~0u;  // dying objects still alive in the store at the battle's end
    uint32_t rows_missing = 0, detached_corpses = 0;  // anim: alive clone without its row; corpse whose store object left
    SimAnimStats stats;
};

CloneRun run_clone_campaign(const Data& d, bool anim_service, bool wreck, int32_t workers, uint64_t ticks) {
    CloneRun out;
    const auto wreck_model = anim_model({{anim::kAnimDie, {1, 30}}});
    jobs::JobSystem js(jobs::JobSystemConfig{workers});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    SimHost host(d.types, js, cfg);
    SimAnimation animation(d.types, *d.files);
    if (wreck) animation.set_model(d.types.resolve_name("S10_Frigate_Death_Clone"), objects::ModeKind::Space, wreck_model.get());
    if (anim_service) animation.attach(host, false);
    galactic::GalacticSim* gs = host.start_galactic(d.rules, make_setup(d, false));
    CHECK(gs != nullptr);
    if (!gs) return out;
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 6000;
    if (anim_service) opt.animation = &animation;
    else opt.clone_models = &animation;
    opt.object_removed = [&host, &out](int32_t, uint32_t, uint32_t, bool) {  // after the battle's final tick, before the pop
        TacticalMode* tm = host.tactical();
        if (!tm || out.end_frame == host.frame()) return;
        out.end_frame = host.frame();
        out.alive_at_end = 0;
        const objects::ObjectStore& st = tm->primary();
        for (objects::ObjectHandle h : st.all_objects()) {  // dead-but-not-flushed too: clones made and removed on this tick
            if (!st.dying(h)) continue;
            out.alive_at_end += st.alive(h) ? 1u : 0u;
            const ObjectId id = st.id(h);
            if (out.life.emplace(id, std::make_pair(out.end_frame, out.end_frame)).second) out.type[id] = static_cast<uint32_t>(st.type(h));
        }
    };
    CampaignBattles battles(host, d.crules, *d.catalog,
                            [](const std::string& name, terrain::MapData& m) {
                                if (name != "S10_Space_B") return false;
                                m = space_map();
                                return true;
                            },
                            opt);
    CHECK(battles.attach(nullptr));
    const std::vector<uint32_t> uf = gs->fleets_at(2, 1);
    if (!uf.empty()) CHECK(gs->move_fleet(1, uf[0], 1) == commands::RejectReason::None);
    for (uint64_t t = 0; t < ticks; ++t) {
        host.tick_once();
        const uint64_t f = host.frame();
        TacticalMode* tm = host.tactical();
        std::vector<ObjectId> present;
        if (tm) {
            const objects::ObjectStore& st = tm->primary();
            const anim::AnimWorld* w = anim_service ? animation.find_world(*tm) : nullptr;
            for (objects::ObjectHandle h : st.all_objects()) {
                if (!st.alive(h) || !st.dying(h)) continue;
                const ObjectId id = st.id(h);
                present.push_back(id);
                if (out.life.emplace(id, std::make_pair(f, uint64_t{0})).second) out.type[id] = static_cast<uint32_t>(st.type(h));
                if (w && wreck && out.type[id] == type_of(d, "S10_Frigate_Death_Clone") && !w->contains(id)) ++out.rows_missing;
            }
            if (anim_service)
                for (const anim::Corpse& c : animation.corpses(*tm)) out.detached_corpses += c.detached ? 1u : 0u;
        }
        for (auto& [id, l] : out.life)
            if (l.second == 0 && std::find(present.begin(), present.end(), id) == present.end()) l.second = f;
    }
    out.crc = host.crc_log();
    out.battles = battles.log();
    out.stats = animation.stats();
    if (anim_service) animation.detach(host);
    return out;
}

void test_death_clone_removal() {
    auto d = make_data(false, true);
    const uint32_t frigate_clone = type_of(*d, "S10_Frigate_Death_Clone"), base_clone = type_of(*d, "S10_Star_Base_Death_Clone");
    for (const bool wreck : {false, true}) {
        const CloneRun h = run_clone_campaign(*d, false, wreck, 0, 2500);
        // Planned lifetime: no clip 60 + 30 frames; with the 30-frame DIE clip (finished 28 advances after its start, seen
        // by the corpse service one frame later) 28 + 1 + 60 + 30.
        const uint64_t expect = wreck ? 119 : 90;
        uint32_t removed = 0, other = 0, base_clones = 0;
        uint64_t base_made = 0;
        for (const auto& [id, l] : h.life) {
            const uint32_t t = h.type.at(id);
            if (t == frigate_clone && l.second != 0 && l.second < h.end_frame) {
                ++removed;
                if (l.second - l.first != expect) ++other;
            }
            if (t == base_clone) {
                ++base_clones;
                base_made = l.first;
            }
        }
        for (const std::string& s : h.battles) std::printf("  %s\n", s.c_str());
        std::printf("  death clones %s: %zu clone objects, %u frigate clones removed after %llu frames (%u other), star base clone made "
                    "at %llu, battle end %llu, dying alive at the end %u\n",
                    wreck ? "(DIE clip)" : "(no clip)", h.life.size(), removed, static_cast<unsigned long long>(expect), other,
                    static_cast<unsigned long long>(base_made), static_cast<unsigned long long>(h.end_frame), h.alive_at_end);
        CHECK(removed > 0 && other == 0);              // headless: removed at the planned frame (were never removed before)
        CHECK(base_clones == 1 && base_made + 210 == h.end_frame);  // made at the declaration (story t06: 210 frames to the end) ...
        CHECK(h.alive_at_end == 0);                    // ... and gone with the battle, like every clone left
        for (const auto& [id, l] : h.life) CHECK(l.second != 0);  // no clone object after the battle
        // The animation service changes nothing in the sim: same CRC log, same clone lifetimes; its corpse rows follow
        // the battle's removals frame for frame (no clone without its row, no detached clone corpse).
        const CloneRun a = run_clone_campaign(*d, true, wreck, 0, 2500);
        const auto dv = first_divergence(h.crc, a.crc);
        std::printf("  death clones + anim service: %s; clone deaths %u, corpses removed %u, rows missing %u, detached %u\n",
                    dv.message.c_str(), a.stats.clone_deaths, a.stats.corpses_removed, a.rows_missing, a.detached_corpses);
        CHECK(!dv.diverged && a.battles == h.battles && a.life == h.life && a.end_frame == h.end_frame && a.alive_at_end == 0);
        CHECK(a.stats.clone_deaths > 0 && a.rows_missing == 0);
        if (wreck) CHECK(a.detached_corpses == 0 && a.stats.corpses_removed >= removed);
        // Worker counts: identical.
        const CloneRun w4 = run_clone_campaign(*d, false, wreck, 4, 2500);
        CHECK(!first_divergence(h.crc, w4.crc).diverged && w4.life == h.life);
    }
}
#endif

// --- P4 tactical: campaign Black Market gate (ABILITIES_HARDPOINTS 2.4, P4_SIM_SYSTEMS 1.5) -------------------------
// The space campaign on the bm data: at battle frame 10 the gate state of every frigate / corvette's POWER_TO_WEAPONS
// is read and the human Empire sends command 43 on it. Without a purchase both players are gated (no activation);
// with S10_BM_Item given to the Empire before the start only the Underworld stays gated. CRC log identical for every
// worker count and equal to testdata/battle_bm_golden.crc (MSVC writes it with --regen; clang must reproduce it).
struct BmRun {
    CrcLog crc;
    std::vector<std::string> battles;
    uint32_t allowed[2] = {0, 0}, gated[2] = {0, 0}, active_after = 0;
    int command = -1;
};

BmRun run_bm_campaign(const Data& d, bool bought, int32_t workers, uint64_t ticks) {
    BmRun out;
    jobs::JobSystem js(jobs::JobSystemConfig{workers});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    SimHost host(d.types, js, cfg);
    galactic::GalacticSim* gs = host.start_galactic(d.rules, make_setup(d, false));
    CHECK(gs != nullptr);
    if (!gs) return out;
    if (bought) CHECK(gs->bm_give_item(0, "S10_BM_Item"));
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 6000;
    CampaignBattles battles(host, d.crules, *d.catalog,
                            [](const std::string& name, terrain::MapData& m) {
                                if (name != "S10_Space_B") return false;
                                m = space_map();
                                return true;
                            },
                            opt);
    std::vector<std::string> diag;
    CHECK(battles.attach(&diag));
    const std::vector<uint32_t> uf = gs->fleets_at(2, 1);
    CHECK(!uf.empty());
    if (!uf.empty()) CHECK(gs->move_fleet(1, uf[0], 1) == commands::RejectReason::None);
    constexpr uint8_t kP2W = 29;
    for (uint64_t t = 0; t < ticks; ++t) {
        host.tick_once();
        TacticalBattle* b = battles.battle();
        if (!b || out.command >= 0 || b->frame() != 10) continue;
        TacticalMode* tm = host.tactical();
        std::vector<uint32_t> ids;
        std::vector<objects::ObjectId> empire;
        for (size_t p = 0; p < 2; ++p)
            for (objects::ObjectHandle h : tm->primary().owned_by(static_cast<int32_t>(p))) {
                const objects::ObjectId id = tm->primary().id(h);
                if (!tm->primary().alive(h) || !b->abilities().has_ability(id, kP2W)) continue;
                if (b->abilities().black_market_allows(id, kP2W)) ++out.allowed[p];
                else ++out.gated[p];
                if (p == 0) {
                    ids.push_back(static_cast<uint32_t>(id));
                    empire.push_back(id);
                }
            }
        commands::EventData ev(commands::EventType::SetUnitAbilityMode);
        ev.set("ability", kP2W).set("on", 1).set_ids("units", ids);
        out.command = static_cast<int>(host.execute_direct(0, commands::CommandSource::Player, ev));
        for (objects::ObjectId id : empire) out.active_after += b->abilities().is_active(id, kP2W) ? 1u : 0u;
    }
    out.crc = host.crc_log();
    out.battles = battles.log();
    return out;
}

void test_black_market_battle() {
    auto d = make_data(false, false, false, true);
    CHECK(d->rules.special.items.size() == 1);
    // Battle set-up: the blocked list per player (galactic purchases -> tactical name CRCs).
    {
        tactical::abilities::AbilityData ad;
        tactical::abilities::AbilityObjectDesc o;
        tactical::abilities::UnitAbilityRecord listed, unlisted, plain;
        listed.type = tactical::abilities::kDefend;
        listed.gui_activated_ability_name = "S10_BM_Power";
        listed.gui_name_crc = name_crc(listed.gui_activated_ability_name);
        listed.must_be_bought_on_black_market = true;
        unlisted = listed;  // marked but listed by no item: allowed (the TR case)
        unlisted.gui_activated_ability_name = "S10_Not_Listed";
        unlisted.gui_name_crc = name_crc(unlisted.gui_activated_ability_name);
        plain = listed;  // listed but not marked: allowed
        plain.must_be_bought_on_black_market = false;
        o.unit_abilities = {listed, unlisted, plain};
        ad.objects.push_back(o);
        jobs::JobSystem js(jobs::JobSystemConfig{0});
        SimConfig cfg;
        cfg.seed = 2026;
        cfg.players = 4;
        SimHost host(d->types, js, cfg);
        galactic::GalacticSim* gs = host.start_galactic(d->rules, make_setup(*d, false));
        CHECK(gs != nullptr);
        if (gs) {
            const std::vector<uint32_t> b0 = black_market_blocked_abilities(ad, *gs, 0);
            CHECK(b0.size() == 1 && b0[0] == name_crc("S10_BM_Power"));
            CHECK(gs->bm_give_item(0, "S10_BM_Item"));
            CHECK(black_market_blocked_abilities(ad, *gs, 0).empty() && black_market_blocked_abilities(ad, *gs, 1).size() == 1);
        }
    }
    const uint64_t ticks = 900;
    const BmRun a = run_bm_campaign(*d, false, 0, ticks);
    const BmRun p = run_bm_campaign(*d, true, 0, ticks);
    std::printf("  not bought: allowed %u/%u gated %u/%u, command 43 -> %d, active %u\n", a.allowed[0], a.allowed[1], a.gated[0],
                a.gated[1], a.command, a.active_after);
    std::printf("  bought:     allowed %u/%u gated %u/%u, command 43 -> %d, active %u\n", p.allowed[0], p.allowed[1], p.gated[0],
                p.gated[1], p.command, p.active_after);
    CHECK(a.command >= 0 && a.allowed[0] == 0 && a.allowed[1] == 0 && a.gated[0] > 0 && a.gated[1] > 0 && a.active_after == 0);
    CHECK(p.command >= 0 && p.allowed[0] == a.gated[0] && p.gated[0] == 0 && p.gated[1] == a.gated[1] && p.active_after == p.allowed[0]);
    CHECK(first_divergence(a.crc, p.crc).diverged);  // the activation shows in the state
    for (int32_t w : jobs::determinism_worker_counts()) {
        if (w == 0) continue;
        const BmRun b = run_bm_campaign(*d, false, w, ticks);
        const auto dv = first_divergence(a.crc, b.crc);
        std::printf("  bm battle workers %2d: %s\n", w, dv.message.c_str());
        CHECK(!dv.diverged && b.battles == a.battles);
    }
    const std::string path = std::string(ALAMO_SIM_TESTDATA) + "/battle_bm_golden.crc";
    CrcLog log = a.crc;
    log.comments = {"P4 tactical Black Market gate: S10 synthetic space campaign, POWER_TO_WEAPONS marked and not bought, "
                    "seed 2026, " + std::to_string(ticks) + " ticks"};
    std::string err;
    if (g_regen) {
        CHECK(log.save(path, &err));
        std::printf("  wrote %s\n", path.c_str());
    } else {
        CrcLog g;
        if (!CrcLog::load(path, g, &err)) {
            std::printf("FAIL golden %s: %s (run with --regen)\n", path.c_str(), err.c_str());
            ++g_fail;
        } else {
            const auto dv = first_divergence(g, log);
            std::printf("  golden battle_bm_golden.crc: %s\n", dv.message.c_str());
            CHECK(!dv.diverged);
        }
    }
}
// --- end P4 tactical ---

// --- battle host (BT pass): land lead / reserve split, HIJACK reserve, ResumeGame, map picker, pad command refusals ---
void test_bt_pass(const Data& d) {
    // Map picker (TACTICAL_GAPS §4.10, [port] deterministic pick).
    {
        using MC = CampaignBattleOptions::MapCandidate;
        std::vector<MC> c(4);
        c[0] = {"Land_A", false, 0, -1, 2, 5, false};
        c[1] = {"Land_B", false, -1, 1, 2, 5, false};
        c[2] = {"Land_C", false, 0, -1, 1, 5, false};  // 1 player: never
        c[3] = {"Space_A", true, 7, -1, 2, 5, false};
        const std::string a = pick_random_map(c, false, 0, 1, 2);
        CHECK(a == "Land_A" || a == "Land_B");
        CHECK(pick_random_map(c, false, 0, 1, 2) == a);         // deterministic
        CHECK(pick_random_map(c, false, 3, 0, 2).empty());       // terrain 3, faction 0: no candidate
        CHECK(pick_random_map(c, true, 7, 0, 1) == "Space_A");
        CHECK(pick_random_map(c, false, 0, 0, 6).empty());       // base level above every max
    }
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    SimHost host(d.types, js, cfg);
    galactic::GalacticSim* gs = host.start_galactic(d.rules, make_setup(d, true));
    CHECK(gs != nullptr);
    if (!gs) return;
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Pending;  // the human defender answers with ResumeGame
    opt.battle.max_frames = 600;
    CampaignBattles battles(host, d.crules, *d.catalog,
                            [](const std::string& name, terrain::MapData& m) {
                                if (name != "S10_Land_B") return false;
                                m = land_map();
                                return true;
                            },
                            opt);
    CHECK(battles.attach(nullptr));
    std::vector<objects::ObjectHandle> up;
    for (objects::ObjectHandle h : gs->planets()[2].surface)
        if (gs->store().owner(h) == 1) up.push_back(h);
    CHECK(up.size() == 2);
    CHECK(gs->launch_units(1, 2, up) == commands::RejectReason::None);
    const std::vector<uint32_t> uf = gs->fleets_at(2, 1);
    if (!uf.empty()) CHECK(gs->move_fleet(1, uf[0], 1) == commands::RejectReason::None);
    bool invaded = false, resumed = false, checked = false;
    for (uint64_t t = 0; t < 1500 && !checked; ++t) {
        host.tick_once();
        if (!invaded && host.modes().depth() == 1) {
            const std::vector<uint32_t> f = gs->fleets_at(1, 1);
            if (!f.empty() && !gs->fleets()[f[0]].moving) invaded = gs->invade(1, 1, {}) == commands::RejectReason::None;
        }
        if (!resumed && battles.conflicts().state() == galactic::ConflictState::Vote) {
            commands::EventData ev(commands::EventType::ResumeGame);
            ev.set("flag", 0);  // bit 0 = fight
            CHECK(host.execute_direct(0, commands::CommandSource::Player, ev) == commands::RejectReason::None);
            resumed = true;
        }
        if (TacticalBattle* b = battles.battle(); b && host.tactical() && b->frame() == 1) {
            // TACTICAL_GAPS §4.2: the attacker starts with its lead unit only; the second squad is in the reserve.
            CHECK(!b->land_raid());
            CHECK(b->reserves(1).size() == 1);
            CHECK(b->reserves(0).empty());
            // HIJACK reserve: a galactic squad created at the battle planet joins player 1's pool; once only; a frigate
            // (not in this land battle's tables) is refused; the defender's own surface unit is already a participant.
            const objects::ObjectHandle hs = gs->spawn_unit(1, 1, type_of(d, "S10_Squad"));
            CHECK(static_cast<bool>(hs));
            CHECK(b->add_reserve(hs, 1));
            CHECK(!b->add_reserve(hs, 1));
            CHECK(b->reserves(1).size() == 2);
            CHECK(!b->can_add_reserve(type_of(d, "S10_Frigate")));
            CHECK(!b->add_reserve(hs, 2));  // not a side of this battle
            // TacticalBuild (33) / TacticalSell (34) refusals without pads.
            commands::EventData tb(commands::EventType::TacticalBuild);
            tb.set("pad", 1).set("type_index", type_of(d, "S10_Trooper"));
            CHECK(b->handle_command(*host.tactical(), 0, tb, host.random().synced()) == commands::RejectReason::InvalidTarget);
            commands::EventData ts(commands::EventType::TacticalSell);
            ts.set("object", 1);
            CHECK(b->handle_command(*host.tactical(), 0, ts, host.random().synced()) == commands::RejectReason::InvalidUnit);
            CHECK(b->lead_object() == objects::kNoObjectId);
            checked = true;
        }
    }
    CHECK(invaded && resumed && checked);
}
// --- end battle host ---

// --- S10 map owners [probe 2026-10-29] (REMAINING_GAPS §3.3): a .ted owner is a faction index resolved to that faction's
// player; natives without a player -> the Hostile player; a playable faction without a player -> dropped. The attacker
// that owns no reinforcement point gets the Neutral one nearest its Attacker Entry marker. The galaxy's player order
// deliberately differs from the faction order: Underworld (faction 1) = player 0 invades, Empire (faction 0) = player 1
// defends, Neutral (2) = player 2, Hostile (faction 4) = player 3. ---
void test_map_owners(const Data& d) {
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    SimHost host(d.types, js, cfg);
    galactic::GalacticSetup setup = make_setup(d, true);
    std::swap(setup.players[0], setup.players[1]);
    for (galactic::StartingForce& f : setup.forces) f.player = f.player == 0 ? 1 : f.player == 1 ? 0 : f.player;
    galactic::PlayerSetup hostile;
    hostile.faction = d.rules.find_faction("Hostile");
    CHECK(hostile.faction == 4u && d.rules.find_faction("S10_Natives") == 3u && d.rules.find_faction("S10_Absent") == 5u);
    setup.players.push_back(hostile);
    galactic::GalacticSim* gs = host.start_galactic(d.rules, std::move(setup));
    CHECK(gs != nullptr);
    if (!gs) return;
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 300;
    CampaignBattles battles(host, d.crules, *d.catalog,
                            [](const std::string& name, terrain::MapData& m) {
                                if (name != "S10_Land_B") return false;
                                m = land_map();
                                auto place = [&m](const char* type, float x, float y, uint32_t raw) {
                                    terrain::ObjectPlacement p;
                                    p.type_crc = name_crc(type);
                                    p.owner = raw;
                                    p.position[0] = x;
                                    p.position[1] = y;
                                    m.objects.push_back(p);
                                };
                                place("Defending Forces Position", 900.0f, 900.0f, 2);
                                place("Attacker Entry Position", 300.0f, 300.0f, 2);
                                place("S4_RPoint", 1500.0f, 1500.0f, 2);  // far Neutral point: stays Neutral
                                place("S4_RPoint", 330.0f, 320.0f, 2);    // nearest the entry: the attacker's landing point
                                place("S4_Post", 600.0f, 1800.0f, 0);     // Empire -> player 1
                                place("S4_Post", 700.0f, 1800.0f, 1);     // Underworld -> player 0 = attacker: branched-map discard (§3.1 item 4)
                                place("S4_Post", 800.0f, 1800.0f, 3);     // native faction, no player -> Hostile (3)
                                place("S4_Post", 900.0f, 1800.0f, 5);     // playable faction without a player -> dropped
                                place("S4_Post", 1000.0f, 1800.0f, 4);    // Hostile -> player 3
                                place("S4_Post", 1100.0f, 1800.0f, 9);    // no such faction -> dropped
                                return true;
                            },
                            opt);
    CHECK(battles.attach(nullptr));
    std::vector<objects::ObjectHandle> up;
    for (objects::ObjectHandle h : gs->planets()[2].surface)
        if (gs->store().owner(h) == 0) up.push_back(h);
    CHECK(gs->launch_units(0, 2, up) == commands::RejectReason::None);
    const std::vector<uint32_t> uf = gs->fleets_at(2, 0);
    if (!uf.empty()) CHECK(gs->move_fleet(0, uf[0], 1) == commands::RejectReason::None);
    bool invaded = false, checked = false;
    for (uint64_t t = 0; t < 1500 && !checked; ++t) {
        host.tick_once();
        if (!invaded && host.modes().depth() == 1) {
            const std::vector<uint32_t> f = gs->fleets_at(1, 0);
            if (!f.empty() && !gs->fleets()[f[0]].moving) invaded = gs->invade(0, 1, {}) == commands::RejectReason::None;
        }
        TacticalBattle* b = battles.battle();
        if (!b || !host.tactical() || b->frame() != 1) continue;
        CHECK(b->request().info.attacker == 0 && b->request().info.defender == 1);
        const objects::ObjectStore& st = host.tactical()->primary();
        const objects::TypeIndex post = d.types.resolve_name("S4_Post"), rp = d.types.resolve_name("S4_RPoint");
        std::map<int, int32_t> post_owner;  // x position -> owner
        int32_t near_owner = -9, far_owner = -9;
        objects::ObjectId near_id = objects::kNoObjectId;
        for (const objects::ObjectHandle h : st.all_objects()) {
            if (!st.alive(h)) continue;
            const Vec3 p = st.position(h);
            if (st.type(h) == post) post_owner[static_cast<int>(std::lround(p.x / 100.0f)) * 100] = st.owner(h);
            if (st.type(h) == rp && p.x < 400.0f) {
                near_owner = st.owner(h);
                near_id = st.id(h);
            }
            if (st.type(h) == rp && p.x > 1000.0f) far_owner = st.owner(h);
        }
        std::printf("  map owners: posts");
        for (const auto& [x, o] : post_owner) std::printf(" %d:%d", x, o);
        std::printf("; landing point owner %d (far point %d), landing points %zu\n", near_owner, far_owner, b->landing_points().size());
        CHECK(post_owner.size() == 3);
        CHECK(post_owner.count(600) && post_owner[600] == 1);
        CHECK(!post_owner.count(700));  // the attacker's Is_Branched_Map_Discardable object (default Yes) is discarded
        CHECK(post_owner.count(800) && post_owner[800] == 3);
        CHECK(post_owner.count(1000) && post_owner[1000] == 3);
        CHECK(!post_owner.count(900) && !post_owner.count(1100));
        CHECK(near_owner == 0 && far_owner == 2);
        CHECK(b->landing_points().size() == 1 && b->landing_points()[0] == near_id);
        checked = true;
    }
    CHECK(invaded && checked);
}

// Campaign galaxy (build_campaign_setup of S10_GC): players Empire 0 (human), Underworld 1, Neutral 2 and the Hostile
// player 3 the setup adds. `drop_hostile` removes it again (the galaxy before the change).
galactic::GalacticSetup campaign_setup(const Data& d, bool drop_hostile) {
    std::vector<std::string> diag;
    galactic::GalacticSetup s = galactic::build_campaign_setup(d.db, d.rules, "S10_GC", {}, &diag);
    for (uint32_t i = 0; i < s.map.planets.size(); ++i)  // the synthetic planets carry no Galactic_Position
        s.map.planets.position[i] = {static_cast<float>(i) * 60.0f, 0.0f, 0.0f};
    if (drop_hostile && !s.players.empty() && s.players.back().faction == d.rules.find_faction("Hostile")) s.players.pop_back();
    return s;
}

// The S10_GC land invasion of S10_B (Underworld 1 attacks Empire 0): map posts at x 600.. with raw owners (faction
// indices) 0 Empire, 1 Underworld, 3 S10_Natives, 4 Hostile; returns x -> owner at battle frame 1.
std::map<int, int32_t> campaign_battle_posts(const Data& d, bool drop_hostile) {
    std::map<int, int32_t> out;
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    SimHost host(d.types, js, cfg);
    galactic::GalacticSim* gs = host.start_galactic(d.rules, campaign_setup(d, drop_hostile));
    CHECK(gs != nullptr);
    if (!gs) return out;
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 300;
    CampaignBattles battles(host, d.crules, *d.catalog,
                            [](const std::string& name, terrain::MapData& m) {
                                if (name != "S10_Land_B") return false;
                                m = land_map();
                                auto place = [&m](const char* type, float x, float y, uint32_t raw) {
                                    terrain::ObjectPlacement p;
                                    p.type_crc = name_crc(type);
                                    p.owner = raw;
                                    p.position[0] = x;
                                    p.position[1] = y;
                                    m.objects.push_back(p);
                                };
                                place("S4_Post", 600.0f, 1800.0f, 0);   // Empire (defender) -> player 0
                                place("S4_Post", 700.0f, 1800.0f, 1);   // Underworld (attacker): branched-map discard
                                place("S4_Post", 800.0f, 1800.0f, 3);   // native faction without a player -> Hostile
                                place("S4_Post", 1000.0f, 1800.0f, 4);  // Hostile
                                return true;
                            },
                            opt);
    CHECK(battles.attach(nullptr));
    std::vector<objects::ObjectHandle> up;
    for (objects::ObjectHandle h : gs->planets()[2].surface)
        if (gs->store().owner(h) == 1) up.push_back(h);
    CHECK(gs->launch_units(1, 2, up) == commands::RejectReason::None);
    const std::vector<uint32_t> uf = gs->fleets_at(2, 1);
    if (!uf.empty()) CHECK(gs->move_fleet(1, uf[0], 1) == commands::RejectReason::None);
    bool invaded = false, checked = false;
    for (uint64_t t = 0; t < 1500 && !checked; ++t) {
        host.tick_once();
        if (!invaded && host.modes().depth() == 1) {
            const std::vector<uint32_t> f = gs->fleets_at(1, 1);
            if (!f.empty() && !gs->fleets()[f[0]].moving) invaded = gs->invade(1, 1, {}) == commands::RejectReason::None;
        }
        TacticalBattle* b = battles.battle();
        if (!b || !host.tactical() || b->frame() != 1) continue;
        CHECK(b->request().info.attacker == 1 && b->request().info.defender == 0);
        const objects::ObjectStore& st = host.tactical()->primary();
        const objects::TypeIndex post = d.types.resolve_name("S4_Post");
        for (const objects::ObjectHandle h : st.all_objects())
            if (st.alive(h) && st.type(h) == post) out[static_cast<int>(std::lround(st.position(h).x / 100.0f)) * 100] = st.owner(h);
        checked = true;
    }
    CHECK(invaded && checked);
    return out;
}

// [probe 2026-10-29, inferred] the campaign's Hostile player: present after Neutral in faction order, inert on the galactic
// map (no AI, no credits, no planets, no income), and the owner of the map's native objects in a campaign land battle.
void test_campaign_hostile(const Data& d) {
    const galactic::GalacticSetup s = campaign_setup(d, false);
    const uint32_t hostile = d.rules.find_faction("Hostile");
    CHECK(s.players.size() == 4 && s.players[0].faction == d.rules.find_faction("Empire") && s.players[0].human &&
          s.players[1].faction == d.rules.find_faction("Underworld") && s.players[2].faction == d.rules.find_faction("Neutral"));
    CHECK(s.players.size() == 4 && s.players[3].faction == hostile && !s.players[3].human && !s.players[3].ai &&
          s.players[3].credits == 0.0f);
    for (const galactic::StartingForce& f : s.forces) CHECK(f.player != 3);
    // Galactic behaviour per faction is the same with and without the Hostile player (no battle: 2400 ticks of economy).
    auto run = [&d](bool drop) {
        std::vector<float> trace;
        jobs::JobSystem js(jobs::JobSystemConfig{0});
        SimConfig cfg;
        cfg.seed = 2026;
        cfg.players = 4;
        SimHost host(d.types, js, cfg);
        galactic::GalacticSim* gs = host.start_galactic(d.rules, campaign_setup(d, drop));
        CHECK(gs != nullptr);
        if (!gs) return trace;
        for (int t = 0; t < 2400; ++t) {
            host.tick_once();
            if (t % 60 != 59) continue;
            for (const char* f : {"Empire", "Underworld", "Neutral"}) {
                const int32_t p = gs->player_of_faction(f);
                trace.push_back(gs->credits(p));
                trace.push_back(static_cast<float>(gs->planets_owned(p)));
                trace.push_back(static_cast<float>(gs->players()[static_cast<size_t>(p)].tech_level));
            }
            for (const galactic::PlanetState& p : gs->planets())
                trace.push_back(p.owner < 0 ? -1.0f : static_cast<float>(gs->players()[static_cast<size_t>(p.owner)].faction));
            const int32_t h = gs->player_of_faction("Hostile");
            if (!drop) CHECK(h == 3 && gs->credits(h) == 0.0f && gs->planets_owned(h) == 0 && gs->fleets_at(1, h).empty());
        }
        std::printf("  campaign hostile (%s): day %d, credits E %.0f U %.0f\n", drop ? "dropped" : "present", gs->galactic_day(),
                    static_cast<double>(gs->credits(gs->player_of_faction("Empire"))),
                    static_cast<double>(gs->credits(gs->player_of_faction("Underworld"))));
        return trace;
    };
    const std::vector<float> with = run(false), without = run(true);
    CHECK(!with.empty() && with == without);
    // The land battle: the Hostile- and native-owned posts survive with the Hostile player; without it they are dropped.
    std::map<int, int32_t> posts = campaign_battle_posts(d, false);
    std::printf("  campaign land battle posts:");
    for (const auto& [x, o] : posts) std::printf(" %d:%d", x, o);
    std::printf("\n");
    CHECK(posts.size() == 3 && posts.count(600) && posts[600] == 0 && !posts.count(700));
    CHECK(posts.count(800) && posts[800] == 3 && posts.count(1000) && posts[1000] == 3);
    posts = campaign_battle_posts(d, true);
    CHECK(posts.size() == 1 && posts.count(600) && posts[600] == 0);
}
// --- end S10 map owners ---

// --- hangars / station contents / skirmish tactical production (tactical_battle_hangar.cpp) ---
// The S10 space battle as a skirmish battle: at frame 2 the human Empire buys S10_Base_Upgrade at its star base
// (Production {type_index, location}); a second purchase is refused (Build_Limit_Current_For_All_Allies 1), a
// non-builder location too. After the 1 s build the upgrade object sits in the star base's contents, its
// Skirmish_Automatic Starbase_Upgrade_Ability fires: S10_Star_Base_2 replaces the star base, the contents move with
// it, the battle tech level rises by one and the upgrade object is removed silently.
void test_station_production() {
    auto d = make_data(false, false, false, false, true);
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    SimHost host(d->types, js, cfg);
    galactic::GalacticSim* gs = host.start_galactic(d->rules, make_setup(*d, false));
    CHECK(gs != nullptr);
    if (!gs) return;
    gs->add_credits(0, 1000.0f, false);
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 600;
    opt.battle.skirmish = true;
    CampaignBattles battles(host, d->crules, *d->catalog,
                            [](const std::string& name, terrain::MapData& m) {
                                if (name != "S10_Space_B") return false;
                                m = space_map();
                                return true;
                            },
                            opt);
    CHECK(battles.attach(nullptr));
    const std::vector<uint32_t> uf = gs->fleets_at(2, 1);
    CHECK(!uf.empty());
    if (!uf.empty()) CHECK(gs->move_fleet(1, uf[0], 1) == commands::RejectReason::None);
    const uint32_t base1 = type_of(*d, "S10_Star_Base_1"), base2 = type_of(*d, "S10_Star_Base_2"), up = type_of(*d, "S10_Base_Upgrade");
    objects::ObjectId station = objects::kNoObjectId;
    bool bought = false, held = false, upgraded = false;
    float before = 0.0f, after = 0.0f;
    int32_t tech0 = 0;
    for (uint64_t t = 0; t < 3000 && !upgraded; ++t) {
        host.tick_once();
        TacticalBattle* b = battles.battle();
        TacticalMode* tm = host.tactical();
        if (!b || !tm) continue;
        if (!bought && b->frame() == 2) {
            CHECK(b->production_enabled());
            for (const objects::ObjectHandle h : tm->primary().owned_by(0))
                if (tm->primary().alive(h) && tm->primary().type(h) == base1) station = tm->primary().id(h);
            CHECK(station != objects::kNoObjectId);
            tech0 = b->battle_tech_level(0);
            before = gs->credits(0);
            commands::EventData ev(commands::EventType::Production);
            ev.set("type_index", up).set("location", static_cast<uint32_t>(station));
            CHECK(host.execute_direct(0, commands::CommandSource::Player, ev) == commands::RejectReason::None);
            after = gs->credits(0);
            CHECK(host.execute_direct(0, commands::CommandSource::Player, ev) == commands::RejectReason::NotAllowed);  // limit 1
            commands::EventData bad(commands::EventType::Production);
            bad.set("type_index", up).set("location", 999999u);
            CHECK(host.execute_direct(0, commands::CommandSource::Player, bad) != commands::RejectReason::None);
            CHECK(b->production() && b->production()->queue(0).size() == 1);
            bought = true;
        }
        if (bought && !held && b->contents() && !b->contents()->contents(station).empty()) {  // the built upgrade object
            held = true;
            const tactical::combat::ObjectId uo = b->contents()->contents(station).front();
            std::printf("  frame %u: upgrade object %d built in the star base\n", b->frame(), uo);
            CHECK(b->abilities().has_object(uo) && b->contents()->container_of(uo) == station);
            CHECK(b->frame() >= 2 + 30);  // Tactical_Build_Time_Seconds 1 at 30 FPS
        }
        if (bought && !b->station_upgrades().empty()) {
            const TacticalBattle::StationUpgrade& u = b->station_upgrades().front();
            const objects::ObjectHandle nh = tm->primary().find(u.new_station);
            std::printf("  station %d -> %d (owner %d), tech %d -> %d, credits %.0f -> %.0f, contents of the new station %zu\n", u.old_station,
                        u.new_station, u.owner, tech0, b->battle_tech_level(0), static_cast<double>(before), static_cast<double>(after),
                        b->contents()->contents(u.new_station).size());
            CHECK(u.old_station == station && u.owner == 0 && tm->primary().alive(nh) && tm->primary().type(nh) == base2);
            CHECK(!tm->primary().alive(tm->primary().find(station)));
            CHECK(b->battle_tech_level(0) == tech0 + 1);
            CHECK(b->contents()->contents(station).empty());
            upgraded = true;
        }
    }
    std::printf("  bought %d, upgrade object held %d, upgraded %d, credits %.1f -> %.1f\n", bought ? 1 : 0, held ? 1 : 0,
                upgraded ? 1 : 0, static_cast<double>(before), static_cast<double>(after));
    CHECK(bought && held && upgraded);
    CHECK(after == before - 100.0f);
}
// --- end hangars ---

// --- OI host: BattleAi orders without AI players (empty definitions; the battle's stand-in off) ---
//   * MoveToObject (Lua Move_To(object)): the mover's attack order is cleared and it moves to the target's position; a
//     missing target is a silent no-op; foreign movers are refused;
//   * hijack_unit (HIJACK_UNIT): a galactic frigate spawned at the battle planet joins the attacker's reserve pool; a type
//     outside the battle tables is refused;
//   * the AI VMs' ObjectBridge::change_owner: the battle's script_change_owner (store owner + the battle side).
#ifdef ALAMO_SIM_HAVE_TACTICAL_AI
void test_battle_ai_orders(const Data& d) {
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    SimHost host(d.types, js, cfg);
    galactic::GalacticSim* gs = host.start_galactic(d.rules, make_setup(d, false));
    CHECK(gs != nullptr);
    if (!gs) return;
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 6000;
    opt.battle.order_interval = 0;  // no stand-in orders: only the test moves units
    CampaignBattles battles(host, d.crules, *d.catalog,
                            [](const std::string& name, terrain::MapData& m) {
                                if (name != "S10_Space_B") return false;
                                m = space_map();
                                return true;
                            },
                            opt);
    CHECK(battles.attach(nullptr));
    const std::vector<uint32_t> uf = gs->fleets_at(2, 1);
    if (!uf.empty()) CHECK(gs->move_fleet(1, uf[0], 1) == commands::RejectReason::None);
    ai::AiDefinitions defs;
    ai::EquationSet eqs;
    TacticalAiSetup setup;
    setup.defs = &defs;
    setup.equations = &eqs;
    setup.files = d.files.get();
    setup.ai_type = [](int32_t, const galactic::GalacticSim&) { return std::string(); };  // no AI player: orders only
    using R = commands::RejectReason;
    objects::ObjectId mover = objects::kNoObjectId, target = objects::kNoObjectId;
    float d0 = 0.0f;
    bool issued = false, closer = false;
    for (uint64_t t = 0; t < 2500; ++t) {
        host.tick_once();
        TacticalBattle* b = battles.battle();
        TacticalMode* tm = host.tactical();
        if (!b || !tm) continue;
        objects::ObjectStore& st = tm->primary();
        auto dist = [&st](objects::ObjectId a, objects::ObjectId c) {
            const Vec3 p = st.position(st.find(a)), q = st.position(st.find(c));
            return std::sqrt((p.x - q.x) * (p.x - q.x) + (p.y - q.y) * (p.y - q.y));
        };
        if (b->frame() == 3 && !issued) {
            issued = true;
            objects::ObjectId own = objects::kNoObjectId, corvette = objects::kNoObjectId;
            for (const objects::ObjectHandle h : st.all_objects()) {
                if (!st.alive(h) || !b->movement().has_unit(st.id(h))) continue;
                const auto ty = static_cast<uint32_t>(st.type(h));
                if (ty == type_of(d, "S10_Frigate") && st.owner(h) == 1 && mover == objects::kNoObjectId) mover = st.id(h);
                if (ty == type_of(d, "S10_Corvette") && st.owner(h) == 1 && corvette == objects::kNoObjectId) corvette = st.id(h);
                if (ty == type_of(d, "S10_Frigate") && st.owner(h) == 0 && target == objects::kNoObjectId) target = st.id(h);
                if (st.owner(h) == 0 && own == objects::kNoObjectId) own = st.id(h);
            }
            CHECK(mover != objects::kNoObjectId && target != objects::kNoObjectId && corvette != objects::kNoObjectId);
            if (mover == objects::kNoObjectId || target == objects::kNoObjectId || corvette == objects::kNoObjectId) break;
            BattleAi ai(*b, *tm, host, setup);
            CHECK(!ai.controls(0) && !ai.controls(1));
            CHECK(ai.execute(1, commands::ev::attack(static_cast<uint32_t>(target), {static_cast<uint32_t>(mover)})) == R::None);
            CHECK(b->combat().ordered_target(mover) == target);
            d0 = dist(mover, target);
            CHECK(ai.execute(1, commands::ev::move_to_object(static_cast<uint32_t>(target), {static_cast<uint32_t>(mover)})) == R::None);
            CHECK(b->combat().ordered_target(mover) == objects::kNoObjectId);  // combat order cleared
            const auto v = b->movement().view(mover);
            const Vec3 tp = st.position(st.find(target));
            // The goal is the target's position up to the movement system's arrival offset.
            const float gd = std::sqrt((v.goal.x - tp.x) * (v.goal.x - tp.x) + (v.goal.y - tp.y) * (v.goal.y - tp.y));
            CHECK(v.has_goal && gd < 0.2f * d0);
            CHECK(ai.execute(1, commands::ev::move_to_object(0x200000u, {static_cast<uint32_t>(mover)})) == R::None);  // no such target: no-op
            CHECK(b->movement().view(mover).has_goal);
            CHECK(ai.execute(1, commands::ev::move_to_object(static_cast<uint32_t>(target), {static_cast<uint32_t>(own)})) == R::InvalidUnit);  // not player 1's unit
            // HIJACK_UNIT: a galactic frigate at the battle planet joins player 1's pool; a land squad is refused.
            const size_t pool0 = b->reserves(1).size();
            const ai::tactical::TacticalReinforcement r = ai.hijack_unit(1, static_cast<int32_t>(type_of(d, "S10_Frigate")), false);
            CHECK(r.key >= 0);
            CHECK(b->reserves(1).size() == pool0 + 1);
            CHECK(ai.hijack_unit(1, static_cast<int32_t>(type_of(d, "S10_Squad")), false).key < 0);
            CHECK(b->reserves(1).size() == pool0 + 1);
            // Change_Owner from an AI VM: the corvette joins the Empire (store owner, no longer an enemy of player 0).
            lua::ObjectBridge& br = ai.world().object_bridge();
            CHECK(static_cast<bool>(br.change_owner));
            if (br.change_owner) {
                CHECK(br.change_owner(st.find(corvette), 0) && st.owner(st.find(corvette)) == 0);
                CHECK(!br.change_owner(objects::ObjectHandle{}, 0));
            }
            CHECK(!b->combat().is_enemy(0, st.owner(st.find(corvette))));
            std::printf("  battle AI: MoveToObject goal (%.0f, %.0f) %.0f from the target, distance %.0f; hijack key %d, pool %zu -> %zu\n",
                        static_cast<double>(v.goal.x), static_cast<double>(v.goal.y), static_cast<double>(gd), static_cast<double>(d0), r.key, pool0,
                        b->reserves(1).size());
        }
        if (issued && b->frame() == 63) {
            CHECK(st.alive(st.find(mover)) && st.alive(st.find(target)));
            if (st.alive(st.find(mover)) && st.alive(st.find(target))) {
                const float d1 = dist(mover, target);
                std::printf("  battle AI: mover %.0f -> %.0f from the target after 60 frames\n", static_cast<double>(d0),
                            static_cast<double>(d1));
                closer = d1 < d0 - 10.0f;
            }
            break;
        }
    }
    CHECK(issued && closer);
}

// refixf t02: no two space units on one exact point (foc-station seeds 1 / 4 stacked the rebel fleet on the station's centre):
//   * a SPACE_OBSTACLE star base is a static entry of the space occupancy from the battle start (MOVEMENT §2.2) and leaves it
//     when it dies;
//   * the tactical AI's task-force Attack_Move on the station (refixf t06: one PortExtension group order, the further units
//     after the 24-byte block) is one group move (MOVEMENT §6.3): every goal outside the station's footprint and no two goals
//     on one point;
//   * no two live movable units share a position at the start, after the orders or while they fly in and fight.
void test_space_obstacle_orders() {
    auto d = make_data(false, false, false, false, false, true);
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    SimHost host(d->types, js, cfg);
    galactic::GalacticSetup gsetup = make_setup(*d, false);
    for (int i = 0; i < 3; ++i) gsetup.forces.push_back({0, 1, type_of(*d, "S10_Frigate")});  // the battle outlives the station
    galactic::GalacticSim* gs = host.start_galactic(d->rules, gsetup);
    CHECK(gs != nullptr);
    if (!gs) return;
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 6000;
    opt.battle.order_interval = 0;  // no stand-in orders: only the test moves units
    CampaignBattles battles(host, d->crules, *d->catalog,
                            [](const std::string& name, terrain::MapData& m) {
                                if (name != "S10_Space_B") return false;
                                m = space_map();
                                return true;
                            },
                            opt);
    CHECK(battles.attach(nullptr));
    const std::vector<uint32_t> uf = gs->fleets_at(2, 1);
    if (!uf.empty()) CHECK(gs->move_fleet(1, uf[0], 1) == commands::RejectReason::None);
    ai::AiDefinitions defs;
    ai::EquationSet eqs;
    TacticalAiSetup setup;
    setup.defs = &defs;
    setup.equations = &eqs;
    setup.files = d->files.get();
    setup.ai_type = [](int32_t, const galactic::GalacticSim&) { return std::string(); };
    namespace mv = tactical::movement;
    // Pairs of live movable battle units on one exact XY point.
    auto stacked = [](const TacticalBattle& b, const objects::ObjectStore& st) {
        std::vector<objects::ObjectId> ids;
        for (const objects::ObjectHandle h : st.all_objects())
            if (st.alive(h) && b.movement().has_unit(st.id(h))) ids.push_back(st.id(h));
        int pairs = 0;
        for (size_t i = 0; i < ids.size(); ++i)
            for (size_t j = i + 1; j < ids.size(); ++j) {
                const Vec3 p = st.position(st.find(ids[i])), q = st.position(st.find(ids[j]));
                if (p.x == q.x && p.y == q.y) ++pairs;
            }
        return pairs;
    };
    objects::ObjectId station = objects::kNoObjectId;
    Vec3 sp{};
    std::vector<objects::ObjectId> attackers;
    bool issued = false, station_died = false, hp_hit = false, hull_hit = false;
    float hp_max = 0.0f, hull_max = 0.0f;
    int stacks = 0;
    uint32_t last = 0;
    float closest = 1.0e9f;
    for (uint64_t t = 0; t < 6000; ++t) {
        host.tick_once();
        TacticalBattle* b = battles.battle();
        TacticalMode* tm = host.tactical();
        if (!b || !tm) {
            if (issued) break;
            continue;
        }
        objects::ObjectStore& st = tm->primary();
        if (b->frame() == 3 && !issued) {
            issued = true;
            for (const objects::ObjectHandle h : st.all_objects()) {
                if (!st.alive(h)) continue;
                if (st.owner(h) == 0 && static_cast<uint32_t>(st.type(h)) == type_of(*d, "S10_Star_Base_1")) station = st.id(h);
                if (st.owner(h) == 1 && b->movement().has_unit(st.id(h))) attackers.push_back(st.id(h));
            }
            CHECK(station != objects::kNoObjectId && attackers.size() >= 4);
            if (station == objects::kNoObjectId || attackers.size() < 2) break;
            sp = st.position(st.find(station));
            const mv::SpaceOccupancy& occ = b->movement().space();
            CHECK(occ.has_static(station));
            CHECK((occ.query_static({sp.x, sp.y}, {sp.x, sp.y}, 1.0f) & mv::kSpaceStatic) != 0);
            CHECK((occ.query_static({sp.x + 300.0f, sp.y}, {sp.x + 300.0f, sp.y}, 1.0f) & mv::kSpaceStatic) == 0);
            CHECK(stacked(*b, st) == 0);  // the start placement
            BattleAi ai(*b, *tm, host, setup);
            {  // as the tactical AI's issue_move (refixf t06): ONE group order for the task force, further units after the block
                std::vector<uint8_t> pay;
                auto put = [&pay](uint32_t v) {
                    for (int i = 0; i < 4; ++i) pay.push_back(static_cast<uint8_t>(v >> (8 * i)));
                };
                put(static_cast<uint32_t>(attackers.front()));
                put(static_cast<uint32_t>(station));
                put(std::bit_cast<uint32_t>(sp.x));
                put(std::bit_cast<uint32_t>(sp.y));
                put(std::bit_cast<uint32_t>(sp.z));
                put(0u);
                for (size_t i = 1; i < attackers.size(); ++i) put(static_cast<uint32_t>(attackers[i]));
                const auto sub = static_cast<uint16_t>(lua::kLuaOrderSubtype + static_cast<uint16_t>(lua::LuaOrder::AttackMove));
                CHECK(ai.execute(1, commands::ev::port_extension(sub, pay)) == commands::RejectReason::None);
            }
            std::vector<mv::V2> goals;
            for (const objects::ObjectId a : attackers) {
                const auto v = b->movement().view(a);
                CHECK(v.has_goal);
                const float gd = std::sqrt((v.goal.x - sp.x) * (v.goal.x - sp.x) + (v.goal.y - sp.y) * (v.goal.y - sp.y));
                CHECK(gd >= 250.0f);  // outside the station's footprint
                closest = std::min(closest, gd);
                for (const mv::V2& g : goals) CHECK(!(g.x == v.goal.x && g.y == v.goal.y));
                goals.push_back(v.goal);
            }
            std::printf("  station %d at (%.0f, %.0f): %zu attack-move orders, nearest goal %.0f from its centre\n", station,
                        static_cast<double>(sp.x), static_cast<double>(sp.y), attackers.size(), static_cast<double>(closest));
            // then an explicit attack on it (the weak station dies, the defender's frigates keep the battle going); refixf t06:
            // as the tactical AI's Attack_Target(station, "WEAPON_LASER"): ref2 = 1 + HardPointType, the laser is hardpoint 0
            std::vector<uint32_t> wire;
            for (const objects::ObjectId a : attackers) wire.push_back(static_cast<uint32_t>(a));
            commands::EventData atk = commands::ev::attack(static_cast<uint32_t>(station), wire);
            atk.set("ref2", 1u + static_cast<uint32_t>(tactical::combat::HardpointKind::WeaponLaser));
            CHECK(ai.execute(1, atk) == commands::RejectReason::None);
            for (const objects::ObjectId a : attackers)
                CHECK(b->combat().ordered_target(a) == station && b->combat().ordered_hardpoint(a) == 0);
            hp_max = b->combat().hardpoint_max(station, 0);
            hull_max = b->combat().health(station);
        }
        if (!issued || station == objects::kNoObjectId) continue;
        last = b->frame();
        stacks += stacked(*b, st);
        if (!station_died && !st.alive(st.find(station))) {
            station_died = true;
            std::printf("  station died at frame %u\n", b->frame());
        }
        if (!station_died) {  // refixf t06: the ordered attack damages the ordered hardpoint and the hull
            hp_hit = hp_hit || b->combat().hardpoint_health(station, 0) < hp_max;
            hull_hit = hull_hit || b->combat().health(station) < hull_max;
        }
        if (station_died) CHECK(!b->movement().space().has_static(station));  // removed with its object (same tick)
        if (station_died || b->frame() >= 4000) break;
    }
    std::printf("  stacked pairs over frames 3-%u: %d, station died %d, hardpoint hit %d, hull hit %d\n", last, stacks,
                station_died ? 1 : 0, hp_hit ? 1 : 0, hull_hit ? 1 : 0);
    CHECK(issued && stacks == 0);
    CHECK(station_died && hp_hit && hull_hit);
}

// S7 t29 / t30 (TACTICAL_COMBAT §2.10): the relation table makes the Is_Neutral player relation 2 (not enemy) both ways.
// The station goes to the Neutral player: a task-force Attack_Move on it is NOT refused - space keeps the object as the
// goal, so every member moves there, but nobody gets it as attack goal (path B needs relation 1); the explicit AI Attack
// and the human attack order are refused; a single unit's own Attack_Move (arg 0) is a plain move. Back with the Empire,
// the same task-force Attack_Move is accepted with the station as every member's attack goal.
void test_neutral_attack_move() {
    auto d = make_data(false, false, false, false, false, true);
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    SimHost host(d->types, js, cfg);
    galactic::GalacticSetup gsetup = make_setup(*d, false);
    galactic::GalacticSim* gs = host.start_galactic(d->rules, gsetup);
    CHECK(gs != nullptr);
    if (!gs) return;
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 6000;
    opt.battle.order_interval = 0;
    CampaignBattles battles(host, d->crules, *d->catalog,
                            [](const std::string& name, terrain::MapData& m) {
                                if (name != "S10_Space_B") return false;
                                m = space_map();
                                return true;
                            },
                            opt);
    CHECK(battles.attach(nullptr));
    const std::vector<uint32_t> uf = gs->fleets_at(2, 1);
    if (!uf.empty()) CHECK(gs->move_fleet(1, uf[0], 1) == commands::RejectReason::None);
    ai::AiDefinitions defs;
    ai::EquationSet eqs;
    TacticalAiSetup setup;
    setup.defs = &defs;
    setup.equations = &eqs;
    setup.files = d->files.get();
    setup.ai_type = [](int32_t, const galactic::GalacticSim&) { return std::string(); };
    using R = commands::RejectReason;
    bool done = false;
    for (uint64_t t = 0; t < 6000 && !done; ++t) {
        host.tick_once();
        TacticalBattle* b = battles.battle();
        TacticalMode* tm = host.tactical();
        if (!b || !tm || b->frame() != 3) continue;
        done = true;
        objects::ObjectStore& st = tm->primary();
        objects::ObjectId station = objects::kNoObjectId;
        std::vector<objects::ObjectId> attackers;
        for (const objects::ObjectHandle h : st.all_objects()) {
            if (!st.alive(h)) continue;
            if (st.owner(h) == 0 && static_cast<uint32_t>(st.type(h)) == type_of(*d, "S10_Star_Base_1")) station = st.id(h);
            if (st.owner(h) == 1 && b->movement().has_unit(st.id(h)) && b->combat().can_attack(st.id(h))) attackers.push_back(st.id(h));
        }
        CHECK(station != objects::kNoObjectId && attackers.size() >= 2);
        if (station == objects::kNoObjectId || attackers.size() < 2) break;
        const Vec3 sp = st.position(st.find(station));
        BattleAi ai(*b, *tm, host, setup);
        auto attack_move = [&](const std::vector<objects::ObjectId>& units, uint32_t arg) {
            std::vector<uint8_t> pay;
            auto put = [&pay](uint32_t v) {
                for (int i = 0; i < 4; ++i) pay.push_back(static_cast<uint8_t>(v >> (8 * i)));
            };
            put(static_cast<uint32_t>(units.front()));
            put(static_cast<uint32_t>(station));
            put(std::bit_cast<uint32_t>(sp.x));
            put(std::bit_cast<uint32_t>(sp.y));
            put(std::bit_cast<uint32_t>(sp.z));
            put(arg);
            for (size_t i = 1; i < units.size(); ++i) put(static_cast<uint32_t>(units[i]));
            const auto sub = static_cast<uint16_t>(lua::kLuaOrderSubtype + static_cast<uint16_t>(lua::LuaOrder::AttackMove));
            return ai.execute(1, commands::ev::port_extension(sub, pay));
        };
        auto no_goal = [&] {
            bool none = true;
            for (const objects::ObjectId a : attackers) none = none && b->combat().attack_goal(a) != station;
            return none;
        };
        std::vector<uint32_t> wire;
        for (const objects::ObjectId a : attackers) wire.push_back(static_cast<uint32_t>(a));
        CHECK(b->change_owner(*tm, st.find(station), 2) && st.owner(st.find(station)) == 2);
        const bool rel = !b->combat().is_enemy(1, 2) && !b->combat().is_enemy(2, 1) && b->combat().is_enemy(1, 0);
        const R tf_neutral = attack_move(attackers, ai::tactical::kTfAttackMoveIssueAttack);
        bool tf_moved = true;
        for (const objects::ObjectId a : attackers) {
            const auto va = b->movement().view(a);
            tf_moved = tf_moved && va.has_goal && std::abs(va.goal.x - sp.x) < 1500.0f && std::abs(va.goal.y - sp.y) < 1500.0f;
        }
        const R atk_neutral = ai.execute(1, commands::ev::attack(static_cast<uint32_t>(station), wire));
        const R human_neutral = b->order_attack(*tm, 1, attackers, station, host.random().synced());
        const bool goals_neutral = no_goal();
        const std::vector<objects::ObjectId> one = {attackers.front()};
        const R own_neutral = attack_move(one, 0u);
        const auto v = b->movement().view(attackers.front());
        const bool moved = v.has_goal && std::abs(v.goal.x - sp.x) < 1500.0f && std::abs(v.goal.y - sp.y) < 1500.0f;
        const bool goals_own = no_goal();
        CHECK(b->change_owner(*tm, st.find(station), 0) && st.owner(st.find(station)) == 0);
        const R tf_enemy = attack_move(attackers, ai::tactical::kTfAttackMoveIssueAttack);
        bool goals_enemy = true;
        for (const objects::ObjectId a : attackers) goals_enemy = goals_enemy && b->combat().attack_goal(a) == station;
        std::printf("  neutral station: relation 2 %d; TF Attack_Move %d (moved %d), Attack %d, human attack %d, goals none %d; own "
                    "Attack_Move %d (moved %d, goals none %d); enemy again: TF Attack_Move %d, goals %d\n",
                    rel ? 1 : 0, static_cast<int>(tf_neutral), tf_moved ? 1 : 0, static_cast<int>(atk_neutral),
                    static_cast<int>(human_neutral), goals_neutral ? 1 : 0, static_cast<int>(own_neutral), moved ? 1 : 0,
                    goals_own ? 1 : 0, static_cast<int>(tf_enemy), goals_enemy ? 1 : 0);
        CHECK(rel);
        CHECK(tf_neutral == R::None && tf_moved && atk_neutral == R::InvalidTarget && human_neutral == R::InvalidTarget && goals_neutral);
        CHECK(own_neutral == R::None && moved && goals_own);
        CHECK(tf_enemy == R::None && goals_enemy);
    }
    CHECK(done);
}

// t41: BattleAi::fill hands the perception the station's DUMMY_STAR_BASE and Base_Level (the inputs of Variable_Self.BaseLevel).
void test_battle_ai_station_level(bool skirmish) {
    auto d = make_data(false, false, false, false, true);  // the star bases carry DUMMY_STAR_BASE here
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    SimHost host(d->types, js, cfg);
    galactic::GalacticSim* gs = host.start_galactic(d->rules, make_setup(*d, false));
    CHECK(gs != nullptr);
    if (!gs) return;
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 600;
    opt.battle.skirmish = skirmish;
    CampaignBattles battles(host, d->crules, *d->catalog,
                            [](const std::string& name, terrain::MapData& m) {
                                if (name != "S10_Space_B") return false;
                                m = space_map();
                                return true;
                            },
                            opt);
    CHECK(battles.attach(nullptr));
    const std::vector<uint32_t> uf = gs->fleets_at(2, 1);
    if (!uf.empty()) CHECK(gs->move_fleet(1, uf[0], 1) == commands::RejectReason::None);
    ai::AiDefinitions defs;
    ai::EquationSet eqs;
    TacticalAiSetup setup;
    setup.defs = &defs;
    setup.equations = &eqs;
    setup.files = d->files.get();
    setup.ai_type = [](int32_t, const galactic::GalacticSim&) { return std::string(); };
    bool checked = false;
    for (uint64_t t = 0; t < 2500 && !checked; ++t) {
        host.tick_once();
        TacticalBattle* b = battles.battle();
        TacticalMode* tm = host.tactical();
        if (!b || !tm || b->frame() < 3) continue;
        BattleAi ai(*b, *tm, host, setup);
        ai::tactical::PerceptionWorld w;
        std::vector<ai::tactical::TacticalAiObject> facts;
        ai.fill(w, facts);
        int stations = 0, others = 0;
        for (const ai::tactical::PerceptionObject& o : w.objects) {
            if (o.dummy_star_base) {
                ++stations;
                CHECK(o.owner == 0 && o.starbase_level == 1 && o.type == static_cast<int32_t>(type_of(*d, "S10_Star_Base_1")));
            } else if (o.starbase_level != 0) {
                ++others;
            }
        }
        CHECK(stations == 1 && others == 0);
        CHECK(w.campaign == !skirmish);  // t48: space skirmish runs the skirmish rules, its markers are the battle anchors
        if (skirmish) {
            CHECK(w.markers.size() == 2);
            for (const ai::tactical::StartMarker& m : w.markers) CHECK(m.kind == ai::tactical::StartMarker::Kind::PlayerPosition);
        }
        checked = true;
    }
    CHECK(checked);
}

// t48: the battle mode reaches the perception (Game.IsCampaignGame, IsDefender, the start markers) and so do both land BaseLevel
// inputs: campaign = the conflict planet's structures, skirmish = each player's DUMMY_GROUND_STRUCTURE objects.
void test_battle_ai_land_mode(bool skirmish) {
    auto d = make_data(false, false, true);
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    SimHost host(d->types, js, cfg);
    galactic::GalacticSetup setup = make_setup(*d, true);
    setup.forces.push_back({0, 1, type_of(*d, "S4_Barracks")});
    galactic::GalacticSim* gs = host.start_galactic(d->rules, setup);
    CHECK(gs != nullptr);
    if (!gs) return;
    CHECK(gs->planets()[1].structures.size() == 1);
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 600;
    opt.battle.skirmish = skirmish;
    CampaignBattles battles(host, d->crules, *d->catalog,
                            [](const std::string& name, terrain::MapData& m) {
                                if (name != "S10_Land_B") return false;
                                m = land_map();
                                return true;
                            },
                            opt);
    CHECK(battles.attach(nullptr));
    std::vector<objects::ObjectHandle> up;
    for (objects::ObjectHandle h : gs->planets()[2].surface)
        if (gs->store().owner(h) == 1) up.push_back(h);
    CHECK(gs->launch_units(1, 2, up) == commands::RejectReason::None);
    const std::vector<uint32_t> uf = gs->fleets_at(2, 1);
    if (!uf.empty()) CHECK(gs->move_fleet(1, uf[0], 1) == commands::RejectReason::None);
    ai::AiDefinitions defs;
    ai::EquationSet eqs;
    TacticalAiSetup ts;
    ts.defs = &defs;
    ts.equations = &eqs;
    ts.files = d->files.get();
    ts.ai_type = [](int32_t, const galactic::GalacticSim&) { return std::string(); };
    bool invaded = false, checked = false;
    for (uint64_t t = 0; t < 2500 && !checked; ++t) {
        host.tick_once();
        if (!invaded && host.modes().depth() == 1) {
            const std::vector<uint32_t> f = gs->fleets_at(1, 1);
            if (!f.empty() && !gs->fleets()[f[0]].moving) invaded = gs->invade(1, 1, {}) == commands::RejectReason::None;
        }
        TacticalBattle* b = battles.battle();
        TacticalMode* tm = host.tactical();
        if (!b || !tm || b->frame() < 3) continue;
        BattleAi ai(*b, *tm, host, ts);
        ai::tactical::PerceptionWorld w;
        std::vector<ai::tactical::TacticalAiObject> facts;
        ai.fill(w, facts);
        CHECK(w.campaign == !skirmish);  // t50: a land skirmish runs the skirmish rules too (it has its skirmish base now)
        CHECK(w.conflict_ground_structures == 1);  // set in both modes; the token picks by mode
        int own = 0, other = 0;
        for (const ai::tactical::PerceptionObject& o : w.objects)
            if (o.dummy_ground_structure) ++(o.owner == 0 ? own : other);
        CHECK(own == 1 && other == 0);
        if (skirmish) {  // the battle anchors stand for Player_%d_Position_Marker
            CHECK(w.markers.size() == 2);
            for (const ai::tactical::StartMarker& m : w.markers) CHECK(m.kind == ai::tactical::StartMarker::Kind::PlayerPosition);
        } else {
            for (const ai::tactical::StartMarker& m : w.markers) CHECK(m.kind != ai::tactical::StartMarker::Kind::PlayerPosition);
        }
        checked = true;
    }
    CHECK(checked);
}

// refixf t03: Find_All_Objects_Of_Type matches the representative type (LUA_API.md "Find_All_Objects_Of_Type and the
// sibling finders"): in a land battle (team_types) a script's 'S10_Squad' finds the squads' TEAM containers (store type
// "Team") through the battle's ObjectBridge::representative_type hook, and 'Team' finds none. Without the hook the store
// type is matched instead.
void test_battle_ai_find_team() {
    auto d = make_data(false, false, false, false, false, false, true);
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    SimHost host(d->types, js, cfg);
    galactic::GalacticSim* gs = host.start_galactic(d->rules, make_setup(*d, true));
    CHECK(gs != nullptr);
    if (!gs) return;
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 600;
    CampaignBattles battles(host, d->crules, *d->catalog,
                            [](const std::string& name, terrain::MapData& m) {
                                if (name != "S10_Land_B") return false;
                                m = land_map();
                                return true;
                            },
                            opt);
    CHECK(battles.attach(nullptr));
    std::vector<objects::ObjectHandle> up;
    for (objects::ObjectHandle h : gs->planets()[2].surface)
        if (gs->store().owner(h) == 1) up.push_back(h);
    CHECK(gs->launch_units(1, 2, up) == commands::RejectReason::None);
    const std::vector<uint32_t> uf = gs->fleets_at(2, 1);
    if (!uf.empty()) CHECK(gs->move_fleet(1, uf[0], 1) == commands::RejectReason::None);
    ai::AiDefinitions defs;
    ai::EquationSet eqs;
    TacticalAiSetup ts;
    ts.defs = &defs;
    ts.equations = &eqs;
    ts.files = d->files.get();
    ts.ai_type = [](int32_t, const galactic::GalacticSim&) { return std::string(); };
    const objects::TypeIndex team = d->types.resolve_name("Team"), squad = d->types.resolve_name("S10_Squad"),
                             trooper = d->types.resolve_name("S10_Trooper");
    CHECK(team != objects::kNone && squad != objects::kNone && trooper != objects::kNone);
    bool invaded = false, checked = false;
    for (uint64_t t = 0; t < 2500 && !checked; ++t) {
        host.tick_once();
        if (!invaded && host.modes().depth() == 1) {
            const std::vector<uint32_t> f = gs->fleets_at(1, 1);
            if (!f.empty() && !gs->fleets()[f[0]].moving) invaded = gs->invade(1, 1, {}) == commands::RejectReason::None;
        }
        TacticalBattle* b = battles.battle();
        TacticalMode* tm = host.tactical();
        if (!b || !tm || b->frame() < 3) continue;
        objects::ObjectStore& st = tm->primary();
        std::vector<objects::ObjectHandle> containers;
        size_t troopers = 0;
        for (const objects::ObjectHandle h : st.all_objects()) {
            if (!st.alive(h)) continue;
            if (st.type(h) == team) containers.push_back(h);
            if (st.type(h) == trooper) ++troopers;
        }
        BattleAi ai(*b, *tm, host, ts);
        lua::ObjectBridge& br = ai.world().object_bridge();
        CHECK(static_cast<bool>(br.representative_type));
        if (!br.representative_type) break;
        for (const objects::ObjectHandle h : containers) CHECK(br.representative_type(h) == squad);
        CHECK(br.representative_type(objects::ObjectHandle{}) == objects::kNone);
        lua::LuaHost lh;
        lh.store = &st;
        lh.types = &d->types;
        lua::LuaVm vm(lh);
        lua::attach_object_bridge(vm, &br);
        // The count goes back through the chunk's success: error() when the finder's result has another size.
        auto finds = [&vm](const char* filter, size_t n) {
            std::string err;
            const std::string code = std::string("local n = 0 for _ in pairs(Find_All_Objects_Of_Type('") + filter +
                                     "')) do n = n + 1 end if n ~= " + std::to_string(n) + " then error('found ' .. n) end";
            const bool ok = vm.run(code, "find_team", &err);
            if (!ok) std::printf("  find team: '%s' (expected %zu): %s\n", filter, n, err.c_str());
            return ok;
        };
        std::printf("  find team: frame %u, %zu TEAM containers, %zu troopers\n", b->frame(), containers.size(), troopers);
        CHECK(!containers.empty() && troopers > 0);
        CHECK(finds("S10_Squad", containers.size()) && finds("Team", 0) && finds("S10_Trooper", troopers));
        const auto hook = br.representative_type;  // without the hook: the store type
        br.representative_type = nullptr;
        CHECK(finds("S10_Squad", 0) && finds("Team", containers.size()));
        br.representative_type = hook;
        lua::attach_object_bridge(vm, nullptr);
        checked = true;
    }
    CHECK(checked);
}

// s78f t07 (TACTICAL_COMBAT §3.1.1 "Team targeting copies"): land team members join combat with their team (the TEAM
// container's object id), so the TeamTargeting copy runs in a real battle: a member keeps no target of its own and reads
// its team head's (the lowest-id live member standing in for the container), and every non-head member holds the same
// copied targeting state.
void test_land_team_targeting() {
    auto d = make_data(false, false, false, false, false, false, true);
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    SimHost host(d->types, js, cfg);
    galactic::GalacticSim* gs = host.start_galactic(d->rules, make_setup(*d, true));
    CHECK(gs != nullptr);
    if (!gs) return;
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 3000;
    CampaignBattles battles(host, d->crules, *d->catalog,
                            [](const std::string& name, terrain::MapData& m) {
                                if (name != "S10_Land_B") return false;
                                m = land_map();
                                return true;
                            },
                            opt);
    CHECK(battles.attach(nullptr));
    std::vector<objects::ObjectHandle> up;
    for (objects::ObjectHandle h : gs->planets()[2].surface)
        if (gs->store().owner(h) == 1) up.push_back(h);
    CHECK(gs->launch_units(1, 2, up) == commands::RejectReason::None);
    const std::vector<uint32_t> uf = gs->fleets_at(2, 1);
    if (!uf.empty()) CHECK(gs->move_fleet(1, uf[0], 1) == commands::RejectReason::None);
    const objects::TypeIndex team = d->types.resolve_name("Team"), trooper = d->types.resolve_name("S10_Trooper");
    CHECK(team != objects::kNone && trooper != objects::kNone);
    bool invaded = false, registered = false;
    uint32_t copies = 0;  // ticks on which a team of >= 3 live members attacked through the copy
    for (uint64_t t = 0; t < 6000 && copies < 5; ++t) {
        host.tick_once();
        if (!invaded && host.modes().depth() == 1) {
            const std::vector<uint32_t> f = gs->fleets_at(1, 1);
            if (!f.empty() && !gs->fleets()[f[0]].moving) invaded = gs->invade(1, 1, {}) == commands::RejectReason::None;
        }
        TacticalBattle* b = battles.battle();
        TacticalMode* tm = host.tactical();
        if (!b || !tm || b->frame() < 3) continue;
        objects::ObjectStore& st = tm->primary();
        const tactical::combat::CombatSystem& cs = b->combat();
        std::vector<objects::ObjectId> teams;
        for (const objects::ObjectHandle h : st.all_objects())
            if (st.alive(h) && st.type(h) == team) teams.push_back(st.id(h));
        std::sort(teams.begin(), teams.end());
        std::vector<std::vector<objects::ObjectId>> members(teams.size());  // per team, ascending id (combat order)
        for (size_t i = 0; i < cs.unit_count(); ++i) {
            const auto id = cs.unit_id_at(i);
            const uint32_t tid = cs.team(id);
            if (tid == 0) continue;
            const auto oid = static_cast<objects::ObjectId>(tid);
            const auto it = std::lower_bound(teams.begin(), teams.end(), oid);
            CHECK(it != teams.end() && *it == oid);  // the team id is a live TEAM container
            if (it == teams.end() || *it != oid) continue;
            const objects::ObjectHandle mh = st.find(id);
            CHECK(st.alive(mh) && st.type(mh) == trooper);
            if (cs.alive(id)) members[static_cast<size_t>(it - teams.begin())].push_back(id);
        }
        for (const auto& m : members) {
            if (m.empty()) continue;
            registered = true;
            const auto head = cs.current_target(m.front());
            for (size_t k = 1; k < m.size(); ++k) {
                CHECK(cs.current_target(m[k]) == head);  // the head's target, none of its own
                CHECK(cs.targeting_state(m[k]) == cs.targeting_state(m[1]));  // the raw state copy
            }
            if (m.size() >= 3 && head != tactical::combat::kNoObject && cs.targeting_state(m[1]) == 4) ++copies;
        }
    }
    std::printf("  land team targeting: registered %d, attacking copy ticks %u\n", registered ? 1 : 0, copies);
    CHECK(registered);
    CHECK(copies >= 5);
}

// s78j t03 / t04: a land team's SPEED and Change_Owner in a battle (sprint_types: TEAM containers whose troopers carry
// SPRINT, SPEED x 1.5).
//  * t03 (TACTICAL_COMBAT §4.4, MOVEMENT §3.4 item 4; s78k t03): the team locomotor runs at the container's speed - the
//    minimum member Max_Speed x the container's SPEED value (CombatSystem::team_mod_multiplier) - while every member
//    keeps its own product: one sprinting member (own factor 1.5) leaves the team cap at 1.0 x Max_Speed (the others
//    cap the minimum) and walks no faster than that; every member sprinting raises the cap to 1.5 x.
//  * t04 (TACTICAL_COMBAT §3.5 step 4): the story / Lua Change_Owner of one member (CampaignBattles::change_owner ->
//    TacticalBattle::script_change_owner) moves the container and every other live member too (store and combat owner);
//    another team is untouched. With the tactical AI build, the AI VMs' bridge (BattleAi) spreads the same way.
void test_team_speed_and_owner() {
    auto d = make_data(false, false, false, false, false, false, false, false, false, false, false, false, true);
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    SimHost host(d->types, js, cfg);
    galactic::GalacticSim* gs = host.start_galactic(d->rules, make_setup(*d, true));
    CHECK(gs != nullptr);
    if (!gs) return;
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 3000;
    CampaignBattles battles(host, d->crules, *d->catalog,
                            [](const std::string& name, terrain::MapData& m) {
                                if (name != "S10_Land_B") return false;
                                m = land_map();
                                return true;
                            },
                            opt);
    CHECK(battles.attach(nullptr));
    std::vector<objects::ObjectHandle> up;
    for (objects::ObjectHandle h : gs->planets()[2].surface)
        if (gs->store().owner(h) == 1) up.push_back(h);
    CHECK(gs->launch_units(1, 2, up) == commands::RejectReason::None);
    const std::vector<uint32_t> uf = gs->fleets_at(2, 1);
    if (!uf.empty()) CHECK(gs->move_fleet(1, uf[0], 1) == commands::RejectReason::None);
    const objects::TypeIndex team_t = d->types.resolve_name("Team");
    const uint8_t sprint = tactical::abilities::ability_type_from_name("SPRINT");
    CHECK(team_t != objects::kNone);
    namespace tc = alamo::tactical::combat;
    bool invaded = false, done = false;
    int stage = 0, stage_ticks = 0;
    uint32_t stage_frame = 0;
    objects::ObjectId team_id = objects::kNoObjectId;
    std::vector<objects::ObjectId> members;
    // The live members (ascending id) of every team in combat, by team id.
    auto teams_now = [](const tc::CombatSystem& cs) {
        std::map<uint32_t, std::vector<objects::ObjectId>> out;
        for (size_t i = 0; i < cs.unit_count(); ++i) {
            const auto id = cs.unit_id_at(i);
            if (cs.team(id) != 0 && cs.alive(id)) out[cs.team(id)].push_back(id);
        }
        return out;
    };
    for (uint64_t t = 0; t < 6000 && !done; ++t) {
        host.tick_once();
        if (!invaded && host.modes().depth() == 1) {
            const std::vector<uint32_t> f = gs->fleets_at(1, 1);
            if (!f.empty() && !gs->fleets()[f[0]].moving) invaded = gs->invade(1, 1, {}) == commands::RejectReason::None;
        }
        TacticalBattle* b = battles.battle();
        TacticalMode* tm = host.tactical();
        if (!b || !tm || b->frame() < 3) continue;
        objects::ObjectStore& st = tm->primary();
        const tc::CombatSystem& cs = b->combat();
        const auto& ms = b->movement();
        if (stage == 0) {  // pick a team of >= 2 live members; SPRINT on its lowest-id member only
            for (const auto& [tid, m] : teams_now(cs))
                if (m.size() >= 2 && team_id == objects::kNoObjectId) {
                    team_id = static_cast<objects::ObjectId>(tid);
                    members = m;
                }
            CHECK(team_id != objects::kNoObjectId);
            if (team_id == objects::kNoObjectId) break;
            CHECK(st.alive(st.find(team_id)) && st.type(st.find(team_id)) == team_t);
            CHECK(b->abilities().set_unit_ability_active(members[0], sprint, true, b->frame()));
            stage = 1;
            stage_frame = b->frame();
            continue;
        }
        if (b->frame() < stage_frame + 2) continue;  // the speed factors read last tick's Mod_Multiplier (phase A)
        const float tm_speed = cs.team_mod_multiplier(static_cast<uint32_t>(team_id), tc::ModKind::Speed);
        bool all_alive = true;
        for (const auto id : members) all_alive = all_alive && cs.alive(id);
        if (!all_alive) {
            std::printf("  team speed: a member died before stage %d, skipped\n", stage);
            break;
        }
        if (stage == 1) {
            CHECK(b->abilities().is_active(members[0], sprint) && !b->abilities().is_active(members[1], sprint));
            CHECK(tm_speed == 1.0f);  // the non-sprinting members cap the team
            CHECK(ms.speed_factor(members[0]) == 1.5f);  // s78k t03: the sprinter keeps its own product
            for (size_t k = 1; k < members.size(); ++k) CHECK(ms.speed_factor(members[k]) == 1.0f);
            // The team cap = min Max_Speed x mult x 1.0 = a plain member's max speed, below the sprinter's own.
            const float cap = ms.view(members[1]).max_speed;
            for (const auto id : members) CHECK(ms.view(id).team_speed == cap && ms.view(id).speed <= cap);
            CHECK(ms.view(members[0]).max_speed > cap);
            if (++stage_ticks < 20) continue;  // watch the sprinter stay within the cap for a while
            for (size_t k = 1; k < members.size(); ++k) CHECK(b->abilities().set_unit_ability_active(members[k], sprint, true, b->frame()));
            stage = 2;
            stage_frame = b->frame();
            continue;
        }
        CHECK(tm_speed == 1.5f);
        for (const auto id : members) CHECK(ms.speed_factor(id) == 1.5f && ms.view(id).team_speed == ms.view(id).max_speed);
        std::printf("  team speed: team %d of %zu members: all sprinting, team cap %.3f = member max speed\n", static_cast<int>(team_id),
                    members.size(), static_cast<double>(ms.view(members[0]).team_speed));
        // t04: Change_Owner of the team's last member through the story bridge.
        const int32_t old_owner = st.owner(st.find(team_id));
        const int32_t new_owner = old_owner == 0 ? 1 : 0;
        std::vector<std::pair<objects::ObjectId, int32_t>> others;  // every other team's members and their owners
        for (const auto& [tid, m] : teams_now(cs))
            if (static_cast<objects::ObjectId>(tid) != team_id)
                for (const auto id : m) others.emplace_back(id, st.owner(st.find(id)));
        CHECK(!others.empty());
        CHECK(battles.change_owner(st.find(members.back()), new_owner));
        CHECK(st.owner(st.find(team_id)) == new_owner);  // the container
        for (const auto id : members) CHECK(st.owner(st.find(id)) == new_owner && cs.owner(id) == new_owner);
        for (const auto& [id, o] : others) CHECK(st.owner(st.find(id)) == o && cs.owner(id) == o);
        // A Change_Owner of the container itself moves every member back.
        CHECK(battles.change_owner(st.find(team_id), old_owner));
        CHECK(st.owner(st.find(team_id)) == old_owner);
        for (const auto id : members) CHECK(st.owner(st.find(id)) == old_owner && cs.owner(id) == old_owner);
#ifdef ALAMO_SIM_HAVE_TACTICAL_AI
        // The AI VMs' Change_Owner (BattleAi object bridge) on another team's lowest-id member.
        if (!others.empty()) {
            ai::AiDefinitions defs;
            ai::EquationSet eqs;
            TacticalAiSetup ts;
            ts.defs = &defs;
            ts.equations = &eqs;
            ts.files = d->files.get();
            ts.ai_type = [](int32_t, const galactic::GalacticSim&) { return std::string(); };
            BattleAi ai(*b, *tm, host, ts);
            lua::ObjectBridge& br = ai.world().object_bridge();
            CHECK(static_cast<bool>(br.change_owner));
            if (br.change_owner) {
                const objects::ObjectId victim = others.front().first;
                const auto oteam = static_cast<objects::ObjectId>(cs.team(victim));
                const int32_t to = others.front().second == 0 ? 1 : 0;
                CHECK(br.change_owner(st.find(victim), to));
                CHECK(st.owner(st.find(oteam)) == to);
                for (const auto& [id, o] : others) {
                    const bool same = static_cast<objects::ObjectId>(cs.team(id)) == oteam;
                    CHECK(st.owner(st.find(id)) == (same ? to : o) && cs.owner(id) == (same ? to : o));
                }
                for (const auto id : members) CHECK(st.owner(st.find(id)) == old_owner);
            }
        }
#endif
        done = true;
    }
    CHECK(done);
}

// s78k t04 (TACTICAL_COMBAT §3.5 "Convert and its team rule" steps 3-5): a converting blaster (convert_types) turns
// enemy team members in a land battle. CombatSystem converts the victim's whole team (combat owner, one
// UnitEffect::Converted each); the host follows each event (store owner) and its TEAM container - the store owner and the
// container record (TacticalBattle::container_owner). At every tick end each live team has one owner: its members (combat
// and store), its container object and its container record. At least one team must change owner.
void test_team_convert() {
    auto d = make_data(false, false, false, false, false, false, false, false, false, false, false, false, false, true);
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    SimHost host(d->types, js, cfg);
    galactic::GalacticSim* gs = host.start_galactic(d->rules, make_setup(*d, true));
    CHECK(gs != nullptr);
    if (!gs) return;
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 3000;
    CampaignBattles battles(host, d->crules, *d->catalog,
                            [](const std::string& name, terrain::MapData& m) {
                                if (name != "S10_Land_B") return false;
                                m = land_map();
                                return true;
                            },
                            opt);
    CHECK(battles.attach(nullptr));
    std::vector<objects::ObjectHandle> up;
    for (objects::ObjectHandle h : gs->planets()[2].surface)
        if (gs->store().owner(h) == 1) up.push_back(h);
    CHECK(gs->launch_units(1, 2, up) == commands::RejectReason::None);
    const std::vector<uint32_t> uf = gs->fleets_at(2, 1);
    if (!uf.empty()) CHECK(gs->move_fleet(1, uf[0], 1) == commands::RejectReason::None);
    namespace tc = alamo::tactical::combat;
    bool invaded = false;
    uint32_t ticks = 0, team_converts = 0, member_converts = 0, mismatches = 0;
    std::map<uint32_t, int32_t> team_owner;           // last seen container owner per team
    std::map<objects::ObjectId, int32_t> unit_owner;  // last seen owner per team member
    for (uint64_t t = 0; t < 6000; ++t) {
        host.tick_once();
        if (!invaded && host.modes().depth() == 1) {
            const std::vector<uint32_t> f = gs->fleets_at(1, 1);
            if (!f.empty() && !gs->fleets()[f[0]].moving) invaded = gs->invade(1, 1, {}) == commands::RejectReason::None;
        }
        const TacticalBattle* b = battles.battle();
        TacticalMode* tm = host.tactical();
        if (!b || !tm) continue;
        ++ticks;
        const objects::ObjectStore& st = tm->primary();
        const tc::CombatSystem& cs = b->combat();
        std::map<uint32_t, std::vector<objects::ObjectId>> teams;
        for (size_t i = 0; i < cs.unit_count(); ++i) {
            const auto id = cs.unit_id_at(i);
            if (cs.team(id) != 0 && cs.alive(id)) teams[cs.team(id)].push_back(id);
        }
        for (const auto& [tid, m] : teams) {
            const auto cid = static_cast<objects::ObjectId>(tid);
            const objects::ObjectHandle ch = st.find(cid);
            if (!st.alive(ch)) continue;
            const int32_t o = cs.owner(m.front());
            bool same = st.owner(ch) == o && b->container_owner(cid) == o;
            for (const auto id : m) {
                same = same && cs.owner(id) == o && st.owner(st.find(id)) == o;
                const auto [it, fresh] = unit_owner.emplace(id, o);
                if (!fresh && it->second != o) {
                    ++member_converts;
                    it->second = o;
                }
            }
            if (!same && mismatches++ < 3)
                std::printf("  team convert: frame %u team %u owner %d: container store %d record %d\n", b->frame(), tid, o,
                            st.owner(ch), b->container_owner(cid));
            const auto [it, fresh] = team_owner.emplace(tid, st.owner(ch));
            if (!fresh && it->second != st.owner(ch)) {
                ++team_converts;
                it->second = st.owner(ch);
            }
        }
    }
    std::printf("  team convert: %u battle ticks, %zu teams, %u container owner changes, %u member owner changes, %u mismatches\n",
                ticks, team_owner.size(), team_converts, member_converts, mismatches);
    CHECK(ticks > 0 && !team_owner.empty());
    CHECK(team_converts > 0);
    CHECK(mismatches == 0);
}

// s78j t04 (TACTICAL_COMBAT §3.1.1 "other spawns"): a land reserve drop without a landing object (S10_Squad has no
// Company_Transport_Unit here) spawns its members - and their TEAM containers - with the owner's entry-facing record
// (TacticalBattle::player_drop_facing, the reinforce_facing hook of the Lua bridges), not the side's start facing.
// s78k t05 (attacker_marker = false): without an Attacker Entry Position the attacker's record stays unset (yaw 0, +x)
// while its start forces face the defender's anchor (the [port] rule for a side without its marker), so the drop's
// facing differs from the side's start facing and the test tells the two apart.
void test_reserve_drop_facing(bool attacker_marker) {
    auto d = make_data(false, false, false, false, false, false, false, false, false, false, false, false, true);
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    SimHost host(d->types, js, cfg);
    galactic::GalacticSim* gs = host.start_galactic(d->rules, make_setup(*d, true));
    CHECK(gs != nullptr);
    if (!gs) return;
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 600;
    CampaignBattles battles(host, d->crules, *d->catalog,
                            [attacker_marker](const std::string& name, terrain::MapData& m) {
                                if (name != "S10_Land_B") return false;
                                m = land_map();
                                // Entry markers (yaw 180 = facing +y): the attacker has start units, so its entry-facing
                                // record is the marker's (TACTICAL_GAPS §4.2.2 item 1), not the yaw-0 default.
                                auto place = [&m](const char* type, float x, float y, float yaw) {
                                    terrain::ObjectPlacement p;
                                    p.type_crc = name_crc(type);
                                    p.owner = 2;
                                    p.position[0] = x;
                                    p.position[1] = y;
                                    p.rotation_deg[2] = yaw;
                                    m.objects.push_back(p);
                                };
                                place("Defending Forces Position", 900.0f, 900.0f, 0.0f);
                                if (attacker_marker) place("Attacker Entry Position", 300.0f, 300.0f, 180.0f);
                                return true;
                            },
                            opt);
    CHECK(battles.attach(nullptr));
    std::vector<objects::ObjectHandle> up;
    for (objects::ObjectHandle h : gs->planets()[2].surface)
        if (gs->store().owner(h) == 1) up.push_back(h);
    CHECK(gs->launch_units(1, 2, up) == commands::RejectReason::None);
    const std::vector<uint32_t> uf = gs->fleets_at(2, 1);
    if (!uf.empty()) CHECK(gs->move_fleet(1, uf[0], 1) == commands::RejectReason::None);
    namespace mv = alamo::tactical::movement;
    const objects::TypeIndex trooper = d->types.resolve_name("S10_Trooper"), team_t = d->types.resolve_name("Team");
    bool invaded = false, dropped = false, ran = false;
    for (uint64_t t = 0; t < 1500 && !ran; ++t) {
        host.tick_once();
        if (!invaded && host.modes().depth() == 1) {
            const std::vector<uint32_t> f = gs->fleets_at(1, 1);
            if (!f.empty() && !gs->fleets()[f[0]].moving) invaded = gs->invade(1, 1, {}) == commands::RejectReason::None;
        }
        TacticalBattle* b = battles.battle();
        TacticalMode* tm = host.tactical();
        if (!b || !tm || b->frame() != 1) continue;
        ran = true;
        objects::ObjectStore& st = tm->primary();
        const auto reserves = b->reserves(1);
        CHECK(reserves.size() == 1);
        if (reserves.empty()) break;
        const objects::ObjectId key = reserves[0].galactic_id;
#ifdef ALAMO_SIM_HAVE_FOG
        if (b->fog()) b->fog()->reveal_all(1);  // the drop test needs unfogged cells
#endif
        // The side's start facing (player 1's troopers already on the map), printed for reference.
        std::vector<objects::ObjectId> before;
        Vec3 start_face{};
        for (const objects::ObjectHandle h : st.all_objects())
            if (st.alive(h)) {
                before.push_back(st.id(h));
                if (st.owner(h) == 1 && st.type(h) == trooper) start_face = st.facing(h);
            }
        std::sort(before.begin(), before.end());
        const uint32_t spawned_before = b->stats().spawned;
        for (int32_t cy = 6; cy < 58 && !dropped; cy += 2)
            for (int32_t cx = 6; cx < 58 && !dropped; cx += 2) {
                const mv::V2 c = mv::cell_center(cx, cy);
                if (!b->land_drop_valid(*tm, 1, key, c.x, c.y, false, false)) continue;
                if (b->order_reinforce(*tm, 1, key, c.x, c.y, host.random().synced()) != commands::RejectReason::None) continue;
                dropped = true;
            }
        CHECK(dropped);
        if (!dropped) break;
        CHECK(b->landings_unloaded().empty() && b->landings_in_flight(1) == 0);  // no landing object
        CHECK(b->stats().spawned > spawned_before);
        const Vec3 df = b->player_drop_facing(1);
        size_t new_troopers = 0, new_teams = 0;
        for (const objects::ObjectHandle h : st.all_objects()) {
            if (!st.alive(h) || std::binary_search(before.begin(), before.end(), st.id(h))) continue;
            if (st.type(h) != trooper && st.type(h) != team_t) continue;
            const Vec3 f = st.facing(h);
            CHECK(f.x == df.x && f.y == df.y && f.z == df.z);
            if (st.type(h) == trooper) ++new_troopers;
            else ++new_teams;
        }
        std::printf("  reserve drop facing: %zu troopers, %zu teams at (%.3f, %.3f); side start facing (%.3f, %.3f)\n", new_troopers,
                    new_teams, static_cast<double>(df.x), static_cast<double>(df.y), static_cast<double>(start_face.x),
                    static_cast<double>(start_face.y));
        CHECK(new_troopers == reserves[0].units && new_teams > 0);
        if (attacker_marker) {
            CHECK(std::fabs(df.x) < 1e-5f && std::fabs(df.y - 1.0f) < 1e-5f);  // the marker's facing, not the yaw-0 default
        } else {
            CHECK(df.x == 1.0f && std::fabs(df.y) < 1e-6f);  // no record: yaw 0
            // The side's start facing points at the defender's anchor (900, 900), well off +x.
            CHECK(std::fabs(start_face.y) > 0.2f && std::fabs(start_face.x - df.x) + std::fabs(start_face.y - df.y) > 0.2f);
        }
    }
    CHECK(dropped);
}

// a1g t04 follow-up: BattleAi::land_drop_ok (TACTICAL_GAPS §4.2): the land Reinforce drop validity of a reserve entry. A spot
// is refused when a slot is impassable (an Obstacle strip on the map), fogged for the player (force 1), or its footprint is
// taken (a live unit stands there); an unknown entry is refused; otherwise accepted.
// a1h t06: the test is run by the host (TacticalBattle::land_drop_valid); blockers are the Collidable_By_Projectile_Living
// objects and S10_Trooper carries hard extents 6 x 6 (a type with neither extents nor a model has a zero drop cell: refused).
void test_battle_ai_land_drop(const Data& d) {
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    SimHost host(d.types, js, cfg);
    galactic::GalacticSim* gs = host.start_galactic(d.rules, make_setup(d, true));
    CHECK(gs != nullptr);
    if (!gs) return;
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 600;
    constexpr int32_t kStrip = 5;  // cells x < 5: Obstacle
    CampaignBattles battles(host, d.crules, *d.catalog,
                            [](const std::string& name, terrain::MapData& m) {
                                if (name != "S10_Land_B") return false;
                                m = land_map();
                                m.legacy_passability.assign(63u * 63u, 0);
                                for (uint32_t x = 0; x < 5u; ++x)  // kStrip
                                    for (uint32_t y = 0; y < 63u; ++y) m.legacy_passability[x * 63u + y] = 2;
                                return true;
                            },
                            opt);
    CHECK(battles.attach(nullptr));
    std::vector<objects::ObjectHandle> up;
    for (objects::ObjectHandle h : gs->planets()[2].surface)
        if (gs->store().owner(h) == 1) up.push_back(h);
    CHECK(gs->launch_units(1, 2, up) == commands::RejectReason::None);
    const std::vector<uint32_t> uf = gs->fleets_at(2, 1);
    if (!uf.empty()) CHECK(gs->move_fleet(1, uf[0], 1) == commands::RejectReason::None);
    ai::AiDefinitions defs;
    ai::EquationSet eqs;
    TacticalAiSetup ts;
    ts.defs = &defs;
    ts.equations = &eqs;
    ts.files = d.files.get();
    ts.ai_type = [](int32_t, const galactic::GalacticSim&) { return std::string(); };
    ts.constants.use_reinforcement_points = false;  // the synthetic map has no reinforcement points (enabling tested below)
    namespace mv = alamo::tactical::movement;
    bool invaded = false, checked = false;
    for (uint64_t t = 0; t < 1500 && !checked; ++t) {
        host.tick_once();
        if (!invaded && host.modes().depth() == 1) {
            const std::vector<uint32_t> f = gs->fleets_at(1, 1);
            if (!f.empty() && !gs->fleets()[f[0]].moving) invaded = gs->invade(1, 1, {}) == commands::RejectReason::None;
        }
        TacticalBattle* b = battles.battle();
        TacticalMode* tm = host.tactical();
        if (!b || !tm || b->frame() != 1) continue;
        checked = true;
        const auto reserves = b->reserves(1);
        CHECK(reserves.size() == 1);
        if (reserves.empty()) break;
        const auto key = static_cast<int32_t>(reserves[0].galactic_id);
        BattleAi ai(*b, *tm, host, ts);
        // Candidate spots over the map (every 4th cell centre right of the strip).
        auto scan = [&](std::vector<ai::tactical::Vec3f>& ok) {
            ok.clear();
            for (int32_t cy = 4; cy < 60; cy += 4)
                for (int32_t cx = kStrip + 6; cx < 58; cx += 4) {
                    const mv::V2 c = mv::cell_center(cx, cy);
                    if (ai.land_drop_ok(1, key, {c.x, c.y, 0.0f})) ok.push_back({c.x, c.y, 0.0f});
                }
        };
        std::vector<ai::tactical::Vec3f> fog_ok, clear_ok;
        scan(fog_ok);
        size_t newly = 0;
#ifdef ALAMO_SIM_HAVE_FOG
        const bool fog = b->fog() != nullptr;
        if (fog) {
            b->fog()->reveal_all(1);  // nothing fogged for player 1 any more
            scan(clear_ok);
            for (const auto& q : clear_ok)
                newly += std::none_of(fog_ok.begin(), fog_ok.end(), [&q](const ai::tactical::Vec3f& e) { return e.x == q.x && e.y == q.y; });
            CHECK(newly > 0);           // spots refused only because they were fogged
            CHECK(clear_ok.size() > fog_ok.size());
        } else {
            clear_ok = fog_ok;
        }
#else
        const bool fog = false;
        clear_ok = fog_ok;
#endif
        CHECK(!clear_ok.empty());
        for (const auto& q : fog_ok) CHECK(ai.land_drop_ok(1, key, q));  // fog cleared: still valid (and same answer twice)
        const mv::V2 strip = mv::cell_center(2, 31);
        CHECK(!ai.land_drop_ok(1, key, {strip.x, strip.y, 0.0f}));  // impassable slots
        // a1h t06 (§4.2.2 item 5): the Reinforce command re-runs the test and refuses the whole drop (no clamp, no move).
        CHECK(b->order_reinforce(*tm, 1, reserves[0].galactic_id, strip.x, strip.y, host.random().synced()) ==
              commands::RejectReason::InvalidPosition);
        CHECK(b->reserves(1).size() == 1);
        // TACTICAL_GAPS §4.2.1: an AI drop with a member slot on an enemy unit passes (the AI ignores movers); the same
        // drop for a human fails (its movers block). Slot 0's offset comes from the shared layout.
        const LandDropLayout at0 = land_drop_layout(b->land_drop_shape(reserves[0].type), 0.0f, 0.0f, b->player_drop_yaw(1));
        CHECK(at0.ok);
        size_t next_to_enemy = 0;
        for (const objects::ObjectHandle h : tm->primary().owned_by(0)) {
            if (!at0.ok || !tm->primary().active(h) || !tm->movable(h) || next_to_enemy >= 3) continue;
            const Vec3 q = tm->primary().position(h);
            const ai::tactical::Vec3f c{q.x - at0.slots[0].x, q.y - at0.slots[0].y, 0.0f};
            if (!ai.land_drop_ok(1, key, c)) continue;  // refused for another reason (passability, radius, a structure)
            CHECK(!ai.land_drop_valid(1, key, c, true));
            ++next_to_enemy;
        }
        CHECK(next_to_enemy > 0);
        {  // Use_Reinforcement_Points: a spot is valid only when every slot is within an allied enabling radius.
            TacticalAiSetup te = ts;
            te.constants.use_reinforcement_points = true;
            BattleAi ae(*b, *tm, host, te);
            size_t enabled = 0;
            for (const auto& q : clear_ok) {
                const bool ok = ae.land_drop_ok(1, key, q);
                enabled += ok ? 1u : 0u;
                if (ok) CHECK(ai.land_drop_ok(1, key, q));  // the radii only remove spots
            }
            std::printf("  land drop: %zu of %zu spots inside the enabling radii\n", enabled, clear_ok.size());
        }
        if (!clear_ok.empty()) {
            CHECK(!ai.land_drop_ok(1, key + 1000, clear_ok[0]));  // unknown entry
            CHECK(!ai.land_drop_ok(0, key, clear_ok[0]));         // another player's entry
        }
        std::printf("  land drop: %zu / %zu spots fogged-valid / valid, %zu refused only by fog (fog %s), %zu next to enemy units\n",
                    fog_ok.size(), clear_ok.size(), newly, fog ? "on" : "off", next_to_enemy);
        // s78i t02 (TACTICAL_COMBAT §3.1.1 "other spawns"): the reinforcement facing of a side = its entry-facing record
        // (+X when unset or for a player without a side); test_landing_drop checks a landing object against it.
        const Vec3 df = b->player_drop_facing(1);
        CHECK(df.z == 0.0f && std::fabs(df.x * df.x + df.y * df.y - 1.0f) < 1e-5f);
        CHECK(b->player_drop_facing(7).x == 1.0f && b->player_drop_facing(7).y == 0.0f && b->player_drop_facing(7).z == 0.0f);
    }
    CHECK(checked);
}
#endif
// --- end OI host ---

// t41: the battle's loss records hold the deaths of the tick that ends the battle. The host can no longer read that tick's
// death events (the battle is gone when the tick returns), so counting them per tick falls short of the records.
void test_final_tick_losses(const Data& d) {
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    SimHost host(d.types, js, cfg);
    galactic::GalacticSim* gs = host.start_galactic(d.rules, make_setup(d, false));
    CHECK(gs != nullptr);
    if (!gs) return;
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 6000;
    CampaignBattles battles(host, d.crules, *d.catalog,
                            [](const std::string& name, terrain::MapData& m) {
                                if (name != "S10_Space_B") return false;
                                m = space_map();
                                return true;
                            },
                            opt);
    CHECK(battles.attach(nullptr));
    const std::vector<uint32_t> uf = gs->fleets_at(2, 1);
    if (!uf.empty()) CHECK(gs->move_fleet(1, uf[0], 1) == commands::RejectReason::None);
    size_t seen = 0;
    for (uint64_t t = 0; t < 7000; ++t) {
        host.tick_once();
        if (const TacticalBattle* b = battles.battle()) seen += b->combat().deaths().size();
        else if (battles.battles_fought() > 0) break;
    }
    CHECK(battles.battles_fought() == 1 && !battles.battle());
    const size_t recorded = battles.last_battle_losses().size();
    std::printf("  final tick: %zu deaths seen per tick, %zu in the loss records\n", seen, recorded);
    // story t06: the battle now ends 210 frames after the victory declaration, so the deaths come before the final tick;
    // the records still hold every one of them.
    CHECK(seen > 0 && recorded >= seen);
}

// story t06 (BT §3.1a): the tactical victory monitor in the S10 space battle (Underworld 1 attacks the Empire 0).
//   held:    ENABLE_VICTORY 0 at the battle's first tick (Param1-less and extra "1" rewards change nothing); the fight runs
//            to its end with every 0xb / 0xf test queued, nothing declared, the battle running; ENABLE_VICTORY 1 releases
//            the hold and replays the queue: a victory is declared at once, its 210-frame countdown ends the battle.
//   VICTORY: the reward for the Empire (Param2 1: 1 s, silent) at battle frame 10 releases a raised hold and ends the
//            battle 30 frames later as a story victory, the Underworld retreating; an unknown faction (-1) does nothing.
struct MonitorRun {
    bool started = false, declared_while_held = false, ended = false;
    size_t queued = 0;
    int32_t hold_after_noops = -1, declared_type = -1, declared_player = -2, countdown = -1;
    bool silent = false, unknown_refused = false;
    uint32_t release_frame = 0, end_frame = 0;
    std::string summary;
    CrcLog crc;
};

MonitorRun run_monitor(const Data& d, bool story_victory, int32_t workers) {
    MonitorRun out;
    jobs::JobSystem js(jobs::JobSystemConfig{workers});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    SimHost host(d.types, js, cfg);
    galactic::GalacticSim* gs = host.start_galactic(d.rules, make_setup(d, false));
    CHECK(gs != nullptr);
    if (!gs) return out;
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 0;  // no time limit: only the monitor ends the battle
    CampaignBattles battles(host, d.crules, *d.catalog,
                            [](const std::string& name, terrain::MapData& m) {
                                if (name != "S10_Space_B") return false;
                                m = space_map();
                                return true;
                            },
                            opt);
    CHECK(battles.attach(nullptr));
    const std::vector<uint32_t> uf = gs->fleets_at(2, 1);
    if (!uf.empty()) CHECK(gs->move_fleet(1, uf[0], 1) == commands::RejectReason::None);
    uint32_t last_deaths = 0, quiet = 0, last_frame = 0;
    for (uint64_t t = 0; t < 9000; ++t) {
        host.tick_once();
        TacticalBattle* b = battles.battle();
        if (!b) {
            if (!out.started) continue;
            out.ended = true;  // finished in this tick, popped in its hook
            out.end_frame = last_frame + 1;
            out.summary = battles.log().empty() ? std::string() : battles.log().back();
            break;
        }
        last_frame = b->frame();
        if (!out.started) {
            out.started = true;
            b->reward_enable_victory(true, 0);   // hold 1
            b->reward_enable_victory(false, 0);  // no Param1: nothing
            b->reward_enable_victory(true, 1);   // release: 0 (nothing queued yet)
            b->reward_enable_victory(true, 1);   // not held: nothing (stays 0)
            b->reward_enable_victory(true, 0);   // hold 1 again
            out.hold_after_noops = b->victory_hold();
        }
        const uint32_t f = b->frame();
        if (b->finished()) continue;
        if (story_victory) {
            if (f == 10) {
                out.unknown_refused = !b->reward_victory(-1, false) && !b->victory_declared();
                CHECK(b->reward_victory(0, true));
                out.release_frame = f;
                out.declared_type = b->victory_declared_type();
                out.declared_player = b->victory_declared_player();
                out.countdown = b->victory_countdown();
                out.silent = b->victory_declared_silent();
            }
            continue;
        }
        if (out.release_frame != 0) continue;
        if (b->victory_declared()) out.declared_while_held = true;
        const uint32_t deaths = b->stats().deaths;
        quiet = deaths == last_deaths ? quiet + 1 : 0;
        last_deaths = deaths;
        if (deaths > 0 && quiet >= 300) {  // the fight is over: release with replay
            out.queued = b->victory_queued();
            b->reward_enable_victory(true, 1);
            out.release_frame = f;
            out.declared_type = b->victory_declared() ? b->victory_declared_type() : -1;
            out.declared_player = b->victory_declared() ? b->victory_declared_player() : -2;
            out.countdown = b->victory_countdown();
            out.silent = b->victory_declared_silent();
        }
    }
    out.crc = host.crc_log();
    return out;
}

void test_victory_monitor(const Data& d) {
    const MonitorRun h = run_monitor(d, false, 0);
    std::printf("  held: hold after the no-ops %d, queued %zu at release frame %u, declared type 0x%x for %d (countdown %d), end %u; %s\n",
                h.hold_after_noops, h.queued, h.release_frame, static_cast<unsigned>(h.declared_type), h.declared_player, h.countdown,
                h.end_frame, h.summary.c_str());
    CHECK(h.started && h.hold_after_noops == 1 && !h.declared_while_held && h.queued > 0 && h.release_frame > 0);
    CHECK(h.declared_type == galactic::kVictoryAllDestroyed || h.declared_type == galactic::kVictoryStarbaseDestroyed);
    CHECK(h.declared_player == 0 || h.declared_player == 1);
    CHECK(h.countdown == 210 && !h.silent && h.ended && h.end_frame == h.release_frame + 210);
    const MonitorRun v = run_monitor(d, true, 0);
    std::printf("  VICTORY: unknown refused %d, declared type 0x%x for %d (countdown %d, silent %d) at %u, end %u; %s\n",
                v.unknown_refused ? 1 : 0, static_cast<unsigned>(v.declared_type), v.declared_player, v.countdown, v.silent ? 1 : 0,
                v.release_frame, v.end_frame, v.summary.c_str());
    CHECK(v.started && v.unknown_refused && v.declared_type == galactic::kVictoryStory && v.declared_player == 0 && v.countdown == 30 && v.silent);
    CHECK(v.ended && v.end_frame == v.release_frame + 30);
    CHECK(v.summary.find("winner Empire, victory 16, retreated Underworld") != std::string::npos);
    const MonitorRun h4 = run_monitor(d, false, 4);
    CHECK(!first_divergence(h.crc, h4.crc).diverged && h4.end_frame == h.end_frame);
}

// --- OI objects: behaviour Initialise draws (TACTICAL_GAPS 6.4) through the SimHost's synced RNG ---
// A space mode on its own: every spawned ship attaches SELECTABLE (1), SHIELDED (computed 90), POWERED (150), HUNT (30,
// starts disabled), AMBIENT_SFX (30), STARSHIP_LOCOMOTOR (1): four synced Get_Int(0, interval - 1) per ship in attach
// order, first due frame = mode frame + draw. Identical schedule and CRC log for every worker count.
void test_oi_objects_scheduling() {
    const fs::path root = fs::temp_directory_path() / "alamo_sim_oi_objects";
    std::error_code ec;
    remove_all_long(root, ec);
    write_file(root / "Data/XML/GameObjectFiles.xml", "<L><File>U.xml</File></L>");
    write_file(root / "Data/XML/U.xml",
               "<R><SpaceUnit Name=\"OI_Ship\"><Victory_Relevant>Yes</Victory_Relevant><Tactical_Health>100</Tactical_Health><Max_Speed>2</Max_Speed>\n"
               "  <Behavior>SELECTABLE, SHIELDED, POWERED, HUNT, AMBIENT_SFX</Behavior><SpaceBehavior>STARSHIP_LOCOMOTOR</SpaceBehavior>\n"
               "</SpaceUnit></R>");
    DirectoryProvider dp(path_to_utf8(root));
    objects::ObjectTypeDatabase raw;
    CHECK(raw.load(dp));
    objects::GameObjectTypes types;
    CHECK(types.build(raw, &dp));
    const objects::TypeIndex ship = raw.find("OI_Ship");
    struct Result {
        CrcLog crc;
        std::vector<uint32_t> next;
        uint64_t draws = 0;
    };
    auto run = [&](int32_t workers) {
        Result r;
        jobs::JobSystem js(jobs::JobSystemConfig{workers});
        SimConfig cfg;
        cfg.seed = 4242;
        cfg.players = 2;
        SimHost host(types, js, cfg);
        auto& tm = static_cast<TacticalMode&>(host.start_game(objects::ModeKind::Space));
        objects::ObjectStore& st = tm.primary();
        auto spawn_checked = [&](float x) {
            SimRng ref = host.random().synced();
            const uint32_t frame = static_cast<uint32_t>(tm.frame());
            objects::SpawnParams sp;
            sp.type = ship;
            sp.owner = 0;
            sp.position = {x, 0.0f, 0.0f};
            const objects::ObjectHandle h = tm.spawn(sp, 1.0f);
            const auto bs = st.behaviors(h);
            for (uint32_t k = 0; k < bs.size(); ++k) {
                const uint32_t iv = st.behavior_interval(h, k);
                const uint32_t want = iv >= 2 ? frame + static_cast<uint32_t>(ref.range_int(0, static_cast<int32_t>(iv) - 1)) : 0u;
                CHECK(st.behavior_next_service(h, k) == want);
                r.next.push_back(st.behavior_next_service(h, k));
            }
            CHECK(ref == host.random().synced());
            CHECK(st.behavior_interval(h, 1) == 90 && st.behavior_interval(h, 2) == 150 && !st.behavior_enabled_by_type(h, objects::beh::Hunt));
        };
        for (int i = 0; i < 3; ++i) spawn_checked(100.0f * static_cast<float>(i));
        for (int t = 0; t < 20; ++t) host.tick_once();
        spawn_checked(500.0f);  // mid-battle creation: first due frame from the current mode frame
        for (int t = 0; t < 20; ++t) host.tick_once();
        r.crc = host.crc_log();
        r.draws = st.attach_draws();
        return r;
    };
    const Result a = run(0);
    std::printf("  oi objects: %llu attach draws, %zu behaviour schedules, %zu CRC records\n",
                static_cast<unsigned long long>(a.draws), a.next.size(), a.crc.records.size());
    CHECK(a.draws == 16 && !a.crc.records.empty());
    for (int32_t w : jobs::determinism_worker_counts()) {
        if (w == 0) continue;
        const Result b = run(w);
        CHECK(b.next == a.next && b.draws == a.draws && !first_divergence(a.crc, b.crc).diverged);
    }
    remove_all_long(root, ec);
}
// --- end OI objects ---

// S8 t10 follow-up (LOB_SOLVER §3.5 "MARKER removal", ABILITIES_HARDPOINTS §2.9): command 57 (special ability with a
// Dummy_Barrage_Target at a ground point) with PLACE_REMOTE_BOMB. The marker has no lifetime: it stays after the apply
// consumed the pending ability, while the thrower waits for its toss (the [port] cleanup of other markers would drop it
// there), and the toss kills it; the bomb (never armed) rests on the ground at the point until DETONATE_REMOTE_BOMB.
void test_remote_bomb_marker() {
    auto d = make_data(false, false, false, false, false, false, false, false, false, false, true);
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    SimHost host(d->types, js, cfg);
    galactic::GalacticSim* gs = host.start_galactic(d->rules, make_setup(*d, true));
    CHECK(gs != nullptr);
    if (!gs) return;
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 3000;
    opt.battle.order_interval = 0;  // no stand-in orders: only the test moves units
    CampaignBattles battles(host, d->crules, *d->catalog,
                            [](const std::string& name, terrain::MapData& m) {
                                if (name != "S10_Land_B") return false;
                                m = land_map();
                                return true;
                            },
                            opt);
    CHECK(battles.attach(nullptr));
    std::vector<objects::ObjectHandle> up;
    for (objects::ObjectHandle h : gs->planets()[2].surface)
        if (gs->store().owner(h) == 1) up.push_back(h);
    CHECK(gs->launch_units(1, 2, up) == commands::RejectReason::None);
    const std::vector<uint32_t> uf = gs->fleets_at(2, 1);
    if (!uf.empty()) CHECK(gs->move_fleet(1, uf[0], 1) == commands::RejectReason::None);
    const uint32_t dummy_type = type_of(*d, "Dummy_Barrage_Target");
    bool invaded = false, issued = false, waited = false, tossed = false;
    objects::ObjectId thrower = objects::kNoObjectId, marker = objects::kNoObjectId;
    float px = 0.0f, py = 0.0f;
    uint32_t issue_frame = 0;
    for (uint64_t t = 0; t < 6000 && !tossed; ++t) {
        host.tick_once();
        if (!invaded && host.modes().depth() == 1) {
            const std::vector<uint32_t> f = gs->fleets_at(1, 1);
            if (!f.empty() && !gs->fleets()[f[0]].moving) invaded = gs->invade(1, 1, {}) == commands::RejectReason::None;
        }
        TacticalBattle* b = battles.battle();
        TacticalMode* tm = host.tactical();
        if (!b || !tm) continue;
        objects::ObjectStore& st = tm->primary();
        auto marker_alive = [&] { return marker != objects::kNoObjectId && st.alive(st.find(marker)); };
        if (!issued) {
            for (const objects::ObjectHandle h : st.owned_by(1))
                if (st.alive(h) && b->combat().has_unit(st.id(h)) && b->combat().alive(st.id(h))) {
                    thrower = st.id(h);
                    break;
                }
            CHECK(thrower > 0);
            if (thrower <= 0) return;
            const Vec3 p = st.position(st.find(thrower));
            px = p.x + 80.0f;
            py = p.y;
            commands::EventData ev(commands::EventType::TacticalSpecialAbilityWithDummyTarget);
            ev.set_ids("users", std::vector<uint32_t>{static_cast<uint32_t>(thrower)});
            ev.set("ability", static_cast<uint32_t>(tactical::abilities::kPlaceRemoteBomb)).set_f("x", px).set_f("y", py).set_f("z", 0.0f);
            CHECK(b->handle_command(*tm, 1, ev, host.random().synced()) == commands::RejectReason::None);
            for (const objects::ObjectHandle h : st.all_objects())
                if (st.alive(h) && static_cast<uint32_t>(st.type(h)) == dummy_type) marker = st.id(h);
            CHECK(marker_alive());
            issued = true;
            issue_frame = b->frame();
            continue;
        }
        // Applied (the pending order is consumed), not yet tossed: the marker stays.
        if (!waited && b->abilities().pending_type(thrower) < 0 && b->combat().projectile_count() == 0) {
            CHECK(marker_alive());
            waited = true;
        }
        for (size_t i = 0; i < b->combat().projectile_count(); ++i) {
            const tactical::combat::ProjectileView v = b->combat().projectile(i);
            if (!v.alive || !v.stuck) continue;
            CHECK(!marker_alive());  // killed by the toss
            CHECK(v.stuck_to == tactical::combat::kNoObject && std::fabs(v.pos.x - px) < 10.0f && std::fabs(v.pos.y - py) < 10.0f);
            std::printf("  remote bomb marker: issued at %u, bomb resting at (%.1f, %.1f) at %u, marker gone\n", issue_frame,
                        static_cast<double>(v.pos.x), static_cast<double>(v.pos.y), b->frame());
            tossed = true;
        }
    }
    CHECK(issued && waited && tossed);
}

// S8 t02 / S7 t13 (MOVEMENT §1.3 / §2.2): obstacle footprints follow their objects.
//   * land: a map-placed LAND_OBSTACLE bunker owns its placement stamp (object id) and loses it when it dies; a
//     Map_Load_Spawn_Table placeholder's stamp is replaced by the spawned rock's footprint; a rock spawned during the
//     battle is stamped at the end of that tick under its object id and unstamped the tick it is removed;
//   * space: a map-placed SPACE_OBSTACLE unit owns its placement entry (object id) and loses it when it dies (Hyperspace_Away
//     kill); a placeholder's entry is replaced by the spawned asteroid's (radius 400, not the placeholder's 50); S8 t09: the
//     asteroid (not a battle unit) loses its entry the tick it is removed;
//   * S8 t09 (S9_DEATH_CLONE_SHIELDS §1.6, MOVEMENT §1.3): a death clone keeps only HIDE_WHEN_FOGGED / IMPOSING_PRESENCE /
//     LAND_OBSTACLE (the last only with Death_Clone_Is_Obstacle) and gets DEATH; a clone without LAND_OBSTACLE is never
//     stamped.
void test_runtime_obstacles(bool space) {
    namespace mv = tactical::movement;
    auto d = make_data(false, false, false, false, false, true);
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    SimHost host(d->types, js, cfg);
    galactic::GalacticSim* gs = host.start_galactic(d->rules, make_setup(*d, !space));
    CHECK(gs != nullptr);
    if (!gs) return;
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 3000;
    opt.battle.order_interval = 0;  // no stand-in orders: only the test moves units
    auto place = [](terrain::MapData& m, const char* type, float x, float y, uint32_t owner) {
        terrain::ObjectPlacement p;
        p.type_crc = name_crc(type);
        p.owner = owner;
        p.position[0] = x;
        p.position[1] = y;
        m.objects.push_back(p);
    };
    CampaignBattles battles(host, d->crules, *d->catalog,
                            [&place](const std::string& name, terrain::MapData& m) {
                                if (name == "S10_Space_B") {
                                    m = space_map();
                                    place(m, "S10_Obst_Mine", 2500.0f, 2500.0f, 0);               // placement 0
                                    place(m, "S10_Obst_Asteroid_Spawner", -2500.0f, 2500.0f, 2);  // placement 1
                                } else if (name == "S10_Land_B") {
                                    m = land_map();
                                    place(m, "S10_Obst_Bunker", 450.0f, 620.0f, 0);         // placement 0, near the attackers
                                    place(m, "S10_Obst_Rock_Spawner", 900.0f, 300.0f, 2);  // placement 1
                                } else {
                                    return false;
                                }
                                return true;
                            },
                            opt);
    CHECK(battles.attach(nullptr));
    if (!space) {
        std::vector<objects::ObjectHandle> up;
        for (objects::ObjectHandle h : gs->planets()[2].surface)
            if (gs->store().owner(h) == 1) up.push_back(h);
        CHECK(gs->launch_units(1, 2, up) == commands::RejectReason::None);
    }
    const std::vector<uint32_t> uf = gs->fleets_at(2, 1);
    if (!uf.empty()) CHECK(gs->move_fleet(1, uf[0], 1) == commands::RejectReason::None);
    // The cells a footprint covers on an empty grid of the battle grid's size (the expected stamp).
    auto cells_of = [](const mv::PassabilityGrid& like, float x, float y, float size) {
        mv::PassabilityGrid g;
        g.init(like.width(), like.height());
        g.stamp(1, mv::make_footprint({x, y}, 0.0f, size, size, -0.5f * size, -0.5f * size, 0, 0, 1.0f, 0));
        const std::span<const uint32_t> c = g.stamp_cells(1);
        return std::vector<uint32_t>(c.begin(), c.end());
    };
    auto same = [](std::span<const uint32_t> a, const std::vector<uint32_t>& b) { return std::equal(a.begin(), a.end(), b.begin(), b.end()); };
    bool invaded = false, checked = false, spawned_checked = false, removed_checked = false, died = false, asteroid_checked = false;
    objects::ObjectHandle asteroid_h{};
    objects::ObjectId asteroid = objects::kNoObjectId, prison_clone = objects::kNoObjectId, bunker_clone = objects::kNoObjectId;
    objects::ObjectId unit = objects::kNoObjectId, spawned = objects::kNoObjectId;
    objects::ObjectHandle spawned_h{};
    uint32_t spawn_frame = 0;
    for (uint64_t t = 0; t < 6000 && !(died && (space || removed_checked)); ++t) {
        host.tick_once();
        if (!space && !invaded && host.modes().depth() == 1) {
            const std::vector<uint32_t> f = gs->fleets_at(1, 1);
            if (!f.empty() && !gs->fleets()[f[0]].moving) invaded = gs->invade(1, 1, {}) == commands::RejectReason::None;
        }
        TacticalBattle* b = battles.battle();
        TacticalMode* tm = host.tactical();
        if (!b || !tm) continue;
        objects::ObjectStore& st = tm->primary();
        const mv::MovementSystem& ms = b->movement();
        if (!checked) {  // the first battle frame: the map objects own their placements' footprints
            checked = true;
            objects::ObjectId other = objects::kNoObjectId;
            const uint32_t unit_type = type_of(*d, space ? "S10_Obst_Mine" : "S10_Obst_Bunker");
            const uint32_t other_type = type_of(*d, space ? "S10_Obst_Asteroid" : "S10_Obst_Rock");
            for (const objects::ObjectHandle h : st.all_objects()) {
                if (!st.alive(h)) continue;
                if (static_cast<uint32_t>(st.type(h)) == unit_type) unit = st.id(h);
                if (static_cast<uint32_t>(st.type(h)) == other_type) other = st.id(h);
                CHECK(static_cast<uint32_t>(st.type(h)) != type_of(*d, space ? "S10_Obst_Asteroid_Spawner" : "S10_Obst_Rock_Spawner"));
            }
            CHECK(unit > 0 && other > 0);
            if (unit <= 0 || other <= 0) return;
            if (space) {
                const mv::SpaceOccupancy& occ = ms.space();
                mv::V2 c{};
                float r = 0.0f;
                uint32_t bits = 0;
                CHECK(!occ.has_static(mv::map_stamp_id(0)) && !occ.has_static(mv::map_stamp_id(1)));
                CHECK(occ.static_entry(unit, c, r, bits) && r == 120.0f && c.x == 2500.0f && c.y == 2500.0f);
                CHECK(occ.static_entry(other, c, r, bits) && r == 400.0f && c.x == -2500.0f && c.y == 2500.0f);  // not 50
                std::printf("  space: map unit %d entry r 120, spawned asteroid %d entry r %.0f\n", unit, other, static_cast<double>(r));
                CHECK(b->hyperspace_out(unit, 0, true));  // killed 90 frames later
                // S8 t09: the asteroid is no battle unit; removing it must drop its (re-keyed) entry at the next sync.
                asteroid = other;
                asteroid_h = st.find(other);
                CHECK(tm->destroy(asteroid_h));
            } else {
                const mv::PassabilityGrid& g = ms.grid();
                CHECK(!g.stamped(mv::map_stamp_id(0)) && !g.stamped(mv::map_stamp_id(1)));
                CHECK(g.stamped(unit) && same(g.stamp_cells(unit), cells_of(g, 450.0f, 620.0f, 60.0f)));
                CHECK(g.stamped(other) && same(g.stamp_cells(other), cells_of(g, 900.0f, 300.0f, 100.0f)));  // the rock's, not 20 x 20
                CHECK(g.effective(mv::world_to_cell(450.0f), mv::world_to_cell(620.0f)) == mv::kPassObstacle);
                std::vector<objects::ObjectId> attackers;
                for (const objects::ObjectHandle h : st.owned_by(1))
                    if (st.alive(h) && b->combat().has_unit(st.id(h)) && b->combat().alive(st.id(h))) attackers.push_back(st.id(h));
                CHECK(!attackers.empty());
                CHECK(b->order_attack(*tm, 1, attackers, unit, host.random().synced()) == commands::RejectReason::None);
                // A rock spawned between two ticks (script spawn path) is stamped by the next tick's end.
                objects::SpawnParams sp;
                sp.type = static_cast<objects::TypeIndex>(other_type);
                sp.owner = 2;
                sp.position = {300.0f, 1000.0f, 0.0f};
                sp.facing = {0.0f, -1.0f, 0.0f};
                spawned_h = tm->spawn(sp);
                spawned = st.id(spawned_h);
                spawn_frame = b->frame();
                CHECK(spawned > 0 && !g.stamped(spawned));
                // S8 t09: death clones of a bunker (Death_Clone_Is_Obstacle default true) and of a prison (false).
                sp.type = static_cast<objects::TypeIndex>(type_of(*d, "S10_Obst_Bunker"));
                sp.position = {1200.0f, 1000.0f, 0.0f};
                const objects::ObjectHandle bc = tm->spawn_death_clone(sp);
                sp.type = static_cast<objects::TypeIndex>(type_of(*d, "S10_Obst_Prison"));
                sp.position = {1200.0f, 600.0f, 0.0f};
                const objects::ObjectHandle pc = tm->spawn_death_clone(sp);
                bunker_clone = st.id(bc);
                prison_clone = st.id(pc);
                const auto list = [&st](objects::ObjectHandle h) {
                    return std::vector<objects::BehaviorTypeId>(st.behaviors(h).begin(), st.behaviors(h).end());
                };
                CHECK(st.dying(bc) && st.dying(pc));
                CHECK(list(bc) == (std::vector<objects::BehaviorTypeId>{objects::beh::LandObstacle, 0x44}));  // SELECTABLE gone
                CHECK(list(pc) == (std::vector<objects::BehaviorTypeId>{0x44}));  // LAND_OBSTACLE gone too
            }
            continue;
        }
        if (!space) {
            const mv::PassabilityGrid& g = ms.grid();
            if (!spawned_checked && b->frame() == spawn_frame + 1) {
                spawned_checked = true;
                CHECK(g.stamped(spawned) && same(g.stamp_cells(spawned), cells_of(g, 300.0f, 1000.0f, 100.0f)));
                CHECK(g.effective(mv::world_to_cell(300.0f), mv::world_to_cell(1000.0f)) == mv::kPassObstacle);
                // S8 t09: the bunker's clone kept LAND_OBSTACLE and is stamped; the prison's is not.
                CHECK(g.stamped(bunker_clone) && same(g.stamp_cells(bunker_clone), cells_of(g, 1200.0f, 1000.0f, 60.0f)));
                CHECK(!g.stamped(prison_clone) && g.effective(mv::world_to_cell(1200.0f), mv::world_to_cell(600.0f)) == mv::kPassClear);
                CHECK(tm->destroy(spawned_h));
            } else if (spawned_checked && !removed_checked) {
                removed_checked = true;
                CHECK(!g.stamped(spawned) && g.effective(mv::world_to_cell(300.0f), mv::world_to_cell(1000.0f)) == mv::kPassClear);
            }
            if (!died && !st.alive(st.find(unit))) {
                died = true;
                // The original's stamp goes; its death clone (no Death_Clone list: the bunker type itself, S9_DEATH_CLONE_SHIELDS
                // §1.6) keeps LAND_OBSTACLE (Death_Clone_Is_Obstacle, default true) and stamps the same cells (MOVEMENT §1.3 [H]).
                objects::ObjectId clone = objects::kNoObjectId;
                for (const objects::ObjectHandle h : st.all_objects())
                    if (st.alive(h) && st.dying(h) && static_cast<uint32_t>(st.type(h)) == type_of(*d, "S10_Obst_Bunker") &&
                        st.id(h) != bunker_clone)
                        clone = st.id(h);
                std::printf("  land: bunker %d died at frame %u (death clone %d), rock %d spawned at %u\n", unit, b->frame(), clone, spawned,
                            spawn_frame);
                CHECK(!g.stamped(unit) && clone > 0 && g.stamped(clone) && same(g.stamp_cells(clone), cells_of(g, 450.0f, 620.0f, 60.0f)));
            }
            if (!died) CHECK(g.stamped(unit));
        } else {
            if (!asteroid_checked && asteroid > 0) {
                asteroid_checked = true;
                CHECK(!st.exists(asteroid_h) && !ms.space().has_static(asteroid));
            }
            if (!died && !st.alive(st.find(unit))) {
                died = true;
                std::printf("  space: map unit %d died at frame %u\n", unit, b->frame());
                CHECK(!ms.space().has_static(unit) && ms.space().query_static({2500.0f, 2500.0f}, {2500.0f, 2500.0f}, 1.0f) == 0);
            }
            if (!died) CHECK(ms.space().has_static(unit));
        }
    }
    CHECK(checked && died);
    if (!space) CHECK(spawned_checked && removed_checked);
    if (space) CHECK(asteroid_checked);
}

}  // namespace

// S7 t12 (FIGHTER_FLYING §2 step 6 / §4.3, TACTICAL_COMBAT §2.8 Formation_Attack_Target): a fighter squadron moved past
// an enemy frigate keeps its move order. Before, the frigate (the squadron's combat auto target) became the locomotor's
// target, the attack run reset the squadron's path (§4.3 step 4) and the squadron never reached its destination (skirmish
// foc seed 5: a TIE squadron on Prepare_Ambush parked ~1900 short when its target went into the fog). Then an ordered
// attack on the frigate starts Directed_Combat at once (an ordered target is the formation attack target).
void test_fighter_move_past_enemy() {
    namespace mv = tactical::movement;
    {  // the pure rule (S7 t22: no auto-target input - an auto target reaches the squadron only as a pushed block)
        FighterTargetInputs in;
        CHECK(fighter_formation_target(in) == kNoObjectId);  // a position block / no formation: no object, keep Moving
        in.goal = 9;
        CHECK(fighter_formation_target(in) == kNoObjectId);  // a goal that is not hostile never qualifies
        in.goal_hostile = true;
        CHECK(fighter_formation_target(in) == 9);  // Attack_Move(object)
        in.pushed = 7;
        CHECK(fighter_formation_target(in) == 7);  // a pushed auto block is newer than the order's block
        in.ordered = 11;
        CHECK(fighter_formation_target(in) == 11);  // Attack / Attack_Target (locked team target)
    }
    auto d = make_data(false, false, false, false, false, false, false, false, true);
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    SimHost host(d->types, js, cfg);
    galactic::GalacticSetup gsetup = make_setup(*d, false);
    gsetup.forces.push_back({0, 1, type_of(*d, "S10_TIE_Squadron")});
    galactic::GalacticSim* gs = host.start_galactic(d->rules, gsetup);
    CHECK(gs != nullptr);
    if (!gs) return;
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 6000;
    opt.battle.order_interval = 0;  // no stand-in orders: only the test moves units
    CampaignBattles battles(host, d->crules, *d->catalog,
                            [](const std::string& name, terrain::MapData& m) {
                                if (name != "S10_Space_B") return false;
                                m = space_map();
                                return true;
                            },
                            opt);
    CHECK(battles.attach(nullptr));
    const std::vector<uint32_t> uf = gs->fleets_at(2, 1);
    if (!uf.empty()) CHECK(gs->move_fleet(1, uf[0], 1) == commands::RejectReason::None);
    using R = commands::RejectReason;
    std::vector<objects::ObjectId> members;
    objects::ObjectId enemy = objects::kNoObjectId;
    mv::V2 dest{};
    bool issued = false, auto_seen = false, arrived = false, attacked = false;
    uint32_t combat_while_moving = 0, arrive_frame = 0, attack_frame = 0, end_frame = 0;
    float closest_pass = 1.0e9f;
    for (uint64_t t = 0; t < 7000; ++t) {
        host.tick_once();
        TacticalBattle* b = battles.battle();
        TacticalMode* tm = host.tactical();
        if (!b || !tm) {
            if (issued) break;
            continue;
        }
        const objects::ObjectStore& st = tm->primary();
        if (b->frame() == 3 && !issued) {
            issued = true;
            for (const mv::FighterSquad& s : b->movement().fighter_squads())
                if (!s.members.empty() && st.owner(st.find(s.members.front())) == 0) members = s.members;
            CHECK(members.size() == 3);
            if (members.empty()) break;
            const Vec3 sp = st.position(st.find(members.front()));
            float best = 0.0f;
            for (const objects::ObjectHandle h : st.owned_by(1)) {
                if (!st.alive(h) || !b->movement().has_unit(st.id(h))) continue;
                const Vec3 p = st.position(h);
                const float d2 = (p.x - sp.x) * (p.x - sp.x) + (p.y - sp.y) * (p.y - sp.y);
                if (enemy == objects::kNoObjectId || d2 < best) {
                    enemy = st.id(h);
                    best = d2;
                }
            }
            CHECK(enemy != objects::kNoObjectId);
            if (enemy == objects::kNoObjectId) break;
            // Straight through the enemy and 1500 beyond it (inside the playable box).
            const Vec3 ep = st.position(st.find(enemy));
            const float dx = ep.x - sp.x, dy = ep.y - sp.y, len = std::sqrt(dx * dx + dy * dy);
            dest = {std::clamp(ep.x + dx / len * 1500.0f, -3700.0f, 3700.0f), std::clamp(ep.y + dy / len * 1500.0f, -3700.0f, 3700.0f)};
            CHECK(b->order_move(*tm, 0, members, dest.x, dest.y, host.random().synced()) == R::None);
            std::printf("  squadron %d..%d at (%.0f, %.0f), enemy %d at (%.0f, %.0f), move to (%.0f, %.0f)\n", members.front(),
                        members.back(), static_cast<double>(sp.x), static_cast<double>(sp.y), enemy, static_cast<double>(ep.x),
                        static_cast<double>(ep.y), static_cast<double>(dest.x), static_cast<double>(dest.y));
            continue;
        }
        if (!issued) continue;
        const objects::ObjectId lead = members.front();
        if (!st.alive(st.find(lead)) || !b->movement().has_unit(lead)) break;
        end_frame = b->frame();
        const mv::UnitView v = b->movement().view(lead);
        const bool enemy_alive = st.alive(st.find(enemy));
        if (enemy_alive) {
            const Vec3 lp = st.position(st.find(lead)), ep = st.position(st.find(enemy));
            closest_pass = std::min(closest_pass, std::sqrt((lp.x - ep.x) * (lp.x - ep.x) + (lp.y - ep.y) * (lp.y - ep.y)));
            if (!arrived && b->combat().current_target(lead) == enemy) auto_seen = true;
        }
        if (!arrived) {
            if (v.loco_state == mv::LocoState::FighterDirectedCombat) ++combat_while_moving;
            const float gx = v.pos.x - dest.x, gy = v.pos.y - dest.y;
            if (v.arrived || gx * gx + gy * gy < 300.0f * 300.0f) {
                arrived = true;
                arrive_frame = b->frame();
                if (enemy_alive) {  // then an ordered attack on the frigate: Directed_Combat from the next frame
                    CHECK(b->order_attack(*tm, 0, members, enemy, host.random().synced()) == R::None);
                    attack_frame = b->frame();
                }
            }
        } else if (attack_frame != 0 && !attacked && b->frame() <= attack_frame + 3) {
            attacked = v.loco_state == mv::LocoState::FighterDirectedCombat;
        }
        if ((arrived && (attack_frame == 0 || b->frame() > attack_frame + 3)) || b->frame() >= 3000) break;
    }
    std::printf("  frames to %u: enemy auto target seen %d, closest pass %.0f, Directed_Combat frames while moving %u, arrived %d (frame %u), "
                "ordered attack -> Directed_Combat %d\n",
                end_frame, auto_seen ? 1 : 0, static_cast<double>(closest_pass), combat_while_moving, arrived ? 1 : 0, arrive_frame,
                attacked ? 1 : 0);
    CHECK(issued && auto_seen && closest_pass < 560.0f);  // the frigate was the auto target and inside the strafe circle
    CHECK(combat_while_moving == 0 && arrived);
    CHECK(attack_frame != 0 && attacked);
}

// a1i t04 / t05: a land Reinforce of a company with a Company_Transport_Unit spawns its landing object (the transport's
// Landing_Transport_Variant at the drop point, z + Hover_Offset, TRANSPORT_LANDING, carried type = the company).
// Untimed ([port] option): the members spawn at the 3.0-factor unload offsets at the order, the unloaded record is posted
// and the landing object leaves after (teams + 1) x 2 s. Timed (BattleConfig::landing_unload_timed, the default since a1j
// t01): the company stays in the hidden landing; one team per 2 s leaves on a locked exit move; the last step unlocks
// them, posts the record and removes the landing; the battle state saves and loads mid-unload. Also the deployed extents
// getter. a1j t01 (A1_READINGS §36), timed with `destroy_at` > 0: the landing object is destroyed `destroy_at` frames after
// the order; the next service posts the signal 0x19 record of the detach (vf15 0x6442d0) with the units out so far
// (destroy_at 90: team 0; destroy_at 30: none [port: the original sends nothing then]), unlocked, and the rest never spawns.
void test_landing_drop(bool timed, uint32_t destroy_at = 0) {
    auto d = make_data(false, false, false, false, false, false, false, false, false, false, false, true);
    const objects::TypeIndex lander = d->types.resolve_name("S10_Lander"), landing_t = d->types.resolve_name("S10_Lander_Landing");
    CHECK(lander != objects::kNone && landing_t != objects::kNone);
    if (!timed) {
        float hx = 0.0f, hy = 0.0f;
        drop_half_extents(d->types, d->catalog.get(), lander, hx, hy);
        CHECK(hx == 20.0f && hy == 15.0f);
        drop_deployed_extents(d->types, d->catalog.get(), lander, hx, hy);
        CHECK(hx == 30.0f && hy == 0.0f);  // 0x371d20: y from Custom_Hard_YExtent_Deployed whenever the deployed x is set
        drop_deployed_extents(d->types, d->catalog.get(), static_cast<uint32_t>(d->types.resolve_name("S10_Trooper")), hx, hy);
        CHECK(hx == 6.0f && hy == 6.0f);  // no deployed x: the normal extents
    }
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    SimHost host(d->types, js, cfg);
    galactic::GalacticSim* gs = host.start_galactic(d->rules, make_setup(*d, true));
    CHECK(gs != nullptr);
    if (!gs) return;
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 900;
    opt.battle.landing_unload_timed = timed;
    CampaignBattles battles(host, d->crules, *d->catalog,
                            [](const std::string& name, terrain::MapData& m) {
                                if (name != "S10_Land_B") return false;
                                m = land_map();
                                return true;
                            },
                            opt);
    CHECK(battles.attach(nullptr));
    std::vector<objects::ObjectHandle> up;
    for (objects::ObjectHandle h : gs->planets()[2].surface)
        if (gs->store().owner(h) == 1) up.push_back(h);
    CHECK(gs->launch_units(1, 2, up) == commands::RejectReason::None);
    const std::vector<uint32_t> uf = gs->fleets_at(2, 1);
    if (!uf.empty()) CHECK(gs->move_fleet(1, uf[0], 1) == commands::RejectReason::None);
    namespace mv = alamo::tactical::movement;
    bool invaded = false, dropped = false, seen_alive_late = false, gone = false, saved = false;
    objects::ObjectId landing_id = objects::kNoObjectId;
    objects::ObjectId key = objects::kNoObjectId;
    uint32_t drop_frame = 0, spawned_before = 0, squad = 0;
    LandDropLayout lay;
    for (uint64_t t = 0; t < 2500 && !gone; ++t) {
        host.tick_once();
        if (!invaded && host.modes().depth() == 1) {
            const std::vector<uint32_t> f = gs->fleets_at(1, 1);
            if (!f.empty() && !gs->fleets()[f[0]].moving) invaded = gs->invade(1, 1, {}) == commands::RejectReason::None;
        }
        TacticalBattle* b = battles.battle();
        TacticalMode* tm = host.tactical();
        if (!b || !tm) {
            if (dropped) break;  // the battle ended before the take-off
            continue;
        }
        objects::ObjectStore& st = tm->primary();
        if (!dropped) {
            if (b->frame() != 1) continue;
            const auto reserves = b->reserves(1);
            CHECK(reserves.size() == 1);
            if (reserves.empty()) break;
            key = reserves[0].galactic_id;
            squad = reserves[0].type;
            spawned_before = b->stats().spawned;
#ifdef ALAMO_SIM_HAVE_FOG
            if (b->fog()) b->fog()->reveal_all(1);  // the drop test needs unfogged cells
#endif
            size_t valid = 0, refused = 0;
            for (int32_t cy = 6; cy < 58 && !dropped; cy += 2)
                for (int32_t cx = 6; cx < 58 && !dropped; cx += 2) {
                    const mv::V2 c = mv::cell_center(cx, cy);
                    if (!b->land_drop_valid(*tm, 1, key, c.x, c.y, false, false)) continue;
                    ++valid;
                    if (const commands::RejectReason rr = b->order_reinforce(*tm, 1, key, c.x, c.y, host.random().synced());
                        rr != commands::RejectReason::None) {
                        if (refused++ == 0) std::printf("  landing drop: first refusal %d at (%.0f, %.0f)\n", static_cast<int>(rr), static_cast<double>(c.x), static_cast<double>(c.y));
                        continue;
                    }
                    dropped = true;
                    drop_frame = b->frame();
                    lay = land_drop_layout(b->land_drop_shape(squad), c.x, c.y, b->player_drop_yaw(1), 3.0f);
                    CHECK(lay.ok && lay.slots.size() == 4 && lay.group_x.size() == 2);  // two teams of Max_Squad_Size 2
                    CHECK(b->reserves(1).empty());
                    const auto recs = b->landings_unloaded();
                    // The landing object.
                    objects::ObjectHandle lh;
                    if (timed) {
                        CHECK(recs.empty() && b->stats().spawned == spawned_before);  // the company is still inside
                        CHECK(b->landings_in_flight(1) == 1);
                        for (const objects::ObjectHandle h : st.all_objects())
                            if (st.alive(h) && st.type(h) == landing_t) lh = h;
                        landing_id = st.alive(lh) ? st.id(lh) : objects::kNoObjectId;
                        CHECK(b->landing_hidden(landing_id));  // state 4 until the service sees it clear
                    } else {
                        CHECK(recs.size() == 1);
                        if (recs.size() != 1) break;
                        landing_id = recs[0].landing;
                        lh = st.find(landing_id);
                        CHECK(recs[0].galactic_id == key && recs[0].owner == 1 && recs[0].carried_type == squad);
                        CHECK(recs[0].units.size() == 4 && b->stats().spawned == spawned_before + 4u);
                        CHECK(b->landings_in_flight(1) == 0 && !b->landing_hidden(landing_id));
                        // The members at the unload offsets (3.0 x the team bounds, the side's drop yaw).
                        for (size_t k = 0; lay.ok && k < lay.slots.size() && k < recs[0].units.size(); ++k) {
                            const Vec3 mp = st.position(st.find(recs[0].units[k]));
                            CHECK(std::fabs(mp.x - lay.slots[k].x) < 1e-3f && std::fabs(mp.y - lay.slots[k].y) < 1e-3f);
                        }
                        // s78j t01 (TACTICAL_COMBAT §3.1.1 "Units leaving a land landing object"): team 0 (units 0, 1)
                        // faces the landing's facing; team 1 (units 2, 3) faces along its offset from the landing centre.
                        const Vec3 psi = b->player_drop_facing(1);
                        const double oy = static_cast<double>(lay.group_y[1] - c.y), ox = static_cast<double>(lay.group_x[1] - c.x);
                        const double a1 = std::atan2(oy, ox);
                        for (size_t k = 0; lay.ok && k < recs[0].units.size(); ++k) {
                            const Vec3 f = st.facing(st.find(recs[0].units[k]));
                            const double ex = k < 2 ? static_cast<double>(psi.x) : std::cos(a1);
                            const double ey = k < 2 ? static_cast<double>(psi.y) : std::sin(a1);
                            CHECK(std::fabs(static_cast<double>(f.x) - ex) < 1e-4 && std::fabs(static_cast<double>(f.y) - ey) < 1e-4);
                        }
                        std::printf("  landing drop: team 0 facing (%.3f, %.3f), team 1 offset yaw %.1f deg\n", static_cast<double>(psi.x),
                                    static_cast<double>(psi.y), a1 * 57.29577951308232);
                    }
                    CHECK(st.alive(lh));
                    CHECK(st.type(lh) == landing_t);  // the Landing_Transport_Variant
                    CHECK(st.has_behavior(lh, objects::beh::TransportLanding) && st.carried_type(lh) == squad);
                    CHECK(st.parent_mode_id(lh) == key && st.owner(lh) == 1 && !st.landing_persist(lh));
                    const Vec3 lp = st.position(lh);
                    CHECK(lp.x == c.x && lp.y == c.y && std::fabs(lp.z - 10.0f) < 1e-4f);  // flat map + Hover_Offset 10
                    // s78i t02 (TACTICAL_COMBAT §3.1.1 "other spawns"): the landing object faces the owner's entry-facing record.
                    const Vec3 lf = st.facing(lh), df = b->player_drop_facing(1);
                    CHECK(lf.x == df.x && lf.y == df.y && std::fabs(df.x * df.x + df.y * df.y - 1.0f) < 1e-5f);
                }
            if (!dropped) std::printf("  landing drop: no drop (%zu valid spots, %zu refused orders)\n", valid, refused);
            CHECK(dropped);
            if (!dropped) break;
            continue;
        }
        const objects::ObjectHandle lh = st.find(landing_id);
        const uint32_t since = b->frame() - drop_frame;  // 3 steps of 2 s (60 frames)
        if (!timed) {
            if (since < 180u) {
                CHECK(st.alive(lh));
                if (since == 179u) seen_alive_late = true;
                if (since == 1u) CHECK(b->landings_unloaded().empty());  // cleared by the next tick
            } else {
                CHECK(!st.alive(lh));
                gone = true;
            }
            continue;
        }
        // Timed: the service starts the step timer on the first tick (frame drop + 1); team 0 at +61, team 1 at +121, the
        // finish at +181.
        const uint32_t spawned = b->stats().spawned - spawned_before;
        if (since == 1u) CHECK(!b->landing_hidden(landing_id) && spawned == 0);
        if (since == 60u) CHECK(spawned == 0);
        if ((since == 61u || since == 121u) && (destroy_at == 0 || since < destroy_at)) {
            const uint32_t want = since == 61u ? 2u : 4u;
            CHECK(spawned == want);
            const objects::ObjectId uid = b->story_reinforced().empty() ? objects::kNoObjectId : b->story_reinforced().back();
            CHECK(uid != objects::kNoObjectId && b->movement_locked(uid));  // the locked exit move
            const Vec3 mp = st.position(st.find(uid));
            CHECK(std::fabs(mp.x - st.position(lh).x) < 30.0f && std::fabs(mp.y - st.position(lh).y) < 30.0f);  // spawned at the landing
            CHECK(b->landings_in_flight(1) == 1 && b->landings_unloaded().empty());
        }
        if (destroy_at > 0) {  // a1j t01: the landing destroyed mid-unload
            if (since == destroy_at) {
                CHECK(st.alive(lh) && tm->destroy(lh));
                continue;
            }
            if (since == destroy_at + 1u) {
                const uint32_t out = destroy_at > 61u ? 2u : 0u;
                const auto recs = b->landings_unloaded();
                CHECK(recs.size() == 1 && b->landings_in_flight(1) == 0 && spawned == out);
                if (recs.size() == 1) {
                    CHECK(recs[0].landing == landing_id && recs[0].galactic_id == key && recs[0].owner == 1 && recs[0].units.size() == out);
                    for (const objects::ObjectId u : recs[0].units) CHECK(!b->movement_locked(u));
                }
                seen_alive_late = true;
                continue;
            }
            if (since > destroy_at + 1u) {
                CHECK(spawned == (destroy_at > 61u ? 2u : 0u) && b->landings_unloaded().empty());  // the rest never comes
                if (since >= 200u) gone = true;
            }
            continue;
        }
        if (since == 90u && !saved) {  // mid-unload save / load of the battle state (in-flight landing included)
            StateHasher h0;
            b->hash(h0);
            ByteWriter w;
            b->write_part(TacticalBattle::SavePart::State, w);
            ByteReader r(w.bytes());
            CHECK(b->read_part(TacticalBattle::SavePart::State, r));
            // s78f t02: battle state version 7 (FighterFormationRecord::pushed dropped, after main's story t06 v6): 6 and 8
            // are refused.
            std::vector<uint8_t> stale(w.bytes().begin(), w.bytes().end());
            for (const uint32_t v : {6u, 8u}) {
                for (size_t k = 0; k < 4; ++k) stale[k] = static_cast<uint8_t>(v >> (8 * k));
                ByteReader rs(stale);
                CHECK(!b->read_part(TacticalBattle::SavePart::State, rs));
            }
            StateHasher h1;
            b->hash(h1);
            CHECK(h0.digest() == h1.digest() && b->landings_in_flight(1) == 1);
            saved = true;
        }
        if (since < 181u) {
            CHECK(st.alive(lh));
            if (since == 180u) seen_alive_late = true;
        } else {
            CHECK(!st.alive(lh));
            const auto recs = b->landings_unloaded();
            CHECK(recs.size() == 1 && b->landings_in_flight(1) == 0);
            if (recs.size() == 1) {
                CHECK(recs[0].landing == landing_id && recs[0].galactic_id == key && recs[0].carried_type == squad);
                CHECK(recs[0].units.size() == 4);
                for (const objects::ObjectId u : recs[0].units) CHECK(!b->movement_locked(u));  // unlocked at the finish
            }
            gone = true;
        }
    }
    CHECK(dropped && seen_alive_late && gone);
    if (timed && destroy_at == 0) CHECK(saved);
    std::printf("  landing drop (%s%s): landing %d at frame %u\n", timed ? "timed" : "untimed", destroy_at > 0 ? ", destroyed" : "",
                static_cast<int>(landing_id), drop_frame);
}

// S7 t22 / t25 (FIGHTER_FLYING §4.10 A, TACTICAL_COMBAT §2.8 "Targets of opportunity"): the response-range test 0x4160a0.
void test_fighter_chase_rule() {
    FighterChaseInputs in;
    in.guard_range = 1000.0f;
    in.idle_range = 200.0f;
    in.attack_move_range = 300.0f;
    in.extension = 300.0f;
    in.attack_range = 500.0f;
    in.candidate = {650.0f, 0.0f, 0.0f};
    in.single_position = true;  // arrived at the end of its only (position) block
    CHECK(fighter_chase_test(in) == 0);  // no formation: no chase (its weapons still fire in range)
    in.formation = true;
    CHECK(fighter_chase_test(in) == 1);  // idle: 650 <= Idle_Chase_Range 200 + range 500 from the slot goal
    in.candidate = {750.0f, 0.0f, 0.0f};
    CHECK(fighter_chase_test(in) == 0);  // beyond it
    in.candidate = {150.0f, 0.0f, 0.0f};
    in.single_position = false;
    CHECK(fighter_chase_test(in) == 0);  // a pushed auto block is the last block (or more than one block): no response
    in.moving = true;
    CHECK(fighter_chase_test(in) == 0);  // a plain position move under way: no response range at all (§4.10 A)
    in.flags = 0x01;
    in.candidate = {5000.0f, 0.0f, 0.0f};
    CHECK(fighter_chase_test(in) == 2);  // an attack-move under way responds [M: the squadron is at its own path point]
    in.moving = false;
    in.single_position = true;
    CHECK(fighter_chase_test(in) == 0);  // arrived: rule 7 with Attack_Move_Response_Range
    in.candidate = {790.0f, 0.0f, 0.0f};
    CHECK(fighter_chase_test(in) == 1);  // 790 <= 300 + 500
    in.flags = 0x02;
    in.single_position = false;
    in.last_guard = true;
    in.candidate = {1450.0f, 0.0f, 0.0f};
    CHECK(fighter_chase_test(in) == 1);  // guard: Guard_Chase_Range 1000 + 500
    in.moving = true;
    in.unit = {900.0f, 0.0f, 0.0f};
    CHECK(fighter_chase_test(in) == 1);  // a guard move is not an attack-move: rule 7 while moving, u within Rr of the goal
    in.unit = {1100.0f, 0.0f, 0.0f};
    CHECK(fighter_chase_test(in) == 0);  // ... but not when u itself is farther than Rr from its slot goal
    in.moving = false;
    in.candidate = {1550.0f, 0.0f, 0.0f};
    CHECK(fighter_chase_test(in) == 0);
    in.candidate_attacker = true;  // c attacks the carrier: + Autonomous_Move_Extension_Vs_Attacker, base 1
    CHECK(fighter_chase_test(in) == 2);
    in.candidate = {1250.0f, 0.0f, 0.0f};
    CHECK(fighter_chase_test(in) == 1);  // within Rr 1300
    in.guarded_moving = true;
    CHECK(fighter_chase_test(in) == 1);  // a moving carrier: its attackers are answered ...
    in.candidate_attacker = false;
    in.candidate = {100.0f, 0.0f, 0.0f};
    CHECK(fighter_chase_test(in) == 0);  // ... nothing else
    in.guarded_moving = false;
    CHECK(fighter_chase_test(in) == 1);
    in.candidate_is_goal = true;
    CHECK(fighter_chase_test(in) == 0);  // the first / last block's object: path B, not a chase
    in.candidate_is_goal = false;
    in.flags = 0;
    in.last_guard = false;
    in.single_position = true;
    in.idle_range = -1.0f;  // Rr < 0 -> 0
    in.candidate = {450.0f, 0.0f, 0.0f};
    CHECK(fighter_chase_test(in) == 1);
    in.candidate = {550.0f, 0.0f, 0.0f};
    CHECK(fighter_chase_test(in) == 0);
}

namespace {
struct FighterBattle {
    std::unique_ptr<Data> d;
    std::unique_ptr<jobs::JobSystem> js;
    std::unique_ptr<SimHost> host;
    std::unique_ptr<CampaignBattles> battles;
    galactic::GalacticSim* gs = nullptr;
};

// The S7 t12 fighter battle: the TIE squadron (player 0) joins the S10 space fight; no stand-in orders.
bool start_fighter_battle(FighterBattle& fb, bool fog, bool carrier = false) {
    fb.d = make_data(fog, false, false, false, false, false, false, false, true);
    fb.js = std::make_unique<jobs::JobSystem>(jobs::JobSystemConfig{0});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    fb.host = std::make_unique<SimHost>(fb.d->types, *fb.js, cfg);
    galactic::GalacticSetup gsetup = make_setup(*fb.d, false);
    gsetup.forces.push_back({0, 1, type_of(*fb.d, "S10_TIE_Squadron")});
    if (carrier) gsetup.forces.push_back({0, 1, type_of(*fb.d, "S10_Carrier")});  // S7 t24
    fb.gs = fb.host->start_galactic(fb.d->rules, gsetup);
    CHECK(fb.gs != nullptr);
    if (!fb.gs) return false;
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 6000;
    opt.battle.order_interval = 0;
    opt.battle.fog = fog;
    fb.battles = std::make_unique<CampaignBattles>(*fb.host, fb.d->crules, *fb.d->catalog,
                                                   [](const std::string& name, terrain::MapData& m) {
                                                       if (name != "S10_Space_B") return false;
                                                       m = space_map();
                                                       return true;
                                                   },
                                                   opt);
    CHECK(fb.battles->attach(nullptr));
    const std::vector<uint32_t> uf = fb.gs->fleets_at(2, 1);
    if (!uf.empty()) CHECK(fb.gs->move_fleet(1, uf[0], 1) == commands::RejectReason::None);
    return true;
}
}  // namespace

// S7 t22 (FIGHTER_FLYING §4.9): an auto target moves a squadron only after it is pushed into the squadron's formation. One
// battle, in phases:
//  A. no order yet: no formation. Then an Attack_Move(position) through the nearest enemy: the enemy (the auto target) is
//     pushed while the move is under way (Attack_Move_Response_Range) and the squadron starts its attack run (the PR #19
//     review: such squadrons never did);
//  B. Stop leaves the formation: the squadron idles next to its auto target for 300 frames and never attacks it;
//  C. a plain move to where it is gives it a formation again: the auto target lies within Idle_Chase_Range + range of the
//     slot goal, is pushed and attacked;
//  D. the pushed target turns friendly (Change_Owner): Directed_Combat finds no goal and runs enter_idle, which drops the
//     members' combat targets (the team target, §4.7 step 4) and keeps the formation.
void test_fighter_auto_push() {
    namespace mv = tactical::movement;
    using R = commands::RejectReason;
    FighterBattle fb;
    if (!start_fighter_battle(fb, false)) return;
    enum class Ph { Wait, AttackMove, Stopped, Idle, Converted, Done } ph = Ph::Wait;
    std::vector<objects::ObjectId> members;
    objects::ObjectId squad = objects::kNoObjectId, enemy = objects::kNoObjectId, pushed = objects::kNoObjectId;
    uint32_t since = 0;
    bool no_formation_at_start = false, am_dc = false, am_moving = false, am_pushed = false;
    bool stop_cleared = false, b_auto = false, b_dc = false, c_formation = false, c_dc = false, c_pushed = false;
    bool d_idle = false, d_dropped = false, d_kept = false;
    std::vector<objects::ObjectId> converted;
    mv::LocoState prev = mv::LocoState::None;
    for (uint64_t t = 0; t < 9000 && ph != Ph::Done; ++t) {
        fb.host->tick_once();
        TacticalBattle* b = fb.battles->battle();
        TacticalMode* tm = fb.host->tactical();
        if (!b || !tm) {
            if (ph != Ph::Wait) break;
            continue;
        }
        const objects::ObjectStore& st = tm->primary();
        if (ph == Ph::Wait) {
            if (b->frame() != 3) continue;
            for (const mv::FighterSquad& s : b->movement().fighter_squads())
                if (!s.members.empty() && st.owner(st.find(s.members.front())) == 0) {
                    members = s.members;
                    squad = s.id;
                }
            CHECK(members.size() == 3);
            if (members.empty()) break;
            no_formation_at_start = b->fighter_formation(squad) == nullptr;
            const Vec3 sp = st.position(st.find(members.front()));
            float best = 0.0f;
            for (const objects::ObjectHandle h : st.owned_by(1)) {
                if (!st.alive(h) || !b->movement().has_unit(st.id(h))) continue;
                const Vec3 p = st.position(h);
                const float d2 = (p.x - sp.x) * (p.x - sp.x) + (p.y - sp.y) * (p.y - sp.y);
                if (enemy == objects::kNoObjectId || d2 < best) {
                    enemy = st.id(h);
                    best = d2;
                }
            }
            CHECK(enemy != objects::kNoObjectId);
            if (enemy == objects::kNoObjectId) break;
            const Vec3 ep = st.position(st.find(enemy));
            const float dx = ep.x - sp.x, dy = ep.y - sp.y, len = std::sqrt(dx * dx + dy * dy);
            const float gx = std::clamp(ep.x + dx / len * 1500.0f, -3700.0f, 3700.0f), gy = std::clamp(ep.y + dy / len * 1500.0f, -3700.0f, 3700.0f);
            CHECK(b->order_move(*tm, 0, members, gx, gy, fb.host->random().synced(), mv::kNoFacing, true) == R::None);
            const FighterFormationRecord* r = b->fighter_formation(squad);
            CHECK(r && r->flags == mv::kMoveFlagAttackMove && b->fighter_pushed(squad).empty());
            ph = Ph::AttackMove;
            since = b->frame();
            continue;
        }
        const objects::ObjectId lead = members.front();
        if (!st.alive(st.find(lead)) || !b->movement().has_unit(lead)) break;
        const mv::UnitView v = b->movement().view(lead);
        const FighterFormationRecord* r = b->fighter_formation(squad);
        switch (ph) {
            case Ph::AttackMove:
                if (v.loco_state == mv::LocoState::FighterDirectedCombat) {
                    am_dc = true;
                    am_moving = !v.arrived;
                    am_pushed = r && !b->fighter_pushed(squad).empty();
                    CHECK(b->order_stop(*tm, 0, members) == R::None);
                    stop_cleared = b->fighter_formation(squad) == nullptr;
                    ph = Ph::Stopped;
                    since = b->frame();
                } else if (b->frame() > since + 2500) {
                    ph = Ph::Done;
                }
                break;
            case Ph::Stopped:
                for (const objects::ObjectId m : members) b_auto = b_auto || b->combat().auto_target(m) != objects::kNoObjectId;
                b_dc = b_dc || v.loco_state == mv::LocoState::FighterDirectedCombat;
                if (b->frame() >= since + 300) {
                    Vec3 lp = st.position(st.find(lead));
                    // S8 t17: the attack-move's scan cube R + Rr (FIGHTER_FLYING §4.10 A) engages earlier, so the stop can
                    // leave the squadron beyond R + Idle_Chase_Range of the enemy: the idle move ends 700 from it.
                    if (const objects::ObjectHandle eh = st.find(enemy); st.alive(eh)) {
                        const Vec3 ep = st.position(eh);
                        const float dx = lp.x - ep.x, dy = lp.y - ep.y, d = std::sqrt(dx * dx + dy * dy);
                        if (d > 700.0f) lp = Vec3{ep.x + dx / d * 700.0f, ep.y + dy / d * 700.0f, lp.z};
                    }
                    CHECK(b->order_move(*tm, 0, members, lp.x, lp.y, fb.host->random().synced()) == R::None);
                    const FighterFormationRecord* nr = b->fighter_formation(squad);
                    c_formation = nr && nr->flags == 0 && b->fighter_pushed(squad).empty();
                    ph = Ph::Idle;
                    since = b->frame();
                }
                break;
            case Ph::Idle:
                if (v.loco_state == mv::LocoState::FighterDirectedCombat && r && !b->fighter_pushed(squad).empty()) {
                    c_dc = true;
                    pushed = b->fighter_pushed(squad).back();
                    c_pushed = b->combat().has_unit(pushed);  // a ship (not a squadron)
                    if (c_pushed) CHECK(b->change_owner(*tm, st.find(pushed), 0));
                    if (c_pushed) converted.push_back(pushed);
                    ph = c_pushed ? Ph::Converted : Ph::Done;
                    since = b->frame();
                } else if (b->frame() > since + 600) {
                    ph = Ph::Done;
                }
                break;
            case Ph::Converted:
                if (prev != mv::LocoState::FighterIdle && v.loco_state == mv::LocoState::FighterIdle) {
                    d_idle = true;
                    d_dropped = true;
                    for (const objects::ObjectId m : members)
                        d_dropped = d_dropped && std::find(converted.begin(), converted.end(), b->combat().current_target(m)) == converted.end();
                    d_kept = r != nullptr;
                    ph = Ph::Done;
                } else if (b->frame() > since + 120) {
                    ph = Ph::Done;
                }
                // A ship pushed after the conversion (a second enemy in reach) turns friendly too, so Directed_Combat runs out
                // of goals (S8 t10-t12 shot timing brought one more frigate into reach).
                if (ph == Ph::Converted && r)
                    for (const objects::ObjectId k : b->fighter_pushed(squad))
                        if (std::find(converted.begin(), converted.end(), k) == converted.end() && b->combat().has_unit(k) &&
                            b->combat().owner(k) != 0) {
                            CHECK(b->change_owner(*tm, st.find(k), 0));
                            converted.push_back(k);
                        }
                break;
            default: break;
        }
        prev = v.loco_state;
    }
    std::printf("  auto push: squadron %d, enemy %d; attack-move: Directed_Combat %d (moving %d, pushed %d); stop: formation gone %d, "
                "auto target %d, Directed_Combat %d; idle move: formation %d, Directed_Combat %d (pushed %d); converted: idle %d, "
                "targets dropped %d, formation kept %d\n",
                squad, enemy, am_dc ? 1 : 0, am_moving ? 1 : 0, am_pushed ? 1 : 0, stop_cleared ? 1 : 0, b_auto ? 1 : 0, b_dc ? 1 : 0,
                c_formation ? 1 : 0, c_dc ? 1 : 0, pushed, d_idle ? 1 : 0, d_dropped ? 1 : 0, d_kept ? 1 : 0);
    CHECK(no_formation_at_start);
    CHECK(am_dc && am_moving && am_pushed);
    CHECK(stop_cleared && b_auto && !b_dc);
    CHECK(c_formation && c_dc && c_pushed);
    CHECK(d_idle && d_dropped && d_kept);
}

// S7 t24 (STATION_HANGARS_SOUND §2.5, A1_READINGS §33.4, FIGHTER_FLYING §4.9): a moving carrier's launch is a guard of the
// carrier. The launched squadron's formation record is {flags 0x02, goal object = the carrier} with a host follow, so its
// chase range is Guard_Chase_Range around the carrier; when the carrier moves away, the squadron re-paths to it (at most
// once per 30 frames, when the carrier is more than half the leader's attack range from the recorded goal) and keeps the
// guard record (flags, goal object). S7 t28 / t35 (A1_READINGS §33.4.1): while the carrier moves the squadron, within
// R = min(range / 2, Space_Guard_Range 750) of it, flies a copy of its path at most twice its speed (s78f t02: only while
// the squadron's formation moves, FIGHTER_FLYING §4.11 A; the leader's goal reaches the carrier's destination). A new
// order of the squadron ends the follow (a plain move record, a position block).
void test_hangar_escort_guard() {
    namespace mv = tactical::movement;
    using R = commands::RejectReason;
    FighterBattle fb;
    if (!start_fighter_battle(fb, false, true)) return;
    const uint32_t carrier_type = type_of(*fb.d, "S10_Carrier");
    objects::ObjectId carrier = objects::kNoObjectId, squad = objects::kNoObjectId;
    std::vector<objects::ObjectId> members;
    bool guard_record = false, followed = false, kept = true, ended = false, capped = false, copied = false;
    uint32_t launch_frame = 0, move_frame = 0, follow_frame = 0, copy_frame = 0;
    mv::V2 dest{};
    for (uint64_t t = 0; t < 5000; ++t) {
        fb.host->tick_once();
        TacticalBattle* b = fb.battles->battle();
        TacticalMode* tm = fb.host->tactical();
        if (!b || !tm) {
            if (carrier != objects::kNoObjectId) break;
            continue;
        }
        const objects::ObjectStore& st = tm->primary();
        if (carrier == objects::kNoObjectId) {
            for (const objects::ObjectHandle h : st.owned_by(0))
                if (st.alive(h) && st.type(h) == carrier_type) carrier = st.id(h);
            if (carrier == objects::kNoObjectId) continue;
        }
        if (!st.alive(st.find(carrier))) break;
        const Vec3 cp = st.position(st.find(carrier));
        if (squad == objects::kNoObjectId) {
            for (const mv::FighterSquad& s : b->movement().fighter_squads()) {
                const FighterFormationRecord* r = b->fighter_formation(s.id);
                if (!s.members.empty() && r && r->goal_object == carrier) {
                    squad = s.id;
                    members = s.members;
                    // S7 t37 / s78f t02: the movement layer's formation holds the guard block as its only block.
                    const mv::FighterFormation* fg = b->movement().fighter_formation(s.id);
                    guard_record = r->flags == mv::kMoveFlagGuardMove && b->fighter_pushed(s.id).empty() && fg && fg->blocks.size() == 1 &&
                                   fg->blocks[0].kind == mv::kFighterBlockObject && fg->blocks[0].object == carrier &&
                                   fg->blocks[0].flags == mv::kMoveFlagGuardMove && fg->next > b->frame() && fg->follow;
                }
            }
            if (squad == objects::kNoObjectId) {
                if (b->frame() > 300) break;
                continue;
            }
            launch_frame = b->frame();
            // The carrier moves 1500 away (inside the playable box); the escort is not ordered.
            dest = {cp.x, cp.y + (cp.y > 0.0f ? -1500.0f : 1500.0f)};
            const objects::ObjectId cid[1] = {carrier};
            CHECK(b->order_move(*tm, 0, cid, dest.x, dest.y, fb.host->random().synced()) == R::None);
            move_frame = b->frame();
            continue;
        }
        const FighterFormationRecord* r = b->fighter_formation(squad);
        if (!r || members.empty() || !b->movement().has_unit(members.front())) break;
        kept = kept && r->flags == mv::kMoveFlagGuardMove && r->goal_object == carrier;
        const mv::UnitView v = b->movement().view(members.front());
        // S7 t28 / s78f t02: while the carrier moves, the squadron formation's speed (F +0x21c) is capped at min(its own speed,
        // 2 x the carrier's speed) and, its centre within R of the carrier, it flies a copy of the carrier's path (its goal =
        // the carrier's goal point).
        const mv::UnitView cv = b->movement().view(carrier);
        const bool c_moving = cv.has_goal && !cv.arrived;
        if (const mv::FighterFormation* ff = b->movement().fighter_formation(squad);
            ff && c_moving && ff->speed >= 0.0f && ff->speed <= 2.0f * cv.max_speed)
            capped = true;
        if (!copied && c_moving && v.has_goal && v.goal.x == cv.goal.x && v.goal.y == cv.goal.y) {
            copied = true;
            copy_frame = b->frame();
        }
        // s78f t02: the follow is the path copy / re-path of the formation (FIGHTER_FLYING §4.11 A): the leader's goal moved
        // with the carrier to its destination while the guard block stays the formation's only block.
        const mv::FighterFormation* ff = b->movement().fighter_formation(squad);
        const bool guard_kept = ff && ff->blocks.size() == 1 && ff->blocks[0].object == carrier;
        const float gx = v.goal.x - dest.x, gy = v.goal.y - dest.y;
        if (!followed && guard_kept && v.has_goal && gx * gx + gy * gy < 600.0f * 600.0f) {
            followed = true;
            follow_frame = b->frame();
            // A plain move order of the squadron replaces the guard: no follow any more.
            const Vec3 lp = st.position(st.find(members.front()));
            CHECK(b->order_move(*tm, 0, members, lp.x, lp.y, fb.host->random().synced()) == R::None);
            const FighterFormationRecord* nr = b->fighter_formation(squad);
            const mv::FighterFormation* nf = b->movement().fighter_formation(squad);
            ended = nr && nr->flags == 0 && nr->goal_object == objects::kNoObjectId && nf && nf->blocks.size() == 1 &&
                    nf->blocks[0].kind == mv::kFighterBlockPosition && (nf->blocks[0].flags & mv::kMoveFlagGuardMove) == 0;
            break;
        }
        if (b->frame() > move_frame + 3000) break;
    }
    std::printf("  escort guard: carrier %d squadron %d launched frame %u (guard record %d); carrier moved frame %u to (%.0f, %.0f); "
                "followed %d (frame %u), speed cap %d, path copy %d (frame %u), guard kept %d, new order ends the follow %d\n",
                carrier, squad, launch_frame, guard_record ? 1 : 0, move_frame, static_cast<double>(dest.x), static_cast<double>(dest.y),
                followed ? 1 : 0, follow_frame, capped ? 1 : 0, copied ? 1 : 0, copy_frame, kept ? 1 : 0, ended ? 1 : 0);
    CHECK(carrier != objects::kNoObjectId && squad != objects::kNoObjectId && members.size() == 3);
    CHECK(guard_record);
    CHECK(followed && kept && ended);
    CHECK(capped && copied);
}

// s78f review (FIGHTER_FLYING §4.11 B 1 / B 3, 0x4160a0): host and movement stack agree around a shift waypoint: a
// launched escort (guard of its carrier) gets a shift-queued waypoint while the carrier moves: the waypoint goes to
//     the bottom of the movement stack and the host record still mirrors the guard on top (flags 0x02, goal object = the
//     carrier; Rr from the first block = the waypoint's); once the carrier stops the guard pops and the record follows the
//     waypoint (a position block: no guard, no object, goal = the waypoint).
void test_escort_waypoint() {
    namespace mv = tactical::movement;
    using R = commands::RejectReason;
    FighterBattle fb;
    if (!start_fighter_battle(fb, false, true)) return;
    const uint32_t carrier_type = type_of(*fb.d, "S10_Carrier");
    objects::ObjectId carrier = objects::kNoObjectId, squad = objects::kNoObjectId;
    std::vector<objects::ObjectId> members;
    mv::V2 wp{};
    bool queued = false, guard_while_moving = false, still_guard = true, popped = false, follows_wp = false;
    uint32_t pop_frame = 0, stop_frame = 0, guard_frames = 0;
    for (uint64_t t = 0; t < 6000; ++t) {
        fb.host->tick_once();
        TacticalBattle* b = fb.battles->battle();
        TacticalMode* tm = fb.host->tactical();
        if (!b || !tm) {
            if (carrier != objects::kNoObjectId) break;
            continue;
        }
        const objects::ObjectStore& st = tm->primary();
        if (carrier == objects::kNoObjectId) {
            for (const objects::ObjectHandle h : st.owned_by(0))
                if (st.alive(h) && st.type(h) == carrier_type) carrier = st.id(h);
            if (carrier == objects::kNoObjectId) continue;
        }
        if (!st.alive(st.find(carrier))) break;
        const Vec3 cp = st.position(st.find(carrier));
        if (squad == objects::kNoObjectId) {
            for (const mv::FighterSquad& s : b->movement().fighter_squads())
                if (const FighterFormationRecord* r = b->fighter_formation(s.id); !s.members.empty() && r && r->goal_object == carrier) {
                    squad = s.id;
                    members = s.members;
                }
            if (squad == objects::kNoObjectId) {
                if (b->frame() > 300) break;
                continue;
            }
            const objects::ObjectId cid[1] = {carrier};
            CHECK(b->order_move(*tm, 0, cid, cp.x, cp.y + (cp.y > 0.0f ? -1200.0f : 1200.0f), fb.host->random().synced()) == R::None);
            wp = {cp.x + (cp.x > 0.0f ? -1500.0f : 1500.0f), cp.y};
            CHECK(b->order_move(*tm, 0, members, wp.x, wp.y, fb.host->random().synced(), mv::kNoFacing, false, true) == R::None);
            const mv::FighterFormation* ff = b->movement().fighter_formation(squad);
            queued = ff && ff->blocks.size() == 2 && ff->blocks[0].kind == mv::kFighterBlockPosition && ff->blocks[1].object == carrier &&
                     (ff->blocks[1].flags & mv::kMoveFlagGuardMove) != 0;
            continue;
        }
        if (members.empty() || !b->movement().has_unit(members.front())) break;
        const FighterFormationRecord* r = b->fighter_formation(squad);
        const mv::FighterFormation* ff = b->movement().fighter_formation(squad);
        if (!r || !ff || ff->blocks.empty()) break;
        const mv::UnitView cv = b->movement().view(carrier);
        const bool c_moving = cv.has_goal && !cv.arrived;
        if (!c_moving && stop_frame == 0) stop_frame = b->frame();
        const bool guard_top = ff->blocks.back().kind == mv::kFighterBlockObject && ff->blocks.back().object == carrier &&
                               (ff->blocks.back().flags & mv::kMoveFlagGuardMove) != 0;
        if (!popped && guard_top) {
            const FighterOrderView ov = b->fighter_order_view(*r);
            const bool g = r->flags == mv::kMoveFlagGuardMove && r->goal_object == carrier && ov.first_flags == 0;
            still_guard = still_guard && g;
            if (c_moving) {
                guard_while_moving = guard_while_moving || g;
                ++guard_frames;
            }
        }
        if (!popped && ff->blocks.size() == 1 && ff->blocks[0].kind == mv::kFighterBlockPosition) {
            popped = true;
            pop_frame = b->frame();
        }
        if (popped && !follows_wp && b->frame() > pop_frame) {
            follows_wp = r->flags == 0 && r->goal_object == objects::kNoObjectId && r->goal.x == ff->blocks[0].point.x &&
                         r->goal.y == ff->blocks[0].point.y && std::abs(r->goal.x - wp.x) < 1.0f && std::abs(r->goal.y - wp.y) < 1.0f;
            break;
        }
        if (b->frame() > 4000) break;
    }
    std::printf("  escort + shift waypoint: squadron %d queued below the guard %d, guard record while the carrier moves %d "
                "(%u frames, kept %d), carrier stopped frame %u, guard popped %d (frame %u), record follows the waypoint %d\n",
                squad, queued ? 1 : 0, guard_while_moving ? 1 : 0, guard_frames, still_guard ? 1 : 0, stop_frame, popped ? 1 : 0,
                pop_frame, follows_wp ? 1 : 0);
    CHECK(squad != objects::kNoObjectId && queued && guard_while_moving && still_guard);
    CHECK(popped && stop_frame > 0 && pop_frame <= stop_frame + 2u * mv::kFighterGuardInterval + 1u && follows_wp);
}

// s78f review (FIGHTER_FLYING §4.11 B 1 / B 3 "new group move"): an Attack on an enemy already in range (exec_attack moves
// nobody) still replaces the squadron's whole stack (a launch guard with a shift waypoint below it) by the object block.
void test_escort_attack_in_range() {
    namespace mv = tactical::movement;
    using R = commands::RejectReason;
    FighterBattle fb;
    if (!start_fighter_battle(fb, false, true)) return;
    const uint32_t carrier_type = type_of(*fb.d, "S10_Carrier");
    objects::ObjectId carrier = objects::kNoObjectId, squad = objects::kNoObjectId, enemy = objects::kNoObjectId;
    bool two = false, attack_stack = false, attack_record = false, in_range = false;
    for (uint64_t t = 0; t < 3000 && squad == objects::kNoObjectId; ++t) {
        fb.host->tick_once();
        TacticalBattle* b = fb.battles->battle();
        TacticalMode* tm = fb.host->tactical();
        if (!b || !tm) continue;
        const objects::ObjectStore& st = tm->primary();
        if (carrier == objects::kNoObjectId)
            for (const objects::ObjectHandle h : st.owned_by(0))
                if (st.alive(h) && st.type(h) == carrier_type) carrier = st.id(h);
        if (carrier == objects::kNoObjectId) continue;
        std::vector<objects::ObjectId> members;
        for (const mv::FighterSquad& s : b->movement().fighter_squads())
            if (const FighterFormationRecord* r = b->fighter_formation(s.id); !s.members.empty() && r && r->goal_object == carrier) {
                squad = s.id;
                members = s.members;
            }
        if (squad == objects::kNoObjectId) {
            if (b->frame() > 300) break;
            continue;
        }
        const Vec3 cp = st.position(st.find(carrier));
        CHECK(b->order_move(*tm, 0, members, cp.x + 900.0f, cp.y, fb.host->random().synced(), mv::kNoFacing, false, true) == R::None);
        const mv::FighterFormation* ff = b->movement().fighter_formation(squad);
        two = ff && ff->blocks.size() == 2 && ff->blocks[1].object == carrier;
        for (const objects::ObjectHandle h : st.owned_by(1))
            if (st.alive(h) && b->movement().has_unit(st.id(h)) && b->combat().has_unit(st.id(h)) &&
                b->movement().view(st.id(h)).locomotor != mv::LocomotorKind::Fighter) {
                enemy = st.id(h);
                break;
            }
        CHECK(enemy != objects::kNoObjectId);
        if (enemy == objects::kNoObjectId) break;
        const Vec3 ep = st.position(st.find(enemy));
        in_range = true;
        for (size_t k = 0; k < members.size(); ++k) {
            const Vec3 at{ep.x + 15.0f * static_cast<float>(k + 1), ep.y + 20.0f, ep.z};
            CHECK(b->teleport(*tm, members[k], at, Vec3{1, 0, 0}));
            const float dx = at.x - ep.x, dy = at.y - ep.y;
            in_range = in_range && std::sqrt(dx * dx + dy * dy) <= 80.0f;  // exec_attack: inside 0.8 x max(range, 100)
        }
        CHECK(b->order_attack(*tm, 0, members, enemy, fb.host->random().synced()) == R::None);
        const mv::FighterFormation* af = b->movement().fighter_formation(squad);
        attack_stack = af && af->blocks.size() == 1 && af->blocks[0].kind == mv::kFighterBlockObject && af->blocks[0].object == enemy &&
                       af->blocks[0].flags == 0 && !af->blocks[0].pushed;
        const FighterFormationRecord* ar = b->fighter_formation(squad);
        attack_record = ar && ar->flags == 0 && ar->goal_object == enemy;
    }
    std::printf("  in-range attack: squadron %d (guard + waypoint stack %d), enemy %d in range %d: stack replaced by the object "
                "block %d (record %d)\n",
                squad, two ? 1 : 0, enemy, in_range ? 1 : 0, attack_stack ? 1 : 0, attack_record ? 1 : 0);
    CHECK(squad != objects::kNoObjectId && two && in_range && attack_stack && attack_record);
}

// s78f t10: the AI order paths give the movement layer the same block as the host record. A Lua Guard_Target of the
// carrier by the squadron (BattleAi, as the tactical AI's group order): the movement stack holds one guard object block
// (flags 0x02, the carrier) and the record matches; the carrier then moves and the squadron's formation keeps that block
// (the follow service no longer re-issues a flags-0 position move to fighters with an object block). Then an AI Attack on
// an enemy already in range replaces the stack by the object block (as the human exec_attack does).
void test_ai_fighter_blocks() {
    namespace mv = tactical::movement;
    using R = commands::RejectReason;
    FighterBattle fb;
    if (!start_fighter_battle(fb, false, true)) return;
    const uint32_t carrier_type = type_of(*fb.d, "S10_Carrier");
    ai::AiDefinitions defs;
    ai::EquationSet eqs;
    TacticalAiSetup setup;
    setup.defs = &defs;
    setup.equations = &eqs;
    setup.files = fb.d->files.get();
    setup.ai_type = [](int32_t, const galactic::GalacticSim&) { return std::string(); };  // no AI player: orders only
    objects::ObjectId carrier = objects::kNoObjectId, squad = objects::kNoObjectId, enemy = objects::kNoObjectId;
    std::vector<objects::ObjectId> members;
    std::unique_ptr<BattleAi> ai;
    bool guard_stack = false, guard_record = false, kept = true, carrier_moved = false, attack_stack = false, attack_record = false;
    uint32_t guard_frame = 0, kept_frames = 0;
    Vec3 c0{};
    for (uint64_t t = 0; t < 6000; ++t) {
        fb.host->tick_once();
        TacticalBattle* b = fb.battles->battle();
        TacticalMode* tm = fb.host->tactical();
        if (!b || !tm) {
            if (carrier != objects::kNoObjectId) break;
            continue;
        }
        const objects::ObjectStore& st = tm->primary();
        if (carrier == objects::kNoObjectId) {
            for (const objects::ObjectHandle h : st.owned_by(0))
                if (st.alive(h) && st.type(h) == carrier_type) carrier = st.id(h);
            if (carrier == objects::kNoObjectId) continue;
        }
        if (!st.alive(st.find(carrier))) break;
        if (squad == objects::kNoObjectId) {
            for (const mv::FighterSquad& s : b->movement().fighter_squads())
                if (!s.members.empty() && b->fighter_formation(s.id)) {
                    squad = s.id;
                    members = s.members;
                }
            if (squad == objects::kNoObjectId) {
                if (b->frame() > 300) break;
                continue;
            }
            ai = std::make_unique<BattleAi>(*b, *tm, *fb.host, setup);
            const Vec3 cp = st.position(st.find(carrier));
            c0 = cp;
            std::vector<uint8_t> pay;
            auto put = [&pay](uint32_t v) {
                for (int i = 0; i < 4; ++i) pay.push_back(static_cast<uint8_t>(v >> (8 * i)));
            };
            put(static_cast<uint32_t>(members.front()));
            put(static_cast<uint32_t>(carrier));
            put(std::bit_cast<uint32_t>(cp.x));
            put(std::bit_cast<uint32_t>(cp.y));
            put(std::bit_cast<uint32_t>(cp.z));
            put(0u);
            for (size_t i = 1; i < members.size(); ++i) put(static_cast<uint32_t>(members[i]));
            const auto sub = static_cast<uint16_t>(lua::kLuaOrderSubtype + static_cast<uint16_t>(lua::LuaOrder::Guard));
            CHECK(ai->execute(0, commands::ev::port_extension(sub, pay)) == R::None);
            const mv::FighterFormation* ff = b->movement().fighter_formation(squad);
            guard_stack = ff && ff->blocks.size() == 1 && ff->blocks[0].kind == mv::kFighterBlockObject && ff->blocks[0].object == carrier &&
                          ff->blocks[0].flags == mv::kMoveFlagGuardMove;
            const FighterFormationRecord* r = b->fighter_formation(squad);
            guard_record = r && r->flags == mv::kMoveFlagGuardMove && r->goal_object == carrier;
            guard_frame = b->frame();
            const objects::ObjectId cid[1] = {carrier};
            CHECK(b->order_move(*tm, 0, cid, cp.x, cp.y + (cp.y > 0.0f ? -1500.0f : 1500.0f), fb.host->random().synced()) == R::None);
            continue;
        }
        // The follow service (a1h t02) runs in update(); the stand-in frames (every 60th, BattleAi::standin_orders: plain moves
        // of the uncontrolled players' units) are skipped so only the follow re-issues moves.
        if ((b->frame() - 1u) % 60u != 0u) ai->update();
        if (b->frame() < guard_frame + 600u) {
            const mv::FighterFormation* ff = b->movement().fighter_formation(squad);
            const bool g = ff && !ff->blocks.empty() && ff->blocks[0].kind == mv::kFighterBlockObject && ff->blocks[0].object == carrier &&
                           (ff->blocks[0].flags & mv::kMoveFlagGuardMove) != 0;
            kept = kept && g;
            ++kept_frames;
            continue;
        }
        const Vec3 cp = st.position(st.find(carrier));
        carrier_moved = std::abs(cp.y - c0.y) > 300.0f;
        for (const objects::ObjectHandle h : st.owned_by(1))
            if (st.alive(h) && b->movement().has_unit(st.id(h)) && b->combat().has_unit(st.id(h)) &&
                b->movement().view(st.id(h)).locomotor != mv::LocomotorKind::Fighter) {
                enemy = st.id(h);
                break;
            }
        CHECK(enemy != objects::kNoObjectId);
        if (enemy == objects::kNoObjectId) break;
        const Vec3 ep = st.position(st.find(enemy));
        for (size_t k = 0; k < members.size(); ++k)
            CHECK(b->teleport(*tm, members[k], Vec3{ep.x + 15.0f * static_cast<float>(k + 1), ep.y + 20.0f, ep.z}, Vec3{1, 0, 0}));
        std::vector<uint32_t> wire;
        for (const objects::ObjectId m : members) wire.push_back(static_cast<uint32_t>(m));
        CHECK(ai->execute(0, commands::ev::attack(static_cast<uint32_t>(enemy), wire)) == R::None);
        const mv::FighterFormation* af = b->movement().fighter_formation(squad);
        attack_stack = af && af->blocks.size() == 1 && af->blocks[0].kind == mv::kFighterBlockObject && af->blocks[0].object == enemy &&
                       af->blocks[0].flags == 0;
        const FighterFormationRecord* ar = b->fighter_formation(squad);
        attack_record = ar && ar->flags == 0 && ar->goal_object == enemy;
        break;
    }
    std::printf("  AI orders: squadron %d, Lua guard of the carrier: stack %d record %d, kept while the carrier moves %d "
                "(%u frames, moved %d); AI attack in range on %d: stack %d record %d\n",
                squad, guard_stack ? 1 : 0, guard_record ? 1 : 0, kept ? 1 : 0, kept_frames, carrier_moved ? 1 : 0, enemy,
                attack_stack ? 1 : 0, attack_record ? 1 : 0);
    CHECK(squad != objects::kNoObjectId && guard_stack && guard_record && kept && kept_frames > 0 && carrier_moved);
    CHECK(attack_stack && attack_record);
}

// s78f review (FIGHTER_FLYING §4.11 B 1, §4.10 E): an auto block on a station (a non-mover: no movement unit) is dropped
// when the station dies - the death reaches MovementSystem::object_removed, and the position hook reads only live objects.
// The player-0 star base is handed to player 1 so player 0's TIE squadron, ordered next to it, auto-attacks it; player 0's
// frigate and corvette are ordered to attack it too so it dies inside the test.
void test_fighter_station_dies() {
    namespace mv = tactical::movement;
    using R = commands::RejectReason;
    FighterBattle fb;
    if (!start_fighter_battle(fb, false)) return;
    const uint32_t sb_type = type_of(*fb.d, "S10_Star_Base_1");
    objects::ObjectId squad = objects::kNoObjectId, station = objects::kNoObjectId;
    std::vector<objects::ObjectId> members;
    bool pushed = false, died = false, dropped = false, record_dropped = false;
    uint32_t push_frame = 0, death_frame = 0;
    for (uint64_t t = 0; t < 9000; ++t) {
        fb.host->tick_once();
        TacticalBattle* b = fb.battles->battle();
        TacticalMode* tm = fb.host->tactical();
        if (!b || !tm) {
            if (squad != objects::kNoObjectId) break;
            continue;
        }
        const objects::ObjectStore& st = tm->primary();
        if (squad == objects::kNoObjectId) {
            if (b->frame() != 3) continue;
            for (const mv::FighterSquad& s : b->movement().fighter_squads())
                if (!s.members.empty() && st.owner(st.find(s.members.front())) == 0) {
                    squad = s.id;
                    members = s.members;
                }
            std::vector<objects::ObjectId> ships;
            for (const objects::ObjectHandle h : st.owned_by(0)) {
                if (!st.alive(h)) continue;
                if (st.type(h) == sb_type) station = st.id(h);
                else if (b->movement().has_unit(st.id(h)) && b->movement().view(st.id(h)).locomotor != mv::LocomotorKind::Fighter)
                    ships.push_back(st.id(h));
            }
            CHECK(squad != objects::kNoObjectId && station != objects::kNoObjectId);
            if (squad == objects::kNoObjectId || station == objects::kNoObjectId) break;
            CHECK(!b->movement().has_unit(station));  // a non-mover
            CHECK(b->change_owner(*tm, st.find(station), 1));
            const Vec3 sp = st.position(st.find(station));
            for (size_t k = 0; k < members.size(); ++k)
                CHECK(b->teleport(*tm, members[k], Vec3{sp.x + 500.0f + 40.0f * static_cast<float>(k), sp.y, sp.z}, Vec3{-1, 0, 0}));
            CHECK(b->order_move(*tm, 0, members, sp.x + 500.0f, sp.y, fb.host->random().synced()) == R::None);
            if (!ships.empty()) CHECK(b->order_attack(*tm, 0, ships, station, fb.host->random().synced()) == R::None);
            continue;
        }
        const objects::ObjectHandle sh = st.find(station);
        const bool alive = st.alive(sh);
        if (alive) {
            if (!pushed)
                for (const objects::ObjectId k : b->fighter_pushed(squad))
                    if (k == station) {
                        pushed = true;
                        push_frame = b->frame();
                    }
            if (b->frame() > 8000) break;
            continue;
        }
        died = true;
        death_frame = b->frame();
        dropped = true;
        if (const mv::FighterFormation* ff = b->movement().fighter_formation(squad))
            for (const mv::FighterBlock& bl : ff->blocks) dropped = dropped && !(bl.kind == mv::kFighterBlockObject && bl.object == station);
        const FighterFormationRecord* r = b->fighter_formation(squad);
        record_dropped = !r || r->goal_object != station;
        break;
    }
    std::printf("  station dies: squadron %d, station %d; auto block on it %d (frame %u), died %d (frame %u), blocks dropped "
                "at once %d, record %d\n",
                squad, station, pushed ? 1 : 0, push_frame, died ? 1 : 0, death_frame, dropped ? 1 : 0, record_dropped ? 1 : 0);
    CHECK(pushed && died && dropped && record_dropped);
}

#ifdef ALAMO_SIM_HAVE_FOG
// S7 t22 (FIGHTER_FLYING §4.9 step 3): a goal hidden by fog for the squadron's player is still its formation goal, so the
// squadron toggles Idle <-> Directed_Combat every frame (Directed_Combat: fogged -> NoTarget -> enter_idle; Idle: a goal
// that is not dying -> Directed_Combat), as in retail. Here the goal is an ordered Attack on an enemy frigate that no
// player-0 REVEAL source sees (the fighters reveal nothing); enter_idle keeps the order's object block.
// s78f review: the Attack's movement formation now holds the object block (exec_attack, §4.11 B 1), so after arrival the
// hidden-goal rule C 3 re-plans it toward the object (d >= 40: state 0, the plan's move, state 1, the members done at
// once, state 3 again): every third frame is a plan frame, so the state changes twice per three frames.
void test_fighter_fog_toggle() {
    namespace mv = tactical::movement;
    using R = commands::RejectReason;
    FighterBattle fb;
    if (!start_fighter_battle(fb, true)) return;
    std::vector<objects::ObjectId> members;
    objects::ObjectId squad = objects::kNoObjectId, enemy = objects::kNoObjectId;
    bool issued = false, stayed_hidden = true, order_kept = true;
    uint32_t settle = 0, window = 0, toggles = 0;
    mv::LocoState prev = mv::LocoState::None;
    for (uint64_t t = 0; t < 6000; ++t) {
        fb.host->tick_once();
        TacticalBattle* b = fb.battles->battle();
        TacticalMode* tm = fb.host->tactical();
        if (!b || !tm) {
            if (issued) break;
            continue;
        }
        const objects::ObjectStore& st = tm->primary();
        if (!issued) {
            if (b->frame() != 3) continue;
            issued = true;
            for (const mv::FighterSquad& s : b->movement().fighter_squads())
                if (!s.members.empty() && st.owner(st.find(s.members.front())) == 0) {
                    members = s.members;
                    squad = s.id;
                }
            const Vec3 sp = members.empty() ? Vec3{} : st.position(st.find(members.front()));
            float best = 0.0f;
            for (const objects::ObjectHandle h : st.owned_by(1)) {
                if (!st.alive(h) || !b->movement().has_unit(st.id(h)) || !b->fog_object_hidden(0, st.id(h), true)) continue;
                const Vec3 p = st.position(h);
                const float d2 = (p.x - sp.x) * (p.x - sp.x) + (p.y - sp.y) * (p.y - sp.y);
                if (enemy == objects::kNoObjectId || d2 < best) {
                    enemy = st.id(h);
                    best = d2;
                }
            }
            CHECK(members.size() == 3 && enemy != objects::kNoObjectId);
            if (members.empty() || enemy == objects::kNoObjectId) break;
            CHECK(b->order_attack(*tm, 0, members, enemy, fb.host->random().synced()) == R::None);
            continue;
        }
        const objects::ObjectId lead = members.front();
        if (!st.alive(st.find(lead)) || !st.alive(st.find(enemy))) break;
        const mv::UnitView v = b->movement().view(lead);
        if (settle == 0 && !v.has_goal && v.loco_state != mv::LocoState::FighterMoving) settle = b->frame();
        if (settle != 0 && b->frame() > settle + 5) {
            ++window;
            if (v.loco_state != prev) ++toggles;
            stayed_hidden = stayed_hidden && b->fog_object_hidden(0, enemy, true);
            order_kept = order_kept && b->combat().ordered_target(lead) == enemy;
            if (window >= 60) break;
        }
        prev = v.loco_state;
    }
    const FighterFormationRecord* r = nullptr;
    if (TacticalBattle* b = fb.battles->battle()) r = b->fighter_formation(squad);
    std::printf("  fog toggle: squadron %d attacks hidden enemy %d; settled at %u, %u state changes in %u frames, hidden %d, order kept %d\n",
                squad, enemy, settle, toggles, window, stayed_hidden ? 1 : 0, order_kept ? 1 : 0);
    CHECK(window >= 60 && stayed_hidden && order_kept && toggles >= 38);
    CHECK(r && r->goal_object == enemy);
}
#endif

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--regen") g_regen = true;
    auto d = make_data();
    std::printf("space battle\n");
    test_space_battle(*d);
    std::printf("land battle\n");
    test_land_battle(*d);
    std::printf("retreat\n");
    test_retreat(*d);
    std::printf("auto-resolve only\n");
    test_auto_only(*d);
    std::printf("story block freezes the conflict state machines and the galactic frame (a1j t05)\n");
    test_story_block_host(*d);
    std::printf("human orders + order queue (integration fixes, [port: Gabriel])\n");
    test_orders_and_queue(*d);
#ifdef ALAMO_SIM_HAVE_STORY
    std::printf("story battle (L1/S4 wave 2a)\n");
    test_story_battle(*d);
    std::printf("linked battle flag (REMAINING_GAPS 6.4)\n");
    test_linked_battle(*d);
    test_linked_params(*d);  // story t07
    test_linked_map_objects(*d);  // story t10
    std::printf("story Invade() of a scripted fleet (a1f t08, S10 PROBE A)\n");
    test_probe_invade(*d);
    test_invade_planet_reward(*d);
    std::printf("S4 story battles: orders, garrisons, capture, map load, corruption missions\n");
    {
        auto ds4 = make_data(false, false, true);  // S4
        test_s4_battle(*ds4);
    }
#endif
#ifdef ALAMO_SIM_HAVE_FOG
    std::printf("fog of war battles\n");
    {
        auto dfog = make_data(true);
        test_fog_battle(*dfog);
    }
    std::printf("fog of war: Battlefield_Modifier reveal multiplier (FOW t01)\n");
    {
        auto dbm = make_data(true, false, false, false, false, false, false, true);
        test_fog_bm_battle(*dbm);
    }
    std::printf("fog of war: land setup phase manager (FOWF t05)\n");
    {
        auto dsetup = make_data(false, false, false, false, false, false, false, false, false, true);
        test_fog_setup_battle(*dsetup);
    }
#endif
#ifdef ALAMO_SIM_HAVE_ANIM
    std::printf("anim battles (R3: flinch, death clips, corpses)\n");
    test_anim_battles();
    std::printf("death clone removal, headless and with the anim service (refixf t04)\n");
    test_death_clone_removal();
    std::printf("s78k t05: slot 47 tests the clip before the draw (animation world)\n");
    test_attack_idle_clip_order();
#endif
    std::printf("black market gate (P4 tactical)\n");
    test_black_market_battle();
    std::printf("battle host BT pass: land split, HIJACK reserve, ResumeGame, map picker\n");
    test_bt_pass(*d);
    std::printf("S10 map owners: faction lookup, Hostile natives, attacker landing point [probe 2026-10-29]\n");
    {
        auto dmo = make_data(false, false, true);
        test_map_owners(*dmo);
        test_campaign_hostile(*dmo);
    }
    std::printf("skirmish station production: Production at the star base, upgrade object, station upgrade\n");
    test_station_production();
#ifdef ALAMO_SIM_HAVE_TACTICAL_AI
    std::printf("OI host: BattleAi MoveToObject, hijack_unit, AI Change_Owner\n");
    test_battle_ai_orders(*d);
    std::printf("refixf t02: SPACE_OBSTACLE station as a static entry, per-unit Lua orders as one group move\n");
    test_space_obstacle_orders();
    test_neutral_attack_move();  // S7 t29 / t30
    test_battle_ai_station_level(false);
    test_battle_ai_station_level(true);
    test_battle_ai_land_mode(false);
    test_battle_ai_land_mode(true);
    std::printf("refixf t03: Find_All_Objects_Of_Type('S10_Squad') finds the TEAM containers (representative type hook)\n");
    test_battle_ai_find_team();
    std::printf("a1g t04 follow-up: BattleAi::land_drop_ok (impassable / fogged / movers / enabling radius)\n");
    test_battle_ai_land_drop(*d);
#endif
    std::printf("a1g t04 follow-up: land company drop layout, oriented boxes, reinforcement radii\n");
    test_land_drop_rules();
    std::printf("s78f t07: land team members registered with combat (TeamTargeting copy in a battle)\n");
    test_land_team_targeting();
    std::printf("s78j t03 / t04: land team SPEED = the container's value; Change_Owner spreads through the team; reserve drop facing\n");
    test_team_speed_and_owner();
    test_reserve_drop_facing(true);
    std::printf("s78k t04 / t05: convert spreads to the team container; reserve drop facing != the side's start facing\n");
    test_team_convert();
    test_reserve_drop_facing(false);
    std::printf("a1i t04 / t05: land Reinforce from a landing object (spawn, carried type, unload offsets, take-off)\n");
    test_landing_drop(false);
    test_landing_drop(true);
    test_landing_drop(true, 90);  // a1j t01: destroyed with team 0 out
    test_landing_drop(true, 30);  // a1j t01: destroyed before any unit is out
    std::printf("S8 t02 / S7 t13: obstacle footprints follow their objects (land stamps, space static entries)\n");
    test_runtime_obstacles(false);
    test_remote_bomb_marker();  // S8 t10 follow-up
    test_runtime_obstacles(true);
    std::printf("S7 t12: a fighter squadron moved past an enemy keeps its move order (formation attack target)\n");
    test_fighter_move_past_enemy();
    std::printf("S7 t22: squadron auto targets through the formation push (chase test, Stop, idle chase, enter_idle)\n");
    test_fighter_chase_rule();
    test_fighter_auto_push();
    std::printf("S7 t24: a carrier's launch guards the carrier (Guard_Chase_Range, follows it)\n");
    test_hangar_escort_guard();
    std::printf("s78f review: host record vs the movement stack (shift waypoint, attack order, a dying station)\n");
    test_escort_waypoint();
    test_escort_attack_in_range();
    test_ai_fighter_blocks();  // s78f t10
    test_fighter_station_dies();
#ifdef ALAMO_SIM_HAVE_FOG
    std::printf("S7 t22: a fogged formation goal toggles Idle <-> Directed_Combat\n");
    test_fighter_fog_toggle();
#endif
    std::printf("t41: the final tick's deaths are in the battle's loss records\n");
    test_final_tick_losses(*d);
    std::printf("story t06: tactical victory monitor (hold, queued tests, ENABLE_VICTORY, VICTORY reward)\n");
    test_victory_monitor(*d);
    std::printf("OI objects: behaviour Initialise draws + scheduling\n");  // OI objects
    test_oi_objects_scheduling();
    test_foc();
    if (g_fail) {
        std::printf("%d check(s) FAILED\n", g_fail);
        return 1;
    }
    std::printf("alamo_sim_battle_tests: all passed\n");
    return 0;
}
