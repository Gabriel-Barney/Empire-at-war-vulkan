// Galactic goal maintenance time test (A2f t02 R2). Private to engine/ai.
//
// AI_GALACTIC §1.6 Resolve_Category step 1 (maintenance 0x722a30), galactic only [H]: a replaceable goal is dropped when
// AllowedTime (0x692860: plan ETA x Build_Time_Delay_Tolerance, Time_Limit without a plan) < ETA, with
// ETA = 0x525200 + elapsed, elapsed = (NOW - created) / FPS. The goal system's "0 < AllowedTime" guard (ai_goals.cpp)
// stays: AllowedTime <= 0 means no limit, as in the greedy phase.
// [M] 0x525200 is not recovered. docs/re/ghidra places it among the AIExecutionSystem / ReferenceListClass<AIBuildTask>
// methods (0x524ce0..0x5250c0) and lists its callees as the planet build queue (0x559b40), its queue time (0x53bb20), a
// behaviour test (0x370320, the queue index) and the build time (0x371230): the time still needed by the goal's build
// tasks. The port's stand-in is GalacticAiWorld::Impl::goal_production_time (galactic_build.cpp). With it an on-schedule
// plan stays near its ETA (remaining + elapsed), a delayed or stalled one grows past ETA x tolerance and is dropped,
// and a plan whose production is over is late once elapsed > ETA x tolerance.
// Replaces the former stand-in "ETA only" (a plan was never late, so a stalled plan lived until another goal displaced
// it). The A2f diagnosis' "PP ETA + elapsed" was not taken: it drops every on-schedule build after (tolerance - 1) x ETA.
#pragma once
#include <cstdint>

#include "alamo/ai/ai_types.hpp"

namespace alamo::ai {

inline float galactic_maintenance_eta(float production_time, int64_t created, int64_t now) {
    const double elapsed = static_cast<double>(now - created) / static_cast<double>(kAiFps);
    return production_time + static_cast<float>(elapsed);
}

}  // namespace alamo::ai
