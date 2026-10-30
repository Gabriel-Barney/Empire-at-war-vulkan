// N1 / t09 (owner: n2-game): alamo_game's save / load on the real FoC data (ALAMO_FOC_DATA; skipped when unset).
//   A  a story campaign with the galactic AI runs to tick T, is saved through the menus' save path (game::save_game into a
//      saves folder), then runs K more ticks (per-tick CRC log from the save on).
//   C  a FRESH PROCESS (this exe, --child) loads the file through the menus' load path (game::load_game) and runs the same
//      K ticks with the same answer policy: its CRC log equals A's from the save on.
//   B  in process: load + replay of A's tick-0 command stream; equal to A as well.
//   Refusals: the same save into a session with another seed; a save whose systems cannot all be saved is never written.
//   Animation on (SimAnimation's "anim" chunk, n2-host / n2-tai): a save the check offers must load (else it is refused at
//   the check with the chunk's name).
//   N1 stage 2: a space and a land skirmish (animation on) saved DURING their battle, checked the same way (C and B).
//   N1f: a human conflict saved while it waits for the choice (Vote) and saved with the answer sent but not yet drained; a
//   battle accepted but not yet pushed ("battles" v2 in flight = 1); a human-fought campaign battle saved mid-battle.
// Human conflicts (ask_battle_choice) are answered 10 ticks after they are asked (Auto-Resolve, or Fight where a check asks
// for it), in A and in the child; a question open at the save carries its ask tick over to the child.
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "alamo/core/file_provider.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/game/game_data.hpp"
#include "alamo/game/game_session.hpp"
#include "alamo/game/save_games.hpp"

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

constexpr uint64_t kSaveAt = 300, kTicksAfter = 300;
constexpr const char* kCampaign = "Sandbox_Heart_Of_The_Maw_Empire";

game::SessionSpec campaign_spec() {
    game::SessionSpec s;
    s.kind = game::SessionKind::Campaign;
    s.campaign = kCampaign;
    s.faction = "Empire";
    s.seed = 11;
    s.ask_battle_choice = true;  // as the game's menus start campaigns
    s.animation = false;         // N1: the anim chunk is n2-tai / n2-host's (checked separately below)
    return s;
}

// The answer policy of the runs: a pending human conflict is auto-resolved 10 ticks after it was first seen.
// The human in a running battle: its first unit attacks the first enemy, the next moves (ordinary recorded commands).
void order_units(game::GameSession& s) {
    const sim::TacticalBattle* b = s.battle();
    CHECK(b != nullptr);
    if (!b) return;
    const objects::ObjectStore& store = s.host().modes().top_tactical()->primary();
    std::vector<uint32_t> mine;
    uint32_t enemy = 0;
    for (const objects::ObjectId id : b->movement().unit_ids()) {
        const objects::ObjectHandle h = store.find(id);
        if (!store.alive(h)) continue;
        if (store.owner(h) == s.local_player()) mine.push_back(static_cast<uint32_t>(id));
        else if (enemy == 0 && b->combat().has_unit(id)) enemy = static_cast<uint32_t>(id);
    }
    CHECK(mine.size() >= 2 && enemy != 0);
    if (mine.size() < 2 || enemy == 0) return;
    CHECK(s.submit(commands::ev::attack(enemy, {mine[0]})).ok());
    CHECK(s.submit(commands::ev::select({mine[1]})).ok());
    CHECK(s.submit(commands::ev::move_to_position(1500.0f, -1200.0f, 0.0f)).ok());
}

struct Answerer {
    uint64_t asked = 0;
    bool seen = false;
    galactic::BattleChoice choice = galactic::BattleChoice::AutoResolve;
    uint64_t order_at = 0;  // host frame of one order_units (0 = none)
    void step(game::GameSession& s) {
        if (order_at != 0 && s.host().frame() == order_at) order_units(s);
        if (!s.battle_choice_pending()) {
            seen = false;
            return;
        }
        if (!seen) {
            seen = true;
            asked = s.host().frame();
        }
        if (s.host().frame() >= asked + 10) s.choose_battle(choice);
    }
};

void run(game::GameSession& s, Answerer& a, uint64_t n) {
    for (uint64_t i = 0; i < n; ++i) {
        a.step(s);
        s.tick();
    }
}

CrcLog records_from(const CrcLog& log, uint64_t tick) {
    CrcLog out;
    out.systems = log.systems;
    out.parity_parts = log.parity_parts;
    for (const CrcRecord& r : log.records)
        if (r.tick >= tick) out.records.push_back(r);
    return out;
}

bool load_data(const char* foc, game::GameData& data) {
    std::string err;
    if (!game::load_game_data(foc, {}, data, &err)) {
        std::printf("game save: data load failed: %s\n", err.c_str());
        return false;
    }
    return true;
}

// --child SAVE TICKS CRC_OUT ANSWERER: load SAVE through the menus' load path, run TICKS, write the CRC log. ANSWERER =
// the saving run's answer policy at the save ("choice,seen,asked,order_at").
int child(const char* foc, const std::string& save, uint64_t ticks, const std::string& crc_out, const Answerer& policy) {
    game::GameData data;
    if (!load_data(foc, data)) return 2;
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    std::string err;
    std::unique_ptr<game::GameSession> s = game::load_game(data, js, save, &err);
    if (!s) {
        std::printf("child: load failed: %s\n", err.c_str());
        return 3;
    }
    Answerer a = policy;
    run(*s, a, ticks);
    if (!s->host().crc_log().save(crc_out, &err)) {
        std::printf("child: %s\n", err.c_str());
        return 4;
    }
    std::printf("child: loaded tick %" PRIu64 ", ran to %" PRIu64 "\n", s->load_point().tick, s->host().frame());
    return 0;
}

std::string quote_arg(const std::string& s) { return "\"" + s + "\""; }

// Where a save check saves.
enum class SavePoint : uint8_t {
    Galactic,      // tick `at` on the galactic map
    Battle,        // `at` ticks into the first battle
    Vote,          // `at` (< 10) ticks after a human conflict asked for the choice, unanswered
    VoteAnswered,  // same, right after the answer was submitted (in the command queue, not drained)
    MapPending,    // tick `at`, right after a battle was accepted: "battles" v2 in flight = 1
};

// The human's fleet ordered at an enemy-held neighbouring planet (an ordinary command), in the order candidates are found;
// `pick` selects one. False when there are no more candidates.
bool order_attack(game::GameSession& s, size_t pick) {
    const galactic::GalacticSim* g = s.galaxy();
    if (!g) return false;
    size_t n = 0;
    for (const galactic::FleetState& f : g->fleets()) {
        if (!f.alive || f.owner != s.local_player() || f.moving || f.planet == kInvalidPlanet) continue;
        for (const uint32_t r : g->planets()[f.planet].links) {
            const auto& routes = g->map().routes;
            const PlanetIndex to = routes.point_a[r] == f.planet ? routes.point_b[r] : routes.point_a[r];
            const galactic::PlanetState& p = g->planets()[to];
            bool enemy = false;
            for (const uint32_t fi : p.orbit) {
                const galactic::FleetState& o = g->fleets()[fi];
                enemy = enemy || (o.alive && o.owner != s.local_player() && o.owner != g->neutral_player());
            }
            if (!enemy || !g->is_planet_revealed(to) || n++ != pick) continue;
            std::printf("  attack %zu: fleet at planet %u -> planet %u (%s)\n", pick, f.planet, to,
                        to < g->map().planets.name.size() ? g->map().planets.name[to].c_str() : "?");
            return s.submit(commands::ev::move_object_to_object(static_cast<uint32_t>(g->store().id(f.object)), static_cast<uint32_t>(g->store().id(p.object))))
                .ok();
        }
    }
    return false;
}

struct SaveCheck {
    const char* name = "";
    std::string title;
    game::SessionSpec spec;
    SavePoint point = SavePoint::Galactic;
    uint64_t at = 0, after = 0;
    galactic::BattleChoice choice = galactic::BattleChoice::AutoResolve;
    size_t attack = kNoAttack;  // order_attack's pick, right after the session starts
    static constexpr size_t kNoAttack = static_cast<size_t>(-1);
    bool expect_battle_after = false;  // A must be in (or past) a battle by the end of the run
    bool orders = false;               // Battle: the human orders units before and after the save
};

// One save check: A runs the spec to the save point, saves through save_game, runs `after` more ticks; C = a fresh process
// loads the file through load_game and runs the same ticks; B = load_game + replay of A's tick-0 stream. Both CRC logs must
// equal A's from the save on. Returns the save's path ("" when A could not save).
std::string save_check(const game::GameData& data, jobs::JobSystem& js, const char* self, const fs::path& dir, const std::string& saves,
                       const SaveCheck& chk) {
    const char* const name = chk.name;
    const std::string& title = chk.title;
    const game::SessionSpec& spec = chk.spec;
    const bool battle = chk.point == SavePoint::Battle;
    const uint64_t at = chk.at, after = chk.after;
    Answerer at_save;
    commands::CommandRecording rec;
    std::string path, err;
    uint64_t save_tick = 0;
    CrcLog log_a;
    std::error_code ec;
    {
        game::GameSession a(data, js, spec);
        std::vector<std::string> diag;
        CHECK(a.start(&diag, nullptr, &rec));
        if (!a.started()) {
            for (const std::string& d : diag) std::printf("  %s: %s\n", name, d.c_str());
            return {};
        }
        std::printf("%s A: %s, galactic AI %s, story %s, animation %s\n", name,
                    spec.kind == game::SessionKind::Campaign ? spec.campaign.c_str() : spec.skirmish_map.c_str(),
                    a.galactic_ai_running() ? "on" : "off", a.story_running() ? "on" : "off", spec.animation ? "on" : "off");
        Answerer ans;
        ans.choice = chk.choice;
        if (chk.attack != SaveCheck::kNoAttack) CHECK(order_attack(a, chk.attack));
        if (battle) {
            for (uint64_t i = 0; i < 6000 && !a.in_battle(); ++i) run(a, ans, 1);
            CHECK(a.in_battle());
            if (!a.in_battle()) return {};
            std::printf("%s A: battle started at tick %" PRIu64 "\n", name, a.host().frame());
            if (chk.orders) order_units(a);
        }
        if (chk.point == SavePoint::Vote || chk.point == SavePoint::VoteAnswered) {
            for (uint64_t i = 0; i < 6000 && !a.battle_choice_pending(); ++i) run(a, ans, 1);
            CHECK(a.battle_choice_pending());
            if (!a.battle_choice_pending()) return {};
            std::printf("%s A: choice asked at tick %" PRIu64 "\n", name, a.host().frame());
        }
        run(a, ans, at);
        if (chk.point == SavePoint::Vote || chk.point == SavePoint::VoteAnswered) {
            CHECK(a.battle_choice_pending() && a.battles()->conflicts().state() == galactic::ConflictState::Vote);
            if (chk.point == SavePoint::VoteAnswered) {
                CHECK(a.choose_battle(chk.choice));
                CHECK(!a.battle_choice_pending());
            }
        }
        if (chk.point == SavePoint::MapPending) {
            // The only way to the branch: a conflict's start_battle runs inside the galactic update at T5 and the hook right
            // after it pushes the battle in the same tick, so a tick never ends with it pending. Accepted between ticks.
            const std::vector<std::string> maps = game::skirmish_space_maps(data);
            const galactic::GalacticSim* g = a.galaxy();
            CHECK(!maps.empty() && g);
            if (maps.empty() || !g) return {};
            galactic::BattleRequest r;
            r.info.kind = galactic::ConflictKind::Space;
            r.info.attacker = a.local_player();
            r.info.map = r.map = maps.front();
            galactic::BattleForces human;  // the first fleet of each side, so that the battle has units and lasts
            human.player = r.info.attacker;
            for (const galactic::FleetState& f : g->fleets()) {
                if (!f.alive || f.planet == kInvalidPlanet) continue;
                if (f.owner == r.info.attacker && human.start.empty()) human.start = f.contents;
                if (f.owner != r.info.attacker && f.owner != g->neutral_player() && r.forces.empty()) {
                    r.info.defender = f.owner;
                    r.info.planet = f.planet;
                    galactic::BattleForces enemy;
                    enemy.player = f.owner;
                    enemy.start = f.contents;
                    r.forces.push_back(std::move(enemy));  // defender first
                }
            }
            CHECK(!human.start.empty() && !r.forces.empty());
            r.forces.push_back(std::move(human));
            CHECK(a.battles()->start_map_battle(std::move(r)));
            CHECK(!a.in_battle());
        }
        for (uint64_t i = 0; i < 3000 && !a.save_blocker().empty(); ++i) run(a, ans, 1);
        at_save = ans;
        const std::string why = a.check_save();
        std::printf("%s A: save check at tick %" PRIu64 "%s: %s\n", name, a.host().frame(), a.in_battle() ? " (in battle)" : "",
                    why.empty() ? "ok" : why.c_str());
        CHECK(why.empty());
        if (battle) CHECK(a.in_battle());
        const game::SaveMeta meta = a.save_meta("x", 0);
        CHECK(meta.tick == a.host().frame() && (spec.kind == game::SessionKind::Campaign ? meta.week >= 1 : meta.kind == "skirmish"));
        CHECK(game::save_game(a, saves, name, title, game::unix_now(), &path, &err));
        if (!err.empty()) std::printf("%s A: save: %s\n", name, err.c_str());
        save_tick = a.host().frame();
        if (battle && spec.kind == game::SessionKind::Skirmish) CHECK(meta.game_seconds == a.battle()->frame() / 30u);  // t06
        if (chk.point == SavePoint::MapPending) {  // pushed by the next tick
            run(a, ans, 1);
            CHECK(a.in_battle());
            run(a, ans, after - 1);
            CHECK(a.in_battle());  // the battle has units: it was still running at the end of the run
        } else {
            if (chk.orders) {  // one more order, after the save: A and the child issue it at the same tick
                ans.order_at = save_tick + 30;
                at_save = ans;
            }
            bool fought = false;
            for (uint64_t i = 0; i < after; ++i) {
                run(a, ans, 1);
                fought = fought || a.in_battle();
            }
            if (chk.expect_battle_after) CHECK(fought);
            if (chk.orders) CHECK(a.order_stats().attacks == 2 && a.order_stats().moves == 2 && a.order_stats().rejected == 0);
        }
        log_a = records_from(a.host().crc_log(), save_tick);
        std::printf("%s A: saved tick %" PRIu64 " (%" PRIuMAX " bytes), ran to %" PRIu64 " (%s), %zu commands recorded\n", name, save_tick,
                    static_cast<uintmax_t>(fs::file_size(path_from_utf8(path), ec)), a.host().frame(), a.in_battle() ? "in battle" : "galactic",
                    rec.command_count());
    }
    if (path.empty()) return {};
    for (const game::SaveEntry& e : game::list_saves(saves))
        if (e.name == name) {
            CHECK(e.readable && e.tick == save_tick && e.meta.title == title);
            std::printf("%s listing: %s\n", name, game::save_label(e.meta).c_str());
        }
    // C: a fresh process loads the file and continues.
    {
        const std::string crc_c = path_to_utf8(dir / (std::string(name) + ".child.crc"));
        std::string cmd = quote_arg(self) + " --child " + quote_arg(path) + " " + std::to_string(after) + " " + quote_arg(crc_c) + " " +
                          std::to_string(static_cast<uint32_t>(at_save.choice)) + "," + (at_save.seen ? "1," : "0,") + std::to_string(at_save.asked) + "," +
                          std::to_string(at_save.order_at);
#ifdef _WIN32
        cmd = "\"" + cmd + "\"";  // cmd.exe /c strips one outer pair of quotes
#endif
        std::fflush(stdout);
        const int rc = std::system(cmd.c_str());
        CrcLog log_c;
        if (rc == 0 && CrcLog::load(crc_c, log_c, &err)) {
            const CrcDivergence d = first_divergence(log_a, log_c);
            std::printf("%s C fresh process load vs original: %zu vs %zu records: %s\n", name, log_c.records.size(), log_a.records.size(),
                        d.message.c_str());
            CHECK(!d.diverged && log_c.records.size() == log_a.records.size() && !log_a.records.empty());
        } else {
            std::printf("%s C fresh process: exit %d %s\n", name, rc, err.c_str());
            CHECK(false);
        }
    }
    // B: in process, load + replay of A's whole stream.
    {
        std::unique_ptr<game::GameSession> b = game::load_game(data, js, path, &err, &rec);
        CHECK(b != nullptr);
        if (b) {
            CHECK(b->in_battle() == battle);
            for (uint64_t i = 0; i < after; ++i) {
                b->tick();
                if (i == 0 && chk.point == SavePoint::MapPending) CHECK(b->in_battle());  // the file held the pending battle
            }
            const CrcDivergence d = first_divergence(log_a, b->host().crc_log());
            std::printf("%s B load + replay vs original: %s (%zu chunks loaded)\n", name, d.message.c_str(), b->load_report().loaded.size());
            CHECK(!d.diverged && b->host().crc_log().records.size() == log_a.records.size());
        } else {
            std::printf("%s B: %s\n", name, err.c_str());
        }
    }
    return path;
}

game::SessionSpec skirmish_spec(const std::string& map) {
    game::SessionSpec s;
    s.kind = game::SessionKind::Skirmish;
    s.skirmish_map = map;
    s.skirmish.human_faction = "Rebel";
    s.skirmish.ai_faction = "Empire";
    s.seed = 5;
    s.animation = true;  // as the game runs it
    return s;
}

}  // namespace

int main(int argc, char** argv) {
    const char* foc = std::getenv("ALAMO_FOC_DATA");
    if (argc == 6 && std::string(argv[1]) == "--child") {
        if (!foc || !*foc) return 2;
        Answerer policy;
        char* p = argv[5];
        policy.choice = static_cast<galactic::BattleChoice>(std::strtoul(p, &p, 10));
        policy.seen = std::strtoul(p + 1, &p, 10) != 0;
        policy.asked = std::strtoull(p + 1, &p, 10);
        policy.order_at = std::strtoull(p + 1, nullptr, 10);
        return child(foc, argv[2], std::strtoull(argv[3], nullptr, 10), argv[4], policy);
    }
    if (!foc || !*foc) {
        std::printf("alamo_game_save_tests: ALAMO_FOC_DATA not set: skipped\n");
        return 0;
    }
    game::GameData data;
    if (!load_data(foc, data)) return 1;
    const fs::path dir = fs::temp_directory_path() / "alamo_game_save_tests";
    std::error_code ec;
    remove_all_long(dir, ec);
    const std::string saves = path_to_utf8(dir / "Saves");
    jobs::JobSystem js(jobs::JobSystemConfig{2});
    std::string err;

    // The campaign on the galactic map.
    SaveCheck galactic_check;
    galactic_check.name = "FoC check";
    galactic_check.title = "Heart of the Maw";
    galactic_check.spec = campaign_spec();
    galactic_check.at = kSaveAt;
    galactic_check.after = kTicksAfter;
    const std::string path = save_check(data, js, argv[0], dir, saves, galactic_check);
    CHECK(!path.empty());
    // Another seed: refused by the descriptor check, nothing loaded.
    if (!path.empty()) {
        SaveFile f;
        game::SessionSpec s;
        CHECK(SaveFile::read_file(path, f, &err) && game::GameSession::spec_from_save(f, s, &err));
        s.seed += 1;
        game::GameSession r(data, js, s);
        const bool ok = r.start_from_save(f, &err);
        std::printf("other seed: %s\n", ok ? "LOADED" : err.c_str());
        CHECK(!ok && err.find("seed") != std::string::npos);
    }
#ifdef ALAMO_SIM_HAVE_ANIM
    // Animation on: the check either refuses (naming the chunk) or the save it offers loads.
    {
        game::SessionSpec s = campaign_spec();
        s.animation = true;
        game::GameSession a(data, js, s);
        CHECK(a.start(nullptr, nullptr, nullptr));
        Answerer ans;
        run(a, ans, 60);
        const std::string why = a.check_save();
        if (!why.empty()) {
            std::printf("animation on: save refused: %s\n", why.c_str());
            CHECK(why.find("anim") != std::string::npos || why.find("pending") != std::string::npos);
        } else {
            std::string p2;
            CHECK(game::save_game(a, saves, "anim", "", game::unix_now(), &p2, &err));
            std::unique_ptr<game::GameSession> b = game::load_game(data, js, p2, &err);
            std::printf("animation on: saved and %s\n", b ? "loaded" : ("NOT loaded: " + err).c_str());
            CHECK(b != nullptr);
        }
    }
#endif
    // N1 stage 2: skirmishes saved DURING the battle (animation on): a space map, and a land map (the human fleet hops and
    // invades; the land battle runs).
    const std::vector<std::string> space = game::skirmish_space_maps(data), land = game::skirmish_land_maps(data);
    CHECK(!space.empty() && !land.empty());
    auto battle_check = [&](const char* name, const game::SessionSpec& spec) {
        SaveCheck c;
        c.name = name;
        c.spec = spec;
        c.point = SavePoint::Battle;
        c.at = 150;
        c.after = 300;
        CHECK(!save_check(data, js, argv[0], dir, saves, c).empty());
    };
    if (!space.empty()) battle_check("Space battle", skirmish_spec(space.front()));
    if (!land.empty()) battle_check("Land battle", skirmish_spec(land.front()));

    // N1f: human-conflict checks on the campaign: the session as the game runs it, the human's fleet ordered at an enemy.
    const auto fought_campaign = [] {
        game::SessionSpec s = campaign_spec();
        s.animation = true;
        return s;
    };
    // The first attack that reaches the choice: an overrun (BT: far stronger side) auto-resolves without asking.
    size_t attack = SaveCheck::kNoAttack;
    for (size_t pick = 0; pick < 32 && attack == SaveCheck::kNoAttack; ++pick) {
        game::GameSession probe(data, js, fought_campaign());
        CHECK(probe.start(nullptr, nullptr, nullptr));
        if (!probe.started() || !order_attack(probe, pick)) break;
        for (uint64_t i = 0; i < 3000 && !probe.battle_choice_pending(); ++i) probe.tick();
        if (probe.battle_choice_pending()) attack = pick;
    }
    std::printf("human conflict: attack %zu\n", attack);
    CHECK(attack != SaveCheck::kNoAttack);
    if (attack != SaveCheck::kNoAttack) {
        // t02: saved while the conflict waits for the choice; Fight is answered after the save (A and the child, same tick).
        SaveCheck vote;
        vote.name = "Choice pending";
        vote.spec = fought_campaign();
        vote.point = SavePoint::Vote;
        vote.at = 5;
        vote.after = 400;
        vote.choice = galactic::BattleChoice::Fight;
        vote.attack = attack;
        vote.expect_battle_after = true;
        CHECK(!save_check(data, js, argv[0], dir, saves, vote).empty());
        // t02: saved with the answer in the command queue.
        SaveCheck answered = vote;
        answered.name = "Choice answered";
        answered.point = SavePoint::VoteAnswered;
        CHECK(!save_check(data, js, argv[0], dir, saves, answered).empty());
        // t03: a human-fought campaign battle saved mid-battle.
        SaveCheck fought;
        fought.name = "Campaign battle";
        fought.spec = fought_campaign();
        fought.point = SavePoint::Battle;
        fought.at = 150;
        fought.after = 300;
        fought.choice = galactic::BattleChoice::Fight;
        fought.attack = attack;
        fought.orders = true;
        CHECK(!save_check(data, js, argv[0], dir, saves, fought).empty());
    }
    // t02: "battles" v2 in flight = 1: the battle accepted, its push due at the next hook.
    SaveCheck pending;
    pending.name = "Battle pending";
    pending.spec = fought_campaign();
    pending.point = SavePoint::MapPending;
    pending.at = 30;
    pending.after = 300;
    CHECK(!save_check(data, js, argv[0], dir, saves, pending).empty());
    remove_all_long(dir, ec);
    if (g_fail) {
        std::printf("alamo_game_save_tests: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("alamo_game_save_tests: all passed\n");
    return 0;
}
