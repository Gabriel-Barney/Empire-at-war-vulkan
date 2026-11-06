// BM2a (alamo_game_benchmark_tests, data-free): the benchmark mode's render-free half (benchmark.hpp).
//   * flags: every --bench-* flag, bad values refused, non-benchmark flags left alone, the option check;
//   * presets: names unique and findable, the default present, sizes / levels in range, V-sync off and no cap;
//   * FixedFrameClock: K counted frames per tick, the first frame after a tick, alpha (sub + 1) / K, presentation time
//     counted frames / (30 K), finished only after the last tick's K frames;
//   * frame_time_stats: nearest-rank median / p99, worst, mean, order independence, empty input;
//   * bench_report_json: key order, escaping, CRC as 16 hex digits.
#include <cmath>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "alamo/game/benchmark.hpp"

using namespace alamo;
using namespace alamo::game;

static int g_fail = 0;
#define CHECK(c)                                                     \
    do {                                                             \
        if (!(c)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++g_fail;                                                \
        }                                                            \
    } while (0)

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
                     "--bench-crc-out", "c.crc", "--bench-png-dir", "png", "--bench-png-every", "30", "--bench-no-verify", "--width", "800"},
                    o, rest, err) == 0);
    CHECK(o.mode == BenchMode::FixedFrames && o.fixed() && o.active());
    CHECK(o.save == "C:\\s\\a.alsv" && o.preset == "bm-low" && o.ticks == 120 && o.frames_per_tick == 3 && o.settle_frames == 5);
    CHECK(o.max_unsettled == 40 && o.report == "r.json" && o.crc_out == "c.crc" && o.png_dir == "png" && o.png_every == 30 && !o.verify);
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
    const std::string j = bench_report_json(r);
    CHECK(j.rfind("{\n  \"alamo_game_benchmark\": 1,\n  \"result\": \"ok\",", 0) == 0);
    CHECK(j.find("\"error\"") == std::string::npos);  // only when failed
    CHECK(j.find("\"save\": \"C:\\\\saves\\\\a \\\"b\\\".alsv\"") != std::string::npos);
    CHECK(j.find("\"final_crc\": \"00000000000000ab\"") != std::string::npos);
    CHECK(j.find("\"frame_ms\": {\"count\": 3, \"mean\": 2.0000, \"median\": 2.0000, \"p99\": 3.0000, \"worst\": 3.0000, \"total\": 6.0000}") !=
          std::string::npos);
    CHECK(j.find("\"autopilot\": [\"load screen: Begin at tick 5\"]\n}\n") != std::string::npos);
    CHECK(j.find("\"mode\"") < j.find("\"preset\"") && j.find("\"preset\"") < j.find("\"frame_ms\""));
    CHECK(bench_report_json(r) == j);  // stable
    r.result = "failed";
    r.error = "line\nbreak";
    const std::string f = bench_report_json(r);
    CHECK(f.find("\"error\": \"line\\u000abreak\"") != std::string::npos);
}

}  // namespace

int main() {
    test_flags();
    test_presets();
    test_fixed_clock();
    test_stats();
    test_report();
    if (g_fail) {
        std::printf("alamo_game_benchmark_tests: %d FAILED\n", g_fail);
        return 1;
    }
    std::printf("alamo_game_benchmark_tests: OK\n");
    return 0;
}
