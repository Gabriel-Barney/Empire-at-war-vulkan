// alamo_headless --economy --battles MODE: see battle_run.hpp.
#include "battle_run.hpp"
#include "crc_out.hpp"

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <memory>

#ifdef ALAMO_SIM_HAVE_BATTLES
#include "alamo/core/file_provider.hpp"
#include "alamo/data/game_database.hpp"
#include "alamo/galactic/campaign.hpp"
#include "alamo/galactic/conflicts.hpp"
#include "alamo/galactic/galactic_rules.hpp"
#include "alamo/jobs/crc_recorder.hpp"
#include "alamo/objects/game_object_types.hpp"
#include "alamo/objects/object_types.hpp"
#include "alamo/sim/campaign_battles.hpp"
#include "alamo/vfs/vfs.hpp"
#ifdef ALAMO_SIM_HAVE_ANIM
#include "alamo/sim/sim_anim.hpp"
#endif
#endif

#ifdef ALAMO_SIM_HAVE_BATTLES
namespace {

std::string env_value(const char* name) {
#ifdef _MSC_VER
    char* v = nullptr;
    size_t len = 0;
    std::string out;
    if (_dupenv_s(&v, &len, name) == 0 && v) {
        out = v;
        std::free(v);
    }
    return out;
#else
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
#endif
}

}  // namespace

int battle_run(const MapRunOptions& opt, const std::string& data_dir, const BattleRunOptions& bopt_in) {
    using namespace alamo;
    // S10-int: "--battles MODE+anim" turns on the sim-owned animation (SimAnimation, "anim" CRC system).
    BattleRunOptions bopt = bopt_in;
    bool anim = false;
    if (const size_t plus = bopt.mode.find("+anim"); plus != std::string::npos) {
        anim = true;
        bopt.mode = bopt.mode.substr(0, plus);
    }
#ifndef ALAMO_SIM_HAVE_ANIM
    if (anim) {
        std::fprintf(stderr, "--battles %s: this build has no engine/anim\n", bopt_in.mode.c_str());
        return 2;
    }
#endif
    std::unique_ptr<FileProvider> files;
    const vfs::Vfs* vfs_files = nullptr;  // = files when it is a VFS (cu8f t03: map listing)
    std::string err;
    if (!data_dir.empty()) {
        files = std::make_unique<DirectoryProvider>(data_dir);
    } else {
        const std::string root = opt.game_dir.empty() ? env_value("ALAMO_FOC_DATA") : opt.game_dir;
        if (root.empty()) {
            std::fprintf(stderr, "--battles needs --game <corruption dir>, --data <dir> or ALAMO_FOC_DATA\n");
            return 2;
        }
        std::unique_ptr<vfs::Vfs> v = vfs::mount_game(root, opt.game_args, &err);
        vfs_files = v.get();
        files = std::move(v);
        if (!files) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return 2;
        }
    }
    std::vector<std::string> diag;
    data::GameDatabase db;
    objects::ObjectTypeDatabase raw;
    objects::GameObjectTypes types;
    galactic::GalacticRules rules;
    galactic::ConflictRules crules;
    if (!db.load(*files, {}, &diag) || !raw.load(*files, &diag) || !types.build(raw, files.get(), &diag) ||
        !galactic::load_galactic_rules(*files, db, types, rules, &diag)) {
        std::fprintf(stderr, "battles: game data incomplete (%zu diagnostics)\n", diag.size());
        return 2;
    }
    std::vector<std::string> cdiag;
    if (!galactic::load_conflict_rules(*files, db, types, rules, crules, &cdiag))
        for (const auto& d : cdiag) std::fprintf(stderr, "  %s\n", d.c_str());
    const sim::BattleCatalog catalog(types, files.get(), &db.constants(), &cdiag);
    const std::string name = !opt.map.empty() ? opt.map : (db.campaigns().empty() ? std::string() : db.campaigns().front().name);
    const galactic::GalacticSetup setup = galactic::build_campaign_setup(db, rules, name, {}, &diag);
    if (setup.map.planets.size() == 0) {
        std::fprintf(stderr, "battles: campaign '%s' has no planets\n", name.c_str());
        return 2;
    }
    std::printf("battles: campaign %s, mode %s, contrast rows %zu, hardpoint records %zu, priority sets %zu, aggressor %u\n",
                name.c_str(), bopt_in.mode.c_str(), crules.contrast.size(), catalog.hardpoint_records(), catalog.priority_sets().size(),
                bopt.aggressor_period);
    struct Out {
        CrcLog crc;
        std::string text;
    };
    auto run_one = [&](int32_t workers, Out& out, bool print) -> bool {
        jobs::JobSystem js(jobs::JobSystemConfig{workers});
        sim::SimConfig cfg;
        cfg.seed = opt.seed;
        cfg.players = sim::session_players(setup.players.size());
        sim::SimHost host(types, js, cfg);
        galactic::GalacticSim* gs = host.start_galactic(rules, setup);
        if (!gs) return false;
        gs->set_emit_events(true);
        sim::CampaignBattleOptions bo;
        bo.conflict.human_choice = bopt.mode == "auto" ? galactic::BattleChoice::AutoResolve : galactic::BattleChoice::Fight;
        bo.conflict.fight_ai_battles = bopt.mode == "fight-all";
        bo.tactical = bopt.mode != "auto";
        bo.battle.retreat_ratio = bopt.retreat_ratio;
        bo.battle.max_frames = bopt.max_frames;
        sim::load_tactical_systems(*files, rules, &db.constants(), bo);  // Int-final: weather, retreat, strikes data
        if (vfs_files) {
            // cu8f t03: the random map picker's candidate table (CU8F_T03_T05_RE §1.1), built on the first pick [port].
            bo.map_candidate_source = sim::map_candidate_source(*files, [v = vfs_files](std::string_view g, bool mod) {
                return v->list(g, mod ? vfs::Vfs::kLayerMod : vfs::Vfs::kLayerBase | vfs::Vfs::kLayerMeg);
            });
        }
#ifdef ALAMO_SIM_HAVE_ANIM
        std::unique_ptr<sim::SimAnimation> animation;
        if (anim) {
            animation = std::make_unique<sim::SimAnimation>(types, *files);
            animation->attach(host, true);
            bo.animation = animation.get();
        }
#endif
        sim::CampaignBattles battles(host, crules, catalog, sim::CampaignBattles::file_map_loader(*files), bo);
        std::vector<std::string> ad;
        if (!battles.attach(&ad)) return false;
        if (bopt.aggressor_period > 0)
            for (uint32_t p = 0; p < host.players() && p < gs->players().size(); ++p)
                host.set_controller(static_cast<uint8_t>(p), sim::CampaignAggressor{bopt.aggressor_period});
        const auto t0 = std::chrono::steady_clock::now();
        std::vector<galactic::GalacticEvent> events;
        uint64_t n = 0;
        while (n < opt.ticks && host.tick_once()) {
            ++n;
            gs->drain_events(events);
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        out.crc = host.crc_log();
        std::string& t = out.text;
        for (const auto& e : battles.conflicts().log()) t += "conflict " + e.text(*gs) + "\n";
        for (const auto& b : battles.log()) t += "battle " + b + "\n";
        uint32_t won = 0, lost = 0, retreat = 0;
        for (const auto& e : events) {
            won += e.kind == galactic::GalacticEventKind::BattleWon;
            lost += e.kind == galactic::GalacticEventKind::BattleLost;
            retreat += e.kind == galactic::GalacticEventKind::BattleRetreat;
        }
        char line[512];
        std::snprintf(line, sizeof line,
                      "campaign %s workers %d: %" PRIu64 " ticks (%.1f ms), galactic frame %d, conflicts %zu (queued now %zu space / %zu land), "
                      "tactical battles %u, story events won %u lost %u retreat %u, final CRC %016" PRIx64 "\n",
                      name.c_str(), workers, n, ms, gs->frame(), battles.conflicts().log().size(),
                      battles.conflicts().space_queue().size(), battles.conflicts().land_queue().size(), battles.battles_fought(), won,
                      lost, retreat, out.crc.records.empty() ? 0 : out.crc.records.back().combined);
        if (print && bopt.verbose) std::printf("%s", t.c_str());
        std::printf("%s", line);
        return true;
    };
    const int32_t w0 = opt.check_workers ? jobs::determinism_worker_counts().front() : opt.workers;
    Out first;
    if (!run_one(w0, first, true)) {
        std::fprintf(stderr, "battles: campaign start failed\n");
        return 1;
    }
    bool ok = true;
    if (opt.check_workers) {
        ok = jobs::check_worker_counts(
            first,
            [&](int32_t w) {
                Out o;
                run_one(w, o, false);
                return o;
            },
            [](const Out& o) -> const CrcLog& { return o.crc; },
            [&](int32_t w, const Out& o, const CrcDivergence& d) {
                std::printf("  workers %d vs %d: CRC %s, conflict/battle log %s\n", w0, w, d.message.c_str(),
                            o.text == first.text ? "identical" : "DIFFERS");
                return o.text == first.text;
            });
    }
    first.crc.comments = {"battles campaign " + name + " mode " + bopt.mode + " seed " + std::to_string(opt.seed) + " ticks " +
                          std::to_string(opt.ticks) + " aggressor " + std::to_string(bopt.aggressor_period)};
    if (!opt.crc_out.empty() && alamo_headless::save_crc_log(first.crc, opt.crc_out, &err)) std::printf("wrote %s\n", opt.crc_out.c_str());
    if (!opt.golden.empty() && alamo_headless::compare_golden(opt.golden, first.crc, ok) != 0) return 2;
    std::printf(ok ? "battles run: OK\n" : "battles run: FAILED\n");
    return ok ? 0 : 1;
}

#else

int battle_run(const MapRunOptions&, const std::string&, const BattleRunOptions&) {
    std::fprintf(stderr, "--battles: this build has no tactical movement/combat modules\n");
    return 2;
}

#endif
