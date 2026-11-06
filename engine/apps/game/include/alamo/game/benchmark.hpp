// BM2a (side goal "repeatable benchmark"): alamo_game's benchmark mode, the render-free half (used by the exe and its
// tests). The exe side (app/app_benchmark.cpp) loads a save, runs the session without the player and writes the report.
//
//   ENTRY POINT  BenchmarkOptions: everything a run needs, filled from the command line (parse_benchmark_arg) today and
//                from a BM1 scenario file later (group bm2). The exe reads nothing else for a benchmark run.
//   MODES        RealTime: the normal frame loop at the original pacing (FrameSynchronizer, 30 ticks/s, wall clock),
//                normal rendering; the report gives frame-time median / p99 / worst.
//                FixedFrames: exactly frames_per_tick counted frames per sim tick, the presentation clock driven by the
//                counted frames (frames / (30 x frames_per_tick) s), V-sync off (Immediate) and no frame cap, models loaded
//                blocking; a frame counts only when the presentation is settled (no pending pipeline, model, texture or
//                terrain work), an unsettled frame is drawn again with the same sim state and sub-frame, so every run
//                draws the same counted frames. TAA keeps the preset's setting (BM2b): every measured frame is drawn
//                TAA-provisional and a frame that does not count is taken back out of the TAA history and jitter cycle
//                (Renderer::discard_taa_frame); the history restarts with the measurement.
//   PRESETS      BenchPreset: the window size and the video / audio settings of a run, by name (bench_presets). Nothing
//                is read from the player's video options file.
//   STATS        frame_time_stats: count, mean, median, p99 and worst of the counted frames' times (nearest-rank
//                percentiles on the sorted samples).
//   REPORT       bench_report_json: one JSON object (fixed key order, "alamo_game_benchmark": 1).
//   SCENARIO     (bm2) --bench-scenario FILE fills these options from a BM1 / BM2 scenario file instead of the flags
//                (benchmark_scenario.hpp); --bench-make FILE runs from --bench-save and writes a scenario at the end.
#pragma once
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "alamo/sim/bench_scenario.hpp"

namespace alamo::game {

enum class BenchMode : uint8_t { Off, RealTime, FixedFrames };
const char* bench_mode_name(BenchMode m);  // "off" / "realtime" / "fixed"

enum class BenchAudio : uint8_t { Null, Off };  // never the device: no output timing in a benchmark

// A named benchmark preset. Detail levels are the Video dialog's 0..100 values (ui::kQualityPresets: 0, 40, 75, 100 =
// Low / Medium / High / Ultra; all seven advanced detail options take that value); anti_alias is the TAA strength
// (0 = off; ui::kAntiAliasChoices), gamma 50 = identity, anisotropy one of ui::kAnisotropyChoices.
struct BenchPreset {
    std::string_view name;
    std::string_view description;
    uint32_t width = 1280, height = 720;
    int32_t detail = 75, anti_alias = 0, gamma = 50, anisotropy = 0;
    bool soft_shadows = true, heat_distortion = true, bloom = true, ambient_occlusion = false;
    bool vsync = false;     // real-time mode only (fixed-frames mode is always V-sync off)
    int32_t frame_cap = 0;  // real-time mode only, fps (0 = none)
    BenchAudio audio = BenchAudio::Null;
};
inline constexpr std::string_view kDefaultBenchPreset = "bm-high";
std::span<const BenchPreset> bench_presets();
const BenchPreset* find_bench_preset(std::string_view name);  // case-sensitive; null when unknown

struct BenchmarkOptions {
    BenchMode mode = BenchMode::Off;
    std::string save;                                   // --bench-save FILE: the .alsv the run starts from (required)
    // bm2 (benchmark_scenario.hpp): --bench-scenario FILE (run a scenario: save, orders, fingerprint, settings, camera) or
    // --bench-make FILE (make = true: run from --bench-save and write the scenario; FILE may be a recipe). The other
    // flags override what the file says.
    std::string scenario;
    bool make = false;
    std::shared_ptr<const sim::bench::Scenario> scenario_data;  // the loaded manifest / recipe (null without one)
    std::string preset = std::string(kDefaultBenchPreset);  // --bench-preset NAME
    uint64_t ticks = 900;                               // --bench-ticks N: measured sim ticks (900 = 30 s of game time)
    uint32_t frames_per_tick = 2;                       // --bench-frames-per-tick K (fixed-frames mode, 1..64)
    uint32_t settle_frames = 3;                         // --bench-settle N: settled frames before the measurement starts
    uint32_t max_unsettled = 600;                       // --bench-max-unsettled N: consecutive unsettled frames -> fail
    std::string report;                                 // --bench-report FILE: the JSON report ("" = stdout only)
    std::string crc_out;                                // --bench-crc-out FILE: the run's per-tick CRC log (CrcLog::save)
    std::string png_dir;                                // --bench-png-dir DIR: PNG of every Nth counted frame
    uint32_t png_every = 0;                             // --bench-png-every N (0 = none)
    uint32_t trace_every = 0;                           // --bench-trace N (BM3): print the live objects per owner every N measured ticks (scene authoring)
    bool verify = true;                                 // --bench-no-verify: skip the presentation on/off replay check
    uint32_t redraw_every = 0;                          // --bench-redraw-every N (tests, fixed frames): every Nth settled
                                                        // measured frame is treated as unsettled and drawn again (0 = off)
    bool active() const { return mode != BenchMode::Off; }
    bool fixed() const { return mode == BenchMode::FixedFrames; }
};

// Parses the benchmark flag at args[i] (advancing i past its value). 1 = consumed, 0 = not a benchmark flag, -1 = bad
// value (*err says why). Flags: --benchmark realtime|fixed, --bench-save, --bench-preset, --bench-ticks,
// --bench-frames-per-tick, --bench-settle, --bench-max-unsettled, --bench-report, --bench-crc-out, --bench-png-dir,
// --bench-png-every, --bench-no-verify, --bench-redraw-every.
int parse_benchmark_arg(std::span<const std::string> args, size_t& i, BenchmarkOptions& o, std::string* err = nullptr);
// After parsing: "" when the options can run, else why not (no save, unknown preset, a value out of range).
std::string check_benchmark_options(const BenchmarkOptions& o);

// Fixed-frames schedule: K counted frames per tick. Starts due, so the first counted frame follows a tick.
//   per frame:  if (tick_due()) { run one tick; ticked(); }   draw with alpha();   if the frame counts: frame_counted()
class FixedFrameClock {
public:
    explicit FixedFrameClock(uint32_t frames_per_tick = 1) : k_(frames_per_tick ? frames_per_tick : 1), sub_(k_) {}
    uint32_t frames_per_tick() const { return k_; }
    bool tick_due() const { return sub_ >= k_; }
    void ticked() {
        sub_ = 0;
        ++ticks_;
    }
    void frame_counted() {
        if (sub_ < k_) ++sub_;
        ++frames_;
    }
    // Interpolation alpha of the frame about to be drawn: (sub + 1) / K, so the last frame of each tick draws the tick.
    float alpha() const { return static_cast<float>(sub_ < k_ ? sub_ + 1 : k_) / static_cast<float>(k_); }
    // Presentation seconds at the frame about to be drawn: counted frames / (30 x K) (the sim runs 30 ticks per second).
    double seconds() const { return static_cast<double>(frames_) / (30.0 * static_cast<double>(k_)); }
    uint64_t frames() const { return frames_; }
    uint64_t ticks() const { return ticks_; }
    // All `target` ticks ran and the last one's K frames were drawn.
    bool finished(uint64_t target) const { return ticks_ >= target && sub_ >= k_; }

private:
    uint32_t k_;
    uint32_t sub_;
    uint64_t frames_ = 0, ticks_ = 0;
};

struct FrameTimeStats {
    uint64_t count = 0;
    double mean = 0, median = 0, p99 = 0, worst = 0, total = 0;  // ms
};
// Nearest-rank percentiles: p-th = sorted[ceil(p / 100 x n) - 1]; median = the 50th. Empty input: all zero.
FrameTimeStats frame_time_stats(std::span<const float> ms);

struct BenchReport {
    std::string mode, preset, save, result = "ok", error;  // result "ok" / "failed" (error says why)
    std::string present_mode;                              // the swapchain's mode in use ("immediate" / "mailbox" / "fifo")
    std::string screen;                                    // where the run ended ("galactic" / "battle" / ...)
    std::string device;                                    // the Vulkan device name
    uint32_t width = 0, height = 0, frames_per_tick = 0;
    int32_t frame_cap = 0;
    bool taa = false;
    uint64_t start_tick = 0, end_tick = 0, ticks = 0, target_ticks = 0;
    uint64_t frames = 0, unsettled_frames = 0, excluded_frames = 0, warmup_frames = 0;
    uint64_t forced_redraws = 0;  // unsettled_frames forced by --bench-redraw-every
    uint64_t taa_discarded = 0;   // measured frames taken back out of the TAA history (fixed frames with TAA)
    double wall_seconds = 0;
    FrameTimeStats frame_ms, render_ms, sim_ms;  // sim: the counted frames that ran a tick
    uint64_t final_crc = 0;
    std::string presentation_check = "skipped";  // "match" / "diverged" / "skipped"
    std::vector<std::string> autopilot;          // what the run answered for the player (battle prompts, dialogs)
    // bm2: the scenario ("" = a plain --bench-save run), its version, the fingerprint check ("match", "skipped" or why it
    // diverged: then the run is not comparable), the scenario written by --bench-make, the camera keys used.
    std::string scenario, fingerprint = "skipped", scenario_made;
    uint32_t scenario_version = 0;
    uint64_t camera_keys = 0;
    // bm2 t06: GPU timer scopes over the counted frames ("frame" = the whole command buffer, then the renderer's
    // "world" / "scene" / "ao" / "post" / "taa" in first-seen order), in GPU milliseconds. gpu_timestamps false: the
    // queue has no timestamps (no scopes).
    bool gpu_timestamps = false;
    uint64_t gpu_frames = 0;
    std::vector<std::pair<std::string, FrameTimeStats>> gpu_ms;
    // bm3 t04: load and first-use cost. load_ms = from the save's load start to the first measured tick (the load screen, the
    // warm-up frames and every pipeline / model / texture the first view waits for); early_ms = the first kEarlyFrames counted
    // frames; hitches = counted frames over hitch_threshold_ms (4 x the median, at least 25 ms): their count and the worst
    // kMaxHitches as {frame index, ms}, worst first.
    double load_ms = 0;
    FrameTimeStats early_ms;
    double hitch_threshold_ms = 0;
    uint64_t hitch_count = 0;
    std::vector<std::pair<uint64_t, float>> hitches;
};
inline constexpr size_t kEarlyFrames = 300, kMaxHitches = 8;
// Fills early_ms, hitch_threshold_ms, hitch_count and hitches from the counted frames' times (frame_ms.median must be set).
void bench_hitches(std::span<const float> frame_ms, BenchReport& r);
std::string bench_report_json(const BenchReport& r);

}  // namespace alamo::game
