// S5 frame pacing: the original FrameSynchronizer's single-player tick decision (docs/re/MAIN_LOOP.md
// §1.2, §1.5). Wall-clock gating only: it decides WHEN a tick runs, never what it computes (the sim
// step is always 1/30 s, sim time = frame / 30).
//
//   * rate = speed table[active index] (10, 20, 30, 45, 60 ticks/s; separate tactical and galactic
//     indices, default 2 = 30); single-player fast mode forces 120; the cinematic lock forces 30.
//   * a tick is allowed when now - last >= floor(1000 / rate) ms; then last = now (NOT += period:
//     drift is never paid back, there is no catch-up and at most one tick per loop iteration).
//   * pause gate: while paused no tick, unless a single-step request is pending (cleared by the tick).
//   * "simulation stopped" (empty mode stack / load in progress): no tick.
//   * every pause / unpause clears fast mode.
// Times are 32-bit wall milliseconds as from timeGetTime (wrapping subtraction).
#pragma once
#include <array>
#include <cstdint>

namespace alamo::sim {

inline constexpr std::array<uint32_t, 5> kSpeedTable = {10, 20, 30, 45, 60};  // ticks/s (0x85aa00)
inline constexpr uint32_t kDefaultSpeedIndex = 2;
inline constexpr uint32_t kFastModeRate = 120;
inline constexpr uint32_t kCinematicRate = 30;
inline constexpr uint32_t kSimTicksPerSecond = 30;  // sim step 1/30 s, independent of the pacing rate
inline constexpr uint32_t kDefaultFpsCap = 200;     // FPSCAP= default (render-rate cap, >= 15)

class FrameSynchronizer {
public:
    // FS reset (Start_Game on an empty stack): rates 30, speed indices 2, pause/fast/step clear.
    void reset();

    // Speed indices (0..4, clamped). The active index follows the top mode (tactical or galactic).
    void set_tactical_speed(uint32_t index);
    void set_galactic_speed(uint32_t index);
    void select_active(bool tactical_top) { tactical_active_ = tactical_top; }
    uint32_t active_speed_index() const { return tactical_active_ ? tactical_index_ : galactic_index_; }

    void set_fast(bool on) { fast_ = on; }
    void set_cinematic_lock(bool on) { cinematic_ = on; }
    void set_paused(bool on);
    void request_step() { step_request_ = true; }
    void set_stopped(bool on) { stopped_ = on; }

    bool fast() const { return fast_; }
    bool paused() const { return paused_; }
    bool stopped() const { return stopped_; }

    // Pacing rate in ticks per wall second (table / fast / cinematic).
    uint32_t rate() const;
    // Milliseconds between ticks: floor(1000 / rate).
    uint32_t period_ms() const { return 1000u / rate(); }

    // FrameSynchronizer::Pace: true = run one tick now (and last = now, step request consumed).
    bool pace(uint32_t now_ms);

private:
    uint32_t tactical_index_ = kDefaultSpeedIndex, galactic_index_ = kDefaultSpeedIndex;
    bool tactical_active_ = false;
    bool fast_ = false, cinematic_ = false, paused_ = false, step_request_ = false;
    bool stopped_ = true;
    bool have_last_ = false;
    uint32_t last_ms_ = 0;
};

}  // namespace alamo::sim
