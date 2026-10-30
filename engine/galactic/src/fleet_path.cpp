#include "alamo/galactic/fleet_path.hpp"

#include <algorithm>

namespace alamo::galactic {

PathRequest PathRequest::for_fleet_move_mode(PlanetIndex src, PlanetIndex dst, int m) {
    PathRequest r;
    r.source = src;
    r.destination = dst;
    r.allow_unknown_or_neutral = r.allow_enemy_owned = r.skip_danger_check = (m == 5);
    r.allow_impassable = m > 2;
    r.always_accept_destination = m > 1;
    return r;
}

PathRequest PathRequest::for_ai_mode(PlanetIndex src, PlanetIndex dst, int m) {
    PathRequest r;
    r.source = src;
    r.destination = dst;
    r.allow_unknown_or_neutral = r.allow_enemy_owned = (m == 5);
    r.skip_danger_check = (m == 1 || m == 5);
    r.allow_impassable = m > 2;
    r.always_accept_destination = m > 1;
    return r;
}

bool PlayerView::is_planet_revealed(PlanetIndex p) const {
    // Q6 step 1, Is_Planet_Revealed (0x3fa420): multiplayer => true; no data pack / hidden =>
    // false; auto-reveal (mode +0x340, `fog_disabled`) => true; else the revealed byte.
    if (multiplayer) return true;
    if (bit(hidden, p, false)) return false;
    if (fog_disabled) return true;
    return bit(revealed, p, true);
}

bool PlayerView::is_passable(PlanetIndex p, const PathRequest& r) const {
    // Q5, Is_Planet_Passable_For_Player (0x3f2510).
    if (bit(held_by_ally, p, false)) return true;                              // 1
    if (bit(corrupted, p, false) && benefits_from_corruption) return true;     // 2
    if (!has_ai_system || is_human) {                                          // 3
        if (!bit(ownership_visible, p, true)) return r.allow_unknown_or_neutral;  // LOCAL mask (quirk)
        if (bit(unowned_or_neutral, p, false)) return r.allow_unknown_or_neutral;
        return r.allow_enemy_owned;
    }
    if (bit(ai_eval_failed, p, false)) return true;                            // 4
    return bit(ai_perceived_non_neutral, p, false) ? r.allow_enemy_owned : r.allow_unknown_or_neutral;
}

float edge_cost(const PlanetGraph& graph, const PlayerView& view, const PathRequest& req,
                PlanetIndex from, uint32_t e) {
    // Q6, Edge_Cost (0x3ef4c0): exact order of checks.
    const PlanetIndex to = graph.neighbour(e);
    // 1. Fog gate.
    if (!view.is_human) {
        if (req.fog_rule && view.has_ai_system &&
            (!view.is_planet_revealed(from) || !view.is_planet_revealed(to)))
            return kRejectedEdge;
    } else if (!view.multiplayer && !PlayerView::bit(view.revealed, to, true)) {
        return kRejectedEdge;  // human, single-player: B's revealed byte only (flag 5 / hidden ignored)
    }
    // 2. Base cost: distance, or distance / HS_Speed_Factor with a route.
    const float base = graph.travel_cost(e);
    // 3. Flag 4: destination accepted with the base cost (no battle / passability / danger / surcharge).
    if (req.always_accept_destination && to == req.destination) return base;
    // 4. Active battle.
    if (PlayerView::bit(view.in_battle, to, false)) return kRejectedEdge;
    if (view.is_passable(to, req)) {
        // 5. Passable: flag 2 skips the danger test (and with it the surcharge).
        if (req.skip_danger_check || !view.is_dangerous(to)) return base;
    } else {
        // 6. Impassable: needs flag 3; flag 2 does not apply here.
        if (!req.allow_impassable) return kRejectedEdge;
        if (!view.is_dangerous(to)) return base;
    }
    // 7. Dangerous fallback: corruption surcharge.
    if (PlayerView::bit(view.corrupted, to, false) && view.benefits_from_corruption)
        return base + kCorruptionSurcharge;
    return kRejectedEdge;
}

void PathScratch::prepare(uint32_t n, uint32_t node_count) {
    if (stamp_.size() != n) {
        best_g_.assign(n, 0.0f);
        stamp_.assign(n, 0);
        closed_.assign(n, 0);
        gen_ = 0;
    }
    if (++gen_ == 0) {  // wraparound: full reset (Q4)
        std::fill(stamp_.begin(), stamp_.end(), 0u);
        gen_ = 1;
    }
    pool_capacity_ = 3u * node_count;
    heap_capacity_ = 3u * node_count + 1u;
    records_.clear();
    records_.reserve(pool_capacity_);
    heap_.assign(heap_capacity_, OpenEntry{});
    heap_size_ = 0;
    dropped_ = 0;
}

namespace {

// Q4 push: append, sift up while the parent's f is STRICTLY greater. Dropped (silently) once
// size reaches capacity - 1.
void heap_push(PathScratch& s, const PathScratch::OpenEntry& x) {
    if (s.heap_size_ + 1 >= s.heap_capacity_) {
        ++s.dropped_;
        return;
    }
    uint32_t i = ++s.heap_size_;
    while (i > 1 && s.heap_[i / 2].f > x.f) {
        s.heap_[i] = s.heap_[i / 2];
        i /= 2;
    }
    s.heap_[i] = x;
}

// Q4 pop: take the root, move the last element to the root, sift down choosing the left child
// if strictly smaller than the current element, then the right child if strictly smaller than
// the choice so far. Ties prefer the parent, then the left child.
PathScratch::OpenEntry heap_pop(PathScratch& s) {
    auto& h = s.heap_;
    const PathScratch::OpenEntry top = h[1];
    h[1] = h[s.heap_size_];
    --s.heap_size_;
    const uint32_t size = s.heap_size_;
    uint32_t i = 1;
    for (;;) {
        uint32_t c = i;
        const uint32_t l = 2 * i, r = l + 1;
        if (l <= size && h[l].f < h[c].f) c = l;
        if (r <= size && h[r].f < h[c].f) c = r;
        if (c == i) break;
        std::swap(h[i], h[c]);
        i = c;
    }
    return top;
}

}  // namespace

bool find_fleet_path(const PlanetGraph& graph, const PlayerView& view, const PathRequest& req,
                     PathScratch& s, std::vector<PlanetIndex>& out, float* cost) {
    out.clear();
    const PlanetIndex src = req.source, dst = req.destination;
    // Q4: a request whose planets are not nodes fails; a same-planet request returns [src].
    if (!graph.included(src) || !graph.included(dst)) {
        s.status_ = FleetPathStatus::InvalidRequest;
        return false;
    }
    if (src == dst) {
        out.push_back(src);
        if (cost) *cost = 0.0f;
        s.status_ = FleetPathStatus::Found;
        return true;
    }
    s.prepare(graph.size(), graph.node_count());
    const uint32_t gen = s.generation();
    const Vec3 goal = graph.position(dst);
    const float max_hs = graph.max_speed_factor();

    // Start: record + entry with g = f = 0. ASSUMPTION (not stated by Q4): the start node is
    // stamped with best g 0 like any accepted node, so it is "seen" and is never re-pushed.
    s.records_.push_back({src, -1});
    s.stamp_[src] = gen;
    s.best_g_[src] = 0.0f;
    s.closed_[src] = 0;
    heap_push(s, {0, 0.0f, 0.0f});

    while (s.heap_size_ > 0) {
        const PathScratch::OpenEntry cur = heap_pop(s);
        const PlanetIndex u = s.records_[static_cast<size_t>(cur.record)].planet;
        if (u == dst) {  // goal test on pop
            for (int32_t r = cur.record; r >= 0; r = s.records_[static_cast<size_t>(r)].parent)
                out.push_back(s.records_[static_cast<size_t>(r)].planet);
            std::reverse(out.begin(), out.end());
            if (cost) *cost = cur.g;
            s.status_ = FleetPathStatus::Found;
            return true;
        }
        s.closed_[u] = 1;
        for (uint32_t e = graph.edge_begin(u); e < graph.edge_end(u); ++e) {
            const PlanetIndex v = graph.neighbour(e);
            const bool seen = s.stamp_[v] == gen;
            if (seen && s.closed_[v]) continue;  // closed nodes never reopen
            const float c = edge_cost(graph, view, req, u, e);
            if (c < 0.0f) continue;
            const float ng = cur.g + c;
            if (seen && !(ng < s.best_g_[v])) continue;
            if (s.records_.size() >= s.pool_capacity_) {  // original: null dereference
                s.status_ = FleetPathStatus::RecordPoolExhausted;
                return false;
            }
            const int32_t rec = static_cast<int32_t>(s.records_.size());
            s.records_.push_back({v, cur.record});
            s.stamp_[v] = gen;
            s.best_g_[v] = ng;
            s.closed_[v] = 0;
            const float h = planet_distance(graph.position(v), goal) / max_hs;  // Q3
            heap_push(s, {rec, ng, h + ng});
        }
    }
    s.status_ = FleetPathStatus::Unreachable;
    return false;
}

}  // namespace alamo::galactic
