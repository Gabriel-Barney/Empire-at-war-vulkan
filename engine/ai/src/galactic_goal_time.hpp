// Galactic goal maintenance time test (A2f t02 R2). Private to engine/ai.
//
// AI_GALACTIC §1.6 Resolve_Category step 1 (maintenance 0x722a30), galactic only [H]: a replaceable goal is dropped when
// AllowedTime (0x692860: plan ETA x Build_Time_Delay_Tolerance, Time_Limit without a plan) < ETA, with
// ETA = 0x525200 + elapsed, elapsed = (NOW - created) / FPS. The goal system's "0 < AllowedTime" guard (ai_goals.cpp)
// stays: AllowedTime <= 0 means no limit, as in the greedy phase.
// 0x525200 is read (AI_GALACTIC_2 §3.4 "Goal production time", q2f t01 [H]); the port is
// GalacticAiWorld::Impl::goal_production_time (galactic_build.cpp).
#pragma once
#include <cstdint>

#include "alamo/ai/ai_types.hpp"

namespace alamo::ai {

inline float galactic_maintenance_eta(float production_time, int64_t created, int64_t now) {
    const double elapsed = static_cast<double>(now - created) / static_cast<double>(kAiFps);
    return production_time + static_cast<float>(elapsed);
}

}  // namespace alamo::ai
