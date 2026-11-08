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

std::vector<PlanetIndex> ai_stage_flood(const galactic::PlanetGraph& graph, const galactic::PlayerView& view,
                                        PlanetIndex target, int m, std::span<const uint32_t> rank) {
    std::vector<PlanetIndex> best;
    const uint32_t n = graph.size();
    if (target >= n || !graph.included(target)) return best;
    // Adjacency in node order: ascending rank (ties and no rank: ascending planet index = graph edge order).
    std::vector<std::vector<PlanetIndex>> adj(n);
    for (PlanetIndex p = 0; p < n; ++p) {
        for (uint32_t e = graph.edge_begin(p); e < graph.edge_end(p); ++e)
            if (graph.neighbour(e) < n) adj[p].push_back(graph.neighbour(e));
        if (rank.size() >= n)
            std::stable_sort(adj[p].begin(), adj[p].end(), [&](PlanetIndex a, PlanetIndex b) { return rank[a] < rank[b]; });
    }
    const galactic::PathRequest req = ai_path_request(kInvalidPlanet, target, m);
    auto held = [&](PlanetIndex p) { return galactic::PlayerView::bit(view.held_by_ally, p, false); };
    auto benefit = [&](PlanetIndex p) { return galactic::PlayerView::bit(view.corrupted, p, false) && view.benefits_from_corruption; };
    auto passes = [&](PlanetIndex p) {  // 0x3f2400
        if (req.fog_rule && view.has_ai_system && !view.is_human && !view.is_planet_revealed(p)) return false;
        if (view.is_passable(p, req)) return req.skip_danger_check || benefit(p) || !view.is_dangerous(p);
        return req.allow_impassable && (!view.is_dangerous(p) || benefit(p));
    };
    if (!req.always_accept_destination && !held(target)) return best;
    std::vector<uint8_t> visited(n, 0);
    std::vector<PlanetIndex> q;
    auto bfs = [&](PlanetIndex start, std::vector<PlanetIndex>& out) {  // 0x3f0ce0
        q.clear();
        visited[start] = 1;
        q.push_back(start);
        for (size_t k = 0; k < q.size(); ++k) {
            const PlanetIndex at = q[k];
            if (held(at)) out.push_back(at);
            for (const PlanetIndex nb : adj[at]) {
                if (visited[nb]) continue;
                visited[nb] = 1;
                if (passes(nb)) q.push_back(nb);
            }
        }
    };
    if (held(target) && (req.skip_danger_check || benefit(target) || !view.is_dangerous(target))) {
        bfs(target, best);
        return best;
    }
    // Level 4 "keep all" (0x665160 -> 0x3f12e0 parameter 6 -> BFS parameter 7, A2L_T03_RE_ANSWERS Q3.1) [H]: every
    // neighbour's BFS appends into the one output list (the visited marks are shared, so each planet once): the union in
    // neighbour order instead of the largest run.
    const bool keep_all = m == 4;
    std::vector<PlanetIndex> run;
    for (const PlanetIndex nb : adj[target]) {
        if (visited[nb]) continue;
        if (!passes(nb)) {
            visited[nb] = 1;
            continue;
        }
        if (keep_all) {
            bfs(nb, best);
            continue;
        }
        run.clear();
        bfs(nb, run);
        if (run.size() > best.size()) best.swap(run);
    }
    return best;
}

double ai_path_length(std::span<const uint32_t> hop_weights, size_t path_size) {
    return static_cast<double>(static_cast<uint32_t>(free_store_path_sum(hop_weights, path_size)) & 0xFFFFu) / 10.0;
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

double ai_reach_path_cost(std::span<const uint32_t> hop_weights, size_t path_size, int level) {
    int32_t sum = -1;
    if (path_size > 0) {
        const size_t end = level == 4 ? (path_size >= 2 ? path_size - 2 : 0) : path_size - 1;
        const size_t count = std::min<size_t>({size_t{1000}, end, hop_weights.size()});
        uint32_t s = 0;  // int32 wrapping add, done in uint32
        for (size_t i = 0; i < count; ++i) s += hop_weights[i];
        sum = static_cast<int32_t>(s);
    }
    const bool strict = level == 0 || level == 2 || level == 3 || level == 4 || level == 6;
    if (!free_store_path_passes(sum, strict ? 1e-4f : 1e18f)) return kReachNone;
    return static_cast<double>(static_cast<uint32_t>(sum) & 0xFFFFu) / 10.0;
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
