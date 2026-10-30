#include "alamo/galactic/planet_graph.hpp"

#include <cmath>
#include <cstring>
#include <utility>

#include "alamo/core/hash.hpp"

namespace alamo::galactic {

float planet_distance(const Vec3& a, const Vec3& b) {
    // b - a vs a - b differ only in sign, so squares (and the result) are bitwise symmetric.
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    const float dz = b.z - a.z;
    const float sq = dx * dx + dy * dy + dz * dz;  // left-to-right, no contraction
    return std::sqrt(sq);                           // sqrtf (Q2)
}

namespace {

struct RowEdge {
    PlanetIndex nbr;
    float dist;
    uint32_t route;
    float speed;
};

template <class T>
uint64_t hash_vec(const std::vector<T>& v, uint64_t seed) {
    return v.empty() ? seed : fnv1a64(v.data(), v.size() * sizeof(T), seed);
}

// Non-positive / non-finite HS_Speed_Factor (GalaxyMap::validate flags it) is treated as 1.0.
// The original would divide by it as-is; such data is rejected by validation.
float sanitize_speed(float s) { return (s > 0.0f && std::isfinite(s)) ? s : 1.0f; }

}  // namespace

PlanetGraph PlanetGraph::build(const GalaxyMap& map, const PlanetGraphOptions& options,
                               const ParallelFor& pf) {
    PlanetGraph g;
    const uint32_t n = static_cast<uint32_t>(map.planets.size());
    g.n_ = n;
    g.max_move_ = map.max_fleet_move_distance;  // Q2: read once per build
    g.max_speed_factor_ = options.max_speed_factor_floor;
    g.position_ = map.planets.position;
    g.position_.resize(n);

    // Q1 node set: PLANET behaviour slot && owner != -1. In_Background is not consulted.
    g.included_.assign(n, 1);
    const bool owned_mask = options.owned && !options.owned->empty();
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t fl = i < map.planets.flags.size() ? map.planets.flags[i] : 0u;
        bool node = true;
        if (options.require_planet_behavior && !(fl & kPlanetHasPlanetBehavior)) node = false;
        if (options.exclude_background && (fl & kPlanetInBackground)) node = false;
        if (owned_mask && (i >= options.owned->size() || !options.owned->test(i))) node = false;
        g.included_[i] = node ? 1 : 0;
        g.node_count_ += node ? 1u : 0u;
    }

    // Route links per planet, in campaign <Trade_Routes> order. Like Create_Trade_Route_Link, a
    // second route between an already linked pair is skipped, so the first listed route wins.
    std::vector<std::vector<std::pair<PlanetIndex, uint32_t>>> links(n);
    auto find_link = [&](PlanetIndex a, PlanetIndex b) -> uint32_t {
        for (const auto& [nb, r] : links[a])
            if (nb == b) return r;
        return kNoRoute;
    };
    for (uint32_t r = 0; r < map.routes.size(); ++r) {
        const PlanetIndex a = map.routes.point_a[r], b = map.routes.point_b[r];
        if (a >= n || b >= n || a == b || !g.included_[a] || !g.included_[b]) continue;
        if (find_link(a, b) != kNoRoute) continue;
        links[a].emplace_back(b, r);
        links[b].emplace_back(a, r);
        // Q3: tracker replaced only when a found route's HS is strictly greater; never lowered.
        const float s = sanitize_speed(map.routes.hs_speed_factor[r]);
        if (s > g.max_speed_factor_) g.max_speed_factor_ = s;
    }

    // Per-planet rows (independent => parallel), neighbours ascending by index (Q4 edge order).
    std::vector<std::vector<RowEdge>> rows(n);
    const float max_move = g.max_move_;
    pf(n, 16, [&](uint32_t begin, uint32_t end) {
        for (uint32_t i = begin; i < end; ++i) {
            if (!g.included_[i]) continue;
            auto& row = rows[i];
            for (uint32_t j = 0; j < n; ++j) {
                if (j == i || !g.included_[j]) continue;
                const float d = planet_distance(g.position_[i], g.position_[j]);
                const uint32_t r = find_link(i, j);
                // Q2: free hop is inclusive (distance <= MaximumFleetMovementDistance).
                if (r == kNoRoute && !(d <= max_move)) continue;
                row.push_back({j, d, r, r == kNoRoute ? 1.0f : sanitize_speed(map.routes.hs_speed_factor[r])});
            }
        }
    });

    g.offsets_.assign(n + 1, 0);
    for (uint32_t i = 0; i < n; ++i) g.offsets_[i + 1] = g.offsets_[i] + static_cast<uint32_t>(rows[i].size());
    const uint32_t m = g.offsets_[n];
    g.nbr_.resize(m);
    g.dist_.resize(m);
    g.route_.resize(m);
    g.speed_.resize(m);
    g.cost_.resize(m);
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t e = g.offsets_[i];
        for (const RowEdge& re : rows[i]) {
            g.nbr_[e] = re.nbr;
            g.dist_[e] = re.dist;
            g.route_[e] = re.route;
            g.speed_[e] = re.speed;
            g.cost_[e] = re.route == kNoRoute ? re.dist : re.dist / re.speed;  // Q6 step 2
            ++e;
        }
    }

    uint64_t h = fnv1a64(&n, sizeof n);
    h = hash_vec(g.included_, h);
    h = hash_vec(g.position_, h);
    h = hash_vec(g.offsets_, h);
    h = hash_vec(g.nbr_, h);
    h = hash_vec(g.dist_, h);
    h = hash_vec(g.route_, h);
    h = hash_vec(g.speed_, h);
    h = fnv1a64(&g.max_speed_factor_, sizeof(float), h);
    h = fnv1a64(&g.max_move_, sizeof(float), h);
    g.fingerprint_ = h;
    return g;
}

}  // namespace alamo::galactic
