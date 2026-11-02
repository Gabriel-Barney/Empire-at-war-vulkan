// alamo_game OPT1 t07: the in-game frame-time overlay (--frame-overlay; F11 toggles it). Top right: the last sim ticks'
// wall time from the tick profiler ring (engine/core profile.hpp) as bars against the budget lines (16.7 ms = the p99
// target, 33.3 ms = one tick at 30 ticks/s), the window's p50 / p99 / max, the worst tick's biggest zone; under it the
// host's own frames as stacked bars (sim step, render, GPU / swapchain wait, the rest of the frame).
//
// Presentation only (rule 3): the values are wall clock and go to the screen and the console, never to the sim, the
// command stream or the CRC. read_ticks() reads the profiler ring, so it runs while no tick is in flight.
#pragma once
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "alamo/core/profile.hpp"
#include "alamo/ui/hud_draw.hpp"

namespace alamo::ui {
class FontCache;
}

namespace alamo_game_app {

struct FrameTimes {  // one host frame, ms
    float frame = 0;    // frame start to the next frame start (includes the frame cap and the window pump)
    float sim = 0;      // step_sim: ticks + post-tick snapshot captures
    float prepare = 0;  // world view prepare (interpolation, instances, particles) wherever it ran
    float render = 0;   // render_frame
    float wait = 0;     // inside render_frame: frame-slot fence + swapchain acquire + present
};

class FrameOverlay {
public:
    static constexpr uint32_t kTicks = 120, kFrames = 120;

    bool shown() const { return shown_; }
    // On: starts the profiler when nothing else did (a small ring); off: stops it again unless `keep` (--profile).
    void set_shown(bool on, bool keep_profiler);
    void set_log(bool on) { log_ = on; }  // --profile: keep every frame for report()
    void add_frame(const FrameTimes& t);
    void read_ticks();  // the profiler's ticks closed since the last call (main thread, no tick in flight)
    void build(alamo::ui::HudDrawList& dl, const alamo::ui::HudCanvas& canvas, alamo::ui::FontCache& fonts) const;
    std::string report() const;  // --profile: per-phase frame ms over the logged frames

private:
    struct Tick {
        uint64_t frame = 0;
        uint32_t total_ns = 0, worst_ns = 0;
        alamo::profile::ZoneId worst = alamo::profile::kNoZone;
    };
    bool shown_ = false, started_profiler_ = false, log_ = false;
    uint64_t seen_ = 0;
    std::deque<Tick> ticks_;
    std::deque<FrameTimes> frames_;
    std::vector<FrameTimes> log_frames_;
};

}  // namespace alamo_game_app
