// alamo_game_mod_picker_tests (modc t08): the mod picker's data side (mod_picker.hpp) on the read-only fixture of
// testdata/mod_fixture (its Mods folders and fake Steam library): the installed-mod list, modinfo.json names, the report
// summary, the launch arguments of a mod and of a likely submod (stacked on its parent), and the relaunch command line.
// modinfo.json decoding also runs on files this test writes to its own temp folder (%TEMP%\alamo_mod_picker_tests).
//   alamo_game_mod_picker_tests <testdata/mod_fixture>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "alamo/game/mod_check.hpp"
#include "alamo/game/mod_picker.hpp"

namespace game = alamo::game;
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

bool has_line(const std::vector<std::string>& lines, const std::string& needle) {
    for (const std::string& l : lines)
        if (l.find(needle) != std::string::npos) return true;
    return false;
}

void test_list(const std::string& fixture, const std::string& foc) {
    const std::vector<std::string> libs = {fixture + "/steamlib", fixture + "/steamlib"};  // a duplicate library: each id once
    const std::vector<game::InstalledMod> mods = game::list_installed_mods(foc, libs);
    CHECK(mods.size() == 4);
    if (mods.size() != 4) return;
    CHECK(mods[0].id == "900000001" && mods[0].workshop && mods[0].name == "900000001" && mods[0].arg == "STEAMMOD=900000001");
    CHECK(mods[1].id == "FixtureChecks" && !mods[1].workshop && mods[1].arg == "MODPATH=Mods\\FixtureChecks");
    CHECK(mods[2].id == "FixtureMod" && mods[2].name == "FixtureMod");
    CHECK(mods[3].id == "FixtureSub" && mods[3].name == "Fixture Sub");  // modinfo.json "name"
    CHECK(game::list_installed_mods(foc, {}).size() == 3);
    CHECK(game::list_installed_mods(fixture + "/no_such_game", {}).empty());
}

void test_modinfo() {
    const fs::path dir = fs::temp_directory_path() / "alamo_mod_picker_tests";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const auto name_of = [&](const std::string& json) {
        std::ofstream(dir / "modinfo.json", std::ios::binary) << json;
        return game::modinfo_name(dir.string());
    };
    CHECK(name_of("{ \"name\": \"Plain\" }") == "Plain");
    CHECK(name_of("\xEF\xBB\xBF{\"steamdata\": {\"name\": \"inner\"}, \"name\": \"Outer \\\"Q\\\" \\u00e9\\\\\"}") ==
          "Outer \"Q\" \xC3\xA9\\");
    CHECK(name_of("{\"summary\": \"name\", \"x\": [\"name\"], \"name\" : \"Late\"}") == "Late");
    CHECK(name_of("{\"title\": \"no name key\"}").empty());
    CHECK(name_of("{\"name\": 5}").empty());
    CHECK(name_of("not json").empty());
    fs::remove_all(dir, ec);
    CHECK(game::modinfo_name((dir / "missing").string()).empty());
}

void test_args() {
    CHECK(game::is_mod_arg("MODPATH=Mods\\X") && game::is_mod_arg("modpath=x") && game::is_mod_arg("SteamMod=1"));
    CHECK(!game::is_mod_arg("FALLBACKPATH=x") && !game::is_mod_arg("--game") && !game::is_mod_arg("MODPATH"));
    const std::vector<std::string> argv = {"alamo_game.exe", "--game", "C:\\Games\\FoC", "MODPATH=Mods\\Old", "--no-intro",
                                           "steammod=123", "FALLBACKPATH=x"};
    const std::vector<std::string> mod = {"STEAMMOD=456", "STEAMMOD=789"};
    CHECK((game::relaunch_args(argv, mod) ==
           std::vector<std::string>{"alamo_game.exe", "--game", "C:\\Games\\FoC", "--no-intro", "FALLBACKPATH=x", "STEAMMOD=456", "STEAMMOD=789"}));
    CHECK((game::relaunch_args(argv, {}) ==
           std::vector<std::string>{"alamo_game.exe", "--game", "C:\\Games\\FoC", "--no-intro", "FALLBACKPATH=x"}));
    // CommandLineToArgvW quoting: spaces, quotes, trailing backslashes, empty arguments.
    const std::vector<std::string> q = {"C:\\Program Files\\a.exe", "plain", "", "say \"hi\"", "C:\\dir with space\\", "a\\\\b"};
    CHECK(game::windows_command_line(q) ==
          "\"C:\\Program Files\\a.exe\" plain \"\" \"say \\\"hi\\\"\" \"C:\\dir with space\\\\\" a\\\\b");
}

void test_report(const std::string& fixture, const std::string& foc) {
    const std::vector<game::InstalledMod> mods = game::list_installed_mods(foc, std::vector<std::string>{fixture + "/steamlib"});
    if (mods.size() != 4) return;
    // A plain mod: its own argument; the summary says it loads cleanly.
    const game::ModCheckReport plain = game::check_mod_data(foc, {mods[2].arg});
    CHECK((game::mod_launch_args(mods[2], &plain) == std::vector<std::string>{"MODPATH=Mods\\FixtureMod"}));
    CHECK((game::mod_launch_args(mods[2], nullptr) == std::vector<std::string>{"MODPATH=Mods\\FixtureMod"}));
    const std::vector<std::string> ps = game::mod_report_summary(plain);
    CHECK(!ps.empty() && ps[0].rfind("Loads: OK, 0 errors", 0) == 0);
    CHECK(has_line(ps, "factions") && has_line(ps, "Lua: ") && !has_line(ps, "Submod"));
    // A likely submod: launched stacked on its parent (the report's stacked arguments).
    game::ModCheckOptions opt;
    opt.workshop_libraries = {fixture + "/steamlib"};
    const game::ModCheckReport sub = game::check_mod_data(foc, {mods[3].arg}, opt);
    CHECK((game::mod_launch_args(mods[3], &sub) == std::vector<std::string>{"MODPATH=Mods\\FixtureSub", "MODPATH=Mods\\FixtureMod"}));
    const std::vector<std::string> ss = game::mod_report_summary(sub);
    CHECK(has_line(ss, "Submod of FixtureMod (Mods folder): it has 2 of 2 missing files"));
    CHECK(has_line(ss, "Launch stacks it on MODPATH=Mods\\FixtureMod"));
    CHECK(has_line(ss, "2 missing file(s), 2 named by the mod"));
    if (g_fail)
        for (const std::string& l : ss) std::printf("  %s\n", l.c_str());
    // A load that fails says so first.
    const game::ModCheckReport bad = game::check_mod_data(fixture + "/no_such_game", {});
    const std::vector<std::string> bs = game::mod_report_summary(bad);
    CHECK(!bad.loaded && !bs.empty() && bs[0].rfind("Does not load: ", 0) == 0);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: alamo_game_mod_picker_tests <testdata/mod_fixture>\n");
        return 2;
    }
    const std::string fixture = argv[1], foc = fixture + "/corruption";
    test_list(fixture, foc);
    test_modinfo();
    test_args();
    test_report(fixture, foc);
    std::printf(g_fail ? "alamo_game_mod_picker_tests: %d FAILED\n" : "alamo_game_mod_picker_tests: OK\n", g_fail);
    return g_fail ? 1 : 0;
}
