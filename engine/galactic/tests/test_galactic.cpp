// alamo_galactic tests (no framework; non-zero exit on failure).
// Semantics under test: docs/re/PHASE2_ANSWERS.md Q1-Q11.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "alamo/galactic/all_pairs.hpp"
#include "alamo/galactic/cadence.hpp"
#include "alamo/galactic/fleet_path.hpp"
#include "alamo/galactic/planet_graph.hpp"
#include "alamo/test/thread_parallel_for.hpp"
#include "alamo/test/support.hpp"

using namespace alamo;
using namespace alamo::galactic;

using ::alamo::test::g_fail;

namespace {

// Portable deterministic RNG (splitmix64).
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed) {}
    uint64_t next() {
        uint64_t z = (s += 0x9e3779b97f4a7c15ull);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }
    uint32_t below(uint32_t n) { return n ? static_cast<uint32_t>(next() % n) : 0; }
    float unit() { return static_cast<float>(next() >> 40) / static_cast<float>(1 << 24); }
    bool chance(float p) { return unit() < p; }
};

struct GalaxyParams {
    uint32_t n = 50;
    bool grid = false;  // integer coordinates on a small grid: many equal distances
    float extent = 1000.0f;
    float routes_per_planet = 1.5f;
    float max_move = 100.0f;
    float nonplanet_prob = 0.0f;  // rows without PLANET behaviour (scenery): not nodes
    bool fast_lanes = false;      // high HS factors (more A* improvements / duplicates)
};

GalaxyMap random_galaxy(Rng& r, const GalaxyParams& p) {
    GalaxyMap m;
    m.max_fleet_move_distance = p.max_move;
    for (uint32_t i = 0; i < p.n; ++i) {
        const PlanetIndex pi = m.planets.add("P" + std::to_string(i));
        if (p.grid) {
            const uint32_t side = std::max(2u, static_cast<uint32_t>(std::sqrt(float(p.n))) + 1);
            m.planets.position[pi] = {float(r.below(side)) * 10.0f, float(r.below(side)) * 10.0f, 0.0f};
        } else {
            m.planets.position[pi] = {r.unit() * p.extent, r.unit() * p.extent, r.unit() * 20.0f};
        }
        if (!r.chance(p.nonplanet_prob)) m.planets.flags[pi] |= kPlanetHasPlanetBehavior;
    }
    static const float speeds[] = {0.5f, 1.0f, 1.0f, 1.5f, 2.0f, 3.0f};
    static const float lanes[] = {1.0f, 4.0f, 8.0f, 16.0f};
    const uint32_t routes = p.n > 1 ? static_cast<uint32_t>(float(p.n) * p.routes_per_planet) : 0;
    for (uint32_t k = 0; k < routes; ++k) {
        const uint32_t a = r.below(p.n), b = r.below(p.n);
        if (a == b) continue;
        const uint32_t ri = m.routes.add("R" + std::to_string(k));
        m.routes.point_a[ri] = a;
        m.routes.point_b[ri] = b;
        m.routes.hs_speed_factor[ri] = p.fast_lanes ? lanes[r.below(4)] : speeds[r.below(6)];
    }
    return m;
}

PlanetIndex add_planet(GalaxyMap& m, const char* name, Vec3 pos, uint32_t flags = kPlanetHasPlanetBehavior) {
    const PlanetIndex p = m.planets.add(name);
    m.planets.position[p] = pos;
    m.planets.flags[p] = flags;
    return p;
}
uint32_t add_route(GalaxyMap& m, PlanetIndex a, PlanetIndex b, float hs = 1.0f) {
    const auto r = m.routes.add("R" + std::to_string(m.routes.size()));
    m.routes.point_a[r] = a;
    m.routes.point_b[r] = b;
    m.routes.hs_speed_factor[r] = hs;
    return r;
}

Bitset mask(uint32_t n, std::initializer_list<uint32_t> bits) {
    Bitset b(n);
    for (auto i : bits) b.set(i);
    return b;
}

// ------------------------------------------------------------------------------------------
void test_graph_build() {
    GalaxyMap m;
    m.max_fleet_move_distance = 100.0f;
    const auto A = add_planet(m, "A", {0, 0, 0});
    const auto B = add_planet(m, "B", {500, 0, 0});
    const auto C = add_planet(m, "C", {50, 0, 0});
    const auto D = add_planet(m, "D", {1000, 1000, 0});
    const auto E = add_planet(m, "E", {10, 0, 0}, 0);  // Behavior IDLE only (Galaxy_Core_Art_Model)
    const auto F = add_planet(m, "F", {0, 100, 0}, kPlanetHasPlanetBehavior | kPlanetInBackground);
    const auto rAB = add_route(m, A, B, 2.0f);
    add_route(m, B, A, 3.0f);  // duplicate link: ignored, first route wins
    add_route(m, D, E, 4.0f);
    add_route(m, B, C, 0.5f);  // HS < 1 never lowers the tracker

    const PlanetGraph g = PlanetGraph::build(m);
    CHECK(g.size() == 6 && g.node_count() == 5);
    CHECK(!g.included(E) && g.included(F));  // Q1: In_Background is not consulted
    CHECK(!g.included(99));
    CHECK(g.edge_end(A) - g.edge_begin(A) == 3);  // B (route), C (free), F (free, exactly 100)
    CHECK(g.neighbour(g.edge_begin(A)) == B);     // ascending neighbour order
    CHECK(g.route(g.edge_begin(A)) == rAB);
    CHECK(g.speed_factor(g.edge_begin(A)) == 2.0f);
    CHECK(g.distance(g.edge_begin(A)) == 500.0f);
    CHECK(g.travel_cost(g.edge_begin(A)) == 250.0f);
    CHECK(g.neighbour(g.edge_begin(A) + 1) == C && g.route(g.edge_begin(A) + 1) == kNoRoute);
    CHECK(g.speed_factor(g.edge_begin(A) + 1) == 1.0f);
    CHECK(g.neighbour(g.edge_begin(A) + 2) == F);
    CHECK(g.edge_end(D) == g.edge_begin(D));  // route to a non-node dropped
    CHECK(g.edge_end(E) == g.edge_begin(E));
    CHECK(g.max_speed_factor() == 2.0f);  // the 3.0 duplicate and the 4.0 to E are not linked

    PlanetGraphOptions o;
    o.require_planet_behavior = false;
    const PlanetGraph gi = PlanetGraph::build(m, o);
    CHECK(gi.included(E) && gi.node_count() == 6);
    CHECK(gi.edge_end(E) - gi.edge_begin(E) == 3);  // A, C (free), D (route); F is 100.5 away
    CHECK(gi.max_speed_factor() == 4.0f);
    CHECK(gi.fingerprint() != g.fingerprint());

    PlanetGraphOptions ob;  // pre-Q1 override for mods / tests
    ob.exclude_background = true;
    CHECK(!PlanetGraph::build(m, ob).included(F));

    Bitset owned(m.planets.size(), true);  // Q1: owner == -1 => not a node
    owned.set(C, false);
    PlanetGraphOptions oo;
    oo.owned = &owned;
    const PlanetGraph go = PlanetGraph::build(m, oo);
    CHECK(!go.included(C) && go.node_count() == 4);

    PlanetGraphOptions of;  // Q3: tracker never reset on rebuild
    of.max_speed_factor_floor = 7.0f;
    CHECK(PlanetGraph::build(m, of).max_speed_factor() == 7.0f);

    // Q2 boundary: free hop at exactly max distance is included; 3-D distance.
    GalaxyMap m2;
    m2.max_fleet_move_distance = 100.0f;
    add_planet(m2, "X", {0, 0, 0});
    add_planet(m2, "Y", {60, 0, 80});
    CHECK(PlanetGraph::build(m2).edge_slots() == 2);
    m2.planets.position[1] = {60, 0, 80.01f};
    CHECK(PlanetGraph::build(m2).edge_slots() == 0);

    // Parallel build == serial build; symmetric edges; ascending order.
    Rng r(7);
    for (int t = 0; t < 20; ++t) {
        GalaxyParams p;
        p.n = 1 + r.below(150);
        p.nonplanet_prob = 0.05f;
        const GalaxyMap gm = random_galaxy(r, p);
        const PlanetGraph s = PlanetGraph::build(gm);
        const PlanetGraph pp = PlanetGraph::build(gm, {}, ::alamo::test::make_thread_parallel_for(4));
        CHECK(s.fingerprint() == pp.fingerprint());
        for (uint32_t a = 0; a < s.size(); ++a)
            for (uint32_t e = s.edge_begin(a); e < s.edge_end(a); ++e) {
                const PlanetIndex b = s.neighbour(e);
                bool found = false;
                for (uint32_t f = s.edge_begin(b); f < s.edge_end(b); ++f)
                    if (s.neighbour(f) == a) found = s.distance(f) == s.distance(e) && s.route(f) == s.route(e);
                CHECK(found);
                if (e + 1 < s.edge_end(a)) CHECK(s.neighbour(e) < s.neighbour(e + 1));
            }
    }
}

// ------------------------------------------------------------------------------------------
// Independent transcription of PHASE2_ANSWERS Q3/Q4 (written separately from fleet_path.cpp:
// explicit 1-based array heap with swap-based sifts, node table by struct).
struct ModelResult {
    FleetPathStatus status = FleetPathStatus::InvalidRequest;
    std::vector<PlanetIndex> path;
    float cost = 0;
    uint32_t dropped = 0;
};
ModelResult model_astar(const PlanetGraph& g, const PlayerView& v, const PathRequest& q) {
    ModelResult res;
    if (!g.included(q.source) || !g.included(q.destination)) return res;
    if (q.source == q.destination) {
        res.status = FleetPathStatus::Found;
        res.path = {q.source};
        return res;
    }
    const uint32_t N = g.node_count();
    struct Rec { PlanetIndex p; int parent; };
    struct Ent { int rec; float g, f; };
    struct Node { bool seen = false, closed = false; float best = 0; };
    std::vector<Rec> recs;
    std::vector<Ent> heap(3 * N + 1);
    uint32_t size = 0;
    std::vector<Node> nodes(g.size());
    auto push = [&](Ent x) {
        if (size >= 3 * N + 1 - 1) {
            ++res.dropped;
            return;
        }
        heap[++size] = x;
        for (uint32_t i = size; i > 1 && heap[i / 2].f > heap[i].f; i /= 2) std::swap(heap[i], heap[i / 2]);
    };
    auto pop = [&]() {
        Ent top = heap[1];
        heap[1] = heap[size--];
        uint32_t i = 1;
        while (true) {
            uint32_t best = i;
            if (2 * i <= size && heap[2 * i].f < heap[best].f) best = 2 * i;
            if (2 * i + 1 <= size && heap[2 * i + 1].f < heap[best].f) best = 2 * i + 1;
            if (best == i) break;
            std::swap(heap[i], heap[best]);
            i = best;
        }
        return top;
    };
    const float mx = g.max_speed_factor();
    recs.push_back({q.source, -1});
    nodes[q.source] = {true, false, 0.0f};
    push({0, 0.0f, 0.0f});
    while (size) {
        const Ent c = pop();
        const PlanetIndex u = recs[size_t(c.rec)].p;
        if (u == q.destination) {
            for (int r = c.rec; r >= 0; r = recs[size_t(r)].parent) res.path.insert(res.path.begin(), recs[size_t(r)].p);
            res.cost = c.g;
            res.status = FleetPathStatus::Found;
            return res;
        }
        nodes[u].closed = true;
        for (uint32_t e = g.edge_begin(u); e < g.edge_end(u); ++e) {
            const PlanetIndex w = g.neighbour(e);
            if (nodes[w].seen && nodes[w].closed) continue;
            const float ec = edge_cost(g, v, q, u, e);
            if (ec < 0) continue;
            const float ng = c.g + ec;
            if (nodes[w].seen && !(ng < nodes[w].best)) continue;
            if (recs.size() == 3 * size_t(N)) {
                res.status = FleetPathStatus::RecordPoolExhausted;
                return res;
            }
            recs.push_back({w, c.rec});
            nodes[w] = {true, false, ng};
            const float h = planet_distance(g.position(w), g.position(q.destination)) / mx;
            push({int(recs.size() - 1), ng, h + ng});
        }
    }
    res.status = FleetPathStatus::Unreachable;
    return res;
}

// Brute force: Dijkstra on edge_cost (any view), float, no heuristic.
float dijkstra_cost(const PlanetGraph& g, const PlayerView& v, const PathRequest& req) {
    const uint32_t n = g.size();
    std::vector<float> d(n, std::numeric_limits<float>::infinity());
    using Item = std::pair<float, uint32_t>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> pq;
    d[req.source] = 0;
    pq.emplace(0.0f, req.source);
    while (!pq.empty()) {
        auto [du, u] = pq.top();
        pq.pop();
        if (du != d[u]) continue;
        for (uint32_t e = g.edge_begin(u); e < g.edge_end(u); ++e) {
            const float c = edge_cost(g, v, req, u, e);
            if (c < 0) continue;
            const uint32_t w = g.neighbour(e);
            if (du + c < d[w]) {
                d[w] = du + c;
                pq.emplace(d[w], w);
            }
        }
    }
    return d[req.destination];
}

bool path_is_valid(const PlanetGraph& g, const PlayerView& v, const PathRequest& req,
                   const std::vector<PlanetIndex>& path, float cost) {
    if (path.empty() || path.front() != req.source || path.back() != req.destination) return false;
    float sum = 0;
    for (size_t k = 1; k < path.size(); ++k) {
        float c = -1;
        for (uint32_t e = g.edge_begin(path[k - 1]); e < g.edge_end(path[k - 1]); ++e)
            if (g.neighbour(e) == path[k]) c = edge_cost(g, v, req, path[k - 1], e);
        if (c < 0) return false;
        sum += c;
    }
    return sum == cost;
}

void test_astar_vs_model_and_dijkstra() {
    Rng r(11);
    int queries = 0, found = 0, model_mismatch = 0, dropped_runs = 0, exhausted = 0, exhausted_sparse = 0;
    PathScratch scratch;
    for (int t = 0; t < 400; ++t) {
        GalaxyParams p;
        p.n = 2 + r.below(150);
        p.grid = r.chance(0.3f);
        p.max_move = r.chance(0.5f) ? 100.0f : 150.0f;
        if (p.grid) p.max_move = r.chance(0.5f) ? 10.0f : 15.0f;
        p.nonplanet_prob = 0.05f;
        const bool dense = r.chance(0.25f);  // many duplicates: exercises the 3N heap / pool caps
        if (dense) {
            p.extent = 120.0f;
            p.routes_per_planet = 3.0f;
            p.fast_lanes = true;
        }
        const PlanetGraph g = PlanetGraph::build(random_galaxy(r, p));
        PlayerView view;
        if (r.chance(0.5f)) {
            view.dangerous = Bitset(p.n);
            view.held_by_ally = Bitset(p.n);
            view.corrupted = Bitset(p.n);
            view.benefits_from_corruption = r.chance(0.5f);
            for (uint32_t i = 0; i < p.n; ++i) {
                view.dangerous.set(i, r.chance(0.2f));
                view.held_by_ally.set(i, r.chance(0.5f));
                view.corrupted.set(i, r.chance(0.2f));
            }
        }
        for (int q = 0; q < 50; ++q) {
            const PlanetIndex qs = r.below(p.n), qd = r.below(p.n);  // sequenced: argument order is unspecified
            const PathRequest req = PathRequest::for_fleet_move_mode(qs, qd, int(r.below(6)));
            std::vector<PlanetIndex> path;
            float cost = -1;
            const bool ok = find_fleet_path(g, view, req, scratch, path, &cost);
            const ModelResult mr = model_astar(g, view, req);
            ++queries;
            if (mr.status != scratch.status() || mr.path != path || (ok && mr.cost != cost) ||
                mr.dropped != scratch.dropped_pushes())
                ++model_mismatch;
            if (scratch.dropped_pushes()) ++dropped_runs;
            if (scratch.status() == FleetPathStatus::RecordPoolExhausted) {
                ++exhausted;
                if (!dense) ++exhausted_sparse;
            }
            if (!g.included(req.source) || !g.included(req.destination)) {
                CHECK(!ok && scratch.status() == FleetPathStatus::InvalidRequest);
                continue;
            }
            const float ref = dijkstra_cost(g, view, req);
            if (scratch.status() != FleetPathStatus::RecordPoolExhausted) CHECK(ok == std::isfinite(ref));
            if (!ok) continue;
            ++found;
            CHECK(path_is_valid(g, view, req, path, cost));
            CHECK(cost <= ref * (1.0f + 1e-4f) + 1e-2f);
        }
    }
    CHECK(model_mismatch == 0);
    // The 3N heap can never be full when a push arrives: every heap entry owns a distinct record
    // and the new record is allocated first, so size <= records - 1 <= 3N - 1. The record pool
    // (the original's crash) always runs out first; the drop rule is kept but is dead.
    CHECK(dropped_runs == 0);
    CHECK(exhausted > 0);
    std::printf("  A*: %d queries, %d reachable, %d model mismatches, %d with heap drops, %d pool-exhausted "
                "(%d of them on sparse maps)\n",
                queries, found, model_mismatch, dropped_runs, exhausted, exhausted_sparse);
}

// Exact f tie: S(0) - A(1) - T(3) and S - B(2) - T, everything ties at f = 30. The heap keeps A
// (pushed first) at the root, so A expands first and T's record comes from A: [0, 1, 3]. (The old
// "deeper g first" tie-break would have returned [0, 2, 3].)
void test_astar_tie_order() {
    GalaxyMap m;
    m.max_fleet_move_distance = 20.0f;
    add_planet(m, "S", {0, 0, 0});
    add_planet(m, "A", {10, 0, 0});
    add_planet(m, "B", {20, 0, 0});
    add_planet(m, "T", {30, 0, 0});
    const PlanetGraph g = PlanetGraph::build(m);
    PathScratch s;
    std::vector<PlanetIndex> out;
    PathRequest q;
    q.source = 0;
    q.destination = 3;
    float c = 0;
    CHECK(find_fleet_path(g, {}, q, s, out, &c) && out == (std::vector<PlanetIndex>{0, 1, 3}) && c == 30.0f);
    CHECK(s.records_used() == 4);  // S, A, B, T (B's re-relaxation via A is not strictly better)
}

// ------------------------------------------------------------------------------------------
// Diamond: 0 -(10)- 1 -(10)- 2 (cheap, through 1) and 0 -(31.6)- 3 -(31.6)- 2 (expensive).
struct Diamond {
    PlanetGraph g;
    Diamond() {
        GalaxyMap m;
        m.max_fleet_move_distance = 5.0f;
        add_planet(m, "S", {0, 0, 0});
        add_planet(m, "Mid", {10, 0, 0});
        add_planet(m, "T", {20, 0, 0});
        add_planet(m, "Alt", {10, 30, 0});
        add_route(m, 0, 1);
        add_route(m, 1, 2);
        add_route(m, 0, 3);
        add_route(m, 3, 2);
        g = PlanetGraph::build(m);
    }
    std::vector<PlanetIndex> run(const PlayerView& v, PathRequest req, float* cost = nullptr) const {
        req.source = 0;
        req.destination = 2;
        PathScratch s;
        std::vector<PlanetIndex> out;
        find_fleet_path(g, v, req, s, out, cost);
        return out;
    }
};

void test_edge_cost_rules() {
    const Diamond d;
    const std::vector<PlanetIndex> via_mid{0, 1, 2}, via_alt{0, 3, 2}, none{};
    auto mk = [](std::initializer_list<uint32_t> bits) { return mask(4, bits); };
    PathRequest all;  // all flags true (Path_Request_Init)
    PathRequest strict = all;
    strict.allow_unknown_or_neutral = strict.allow_enemy_owned = strict.skip_danger_check = false;
    strict.allow_impassable = strict.always_accept_destination = false;
    PathRequest f0 = strict, f1 = strict;
    f0.allow_unknown_or_neutral = true;
    f1.allow_enemy_owned = true;

    CHECK(d.run({}, all) == via_mid);
    {  // same planet / non-node
        PathScratch s;
        std::vector<PlanetIndex> out;
        PathRequest q;
        q.source = q.destination = 3;
        float c = -1;
        CHECK(find_fleet_path(d.g, {}, q, s, out, &c) && out == std::vector<PlanetIndex>{3} && c == 0.0f);
        q.destination = 99;
        CHECK(!find_fleet_path(d.g, {}, q, s, out) && out.empty() && s.status() == FleetPathStatus::InvalidRequest);
    }

    // ---- Q6 step 1, fog gate ----
    PlayerView human;  // human, single player: only B's revealed byte; flag 5 / hidden ignored
    human.is_human = true;
    human.revealed = mk({0, 2, 3});
    CHECK(d.run(human, all) == via_alt);
    PathRequest nofog = all;
    nofog.fog_rule = false;
    CHECK(d.run(human, nofog) == via_alt);  // flag 5 is irrelevant for humans
    human.revealed = mk({1, 2, 3});
    CHECK(d.run(human, all) == via_mid);    // source A is not checked
    human.revealed = mk({0, 1, 3});
    CHECK(d.run(human, all) == none);       // destination: gate runs before flag 4
    human.revealed = Bitset();
    human.hidden = mk({1});
    CHECK(d.run(human, all) == via_mid);    // hidden byte ignored on the human path
    human.revealed = mk({0, 2, 3});
    human.multiplayer = true;
    CHECK(d.run(human, all) == via_mid);    // multiplayer: no gate

    PlayerView ai;  // non-human with an AI system: gated by flag 5, both ends Is_Planet_Revealed
    ai.has_ai_system = true;
    ai.revealed = mk({1, 2, 3});
    CHECK(d.run(ai, all) == none);          // source must be revealed too
    CHECK(d.run(ai, nofog) == via_mid);
    ai.revealed = mk({0, 2, 3});
    CHECK(d.run(ai, all) == via_alt);
    ai.fog_disabled = true;
    CHECK(d.run(ai, all) == via_mid);
    ai.hidden = mk({1});
    CHECK(d.run(ai, all) == via_alt);       // hidden beats the fog option
    ai.multiplayer = true;
    CHECK(d.run(ai, all) == via_mid);       // multiplayer: Is_Planet_Revealed is true
    PlayerView noai;                         // non-human without AI system: no gate at all
    noai.revealed = mk({0, 2, 3});
    noai.hidden = mk({1});
    CHECK(d.run(noai, all) == via_mid);

    // ---- step 4, active battle ----
    PlayerView battle;
    battle.in_battle = mk({1});
    CHECK(d.run(battle, all) == via_alt);
    battle.in_battle = mk({2});
    CHECK(d.run(battle, all) == via_mid);  // flag 4 returns before the battle test
    CHECK(d.run(battle, strict) == none);

    // ---- Q5 passability, human / no-AI branch ----
    PlayerView own;  // planets 0, 2, 3 held by me / an ally; Mid owned by a non-neutral enemy
    own.held_by_ally = mk({0, 2, 3});
    CHECK(d.run(own, strict) == via_alt);
    CHECK(d.run(own, f1) == via_mid);
    CHECK(d.run(own, f0) == via_alt);
    own.unowned_or_neutral = mk({1});
    CHECK(d.run(own, f0) == via_mid);
    CHECK(d.run(own, f1) == via_alt);
    own.unowned_or_neutral = Bitset();
    own.ownership_visible = mk({0, 2, 3});  // LOCAL visibility bit 0 clear => flag 0 decides
    CHECK(d.run(own, f0) == via_mid);
    CHECK(d.run(own, f1) == via_alt);
    own.ownership_visible = Bitset();
    own.corrupted = mk({1});
    CHECK(d.run(own, strict) == via_alt);
    own.benefits_from_corruption = true;   // corrupted + Benefits_From_Corruption => passable
    CHECK(d.run(own, strict) == via_mid);
    PathRequest imp = strict;              // flag 3: impassable allowed when not dangerous
    imp.allow_impassable = true;
    CHECK(d.run(PlayerView{}, imp) == via_mid);

    // ---- Q5 AI branch (has AI system, not human) ----
    PlayerView aip;
    aip.has_ai_system = true;
    aip.held_by_ally = mk({0, 2, 3});
    CHECK(d.run(aip, f0) == via_mid);  // no perceived player => flag 0
    CHECK(d.run(aip, f1) == via_alt);
    aip.ai_perceived_non_neutral = mk({1});
    CHECK(d.run(aip, f0) == via_alt);
    CHECK(d.run(aip, f1) == via_mid);
    aip.ai_eval_failed = mk({1});
    CHECK(d.run(aip, strict) == via_mid);  // evaluation failure => passable
    aip.is_human = true;                   // human => human branch even with an AI system
    aip.ai_eval_failed = Bitset();
    CHECK(d.run(aip, f1) == via_mid && d.run(aip, f0) == via_alt);

    // ---- step 5-7, danger and the corruption surcharge ----
    PlayerView danger;
    danger.dangerous = mk({1});
    CHECK(d.run(danger, all) == via_mid);  // flag 2 skips the danger test on passable planets
    PathRequest nod = all;
    nod.skip_danger_check = false;
    CHECK(d.run(danger, nod) == via_alt);
    PathRequest impd = all;  // impassable (no flag 0/1) + dangerous: rejected even with flags 2 and 3
    impd.allow_unknown_or_neutral = impd.allow_enemy_owned = false;
    CHECK(d.run(danger, impd) == via_alt);
    danger.dangerous = mk({2});
    CHECK(d.run(danger, nod) == via_mid);  // flag 4 accepts the dangerous destination
    PathRequest nod4 = nod;
    nod4.always_accept_destination = false;
    CHECK(d.run(danger, nod4) == none);

    PlayerView corr;  // +100.0f: dangerous, corrupted, mover benefits from corruption
    corr.dangerous = mk({1, 3});
    corr.corrupted = mk({1, 3});
    float c = 0;
    CHECK(d.run(corr, nod, &c) == none);   // no Benefits_From_Corruption: rejected
    corr.benefits_from_corruption = true;
    CHECK(d.run(corr, nod, &c) == via_mid && c == 120.0f);
    CHECK(d.run(corr, all, &c) == via_mid && c == 20.0f);  // flag 2: no danger test, no surcharge
    corr.dangerous = mk({2});
    corr.corrupted = mk({2});
    CHECK(d.run(corr, nod, &c) == via_mid && c == 20.0f);   // flag 4 skips the surcharge
    CHECK(d.run(corr, nod4, &c) == via_mid && c == 120.0f);
    PlayerView pv;
    pv.dangerous = mk({1});
    pv.corrupted = mk({1});
    pv.benefits_from_corruption = true;
    CHECK(edge_cost(d.g, pv, nod4, 0, d.g.edge_begin(0)) == 110.0f);

    // Mode helpers.
    const PathRequest m5 = PathRequest::for_fleet_move_mode(0, 2, 5);
    CHECK(m5.allow_unknown_or_neutral && m5.allow_enemy_owned && m5.skip_danger_check && m5.allow_impassable &&
          m5.always_accept_destination && m5.fog_rule);
    const PathRequest m2 = PathRequest::for_fleet_move_mode(0, 2, 2);
    CHECK(!m2.allow_unknown_or_neutral && !m2.skip_danger_check && !m2.allow_impassable && m2.always_accept_destination);
    const PathRequest a1 = PathRequest::for_ai_mode(0, 2, 1);
    CHECK(!a1.allow_unknown_or_neutral && !a1.allow_enemy_owned && a1.skip_danger_check &&
          !a1.always_accept_destination && !a1.allow_impassable && a1.fog_rule);
    CHECK(PathRequest::for_ai_mode(0, 2, 2).always_accept_destination);
}

void test_astar_concurrent_and_wrap() {
    Rng r(23);
    GalaxyParams p;
    p.n = 200;
    p.routes_per_planet = 1.5f;
    const PlanetGraph g = PlanetGraph::build(random_galaxy(r, p));
    PlayerView view;
    view.dangerous = Bitset(p.n);
    view.held_by_ally = Bitset(p.n);
    for (uint32_t i = 0; i < p.n; ++i) {
        if (r.chance(0.1f)) view.dangerous.set(i);
        if (r.chance(0.9f)) view.held_by_ally.set(i);
    }
    std::vector<PathRequest> reqs;
    for (int q = 0; q < 4000; ++q)
    {
        const PlanetIndex qs = r.below(p.n), qd = r.below(p.n);
        reqs.push_back(PathRequest::for_fleet_move_mode(qs, qd, int(r.below(6))));
    }
    std::vector<std::vector<PlanetIndex>> serial(reqs.size());
    std::vector<float> serial_cost(reqs.size());
    {
        PathScratch s;
        for (size_t q = 0; q < reqs.size(); ++q) find_fleet_path(g, view, reqs[q], s, serial[q], &serial_cost[q]);
    }
    constexpr unsigned kThreads = 8;
    std::vector<std::vector<std::vector<PlanetIndex>>> res(kThreads, std::vector<std::vector<PlanetIndex>>(reqs.size()));
    std::vector<std::vector<float>> cost(kThreads, std::vector<float>(reqs.size()));
    std::vector<std::thread> ts;
    for (unsigned t = 0; t < kThreads; ++t)
        ts.emplace_back([&, t] {
            PathScratch s;
            for (size_t k = 0; k < reqs.size(); ++k) {
                const size_t q = (k + t * 977) % reqs.size();  // different order per thread
                find_fleet_path(g, view, reqs[q], s, res[t][q], &cost[t][q]);
            }
        });
    for (auto& th : ts) th.join();
    bool same = true;
    for (unsigned t = 0; t < kThreads; ++t)
        for (size_t q = 0; q < reqs.size(); ++q)
            same = same && res[t][q] == serial[q] && (serial[q].empty() || cost[t][q] == serial_cost[q]);
    CHECK(same);

    // Generation stamp wraparound.
    PathScratch s;
    std::vector<PlanetIndex> out;
    find_fleet_path(g, view, reqs[0], s, out);
    s.force_generation_for_test(0xFFFFFFFFu);
    bool wrap_ok = true;
    for (size_t q = 0; q < 50; ++q) {
        find_fleet_path(g, view, reqs[q], s, out);
        wrap_ok = wrap_ok && out == serial[q];
    }
    CHECK(wrap_ok);
    CHECK(s.generation() < 100);
}

// ------------------------------------------------------------------------------------------
void test_weight_arithmetic() {
    // Q7
    CHECK(node_cost_from_value(-50.0) == 42949672u);
    CHECK(node_cost_from_value(123456.9) == 1234u);
    CHECK(node_cost_from_value(99.99) == 0u);
    CHECK(node_cost_from_value(-0.9) == 0u);  // trunc toward zero, not floor
    CHECK(node_cost_from_value(4294967296.0 + 512.0) == 5u);  // low 32 bits
    CHECK(node_cost_from_value(std::nan("")) == 0u && node_cost_from_value(1e30) == 0u);
    CHECK(node_cost_from_raw(0xFFFFFFFFu) == 42949672u);
    // Q8
    CHECK(hop_from_distance(12.34f) == 123);
    CHECK(hop_from_distance(7000.0f) == (70000 & 0xFFFF));
    CHECK(hop_from_distance(1e10f) == static_cast<uint16_t>(static_cast<int64_t>(1e10f * 10.0f) & 0xFFFF));
    CHECK(hop_from_distance(1e30f) == 0 && hop_from_distance(std::numeric_limits<float>::quiet_NaN()) == 0);
    CHECK(pack_edge_weight(3, 0x1234) == 0x31234u);
    CHECK(pack_edge_weight(0x10005u, 7) == 0x50007u);  // wraps
    CHECK(pack_edge_weight(0xFFFF, 0xFFFF) == kUnlinked);

    // Q8 operation order (d / HS) * 10.0f, observed through the matrix: find a (d, hs) where it
    // differs from (d * 10) / hs and build the 2-planet galaxy.
    float dd = 0, hs = 0;
    for (int a = 1; a < 2000 && hs == 0; ++a)
        for (float h : {0.3f, 0.7f, 1.1f, 1.3f, 3.0f}) {
            const float x = float(a) * 0.37f;
            if (static_cast<int64_t>((x / h) * 10.0f) != static_cast<int64_t>((x * 10.0f) / h)) {
                dd = x;
                hs = h;
                break;
            }
        }
    CHECK(hs != 0);
    GalaxyMap m;
    m.max_fleet_move_distance = 1.0f;
    add_planet(m, "A", {0, 0, 0});
    add_planet(m, "B", {dd, 0, 0});
    add_route(m, 0, 1, hs);
    const PlanetGraph g = PlanetGraph::build(m);
    const double costs[2] = {0.0, 200.0};  // node costs 0, 2
    AllPairsInput in;
    in.node_value = costs;
    AllPairsMatrices mm;
    reference_all_pairs(g, in, mm);
    const float dist = g.distance(g.edge_begin(0));
    CHECK(mm.weight(0, 1) == ((2u << 16) | uint32_t(static_cast<int64_t>((dist / hs) * 10.0f) & 0xFFFF)));
    CHECK(mm.weight(1, 0) == uint32_t(static_cast<int64_t>((dist / hs) * 10.0f) & 0xFFFF));
    CHECK(mm.pred(0, 1) == 0 && mm.pred(0, 0) == kNoPred && mm.weight(0, 0) == 0);

    // node_value path (Q7): a negative total wraps.
    const double vals[2] = {-50.0, 299.0};
    AllPairsInput iv;
    iv.node_value = vals;
    AllPairsMatrices mv;
    reference_all_pairs(g, iv, mv);
    CHECK(mv.weight(1, 0) == pack_edge_weight(42949672u, hop_from_distance(dist / hs)));
    CHECK(mv.weight(0, 1) == pack_edge_weight(2u, hop_from_distance(dist / hs)));

    // Q8 quirk: a linked pair weighing exactly 0xFFFFFFFF is "unlinked".
    GalaxyMap q;
    q.max_fleet_move_distance = 10000.0f;
    add_planet(q, "A", {0, 0, 0});
    add_planet(q, "B", {6553.5f, 0, 0});
    const PlanetGraph gq = PlanetGraph::build(q);
    const double qc[2] = {0.0, 6553500.0};  // node costs 0, 0xFFFF
    AllPairsInput iq;
    iq.node_value = qc;
    AllPairsMatrices a, b;
    reference_all_pairs(gq, iq, a);
    fast_all_pairs(gq, iq, b);
    CHECK(a.weight(0, 1) == kUnlinked && a.pred(0, 1) == kNoPred && a.weight(1, 0) == 0xFFFFu && a == b);

    // Q2/Q8: source type's Layer_Z_Adjust added to the destination z, AI matrix only.
    GalaxyMap z;
    z.max_fleet_move_distance = 100.0f;
    add_planet(z, "A", {0, 0, 0});
    add_planet(z, "B", {0, 0, 120});  // 120 apart: not a graph edge
    add_planet(z, "C", {50, 0, 0});
    const PlanetGraph gz = PlanetGraph::build(z);
    const float zo[3] = {-30.0f, 0.0f, 100.0f};
    AllPairsInput iz;
    iz.z_offset = zo;
    AllPairsMatrices mz;
    reference_all_pairs(gz, iz, mz);
    CHECK(mz.weight(0, 1) == hop_from_distance(90.0f) && mz.pred(0, 1) == 0);  // linked by the adjust
    CHECK(mz.weight(1, 0) == kUnlinked);  // B->A unadjusted: 120 > 100
    CHECK(mz.weight(0, 2) == hop_from_distance(planet_distance({0, 0, 0}, {50, 0, -30})));
    CHECK(mz.weight(2, 0) == kUnlinked);  // C->A: dest z + 100 => 111.8 > 100
}

// ------------------------------------------------------------------------------------------
int g_cyclic_chains = 0, g_short_chains = 0;
// get_path from the reference must walk real edges with per-hop weights (Q10).
bool check_paths(const detail::RawGraph& g, const AllPairsMatrices& m, Rng& r, bool exact) {
    std::vector<PlanetIndex> path;
    std::vector<uint32_t> hw;
    auto edge = [&](uint32_t a, uint32_t v) -> uint32_t {
        for (uint32_t e = g.offsets[a]; e < g.offsets[a + 1]; ++e)
            if (g.nbr[e] == v) return g.w[e];
        return kUnlinked;
    };
    for (int q = 0; q < 20 && g.n; ++q) {
        const uint32_t a = r.below(g.n), b = r.below(g.n);
        const bool ok = m.get_path(a, b, path, &hw);
        const bool node = g.node.empty() || (g.node[a] && g.node[b]);
        if (a == b) {
            if (ok != node) return false;
            if (ok && (path != std::vector<PlanetIndex>{a, a} || hw != std::vector<uint32_t>{0, 0})) return false;
            continue;
        }
        if (!ok && m.pred(a, b) >= 0) {
            // Only legal for a wrapped matrix whose predecessor chain is cyclic (the original's
            // Get_Path would never terminate there).
            if (exact) return false;
            uint32_t x = b;
            for (uint32_t s = 0; s <= g.n && x != kInvalidPlanet; ++s)
                x = m.pred(a, x) >= 0 ? static_cast<uint32_t>(m.pred(a, x)) : kInvalidPlanet;
            if (x == kInvalidPlanet) return false;
            ++g_cyclic_chains;
            continue;
        }
        if (ok != (m.pred(a, b) >= 0)) return false;
        if (!ok) continue;
        if (path.back() != b || hw.size() != path.size() || hw.back() != 0) return false;
        if (path.front() != a) {
            if (exact) return false;  // wrapped: chain may stop at a vertex unreachable from a
            ++g_short_chains;
        }
        uint64_t sum = 0;
        for (size_t k = 1; k < path.size(); ++k) {
            if (hw[k - 1] != m.weight(path[k - 1], path[k])) return false;
            if (edge(path[k - 1], path[k]) == kUnlinked) return false;
            if (exact && hw[k - 1] != edge(path[k - 1], path[k])) return false;
            sum += hw[k - 1];
        }
        if (exact && sum != m.weight(a, b)) return false;
    }
    return true;
}

// Random directed graph straight into the raw form.
detail::RawOwned random_raw(Rng& r, uint32_t n, int mode) {
    const float density = r.chance(0.5f) ? 2.5f / float(std::max(1u, n)) : (r.chance(0.5f) ? 0.3f : 0.9f);
    const bool symmetric = r.chance(0.5f);
    std::vector<uint32_t> cost(n);
    for (auto& c : cost) {
        if (mode == 0) c = r.below(3);
        else if (mode == 1) c = r.chance(0.7f) ? 0 : r.below(4);
        else if (mode == 2) c = r.below(64);
        else c = r.chance(0.5f) ? 0xFFFFu - r.below(3) : (r.chance(0.5f) ? 0x4000u + r.below(4) : 0u);  // wraps
    }
    std::vector<uint8_t> node(n, 1);
    for (auto& x : node) x = r.chance(0.1f) ? 0 : 1;
    std::vector<std::vector<uint32_t>> hop(n, std::vector<uint32_t>(n, 0x10000u));  // 0x10000 = none
    for (uint32_t a = 0; a < n; ++a)
        for (uint32_t b = 0; b < n; ++b) {
            if (a == b || !node[a] || !node[b]) continue;
            if (symmetric && b < a) {
                hop[a][b] = hop[b][a];
                continue;
            }
            if (!r.chance(density)) continue;
            uint32_t h;
            if (mode == 0) h = r.below(3);
            else if (mode == 1) h = r.below(2) * 7;
            else if (mode == 2) h = r.below(2000);
            else h = r.chance(0.5f) ? 0xFFFFu : r.below(3);
            hop[a][b] = h;
        }
    detail::RawOwned o;
    o.n = n;
    o.offsets.push_back(0);
    o.node = node;
    for (uint32_t a = 0; a < n; ++a) {
        for (uint32_t b = 0; b < n; ++b) {
            if (hop[a][b] > 0xFFFFu) continue;
            const uint32_t w = pack_edge_weight(cost[b], static_cast<uint16_t>(hop[a][b]));
            if (w == kUnlinked) continue;
            o.nbr.push_back(b);
            o.w.push_back(w);
        }
        o.offsets.push_back(static_cast<uint32_t>(o.nbr.size()));
    }
    return o;
}

uint32_t random_size(Rng& r) {
    const uint32_t k = r.below(10);
    if (k < 3) return 1 + r.below(8);
    if (k < 7) return 1 + r.below(40);
    return 1 + r.below(120);
}

void test_all_pairs_equivalence() {
    Rng r(0xA11);
    const ParallelFor threaded = ::alamo::test::make_thread_parallel_for(4);
    int raw_graphs = 0, geo_graphs = 0, mismatches = 0, path_fail = 0, eligible = 0, fallback = 0, wrapped = 0;
    int par_ref_checked = 0;
    uint64_t entries = 0;
    for (int t = 0; t < 6000; ++t) {
        const uint32_t n = random_size(r);
        const int mode = t % 4;
        const detail::RawOwned o = random_raw(r, n, mode);
        const detail::RawGraph g = o.view();
        AllPairsMatrices ref;
        detail::reference_raw(g, ref);
        if (t % 50 == 0) {  // row-parallel reference is exact (wrapping included)
            AllPairsMatrices refp;
            detail::reference_raw(g, refp, threaded);
            if (!(ref == refp)) ++mismatches;
            ++par_ref_checked;
        }
        const bool elig = detail::fast_path_eligible(g);
        if (elig) {
            ++eligible;
            AllPairsMatrices fast, fastp;
            detail::fast_raw(g, fast, serial_for);
            detail::fast_raw(g, fastp, threaded);
            if (!(ref == fast) || !(ref == fastp)) {
                ++mismatches;
                if (mismatches < 4) std::printf("  raw mismatch: graph %d n=%u mode=%d\n", t, n, mode);
            }
        } else {
            ++fallback;
            // Some ineligible graphs really wrap: a stored weight below the exact shortest distance.
            for (uint32_t i = 0; i < n && n > 2; ++i)
                for (uint32_t j = 0; j < n; ++j)
                    if (ref.weight(i, j) != kUnlinked && ref.pred(i, j) >= 0) {
                        // per-hop sum (uint64) larger than the stored value => a wrap happened
                        std::vector<PlanetIndex> p;
                        std::vector<uint32_t> hw;
                        if (ref.get_path(i, j, p, &hw)) {
                            uint64_t s = 0;
                            for (uint32_t x : hw) s += x;
                            if (s != ref.weight(i, j)) {
                                ++wrapped;
                                i = n;
                                break;
                            }
                        }
                    }
        }
        if (!check_paths(g, ref, r, elig)) ++path_fail;
        entries += uint64_t(n) * n;
        ++raw_graphs;
    }
    for (int t = 0; t < 2500; ++t) {
        GalaxyParams p;
        p.n = random_size(r);
        p.grid = r.chance(0.5f);
        p.extent = 300.0f;
        p.routes_per_planet = r.chance(0.5f) ? 1.5f : 0.5f;
        p.max_move = p.grid ? (r.chance(0.5f) ? 10.0f : 15.0f) : (r.chance(0.5f) ? 60.0f : 120.0f);
        p.nonplanet_prob = 0.05f;
        const GalaxyMap gm = random_galaxy(r, p);
        const PlanetGraph pg = PlanetGraph::build(gm);
        std::vector<double> value(p.n);
        const uint32_t cmode = r.below(5);
        for (auto& v : value)
            v = cmode == 0 ? 0.0
                : cmode == 1 ? double(r.below(300))
                : cmode == 2 ? double(r.below(5000000)) - 50000.0  // negatives wrap to ~42.9M
                : cmode == 3 ? double(r.below(2000)) + 0.5
                             : double(r.below(100000));
        Bitset fog(p.n, true);
        const bool use_fog = r.chance(0.5f);
        for (uint32_t i = 0; i < p.n; ++i)
            if (r.chance(0.15f)) fog.set(i, false);
        std::vector<float> z(p.n);
        for (auto& v : z) v = r.chance(0.5f) ? 0.0f : (float(r.below(5)) - 2.0f) * 15.0f;
        AllPairsInput in;
        in.node_value = value;
        in.revealed = use_fog ? &fog : nullptr;
        if (r.chance(0.4f)) in.z_offset = z;
        AllPairsMatrices ref, fast, fastp;
        reference_all_pairs(pg, in, ref);
        const AllPairsMethod m1 = fast_all_pairs(pg, in, fast, serial_for);
        fast_all_pairs(pg, in, fastp, threaded);
        if (m1 == AllPairsMethod::Fast) ++eligible; else ++fallback;
        if (!(ref == fast) || !(ref == fastp)) {
            ++mismatches;
            if (mismatches < 4) std::printf("  geo mismatch: graph %d n=%u\n", t, p.n);
        }
        detail::RawOwned o;
        detail::build_raw(pg, in, o);
        if (!check_paths(o.view(), ref, r, detail::fast_path_eligible(o.view()))) ++path_fail;
        entries += uint64_t(p.n) * p.n;
        ++geo_graphs;
    }
    CHECK(mismatches == 0);
    CHECK(path_fail == 0);
    CHECK(eligible > 1000 && fallback > 500 && wrapped > 0);
    std::printf("  FW == fast: %d raw + %d geometric graphs (%d fast-eligible, %d fallback, %d with real wraps), "
                "%llu matrix entries, %d mismatches, %d bad paths, %d row-parallel FW checks; wrapped Get_Path: "
                "%d cyclic chains, %d chains not ending at src\n",
                raw_graphs, geo_graphs, eligible, fallback, wrapped, static_cast<unsigned long long>(entries),
                mismatches, path_fail, par_ref_checked, g_cyclic_chains, g_short_chains);
}

void test_fw_semantics() {
    // Hand-checked tie: 0-1, 0-2, 1-3, 2-3, all weight-equal. FW: M(0,3) = min(max 1, max 2) = 1.
    detail::RawOwned o;
    o.n = 4;
    o.offsets = {0, 2, 4, 6, 8};
    o.nbr = {1, 2, 0, 3, 0, 3, 1, 2};
    o.w = std::vector<uint32_t>(8, 5);
    AllPairsMatrices ref, fast;
    detail::reference_raw(o.view(), ref);
    detail::fast_raw(o.view(), fast, serial_for);
    CHECK(ref == fast);
    CHECK(ref.pred(0, 3) == 1 && ref.pred(3, 0) == 1 && ref.weight(0, 3) == 10);
    for (uint32_t i = 0; i < 4; ++i) CHECK(ref.weight(i, i) == 0 && ref.pred(i, i) == kNoPred);
    std::vector<PlanetIndex> path;
    std::vector<uint32_t> hw;
    CHECK(ref.get_path(0, 3, path, &hw) && path == (std::vector<PlanetIndex>{0, 1, 3}) &&
          hw == (std::vector<uint32_t>{5, 5, 0}));  // per hop, destination entry 0
    CHECK(ref.get_path(2, 2, path, &hw) && path == (std::vector<PlanetIndex>{2, 2}) && hw == (std::vector<uint32_t>{0, 0}));
    CHECK(!ref.get_path(0, 9, path, &hw) && path.empty() && hw.empty());

    // Wrap: 0->1 = 0xFFFF0000, 1->2 = 0x00020000, 0->2 = 0x00100000. The uint32 sum through 1
    // wraps to 0x00010000 and "wins" (Q9: not detected).
    detail::RawOwned w;
    w.n = 3;
    w.offsets = {0, 2, 3, 3};
    w.nbr = {1, 2, 2};
    w.w = {0xFFFF0000u, 0x00100000u, 0x00020000u};
    w.node = {1, 1, 0};  // planet 2 is not a node in get_path terms (matrices still filled)
    AllPairsMatrices mw;
    detail::reference_raw(w.view(), mw);
    CHECK(!detail::fast_path_eligible(w.view()));
    CHECK(mw.weight(0, 2) == 0x00010000u && mw.pred(0, 2) == 1);
    CHECK(!mw.get_path(0, 2, path) && !mw.get_path(2, 2, path));  // non-node => empty
    CHECK(mw.get_path(1, 1, path) && path.size() == 2);
    CHECK(mw.weight(2, 0) == kUnlinked && !mw.get_path(1, 0, path));  // unreachable => empty

    // Eligibility boundary: B = 2 * sum(max in-weight).
    detail::RawOwned b;
    b.n = 2;
    b.offsets = {0, 1, 2};
    b.nbr = {1, 0};
    b.w = {0x3FFFFFFFu, 0x40000000u};
    CHECK(detail::walk_bound(b.view()) == 0xFFFFFFFEull && detail::fast_path_eligible(b.view()));
    b.w[1] = 0x40000001u;
    CHECK(!detail::fast_path_eligible(b.view()));
}

void test_cadence() {
    CHECK(first_recompute_due(1000, 3, 1) == 1090);  // K < 2
    CHECK(first_recompute_due(1000, 0, 4) == 1000);
    CHECK(first_recompute_due(1000, 1, 4) == 1023);  // 22.5 + 0.5 -> 23
    CHECK(first_recompute_due(1000, 3, 7) == 1039);  // 38.57 + 0.5 -> 39
    CHECK(first_recompute_due(0, 2, 3) == 60);
    CHECK(stagger_divisor(8, 1) == 7);
    CHECK(recompute_due(1039, 1039) && !recompute_due(1038, 1039) && recompute_due(5, kLazyCreatedNextRecompute));
    CHECK(next_after_service(1039) == 1129);
}

void test_cache() {
    Rng r(99);
    GalaxyParams p;
    p.n = 60;
    GalaxyMap gm = random_galaxy(r, p);
    PlanetGraph g = PlanetGraph::build(gm);
    std::vector<double> cost(p.n, 100.0);  // node cost 1
    Bitset fog(p.n, true);
    AllPairsInput in;
    in.node_value = cost;
    in.revealed = &fog;
    AllPairsCache cache;
    AllPairsMatrices ref;
    CHECK(cache.update(g, in));
    CHECK(cache.last_method() == AllPairsMethod::Reference);  // 60 planets: below the crossover
    CHECK(!cache.update(g, in));
    CHECK(cache.recompute_count() == 1);
    reference_all_pairs(g, in, ref);
    CHECK(cache.matrices() == ref);
    cost[3] = 700.0;
    CHECK(cache.update(g, in));
    CHECK(!cache.update(g, in));
    fog.set(5, false);
    CHECK(cache.update(g, in));
    reference_all_pairs(g, in, ref);
    CHECK(cache.matrices() == ref);
    in.revealed = nullptr;  // human or no AI system: no fog gate
    CHECK(cache.update(g, in));
    std::vector<float> z(p.n, 0.0f);
    in.z_offset = z;
    CHECK(cache.update(g, in));
    CHECK(!cache.update(g, in));
    z[0] = 1.0f;
    CHECK(cache.update(g, in));
    PlanetIndex far = kInvalidPlanet;  // a planet not yet linked to planet 0
    for (PlanetIndex b = 1; b < p.n && far == kInvalidPlanet; ++b) {
        bool linked = false;
        for (uint32_t e = g.edge_begin(0); e < g.edge_end(0); ++e) linked = linked || g.neighbour(e) == b;
        if (!linked) far = b;
    }
    add_route(gm, 0, far);
    const PlanetGraph g2 = PlanetGraph::build(gm);
    CHECK(g2.fingerprint() != g.fingerprint());
    CHECK(cache.update(g2, in));
    CHECK(!cache.update(g2, in));
    cache.invalidate();
    cache.set_fast_min_planets(0);
    CHECK(cache.update(g2, in, ::alamo::test::make_thread_parallel_for(3)));
    CHECK(cache.last_method() == AllPairsMethod::Fast);
    reference_all_pairs(g2, in, ref);
    CHECK(cache.matrices() == ref);
    CHECK(cache.recompute_count() == 8);
}

}  // namespace

int main() {
    test_graph_build();
    test_astar_tie_order();
    test_astar_vs_model_and_dijkstra();
    test_edge_cost_rules();
    test_astar_concurrent_and_wrap();
    test_weight_arithmetic();
    test_fw_semantics();
    test_all_pairs_equivalence();
    test_cadence();
    test_cache();
    if (g_fail) {
        std::printf("alamo_galactic_tests: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("alamo_galactic_tests: all passed\n");
    return 0;
}
