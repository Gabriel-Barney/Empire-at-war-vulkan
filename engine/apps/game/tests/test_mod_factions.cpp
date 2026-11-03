// alamo_game_mod_faction_tests (q6 t08): AI player types and default skirmish forces for factions the port does not
// hard-code (game_data.hpp resolve_ai_type / default_skirmish_fleet / default_skirmish_ground / data_fallback_forces).
//   * synthetic data in its own temp folder: a stock Empire (Basic_AI BasicEmpire), a Rebel a mod redefined (GenericAI),
//     mod factions with and without Space_ / Land_Skirmish_AI_Default_Forces, a campaign's AI_Player_Control pairs;
//   * with ALAMO_FOC_DATA: the shipped factions resolve exactly as the hard-coded port tables (vanilla unchanged).
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

    // The hard-coded table is unchanged; Basic_AI decides which factions are stock.
    CHECK(game::ai_type_for("Empire") == "BasicEmpire" && game::ai_type_for("rebel") == "BasicRebel" &&
          game::ai_type_for("Underworld") == "AI_Player_Underworld" && game::ai_type_for("Chiss").empty());
    CHECK(game::is_stock_faction("Empire", {{"EMPIRE", "basicempire"}}));
    CHECK(game::is_stock_faction("Underworld", {}));  // no Basic_AI in the data: stock
    CHECK(!game::is_stock_faction("Empire", {{"EMPIRE", "GenericAI"}}));
    CHECK(!game::is_stock_faction("Chiss", {{"CHISS", "ChissAI"}}));

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
    // Skirmish: Basic_AI; stock factions keep the port's names (BasicEmpire is not in `defs` and still resolves).
    CHECK(game::resolve_ai_type("Empire", d.basic_ai, nullptr, &defs) == "BasicEmpire");
    CHECK(game::resolve_ai_type("Underworld", d.basic_ai, nullptr, &defs) == "AI_Player_Underworld");
    CHECK(game::resolve_ai_type("Chiss", d.basic_ai, nullptr, &defs) == "ChissAI");
    CHECK(game::resolve_ai_type("Yevetha", d.basic_ai, nullptr, &defs) == "YevethaAI");
    CHECK(game::resolve_ai_type("Rebel", d.basic_ai, nullptr, nullptr) == "GenericAI");
    CHECK(game::resolve_ai_type("Rebel", d.basic_ai, nullptr, &defs).empty());  // GenericAI undefined: no AI
    CHECK(game::resolve_ai_type("Pirates", d.basic_ai, nullptr, &defs).empty());
    CHECK(game::resolve_ai_type("Neutral", d.basic_ai, nullptr, &defs).empty());
    // Campaign: the AI_Player_Control pairs (stock factions excepted; no pair = no AI).
    const data::CampaignDef* c = d.db.find_campaign("MF_GC");
    CHECK(c != nullptr);
    if (c) {
        const std::vector<std::string>* pairs = &c->ai_player_control;
        CHECK(game::resolve_ai_type("Chiss", d.basic_ai, pairs, &defs) == "ChissCampaignAI");
        CHECK(game::resolve_ai_type("Empire", d.basic_ai, pairs, &defs) == "BasicEmpire");
        CHECK(game::resolve_ai_type("Yevetha", d.basic_ai, pairs, &defs).empty());  // Human
        CHECK(game::resolve_ai_type("Rebel", d.basic_ai, pairs, &defs) == "None");  // a real AIPlayerType
        CHECK(game::resolve_ai_type("Pirates", d.basic_ai, pairs, &defs).empty());  // no Basic_AI: as before
        const std::vector<std::string> none;
        CHECK(game::resolve_ai_type("Chiss", d.basic_ai, &none, &defs).empty());
    }
#endif

    // Stock faction: the hard-coded lists (the data defines Star_Destroyer / Imperial_Stormtrooper_Squad).
    CHECK(game::default_skirmish_fleet(d, "Empire") == game::default_skirmish_fleet("Empire"));
    CHECK(game::default_skirmish_ground(d, "Empire") == game::default_skirmish_ground("Empire"));
    // Chiss: squadrons only, no hyperspace -> the cheapest buildable hyperspace ship holding 2 squadrons leads.
    CHECK((game::default_skirmish_fleet(d, "Chiss") == Names{"MF_Frigate", "MF_Squadron", "MF_Squadron"}));
    CHECK((game::default_skirmish_ground(d, "Chiss") == Names{"MF_Trooper_Company"}));
    // Yevetha: placeholder list -> the fallback (cheapest two + median; heroes, locked, free, non-hyperspace left out).
    CHECK((game::data_fallback_forces(d, "Yevetha", false) == Names{"Y_A", "Y_B", "Y_D"}));
    CHECK((game::default_skirmish_fleet(d, "Yevetha") == Names{"Y_A", "Y_B", "Y_D"}));
    CHECK((game::default_skirmish_ground(d, "Yevetha") == Names{"Y_G2", "Y_G1"}));  // its land list names a ship: dropped
    // Rebel (GenericAI, so not stock): no list, no affiliated type.
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
    s.land = true;
    err.clear();
    CHECK(game::make_skirmish_galaxy(d, s, g, &err));
    if (!err.empty()) std::printf("land skirmish: %s\n", err.c_str());
    CHECK(forces_of(g, 0, game::kSkirmishHumanPlanet) == 2 && forces_of(g, 1, game::kSkirmishBattlePlanet) == 2);
    if (g.forces.size() > 2) CHECK(d.rules.types[g.forces[2].type].name == "MF_Frigate");  // the human's carrier
    std::error_code ec;
    fs::remove_all(root, ec);
}

// The shipped factions resolve as the hard-coded port tables: vanilla sessions are unchanged.
void test_shipped_data() {
    const char* foc = std::getenv("ALAMO_FOC_DATA");
    if (!foc || !fs::exists(fs::path(foc) / "Data")) {
        std::printf("shipped data: skipped (ALAMO_FOC_DATA not set)\n");
        return;
    }
    game::GameData d;
    std::string err;
    CHECK(game::load_game_data(foc, {}, d, &err));
    for (const char* f : {"Empire", "Rebel", "Underworld"}) {
        CHECK(game::default_skirmish_fleet(d, f) == game::default_skirmish_fleet(f));
        CHECK(game::default_skirmish_ground(d, f) == game::default_skirmish_ground(f));
    }
#ifdef ALAMO_GAME_HAVE_GALACTIC_AI
    CHECK(d.ai != nullptr);
    const ai::AiDefinitions* defs = d.ai ? &d.ai->defs : nullptr;
    for (const galactic::FactionRules& f : d.rules.factions) {
        CHECK(game::resolve_ai_type(f.name, d.basic_ai, nullptr, defs) == game::ai_type_for(f.name));
        for (const data::CampaignDef& c : d.db.campaigns())
            if (game::resolve_ai_type(f.name, d.basic_ai, &c.ai_player_control, defs) != game::ai_type_for(f.name)) {
                std::printf("FAIL campaign %s faction %s\n", c.name.c_str(), f.name.c_str());
                ++g_fail;
            }
    }
    for (const char* f : {"Empire", "Rebel", "Underworld"}) CHECK(game::is_stock_faction(f, d.basic_ai));
#endif
}

}  // namespace

int main() {
    test_synthetic();
    test_shipped_data();
    std::printf(g_fail ? "alamo_game_mod_faction_tests: %d FAILED\n" : "alamo_game_mod_faction_tests: OK\n", g_fail);
    return g_fail ? 1 : 0;
}
