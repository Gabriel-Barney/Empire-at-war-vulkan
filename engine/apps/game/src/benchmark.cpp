// BM2a: benchmark options, presets, the fixed-frames schedule, frame-time statistics and the JSON report (benchmark.hpp).
#include "alamo/game/benchmark.hpp"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "json_text.hpp"

namespace alamo::game {

const char* bench_mode_name(BenchMode m) {
    switch (m) {
        case BenchMode::Off: return "off";
        case BenchMode::RealTime: return "realtime";
        case BenchMode::FixedFrames: return "fixed";
    }
    return "?";
}

namespace {

// Anti-aliasing modes of the Video dialog's list (ui::kAaOff / ui::kAaTaa in gui_menus.hpp; this library may build without
// the ui module, app_benchmark.cpp checks they agree).
constexpr int32_t kAaOff = 0, kAaTaa = 2;

constexpr BenchPreset make_preset(std::string_view name, std::string_view description, uint32_t w, uint32_t h, int32_t detail,
                                  int32_t aa, int32_t aniso, bool effects, bool ao) {
    BenchPreset p;
    p.name = name;
    p.description = description;
    p.width = w;
    p.height = h;
    p.detail = detail;
    p.anti_alias = aa;
    p.anisotropy = aniso;
    p.soft_shadows = p.heat_distortion = p.bloom = effects;
    p.ambient_occlusion = ao;
    return p;
}

// [port] BM2a: the four quality levels of the Video dialog at 720p / 1080p, plus a small one for quick checks. V-sync off,
// no frame cap, null audio everywhere.
constexpr std::array<BenchPreset, 5> kPresets = {
    make_preset("bm-low", "1280x720, Low detail (0), no TAA, no soft shadows / heat / bloom", 1280, 720, 0, kAaOff, 0, false, false),
    make_preset("bm-medium", "1920x1080, Medium detail (40), no TAA, effects on", 1920, 1080, 40, kAaOff, 4, true, false),
    make_preset("bm-high", "1920x1080, High detail (75), TAA, effects on", 1920, 1080, 75, kAaTaa, 8, true, false),
    make_preset("bm-ultra", "1920x1080, Ultra detail (100), TAA, effects and ambient occlusion on, 16x anisotropy", 1920, 1080,
                100, kAaTaa, 16, true, true),
    make_preset("bm-smoke", "640x360, High detail (75), no TAA: quick test runs", 640, 360, 75, kAaOff, 0, true, false),
};

bool parse_u64(const std::string& s, uint64_t& out) {
    if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos || s.size() > 18) return false;
    out = std::strtoull(s.c_str(), nullptr, 10);
    return true;
}

std::string num(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.4f", std::isfinite(v) ? v : 0.0);
    return buf;
}

std::string stats_json(const FrameTimeStats& s) {
    return "{\"count\": " + std::to_string(s.count) + ", \"mean\": " + num(s.mean) + ", \"median\": " + num(s.median) + ", \"p99\": " + num(s.p99) +
           ", \"worst\": " + num(s.worst) + ", \"total\": " + num(s.total) + "}";
}

}  // namespace

std::span<const BenchPreset> bench_presets() { return kPresets; }

const BenchPreset* find_bench_preset(std::string_view name) {
    for (const BenchPreset& p : kPresets)
        if (p.name == name) return &p;
    return nullptr;
}

int parse_benchmark_arg(std::span<const std::string> args, size_t& i, BenchmarkOptions& o, std::string* err) {
    if (i >= args.size()) return 0;
    const std::string& a = args[i];
    if (a.rfind("--bench", 0) != 0) return 0;
    auto fail = [&](std::string why) {
        if (err) *err = a + ": " + why;
        return -1;
    };
    if (a == "--bench-no-verify") {
        o.verify = false;
        return 1;
    }
    static constexpr std::array<std::string_view, 13> kValued = {"--benchmark",      "--bench-save",        "--bench-preset",  "--bench-ticks",
                                                                 "--bench-frames-per-tick", "--bench-settle", "--bench-max-unsettled",
                                                                 "--bench-report",   "--bench-crc-out",     "--bench-png-dir", "--bench-png-every",
                                                                 "--bench-redraw-every", "--bench-trace"};
    if (std::find(kValued.begin(), kValued.end(), std::string_view(a)) == kValued.end()) return 0;
    if (i + 1 >= args.size()) return fail("needs a value");
    const std::string& v = args[++i];
    uint64_t n = 0;
    if (a == "--benchmark") {
        if (v == "realtime") o.mode = BenchMode::RealTime;
        else if (v == "fixed") o.mode = BenchMode::FixedFrames;
        else return fail("'" + v + "' is not realtime or fixed");
    } else if (a == "--bench-save") {
        o.save = v;
    } else if (a == "--bench-preset") {
        o.preset = v;
    } else if (a == "--bench-report") {
        o.report = v;
    } else if (a == "--bench-crc-out") {
        o.crc_out = v;
    } else if (a == "--bench-png-dir") {
        o.png_dir = v;
    } else {
        if (!parse_u64(v, n)) return fail("'" + v + "' is not a number");
        if (a == "--bench-ticks") o.ticks = n;
        else if (n > UINT32_MAX) return fail("too large");
        else if (a == "--bench-frames-per-tick") o.frames_per_tick = static_cast<uint32_t>(n);
        else if (a == "--bench-settle") o.settle_frames = static_cast<uint32_t>(n);
        else if (a == "--bench-max-unsettled") o.max_unsettled = static_cast<uint32_t>(n);
        else if (a == "--bench-trace") o.trace_every = static_cast<uint32_t>(n);
        else if (a == "--bench-redraw-every") o.redraw_every = static_cast<uint32_t>(n);
        else o.png_every = static_cast<uint32_t>(n);  // --bench-png-every
    }
    return 1;
}

std::string check_benchmark_options(const BenchmarkOptions& o) {
    if (!o.active()) return {};
    if (o.save.empty()) return "--benchmark needs --bench-save FILE (a saved game to start from)";
    if (!find_bench_preset(o.preset)) {
        std::string names;
        for (const BenchPreset& p : bench_presets()) names += (names.empty() ? "" : ", ") + std::string(p.name);
        return "unknown --bench-preset '" + o.preset + "' (presets: " + names + ")";
    }
    if (o.ticks == 0) return "--bench-ticks must be at least 1";
    if (o.frames_per_tick == 0 || o.frames_per_tick > 64) return "--bench-frames-per-tick must be 1..64";
    if (o.max_unsettled == 0) return "--bench-max-unsettled must be at least 1";
    if (o.png_every != 0 && o.png_dir.empty()) return "--bench-png-every needs --bench-png-dir DIR";
    if (o.redraw_every != 0 && !o.fixed()) return "--bench-redraw-every needs --benchmark fixed";
    if (o.redraw_every == 1) return "--bench-redraw-every must be 0 (off) or at least 2";
    return {};
}

FrameTimeStats frame_time_stats(std::span<const float> ms) {
    FrameTimeStats s;
    if (ms.empty()) return s;
    std::vector<float> v(ms.begin(), ms.end());
    std::sort(v.begin(), v.end());
    s.count = v.size();
    double total = 0.0;
    for (const float x : v) total += static_cast<double>(x);  // sorted order: the same sum for the same samples
    s.total = total;
    s.mean = total / static_cast<double>(v.size());
    const auto rank = [&](double p) {
        const double r = std::ceil(p / 100.0 * static_cast<double>(v.size()));
        const size_t k = r < 1.0 ? 0 : static_cast<size_t>(r) - 1;
        return static_cast<double>(v[std::min(k, v.size() - 1)]);
    };
    s.median = rank(50.0);
    s.p99 = rank(99.0);
    s.worst = static_cast<double>(v.back());
    return s;
}

void bench_hitches(std::span<const float> frame_ms, BenchReport& r) {
    r.early_ms = frame_time_stats(frame_ms.first(std::min(frame_ms.size(), kEarlyFrames)));
    r.hitch_threshold_ms = std::max(4.0 * r.frame_ms.median, 25.0);
    r.hitch_count = 0;
    r.hitches.clear();
    for (size_t k = 0; k < frame_ms.size(); ++k)
        if (static_cast<double>(frame_ms[k]) > r.hitch_threshold_ms) {
            ++r.hitch_count;
            r.hitches.emplace_back(k, frame_ms[k]);
        }
    std::stable_sort(r.hitches.begin(), r.hitches.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    if (r.hitches.size() > kMaxHitches) r.hitches.resize(kMaxHitches);
}

std::string bench_report_json(const BenchReport& r) {
    std::string o = "{\n  \"alamo_game_benchmark\": 1,\n";
    auto str = [&](const char* k, std::string_view v) {
        o += "  \"";
        o += k;
        o += "\": ";
        json_text::json_str(o, v);
        o += ",\n";
    };
    auto val = [&](const char* k, const std::string& v) {
        o += "  \"";
        o += k;
        o += "\": " + v + ",\n";
    };
    str("result", r.result);
    if (!r.error.empty()) str("error", r.error);
    str("mode", r.mode);
    str("preset", r.preset);
    str("save", r.save);
    str("device", r.device);
    val("width", std::to_string(r.width));
    val("height", std::to_string(r.height));
    str("present_mode", r.present_mode);
    val("frame_cap", std::to_string(r.frame_cap));
    val("taa", r.taa ? "true" : "false");
    val("frames_per_tick", std::to_string(r.frames_per_tick));
    val("target_ticks", std::to_string(r.target_ticks));
    val("start_tick", std::to_string(r.start_tick));
    val("end_tick", std::to_string(r.end_tick));
    val("ticks", std::to_string(r.ticks));
    val("frames", std::to_string(r.frames));
    val("warmup_frames", std::to_string(r.warmup_frames));
    val("unsettled_frames", std::to_string(r.unsettled_frames));
    val("excluded_frames", std::to_string(r.excluded_frames));
    val("forced_redraws", std::to_string(r.forced_redraws));
    val("taa_discarded", std::to_string(r.taa_discarded));
    val("wall_seconds", num(r.wall_seconds));
    val("frame_ms", stats_json(r.frame_ms));
    val("render_ms", stats_json(r.render_ms));
    val("sim_ms", stats_json(r.sim_ms));
    char crc[24];
    std::snprintf(crc, sizeof crc, "%016" PRIx64, r.final_crc);
    str("final_crc", crc);
    str("presentation_check", r.presentation_check);
    str("screen", r.screen);
    // bm2: the scenario, its fingerprint check and the GPU timer scopes.
    str("scenario", r.scenario);
    val("scenario_version", std::to_string(r.scenario_version));
    str("fingerprint", r.fingerprint);
    str("scenario_made", r.scenario_made);
    val("camera_keys", std::to_string(r.camera_keys));
    val("gpu_timestamps", r.gpu_timestamps ? "true" : "false");
    val("gpu_frames", std::to_string(r.gpu_frames));
    std::string gpu = "{";
    for (size_t k = 0; k < r.gpu_ms.size(); ++k) {
        if (k) gpu += ", ";
        json_text::json_str(gpu, r.gpu_ms[k].first);
        gpu += ": " + stats_json(r.gpu_ms[k].second);
    }
    val("gpu_ms", gpu + "}");
    // bm3 t04: load time and the first frames' hitches.
    val("load_ms", num(r.load_ms));
    val("early_ms", stats_json(r.early_ms));
    std::string hit = "{\"threshold_ms\": " + num(r.hitch_threshold_ms) + ", \"count\": " + std::to_string(r.hitch_count) + ", \"worst\": [";
    for (size_t k = 0; k < r.hitches.size(); ++k) {
        if (k) hit += ", ";
        hit += "[" + std::to_string(r.hitches[k].first) + ", " + num(static_cast<double>(r.hitches[k].second)) + "]";
    }
    val("hitches", hit + "]}");
    o += "  \"autopilot\": [";
    for (size_t k = 0; k < r.autopilot.size(); ++k) {
        if (k) o += ", ";
        json_text::json_str(o, r.autopilot[k]);
    }
    o += "]\n}\n";
    return o;
}

}  // namespace alamo::game
