// alamo_game_mod_fixture_tests (modc t07): the synthetic mod of testdata/mod_fixture (README there) loaded through the real
// MODPATH= route - load_game_data -> vfs::parse_command_line (relative to the FoC folder) -> the VFS mod layer - with no
// game data. Checks the load report summary (mod_check.hpp, docs/MODS.md §5.1) with and without the mod, and that the
// mod's faction, unit, planet, campaign and Lua override reach the loaded data. Two more fixture mods cover the report's
// findings: FixtureChecks (unknown tags in the non-object databases, modc t15; a main chunk that fails only for want of
// session state and one that really fails, t12) and FixtureSub (a submod of FixtureMod, t16; fake Steam library under
// testdata/mod_fixture/steamlib). Read only: nothing is written anywhere.
//   alamo_game_mod_fixture_tests <testdata/mod_fixture>
#include <cstdio>
#include <string>
#include <vector>

#include "alamo/core/hash.hpp"
#include "alamo/game/game_data.hpp"
#include "alamo/game/mod_check.hpp"
#include "alamo/test/support.hpp"

using alamo::game::ModCheckReport;

using ::alamo::test::g_fail;

namespace {

const alamo::game::ModFolderStats* folder(const ModCheckReport& r, const std::string& name) {
    for (const auto& f : r.mod_folders)
        if (f.folder == name) return &f;
    return nullptr;
}

bool contains(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }

// What every clean load of the fixture shares: no errors, nothing missing, no unknown tags, every script compiles and
// every entry script's main chunk runs without an error.
void check_clean(const ModCheckReport& r) {
    CHECK(r.loaded);
    CHECK(r.error_count() == 0);
    CHECK(r.check_errors.empty());
    for (const auto& s : r.stages) CHECK(s.ok);
    CHECK(r.campaigns_without_planets.empty());
    CHECK(r.campaign_warnings.empty());
    CHECK(r.missing_files.empty());
    CHECK(r.unknown_xml_tags.empty());
    CHECK(r.lua_errors.empty());
    CHECK(r.lua_run_errors.empty());
    CHECK(r.lua_session_errors.empty());
    CHECK(r.submod.missing == 0 && !r.submod.likely && r.submod.parents.empty());
    if (g_fail) std::fputs(alamo::game::format_mod_report(r, true).c_str(), stdout);
}

void test_base(const std::string& foc) {
    const ModCheckReport r = alamo::game::check_mod_data(foc, {});
    check_clean(r);
    CHECK(r.mod_dirs.empty() && r.mod_folders.empty());
    CHECK(r.counts.factions == 2);
    CHECK(r.counts.planets == 2);
    CHECK(r.counts.object_types == 3);
    CHECK(r.counts.campaigns == 1);
    CHECK(r.lua_scripts == 2);
    CHECK(r.lua_run == 1);  // Base_Story; FixtureLib is a Library script (reached through require)
    const std::string js = alamo::game::format_mod_report_json(r);
    CHECK(contains(js, "\"result\": \"ok\""));
    CHECK(contains(js, "\"mod_dirs\": []"));
}

void test_mod(const std::string& foc) {
    // Retail spelling: relative to the FoC folder, either separator.
    for (const char* arg : {"MODPATH=Mods\\FixtureMod", "MODPATH=Mods/FixtureMod"}) {
        const ModCheckReport r = alamo::game::check_mod_data(foc, {arg});
        check_clean(r);
        CHECK(r.args == std::vector<std::string>{arg});
        CHECK(r.mod_dirs.size() == 1);
        CHECK(r.mod_dirs.size() == 1 && contains(r.mod_dirs[0], "FixtureMod"));
        CHECK(r.counts.factions == 3);      // + Mod_Alliance
        CHECK(r.counts.planets == 3);       // + Mod_Prime
        CHECK(r.counts.object_types == 5);  // + Mod_Prime, Mod_Corvette
        CHECK(r.counts.campaigns == 2);     // + Mod_GC
        CHECK(r.campaigns_checked == 2);
        CHECK(r.lua_scripts == 3);          // FixtureLib once (the mod's copy), Base_Story, Mod_Story
        CHECK(r.lua_run == 2);              // the two story scripts (each requires FixtureLib)
        const auto* xml = folder(r, "XML");
        CHECK(xml && xml->added == 3 && xml->overrides == 3);  // Mod*.xml new; the three list files replaced
        const auto* scripts = folder(r, "SCRIPTS");
        CHECK(scripts && scripts->added == 1 && scripts->overrides == 1);  // Mod_Story new; FixtureLib replaced
        const auto* art = folder(r, "ART");
        CHECK(art && art->added == 1 && art->overrides == 0);
        CHECK(r.mod_folders.size() == 3);

        // The machine-readable summary.
        const std::string js = alamo::game::format_mod_report_json(r);
        CHECK(contains(js, "\"schema\": \"alamo.mod_report\""));
        CHECK(contains(js, "\"version\": 3"));
        CHECK(contains(js, "\"result\": \"ok\""));
        CHECK(contains(js, "\"errors\": 0"));
        CHECK(contains(js, "\"added\": 5"));
        CHECK(contains(js, "\"replaced\": 4"));
        CHECK(contains(js, "\"factions\": 3"));
        CHECK(contains(js, "\"object_types\": 5"));
        CHECK(contains(js, "\"missing_files\": []"));
        CHECK(contains(js, "\"unknown_xml_tags\": []"));
        CHECK(contains(js, "\"compile_errors\": []"));
        CHECK(contains(js, "\"run_errors\": []"));
        CHECK(contains(js, "\"session_errors\": []"));
        CHECK(contains(js, "\"unused_scripts\": []"));  // modc t24
        CHECK(contains(js, "\"shaders\": {"));           // modc t21: the fixture ships no effect
        CHECK(r.shaders.checked && r.shaders.effects == 0 && r.shaders.issues.empty());
        CHECK(contains(js, "\"likely\": false"));
        CHECK(contains(js, "\"chunks_run\": 2"));
        if (g_fail) std::fputs(js.c_str(), stdout);
    }

    // The mod's content in the loaded data itself.
    alamo::game::GameData d;
    std::string err;
    CHECK(alamo::game::load_game_data(foc, {"MODPATH=Mods\\FixtureMod"}, d, &err));
    bool faction = false;
    for (const auto& f : d.rules.factions) faction |= alamo::iequals(f.name, "Mod_Alliance");
    CHECK(faction);
    CHECK(d.raw.find("Mod_Corvette") != alamo::objects::kNone);
    CHECK(d.raw.find("Mod_Prime") != alamo::objects::kNone);
    std::vector<uint8_t> lib;
    CHECK(d.files->read("Data\\Scripts\\Library\\FixtureLib.lua", lib));
    CHECK(std::string(lib.begin(), lib.end()).find("Fixture_Lib_Version = 2") != std::string::npos);

    // A MODPATH that names no folder: nothing mounted, the base still loads, a VFS warning.
    const ModCheckReport none = alamo::game::check_mod_data(foc, {"MODPATH=Mods\\NoSuchMod"});
    CHECK(none.loaded && none.error_count() == 0 && none.counts.factions == 2);
    bool warned = false;
    for (const auto& w : none.vfs_warnings) warned |= contains(w, "no Data folder");
    CHECK(warned);
}

const alamo::game::UnknownXmlTag* unknown_tag(const ModCheckReport& r, const std::string& database, const std::string& tag) {
    for (const auto& t : r.unknown_xml_tags)
        if (t.database == database && t.tag == tag) return &t;
    return nullptr;
}

// modc t12 / t15: FixtureChecks.
void test_checks(const std::string& foc) {
    const ModCheckReport r = alamo::game::check_mod_data(foc, {"MODPATH=Mods\\FixtureChecks"});
    CHECK(r.loaded && r.error_count() == 0);
    CHECK(r.missing_files.empty());
    CHECK(r.submod.missing == 0 && !r.submod.likely);
    // A campaign with no Locations next to ones with planets: a menu entry (retail lists it), a warning.
    CHECK(r.campaigns_checked == 3);
    CHECK(r.campaigns_without_planets == std::vector<std::string>{"Checks_Menu_Header"});
    CHECK(!r.no_startable_campaign());

    // t15: one unread tag per database, each in the mod; the tags the original's tables or the port's loaders read are
    // not listed (Is_Playable, Locations, Damage_Types, Ships_Per_Stack, Files, Volume_Percent).
    CHECK(r.unknown_xml_tags.size() == 4);
    const auto* faction = unknown_tag(r, "factions", "Checks_Unread_Faction_Tag");
    CHECK(faction && faction->in_mod && faction->occurrences == 1 && faction->types == 1);
    CHECK(faction && contains(faction->first_file, "ChecksFactions.xml"));
    const auto* campaign = unknown_tag(r, "campaigns", "Checks_Unread_Campaign_Tag");
    CHECK(campaign && campaign->in_mod && campaign->occurrences == 1);
    const auto* constant = unknown_tag(r, "game constants", "Checks_Unread_Constant");
    CHECK(constant && constant->in_mod && constant->types == 1);
    const auto* speech = unknown_tag(r, "speech events", "Checks_Unread_Speech_Tag");
    CHECK(speech && speech->in_mod && speech->occurrences == 2 && speech->types == 2);
    CHECK(!r.unknown_xml_tags.empty() && r.unknown_xml_tags.front().tag == "Checks_Unread_Speech_Tag");  // most uses first

    // t12: Checks_Session reads CHECKS_MOD_ID, which Checks_Scoring sets only in Base_Definitions (session state);
    // Checks_Broken reads a key no script sets.
    CHECK(r.lua_run == 4);  // Base_Story, Checks_Broken, Checks_Session, Checks_Scoring
    CHECK(r.lua_run_errors.size() == 1);
    CHECK(r.lua_run_errors.size() == 1 && contains(r.lua_run_errors[0].path, "CHECKS_BROKEN.LUA"));
    CHECK(r.lua_run_errors.size() == 1 && r.lua_run_errors[0].global_value.empty());
    CHECK(r.lua_session_errors.size() == 1);
    if (r.lua_session_errors.size() == 1) {
        const alamo::game::LuaIssue& e = r.lua_session_errors[0];
        CHECK(contains(e.path, "CHECKS_SESSION.LUA") && e.in_mod);
        CHECK(e.global_value == "CHECKS_MOD_ID");
        CHECK(contains(e.set_by, "MISCELLANEOUS\\CHECKS_SCORING.LUA"));
    }
    // Session-dependent errors are not counted as warnings.
    CHECK(r.warning_count() == r.vfs_warnings.size() + r.diagnostics.size() + r.campaigns_without_planets.size() +
                                   r.campaign_warning_count + r.unknown_xml_tags.size() + r.lua_run_errors.size());

    const std::string text = alamo::game::format_mod_report(r);
    CHECK(contains(text, "1 more fail only for session state"));
    CHECK(contains(text, "GlobalValue \"CHECKS_MOD_ID\" (set by DATA\\SCRIPTS\\MISCELLANEOUS\\CHECKS_SCORING.LUA): 1 scripts"));
    CHECK(contains(text, "speech events"));
    const std::string js = alamo::game::format_mod_report_json(r);
    CHECK(contains(js, "\"database\": \"factions\""));
    CHECK(contains(js, "\"global_value\": \"CHECKS_MOD_ID\""));
    CHECK(contains(js, "\"set_by\": \"DATA\\\\SCRIPTS\\\\MISCELLANEOUS\\\\CHECKS_SCORING.LUA\""));
    if (g_fail) std::fputs(text.c_str(), stdout);

    // Each check has its own GlobalValue store (not the process-wide one): a second check finds the same.
    const ModCheckReport again = alamo::game::check_mod_data(foc, {"MODPATH=Mods\\FixtureChecks"});
    CHECK(again.lua_run_errors.size() == 1 && again.lua_session_errors.size() == 1);
}

// modc t16: FixtureSub's lists name ModObjects.xml and ModFactions.xml, which FixtureMod has; workshop item 900000001
// (named in FixtureSub's modinfo.json) has ModFactions.xml only.
void test_submod(const std::string& fixture, const std::string& foc) {
    alamo::game::ModCheckOptions opt;
    opt.workshop_libraries = {fixture + "/steamlib"};
    const ModCheckReport r = alamo::game::check_mod_data(foc, {"MODPATH=Mods\\FixtureSub"}, opt);
    CHECK(r.loaded);
    const alamo::game::SubmodHint& h = r.submod;
    CHECK(h.missing == 2);
    CHECK(h.likely);
    CHECK(h.parents.size() == 2);  // FixtureChecks has neither file: not listed
    if (h.parents.size() == 2) {
        CHECK(h.parents[0].id == "FixtureMod" && !h.parents[0].workshop && h.parents[0].provides == 2);
        CHECK(h.parents[0].arg == "MODPATH=Mods\\FixtureMod" && !h.parents[0].mentioned);
        CHECK(h.parents[1].id == "900000001" && h.parents[1].workshop && h.parents[1].provides == 1);
        CHECK(h.parents[1].arg == "STEAMMOD=900000001" && h.parents[1].mentioned);
    }
    CHECK((h.stacked_args == std::vector<std::string>{"MODPATH=Mods\\FixtureSub", "MODPATH=Mods\\FixtureMod"}));
    const std::string text = alamo::game::format_mod_report(r);
    CHECK(contains(text, "likely a submod"));
    CHECK(contains(text, "check it stacked: MODPATH=Mods\\FixtureSub MODPATH=Mods\\FixtureMod"));
    const std::string js = alamo::game::format_mod_report_json(r);
    CHECK(contains(js, "\"likely\": true"));
    CHECK(contains(js, "\"missing_list_entries\": 2"));
    CHECK(contains(js, "\"kind\": \"workshop\""));
    CHECK(contains(js, "\"mentioned\": true"));
    if (g_fail) std::fputs(text.c_str(), stdout);

    // Without the workshop library only the local parent is considered.
    const ModCheckReport local = alamo::game::check_mod_data(foc, {"MODPATH=Mods\\FixtureSub"});
    CHECK(local.submod.likely && local.submod.parents.size() == 1);

    // Stacked as the hint says: nothing missing, no hint, the submod's unit loads.
    const ModCheckReport stacked = alamo::game::check_mod_data(foc, h.stacked_args, opt);
    CHECK(stacked.loaded && stacked.error_count() == 0);
    CHECK(stacked.missing_files.empty());
    CHECK(stacked.submod.missing == 0 && !stacked.submod.likely);
    CHECK(stacked.counts.object_types == 6);  // base 3 + Mod_Prime, Mod_Corvette + Sub_Frigate
    if (g_fail) std::fputs(alamo::game::format_mod_report(stacked, true).c_str(), stdout);
}

// Campaigns without planets are an error only when no checked campaign has planets.
void test_campaign_levels() {
    ModCheckReport r;
    r.loaded = true;
    r.campaigns_checked = 2;
    r.campaigns_without_planets = {"Header_A", "Header_B"};
    CHECK(r.no_startable_campaign());
    CHECK(r.error_count() == 1 && r.warning_count() == 0);
    CHECK(contains(alamo::game::format_mod_report(r), "no campaign can be started"));
    r.campaigns_checked = 3;
    CHECK(!r.no_startable_campaign());
    CHECK(r.error_count() == 0 && r.warning_count() == 2);
    r.campaigns_checked = 0;
    r.campaigns_without_planets.clear();
    CHECK(!r.no_startable_campaign() && r.error_count() == 0);  // no campaigns at all (a skirmish-only mod)
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: alamo_game_mod_fixture_tests <testdata/mod_fixture>\n");
        return 2;
    }
    const std::string foc = std::string(argv[1]) + "/corruption";
    test_base(foc);
    test_mod(foc);
    test_checks(foc);
    test_campaign_levels();
    test_submod(argv[1], foc);
    std::printf(g_fail ? "alamo_game_mod_fixture_tests: %d FAILED\n" : "alamo_game_mod_fixture_tests: OK\n", g_fail);
    return g_fail ? 1 : 0;
}
