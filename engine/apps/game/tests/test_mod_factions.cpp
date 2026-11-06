// alamo_game_mod_faction_tests (q6 t08): AI player types and default skirmish forces for factions the port does not
// hard-code (game_data.hpp resolve_ai_type / default_skirmish_fleet / default_skirmish_ground / data_fallback_forces);
// q6: the skirmish buy-units list (skirmish_rules.hpp skirmish_buy_list) on synthetic, shipped and Thrawn's Revenge data.
//   * synthetic data in its own temp folder: an Empire with Basic_AI BasicEmpire, a Rebel a mod redefined (GenericAI),
//     mod factions with and without Space_ / Land_Skirmish_AI_Default_Forces, a campaign's AI_Player_Control pairs;
//     modc t03: with and without a mod layer (GameData::mod_layer), no rule keyed on a faction's name;
//   * with ALAMO_FOC_DATA: the shipped factions resolve as the port has always played them (vanilla unchanged).
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "alamo/game/game_data.hpp"

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

using Names = std::vector<std::string>;

// A moving hyperspace ship / a ground company / a squadron of `faction` costing `cost`, plus `extra` tags.
std::string ship(const char* name, const char* faction, int cost, const std::string& extra = "", int tech = 1, int speed = 2) {
    return std::string(" <SpaceUnit Name=\"") + name + "\"><Hyperspace>Yes</Hyperspace><Hyperspace_Speed>1.0</Hyperspace_Speed><Max_Speed>" +
           std::to_string(speed) + "</Max_Speed><Tech_Level>" + std::to_string(tech) + "</Tech_Level><Affiliation>" + faction +
           "</Affiliation><Build_Cost_Credits>" + std::to_string(cost) + "</Build_Cost_Credits>" + extra + "</SpaceUnit>\n";
}
std::string company(const char* name, const char* faction, int cost) {
    return std::string(" <GroundCompany Name=\"") + name + "\"><Tech_Level>1</Tech_Level><Affiliation>" + faction + "</Affiliation><Build_Cost_Credits>" +
           std::to_string(cost) + "</Build_Cost_Credits></GroundCompany>\n";
}

std::string make_types() {
    std::string s = "<MF>\n <Planet Name=\"MF_Planet\"><Behavior>PLANET</Behavior><Terrain>Temperate</Terrain></Planet>\n";
    s += ship("Star_Destroyer", "Empire", 3000);
    s += company("Imperial_Stormtrooper_Squad", "Empire", 100);
    s += ship("MF_Cruiser", "Chiss", 1000, "<Squadron_Capacity>4</Squadron_Capacity>");
    s += ship("MF_Frigate", "Chiss", 500, "<Squadron_Capacity>2</Squadron_Capacity>");
    s += ship("MF_Corvette", "Chiss", 200, "<Squadron_Capacity>1</Squadron_Capacity>");
    s += " <Squadron Name=\"MF_Squadron\"><Affiliation>Chiss</Affiliation><Build_Cost_Credits>100</Build_Cost_Credits></Squadron>\n";
    s += company("MF_Trooper_Company", "Chiss", 150);
    s += ship("Y_B", "Yevetha", 300);
    s += ship("Y_A", "Yevetha", 300);
    s += ship("Y_D", "Yevetha", 600);
    s += ship("Y_C", "Yevetha", 900);
    // Left out of the fallback: hero, locked, free, tech 0, planet-bound, station (Max_Speed 0), no hyperspace, other faction.
    s += ship("Y_Hero", "Yevetha", 100, "<Is_Named_Hero>Yes</Is_Named_Hero>");
    s += ship("Y_Locked", "Yevetha", 50, "<Build_Initially_Locked>Yes</Build_Initially_Locked>");
    s += ship("Y_Free", "Yevetha", 0);
    s += ship("Y_Tech0", "Yevetha", 40, "", 0);
    s += ship("Y_Planet_Bound", "Yevetha", 40, "<Required_Planets>MF_Planet</Required_Planets>");
    s += ship("Y_Station", "Yevetha", 40, "", 1, 0);
    s += " <SpaceUnit Name=\"Y_Slow\"><Max_Speed>2</Max_Speed><Tech_Level>1</Tech_Level><Affiliation>Yevetha</Affiliation>"
         "<Build_Cost_Credits>10</Build_Cost_Credits></SpaceUnit>\n";
    s += ship("Y_Chiss_Ship", "Chiss", 20);
    s += company("Y_G1", "Yevetha", 200);
    s += company("Y_G2", "Yevetha", 100);
    // q6 buy list (SKIRMISH_SETUP §2.7): Hapan types, in this order (type index = type-manager order).
    auto unit = [&s](const char* tag, const char* name, const char* faction, int cost, const std::string& extra) {
        s += std::string(" <") + tag + " Name=\"" + name + "\">" + (faction ? std::string("<Affiliation>") + faction + "</Affiliation>" : std::string()) +
             "<Build_Cost_Credits>" + std::to_string(cost) + "</Build_Cost_Credits>" + extra + "</" + tag + ">\n";
    };
    const std::string space_tab = "<Build_Tab_Space_Units>Yes</Build_Tab_Space_Units>";
    unit("SpaceUnit", "H_Cheap", "Hapan", 100, space_tab);
    unit("SpaceUnit", "H_Big", "Hapan", 900, "<Available_In_Skirmish>Yes</Available_In_Skirmish>");  // both map kinds
    unit("SpaceUnit", "H_Mid_A", "Hapan", 500, space_tab);
    unit("SpaceUnit", "H_Mid_B", "Hapan, Empire", 500, space_tab);  // same cost: type order
    unit("GroundCompany", "H_Land", "Hapan", 200, "<Build_Tab_Land_Units>Yes</Build_Tab_Land_Units>");
    unit("SpaceUnit", "H_Hero", "Hapan", 300, space_tab + "<Is_Named_Hero>Yes</Is_Named_Hero>");
    unit("SpaceUnit", "H_Cine", "Hapan", 300, space_tab + "<Cinematic_Object_Only>Yes</Cinematic_Object_Only>");
    unit("SpaceUnit", "H_Free", "Hapan", 0, space_tab);
    unit("SpaceUnit", "H_No_Tab", "Hapan", 300, "<Build_Tab_Land_Units>Yes</Build_Tab_Land_Units>");  // land tab: land only
    unit("SpaceUnit", "H_Other", "Chiss", 300, space_tab);
    unit("SpaceUnit", "H_No_Affiliation", nullptr, 300, space_tab);
    s += " <SpecialStructure Name=\"H_Pad\"><Tactical_Buildable_Objects_Multiplayer>Hapan, H_Mid_B, H_Land, Chiss, H_Other"
         "</Tactical_Buildable_Objects_Multiplayer></SpecialStructure>\n";
    return s + "</MF>\n";
}

// Chiss is defined twice: the second definition's space list wins, its land list comes from the first.
const char* kFactions = R"(<Factions>
 <Faction Name="Empire"><Is_Playable>Yes</Is_Playable><Can_Control_Planets>Yes</Can_Control_Planets><Basic_AI>BasicEmpire</Basic_AI></Faction>
 <Faction Name="Rebel"><Is_Playable>Yes</Is_Playable><Can_Control_Planets>Yes</Can_Control_Planets><Basic_AI> GenericAI </Basic_AI></Faction>
 <Faction Name="Neutral"><Is_Neutral>Yes</Is_Neutral></Faction>
 <Faction Name="Chiss"><Is_Playable>Yes</Is_Playable><Can_Control_Planets>Yes</Can_Control_Planets><Basic_AI>ChissAI</Basic_AI>
  <Space_Skirmish_AI_Default_Forces> MF_Cruiser </Space_Skirmish_AI_Default_Forces>
  <Land_Skirmish_AI_Default_Forces>
   MF_Trooper_Company
  </Land_Skirmish_AI_Default_Forces></Faction>
 <Faction Name="Yevetha"><Is_Playable>Yes</Is_Playable><Can_Control_Planets>Yes</Can_Control_Planets><Basic_AI>YevethaAI</Basic_AI>
  <Space_Skirmish_AI_Default_Forces> Factions_Thing_To_Replace </Space_Skirmish_AI_Default_Forces>
  <Land_Skirmish_AI_Default_Forces> Y_A </Land_Skirmish_AI_Default_Forces></Faction>
 <Faction Name="Pirates"><Can_Control_Planets>Yes</Can_Control_Planets></Faction>
 <Faction Name="Hapan"><Is_Playable>Yes</Is_Playable><Can_Control_Planets>Yes</Can_Control_Planets></Faction>
</Factions>
)";
const char* kFactions2 = R"(<Factions>
 <Faction Name="CHISS"><Basic_AI>Ignored_Second_Basic_AI</Basic_AI>
  <Space_Skirmish_AI_Default_Forces> MF_Squadron, Missing_Type, MF_Squadron </Space_Skirmish_AI_Default_Forces></Faction>
</Factions>
)";

void write_file(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << s;
}

fs::path make_data() {
    const fs::path root = fs::temp_directory_path() / "alamo_game_mod_faction_tests";
    std::error_code ec;
    fs::remove_all(root, ec);
    const fs::path x = root / "Data/XML";
    write_file(x / "GameObjectFiles.xml", "<GameObjectFiles><File>MFTypes.xml</File></GameObjectFiles>\n");
    write_file(x / "MFTypes.xml", make_types());
    write_file(x / "FactionFiles.xml", "<FactionFiles><File>MFFactions.xml</File><File>MFFactions2.xml</File></FactionFiles>\n");
    write_file(x / "MFFactions.xml", kFactions);
    write_file(x / "MFFactions2.xml", kFactions2);
    write_file(x / "TradeRouteFiles.xml", "<TradeRouteFiles></TradeRouteFiles>\n");
    write_file(x / "CampaignFiles.xml", "<Campaign_Files><File>MFCampaigns.xml</File></Campaign_Files>\n");
    write_file(x / "MFCampaigns.xml",
               "<Campaigns><Campaign Name=\"MF_GC\"><Locations>MF_Planet</Locations>"
               "<AI_Player_Control> Chiss, ChissCampaignAI </AI_Player_Control><AI_Player_Control>Empire, None</AI_Player_Control>"
               "<AI_Player_Control>Yevetha, Human</AI_Player_Control><AI_Player_Control>Rebel, None</AI_Player_Control>"
               "<AI_Player_Control>Pirates, None</AI_Player_Control></Campaign></Campaigns>\n");
    write_file(x / "GameConstants.xml", "<GameConstants></GameConstants>\n");
    return root;
}

size_t forces_of(const galactic::GalacticSetup& g, int32_t player, PlanetIndex at) {
    return static_cast<size_t>(std::count_if(g.forces.begin(), g.forces.end(), [&](const auto& f) { return f.player == player && f.planet == at; }));
}

void test_synthetic() {
    const fs::path root = make_data();
    game::GameData d;
    std::string err;
    CHECK(game::load_game_data_dir(root.string(), d, &err));
    if (!err.empty()) std::printf("load: %s\n", err.c_str());

    CHECK(!d.mod_layer);  // synthetic data: no mod layer

    // Forces as read from the faction files (last list wins, names as written).
    const auto chiss = d.skirmish_forces.find("CHISS");
    CHECK(chiss != d.skirmish_forces.end());
    if (chiss != d.skirmish_forces.end()) {
        CHECK((chiss->second.space == Names{"MF_Squadron", "Missing_Type", "MF_Squadron"}));
        CHECK((chiss->second.land == Names{"MF_Trooper_Company"}));
    }
    CHECK(d.skirmish_forces.count("EMPIRE") == 0 && d.skirmish_forces.count("PIRATES") == 0);

#ifdef ALAMO_GAME_HAVE_GALACTIC_AI
    CHECK(d.basic_ai.size() == 4);
    CHECK(d.basic_ai.count("CHISS") && d.basic_ai.at("CHISS") == "ChissAI");  // first definition wins (ai::load_faction_basic_ai)
    CHECK(d.basic_ai.count("REBEL") && d.basic_ai.at("REBEL") == "GenericAI");

    ai::AiDefinitions defs;
    for (const char* n : {"ChissAI", "ChissCampaignAI", "YevethaAI", "None"})
        defs.add_players(std::string("<AIPlayer><Name>") + n + "</Name></AIPlayer>", std::string(n) + ".xml");
    // Skirmish: Basic_AI for every faction alike (modc t03: no built-in types by name). BasicEmpire is not in `defs`.
    CHECK(game::resolve_ai_type("Empire", d.basic_ai, nullptr, nullptr) == "BasicEmpire");
    CHECK(game::resolve_ai_type("Empire", d.basic_ai, nullptr, &defs).empty());  // undefined: no AI
    CHECK(game::resolve_ai_type("Underworld", d.basic_ai, nullptr, nullptr).empty());  // not in the data
    CHECK(game::resolve_ai_type("Chiss", d.basic_ai, nullptr, &defs) == "ChissAI");
    CHECK(game::resolve_ai_type("Yevetha", d.basic_ai, nullptr, &defs) == "YevethaAI");
    CHECK(game::resolve_ai_type("Rebel", d.basic_ai, nullptr, nullptr) == "GenericAI");
    CHECK(game::resolve_ai_type("Rebel", d.basic_ai, nullptr, &defs).empty());  // GenericAI undefined: no AI
    CHECK(game::resolve_ai_type("Pirates", d.basic_ai, nullptr, &defs).empty());
    CHECK(game::resolve_ai_type("Neutral", d.basic_ai, nullptr, &defs).empty());
    // Campaign with a mod layer (campaign_pairs): the AI_Player_Control pairs for every faction (no pair = no AI).
    const data::CampaignDef* c = d.db.find_campaign("MF_GC");
    CHECK(c != nullptr);
    if (c) {
        const std::vector<std::string>* pairs = &c->ai_player_control;
        CHECK(game::resolve_ai_type("Chiss", d.basic_ai, pairs, &defs) == "ChissCampaignAI");
        CHECK(game::resolve_ai_type("Empire", d.basic_ai, pairs, &defs) == "None");  // "Empire, None": no exception by name
        CHECK(game::resolve_ai_type("Yevetha", d.basic_ai, pairs, &defs).empty());  // Human
        CHECK(game::resolve_ai_type("Rebel", d.basic_ai, pairs, &defs) == "None");  // a real AIPlayerType
        CHECK(game::resolve_ai_type("Pirates", d.basic_ai, pairs, &defs).empty());  // no Basic_AI: as before
        const std::vector<std::string> none;
        CHECK(game::resolve_ai_type("Chiss", d.basic_ai, &none, &defs).empty());
        // Base game (no mod layer, campaign_pairs false): the pairs are not read, Basic_AI as in a skirmish.
        CHECK(game::resolve_ai_type("Empire", d.basic_ai, pairs, nullptr, false) == "BasicEmpire");
        CHECK(game::resolve_ai_type("Chiss", d.basic_ai, pairs, &defs, false) == "ChissAI");
        CHECK(game::resolve_ai_type("Yevetha", d.basic_ai, &none, &defs, false) == "YevethaAI");
        CHECK(game::resolve_ai_type("Pirates", d.basic_ai, pairs, &defs, false).empty());
    }
#endif

    // No mod layer: the [port] base-game lists (the data defines Star_Destroyer / Imperial_Stormtrooper_Squad).
    CHECK(game::default_skirmish_fleet(d, "Empire") == game::default_skirmish_fleet("Empire"));
    CHECK(game::default_skirmish_ground(d, "Empire") == game::default_skirmish_ground("Empire"));
    // With a mod layer the base-game lists are never used: Empire starts from its own data (none: the fallback).
    d.mod_layer = true;
    CHECK(game::default_skirmish_fleet(d, "Empire") != game::default_skirmish_fleet("Empire"));
    CHECK(game::default_skirmish_ground(d, "Empire") != game::default_skirmish_ground("Empire"));
    d.mod_layer = false;
    // Chiss: squadrons only, no hyperspace -> the cheapest buildable hyperspace ship holding 2 squadrons leads.
    CHECK((game::default_skirmish_fleet(d, "Chiss") == Names{"MF_Frigate", "MF_Squadron", "MF_Squadron"}));
    CHECK((game::default_skirmish_ground(d, "Chiss") == Names{"MF_Trooper_Company"}));
    // Yevetha: placeholder list -> the fallback (cheapest two + median; heroes, locked, free, non-hyperspace left out).
    CHECK((game::data_fallback_forces(d, "Yevetha", false) == Names{"Y_A", "Y_B", "Y_D"}));
    CHECK((game::default_skirmish_fleet(d, "Yevetha") == Names{"Y_A", "Y_B", "Y_D"}));
    CHECK((game::default_skirmish_ground(d, "Yevetha") == Names{"Y_G2", "Y_G1"}));  // its land list names a ship: dropped
    // Rebel: no list in the data, none of the base-game names defined, no affiliated type.
    CHECK(game::default_skirmish_fleet(d, "Rebel").empty());
    CHECK(game::data_fallback_forces(d, "Nobody", true).empty());

    // The skirmish galaxy for two mod factions (space and land).
    game::SkirmishSetup s;
    s.human_faction = "Chiss";
    s.ai_faction = "Yevetha";
    s.planet_type = "MF_Planet";
    galactic::GalacticSetup g;
    err.clear();
    CHECK(game::make_skirmish_galaxy(d, s, g, &err));
    if (!err.empty()) std::printf("space skirmish: %s\n", err.c_str());
    CHECK(forces_of(g, 0, game::kSkirmishHumanPlanet) == 3 && forces_of(g, 1, game::kSkirmishBattlePlanet) == 3);
    // q6: a bought space fleet without a hyperspace member (Y_Slow) is led by the faction's cheapest hyperspace ship (Y_A),
    // put in front; a bought fleet that can jump is kept as bought.
    {
        game::SkirmishSetup b = s;
        b.human_faction = "Yevetha";
        b.ai_faction = "Chiss";
        b.units_human = {"Y_Slow", "Y_Slow"};
        err.clear();
        CHECK(game::make_skirmish_galaxy(d, b, g, &err));
        CHECK(forces_of(g, 0, game::kSkirmishHumanPlanet) == 3);
        for (const auto& f : g.forces)
            if (f.player == 0) {
                CHECK(d.rules.types[f.type].name == "Y_A");
                break;
            }
        b.units_human = {"Y_Slow", "Y_C"};
        CHECK(game::make_skirmish_galaxy(d, b, g, &err));
        CHECK(forces_of(g, 0, game::kSkirmishHumanPlanet) == 2);
        const uint32_t slow = d.rules.find_type("Y_Slow"), fac = d.rules.find_faction("Yevetha");
        CHECK(game::skirmish_hyperspace_leader(d.rules, d.types, fac, {slow}) == d.rules.find_type("Y_A"));
        CHECK(game::skirmish_hyperspace_leader(d.rules, d.types, fac, {slow, d.rules.find_type("Y_C")}) == galactic::kNoIndex);
        CHECK(game::skirmish_hyperspace_leader(d.rules, d.types, fac, {}) == galactic::kNoIndex);
    }
    s.land = true;
    err.clear();
    CHECK(game::make_skirmish_galaxy(d, s, g, &err));
    if (!err.empty()) std::printf("land skirmish: %s\n", err.c_str());
    CHECK(forces_of(g, 0, game::kSkirmishHumanPlanet) == 2 && forces_of(g, 1, game::kSkirmishBattlePlanet) == 2);
    if (g.forces.size() > 2) CHECK(d.rules.types[g.forces[2].type].name == "MF_Frigate");  // the human's carrier
    std::error_code ec;
    fs::remove_all(root, ec);
}

// q6: the skirmish buy-units list (SKIRMISH_SETUP §2.7, game::skirmish_buy_list).
Names buy_names(const game::GameData& d, const char* faction, bool land, bool heroes = false, bool mp_only = false) {
    game::SkirmishBuyOptions o;
    o.heroes = heroes;
    o.multiplayer_list_only = mp_only;
    Names out;
    for (const uint32_t t : game::skirmish_buy_list(d.rules, d.types, d.rules.find_faction(faction), land, o)) out.push_back(d.rules.types[t].name);
    return out;
}

void test_buy_list_synthetic() {
    const fs::path root = make_data();
    game::GameData d;
    std::string err;
    CHECK(game::load_game_data_dir(root.string(), d, &err));
    // Cost > 0, Affiliation (an empty one never matches), Available_In_Skirmish or the map kind's tab, not cinematic-only,
    // no named hero; most expensive first, equal costs in type order.
    CHECK((buy_names(d, "Hapan", false) == Names{"H_Big", "H_Mid_A", "H_Mid_B", "H_Cheap"}));
    CHECK((buy_names(d, "Hapan", false, true) == Names{"H_Big", "H_Mid_A", "H_Mid_B", "H_Hero", "H_Cheap"}));  // heroes option
    CHECK((buy_names(d, "Hapan", true) == Names{"H_Big", "H_No_Tab", "H_Land"}));
    CHECK((buy_names(d, "Empire", false) == Names{"H_Mid_B"}));
    CHECK((buy_names(d, "Chiss", false) == Names{"H_Other"}));  // the Chiss ships carry no tab
    // [port interim rule] only types named in some Tactical_Buildable_Objects_Multiplayer list (any faction row).
    CHECK((buy_names(d, "Hapan", false, false, true) == Names{"H_Mid_B"}));
    CHECK((buy_names(d, "Hapan", true, false, true) == Names{"H_Land"}));
    CHECK((buy_names(d, "Chiss", false, false, true) == Names{"H_Other"}));
    CHECK(game::skirmish_buy_list(d.rules, d.types, galactic::kNoIndex, false).empty());
    std::error_code ec;
    fs::remove_all(root, ec);
}

// The buy list on the shipped data (SKIRMISH_SETUP §2.7 "What the filter gives on shipped data") and, when the mod is
// installed next to the Steam FoC (or ALAMO_TR_DATA), on Thrawn's Revenge with and without the [port interim rule].
void test_buy_list_shipped() {
    const char* foc = std::getenv("ALAMO_FOC_DATA");
    if (!foc || !fs::exists(fs::path(foc) / "Data")) {
        std::printf("buy list (shipped data): skipped (ALAMO_FOC_DATA not set)\n");
        return;
    }
    game::GameData d;
    std::string err;
    CHECK(game::load_game_data(foc, {}, d, &err));
    auto count = [&](const game::GameData& g, const char* f, bool land, bool heroes, bool mp) { return buy_names(g, f, land, heroes, mp).size(); };
    for (const char* f : {"Empire", "Rebel", "Underworld"}) {
        const Names sp = buy_names(d, f, false), ld = buy_names(d, f, true);
        std::printf("buy list FoC %s: space %zu (heroes on %zu, MP-listed %zu), land %zu (heroes on %zu, MP-listed %zu); first: %s / %s\n", f,
                    sp.size(), count(d, f, false, true, false), count(d, f, false, false, true), ld.size(), count(d, f, true, true, false),
                    count(d, f, true, false, true), sp.empty() ? "-" : sp[0].c_str(), ld.empty() ? "-" : ld[0].c_str());
    }
    // §2.7: Empire space 23 rows, 20 without named heroes.
    CHECK(count(d, "Empire", false, true, false) == 23 && count(d, "Empire", false, false, false) == 20);
    const Names e = buy_names(d, "Empire", false);
    CHECK(std::find(e.begin(), e.end(), "Executor_Super_Star_Destroyer") != e.end());  // a story type with a tab and a cost
    std::string tr;
    if (const char* v = std::getenv("ALAMO_TR_DATA")) tr = v;
    const std::string fs_foc(foc);
    const size_t at = fs_foc.find("\\steamapps\\common\\");
    if (tr.empty() && at != std::string::npos) tr = fs_foc.substr(0, at) + "\\steamapps\\workshop\\content\\32470\\1125571106";
    std::error_code ec;
    if (tr.empty() || !fs::is_directory(fs::path(tr) / "Data" / "XML", ec)) {
        std::printf("buy list (Thrawn's Revenge): skipped (mod not found)\n");
        return;
    }
    game::GameData m;
    err.clear();
    CHECK(game::load_game_data(foc, {"MODPATH=" + tr}, m, &err));
    if (!err.empty()) std::printf("TR load: %s\n", err.c_str());
    for (const char* f : {"Empire", "Rebel"}) {
        const Names sp = buy_names(m, f, false, false, true), ld = buy_names(m, f, true, false, true);
        std::printf("buy list TR %s: space %zu -> %zu, land %zu -> %zu (heroes on, rules 1-4 only: %zu / %zu); first: %s / %s\n", f,
                    count(m, f, false, false, false), sp.size(), count(m, f, true, false, false), ld.size(), count(m, f, false, true, false),
                    count(m, f, true, true, false), sp.empty() ? "-" : sp[0].c_str(), ld.empty() ? "-" : ld[0].c_str());
        for (const std::string& n : sp)
            if (n.rfind("Option_", 0) == 0 || n.rfind("Cheat_", 0) == 0 || n.rfind("ZLayer_", 0) == 0 || n == "Dummy_Treaty_Victory") {
                std::printf("FAIL TR %s space list keeps marker %s\n", f, n.c_str());
                ++g_fail;
            }
    }
    // §2.7: rules 1-4 list the TR option / cheat markers (Empire space 407 rows with heroes); the interim rule keeps 106 / 38.
    const Names all = buy_names(m, "Empire", false, true, false);
    CHECK(std::find(all.begin(), all.end(), "Cheat_Give_Credits") != all.end());
    CHECK(count(m, "Empire", false, true, false) == 407 && count(m, "Empire", true, true, false) == 63);
    CHECK(count(m, "Empire", false, true, true) == 106 && count(m, "Empire", true, true, true) == 38);
}

// The shipped factions resolve as the port has always played them: vanilla sessions are unchanged.
void test_shipped_data() {
    const char* foc = std::getenv("ALAMO_FOC_DATA");
    if (!foc || !fs::exists(fs::path(foc) / "Data")) {
        std::printf("shipped data: skipped (ALAMO_FOC_DATA not set)\n");
        return;
    }
    game::GameData d;
    std::string err;
    CHECK(game::load_game_data(foc, {}, d, &err));
    CHECK(!d.mod_layer);
    for (const char* f : {"Empire", "Rebel", "Underworld"}) {
        CHECK(game::default_skirmish_fleet(d, f) == game::default_skirmish_fleet(f));
        CHECK(game::default_skirmish_ground(d, f) == game::default_skirmish_ground(f));
    }
#ifdef ALAMO_GAME_HAVE_GALACTIC_AI
    CHECK(d.ai != nullptr);
    const ai::AiDefinitions* defs = d.ai ? &d.ai->defs : nullptr;
    // modc t03: the types the port's old built-in table gave (Empire -> BasicEmpire, Rebel -> BasicRebel, Underworld ->
    // AI_Player_Underworld, others none) now come from the shipped Basic_AI, in skirmishes and (no mod layer) campaigns.
    const auto old_table = [](const std::string& f) -> std::string {
        if (f == "Empire") return "BasicEmpire";
        if (f == "Rebel") return "BasicRebel";
        if (f == "Underworld") return "AI_Player_Underworld";
        return {};
    };
    for (const galactic::FactionRules& f : d.rules.factions) {
        CHECK(game::resolve_ai_type(f.name, d.basic_ai, nullptr, defs) == old_table(f.name));
        for (const data::CampaignDef& c : d.db.campaigns())
            if (game::resolve_ai_type(f.name, d.basic_ai, &c.ai_player_control, defs, d.mod_layer) != old_table(f.name)) {
                std::printf("FAIL campaign %s faction %s\n", c.name.c_str(), f.name.c_str());
                ++g_fail;
            }
    }
#endif
}

}  // namespace

int main() {
    test_synthetic();
    test_shipped_data();
    test_buy_list_synthetic();  // q6
    test_buy_list_shipped();    // q6
    std::printf(g_fail ? "alamo_game_mod_faction_tests: %d FAILED\n" : "alamo_game_mod_faction_tests: OK\n", g_fail);
    return g_fail ? 1 : 0;
}
