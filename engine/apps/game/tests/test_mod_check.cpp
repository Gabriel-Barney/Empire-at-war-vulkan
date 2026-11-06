// alamo_game_mod_check_tests (Q5 mod diagnostics, mod_check.hpp): the report on a fake FoC install with a mod folder in its
// own temp folder (new and replaced files, a campaign with an unknown planet, a campaign without planets, a script that
// does not compile); and, with ALAMO_FOC_DATA, on the shipped data (loads, no errors).
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "alamo/game/mod_check.hpp"

namespace fs = std::filesystem;
using alamo::game::ModCheckReport;

static int g_fail = 0;
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

bool has(const std::vector<std::string>& v, const std::string& needle) {
    for (const auto& s : v)
        if (s.find(needle) != std::string::npos) return true;
    return false;
}
bool has(const std::vector<alamo::game::LuaIssue>& v, const std::string& needle) {
    for (const auto& e : v)
        if (e.text().find(needle) != std::string::npos) return true;
    return false;
}

const alamo::game::ModFolderStats* folder(const ModCheckReport& r, const std::string& name) {
    for (const auto& f : r.mod_folders)
        if (f.folder == name) return &f;
    return nullptr;
}

// Base install: two planets, one campaign, one script.
void write_base(const fs::path& root) {
    const fs::path x = root / "Data/XML";
    write_file(x / "GameObjectFiles.xml", "<GameObjectFiles><File>MCPlanets.xml</File></GameObjectFiles>\n");
    write_file(x / "MCPlanets.xml",
               "<Planets><Planet Name=\"MC_A\"><Galactic_Position>0, 0, 0</Galactic_Position></Planet>\n"
               "<Planet Name=\"MC_B\"><Galactic_Position>100, 0, 0</Galactic_Position></Planet></Planets>\n");
    write_file(x / "FactionFiles.xml", "<FactionFiles><File>MCFactions.xml</File></FactionFiles>\n");
    write_file(x / "MCFactions.xml",
               "<Factions><Faction Name=\"Empire\"><Is_Playable>Yes</Is_Playable></Faction>"
               "<Faction Name=\"Neutral\"><Is_Neutral>Yes</Is_Neutral></Faction></Factions>\n");
    write_file(x / "TradeRouteFiles.xml", "<TradeRouteFiles></TradeRouteFiles>\n");
    write_file(x / "CampaignFiles.xml", "<CampaignFiles><File>MCCampaigns.xml</File></CampaignFiles>\n");
    write_file(x / "MCCampaigns.xml", "<Campaigns><Campaign Name=\"Base_GC\"><Locations>MC_A, MC_B</Locations></Campaign></Campaigns>\n");
    write_file(x / "GameConstants.xml", "<GameConstants><Damage_Types>Damage_Default</Damage_Types></GameConstants>\n");
    write_file(root / "Data/Scripts/Story/Base_Story.lua", "function Base_Story() return 1 end\n");
}

// Mod: replaces the campaign list and a script, adds a campaign file, two scripts (one broken) and a texture.
void write_mod(const fs::path& mod) {
    const fs::path x = mod / "Data/XML";
    write_file(x / "CampaignFiles.xml",
               "<CampaignFiles><File>MCCampaigns.xml</File><File>ModCampaigns.xml</File></CampaignFiles>\n");
    write_file(x / "ModCampaigns.xml",
               "<Campaigns><Campaign Name=\"Mod_GC\"><Locations>MC_A, MC_Missing</Locations></Campaign>\n"
               "<Campaign Name=\"Empty_GC\"><Locations>MC_Nowhere</Locations></Campaign></Campaigns>\n");
    write_file(mod / "Data/Scripts/Story/Base_Story.lua", "function Base_Story() return 2 end\n");
    write_file(mod / "Data/Scripts/Story/Mod_Story.lua", "function Mod_Story() return 3 end\n");
    write_file(mod / "Data/Scripts/Story/Mod_Broken.lua", "function Mod_Broken( return end\n");
    write_file(mod / "Data/Art/Textures/MC_Mod.tga", "x");
}

void test_fake_install() {
    const fs::path root = fs::temp_directory_path() / "alamo_game_mod_check_tests";
    std::error_code ec;
    fs::remove_all(root, ec);
    const fs::path foc = root / "corruption", mod = root / "Mods" / "MC";
    write_base(foc);
    write_mod(mod);

    {  // base game alone: loads, no errors, no mod section
        const ModCheckReport r = alamo::game::check_mod_data(foc.string(), {});
        CHECK(r.loaded);
        CHECK(r.mod_dirs.empty());
        CHECK(r.counts.campaigns == 1);
        CHECK(r.counts.planets == 2);
        CHECK(r.campaigns_checked == 1);
        CHECK(r.campaigns_without_planets.empty() && r.campaign_warnings.empty());
        CHECK(r.lua_scripts == 1);
        CHECK(r.lua_errors.empty());
        CHECK(r.error_count() == 0);
        CHECK(!r.stages.empty() && r.stages.front().name == "game database");
        for (const auto& s : r.stages) CHECK(s.ok);
        const std::string text = alamo::game::format_mod_report(r);
        CHECK(text.find("mod dirs: none") != std::string::npos);
        CHECK(text.find("== result: OK") != std::string::npos);
    }
    {  // with the mod
        const ModCheckReport r = alamo::game::check_mod_data(foc.string(), {"MODPATH=" + mod.string()});
        CHECK(r.loaded);
        CHECK(r.mod_dirs.size() == 1);
        const auto* xml = folder(r, "XML");
        CHECK(xml && xml->added == 1 && xml->overrides == 1);  // ModCampaigns.xml new, CampaignFiles.xml replaced
        const auto* scripts = folder(r, "SCRIPTS");
        CHECK(scripts && scripts->added == 2 && scripts->overrides == 1);
        const auto* art = folder(r, "ART");
        CHECK(art && art->added == 1 && art->overrides == 0);
        CHECK(r.counts.campaigns == 3);
        CHECK(r.campaigns_checked == 3);
        CHECK(r.campaigns_without_planets == std::vector<std::string>{"Empty_GC"});  // a warning: two others have planets
        CHECK(!r.no_startable_campaign());
        CHECK(has(r.campaign_warnings, "MC_Missing"));  // unknown planet skipped: a warning, one line per message
        CHECK(has(r.campaign_warnings, "(1 campaign: Mod_GC)"));
        CHECK(!has(r.campaign_warnings, "Base_GC"));
        CHECK(r.campaign_warning_count >= 1);
        CHECK(r.lua_scripts == 3);
        CHECK(r.lua_errors.size() == 1 && has(r.lua_errors, "MOD_BROKEN.LUA"));  // a warning
        CHECK(r.lua_errors.size() == 1 && r.lua_errors[0].in_mod);
        CHECK(r.lua_run == 2 && r.lua_run_errors.empty());  // the two story scripts that compile
        CHECK(r.error_count() == 0);  // Empty_GC is a menu entry only
        CHECK(r.warning_count() >= 2);
        const std::string text = alamo::game::format_mod_report(r);
        CHECK(text.find("mod files: 4 new, 2 replace") != std::string::npos);
        CHECK(text.find("== result: OK, 0 errors") != std::string::npos);
        CHECK(text.find("1 without planets (warnings: menu entries") != std::string::npos);
    }
    {  // options off
        alamo::game::ModCheckOptions o;
        o.campaigns = o.lua = false;
        const ModCheckReport r = alamo::game::check_mod_data(foc.string(), {"MODPATH=" + mod.string()}, o);
        CHECK(r.loaded && r.campaigns_checked == 0 && r.lua_scripts == 0 && r.error_count() == 0);
    }
    {  // modc t05: a second mod with the report's other warnings - missing files, an unknown tag, Lua load errors
        const fs::path mod2 = root / "Mods" / "MC2";
        const fs::path x = mod2 / "Data/XML";
        write_file(x / "GameObjectFiles.xml",
                   "<GameObjectFiles><File>MCPlanets.xml</File><File>MC2Units.xml</File><File>MC2Ghost.xml</File></GameObjectFiles>\n");
        write_file(x / "MC2Units.xml",
                   "<Units><SpaceUnit Name=\"MC2_Ship\"><Space_Model_Name>MC2_Missing.alo</Space_Model_Name>"
                   "<MC2_Not_A_Real_Tag>1</MC2_Not_A_Real_Tag></SpaceUnit>\n"
                   "<SpaceUnit Name=\"MC2_Ship_B\"><Space_Model_Name>mc2_missing.ALO</Space_Model_Name>"
                   "<MC2_Not_A_Real_Tag>2</MC2_Not_A_Real_Tag></SpaceUnit>\n"
                   "<SpaceUnit Name=\"MC2_Ok\"><Land_Model_Name>MC2_Present</Land_Model_Name></SpaceUnit></Units>\n");
        write_file(mod2 / "Data/Art/Models/MC2_Present.alo", "x");  // no extension in the XML: .ALO tried
        // The message carries a byte that is not UTF-8 (Lua \233 = 0xE9, Latin-1 e acute): the JSON must stay valid.
        write_file(mod2 / "Data/Scripts/Story/MC2_Errors.lua", "error(\"MC2 load error caf\\233\")\n");
        write_file(mod2 / "Data/Scripts/Story/MC2_Requires.lua", "require(\"MC2_No_Such_Library\")\n");
        const ModCheckReport r = alamo::game::check_mod_data(foc.string(), {"MODPATH=" + mod2.string()});
        CHECK(r.loaded && r.error_count() == 0);
        CHECK(r.missing_files.size() == 2);
        if (r.missing_files.size() == 2) {  // sorted by path
            CHECK(r.missing_files[0].path == "DATA\\ART\\MODELS\\MC2_MISSING.ALO");
            CHECK(r.missing_files[0].references == 2);
            CHECK(r.missing_files[0].referenced_by == "MC2_Ship (Space_Model_Name)");
            CHECK(r.missing_files[0].in_mod && r.missing_files[1].in_mod);
            CHECK(r.missing_files[1].path == "DATA\\XML\\MC2GHOST.XML");
            CHECK(r.missing_files[1].referenced_by.find("GAMEOBJECTFILES.XML") != std::string::npos);
        }
        CHECK(r.unknown_xml_tags.size() == 1);
        if (!r.unknown_xml_tags.empty()) {
            CHECK(r.unknown_xml_tags[0].tag == "MC2_Not_A_Real_Tag");
            CHECK(r.unknown_xml_tags[0].occurrences == 2 && r.unknown_xml_tags[0].types == 2);
            CHECK(r.unknown_xml_tags[0].first_file.find("MC2Units.xml") != std::string::npos);
            CHECK(r.unknown_xml_tags[0].in_mod);
        }
        CHECK(r.lua_scripts == 3 && r.lua_run == 3);
        CHECK(r.lua_run_errors.size() == 2);
        CHECK(has(r.lua_run_errors, "MC2_ERRORS.LUA") && has(r.lua_run_errors, "MC2 load error"));
        CHECK(has(r.lua_run_errors, "MC2_REQUIRES.LUA"));
        for (const auto& e : r.lua_run_errors) CHECK(e.in_mod);
        CHECK(r.warning_count() >= 5);
        const std::string text = alamo::game::format_mod_report(r);
        CHECK(text.find("== missing files\n2 files") != std::string::npos);
        CHECK(text.find("MC2_Not_A_Real_Tag") != std::string::npos);
        const std::string js = alamo::game::format_mod_report_json(r);
        CHECK(js.find("\"path\": \"DATA\\\\ART\\\\MODELS\\\\MC2_MISSING.ALO\"") != std::string::npos);
        CHECK(js.find("\"tag\": \"MC2_Not_A_Real_Tag\"") != std::string::npos);
        CHECK(js.find("\"error\": ") != std::string::npos);
        CHECK(js.find("MC2 load error caf\\u00e9") != std::string::npos);
        CHECK(js.find('\xE9') == std::string::npos);
        alamo::game::ModCheckOptions o;
        o.files = o.xml_tags = o.lua_run = false;
        const ModCheckReport q = alamo::game::check_mod_data(foc.string(), {"MODPATH=" + mod2.string()}, o);
        CHECK(q.missing_files.empty() && q.unknown_xml_tags.empty() && q.lua_run == 0 && q.lua_run_errors.empty());
        CHECK(q.lua_scripts == 3);
    }
    {  // not a folder: load fails, reported as one error
        const ModCheckReport r = alamo::game::check_mod_data((root / "no_such_dir").string(), {});
        CHECK(!r.loaded && !r.error.empty() && r.error_count() == 1);
        CHECK(alamo::game::format_mod_report(r).find("LOAD FAILED") != std::string::npos);
    }
    fs::remove_all(root, ec);
}

void test_shipped_data() {
    const char* foc = std::getenv("ALAMO_FOC_DATA");
    if (!foc || !fs::exists(fs::path(foc) / "Data")) {
        std::printf("shipped data: skipped (ALAMO_FOC_DATA not set)\n");
        return;
    }
    alamo::game::ModCheckOptions o;
    o.lua = false;  // the script compile pass is covered above; FoC's are shipped as source in MEGs and take a while
    const ModCheckReport r = alamo::game::check_mod_data(foc, {}, o);
    CHECK(r.loaded);
    CHECK(r.counts.campaigns > 0 && r.counts.object_types > 1000);
    CHECK(r.counts.official_maps == 48);
    CHECK(r.error_count() == 0);
    if (r.error_count()) std::fputs(alamo::game::format_mod_report(r).c_str(), stdout);
}

}  // namespace

int main() {
    test_fake_install();
    test_shipped_data();
    std::printf(g_fail ? "alamo_game_mod_check_tests: %d FAILED\n" : "alamo_game_mod_check_tests: OK\n", g_fail);
    return g_fail ? 1 : 0;
}
