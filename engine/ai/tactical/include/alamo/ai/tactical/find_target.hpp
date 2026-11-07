// A1 - FindTarget / FindTarget.Reachable_Target selection (docs/re/AI_TACTICAL.md 6.1, 6.2, 10).
//
// Scoring (perception evaluation with Self / Enemy / Human / Target bound) is stage 2; this file is
// the pure part: candidate filtering in list order and the best-or-weighted pick.
//
// select_target [H] - in candidate (= target list) order, best = none, bestScore = 0.0 (double),
// a list of (target, cumulative float weight):
//   * score <= bestScore: append the target with weight (float)score if that is > 0;
//   * else: append the old best with weight (float)bestScore if that is > 0, the target becomes best
//     (ties keep the earlier target as best).
// If the list is empty or w == 0 -> best (none if no candidate scored > 0). Otherwise best is
// appended with weight lastCum / w (if > 0; this makes P(best) = p) and ONE synced draw
// r = Get_Float(0, total) (0x532480) picks the first entry whose cumulative >= r (binary search,
// Weighted_Random_Pick 0x698de0). No draw when total <= 0. So at most one draw per call, and only
// with >= 2 positive candidates and p < 1 (section 10).
//
// w = 1 / (float)p - 1 (float); p must be in (0, 1]. FindTarget with only 3 arguments: w = 0 and
// (port) max range 0 = no limit (the original reads an uninitialised value) [H].
//
// Candidate filters, in order (A1_READINGS §5) [H]: Applies(flags) -> (Reachable_Target only, with a
// source) reachability of targets that have a reachability object (land regions): cost == 1e18
// skipped -> evaluation (failed evaluations skipped) -> range (maxRange > 0 and squared 3-D distance
// target -> origin > maxRange^2). A filtered target is never evaluated, so it makes no '#' draws.
// The live filter loop is in TacticalAiWorld (tactical_ai.cpp, FindTarget).
// Return kinds: FindTarget -> GameObjectWrapper for a unit target, AITargetLocationWrapper for a
// region; Reachable_Target -> always AITargetLocationWrapper (or nil).
#pragma once
#include <cstdint>
#include <span>
#include <vector>

#include "alamo/ai/tactical/target_list.hpp"
#include "alamo/core/sim_rng.hpp"

namespace alamo::ai::tactical {

struct ScoredTarget {
    uint32_t target = 0;  // index into the target list
    double score = 0.0;
};

struct TargetPick {
    int32_t target = -1;  // index into the target list, -1 = nil
    bool drew = false;    // one synced draw was consumed
};

// True when p is a legal probability for FindTarget (0 < p <= 1).
bool valid_pick_probability(float p);
// w = 1 / (float)p - 1.
float pick_weight(float p);

TargetPick select_target(std::span<const ScoredTarget> candidates_in_list_order, float w, SimRng& rng);

enum class FindTargetReturn : uint8_t { Nil = 0, GameObject = 1, TargetLocation = 2 };
FindTargetReturn find_target_return_kind(const TacticalTarget* picked);
FindTargetReturn reachable_target_return_kind(const TacticalTarget* picked);

}  // namespace alamo::ai::tactical
