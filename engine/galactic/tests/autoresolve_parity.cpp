// alamo_galactic_autoresolve_parity: runs the galactic auto-resolve (auto_resolve.hpp) on battles described in a small
// text spec, over many seeds, with the FoC rules (ALAMO_FOC_DATA), and prints every outcome. bkc1 t08 (backlog b0027):
// tools/parity/autoresolve_check.py writes the spec from tools/parity/autoresolve_refs.json (the retail outcomes) and
// grades the printed outcomes against them (ctest parity_autoresolve).
//
// Spec (one item per line, '#' comments):
//   battle <id> space|land <attacker player> <defender player>
//   side <player> <ai 0|1> <playable 0|1>
//   unit <player> <type name> <count>          combatants in list order (defender units, attacker units, structures)
//   starbase <player> <type name>              the planet's starbase (space; levels below it from the rules)
//   end
// Output (stdout):
//   result <id> <seed> <winner> <loser> <retreating>
//   lost <id> <seed> <player> <type name> <combatants of that type destroyed>
//   starbase <id> <seed> <player> <surviving type name | destroyed>
// Exit codes: 0 ok, 1 bad spec / data error, 77 no FoC data (ctest skip).
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "alamo/core/sim_rng.hpp"
#include "alamo/data/game_database.hpp"
#include "alamo/galactic/auto_resolve.hpp"
#include "alamo/galactic/conflicts.hpp"
#include "alamo/galactic/galactic_rules.hpp"
#include "alamo/objects/game_object_types.hpp"
#include "alamo/objects/object_types.hpp"
#include "alamo/test/support.hpp"
#include "foc_data.hpp"

using namespace alamo;
using namespace alamo::galactic;

namespace {

struct Unit {
    int32_t player = -1;
    std::string type;
    int count = 1;
};
struct Battle {
    std::string id;
    bool space = true;
    int32_t attacker = -1, defender = -1;
    std::vector<ArSideInfo> sides;
    std::vector<Unit> units;
    int32_t starbase_player = -1;
    std::string starbase;
};

bool read_spec(const char* path, std::vector<Battle>& out) {
    std::ifstream in(path);
    if (!in) {
        std::fprintf(stderr, "cannot open spec %s\n", path);
        return false;
    }
    std::string line;
    Battle cur;
    bool open = false;
    int n = 0;
    while (std::getline(in, line)) {
        ++n;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ls(line);
        std::string kw;
        ls >> kw;
        bool ok = true;
        if (kw == "battle") {
            std::string kind;
            cur = Battle{};
            ls >> cur.id >> kind >> cur.attacker >> cur.defender;
            cur.space = kind == "space";
            ok = !ls.fail() && (kind == "space" || kind == "land");
            open = true;
        } else if (kw == "side" && open) {
            ArSideInfo s;
            int ai = 1, playable = 1;
            ls >> s.player >> ai >> playable;
            s.ai = ai != 0;
            s.playable = playable != 0;
            ok = !ls.fail();
            cur.sides.push_back(s);
        } else if (kw == "unit" && open) {
            Unit u;
            ls >> u.player >> u.type >> u.count;
            ok = !ls.fail() && u.count > 0;
            cur.units.push_back(u);
        } else if (kw == "starbase" && open) {
            ls >> cur.starbase_player >> cur.starbase;
            ok = !ls.fail();
        } else if (kw == "end" && open) {
            out.push_back(cur);
            open = false;
        } else {
            ok = false;
        }
        if (!ok) {
            std::fprintf(stderr, "%s:%d: bad line: %s\n", path, n, line.c_str());
            return false;
        }
    }
    return !open;
}

}  // namespace

int main(int argc, char** argv) {
    const char* spec = nullptr;
    int seeds = 200;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--spec" && i + 1 < argc) spec = argv[++i];
        else if (a == "--seeds" && i + 1 < argc) seeds = std::atoi(argv[++i]);
        else {
            std::fprintf(stderr, "usage: alamo_galactic_autoresolve_parity --spec <file> [--seeds N]\n");
            return 1;
        }
    }
    if (!spec || seeds <= 0) {
        std::fprintf(stderr, "usage: alamo_galactic_autoresolve_parity --spec <file> [--seeds N]\n");
        return 1;
    }
    std::vector<Battle> battles;
    if (!read_spec(spec, battles)) return 1;

    const std::string root = test::env_or_empty("ALAMO_FOC_DATA");
    std::string how;
    std::unique_ptr<FileProvider> provider = testing::open_foc_data(root, how);
    if (!provider) {
        std::printf("ALAMO_FOC_DATA unset or without Data\\XML: auto-resolve parity skipped\n");
        return 77;
    }
    data::GameDatabase db;
    objects::ObjectTypeDatabase raw;
    objects::GameObjectTypes types;
    GalacticRules rules;
    ConflictRules crules;
    std::vector<std::string> diag;
    if (!db.load(*provider, {}, &diag) || !raw.load(*provider, &diag) || !types.build(raw, provider.get(), &diag) ||
        !load_galactic_rules(*provider, db, types, rules, &diag) ||
        !load_conflict_rules(*provider, db, types, rules, crules, &diag)) {
        std::fprintf(stderr, "FoC data (%s) did not load\n", how.c_str());
        return 1;
    }
    std::printf("# FoC data: %s; %d seeds\n", how.c_str(), seeds);

    for (const Battle& b : battles) {
        AutoResolveInput in;
        in.space = b.space;
        in.attacker = b.attacker;
        in.players = b.sides;
        std::vector<uint32_t> type_of;  // per combatant
        for (const Unit& u : b.units) {
            const uint32_t t = rules.find_type(u.type);
            if (t >= rules.types.size()) {
                std::fprintf(stderr, "%s: unknown type %s\n", b.id.c_str(), u.type.c_str());
                return 1;
            }
            for (int k = 0; k < u.count; ++k) {
                in.combatants.push_back(make_ar_combatant(rules, crules, t, u.player, b.space));
                type_of.push_back(t);
            }
        }
        if (!b.starbase.empty()) {
            uint32_t t = rules.find_type(b.starbase);
            if (t >= rules.types.size()) {
                std::fprintf(stderr, "%s: unknown starbase %s\n", b.id.c_str(), b.starbase.c_str());
                return 1;
            }
            ArCombatant c = make_ar_combatant(rules, crules, t, b.starbase_player, true);  // as ConflictManager::auto_setup
            for (int guard = 0; guard < 16 && t < rules.types.size(); ++guard) {
                c.starbase.push_back({t, rules.types[t].ai_combat_power});
                t = crules.type(t).lower_starbase;
            }
            type_of.push_back(c.starbase.front().type);
            in.combatants.push_back(std::move(c));
        }
        for (int s = 0; s < seeds; ++s) {
            // One seed per run; the LCG state the battle starts from stands in for the galaxy's synced RNG state.
            SimRng rng(static_cast<uint32_t>(s) * 0x9E3779B9u + 1u);
            const AutoResolveResult r = auto_resolve(in, crules.contrast, crules.auto_resolve, rng);
            if (!r.valid) {
                std::fprintf(stderr, "%s: no two sides\n", b.id.c_str());
                return 1;
            }
            std::printf("result %s %d %d %d %d\n", b.id.c_str(), s, r.winner, r.loser, r.retreating);
            std::map<std::pair<int32_t, uint32_t>, int> dead;  // (player, type) -> combatants destroyed (sorted: stable)
            for (size_t i = 0; i < in.combatants.size(); ++i) {
                if (!in.combatants[i].starbase.empty()) {
                    const uint32_t st = r.starbase_type[i];
                    std::printf("starbase %s %d %d %s\n", b.id.c_str(), s, in.combatants[i].owner,
                                st < rules.types.size() ? rules.types[st].name.c_str() : "destroyed");
                    continue;
                }
                if (!r.survives[i]) ++dead[{in.combatants[i].owner, type_of[i]}];
            }
            for (const auto& [k, n] : dead)
                std::printf("lost %s %d %d %s %d\n", b.id.c_str(), s, k.first, rules.types[k.second].name.c_str(), n);
        }
    }
    return 0;
}
