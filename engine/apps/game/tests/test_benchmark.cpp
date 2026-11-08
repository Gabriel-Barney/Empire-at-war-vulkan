// BM2a (alamo_game_benchmark_tests, data-free): the benchmark mode's render-free half (benchmark.hpp).
//   * flags: every --bench-* flag, bad values refused, non-benchmark flags left alone, the option check;
//   * presets: names unique and findable, the default present, sizes / levels in range, V-sync off and no cap;
//   * FixedFrameClock: K counted frames per tick, the first frame after a tick, alpha (sub + 1) / K, presentation time
//     counted frames / (30 K), finished only after the last tick's K frames;
//   * frame_time_stats: nearest-rank median / p99, worst, mean, order independence, empty input;
//   * bench_report_json: key order, escaping, CRC as 16 hex digits.
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include "alamo/game/benchmark.hpp"
#include "alamo/game/benchmark_save.hpp"
#include "alamo/game/benchmark_scenario.hpp"
#include "alamo/test/support.hpp"

using namespace alamo;
using namespace alamo::game;

using ::alamo::test::g_fail;

namespace {

// Parses a whole command line the way main() does: benchmark flags consumed, the rest returned.
int parse_all(const std::vector<std::string>& args, BenchmarkOptions& o, std::vector<std::string>& rest, std::string& err) {
    for (size_t i = 0; i < args.size(); ++i) {
        const int r = parse_benchmark_arg(args, i, o, &err);
        if (r < 0) return -1;
        if (r == 0) rest.push_back(args[i]);
    }
    return 0;
}

void test_flags() {
    BenchmarkOptions o;
    CHECK(!o.active());
    CHECK(check_benchmark_options(o).empty());  // off: nothing to check
    std::vector<std::string> rest;
    std::string err;
    CHECK(parse_all({"--hidden", "--benchmark", "fixed", "--bench-save", "C:\\s\\a.alsv", "--bench-preset", "bm-low", "--bench-ticks", "120",
                     "--bench-frames-per-tick", "3", "--bench-settle", "5", "--bench-max-unsettled", "40", "--bench-report", "r.json",
                     "--bench-crc-out", "c.crc", "--bench-png-dir", "png", "--bench-png-every", "30", "--bench-no-verify", "--bench-redraw-every",
                     "4", "--width", "800"},
                    o, rest, err) == 0);
    CHECK(o.mode == BenchMode::FixedFrames && o.fixed() && o.active());
    CHECK(o.save == "C:\\s\\a.alsv" && o.preset == "bm-low" && o.ticks == 120 && o.frames_per_tick == 3 && o.settle_frames == 5);
    CHECK(o.max_unsettled == 40 && o.report == "r.json" && o.crc_out == "c.crc" && o.png_dir == "png" && o.png_every == 30 && !o.verify);
    CHECK(o.redraw_every == 4);
    CHECK(rest == (std::vector<std::string>{"--hidden", "--width", "800"}));
    CHECK(check_benchmark_options(o).empty());

    BenchmarkOptions r;
    rest.clear();
    CHECK(parse_all({"--benchmark", "realtime"}, r, rest, err) == 0);
    CHECK(r.mode == BenchMode::RealTime && !r.fixed());
    CHECK(r.preset == kDefaultBenchPreset && r.ticks == 900 && r.verify);
    CHECK(check_benchmark_options(r).find("--bench-save") != std::string::npos);  // a save is required
    r.save = "x.alsv";
    CHECK(check_benchmark_options(r).empty());

    // Bad values.
    const std::vector<std::vector<std::string>> bad = {{"--benchmark", "slow"},        {"--benchmark"},         {"--bench-ticks", "-3"},
                                                       {"--bench-ticks", "12x"},       {"--bench-ticks", ""},   {"--bench-frames-per-tick", "99999999999"},
                                                       {"--bench-save"}};
    for (const std::vector<std::string>& b : bad) {
        BenchmarkOptions x;
        std::vector<std::string> rr;
        std::string e;
        CHECK(parse_all(b, x, rr, e) == -1);
        CHECK(!e.empty());
    }
    // An unknown --bench-* flag is not consumed (main reports it as unknown).
    {
        BenchmarkOptions x;
        std::vector<std::string> rr;
        CHECK(parse_all({"--bench-unknown"}, x, rr, err) == 0 && rr.size() == 1);
    }
    // The option check.
    BenchmarkOptions c = o;
    c.preset = "nope";
    CHECK(check_benchmark_options(c).find("bm-high") != std::string::npos);  // lists the presets
    c = o;
    c.ticks = 0;
    CHECK(!check_benchmark_options(c).empty());
    c = o;
    c.frames_per_tick = 0;
    CHECK(!check_benchmark_options(c).empty());
    c.frames_per_tick = 65;
    CHECK(!check_benchmark_options(c).empty());
    c = o;
    c.png_dir.clear();
    CHECK(!check_benchmark_options(c).empty());
    c = o;
    c.max_unsettled = 0;
    CHECK(!check_benchmark_options(c).empty());
    c = o;
    c.redraw_every = 1;  // every frame redrawn: the run could never count one
    CHECK(!check_benchmark_options(c).empty());
    c = o;
    c.mode = BenchMode::RealTime;  // forced redraws are a fixed-frames test tool
    CHECK(!check_benchmark_options(c).empty());
    c.redraw_every = 0;
    CHECK(check_benchmark_options(c).empty());
    CHECK(std::string(bench_mode_name(BenchMode::RealTime)) == "realtime" && std::string(bench_mode_name(BenchMode::FixedFrames)) == "fixed");
}

void test_presets() {
    std::set<std::string_view> names;
    for (const BenchPreset& p : bench_presets()) {
        CHECK(names.insert(p.name).second);
        CHECK(find_bench_preset(p.name) == &p);
        CHECK(p.width >= 320 && p.height >= 200);
        CHECK(p.detail >= 0 && p.detail <= 100 && p.anti_alias >= 0 && p.anti_alias <= 100 && p.gamma == 50);
        CHECK(p.anisotropy == 0 || p.anisotropy == 2 || p.anisotropy == 4 || p.anisotropy == 8 || p.anisotropy == 16);
        CHECK(!p.vsync && p.frame_cap == 0 && p.audio == BenchAudio::Null);
        CHECK(!p.description.empty());
    }
    CHECK(names.size() >= 4);
    CHECK(find_bench_preset(kDefaultBenchPreset) != nullptr);
    CHECK(find_bench_preset("BM-HIGH") == nullptr);  // names are exact
    CHECK(find_bench_preset("") == nullptr);
}

void test_fixed_clock() {
    for (uint32_t k : {1u, 2u, 3u, 5u}) {
        FixedFrameClock c(k);
        CHECK(c.frames_per_tick() == k);
        CHECK(c.tick_due());  // the first counted frame follows a tick
        CHECK(c.seconds() == 0.0);
        std::vector<int> frames_per_tick;
        const uint64_t target = 4;
        uint64_t guard = 0;
        while (!c.finished(target) && ++guard < 1000) {
            if (c.tick_due()) {
                c.ticked();
                frames_per_tick.push_back(0);
            }
            const uint32_t sub = static_cast<uint32_t>(frames_per_tick.back());
            CHECK(std::fabs(c.alpha() - static_cast<float>(sub + 1) / static_cast<float>(k)) < 1e-6f);
            c.frame_counted();
            ++frames_per_tick.back();
        }
        CHECK(c.ticks() == target);
        CHECK(c.frames() == target * k);
        CHECK(frames_per_tick.size() == target);
        for (const int n : frames_per_tick) CHECK(n == static_cast<int>(k));
        CHECK(std::fabs(c.seconds() - static_cast<double>(target) / 30.0) < 1e-12);  // 30 ticks per presentation second
        CHECK(!c.tick_due() || c.finished(target));
        CHECK(std::fabs(c.alpha() - 1.0f) < 1e-6f);  // the last frame drew the tick itself
    }
    FixedFrameClock z(0);  // 0 is treated as 1
    CHECK(z.frames_per_tick() == 1);
    // Not finished while the last tick's frames are still to be drawn.
    FixedFrameClock c(3);
    c.ticked();
    c.frame_counted();
    CHECK(!c.finished(1));
    c.frame_counted();
    c.frame_counted();
    CHECK(c.finished(1) && !c.finished(2));
}

void test_stats() {
    const FrameTimeStats e = frame_time_stats({});
    CHECK(e.count == 0 && e.median == 0.0 && e.p99 == 0.0 && e.worst == 0.0);
    std::vector<float> v;
    for (int i = 1; i <= 100; ++i) v.push_back(static_cast<float>(i));
    FrameTimeStats s = frame_time_stats(v);
    CHECK(s.count == 100 && s.median == 50.0 && s.p99 == 99.0 && s.worst == 100.0);
    CHECK(std::fabs(s.mean - 50.5) < 1e-9 && std::fabs(s.total - 5050.0) < 1e-9);
    std::vector<float> rev(v.rbegin(), v.rend());  // order independent
    const FrameTimeStats s2 = frame_time_stats(rev);
    CHECK(s2.median == s.median && s2.p99 == s.p99 && s2.worst == s.worst && s2.total == s.total);
    const std::vector<float> one = {7.5f};
    s = frame_time_stats(one);
    CHECK(s.count == 1 && s.median == 7.5 && s.p99 == 7.5 && s.worst == 7.5 && s.mean == 7.5);
    const std::vector<float> spike = {10, 10, 10, 10, 10, 10, 10, 10, 10, 250};
    s = frame_time_stats(spike);
    CHECK(s.median == 10.0 && s.p99 == 250.0 && s.worst == 250.0);  // nearest rank: ceil(0.99 x 10) = 10th
}

void test_report() {
    BenchReport r;
    r.mode = "fixed";
    r.preset = "bm-smoke";
    r.save = "C:\\saves\\a \"b\".alsv";
    r.present_mode = "immediate";
    r.width = 640;
    r.height = 360;
    r.frames_per_tick = 2;
    r.ticks = r.target_ticks = 60;
    r.frames = 120;
    r.final_crc = 0x00ab;
    r.frame_ms = frame_time_stats(std::vector<float>{1.0f, 2.0f, 3.0f});
    r.autopilot = {"load screen: Begin at tick 5"};
    r.presentation_check = "match";
    r.device = "GPU";
    r.taa = true;
    r.forced_redraws = 7;
    r.taa_discarded = 9;
    const std::string j = bench_report_json(r);
    CHECK(j.find("\"taa\": true,") != std::string::npos);
    CHECK(j.find("\"forced_redraws\": 7,\n  \"taa_discarded\": 9,") != std::string::npos);
    CHECK(j.find("\"excluded_frames\"") < j.find("\"forced_redraws\"") && j.find("\"taa_discarded\"") < j.find("\"wall_seconds\""));
    CHECK(j.rfind("{\n  \"alamo_game_benchmark\": 1,\n  \"result\": \"ok\",", 0) == 0);
    CHECK(j.find("\"error\"") == std::string::npos);  // only when failed
    CHECK(j.find("\"save\": \"C:\\\\saves\\\\a \\\"b\\\".alsv\"") != std::string::npos);
    CHECK(j.find("\"final_crc\": \"00000000000000ab\"") != std::string::npos);
    CHECK(j.find("\"device\": \"GPU\"") != std::string::npos);
    CHECK(j.find("\"frame_ms\": {\"count\": 3, \"mean\": 2.0000, \"median\": 2.0000, \"p99\": 3.0000, \"worst\": 3.0000, \"total\": 6.0000}") !=
          std::string::npos);
    CHECK(j.find("\"autopilot\": [\"load screen: Begin at tick 5\"]\n}\n") != std::string::npos);
    CHECK(j.find("\"mode\"") < j.find("\"preset\"") && j.find("\"preset\"") < j.find("\"frame_ms\""));
    CHECK(bench_report_json(r) == j);  // stable
    r.result = "failed";
    r.error = "line\nbreak";
    const std::string f = bench_report_json(r);
    CHECK(f.find("\"error\": \"line\\u000abreak\"") != std::string::npos);
    // CU2 t12 (src/json_text.hpp, shared with the mod check): valid UTF-8 passes through, a byte that does not start a valid
    // UTF-8 sequence is written as its Latin-1 character, so the report stays valid JSON.
    r.error = "caf\xc3\xa9 \xe9";
    CHECK(bench_report_json(r).find("\"error\": \"caf\xc3\xa9 \\u00e9\"") != std::string::npos);
}

// bm2: scenario files in alamo_game (benchmark_scenario.hpp): flags, settings, the headless refusal, the make and the
// fingerprint check, on temporary files.
CrcLog fake_log(uint64_t first, uint64_t last, uint64_t salt) {
    CrcLog log;
    log.systems = {"a", "b"};
    for (uint64_t t = first; t <= last; ++t) {
        CrcRecord r;
        r.tick = t;
        r.systems = {t * 3 + salt, t * 5};
        r.combined = t * 7 + salt;
        log.records.push_back(r);
    }
    return log;
}

void write_file(const std::filesystem::path& p, const std::string& text) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
}

void test_scenario() {
    namespace fs = std::filesystem;
    namespace bench = sim::bench;
    const fs::path dir = fs::temp_directory_path() / "alamo_game_benchmark_tests_scenario";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const std::string recipe = (dir / "orbit.abench").string();
    write_file(recipe, "ALAMO_BENCH 2\nname orbit\ndescription a test\nticks 40\nhost game\nmode realtime\npreset bm-smoke\n"
                       "camera_anchor start\ncamera 0 0 0 0 0 30 900\ncamera 40 0 0 0 90 30 600\n");
    write_file(dir / "game.alsv", "save bytes");
    std::string err;

    // No scenario flag: nothing happens.
    BenchmarkOptions o;
    CHECK(load_bench_scenario_arg(std::vector<std::string>{"alamo_game", "--benchmark", "fixed"}, o, &err) == 0 && !o.scenario_data);
    CHECK(check_bench_scenario(o).empty());
    // Two of them, or no value: refused.
    CHECK(load_bench_scenario_arg(std::vector<std::string>{"g", "--bench-make", recipe, "--bench-scenario", recipe}, o, &err) == -1);
    o = {};
    CHECK(load_bench_scenario_arg(std::vector<std::string>{"g", "--bench-scenario"}, o, &err) == -1 && err.find("value") != std::string::npos);

    // --bench-make on the recipe: its settings apply, flags parsed afterwards win.
    o = {};
    const std::vector<std::string> make_args = {"g", "--bench-make", recipe, "--bench-save", (dir / "game.alsv").string(), "--bench-preset",
                                                "bm-low"};
    CHECK(load_bench_scenario_arg(make_args, o, &err) == 1 && o.make && o.scenario_data);
    CHECK(o.mode == BenchMode::RealTime && o.preset == "bm-smoke" && o.ticks == 40 && o.save.empty());
    for (size_t i = 0; i < make_args.size(); ++i) (void)parse_benchmark_arg(make_args, i, o, &err);
    CHECK(o.preset == "bm-low" && o.save == (dir / "game.alsv").string());
    CHECK(check_bench_scenario(o).empty());
    BenchmarkOptions nv = o;
    nv.verify = false;
    CHECK(check_bench_scenario(nv).find("presentation check") != std::string::npos);

    // The make writes a host game scenario: pins, the run's settings, the recipe's camera and description.
    const CrcLog log = fake_log(300, 340, 0);
    commands::CommandRecording rec;
    rec.from_save = commands::SavePoint{300, 0x1234};
    std::string summary;
    CHECK(write_game_scenario(recipe, o.scenario_data.get(), o.save, rec, log, 300, 40, o, &summary, &err));
    CHECK(summary.find("version 1") != std::string::npos);
    bench::Scenario made;
    CHECK(bench::Scenario::load(recipe, made, &err) && bench::verify_files(made, &err));
    CHECK(made.host == bench::Host::Game && made.mode == "realtime" && made.preset == "bm-low" && made.frames_per_tick == 0);
    CHECK(made.name == "orbit" && made.description == "a test" && made.version == 1 && made.camera.size() == 2);
    CHECK(made.camera_anchor == bench::CameraAnchor::Start && made.start_tick == 300 && made.ticks == 40 && made.fingerprint_every == 16);
    CHECK(made.final_crc == 340 * 7 && made.save.bytes == 10 && made.make_args.empty());
    CHECK(fs::exists(dir / "orbit.alsv") && fs::exists(dir / "orbit.acmd") && fs::exists(dir / "orbit.crc"));
    // Remade with nothing changed: the version stays; with another camera it is bumped.
    BenchmarkOptions o2;
    CHECK(load_bench_scenario_arg(std::vector<std::string>{"g", "--bench-make", recipe, "--bench-preset", "bm-low"}, o2, &err) == 1);
    for (size_t i = 0; i < 4; ++i) (void)parse_benchmark_arg(std::vector<std::string>{"g", "--bench-make", recipe, "--bench-preset", "bm-low"}, i, o2, &err);
    CHECK(o2.save == made.resolve(made.save));  // a full scenario's own save is the default
    CHECK(write_game_scenario(recipe, o2.scenario_data.get(), o2.save, rec, log, 300, 40, o2, &summary, &err) &&
          summary.find("(unchanged)") != std::string::npos);
    bench::Scenario moved = *o2.scenario_data;
    moved.camera[1].yaw_deg = 180;
    CHECK(write_game_scenario(recipe, &moved, o2.save, rec, log, 300, 40, o2, &summary, &err) && summary.find("version 2") != std::string::npos);

    // --bench-scenario: the file's settings, the pins checked; then the fingerprint.
    BenchmarkOptions r;
    CHECK(load_bench_scenario_arg(std::vector<std::string>{"g", "--bench-scenario", recipe}, r, &err) == 1 && !r.make);
    CHECK(r.mode == BenchMode::RealTime && r.ticks == 40 && r.save == made.resolve(made.save) && r.preset == "bm-low");
    CHECK(check_bench_scenario(r).empty());
    BenchmarkOptions longer = r;
    longer.ticks = 41;
    CHECK(check_bench_scenario(longer).find("more than") != std::string::npos);
    const bench::Scenario& sc = *r.scenario_data;
    CHECK(check_bench_fingerprint(sc, log, 340).empty());
    CHECK(check_bench_fingerprint(sc, fake_log(300, 320, 0), 320).empty());  // a shorter run: compared up to its last checkpoint
    CHECK(!check_bench_fingerprint(sc, fake_log(300, 340, 1), 340).empty());  // other work: not comparable
    CHECK(!check_bench_fingerprint(sc, fake_log(300, 330, 0), 340).empty());  // the run stopped early
    // A changed pinned file: refused at load.
    write_file(dir / "orbit.crc", "changed");
    BenchmarkOptions bad;
    CHECK(load_bench_scenario_arg(std::vector<std::string>{"g", "--bench-scenario", recipe}, bad, &err) == -1 &&
          err.find("not comparable") != std::string::npos);

    // A headless (BM1) scenario: alamo_game cannot run it, and says why.
    const std::string headless = (dir / "space.abench").string();
    write_file(headless, "ALAMO_BENCH 1\nname space\nsave s.alsv 1 1\nfingerprint s.crc 1 1\nticks 5\n");
    BenchmarkOptions h;
    CHECK(load_bench_scenario_arg(std::vector<std::string>{"g", "--bench-scenario", headless}, h, &err) == -1 &&
          err.find("alamo_headless scenario (host headless)") != std::string::npos);
    CHECK(load_bench_scenario_arg(std::vector<std::string>{"g", "--bench-make", headless}, h, &err) == -1 &&
          err.find("--bench-regen") != std::string::npos);
    fs::remove_all(dir, ec);
}

void test_report_bm2() {
    BenchReport r;
    r.scenario = "s.abench";
    r.scenario_version = 2;
    r.fingerprint = "match";
    r.camera_keys = 3;
    r.gpu_timestamps = true;
    r.gpu_frames = 2;
    const std::vector<float> ms = {1.0f, 3.0f};
    r.gpu_ms = {{"frame", frame_time_stats(ms)}, {"scene", frame_time_stats(ms)}};
    const std::string j = bench_report_json(r);
    CHECK(j.find("\"scenario\": \"s.abench\",\n  \"scenario_version\": 2,\n  \"fingerprint\": \"match\",\n  \"scenario_made\": \"\",\n"
                 "  \"camera_keys\": 3,\n  \"gpu_timestamps\": true,\n  \"gpu_frames\": 2,\n  \"gpu_ms\": {\"frame\": {\"count\": 2") !=
          std::string::npos);
    CHECK(j.find("}, \"scene\": {\"count\": 2") != std::string::npos);
    CHECK(bench_report_json(BenchReport{}).find("\"gpu_ms\": {},\n") != std::string::npos);
}

}  // namespace

// bm3: the load / first-use numbers (bench_hitches, the report keys) and the --bench-new-save / --bench-maps flags.
void test_bm3() {
    BenchReport r;
    std::vector<float> ms(600, 5.0f);
    ms[0] = 80.0f;   // the cold first frame
    ms[7] = 30.0f;   // a hitch
    ms[450] = 26.0f; // a hitch after the early window
    ms[451] = 24.0f; // under the 25 ms floor: not one
    r.frame_ms = frame_time_stats(ms);
    CHECK(r.frame_ms.median == 5.0);
    bench_hitches(ms, r);
    CHECK(r.hitch_threshold_ms == 25.0);  // 4 x 5 ms = 20 ms is below the floor
    CHECK(r.hitch_count == 3 && r.hitches.size() == 3);
    CHECK(r.hitches[0].first == 0 && r.hitches[0].second == 80.0f);  // worst first
    CHECK(r.hitches[1].first == 7 && r.hitches[2].first == 450);
    CHECK(r.early_ms.count == kEarlyFrames && r.early_ms.worst == 80.0);  // only the first kEarlyFrames frames
    std::vector<float> slow(100, 100.0f);  // a slow scene: the threshold follows the median (4 x 100 ms), nothing is a hitch
    slow[3] = 399.0f;
    BenchReport s;
    s.frame_ms = frame_time_stats(slow);
    bench_hitches(slow, s);
    CHECK(s.hitch_threshold_ms == 400.0 && s.hitch_count == 0 && s.hitches.empty());
    std::vector<float> many(200, 5.0f);
    for (size_t k = 0; k < 20; ++k) many[k * 5] = 40.0f + static_cast<float>(k);
    BenchReport m;
    m.frame_ms = frame_time_stats(many);
    bench_hitches(many, m);
    CHECK(m.hitch_count == 20 && m.hitches.size() == kMaxHitches && m.hitches[0].second == 59.0f);
    BenchReport e;
    bench_hitches(std::vector<float>{}, e);  // no frames: nothing, no crash
    CHECK(e.hitch_count == 0 && e.early_ms.count == 0);

    r.load_ms = 1234.5;
    r.hitch_threshold_ms = 25;
    const std::string j = bench_report_json(r);
    CHECK(j.find("\"load_ms\": 1234.5000,") != std::string::npos);
    CHECK(j.find("\"early_ms\": {\"count\": 300") != std::string::npos);
    CHECK(j.find("\"hitches\": {\"threshold_ms\": 25.0000, \"count\": 3, \"worst\": [[0, 80.0000], [7, 30.0000], [450, 26.0000]]},\n  \"autopilot\"") !=
          std::string::npos);
    CHECK(j.find("\"gpu_ms\"") < j.find("\"load_ms\"") && j.find("\"load_ms\"") < j.find("\"hitches\""));

    auto parse = [](std::vector<std::string> a, BenchNewSave& o, std::string& err) {
        for (size_t i = 0; i < a.size(); ++i) {
            const int k = parse_bench_new_arg(a, i, o, &err);
            if (k <= 0) return k;
        }
        return 1;
    };
    BenchNewSave o;
    std::string err;
    CHECK(!o.active() && o.scale == 1 && o.human == "Empire" && o.ai == "Rebel" && o.save_after == 300 && o.seed == 1);
    CHECK(parse({"--bench-new-save", "a.alsv", "--bench-map", "_MP_SPACE_KUAT", "--bench-human", "Rebel", "--bench-ai", "Empire", "--bench-scale", "6",
                 "--bench-save-after", "1000", "--bench-seed", "9"}, o, err) == 1);
    CHECK(o.out == "a.alsv" && o.map == "_MP_SPACE_KUAT" && o.human == "Rebel" && o.ai == "Empire" && o.scale == 6 && o.save_after == 1000 && o.seed == 9);
    CHECK(o.active() && check_bench_new_save(o).empty());
    BenchNewSave p;
    CHECK(parse({"--bench-maps"}, p, err) == 1 && p.maps && p.active() && check_bench_new_save(p).empty());
    for (const auto& bad : std::vector<std::vector<std::string>>{{"--bench-scale", "0"}, {"--bench-scale", "17"}, {"--bench-scale", "x"}, {"--bench-save-after", "-1"},
                                                                  {"--bench-seed", "4294967296"}, {"--bench-map"}}) {
        BenchNewSave x;
        err.clear();
        CHECK(parse(bad, x, err) == -1 && !err.empty());
    }
    BenchNewSave u;
    CHECK(parse({"--bench-preset", "bm-low"}, u, err) == 0);  // not ours
    BenchNewSave n;
    n.out = "a.alsv";
    CHECK(check_bench_new_save(n).find("--bench-map") != std::string::npos);  // a map is required
    n.map = "_MP_SPACE_KUAT";
    CHECK(check_bench_new_save(n).empty());
    n.ai = "EMPIRE";
    CHECK(check_bench_new_save(n).find("different") != std::string::npos);  // factions compare without case
    n.ai = "";
    CHECK(!check_bench_new_save(n).empty());
    n.ai = "Rebel";
    n.maps = true;
    CHECK(check_bench_new_save(n).find("do not mix") != std::string::npos);
}

int main() {
    test_flags();
    test_presets();
    test_fixed_clock();
    test_stats();
    test_report();
    test_scenario();
    test_report_bm2();
    test_bm3();
    if (g_fail) {
        std::printf("alamo_game_benchmark_tests: %d FAILED\n", g_fail);
        return 1;
    }
    std::printf("alamo_game_benchmark_tests: OK\n");
    return 0;
}
