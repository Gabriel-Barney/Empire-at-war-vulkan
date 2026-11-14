// alamo_mod_check - Q5 / modc t05 mod load report (alamo/game/mod_check.hpp): loads the data as alamo_game does and
// reports files, loader steps, content counts, campaign maps, missing files, unknown XML tags, Lua compile / load errors
// unbound engine calls and shader issues; optionally the machine-readable summary (docs/MODS.md §5.1).
//
//   alamo_mod_check [--game DIR] [MODPATH=..] [STEAMMOD=..] [FALLBACKPATH=..] [--out FILE] [--json FILE|-] [--verbose]
//                   [--no-campaigns] [--no-lua] [--no-lua-run] [--no-files] [--no-xml-tags] [--no-submod]
//                   [--no-shaders] [--no-dxc]
//   --game defaults to ALAMO_FOC_DATA. --json - writes the JSON to stdout instead of the text report.
//   STEAMMOD=<id> is found in the Steam library folders (alamo/vfs/steam_workshop.hpp); the workshop items of the same
//   folders and <game>\Mods\* are the parents the submod check considers (modc t16).
//   --no-shaders skips the effect audit (modc t21); --no-dxc keeps it to translation (no runtime DXC compile).
//   Exit 0: no errors (warnings allowed), 1: errors, 2: usage.
//   Example (a Steam workshop mod):
//     alamo_mod_check --game "...\corruption" STEAMMOD=1125571106 --json report.json
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "alamo/game/mod_check.hpp"
#include "alamo/vfs/steam_workshop.hpp"

namespace {

int usage() {
    std::fprintf(stderr,
                 "usage: alamo_mod_check [--game DIR] [MODPATH=..] [STEAMMOD=..] [FALLBACKPATH=..] [--out FILE] [--json FILE|-]\n"
                 "                       [--verbose] [--no-campaigns] [--no-lua] [--no-lua-run] [--no-files] [--no-xml-tags]\n"
                 "                       [--no-submod] [--no-shaders] [--no-dxc]\n");
    return 2;
}

bool write_file(const std::string& path, const std::string& text) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        std::fprintf(stderr, "cannot write %s\n", path.c_str());
        return false;
    }
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    alamo::vfs::install_steam_workshop_lookup();  // modc t10: STEAMMOD= from the Steam library folders
    std::string game, out, json;
    std::vector<std::string> args;
    bool verbose = false;
    alamo::game::ModCheckOptions opt;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--game" && i + 1 < argc) game = argv[++i];
        else if (a == "--out" && i + 1 < argc) out = argv[++i];
        else if (a == "--json" && i + 1 < argc) json = argv[++i];
        else if (a == "--verbose") verbose = true;
        else if (a == "--no-campaigns") opt.campaigns = false;
        else if (a == "--no-lua") opt.lua = false;
        else if (a == "--no-lua-run") opt.lua_run = false;
        else if (a == "--no-files") opt.files = false;
        else if (a == "--no-xml-tags") opt.xml_tags = false;
        else if (a == "--no-submod") opt.submod = false;
        else if (a == "--no-shaders") opt.shaders = false;
        else if (a == "--no-dxc") opt.shader_compile = false;
        else if (a.find('=') != std::string::npos) args.push_back(a);  // MODPATH= etc.
        else return usage();
    }
    if (game.empty())
        if (const char* env = std::getenv("ALAMO_FOC_DATA")) game = env;
    if (game.empty()) return usage();
    if (opt.submod) opt.workshop_libraries = alamo::vfs::find_steam_libraries(game, alamo::vfs::default_steam_roots());

    const alamo::game::ModCheckReport report = alamo::game::check_mod_data(game, args, opt);
    const std::string text = alamo::game::format_mod_report(report, verbose);
    if (json != "-") std::fputs(text.c_str(), stdout);
    if (!out.empty() && !write_file(out, text)) return 1;
    if (!json.empty()) {
        const std::string js = alamo::game::format_mod_report_json(report);
        if (json == "-") std::fputs(js.c_str(), stdout);
        else if (!write_file(json, js)) return 1;
    }
    return report.error_count() ? 1 : 0;
}
