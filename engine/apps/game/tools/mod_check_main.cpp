// alamo_mod_check - Q5 mod diagnostics (alamo/game/mod_check.hpp): loads the data as alamo_game does and reports files,
// loader steps, content counts, campaign maps and Lua compile errors.
//
//   alamo_mod_check [--game DIR] [MODPATH=..] [STEAMMOD=..] [FALLBACKPATH=..] [--out FILE] [--verbose]
//                   [--no-campaigns] [--no-lua]
//   --game defaults to ALAMO_FOC_DATA. Exit 0: no errors (warnings allowed), 1: errors, 2: usage.
//   Example (Thrawn's Revenge from the Steam workshop):
//     alamo_mod_check --game "...\corruption" MODPATH="...\workshop\content\32470\1125571106"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "alamo/game/mod_check.hpp"

namespace {

int usage() {
    std::fprintf(stderr,
                 "usage: alamo_mod_check [--game DIR] [MODPATH=..] [STEAMMOD=..] [FALLBACKPATH=..] [--out FILE] [--verbose]\n"
                 "                       [--no-campaigns] [--no-lua]\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    std::string game, out;
    std::vector<std::string> args;
    bool verbose = false;
    alamo::game::ModCheckOptions opt;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--game" && i + 1 < argc) game = argv[++i];
        else if (a == "--out" && i + 1 < argc) out = argv[++i];
        else if (a == "--verbose") verbose = true;
        else if (a == "--no-campaigns") opt.campaigns = false;
        else if (a == "--no-lua") opt.lua = false;
        else if (a.find('=') != std::string::npos) args.push_back(a);  // MODPATH= etc.
        else return usage();
    }
    if (game.empty())
        if (const char* env = std::getenv("ALAMO_FOC_DATA")) game = env;
    if (game.empty()) return usage();

    const alamo::game::ModCheckReport report = alamo::game::check_mod_data(game, args, opt);
    const std::string text = alamo::game::format_mod_report(report, verbose);
    std::fputs(text.c_str(), stdout);
    if (!out.empty()) {
        FILE* f = std::fopen(out.c_str(), "wb");
        if (!f) {
            std::fprintf(stderr, "cannot write %s\n", out.c_str());
            return 1;
        }
        std::fwrite(text.data(), 1, text.size(), f);
        std::fclose(f);
    }
    return report.error_count() ? 1 : 0;
}
