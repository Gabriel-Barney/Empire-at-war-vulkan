// alamo_game_mod_fixture_tests (modc t07): the synthetic mod of testdata/mod_fixture (README there) loaded through the real
// MODPATH= route - load_game_data -> vfs::parse_command_line (relative to the FoC folder) -> the VFS mod layer - with no
// game data. Checks the load report summary (mod_check.hpp, docs/MODS.md §5.1) with and without the mod, and that the
// mod's faction, unit, planet, campaign and Lua override reach the loaded data. Read only: nothing is written anywhere.
//   alamo_game_mod_fixture_tests <testdata/mod_fixture>
#include <cstdio>
#include <string>
#include <vector>

#include "alamo/core/hash.hpp"
#include "alamo/game/game_data.hpp"
#include "alamo/game/mod_check.hpp"

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
        CHECK(contains(js, "\"version\": 1"));
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

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: alamo_game_mod_fixture_tests <testdata/mod_fixture>\n");
        return 2;
    }
    const std::string foc = std::string(argv[1]) + "/corruption";
    test_base(foc);
    test_mod(foc);
    std::printf(g_fail ? "alamo_game_mod_fixture_tests: %d FAILED\n" : "alamo_game_mod_fixture_tests: OK\n", g_fail);
    return g_fail ? 1 : 0;
}
