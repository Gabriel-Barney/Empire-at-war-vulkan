#include "alamo/sim/pacing.hpp"

namespace alamo::sim {

void FrameSynchronizer::reset() {
    tactical_index_ = galactic_index_ = kDefaultSpeedIndex;
    fast_ = cinematic_ = paused_ = step_request_ = false;
    have_last_ = false;
    last_ms_ = 0;
}

void FrameSynchronizer::set_tactical_speed(uint32_t index) {
    tactical_index_ = index < kSpeedTable.size() ? index : static_cast<uint32_t>(kSpeedTable.size() - 1);
}

void FrameSynchronizer::set_galactic_speed(uint32_t index) {
    galactic_index_ = index < kSpeedTable.size() ? index : static_cast<uint32_t>(kSpeedTable.size() - 1);
}

void FrameSynchronizer::set_paused(bool on) {
    paused_ = on;
    fast_ = false;  // every pause / unpause clears fast mode
}

uint32_t FrameSynchronizer::rate() const {
    // --- OI host: MAIN_LOOP.md §1.2 applies "fast -> 120" then "cinematic -> 30": the cinematic lock wins ---
    if (cinematic_) return kCinematicRate;
    if (fast_) return kFastModeRate;
    // --- end OI host ---
    return kSpeedTable[active_speed_index()];
}

bool FrameSynchronizer::pace(uint32_t now_ms) {
    if (stopped_) return false;
    if (have_last_ && now_ms - last_ms_ < period_ms()) return false;  // wrapping, as timeGetTime
    if (paused_) {
        if (!step_request_) return false;
        step_request_ = false;
    }
    have_last_ = true;
    last_ms_ = now_ms;
    return true;
}

}  // namespace alamo::sim
