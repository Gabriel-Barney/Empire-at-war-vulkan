// alamo_sim_l1_tests: the L1 systems in a campaign battle (tactical_battle.hpp "L1 systems"; docs/re/SCRIPT_GAPS.md
// §1-§3). Synthetic data: an Underworld fleet with a tactical superweapon attacks the Empire at S10_B; in the space
// battle the Empire spawns a special weapon at the map's DUMMY_SPECIAL_WEAPON_SOURCE prop (through the script spawn
// path: TacticalMode::spawn + adopt_object), waits for its charge and fires it at an Underworld frigate; the TSW
// (battle attacker: starts ready at the attack distance) fires and destroys the battle planet; an Underworld frigate
// hyperspaces out (leaves as retreated), an Empire corvette jumps cinematically (stays), timed engine disables and
// teleport. Per-tick CRC logs + an L1 event trace identical for 0 / 1 / 4 / 15 workers.
// With ALAMO_FOC_DATA: the FoC special-weapon types join space / land battle tables; retreat increments from FactionFiles.
#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "alamo/core/file_provider.hpp"
#include "alamo/core/hash.hpp"
#include "alamo/data/game_database.hpp"
#include "alamo/galactic/campaign.hpp"
#include "alamo/galactic/conflicts.hpp"
#include "alamo/galactic/galactic_rules.hpp"
#include "alamo/objects/game_object_types.hpp"
#include "alamo/objects/object_types.hpp"
#include "alamo/sim/campaign_battles.hpp"
#include "alamo/sim/sim_host.hpp"
#include "alamo/vfs/vfs.hpp"
#include "alamo/test/support.hpp"

using namespace alamo;
using namespace alamo::sim;
namespace fs = std::filesystem;

using ::alamo::test::g_fail;
static bool g_regen = false;

namespace {

using ::alamo::test::write_file;

const char* kTypes = R"(<L1>
 <Planet Name="S10_A"><Behavior>PLANET</Behavior><Terrain>Temperate</Terrain><Planet_Credit_Value>10</Planet_Credit_Value></Planet>
 <Planet Name="S10_B"><Behavior>PLANET</Behavior><Terrain>Temperate</Terrain><Planet_Credit_Value>10</Planet_Credit_Value>
  <Space_Tactical_Map>L1_Space_B</Space_Tactical_Map><Land_Tactical_Map>L1_Land_B</Land_Tactical_Map><Planet_Is_Destroyable_By_TSW>Yes</Planet_Is_Destroyable_By_TSW></Planet>
 <Planet Name="S10_C"><Behavior>PLANET</Behavior><Terrain>Desert</Terrain><Planet_Credit_Value>10</Planet_Credit_Value></Planet>
 <Projectile Name="L1_Laser_Proj"><Max_Speed>40</Max_Speed><Projectile_Damage>20</Projectile_Damage>
  <Projectile_Max_Flight_Distance>2500</Projectile_Max_Flight_Distance></Projectile>
 <Projectile Name="L1_Ion_Proj"><Max_Speed>200</Max_Speed><Projectile_Damage>250</Projectile_Damage>
  <Projectile_Max_Lifetime>10</Projectile_Max_Lifetime></Projectile>
 <SpaceUnit Name="S10_Frigate"><Victory_Relevant>Yes</Victory_Relevant>
  <Tactical_Health>3000</Tactical_Health><Max_Speed>3.0</Max_Speed><Max_Rate_Of_Turn>2.0</Max_Rate_Of_Turn>
  <Hyperspace>Yes</Hyperspace><Hyperspace_Speed>1.5</Hyperspace_Speed><AI_Combat_Power>1000</AI_Combat_Power>
  <CategoryMask>Frigate</CategoryMask><Targeting_Max_Attack_Distance>1400</Targeting_Max_Attack_Distance>
  <Population_Value>1</Population_Value><HardPoints>HP_L1_Laser</HardPoints>
  <Behavior>SELECTABLE</Behavior><Collidable_By_Projectile_Living>Yes</Collidable_By_Projectile_Living><SpaceBehavior>SIMPLE_SPACE_LOCOMOTOR</SpaceBehavior>
  <Custom_Soft_Footprint_Radius>60</Custom_Soft_Footprint_Radius>
 </SpaceUnit>
 <SpaceUnit Name="S10_Corvette"><Variant_Of_Existing_Type>S10_Frigate</Variant_Of_Existing_Type>
  <CategoryMask>Corvette</CategoryMask><Custom_Soft_Footprint_Radius>30</Custom_Soft_Footprint_Radius></SpaceUnit>
 <SpaceUnit Name="L1_Old_Ship"><Variant_Of_Existing_Type>S10_Frigate</Variant_Of_Existing_Type><SpaceBehavior>STARSHIP_LOCOMOTOR</SpaceBehavior></SpaceUnit>
 <SpaceUnit Name="L1_Death_Star"><Tactical_Health>50000</Tactical_Health><Max_Speed>0.75</Max_Speed>
  <Hyperspace>Yes</Hyperspace><Hyperspace_Speed>1.5</Hyperspace_Speed><CategoryMask>Super</CategoryMask><Population_Value>1</Population_Value>
  <SpaceBehavior>TACTICAL_SUPER_WEAPON, IDLE</SpaceBehavior>
  <TSW_Start_Distance_From_Target>8000.0</TSW_Start_Distance_From_Target><TSW_Start_Pos_Match_Targets_Z>Yes</TSW_Start_Pos_Match_Targets_Z>
  <TSW_Attack_Distance_From_Target>2500.0</TSW_Attack_Distance_From_Target><TSW_Power_Up_Countdown_Seconds>1.0</TSW_Power_Up_Countdown_Seconds>
  <TSW_Attack_Anim_Duration_Seconds>0.5</TSW_Attack_Anim_Duration_Seconds><TSW_Explosion_Debris_Creation_Frame_Delay>10</TSW_Explosion_Debris_Creation_Frame_Delay>
  <TSW_Post_Destruction_Wait_Frames>40</TSW_Post_Destruction_Wait_Frames><TSW_Z_Adjust_Relative_To_Target_Pos>300.0</TSW_Z_Adjust_Relative_To_Target_Pos>
 </SpaceUnit>
 <SpecialStructure Name="L1_Ion_Cannon"><Victory_Relevant>Yes</Victory_Relevant><SpaceBehavior>SPECIAL_WEAPON</SpaceBehavior><Tactical_Health>1500</Tactical_Health>
  <Projectile_Types>L1_Ion_Proj</Projectile_Types><Projectile_Fire_Recharge_Seconds>10.0</Projectile_Fire_Recharge_Seconds>
  <Projectile_Fire_Pulse_Count>2</Projectile_Fire_Pulse_Count><Projectile_Fire_Pulse_Delay_Seconds>0.5</Projectile_Fire_Pulse_Delay_Seconds>
  <Is_Special_Weapon_In_Space>Yes</Is_Special_Weapon_In_Space><Special_Weapon_Index>0</Special_Weapon_Index>
  <Special_Weapon_Valid_Targets>Frigate | Corvette</Special_Weapon_Valid_Targets></SpecialStructure>
 <SpaceProp Name="L1_Nebula"><Behavior>SPACE_OBSTACLE</Behavior><Is_Nebula>Yes</Is_Nebula><Space_Obstacle_Radius>800</Space_Obstacle_Radius>
  <Space_Obstacle_Offset>100 0 0</Space_Obstacle_Offset></SpaceProp>
 <SpaceProp Name="L1_Planet_Prop"><Behavior>DUMMY_SPECIAL_WEAPON_SOURCE, DUMMY_TACTICAL_SUPER_WEAPON_TARGET</Behavior></SpaceProp>
 <Projectile Name="L1_Bomb"><Max_Speed>20</Max_Speed><Projectile_Damage>0</Projectile_Damage><Projectile_Category>BOMB</Projectile_Category>
  <Projectile_Blast_Area_Damage>30</Projectile_Blast_Area_Damage><Projectile_Blast_Area_Range>150</Projectile_Blast_Area_Range></Projectile>
 <Projectile Name="L1_Bombard_Proj"><Max_Speed>40</Max_Speed><Projectile_Damage>0</Projectile_Damage>
  <Projectile_Blast_Area_Damage>40</Projectile_Blast_Area_Damage><Projectile_Blast_Area_Range>200</Projectile_Blast_Area_Range></Projectile>
 <GroundInfantry Name="S10_Trooper"><Victory_Relevant>Yes</Victory_Relevant><Tactical_Health>2000</Tactical_Health><Max_Speed>1.0</Max_Speed>
  <AI_Combat_Power>50</AI_Combat_Power><CategoryMask>Infantry</CategoryMask><Targeting_Max_Attack_Distance>250</Targeting_Max_Attack_Distance>
  <MovementClass>Infantry</MovementClass><Weather_Category>Infantry</Weather_Category>
  <Behavior>SELECTABLE</Behavior><Collidable_By_Projectile_Living>Yes</Collidable_By_Projectile_Living><LandBehavior>WALK_LOCOMOTOR</LandBehavior><Custom_Soft_Footprint_Radius>6</Custom_Soft_Footprint_Radius>
 </GroundInfantry>
 <GroundCompany Name="S10_Squad"><Company_Units>S10_Trooper, S10_Trooper</Company_Units>
  <Hyperspace>Yes</Hyperspace><Hyperspace_Speed>1.5</Hyperspace_Speed><Population_Value>1</Population_Value></GroundCompany>
 <SpaceUnit Name="L1_Bomber_Squadron"><Variant_Of_Existing_Type>S10_Frigate</Variant_Of_Existing_Type><Ship_Class>bomber</Ship_Class>
  <Land_Bomber_Type>L1_Bomber</Land_Bomber_Type></SpaceUnit>
 <LandBombingUnit Name="L1_Bomber"><Victory_Relevant>Yes</Victory_Relevant><Max_Speed>6.0</Max_Speed><Tactical_Health>50</Tactical_Health>
  <LandBehavior>LANDBOMBER_LOCOMOTOR, WEAPON</LandBehavior><Targeting_Max_Attack_Distance>500.0</Targeting_Max_Attack_Distance>
  <Projectile_Types>L1_Bomb</Projectile_Types><Projectile_Fire_Recharge_Seconds>0.2</Projectile_Fire_Recharge_Seconds></LandBombingUnit>
 <Marker Name="IN_GAME_BOMBING_RUN_MARKER"><Behavior>MARKER</Behavior></Marker>
 <SpecialStructure Name="L1_Build_Pad"><Victory_Relevant>Yes</Victory_Relevant><Behavior>TACTICAL_BUILD_OBJECTS</Behavior><Capture_Point_Radius>100</Capture_Point_Radius>
  <Tactical_Buildable_Objects_Campaign>Empire, L1_UC_Turret, Underworld, L1_UC_Turret</Tactical_Buildable_Objects_Campaign>
  <Tactical_Buildable_Objects_Multiplayer>Empire, L1_UC_Turret_MP, Underworld, L1_UC_Turret_MP</Tactical_Buildable_Objects_Multiplayer>
  <Minimum_Time_Before_Pad_Can_Build_Again>1.0</Minimum_Time_Before_Pad_Can_Build_Again></SpecialStructure>
 <SpecialStructure Name="L1_UC_Turret"><Victory_Relevant>Yes</Victory_Relevant><Tactical_Health>100</Tactical_Health><Tactical_Build_Time_Seconds>2</Tactical_Build_Time_Seconds>
  <Tactical_Buildable_Constructed>L1_Turret</Tactical_Buildable_Constructed></SpecialStructure>
 <SpecialStructure Name="L1_UC_Turret_MP"><Victory_Relevant>Yes</Victory_Relevant><Tactical_Health>100</Tactical_Health><Tactical_Build_Time_Seconds>2</Tactical_Build_Time_Seconds>
  <Tactical_Buildable_Constructed>L1_Turret</Tactical_Buildable_Constructed></SpecialStructure>
 <SpecialStructure Name="L1_Turret"><Victory_Relevant>Yes</Victory_Relevant><Tactical_Health>500</Tactical_Health></SpecialStructure>
 <GroundInfantry Name="L1_Scout"><Variant_Of_Existing_Type>S10_Trooper</Variant_Of_Existing_Type><Land_FOW_Reveal_Range>300</Land_FOW_Reveal_Range>
  <Behavior>SELECTABLE, REVEAL</Behavior></GroundInfantry>
 <GroundInfantry Name="L1_Jet_Trooper"><Variant_Of_Existing_Type>S10_Trooper</Variant_Of_Existing_Type>
  <Uses_Multiple_Locomotors>Yes</Uses_Multiple_Locomotors><Primary_Locomotor_Name>WALK_LOCOMOTOR</Primary_Locomotor_Name>
  <Secondary_Locomotor_Name>JETPACK_LOCOMOTOR</Secondary_Locomotor_Name><Alternate_Max_Speed>2.5</Alternate_Max_Speed>
  <Hover_Offset>30</Hover_Offset><Behavior>SELECTABLE, WEAPON</Behavior><Projectile_Types>L1_Laser_Proj</Projectile_Types>
  <Projectile_Fire_Recharge_Seconds>0.5</Projectile_Fire_Recharge_Seconds></GroundInfantry>
 <GroundCompany Name="L1_Jet_Squad"><Company_Units>L1_Jet_Trooper, L1_Jet_Trooper</Company_Units>
  <Hyperspace>Yes</Hyperspace><Hyperspace_Speed>1.5</Hyperspace_Speed><Population_Value>1</Population_Value></GroundCompany>
 <GroundInfantry Name="L1_Deploy_Trooper"><Variant_Of_Existing_Type>S10_Trooper</Variant_Of_Existing_Type>
  <Unit_Abilities_Data SubObjectList="Yes"><Unit_Ability><Type>DEPLOY</Type></Unit_Ability></Unit_Abilities_Data></GroundInfantry>
 <GroundCompany Name="L1_Deploy_Squad"><Company_Units>L1_Deploy_Trooper, L1_Deploy_Trooper</Company_Units>
  <Hyperspace>Yes</Hyperspace><Hyperspace_Speed>1.5</Hyperspace_Speed><Population_Value>1</Population_Value></GroundCompany>
 <GroundInfantry Name="L1_Team_Trooper"><Variant_Of_Existing_Type>S10_Trooper</Variant_Of_Existing_Type><Create_Team>Yes</Create_Team></GroundInfantry>
 <GroundCompany Name="L1_Team_Squad"><Company_Units>L1_Team_Trooper, L1_Team_Trooper</Company_Units><Create_Team_Type>L1_Team</Create_Team_Type>
  <Population_Value>1</Population_Value></GroundCompany>
 <Team Name="L1_Team"><Behavior>TEAM</Behavior></Team>
</L1>
)";

const char* kHardpoints = R"(<HardPoints>
 <HardPoint Name="HP_L1_Laser"><Type>HARD_POINT_WEAPON_LASER</Type><Health>500</Health>
  <Is_Targetable>Yes</Is_Targetable><Is_Destroyable>Yes</Is_Destroyable>
  <Fire_Projectile_Type>L1_Laser_Proj</Fire_Projectile_Type><Fire_Min_Recharge_Seconds>2.0</Fire_Min_Recharge_Seconds>
  <Fire_Max_Recharge_Seconds>3.0</Fire_Max_Recharge_Seconds><Fire_Range_Distance>1400</Fire_Range_Distance><Fire_Cone_Width>360</Fire_Cone_Width><Fire_Cone_Height>180</Fire_Cone_Height></HardPoint>
</HardPoints>
)";

const char* kFactions = R"(<Factions>
 <Faction Name="Empire"><Is_Playable>Yes</Is_Playable><Basic_AI>BasicEmpire</Basic_AI><Can_Control_Planets>Yes</Can_Control_Planets>
  <Space_Tactical_Unit_Cap>20</Space_Tactical_Unit_Cap><Space_Retreat_Unit_Increment_Wait_Frames>3</Space_Retreat_Unit_Increment_Wait_Frames></Faction>
 <Faction Name="Underworld"><Is_Playable>Yes</Is_Playable><Basic_AI>AI_Player_Underworld</Basic_AI><Can_Control_Planets>Yes</Can_Control_Planets>
  <Space_Tactical_Unit_Cap>20</Space_Tactical_Unit_Cap>
  <Bombardment_Required_Orbital_Ships> S10_Frigate </Bombardment_Required_Orbital_Ships><Bombardment_Projectile> L1_Bombard_Proj </Bombardment_Projectile>
  <Bombardment_Number_Of_Salvos> 2 </Bombardment_Number_Of_Salvos><Bombardment_Shots_Per_Salvo> 3 </Bombardment_Shots_Per_Salvo>
  <Bombardment_Shot_Delay> 0.2 </Bombardment_Shot_Delay><Bombardment_Salvo_Delay> 1 </Bombardment_Salvo_Delay><Bombardment_Initial_Delay> 1.0 </Bombardment_Initial_Delay></Faction>
 <Faction Name="Neutral"><Is_Neutral>Yes</Is_Neutral></Faction>
</Factions>
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

// extra: more type entries for kTypes, in their own folder (the golden run's type table stays as it is).
std::unique_ptr<Data> make_data(const char* folder = "alamo_sim_l1_tests", const char* extra = nullptr) {
    auto d = std::make_unique<Data>();
    d->root = fs::temp_directory_path() / folder;
    std::error_code ec;
    remove_all_long(d->root, ec);
    const fs::path x = d->root / "Data/XML";
    write_file(x / "GameObjectFiles.xml", "<GameObjectFiles><File>L1Types.xml</File></GameObjectFiles>\n");
    std::string types = kTypes;
    if (extra) types.insert(types.rfind("</L1>"), extra);
    write_file(x / "L1Types.xml", types);
    write_file(x / "HardPointDataFiles.xml", "<Hard_Point_Files><File>L1HardPoints.xml</File></Hard_Point_Files>\n");
    write_file(x / "L1HardPoints.xml", kHardpoints);
    write_file(x / "FactionFiles.xml", "<FactionFiles><File>L1Factions.xml</File></FactionFiles>\n");
    write_file(x / "L1Factions.xml", kFactions);
    write_file(x / "TradeRouteFiles.xml", "<TradeRouteFiles></TradeRouteFiles>\n");
    write_file(x / "CampaignFiles.xml", "<CampaignFiles></CampaignFiles>\n");
    write_file(x / "WeatherScenarios.xml", "<Weather_Scenarios><Weather_Scenario name=\"Rain_Test\"><Phase name=\"CLEAR\"><Emitter_Intensity>0.0</Emitter_Intensity><Duration>4</Duration><Ease_Out_Duration>2</Ease_Out_Duration></Phase><Phase name=\"RAIN\"><Emitter_Intensity>1.0</Emitter_Intensity><Duration>6</Duration><Ease_Out_Duration>2</Ease_Out_Duration></Phase></Weather_Scenario></Weather_Scenarios>\n");
    write_file(x / "WeatherModifiers.xml", "<WeatherModifies><WeatherModifier name=\"rain\"><Sight_Range_Modifier name=\"Infantry\">0.5f</Sight_Range_Modifier><Weapon_Accuracy_Modifier name=\"Bomb\">0.4f</Weapon_Accuracy_Modifier></WeatherModifier></WeatherModifies>\n");
    write_file(x / "GameConstants.xml", "<GameConstants>\n <Space_Retreat_Allowed_Countdown_Seconds>5.0</Space_Retreat_Allowed_Countdown_Seconds>\n</GameConstants>\n");
    write_file(d->root / "Data/Scripts/Library/PGAICommands.lua",
               "function Set_Contrast_Values()\n\tEnemyContrastTypes[1] = \"Frigate\"\n\tFriendlyContrastTypeNames = {\"Frigate\"}\n"
               "\tFriendlyContrastWeights =\t{1.0}\n\tFriendlyContrastTypes[1].Parse(FriendlyContrastTypeNames, FriendlyContrastWeights)\nend\n");
    d->files = std::make_unique<DirectoryProvider>(path_to_utf8(d->root));
    std::vector<std::string> diag;
    CHECK(d->db.load(*d->files, {}, &diag));
    CHECK(d->raw.load(*d->files));
    CHECK(d->types.build(d->raw, d->files.get(), &diag));
    CHECK(galactic::load_galactic_rules(*d->files, d->db, d->types, d->rules, &diag));
    CHECK(galactic::load_conflict_rules(*d->files, d->db, d->types, d->rules, d->crules, &diag));
    d->catalog = std::make_unique<BattleCatalog>(d->types, d->files.get(), &d->db.constants(), &diag);
    return d;
}

terrain::MapData space_map() {
    terrain::MapData m;
    m.space.present = true;
    for (int i = 0; i < 3; ++i) {
        m.space.playable_min[i] = -6000.0f;
        m.space.playable_max[i] = 6000.0f;
    }
    m.playable.present = m.playable.has_outer_box = true;
    for (int i = 0; i < 3; ++i) {
        m.playable.outer_min[i] = -6000.0f;
        m.playable.outer_max[i] = 6000.0f;
    }
    terrain::ObjectPlacement prop;  // the planet: special-weapon source + TSW target
    prop.type_crc = name_crc("L1_PLANET_PROP");
    prop.owner = 2;
    prop.position[0] = 0.0f;
    prop.position[1] = 5000.0f;
    prop.position[2] = 100.0f;
    m.objects.push_back(prop);
    terrain::ObjectPlacement neb;  // a nebula field (SPACE_OBSTACLE + Is_Nebula)
    neb.type_crc = name_crc("L1_NEBULA");
    neb.owner = 2;
    neb.position[1] = -4000.0f;
    m.objects.push_back(neb);
    return m;
}

uint32_t type_of(const Data& d, const char* name) { return static_cast<uint32_t>(d.types.resolve_name(name)); }

galactic::GalacticSetup make_setup(const Data& d) {
    galactic::GalacticSetup s;
    GalaxyMap& m = s.map;
    m.campaign_name = "L1";
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
    galactic::PlayerSetup n;
    n.faction = d.rules.find_faction("Neutral");
    s.players = {e, u, n};
    const uint32_t fr = type_of(d, "S10_Frigate"), co = type_of(d, "S10_Corvette"), ds = type_of(d, "L1_Death_Star"), old = type_of(d, "L1_Old_Ship");
    s.forces = {{0, 1, fr}, {0, 1, fr}, {0, 1, co}, {1, 2, fr}, {1, 2, fr}, {1, 2, fr}, {1, 2, ds}, {0, 1, old}};
    return s;
}

struct Run {
    CrcLog crc;
    uint64_t trace = 0;
    bool fought = false;
    bool weapon_adopted = false, fired = false, hit = false, not_ready_refused = false, allowed_after = true;
    bool tsw_ready = false, tsw_fired = false, planet_destroyed = false;
    bool jumped_out = false, away_died = false, engines_off = false, engines_back = false, teleported = false;
    int32_t increment = -1;
    float ground = 0.0f;
    bool fields = false, jump_timing = false;
    uint32_t deaths_before = 0;
    std::vector<std::string> log;
    // TSW_PLANET_DESTROY follow-up runs (follow > 0): the battle runs on to its end.
    uint32_t tsw_fire_frame = 0, destroy_frame = 0, debris_frame = 0, done_frame = 0, end_frame = 0;
    uint32_t first_unheld = 0, last_held = 0;
    bool galaxy_same_tick = false, fleets_kept = false, owner_kept = false, dummy_killed = false;
    bool held_from_fire = true, finished_while_held = false, ended = false, story_pending = false;
    size_t claims = 0;
    std::string summary;
    // follow 3: the attacker's escort killed after the release; the Death Star as last seen in the battle.
    uint32_t kill_frame = 0;
    size_t escorts_killed = 0;
    bool ds_alive = false;
    float ds_health0 = 0.0f, ds_health = 0.0f;  // at frame 2 and as last seen
};

// Battle frame schedule (frames of the battle's own counter). follow: 0 = the golden run (stops at the planet's
// destruction); 1 = on to the end, the Empire (defender) retreats during the TSW sequence (victory hold, §2); 2 = on to
// the end, the Empire's Faction_Leader is a member of a company on the battle planet's surface (victory 7 / 0x10, §6);
// 3 = on to the end, the Underworld escort frigates die (Hyperspace_Away) on the release frame, the Death Star is left.
Run run(const Data& d, int32_t workers, int follow = 0) {
    Run out;
    jobs::JobSystem js(jobs::JobSystemConfig{workers});
    SimConfig cfg;
    cfg.seed = 7;
    cfg.players = 4;
    SimHost host(d.types, js, cfg);
    galactic::GalacticRules rules = d.rules;
    galactic::ConflictRules crules = d.crules;
    galactic::GalacticSetup setup = make_setup(d);
    const uint32_t empire = d.rules.find_faction("Empire");
    if (follow > 0) {  // a 1 s space retreat countdown for the Empire (the retreat completes inside the hold)
        if (crules.factions.size() <= empire) crules.factions.resize(empire + 1);
        crules.factions[empire].space_retreat_countdown = 1.0f;
    }
    if (follow == 2) {
        rules.factions[empire].leader = type_of(d, "S10_Trooper");
        setup.forces.push_back({0, 1, type_of(d, "S10_Squad")});  // on S10_B's surface
    }
    galactic::GalacticSim* gs = host.start_galactic(rules, setup);
    CHECK(gs != nullptr);
    if (!gs) return out;
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 6000;
    opt.battle.order_interval = 0;  // no stand-in orders: the sides stay at their anchors (12000 apart)
    opt.retreat_increment_frames = load_retreat_increments(*d.files, d.rules);
    opt.retreat_flight_increments = load_retreat_flight_increments(*d.files, d.rules);
    CampaignBattles battles(host, crules, *d.catalog,
                            [](const std::string& name, terrain::MapData& m) {
                                if (name != "L1_Space_B") return false;
                                m = space_map();
                                return true;
                            },
                            opt);
    CHECK(battles.attach());
    const std::vector<uint32_t> uf = gs->fleets_at(2, 1);
    CHECK(!uf.empty());
    if (!uf.empty()) CHECK(gs->move_fleet(1, uf[0], 1) == commands::RejectReason::None);
    StateHasher trace;
    objects::ObjectId weapon = objects::kNoObjectId, target = objects::kNoObjectId, jumper = objects::kNoObjectId,
                      corvette = objects::kNoObjectId, old = objects::kNoObjectId, slowed = objects::kNoObjectId, tsw = objects::kNoObjectId;
    float target_hp0 = 0.0f;
    uint32_t fire_frame = 0;
    objects::ObjectId dummy = objects::kNoObjectId;
    std::vector<objects::ObjectId> escorts;  // the Underworld (attacker) frigates
    for (uint64_t t = 0; t < 4000 && (follow ? !out.ended : !out.planet_destroyed); ++t) {
        host.tick_once();
        TacticalBattle* b = battles.battle();
        TacticalMode* tm = host.tactical();
        if (follow && out.fought && !b) {  // the battle ended in this tick's hook
            out.ended = true;
            out.summary = battles.log().empty() ? std::string() : battles.log().back();
            const size_t at = out.summary.find("L1_Space_B: ");
            if (at != std::string::npos) out.end_frame = static_cast<uint32_t>(std::strtoul(out.summary.c_str() + at + 12, nullptr, 10));
            continue;
        }
        if (!b || !tm) continue;
        out.fought = true;
        objects::ObjectStore& st = tm->primary();
        const uint32_t f = b->frame();
        if (f == 2) {
            // The ids of interest; a special weapon spawned at the source prop through the script spawn path.
            for (const objects::ObjectHandle h : st.all_objects()) {
                if (!st.alive(h)) continue;
                const objects::ObjectId id = st.id(h);
                const auto ty = static_cast<uint32_t>(st.type(h));
                if (ty == type_of(d, "L1_Death_Star")) tsw = id;
                if (ty == type_of(d, "S10_Frigate") && st.owner(h) == 1) {
                    escorts.push_back(id);
                    if (target == objects::kNoObjectId) target = id;
                    else if (jumper == objects::kNoObjectId) jumper = id;
                }
                if (ty == type_of(d, "S10_Corvette") && st.owner(h) == 0) corvette = id;
                if (ty == type_of(d, "L1_Old_Ship")) old = id;
                if (ty == type_of(d, "S10_Frigate") && st.owner(h) == 0 && slowed == objects::kNoObjectId) slowed = id;
            }
            objects::ObjectHandle source;
            for (const objects::ObjectHandle h : st.all_objects())
                if (st.alive(h) && st.type(h) == static_cast<objects::TypeIndex>(type_of(d, "L1_Planet_Prop"))) source = h;
            CHECK(static_cast<bool>(source));
            objects::SpawnParams sp;
            sp.type = static_cast<objects::TypeIndex>(type_of(d, "L1_Ion_Cannon"));
            sp.owner = 0;
            sp.position = st.position(source);
            const objects::ObjectHandle wh = tm->spawn(sp);
            b->adopt_object(*tm, wh);
            weapon = st.id(wh);
            out.weapon_adopted = b->sw_readiness(weapon) >= 0.0f && b->sw_readiness(weapon) < 1.0f;
            out.not_ready_refused = !b->sw_fire_at_object(*tm, weapon, target);
            out.tsw_ready = b->tsw_state(tsw) == tactical::abilities::kTswReady;  // the attacker's TSW starts ready
            // Hyperspace: an Underworld frigate leaves; an Empire corvette jumps cinematically.
            CHECK(b->hyperspace_out(jumper, 5, false));    // cinematic retreat form: galactic unit survives
            CHECK(b->hyperspace_out(corvette, 0, true));   // Hyperspace_Away(): a death
            CHECK(!b->hyperspace_out(old, 0, true));       // STARSHIP_LOCOMOTOR: empty stub
            out.deaths_before = b->stats().deaths;
            CHECK(!b->hyperspace_out(weapon, 0, false));  // no locomotor
            // Engines: 2 s timed disable on an Empire frigate.
            CHECK(b->disable_engines(slowed, 2.0f));
            out.engines_off = b->engines_online(slowed) == 0;
            CHECK(b->engines_online(weapon) == -1);
            // Teleport the frigate (store, movement, combat).
            CHECK(b->teleport(*tm, slowed, Vec3{-1000.0f, -2000.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f}));
            out.teleported = b->movement().view(slowed).pos.x == -1000.0f && b->combat().position(slowed).y == -2000.0f &&
                             st.position(st.find(slowed)).x == -1000.0f;
            out.increment = b->retreat_increment_frames(0);
            out.ground = b->ground_height(0.0f, 0.0f);
            out.fields = b->field_kinds_at(100.0f, -4000.0f) == 4u && b->field_kinds_at(100.0f, -3150.0f) == 0u &&
                         b->nearest_field(0.0f, 0.0f, 4u) != objects::kNoObjectId && b->nearest_field(0.0f, 0.0f, 1u) == objects::kNoObjectId;
        }
        if (f == 70) out.engines_back = b->engines_online(slowed) == 1;
        if (weapon != objects::kNoObjectId && !out.fired && b->sw_readiness(weapon) >= 1.0f) {
            CHECK(b->sw_begin_targeting(weapon));
            target_hp0 = b->combat().health(target);
            out.fired = b->sw_fire_at_object(*tm, weapon, target);
            fire_frame = f;
        }
        if (out.fired && !out.hit && b->combat().has_unit(target) && b->combat().health(target) < target_hp0) out.hit = true;
        if (out.fired && f == fire_frame + 60 && !out.tsw_fired) {
            out.tsw_fired = b->tsw_fire(tsw);
            out.tsw_fire_frame = f;
            dummy = b->special_weapons().tsw_target(tsw);
            if (follow == 1) CHECK(b->request_retreat(0));  // the Empire retreats: complete ~30 frames later, inside the hold
        }
        const TacticalBattle* cb = b;
        for (const auto& e : cb->special_weapon_events()) {
            trace.u8(static_cast<uint8_t>(e.kind));
            trace.i32(e.id);
            trace.i32(e.value);
            trace.u32(e.frame);
            if (!follow) continue;
            if (e.kind == tactical::abilities::SwEventKind::TswPlanetDestroyed) {
                out.destroy_frame = f;
                // §4 steps 7-9 in the same tick: the galactic planet is destroyed; the fleets (incl. the Death Star's),
                // the owner and the TSW stay; the target dummy is killed.
                out.galaxy_same_tick = gs->planets()[1].destroyed;
                out.fleets_kept = !gs->fleets_at(1, 0).empty() && !gs->fleets_at(1, 1).empty();
                bool ds_alive = false;  // the Death Star's galactic object
                for (const objects::ObjectHandle h : gs->store().all_objects())
                    ds_alive = ds_alive || (gs->store().alive(h) && static_cast<uint32_t>(gs->store().type(h)) == type_of(d, "L1_Death_Star"));
                out.fleets_kept = out.fleets_kept && ds_alive;
                out.owner_kept = gs->planets()[1].owner == 0;
                out.dummy_killed = dummy != objects::kNoObjectId && !st.alive(st.find(dummy)) && st.alive(st.find(tsw));
                out.story_pending = b->tsw_story_victory() >= 0;
                out.claims = gs->victory_claims().size();
            }
            if (e.kind == tactical::abilities::SwEventKind::TswDebris) out.debris_frame = f;
            if (e.kind == tactical::abilities::SwEventKind::TswDone) out.done_frame = f;
        }
        if (follow && out.tsw_fired) {
            if (b->victory_held()) {
                if (out.first_unheld) out.held_from_fire = false;
                out.last_held = f;
            } else if (!out.first_unheld) {
                out.first_unheld = f;
            }
            if (b->finished() && b->victory_held()) out.finished_while_held = true;
        }
        if (follow == 3 && out.first_unheld == f && out.kill_frame == 0) {
            out.kill_frame = f;
            for (const objects::ObjectId e : escorts)
                if (e != jumper && b->hyperspace_out(e, 0, true)) ++out.escorts_killed;  // a death, 90 frames on
        }
        if (follow == 3) {
            if (f == 2) out.ds_health0 = b->combat().health(tsw);
            out.ds_alive = st.alive(st.find(tsw)) && b->combat().has_unit(tsw);
            out.ds_health = out.ds_alive ? b->combat().health(tsw) : 0.0f;
        }
        if (f == 2 + 5 + 90) out.jumped_out = !b->combat().has_unit(jumper) && !st.alive(st.find(jumper));
        if (f == 2 + 89) out.jump_timing = b->combat().has_unit(jumper) && b->hyperspace().departing(corvette);
        if (f == 2 + 90) out.away_died = !st.alive(st.find(corvette)) && b->stats().deaths == out.deaths_before + 1;
        if (b->tsw_planet_destroyed() && !out.planet_destroyed) {
            out.planet_destroyed = true;
            out.allowed_after = b->special_weapons_allowed(0);
        }
        trace.u32(f);
        trace.f32(b->sw_readiness(weapon));
        trace.i32(b->tsw_state(tsw));
    }
    out.trace = trace.digest();
    out.crc = host.crc_log();
    out.log = battles.log();
    return out;
}

void test_battle(const Data& d) {
    const Run a = run(d, 0);
    std::printf("  fought %d; weapon adopted %d, not-ready refused %d, fired %d, hit %d; TSW ready %d fired %d planet destroyed %d "
                "(weapons allowed after %d); jump out %d, Hyperspace_Away death %d; engines off %d back %d; teleport %d; increment %d; "
                "trace %016" PRIx64 "\n",
                a.fought, a.weapon_adopted, a.not_ready_refused, a.fired, a.hit, a.tsw_ready, a.tsw_fired, a.planet_destroyed,
                a.allowed_after, a.jumped_out, a.away_died, a.engines_off, a.engines_back, a.teleported, a.increment, a.trace);
    CHECK(a.fought && a.weapon_adopted && a.not_ready_refused && a.fired && a.hit);
    CHECK(a.tsw_ready && a.tsw_fired && a.planet_destroyed && !a.allowed_after);
    CHECK(a.jumped_out && a.away_died && a.jump_timing);
    CHECK(a.engines_off && a.engines_back && a.teleported);
    CHECK(a.increment == 3);
    CHECK(a.ground < -1.0e30f);
    CHECK(a.fields);
    for (int32_t w : jobs::determinism_worker_counts()) {
        if (w == 0) continue;
        const Run b = run(d, w);
        const auto dv = first_divergence(a.crc, b.crc);
        std::printf("  workers %2d: %s, trace %s\n", w, dv.message.c_str(), b.trace == a.trace ? "identical" : "DIFFERENT");
        CHECK(!dv.diverged && b.trace == a.trace);
    }
    // Golden (MSVC writes it with --regen; clang / gcc must reproduce it): includes the L1 state ("battle.abilities").
    const std::string path = std::string(ALAMO_SIM_TESTDATA) + "/battle_l1_golden.crc";
    CrcLog log = a.crc;
    log.comments = {"L1 systems synthetic campaign: special weapon, TSW, hyperspace, engines, teleport; seed 7"};
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
            std::printf("  golden battle_l1_golden.crc: %s\n", dv.message.c_str());
            CHECK(!dv.diverged);
        }
    }
}

// ---- land: weather, orbital bombardment, bombing run ----
terrain::MapData land_map() {
    terrain::MapData m;
    m.terrain.present = true;
    m.terrain.width = m.terrain.height = 63;
    m.terrain.heights.assign(63u * 63u, 0);
    m.terrain.material.assign(63u * 63u, 0);
    terrain::Environment env;
    env.weather_effect = "Rain_Test";
    m.environments.environments.push_back(env);
    return m;
}

struct LandRun {
    CrcLog crc;
    uint64_t trace = 0;
    bool fought = false, weather_on = false, weather_advanced = false, forced = false, rain_sight = false;
    bool bombard_started = false, bombard_hit = false, bombard_reset = false;
    bool run_started = false, bombers_flew = false, run_done = false, refused_unavailable = false;
};

LandRun run_land(const Data& d, int32_t workers) {
    LandRun out;
    jobs::JobSystem js(jobs::JobSystemConfig{workers});
    SimConfig cfg;
    cfg.seed = 11;
    cfg.players = 4;
    SimHost host(d.types, js, cfg);
    galactic::GalacticSetup s = make_setup(d);
    const uint32_t sq = type_of(d, "S10_Squad"), fr = type_of(d, "S10_Frigate"), bs = type_of(d, "L1_Bomber_Squadron");
    s.forces = {{0, 1, sq}, {1, 2, fr}, {1, 2, bs}, {1, 2, sq}, {1, 2, sq}};
    galactic::GalacticSim* gs = host.start_galactic(d.rules, std::move(s));
    CHECK(gs != nullptr);
    if (!gs) return out;
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 6000;
    opt.battle.order_interval = 0;
    opt.battle.fog = false;
    opt.l1_constants.min_bombard_interval = opt.l1_constants.max_bombard_interval = 3.0f;
    opt.l1_constants.min_bombing_run_interval = opt.l1_constants.max_bombing_run_interval = 3.0f;
    opt.l1_factions = load_l1_factions(*d.files, d.rules);
    auto wd = std::make_shared<tactical::combat::WeatherData>();
    CHECK(tactical::combat::load_weather_data(*d.files, *wd));
    opt.weather = wd;
    CampaignBattles battles(host, d.crules, *d.catalog,
                            [](const std::string& name, terrain::MapData& m) {
                                if (name != "L1_Land_B") return false;
                                m = land_map();
                                return true;
                            },
                            opt);
    CHECK(battles.attach());
    std::vector<objects::ObjectHandle> up;
    for (objects::ObjectHandle h : gs->planets()[2].surface)
        if (gs->store().owner(h) == 1) up.push_back(h);
    CHECK(gs->launch_units(1, 2, up) == commands::RejectReason::None);
    const std::vector<uint32_t> uf = gs->fleets_at(2, 1);
    CHECK(!uf.empty());
    if (!uf.empty()) CHECK(gs->move_fleet(1, uf[0], 1) == commands::RejectReason::None);
    bool invaded = false;
    StateHasher trace;
    Vec3 target{};
    float hp0 = -1.0f;
    objects::ObjectId victim = objects::kNoObjectId;
    uint32_t phase0 = 0, bombard_end_ready = 0;
    uint32_t last_frame = 0;
    for (uint64_t t = 0; t < 6000 && !(out.run_done && last_frame > 400); ++t) {
        host.tick_once();
        if (!invaded && host.modes().depth() == 1) {
            const std::vector<uint32_t> f = gs->fleets_at(1, 1);
            if (!f.empty() && !gs->fleets()[f[0]].moving) {
                CHECK(gs->invade(1, 1, {}) == commands::RejectReason::None);
                invaded = true;
            }
        }
        TacticalBattle* b = battles.battle();
        TacticalMode* tm = host.tactical();
        if (!b || !tm) continue;
        out.fought = true;
        objects::ObjectStore& st = tm->primary();
        const uint32_t f = b->frame();
        last_frame = f;
        if (f == 1) {
            // World unit icons (QA U7): a company's members share one marker, its first member (ascending id).
            const std::vector<TacticalBattle::IconLink> links = b->icon_links();
            size_t followers = 0;
            for (size_t i = 0; i < links.size(); ++i) {
                CHECK(i == 0 || links[i - 1].unit < links[i].unit);
                CHECK(links[i].squad <= links[i].unit);
                if (links[i].squad == links[i].unit) continue;
                ++followers;
                CHECK(std::any_of(links.begin(), links.end(), [&](const TacticalBattle::IconLink& l) { return l.unit == links[i].squad && l.squad == l.unit; }));
            }
            CHECK(followers > 0);
            out.weather_on = b->weather().active();
            phase0 = b->weather().phase_index();
            for (const objects::ObjectHandle h : st.owned_by(0))
                if (st.alive(h) && b->combat().has_unit(st.id(h))) {
                    victim = st.id(h);
                    target = st.position(h);
                    break;
                }
            out.refused_unavailable = !b->start_bombardment(*tm, 1, target, host.random().synced()) && !b->bombing_run_available(1);
        }
        if (f > 1 && b->weather().phase_index() != phase0) out.weather_advanced = true;
        if (b->weather().type() == tactical::combat::WeatherType::Rain && b->weather().intensity() > 0.5f && victim != objects::kNoObjectId)
            out.rain_sight = out.rain_sight || b->weather_sight_modifier(victim) == 0.5f;
        if (f == 400) {
            const uint32_t before = b->weather().phase_index();
            out.forced = b->force_weather(host.random().synced()) && b->weather().phase_index() != before;
        }
        if (!out.bombard_started && b->bombardment_available(1)) {
            hp0 = b->combat().health(victim);
            out.bombard_started = b->start_bombardment(*tm, 1, target, host.random().synced());
        }
        if (out.bombard_started && !out.bombard_hit && b->combat().has_unit(victim) && b->combat().health(victim) < hp0) out.bombard_hit = true;
        if (out.bombard_hit && !out.bombard_reset && !b->bombardment_available(1) && bombard_end_ready == 0) bombard_end_ready = f;
        if (out.bombard_hit && b->bombardment_available(1) && f > 120) out.bombard_reset = true;
        if (!out.run_started && out.bombard_hit && b->bombing_run_available(1)) {
            out.run_started = b->start_bombing_run(*tm, 1, target);
        }
        if (out.run_started && !b->bombing_run_finished(1)) out.bombers_flew = true;
        if (out.bombers_flew && b->bombing_run_finished(1)) out.run_done = true;
        trace.u32(f);
        trace.u32(b->weather().phase_index());
        trace.f32(b->weather().intensity());
        trace.f32(b->combat().has_unit(victim) ? b->combat().health(victim) : -1.0f);
    }
    out.trace = trace.digest();
    out.crc = host.crc_log();
    return out;
}

// L1 systems t02/t03 (docs/re/TSW_PLANET_DESTROY.md): the galactic planet dies in the TSW's state-9 tick with the core
// routine only (fleets incl. the Death Star's, owner kept; the target dummy killed); the timers run in sequence (fire
// F, destroy F+48, debris +10, release +10+40+1 with the test type); no victory is declared while the TSW holds the
// monitor, and the battle ends on the release frame: (1) the Empire's retreat completed inside the hold -> all-retreated;
// (2) the Empire's leader was on the planet's surface -> galactic claim 7 at state 9 and the story victory 0x10 for the
// TSW owner (Underworld) at the release, the Empire retreating. 0 / 4 workers identical.
void test_tsw_follow(const Data& d) {
    for (int follow = 1; follow <= 2; ++follow) {
        const Run a = run(d, 0, follow);
        std::printf("  TSW follow %d: fire %u destroy %u debris %u last held %u first unheld %u end %u; galaxy same tick %d fleets kept %d "
                    "owner kept %d dummy killed %d story %d claims %zu; %s\n",
                    follow, a.tsw_fire_frame, a.destroy_frame, a.debris_frame, a.last_held, a.first_unheld, a.end_frame, a.galaxy_same_tick,
                    a.fleets_kept, a.owner_kept, a.dummy_killed, a.story_pending, a.claims, a.summary.c_str());
        CHECK(a.tsw_fired && a.ended && a.planet_destroyed);
        // 5 (30) + 6 + 7 + 8 (15) + 9 with the L1_Death_Star values: F + 30 + 1 + 1 + 15 + 1.
        CHECK(a.destroy_frame == a.tsw_fire_frame + 48);
        CHECK(a.debris_frame == a.destroy_frame + 10);
        CHECK(a.galaxy_same_tick && a.fleets_kept && a.owner_kept && a.dummy_killed);
        // Held on every frame from the fire until a release, never held again after it, never finished while held.
        // story t06 (BT §3.1a): follow 1 - the Empire's retreat completes inside the hold and releases every level (0x340750,
        // no replay), 0xc is declared at once and the battle ends 210 frames later; follow 2 - the TSW's state-13 service
        // (destroy + 10 + 40 + 1) releases the hold and replays the queued story victory {0x10, 30 frames}.
        CHECK(a.held_from_fire && !a.finished_while_held && a.first_unheld != 0 && a.last_held == a.first_unheld - 1);
        if (follow == 1) CHECK(a.first_unheld > a.tsw_fire_frame && a.first_unheld < a.destroy_frame && a.end_frame == a.first_unheld + 210);
        else CHECK(a.first_unheld == a.destroy_frame + 10 + 40 + 1 && a.end_frame == a.first_unheld + 30);
        if (follow == 1) {
            CHECK(!a.story_pending && a.claims == 0);
            CHECK(a.summary.find("winner Underworld, victory all-retreated, retreated Empire") != std::string::npos);
        } else {
            CHECK(a.story_pending && a.claims == 1);
            CHECK(a.summary.find("winner Underworld, victory 16, retreated Empire") != std::string::npos);
        }
        const Run b = run(d, 4, follow);
        const auto dv = first_divergence(a.crc, b.crc);
        CHECK(!dv.diverged && b.trace == a.trace && b.end_frame == a.end_frame);
    }
}

// L1 systems t06 (docs/re/BATTLE_TRANSITIONS.md §3.1a item 1, TSW_PLANET_DESTROY.md §8; probe 2026-10-31, l1sys t06):
// only victory-relevant types keep a side alive. The test types set Victory_Relevant Yes, L1_Death_Star sets none of the
// three flags (like Death_Star_II). After the planet kill and the hold release the Underworld (attacker) escort frigates
// die (the third one retreated at frame 2+5+90); with only the Death Star left the Empire (defender) wins all-destroyed,
// declared at the deaths 90 frames after the Hyperspace_Away (story t06: the 0xb test of the escorts' loss) and ended 210
// frames later, the Death Star alive and undamaged. 0 / 4 workers.
void test_tsw_lost(const Data& d) {
    const Run a = run(d, 0, 3);
    std::printf("  TSW lost: destroy %u first unheld %u kill %u (escorts %zu) end %u; Death Star alive %d health %.1f (start %.1f); %s\n",
                a.destroy_frame, a.first_unheld, a.kill_frame, a.escorts_killed, a.end_frame, a.ds_alive, static_cast<double>(a.ds_health),
                static_cast<double>(a.ds_health0), a.summary.c_str());
    CHECK(a.tsw_fired && a.planet_destroyed && a.ended);
    CHECK(a.first_unheld == a.destroy_frame + 10 + 40 + 1 && a.kill_frame == a.first_unheld && a.escorts_killed == 2);
    CHECK(a.end_frame == a.kill_frame + 90 + 210);
    CHECK(a.ds_alive && a.ds_health0 > 0.0f && a.ds_health == a.ds_health0);  // alive and undamaged
    CHECK(a.summary.find("winner Empire, victory all-destroyed") != std::string::npos);
    const Run b = run(d, 4, 3);
    const auto dv = first_divergence(a.crc, b.crc);
    CHECK(!dv.diverged && b.trace == a.trace && b.end_frame == a.end_frame);
}

void test_land(const Data& d) {
    const LandRun a = run_land(d, 0);
    std::printf("  land: fought %d; weather on %d advanced %d forced %d rain sight %d; bombardment refused-before-ready %d started %d hit %d "
                "ready again %d; bombing run started %d flew %d done %d; trace %016" PRIx64 "\n",
                a.fought, a.weather_on, a.weather_advanced, a.forced, a.rain_sight, a.refused_unavailable, a.bombard_started, a.bombard_hit,
                a.bombard_reset, a.run_started, a.bombers_flew, a.run_done, a.trace);
    CHECK(a.fought && a.weather_on && a.weather_advanced && a.forced && a.rain_sight);
    CHECK(a.refused_unavailable && a.bombard_started && a.bombard_hit && a.bombard_reset);
    CHECK(a.run_started && a.bombers_flew && a.run_done);
    const LandRun w = run_land(d, 4);
    CHECK(!first_divergence(a.crc, w.crc).diverged && a.trace == w.trace);
}
// ---- S7 t10 / t14: the MoveToPosition switch-locomotor bit through order_move, weapons suspended on the jetpack ----
// (MOVEMENT §3.1.1 step 6 / §6.2) A Uses_Multiple_Locomotors trooper (WALK primary, JETPACK secondary, a blaster) is ordered
// with the switch bit next to an enemy trooper: it swaps, flies and lands, then swaps back to WALK. While on the jetpack
// the combat flag kUnitWeaponsSuspended mirrors the active locomotor and it fires nothing; back on WALK it fires again.
// S7 t15 A1: the swap back raises "unit ability finished" (signal 0x29) for the jet trooper in the tick its flag clears.
struct JetRun {
    bool fought = false, ordered = false, mirror = true, flew = false, landed = false, other_walks = true;
    bool current_projectile = false;
    uint32_t shots_flying = 0, shots_after = 0, suspended_frames = 0;
    uint32_t ability_finished = 0, finished_at_clear = 0;  // the jet's signals; those in the tick its flag cleared
    uint32_t forced_frame = 0, signal_frame = 0;  // S7 t27: the host-side swap back (after that battle frame) / the signal's frame
    CrcLog crc;
};

// force_back (S7 t27): after the first airborne frame the test swaps the trooper back to WALK outside the movement tick (as
// a host-side swap back would, MOVEMENT §3.1.1 callers); the next tick reports signal 0x29 once and clears the flag.
JetRun run_jet(const Data& d, int32_t workers, bool force_back = false) {
    JetRun out;
    jobs::JobSystem js(jobs::JobSystemConfig{workers});
    SimConfig cfg;
    cfg.seed = 11;
    cfg.players = 4;
    SimHost host(d.types, js, cfg);
    galactic::GalacticSetup s = make_setup(d);
    const uint32_t sq = type_of(d, "S10_Squad"), fr = type_of(d, "S10_Frigate"), jsq = type_of(d, "L1_Jet_Squad");
    s.forces = {{0, 1, sq}, {1, 2, fr}, {1, 2, jsq}};
    galactic::GalacticSim* gs = host.start_galactic(d.rules, std::move(s));
    CHECK(gs != nullptr);
    if (!gs) return out;
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 3000;
    opt.battle.order_interval = 0;
    opt.battle.fog = false;
    CampaignBattles battles(host, d.crules, *d.catalog,
                            [](const std::string& name, terrain::MapData& m) {
                                if (name != "L1_Land_B") return false;
                                m = land_map();
                                return true;
                            },
                            opt);
    CHECK(battles.attach());
    std::vector<objects::ObjectHandle> up;
    for (objects::ObjectHandle h : gs->planets()[2].surface)
        if (gs->store().owner(h) == 1) up.push_back(h);
    CHECK(gs->launch_units(1, 2, up) == commands::RejectReason::None);
    const std::vector<uint32_t> uf = gs->fleets_at(2, 1);
    CHECK(!uf.empty());
    if (!uf.empty()) CHECK(gs->move_fleet(1, uf[0], 1) == commands::RejectReason::None);
    bool invaded = false;
    const auto jet_t = static_cast<objects::TypeIndex>(type_of(d, "L1_Jet_Trooper"));
    objects::ObjectId jet = objects::kNoObjectId, other = objects::kNoObjectId;
    std::vector<uint32_t> seen;  // projectile serials of the jet trooper
    bool was_suspended = false;
    for (uint64_t t = 0; t < 6000; ++t) {
        host.tick_once();
        if (!invaded && host.modes().depth() == 1) {
            const std::vector<uint32_t> f = gs->fleets_at(1, 1);
            if (!f.empty() && !gs->fleets()[f[0]].moving) invaded = gs->invade(1, 1, {}) == commands::RejectReason::None;
        }
        TacticalBattle* b = battles.battle();
        TacticalMode* tm = host.tactical();
        if (!b || !tm) {
            if (out.fought) break;
            continue;
        }
        out.fought = true;
        objects::ObjectStore& st = tm->primary();
        const uint32_t f = b->frame();
        if (f == 1) {
            for (const objects::ObjectHandle h : st.owned_by(1))
                if (st.alive(h) && st.type(h) == jet_t && b->combat().has_unit(st.id(h))) (jet == objects::kNoObjectId ? jet : other) = st.id(h);
            Vec3 enemy{};
            bool found = false;
            for (const objects::ObjectHandle h : st.owned_by(0))
                if (!found && st.alive(h) && b->combat().has_unit(st.id(h))) {
                    enemy = st.position(h);
                    found = true;
                }
            CHECK(jet != objects::kNoObjectId && other != objects::kNoObjectId && found);
            if (jet == objects::kNoObjectId || !found) break;
            // S8 t12 (d): Get_Current_Projectile_Type = Projectile_Types[weapon index 0]; a unit without the list: nothing
            out.current_projectile = b->current_projectile(jet) == static_cast<objects::TypeIndex>(type_of(d, "L1_Laser_Proj"));
            const objects::ObjectId ids[] = {jet};
            out.ordered = b->order_move(*tm, 1, ids, enemy.x + 80.0f, enemy.y, host.random().synced(), tactical::movement::kNoFacing,
                                        false, false, 0, true) == commands::RejectReason::None;
            out.ordered = out.ordered && b->movement().view(jet).secondary_locomotor;  // swapped at once (§6.2)
            continue;  // the combat flag follows at the next tick, before its combat phase
        }
        if (jet == objects::kNoObjectId || !b->combat().has_unit(jet)) continue;
        const tactical::movement::UnitView v = b->movement().view(jet);
        const bool suspended = (b->combat().flags(jet) & tactical::combat::kUnitWeaponsSuspended) != 0;
        out.mirror = out.mirror && suspended == v.secondary_locomotor;
        if (suspended) ++out.suspended_frames;
        for (const objects::ObjectId e : b->ability_finished_events())
            if (e == jet) {
                ++out.ability_finished;
                out.finished_at_clear += was_suspended && !suspended ? 1u : 0u;
                if (out.signal_frame == 0) out.signal_frame = f;
            }
        was_suspended = suspended;
        if (force_back && out.forced_frame == 0 && suspended && v.airborne) {
            out.forced_frame = f;
            CHECK(const_cast<tactical::movement::MovementSystem&>(b->movement()).use_secondary_locomotor(jet, false));
        }
        out.flew = out.flew || v.airborne;
        if (out.flew && !v.secondary_locomotor && !out.landed) out.landed = v.locomotor == tactical::movement::LocomotorKind::Default;
        out.other_walks = out.other_walks && !b->movement().view(other).secondary_locomotor &&
                          (b->combat().flags(other) & tactical::combat::kUnitWeaponsSuspended) == 0;
        for (size_t i = 0; i < b->combat().projectile_count(); ++i) {
            const tactical::combat::ProjectileView p = b->combat().projectile(i);
            if (p.shooter != jet || std::find(seen.begin(), seen.end(), p.serial) != seen.end()) continue;
            seen.push_back(p.serial);
            if (suspended) ++out.shots_flying;
            else if (out.landed) ++out.shots_after;
        }
        if (out.landed && out.shots_after > 0) break;
    }
    out.crc = host.crc_log();
    return out;
}

// ---- q6 t12: object-script spawns of types outside the battle tables, script Take_Damage ----
// An object script's Spawn_Unit of a type no participant brought (L1_Scout, the company L1_Deploy_Squad) used to make a
// store prop (no combat, movement, AI). TacticalBattle::script_spawn / script_spawn_company extend the battle tables and
// make battle units: combat + movement registered, a move order of the owner accepted, the company's members contained
// in its TEAM. script_damage is damage kind 0 through combat (passes invulnerability; Set_Cannot_Be_Killed holds at the
// floor) and processes the kill at once.
struct ScriptSpawnRun {
    bool fought = false, scout_unit = false, scout_moves = false, company_units = false, company_moves = false;
    bool held = false, killed = false, scout_moved = false;
    uint32_t members = 0;
    CrcLog crc;
};

ScriptSpawnRun run_script_spawn(const Data& d, int32_t workers) {
    ScriptSpawnRun out;
    jobs::JobSystem js(jobs::JobSystemConfig{workers});
    SimConfig cfg;
    cfg.seed = 12;
    cfg.players = 4;
    SimHost host(d.types, js, cfg);
    galactic::GalacticSetup s = make_setup(d);
    const uint32_t sq = type_of(d, "S10_Squad"), fr = type_of(d, "S10_Frigate");
    s.forces = {{0, 1, sq}, {1, 2, fr}, {1, 2, sq}};
    galactic::GalacticSim* gs = host.start_galactic(d.rules, std::move(s));
    CHECK(gs != nullptr);
    if (!gs) return out;
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 3000;
    opt.battle.order_interval = 0;
    opt.battle.fog = false;
    CampaignBattles battles(host, d.crules, *d.catalog,
                            [](const std::string& name, terrain::MapData& m) {
                                if (name != "L1_Land_B") return false;
                                m = land_map();
                                return true;
                            },
                            opt);
    CHECK(battles.attach());
    std::vector<objects::ObjectHandle> up;
    for (objects::ObjectHandle h : gs->planets()[2].surface)
        if (gs->store().owner(h) == 1) up.push_back(h);
    CHECK(gs->launch_units(1, 2, up) == commands::RejectReason::None);
    const std::vector<uint32_t> uf = gs->fleets_at(2, 1);
    CHECK(!uf.empty());
    if (!uf.empty()) CHECK(gs->move_fleet(1, uf[0], 1) == commands::RejectReason::None);
    bool invaded = false;
    const auto scout_t = static_cast<objects::TypeIndex>(type_of(d, "L1_Scout"));
    const auto squad_t = static_cast<objects::TypeIndex>(type_of(d, "L1_Deploy_Squad"));
    const auto member_t = static_cast<objects::TypeIndex>(type_of(d, "L1_Deploy_Trooper"));
    const auto team_sq_t = static_cast<objects::TypeIndex>(type_of(d, "L1_Team_Squad"));
    const auto team_member_t = static_cast<objects::TypeIndex>(type_of(d, "L1_Team_Trooper"));
    const auto team_t = static_cast<objects::TypeIndex>(type_of(d, "L1_Team"));
    objects::ObjectId scout = objects::kNoObjectId;
    tactical::movement::V2 scout_from{};
    for (uint64_t t = 0; t < 6000; ++t) {
        host.tick_once();
        if (!invaded && host.modes().depth() == 1) {
            const std::vector<uint32_t> f = gs->fleets_at(1, 1);
            if (!f.empty() && !gs->fleets()[f[0]].moving) invaded = gs->invade(1, 1, {}) == commands::RejectReason::None;
        }
        TacticalBattle* b = battles.battle();
        TacticalMode* tm = host.tactical();
        if (!b || !tm) {
            if (out.fought) break;
            continue;
        }
        out.fought = true;
        objects::ObjectStore& st = tm->primary();
        const uint32_t f = b->frame();
        if (f == 1) {
            Vec3 at{};
            bool found = false;
            for (const objects::ObjectHandle h : st.owned_by(1))
                if (!found && st.alive(h) && b->combat().has_unit(st.id(h))) {
                    at = st.position(h);
                    found = true;
                }
            CHECK(found);
            if (!found) break;
            objects::SpawnParams p;
            p.type = scout_t;
            p.owner = 1;
            p.position = Vec3{at.x + 20.0f, at.y, at.z};
            p.facing = Vec3{1.0f, 0.0f, 0.0f};
            const objects::ObjectHandle sh = b->script_spawn(*tm, p);
            scout = sh ? st.id(sh) : objects::kNoObjectId;
            out.scout_unit = sh && st.type(sh) == scout_t && b->combat().has_unit(scout) && b->movement().has_unit(scout);
            if (!out.scout_unit) break;
            scout_from = b->movement().view(scout).pos;
            const objects::ObjectId ids[] = {scout};
            out.scout_moves = b->order_move(*tm, 1, ids, scout_from.x + 60.0f, scout_from.y, host.random().synced()) ==
                              commands::RejectReason::None;
            // A company without Create_Team: two members, the first returned; with Create_Team + Create_Team_Type: two
            // members and their L1_Team ability container, which is returned.
            auto company = [&](objects::TypeIndex ct, objects::TypeIndex mt, float dy, bool team) {
                p.type = ct;
                p.position = Vec3{at.x, at.y + dy, at.z};
                const objects::ObjectHandle ch = b->script_spawn_company(*tm, p);
                std::vector<objects::ObjectId> mids;
                for (const objects::ObjectHandle h : st.owned_by(1))
                    if (st.alive(h) && st.type(h) == mt && b->combat().has_unit(st.id(h)) && b->movement().has_unit(st.id(h)))
                        mids.push_back(st.id(h));
                out.members += static_cast<uint32_t>(mids.size());
                bool ok = ch && mids.size() == 2;
                if (team) ok = ok && st.type(ch) == team_t && b->abilities().has_object(st.id(ch));  // the ability TEAM container
                else ok = ok && (st.id(ch) == mids[0] || st.id(ch) == mids[1]);
                out.company_units = out.company_units && ok;
                out.company_moves = out.company_moves && !mids.empty() &&
                                    b->order_move(*tm, 1, mids, at.x + 60.0f, at.y + dy, host.random().synced()) == commands::RejectReason::None;
            };
            out.company_units = out.company_moves = true;
            company(squad_t, member_t, 30.0f, false);
            company(team_sq_t, team_member_t, -30.0f, true);
            continue;
        }
        if (scout == objects::kNoObjectId) continue;
        if (f == 120) {
            out.scout_moved = b->combat().has_unit(scout) && b->movement().view(scout).pos.x > scout_from.x + 1.0f;
            // Cannot-be-killed holds at the floor; then a kill through combat, processed at once (no combat unit left).
            out.held = b->script_damage(*tm, scout, 1.0e7f, true) && b->combat().has_unit(scout) && b->combat().alive(scout);
            out.killed = b->script_damage(*tm, scout, 1.0e7f, false) && !st.alive(st.find(scout));
        }
        if (f >= 200) break;
    }
    out.crc = host.crc_log();
    return out;
}

void test_script_spawn(const Data& d) {
    const ScriptSpawnRun a = run_script_spawn(d, 0);
    std::printf("  script spawn: fought %d; scout unit %d ordered %d moved %d; companies %d (%u members) ordered %d; "
                "cannot-die held %d, killed %d\n",
                a.fought, a.scout_unit, a.scout_moves, a.scout_moved, a.company_units, a.members, a.company_moves, a.held, a.killed);
    CHECK(a.fought && a.scout_unit && a.scout_moves && a.scout_moved);
    CHECK(a.company_units && a.company_moves);
    CHECK(a.held && a.killed);
    const ScriptSpawnRun w = run_script_spawn(d, 4);
    CHECK(!first_divergence(a.crc, w.crc).diverged);
}

void test_jet(const Data& d) {
    const JetRun a = run_jet(d, 0);
    std::printf("  jetpack: fought %d ordered %d flew %d landed %d mirror %d suspended %u frames, shots in flight %u, after %u\n",
                a.fought, a.ordered, a.flew, a.landed, a.mirror, a.suspended_frames, a.shots_flying, a.shots_after);
    CHECK(a.fought && a.ordered && a.flew && a.landed && a.mirror && a.other_walks);
    CHECK(a.suspended_frames > 30 && a.shots_flying == 0 && a.shots_after > 0);
    CHECK(a.ability_finished == 1 && a.finished_at_clear == 1);
    CHECK(a.current_projectile);
    const JetRun w = run_jet(d, 4);
    CHECK(!first_divergence(a.crc, w.crc).diverged && w.suspended_frames == a.suspended_frames && w.ability_finished == 1);
    // S7 t27: a swap back outside the movement tick is reported once, in the first tick after it, with the flag cleared.
    const JetRun fb = run_jet(d, 0, true);
    std::printf("  jetpack forced swap back after frame %u: signal at frame %u (%u signals), mirror %d\n", fb.forced_frame,
                fb.signal_frame, fb.ability_finished, fb.mirror);
    CHECK(fb.forced_frame > 0 && fb.signal_frame == fb.forced_frame + 1 && fb.ability_finished == 1 && fb.finished_at_clear == 1);
    CHECK(fb.mirror && fb.shots_flying == 0);
}

// ---- S7 t26: DEPLOY raises "unit ability finished" (signal 0x29) at the deploy completion ----
// (AI_TACTICAL §7.3.3 item 3) A deploy squad is ordered DEPLOY on, then off: each deploy / undeploy clip end raises 0x29
// once for the trooper (S8 t11: oi_deploy_sync from the movement deploy events; the trooper has no deploy clip, so the
// clip ends in the next tick) and only then is the ability active / inactive; a second DEPLOY on while deployed changes
// nothing and raises nothing.
struct DeployRun {
    bool fought = false, deployed = false, undeployed = false, repeat_refused = false;
    uint32_t on_frame = 0, off_frame = 0, signals = 0, signal_on = 0, signal_off = 0, other_signals = 0;
    uint32_t deployed_at = 0, undeployed_at = 0;  // the frames Is_Active flipped (the clip ends)
};
DeployRun run_deploy(const Data& d) {
    DeployRun out;
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    SimConfig cfg;
    cfg.seed = 11;
    cfg.players = 4;
    SimHost host(d.types, js, cfg);
    galactic::GalacticSetup s = make_setup(d);
    const uint32_t sq = type_of(d, "S10_Squad"), fr = type_of(d, "S10_Frigate"), dsq = type_of(d, "L1_Deploy_Squad");
    s.forces = {{0, 1, sq}, {1, 2, fr}, {1, 2, dsq}};
    galactic::GalacticSim* gs = host.start_galactic(d.rules, std::move(s));
    CHECK(gs != nullptr);
    if (!gs) return out;
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 3000;
    opt.battle.order_interval = 0;
    opt.battle.fog = false;
    CampaignBattles battles(host, d.crules, *d.catalog,
                            [](const std::string& name, terrain::MapData& m) {
                                if (name != "L1_Land_B") return false;
                                m = land_map();
                                return true;
                            },
                            opt);
    CHECK(battles.attach());
    std::vector<objects::ObjectHandle> up;
    for (objects::ObjectHandle h : gs->planets()[2].surface)
        if (gs->store().owner(h) == 1) up.push_back(h);
    CHECK(gs->launch_units(1, 2, up) == commands::RejectReason::None);
    const std::vector<uint32_t> uf = gs->fleets_at(2, 1);
    CHECK(!uf.empty());
    if (!uf.empty()) CHECK(gs->move_fleet(1, uf[0], 1) == commands::RejectReason::None);
    bool invaded = false;
    const auto dep_t = static_cast<objects::TypeIndex>(type_of(d, "L1_Deploy_Trooper"));
    objects::ObjectId dep = objects::kNoObjectId;
    namespace ab = tactical::abilities;
    for (uint64_t t = 0; t < 6000; ++t) {
        host.tick_once();
        if (!invaded && host.modes().depth() == 1) {
            const std::vector<uint32_t> f = gs->fleets_at(1, 1);
            if (!f.empty() && !gs->fleets()[f[0]].moving) invaded = gs->invade(1, 1, {}) == commands::RejectReason::None;
        }
        TacticalBattle* b = battles.battle();
        TacticalMode* tm = host.tactical();
        if (!b || !tm) {
            if (out.fought) break;
            continue;
        }
        out.fought = true;
        objects::ObjectStore& st = tm->primary();
        const uint32_t f = b->frame();
        if (dep != objects::kNoObjectId && out.on_frame != 0 && b->combat().has_unit(dep)) {  // the clip ends: Is_Active flips
            const bool act = b->abilities().is_active(dep, ab::kDeploy);
            if (act && out.deployed_at == 0) out.deployed_at = f;
            if (out.off_frame != 0 && !act && out.undeployed_at == 0) out.undeployed_at = f;
        }
        for (const objects::ObjectId e : b->ability_finished_events()) {
            if (e != dep) {
                ++out.other_signals;
                continue;
            }
            ++out.signals;
            if (out.deployed_at != 0 && f == out.deployed_at) ++out.signal_on;
            if (out.undeployed_at != 0 && f == out.undeployed_at) ++out.signal_off;
        }
        if (f == 1) {
            for (const objects::ObjectHandle h : st.owned_by(1))
                if (dep == objects::kNoObjectId && st.alive(h) && st.type(h) == dep_t && b->combat().has_unit(st.id(h))) dep = st.id(h);
            CHECK(dep != objects::kNoObjectId);
            if (dep == objects::kNoObjectId) break;
        }
        if (dep == objects::kNoObjectId) continue;
        const objects::ObjectId ids[] = {dep};
        // S8 t11: with the Walk locomotor Is_Active(DEPLOY) follows the walk state (0x19 Deployed), so it is read after the
        // clip end (this model has no clip data: the clip ends in the next tick), not right after the request.
        if (f == 10) {  // applied in the next tick's ability phase: the signal is in frame 11's events
            out.deployed = b->abilities().set_unit_ability_mode(ids, ab::kDeploy, true, f) == 1;
            out.on_frame = f;
        }
        if (f == 20) {
            out.deployed = out.deployed && b->abilities().is_active(dep, ab::kDeploy);
            out.repeat_refused = b->abilities().set_unit_ability_mode(ids, ab::kDeploy, true, f) == 0;
        }
        if (f == 30) {
            out.undeployed = b->abilities().set_unit_ability_mode(ids, ab::kDeploy, false, f) == 1;
            out.off_frame = f;
        }
        if (f == 40) out.undeployed = out.undeployed && !b->abilities().is_active(dep, ab::kDeploy);
        if (f >= 60) break;
    }
    return out;
}

void test_deploy(const Data& d) {
    const DeployRun r = run_deploy(d);
    std::printf("  deploy: fought %d on %d (frame %u, deployed at %u) repeat refused %d off %d (frame %u, undeployed at %u): %u signals "
                "(at the clip ends: on %u, off %u), others %u\n",
                r.fought, r.deployed, r.on_frame, r.deployed_at, r.repeat_refused, r.undeployed, r.off_frame, r.undeployed_at, r.signals,
                r.signal_on, r.signal_off, r.other_signals);
    CHECK(r.fought && r.deployed && r.repeat_refused && r.undeployed);
    CHECK(r.signals == 2 && r.signal_on == 1 && r.signal_off == 1 && r.deployed_at > r.on_frame && r.undeployed_at > r.off_frame);
}

// ---- Int-final: build pads (L1_S4_REMAINING §3.6) and the weather factor of the fog sight (FOG_OF_WAR §3.1.1) ----
struct PadRun {
    bool fought = false, refused_rules = false, refused_full = false, started = false, uc_seen = false, built = false, cleared = false;
    bool cooldown_refused = false, rebuilt = false, locked = false;
    bool other_list_refused = false;  // t50: the type of the other mode's list (_Multiplayer in a campaign battle, _Campaign in a land skirmish)
    // q6 t19 (AI_TACTICAL §7.5.1): an enemy builder is refused; the UC belongs to the builder and the pad keeps its owner;
    // a Neutral-owned pad (player 2) refuses every build in skirmish (relation(Neutral, Neutral) = 2); the pad owner after
    // its child died.
    bool enemy_builder_refused = false, uc_owner_ok = false, neutral_refused = false;
    int32_t owner_after_death = -9;
    // q6 t19 (SKIRMISH_SETUP §3.1): a finished land build is a pool entry (key >= kBuiltReserveKeyBase, no galactic
    // counterpart); in a skirmish a type without a Company_Transport_Unit (S10_Squad) cannot be dropped and stays pooled.
    bool pool_added = false, pool_drop_refused = false;
    float sight_clear = -1.0f, sight_rain = -1.0f;
    bool fog_on = false;
    CrcLog crc;
};

PadRun run_pads(const Data& d, int32_t workers, bool skirmish = false) {
    PadRun out;
    jobs::JobSystem js(jobs::JobSystemConfig{workers});
    SimConfig cfg;
    cfg.seed = 11;
    cfg.players = 4;
    SimHost host(d.types, js, cfg);
    galactic::GalacticSetup s = make_setup(d);
    const uint32_t sq = type_of(d, "S10_Squad"), fr = type_of(d, "S10_Frigate");
    s.forces = {{0, 1, sq}, {1, 2, fr}, {1, 2, sq}};
    galactic::GalacticSim* gs = host.start_galactic(d.rules, std::move(s));
    CHECK(gs != nullptr);
    if (!gs) return out;
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 900;
    opt.battle.order_interval = 0;
    opt.battle.skirmish = skirmish;  // t50: a land skirmish lists / prices its pads from the _Multiplayer tags
    load_tactical_systems(*d.files, d.rules, &d.db.constants(), opt);  // the hosts' loader (weather on)
    CHECK(opt.weather != nullptr);
    CampaignBattles battles(host, d.crules, *d.catalog,
                            [](const std::string& name, terrain::MapData& m) {
                                if (name != "L1_Land_B") return false;
                                m = land_map();
                                return true;
                            },
                            opt);
    CHECK(battles.attach());
    std::vector<objects::ObjectHandle> up;
    for (objects::ObjectHandle h : gs->planets()[2].surface)
        if (gs->store().owner(h) == 1) up.push_back(h);
    CHECK(gs->launch_units(1, 2, up) == commands::RejectReason::None);
    const std::vector<uint32_t> uf = gs->fleets_at(2, 1);
    if (!uf.empty()) CHECK(gs->move_fleet(1, uf[0], 1) == commands::RejectReason::None);
    bool invaded = false;
    objects::ObjectId pad = objects::kNoObjectId, scout = objects::kNoObjectId;
    const auto uc_t = static_cast<objects::TypeIndex>(type_of(d, skirmish ? "L1_UC_Turret_MP" : "L1_UC_Turret"));
    const auto other_t = static_cast<objects::TypeIndex>(type_of(d, skirmish ? "L1_UC_Turret" : "L1_UC_Turret_MP"));
    const auto fr_t = static_cast<objects::TypeIndex>(fr);
    uint32_t built_frame = 0;
    for (uint64_t t = 0; t < 4000; ++t) {
        host.tick_once();
        if (!invaded && host.modes().depth() == 1) {
            const std::vector<uint32_t> f = gs->fleets_at(1, 1);
            if (!f.empty() && !gs->fleets()[f[0]].moving) invaded = gs->invade(1, 1, {}) == commands::RejectReason::None;
        }
        TacticalBattle* b = battles.battle();
        TacticalMode* tm = host.tactical();
        if (!b || !tm) {
            if (out.fought) break;
            continue;
        }
        out.fought = true;
        objects::ObjectStore& st = tm->primary();
        const uint32_t f = b->frame();
        if (f == 1) {
            objects::SpawnParams sp;
            sp.type = static_cast<objects::TypeIndex>(type_of(d, "L1_Build_Pad"));
            sp.owner = 1;
            sp.position = {-1500.0f, -1500.0f, 0.0f};
            pad = st.id(tm->spawn(sp));
            sp.type = static_cast<objects::TypeIndex>(type_of(d, "L1_Scout"));
            sp.position = {-1400.0f, -1500.0f, 0.0f};
            scout = st.id(tm->spawn(sp));
            out.refused_rules = b->pad_build(*tm, pad, fr_t, true) == 0;  // not in the faction row
            out.other_list_refused = b->pad_build(*tm, pad, other_t, true) == 0;  // t50: only the mode's own list counts
            out.enemy_builder_refused = b->pad_build(*tm, pad, uc_t, false, 0) == -1;  // q6 t19: player 0 is not allied with 1
            out.started = b->pad_build(*tm, pad, uc_t, true) == 1;
            if (const objects::ObjectHandle uh = st.find(b->pad_contents(pad)); st.alive(uh))
                out.uc_owner_ok = st.owner(uh) == 1 && st.owner(st.find(pad)) == 1;
            sp.type = static_cast<objects::TypeIndex>(type_of(d, "L1_Build_Pad"));
            sp.owner = 2;  // the Neutral player
            sp.position = {1500.0f, 1500.0f, 0.0f};
            const objects::ObjectId npad = st.id(tm->spawn(sp));
            out.neutral_refused = b->pad_build(*tm, npad, uc_t, false) == -1;
            out.refused_full = b->pad_build(*tm, pad, uc_t, false) == -1;  // already has something built on it
            b->pad_lock(pad, true);
            out.locked = b->pad_locked(pad);
        }
        if (f == 2) {
            objects::ObjectId key = objects::kNoObjectId;
            const bool added = b->add_built_reserve(1, sq, key);
            bool listed = false;
            for (const TacticalBattle::Reserve& r : b->reserves(1)) listed = listed || (r.galactic_id == key && r.type == sq);
            out.pool_added = added && key >= TacticalBattle::kBuiltReserveKeyBase && listed;
            if (skirmish) {
                const commands::RejectReason rr = b->order_reinforce(*tm, 1, key, -1000.0f, -1000.0f, host.random().synced());
                bool still = false;
                for (const TacticalBattle::Reserve& r : b->reserves(1)) still = still || r.galactic_id == key;
                out.pool_drop_refused = rr != commands::RejectReason::None && still;
            }
        }
        const objects::ObjectId c = b->pad_contents(pad);
        if (f > 1 && f < 60 && c != objects::kNoObjectId && st.alive(st.find(c)) && st.type(st.find(c)) == uc_t) out.uc_seen = true;
        if (!out.built && c != objects::kNoObjectId && st.alive(st.find(c)) && st.type(st.find(c)) == static_cast<objects::TypeIndex>(type_of(d, "L1_Turret"))) {
            out.built = f >= 61;  // 2 s x 30
            built_frame = f;
            tm->destroy(st.find(c));  // the built object dies -> pad cleared, lock cleared, 1 s cooldown
        }
        if (built_frame && f == built_frame + 1) {
            out.cleared = b->pad_contents(pad) == objects::kNoObjectId && !b->pad_locked(pad);
            out.owner_after_death = st.owner(st.find(pad));
            out.cooldown_refused = b->pad_build(*tm, pad, uc_t, false) == -1;
        }
        if (built_frame && f == built_frame + 40) out.rebuilt = b->pad_build(*tm, pad, uc_t, false) == 1;
        tactical::fog::FogObject fo;
        if (scout != objects::kNoObjectId && b->fog() && b->fog_object(scout, fo)) {
            out.fog_on = true;
            const bool rain = b->weather().type() == tactical::combat::WeatherType::Rain && b->weather().intensity() > 0.5f;
            if (rain) out.sight_rain = fo.sight_range;
            else if (b->weather().intensity() <= 0.5f) out.sight_clear = fo.sight_range;
        }
        if (out.rebuilt && out.sight_rain >= 0.0f && out.sight_clear >= 0.0f) break;
    }
    out.crc = host.crc_log();
    return out;
}

void test_pads(const Data& d) {
    const PadRun a = run_pads(d, 0);
    std::printf("  pads: fought %d refused-rules %d started %d refused-full %d locked %d uc %d built %d cleared %d cooldown %d rebuilt %d; "
                "fog %d sight clear %.1f rain %.1f\n",
                a.fought, a.refused_rules, a.started, a.refused_full, a.locked, a.uc_seen, a.built, a.cleared, a.cooldown_refused, a.rebuilt,
                a.fog_on, static_cast<double>(a.sight_clear), static_cast<double>(a.sight_rain));
    CHECK(a.fought && a.refused_rules && a.started && a.refused_full && a.locked && a.uc_seen && a.built && a.cleared && a.cooldown_refused &&
          a.rebuilt);
    CHECK(a.fog_on && a.sight_clear == 300.0f && a.sight_rain == 150.0f);  // Land_FOW_Reveal_Range x Sight_Range_Modifier 0.5
    CHECK(a.other_list_refused);
    const PadRun w = run_pads(d, 4);
    CHECK(!first_divergence(a.crc, w.crc).diverged);
    // t50: the same pad in a land skirmish builds from Tactical_Buildable_Objects_Multiplayer and refuses the _Campaign list.
    const PadRun k = run_pads(d, 0, true);
    std::printf("  pads (land skirmish): fought %d refused-rules %d other-list-refused %d started %d uc %d built %d cleared %d rebuilt %d\n", k.fought,
                k.refused_rules, k.other_list_refused, k.started, k.uc_seen, k.built, k.cleared, k.rebuilt);
    CHECK(k.fought && k.refused_rules && k.other_list_refused && k.started && k.refused_full && k.uc_seen && k.built && k.cleared && k.rebuilt);
    std::printf("  pads q6 t19: enemy builder refused %d/%d, uc owner %d/%d, neutral pad refused %d/%d, owner after child death %d/%d\n",
                a.enemy_builder_refused, k.enemy_builder_refused, a.uc_owner_ok, k.uc_owner_ok, a.neutral_refused, k.neutral_refused,
                a.owner_after_death, k.owner_after_death);
    CHECK(a.enemy_builder_refused && k.enemy_builder_refused && a.uc_owner_ok && k.uc_owner_ok);
    CHECK(k.neutral_refused && !a.neutral_refused);  // campaign: the Neutral player is allied with itself (p == q)
    std::printf("  pads q6 t19: built pool entry %d/%d, skirmish drop without a transport refused %d\n", a.pool_added, k.pool_added,
                k.pool_drop_refused);
    CHECK(a.pool_added && k.pool_added && k.pool_drop_refused);
}
// --- end Int-final ---

// ---- q6: persisted tactical built objects (AI_GALACTIC_TOKENS §9.4) ----
// P1 (Empire, player 0) is invaded by the Underworld (player 1). Before the battle two records are put on P1: R1 for the map
// pad at (-1500, -1500) (a turret of player 0) and R2 at a spot without a pad (player 1). At setup R1's turret is recreated
// on the pad for its owner; R2 creates nothing and stays. A second pad (spawned at frame 1, owned by player 1) builds a
// turret: its completion appends a record {finished type, the object's position, base level 0, pad owner}. Selling R1's
// turret removes R1. After the battle only the winner's records remain (0x2bff60). A land skirmish does none of it.
struct PersistRun {
    bool fought = false, restored = false, restored_ok = false, built = false, added_ok = false, sold = false, sold_ok = false;
    // q6 t19 (AI_GALACTIC_TOKENS §9.11, AI_TACTICAL §7.5.1 item 5): the recreated child keeps the pad's owner (pad 0, the
    // object 0 = the record owner); a sold child leaves the pad owner as it was.
    bool pad_owner_kept = false, sold_owner_kept = false;
    size_t after = 99, expect_after = 0;
    bool filtered_ok = false;
    CrcLog crc;
};

PersistRun run_persist(const Data& d, int32_t workers, bool skirmish) {
    PersistRun out;
    jobs::JobSystem js(jobs::JobSystemConfig{workers});
    SimConfig cfg;
    cfg.seed = 11;
    cfg.players = 4;
    SimHost host(d.types, js, cfg);
    galactic::GalacticSetup s = make_setup(d);
    const uint32_t sq = type_of(d, "S10_Squad"), fr = type_of(d, "S10_Frigate");
    s.forces = {{0, 1, sq}, {1, 2, fr}, {1, 2, sq}};
    galactic::GalacticSim* gs = host.start_galactic(d.rules, std::move(s));
    CHECK(gs != nullptr);
    if (!gs) return out;
    const uint32_t turret = type_of(d, "L1_Turret");
    std::vector<galactic::PersistedBuiltObject>* recs = gs->built_objects(1);
    CHECK(recs != nullptr);
    if (!recs) return out;
    recs->push_back({turret, -1500.0f, -1500.0f, 0.1f, 0, 0});  // R1: on the map pad
    recs->push_back({turret, 777.0f, 888.0f, 0.1f, 3, 1});      // R2: no pad there
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 900;
    opt.battle.order_interval = 0;
    opt.battle.skirmish = skirmish;
    load_tactical_systems(*d.files, d.rules, &d.db.constants(), opt);
    const uint32_t empire_faction = d.rules.find_faction("Empire");
    CampaignBattles battles(host, d.crules, *d.catalog,
                            [empire_faction](const std::string& name, terrain::MapData& m) {
                                if (name != "L1_Land_B") return false;
                                m = land_map();
                                terrain::ObjectPlacement p;
                                p.type_crc = name_crc("L1_Build_Pad");
                                p.owner = empire_faction;  // the faction index -> player 0
                                p.position[0] = -1500.0f;
                                p.position[1] = -1500.0f;
                                m.objects.push_back(p);
                                return true;
                            },
                            opt);
    CHECK(battles.attach());
    std::vector<objects::ObjectHandle> up;
    for (objects::ObjectHandle h : gs->planets()[2].surface)
        if (gs->store().owner(h) == 1) up.push_back(h);
    CHECK(gs->launch_units(1, 2, up) == commands::RejectReason::None);
    const std::vector<uint32_t> uf = gs->fleets_at(2, 1);
    if (!uf.empty()) CHECK(gs->move_fleet(1, uf[0], 1) == commands::RejectReason::None);
    bool invaded = false;
    objects::ObjectId map_pad = objects::kNoObjectId, pad2 = objects::kNoObjectId;
    const auto uc_t = static_cast<objects::TypeIndex>(type_of(d, skirmish ? "L1_UC_Turret_MP" : "L1_UC_Turret"));
    for (uint64_t t = 0; t < 4000; ++t) {
        host.tick_once();
        if (!invaded && host.modes().depth() == 1) {
            const std::vector<uint32_t> f = gs->fleets_at(1, 1);
            if (!f.empty() && !gs->fleets()[f[0]].moving) invaded = gs->invade(1, 1, {}) == commands::RejectReason::None;
        }
        TacticalBattle* b = battles.battle();
        TacticalMode* tm = host.tactical();
        if (!b || !tm) {
            if (out.fought) break;
            continue;
        }
        out.fought = true;
        objects::ObjectStore& st = tm->primary();
        const uint32_t f = b->frame();
        if (f == 1) {
            for (const objects::ObjectHandle h : st.all_objects())
                if (st.active(h) && st.type(h) == static_cast<objects::TypeIndex>(type_of(d, "L1_Build_Pad"))) map_pad = st.id(h);
            const objects::ObjectId c = b->pad_contents(map_pad);
            const objects::ObjectHandle ch = st.find(c);
            out.restored = c != objects::kNoObjectId && st.alive(ch);
            out.restored_ok = out.restored && st.type(ch) == static_cast<objects::TypeIndex>(turret) && st.owner(ch) == 0 &&
                              st.position(ch).x == -1500.0f && st.position(ch).y == -1500.0f && recs->size() == 2;
            out.pad_owner_kept = st.alive(st.find(map_pad)) && st.owner(st.find(map_pad)) == 0;
            objects::SpawnParams sp;
            sp.type = static_cast<objects::TypeIndex>(type_of(d, "L1_Build_Pad"));
            sp.owner = 1;
            sp.position = {-1400.0f, -1600.0f, 0.0f};
            pad2 = st.id(tm->spawn(sp));
            CHECK(b->pad_build(*tm, pad2, uc_t, true) == 1);
        }
        const objects::ObjectId c2 = b->pad_contents(pad2);
        if (!out.built && c2 != objects::kNoObjectId && st.alive(st.find(c2)) && st.type(st.find(c2)) == static_cast<objects::TypeIndex>(turret)) {
            out.built = true;
            const Vec3 p = st.position(st.find(c2));
            const size_t want = skirmish ? 2u : 3u;
            out.added_ok = recs->size() == want;
            if (!skirmish && recs->size() == 3) {
                const galactic::PersistedBuiltObject& r = recs->back();
                out.added_ok = r.type == turret && r.x == p.x && r.y == p.y && r.z == p.z && r.base_level == 0 && r.owner == 1;
            }
            // Selling R1's turret removes R1 (the record at its x, y with its type).
            const objects::ObjectId c1 = b->pad_contents(map_pad);
            if (!skirmish && c1 != objects::kNoObjectId) {
                out.sold = b->tactical_sell(*tm, 0, c1) == commands::RejectReason::None;
                out.sold_owner_kept = st.alive(st.find(map_pad)) && st.owner(st.find(map_pad)) == 0;
                out.sold_ok = recs->size() == 2 && (*recs)[0].x == 777.0f && (*recs)[1].owner == 1;
            }
        }
    }
    const std::vector<galactic::ConflictLogEntry>& log = battles.conflicts().log();
    const int32_t w = log.empty() ? -2 : log.back().winner;
    out.after = recs->size();
    out.filtered_ok = !log.empty() && std::all_of(recs->begin(), recs->end(), [w](const galactic::PersistedBuiltObject& r) { return r.owner == w; });
    out.expect_after = skirmish ? (w == 0 ? 1u : w == 1 ? 1u : 0u) : (w == 1 ? 2u : 0u);
    std::printf("  persist%s: winner %d, %zu record(s) after the battle\n", skirmish ? " (skirmish)" : "", w, out.after);
    out.crc = host.crc_log();
    return out;
}

void test_persist(const Data& d) {
    const PersistRun a = run_persist(d, 0, false);
    std::printf("  persist: fought %d restored %d (ok %d) built %d added %d sold %d (ok %d) filtered %d\n", a.fought, a.restored, a.restored_ok,
                a.built, a.added_ok, a.sold, a.sold_ok, a.filtered_ok);
    CHECK(a.fought && a.restored && a.restored_ok && a.built && a.added_ok && a.sold && a.sold_ok);
    std::printf("  persist q6 t19: pad owner kept at the recreate %d, after the sale %d\n", a.pad_owner_kept, a.sold_owner_kept);
    CHECK(a.pad_owner_kept && a.sold_owner_kept);
    CHECK(a.filtered_ok && a.after == a.expect_after);
    const PersistRun w = run_persist(d, 4, false);
    CHECK(!first_divergence(a.crc, w.crc).diverged);
    // A land skirmish: no recreate, no add (the seeded records only meet the winner filter of the conflict).
    const PersistRun k = run_persist(d, 0, true);
    std::printf("  persist (land skirmish): fought %d restored %d built %d added %d\n", k.fought, k.restored, k.built, k.added_ok);
    CHECK(k.fought && !k.restored && k.built && k.added_ok && k.filtered_ok && k.after == k.expect_after);
}

using ::alamo::test::env_or_empty;

// ---- s78f t05: command 25 SpecialWeaponFire (TACTICAL_COMBAT §3.1.1 "Fire-at-position order", LOB_SOLVER §9) ----
// The Empire defends S10_B with a lobbing superweapon (LOBBING_SUPERWEAPON + TURRET + WEAPON, a grenade projectile,
// 1 s recharge, 10-frame charge-up). Three markers with DUMMY_DESTROY_AFTER_SPECIAL_WEAPON_FIRED (0x67): two of the
// Empire, one of the Underworld. TacticalBattle::handle_command with SpecialWeaponFire:
//   (a) before the pack is ready: AbilityNotReady, no marker dies;
//   (b) the Underworld naming the Empire's weapon: InvalidUnit;
//   (b2) ready, but a Requires_Base_Power variant without base power: Fire_Lobbing_At_Position refuses -> NotAllowed and
//       no sweep (the sweep follows an accepted order only);
//   (c) ready, at_position 1: None; both Empire markers die silently (no battle death, no new object such as a death
//       clone), the Underworld's marker stays; the order (point = the command position) is pending in the combat unit;
//   (d) the WeaponBehavior charges up (LobChargeup, then LobCharged after the charge-up frames, both at the point) and
//       fires one lob shot, which clears the order; ready again, at_position 0 with a live target: the order's point is
//       the target's position.
const char* kSwFireTypes = R"(
 <Projectile Name="L1_Lob_Proj"><Max_Speed>20.0</Max_Speed><Projectile_Grenade_Gravity>1.0</Projectile_Grenade_Gravity>
  <Projectile_Grenade_Gravity_Lob_Mod>1.0</Projectile_Grenade_Gravity_Lob_Mod><Projectile_Category>GRENADE</Projectile_Category>
  <Projectile_Damage>0.0</Projectile_Damage><Projectile_Blast_Area_Damage>10</Projectile_Blast_Area_Damage>
  <Projectile_Blast_Area_Range>30</Projectile_Blast_Area_Range><Projectile_Max_Lifetime>30.0</Projectile_Max_Lifetime></Projectile>
 <GroundVehicle Name="L1_Lobber"><Victory_Relevant>Yes</Victory_Relevant><Tactical_Health>5000</Tactical_Health>
  <CategoryMask>Vehicle</CategoryMask><Population_Value>1</Population_Value><Targeting_Max_Attack_Distance>400</Targeting_Max_Attack_Distance>
  <Behavior>SELECTABLE</Behavior><LandBehavior>LOBBING_SUPERWEAPON, TURRET, WEAPON</LandBehavior><Collidable_By_Projectile_Living>Yes</Collidable_By_Projectile_Living>
  <Projectile_Types>L1_Lob_Proj</Projectile_Types><Projectile_Fire_Recharge_Seconds>1.0</Projectile_Fire_Recharge_Seconds>
  <Lobbing_Superweapon_Chargeup_Frames>10</Lobbing_Superweapon_Chargeup_Frames><Custom_Soft_Footprint_Radius>10</Custom_Soft_Footprint_Radius>
 </GroundVehicle>
 <GroundVehicle Name="L1_Lobber_Powered"><Variant_Of_Existing_Type>L1_Lobber</Variant_Of_Existing_Type><Requires_Base_Power>Yes</Requires_Base_Power></GroundVehicle>
 <MiscObject Name="L1_SW_Marker"><Behavior>DUMMY_DESTROY_AFTER_SPECIAL_WEAPON_FIRED</Behavior><Is_Decoration>No</Is_Decoration></MiscObject>
)";

struct SwFireRun {
    bool fought = false, not_ready = false, markers_kept = false, foreign_refused = false, unpowered_refused = false;
    bool fired = false, own_markers_gone = false, other_marker_kept = false, silent = false, pending_at_point = false;
    bool restarted = false, chargeup = false, charged = false, shot = false, order_cleared = false;
    bool target_fired = false, target_point = false;
    uint32_t fire_frame = 0, chargeup_frame = 0, charged_frame = 0, shot_frame = 0;
};

SwFireRun run_sw_fire(const Data& d) {
    using R = commands::RejectReason;
    using K = tactical::abilities::SwEventKind;
    SwFireRun out;
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    SimConfig cfg;
    cfg.seed = 25;
    cfg.players = 4;
    SimHost host(d.types, js, cfg);
    galactic::GalacticSetup s = make_setup(d);
    const uint32_t sq = type_of(d, "S10_Squad"), fr = type_of(d, "S10_Frigate"), lob = type_of(d, "L1_Lobber");
    const uint32_t lobp = type_of(d, "L1_Lobber_Powered");
    s.forces = {{0, 1, sq}, {0, 1, lob}, {0, 1, lobp}, {1, 2, fr}, {1, 2, sq}};
    galactic::GalacticSim* gs = host.start_galactic(d.rules, std::move(s));
    CHECK(gs != nullptr);
    if (!gs) return out;
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 3000;
    opt.battle.order_interval = 0;
    opt.battle.fog = false;
    CampaignBattles battles(host, d.crules, *d.catalog,
                            [](const std::string& name, terrain::MapData& m) {
                                if (name != "L1_Land_B") return false;
                                m = land_map();
                                return true;
                            },
                            opt);
    CHECK(battles.attach());
    std::vector<objects::ObjectHandle> up;
    for (objects::ObjectHandle h : gs->planets()[2].surface)
        if (gs->store().owner(h) == 1) up.push_back(h);
    CHECK(gs->launch_units(1, 2, up) == R::None);
    const std::vector<uint32_t> uf = gs->fleets_at(2, 1);
    CHECK(!uf.empty());
    if (!uf.empty()) CHECK(gs->move_fleet(1, uf[0], 1) == R::None);
    bool invaded = false;
    objects::ObjectId weapon = objects::kNoObjectId, powered = objects::kNoObjectId, enemy = objects::kNoObjectId;
    objects::ObjectId markers[3] = {objects::kNoObjectId, objects::kNoObjectId, objects::kNoObjectId};  // owners 0, 0, 1
    Vec3 point{};
    auto fire_event = [](objects::ObjectId w, bool at_position, Vec3 p, objects::ObjectId target) {
        commands::EventData ev(commands::EventType::SpecialWeaponFire);
        ev.set("weapon", static_cast<uint32_t>(w)).set("player", 0);
        if (at_position) ev.set("at_position", 1).set_f("x", p.x).set_f("y", p.y).set_f("z", p.z);
        else ev.set("at_position", 0).set("target", static_cast<uint32_t>(target));
        return ev;
    };
    bool done = false;
    for (uint64_t t = 0; t < 4000 && !done; ++t) {
        host.tick_once();
        if (!invaded && host.modes().depth() == 1) {
            const std::vector<uint32_t> f = gs->fleets_at(1, 1);
            if (!f.empty() && !gs->fleets()[f[0]].moving) invaded = gs->invade(1, 1, {}) == R::None;
        }
        TacticalBattle* b = battles.battle();
        TacticalMode* tm = host.tactical();
        if (!b || !tm) {
            if (out.fought) break;
            continue;
        }
        objects::ObjectStore& st = tm->primary();
        auto alive = [&st](objects::ObjectId id) { return id != objects::kNoObjectId && st.alive(st.find(id)); };
        const uint32_t f = b->frame();
        if (!out.fought) {
            out.fought = true;
            for (const objects::ObjectHandle h : st.owned_by(0))
                if (st.alive(h)) {
                    if (static_cast<uint32_t>(st.type(h)) == lob) weapon = st.id(h);
                    if (static_cast<uint32_t>(st.type(h)) == lobp) powered = st.id(h);
                }
            for (const objects::ObjectHandle h : st.owned_by(1))
                if (enemy == objects::kNoObjectId && st.alive(h) && b->combat().has_unit(st.id(h)) && b->combat().alive(st.id(h))) enemy = st.id(h);
            CHECK(weapon != objects::kNoObjectId && powered != objects::kNoObjectId && enemy != objects::kNoObjectId);
            CHECK(b->combat().fires_at_position(weapon) && b->combat().fires_at_position(powered));
            if (weapon == objects::kNoObjectId || powered == objects::kNoObjectId || enemy == objects::kNoObjectId) break;
            const Vec3 wp = st.position(st.find(weapon));
            point = Vec3{wp.x + 120.0f, wp.y + 40.0f, wp.z};
            for (int i = 0; i < 3; ++i) {
                objects::SpawnParams sp;
                sp.type = static_cast<objects::TypeIndex>(type_of(d, "L1_SW_Marker"));
                sp.owner = i < 2 ? 0 : 1;
                sp.position = {wp.x - 50.0f, wp.y + 30.0f * static_cast<float>(i), wp.z};
                markers[i] = st.id(tm->spawn(sp));
            }
            // (a) not ready (the pack restarted at registration, 1 s recharge); (b) not the issuing player's weapon.
            const commands::EventData ev = fire_event(weapon, true, point, objects::kNoObjectId);
            out.not_ready = b->sw_readiness(weapon) >= 0.0f && b->sw_readiness(weapon) < 0.99f &&
                            b->handle_command(*tm, 0, ev, host.random().synced()) == R::AbilityNotReady;
            out.foreign_refused = b->handle_command(*tm, 1, ev, host.random().synced()) == R::InvalidUnit;
            out.markers_kept = alive(markers[0]) && alive(markers[1]) && alive(markers[2]) &&
                               b->combat().fire_at_position_state(weapon, nullptr) == 0;
            continue;
        }
        if (!alive(weapon)) break;
        // (c) the first ready frame: an at-position order and the destroy-after sweep.
        if (!out.fired && b->sw_readiness(weapon) >= 0.99f) {
            auto live_objects = [&st] {
                size_t n = 0;
                for (const objects::ObjectHandle h : st.all_objects()) n += st.alive(h) ? 1u : 0u;
                return n;
            };
            const uint32_t deaths = b->stats().deaths;
            const size_t before = live_objects();
            // (b2) refused by the weapon's own behaviour (no base power): no sweep.
            out.unpowered_refused = b->sw_readiness(powered) >= 0.99f &&
                                    b->handle_command(*tm, 0, fire_event(powered, true, point, objects::kNoObjectId), host.random().synced()) ==
                                        R::NotAllowed &&
                                    alive(markers[0]) && alive(markers[1]) && alive(markers[2]) && live_objects() == before &&
                                    b->combat().fire_at_position_state(powered, nullptr) == 0;
            const commands::EventData ev = fire_event(weapon, true, point, objects::kNoObjectId);
            point = Vec3{ev.f("x"), ev.f("y"), ev.f("z")};  // the quantized command position
            out.fired = b->handle_command(*tm, 0, ev, host.random().synced()) == R::None;
            out.fire_frame = f;
            out.own_markers_gone = !alive(markers[0]) && !alive(markers[1]);
            out.other_marker_kept = alive(markers[2]);
            out.silent = b->stats().deaths == deaths && live_objects() + 2 == before;  // no death, no death clone
            tactical::Vec3 at{};
            out.pending_at_point = b->combat().fire_at_position_state(weapon, &at) == 1 && at.x == point.x && at.y == point.y &&
                                   at.z == point.z;
            out.restarted = b->sw_readiness(weapon) < 0.99f;
            continue;
        }
        if (!out.fired) continue;
        // (d) the charge-up and the shot at the point.
        for (const tactical::abilities::SpecialWeaponEvent& e : static_cast<const TacticalBattle*>(b)->special_weapon_events()) {
            if (e.id != weapon) continue;
            const bool at_point = e.point.x == point.x && e.point.y == point.y && e.point.z == point.z;
            if (e.kind == K::LobChargeup && !out.chargeup) {
                out.chargeup = at_point;
                out.chargeup_frame = f;
            }
            if (e.kind == K::LobCharged && !out.charged) {
                out.charged = at_point;
                out.charged_frame = f;
            }
        }
        if (!out.shot)
            for (size_t i = 0; i < b->combat().projectile_count(); ++i) {
                const tactical::combat::ProjectileView v = b->combat().projectile(i);
                if (v.shooter != weapon || !v.alive) continue;
                out.shot = v.lob;
                out.shot_frame = f;
                out.order_cleared = b->combat().fire_at_position_state(weapon, nullptr) == 0;
            }
        // Ready again: a target order (at_position 0) aims at the target's bare position.
        if (out.shot && b->sw_readiness(weapon) >= 0.99f) {
            if (!alive(enemy)) {
                for (const objects::ObjectHandle h : st.owned_by(1))
                    if (st.alive(h) && b->combat().has_unit(st.id(h)) && b->combat().alive(st.id(h))) enemy = st.id(h);
            }
            CHECK(alive(enemy));
            if (!alive(enemy)) break;
            const Vec3 tp = st.position(st.find(enemy));
            out.target_fired = b->handle_command(*tm, 0, fire_event(weapon, false, {}, enemy), host.random().synced()) == R::None;
            tactical::Vec3 at{};
            out.target_point = b->combat().fire_at_position_state(weapon, &at) == 1 && at.x == tp.x && at.y == tp.y && at.z == tp.z;
            done = true;
        }
    }
    return out;
}

void test_sw_fire() {
    const auto d = make_data("alamo_sim_l1_swfire", kSwFireTypes);
    const SwFireRun r = run_sw_fire(*d);
    std::printf("  special weapon fire (cmd 25): fought %d not ready %d foreign refused %d markers kept %d unpowered refused %d; fired %d "
                "at %u: own markers gone %d other kept %d silent %d pending %d restarted %d; chargeup %d at %u charged %d at %u shot %d "
                "at %u cleared %d; target order %d at the target %d\n",
                r.fought, r.not_ready, r.foreign_refused, r.markers_kept, r.unpowered_refused, r.fired, r.fire_frame, r.own_markers_gone,
                r.other_marker_kept, r.silent, r.pending_at_point, r.restarted, r.chargeup, r.chargeup_frame, r.charged, r.charged_frame,
                r.shot, r.shot_frame, r.order_cleared, r.target_fired, r.target_point);
    CHECK(r.fought && r.not_ready && r.foreign_refused && r.markers_kept && r.unpowered_refused);
    CHECK(r.fired && r.own_markers_gone && r.other_marker_kept && r.silent && r.pending_at_point && r.restarted);
    CHECK(r.chargeup && r.charged && r.charged_frame == r.chargeup_frame + 10 && r.shot && r.shot_frame >= r.charged_frame && r.order_cleared);
    CHECK(r.target_fired && r.target_point);
}

void test_foc() {
    const std::string root = env_or_empty("ALAMO_FOC_DATA");
    if (root.empty()) {
        std::printf("foc: ALAMO_FOC_DATA not set, skipped\n");
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
    std::vector<std::string> diag;
    CHECK(db.load(*files, {}, &diag) && raw.load(*files) && types.build(raw, files.get(), &diag));
    CHECK(galactic::load_galactic_rules(*files, db, types, rules, &diag));
    const std::vector<int32_t> inc = load_retreat_increments(*files, rules);
    int32_t ones = 0;
    for (int32_t v : inc) ones += v == 1 ? 1 : 0;
    const uint32_t rebel = rules.find_faction("Rebel"), empire = rules.find_faction("Empire");
    std::printf("foc: %zu factions, %d with Space_Retreat_Unit_Increment_Wait_Frames 1 (Rebel %d, Empire %d)\n", inc.size(), ones,
                rebel < inc.size() ? inc[rebel] : -1, empire < inc.size() ? inc[empire] : -1);
    CHECK(rebel < inc.size() && inc[rebel] == 1 && empire < inc.size() && inc[empire] == 1);
    // The space special weapons of the shipped data (Spawn_Special_Weapon set) and the TSW.
    const objects::BehaviorTypeId sw = objects::behavior_type_id("SPECIAL_WEAPON"), lob = objects::behavior_type_id("LOBBING_SUPERWEAPON");
    uint32_t space_sw = 0, land_lob = 0, tsw = 0;
    for (uint32_t t = 0; t < types.size(); ++t) {
        if (types.has_behavior(t, objects::BehaviorSet::Space, sw) && types.get_bool(t, "Is_Special_Weapon_In_Space") &&
            types.get_int(t, "Special_Weapon_Index") >= 0)
            ++space_sw;
        if (types.has_behavior(t, objects::BehaviorSet::Land, lob)) ++land_lob;
        if (types.has_behavior(t, objects::BehaviorSet::Space, objects::beh::TacticalSuperWeapon)) ++tsw;
    }
    const objects::TypeIndex ion = types.resolve_name("Ground_Ion_Cannon"), ds = types.resolve_name("Death_Star");
    std::printf("foc: %u space special weapons, %u land lobbing superweapons, %u TSW types; Ground_Ion_Cannon recharge %.1f s x %d "
                "pulses, Death Star attack distance %.0f\n",
                space_sw, land_lob, tsw, static_cast<double>(ion != objects::kNone ? types.get_float(ion, "Projectile_Fire_Recharge_Seconds") : 0.0f),
                ion != objects::kNone ? types.get_int(ion, "Projectile_Fire_Pulse_Count") : 0,
                static_cast<double>(ds != objects::kNone ? types.get_float(ds, "TSW_Attack_Distance_From_Target") : 0.0f));
    CHECK(space_sw >= 2 && land_lob >= 1 && tsw >= 2);
    CHECK(ion != objects::kNone && types.get_float(ion, "Projectile_Fire_Recharge_Seconds") == 30.0f &&
          types.get_int(ion, "Projectile_Fire_Pulse_Count") == 3);
    CHECK(ds != objects::kNone && types.get_float(ds, "TSW_Attack_Distance_From_Target") == 5800.0f);
}

}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) g_regen = g_regen || std::string(argv[i]) == "--regen";
    const auto d = make_data();
    test_battle(*d);
    test_tsw_follow(*d);
    test_tsw_lost(*d);
    test_land(*d);
    test_pads(*d);  // Int-final
    test_persist(*d);  // q6: persisted tactical built objects
    test_jet(*d);   // S7 t10 / t14
    test_deploy(*d);  // S7 t26
    test_script_spawn(*d);  // q6 t12
    test_sw_fire();   // s78f t05: command 25
    test_foc();
    if (g_fail) {
        std::printf("alamo_sim_l1_tests: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("alamo_sim_l1_tests: OK\n");
    return 0;
}
