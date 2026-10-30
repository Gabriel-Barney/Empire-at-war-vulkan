// alamo_headless --map: loads the game's object types and a .ted map through the VFS, spawns the
// map into a tactical mode and runs the G0 host (engine/apps/sim), printing per-tick CRCs.
#include "map_run.hpp"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>

#include "alamo/commands/recording.hpp"
#include "alamo/core/file_provider.hpp"
#include "alamo/data/game_database.hpp"
#include "alamo/galactic/campaign.hpp"
#include "alamo/galactic/galactic_rules.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/jobs/crc_recorder.hpp"
#include "alamo/objects/game_object_types.hpp"
#include "alamo/objects/object_types.hpp"
#include "alamo/sim/scenario.hpp"
#include "alamo/terrain/ted.hpp"
#include "alamo/vfs/vfs.hpp"

namespace {

std::string env_or_empty(const char* name) {
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

double ms_since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

}  // namespace

int economy_run(const MapRunOptions& opt, const std::string& data_dir) {
    using namespace alamo;
    std::unique_ptr<FileProvider> files;
    std::string err;
    if (!data_dir.empty()) {
        files = std::make_unique<DirectoryProvider>(data_dir);
    } else {
        const std::string root = opt.game_dir.empty() ? env_or_empty("ALAMO_FOC_DATA") : opt.game_dir;
        if (root.empty()) {
            std::fprintf(stderr, "--economy needs --game <corruption dir>, --data <dir> or ALAMO_FOC_DATA\n");
            return 2;
        }
        vfs::VfsConfig vcfg;
        vcfg.foc_root = root;
        vfs::parse_command_line(opt.game_args, vcfg);
        files = vfs::Vfs::create(vcfg, &err);
        if (!files) {
            std::fprintf(stderr, "VFS: %s\n", err.c_str());
            return 2;
        }
    }
    std::vector<std::string> diag;
    data::GameDatabase db;
    objects::ObjectTypeDatabase raw;
    objects::GameObjectTypes types;
    galactic::GalacticRules rules;
    if (!db.load(*files, {}, &diag) || !raw.load(*files, &diag) || !types.build(raw, files.get(), &diag) ||
        !galactic::load_galactic_rules(*files, db, types, rules, &diag)) {
        std::fprintf(stderr, "economy: game data incomplete (%zu diagnostics)\n", diag.size());
        return 2;
    }
    const std::string name = !opt.map.empty() ? opt.map : (db.campaigns().empty() ? std::string() : db.campaigns().front().name);
    const galactic::GalacticSetup setup = galactic::build_campaign_setup(db, rules, name, {}, &diag);
    if (setup.map.planets.size() == 0) {
        std::fprintf(stderr, "economy: campaign '%s' has no planets\n", name.c_str());
        return 2;
    }
    auto run_one = [&](int32_t workers, CrcLog& log) -> bool {
        jobs::JobSystem js(jobs::JobSystemConfig{workers});
        sim::SimConfig cfg;
        cfg.seed = opt.seed;
        sim::SimHost host(types, js, cfg);
        galactic::GalacticSim* gs = host.start_galactic(rules, setup);
        if (!gs) return false;
        const auto t0 = std::chrono::steady_clock::now();
        const uint64_t n = host.run_fast(opt.ticks);
        log = host.crc_log();
        std::printf("campaign %s workers %d: %" PRIu64 " ticks (%.1f ms), galactic frame %d, fiscal cycles %d, planets %zu, "
                    "fleets %zu, objects %zu, credits:",
                    name.c_str(), workers, n, ms_since(t0), gs->frame(), gs->galactic_day(), gs->planets().size(),
                    gs->fleets().size(), gs->store().live_count());
        for (size_t p = 0; p < gs->players().size(); ++p)
            std::printf(" %zu=%.0f/%d", p, static_cast<double>(gs->credits(static_cast<int32_t>(p))),
                        gs->planets_owned(static_cast<int32_t>(p)));
        std::printf(", final CRC %016" PRIx64 " parity %08x\n", log.records.empty() ? 0 : log.records.back().combined,
                    log.records.empty() ? 0u : log.records.back().parity);
        return true;
    };
    CrcLog first;
    if (!run_one(opt.check_workers ? jobs::determinism_worker_counts().front() : opt.workers, first)) {
        std::fprintf(stderr, "economy: GalacticSim::start failed\n");
        return 1;
    }
    bool ok = true;
    if (opt.check_workers) {
        const auto counts = jobs::determinism_worker_counts();
        for (size_t i = 1; i < counts.size(); ++i) {
            CrcLog other;
            run_one(counts[i], other);
            const auto d = first_divergence(first, other);
            std::printf("  workers %d vs %d: %s\n", counts.front(), counts[i], d.message.c_str());
            if (d.diverged) ok = false;
        }
    }
    first.comments = {"economy campaign " + name + " seed " + std::to_string(opt.seed) + " ticks " + std::to_string(opt.ticks)};
    if (!opt.crc_out.empty() && first.save(opt.crc_out, &err)) std::printf("wrote %s\n", opt.crc_out.c_str());
    if (!opt.golden.empty()) {
        CrcLog golden;
        if (!CrcLog::load(opt.golden, golden, &err)) {
            std::fprintf(stderr, "golden: %s\n", err.c_str());
            return 2;
        }
        const auto d = first_divergence(golden, first);
        std::printf("golden: %s\n", d.message.c_str());
        if (d.diverged) ok = false;
    }
    std::printf(ok ? "economy run: OK\n" : "economy run: FAILED\n");
    return ok ? 0 : 1;
}

int story_run(const MapRunOptions& opt, const std::string& data_dir, const std::string& story_out,
              const std::string& story_golden) {
    using namespace alamo;
    if (!sim::SimHost::story_supported()) {
        std::fprintf(stderr, "--story: this build has no engine/lua\n");
        return 2;
    }
    std::unique_ptr<FileProvider> files;
    std::string err;
    if (!data_dir.empty()) {
        files = std::make_unique<DirectoryProvider>(data_dir);
    } else {
        const std::string root = opt.game_dir.empty() ? env_or_empty("ALAMO_FOC_DATA") : opt.game_dir;
        if (root.empty()) {
            std::fprintf(stderr, "--story needs --game <corruption dir>, --data <dir> or ALAMO_FOC_DATA\n");
            return 2;
        }
        vfs::VfsConfig vcfg;
        vcfg.foc_root = root;
        vfs::parse_command_line(opt.game_args, vcfg);
        files = vfs::Vfs::create(vcfg, &err);
        if (!files) {
            std::fprintf(stderr, "VFS: %s\n", err.c_str());
            return 2;
        }
    }
    std::vector<std::string> diag;
    data::GameDatabase db;
    objects::ObjectTypeDatabase raw;
    objects::GameObjectTypes types;
    galactic::GalacticRules rules;
    if (!db.load(*files, {}, &diag) || !raw.load(*files, &diag) || !types.build(raw, files.get(), &diag) ||
        !galactic::load_galactic_rules(*files, db, types, rules, &diag)) {
        std::fprintf(stderr, "story: game data incomplete (%zu diagnostics)\n", diag.size());
        return 2;
    }
    const std::string name = !opt.map.empty() ? opt.map : std::string("Full_Story_Campaign_Underworld");
    const data::CampaignDef* camp = nullptr;
    for (const auto& c : db.campaigns())
        if (c.name == name) camp = &c;
    if (!camp) {
        std::fprintf(stderr, "story: campaign '%s' not found\n", name.c_str());
        return 2;
    }
    const galactic::GalacticSetup setup = galactic::build_campaign_setup(db, rules, name, {}, &diag);
    const std::string header = "story campaign " + name + " seed " + std::to_string(opt.seed) + " ticks " + std::to_string(opt.ticks);
    auto run_one = [&](int32_t workers, std::string& log, CrcLog& crc) -> bool {
        jobs::JobSystem js(jobs::JobSystemConfig{workers});
        sim::SimConfig cfg;
        cfg.seed = opt.seed;
        cfg.story_crc = true;
        sim::SimHost host(types, js, cfg);
        galactic::GalacticSim* gs = host.start_galactic(rules, setup);
        if (!gs) return false;
        sim::StoryStart st;
        st.files = files.get();
        st.plot_sets = sim::campaign_plot_sets(*camp, *gs);
        std::vector<std::string> sd;
        if (!host.start_story(st, &sd)) {
            for (const auto& d : sd) std::fprintf(stderr, "%s\n", d.c_str());
            return false;
        }
        const auto t0 = std::chrono::steady_clock::now();
        const uint64_t n = host.run_fast(opt.ticks);
        const sim::StorySession& s = *host.story();
        log = s.log_text(header);
        crc = host.crc_log();
        std::printf("story %s workers %d: %" PRIu64 " ticks (%.1f ms), %zu plot sets, %zu scripts, %zu triggers, %zu story events "
                    "fired, %zu Lua messages, final CRC %016" PRIx64 "\n",
                    name.c_str(), workers, n, ms_since(t0), s.plot_set_count(), s.script_count(), s.triggers().size(),
                    s.log().size(), s.lua_messages().size(), crc.records.empty() ? 0 : crc.records.back().combined);
        if (!opt.quiet && workers == (opt.check_workers ? jobs::determinism_worker_counts().front() : opt.workers)) {
            for (const auto& e : s.log()) std::printf("  %s\n", e.text().c_str());
            for (const auto& m : s.lua_messages()) std::printf("  lua: %s\n", m.c_str());
        }
        return true;
    };
    std::string log;
    CrcLog crc;
    if (!run_one(opt.check_workers ? jobs::determinism_worker_counts().front() : opt.workers, log, crc)) {
        std::fprintf(stderr, "story: campaign start failed\n");
        return 1;
    }
    bool ok = true;
    if (opt.check_workers) {
        const auto counts = jobs::determinism_worker_counts();
        for (size_t i = 1; i < counts.size(); ++i) {
            std::string l2;
            CrcLog c2;
            run_one(counts[i], l2, c2);
            const auto d = first_divergence(crc, c2);
            std::printf("  workers %d vs %d: CRC %s, story log %s\n", counts.front(), counts[i], d.message.c_str(),
                        l2 == log ? "identical" : "DIFFERS");
            if (d.diverged || l2 != log) ok = false;
        }
    }
    crc.comments = {header};
    if (!story_out.empty()) {
        std::ofstream(alamo::path_from_utf8(story_out), std::ios::binary) << log;
        std::printf("wrote %s\n", story_out.c_str());
    }
    if (!opt.crc_out.empty() && crc.save(opt.crc_out, &err)) std::printf("wrote %s\n", opt.crc_out.c_str());
    if (!story_golden.empty()) {
        std::ifstream in(alamo::path_from_utf8(story_golden), std::ios::binary);
        std::string want((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        want.erase(std::remove(want.begin(), want.end(), '\r'), want.end());
        // '#' header lines (run description) are not compared.
        auto body = [](const std::string& s) {
            std::string out;
            size_t i = 0;
            while (i < s.size()) {
                const size_t e = std::min(s.find('\n', i), s.size());
                if (s[i] != '#') out.append(s, i, e - i).push_back('\n');
                i = e + 1;
            }
            return out;
        };
        const bool same = !want.empty() && body(want) == body(log);
        std::printf("story golden %s: %s\n", story_golden.c_str(), same ? "MATCH" : "DIFFERS");
        if (!same) ok = false;
    }
    if (!opt.golden.empty()) {
        CrcLog golden;
        if (!CrcLog::load(opt.golden, golden, &err)) {
            std::fprintf(stderr, "golden: %s\n", err.c_str());
            return 2;
        }
        const auto d = first_divergence(golden, crc);
        std::printf("golden: %s\n", d.message.c_str());
        if (d.diverged) ok = false;
    }
    std::printf(ok ? "story run: OK\n" : "story run: FAILED\n");
    return ok ? 0 : 1;
}

int map_run(const MapRunOptions& opt) {
    using namespace alamo;
    const std::string root = opt.game_dir.empty() ? env_or_empty("ALAMO_FOC_DATA") : opt.game_dir;
    if (root.empty()) {
        std::fprintf(stderr, "--map needs --game <corruption dir> or ALAMO_FOC_DATA\n");
        return 2;
    }
    vfs::VfsConfig vcfg;
    vcfg.foc_root = root;
    vfs::parse_command_line(opt.game_args, vcfg);
    std::string err;
    auto files = vfs::Vfs::create(vcfg, &err);
    if (!files) {
        std::fprintf(stderr, "VFS: %s\n", err.c_str());
        return 2;
    }
    auto t0 = std::chrono::steady_clock::now();
    objects::ObjectTypeDatabase db;
    if (!db.load(*files)) {
        std::fprintf(stderr, "GameObjectFiles.xml not found under %s\n", root.c_str());
        return 2;
    }
    objects::GameObjectTypes types;
    types.build(db, files.get());
    const sim::TypeCrcIndex index(types);
    std::printf("types: %zu definitions (%zu names) in %.0f ms\n", types.size(), index.size(), ms_since(t0));
    terrain::MapData map;
    if (!terrain::load_map(*files, opt.map, map, &err)) {
        std::fprintf(stderr, "map %s: %s\n", opt.map.c_str(), err.c_str());
        return 2;
    }

    commands::CommandRecording replay;
    if (!opt.replay.empty() && !commands::CommandRecording::load(opt.replay, replay, &err)) {
        std::fprintf(stderr, "replay: %s\n", err.c_str());
        return 2;
    }
    sim::MapRunSpec spec;
    spec.ticks = opt.ticks;
    if (!opt.replay.empty() && !opt.ticks_set && replay.end_tick > 1) spec.ticks = replay.end_tick - 1;
    spec.seed = opt.seed;
    spec.ai = opt.ai;
    spec.realtime = opt.realtime;
    spec.replay = opt.replay.empty() ? nullptr : &replay;

    auto print_run = [&](const sim::MapRunResult& r, int32_t w, bool ticks) {
        if (ticks && !opt.quiet)
            for (const CrcRecord& c : r.log.records)
                std::printf("tick %5" PRIu64 "  crc %016" PRIx64 "  parity %08x\n", c.tick, c.combined, c.parity);
        const uint64_t fin = r.log.records.empty() ? 0 : r.log.records.back().combined;
        const uint32_t par = r.log.records.empty() ? 0 : r.log.records.back().parity;
        std::printf("map %s workers %d: %" PRIu64 " ticks (%.1f ms, %.0f ticks/s), commands applied %" PRIu64
                    " rejected %" PRIu64 ", live objects %u, orders pending %u, final CRC %016" PRIx64 " parity %08x\n",
                    opt.map.c_str(), w, r.ticks, r.run_ms, r.run_ms > 0 ? static_cast<double>(r.ticks) * 1000.0 / r.run_ms : 0.0,
                    r.applied, r.rejected, r.live_objects, r.orders_pending, fin, par);
    };

    commands::CommandRecording rec;
    spec.record = opt.record_orders.empty() ? nullptr : &rec;
    spec.workers = opt.check_workers ? alamo::jobs::determinism_worker_counts().front() : opt.workers;
    sim::MapRunResult first = sim::run_map(types, map, index, spec);
    const sim::SpawnReport& s = first.spawn;
    std::printf("%s map %s: %u placements, %u spawned (%u units, %u structures, %u props), %u unknown types, "
                "%u in spatial trees, owners:", map.is_space() ? "space" : "land", opt.map.c_str(), s.placements,
                s.spawned, s.units, s.structures, s.props, s.unknown_type, s.in_trees);
    for (const auto& [o, n] : s.per_owner) std::printf(" %d=%u", o, n);
    std::printf("\n");
    print_run(first, spec.workers, true);

    bool ok = true;
    if (opt.check_workers) {
        const auto counts = alamo::jobs::determinism_worker_counts();
        for (size_t i = 1; i < counts.size(); ++i) {
            sim::MapRunSpec o = spec;
            o.workers = counts[i];
            o.record = nullptr;
            const sim::MapRunResult r = sim::run_map(types, map, index, o);
            print_run(r, o.workers, false);
            const auto d = first_divergence(first.log, r.log);
            std::printf("  workers %d vs %d: %s\n", counts.front(), counts[i], d.message.c_str());
            if (d.diverged) ok = false;
        }
    }
    first.log.comments = {"map " + opt.map + " seed " + std::to_string(opt.replay.empty() ? opt.seed : replay.seed) +
                          " ticks " + std::to_string(first.ticks) +
                          (opt.replay.empty() ? std::string(" live orders")
                                              : " replay " + std::filesystem::path(opt.replay).filename().string()),
                          "spawned " + std::to_string(s.spawned) + " commands applied " + std::to_string(first.applied) +
                              " rejected " + std::to_string(first.rejected)};
    if (!opt.record_orders.empty()) {
        rec.meta = {{"map", opt.map}};
        if (!rec.save(opt.record_orders, &err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return 2;
        }
        std::printf("wrote %s: %zu commands in %zu batches\n", opt.record_orders.c_str(), rec.command_count(),
                    rec.batches.size());
    }
    if (!opt.crc_out.empty()) {
        if (!first.log.save(opt.crc_out, &err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return 2;
        }
        std::printf("wrote %s (%zu records)\n", opt.crc_out.c_str(), first.log.records.size());
    }
    if (!opt.golden.empty()) {
        CrcLog golden;
        if (!CrcLog::load(opt.golden, golden, &err)) {
            std::fprintf(stderr, "golden: %s\n", err.c_str());
            return 2;
        }
        const auto d = first_divergence(golden, first.log);
        std::printf("golden %s: %s\n", std::filesystem::path(opt.golden).filename().string().c_str(), d.message.c_str());
        if (d.diverged) ok = false;
    }
    std::printf(ok ? "map run: OK\n" : "map run: FAILED\n");
    return ok ? 0 : 1;
}
