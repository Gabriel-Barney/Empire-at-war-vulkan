// alamo_session_run - q6 t02: starts a GameSession (render-free, as alamo_game does after the menus) on any data set and
// ticks it, printing the players, their AI types, the starting forces and how far the session got. For trying mods
// without the menus.
//
//   alamo_session_run [--game DIR] [MODPATH=..] (--campaign NAME [--faction F] | --skirmish MAP --human F --ai F)
//                     [--ticks N] [--seed S] [--difficulty 0..2] [--workers N] [--no-galactic-ai] [--no-tactical-ai]
//   --game defaults to ALAMO_FOC_DATA. Exit 0: started and ticked, 1: load / start failed, 2: usage.
//   Example (Thrawn's Revenge): alamo_session_run --game "...\corruption" MODPATH="...\32470\1125571106"
//                               --campaign Thrawn_Empire --ticks 3000
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "alamo/game/game_session.hpp"
#include "alamo/jobs/job_system.hpp"

using namespace alamo;

namespace {

int usage() {
    std::fprintf(stderr,
                 "usage: alamo_session_run [--game DIR] [MODPATH=..] (--campaign NAME [--faction F] | --skirmish MAP --human F --ai F)\n"
                 "                         [--ticks N] [--seed S] [--difficulty 0..2] [--workers N] [--no-galactic-ai] [--no-tactical-ai]\n");
    return 2;
}

std::string faction_name(const game::GameData& d, uint32_t f) { return f < d.rules.factions.size() ? d.rules.factions[f].name : "?"; }

void print_galaxy(const game::GameData& d, const game::GameSession& s, bool list_units) {
    const galactic::GalacticSim* g = s.galaxy();
    if (!g) return;
    const std::vector<std::string>& types = s.ai_types();
    for (size_t p = 0; p < g->players().size(); ++p) {
        const galactic::PlayerState& ps = g->players()[p];
        size_t fleets = 0, units = 0, planets = 0;
        for (const galactic::FleetState& f : g->fleets())
            if (f.alive && f.owner == static_cast<int32_t>(p)) {
                ++fleets;
                units += f.contents.size();
            }
        for (const galactic::PlanetState& pl : g->planets())
            if (pl.owner == static_cast<int32_t>(p)) ++planets;
        std::printf("  player %zu %-20s %s ai_type=%-24s credits %.0f planets %zu fleets %zu (%zu units)\n", p, faction_name(d, ps.faction).c_str(),
                    ps.human ? "human" : ps.ai ? "AI   " : "-    ", p < types.size() && !types[p].empty() ? types[p].c_str() : "(none)",
                    static_cast<double>(ps.account.credits), planets, fleets, units);
        if (!list_units) continue;
        std::string names;
        for (const galactic::FleetState& f : g->fleets())
            if (f.alive && f.owner == static_cast<int32_t>(p))
                for (const objects::ObjectHandle h : f.contents) names += " " + d.rules.types[g->store().type(h)].name;
        for (const galactic::PlanetState& pl : g->planets())
            for (const objects::ObjectHandle h : pl.surface)
                if (g->store().alive(h) && g->store().owner(h) == static_cast<int32_t>(p)) names += " [" + d.rules.types[g->store().type(h)].name + "]";
        if (!names.empty()) std::printf("    units:%s\n", names.c_str());
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::string root;
    std::vector<std::string> args;
    game::SessionSpec spec;
    uint64_t ticks = 1800;
    int32_t workers = 2;
    bool skirmish = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (a == "--game") root = next();
        else if (a == "--campaign") spec.campaign = next();
        else if (a == "--faction") spec.faction = next();
        else if (a == "--skirmish") skirmish = true, spec.skirmish_map = next();
        else if (a == "--human") spec.skirmish.human_faction = next();
        else if (a == "--ai") spec.skirmish.ai_faction = next();
        else if (a == "--ticks") ticks = std::strtoull(next().c_str(), nullptr, 10);
        else if (a == "--seed") spec.seed = static_cast<uint32_t>(std::strtoul(next().c_str(), nullptr, 10));
        else if (a == "--difficulty") spec.difficulty = std::atoi(next().c_str());
        else if (a == "--workers") workers = std::atoi(next().c_str());
        else if (a == "--no-galactic-ai") spec.galactic_ai = false;
        else if (a == "--no-tactical-ai") spec.tactical_ai = false;
        else if (a.find('=') != std::string::npos) args.push_back(a);
        else return usage();
    }
    if (root.empty())
        if (const char* env = std::getenv("ALAMO_FOC_DATA")) root = env;
    if (root.empty() || (skirmish ? spec.skirmish_map.empty() : spec.campaign.empty())) return usage();
    spec.kind = skirmish ? game::SessionKind::Skirmish : game::SessionKind::Campaign;

    game::GameData data;
    std::string err;
    if (!game::load_game_data(root, args, data, &err)) {
        std::fprintf(stderr, "load failed: %s\n", err.c_str());
        return 1;
    }
    std::printf("data: %zu factions, %zu types, %zu load diagnostics, AI data %s\n", data.rules.factions.size(), data.rules.types.size(),
                data.diagnostics.size(), data.ai ? "loaded" : "missing");
    jobs::JobSystem js(jobs::JobSystemConfig{workers});
    game::GameSession s(data, js, spec);
    std::vector<std::string> diag;
    const bool ok = s.start(&diag);
    for (const std::string& m : diag) std::printf("start: %s\n", m.c_str());
    if (!ok) {
        std::printf("session did not start\n");
        return 1;
    }
    std::printf("started: %s, local player %d\n", skirmish ? spec.skirmish_map.c_str() : spec.campaign.c_str(), s.local_player());
    print_galaxy(data, s, skirmish);
    uint64_t battle_ticks = 0, battles = 0;
    bool was_in_battle = false;
    for (uint64_t t = 0; t < ticks; ++t) {
        s.tick();
        const bool in = s.in_battle();
        if (in && !was_in_battle) {
            ++battles;
            std::printf("tick %" PRIu64 ": battle %" PRIu64 " started\n", s.ticks(), battles);
        }
        if (!in && was_in_battle) std::printf("tick %" PRIu64 ": battle %" PRIu64 " ended\n", s.ticks(), battles);
        battle_ticks += in ? 1u : 0u;
        was_in_battle = in;
        if (s.skirmish_over()) {
            std::printf("tick %" PRIu64 ": skirmish over\n", s.ticks());
            break;
        }
    }
    std::printf("ran %" PRIu64 " ticks (%" PRIu64 " in battle, %" PRIu64 " battles), galactic AI orders %" PRIu64 ", commands applied %" PRIu64
                " rejected %" PRIu64 "\n",
                s.ticks(), battle_ticks, battles, s.galactic_ai_orders(), s.host().commands_applied(), s.host().commands_rejected());
    print_galaxy(data, s, false);
    return 0;
}
