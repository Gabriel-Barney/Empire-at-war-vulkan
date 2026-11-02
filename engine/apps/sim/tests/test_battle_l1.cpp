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
  <Minimum_Time_Before_Pad_Can_Build_Again>1.0</Minimum_Time_Before_Pad_Can_Build_Again></SpecialStructure>
 <SpecialStructure Name="L1_UC_Turret"><Victory_Relevant>Yes</Victory_Relevant><Tactical_Health>100</Tactical_Health><Tactical_Build_Time_Seconds>2</Tactical_Build_Time_Seconds>
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
 <Faction Name="Empire"><Is_Playable>Yes</Is_Playable><Can_Control_Planets>Yes</Can_Control_Planets>
  <Space_Tactical_Unit_Cap>20</Space_Tactical_Unit_Cap><Space_Retreat_Unit_Increment_Wait_Frames>3</Space_Retreat_Unit_Increment_Wait_Frames></Faction>
 <Faction Name="Underworld"><Is_Playable>Yes</Is_Playable><Can_Control_Planets>Yes</Can_Control_Planets>
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

std::unique_ptr<Data> make_data() {
    auto d = std::make_unique<Data>();
    d->root = fs::temp_directory_path() / "alamo_sim_l1_tests";
    std::error_code ec;
    remove_all_long(d->root, ec);
    const fs::path x = d->root / "Data/XML";
    write_file(x / "GameObjectFiles.xml", "<GameObjectFiles><File>L1Types.xml</File></GameObjectFiles>\n");
    write_file(x / "L1Types.xml", kTypes);
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
        // Held on every frame from the fire to the one before the release (state 13 serviced at destroy + 10 + 40 + 1);
        // the battle ends in the release tick (the TswDone event and the end are in the same tick).
        CHECK(a.held_from_fire && !a.finished_while_held && a.first_unheld == 0);
        CHECK(a.end_frame == a.destroy_frame + 10 + 40 + 1 && a.last_held == a.end_frame - 1);
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
// die (the third one retreated at frame 2+5+90); with only the Death Star left the battle ends at once: the Empire
// (defender) wins all-destroyed 90 frames after the Hyperspace_Away, the Death Star alive and undamaged. 0 / 4 workers.
void test_tsw_lost(const Data& d) {
    const Run a = run(d, 0, 3);
    std::printf("  TSW lost: destroy %u first unheld %u kill %u (escorts %zu) end %u; Death Star alive %d health %.1f (start %.1f); %s\n",
                a.destroy_frame, a.first_unheld, a.kill_frame, a.escorts_killed, a.end_frame, a.ds_alive, static_cast<double>(a.ds_health),
                static_cast<double>(a.ds_health0), a.summary.c_str());
    CHECK(a.tsw_fired && a.planet_destroyed && a.ended);
    CHECK(a.first_unheld == a.destroy_frame + 10 + 40 + 1 && a.kill_frame == a.first_unheld && a.escorts_killed == 2);
    CHECK(a.end_frame == a.kill_frame + 90);
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
    uint32_t shots_flying = 0, shots_after = 0, suspended_frames = 0;
    uint32_t ability_finished = 0, finished_at_clear = 0;  // the jet's signals; those in the tick its flag cleared
    CrcLog crc;
};

JetRun run_jet(const Data& d, int32_t workers) {
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
            }
        was_suspended = suspended;
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

void test_jet(const Data& d) {
    const JetRun a = run_jet(d, 0);
    std::printf("  jetpack: fought %d ordered %d flew %d landed %d mirror %d suspended %u frames, shots in flight %u, after %u\n",
                a.fought, a.ordered, a.flew, a.landed, a.mirror, a.suspended_frames, a.shots_flying, a.shots_after);
    CHECK(a.fought && a.ordered && a.flew && a.landed && a.mirror && a.other_walks);
    CHECK(a.suspended_frames > 30 && a.shots_flying == 0 && a.shots_after > 0);
    CHECK(a.ability_finished == 1 && a.finished_at_clear == 1);
    const JetRun w = run_jet(d, 4);
    CHECK(!first_divergence(a.crc, w.crc).diverged && w.suspended_frames == a.suspended_frames && w.ability_finished == 1);
}

// ---- Int-final: build pads (L1_S4_REMAINING §3.6) and the weather factor of the fog sight (FOG_OF_WAR §3.1.1) ----
struct PadRun {
    bool fought = false, refused_rules = false, refused_full = false, started = false, uc_seen = false, built = false, cleared = false;
    bool cooldown_refused = false, rebuilt = false, locked = false;
    float sight_clear = -1.0f, sight_rain = -1.0f;
    bool fog_on = false;
    CrcLog crc;
};

PadRun run_pads(const Data& d, int32_t workers) {
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
    const auto uc_t = static_cast<objects::TypeIndex>(type_of(d, "L1_UC_Turret"));
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
            out.started = b->pad_build(*tm, pad, uc_t, true) == 1;
            out.refused_full = b->pad_build(*tm, pad, uc_t, false) == -1;  // already has something built on it
            b->pad_lock(pad, true);
            out.locked = b->pad_locked(pad);
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
            out.cooldown_refused = b->pad_build(*tm, pad, uc_t, false) == -1;
        }
        if (built_frame && f == built_frame + 40) out.rebuilt = b->pad_build(*tm, pad, uc_t, false) == 1;
#ifdef ALAMO_SIM_HAVE_FOG
        tactical::fog::FogObject fo;
        if (scout != objects::kNoObjectId && b->fog() && b->fog_object(scout, fo)) {
            out.fog_on = true;
            const bool rain = b->weather().type() == tactical::combat::WeatherType::Rain && b->weather().intensity() > 0.5f;
            if (rain) out.sight_rain = fo.sight_range;
            else if (b->weather().intensity() <= 0.5f) out.sight_clear = fo.sight_range;
        }
#endif
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
#ifdef ALAMO_SIM_HAVE_FOG
    CHECK(a.fog_on && a.sight_clear == 300.0f && a.sight_rain == 150.0f);  // Land_FOW_Reveal_Range x Sight_Range_Modifier 0.5
#endif
    const PadRun w = run_pads(d, 4);
    CHECK(!first_divergence(a.crc, w.crc).diverged);
}
// --- end Int-final ---

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
    test_jet(*d);   // S7 t10 / t14
    test_foc();
    if (g_fail) {
        std::printf("alamo_sim_l1_tests: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("alamo_sim_l1_tests: OK\n");
    return 0;
}
