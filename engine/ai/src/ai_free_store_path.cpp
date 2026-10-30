#include "alamo/ai/ai_free_store_path.hpp"

#include <algorithm>

namespace alamo::ai {

galactic::PathRequest ai_path_request(PlanetIndex src, PlanetIndex dst, int m) {
    // 0x665160 (FIXPASS_GALACTIC §1.4) [H]: galactic::PathRequest::for_ai_mode carries the corrected table.
    return galactic::PathRequest::for_ai_mode(src, dst, m);
}

std::vector<PlanetIndex> ai_reach_flood(const galactic::PlanetGraph& graph, const galactic::PlayerView& view,
                                        std::span<const PlanetIndex> starts, int m, std::vector<uint8_t>* reached) {
    const uint32_t n = graph.size();
    std::vector<uint8_t> r(n, 0);
    std::vector<PlanetIndex> q, out;
    const galactic::PathRequest transit = ai_path_request(kInvalidPlanet, kInvalidPlanet, m);
    for (const PlanetIndex s : starts) {
        if (s >= n || r[s] || !graph.included(s)) continue;
        // Flag 4 clear: the flood refuses a start not held by the player or an ally (0x4ad9f0) [H].
        if (!transit.always_accept_destination && !galactic::PlayerView::bit(view.held_by_ally, s, false)) continue;
        r[s] = 1;
        q.push_back(s);
        out.push_back(s);
    }
    galactic::PathRequest as_dest = transit;
    for (size_t k = 0; k < q.size(); ++k) {
        const PlanetIndex from = q[k];
        for (uint32_t e = graph.edge_begin(from); e < graph.edge_end(from); ++e) {
            const PlanetIndex nb = graph.neighbour(e);
            if (nb >= n || r[nb]) continue;
            // Reached = Edge_Cost accepts nb as the request's destination (flag 4 short-circuit included); expanded =
            // accepted as a transit planet. Not reached through this edge (fog / battle / impassable / danger): another
            // edge may still reach it.
            as_dest.destination = nb;
            if (galactic::edge_cost(graph, view, as_dest, from, e) < 0.0f) continue;
            r[nb] = 1;
            out.push_back(nb);
            if (!as_dest.always_accept_destination || galactic::edge_cost(graph, view, transit, from, e) >= 0.0f)
                q.push_back(nb);
        }
    }
    if (reached) *reached = std::move(r);
    return out;
}

int32_t free_store_path_sum(std::span<const uint32_t> hop_weights, size_t path_size) {
    if (path_size == 0) return -1;
    const size_t count = std::min<size_t>({size_t{1000}, path_size - 1, hop_weights.size()});
    uint32_t s = 0;  // int32 wrapping add, done in uint32 (defined behaviour)
    for (size_t i = 0; i < count; ++i) s += hop_weights[i];
    return static_cast<int32_t>(s);
}

bool free_store_path_passes(int32_t sum, float threshold) {
    const uint32_t high = static_cast<uint32_t>(sum) >> 16;
    return static_cast<float>(high * 100u) <= threshold;
}

int32_t orbit_owner(bool has_starbase, int32_t planet_owner, std::span<const OrbitFleet> orbit) {
    if (has_starbase) return planet_owner;
    for (const OrbitFleet& f : orbit)
        if (!f.transient) return f.owner;
    return orbit.empty() ? -1 : orbit.front().owner;
}

FreeStoreHopResult free_store_next_hop(const AiPathFinder& paths, int32_t player, PlanetIndex current, PlanetIndex dest,
                                       bool paused, const std::function<int32_t(PlanetIndex)>& orbit_owner_of,
                                       float threshold) {
    FreeStoreHopResult res;
    if (paused) {  // step 1: either galactic pause counter > 0
        res.kind = FreeStoreHop::Wait;
        return res;
    }
    if (current == kInvalidPlanet) {  // step 2
        res.kind = FreeStoreHop::NotOnPlanet;
        return res;
    }
    if (current == dest) {
        res.kind = FreeStoreHop::Arrived;
        return res;
    }
    std::vector<PlanetIndex> path;  // step 3: the matrix of the last recompute
    std::vector<uint32_t> weights;
    paths.get_path(current, dest, path, &weights);
    res.sum = free_store_path_sum(weights, path.size());  // step 4
    if (!free_store_path_passes(res.sum, threshold) || path.size() < 2) {
        res.kind = FreeStoreHop::NoPath;  // step 5: no fallback
        return res;
    }
    const int32_t owner = orbit_owner_of ? orbit_owner_of(path[1]) : -1;  // step 6
    if (owner != -1 && owner != player) {
        res.kind = FreeStoreHop::OrbitBlocked;
        return res;
    }
    res.kind = FreeStoreHop::Issue;  // step 7
    res.hop = path[1];
    return res;
}

int32_t find_target_pick(std::span<const double> scores, double probability, const std::function<float(float)>& draw) {
    // w = 1/(float)p - 1; p outside (0, 1] (an error in the original) -> w = 0 [port].
    const float pf = static_cast<float>(probability);
    const float w = (pf > 0.0f && pf < 1.0f) ? 1.0f / pf - 1.0f : 0.0f;
    int32_t best = -1;
    double best_score = 0.0;
    std::vector<int32_t> idx;
    std::vector<float> cum;
    auto append = [&](int32_t i, float weight) {
        idx.push_back(i);
        cum.push_back((cum.empty() ? 0.0f : cum.back()) + weight);
    };
    for (size_t i = 0; i < scores.size(); ++i) {
        const double s = scores[i];
        if (s <= best_score) {
            if (static_cast<float>(s) > 0.0f) append(static_cast<int32_t>(i), static_cast<float>(s));
        } else {
            if (best >= 0 && static_cast<float>(best_score) > 0.0f) append(best, static_cast<float>(best_score));
            best = static_cast<int32_t>(i);
            best_score = s;
        }
    }
    if (idx.empty() || w == 0.0f) return best;
    const float last = cum.back() / w;
    if (last > 0.0f && best >= 0) append(best, last);
    const float total = cum.back();
    if (!(total > 0.0f) || !draw) return best;  // Weighted_Random_Pick 0x698de0: no draw when total <= 0
    const float r = draw(total);
    const auto it = std::lower_bound(cum.begin(), cum.end(), r);
    return it == cum.end() ? idx.back() : idx[static_cast<size_t>(it - cum.begin())];
}

}  // namespace alamo::ai
