// Galactic goal maintenance time test (A2f t02 R2). Private to engine/ai.
//
// AI_GALACTIC §1.6 Resolve_Category step 1 (maintenance 0x722a30), galactic only [H]: a replaceable goal is dropped when
// AllowedTime (0x692860: nominal time x Build_Time_Delay_Tolerance, Time_Limit without a plan; A2f t09) < ETA, with
// ETA = 0x525200 + elapsed, elapsed = (NOW - created) / FPS. The goal system's "0 < AllowedTime" guard (ai_goals.cpp)
// stays: AllowedTime <= 0 means no limit, as in the greedy phase.
// 0x525200 is read (AI_GALACTIC_2 §3.4 "Goal production time", q2f t01; A2F_RE_READS.md §1 [H]); the port is
// GalacticAiWorld::Impl::goal_production_time (galactic_build.cpp).
#pragma once
#include <cstdint>
#include <vector>

#include "alamo/ai/ai_types.hpp"
#include "alamo/galactic/economy.hpp"

namespace alamo::ai {

inline float galactic_maintenance_eta(float production_time, int64_t created, int64_t now) {
    // 0x722a30 [H]: p = 0x525200(); if (0 < p) p += (float)(NOW - created) / (float)FPS (int subtraction, float division).
    // With every build task producing (or none produced) p is 0 and the goal is never time-dropped (A2f t06).
    if (!(production_time > 0.0f)) return production_time;
    return production_time + static_cast<float>(now - created) / static_cast<float>(kAiFps);
}

// 0x4b0500 in galactic mode (PP +0x198 == 0) [H field, M argument = the plan player]: the player's difficulty
// adjustment's Galactic_Build_Time_Multiplier (+0x28); the all-1.0 adjustment when the player has none (index out of range,
// galactic::kNoIndex).
inline float galactic_build_time_multiplier(const std::vector<galactic::DifficultyRules>& difficulties, uint32_t difficulty) {
    return difficulty < difficulties.size() ? difficulties[difficulty].galactic_build_time_multiplier : 1.0f;
}

// 0x6c0450(PP) [H structure], the base of AllowedTime (A2F_RE_READS §1.4 item 2, A2f t09): S = the float sum, in selection
// order, of btime(T, none) over every selected type of the PP (PP +0x58 .. +0x60, free-store entries included), then
// S x the multiplier above (PP +0x28, the player, is set for every galactic PP).
inline float galactic_nominal_time(const std::vector<float>& selection_btime_none, float multiplier) {
    float s = 0.0f;
    for (const float t : selection_btime_none) s += t;
    return s * multiplier;
}

}  // namespace alamo::ai
