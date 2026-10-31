// alamo_game_session_tests (P host, no game data needed): a synthetic skirmish through GameSession -
//   * the human fleet's opening hop (a recorded command) starts the space conflict, the battle is pushed;
//   * human Select + Attack / MoveToPosition / Stop commands reach the battle's CombatSystem / MovementSystem through
//     the SimHost order hook (routed counters, ordered target, movement goal);
//   * the battle ends (time limit) and the galaxy is back on top;
//   * determinism / replay: the recorded command stream replayed by a fresh session (0 and 4 workers) gives the
//     identical per-tick CRC log - the contract the alamo_game presentation-on/off check relies on.
//   * P4 host: the auto-resolve dialog's Withdraw accepted while the auto-resolve runs, the battle's Autoresolve ending
//     the battle, and a black-market purchase / sabotage / tech slice / super-weapon hack through the SpecialAbility
//     dialogs.
//   * OI host: the cinematic rate lock (pacing 30 / parity not inverted on exactly the locked ticks, ordered CRCs
//     untouched), fast forward, the attract demo session (AI vs AI, no human, replayable) and its controller, and the
//     camera / controls decisions on the synced hotkey / binding tables.
//   * L1/S4 wave 1: StoryUiFeed's transition rules (story_ui.hpp) and the story UI port event round trip.
#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "alamo/data/xml.hpp"
#include "alamo/game/attract_mode.hpp"
#include "alamo/game/game_controls.hpp"
#include "alamo/game/hud_feeds.hpp"
#include "alamo/game/radar_feed.hpp"
#include "alamo/game/game_session.hpp"
#include "alamo/game/save_games.hpp"
#include "alamo/game/story_script.hpp"
#include "alamo/game/story_ui.hpp"
#include "alamo/jobs/crc_recorder.hpp"
#include "alamo/sim/save_session.hpp"
#include "alamo/sim/tactical_battle.hpp"
#include "alamo/vfs/vfs.hpp"

using namespace alamo;
namespace fs = std::filesystem;

static int g_fail = 0;
#define CHECK(c)                                                     \
    do {                                                             \
        if (!(c)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++g_fail;                                                \
        }                                                            \
    } while (0)

namespace {

const char* kTypes = R"(<GS>
 <Planet Name="GS_Planet"><Behavior>PLANET</Behavior><Terrain>Temperate</Terrain><Planet_Credit_Value>10</Planet_Credit_Value></Planet>
 <Projectile Name="GS_Laser_Proj"><Max_Speed>40</Max_Speed><Projectile_Damage>60</Projectile_Damage>
  <Projectile_Max_Flight_Distance>2500</Projectile_Max_Flight_Distance><Damage_Type>Damage_Laser</Damage_Type></Projectile>
 <SpaceUnit Name="GS_Frigate"><Victory_Relevant>Yes</Victory_Relevant>
  <Tactical_Health>900</Tactical_Health><Max_Speed>3.0</Max_Speed><Max_Rate_Of_Turn>2.0</Max_Rate_Of_Turn>
  <Hyperspace>Yes</Hyperspace><Hyperspace_Speed>1.5</Hyperspace_Speed><AI_Combat_Power>1000</AI_Combat_Power>
  <CategoryMask>Frigate</CategoryMask><Targeting_Max_Attack_Distance>1400</Targeting_Max_Attack_Distance>
  <Population_Value>2</Population_Value><Armor_Type>Armor_Ship</Armor_Type>
  <HardPoints>HP_GS_Laser</HardPoints>
  <Behavior>SELECTABLE</Behavior><Collidable_By_Projectile_Living>Yes</Collidable_By_Projectile_Living><SpaceBehavior>STARSHIP_LOCOMOTOR, REVEAL</SpaceBehavior>
  <Space_FOW_Reveal_Range>1500</Space_FOW_Reveal_Range>
  <Custom_Soft_Footprint_Radius>60</Custom_Soft_Footprint_Radius>
 </SpaceUnit>
 <SpaceUnit Name="GS_Corvette"><Variant_Of_Existing_Type>GS_Frigate</Variant_Of_Existing_Type>
  <Tactical_Health>400</Tactical_Health><Max_Speed>5.0</Max_Speed><AI_Combat_Power>400</AI_Combat_Power>
  <CategoryMask>Corvette</CategoryMask><Population_Value>1</Population_Value>
  <Custom_Soft_Footprint_Radius>30</Custom_Soft_Footprint_Radius></SpaceUnit>
 <GroundInfantry Name="GS_Trooper"><Victory_Relevant>Yes</Victory_Relevant><Tactical_Health>100</Tactical_Health><AI_Combat_Power>50</AI_Combat_Power>
  <Population_Value>1</Population_Value></GroundInfantry>
 <SpaceUnit Name="GS_Smuggler"><Variant_Of_Existing_Type>GS_Corvette</Variant_Of_Existing_Type><Is_Named_Hero>Yes</Is_Named_Hero>
  <Abilities SubObjectList="Yes"><Black_Market_Ability Name="GS_Smuggler_Black_Market">
   <Min_Respawn_Times>30, 30, 30, 30, 30</Min_Respawn_Times><Max_Respawn_Times>40, 40, 40, 40, 40</Max_Respawn_Times>
   <Price_Modifier>1.0</Price_Modifier></Black_Market_Ability></Abilities></SpaceUnit>
 <SpaceUnit Name="GS_Saboteur"><Variant_Of_Existing_Type>GS_Corvette</Variant_Of_Existing_Type><Is_Generic_Hero>Yes</Is_Generic_Hero>
  <Abilities SubObjectList="Yes"><Galactic_Sabotage_Ability Name="GS_Saboteur_Sabotage">
   <Can_Destroy_Special_Structure>Yes</Can_Destroy_Special_Structure><Sabotage_Cost_Increase_Factor>2.0</Sabotage_Cost_Increase_Factor>
  </Galactic_Sabotage_Ability></Abilities></SpaceUnit>
 <SpaceUnit Name="GS_Slicer"><Variant_Of_Existing_Type>GS_Corvette</Variant_Of_Existing_Type><Is_Named_Hero>Yes</Is_Named_Hero>
  <Abilities SubObjectList="Yes"><Hack_Super_Weapon_Ability Name="GS_Slicer_Hack"><Causes_Despawn>No</Causes_Despawn>
  </Hack_Super_Weapon_Ability></Abilities></SpaceUnit>
 <SpaceUnit Name="GS_R2"><Variant_Of_Existing_Type>GS_Corvette</Variant_Of_Existing_Type><Is_Named_Hero>Yes</Is_Named_Hero>
  <Abilities SubObjectList="Yes"><Slicer_Ability Name="GS_R2_Slicer">
   <Min_Respawn_Time_Per_Tech_Level>20, 20, 20, 20, 20</Min_Respawn_Time_Per_Tech_Level>
   <Max_Respawn_Time_Per_Tech_Level>30, 30, 30, 30, 30</Max_Respawn_Time_Per_Tech_Level></Slicer_Ability></Abilities></SpaceUnit>
 <SpaceUnit Name="GS_Stolen"><Variant_Of_Existing_Type>GS_Corvette</Variant_Of_Existing_Type><Affiliation>Rebel</Affiliation>
  <Tech_Level>1</Tech_Level><Build_Initially_Locked>Yes</Build_Initially_Locked>
  <Build_Can_Be_Unlocked_By_Slicer>Yes</Build_Can_Be_Unlocked_By_Slicer><Slice_Cost_Credits>1500</Slice_Cost_Credits></SpaceUnit>
 <SpaceUnit Name="GS_DeathStar"><Variant_Of_Existing_Type>GS_Frigate</Variant_Of_Existing_Type></SpaceUnit>
 <GroundStructure Name="GS_Cantina"><Victory_Relevant>Yes</Victory_Relevant><Tactical_Health>500</Tactical_Health><Sabotage_Cost_Credits>800</Sabotage_Cost_Credits></GroundStructure>
</GS>
)";
// P4 host: one Empire item (offered at Empire planets) and one Rebel item.
const char* kBlackMarket = R"(<BlackMarketItems>
 <Item Name="GS_Item_Empire"><Faction>Empire</Faction><Tech_Level>1</Tech_Level><Base_Cost_Credits>1000</Base_Cost_Credits>
  <Uncommon_Price_Multiplier>5.5</Uncommon_Price_Multiplier><Ability_Names>GS_Ability_X</Ability_Names></Item>
 <Item Name="GS_Item_Rebel"><Faction>Rebel</Faction><Tech_Level>2</Tech_Level><Base_Cost_Credits>2000</Base_Cost_Credits></Item>
</BlackMarketItems>
)";
const char* kHardpoints = R"(<HardPoints>
 <HardPoint Name="HP_GS_Laser"><Type>HARD_POINT_WEAPON_LASER</Type><Health>150</Health>
  <Is_Targetable>Yes</Is_Targetable><Is_Destroyable>Yes</Is_Destroyable>
  <Fire_Projectile_Type>GS_Laser_Proj</Fire_Projectile_Type><Fire_Min_Recharge_Seconds>2.0</Fire_Min_Recharge_Seconds>
  <Fire_Max_Recharge_Seconds>3.0</Fire_Max_Recharge_Seconds><Fire_Pulse_Count>2</Fire_Pulse_Count>
  <Fire_Pulse_Delay_Seconds>0.2</Fire_Pulse_Delay_Seconds><Fire_Range_Distance>1400</Fire_Range_Distance></HardPoint>
</HardPoints>
)";
const char* kFactions = R"(<Factions>
 <Faction Name="Empire"><Is_Playable>Yes</Is_Playable><Can_Control_Planets>Yes</Can_Control_Planets>
  <Space_Tactical_Unit_Cap>20</Space_Tactical_Unit_Cap><Space_Retreat_Countdown_Seconds>5</Space_Retreat_Countdown_Seconds></Faction>
 <Faction Name="Rebel"><Is_Playable>Yes</Is_Playable><Can_Control_Planets>Yes</Can_Control_Planets>
  <Space_Tactical_Unit_Cap>5</Space_Tactical_Unit_Cap><Space_Retreat_Countdown_Seconds>5</Space_Retreat_Countdown_Seconds></Faction>
 <Faction Name="Neutral"><Is_Neutral>Yes</Is_Neutral></Faction>
</Factions>
)";

void write_file(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << s;
}

std::string make_data() {
    const fs::path root = fs::temp_directory_path() / "alamo_game_session_tests";
    std::error_code ec;
    remove_all_long(root, ec);
    const fs::path x = root / "Data/XML";
    write_file(x / "GameObjectFiles.xml", "<GameObjectFiles><File>GSTypes.xml</File></GameObjectFiles>\n");
    write_file(x / "GSTypes.xml", kTypes);
    write_file(x / "HardPointDataFiles.xml", "<Hard_Point_Files><File>GSHardPoints.xml</File></Hard_Point_Files>\n");
    write_file(x / "GSHardPoints.xml", kHardpoints);
    write_file(x / "FactionFiles.xml", "<FactionFiles><File>GSFactions.xml</File></FactionFiles>\n");
    write_file(x / "GSFactions.xml", kFactions);
    write_file(x / "TradeRouteFiles.xml", "<TradeRouteFiles></TradeRouteFiles>\n");
    write_file(x / "CampaignFiles.xml", "<CampaignFiles></CampaignFiles>\n");
    write_file(x / "BlackMarketItems.xml", kBlackMarket);
    // P4 host: the hack's required type and cost; an auto-resolve shown for 2 s (the Withdraw drains while it runs).
    write_file(x / "GameConstants.xml",
               "<GameConstants>\n <Damage_Types>Damage_Default, Damage_Laser</Damage_Types>\n"
               " <Armor_Types>Armor_Default, Armor_Ship</Armor_Types>\n"
               " <Damage_To_Armor_Mod>Damage_Laser, Armor_Ship, 0.8</Damage_To_Armor_Mod>\n"
               " <Space_Retreat_Allowed_Countdown_Seconds>5.0</Space_Retreat_Allowed_Countdown_Seconds>\n"
               " <Hack_Super_Weapon_Required_Type>GS_DeathStar</Hack_Super_Weapon_Required_Type>\n"
               " <Hack_Super_Weapon_Cost>500</Hack_Super_Weapon_Cost>\n"
               " <AutoResolveDisplayTime>2.0</AutoResolveDisplayTime>\n"
               // OI host: the attract demo constants (FoC values, two maps / delays for the cycle test).
               " <Demo_Attract_Maps>Demo_Attract_Kamino.ted, GS_SPACE</Demo_Attract_Maps>\n"
               " <Demo_Attract_Start_Timeout_Seconds>3600</Demo_Attract_Start_Timeout_Seconds>\n"
               " <Demo_Attract_Map_Cycle_Delay_Seconds>1200, 600</Demo_Attract_Map_Cycle_Delay_Seconds>\n"
               " <Main_Menu_Demo_Attract_Mode> Yes </Main_Menu_Demo_Attract_Mode>\n</GameConstants>\n");
    return path_to_utf8(root);
}

std::shared_ptr<terrain::MapData> space_map() {
    auto map = std::make_shared<terrain::MapData>();
    map->space.present = true;
    for (int i = 0; i < 3; ++i) {
        map->space.playable_min[i] = -3000.0f;
        map->space.playable_max[i] = 3000.0f;
    }
    map->playable.present = map->playable.has_outer_box = true;
    for (int i = 0; i < 3; ++i) {
        map->playable.outer_min[i] = -3000.0f;
        map->playable.outer_max[i] = 3000.0f;
    }
    return map;
}

game::SessionSpec spec_for(const std::shared_ptr<terrain::MapData>& map) {
    game::SessionSpec s;
    s.kind = game::SessionKind::Skirmish;
    s.skirmish_map = "GS_SPACE";
    s.skirmish_map_data = map;
    s.skirmish.human_faction = "Rebel";
    s.skirmish.ai_faction = "Empire";
    // Rebel cap 5: the second frigate stays in the reinforcement pool (Reinforce test).
    s.skirmish.units_human = {"GS_Frigate", "GS_Frigate", "GS_Corvette", "GS_Corvette"};
    s.skirmish.units_ai = {"GS_Frigate", "GS_Frigate", "GS_Frigate", "GS_Corvette"};
    s.skirmish.ground_human = {"GS_Trooper", "GS_Trooper"};  // on the human planet's surface (FleetManagement launch / land)
    s.skirmish.planet_type = "GS_Planet";
    s.seed = 7;
    s.battle_max_frames = 900;
    s.ask_battle_choice = true;
    return s;
}

struct RunResult {
    CrcLog crc;
    uint64_t ticks = 0;
};

// The fleet retreat destination rule of docs/re/TACTICAL_GAPS.md §4.5, written out for this galaxy: planets newest
// first (reverse row order), skipping the battle planet and unowned (-1) planets, smallest squared 3-D distance
// (double, strict <) under the first filter that has a candidate: (1) orbit controller = R (the starbase owner at
// starbase level > 0, else the owner of the first idle fleet in orbit with a live unit - every fleet here is a combat
// fleet); (2) ground-base owner (the first live structure's owner) = R or starbase owner = R; (3) a neutral faction's
// planet; (4) any.
PlanetIndex expected_retreat(const galactic::GalacticSim& g, int32_t r, PlanetIndex from) {
    const objects::ObjectStore& st = g.store();
    const Vec3 o = g.planets()[from].position;
    auto controller = [&](size_t p) -> int32_t {
        const galactic::PlanetState& pl = g.planets()[p];
        if (pl.starbase_level > 0) return pl.starbase_owner;
        for (const uint32_t f : pl.orbit) {
            const galactic::FleetState& fl = g.fleets()[f];
            if (!fl.alive || fl.moving) continue;
            for (const objects::ObjectHandle m : fl.contents)
                if (st.alive(m)) return fl.owner;
        }
        return -1;
    };
    auto ground_owner = [&](size_t p) -> int32_t {
        for (const objects::ObjectHandle h : g.planets()[p].structures)
            if (st.alive(h)) return st.owner(h);
        return -1;
    };
    auto neutral = [&](size_t p) {
        const uint32_t f = g.players()[static_cast<size_t>(g.planets()[p].owner)].faction;
        return f < g.rules().factions.size() && g.rules().factions[f].is_neutral;
    };
    const std::function<bool(size_t)> filters[] = {[&](size_t p) { return controller(p) == r; },
                                                   [&](size_t p) { return ground_owner(p) == r || g.planets()[p].starbase_owner == r; },
                                                   [&](size_t p) { return neutral(p); },
                                                   [](size_t) { return true; }};
    for (const auto& keep : filters) {
        PlanetIndex best = kInvalidPlanet;
        double best_d = 0.0;
        for (size_t i = g.planets().size(); i-- > 0;) {
            if (!g.planets()[i].is_planet || i == from || g.planets()[i].owner < 0 || !keep(i)) continue;
            const Vec3 q = g.planets()[i].position;
            const double dx = static_cast<double>(q.x) - o.x, dy = static_cast<double>(q.y) - o.y, dz = static_cast<double>(q.z) - o.z;
            const double d = dx * dx + dy * dy + dz * dz;
            if (best == kInvalidPlanet || d < best_d) {
                best = static_cast<PlanetIndex>(i);
                best_d = d;
            }
        }
        if (best != kInvalidPlanet) return best;
    }
    return kInvalidPlanet;
}

// The live run's setup (applied right after start() by the live run and its replays, a GalacticSim direct operation like
// the starting forces): a human cantina on the home planet - a ground base, so the withdrawn fleet's retreat finder picks
// the home planet (filter 2) and the FleetManagement launch / land below happen there.
void home_base_setup(game::GameSession& s) {
    sim::GalacticMode* gm = s.host().galactic_mode();
    galactic::GalacticSim* gs = gm ? gm->sim() : nullptr;
    if (!gs) return;
    const objects::ObjectHandle h = gs->spawn_unit(s.local_player(), game::kSkirmishHumanPlanet, gs->rules().find_type("GS_Cantina"));
    CHECK(gs->store().exists(h));
}

// Live run with scripted orders: returns the recording and the CRC log.
bool live_run(const game::GameData& data, const game::SessionSpec& spec, commands::CommandRecording& rec, RunResult& out) {
    jobs::JobSystem js(jobs::JobSystemConfig{2});
    game::GameSession s(data, js, spec);
    std::vector<std::string> diag;
    CHECK(s.start(&diag, nullptr, &rec));
    for (const std::string& d : diag) std::printf("  diag: %s\n", d.c_str());
    if (!s.started()) return false;
    home_base_setup(s);
    // Hop + conflict: the battle starts within a few hundred ticks.
    // The pending-battle choice (ask_battle_choice): the conflict waits until the Fight answer drains from the stream.
    uint64_t t = 0, asked = 0;
    while (t < 4000 && !s.in_battle()) {
        if (s.battle_choice_pending()) {
            if (asked == 0) asked = t;
            if (t >= asked + 10) CHECK(s.choose_battle(galactic::BattleChoice::Fight));  // answered 10 ticks later
        }
        s.tick();
        ++t;
    }
    std::printf("battle choice asked at tick %" PRIu64 ", battle started at tick %" PRIu64 "\n", asked, t);
    CHECK(asked > 0 && t > asked + 10);
    CHECK(s.in_battle());
    if (!s.in_battle()) return false;
    const sim::TacticalBattle* b = s.battle();
    const objects::ObjectStore& store = s.host().modes().top_tactical()->primary();
    std::vector<uint32_t> mine;
    uint32_t enemy = 0;
    for (const objects::ObjectId id : b->movement().unit_ids()) {
        const objects::ObjectHandle h = store.find(id);
        if (!store.alive(h)) continue;
        if (store.owner(h) == s.local_player()) mine.push_back(static_cast<uint32_t>(id));
        else if (enemy == 0 && b->combat().has_unit(id)) enemy = static_cast<uint32_t>(id);
    }
    std::printf("battle: %zu human units, enemy %u\n", mine.size(), enemy);
    CHECK(mine.size() == 3 && enemy != 0);
    // Select, then attack with the selection; a few ticks later move the first unit away, then stop it.
    CHECK(s.submit(commands::ev::select(mine)).ok());
    CHECK(s.submit(commands::ev::attack(enemy, {})).ok());
    for (int i = 0; i < 5; ++i) s.tick();
    t += 5;
    CHECK(s.order_stats().attacks == 1);
    if (s.battle()) CHECK(s.battle()->combat().current_target(static_cast<tactical::combat::ObjectId>(mine[0])) ==
                          static_cast<tactical::combat::ObjectId>(enemy));
    CHECK(s.submit(commands::ev::select({mine[0]})).ok());
    CHECK(s.submit(commands::ev::move_to_position(1500.0f, -1200.0f, 0.0f)).ok());
    for (int i = 0; i < 3; ++i) s.tick();
    t += 3;
    CHECK(s.order_stats().moves == 1);
    if (s.battle()) CHECK(s.battle()->movement().view(static_cast<objects::ObjectId>(mine[0])).has_goal);
    CHECK(s.submit(commands::ev::stop()).ok());
    for (int i = 0; i < 3; ++i) s.tick();
    t += 3;
    CHECK(s.order_stats().stops == 1 && s.order_stats().rejected == 0);
    // Control groups (SimHost P host block): Ctrl+3 assigns the selection, a recall restores it.
    CHECK(s.submit(commands::ev::select(mine)).ok());
    CHECK(s.submit(commands::ev::control_group(3, true)).ok());
    CHECK(s.submit(commands::ev::select({mine[0]})).ok());
    for (int i = 0; i < 3; ++i) s.tick();
    CHECK(s.host().control_group(static_cast<uint8_t>(s.local_player()), 3).size() == mine.size());
    CHECK(s.host().selection(static_cast<uint8_t>(s.local_player())).size() == 1);
    CHECK(s.submit(commands::ev::control_group(3, false)).ok());
    for (int i = 0; i < 3; ++i) s.tick();
    t += 6;
    CHECK(s.host().selection(static_cast<uint8_t>(s.local_player())).size() == mine.size());
    // MoveToPositionFacing (id 47: point + facing in degrees, explicit units) through the order hook.
    {
        commands::EventData mf(commands::EventType::MoveToPositionFacing);
        mf.set_f("x", 1200.0f).set_f("y", -1000.0f).set_f("angle", 90.0f).set_ids("units", {mine[0]});
        CHECK(s.submit(std::move(mf)).ok());
        for (int i = 0; i < 3; ++i) s.tick();
        t += 3;
        CHECK(s.order_stats().moves == 2 && s.order_stats().rejected == 0);
        if (s.battle()) CHECK(s.battle()->movement().view(static_cast<objects::ObjectId>(mine[0])).has_goal);
    }
#ifdef ALAMO_GAME_HAVE_UI
    // Pause / resume through the command stream: once the pause entry drained, no tick runs until the resume.
    CHECK(s.set_paused(true));
    for (int i = 0; i < 3; ++i) s.tick();
    CHECK(s.paused());
    const uint64_t paused_at = s.host().frame();
    for (int i = 0; i < 10; ++i) CHECK(!s.tick());
    CHECK(s.host().frame() == paused_at);
    CHECK(s.set_paused(false) && !s.paused());
    for (int i = 0; i < 3; ++i) s.tick();
    CHECK(s.pause_events() == 2);
    std::printf("pause: paused at frame %" PRIu64 ", resumed\n", paused_at);
#endif
    // Queueable actions (shift+click): a hard point attack, then a queued move behind it; they run in order.
    if (s.battle() && s.battle()->combat().has_unit(static_cast<tactical::combat::ObjectId>(enemy)) &&
        s.battle()->combat().hardpoint_count(static_cast<tactical::combat::ObjectId>(enemy)) > 0) {
        const auto hp_kind = static_cast<uint32_t>(s.battle()->combat().hardpoint_kind(static_cast<tactical::combat::ObjectId>(enemy), 0));
        CHECK(s.submit(commands::ev::select({mine[1]})).ok());
        CHECK(s.submit(commands::ev::stop()).ok());  // idle: the first queued order starts at once
        s.tick();
        commands::EventData atk = commands::ev::attack(enemy, {});
        atk.set("ref2", hp_kind + 1);
        atk.set_queued(true);
        commands::EventData mv = commands::ev::move_to_position(-1500.0f, 1200.0f, 0.0f);
        mv.set_queued(true);
        CHECK(s.submit(std::move(atk)).ok());
        CHECK(s.submit(std::move(mv)).ok());
        for (int i = 0; i < 3; ++i) s.tick();
        if (const sim::TacticalBattle* bt = s.battle()) {
            const auto q = bt->queued_orders(static_cast<objects::ObjectId>(mine[1]));
            std::printf("queue: unit %u has %zu queued order(s) behind its current one\n", mine[1], q.size());
            CHECK(q.size() == 1 && q[0].kind == sim::TacticalBattle::QueuedOrder::Kind::Move);  // the attack runs, the move waits
            CHECK(bt->combat().current_target(static_cast<tactical::combat::ObjectId>(mine[1])) == static_cast<tactical::combat::ObjectId>(enemy));
        }
        // When the hard point attack completes, the queued move starts (the unit gets a movement goal).
        uint64_t waited = 0;
        while (waited < 600 && s.battle() && !s.battle()->queued_orders(static_cast<objects::ObjectId>(mine[1])).empty()) {
            s.tick();
            ++waited;
        }
        if (const sim::TacticalBattle* bt = s.battle()) {
            const bool popped = bt->queued_orders(static_cast<objects::ObjectId>(mine[1])).empty();
            std::printf("queue: %s after %" PRIu64 " ticks\n", popped ? "the move started" : "the attack still runs", waited);
            if (popped && bt->movement().has_unit(static_cast<objects::ObjectId>(mine[1])))
                CHECK(bt->movement().view(static_cast<objects::ObjectId>(mine[1])).has_goal ||
                      bt->movement().view(static_cast<objects::ObjectId>(mine[1])).arrived);
        }
    }
    // Reinforce (human, through the command stream): the frigate left in the pool by the cap is refused while the cap is
    // full (QueueFull), then lands at the requested point once a human unit died and freed the population.
    uint32_t reserve = 0;
    if (const sim::TacticalBattle* bt = s.battle()) {
        const auto pool = bt->reserves(s.local_player());
        std::printf("reinforce: %zu reserve(s) in the pool\n", pool.size());
        CHECK(pool.size() == 1);
        if (!pool.empty()) reserve = static_cast<uint32_t>(pool[0].galactic_id);
    }
    // Drop point: next to a live human unit (battle host: the space drop validity 0x4d6fd0 refuses fogged points, TACTICAL_GAPS §4.2).
    float rx = -1500.0f, ry = 0.0f;
    if (const sim::TacticalBattle* bt = s.battle())
        for (const uint32_t id : mine)
            if (bt->movement().has_unit(static_cast<objects::ObjectId>(id))) {
                const auto v = bt->movement().view(static_cast<objects::ObjectId>(id));
                rx = v.pos.x;
                ry = v.pos.y + 300.0f;
                break;
            }
    if (reserve != 0) {
        const uint64_t rej0 = s.order_stats().rejected;
        CHECK(s.submit(commands::EventData(commands::EventType::Reinforce).set("unit", reserve).set_f("x", rx).set_f("y", ry)).ok());
        for (int i = 0; i < 3; ++i) s.tick();
        t += 3;
        CHECK(s.order_stats().reinforcements == 0 && s.order_stats().rejected == rej0 + 1);  // QueueFull: the cap is used
        uint64_t waited = 0;
#ifdef ALAMO_GAME_HAVE_CONTROLS
        game::RadarFeed radar_feed;  // read-only radar feed over the live battle: one Default_Unit_Die ping per visible death
        ui::RadarSettings radar_cfg;
        ui::RadarEvents radar_events;
        uint64_t radar_deaths = 0;
#endif
        while (waited < 2000 && s.battle() && s.order_stats().reinforcements == 0) {
            if (waited % 20 == 0)
                (void)s.submit(commands::EventData(commands::EventType::Reinforce).set("unit", reserve).set_f("x", rx).set_f("y", ry));
            s.tick();
            ++waited;
            ++t;
#ifdef ALAMO_GAME_HAVE_CONTROLS
            if (const sim::TacticalBattle* bt = s.battle()) {
                radar_deaths += bt->combat().deaths().size();
                radar_feed.battle_tick(*bt, store, data.types, {}, s.local_player(), nullptr, radar_cfg, radar_events);
            }
#endif
        }
#ifdef ALAMO_GAME_HAVE_CONTROLS
        std::printf("radar feed: %" PRIu64 " death(s), %" PRIu64 " ping(s), %" PRIu64 " capture ping(s), %zu event(s) live\n", radar_deaths,
                    radar_feed.death_pings(), radar_feed.transition_pings(), radar_events.items().size());
        CHECK(radar_feed.death_pings() <= radar_deaths);
        CHECK(radar_deaths == 0 || radar_feed.death_pings() > 0);  // the human unit that died is visible to its owner
#endif
        std::printf("reinforce: %s after %" PRIu64 " ticks\n", s.order_stats().reinforcements ? "deployed" : "not deployed", waited);
        CHECK(s.order_stats().reinforcements == 1);
        if (const sim::TacticalBattle* bt = s.battle()) CHECK(bt->reserves(s.local_player()).empty());
    }
    // (The retreat dialog's Autoresolve ends the battle: battle_auto_resolve_run.)
    // The human's withdrawn fleet goes where the retreat destination finder (TACTICAL_GAPS.md §4.5) points: the rule
    // computed here from the galaxy as it stands during the battle, cross-checked with GalacticSim's finder.
    PlanetIndex retreat_to = kInvalidPlanet;
    if (const galactic::GalacticSim* g = s.galaxy()) {
        retreat_to = expected_retreat(*g, s.local_player(), game::kSkirmishBattlePlanet);
        const PlanetIndex finder = s.battles()->conflicts().retreat_destination(s.local_player(), game::kSkirmishBattlePlanet);
        std::printf("retreat: rule -> planet %u, finder -> planet %u\n", retreat_to, finder);
        CHECK(retreat_to != kInvalidPlanet && finder == retreat_to);
    }
    // Run to the battle's end (time limit 900 frames) and a little past it.
    uint64_t after = 0;
    while (t < 8000 && after < 60) {
        s.tick();
        ++t;
        if (s.skirmish_over()) ++after;
    }
    CHECK(s.skirmish_over() && s.host().modes().depth() == 1);
    // Fleet drag and drop (FleetManagement, GalacticSim P host block): detach one unit of a human fleet into a fleet of its
    // own at the planet, then merge it back.
    if (const galactic::GalacticSim* g = s.galaxy()) {
        const objects::ObjectStore& gst = g->store();
        auto human_fleets = [&] {
            std::vector<uint32_t> v;
            for (uint32_t i = 0; i < g->fleets().size(); ++i)
                if (g->fleets()[i].alive && g->fleets()[i].owner == s.local_player() && !g->fleets()[i].moving && g->fleets()[i].planet != kInvalidPlanet)
                    v.push_back(i);
            return v;
        };
        for (int i = 0; i < 3000 && human_fleets().empty(); ++i) {  // the withdrawn fleet is still in hyperspace
            s.tick();
            ++t;
        }
        const std::vector<uint32_t> before = human_fleets();
        const galactic::FleetState* big = nullptr;
        for (const uint32_t i : before)
            if (g->fleets()[i].contents.size() >= 2) big = &g->fleets()[i];
        std::printf("fleet management: %zu human fleet(s), %s\n", before.size(), big ? "one with 2+ units" : "none with 2+ units");
        CHECK(big != nullptr);
        if (big) {
            std::printf("retreat: the withdrawn fleet is at planet %u (expected %u)\n", big->planet, retreat_to);
            CHECK(big->planet == retreat_to);
        }
        if (big) {
            const auto fleet_id = static_cast<uint32_t>(gst.id(big->object));
            const auto unit_id = static_cast<uint32_t>(gst.id(big->contents.back()));
            const auto planet_id = static_cast<uint32_t>(gst.id(g->planets()[big->planet].object));
            const size_t n0 = big->contents.size();
            commands::EventData det(commands::EventType::FleetManagement);
            det.set("target", planet_id).set("fleet", fleet_id).set("unit", unit_id).set("op", 2u);
            CHECK(s.submit(std::move(det)).ok());
            for (int i = 0; i < 3; ++i) s.tick();
            t += 3;
            const std::vector<uint32_t> mid = human_fleets();
            CHECK(mid.size() == before.size() + 1);
            uint32_t lone = 0;
            for (const uint32_t i : mid)
                if (g->fleets()[i].contents.size() == 1 && static_cast<uint32_t>(gst.id(g->fleets()[i].contents[0])) == unit_id)
                    lone = static_cast<uint32_t>(gst.id(g->fleets()[i].object));
            CHECK(lone != 0);
            commands::EventData mrg(commands::EventType::FleetManagement);
            mrg.set("target", fleet_id).set("fleet", lone).set("unit", commands::kNoObject).set("op", 3u);
            CHECK(s.submit(std::move(mrg)).ok());
            for (int i = 0; i < 3; ++i) s.tick();
            t += 3;
            CHECK(human_fleets().size() == before.size());
            const uint32_t back = g->fleet_of(gst.find(static_cast<objects::ObjectId>(fleet_id)));
            CHECK(back != galactic::kNoFleet && g->fleets()[back].contents.size() == n0);
            std::printf("fleet management: detached unit %u and merged it back (%zu units)\n", unit_id, n0);
        }
        // FleetManagement launch (op 4) and land (op 5): a trooper of the human planet's surface goes up into a human fleet
        // in orbit there, then lands again.
        size_t at = g->planets().size();
        uint32_t trooper = 0;
        for (size_t p = 0; p < g->planets().size() && trooper == 0; ++p)
            for (const objects::ObjectHandle h : g->planets()[p].surface)
                if (gst.alive(h) && gst.owner(h) == s.local_player()) {
                    trooper = static_cast<uint32_t>(gst.id(h));
                    at = p;
                    break;
                }
        uint32_t fleet = galactic::kNoFleet;
        for (int i = 0; i < 3000 && trooper != 0 && fleet == galactic::kNoFleet; ++i) {
            for (const uint32_t f : human_fleets())
                if (g->fleets()[f].planet == at) fleet = f;
            if (fleet == galactic::kNoFleet) {
                s.tick();
                ++t;
            }
        }
        std::printf("fleet management: trooper %u, human fleet at its planet: %s\n", trooper, fleet != galactic::kNoFleet ? "yes" : "no");
        CHECK(trooper != 0 && fleet != galactic::kNoFleet);
        if (trooper != 0 && fleet != galactic::kNoFleet) {
            const auto fleet_id = static_cast<uint32_t>(gst.id(g->fleets()[fleet].object));
            const auto planet_id = static_cast<uint32_t>(gst.id(g->planets()[at].object));
            const size_t n0 = g->fleets()[fleet].contents.size(), s0 = g->planets()[at].surface.size();
            CHECK(s.submit(galactic::fleet_management_event(galactic::kFleetLaunch, fleet_id, commands::kNoObject, trooper)).ok());
            for (int i = 0; i < 3; ++i) s.tick();
            t += 3;
            CHECK(g->fleets()[fleet].contents.size() == n0 + 1 && g->planets()[at].surface.size() == s0 - 1);
            CHECK(s.submit(galactic::fleet_management_event(galactic::kFleetLand, planet_id, fleet_id, trooper)).ok());
            for (int i = 0; i < 3; ++i) s.tick();
            t += 3;
            CHECK(g->fleets()[fleet].contents.size() == n0 && g->planets()[at].surface.size() == s0);
            std::printf("fleet management: launched trooper %u into fleet %u and landed it again\n", trooper, fleet_id);
        }
    }
    if (s.battles()) for (const std::string& l : s.battles()->log()) std::printf("  %s\n", l.c_str());
    std::printf("live run: %" PRIu64 " ticks, orders routed %" PRIu64 ", final CRC %016" PRIx64 "\n", t, s.order_stats().routed,
                s.host().crc_log().records.empty() ? 0 : s.host().crc_log().records.back().combined);
    out.crc = s.host().crc_log();
    out.ticks = s.ticks();
    return true;
}

// The pending-battle choice answered Auto-resolve: the conflict resolves without a battle (conflict log entry with the
// human as attacker). The prompt's Auto-Resolve forces the result (GALACTIC_DIALOGS §1.5: no AutoResolveDisplayTime
// wait), so the auto-resolve is normally never observed running; when it is, the dialog's Withdraw (Withdrawl 49, no
// payload) drains and Set_Retreating takes it. A Withdraw after the result is a silent drop.
bool auto_resolve_run(const game::GameData& data, const game::SessionSpec& spec, commands::CommandRecording& rec, RunResult& out) {
    jobs::JobSystem js(jobs::JobSystemConfig{2});
    game::GameSession s(data, js, spec);
    CHECK(s.start(nullptr, nullptr, &rec));
    if (!s.started()) return false;
    uint64_t t = 0;
    bool chose = false, saw_running = false;
    while (t < 4000 && s.battles()->conflicts().log().empty()) {
        if (!chose && s.battle_choice_pending()) chose = s.choose_battle(galactic::BattleChoice::AutoResolve);
        if (s.auto_resolving() && !saw_running) {  // the dialog's Withdraw while the auto-resolve runs
            saw_running = true;
            CHECK(s.can_withdraw());  // the Withdraw enable rule: the human is a side of the head conflict
            CHECK(s.withdraw());
        }
        CHECK(!s.in_battle());
        s.tick();
        ++t;
    }
    for (int i = 0; i < 5; ++i) s.tick();  // the Withdraw drains (if it was sent on the last tick)
    t += 5;
    const auto& log = s.battles()->conflicts().log();
    CHECK(chose && log.size() == 1);
    if (!log.empty()) {
        std::printf("auto-resolve: how %u, attacker %d, defender %d, winner %d, losses %u / %u, %u RNG draws\n", static_cast<unsigned>(log[0].how), log[0].attacker,
                    log[0].defender, log[0].winner, log[0].attacker_losses, log[0].defender_losses, log[0].rng_draws);
        CHECK(log[0].attacker == s.local_player());
    }
    CHECK(s.battles()->battles_fought() == 0);
    std::printf("auto-resolve: withdraw drained %" PRIu64 ", accepted %" PRIu64 ", dropped %" PRIu64 "\n", s.port_stats().withdraw,
                s.port_stats().withdraw_accepted, s.port_stats().withdraw_dropped);
    const uint64_t early = saw_running ? 1u : 0u;
    CHECK(s.port_stats().withdraw == early && s.port_stats().withdraw_accepted == early);
    // A late Withdraw (the auto-resolve has its result): dropped silently, the command itself succeeds (§9.2).
    const uint64_t rej0 = s.host().commands_rejected();
    CHECK(s.withdraw());
    for (int i = 0; i < 3; ++i) s.tick();
    t += 3;
    CHECK(s.port_stats().withdraw == early + 1 && s.port_stats().withdraw_dropped == 1 && s.host().commands_rejected() == rej0);
    out.crc = s.host().crc_log();
    out.ticks = s.ticks();
    std::printf("auto-resolve run: %" PRIu64 " ticks, final CRC %016" PRIx64 "\n", t, out.crc.records.empty() ? 0 : out.crc.records.back().combined);
    return true;
}

// The retreat dialog's Autoresolve (P4_SIM_SYSTEMS §9.3; the port event 0x5441 through the command stream) in a running
// battle: CampaignBattles::auto_resolve_battle stops it, the galaxy resolves it from tactical and the tactical mode pops.
bool battle_auto_resolve_run(const game::GameData& data, const game::SessionSpec& spec, commands::CommandRecording& rec, RunResult& out) {
    jobs::JobSystem js(jobs::JobSystemConfig{2});
    game::GameSession s(data, js, spec);
    CHECK(s.start(nullptr, nullptr, &rec));
    if (!s.started()) return false;
    uint64_t t = 0;
    while (t < 4000 && !s.in_battle()) {
        if (s.battle_choice_pending()) (void)s.choose_battle(galactic::BattleChoice::Fight);
        s.tick();
        ++t;
    }
    CHECK(s.in_battle());
    if (!s.in_battle()) return false;
    // Past the retreat countdown (Space_Retreat_Allowed_Countdown_Seconds 5 = 150 frames): the gate lets it through.
    for (int i = 0; i < 200 && s.in_battle(); ++i, ++t) s.tick();
    CHECK(s.in_battle() && s.auto_resolve_battle());
    uint64_t waited = 0;
    while (waited < 10 && s.in_battle()) {
        s.tick();
        ++waited;
        ++t;
    }
    std::printf("battle auto-resolve: %" PRIu64 " request(s), %" PRIu64 " refused, battle %s after %" PRIu64 " tick(s)\n",
                s.port_stats().tactical_auto_resolve, s.port_stats().tactical_auto_resolve_refused, s.in_battle() ? "still running" : "ended", waited);
    CHECK(s.port_stats().tactical_auto_resolve == 1 && s.port_stats().tactical_auto_resolve_refused == 0);
    CHECK(!s.in_battle() && s.host().modes().depth() == 1 && s.battles()->battles_fought() == 1);
    bool logged = false;
    for (const std::string& l : s.battles()->log()) logged = logged || l.find("auto-resolve from tactical") != std::string::npos;
    CHECK(logged);
    const auto& log = s.battles()->conflicts().log();
    CHECK(!log.empty() && log.back().how == 1);
    if (!log.empty()) std::printf("battle auto-resolve: conflict log how %u, winner %d\n", static_cast<unsigned>(log.back().how), log.back().winner);
    // A second request with no battle running: refused.
    CHECK(s.auto_resolve_battle());
    for (int i = 0; i < 3; ++i, ++t) s.tick();
    CHECK(s.port_stats().tactical_auto_resolve == 2 && s.port_stats().tactical_auto_resolve_refused == 1);
    out.crc = s.host().crc_log();
    out.ticks = s.ticks();
    return true;
}

// P4 host: the FoC galactic special systems through GameSession (synthetic: the shipped skirmish has none of it). The
// setup, applied right after start() by the live run and every replay alike (GalacticSim direct operations, like the
// session's starting forces): Corruption_Black_Market at the AI's Empire planet, a cantina there (Sabotage_Cost_Credits
// 800), a Death Star in the AI's orbit fleet, the human's smuggler / saboteur / slicer in orbit, 100000 credits.
struct SpecialIds {
    uint32_t planet = 0, smuggler = 0, saboteur = 0, slicer = 0, cantina = 0, death_star = 0, r2 = 0;
};
SpecialIds special_setup(game::GameSession& s) {
    SpecialIds ids;
    sim::GalacticMode* gm = s.host().galactic_mode();
    galactic::GalacticSim* gs = gm ? gm->sim() : nullptr;
    if (!gs) return ids;
    const PlanetIndex p = game::kSkirmishBattlePlanet;
    const int32_t human = s.local_player(), ai = gs->planets()[p].owner;
    auto spawn = [&](int32_t player, const char* type) {
        const objects::ObjectHandle h = gs->spawn_unit(player, p, gs->rules().find_type(type));
        return gs->store().exists(h) ? static_cast<uint32_t>(gs->store().id(h)) : 0u;
    };
    gs->set_corruption(p, 6, ai);  // Corruption_Black_Market
    ids.cantina = spawn(ai, "GS_Cantina");
    ids.death_star = spawn(ai, "GS_DeathStar");
    ids.smuggler = spawn(human, "GS_Smuggler");
    ids.saboteur = spawn(human, "GS_Saboteur");
    ids.slicer = spawn(human, "GS_Slicer");
    ids.r2 = spawn(human, "GS_R2");
    gs->add_credits(human, 100000.0f, false);
    ids.planet = static_cast<uint32_t>(gs->store().id(gs->planets()[p].object));
    return ids;
}

bool special_run(const game::GameData& data, const game::SessionSpec& spec, commands::CommandRecording& rec, RunResult& out) {
    jobs::JobSystem js(jobs::JobSystemConfig{2});
    game::GameSession s(data, js, spec);
    CHECK(s.start(nullptr, nullptr, &rec));
    if (!s.started()) return false;
    const SpecialIds ids = special_setup(s);
    const galactic::GalacticSim* g = s.galaxy();
    CHECK(g && ids.cantina && ids.death_star && ids.smuggler && ids.saboteur && ids.slicer && ids.r2);
    if (!g || !ids.smuggler) return false;
    const PlanetIndex p = game::kSkirmishBattlePlanet;
    const int32_t me = s.local_player();
    auto run = [&](int n) {
        for (int i = 0; i < n; ++i) s.tick();
    };
    auto dialog_kind = [&]() { return s.special_dialog() ? s.special_dialog()->kind : 0u; };
    // Executors charge the q8-requantized amount (COMMANDS 3.2 local round-trip).
    auto q8 = [](float v) { return commands::requantize(galactic::kQuantAmount, v); };
    // 1. Black market: activate (kind 1) -> SpecialDialog kind 4 -> buy (kind 4) at the offered price.
    CHECK(s.activate_special(ids.smuggler, ids.planet));
    run(2);
    std::printf("special: black-market dialog kind %u, respawn R %.2f s\n", dialog_kind(), s.special_dialog() ? s.special_dialog()->respawn_seconds : 0.0f);
    CHECK(dialog_kind() == galactic::kSpecialBlackMarket);
    const uint32_t item = g->rules().special.find_item_by_name("GS_Item_Empire");
    const std::vector<galactic::BlackMarketOffer> offers = g->bm_offers(me, p, g->store().find(static_cast<objects::ObjectId>(ids.smuggler)));
    CHECK(offers.size() == 1 && offers[0].item == item && offers[0].price == 1000.0f && !offers[0].purchased);
    {   // L1P4_LUA_GAPS 2.3: the row is clickable when not purchased and the ROUNDED price fits the ROUNDED credits.
        galactic::BlackMarketOffer o;
        o.price = 1000.3f;
        CHECK(game::black_market_row_enabled(o, 999.6f) && game::black_market_row_enabled(o, 1000.0f));
        CHECK(!game::black_market_row_enabled(o, 999.4f));
        o.purchased = true;
        CHECK(!game::black_market_row_enabled(o, 1.0e6f));
    }
    const float c0 = g->credits(me);
    CHECK(s.buy_black_market(item) && !s.special_dialog());
    run(2);
    std::printf("special: purchase: item owned %d, credits %.3f -> %.3f\n", g->bm_has_item(me, item) ? 1 : 0, c0, g->credits(me));
    CHECK(g->bm_has_item(me, item) && g->credits(me) == c0 - q8(1000.0f));
    CHECK(!g->store().alive(g->store().find(static_cast<objects::ObjectId>(ids.smuggler))));  // departed (respawn scheduled)
    CHECK(s.special_stats().purchases == 1 && !g->respawns_pending(p).empty());
    // 2. Sabotage: activate -> SpecialDialog kind 5 (the corrupted planet's special structure) -> the cantina (kind 5).
    CHECK(s.activate_special(ids.saboteur, ids.planet));
    run(2);
    CHECK(dialog_kind() == galactic::kSpecialSabotage);
    const std::vector<galactic::SabotageTarget> rows =
        g->sabotage_targets(me, p, g->store().find(static_cast<objects::ObjectId>(ids.saboteur)));
    CHECK(rows.size() == 1 && rows[0].cost == 800);
    const float c1 = g->credits(me);
    CHECK(s.sabotage_structure(ids.cantina));
    run(2);
    std::printf("special: sabotage: cantina alive %d, count %d, credits %.3f -> %.3f, events %" PRIu64 "\n",
                g->store().alive(g->store().find(static_cast<objects::ObjectId>(ids.cantina))) ? 1 : 0, g->sabotage_count(p), c1, g->credits(me),
                s.special_stats().sabotages);
    CHECK(!g->store().alive(g->store().find(static_cast<objects::ObjectId>(ids.cantina))) && g->sabotage_count(p) == 1 &&
          g->credits(me) == c1 - q8(800.0f) && s.special_stats().sabotages == 1);
    // 3. Slice (REMAINING_GAPS 1.3): activate -> SpecialDialog kind 2 with R from the tech-1 respawn range; Cancel sends
    // nothing; activate again -> pick the single row (kind 2, only-one): unlocked, charged, tech + 1, the slicer departs.
    const uint32_t stolen = g->rules().find_type("GS_Stolen");
    auto r2_alive = [&]() { return g->store().alive(g->store().find(static_cast<objects::ObjectId>(ids.r2))); };
    CHECK(s.activate_special(ids.r2, ids.planet));
    run(2);
    CHECK(dialog_kind() == galactic::kSpecialSlice);
    const float r_slice = s.special_dialog() ? s.special_dialog()->respawn_seconds : 0.0f;
    const std::vector<galactic::SliceEntry> slice = game::slice_rows(*g, me, p, ids.r2);
    std::printf("special: slice dialog: %zu row(s), R %.2f s\n", slice.size(), r_slice);
    CHECK(slice.size() == 1 && r_slice >= 20.0f && r_slice <= 30.0f);
    if (!slice.empty()) CHECK(slice[0].type == stolen && slice[0].price == 1500.0f && slice[0].chance == 1.0f);
    CHECK(!s.slice_tech(stolen + 1000u) && s.special_dialog());  // not a row: nothing sent, the dialog stays open
    s.close_special_dialog();                                    // Cancel
    CHECK(!s.slice_tech(stolen));
    run(2);
    CHECK(!g->type_unlocked(me, stolen) && r2_alive() && s.special_stats().slices == 0);
    CHECK(s.activate_special(ids.r2, ids.planet));
    run(2);
    CHECK(dialog_kind() == galactic::kSpecialSlice);
    const float cs = g->credits(me);
    const int32_t tech0 = g->players()[static_cast<size_t>(me)].tech_level;
    CHECK(s.slice_tech(stolen) && !s.special_dialog());
    run(2);
    const int32_t tech1 = g->players()[static_cast<size_t>(me)].tech_level;
    std::printf("special: slice: unlocked %d, credits %.3f -> %.3f, tech %d -> %d, slices %" PRIu64 " (%" PRIu64 " ok), unlocks %" PRIu64 "\n",
                g->type_unlocked(me, stolen) ? 1 : 0, cs, g->credits(me), tech0, tech1, s.special_stats().slices, s.special_stats().slice_successes,
                s.special_stats().types_unlocked);
    CHECK(g->type_unlocked(me, stolen) && g->credits(me) == cs - q8(1500.0f) && tech1 == tech0 + 1 && !r2_alive());
    CHECK(s.special_stats().slices == 1 && s.special_stats().slice_successes == 1 && s.special_stats().types_unlocked >= 1);
    // 4. Hack: activate -> SpecialDialog kind 7 -> hack (kind 7, Hack_Super_Weapon_Cost 500): the planet is destroyed.
    CHECK(s.activate_special(ids.slicer, ids.planet));
    run(2);
    CHECK(dialog_kind() == galactic::kSpecialHack);
    CHECK(g->store().exists(g->hack_find_super_weapon(p)));
    const float c2 = g->credits(me);
    CHECK(s.hack_super_weapon());
    run(2);
    std::printf("special: hack: destroyed %d, credits %.3f -> %.3f, hacks %" PRIu64 ", planets destroyed %" PRIu64 "\n", g->planets()[p].destroyed ? 1 : 0, c2,
                g->credits(me), s.special_stats().hacks, s.special_stats().planets_destroyed);
    CHECK(g->planets()[p].destroyed && g->credits(me) == c2 - q8(500.0f) && s.special_stats().hacks == 1 &&
          s.special_stats().planets_destroyed == 1);
    // A kind-4 answer with no dialog open is not sent.
    CHECK(!s.buy_black_market(item));
    // The presentation queue saw the events in stream order.
    const std::vector<galactic::GalacticEvent> ev = s.take_special_events();
    size_t dialogs = 0, slice_ok = 0, slice_unlocks = 0;
    for (const galactic::GalacticEvent& e : ev) {
        dialogs += e.kind == galactic::GalacticEventKind::SpecialDialog ? 1u : 0u;
        slice_ok += e.kind == galactic::GalacticEventKind::Slice && e.value == 0 && e.type == stolen ? 1u : 0u;
        slice_unlocks += e.kind == galactic::GalacticEventKind::TypeUnlocked && e.value == 2 && e.type == stolen ? 1u : 0u;
    }
    CHECK(dialogs == 5 && slice_ok == 1 && slice_unlocks == 1 && s.take_special_events().empty());
    std::printf("special: purchase / sabotage / slice / hack done by tick %" PRIu64 " (%zu events), credits %.0f\n", s.ticks(), ev.size(), g->credits(me));
    run(30);
    out.crc = s.host().crc_log();
    out.ticks = s.ticks();
    return true;
}

// Story dialog script parse (story_script.hpp): colours, waits, cues, movie, EXTRA_TEXT, chapters.
void story_script_test() {
    const char* kScript =
        "[CHAPTER 0]\r\n\r\nTITLE TEXT_T0\r\nMOVIE Mon_Mothma_Loop\r\n\r\nWAIT 500\r\nSFX Button_Press\r\nTEXTCOLOR 128 55 136 255\r\n"
        "TEXT TEXT_A\r\nTEXTCOLOR 255 255 255 176\r\nNEWLINE 1\r\nTEXT TEXT_B\r\nDIALOG MON_01\r\nWAIT 1000\r\nPAUSE 1\r\n"
        "NEWLINE 2\r\nEXTRA_TEXT\r\n\r\n[CHAPTER 1]\r\nTITLE TEXT_T1\r\nMOVIE_ONCE Empire_Symbol\r\nIMAGE i_pic.tga\r\nTEXT TEXT_C\r\n";
    game::StoryScript sc;
    CHECK(game::parse_story_script(kScript, 0, sc));
    CHECK(sc.title == "TEXT_T0" && sc.movie == "Mon_Mothma_Loop" && sc.movie_loop && sc.pauses == 1);
    CHECK(sc.lines.size() == 6);  // A, blank, B, blank x2, extra
    if (sc.lines.size() == 6) {
        CHECK(sc.lines[0].kind == game::StoryScriptLine::Kind::Text && sc.lines[0].text_id == "TEXT_A" && sc.lines[0].rgba[0] == 128 &&
              sc.lines[0].at == 0.5);
        CHECK(sc.lines[2].text_id == "TEXT_B" && sc.lines[2].rgba[3] == 176 && sc.lines[2].at == 0.5);
        CHECK(sc.lines[3].kind == game::StoryScriptLine::Kind::Blank && sc.lines[3].at == 1.5);
        CHECK(sc.lines[5].kind == game::StoryScriptLine::Kind::Extra);
    }
    CHECK(sc.cues.size() == 2 && sc.cues[0].kind == game::StoryScriptCue::Kind::Sfx && sc.cues[0].event == "Button_Press" &&
          sc.cues[1].kind == game::StoryScriptCue::Kind::Speech && sc.cues[1].at == 0.5);
    CHECK(game::parse_story_script(kScript, 1, sc));
    CHECK(sc.title == "TEXT_T1" && sc.movie == "Empire_Symbol" && !sc.movie_loop && sc.image == "i_pic.tga" && sc.lines.size() == 1 &&
          sc.lines[0].rgba[3] == 0);
    CHECK(!game::parse_story_script(kScript, 2, sc));
    // GALACTIC_DIALOGS §2.6: token stream (arguments across lines, ']' a delimiter), case-insensitive keywords, '#' the
    // only comment, the unused keywords recognised, TEXTCOLOR alpha 255 at the end of the file, an unknown keyword (also
    // in another chapter) or a ';' / '//' line rejects the file.
    const char* kStream =
        "text TEXT_X\n[chapter 3]tExT\n  TEXT_Y # comment TEXT ignored\nMusic m ANIM 2 CLEARTEXT LOADCINEMATIC c CLEARIMAGE HIDECINEMATIC 1\n"
        "STOPMOVIE WAIT_SPEECH wait\n250 newline 2 TEXT TEXT_Z TEXTCOLOR 1 2 3";
    CHECK(game::parse_story_script(kStream, 3, sc));
    CHECK(sc.lines.size() == 4 && sc.lines[0].text_id == "TEXT_Y" && sc.lines[0].at == 0.0);
    if (sc.lines.size() == 4) CHECK(sc.lines[1].kind == game::StoryScriptLine::Kind::Blank && sc.lines[3].text_id == "TEXT_Z" && sc.lines[3].at == 0.25);
    CHECK(game::parse_story_script("[CHAPTER 0] TEXTCOLOR 9 2 3\n", 0, sc));  // alpha 255 at the end of the file
    // Mid-file the 4th integer is the next token whatever it is (atoi("TEXT") = 0), so "T" is then an unknown keyword.
    CHECK(!game::parse_story_script("[CHAPTER 0] TEXTCOLOR 1 2 3 TEXT T", 0, sc));
    CHECK(!game::parse_story_script("[CHAPTER 0]\nTEXT T\n[CHAPTER 1]\nTEXTCOLOR255 255 255 176\n", 0, sc) && sc.lines.empty());
    CHECK(!game::parse_story_script("[CHAPTER 0]\n; note\nTEXT T\n", 0, sc));
    CHECK(!game::parse_story_script("[CHAPTER 0]\n// note\nTEXT T\n", 0, sc));
    std::printf("story script: chapters 0 / 1 parsed, chapter 2 missing, token stream / keyword rules ok\n");
}

// GALACTIC_DIALOGS §2.6 on real data (ALAMO_FOC_DATA, else skipped): every shipped Data\Scripts\Story\*.txt parses
// (some chapter 0..99 present) except DIALOG_TUTORIAL_07, whose "TEXTCOLOR255" typo rejects it as in the original.
void story_scripts_real_test() {
    const char* foc = std::getenv("ALAMO_FOC_DATA");
    if (!foc || !*foc) {
        std::printf("story scripts (real data): ALAMO_FOC_DATA not set: skipped\n");
        return;
    }
    game::GameData data;
    std::string err;
    if (!game::load_game_data(foc, {}, data, &err) || !data.vfs) {
        std::printf("story scripts (real data): load failed: %s\n", err.c_str());
        CHECK(false);
        return;
    }
    size_t ok = 0;
    std::vector<std::string> rejected;
    const std::vector<std::string> files = data.vfs->list("Data\\Scripts\\Story\\*.txt");
    for (const std::string& f : files) {
        std::vector<uint8_t> bytes;
        bool loaded = false;
        if (data.files->read(f, bytes)) {
            const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            game::StoryScript sc;
            for (int32_t c = 0; c < 100 && !loaded; ++c) loaded = game::parse_story_script(text, c, sc);
        }
        if (loaded) ++ok;
        else rejected.push_back(f);
    }
    CHECK(files.size() >= 90);
    CHECK(rejected.size() == 1 && rejected[0].find("DIALOG_TUTORIAL_07.TXT") != std::string::npos);
    for (const std::string& f : rejected) std::printf("story scripts (real data): rejected %s\n", f.c_str());
    std::printf("story scripts (real data): %zu / %zu load\n", ok, files.size());
}

void replay_run(const game::GameData& data, const game::SessionSpec& spec, const commands::CommandRecording& rec, uint64_t ticks,
                int32_t workers, RunResult& out, bool special = false, bool home_base = false) {
    jobs::JobSystem js(jobs::JobSystemConfig{workers});
    game::GameSession s(data, js, spec);
    std::vector<std::string> diag;
    CHECK(s.start(&diag, &rec, nullptr));
    if (special) special_setup(s);
    if (home_base) home_base_setup(s);
    for (uint64_t i = 0; i < ticks; ++i) s.tick();
    out.crc = s.host().crc_log();
    out.ticks = ticks;
}

// --- OI host ---
// Cinematic rate lock: the same session run twice, the second with the lock forced on for host frames [50, 80) (the
// after-T9 hook reads the source inside the tick): the ordered per-system CRCs are identical, the parity CRC differs on
// exactly the locked ticks (no XOR, COMMANDS.md §7.1 step 6), the pacing is pinned to 30 ticks/s under the lock even in
// fast mode, and fast mode itself is wall-clock only.
void rate_lock_test(const game::GameData& data, const game::SessionSpec& spec) {
    constexpr uint64_t kTicks = 120, kFrom = 50, kTo = 80;
    auto run = [&](bool lock, CrcLog& out, bool& pacing_ok) {
        jobs::JobSystem js(jobs::JobSystemConfig{2});
        game::GameSession s(data, js, spec);
        CHECK(s.start());
        if (!s.started()) return;
        sim::SimHost* h = &s.host();
        if (lock) s.set_rate_lock_source([h] { return h->frame() >= kFrom && h->frame() < kTo; });
        s.set_fast_forward(true);
        pacing_ok = s.fast_forward() && s.host().pacing().rate() == sim::kFastModeRate;
        for (uint64_t i = 0; i < kTicks; ++i) {
            if (s.battle_choice_pending()) (void)s.choose_battle(galactic::BattleChoice::Fight);
            s.tick();
            const bool locked = lock && s.host().frame() >= kFrom && s.host().frame() < kTo;
            if (s.cinematic_rate_lock() != locked) pacing_ok = false;
            if (s.host().pacing().rate() != (locked ? sim::kCinematicRate : sim::kFastModeRate)) pacing_ok = false;
        }
        if (lock) CHECK(s.rate_lock_ticks() == kTo - kFrom);
        s.set_fast_forward(false);
        pacing_ok = pacing_ok && !s.fast_forward() && s.host().pacing().rate() == sim::kSpeedTable[sim::kDefaultSpeedIndex];
        out = s.host().crc_log();
    };
    CrcLog plain, locked;
    bool p0 = false, p1 = false;
    run(false, plain, p0);
    run(true, locked, p1);
    CHECK(p0 && p1);
    CHECK(plain.records.size() == locked.records.size() && plain.records.size() > kTo);
    size_t parity_diff = 0, combined_diff = 0;
    bool outside_same = true;
    for (size_t i = 0; i < plain.records.size() && i < locked.records.size(); ++i) {
        const CrcRecord& a = plain.records[i];
        const CrcRecord& b = locked.records[i];
        combined_diff += a.combined != b.combined || a.systems != b.systems ? 1u : 0u;
        const bool in = a.tick >= kFrom && a.tick < kTo;
        if (a.parity != b.parity) ++parity_diff;
        if (!in && a.parity != b.parity) outside_same = false;
    }
    std::printf("rate lock: %zu records, parity differs on %zu (locked %" PRIu64 "), ordered CRCs differ on %zu\n", plain.records.size(),
                parity_diff, kTo - kFrom, combined_diff);
    CHECK(combined_diff == 0 && parity_diff == kTo - kFrom && outside_same);
}

// The attract demo session: a space skirmish with no human (both sides AI controlled), player 0's fleet hop done at setup
// (no command), the AI-vs-AI battle fought, nothing recorded; a fresh session replays it bit-identically.
void attract_session_test(const game::GameData& data, const game::SessionSpec& base) {
    game::SessionSpec spec = base;
    spec.attract = true;
    spec.ask_battle_choice = false;
    auto run = [&](int32_t workers, CrcLog& out, uint64_t& fought, size_t& recorded) {
        jobs::JobSystem js(jobs::JobSystemConfig{workers});
        game::GameSession s(data, js, spec);
        commands::CommandRecording rec;
        std::vector<std::string> diag;
        CHECK(s.start(&diag, nullptr, &rec));
        for (const std::string& d : diag) std::printf("  attract diag: %s\n", d.c_str());
        if (!s.started()) return;
        if (const galactic::GalacticSim* g = s.galaxy())
            for (const galactic::PlayerState& p : g->players()) CHECK(!p.human);
        uint64_t t = 0;
        while (t < 6000 && !s.skirmish_over()) {
            CHECK(!s.battle_choice_pending());
            s.tick();
            ++t;
        }
        CHECK(s.skirmish_over());
        fought = s.battles() ? s.battles()->battles_fought() : 0;
        recorded = 0;
        for (const auto& b : rec.batches) recorded += b.commands.size();
        out = s.host().crc_log();
        std::printf("attract (%d workers): %" PRIu64 " ticks, %" PRIu64 " battle(s) fought, %zu commands recorded\n", workers, t, fought, recorded);
    };
    CrcLog a, b;
    uint64_t fa = 0, fb = 0;
    size_t ra = 0, rb = 0;
    run(2, a, fa, ra);
    run(0, b, fb, rb);
    CHECK(fa == 1 && fb == 1 && ra == 0 && rb == 0);
    const CrcDivergence d = first_divergence(a, b);
    std::printf("attract replay (0 vs 2 workers): %s\n", d.message.c_str());
    CHECK(!d.diverged && a.records.size() == b.records.size());
}

// EaW .rec attract entries: only the map path is read from the recording header ([port] scan); a synthetic buffer, then
// the real GameData\Recordings\Front_end_demo_attract.rec when ALAMO_EAW_DATA is set.
void attract_rec_test() {
    std::vector<uint8_t> buf = {0x52, 0x45, 0x43, 0x00, 0x01, 0x02, 0xff, 0x00};  // binary noise first
    const std::string decoy = "Data\\Art\\Maps\\Not_This.ted";                  // no ".\" prefix: skipped
    buf.insert(buf.end(), decoy.begin(), decoy.end());
    buf.push_back(0);
    const std::string path = ".\\Data\\Art\\Maps\\Front_end_demo_attract.TED";
    buf.insert(buf.end(), {0x3c, 0x00, 0x00, 0x00, 0x64, 0x2b});  // printable header bytes right before the path (as EaW's)
    buf.insert(buf.end(), path.begin(), path.end());
    buf.push_back(0);
    buf.insert(buf.end(), {0x10, 0x20, 0x00});
    CHECK(game::attract_rec_map(buf) == "Front_end_demo_attract");
    CHECK(game::is_attract_recording("Front_end_demo_attract.rec") && game::is_attract_recording("X.REC") &&
          !game::is_attract_recording("Demo_Attract_Kamino"));
    std::vector<uint8_t> none(buf.begin(), buf.begin() + 8);
    CHECK(game::attract_rec_map(none).empty());
    std::vector<uint8_t> late(9000, 0x01);  // the string past the scanned 8 KB is not found
    late.insert(late.end(), path.begin(), path.end());
    late.push_back(0);
    CHECK(game::attract_rec_map(late).empty());
    const char* eaw = std::getenv("ALAMO_EAW_DATA");
    if (!eaw || !*eaw) {
        std::printf("attract .rec: synthetic ok; ALAMO_EAW_DATA not set: real recording skipped\n");
        return;
    }
    std::ifstream f(std::string(eaw) + "\\Recordings\\Front_end_demo_attract.rec", std::ios::binary);
    if (!f) {
        std::printf("attract .rec: Front_end_demo_attract.rec not found under ALAMO_EAW_DATA: skipped\n");
        return;
    }
    const std::vector<uint8_t> rec((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const std::string m = game::attract_rec_map(rec);
    CHECK(m == "Front_end_demo_attract");
    std::printf("attract .rec: EaW Front_end_demo_attract.rec (%zu bytes) -> map %s\n", rec.size(), m.c_str());
}

// Playtest 3 t14: the demo camera's yaw (attract_mode.hpp demo_shot_yaw_deg), pure part. The first stand-in spun the camera
// at 4 degrees per second, so every hull was seen nose-on or tail-on (view within 30 degrees of its facing) for a third of
// the time, in stretches of 15 s; the side-view camera keeps the view near broadside.
void attract_camera_pure_test() {
    const float kNoseOn = 0.866f;  // cos 30 degrees
    const float kRad = 0.017453292519943295f;
    // Yaw convention: 0 looks north (+Y), clockwise from above; view = (sin yaw, cos yaw).
    CHECK(std::fabs(game::demo_view_alignment(0.0f, 0.0f, 1.0f) - 1.0f) < 1e-5f);   // looking along a northbound nose
    CHECK(std::fabs(game::demo_view_alignment(180.0f, 0.0f, 1.0f) - 1.0f) < 1e-5f);  // tail-on
    CHECK(game::demo_view_alignment(90.0f, 0.0f, 1.0f) < 1e-5f);                     // east view of a northbound hull: broadside
    CHECK(game::demo_view_alignment(33.0f, 0.0f, 0.0f) == 0.0f);                     // no horizontal facing
    CHECK(std::fabs(game::demo_wrap_deg(190.0f) + 170.0f) < 1e-4f && std::fabs(game::demo_wrap_deg(-190.0f) - 170.0f) < 1e-4f &&
          std::fabs(game::demo_wrap_deg(720.0f)) < 1e-4f);
    // The side yaw is perpendicular to every facing and picks the side nearest the current yaw.
    for (int deg = 0; deg < 360; deg += 15) {
        const float r = static_cast<float>(deg) * kRad;
        const float fx = std::cos(r), fy = std::sin(r);
        const float y = game::demo_side_yaw_deg(fx, fy, 0.0f);
        CHECK(game::demo_view_alignment(y, fx, fy) < 1e-4f);
        const float other = game::demo_wrap_deg(y + 180.0f);
        CHECK(std::fabs(game::demo_wrap_deg(y)) <= std::fabs(other) + 1e-3f);  // nearest to 0
    }
    CHECK(game::demo_side_yaw_deg(0.0f, 0.0f, 42.0f) == 42.0f);
    // First stand-in model, 360 s at 30 Hz from yaw 0: a northbound hull is nose-on / tail-on for a third of the time.
    {
        uint32_t on = 0, run = 0, longest = 0, n = 0;
        for (uint32_t i = 0; i < 360 * 30; ++i, ++n) {
            const float yaw = game::demo_wrap_deg(4.0f * static_cast<float>(i) / 30.0f);
            if (game::demo_view_alignment(yaw, 0.0f, 1.0f) > kNoseOn) {
                ++on;
                longest = std::max(longest, ++run);
            } else {
                run = 0;
            }
        }
        const double frac = static_cast<double>(on) / n;
        std::printf("attract camera (first stand-in model, 360 s spin): %.1f%% of the time nose-on / tail-on, longest stretch %.1f s\n",
                    frac * 100.0, static_cast<double>(longest) / 30.0);
        CHECK(frac > 0.30 && frac < 0.37 && longest > 14 * 30);
    }
    // Side-view camera: a hull flying a circle (turning 6 deg/s) for 600 s, shots every 8 s: never nose-on.
    {
        const game::DemoCameraTuning tune;
        float yaw = 0.0f, worst = 0.0f;
        uint32_t on = 0, n = 0;
        for (uint32_t i = 0; i < 600 * 30; ++i, ++n) {
            const float t = static_cast<float>(i) / 30.0f;
            const float hr = t * 6.0f * kRad;
            const float fx = std::cos(hr), fy = std::sin(hr);
            const float shot_t = std::fmod(t, static_cast<float>(tune.shot_seconds));
            if (shot_t < 1.0f / 30.0f) yaw = game::demo_side_yaw_deg(fx, fy, yaw);  // a cut
            yaw = game::demo_shot_yaw_deg(yaw, fx, fy, shot_t, 1.0f / 30.0f, tune);
            const float al = game::demo_view_alignment(yaw, fx, fy);
            worst = std::max(worst, al);
            if (al > kNoseOn) ++on;
        }
        std::printf("attract camera (side view, hull turning 6 deg/s, 600 s): %u of %u frames nose-on / tail-on, worst |cos| %.3f\n", on, n,
                    static_cast<double>(worst));
        CHECK(on == 0 && worst < 0.7f);
        // Deterministic: the same inputs give the same yaw.
        CHECK(game::demo_shot_yaw_deg(10.0f, 0.3f, -0.9f, 2.5f, 0.033f, tune) == game::demo_shot_yaw_deg(10.0f, 0.3f, -0.9f, 2.5f, 0.033f, tune));
        // The turn rate limit: a hull turn is followed at max_turn_deg_per_s.
        const float y0 = game::demo_side_yaw_deg(0.0f, 1.0f, 0.0f);
        const float y1 = game::demo_shot_yaw_deg(y0, 1.0f, 0.0f, 0.0f, 0.1f, tune);
        CHECK(std::fabs(game::demo_wrap_deg(y1 - y0)) <= tune.max_turn_deg_per_s * 0.1f + 1e-3f);
    }
}

// The same camera on the real demo session (ALAMO_FOC_DATA, Demo_Attract_Kamino as the game starts it): a virtual camera (30 Hz,
// shots of 8 s on the next combat unit in ascending id order) reads each tick's facings; the followed hull is not seen
// nose-on for long, and reading the session for the camera leaves its per-tick CRC log untouched (presentation never
// changes the sim).
void attract_camera_session_test() {
    const char* foc = std::getenv("ALAMO_FOC_DATA");
    if (!foc || !*foc) {
        std::printf("attract camera (demo session): ALAMO_FOC_DATA not set: skipped\n");
        return;
    }
    game::GameData data;
    std::string err;
    if (!game::load_game_data(foc, {}, data, &err)) {
        std::printf("attract camera (demo session): load failed: %s\n", err.c_str());
        CHECK(false);
        return;
    }
    game::SessionSpec spec;
    spec.kind = game::SessionKind::Skirmish;
    spec.attract = true;
    spec.seed = 1;
    spec.skirmish.human_faction = "Rebel";
    spec.skirmish.ai_faction = "Underworld";
    spec.skirmish_map = "Demo_Attract_Kamino";
    spec.battle_max_frames = 3000;
    uint32_t old_nose_on = 0;
    auto run = [&](bool camera, CrcLog& out, uint32_t& frames, uint32_t& nose_on, uint32_t& longest) {
        jobs::JobSystem js(jobs::JobSystemConfig{2});
        game::GameSession s(data, js, spec);
        commands::CommandRecording rec;
        std::vector<std::string> diag;
        CHECK(s.start(&diag, nullptr, &rec));
        if (!s.started()) return;
        const game::DemoCameraTuning tune;
        float yaw = 0.0f;
        uint32_t shot = 0, run_len = 0;
        objects::ObjectId target = 0;
        for (uint64_t t = 0; t < 1800 && !s.skirmish_over(); ++t) {
            s.tick();
            const sim::TacticalBattle* b = s.battle();
            const sim::TacticalMode* tm = s.host().modes().top_tactical();
            if (!camera || !b || !tm) continue;
            const objects::ObjectStore& st = tm->primary();
            const float shot_t = static_cast<float>(t % 240) / 30.0f;
            objects::ObjectHandle h = st.find(target);
            if (t % 240 == 0 || !st.alive(h)) {  // a cut to the next combat unit (ascending id, cycling)
                std::vector<objects::ObjectId> units;
                for (const objects::ObjectId id : b->movement().unit_ids())
                    if (b->combat().has_unit(id) && st.alive(st.find(id))) units.push_back(id);
                if (units.empty()) continue;
                std::sort(units.begin(), units.end());
                target = units[shot++ % units.size()];
                h = st.find(target);
                yaw = game::demo_side_yaw_deg(st.facing(h).x, st.facing(h).y, yaw);
            }
            yaw = game::demo_shot_yaw_deg(yaw, st.facing(h).x, st.facing(h).y, shot_t, 1.0f / 30.0f, tune);
            ++frames;
            // The first stand-in camera on the same shots: the yaw spun 4 degrees per second.
            if (game::demo_view_alignment(game::demo_wrap_deg(4.0f * static_cast<float>(t) / 30.0f), st.facing(h).x, st.facing(h).y) > 0.866f) ++old_nose_on;
            if (game::demo_view_alignment(yaw, st.facing(h).x, st.facing(h).y) > 0.866f) {
                ++nose_on;
                longest = std::max(longest, ++run_len);
            } else {
                run_len = 0;
            }
        }
        out = s.host().crc_log();
    };
    CrcLog with_cam, without;
    uint32_t f1 = 0, n1 = 0, l1 = 0, f2 = 0, n2 = 0, l2 = 0;
    old_nose_on = 0;
    run(true, with_cam, f1, n1, l1);
    run(false, without, f2, n2, l2);
    std::printf("attract camera on the demo session: %u frames followed, %u nose-on / tail-on (the first stand-in's 4 deg/s spin: %u), longest stretch %u frames\n",
                f1, n1, old_nose_on, l1);
    CHECK(f1 > 0 && n1 * 10 <= f1);
    const CrcDivergence d = first_divergence(with_cam, without);
    CHECK(!d.diverged && with_cam.records.size() == without.records.size() && !without.records.empty());
}

// The attract controller on the synthetic GameConstants (Yes / 3600 s / two maps / 1200, 600 s).
void attract_controller_test(const game::GameData& data) {
    const game::AttractConfig cfg = game::load_attract_config(data.db.constants());
    CHECK(cfg.enabled && cfg.start_timeout_s == 3600.0 && cfg.maps.size() == 2 && cfg.cycle_delay_s.size() == 2);
    if (cfg.maps.size() == 2) CHECK(cfg.maps[0] == "Demo_Attract_Kamino" && cfg.maps[1] == "GS_SPACE");
    // MAIN_MENU_BACKGROUND.md §3: started when the menu opens, never stopped by input, idle pause, idle-gated cycling.
    using R = game::AttractController::Request;
    using D = game::AttractController::Dialogs;
    game::AttractController c(cfg);
    CHECK(c.update(0.0, false, D::MainMenu) == R::None && !c.armed());  // nothing before the menu opens
    CHECK(c.menu_opened(10.0) == R::Start && c.running() && c.map_index() == 1 && c.map() == "GS_SPACE");  // index advances first
    CHECK(c.cycle_delay() == 600.0);
    CHECK(c.update(100.0, true, D::MainMenu) == R::None && c.running());   // input never stops it
    CHECK(c.menu_opened(200.0) == R::None && c.running());                 // re-opened: the cycle check only
    CHECK(c.update(700.0, false, D::MainMenu) == R::Next && c.map() == "Demo_Attract_Kamino" && c.cycle_delay() == 1200.0);
    CHECK(c.update(1950.0, true, D::Other) == R::None);    // delay passed, but the user is busy
    CHECK(c.update(1960.0, false, D::Other) == R::None);   // another dialog: 20 s idle needed
    CHECK(c.update(1971.0, false, D::Other) == R::Next && c.map_index() == 1);
    CHECK(c.update(1971.0 + 600.0 + 3601.0, false, D::MainMenu) == R::None && c.idle_paused());  // idle pause: no cycle
    CHECK(c.update(6200.0, true, D::MainMenu) == R::None && !c.idle_paused());                   // input resumes
    CHECK(c.update(6209.0, false, D::MainMenu) == R::Next);
    CHECK(c.update(9000.0, false, D::MainMenu, false) == R::None);  // window inactive: no cycle under a dialog
    CHECK(c.update(9001.0, false, D::None) == R::Next);             // no dialog: due at once
    c.stop();
    CHECK(!c.running() && !c.armed() && c.update(20000.0, false, D::None) == R::None);
    const size_t before = c.map_index();
    CHECK(c.menu_opened(20001.0) == R::Start && c.map_index() == (before + 1) % 2);
    game::AttractConfig short_list = cfg;
    short_list.cycle_delay_s = {1200.0};
    game::AttractController sl(short_list);
    CHECK(sl.menu_opened(0.0) == R::Start && sl.map_index() == 1 && sl.cycle_delay() == 120.0);  // past the list: 120 s
    game::AttractController off(game::AttractConfig{});
    CHECK(off.menu_opened(0.0) == R::None && !off.running());
    game::AttractConfig one;
    one.maps = {"Demo_Attract_Kamino"};
    game::AttractController forced(one);  // --attract: started although the data's mode is off
    CHECK(forced.menu_opened(0.0) == R::None && forced.menu_opened(0.0, true) == R::Start && forced.map_index() == 0);
    std::printf("attract controller: %" PRIu64 " starts\n", c.starts());
}

#ifdef ALAMO_GAME_HAVE_CONTROLS
// docs/PHASE4.md "Camera and controls" on the host's tables: after sync_control_bindings the default hotkey table and the
// bindings satisfy every rule; a W/A/S/D binding (here Ctrl+S on Stop) and a missing queue modifier are reported.
void controls_test() {
    ui::HotkeyTable hk;
    ui::Bindings b;
    ui::CameraSettings cam;
    game::sync_control_bindings(hk, cam, b);
    const std::vector<std::string> ok = game::check_control_decisions(hk, b);
    for (const std::string& v : ok) std::printf("  controls: %s\n", v.c_str());
    CHECK(ok.empty());
    // Movement on W A S D only (no arrows), the arrows yaw / pitch the free camera, shift queues.
    CHECK(b.binding(ui::Action::ScrollUp, 0).key == 'W' && b.binding(ui::Action::ScrollUp, 1).key == 0);
    CHECK(b.binding(ui::Action::RotateLeft, 0).key == ui::vk::kLeft && b.binding(ui::Action::PitchUp, 0).key == ui::vk::kUp);
    CHECK(b.binding(ui::Action::Pause, 0).key == ui::vk::kPause && b.queue_mods() == ui::kModShift);
    hk.bind(ui::hk::kStop, {'S', ui::HotkeyMod::Ctrl});
    cam.queue_mods = 0;
    game::sync_control_bindings(hk, cam, b);
    const std::vector<std::string> bad = game::check_control_decisions(hk, b);
    for (const std::string& v : bad) std::printf("  controls (expected): %s\n", v.c_str());
    CHECK(bad.size() == 3);  // hotkey Stop, binding Stop, no queue modifier
    std::printf("controls: decisions hold on the synced defaults, %zu violation(s) reported for the broken tables\n", bad.size());
}
#endif
#ifdef ALAMO_GAME_HAVE_CONTROLS
// Playtest 2: the Empire Heart of the Maw galaxy starts with Piett, Tarkin, Veers, Boba Fett and the Emperor in the hero
// roster (base screenshots). Real data (ALAMO_FOC_DATA); skipped when unset.
void hero_roster_real_test() {
    const char* foc = std::getenv("ALAMO_FOC_DATA");
    if (!foc || !*foc) {
        std::printf("hero roster (real data): ALAMO_FOC_DATA not set: skipped\n");
        return;
    }
    game::GameData data;
    std::string err;
    if (!game::load_game_data(foc, {}, data, &err)) {
        std::printf("hero roster (real data): load failed: %s\n", err.c_str());
        CHECK(false);
        return;
    }
    game::SessionSpec spec;
    spec.kind = game::SessionKind::Campaign;
    spec.campaign = "Sandbox_Heart_Of_The_Maw_Empire";
    spec.faction = "Empire";
    spec.galactic_ai = false;
    spec.story = true;
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    game::GameSession s(data, js, spec);
    CHECK(s.start(nullptr, nullptr, nullptr));
    const galactic::GalacticSim* g = s.galaxy();
    CHECK(g != nullptr);
    if (!g) return;
    const auto icon = [&data](uint32_t t) {
        return t < data.types.size() ? std::string(data.types.get_string(static_cast<objects::TypeIndex>(t), "Icon_Name")) : std::string();
    };
    // GALAXY_MAP_PRESENTATION §7: the story (Story_Sandbox_12U_Empire) spawns Tarkin, the Emperor, Boba Fett, Veers
    // (SPAWN_HERO) and the Accuser / Piett (UNIQUE_UNIT) at Coruscant, which this map lacks: the fallback = the lowest-index
    // planet the Empire owns = Corellia (base 24 / 26.jpg). They fill the hero roster.
    std::vector<ui::HeroEntry> heroes;
    for (int i = 0; i < 300 && heroes.size() < 5; ++i) {
        s.tick();
        heroes = game::galactic_hero_entries(*g, s.local_player(), icon, {}, 11);
    }
    PlanetIndex corellia = kInvalidPlanet;
    for (PlanetIndex i = 0; i < g->planets().size(); ++i)
        if (i < g->map().planets.name.size() && g->map().planets.name[i] == "Corellia") corellia = i;
    CHECK(corellia != kInvalidPlanet);
    if (corellia == kInvalidPlanet) return;
    size_t at_corellia = 0;  // named heroes of the player in Corellia's orbit fleets or on its surface
    const objects::ObjectStore& st = g->store();
    auto count = [&](objects::ObjectHandle h) {
        if (st.alive(h) && st.owner(h) == s.local_player() && game::is_named_hero(*g, static_cast<uint32_t>(st.type(h)))) ++at_corellia;
    };
    for (const uint32_t fi : g->fleets_at(corellia, s.local_player()))
        for (const objects::ObjectHandle m : g->fleets()[fi].contents) count(m);
    for (const objects::ObjectHandle m : g->planets()[corellia].surface) count(m);
    std::printf("hero roster (real data): %zu entries, %zu named heroes at Corellia:", heroes.size(), at_corellia);
    for (const ui::HeroEntry& h : heroes) std::printf(" %s", h.icon.c_str());
    std::printf("\n");
    CHECK(heroes.size() == 5 && at_corellia == 5);
    // FIXPASS_GALACTIC §2.4, capture C3: the bar's order right to left, keyed by the bar type's ranking and upper-case CRC.
    std::vector<ui::HeroEntry> ranked = game::galactic_hero_entries(*g, s.local_player(), icon, {}, 11, [&data](uint32_t t) {
        return t < data.types.size() ? data.types.get_int(static_cast<objects::TypeIndex>(t), "Ranking_In_Category") : 0;
    });
    ui::sort_hero_entries(ranked);
    std::string order;
    for (const ui::HeroEntry& h : ranked) order += h.type_name + " ";
    std::printf("hero bar order (real data): %s\n", order.c_str());
    CHECK(order == "Emperor_Palpatine Boba_Fett General_Veers Grand_Moff_Tarkin Accuser_Star_Destroyer ");
    // The rule on the other factions' companies: a named transport wins, else the first member; plain types stay.
    const galactic::GalacticRules& rules = g->rules();
    const auto bar_of = [&rules](std::string_view company) {
        for (uint32_t t = 0; t < rules.types.size(); ++t)
            if (rules.types[t].name == company) return rules.types[game::hero_bar_type(rules, t)].name;
        return std::string();
    };
    const std::pair<const char*, const char*> bars[] = {{"Han_Solo_Team", "Millennium_Falcon"}, {"Droids_Team", "Droid_R2D2"},
                                                        {"Boba_Fett_Team", "Boba_Fett"},       {"Tyber_Zann_Team", "The_Peacebringer"},
                                                        {"IG88_Team", "IG-2000"},              {"Bossk_Team", "Houndstooth"},
                                                        {"Silri_Team", "Silri"},               {"Accuser_Star_Destroyer", "Accuser_Star_Destroyer"}};
    for (const auto& [company, bar] : bars) {
        const std::string got = bar_of(company);
        if (got != bar) std::printf("hero_bar_type(%s) = '%s', expected %s\n", company, got.c_str(), bar);
        CHECK(got == bar);
    }
}

// HUD_GAPS feeds (hud_feeds.hpp): day pips (§4), order buttons (§5), hard-point type names / placement (§1), galactic
// hero roster (§3.2) on a running session galaxy.
void hud_feeds_test(const game::GameData& data, const game::SessionSpec& spec) {
    // §4: (frame - cycle start) / cycle frames, clamped; no cycle length hides the pips.
    CHECK(game::day_cycle_fraction(0, 0, 1350) == 0.0f && game::day_cycle_fraction(675, 0, 1350) == 0.5f);
    CHECK(game::day_cycle_fraction(1350 + 135, 1350, 1350) == 0.1f && game::day_cycle_fraction(5000, 0, 1350) == 1.0f);
    CHECK(game::day_cycle_fraction(10, 0, 0) < 0.0f);
    // §5: own units only; attack / move independent; controls locked disables everything.
    const game::OrderUnitState own_gun{0, true, false}, own_mover{0, false, true}, enemy_both{1, true, true};
    const game::OrderUnitState a[] = {own_gun, enemy_both};
    const ui::HudOrderButtons b1 = game::order_buttons(a, 0, false);
    CHECK(b1.can_attack && !b1.can_move && !b1.controls_locked);
    CHECK(ui::order_button_enabled(0, b1) && !ui::order_button_enabled(2, b1) && ui::order_button_enabled(1, b1));
    const game::OrderUnitState m[] = {own_mover};
    const ui::HudOrderButtons b2 = game::order_buttons(m, 0, false);
    CHECK(!b2.can_attack && b2.can_move && !ui::order_button_enabled(0, b2) && ui::order_button_enabled(2, b2));
    const ui::HudOrderButtons b3 = game::order_buttons(a, 0, true);
    bool any = false;
    for (uint32_t i = 0; i < 6; ++i) any = any || ui::order_button_enabled(i, b3);
    CHECK(b3.controls_locked && !any);
    const game::OrderUnitState e[] = {enemy_both};
    const ui::HudOrderButtons b4 = game::order_buttons(e, 0, false);
    CHECK(!b4.can_attack && !b4.can_move);
    // §1: HardPointType names (reticle table keys) and the yaw-rotated attachment point.
    CHECK(std::string(game::hardpoint_type_name(0)) == "HARD_POINT_ENGINE" && std::string(game::hardpoint_type_name(5)) == "HARD_POINT_WEAPON_LASER");
    CHECK(std::string(game::hardpoint_type_name(12)) == "HARD_POINT_DUMMY_ART" && std::string(game::hardpoint_type_name(13)).empty());
    const ui::Vec3 w = game::hardpoint_world({100, 200, 5}, {0, 2, 0}, {10, 3, 1});  // facing +y: forward = +y, left = -x
    CHECK(std::fabs(w.x - 97.0f) < 1e-4f && std::fabs(w.y - 210.0f) < 1e-4f && std::fabs(w.z - 6.0f) < 1e-4f);
    // §3.2 / §4 on a session galaxy: the roster holds only named heroes with an icon (max 11), the pips advance.
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    game::GameSession s(data, js, spec);
    CHECK(s.start(nullptr, nullptr, nullptr));
    const galactic::GalacticSim* g = s.galaxy();
    CHECK(g != nullptr);
    if (!g) return;
    const float f0 = game::galactic_cycle_fraction(*g);
    for (int i = 0; i < 30 && s.galaxy(); ++i) s.tick();
    if (!s.galaxy()) return;
    const float f1 = game::galactic_cycle_fraction(*s.galaxy());
    CHECK(f0 >= 0.0f && f1 >= 0.0f && f1 <= 1.0f && (f1 > f0 || f1 < f0));
    size_t named = 0;
    const auto icon = [](uint32_t) { return std::string("i_hero.tga"); };
    const std::vector<ui::HeroEntry> heroes = game::galactic_hero_entries(*s.galaxy(), s.local_player(), icon, {}, 11);
    for (const ui::HeroEntry& h : heroes) named += h.icon == "i_hero.tga" ? 1u : 0u;
    CHECK(heroes.size() <= 11 && named == heroes.size());
    const std::vector<ui::HeroEntry> none = game::galactic_hero_entries(*s.galaxy(), s.local_player(), [](uint32_t) { return std::string(); }, {}, 11);
    CHECK(none.empty());
    std::printf("hud feeds: cycle %.4f -> %.4f, %zu galactic hero entries\n", static_cast<double>(f0), static_cast<double>(f1), heroes.size());
}
#endif
// --- end OI host ---

// ---- N1 / t09: saved games (save_games.hpp) and GameSession save / load (game_session.hpp) ----
CrcLog records_from(const CrcLog& log, uint64_t tick) {
    CrcLog out;
    out.systems = log.systems;
    out.parity_parts = log.parity_parts;
    for (const CrcRecord& r : log.records)
        if (r.tick >= tick) out.records.push_back(r);
    return out;
}

// Names, descriptions, the listing order and delete (no session).
void save_files_test() {
    CHECK(game::sanitize_save_name("  My: save?  ") == "My_ save_");
    CHECK(game::sanitize_save_name("a/b\\c*d") == "a_b_c_d");
    CHECK(game::sanitize_save_name("...").empty() && game::sanitize_save_name("").empty());
    CHECK(game::sanitize_save_name("CON") == "CON_" && game::sanitize_save_name("com1") == "com1_");
    CHECK(game::sanitize_save_name(std::string(100, 'x')).size() == game::kMaxSaveName);
    CHECK(game::save_path("d", "").empty());
    game::SaveMeta m;
    m.name = "Week 3";
    m.kind = "campaign";
    m.campaign = "Sandbox_X";
    m.title = "Heart of the Maw";
    m.faction = "Empire";
    m.difficulty = 2;
    m.tick = 12345;
    m.week = 3;
    m.game_seconds = 411;
    m.saved_at = 1790000000;
    const std::string desc = game::encode_description(m);
    game::SaveMeta back;
    CHECK(game::decode_description(desc, back));
    CHECK(back.name == m.name && back.kind == m.kind && back.campaign == m.campaign && back.title == m.title && back.faction == m.faction &&
          back.difficulty == 2 && back.tick == 12345 && back.week == 3 && back.game_seconds == 411 && back.saved_at == m.saved_at);
    const std::string label = game::save_label(m);
    CHECK(label.rfind("Week 3  -  Heart of the Maw (Empire), week 3, ", 0) == 0);
    CHECK(desc.substr(0, desc.find('\n')) == label);  // line 1 = what other tools show
    game::SaveMeta sk;
    sk.name = "s";
    sk.kind = "skirmish";
    sk.campaign = "_MP_SPACE_KUAT";
    sk.game_seconds = 125;
    CHECK(game::save_label(sk) == "s  -  Skirmish _MP_SPACE_KUAT, 2:05");
    CHECK(!game::decode_description("just a line", back));

    // Listing: newest first, then by name (case-insensitive); unreadable files listed; other extensions ignored.
    const fs::path dir = fs::temp_directory_path() / "alamo_game_save_files_test";
    std::error_code ec;
    remove_all_long(dir, ec);
    const std::string d = path_to_utf8(dir);
    auto write = [&](const std::string& name, int64_t at, uint64_t tick) {
        SaveFile f;
        f.tick = tick;
        game::SaveMeta mm;
        mm.name = name;
        mm.kind = "campaign";
        mm.campaign = "C";
        mm.saved_at = at;
        mm.tick = tick;
        f.description = game::encode_description(mm);
        ByteWriter w;
        w.u32(1).u64(tick);
        f.add("frame", 1, w);
        std::string err;
        CHECK(game::write_save_file(f, game::save_path(d, name), &err));
        if (!err.empty()) std::printf("  write %s: %s\n", name.c_str(), err.c_str());
    };
    write("beta", 2000, 20);
    write("Alpha", 2000, 30);
    write("old", 1000, 10);
    write("new", 3000, 40);
    write_file(dir / "broken.alsv", "not a save");
    write_file(dir / "notes.txt", "ignored");
    const std::vector<game::SaveEntry> list = game::list_saves(d);
    std::string order;
    for (const game::SaveEntry& e : list) order += e.name + (e.readable ? "" : "!") + " ";
    std::printf("save listing: %s\n", order.c_str());
    CHECK(list.size() == 5);
    if (list.size() == 5) {
        CHECK(list[0].name == "broken" && !list[0].readable && !list[0].error.empty());  // no saved_at: its write time (now)
        CHECK(list[1].name == "new" && list[2].name == "Alpha" && list[3].name == "beta" && list[4].name == "old");
        CHECK(list[2].tick == 30 && list[2].meta.tick == 30 && list[1].readable);
    }
    uint64_t tick = 0;
    std::string text, err;
    CHECK(game::read_save_header(game::save_path(d, "old"), tick, text, &err) && tick == 10 && text.rfind("old  -  C", 0) == 0);
    CHECK(game::save_exists(d, "new") && game::save_exists(d, "NEW") && !game::save_exists(d, "missing"));
    CHECK(game::delete_save(d, "new", &err) && !game::save_exists(d, "new") && game::list_saves(d).size() == 4);
    CHECK(!game::delete_save(d, "missing", &err) && !err.empty());
    CHECK(game::list_saves(path_to_utf8(dir / "none")).empty());
    remove_all_long(dir, ec);
}

// A skirmish saved through the menus' path (save_game) on the galactic map, loaded into a new session: the load continues
// the saving run bit for bit; another seed / fleet / a non-game save / a corrupt file / a system without a save are
// refused with a reason.
void save_load_test(const game::GameData& data, game::SessionSpec spec) {
    spec.animation = false;  // N1: the anim chunk is n2-tai / n2-host's (the FoC save check covers the full session)
    const fs::path dir = fs::temp_directory_path() / "alamo_game_save_load_test";
    std::error_code ec;
    remove_all_long(dir, ec);
    const std::string d = path_to_utf8(dir);
    jobs::JobSystem js(jobs::JobSystemConfig{2});
    commands::CommandRecording rec;
    std::string path, err;
    uint64_t save_tick = 0, after = 0, battle_tick = 0, battle_after = 0;
    std::string battle_path;
    CrcLog log_a, log_battle;
    {
        game::GameSession a(data, js, spec);
        std::vector<std::string> diag;
        CHECK(a.start(&diag, nullptr, &rec));
        if (!a.started()) return;
        for (int i = 0; i < 20; ++i) a.tick();
        const std::string why = a.save_blocker();
        std::printf("save / load: blocker at tick %" PRIu64 ": '%s'\n", a.host().frame(), why.c_str());
        CHECK(why.empty() && a.check_save().empty());
        const game::SaveMeta meta = a.save_meta("x", 1);
        CHECK(meta.kind == "skirmish" && meta.campaign == "GS_SPACE" && meta.faction == "Rebel" && meta.tick == a.host().frame() && meta.week == 0);
        CHECK(meta.game_seconds == 0);  // no battle yet (N1f t06)
        CHECK(game::save_game(a, d, "Skirmish: test", "", game::unix_now(), &path, &err));
        if (!err.empty()) std::printf("  save: %s\n", err.c_str());
        save_tick = a.host().frame();
        CHECK(game::save_exists(d, "Skirmish_ test") && path == game::save_path(d, "Skirmish_ test"));
        // On to the battle (the choice answered 10 ticks after it is asked) and 60 battle ticks.
        uint64_t asked = 0, t = 0;
        while (t < 4000 && !a.in_battle()) {
            if (a.battle_choice_pending()) {
                if (asked == 0) asked = t;
                if (t >= asked + 10) a.choose_battle(galactic::BattleChoice::Fight);
            }
            a.tick();
            ++t;
        }
        CHECK(a.in_battle());
        for (int i = 0; i < 60; ++i) a.tick();
        // N1 stage 2: a save DURING the battle (the menus' path), then 120 more battle ticks.
        const std::string in_battle = a.save_blocker().empty() ? a.check_save() : a.save_blocker();
        std::printf("save / load: in battle at tick %" PRIu64 ": %s\n", a.host().frame(),
                    in_battle.empty() ? "saveable (stage 2)" : in_battle.c_str());
        CHECK(in_battle.empty() && a.in_battle());
        // N1f t06: a skirmish's save label shows the battle clock, not the galactic one (which ran through the fleet's hop).
        const game::SaveMeta battle_meta = a.save_meta("x", 0);
        const uint64_t battle_seconds = a.battle()->frame() / 30u, galactic_seconds = static_cast<uint64_t>(a.galaxy()->frame()) / 30u;
        std::printf("save / load: skirmish clock: battle %" PRIu64 " s, galaxy %" PRIu64 " s, label %s\n", battle_seconds, galactic_seconds,
                    game::save_label(battle_meta).c_str());
        CHECK(battle_meta.game_seconds == battle_seconds && battle_seconds < galactic_seconds);
        CHECK(game::save_game(a, d, "In battle", "", game::unix_now(), &battle_path, &err));
        battle_tick = a.host().frame();
        for (int i = 0; i < 120; ++i) a.tick();
        after = a.host().frame() - save_tick;
        battle_after = a.host().frame() - battle_tick;
        log_a = records_from(a.host().crc_log(), save_tick);
        log_battle = records_from(a.host().crc_log(), battle_tick);
    }
    const std::vector<game::SaveEntry> list = game::list_saves(d);
    CHECK(list.size() == 2);
    for (const game::SaveEntry& e : list)
        CHECK(e.readable && e.meta.kind == "skirmish" && e.tick == (e.name == "In battle" ? battle_tick : save_tick));
    SaveFile f;
    CHECK(SaveFile::read_file(path, f, &err));
    game::SessionSpec back;
    CHECK(game::GameSession::spec_from_save(f, back, &err));
    CHECK(back.kind == game::SessionKind::Skirmish && back.skirmish_map == "GS_SPACE" && back.seed == spec.seed &&
          back.skirmish.units_human == spec.skirmish.units_human && back.skirmish.units_ai == spec.skirmish.units_ai &&
          back.skirmish.ground_human == spec.skirmish.ground_human && back.ask_battle_choice && !back.animation &&
          back.battle_max_frames == spec.battle_max_frames);
    back.skirmish_map_data = spec.skirmish_map_data;  // in-memory test map (not saved)
    // B: loaded, replaying the saving run's own stream (tick-0 recording) from the save.
    {
        game::GameSession b(data, js, back);
        CHECK(b.start_from_save(f, &err, &rec));
        if (!err.empty()) std::printf("  load: %s\n", err.c_str());
        CHECK(b.loaded() && b.host().frame() == save_tick && b.load_report().ok());
        for (uint64_t i = 0; i < after; ++i) b.tick();
        const CrcDivergence dv = first_divergence(log_a, b.host().crc_log());
        std::printf("save / load: saved at %" PRIu64 ", +%" PRIu64 " ticks: loaded + replay vs original: %zu vs %zu records: %s\n", save_tick,
                    after, b.host().crc_log().records.size(), log_a.records.size(), dv.message.c_str());
        CHECK(!dv.diverged && b.host().crc_log().records.size() == log_a.records.size() && !log_a.records.empty());
        CHECK(!b.save_blocker().empty());  // a replay is never saved
    }
    // C: loaded live (records on from the save), answering the battle choice the same way.
    {
        game::GameSession c(data, js, back);
        commands::CommandRecording rc;
        CHECK(c.start_from_save(f, &err, nullptr, &rc));
        CHECK(c.loaded() && c.save_blocker().empty());
        CHECK(c.check_save().empty());  // the loaded session saves again
        c.tick();
        CHECK(rc.from_save.has_value() && *rc.from_save == c.load_point());
    }
    // N1 stage 2: the battle save loads into a rebuilt galaxy (SimHost::load's two passes push the saved battle), then
    // continues identically - replaying the original stream (D) and live with nothing more to answer (E).
    {
        SaveFile fb;
        game::SessionSpec bs;
        CHECK(SaveFile::read_file(battle_path, fb, &err) && game::GameSession::spec_from_save(fb, bs, &err));
        bs.skirmish_map_data = spec.skirmish_map_data;
        for (const bool replay : {true, false}) {
            game::GameSession b(data, js, bs);
            const bool ok = b.start_from_save(fb, &err, replay ? &rec : nullptr);
            if (!ok) std::printf("  battle load: %s\n", err.c_str());
            CHECK(ok && b.loaded() && b.host().frame() == battle_tick && b.in_battle());
            if (!ok) continue;
            for (uint64_t i = 0; i < battle_after; ++i) b.tick();
            const CrcDivergence dv = first_divergence(log_battle, b.host().crc_log());
            std::printf("save / load: battle save at %" PRIu64 " (%zu chunks), +%" PRIu64 " ticks: %s vs original: %zu vs %zu records: %s\n",
                        battle_tick, fb.chunks().size(), battle_after, replay ? "loaded + replay" : "loaded live",
                        b.host().crc_log().records.size(), log_battle.records.size(), dv.message.c_str());
            CHECK(!dv.diverged && b.host().crc_log().records.size() == log_battle.records.size() && !log_battle.records.empty());
        }
    }
    // Refusals: another seed / fleet (SessionDescriptor::mismatch), a non-game save, a corrupt file, a pending system.
    auto refused = [&](game::SessionSpec s, const char* what) {
        game::GameSession r(data, js, std::move(s));
        std::string e;
        const bool ok = r.start_from_save(f, &e);
        std::printf("save / load: %s: %s\n", what, ok ? "LOADED" : e.c_str());
        CHECK(!ok && !e.empty());
    };
    game::SessionSpec other = back;
    other.seed = back.seed + 1;
    refused(other, "other seed");
    other = back;
    other.skirmish.units_ai = {"GS_Frigate"};
    refused(other, "other AI fleet");
    {
        SaveFile foreign;
        sim::SessionDescriptor sd;
        sd.kind = "galactic";
        ByteWriter w;
        sd.write(w);
        foreign.add("session", sim::SessionDescriptor::kVersion, w);
        game::SessionSpec s;
        CHECK(!game::GameSession::spec_from_save(foreign, s, &err) && err.find("galactic") != std::string::npos);
        CHECK(!game::GameSession::spec_from_save(SaveFile{}, s, &err));
    }
    write_file(dir / "corrupt.alsv", "ALSV but not really");
    CHECK(game::load_game(data, js, game::save_path(d, "corrupt"), &err) == nullptr && !err.empty());
    std::printf("save / load: corrupt file: %s\n", err.c_str());
    {
        game::GameSession p(data, js, spec);
        CHECK(p.start(nullptr, nullptr, nullptr));
        sim::SaveParticipant part;
        part.name = "pending_test";
        part.write = [](ByteWriter&) {};  // a system whose save has not landed (write_if_saveable writes nothing)
        part.read = [](ByteReader&) { return false; };
        p.host().add_save_participant(std::move(part));
        p.tick();
        const std::string why = p.check_save();
        std::printf("save / load: pending system: %s\n", why.c_str());
        CHECK(why.rfind("pending_test: pending", 0) == 0);
        CHECK(!game::save_game(p, d, "pending", "", 1, nullptr, &err) && !game::save_exists(d, "pending"));
    }
    remove_all_long(dir, ec);
}

}  // namespace

// L1/S4 wave 1: UI transitions -> story notifications (galactic view), one-shot events, the command codec.
static void story_ui_test() {
    using E = sim::StoryUiEvent;
    game::StoryUiFeed f;
    auto kinds = [](const std::vector<sim::StoryUiNotification>& v) {
        std::vector<E> k;
        for (const auto& n : v) k.push_back(n.kind);
        return k;
    };
    game::StoryUiView v;
    f.observe(v);
    CHECK(f.take().empty());  // nothing selected, nothing zoomed
    v.selection = {12, 7, 0, 7};
    f.observe(v);
    auto out = f.take();
    CHECK(out.size() == 2 && out[0].kind == E::SelectUnit && out[0].object == 7 && out[1].object == 12 && out[0].player == -1);
    f.observe(v);
    CHECK(f.take().empty());  // the same selection: no repeat
    v.selection = {12, 30};
    f.observe(v);
    out = f.take();
    CHECK(out.size() == 1 && out[0].object == 30);  // the new member only
    v.planet = 100;
    f.observe(v);
    out = f.take();
    CHECK(kinds(out) == std::vector<E>{E::SelectPlanet} && out[0].object == 100);
    v.zoomed = true;  // zoom planet unknown -> the selected planet
    f.observe(v);
    out = f.take();
    CHECK(kinds(out) == std::vector<E>{E::ZoomIntoPlanet} && out[0].object == 100);
    f.observe(v);
    CHECK(f.take().empty());
    v.zoom_planet = 101;  // re-targeted while zoomed: out of the old one, into the new one
    f.observe(v);
    out = f.take();
    CHECK(kinds(out) == (std::vector<E>{E::ZoomOutPlanet, E::ZoomIntoPlanet}) && out[0].object == 100 && out[1].object == 101);
    v.zoomed = false;
    f.observe(v);
    out = f.take();
    CHECK(kinds(out) == std::vector<E>{E::ZoomOutPlanet} && out[0].object == 101);
    v.corruption_planet = 100;
    f.observe(v);
    out = f.take();
    CHECK(kinds(out) == std::vector<E>{E::OpenCorruption} && out[0].object == 100);
    f.observe(v);
    CHECK(f.take().empty());  // still open: once
    v.planet = 0;
    v.corruption_planet = 0;
    f.observe(v);
    CHECK(f.take().empty());  // deselect / close raise nothing
    f.click_gui("b_Create_00");
    f.click_gui("");
    f.speech_done("UW_ACT01_G01_01");
    f.movie_done("Empire_Win_Deathstar");
    f.generic("battle_end_closed");
    f.mission_lost("Story_Plots_Empire_ActI_M02_LAND.xml");
    out = f.take();
    CHECK(kinds(out) == (std::vector<E>{E::ClickGui, E::SpeechDone, E::MovieDone, E::Generic, E::MissionLost}));
    CHECK(out.size() == 5 && out[0].name == "b_Create_00" && out[4].name == "Story_Plots_Empire_ActI_M02_LAND.xml");
    CHECK(f.raised(E::SelectUnit) == 3 && f.raised(E::ZoomOutPlanet) == 2 && f.raised_total() == 14);
    f.reset();
    v = {};
    v.selection = {12};
    f.observe(v);
    CHECK(f.take().size() == 1);  // after reset: compared with an empty view again
    // Command codec: kind, object and name survive; other events / bad payloads are refused.
    sim::StoryUiNotification n;
    n.kind = E::OpenCorruption;
    n.player = 3;
    n.object = 0x01020304;
    n.name = "Kuat";
    const commands::EventData ev = game::story_ui_event(n);
    sim::StoryUiNotification back;
    CHECK(game::read_story_ui_event(ev, back));
    CHECK(back.kind == E::OpenCorruption && back.object == 0x01020304 && back.name == "Kuat" && back.player == -1);
    n.name.assign(400, 'x');
    CHECK(game::read_story_ui_event(game::story_ui_event(n), back) && back.name.size() == game::kStoryUiMaxName);
    CHECK(!game::read_story_ui_event(game::tactical_auto_resolve_event(), back));
    const uint8_t bad[5] = {static_cast<uint8_t>(E::Count), 0, 0, 0, 0};
    CHECK(!game::read_story_ui_event(commands::ev::port_extension(game::kStoryUiSubtype, std::span<const uint8_t>(bad, 5)), back));
    CHECK(!back.tactical);  // the old payload layout (bit 7 clear) decodes as galactic
    // Tactical (battle screen): the first battle observation only records (galactic ids carried over the push); then the
    // newly selected battle ids, and the clicks / speech ends raised during the battle carry `tactical`.
    f.reset();
    (void)f.take();
    f.set_battle(true);
    f.observe_battle({5, 9});
    CHECK(f.take().empty());
    f.observe_battle({9, 0, 21, 21});
    out = f.take();
    CHECK(out.size() == 1 && out[0].kind == E::SelectUnit && out[0].object == 21 && out[0].tactical);
    f.click_gui("b_reinforcement");
    f.speech_done("TUT_M05_LAND_01");
    f.movie_done("Any");
    out = f.take();
    CHECK(kinds(out) == (std::vector<E>{E::ClickGui, E::SpeechDone, E::MovieDone}) && out[0].tactical && out[1].tactical &&
          !out[2].tactical && out[0].name == "b_reinforcement");
    f.set_battle(false);
    f.speech_done("X");
    f.set_battle(true);
    f.observe_battle({21});  // a new battle: seeded again
    out = f.take();
    CHECK(out.size() == 1 && !out[0].tactical);
    n.kind = E::SelectUnit;
    n.tactical = true;
    n.object = 77;
    n.name.clear();
    const commands::EventData tev = game::story_ui_event(n);
    CHECK(game::read_story_ui_event(tev, back) && back.kind == E::SelectUnit && back.tactical && back.object == 77);
    CHECK(tev.bytes_view("data").size() == 5 && tev.bytes_view("data")[0] == (game::kStoryUiTacticalBit | 0u));
    // Story reward media: START_MOVIE Param1, MULTIMEDIA Param8 (trimmed); other rewards / empty params: nothing.
    std::vector<game::StoryMediaCue> media;
    game::story_reward_media("START_MOVIE", {" Empire_Win_Deathstar "}, media);
    game::story_reward_media("MULTIMEDIA", {"TEXT_SPEECH_X", "-1", "", "", "", "", "", "EMPIRE_ACT01_MISSION_TWO_20"}, media);
    game::story_reward_media("MULTIMEDIA", {"TEXT_SPEECH_X", "-1"}, media);
    game::story_reward_media("SCREEN_TEXT", {"TEXT_A"}, media);
    game::story_reward_media("START_MOVIE", {}, media);
    CHECK(media.size() == 2 && media[0].kind == game::StoryMediaCue::Kind::Movie && media[0].name == "Empire_Win_Deathstar" &&
          media[1].kind == game::StoryMediaCue::Kind::Speech && media[1].name == "EMPIRE_ACT01_MISSION_TWO_20");
    // FIXPASS_GALACTIC §4.2: MULTIMEDIA Param9 *_Loop (any case) = the talking head, "-1" / "" = none; STOP_COMMANDBAR_MOVIE ends it.
    media.clear();
    game::story_reward_media("MULTIMEDIA", {"T", "-1", "", "", "", "", "", "", " Tyber_loop "}, media);
    game::story_reward_media("MULTIMEDIA", {"T", "-1", "", "", "", "", "", "", "-1"}, media);
    game::story_reward_media("MULTIMEDIA", {"T", "-1", "", "", "", "", "", "", ""}, media);
    game::story_reward_media("STOP_COMMANDBAR_MOVIE", {}, media);
    CHECK(media.size() == 2 && media[0].kind == game::StoryMediaCue::Kind::AdvisorMovie && media[0].name == "Tyber_loop" &&
          media[1].kind == game::StoryMediaCue::Kind::AdvisorStop);
    std::printf("story UI feed: %" PRIu64 " notifications raised\n", f.raised_total());
}

#ifdef ALAMO_GAME_HAVE_CONTROLS
// Radar feeds (radar_feed.hpp): asteroid fields, Default_Unit_Die, capture-point transitions, story blips (data-free).
void radar_feed_test() {
    namespace ta = tactical::abilities;
    // Asteroid fields: Is_Asteroid_Field and Is_Impassable_Asteroid discs; nebulae / ion storms / zero radius are left out.
    ta::SpaceField f[5];
    f[0].kinds = ta::kFieldAsteroid;
    f[0].x = 10;
    f[0].y = 20;
    f[0].radius = 300;
    f[1].kinds = ta::kFieldNebula;
    f[1].radius = 500;
    f[2].kinds = ta::kFieldImpassable;
    f[2].x = -5;
    f[2].radius = 120;
    f[3].kinds = ta::kFieldIonStorm | ta::kFieldAsteroid;  // an asteroid that is also a storm: still drawn
    f[3].radius = 50;
    f[4].kinds = ta::kFieldAsteroid;  // radius 0
    const std::vector<ui::RadarField> rf = game::radar_asteroid_fields(f);
    CHECK(rf.size() == 3 && rf[0].x == 10.0f && rf[0].y == 20.0f && rf[0].radius == 300.0f && rf[1].x == -5.0f && rf[2].radius == 50.0f);
    const ui::RadarFieldPaint paint{rf, -1000, -1000, 1000, 1000};
    ui::RadarImage img;
    img.build(32, {}, 0, 0, {}, 0, 0, ui::Rgba8{25, 66, 120, 100}, paint);
    size_t painted = 0;
    for (size_t i = 3; i < img.rgba().size(); i += 4) painted += img.rgba()[i] != 0;
    CHECK(painted > 0 && painted < 32 * 32 / 2);

    const data::XmlDocument doc = data::XmlDocument::parse(std::string_view(
        R"(<RadarMap><RadarMapEvents>
        <Radar_Map_Event name="Default_Unit_Die"><Event_Model_Name>I_minimap_death_target.alo</Event_Model_Name><Event_Duration>1.0</Event_Duration></Radar_Map_Event>
        <Radar_Map_Event name="Default_Control_Point_Transition"><Event_Model_Name>I_minimap_control_point.alo</Event_Model_Name><Event_Duration>1.0</Event_Duration></Radar_Map_Event>
        </RadarMapEvents></RadarMap>)"));
    ui::RadarSettings cfg;
    CHECK(cfg.parse(doc.root()));
    ui::RadarEvents ev;
    game::RadarFeed feed;
    // Deaths: only the ones the local player sees, of radar-visible types.
    const game::RadarDeathNote deaths[] = {{1, 2, true, true}, {3, 4, false, true}, {5, 6, true, false}, {7, 8, true, true}};
    feed.deaths(deaths, cfg, ev);
    CHECK(ev.items().size() == 2 && ev.items()[0].type == "Default_Unit_Die" && ev.items()[1].x == 7.0f && feed.death_pings() == 2);
    ev.update(1.5f);
    CHECK(ev.items().empty());
    // Capture points: the ping plays once when the progress leaves 0 towards another controller than the owner.
    game::RadarCapturePointNote cp;
    cp.id = 40;
    cp.owner = 2;
    cp.pending = -1;
    cp.progress = 0.0f;
    cp.x = 100;
    cp.y = 50;
    cp.begin_event = "DEFAULT_CONTROL_POINT_TRANSITION";
    feed.capture_points(std::span<const game::RadarCapturePointNote>(&cp, 1), cfg, ev);
    CHECK(ev.items().empty());  // idle point
    cp.pending = 1;
    cp.progress = 0.1f;
    feed.capture_points(std::span<const game::RadarCapturePointNote>(&cp, 1), cfg, ev);
    CHECK(ev.items().size() == 1 && ev.items()[0].x == 100.0f && feed.transition_pings() == 1);
    cp.progress = 0.4f;  // still rising: no second ping
    feed.capture_points(std::span<const game::RadarCapturePointNote>(&cp, 1), cfg, ev);
    CHECK(ev.items().size() == 1 && feed.transition_pings() == 1);
    cp.progress = 0.0f;  // owner won it back, then a new attempt
    cp.pending = 2;
    feed.capture_points(std::span<const game::RadarCapturePointNote>(&cp, 1), cfg, ev);
    cp.pending = 1;
    cp.progress = 0.05f;
    cp.visible = false;  // fogged: the local player does not see it
    feed.capture_points(std::span<const game::RadarCapturePointNote>(&cp, 1), cfg, ev);
    CHECK(feed.transition_pings() == 1);
    // Story blips: upsert, follow the object, keep the last place when it is gone, removed when no longer listed.
    ui::RadarEvents bl;
    game::RadarBlipNote n[2];
    n[0].name = "objective_blip";
    n[0].x = 1;
    n[0].y = 2;
    n[1].name = "starbase_01_blip";
    n[1].x = 3;
    n[1].y = 4;
    feed.blips(n, bl);
    CHECK(bl.items().size() == 2);
    n[0].x = 10;
    n[1].valid = false;
    n[1].x = 99;
    feed.blips(n, bl);
    CHECK(bl.items().size() == 2 && bl.items()[0].x == 10.0f && bl.items()[1].x == 3.0f);  // moved / kept
    feed.blips(std::span<const game::RadarBlipNote>(n, 1), bl);
    CHECK(bl.items().size() == 1 && bl.items()[0].type == "objective_blip");
    feed.blips({}, bl);
    CHECK(bl.items().empty());
}
#endif

int main() {
#ifdef ALAMO_GAME_HAVE_CONTROLS
    radar_feed_test();
#endif
    story_ui_test();
    story_script_test();
    story_scripts_real_test();
    game::GameData data;
    std::string err;
    const std::string dir = make_data();
    CHECK(game::load_game_data_dir(dir, data, &err));
    if (!err.empty()) std::printf("data: %s\n", err.c_str());
    if (g_fail == 0) {
        const auto map = space_map();
        const game::SessionSpec spec = spec_for(map);
        commands::CommandRecording rec;
        RunResult live;
        if (live_run(data, spec, rec, live)) {
            for (const int32_t w : {0, 4}) {
                RunResult rp;
                replay_run(data, spec, rec, live.ticks, w, rp, false, true);
                const CrcDivergence d = first_divergence(live.crc, rp.crc);
                std::printf("replay (%d workers) vs live: %zu vs %zu records: %s\n", w, rp.crc.records.size(), live.crc.records.size(),
                            d.message.c_str());
                CHECK(!d.diverged && rp.crc.records.size() == live.crc.records.size());
            }
        }
        // P4 host: auto-resolve choice + Withdraw, the battle's Autoresolve, the special systems; each replayed with 0 and
        // 4 workers.
        auto replays = [&](const char* name, const commands::CommandRecording& r, const RunResult& live_run_result, bool special) {
            for (const int32_t w : {0, 4}) {
                RunResult rp;
                replay_run(data, spec, r, live_run_result.ticks, w, rp, special);
                const CrcDivergence d = first_divergence(live_run_result.crc, rp.crc);
                std::printf("%s replay (%d workers) vs live: %s\n", name, w, d.message.c_str());
                CHECK(!d.diverged && rp.crc.records.size() == live_run_result.crc.records.size());
            }
        };
        commands::CommandRecording rec2;
        RunResult ar;
        if (auto_resolve_run(data, spec, rec2, ar)) replays("auto-resolve", rec2, ar, false);
        commands::CommandRecording rec3;
        RunResult bar;
        if (battle_auto_resolve_run(data, spec, rec3, bar)) replays("battle auto-resolve", rec3, bar, false);
        commands::CommandRecording rec4;
        RunResult sp;
        if (special_run(data, spec, rec4, sp)) replays("special systems", rec4, sp, true);
        // [port] skirmish map -> planet type: the name after _MP_SPACE_ / _MP_LAND_ when it is a PLANET type.
        CHECK(game::skirmish_map_planet_type(data.rules, "_MP_LAND_GS_PLANET") == "GS_Planet");
        CHECK(game::skirmish_map_planet_type(data.rules, "Data\\Art\\Maps\\_mp_space_gs_planet.ted") == "GS_Planet");
        CHECK(game::skirmish_map_planet_type(data.rules, "_MP_SPACE_GS_FRIGATE").empty());  // not a planet
        CHECK(game::skirmish_map_planet_type(data.rules, "_MP_LAND_NOWHERE").empty());
        CHECK(game::skirmish_map_planet_type(data.rules, "GS_SPACE").empty());
        // OI host.
        rate_lock_test(data, spec);
        attract_session_test(data, spec);
        attract_controller_test(data);
        attract_camera_pure_test();
        attract_rec_test();
        attract_camera_session_test();
        // N1 / t09: saved games and GameSession save / load.
        save_files_test();
        save_load_test(data, spec);
#ifdef ALAMO_GAME_HAVE_CONTROLS
        controls_test();
        hud_feeds_test(data, spec);
        hero_roster_real_test();
#endif
    }
    std::error_code ec;
    remove_all_long(fs::path(path_from_utf8(dir)), ec);
    if (g_fail) {
        std::printf("alamo_game_session_tests: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("alamo_game_session_tests: all passed\n");
    return 0;
}
