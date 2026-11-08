// Galactic AI free-store movement and reachability helpers (docs/re/FIXPASS_GALACTIC.md §1).
//
//   ai_path_request    : the AI request builder 0x665160(level m) with the corrected flag table (§1.4):
//                        flag0 = flag1 = (m == 5), flag2 = (m == 1 || m == 5), flag3 = (m > 2), flag4 = (m > 1),
//                        flag5 (fog) left at its default 1 (= galactic::PathRequest::for_ai_mode). GALACTIC.md §3's
//                        older "flag2 = flag0 = (m == 5), flag4 = (m == 1 or 5)" is superseded.
//   ai_reach_flood     : the reachability flood 0x3f12e0 from a set of start planets under those flags: a planet is
//                        reached when Edge_Cost (galactic::edge_cost) accepts it as the request destination, and expanded
//                        when it accepts it as a transit planet. With flag 4 clear the two are the same test and a start
//                        planet must be held by the player or an ally (0x4ad9f0), so a Friendly_Only target must be
//                        held by the player or an ally (§1.4).
//   free_store_path_*  : the Friendly_Only path test of FreeStoreMovementBlockStatus::Get_Next_Hop (0x71f710):
//                        S = wrapping int32 sum of the hop weights [0, min(1000, n - 1)) (0x691250; empty path -1),
//                        pass when float(((uint32)S >> 16) x 100) <= threshold (0x6913a0; mode 0 = 0.0f), i.e. every
//                        planet after the start, destination included, has node cost 0.
//   orbit_owner        : 0x3f9270 (§1.3 step 6).
//   free_store_next_hop: one Get_Next_Hop on the AI all-pairs matrix (AiPathFinder, last 90-frame recompute).
//   find_target_pick   : the FindTarget / Reachable_Target selection (AI_TACTICAL §6.1 step 4-5): at most one synced
//                        draw, only with >= 2 positive candidates and p < 1.
// Nothing here iterates a pointer-keyed container; all outputs are pure functions of the inputs.
#pragma once
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include "alamo/ai/ai_path_finder.hpp"
#include "alamo/core/galaxy.hpp"
#include "alamo/galactic/fleet_path.hpp"
#include "alamo/galactic/planet_graph.hpp"

namespace alamo::ai {

// 0x665160 (§1.4), AI level m in 0..5.
galactic::PathRequest ai_path_request(PlanetIndex src, PlanetIndex dst, int m);

// The flood 0x3f12e0 from `starts` (in order) for the mover of `view` at AI level m. Returns the reached planets in
// flood output order (starts first); reached[p] != 0 for each (sized to the graph). Starts that are not graph nodes are
// skipped, and with flag 4 clear so are starts not held by the mover or an ally (view.held_by_ally).
std::vector<PlanetIndex> ai_reach_flood(const galactic::PlanetGraph& graph, const galactic::PlayerView& view,
                                        std::span<const PlanetIndex> starts, int m,
                                        std::vector<uint8_t>* reached = nullptr);

// A2k t02 (A2K_T02_RE_ANSWERS Q1(b), A2K_T08_RE_ANSWERS Q3.3) [H]: the per-target source list of the target's
// PlanetReachabilityClass (0x665160 -> flood 0x3f12e0 with destination = `target`) at AI level m in 0..5. Returns the
// planets held by the mover or an ally (view.held_by_ally, 0x4ad9f0) in breadth-first pop order:
//   * flag 4 clear (m <= 1) and the target not held -> empty;
//   * target held (and, with flag 2 clear, passing the danger test) -> one BFS from the target;
//   * otherwise one BFS from each neighbour of the target in edge order (ascending planet index) that is not yet
//     visited and passes, with the visited marks shared; a neighbour that fails is marked visited. The run with
//     strictly the most held planets is kept (the first wins ties). The target itself is not marked first.
// BFS: the start is marked visited; each popped planet that is held is emitted; its unvisited neighbours (edge order)
// are marked visited and queued when they pass. Pass (0x3f2400): an AI mover (has_ai_system, not human) needs the
// planet revealed; then passable for the player (0x3f2510) -> flag 2, or corrupted with Benefits_From_Corruption, or
// not dangerous; impassable -> flag 3 and (not dangerous or corrupted with the benefit). [M] the level-4 "keep all"
// flag of the flood is not modelled (the stage at level 4 is the target itself and never reads the list).
// Node order (0x3f1c10): the GOM planet list = FindPlanet.Get_All_Planets order. `rank[p]` = p's position in that
// order (sized to the graph); adjacency is walked in ascending rank. Empty rank = ascending planet index.
std::vector<PlanetIndex> ai_stage_flood(const galactic::PlanetGraph& graph, const galactic::PlayerView& view,
                                        PlanetIndex target, int m, std::span<const uint32_t> rank = {});

// 0x691250: wrapping int32 sum of hop_weights[0 .. min(1000, path_size - 1)); -1 for an empty path.
int32_t free_store_path_sum(std::span<const uint32_t> hop_weights, size_t path_size);
// 0x6913a0 with the move's threshold (field +0x60 read as a float; mode 0 -> 0.0f).
bool free_store_path_passes(int32_t sum, float threshold = 0.0f);

// a2l t03: PlanetReachabilityClass slot 7 (0x663810, A2L_T03_RE_ANSWERS Q1.2) [H] on an AI all-pairs path from the
// source planet to the target, `path_size` planets long with `hop_weights`, at AI level `level`. The path is accepted by
// 0x6913a0 when float(((uint32)S >> 16) x 100) <= the level's threshold, S = the wrapping int32 sum of the hop weights
// [0, min(1000, path_size - 1)) (empty path: -1), or [0, path_size - 2) at level 4 (the final hop is not counted):
//   levels 0, 2, 3, 4, 6: 1e-4; levels 1, 5, 10: 1e18 (always accepted, even without a path);
//   levels 7, 8, 9: GameConstants 0x140b16d7c / d80 / d84 [port: not loaded; 1e18, L: no galactic caller passes them].
// Accepted: (low 16 bits of S) / 10; rejected: kReachNone. The source == target case (0) is the caller's.
inline constexpr double kReachNone = 1e18;
double ai_reach_path_cost(std::span<const uint32_t> hop_weights, size_t path_size, int level);

// 0x3f9270: with a starbase, the planet's owner; else the owner of the first orbiting fleet (arrival order) whose
// transient test 0x3a9760 is false, or the first one's owner when all pass it; kNoPlayer (-1) when no fleet orbits.
// `orbit` = (owner, transient) per orbiting fleet with fleet state < 3, in orbit order.
struct OrbitFleet {
    int32_t owner = -1;
    bool transient = false;
};
int32_t orbit_owner(bool has_starbase, int32_t planet_owner, std::span<const OrbitFleet> orbit);

enum class FreeStoreHop : uint8_t {
    Issue,         // move the fleet to `hop` (executed directly); the move continues on arrival
    Wait,          // galactic pause: retry on the next signal
    Arrived,       // already at the destination: finished
    NotOnPlanet,   // the fleet's parent is not a planet: finished, logged
    NoPath,        // Get_Path empty or the Friendly_Only test failed: finished, logged, no fallback
    OrbitBlocked,  // path[1]'s orbit owner is neither null nor this player (allies count as other): finished silently
};
struct FreeStoreHopResult {
    FreeStoreHop kind = FreeStoreHop::NoPath;
    PlanetIndex hop = kInvalidPlanet;
    int32_t sum = -1;  // the 0x691250 sum (diagnostics)
};
// One Get_Next_Hop (0x71f710) for `player`'s fleet at `current` (kInvalidPlanet: not on a planet) toward `dest`.
// `orbit_owner_of(p)` answers 0x3f9270 for path[1].
FreeStoreHopResult free_store_next_hop(const AiPathFinder& paths, int32_t player, PlanetIndex current, PlanetIndex dest,
                                       bool paused, const std::function<int32_t(PlanetIndex)>& orbit_owner_of,
                                       float threshold = 0.0f);

// FindTarget / Reachable_Target selection (AI_TACTICAL §6.1 steps 4-5, FIXPASS_GALACTIC §1.5). `scores` holds the
// evaluated candidates in target-list order (failed evaluations / range rejects already removed). w = 1/(float)p - 1;
// the best keeps the earliest of equal scores; others with (float)score > 0 join the list. With an empty list or w == 0
// the best is returned (-1 when none is > 0) without a draw; else the best joins with weight lastCum / w and ONE draw
// r = draw(total) picks the first entry whose float cumulative >= r. Returns an index into `scores`, or -1.
int32_t find_target_pick(std::span<const double> scores, double probability, const std::function<float(float)>& draw);

}  // namespace alamo::ai
