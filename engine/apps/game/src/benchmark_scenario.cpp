// bm2: BM1 scenario files in alamo_game, the render-free half (benchmark_scenario.hpp).
#include "alamo/game/benchmark_scenario.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>

namespace alamo::game {

namespace fs = std::filesystem;
namespace bench = sim::bench;

namespace {

void fail(std::string* err, std::string msg) {
    if (err) *err = std::move(msg);
}

// Records with tick <= last.
CrcLog until(const CrcLog& log, uint64_t last) {
    CrcLog out = log;
    out.records.clear();
    for (const CrcRecord& r : log.records)
        if (r.tick <= last) out.records.push_back(r);
    return out;
}

bool read_bytes(const std::string& path, std::string& out) {
    std::ifstream f(fs::path(path), std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return !f.bad();
}

bool write_bytes(const std::string& path, const std::string& bytes) {
    std::ofstream f(fs::path(path), std::ios::binary | std::ios::trunc);
    return f && f.write(bytes.data(), static_cast<std::streamsize>(bytes.size())) && f.flush();
}

// The fingerprint of a made scenario: a checkpoint log (every 16th tick plus the first and last), as BM1's space battle.
constexpr uint32_t kFingerprintEvery = 16;

}  // namespace

void apply_bench_scenario(const bench::Scenario& sc, BenchmarkOptions& o) {
    if (sc.save.present()) o.save = sc.resolve(sc.save);
    if (sc.ticks) o.ticks = sc.ticks;
    o.mode = sc.mode == "realtime" ? BenchMode::RealTime : BenchMode::FixedFrames;  // no mode key: fixed frames
    if (!sc.preset.empty()) o.preset = sc.preset;
    if (sc.frames_per_tick) o.frames_per_tick = sc.frames_per_tick;
}

int load_bench_scenario_arg(std::span<const std::string> args, BenchmarkOptions& o, std::string* err) {
    std::string flag;
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] != "--bench-scenario" && args[i] != "--bench-make") continue;
        if (!flag.empty()) {
            fail(err, args[i] + ": only one --bench-scenario or --bench-make per run");
            return -1;
        }
        if (i + 1 >= args.size()) {
            fail(err, args[i] + ": needs a value");
            return -1;
        }
        flag = args[i];
        o.scenario = args[++i];
        o.make = flag == "--bench-make";
    }
    if (flag.empty()) return 0;
    auto sc = std::make_shared<bench::Scenario>();
    std::string e;
    if (o.make) {
        // A recipe, a full scenario (remade over itself) or nothing yet (a new scenario named after the file).
        std::error_code ec;
        if (fs::exists(fs::path(o.scenario), ec)) {
            if (!bench::Scenario::load_recipe(o.scenario, *sc, &e)) {
                fail(err, "--bench-make: " + e);
                return -1;
            }
        } else {
            sc->name = fs::path(o.scenario).stem().string();
            sc->dir = fs::path(o.scenario).parent_path().string();
            if (sc->name.empty() || sc->name.find_first_of(" \t") != std::string::npos) {
                fail(err, "--bench-make: the scenario file name must be one word (" + o.scenario + ")");
                return -1;
            }
        }
        if (sc->host == bench::Host::Headless && sc->save.present()) {
            fail(err, "--bench-make: " + o.scenario + " is an alamo_headless scenario (host headless): remake it with alamo_headless --bench-regen");
            return -1;
        }
    } else {
        if (!bench::Scenario::load(o.scenario, *sc, &e)) {
            fail(err, "--bench-scenario: " + e);
            return -1;
        }
        if (sc->host != bench::Host::Game) {
            fail(err, "--bench-scenario: " + o.scenario +
                          " is an alamo_headless scenario (host headless): its save is an alamo_headless --save-session harness session, "
                          "which is not an alamo_game session; run it with alamo_headless --bench, or make a game scenario with "
                          "alamo_game --bench-make");
            return -1;
        }
        if (!bench::verify_files(*sc, &e)) {
            fail(err, "--bench-scenario: scenario changed, not comparable: " + e);
            return -1;
        }
    }
    apply_bench_scenario(*sc, o);
    o.scenario_data = std::move(sc);
    return 1;
}

std::string check_bench_scenario(const BenchmarkOptions& o) {
    if (o.scenario.empty()) return {};
    if (!o.active()) return "--bench-scenario / --bench-make need a benchmark mode";
    if (!o.scenario_data) return "the scenario was not loaded";
    const bench::Scenario& sc = *o.scenario_data;
    if (o.make) {
        if (o.save.empty()) return "--bench-make needs --bench-save FILE (or a scenario that names its save)";
        if (!o.verify) return "--bench-make needs the presentation check (no --bench-no-verify): it proves the recorded orders replay";
        return {};
    }
    if (o.ticks > sc.ticks) return "--bench-ticks " + std::to_string(o.ticks) + " is more than the scenario's " + std::to_string(sc.ticks);
    if (o.save != sc.resolve(sc.save)) return "--bench-save and --bench-scenario do not mix (the scenario names its save)";
    return {};
}

std::string check_bench_fingerprint(const bench::Scenario& sc, const CrcLog& run, uint64_t end_tick) {
    CrcLog fp;
    std::string e;
    if (!CrcLog::load(sc.resolve(sc.fingerprint), fp, &e)) return "fingerprint: " + e;
    const CrcLog expected = until(fp, end_tick);
    if (expected.records.empty()) return "the fingerprint has no record at or before tick " + std::to_string(end_tick);
    const CrcLog actual = until(run, expected.records.back().tick);
    const CrcDivergence d = first_divergence(expected, actual);
    return d.diverged ? d.message : std::string();
}

bool write_game_scenario(const std::string& path, const bench::Scenario* base, const std::string& save,
                         const commands::CommandRecording& orders, const CrcLog& log, uint64_t start_tick, uint64_t ticks,
                         const BenchmarkOptions& o, std::string* summary, std::string* err) {
    const fs::path manifest(path);
    const fs::path dir = manifest.parent_path();
    std::error_code ec;
    if (!dir.empty()) fs::create_directories(dir, ec);
    bench::Scenario sc;
    if (base) sc = *base;  // name, description, camera, version
    if (sc.name.empty()) sc.name = manifest.stem().string();
    sc.dir = dir.string();
    auto in_dir = [&](const std::string& f) { return (dir.empty() ? fs::path(f) : dir / f).string(); };
    const std::string save_name = sc.name + ".alsv", orders_name = sc.name + ".acmd", fp_name = sc.name + ".crc";
    const std::string tmp_save = in_dir(save_name + ".tmp"), tmp_orders = in_dir(orders_name + ".tmp"), tmp_fp = in_dir(fp_name + ".tmp");
    const std::string tmp_manifest = path + ".tmp";
    auto cleanup = [&] {
        std::error_code e2;
        for (const std::string& p : {tmp_save, tmp_orders, tmp_fp, tmp_manifest}) fs::remove(fs::path(p), e2);
    };
    std::string e;
    std::string save_bytes;
    if (!read_bytes(save, save_bytes)) {
        fail(err, "cannot read " + save);
        return false;
    }
    CrcLog fp = until(log, start_tick + ticks);
    fp.comments = {"bench scenario " + sc.name + ": alamo_game " + std::string(bench_mode_name(o.mode)) + " run from tick " +
                   std::to_string(start_tick) + ", " + std::to_string(ticks) + " ticks"};
    fp = fp.checkpointed(kFingerprintEvery);
    if (!write_bytes(tmp_save, save_bytes) || !orders.save(tmp_orders, &e) || !fp.save(tmp_fp, &e)) {
        fail(err, e.empty() ? "cannot write next to " + path : e);
        cleanup();
        return false;
    }
    // make_args (bm3): kept from the scenario / recipe - the --bench-new-save arguments `alamo_game --bench-regen S` remakes the save with.
    sc.save.path = save_name;
    sc.orders.path = orders_name;
    sc.fingerprint.path = fp_name;
    for (auto [tmp, file] : {std::pair{tmp_save, &sc.save}, std::pair{tmp_orders, &sc.orders}, std::pair{tmp_fp, &sc.fingerprint}}) {
        if (!bench::pin_file(tmp, *file, &e)) {
            fail(err, e);
            cleanup();
            return false;
        }
    }
    sc.fingerprint_every = kFingerprintEvery;
    sc.start_tick = start_tick;
    sc.ticks = ticks;
    sc.workers = -1;
    sc.final_crc = fp.records.empty() ? 0 : fp.records.back().combined;
    sc.host = bench::Host::Game;
    sc.mode = bench_mode_name(o.mode);
    sc.preset = o.preset;
    sc.frames_per_tick = o.fixed() ? o.frames_per_tick : 0;
    // The version: the scenario on disk now (a full one), kept when the remade one is the same work, else bumped.
    bench::Scenario old;
    const bool had_full = bench::Scenario::load(path, old, nullptr);
    sc.version = had_full ? (old.same_content(sc) ? old.version : old.version + 1) : 1;
    if (!sc.save_manifest(tmp_manifest, &e)) {
        fail(err, e);
        cleanup();
        return false;
    }
    // The files first, the manifest last: a failure before the moves leaves the old scenario untouched.
    for (auto [tmp, final_path] : {std::pair{tmp_save, in_dir(save_name)}, std::pair{tmp_orders, in_dir(orders_name)},
                                   std::pair{tmp_fp, in_dir(fp_name)}, std::pair{tmp_manifest, path}}) {
        fs::rename(fs::path(tmp), fs::path(final_path), ec);
        if (ec) {
            fail(err, final_path + ": " + ec.message() + " (rerun --bench-make)");
            return false;
        }
    }
    if (summary)
        *summary = "wrote " + path + " version " + std::to_string(sc.version) +
                   (had_full ? (sc.version == old.version ? " (unchanged)" : " (changed: version bumped)") : "") + ": save " +
                   std::to_string(sc.save.bytes) + " bytes, " + std::to_string(orders.command_count()) + " orders, fingerprint " +
                   std::to_string(fp.records.size()) + " records, final CRC " + bench::hex64(sc.final_crc);
    return true;
}

}  // namespace alamo::game
