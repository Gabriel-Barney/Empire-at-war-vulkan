// alamo_tactical_movement tests (no framework; non-zero exit on failure).
// S7 movement & pathing (docs/re/MOVEMENT.md): heap tie rules, movement classes, grid + stamping,
// region decomposition vs a cell BFS, land paths on random mazes (validity, reachability,
// near-optimality, determinism), smoothing and clearance, locomotion, formations, collision
// helpers, the collision-object layer (priority sort, crowd refresh, cancellation, budget fallback) and group moves, space occupancy / planner / Hermite playback, the space
// fighter locomotor (FIGHTER_FLYING.md: primitives, cell grids, Moving, squadron orbit, attack run, dogfight lock), determinism across
// 1 / 4 / N threads with golden CRCs, and (ALAMO_FOC_DATA) every shipped land and space map.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include "alamo/core/sim_crc.hpp"
#include "alamo/core/sim_rng.hpp"
#include "alamo/tactical/movement/movement_system.hpp"
#include "alamo/test/support.hpp"
#include "alamo/test/thread_parallel_for.hpp"

#ifdef ALAMO_MOVEMENT_HAVE_TERRAIN
#include <filesystem>
#include <fstream>

#include "alamo/core/file_provider.hpp"
#include "alamo/objects/game_object_types.hpp"
#include "alamo/objects/object_types.hpp"
#include "alamo/tactical/movement/ted_grid.hpp"
#endif
#if defined(ALAMO_MOVEMENT_HAVE_DATA) && defined(ALAMO_MOVEMENT_HAVE_TERRAIN)
#include "alamo/core/hash.hpp"
#include "alamo/objects/game_object_types.hpp"
#include "alamo/objects/object_types.hpp"
#include "alamo/tactical/movement/ted_grid.hpp"
#include "alamo/vfs/vfs.hpp"
#define MOVEMENT_REAL_DATA 1
#endif

using namespace alamo;
using namespace alamo::tactical::movement;

using ::alamo::test::g_fail;

static ParallelFor make_thread_pf(unsigned threads) { return ::alamo::test::make_thread_parallel_for(threads); }

// ---------------------------------------------------------------- helpers
static PassabilityGrid random_maze(uint32_t w, uint32_t h, uint32_t seed, int32_t wall_pct) {
    PassabilityGrid g;
    g.init(w, h);
    SimRng rng(seed);
    std::vector<uint32_t> v(static_cast<size_t>(w) * h, kPassClear);
    // Random blobs of walls / water / steep.
    const uint32_t blobs = w * h * static_cast<uint32_t>(wall_pct) / 400;
    for (uint32_t b = 0; b < blobs; ++b) {
        const int32_t cx = rng.range_int(0, static_cast<int32_t>(w) - 1), cy = rng.range_int(0, static_cast<int32_t>(h) - 1);
        const int32_t r = rng.range_int(0, 2);
        const int32_t kind = rng.range_int(0, 9);
        const uint32_t bits = kind < 6 ? kPassImpassable : (kind < 8 ? kPassWater : kPassSteep);
        for (int32_t y = cy - r; y <= cy + r; ++y)
            for (int32_t x = cx - r; x <= cx + r; ++x)
                if (x >= 0 && y >= 0 && x < static_cast<int32_t>(w) && y < static_cast<int32_t>(h)) v[static_cast<size_t>(y) * w + static_cast<size_t>(x)] = bits;
    }
    g.set_base(v);
    return g;
}

// 4-connected BFS component labels over covered cells (x < w-1, y < h-1) passable for mask.
static std::vector<int32_t> cell_components(const PassabilityGrid& g, uint32_t mask) {
    const int32_t W = static_cast<int32_t>(g.width()), H = static_cast<int32_t>(g.height());
    std::vector<int32_t> c(static_cast<size_t>(W) * static_cast<size_t>(H), -1);
    int32_t next = 0;
    std::deque<std::pair<int32_t, int32_t>> q;
    for (int32_t y = 0; y < H - 1; ++y) {
        for (int32_t x = 0; x < W - 1; ++x) {
            if (c[static_cast<size_t>(y * W + x)] >= 0 || !g.passable_cell(x, y, mask)) continue;
            c[static_cast<size_t>(y * W + x)] = next;
            q.push_back({x, y});
            while (!q.empty()) {
                auto [px, py] = q.front();
                q.pop_front();
                const int32_t dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
                for (int k = 0; k < 4; ++k) {
                    const int32_t nx = px + dx[k], ny = py + dy[k];
                    if (nx < 0 || ny < 0 || nx >= W - 1 || ny >= H - 1) continue;
                    if (c[static_cast<size_t>(ny * W + nx)] >= 0 || !g.passable_cell(nx, ny, mask)) continue;
                    c[static_cast<size_t>(ny * W + nx)] = next;
                    q.push_back({nx, ny});
                }
            }
            ++next;
        }
    }
    return c;
}

// 8-connected (no corner cutting) Dijkstra distance in world units between cell centres.
static float octile_distance(const PassabilityGrid& g, uint32_t mask, int32_t sx, int32_t sy, int32_t tx, int32_t ty) {
    const int32_t W = static_cast<int32_t>(g.width()), H = static_cast<int32_t>(g.height());
    std::vector<float> d(static_cast<size_t>(W) * static_cast<size_t>(H), 3e38f);
    RetailHeap<int32_t> heap;
    d[static_cast<size_t>(sy * W + sx)] = 0.0f;
    heap.push(0.0f, sy * W + sx);
    while (!heap.empty()) {
        auto it = heap.pop();
        const int32_t ci = it.value;
        if (it.f > d[static_cast<size_t>(ci)]) continue;
        const int32_t cx = ci % W, cy = ci / W;
        if (cx == tx && cy == ty) return it.f * kCellSize;
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                if (!dx && !dy) continue;
                const int32_t nx = cx + dx, ny = cy + dy;
                if (!g.passable_cell(nx, ny, mask)) continue;
                if (dx && dy && (!g.passable_cell(cx + dx, cy, mask) || !g.passable_cell(cx, cy + dy, mask))) continue;
                const float nd = it.f + ((dx && dy) ? 1.41421356f : 1.0f);
                if (nd < d[static_cast<size_t>(ny * W + nx)]) {
                    d[static_cast<size_t>(ny * W + nx)] = nd;
                    heap.push(nd, ny * W + nx);
                }
            }
    }
    return -1.0f;
}

static bool path_valid(const PassabilityGrid& g, const std::vector<V2>& p, uint32_t mask, size_t from = 0) {
    for (size_t i = from + 1; i < p.size(); ++i)
        if (!segment_passable(g, p[i - 1], p[i], mask)) return false;
    return true;
}

static UnitDesc land_unit(ObjectId id, V2 pos, int32_t cls, float speed, float radius, int32_t ucc) {
    UnitDesc d;
    d.id = id;
    d.pos = pos;
    d.max_speed = speed;
    d.turn_rate = 10.0f;
    d.movement_class = cls;
    d.collision_class = ucc;
    d.radius = radius;
    d.type_name = "TEST_UNIT";
    return d;
}

// ---------------------------------------------------------------- 1. heap
static void test_heap() {
    RetailHeap<int> h;
    h.push(1.0f, 10);
    h.push(1.0f, 11);
    h.push(1.0f, 12);
    // Root 10; on pop, 12 moves to the root and does not sift (left child 11 is not strictly smaller).
    CHECK(h.pop().value == 10);
    CHECK(h.pop().value == 12);
    CHECK(h.pop().value == 11);
    // A pushed equal key never rises above an existing equal key.
    RetailHeap<int> h2;
    h2.push(2.0f, 1);
    h2.push(1.0f, 2);
    h2.push(1.0f, 3);
    CHECK(h2.pop().value == 2);
    CHECK(h2.pop().value == 3);
    CHECK(h2.pop().value == 1);
    // Right child replaces the left only when strictly smaller.
    RetailHeap<int> h3;
    for (int i = 0; i < 7; ++i) h3.push(static_cast<float>(i / 2), i);
    std::vector<int> order;
    while (!h3.empty()) order.push_back(h3.pop().value);
    CHECK((order == std::vector<int>{0, 1, 3, 2, 5, 4, 6}));
    // Capacity: a push into a full heap is dropped.
    RetailHeap<int> h4(2);
    CHECK(h4.push(1.0f, 1) && h4.push(2.0f, 2) && !h4.push(0.0f, 3));
    CHECK(h4.size() == 2 && h4.pop().value == 1);
    // Random stress: pops are non-decreasing.
    RetailHeap<int> h5;
    SimRng r(7);
    for (int i = 0; i < 2000; ++i) h5.push(static_cast<float>(r.range_int(0, 50)), i);
    float last = -1.0f;
    bool sorted = true;
    while (!h5.empty()) {
        const float f = h5.pop().f;
        sorted = sorted && f >= last;
        last = f;
    }
    CHECK(sorted);
}

// ---------------------------------------------------------------- 2. movement classes
static const char* kVanillaDefs = R"(<?xml version="1.0" ?>
<MovementClassType>
	<Infantry> Clear | Shield | InfantryOnly </Infantry>
	<InfantryNoShield> Clear | InfantryOnly </InfantryNoShield>
	<Wheeled> Clear | Shield </Wheeled>
	<WheeledNoShield> Clear </WheeledNoShield>
	<Walker> Clear | Shield </Walker>
	<WalkerNoShield> Clear </WalkerNoShield>
	<Tracked> Clear | Shield </Tracked>
	<TrackedNoShield> Clear </TrackedNoShield>
	<Flying> Clear | Steep | Water | RebelWall | EmpireWall | Shield | Obstacle | InfantryOnly </Flying>
	<Hover> Clear | Water | Shield </Hover>
	<HoverNoShield> Clear | Water </HoverNoShield>
	<LargeWalker> Clear | RebelWall | Shield </LargeWalker>
	<LargeWalkerNoShield> Clear | RebelWall </LargeWalkerNoShield>
	<Space> Clear | Steep | Obstacle | Water | RebelWall | EmpireWall | Shield | InfantryOnly </Space>
	<Lancet> Clear | Water | RebelWall | EmpireWall | Shield | InfantryOnly | Steep</Lancet>
	<Spotter> Clear | Shield | InfantryOnly | Water </Spotter>
	<LargeHover> Clear | EmpireWall | Shield | Water </LargeHover>
	<BombingRun> Clear | Steep | Obstacle | Water | RebelWall | EmpireWall | Shield | InfantryOnly | Impassable</BombingRun>
</MovementClassType>)";

static void test_classes() {
    MovementClassTable vanilla;
    MovementClassTable loaded;
    for (int32_t c = 0; c < 18; ++c) loaded.set_mask(c, 0);
    CHECK(loaded.load_xml(kVanillaDefs));
    for (int32_t c = 0; c < 18; ++c) CHECK(loaded.mask(c) == vanilla.mask(c));
    CHECK(vanilla.mask(-1) == kPassClear);
    CHECK(vanilla.mask(0) == (kPassClear | kPassShield | kPassInfantryOnly));
    CHECK(vanilla.mask(17) == 0x1FFu);
    CHECK(passable(kPassClear, vanilla.mask(7)));
    CHECK(!passable(kPassWater, vanilla.mask(7)));
    CHECK(passable(kPassWater, vanilla.mask(9)));
    CHECK(!passable(kPassInfantryOnly, vanilla.mask(7)) && passable(kPassInfantryOnly, vanilla.mask(0)));
    CHECK(!passable(kOutOfRangeCell, vanilla.mask(17)));
    uint32_t bits = 0;
    CHECK(!parse_passability_bits("Clear | Bogus", bits));
    CHECK(parse_passability_bits(" clear|WATER ", bits) && bits == (kPassClear | kPassWater));
}

// ---------------------------------------------------------------- 3. grid
static void test_grid() {
    CHECK(world_to_cell(-10.0f) == 0 && world_to_cell(9.99f) == 0 && world_to_cell(10.0f) == 1 && world_to_cell(-10.01f) == -1);
    PassabilityGrid g;
    g.init(32, 32);
    CHECK(g.effective(0, 0) == kPassClear && g.effective(-1, 0) == kOutOfRangeCell && g.effective(32, 0) == kOutOfRangeCell);
    g.set_base(3, 3, kPassWater);
    CHECK(g.effective(3, 3) == kPassWater);
    g.set_base(4, 4, kPassClear | kPassSteep);  // any non-Clear bit clears Clear
    CHECK(g.effective(4, 4) == kPassSteep);
    // Playable box: cell centres strictly outside -> Obstacle.
    g.set_playable_box({40.0f, 40.0f}, {500.0f, 500.0f});
    CHECK(g.effective(1, 5) == kPassObstacle && g.effective(2, 5) == kPassClear && g.effective(25, 5) == kPassClear &&
          g.effective(26, 5) == kPassObstacle);
    g.clear_playable_box();
    // Stamping: a 40 x 20 footprint at (200, 200), yaw 0, offset -half.
    const uint64_t v0 = g.version();
    ObstacleFootprint fp = make_footprint({200.0f, 200.0f}, 0.0f, 40.0f, 20.0f, -20.0f, -10.0f, 0, 0, 1.0f, 0);
    const uint32_t n = g.stamp(77, fp);
    CHECK(n == 6);  // x 180..220 (cells 9, 10, 11) x y 190..210 (cells 10, 11)
    CHECK(g.effective(10, 10) == kPassObstacle && g.effective(9, 10) == kPassObstacle && g.effective(11, 11) == kPassObstacle);
    CHECK(g.effective(12, 10) == kPassClear && g.version() != v0);
    // Overlapping stamp with another bit, then unstamp the first: exact restore.
    ObstacleFootprint fp2 = make_footprint({220.0f, 200.0f}, 90.0f, 20.0f, 20.0f, -10.0f, -10.0f, 0, 0, 1.0f, kPassRebelWall);
    CHECK(g.stamp(78, fp2) > 0);
    CHECK(g.effective(11, 10) == (kPassObstacle | kPassRebelWall));
    CHECK(g.unstamp(77));
    CHECK(g.effective(11, 10) == kPassRebelWall && g.effective(9, 10) == kPassClear);
    CHECK(g.unstamp(78) && !g.unstamp(78));
    CHECK(g.effective(11, 10) == kPassClear);
    // S8 t13 (MOVEMENT §1.3 "Unstamp", corrected): every unstamp recomputes the cell from base | the remaining occupants'
    // bits, in any order, also when the removed occupant shared a bit with a remaining one. Checked against a grid
    // stamped with only the remaining occupants.
    {
        const ObstacleFootprint a = make_footprint({200.0f, 200.0f}, 0.0f, 40.0f, 40.0f, -20.0f, -20.0f, 0, 0, 1.0f,
                                                   kPassObstacle | kPassRebelWall);
        const ObstacleFootprint b = make_footprint({210.0f, 200.0f}, 0.0f, 40.0f, 40.0f, -20.0f, -20.0f, 0, 0, 1.0f, kPassShield);
        const ObstacleFootprint c = make_footprint({190.0f, 210.0f}, 0.0f, 40.0f, 40.0f, -20.0f, -20.0f, 0, 0, 1.0f, kPassObstacle);
        const ObstacleFootprint* fps[3] = {&a, &b, &c};
        const uint32_t orders[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
        bool same = true;
        for (const auto& ord : orders) {
            PassabilityGrid live;
            live.init(32, 32);
            live.set_base(10, 10, kPassSteep);
            for (uint32_t k = 0; k < 3; ++k) live.stamp(static_cast<ObjectId>(k + 1), *fps[k]);
            for (uint32_t removed = 0; removed < 3; ++removed) {
                live.unstamp(static_cast<ObjectId>(ord[removed] + 1));
                PassabilityGrid ref;  // only the occupants still stamped
                ref.init(32, 32);
                ref.set_base(10, 10, kPassSteep);
                for (uint32_t k = removed + 1; k < 3; ++k) ref.stamp(static_cast<ObjectId>(ord[k] + 1), *fps[ord[k]]);
                for (int32_t y = 0; y < 32; ++y)
                    for (int32_t x = 0; x < 32; ++x) same = same && live.effective(x, y) == ref.effective(x, y);
            }
        }
        CHECK(same);
        PassabilityGrid two;  // a shared bit stays while one holder remains; the other's own bit leaves
        two.init(32, 32);
        two.stamp(1, a);
        two.stamp(3, c);
        CHECK(two.effective(10, 10) == (kPassObstacle | kPassRebelWall) && two.unstamp(1) && two.effective(10, 10) == kPassObstacle);
    }
    // OPT3 t11: stamp_all (cells sampled through a ParallelFor, here one that runs the entries back to front) gives the
    // grid and stamps of the stamp() loop, also with a fine-tune ray, a repeated id and an id stamped before.
    {
        std::vector<std::pair<ObjectId, ObstacleFootprint>> list;
        for (uint32_t k = 0; k < 9; ++k) {
            ObstacleFootprint f = make_footprint({60.0f + 25.0f * static_cast<float>(k), 150.0f + 7.0f * static_cast<float>(k)},
                                                 static_cast<float>(k) * 35.0f, 50.0f, 30.0f, -25.0f, -15.0f, 0, 0, 1.0f,
                                                 k % 2 ? kPassRebelWall : 0u);
            if (k % 3 == 0) f.fine_tune = [](V2 p) { return (static_cast<int32_t>(p.x / 10.0f) + static_cast<int32_t>(p.y / 10.0f)) % 2 == 0; };
            list.emplace_back(static_cast<ObjectId>(k == 7 ? 2 : k), std::move(f));
        }
        const ParallelFor backwards = [](uint32_t n, uint32_t, const RangeFn& fn) {
            for (uint32_t i = n; i-- > 0;) fn(i, i + 1);
        };
        PassabilityGrid ser, par;
        for (PassabilityGrid* q : {&ser, &par}) {
            q->init(32, 32);
            q->set_base(8, 9, kPassSteep);
            q->stamp(4, make_footprint({40.0f, 40.0f}, 0.0f, 30.0f, 30.0f, -15.0f, -15.0f, 0, 0, 1.0f, 0));
        }
        std::vector<uint32_t> counts;
        for (const auto& [id, f] : list) counts.push_back(ser.stamp(id, f));
        CHECK(par.stamp_all(list, backwards) == counts);
        bool same = ser.stamped_ids() == par.stamped_ids();
        for (const ObjectId id : ser.stamped_ids()) {
            const auto a = ser.stamp_cells(id), b = par.stamp_cells(id);
            same = same && std::equal(a.begin(), a.end(), b.begin(), b.end());
        }
        const auto ea = ser.effective_values(), eb = par.effective_values();
        CHECK(same && std::equal(ea.begin(), ea.end(), eb.begin(), eb.end()));
        // OPT3 t12: stamp() with a ParallelFor (the fine-tune samples' rays through it, here back to front) stamps the same
        // cells as the serial stamp().
        PassabilityGrid one;
        one.init(32, 32);
        one.set_base(8, 9, kPassSteep);
        one.stamp(4, make_footprint({40.0f, 40.0f}, 0.0f, 30.0f, 30.0f, -15.0f, -15.0f, 0, 0, 1.0f, 0));
        std::vector<uint32_t> counts_pf;
        for (const auto& [id, f] : list) counts_pf.push_back(one.stamp(id, f, backwards));
        bool same_pf = counts_pf == counts && ser.stamped_ids() == one.stamped_ids();
        for (const ObjectId id : ser.stamped_ids()) {
            const auto a = ser.stamp_cells(id), b = one.stamp_cells(id);
            same_pf = same_pf && std::equal(a.begin(), a.end(), b.begin(), b.end());
        }
        const auto ec = one.effective_values();
        CHECK(same_pf && std::equal(ea.begin(), ea.end(), ec.begin(), ec.end()));
    }
    // Rotated footprint: 60 x 10 at yaw 90 covers a vertical strip.
    ObstacleFootprint fp3 = make_footprint({300.0f, 300.0f}, 90.0f, 60.0f, 10.0f, 0.0f, -5.0f, 0, 0, 1.0f, 0);
    g.stamp(79, fp3);
    CHECK(g.effective(15, 15) == kPassObstacle && g.effective(15, 18) == kPassObstacle && g.effective(18, 15) == kPassClear);
    // S8 t01: per-object cell query (0x59efa0) and rekey (map placement stamp -> object id).
    CHECK(g.occupies(79, 15, 15) && g.occupies(79, 15, 18) && !g.occupies(79, 18, 15) && !g.occupies(79, 16, 15));
    CHECK(g.occupies_world(79, {300.0f, 330.0f}) && !g.occupies_world(79, {340.0f, 300.0f}));
    CHECK(!g.occupies(80, 15, 15) && !g.occupies(79, -1, 15) && !g.occupies(79, 15, 99));
    CHECK(g.stamp_cells(79).size() == 4 && g.stamp_cells(80).empty());  // y 300..360: cells 15..18 of column 15
    {
        ObstacleFootprint wall = make_footprint({100.0f, 100.0f}, 0.0f, 40.0f, 20.0f, -20.0f, -10.0f, 0, 0, 1.0f, 0);
        PassabilityGrid a, b;
        a.init(32, 32);
        b.init(32, 32);
        a.stamp(-5, wall);  // map placement 4
        b.stamp(12, wall);  // stamped under the object id directly
        b.stamp(-6, wall);
        a.stamp(-6, wall);
        const uint64_t va = a.version();
        CHECK(a.occupies(-5, 5, 5) && !a.occupies(12, 5, 5));
        CHECK(!a.rekey(-5, -5) && !a.rekey(-9, 13) && !a.rekey(-5, -6));  // same id, unknown id, target taken
        CHECK(a.rekey(-5, 12));
        CHECK(a.occupies(12, 5, 5) && !a.occupies(-5, 5, 5) && !a.stamped(-5) && a.stamped(12) && a.version() == va);
        StateHasher ha, hb;
        a.hash(ha);
        b.hash(hb);
        CHECK(ha.digest() == hb.digest());  // same as stamping under the object id
        CHECK(a.unstamp(12) && a.effective(5, 5) == kPassObstacle && a.unstamp(-6) && a.effective(5, 5) == kPassClear);
    }
    {  // S8 t02: stamped ids ascending (placement ids first); a footprint on no cell is still a stamp (stamped, no cells)
        PassabilityGrid z;
        z.init(16, 16);
        const ObstacleFootprint off = make_footprint({-400.0f, 100.0f}, 0.0f, 20.0f, 20.0f, -10.0f, -10.0f, 0, 0, 1.0f, 0);
        const ObstacleFootprint on = make_footprint({100.0f, 100.0f}, 0.0f, 20.0f, 20.0f, -10.0f, -10.0f, 0, 0, 1.0f, 0);
        const uint64_t zv = z.version();
        CHECK(z.stamp(30, off) == 0 && z.stamped(30) && z.stamp_cells(30).empty() && !z.occupies(30, 0, 5) && z.version() == zv);
        CHECK(z.stamp(4, on) > 0 && z.stamp(-2, on) > 0);
        CHECK((z.stamped_ids() == std::vector<ObjectId>{-2, 4, 30}));
        CHECK(z.unstamp(30) && z.unstamp(4) && z.effective(5, 5) == kPassObstacle && (z.stamped_ids() == std::vector<ObjectId>{-2}));
    }
    {  // S7 t13: space static entries handed from a placement id to the object id
        SpaceOccupancy occ;
        occ.add_static(-4, {10.0f, 20.0f}, 50.0f, kSpaceStatic);
        occ.add_static(9, {500.0f, 0.0f}, 5.0f, kSpaceStatic);
        CHECK(!occ.rekey_static(-4, -4) && !occ.rekey_static(-7, 12) && !occ.rekey_static(-4, 9));  // same, unknown, taken
        CHECK(occ.rekey_static(-4, 12) && !occ.has_static(-4) && occ.has_static(12));
        V2 c{};
        float r = 0.0f;
        uint32_t bits = 0;
        CHECK(occ.static_entry(12, c, r, bits) && c.x == 10.0f && c.y == 20.0f && r == 50.0f && bits == kSpaceStatic);
        CHECK(!occ.static_entry(-4, c, r, bits));
        CHECK((occ.query_static({40.0f, 20.0f}, {40.0f, 20.0f}, 1.0f) & kSpaceStatic) != 0);
        CHECK(occ.remove_static(12) && occ.query_static({40.0f, 20.0f}, {40.0f, 20.0f}, 1.0f) == 0);
    }
    // Water rule: a low object over water does not block.
    PassabilityGrid gw;
    gw.init(16, 16);
    gw.set_base(5, 5, kPassWater);
    gw.set_water_height(10.0f);
    ObstacleFootprint low = make_footprint({100.0f, 100.0f}, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0, 0, 1.0f, 0);
    low.terrain_z = 0.0f;
    low.obstacle_height = 2.0f;
    CHECK(gw.stamp(1, low) == 0 && gw.effective(5, 5) == kPassWater);
    low.obstacle_height = 20.0f;
    CHECK(gw.stamp(1, low) == 1 && gw.effective(5, 5) == (kPassWater | kPassObstacle));
}

// ---------------------------------------------------------------- 4. regions
static void test_regions() {
    const MovementClassTable cls;
    const auto masks = cls.distinct_masks();
    for (uint32_t seed = 1; seed <= 6; ++seed) {
        const uint32_t W = 40 + seed * 7, H = 35 + seed * 5;
        PassabilityGrid g = random_maze(W, H, seed, 30);
        RegionMap rm;
        rm.build(g, masks);
        bool uniform = true, uncovered_ok = true, sym = true, aspect_ok = true;
        for (int32_t y = 0; y < static_cast<int32_t>(H); ++y)
            for (int32_t x = 0; x < static_cast<int32_t>(W); ++x) {
                const int32_t r = rm.region_at(x, y);
                if (x == static_cast<int32_t>(W) - 1 || y == static_cast<int32_t>(H) - 1) {
                    uncovered_ok = uncovered_ok && r == -1;
                    continue;
                }
                if (r < 0 || !rm.region(r).contains(x, y) || rm.region(r).bits != g.effective(x, y)) uniform = false;
            }
        for (uint32_t r = 0; r < rm.region_count(); ++r) {
            const Region& R = rm.region(static_cast<int32_t>(r));
            if (R.w > 2 * R.h || R.h > 2 * R.w) aspect_ok = false;
            if ((R.w == 1 && R.h == 2) || (R.w == 2 && R.h == 1)) aspect_ok = false;
            for (int32_t n : rm.neighbors(static_cast<int32_t>(r))) {
                const auto back = rm.neighbors(n);
                if (std::find(back.begin(), back.end(), static_cast<int32_t>(r)) == back.end()) sym = false;
            }
        }
        CHECK(uniform && uncovered_ok && sym && aspect_ok);
        // Region components == cell components (covered cells), for every mask.
        for (uint32_t mask : masks) {
            const int32_t slot = rm.mask_slot(mask);
            const auto cc = cell_components(g, mask);
            bool same = true;
            std::vector<int32_t> map_a(W * H, -2), map_b(W * H, -2);
            for (int32_t y = 0; y + 1 < static_cast<int32_t>(H); ++y)
                for (int32_t x = 0; x + 1 < static_cast<int32_t>(W); ++x) {
                    const int32_t a = cc[static_cast<size_t>(y) * W + static_cast<size_t>(x)];
                    const int32_t b = rm.component_at(slot, x, y);
                    if ((a < 0) != (b < 0)) {
                        same = false;
                        continue;
                    }
                    if (a < 0) continue;
                    if (map_a[static_cast<size_t>(a)] == -2) map_a[static_cast<size_t>(a)] = b;
                    if (map_b[static_cast<size_t>(b)] == -2) map_b[static_cast<size_t>(b)] = a;
                    if (map_a[static_cast<size_t>(a)] != b || map_b[static_cast<size_t>(b)] != a) same = false;
                }
            CHECK(same);
        }
        // Clearance: 0 on blocked cells, >= 10 next to one.
        const int32_t slot = rm.mask_slot(cls.mask(7));
        bool clear_ok = true;
        for (int32_t y = 0; y < static_cast<int32_t>(H); ++y)
            for (int32_t x = 0; x < static_cast<int32_t>(W); ++x) {
                const float c = rm.clearance(slot, x, y);
                if (!g.passable_cell(x, y, cls.mask(7)) ? c != 0.0f : c < 10.0f) clear_ok = false;
            }
        CHECK(clear_ok);
    }
}

// ---------------------------------------------------------------- 5. land paths
static void test_land_paths() {
    const MovementClassTable cls;
    const auto masks = cls.distinct_masks();
    uint32_t total = 0, reachable = 0, ok_paths = 0, valid = 0, near_opt = 0, unreachable_fail = 0, deterministic = 0;
    double ratio_sum = 0.0;
    float worst = 0.0f;
    for (uint32_t seed = 11; seed <= 16; ++seed) {
        const uint32_t W = 64, H = 64;
        PassabilityGrid g = random_maze(W, H, seed, 12);
        RegionMap rm;
        rm.build(g, masks);
        LandPathScratch sc;
        SimRng r(seed * 31);
        const uint32_t mask = cls.mask(seed % 2 ? 7 : 0);
        const auto cc = cell_components(g, mask);
        for (int k = 0; k < 100; ++k) {
            const int32_t sx = r.range_int(0, static_cast<int32_t>(W) - 2), sy = r.range_int(0, static_cast<int32_t>(H) - 2);
            const int32_t tx = r.range_int(0, static_cast<int32_t>(W) - 2), ty = r.range_int(0, static_cast<int32_t>(H) - 2);
            if (!g.passable_cell(sx, sy, mask) || !g.passable_cell(tx, ty, mask)) continue;
            ++total;
            const V2 s = cell_center(sx, sy) + V2{3.0f, -4.0f}, t = cell_center(tx, ty) + V2{-2.0f, 5.0f};
            const bool reach = cc[static_cast<size_t>(sy) * W + static_cast<size_t>(sx)] == cc[static_cast<size_t>(ty) * W + static_cast<size_t>(tx)];
            LandPathRequest q;
            q.start = s;
            q.goal = t;
            q.mask = mask;
            q.snap_goal = false;
            LandPath p;
            const bool ok = find_land_path(g, rm, q, sc, p);
            if (!reach) {
                if (!ok) ++unreachable_fail;
                continue;
            }
            ++reachable;
            if (!ok) continue;
            ++ok_paths;
            if (path_valid(g, p.points, mask) && p.points.front() == s && p.points.back() == t) {
                ++valid;
            } else {
                std::printf("invalid path seed %u (%zu points):", seed, p.points.size());
                for (size_t i = 0; i < p.points.size(); ++i) {
                    const bool segok = i == 0 || segment_passable(g, p.points[i - 1], p.points[i], mask);
                    std::printf(" %s(%.3f,%.3f)", segok ? "" : "!", static_cast<double>(p.points[i].x), static_cast<double>(p.points[i].y));
                }
                std::printf("\n");
            }
            const float ref = octile_distance(g, mask, sx, sy, tx, ty);
            const float len = path_length(p.points);
            if (ref > 0.0f) {
                const float ratio = len / (ref + kCellSize);
                ratio_sum += ratio;
                worst = std::max(worst, ratio);
                if (ratio <= 1.35f) ++near_opt;
            } else {
                ++near_opt;
            }
            LandPath p2;
            find_land_path(g, rm, q, sc, p2);
            if (p2.points == p.points) ++deterministic;
        }
    }
    std::printf("land paths: %u queries, %u reachable, %u found, %u valid, %u near-optimal (mean ratio %.3f, worst %.3f), "
                "%u unreachable rejected, %u deterministic\n",
                total, reachable, ok_paths, valid, near_opt, reachable ? ratio_sum / reachable : 0.0, static_cast<double>(worst),
                unreachable_fail, deterministic);
    CHECK(total > 100 && ok_paths == reachable && valid == ok_paths && deterministic == ok_paths);
    CHECK(unreachable_fail + reachable == total);
    CHECK(near_opt * 100 >= ok_paths * 95);

    // Smoothing: open field -> straight line; L corridor -> one corner.
    PassabilityGrid open;
    open.init(64, 64);
    RegionMap rmo;
    rmo.build(open, masks);
    LandPathScratch sc;
    LandPathRequest q;
    q.start = {25.0f, 33.0f};
    q.goal = {1100.0f, 900.0f};
    q.mask = cls.mask(7);
    LandPath p;
    CHECK(find_land_path(open, rmo, q, sc, p) && p.points.size() == 2);
    PassabilityGrid corr;
    corr.init(64, 64);
    std::vector<uint32_t> v(64 * 64, kPassImpassable);
    for (size_t x = 2; x < 50; ++x) v[5 * 64 + x] = kPassClear;   // horizontal corridor y = 5
    for (size_t y = 5; y < 50; ++y) v[y * 64 + 49] = kPassClear;  // vertical corridor x = 49
    corr.set_base(v);
    RegionMap rmc;
    rmc.build(corr, masks);
    q.start = cell_center(3, 5);
    q.goal = cell_center(49, 45);
    CHECK(find_land_path(corr, rmc, q, sc, p));
    if (!(path_valid(corr, p.points, q.mask) && p.points.size() == 3)) {
        std::printf("corridor path (%zu, valid %d):", p.points.size(), path_valid(corr, p.points, q.mask) ? 1 : 0);
        for (V2 pt : p.points) std::printf(" (%.1f,%.1f)", static_cast<double>(pt.x), static_cast<double>(pt.y));
        std::printf("\n");
    }
    CHECK(path_valid(corr, p.points, q.mask) && p.points.size() == 3);
    // Unsmoothed region path is longer in vertices.
    q.smooth = false;
    LandPath raw;
    CHECK(find_land_path(corr, rmc, q, sc, raw) && raw.points.size() >= p.points.size() && path_valid(corr, raw.points, q.mask));
    // Cell A* variant.
    q.smooth = true;
    q.use_cell_astar = true;
    LandPath pc;
    CHECK(find_land_path(corr, rmc, q, sc, pc) && path_valid(corr, pc.points, q.mask) && pc.points.size() == 3);
    // Goal snapping: a blocked goal snaps to the nearest reachable cell.
    q.use_cell_astar = false;
    q.goal = cell_center(55, 30);
    CHECK(find_land_path(corr, rmc, q, sc, p) && path_valid(corr, p.points, q.mask));
    CHECK(world_to_cell(p.goal_used.x) == 49 && world_to_cell(p.goal_used.y) == 30);

    // Clearance: a 1-cell gap in a wall (short) vs a wide detour.
    PassabilityGrid cg;
    cg.init(80, 80);
    std::vector<uint32_t> cv(80 * 80, kPassClear);
    for (int y = 0; y < 70; ++y) cv[static_cast<size_t>(y) * 80 + 40] = kPassImpassable;  // wall x = 40, open above y = 70
    cv[static_cast<size_t>(20) * 80 + 40] = kPassClear;                                   // 1-cell gap at y = 20
    cg.set_base(cv);
    RegionMap rmg;
    rmg.build(cg, masks);
    LandPathRequest qc;
    qc.start = cell_center(30, 20);
    qc.goal = cell_center(50, 20);
    qc.mask = cls.mask(7);
    LandPath small;
    CHECK(find_land_path(cg, rmg, qc, sc, small) && path_length(small.points) < 450.0f);
    qc.clearance_radius = 30.0f;
    qc.goal = cell_center(50, 60);  // detour length within 1.4x
    qc.start = cell_center(30, 60);
    LandPath big;
    CHECK(find_land_path(cg, rmg, qc, sc, big) && path_valid(cg, big.points, qc.mask));
    CHECK(big.clearance_replanned);
    bool kept_clear = true;
    const int32_t slot = rmg.mask_slot(qc.mask);
    for (size_t i = 1; i < big.points.size(); ++i) {
        walk_cells(big.points[i - 1], big.points[i], [&](int32_t x, int32_t y) {
            if (rmg.clearance(slot, x, y) < 30.0f && std::abs(y - 60) > 2) kept_clear = false;
            return true;
        });
    }
    CHECK(kept_clear);
    std::printf("clearance: small-unit path %.0f, large-unit path %.0f (%zu points)\n", static_cast<double>(path_length(small.points)),
                static_cast<double>(path_length(big.points)), big.points.size());
}

// ---------------------------------------------------------------- 6. locomotion
static void test_locomotion() {
    // Unit kinematics.
    LandMotion m;
    m.pos = {0.0f, 0.0f};
    m.path = {{0.0f, 0.0f}, {100.0f, 0.0f}, {100.0f, 100.0f}};
    m.next = 1;
    LocomotorParams p;
    p.max_speed = 4.0f;
    p.accel = 0.5f;
    p.decel = 1.0f;
    p.turn_rate = 15.0f;
    p.mask = kPassClear;
    float prev_speed = 0.0f;
    bool accel_ok = true, arrived = false;
    int frames = 0;
    for (; frames < 400 && !arrived; ++frames) {
        const WalkResult r = walk_step(m, p, 3.0e38f, nullptr);
        if (m.speed > prev_speed + p.accel + 1e-5f || m.speed > p.max_speed + 1e-5f) accel_ok = false;
        prev_speed = m.speed;
        arrived = r.arrived;
    }
    CHECK(arrived && accel_ok && m.pos == (V2{100.0f, 100.0f}) && m.speed == 0.0f);
    CHECK(frames > 50 && frames < 80);  // 200 units at <= 4 / frame with accel ramp
    // Passability check before commit: a blocked cell stops the unit.
    PassabilityGrid g;
    g.init(16, 16);
    g.set_base(5, 0, kPassImpassable);
    LandMotion b;
    b.pos = {40.0f, 0.0f};
    b.path = {{40.0f, 0.0f}, {200.0f, 0.0f}};
    b.next = 1;
    bool blocked = false;
    for (int i = 0; i < 60 && !blocked; ++i) blocked = walk_step(b, p, 3.0e38f, &g).blocked;
    CHECK(blocked && b.pos.x < 90.0f && g.passable_world(b.pos, kPassClear));
    // Speed cap 0 holds the unit.
    LandMotion c = m;
    c.path = {{100.0f, 100.0f}, {300.0f, 100.0f}};
    c.next = 1;
    c.state = LocoState::WalkStopped;
    walk_step(c, p, 0.0f, nullptr);
    CHECK(c.pos == (V2{100.0f, 100.0f}));
}

// ---------------------------------------------------------------- 7. formations
static void test_formations() {
    MovementConstants k;
    std::vector<FormationMember> m;
    // Two clusters of 30 big units each, far apart.
    for (int i = 0; i < 60; ++i) {
        FormationMember f;
        f.id = 100 + i;
        f.pos = (i % 2 ? V2{5000.0f, 5000.0f} : V2{0.0f, 0.0f}) + V2{static_cast<float>(i % 7) * 30.0f, static_cast<float>(i / 7) * 30.0f};
        f.radius = 30.0f;
        f.max_speed = 2.0f;
        m.push_back(f);
    }
    const auto buckets = kmeans_buckets(m, k.max_formation_area);
    bool pure = true;
    for (const auto& b : buckets) {
        const bool far = m[b[0]].pos.x > 2000.0f;
        for (uint32_t i : b) pure = pure && ((m[i].pos.x > 2000.0f) == far);
    }
    CHECK(buckets.size() >= 2 && pure);

    // Rank / rows: hero first, slowest in front, left/right order kept.
    std::vector<FormationMember> s;
    const float speeds[8] = {3.0f, 1.0f, 2.0f, 1.0f, 3.0f, 2.0f, 2.5f, 1.0f};
    for (int i = 0; i < 8; ++i) {
        FormationMember f;
        f.id = 10 + i;
        f.pos = {static_cast<float>(i) * 40.0f, 0.0f};  // laterally spread along +x
        f.max_speed = speeds[i];
        f.radius = 8.0f;
        f.spacing = 1.0f;
        f.type_name = i % 2 ? "Unit_B" : "unit_a";
        s.push_back(f);
    }
    s[4].hero = true;
    std::vector<uint32_t> all{0, 1, 2, 3, 4, 5, 6, 7};
    const auto ranked = rank_members(s, all);
    CHECK(ranked.front().member == 4);  // hero first
    // 4 distinct non-hero speeds (1, 2, 2.5, 3) -> tolerance grows to 15: one rank behind the hero.
    bool one_rank = true;
    for (size_t i = 1; i < ranked.size(); ++i) one_rank = one_rank && ranked[i].rank == 1;
    CHECK(one_rank);
    const V2 fwd{0.0f, 1.0f};  // moving +y: right = +x
    const auto slots = layout_formation(s, all, fwd, k);
    CHECK(slots.size() == 8 && slots[0].member == 4 && slots[0].row == 0);
    // Lateral order within each row follows the units' current x (left/right order kept).
    bool order_kept = true;
    for (size_t i = 1; i < slots.size(); ++i)
        if (slots[i].row == slots[i - 1].row && !(s[slots[i].member].pos.x > s[slots[i - 1].member].pos.x && slots[i].lateral > slots[i - 1].lateral))
            order_kept = false;
    CHECK(order_kept);
    // Distinct slots.
    bool distinct = true;
    for (size_t i = 0; i < slots.size(); ++i)
        for (size_t j = i + 1; j < slots.size(); ++j)
            if (std::fabs(slots[i].lateral - slots[j].lateral) < 1.0f && std::fabs(slots[i].back - slots[j].back) < 1.0f) distinct = false;
    CHECK(distinct);
    // With fewer distinct speeds (no tolerance growth) the slowest rank is in front.
    for (int i = 0; i < 8; ++i) s[static_cast<size_t>(i)].max_speed = i < 4 ? 2.0f : 1.0f;
    s[4].hero = false;
    const auto r2 = rank_members(s, all);
    CHECK(s[r2.front().member].max_speed == 1.0f && r2.front().rank == 0 && r2.back().rank == 1);
    // Equal speed ties: case-insensitive type name, then id.
    CHECK(s[r2[0].member].type_name == "unit_a" || s[r2[0].member].type_name == "Unit_B");
    // Recruitment: a Solo seed stays alone; far units are not recruited.
    std::vector<FormationMember> rec(4);
    for (int i = 0; i < 4; ++i) {
        rec[static_cast<size_t>(i)].id = i;
        rec[static_cast<size_t>(i)].pos = {static_cast<float>(i) * 50.0f, 0.0f};
        rec[static_cast<size_t>(i)].collision_class = 7;
    }
    rec[3].pos = {2000.0f, 0.0f};
    std::vector<uint32_t> b{0, 1, 2, 3};
    const auto forms = split_into_formations(rec, b, k);
    CHECK(forms.size() == 2);
    rec[1].grouping = kGroupingSolo;
    CHECK(split_into_formations(rec, b, k).size() >= 3);
    CHECK(formation_side_correction(0.05f) == 0.0f && formation_side_correction(3.1f) == 0.1f && formation_side_correction(-3.1f) == -0.1f);
}

// ---------------------------------------------------------------- 7b. double-click move (P1_DOUBLE_CLICK_MOVE.md §4)
static void test_double_click() {
    CHECK(kMoveFlagAttackMove == 0x01 && kMoveFlagGuardMove == 0x02 && kMoveFlagPlayerOrder == 0x04 && kMoveFlagDoubleClick == 0x80);
    MovementConstants k;
    CHECK(k.double_click_move_max_speed_ratio == 1.25f);
    // Recruitment test: symmetric ratio against the seed; zero-speed candidates are not tested.
    std::vector<FormationMember> m(5);
    const float sp[5] = {2.0f, 2.5f, 2.6f, 1.5f, 0.0f};  // seed 2.0: 2.5 ok (1.25), 2.6 no, 1.5 no (1.333), 0 ok
    for (size_t i = 0; i < 5; ++i) {
        m[i].id = static_cast<ObjectId>(i);
        m[i].max_speed = sp[i];
        m[i].collision_class = 7;
    }
    const float xs[5] = {0.0f, -10.0f, 10.0f, -20.0f, 20.0f};  // centroid 0: member 0 seeds
    for (size_t i = 0; i < 5; ++i) m[i].pos = {xs[i], 0.0f};
    const std::vector<uint32_t> b{0, 1, 2, 3, 4};
    CHECK(split_into_formations(m, b, k).size() == 1);  // no speed test without 0x80
    const auto f = split_into_formations(m, b, k, {}, k.double_click_move_max_speed_ratio);
    // {0, 1, 4}; then 2 seeds (nearest the centroid) and rejects 3 (2.6 / 1.5 > 1.25).
    CHECK(f.size() == 3 && (f[0] == std::vector<uint32_t>{0, 1, 4}) && (f[1] == std::vector<uint32_t>{2}));
    // Ordering of LargeHover formations.
    const std::vector<FormationKey> keys{{1.0f, {0.0f, 100.0f}}, {3.0f, {0.0f, 0.0f}}, {2.0f, {0.0f, 50.0f}}};
    CHECK((large_hover_formation_order(keys, {0.0f, 1.0f}, false) == std::vector<uint32_t>{0, 2, 1}));
    CHECK((large_hover_formation_order(keys, {0.0f, 1.0f}, true) == std::vector<uint32_t>{1, 2, 0}));

    // System: 3 fast (speeder-like, 3.0) + 3 slow (walker-like, 1.0) vehicles side by side on an open grid.
    const auto run = [](uint8_t flags, int32_t cls, std::vector<UnitView>& out) {
        MovementSystem sys{MovementConfig{}};
        PassabilityGrid g;
        g.init(80, 80);
        sys.set_grid(std::move(g));
        for (uint32_t i = 0; i < 6; ++i)
            CHECK(sys.add_unit(land_unit(static_cast<ObjectId>(1 + i), cell_center(10 + static_cast<int32_t>(i) * 2, 10), cls,
                                         i % 2 ? 1.0f : 3.0f, 12.0f, kUccVehicle)));
        SimRng rng(77);
        MoveOrder o;
        o.units = sys.unit_ids();
        o.destination = cell_center(10, 60);
        o.flags = flags;
        sys.move(o, rng, 0);
        out.clear();
        for (ObjectId id : sys.unit_ids()) out.push_back(sys.view(id));
    };
    std::vector<UnitView> plain, dbl;
    run(kMoveFlagPlayerOrder, 7, plain);
    run(kMoveFlagPlayerOrder | kMoveFlagDoubleClick, 7, dbl);
    bool one = true, bands = true;
    float slow = 3.0e38f, fast = 0.0f;  // effective speeds (land mode multiplier applied)
    for (const UnitView& v : plain) {
        slow = std::min(slow, v.max_speed);
        fast = std::max(fast, v.max_speed);
    }
    CHECK(fast > slow * k.double_click_move_max_speed_ratio);
    for (const UnitView& v : plain) one = one && v.formation_speed == slow;  // one formation at the slowest speed
    for (const UnitView& v : dbl) bands = bands && v.formation_speed == v.max_speed;  // fast and slow bands
    CHECK(one && bands);
    // LargeHover: without 0x80 one formation (index 0); with 0x80 the fast band gets index 0.
    run(kMoveFlagPlayerOrder, static_cast<int32_t>(MovementClass::LargeHover), plain);
    run(kMoveFlagPlayerOrder | kMoveFlagDoubleClick, static_cast<int32_t>(MovementClass::LargeHover), dbl);
    bool idx_plain = true, idx_dbl = true;
    float fast_h = 0.0f;
    for (const UnitView& v : dbl) fast_h = std::max(fast_h, v.max_speed);
    for (const UnitView& v : plain) idx_plain = idx_plain && v.large_hover_index == 0;
    for (const UnitView& v : dbl) idx_dbl = idx_dbl && v.large_hover_index == (v.max_speed == fast_h ? 0 : 1);
    CHECK(idx_plain && idx_dbl);
    // Formations of other movement classes get no LargeHover index.
    std::vector<UnitView> none;
    run(kMoveFlagPlayerOrder | kMoveFlagDoubleClick, 0, none);
    bool no_hover = true;
    for (const UnitView& v : none) no_hover = no_hover && v.large_hover_index == -1;
    CHECK(no_hover);
}

// ---------------------------------------------------------------- 8. collision helpers
static void test_collision_helpers() {
    CHECK(collision_classes_interact(kUccInfantry, kUccVehicle));
    CHECK(!collision_classes_interact(kUccLowObstacle, kUccVehicle) && collision_classes_interact(kUccLowObstacle, kUccInfantry));
    CHECK(!collision_classes_interact(kUccMediumObstacle, kUccGiantVehicle) && collision_classes_interact(kUccHighObstacle, kUccGiantVehicle));
    float t = 0.0f;
    CHECK(predict_moving_contact({0, 0}, {2, 0}, {100, 0}, {-2, 0}, 10.0f, 45.0f, t) && std::fabs(t - 22.5f) < 1e-3f);
    CHECK(!predict_moving_contact({0, 0}, {2, 0}, {100, 50}, {-2, 0}, 10.0f, 45.0f, t));
    CHECK(!predict_moving_contact({0, 0}, {-2, 0}, {100, 0}, {2, 0}, 10.0f, 45.0f, t));
    const std::vector<V2> path{{0, 0}, {100, 0}, {100, 100}};
    const float d = path_contact_distance({0, 0}, path, 1, {100, 50}, 10.0f, 1000.0f);
    CHECK(std::fabs(d - 140.0f) < 1e-3f);
    CHECK(path_contact_distance({0, 0}, path, 1, {300, 300}, 10.0f, 1000.0f) < 0.0f);
    CHECK(overlap_ratio({0, 0}, 10, {30, 0}, 10) == 0.0f && std::fabs(overlap_ratio({0, 0}, 10, {10, 0}, 10) - 0.5f) < 1e-6f);
    {  // s78n t18 (MOVEMENT 5.3.1): footprint rectangles and the three contact tests
        const auto nearf = [](float x, float y, float eps = 1e-4f) { return std::fabs(x - y) <= eps; };
        const FootRect r1 = foot_rect({0.0f, 0.0f}, 0.0f, 10.0f, 5.0f);
        // R(rect, n) = |a . n| hx + |b . n| hy
        CHECK(nearf(foot_extent(r1, {1.0f, 0.0f}), 10.0f) && nearf(foot_extent(r1, {0.0f, 1.0f}), 5.0f));
        const FootRect r2 = foot_rect({12.0f, 0.0f}, 0.0f, 10.0f, 5.0f);
        // overlap coefficient: x axis 1 - 12 / 20, y axis 1 -> 0.4; gap 0
        CHECK(foot_overlap(r1, r2) && nearf(foot_overlap_coefficient(r1, r2), 0.4f) && foot_gap(r1, r2) == 0.0f);
        const FootRect r3 = foot_rect({25.0f, 0.0f}, 0.0f, 10.0f, 5.0f);
        CHECK(!foot_overlap(r1, r3) && foot_overlap_coefficient(r1, r3) == 0.0f && nearf(foot_gap(r1, r3), 5.0f));
        const FootRect touch = foot_rect({20.0f, 0.0f}, 0.0f, 10.0f, 5.0f);  // touching counts as overlap; coefficient 0
        CHECK(foot_overlap(r1, touch) && foot_overlap_coefficient(r1, touch) == 0.0f && foot_gap(r1, touch) == 0.0f);
        const FootRect r4 = foot_rect({12.0f, 0.0f}, 90.0f, 10.0f, 5.0f);  // turned: its x extent is 5 -> 1 - 12 / 15
        CHECK(nearf(foot_overlap_coefficient(r1, r4), 0.2f, 1e-3f));
        CHECK(nearf(foot_overlap_coefficient(r1, r1), 1.0f));
        // directional radius rho = hy + (hx - hy) cos^2
        CHECK(foot_rho(10.0f, 5.0f, 1.0f) == 10.0f && foot_rho(10.0f, 5.0f, 0.0f) == 5.0f &&
              nearf(foot_rho(10.0f, 5.0f, 0.70710678f), 7.5f));
        // the swept point against a radius running linearly r0 -> r1
        CHECK(std::fabs(foot_swept_contact({-100.0f, 0.0f}, {100.0f, 0.0f}, 18.0f, 18.0f) - 0.82) < 1e-6);
        CHECK(foot_swept_contact({-10.0f, 0.0f}, {100.0f, 0.0f}, 18.0f, 18.0f) == 0.0);       // already inside
        CHECK(foot_swept_contact({-100.0f, 30.0f}, {200.0f, 0.0f}, 18.0f, 18.0f) < 0.0);      // passes 30 to the side
        CHECK(std::fabs(foot_swept_contact({-100.0f, 0.0f}, {0.0f, 0.0f}, 0.0f, 200.0f) - 0.5) < 1e-6);  // the radius grows
        // MovingVsStopped: a (10, 5) mover along +x; head-on against an (8, 4) box end-on: hx_a + hx_b = 18 -> 82;
        // against its side (the box turned 90 degrees): hx_a + hy_b = 14 -> 86
        const std::vector<V2> px{{0.0f, 0.0f}, {200.0f, 0.0f}};
        const V2 ha{10.0f, 5.0f};
        CHECK(nearf(foot_path_contact({0.0f, 0.0f}, px, 1, ha, foot_rect({100.0f, 0.0f}, 0.0f, 8.0f, 4.0f), 1000.0f), 82.0f, 1e-2f));
        CHECK(nearf(foot_path_contact({0.0f, 0.0f}, px, 1, ha, foot_rect({100.0f, 0.0f}, 90.0f, 8.0f, 4.0f), 1000.0f), 86.0f, 1e-2f));
        CHECK(foot_path_contact({0.0f, 0.0f}, px, 1, ha, foot_rect({100.0f, 0.0f}, 0.0f, 8.0f, 4.0f), 80.0f) < 0.0f);  // beyond the look-ahead
        // beside the path: hy_a + hy_b = 9 abeam, a little more ahead of abeam (the cos^2 blend): 8.5 touches, 10.5 not
        CHECK(foot_path_contact({0.0f, 0.0f}, px, 1, ha, foot_rect({100.0f, 8.5f}, 0.0f, 8.0f, 4.0f), 1000.0f) >= 0.0f);
        CHECK(foot_path_contact({0.0f, 0.0f}, px, 1, ha, foot_rect({100.0f, 10.5f}, 0.0f, 8.0f, 4.0f), 1000.0f) < 0.0f);
        // turning in place: one zero-length segment at the mover's position
        CHECK(foot_path_contact({0.0f, 0.0f}, px, 1, ha, foot_rect({17.0f, 0.0f}, 0.0f, 8.0f, 4.0f), 1000.0f, 0.0f) == 0.0f);
        CHECK(foot_path_contact({0.0f, 0.0f}, px, 1, ha, foot_rect({19.0f, 0.0f}, 0.0f, 8.0f, 4.0f), 1000.0f, 0.0f) < 0.0f);
        // MovingVsMoving: two (5, 5) movers head-on at speed 2 each, 100 apart: touch at 10 after 22.5 frames (< 45);
        // 300 apart: not inside 45 frames, inside 90
        const std::vector<V2> pp{{0.0f, 0.0f}, {400.0f, 0.0f}};
        const auto track = [](V2 pos, float yaw, const std::vector<V2>& path, float hx, float hy) {
            FootTrack t;
            t.pos = pos;
            t.yaw = yaw;
            t.path = path;
            t.next = 1;
            t.speed = 2.0f;
            t.max_speed = 2.0f;
            t.hx = hx;
            t.hy = hy;
            return t;
        };
        const std::vector<V2> q1{{100.0f, 0.0f}, {-300.0f, 0.0f}}, q2{{300.0f, 0.0f}, {-100.0f, 0.0f}};
        CHECK(foot_moving_contact(track({0.0f, 0.0f}, 0.0f, pp, 5.0f, 5.0f), track({100.0f, 0.0f}, 180.0f, q1, 5.0f, 5.0f), 45));
        CHECK(!foot_moving_contact(track({0.0f, 0.0f}, 0.0f, pp, 5.0f, 5.0f), track({300.0f, 0.0f}, 180.0f, q2, 5.0f, 5.0f), 45));
        CHECK(foot_moving_contact(track({0.0f, 0.0f}, 0.0f, pp, 5.0f, 5.0f), track({300.0f, 0.0f}, 180.0f, q2, 5.0f, 5.0f), 90));
        // lane rule: (10, 5) movers passing on parallel lines 12 apart keep their side radius hy (5 + 5 < 12): no contact;
        // 9 apart they touch
        const std::vector<V2> l12{{100.0f, 12.0f}, {-300.0f, 12.0f}}, l9{{100.0f, 9.0f}, {-300.0f, 9.0f}};
        CHECK(!foot_moving_contact(track({0.0f, 0.0f}, 0.0f, pp, 10.0f, 5.0f), track({100.0f, 12.0f}, 180.0f, l12, 10.0f, 5.0f), 45));
        CHECK(foot_moving_contact(track({0.0f, 0.0f}, 0.0f, pp, 10.0f, 5.0f), track({100.0f, 9.0f}, 180.0f, l9, 10.0f, 5.0f), 45));
        // neither moves: the rectangle overlap
        FootTrack s1 = track({0.0f, 0.0f}, 0.0f, pp, 10.0f, 5.0f), s2 = track({12.0f, 0.0f}, 0.0f, pp, 10.0f, 5.0f);
        s1.next = s2.next = 2;
        CHECK(foot_moving_contact(s1, s2, 45));
        s2.pos = {25.0f, 0.0f};
        CHECK(!foot_moving_contact(s1, s2, 45));
    }
    // (the dynamic local pathfinder: test_path_around)
    // Crush victims.
    const std::vector<Box2> crushers{{{0, 0}, {10, 10}}};
    const std::vector<Box2> victims{{{20, 20}, {30, 30}}, {{5, 5}, {6, 6}}, {{9, -5}, {12, 0}}};
    const auto hit = find_crush_victims(crushers, victims);
    CHECK((hit == std::vector<uint32_t>{1, 2}));
}

// ---------------------------------------------------------------- S7 collision layer (TACTICAL_GAPS §6.7)
static void test_collision_layer() {
    // Pair filter: unordered, one collision per pair.
    {
        CollisionLayer L;
        CHECK(L.add(CollisionKind::MovingVsStopped, 3, 7, 0) == 0);
        CHECK(L.has_pair(7, 3) && L.has_pair(3, 7) && !L.has_pair(3, 8));
        CHECK(L.add(CollisionKind::MovingVsMoving, 7, 3, 1) == UINT32_MAX && L.list().size() == 1);
    }
    const auto keep_all = [](CollisionObject&) { return true; };
    const auto all_quota = [](const CollisionObject&) { return true; };  // every record takes the quota pass
    const auto full_budget = [] { return true; };
    const auto no_resolve = [](CollisionObject&, bool) {};
    // Priority order: ascending crowd - age, ties in insertion order -> older and less crowded first.
    {
        CollisionLayer L;
        // crowd counts per pair (keyed by participant a)
        const auto count = [](const CollisionObject& c) -> uint32_t { return c.a == 3 ? 4u : 2u; };
        L.add(CollisionKind::MovingVsMoving, 1, 2, 5);    // seq 0: crowd 10, created 5
        L.add(CollisionKind::MovingVsMoving, 3, 4, 0);    // seq 1: crowd 20, created 0
        L.add(CollisionKind::MovingVsStopped, 5, 6, 0);   // seq 2: crowd 10, created 0
        L.add(CollisionKind::StoppedVsStopped, 7, 8, 0);  // seq 3: crowd 10, created 0 (ties with seq 2)
        L.run(10, keep_all, all_quota, count, full_budget, no_resolve);
        std::vector<uint32_t> order;
        for (const CollisionLayer::Step& s : L.last_order()) order.push_back(s.seq);
        // priorities: seq0 10-5 = 5, seq1 20-10 = 10, seq2 10-10 = 0, seq3 0
        CHECK((order == std::vector<uint32_t>{2, 3, 0, 1}));
        CHECK(L.list()[0].priority == 5.0f && L.list()[1].priority == 10.0f && L.list()[2].priority == 0.0f);
        CHECK(collision_priority(10.0f, 10, 5) == 5.0f && collision_priority(0.0f, 7, 7) == 0.0f);
        // The list itself stays in insertion order.
        CHECK(L.list()[0].seq == 0 && L.list()[3].seq == 3);
    }
    // Crowd refresh: on the first run, then every 30 frames (now >= refresh frame); the age keeps counting.
    {
        CollisionLayer L;
        uint32_t calls = 0, objects = 2;
        const auto count = [&](const CollisionObject&) {
            ++calls;
            return objects;
        };
        L.add(CollisionKind::StoppedVsStopped, 1, 2, 100);
        bool ok = true;
        for (uint32_t f = 100; f <= 161; ++f) {
            objects = 2 + (f - 100);  // the box keeps filling up; only a refresh sees it
            L.run(f, keep_all, all_quota, count, full_budget, no_resolve);
            const CollisionObject& c = L.list()[0];
            const uint32_t last_refresh = 100 + ((f - 100) / 30) * 30;
            ok = ok && c.crowd == static_cast<float>(5 * (2 + (last_refresh - 100))) && c.crowd_frame == last_refresh + 30 &&
                 c.priority == c.crowd - static_cast<float>(f - 100);
        }
        CHECK(ok && calls == 3);  // frames 100, 130, 160
        CHECK(L.list()[0].crowd == 5.0f * 62.0f);
    }
    // Cancellation: collisions whose keep test fails leave in list order (pair freed); Resolved ones leave after the pass.
    {
        CollisionLayer L;
        for (ObjectId i = 0; i < 4; ++i) L.add(CollisionKind::MovingVsStopped, 10 + i, 20 + i, 0);
        const auto count = [](const CollisionObject&) { return 2u; };
        std::vector<uint32_t> kept_calls;
        const auto keep = [&](CollisionObject& c) {
            kept_calls.push_back(c.seq);
            return c.seq != 1;
        };
        const auto resolve = [](CollisionObject& c, bool) {
            if (c.seq == 3) c.state = CollisionState::Resolved;
        };
        L.run(4, keep, all_quota, count, full_budget, resolve);
        CHECK((kept_calls == std::vector<uint32_t>{0, 1, 2, 3}));  // keep tests run in list order
        CHECK(L.last_cancelled() == 1 && L.last_resolved() == 1 && L.list().size() == 2);
        CHECK(L.list()[0].seq == 0 && L.list()[1].seq == 2);
        CHECK(!L.has_pair(11, 21) && !L.has_pair(13, 23) && L.has_pair(12, 22));
        CHECK(L.last_order().size() == 3);  // the cancelled one is not resolved
        // A freed pair can be found again; it is a new collision (new seq, new creation frame).
        CHECK(L.add(CollisionKind::MovingVsMoving, 21, 11, 9) == 4 && L.list().back().created == 9);
    }
    // Budget: full resolution while the quota allows, in priority order; the rest fall back.
    {
        CollisionLayer L;
        uint32_t quota = 0;
        const auto budget = [&] { return quota < 100; };
        std::vector<std::pair<uint32_t, bool>> seen;
        const auto resolve = [&](CollisionObject& c, bool full) {
            seen.push_back({c.seq, full});
            if (full) quota += 40;  // each full search adds its complexity
        };
        // seq i created at frame 10 - 2i: the higher seqs are older, so the priority order is 4, 3, 2, 1, 0.
        for (uint32_t i = 0; i < 5; ++i) L.add(CollisionKind::MovingVsStopped, static_cast<ObjectId>(i), 100, 10 - 2 * i);
        L.run(20, keep_all, all_quota, [](const CollisionObject&) { return 2u; }, budget, resolve);
        CHECK((seen == std::vector<std::pair<uint32_t, bool>>{{4, true}, {3, true}, {2, true}, {1, false}, {0, false}}));
        bool same = L.last_order().size() == seen.size();
        for (size_t i = 0; same && i < seen.size(); ++i)
            same = L.last_order()[i].seq == seen[i].first && L.last_order()[i].full == seen[i].second;
        CHECK(same);
    }
    // s78n t20 (MOVEMENT 5.4.1 "Per-frame driver"): the records that do not take the quota pass (vf +0x50 false: a decided
    // MovingVsMoving, every StoppedVsStopped) are serviced first, in creation order and whatever their priority, each with
    // `full`; the others follow in priority order under the budget. The crowd / priority of a pass-1 record is not touched.
    {
        CollisionLayer L;
        L.add(CollisionKind::MovingVsStopped, 1, 2, 0);    // seq 0: undecided, old (priority 10 - 50)
        L.add(CollisionKind::MovingVsMoving, 3, 4, 49);    // seq 1: decided below, young
        L.add(CollisionKind::MovingVsStopped, 5, 6, 40);   // seq 2
        L.add(CollisionKind::StoppedVsStopped, 7, 8, 50);  // seq 3: never in the quota pass
        L.add(CollisionKind::MovingVsMoving, 9, 10, 45);   // seq 4: undecided -> quota pass
        const auto quota = [](const CollisionObject& c) {
            return c.kind == CollisionKind::MovingVsStopped || (c.kind == CollisionKind::MovingVsMoving && c.state == CollisionState::Active);
        };
        L.run(49, keep_all, quota, [](const CollisionObject&) { return 2u; }, full_budget, [](CollisionObject& c, bool) {
            if (c.seq == 1) c.state = CollisionState::SpeedSlave;  // the MovingVsMoving record is decided
        });
        uint32_t budget_calls = 0;
        std::vector<std::pair<uint32_t, bool>> seen;
        L.run(50, keep_all, quota, [](const CollisionObject&) { return 2u; }, [&] { return ++budget_calls <= 1; },
              [&](CollisionObject& c, bool full) { seen.push_back({c.seq, full}); });
        // pass 1: seq 1 (decided MvM), seq 3 (SvS), creation order, full; pass 2 by priority: seq 0 (10 - 50), seq 2 (10 -
        // 10), seq 4 (10 - 5); the budget lets only the first decide
        CHECK((seen == std::vector<std::pair<uint32_t, bool>>{{1, true}, {3, true}, {0, true}, {2, false}, {4, false}}));
        CHECK(budget_calls == 3);  // asked only for the quota pass
        CHECK(L.list()[3].crowd == 0.0f && L.list()[3].crowd_frame == 50);  // the SvS record: no crowd refresh, no priority
    }
    // Hash: covers the state.
    {
        CollisionLayer L1, L2;
        L1.add(CollisionKind::MovingVsStopped, 1, 2, 0);
        L2.add(CollisionKind::MovingVsStopped, 1, 2, 0);
        StateHasher h1, h2;
        L1.hash(h1);
        L2.hash(h2);
        CHECK(h1.digest() == h2.digest());
        L2.run(1, keep_all, all_quota, [](const CollisionObject&) { return 2u; }, full_budget, [](CollisionObject& c, bool) {
            c.state = CollisionState::TryAgainLater;
            c.retry = 5;
        });
        StateHasher h3;
        L2.hash(h3);
        CHECK(h1.digest() != h3.digest());
    }
}

// s78n t20 (MOVEMENT 1.5.1, 5.4.2, 5.4.3; path_around.hpp): the relation code, the swept box, the ring search and its
// occupancy test, the rejoin candidates and the dynamic pathfinder's visibility-graph search.
static void test_path_around() {
    // ---- relation code 0x679750(p0, p1)
    {
        RelationInput in;
        in.h0 = {1.0f, 0.0f};
        in.h1 = {1.0f, 0.0f};
        in.v0 = 1.0f;
        in.v1 = 2.0f;
        in.rel = {50.0f, 0.0f};
        in.hw1 = 6.0f;
        CHECK(relation_code(in) == kRelStanding);  // p0 does not move
        in.p0_moving = true;
        in.p0_cap = 0.0f;
        CHECK(relation_code(in) == kRelHeld);  // held at speed 0 by its records
        in.p0_cap = 3.0e38f;
        CHECK(relation_code(in) == kRelSameWaySlower);  // within 60 degrees, p0 slower
        in.v0 = 2.0f;
        CHECK(relation_code(in) == kRelSameWay);        // not slower (equal counts)
        in.h1 = dir_from_deg(59.0f);
        CHECK(relation_code(in) == kRelSameWay);
        in.h1 = dir_from_deg(61.0f);
        CHECK(relation_code(in) == kRelCrossing);  // past 60 degrees with p1 ahead: crossing
        in.h1 = dir_from_deg(119.0f);
        CHECK(relation_code(in) == kRelCrossing);
        in.h1 = dir_from_deg(121.0f);
        CHECK(relation_code(in) == kRelHeadOn);  // within 60 degrees of opposed
        in.rel = {5.9f, 40.0f};  // p1 less than its half width ahead of p0 along h0
        CHECK(relation_code(in) == kRelSameWay);
        in.own_record = true;  // ... unless the pair's own record is an undecided / detouring MovingVsMoving
        CHECK(relation_code(in) == kRelHeadOn);
        in.rel = {6.0f, 40.0f};
        in.own_record = false;
        CHECK(relation_code(in) == kRelHeadOn);
    }
    // ---- swept box 0x677650
    {
        SweptInput in;
        in.pos = {500.0f, 400.0f};
        in.hx = 10.0f;
        in.hy = 6.0f;
        FootRect r = swept_box(in);
        CHECK(r.c == in.pos && r.hx == 11.0f && r.hy == 7.0f && r.a == (V2{1.0f, 0.0f}));
        in.turning = true;
        in.wanted = {0.0f, 1.0f};
        r = swept_box(in);
        CHECK(r.hx == 11.0f && r.hy == 11.0f && r.a == (V2{0.0f, 1.0f}));  // a square of the larger extent + 1
        in.turning = false;
        const std::vector<V2> straight{{500.0f, 400.0f}, {600.0f, 400.0f}};
        in.poly = straight;
        in.sweep = 30.0f;
        r = swept_box(in);
        // rear centre = 1 + hx behind its position, front centre = chord + hx + 1 ahead (MOVEMENT 5.4.6 "Pass hold lines")
        CHECK(std::fabs(r.c.x - r.hx - (500.0f - 11.0f)) < 1e-3f && std::fabs(r.c.x + r.hx - (500.0f + 30.0f + 11.0f)) < 1e-3f && r.hy == 7.0f);
        const std::vector<V2> bend{{500.0f, 400.0f}, {510.0f, 400.0f}, {510.0f, 420.0f}};
        in.poly = bend;
        r = swept_box(in);  // 10 along +x, 20 along +y: the chord (10, 20); the corner (510, 400) lies 8.94 beside it
        const float chord = std::sqrt(500.0f);
        CHECK(std::fabs(r.hx - (11.0f + 0.5f * chord)) < 1e-3f && std::fabs(r.hy - (7.0f + 200.0f / chord)) < 1e-3f);
        CHECK(std::fabs(r.a.x - 10.0f / chord) < 1e-4f && std::fabs(r.a.y - 20.0f / chord) < 1e-4f);
        V2 exit;
        const FootRect box{{500.0f, 400.0f}, {1.0f, 0.0f}, 11.0f, 7.0f, false};
        CHECK(ray_leaves_rect(box, {440.0f, 400.0f}, {1.0f, 0.0f}, exit) && std::fabs(exit.x - 511.0f) < 1e-4f && exit.y == 400.0f);
        CHECK(!ray_leaves_rect(box, {440.0f, 400.0f}, {-1.0f, 0.0f}, exit) && !ray_leaves_rect(box, {440.0f, 420.0f}, {1.0f, 0.0f}, exit));
        // s78n t20 (MOVEMENT §1.5.1 item 4a, 0x6b1d00): a closed separating-axis test, touching counts
        CHECK(segment_meets_rect(box, {480.0f, 400.0f}, {520.0f, 400.0f}));
        CHECK(segment_meets_rect(box, {489.0f, 393.0f}, {511.0f, 393.0f}));  // along its edge
        CHECK(segment_meets_rect(box, {480.0f, 400.0f}, {489.0f, 400.0f}));  // ending on its side
        CHECK(segment_meets_rect(box, {505.0f, 401.0f}, {505.0f, 401.0f}));  // a point inside
        CHECK(!segment_meets_rect(box, {480.0f, 400.0f}, {488.0f, 400.0f}));
        CHECK(!segment_meets_rect(box, {480.0f, 380.0f}, {530.0f, 392.0f}));  // passes below the corner (511, 393)
        CHECK(segment_meets_rect(box, {501.0f, 383.0f}, {521.0f, 403.0f}));   // through the corner (511, 393) exactly
    }
    // ---- ring search 0x440f90: rings 0, 1, 2.8, 5.4, 8.8, 13, 18 (step 1, + 0.8 per ring, capped at min(1.5 r, 10)); n =
    // trunc(2 pi R / r) + 1 samples, at least 4 after ring 0; counter-clockwise from the search direction; a sample past
    // the cone sits at the outside radius
    {
        const float r = std::sqrt(136.0f);  // footprint radius of (10, 6)
        RingSetup s;
        s.centre = {100.0f, 100.0f};
        s.dir = {0.0f, 1.0f};
        s.r = r;
        std::vector<std::vector<V2>> rings;
        V2 out;
        CHECK(!ring_search(s, [&](V2 p, uint32_t ring) {
            if (rings.size() <= ring) rings.resize(ring + 1);
            rings[ring].push_back(p);
            return false;
        }, out));
        CHECK(rings.size() == 20 && rings[0].size() == 1 && rings[0][0] == s.centre);
        float want = 0.0f, step = 1.0f;
        bool radii = true, counts = true;
        for (uint32_t k = 0; k < 20; ++k) {
            radii = radii && std::fabs(distance(rings[k][0], s.centre) - want) < 1e-3f;
            const uint32_t n = k == 0 ? 1u : std::max(4u, static_cast<uint32_t>(2.0f * 3.1415927f * want / r) + 1u);
            counts = counts && rings[k].size() == n;
            want += step;
            step = std::min(step + 0.8f, 10.0f);
        }
        CHECK(radii && counts);
        CHECK(std::fabs(distance(rings[5][0], s.centre) - 13.0f) < 1e-3f && std::fabs(distance(rings[6][0], s.centre) - 18.0f) < 1e-3f);
        // sample 0 lies along the direction (+y), sample 1 counter-clockwise from it (toward -x)
        CHECK(std::fabs(rings[6][0].x - 100.0f) < 1e-3f && rings[6][0].y > 100.0f && rings[6][1].x < 100.0f);
        // a cone of 3 pi / 4 with the outside radius 0.75 (StoppedVsStopped, not Infantry)
        s.cone = 2.3561945f;
        s.outside = 0.75f;
        std::vector<V2> ring6;
        (void)ring_search(s, [&](V2 p, uint32_t ring) {
            if (ring == 6) ring6.push_back(p);
            return false;
        }, out);
        bool cone = ring6.size() == rings[6].size();
        for (size_t i = 0; cone && i < ring6.size(); ++i) {
            const float th = 2.0f * 3.1415927f * static_cast<float>(i) / static_cast<float>(ring6.size());
            cone = std::fabs(distance(ring6[i], s.centre) - (th <= s.cone ? 18.0f : 0.75f)) < 1e-3f;
        }
        CHECK(cone);
        // Occupancy (MOVEMENT 5.4.3): a record blocks a sample when its centre is within (r_X + margin) + rho of the
        // sample swept one unit along world +x; rho = 10 seen end-on, 6 seen from the side
        const FootRect rec{{0.0f, 0.0f}, {1.0f, 0.0f}, 10.0f, 6.0f, false};
        CHECK(ring_blocked({21.6f, 0.0f}, r, rec) && !ring_blocked({22.7f, 0.0f}, r, rec));  // 11.66 + 10 = 21.66
        CHECK(ring_blocked({0.0f, 17.6f}, r, rec) && !ring_blocked({0.0f, 17.7f}, r, rec));  // 11.66 + 6 = 17.66
        CHECK(ring_blocked({-22.6f, 0.0f}, r, rec) && !ring_blocked({-22.7f, 0.0f}, r, rec));  // the +x sweep: 1 wider on that side
    }
    // ---- s78n t20 batch 1: the path record cut 0x54e780(points, keep) - keep measured from the first point
    {
        const std::vector<V2> line{{0.0f, 0.0f}, {10.0f, 0.0f}, {20.0f, 0.0f}};
        std::vector<V2> p = line;
        CHECK(cut_points(p, 15.0f) && p.size() == 3 && p[1] == (V2{10.0f, 0.0f}) && p[2] == (V2{15.0f, 0.0f}));
        p = line;
        CHECK(!cut_points(p, 10.0f) && p.size() == 2 && p[1] == (V2{10.0f, 0.0f}));  // on a point: none added
        p = line;
        CHECK(!cut_points(p, 0.0f) && p.size() == 1 && p[0] == (V2{0.0f, 0.0f}));  // keep <= 0: a one-node path
        p = line;
        CHECK(!cut_points(p, -4.0f) && p.size() == 1);
        p = line;
        CHECK(!cut_points(p, 20.0f) && p == line);  // keep >= the total: unchanged
        p = line;
        CHECK(!cut_points(p, 25.0f) && p == line);
        p = {{0.0f, 0.0f}, {0.0f, 0.0f}, {10.0f, 0.0f}};  // a zero-length segment is skipped
        CHECK(cut_points(p, 4.0f) && p.size() == 3 && p[2] == (V2{4.0f, 0.0f}));
        p = {{3.0f, 4.0f}};
        CHECK(!cut_points(p, -1.0f) && p.size() == 1);  // fewer than 2 points: left alone
    }
    // ---- rejoin candidates along the path (0x67ad20 a)
    {
        const std::vector<V2> line{{0.0f, 0.0f}, {300.0f, 0.0f}};
        std::vector<RejoinPoint> pts;
        const float d = 71.0f + std::sqrt(136.0f);  // 82.66
        rejoin_on_path(line, 0, {50.0f, 3.0f}, d, 11, pts);
        CHECK(pts.size() == 11);  // 134.66, 144.66, ... (17 below 300, 11 tried)
        bool ok = pts.size() == 11;
        for (size_t i = 0; ok && i < pts.size(); ++i)
            ok = std::fabs(pts[i].p.x - (50.0f + d + 2.0f + 10.0f * static_cast<float>(i))) < 1e-3f && pts[i].p.y == 0.0f && pts[i].next == 1;
        CHECK(ok);
        rejoin_on_path(line, 0, {250.0f, 0.0f}, 30.0f, 11, pts);
        CHECK(pts.size() == 2 && std::fabs(pts[0].p.x - 282.0f) < 1e-3f && std::fabs(pts[1].p.x - 292.0f) < 1e-3f);  // < the length
        rejoin_on_path(line, 0, {280.0f, 0.0f}, 30.0f, 11, pts);
        CHECK(pts.empty());
        rejoin_on_path(line, 1, {280.0f, 0.0f}, 5.0f, 11, pts);  // on the last waypoint: none
        CHECK(pts.empty());
        const std::vector<V2> corner{{0.0f, 0.0f}, {100.0f, 0.0f}, {100.0f, 200.0f}};
        rejoin_on_path(corner, 0, {50.0f, 0.0f}, 30.0f, 3, pts);  // 82, 92 on the first segment, 102 on the second
        CHECK(pts.size() == 3 && pts[0].next == 1 && pts[1].next == 1 && pts[2].next == 2);
        CHECK(std::fabs(pts[2].p.x - 100.0f) < 1e-3f && std::fabs(pts[2].p.y - 2.0f) < 1e-3f);
        rejoin_on_path(corner, 1, {100.0f, 50.0f}, 30.0f, 2, pts);  // arcs run from the current waypoint
        CHECK(pts.size() == 2 && pts[0].next == 2 && std::fabs(pts[0].p.y - 82.0f) < 1e-3f);
    }
    // ---- window 0x67a0d0
    {
        CellBox w = around_window({200.0f, 200.0f}, {600.0f, 200.0f}, 3, false, 100, 100);  // cells (10, 10) -> (30, 10)
        CHECK(w.x0 == 7 && w.x1 == 33 && w.y0 == 4 && w.y1 == 16);  // 27 x 7: ratio 3.86 -> k = 3 on both y sides
        w = around_window({200.0f, 200.0f}, {600.0f, 200.0f}, 3, true, 100, 100);  // Giant: + 2
        CHECK(w.x0 == 5 && w.x1 == 35 && w.y0 == 3 && w.y1 == 17);  // 31 x 11: ratio 2.82 -> k = 2
        w = around_window({200.0f, 200.0f}, {200.0f, 600.0f}, 3, false, 100, 100);
        CHECK(w.x0 == 4 && w.x1 == 13 && w.y0 == 7 && w.y1 == 36);  // 7 x 27: k = 3 on x0 and on y1 (as coded)
        w = around_window({20.0f, 20.0f}, {60.0f, 60.0f}, 3, false, 100, 100);
        CHECK(w.x0 == 0 && w.y0 == 0 && w.x1 == 6 && w.y1 == 6);  // clamped to the grid
    }
    // ---- the search: one standing obstacle (10, 6) on the mover's line
    MovementConstants k;
    {
        PassabilityGrid g;
        g.init(64, 64);
        const float r_u = std::sqrt(136.0f);
        // the rejoin distance of the call: the travel ray leaves the obstacle's swept box (11, 7) at x = 511
        const V2 start{440.0f, 400.0f}, goal{440.0f + (511.0f - 440.0f) + r_u + 2.0f, 400.0f};
        CHECK(std::fabs(goal.x - 524.6619f) < 1e-3f);
        SweptInput si;
        si.pos = {500.0f, 400.0f};
        si.hx = 10.0f;
        si.hy = 6.0f;
        const std::vector<AroundObstacle> obs{{77, swept_box(si), 10.0f, false}};
        AroundRequest q;
        q.start = start;
        q.goal = goal;
        q.grow = 6.0f + 0.5f;     // the mover's half width + 0.5: rect1 half extents (17.5, 13.5)
        q.tracker = 6.0f + 0.5f;  // min(10, 6) + 0.5
        q.mover_hx = 10.0f;
        q.obstacles = obs;
        AroundResult res;
        CHECK(around_search(g, k, q, res));
        CHECK(res.vertices == 6 && res.complexity == 6 && !res.crossing && res.window.size() == 1);
        // around the -b side: the two side routes tie and the heap rules take the one expanded first
        CHECK(res.points.size() == 4 && res.points[0] == start && res.points[3] == goal);
        if (res.points.size() == 4) {
            CHECK(res.points[1] == (V2{482.5f, 386.5f}) && res.points[2] == (V2{517.5f, 386.5f}));
            const float want = std::sqrt(42.5f * 42.5f + 13.5f * 13.5f) + 35.0f +
                               std::sqrt((goal.x - 517.5f) * (goal.x - 517.5f) + 13.5f * 13.5f);
            CHECK(std::fabs(res.cost - want) < 1e-3f && std::fabs(want - 94.87f) < 0.01f);
        }
        // the front corner (+a -b) carries the obstacle's id and hold line: corner - (hx_o + hx_u) a, direction a
        CHECK(res.tags.size() == 1);
        if (res.tags.size() == 1)
            CHECK(res.tags[0].index == 2 && res.tags[0].obstacle == 77 && res.tags[0].pt == (V2{497.5f, 386.5f}) && res.tags[0].dir == (V2{1.0f, 0.0f}));
        // no obstacle: start and goal see each other
        q.obstacles = {};
        CHECK(around_search(g, k, q, res) && res.points.size() == 2 && res.vertices == 2 && res.complexity == 2);
        // a goal inside the obstacle's grown rectangle: refused (the caller moves on to the next candidate)
        q.obstacles = obs;
        q.goal = {515.0f, 400.0f};
        CHECK(!around_search(g, k, q, res));
        // a start inside it: the search starts at the nearest corner; the output begins with the real start and the foot
        // of the start on the first segment
        q.start = {484.0f, 388.0f};
        q.goal = goal;
        CHECK(around_search(g, k, q, res));
        CHECK(res.points.size() >= 3 && res.points.front() == q.start && res.points.back() == goal);
        if (res.points.size() >= 3) CHECK(std::fabs(res.points[1].x - 484.0f) < 1e-3f && std::fabs(res.points[1].y - 386.5f) < 1e-3f);
    }
    // ---- s78n t20 (MOVEMENT §1.5.1 item 4a, its worked example moved to (500, 400)): one standing obstacle, rect1 = rect2
    // = tracker = [-10, 10] x [-5, 5], a = +x; corners k0 (+a +b) = (10, 5), k1 (+a -b), k2 (-a -b), k3 (-a +b) = (-10, 5);
    // S = (20, 0), G = (-30, 0). No edge from a corner into its inward 44-degree cone (d . n >= 0.71934, n = the unit
    // vector of (-sa a - sb b)); crossing = touching the tracker rectangle of an obstacle that is neither end's own.
    {
        PassabilityGrid g;
        g.init(64, 64);
        const V2 o{500.0f, 400.0f};
        std::vector<AroundObstacle> obs{{3, FootRect{o, {1.0f, 0.0f}, 10.0f, 5.0f, false}, 10.0f, false}};
        AroundRequest q;
        q.start = o + V2{20.0f, 0.0f};
        q.goal = o + V2{-30.0f, 0.0f};
        q.grow = 0.0f;
        q.tracker = 0.0f;
        q.obstacles = obs;
        const AroundVertexRef s{-1, 0, q.start}, gl{-1, 0, q.goal}, k0{0, 0, {}}, k1{0, 1, {}}, k2{0, 2, {}}, k3{0, 3, {}};
        CHECK(around_edge_class(g, q, s, k0) == 2);   // d . n = -0.316: an edge at x 1
        CHECK(around_edge_class(g, q, s, k3) == 1);   // 0.814: none in either pass (the segment runs through the rectangle)
        CHECK(around_edge_class(g, q, k0, gl) == 1);  // 0.789: none
        CHECK(around_edge_class(g, q, k3, gl) == 2);  // -0.514
        CHECK(around_edge_class(g, q, k0, k3) == 2);  // a side of one rectangle: no cone test, its own rectangle is free
        CHECK(around_edge_class(g, q, k0, k2) == 1);  // a diagonal
        CHECK(around_edge_class(g, q, s, gl) == 3);   // two obstacle-less vertices: no cone, the tracker rectangle is met
        const AroundVertexRef s2{-1, 0, o + V2{20.0f, 10.0f}};
        CHECK(around_edge_class(g, q, s2, k0) == 2);  // d . n = -0.949 (straight along the outward diagonal)
        AroundResult res;
        CHECK(around_search(g, k, q, res) && !res.crossing && res.points.size() == 4);
        if (res.points.size() == 4) {
            CHECK(res.points[0] == q.start && res.points[3] == q.goal && res.points[1].x == 510.0f && res.points[2].x == 490.0f);
            CHECK(std::fabs(res.points[1].y - 400.0f) == 5.0f && res.points[2].y == res.points[1].y);
        }
        CHECK(std::fabs(res.cost - (std::sqrt(125.0f) + 20.0f + std::sqrt(425.0f))) < 1e-3f && std::fabs(res.cost - 51.80f) < 0.01f);
        // a third obstacle B whose tracker rectangle touches the side k0 - k3 (y = 405): that side crosses it; the far
        // side k1 - k2 does not
        obs.push_back({4, FootRect{o + V2{0.0f, 10.0f}, {1.0f, 0.0f}, 3.0f, 5.0f, false}, 3.0f, false});
        q.obstacles = obs;
        CHECK(around_edge_class(g, q, k0, k3) == 3 && around_edge_class(g, q, k1, k2) == 2);
        // ... and the front side (k0 - k1) of an obstacle in relation 2 / 4 / 8 / 0x20 costs 1 whatever it meets
        obs[1].shape.c = o + V2{12.0f, 0.0f};
        q.obstacles = obs;
        CHECK(around_edge_class(g, q, k0, k1) == 3);
        obs[0].front_free = true;
        q.obstacles = obs;
        CHECK(around_edge_class(g, q, k0, k1) == 2);
    }
    // ---- pass 2: a wall with a gap that a standing obstacle's grown rectangle covers - no way around it, so the second
    // pass crosses it at x DynamicObstacleOverlapPenalty on the crossing segment only
    {
        PassabilityGrid g;
        g.init(64, 64);
        std::vector<uint32_t> cells(64 * 64, kPassClear);
        for (int y = 0; y < 64; ++y)
            if (y < 19 || y > 21) cells[static_cast<size_t>(y) * 64 + 25] = kPassImpassable;  // x = 500, gap y 370 .. 430
        g.set_base(cells);
        SweptInput si;
        si.pos = {500.0f, 400.0f};
        si.hx = 10.0f;
        si.hy = 26.0f;  // rect1 (17.5, 33.5): wider than the gap
        const std::vector<AroundObstacle> obs{{5, swept_box(si), 10.0f, false}};
        AroundRequest q;
        q.start = {440.0f, 400.0f};
        q.goal = {560.0f, 400.0f};
        q.grow = 6.5f;
        q.tracker = 6.5f;
        q.obstacles = obs;
        AroundResult res;
        CHECK(around_search(g, k, q, res));
        CHECK(res.crossing && res.complexity == 2 * res.vertices && res.vertices >= 6);
        const FootRect rect1 = rect_grown(obs[0].shape, q.grow);
        uint32_t crossing = 0;
        float want = 0.0f;
        for (size_t i = 1; i < res.points.size(); ++i) {
            const bool x = segment_meets_rect(rect1, res.points[i - 1], res.points[i]);
            crossing += x ? 1u : 0u;
            want += distance(res.points[i - 1], res.points[i]) * (x ? k.dynamic_obstacle_overlap_penalty : 1.0f);
        }
        CHECK(crossing == 1 && std::fabs(res.cost - want) < 1e-2f);
        // cheaper than crossing in one straight segment (15 x 120): only the stretch across the rectangle is paid for
        CHECK(res.cost < 15.0f * 120.0f && res.points.size() == 4);
        std::printf("path around: pass 2 through a covered gap - %zu points, cost %.2f, %u vertices\n", res.points.size(),
                    static_cast<double>(res.cost), res.vertices);
    }
    // ---- s78n t20 F3 [port]: a start inside a long obstacle's grown rectangle (67.5, 13.5) with a wall cell (22, 19)
    // beside it: the search starts at a vertex the start sees, and every leg of the output (the start's first leg and
    // the foot included) passes the grid
    {
        PassabilityGrid g;
        g.init(64, 64);
        std::vector<uint32_t> cells(64 * 64, kPassClear);
        cells[static_cast<size_t>(19) * 64 + 22] = kPassImpassable;
        g.set_base(cells);
        SweptInput si;
        si.pos = {500.0f, 400.0f};
        si.hx = 60.0f;
        si.hy = 6.0f;
        const std::vector<AroundObstacle> obs{{9, swept_box(si), 60.0f, false}};
        AroundRequest q;
        q.start = {465.0f, 390.0f};
        q.goal = {380.0f, 400.0f};
        q.grow = 6.5f;
        q.tracker = 6.5f;
        q.mover_hx = 10.0f;
        q.obstacles = obs;
        AroundResult res;
        const bool found = around_search(g, k, q, res);
        bool legs = res.points.size() >= 2;
        for (size_t i = 1; i < res.points.size(); ++i) legs = legs && line_passable(g, res.points[i - 1], res.points[i], q.mask);
        CHECK(found && legs && res.points.front() == q.start && res.points.back() == q.goal);
        std::printf("path around (s78n t20 F3): a start inside, a wall cell beside it - %zu points, first leg to "
                    "(%.1f, %.1f)\n", res.points.size(), res.points.size() >= 2 ? static_cast<double>(res.points[1].x) : 0.0,
                    res.points.size() >= 2 ? static_cast<double>(res.points[1].y) : 0.0);
    }
    // ---- a goal walled in: five windows without a feasible flood fill
    {
        PassabilityGrid g;
        g.init(64, 64);
        std::vector<uint32_t> cells(64 * 64, kPassClear);
        for (int y = 17; y <= 23; ++y)
            for (int x = 27; x <= 33; ++x)
                if (x == 27 || x == 33 || y == 17 || y == 23) cells[static_cast<size_t>(y) * 64 + static_cast<size_t>(x)] = kPassImpassable;
        g.set_base(cells);
        AroundRequest q;
        q.start = {440.0f, 400.0f};
        q.goal = {600.0f, 400.0f};  // cell (30, 20), inside the ring of walls
        AroundResult res;
        CHECK(!around_search(g, k, q, res) && res.complexity == 0 && res.points.empty());
    }
}

// s78n t20 (MOVEMENT 4.3.1 item 4, 0x43d6a0): the try offsets of one formation's destination check, against the spec's own
// procedure - t = 0 at first; tries with k mod 8 < 4 use t and then t += 7; tries 4 .. 7 use t - 28 plus min(HX + S, HY + S)
// on the four world axes; the order's first formation, until impassable ground was met, halves the v term and gives it
// the factor +1 (even k) / -0.75 (odd k).
static void test_destination_offsets() {
    const V2 v{-1.0f, 0.0f};  // one formation: back along the final direction +x
    const float m = 20.0f;
    const V2 axes[4] = {{1.0f, 0.0f}, {0.0f, 1.0f}, {-1.0f, 0.0f}, {0.0f, -1.0f}};
    for (int mode = 0; mode < 2; ++mode) {
        const bool first = mode == 0;
        float t = 0.0f;
        bool ok = true;
        for (uint32_t k = 0; k < 40; ++k) {
            const float half = first ? 0.5f * ((k % 2u) == 0u ? 1.0f : -0.75f) : 1.0f;
            V2 want;
            if (k % 8u < 4u) {
                want = v * (t * half);
                t += 7.0f;
            } else {
                want = v * ((t - 28.0f) * half) + axes[k % 8u - 4u] * m;
            }
            const V2 got = destination_try_offset(k, first, v, m);
            ok = ok && std::fabs(got.x - want.x) < 1e-4f && std::fabs(got.y - want.y) < 1e-4f;
        }
        CHECK(ok);
    }
    // the list of the spec: 0, 2.625 beyond, 7 short, 7.875 beyond, four sideways, 14 short, 13.125 beyond, 21 short,
    // 18.375 beyond (v points back, so "short" is +v and "beyond" is -v)
    const float along[12] = {0.0f, 2.625f, -7.0f, 7.875f, 0.0f, 0.0f, 0.0f, 0.0f, -14.0f, 13.125f, -21.0f, 18.375f};
    for (uint32_t k = 0; k < 12; ++k) CHECK(std::fabs(destination_try_offset(k, true, v, m).x - (k % 8u == 4u ? m : k % 8u == 6u ? -m : along[k])) < 1e-4f);
    const V2 k12 = destination_try_offset(12, true, v, m), k13 = destination_try_offset(13, true, v, m), k22 = destination_try_offset(22, true, v, m);
    CHECK(std::fabs(k12.x - (-14.0f + m)) < 1e-4f && k12.y == 0.0f);          // 14 short + m east
    CHECK(std::fabs(k13.x - 10.5f) < 1e-4f && std::fabs(k13.y - m) < 1e-4f);  // 10.5 beyond + m north
    CHECK(std::fabs(k22.x - (-28.0f - m)) < 1e-4f && k22.y == 0.0f);          // 28 short + m west
    // not the first formation: t v and (t - 28) v as they are
    CHECK(std::fabs(destination_try_offset(1, false, v, m).x + 7.0f) < 1e-4f && std::fabs(destination_try_offset(3, false, v, m).x + 21.0f) < 1e-4f);
    CHECK(std::fabs(destination_try_offset(13, false, v, m).x + 28.0f) < 1e-4f);
}

// s78n t20 (MOVEMENT 3.5 step 2, 1.5.1 item 5): the walk step's "at once" collision cap and the record on top of a
// started path (no braking for its end, the speed kept when its last waypoint is passed).
static void test_walk_step_t20() {
    LocomotorParams p;
    p.max_speed = 1.0f;
    p.accel = 0.1f;
    p.decel = 0.1f;
    p.turn_rate = 10.0f;
    const auto walker = [] {
        LandMotion m;
        m.pos = {0.0f, 0.0f};
        m.path = {{0.0f, 0.0f}, {400.0f, 0.0f}};
        m.next = 1;
        m.speed = 1.0f;
        m.state = LocoState::WalkMoving;
        return m;
    };
    {  // the cap 0.2 is taken at the deceleration without the flag, in one frame with it
        LandMotion a = walker(), b = walker();
        WalkExtras now;
        now.cap_now = true;
        walk_step(a, p, 0.2f, nullptr, nullptr);
        walk_step(b, p, 0.2f, nullptr, &now);
        CHECK(std::fabs(a.speed - 0.9f) < 1e-6f && b.speed == 0.2f);
        // the flag never raises the speed: a higher target is still reached at the acceleration
        b.speed = 0.1f;
        walk_step(b, p, 0.6f, nullptr, &now);
        CHECK(std::fabs(b.speed - 0.2f) < 1e-6f);
    }
    {  // a 6-unit record: alone the walk brakes for its end (v <= sqrt(2 x 0.1 x remaining)) and stops on it; on top of a
       // started path of 300 more it keeps full speed and reports the end with the speed kept
        LandMotion a = walker(), b = walker();
        a.path = b.path = {{0.0f, 0.0f}, {6.0f, 0.0f}};
        WalkExtras top;
        top.tail = 300.0f;
        WalkResult ra, rb;
        float min_b = 1.0f;
        uint32_t fa = 0, fb = 0;
        for (; fa < 200 && !ra.arrived; ++fa) ra = walk_step(a, p, 3.0e38f, nullptr, nullptr);
        for (; fb < 200 && !rb.arrived; ++fb) {
            rb = walk_step(b, p, 3.0e38f, nullptr, &top);
            min_b = std::min(min_b, b.speed);
        }
        CHECK(ra.arrived && a.speed == 0.0f && a.state == LocoState::WalkStopped && fa > 6);
        // (6 or 7 frames: the fast normalise makes a step a few ulps short of 1)
        CHECK(rb.arrived && fb <= 7 && min_b == 1.0f && b.speed == 1.0f && b.state == LocoState::WalkMoving);
        CHECK(b.pos == (V2{6.0f, 0.0f}) && b.next == 2);
    }
}

// Collision objects inside MovementSystem: creation, crowd box / refresh, cancellation, budget fallback.
static void test_collision_system() {
    // StoppedVsStopped (s78n t20, MOVEMENT 5.4.3): two overlapping parked units of radius 40. No instant displacement: the
    // cheaper one to move - both would back straight away from the other; unit 2 already faces that way, unit 1 would
    // have to turn 180 degrees first - walks a path to the first free spot of the ring search around itself (rings 0, 1,
    // 2.8, 5.4, 8.8, 13, 18, 23.8, 30.4, 37.8, 46, 55, 64.8, 74.8, 84.8: the step grows by 0.8 up to 10; the first ring
    // whose sample along its escape direction is more than 40 + 40 from the other's centre, 5 behind it, is 84.8). The
    // record is Resolving while it walks and gone when the path is popped.
    {
        MovementSystem s{MovementConfig{}};
        PassabilityGrid g;
        g.init(40, 40);
        s.set_grid(std::move(g));
        s.add_unit(land_unit(1, {400.0f, 400.0f}, 7, 1.0f, 40.0f, kUccVehicle));
        s.add_unit(land_unit(2, {405.0f, 400.0f}, 7, 1.0f, 40.0f, kUccVehicle));
        SimRng rng(1);
        s.tick(0, rng);
        CHECK(s.collisions().list().size() == 1);
        if (s.collisions().list().size() == 1) {
            const CollisionObject& c = s.collisions().list()[0];
            CHECK(c.kind == CollisionKind::StoppedVsStopped && c.a == 1 && c.b == 2 && c.created == 0 && c.retry == 30);
            // a StoppedVsStopped record never takes the quota pass: no crowd, no priority; decided on its first service
            CHECK(c.crowd == 0.0f && c.crowd_frame == 0 && c.state == CollisionState::Resolving && c.master == 1);
        }
        // nobody was displaced in the decision frame; unit 2 holds the record's path (kind 3) to the spot
        CHECK(s.view(1).pos == (V2{400.0f, 400.0f}) && s.view(2).pos.y == 400.0f && s.view(2).pos.x < 405.0f + 1.56f + 1e-3f);
        CHECK(s.view(2).pushed && s.view(2).push_kind == 3 && !s.view(1).pushed);
        float ring = 0.0f, step = 1.0f;
        while (5.0f + ring <= 80.0f) {
            ring += step;
            step = std::min(step + 0.8f, 10.0f);
        }
        CHECK(std::fabs(ring - 84.8f) < 1e-3f);
        const std::span<const V2> top = s.top_path(2);
        CHECK(top.size() >= 2 && std::fabs(top.back().x - (405.0f + ring)) < 1e-2f && std::fabs(top.back().y - 400.0f) < 1e-3f);
        uint32_t f = 1;
        for (; f < 300 && !s.collisions().list().empty(); ++f) s.tick(f, rng);
        CHECK(s.collisions().list().empty() && s.stats().collisions_created == 1 && s.stats().collisions_resolved == 1 &&
              s.stats().collisions_cancelled == 0);
        CHECK(s.view(1).pos == (V2{400.0f, 400.0f}) && !s.view(2).pushed && std::fabs(s.view(2).pos.x - (405.0f + ring)) < 1e-2f);
        CHECK(overlap_ratio(s.view(1).pos, 40.0f, s.view(2).pos, 40.0f) <= 0.15f);
        // no new record afterwards (they no longer overlap)
        for (uint32_t n = 0; n < 20; ++n) s.tick(++f, rng);
        CHECK(s.collisions().list().empty() && s.stats().collisions_created == 1);
    }
    // s78n t18 (MOVEMENT 5.4.1): a MovingVsStopped record lives across frames in its state - Path Around while the mover
    // holds the detour made for it - and ends by its state: once the mover is stopped (no path left) it is Resolved.
    {
        MovementSystem s{MovementConfig{}};
        PassabilityGrid g;
        g.init(60, 40);
        s.set_grid(std::move(g));
        s.add_unit(land_unit(1, cell_center(5, 20), 7, 1.5f, 18.0f, kUccVehicle));
        s.add_unit(land_unit(2, cell_center(12, 20), 7, 1.5f, 18.0f, kUccVehicle));  // parked on the line
        SimRng rng(2);
        MoveOrder o;
        o.units = {1};
        o.destination = cell_center(55, 20);
        s.move(o, rng, 0);
        uint32_t f = 0, seq = UINT32_MAX, kept = 0;
        for (; f < 200; ++f) {
            s.tick(f, rng);
            if (s.collisions().list().size() == 1 && s.collisions().list()[0].kind == CollisionKind::MovingVsStopped &&
                s.collisions().list()[0].a == 1 && s.collisions().list()[0].b == 2) {
                if (seq == UINT32_MAX) seq = s.collisions().list()[0].seq;
                if (s.collisions().list()[0].seq == seq && ++kept == 3) break;  // persisted across frames
            }
        }
        CHECK(seq != UINT32_MAX && kept == 3 && s.collisions().list()[0].state == CollisionState::PathAround);
        CHECK(s.stats().dynamic_repaths == 1);  // one search for the record, not one per check
        const uint32_t resolved0 = s.stats().collisions_resolved;
        const ObjectId one = 1;
        s.stop(std::span<const ObjectId>(&one, 1));
        s.tick(++f, rng);
        CHECK(s.collisions().list().empty() && s.stats().collisions_resolved == resolved0 + 1 && s.stats().collisions_cancelled == 0);
    }
    // Quota (s78n t18, MOVEMENT 5.4.1 driver step 2): with a zero quota a record is never decided - it stays in state
    // None, which answers max speed, so the mover walks on through the blocker's footprint; no dynamic search runs and
    // nothing is displaced (retail has no instant shove).
    {
        MovementConfig cfg;
        cfg.constants.dynamic_land_complexity_quota = 0;
        MovementSystem s(cfg);
        PassabilityGrid g;
        g.init(60, 40);
        s.set_grid(std::move(g));
        s.add_unit(land_unit(1, cell_center(5, 20), 7, 1.5f, 18.0f, kUccVehicle));
        s.add_unit(land_unit(2, cell_center(30, 20), 7, 1.5f, 18.0f, kUccVehicle));
        SimRng rng(3);
        MoveOrder o;
        o.units = {1};
        o.destination = cell_center(55, 20);
        s.move(o, rng, 0);
        bool all_fallback = true;
        bool ground = true;
        for (uint32_t f = 0; f < 3000; ++f) {
            s.tick(f, rng);
            for (const CollisionLayer::Step& st : s.collisions().last_order()) all_fallback = all_fallback && !st.full;
            ground = ground && s.grid().passable_world(s.view(1).pos, s.mask_of(1));
            if (!s.view(1).has_goal) break;
        }
        CHECK(all_fallback && s.stats().dynamic_repaths == 0 && s.stats().quota_deferrals > 0);
        CHECK(ground && s.view(1).arrived && distance(s.view(1).pos, cell_center(55, 20)) < 1.0f);
        CHECK(s.view(2).pos == cell_center(30, 20));  // the blocker stands where it stood
    }
}

// ---------------------------------------------------------------- land scenario (group moves + collisions)
static void build_land_scenario(MovementSystem& sys, uint32_t units) {
    PassabilityGrid g = random_maze(96, 96, 99, 20);
    // Clear spawn and destination areas.
    for (int32_t y = 4; y < 22; ++y)
        for (int32_t x = 4; x < 30; ++x) g.set_base(x, y, kPassClear);
    for (int32_t y = 70; y < 90; ++y)
        for (int32_t x = 60; x < 90; ++x) g.set_base(x, y, kPassClear);
    g.set_playable_box({20.0f, 20.0f}, {1880.0f, 1880.0f});
    sys.set_grid(std::move(g));
    for (uint32_t i = 0; i < units; ++i) {
        const int32_t cls = i % 3 == 0 ? 7 : 0;
        const float r = cls == 7 ? 18.0f : 6.0f;
        UnitDesc d = land_unit(static_cast<ObjectId>(1 + i * 3), cell_center(5 + static_cast<int32_t>(i % 12) * 2, 5 + static_cast<int32_t>(i / 12) * 2),
                               cls, cls == 7 ? 1.4f : 1.0f, r, cls == 7 ? kUccVehicle : kUccInfantry);
        d.hero = i == 5;
        CHECK(sys.add_unit(d));
    }
}

// Runs until every unit stopped; returns frames. Checks passability every frame.
static uint32_t run_land(MovementSystem& sys, uint32_t first, uint32_t max_frames, SimRng& rng, const ParallelFor& pf,
                         bool& on_ground, CrcLog* log = nullptr, CrcRegistry* reg = nullptr) {
    on_ground = true;
    uint32_t f = first;
    for (; f < first + max_frames; ++f) {
        sys.tick(f, rng, pf);
        bool any = false;
        for (ObjectId id : sys.unit_ids()) {
            const UnitView v = sys.view(id);
            if (!sys.grid().passable_world(v.pos, sys.mask_of(id))) on_ground = false;
            any = any || v.has_goal;
        }
        if (log && reg) log->records.push_back(reg->capture(f));
        if (!any) break;
    }
    return f - first;
}

static void test_land_group() {
    MovementSystem sys{MovementConfig{}};
    build_land_scenario(sys, 36);
    SimRng rng(1234);
    MoveOrder o;
    o.units = sys.unit_ids();
    o.destination = cell_center(75, 80);
    o.flags = kMoveFlagQueue;
    sys.move(o, rng, 0);
    CHECK(rng.draws() >= 36);  // one start delay per unit
    bool ground = true;
    // s78n t23 review fix 2: 6101 frames. Units 13 and 49 stand from frame ~300 to ~4800 as M of Starved records (state
    // 15, MOVEMENT 5.4.6) with 85: M stands while it has more than one collision record (13 is also S of a Path Around
    // record of 19), and a Starved record has no contact or end test of its own - it ends by validity when 85 stops at
    // its goal. Retail-literal by the spec; the bound allows that one hold.
    const uint32_t frames = run_land(sys, 0, 6500, rng, serial_for, ground);
    uint32_t arrived = 0, near = 0;
    for (ObjectId id : sys.unit_ids()) {
        const UnitView v = sys.view(id);
        if (v.arrived) ++arrived;
        if (distance(v.pos, cell_center(75, 80)) < 400.0f) ++near;
        if (!v.arrived && std::getenv("MOVEMENT_DEBUG"))
            std::printf("  unit %d at (%.1f,%.1f) goal (%.1f,%.1f) has_goal %d failed %d speed %.2f replans %u path %zu\n", id,
                        static_cast<double>(v.pos.x), static_cast<double>(v.pos.y), static_cast<double>(v.goal.x),
                        static_cast<double>(v.goal.y), v.has_goal ? 1 : 0, v.failed ? 1 : 0, static_cast<double>(v.speed), v.replans,
                        sys.land_path(id).size());
    }
    const MovementStats& st = sys.stats();
    std::printf("land group: 36 units, %u frames, %u arrived, %u near destination; paths %u (fail %u), replans %u, "
                "collision checks %u, dynamic repaths %u, quota deferrals %u, early arrivals %u, blocked steps %u\n",
                frames, arrived, near, st.paths_planned, st.path_failures, st.replans, st.collision_checks, st.dynamic_repaths,
                st.quota_deferrals, st.early_arrivals, st.blocked_steps);
    CHECK(ground && arrived == 36 && near == 36 && frames < 6500);

    // Head-on corridor: two units swap ends through a 3-cell-wide corridor.
    MovementSystem s2{MovementConfig{}};
    PassabilityGrid g;
    g.init(60, 20);
    std::vector<uint32_t> v(60 * 20, kPassImpassable);
    for (int y = 8; y <= 10; ++y)
        for (int x = 2; x < 58; ++x) v[static_cast<size_t>(y) * 60 + static_cast<size_t>(x)] = kPassClear;
    g.set_base(v);
    s2.set_grid(std::move(g));
    s2.add_unit(land_unit(1, cell_center(4, 9), 0, 1.0f, 6.0f, kUccInfantry));
    s2.add_unit(land_unit(2, cell_center(55, 9), 0, 1.0f, 6.0f, kUccInfantry));
    MoveOrder a, b;
    a.units = {1};
    a.destination = cell_center(55, 9);
    b.units = {2};
    b.destination = cell_center(4, 9);
    s2.move(a, rng, 0);
    s2.move(b, rng, 0);
    bool g2 = true;
    const uint32_t f2 = run_land(s2, 0, 3000, rng, serial_for, g2);
    CHECK(g2 && s2.view(1).arrived && s2.view(2).arrived && f2 < 3000);
    CHECK(distance(s2.view(1).pos, cell_center(55, 9)) < 1.0f && distance(s2.view(2).pos, cell_center(4, 9)) < 1.0f);

    // Stopped blocker on the path: path around (dynamic pathfinder) and arrive.
    MovementSystem s3{MovementConfig{}};
    PassabilityGrid g3;
    g3.init(60, 40);
    s3.set_grid(std::move(g3));
    s3.add_unit(land_unit(1, cell_center(5, 20), 7, 1.5f, 18.0f, kUccVehicle));
    s3.add_unit(land_unit(2, cell_center(30, 20), 7, 1.5f, 18.0f, kUccVehicle));  // parked on the line
    MoveOrder c;
    c.units = {1};
    c.destination = cell_center(55, 20);
    s3.move(c, rng, 0);
    bool g3ok = true;
    run_land(s3, 0, 2000, rng, serial_for, g3ok);
    CHECK(g3ok && s3.view(1).arrived && distance(s3.view(1).pos, cell_center(55, 20)) < 1.0f && s3.stats().dynamic_repaths >= 1);
    CHECK(distance(s3.view(2).pos, cell_center(30, 20)) < 1.0f);  // the blocker was not pushed

    // Destination occupied by a stopped unit (s78n t18, MOVEMENT 5.4.1 0x684710 / 0x685e10): the early-arrival cut - the
    // path ends at the last point where the mover's box (hx + 5, hy) is clear of the standing unit (18 + 5 + 18 = 41,
    // found in steps of 5, cut 3 more).
    MovementSystem s4{MovementConfig{}};
    PassabilityGrid g4;
    g4.init(40, 40);
    s4.set_grid(std::move(g4));
    s4.add_unit(land_unit(1, cell_center(5, 5), 7, 1.5f, 18.0f, kUccVehicle));
    MoveOrder d;
    d.units = {1};
    d.destination = cell_center(30, 30);
    s4.move(d, rng, 0);
    // (s78n t20: the blocker comes after the order - the destination check 0x439360 of the order would move the goal off
    // a unit that already stands there)
    s4.add_unit(land_unit(2, cell_center(30, 30), 7, 1.5f, 18.0f, kUccVehicle));
    bool g4ok = true;
    run_land(s4, 0, 2000, rng, serial_for, g4ok);
    // s78n t18 review: the bound the comment derives - the first clear point is 41 .. 46 before the standing unit (steps of
    // 5 from the path end), the cut takes 3 more, and the walker stops on the cut path's end: 44 .. 49
    const float d4 = distance(s4.view(1).pos, cell_center(30, 30));
    std::printf("early arrival: stopped %.3f before the standing unit\n", static_cast<double>(d4));
    CHECK(s4.view(1).arrived && d4 >= 44.0f - 1e-3f && d4 <= 49.0f + 1e-3f && s4.stats().early_arrivals == 1);

    // a1j t02 (A1_READINGS §36.4): a Giant Vehicle pushes a stopped unit it does not crush aside (0x684f90 -> 0x6859a0)
    // and keeps its path: no path-around, it arrives at its destination, the parked infantry stands off the line.
    MovementSystem s5{MovementConfig{}};
    PassabilityGrid g5;
    g5.init(60, 40);
    s5.set_grid(std::move(g5));
    s5.add_unit(land_unit(1, cell_center(5, 20), 11, 1.0f, 30.0f, kUccGiantVehicle));
    s5.add_unit(land_unit(2, cell_center(30, 20), 0, 1.5f, 5.0f, kUccInfantry));  // parked on the line
    MoveOrder e;
    e.units = {1};
    e.destination = cell_center(55, 20);
    s5.move(e, rng, 0);
    bool g5ok = true;
    run_land(s5, 0, 3000, rng, serial_for, g5ok);
    const V2 pushed = s5.view(2).pos;
    std::printf("giant push: arrived %d, dynamic re-paths %u, infantry moved to (%.1f, %.1f)\n", s5.view(1).arrived ? 1 : 0,
                s5.stats().dynamic_repaths, static_cast<double>(pushed.x), static_cast<double>(pushed.y));
    CHECK(g5ok && s5.view(1).arrived && distance(s5.view(1).pos, cell_center(55, 20)) < 1.0f && s5.stats().dynamic_repaths == 0);
    CHECK(std::fabs(pushed.y - cell_center(30, 20).y) > 5.0f);
}

// a1k t03 (A1_READINGS §40): the push's free-position search 0x440f90, the pushed move and what follows it.
static void test_push_search() {
    // A Giant Vehicle (radius 30) on the line y = row 20 meets parked infantry (radius 5) on the line. The infantry is on
    // the line (cross = 0: the right side, d = heading - 45 degrees); the search samples the 45-degree arc from d and the
    // first spot off the pusher's path (clearance 30 + 5 + 4 = 39) is ring 12 (radius 60.3) at theta 0, i.e. d itself.
    {
        MovementSystem s{MovementConfig{}};
        PassabilityGrid g;
        g.init(60, 40);
        s.set_grid(std::move(g));
        s.add_unit(land_unit(1, cell_center(5, 20), 11, 1.0f, 30.0f, kUccGiantVehicle));
        s.add_unit(land_unit(2, cell_center(30, 20), 0, 1.5f, 5.0f, kUccInfantry));
        SimRng rng(5);
        MoveOrder o;
        o.units = {1};
        o.destination = cell_center(55, 20);
        s.move(o, rng, 0);
        const V2 b0 = cell_center(30, 20);
        uint32_t f = 0, push_frame = 0, end_frame = 0;
        bool order_kept = true;
        for (; f < 3000; ++f) {
            s.tick(f, rng);
            const UnitView v = s.view(2);
            if (v.pushed && push_frame == 0) push_frame = f;
            if (push_frame && !v.pushed && end_frame == 0) end_frame = f;
            order_kept = order_kept && !v.has_goal && !v.arrived;  // a pushed move is no order
            if (end_frame && !s.view(1).has_goal) break;
        }
        const V2 spot = s.view(2).pos;
        const float ring = 60.3f * 0.70710678f;  // R12 at -45 degrees
        std::printf("push search: pushed at %u, reached the spot at %u, spot (%.2f, %.2f) from (%.2f, %.2f)\n", push_frame,
                    end_frame, static_cast<double>(spot.x), static_cast<double>(spot.y), static_cast<double>(b0.x),
                    static_cast<double>(b0.y));
        CHECK(s.stats().pushes == 1 && s.stats().push_failures == 0 && push_frame > 0 && end_frame > push_frame);
        CHECK(std::fabs(spot.x - (b0.x + ring)) < 0.5f && std::fabs(spot.y - (b0.y - ring)) < 0.5f);
        CHECK(order_kept && s.view(1).arrived && distance(s.view(1).pos, cell_center(55, 20)) < 1.0f && s.stats().dynamic_repaths == 0);
        // No return move (MinimumPushReturnDistance / RepushDistance have no reader): the idle unit stays at the spot.
        for (uint32_t n = 0; n < 300; ++n) s.tick(++f, rng);
        CHECK(distance(s.view(2).pos, spot) < 0.01f && !s.view(2).pushed && !s.view(2).has_goal);
    }
    // No free spot: a one-cell corridor leaves no reachable sample off the pusher's path (clearance 10 + 5 + 4 = 19 >
    // the half corridor): the push fails every time and the blocker is never moved (the decision falls through to the
    // path-around / try-again rules; their fallback, not the push, is what lets the Giant Vehicle creep past).
    {
        MovementSystem s{MovementConfig{}};
        PassabilityGrid g;
        g.init(60, 40);
        std::vector<uint32_t> v(60 * 40, kPassImpassable);
        for (int x = 2; x < 58; ++x) v[static_cast<size_t>(20) * 60 + static_cast<size_t>(x)] = kPassClear;
        g.set_base(v);
        s.set_grid(std::move(g));
        s.add_unit(land_unit(1, cell_center(5, 20), 0, 1.0f, 10.0f, kUccGiantVehicle));
        s.add_unit(land_unit(2, cell_center(30, 20), 0, 1.5f, 5.0f, kUccInfantry));
        SimRng rng(6);
        MoveOrder o;
        o.units = {1};
        o.destination = cell_center(55, 20);
        s.move(o, rng, 0);
        bool never_pushed = true;
        for (uint32_t f = 0; f < 1500; ++f) {
            s.tick(f, rng);
            never_pushed = never_pushed && !s.view(2).pushed;
        }
        std::printf("push search, no spot: %u failed pushes, giant at x %.1f\n", s.stats().push_failures,
                    static_cast<double>(s.view(1).pos.x));
        CHECK(never_pushed && s.stats().pushes == 0 && s.stats().push_failures > 0);
        CHECK(distance(s.view(2).pos, cell_center(30, 20)) < 0.01f);
    }
    // The move under the push resumes: the blocker holds an order that has not started yet (path not planned, start
    // frame 900), so it is stopped and gets pushed; the pushed path is walked first, the order stays underneath and
    // runs from the spot when it starts. A new order (or stop) ends a pushed move.
    {
        MovementSystem s{MovementConfig{}};
        PassabilityGrid g;
        g.init(60, 40);
        s.set_grid(std::move(g));
        s.add_unit(land_unit(1, cell_center(5, 20), 11, 1.0f, 30.0f, kUccGiantVehicle));
        s.add_unit(land_unit(2, cell_center(30, 20), 0, 1.5f, 5.0f, kUccInfantry));
        SimRng rng(7);
        MoveOrder o;
        o.units = {1};
        o.destination = cell_center(55, 20);
        s.move(o, rng, 0);
        s.coordinator_update(0, 0, rng);  // cu8j: each order composes at its own (next) coordinator update
        MoveOrder later;
        later.units = {2};
        later.destination = cell_center(30, 35);
        s.move(later, rng, 900);
        s.coordinator_update(900, 900, rng);
        CHECK(s.view(2).has_goal && s.view(2).start_frame >= 900);
        uint32_t push_frame = 0, end_frame = 0;
        bool kept = true;
        uint32_t f = 0;
        for (; f < 3000; ++f) {
            s.tick(f, rng);
            const UnitView v = s.view(2);
            if (v.pushed && push_frame == 0) push_frame = f;
            if (push_frame && !v.pushed && end_frame == 0) end_frame = f;
            if (push_frame && !end_frame) kept = kept && v.has_goal && distance(v.goal, cell_center(30, 35)) < 1.0f;
            if (v.arrived) break;
        }
        std::printf("push resume: pushed at %u, spot reached at %u, order arrived at %u\n", push_frame, end_frame, f);
        CHECK(push_frame > 0 && push_frame < 900 && end_frame > push_frame && end_frame < 900 && kept);
        CHECK(s.view(2).arrived && distance(s.view(2).pos, cell_center(30, 35)) < 1.0f && f > 900);
        // A stop during a pushed move ends it.
        MovementSystem s2{MovementConfig{}};
        PassabilityGrid g2;
        g2.init(60, 40);
        s2.set_grid(std::move(g2));
        s2.add_unit(land_unit(1, cell_center(5, 20), 11, 1.0f, 30.0f, kUccGiantVehicle));
        s2.add_unit(land_unit(2, cell_center(30, 20), 0, 0.2f, 5.0f, kUccInfantry));  // slow: still walking when stopped
        MoveOrder o2;
        o2.units = {1};
        o2.destination = cell_center(55, 20);
        s2.move(o2, rng, 0);
        bool stopped = false;
        for (uint32_t n = 0; n < 2000 && !stopped; ++n) {
            s2.tick(n, rng);
            if (s2.view(2).pushed) {
                const ObjectId two = 2;
                s2.stop(std::span<const ObjectId>(&two, 1));
                stopped = true;
            }
        }
        CHECK(stopped && !s2.view(2).pushed);
    }
}

// ---------------------------------------------------------------- space
static void test_space() {
    SpaceOccupancy occ;
    occ.set_box({-3000.0f, -3000.0f}, {3000.0f, 3000.0f});
    occ.add_static(-1, {0.0f, 0.0f}, 400.0f, kSpaceStatic | kSpaceAsteroid);
    CHECK(occ.query_static({-1000.0f, 0.0f}, {1000.0f, 0.0f}, 10.0f) == (kSpaceStatic | kSpaceAsteroid));
    CHECK(occ.query_static({-1000.0f, 500.0f}, {1000.0f, 500.0f}, 10.0f) == 0);
    // A moving track in layer 2 crossing a segment at the right time.
    std::vector<SpaceWaypoint> tw(2);
    tw[0].pos = {1000.0f, -1000.0f};
    tw[1].pos = {1000.0f, 1000.0f};
    tw[1].time = 200.0f;
    occ.set_track(5, kLayerFrigate, 50.0f, 100, tw);
    CHECK(occ.query_segment({900.0f, 0.0f}, {1100.0f, 0.0f}, 190.0f, 210.0f, 10.0f, kLayerFrigate, 9, false, 100) == kSpaceMoving);
    CHECK(occ.query_segment({900.0f, 0.0f}, {1100.0f, 0.0f}, 100.0f, 120.0f, 10.0f, kLayerFrigate, 9, false, 100) == 0);
    CHECK(occ.query_segment({900.0f, 0.0f}, {1100.0f, 0.0f}, 190.0f, 210.0f, 10.0f, kLayerCapital, 9, false, 100) == 0);  // other layer
    CHECK(occ.query_point({1000.0f, 1000.0f}, 900.0f, 10.0f, kLayerFrigate, 9, 100) == kSpaceStatic);  // parked after its path
    occ.clear_track(5);

    // Plan around the asteroid.
    MovementConstants k;
    SpaceRequest q;
    q.self = 1;
    q.start = {-1500.0f, 0.0f};
    q.goal = {1500.0f, 0.0f};
    q.start_heading = 0.0f;
    q.speed = 6.0f;
    q.turn_rate = 1.5f;
    q.radius = 60.0f;
    q.layer = kLayerFrigate;
    SpaceScratch sc;
    SpacePath p;
    CHECK(plan_space_path(occ, k, q, sc, p) && p.ok && !p.fallback && p.wps.size() >= 3);
    bool clear = true;
    float t_prev = -1.0f;
    bool times = true;
    for (size_t i = 0; i < p.wps.size(); ++i) {
        if (p.wps[i].time < t_prev) times = false;
        t_prev = p.wps[i].time;
    }
    // Hermite playback stays clear of the asteroid and reaches the goal.
    SpacePose last;
    float max_step = 0.0f;
    V2 prev = q.start;
    for (float t = 0.0f; t <= p.wps.back().time + 1.0f; t += 1.0f) {
        const SpacePose s = eval_space_path(p.wps, t);
        if (distance(s.pos, {0.0f, 0.0f}) < 400.0f + 30.0f) clear = false;
        max_step = std::max(max_step, distance(prev, s.pos));
        prev = s.pos;
        last = s;
    }
    std::printf("space path: %zu waypoints, %.0f frames, tries %u, pushes %u, max step %.2f\n", p.wps.size(),
                static_cast<double>(p.wps.back().time), p.tries, p.pushes, static_cast<double>(max_step));
    CHECK(clear && times && last.done && last.pos == q.goal && max_step < 2.0f * q.speed);
    // Short moves skip planning; retries kick in with a tiny budget.
    q.goal = {-1480.0f, 10.0f};
    CHECK(plan_space_path(occ, k, q, sc, p) && p.skipped);
    MovementConstants tight = k;
    tight.space_pathfind_max_expansions = 20;
    q.goal = {1500.0f, 0.0f};
    CHECK(plan_space_path(occ, tight, q, sc, p) && p.tries > 1);
    // Destination search: a goal inside the asteroid moves to a free ring point biased toward the ship.
    const V2 dst = find_space_destination(occ, k, {0.0f, 0.0f}, {-2000.0f, 0.0f}, 60.0f, kLayerFrigate, 1, 0, {});
    CHECK(distance(dst, {0.0f, 0.0f}) >= 400.0f + 72.0f && dst.x < 0.0f);
    // Hermite: zero-length segment interpolates yaw.
    std::vector<SpaceWaypoint> turn(2);
    turn[0].heading = 0.0f;
    turn[1].heading = 90.0f;
    turn[1].time = 10.0f;
    CHECK(std::fabs(eval_space_path(turn, 5.0f).heading - 45.0f) < 1e-4f);
}

// OPT3 t12: SpaceOccupancy::build_index (query_segment visits only the entries near the query) answers every query as the
// full scan does: random statics and tracks, random segments, layers, self ids and times, static / no static; a mutation
// drops the index (full scan again) until the next build; a non-finite query scans everything.
static void test_space_index() {
    SimRng r(1234);
    auto rf = [&r](float lo, float hi) { return r.range_float(lo, hi); };
    SpaceOccupancy occ;
    occ.set_box({-6000.0f, -6000.0f}, {6000.0f, 6000.0f});
    for (int32_t i = 0; i < 40; ++i)
        occ.add_static(-1 - i, {rf(-5000.0f, 5000.0f), rf(-5000.0f, 5000.0f)}, rf(20.0f, 600.0f),
                       i % 3 == 0 ? (kSpaceStatic | kSpaceAsteroid) : kSpaceStatic);
    for (int32_t i = 0; i < 30; ++i) {
        std::vector<SpaceWaypoint> wps(static_cast<size_t>(r.range_int(1, 5)));
        float t = 0.0f;
        V2 p{rf(-5000.0f, 5000.0f), rf(-5000.0f, 5000.0f)};
        for (SpaceWaypoint& w : wps) {
            w.pos = p;
            w.time = t;
            p = p + V2{rf(-900.0f, 900.0f), rf(-900.0f, 900.0f)};
            t += rf(10.0f, 200.0f);
        }
        occ.set_track(10 + i, i % 2 ? kLayerFrigate : kLayerCapital, rf(20.0f, 150.0f), static_cast<uint32_t>(r.range_int(0, 300)), wps);
    }
    auto compare = [&](const SpaceOccupancy& indexed, int32_t n) {
        bool same = true;
        int32_t hits = 0;
        for (int32_t k = 0; k < n; ++k) {
            const V2 a{rf(-6500.0f, 6500.0f), rf(-6500.0f, 6500.0f)};
            const V2 b = k % 4 == 0 ? a : a + V2{rf(-2500.0f, 2500.0f), rf(-2500.0f, 2500.0f)};
            const float t0 = rf(0.0f, 900.0f), t1 = t0 + rf(-50.0f, 300.0f), radius = rf(0.0f, 300.0f);
            const uint32_t layer = k % 3 == 0 ? kLayerFrigate : (k % 3 == 1 ? kLayerCapital : (kLayerFrigate | kLayerCapital));
            const ObjectId self = k % 5 == 0 ? static_cast<ObjectId>(10 + k % 30) : (k % 5 == 1 ? static_cast<ObjectId>(-1 - k % 40) : 999);
            const bool with_static = k % 2 == 0;
            const auto now = static_cast<uint32_t>(r.range_int(0, 600));
            const uint32_t x = indexed.query_segment(a, b, t0, t1, radius, layer, self, with_static, now);
            same = same && x == occ.query_segment(a, b, t0, t1, radius, layer, self, with_static, now);
            hits += x != 0 ? 1 : 0;
        }
        return same && hits > n / 20;
    };
    CHECK(!occ.index_valid());
    SpaceOccupancy ix = occ;
    ix.build_index();
    CHECK(ix.index_valid() && compare(ix, 4000));
    // A mutation drops the index; queries stay right; the rebuild indexes the new entries.
    ix.add_static(-100, {0.0f, 0.0f}, 300.0f, kSpaceStatic);
    occ.add_static(-100, {0.0f, 0.0f}, 300.0f, kSpaceStatic);
    CHECK(!ix.index_valid() && compare(ix, 500));
    ix.build_index();
    std::vector<SpaceWaypoint> w2(2);
    w2[0].pos = {-4000.0f, 4000.0f};
    w2[1].pos = {4000.0f, -4000.0f};
    w2[1].time = 400.0f;
    ix.clear_track(12);
    occ.clear_track(12);
    ix.set_track(77, kLayerFrigate, 80.0f, 50, w2);
    occ.set_track(77, kLayerFrigate, 80.0f, 50, w2);
    CHECK(!ix.index_valid());
    ix.build_index();
    CHECK(ix.index_valid() && compare(ix, 2000));
    // Non-finite query: the full scan (no cell lookup).
    const float inf = std::numeric_limits<float>::infinity();
    CHECK(ix.query_segment({-inf, 0.0f}, {0.0f, 0.0f}, 0.0f, 10.0f, 10.0f, kLayerFrigate, 999, true, 0) ==
          occ.query_segment({-inf, 0.0f}, {0.0f, 0.0f}, 0.0f, 10.0f, 10.0f, kLayerFrigate, 999, true, 0));
    // An empty occupancy indexes to nothing.
    SpaceOccupancy empty;
    empty.build_index();
    CHECK(empty.query_segment({0.0f, 0.0f}, {100.0f, 0.0f}, 0.0f, 10.0f, 50.0f, kLayerFrigate, 1, true, 0) == 0);

    // SpaceScratch::VisitTable: find after emplace, overwrite, growth past the sized capacity, clear forgets everything.
    SpaceScratch::VisitTable vt;
    CHECK(vt.find(5) == nullptr);
    bool ok = true;
    for (uint32_t round = 0; round < 300; ++round) {
        vt.clear(8);
        const uint64_t base = static_cast<uint64_t>(round) * 1000003ull;
        const uint32_t n = round % 7 == 0 ? 500u : 20u;  // some rounds grow the table
        for (uint32_t i = 0; i < n; ++i) vt.emplace(base + i * 977ull, {static_cast<float>(i), i % 2 == 0});
        vt.emplace(base, {-1.0f, true});  // overwrite
        for (uint32_t i = 0; i < n; ++i) {
            const SpaceScratch::Visit* v = vt.find(base + i * 977ull);
            ok = ok && v && v->g == (i == 0 ? -1.0f : static_cast<float>(i)) && v->closed == (i == 0 || i % 2 == 0);
        }
        ok = ok && vt.find(base + 1) == nullptr && vt.find(base + 977ull * n) == nullptr;
        if (round > 0) ok = ok && vt.find((static_cast<uint64_t>(round) - 1) * 1000003ull + 977ull) == nullptr;  // last round's
    }
    CHECK(ok);
}

static void build_space_scenario(MovementSystem& sys, uint32_t ships) {
    SpaceOccupancy& occ = sys.space();
    occ.set_box({-5000.0f, -5000.0f}, {5000.0f, 5000.0f});
    SimRng r(77);
    for (int i = 0; i < 12; ++i) {
        const V2 c{static_cast<float>(r.range_int(-2500, 2500)), static_cast<float>(r.range_int(-2500, 2500))};
        occ.add_static(-1 - i, c, static_cast<float>(r.range_int(100, 350)), kSpaceStatic | kSpaceAsteroid);
    }
    for (uint32_t i = 0; i < ships; ++i) {
        UnitDesc d;
        d.id = static_cast<ObjectId>(10 + i);
        d.pos = {-4200.0f + static_cast<float>(i % 4) * 300.0f, -4200.0f + static_cast<float>(i / 4) * 300.0f};
        d.max_speed = i % 3 == 0 ? 2.5f : 4.0f;
        d.turn_rate = i % 3 == 0 ? 0.5f : 1.5f;
        d.radius = i % 3 == 0 ? 120.0f : 50.0f;
        d.space_layer = i % 3 == 0 ? kLayerCapital : kLayerFrigate;
        d.type_name = i % 3 == 0 ? "Capital" : "Frigate";
        CHECK(sys.add_unit(d));
    }
}

static uint32_t run_space(MovementSystem& sys, uint32_t first, uint32_t max_frames, SimRng& rng, const ParallelFor& pf,
                          CrcLog* log = nullptr, CrcRegistry* reg = nullptr) {
    uint32_t f = first;
    for (; f < first + max_frames; ++f) {
        sys.tick(f, rng, pf);
        if (log && reg) log->records.push_back(reg->capture(f));
        bool any = false;
        for (ObjectId id : sys.unit_ids()) any = any || sys.view(id).has_goal;
        if (!any) break;
    }
    return f - first;
}

static void test_space_group() {
    MovementConfig cfg;
    cfg.mode = MoveMode::Space;
    MovementSystem sys(cfg);
    build_space_scenario(sys, 12);
    SimRng rng(5);
    MoveOrder o;
    o.units = sys.unit_ids();
    o.destination = {3800.0f, 3600.0f};
    o.facing_deg = 45.0f;
    sys.move(o, rng, 0);
    const uint32_t frames = run_space(sys, 0, 12000, rng, serial_for);
    uint32_t arrived = 0;
    bool facing = true;
    for (ObjectId id : sys.unit_ids()) {
        const UnitView v = sys.view(id);
        if (v.arrived && distance(v.pos, v.goal) < 1.0f) ++arrived;
        if (std::fabs(wrap_deg(v.yaw - 45.0f)) > 0.01f) facing = false;
    }
    std::printf("space group: 12 ships, %u frames, %u arrived, %u paths, %u fallbacks\n", frames, arrived,
                sys.stats().paths_planned, sys.stats().space_fallbacks);
    CHECK(arrived == 12 && facing && frames < 12000);
}

// s78n t10 (TACTICAL_COMBAT §5.4.6 port spec item 4, MOVEMENT §4.6.4): the space composition's recruit test reads the
// world AABB 0x3982b0 of the seed and the candidate (the host's model data through set_model_bound_fn) and compares the
// box centres: |cc - cs| <= max(2 es, 0.33 D) + ec per axis. Seed S at (0, 0), candidate C at (0, 150), both role 0, goal
// (300, 0): D = 300, reach 99. One draw per formation, so the draw count is the formation count.
static void test_space_compose_boxes() {
    // Facing +x (yaw 0) the model's half x lies across (world y) and its half y along (world x): models face -Y.
    AvoidModel m30, m60, mshift;
    m30.bound_h = {30.0f, 30.0f, 10.0f};
    m60.bound_h = {60.0f, 30.0f, 10.0f};  // world half y 60
    mshift.bound_h = {30.0f, 30.0f, 10.0f};
    mshift.bound_c = {-25.0f, 0.0f, 0.0f};  // world centre y = 150 - 25 = 125
    struct Case {
        const AvoidModel* c_model;  // candidate's model (the seed has m30); null with `hook` = a type without a model
        bool hook;
        float c_radius;
        int formations;
    };
    const Case cases[] = {
        {&m30, true, 8.0f, 2},     // 150 > 99 + 30
        {&m60, true, 8.0f, 1},     // 150 <= 99 + 60
        {&mshift, true, 8.0f, 1},  // box centre at y 125 <= 99 + 30
        {nullptr, true, 8.0f, 2},  // no model: the 0.1 box at the position, 150.05 > 99 + 0.05
        {&m60, false, 8.0f, 2},    // no hook: the older stand-in, the radius - 150 > 99 + 8
        {&m30, false, 60.0f, 1},   // no hook: 150 <= 99 + 60 (the radius, not the model)
    };
    for (const Case& c : cases) {
        MovementConfig cfg;
        cfg.mode = MoveMode::Space;
        MovementSystem sys(cfg);
        sys.space().set_box({-5000.0f, -5000.0f}, {5000.0f, 5000.0f});
        for (const ObjectId id : {ObjectId{1}, ObjectId{2}}) {
            UnitDesc d;
            d.id = id;
            d.pos = id == 1 ? V2{0.0f, 0.0f} : V2{0.0f, 150.0f};
            d.max_speed = 4.0f;
            d.turn_rate = 1.5f;
            d.radius = id == 1 ? 8.0f : c.c_radius;
            d.space_layer = kLayerFrigate;
            d.space_layer_type = 0;  // role 0
            CHECK(sys.add_unit(d));
        }
        if (c.hook)
            sys.set_model_bound_fn([&](ObjectId id, const AvoidModel*& model, float& scale) {
                model = id == 1 ? &m30 : c.c_model;
                scale = 1.0f;
                return true;
            });
        SimRng rng(31);
        MoveOrder o;
        o.units = {1, 2};
        o.destination = {300.0f, 0.0f};
        sys.move(o, rng, 100);
        CHECK(rng.draws() == 0 && sys.pending_compositions() == 1);
        sys.coordinator_update(100, 100, rng);
        CHECK(sys.pending_compositions() == 0);
        CHECK(rng.draws() == static_cast<uint64_t>(c.formations));
    }
    {  // a hook that has no data for the candidate (false): that candidate keeps the stand-in, the seed its box
        MovementConfig cfg;
        cfg.mode = MoveMode::Space;
        MovementSystem sys(cfg);
        sys.space().set_box({-5000.0f, -5000.0f}, {5000.0f, 5000.0f});
        for (const ObjectId id : {ObjectId{1}, ObjectId{2}}) {
            UnitDesc d;
            d.id = id;
            d.pos = id == 1 ? V2{0.0f, 0.0f} : V2{0.0f, 150.0f};
            d.max_speed = 4.0f;
            d.turn_rate = 1.5f;
            d.radius = id == 1 ? 8.0f : 55.0f;
            d.space_layer = kLayerFrigate;
            CHECK(sys.add_unit(d));
        }
        sys.set_model_bound_fn([&](ObjectId id, const AvoidModel*& model, float& scale) {
            model = &m30;
            scale = 1.0f;
            return id == 1;
        });
        SimRng rng(31);
        MoveOrder o;
        o.units = {1, 2};
        o.destination = {300.0f, 0.0f};
        sys.move(o, rng, 100);
        sys.coordinator_update(100, 100, rng);
        CHECK(rng.draws() == 1);  // 150 <= max(2 x 30, 99) + 55
    }
}

// ---------------------------------------------------------------- OI tactical (PHASE4 S7 open items)
static void test_oi_land() {
    // PathFrameDelay stagger (§1.6) with the FormationClass ctor draw (cu8j t01, correcting cu8i t02): the order draws
    // nothing; the next coordinator update draws r' = int(0, PathFrameDelay) per staggered formation (formation order),
    // then one int(1, FPS) per formation. The plan gate is now + accumulated PathFrameDelay + r' - 2 FPS, so the first
    // plan frame is now + acc + r' + 1; the ctor draw only sets the next-check timer.
    {
        MovementSystem sys{MovementConfig{}};
        PassabilityGrid g;
        g.init(64, 64);
        sys.set_grid(std::move(g));
        UnitDesc a = land_unit(1, cell_center(10, 10), 0, 1.0f, 6.0f, kUccInfantry);
        a.grouping = kGroupingSolo;
        a.path_frame_delay = 40;
        UnitDesc b = land_unit(2, cell_center(10, 30), 0, 1.0f, 6.0f, kUccInfantry);
        b.grouping = kGroupingSolo;
        b.path_frame_delay = 4;
        CHECK(sys.add_unit(a) && sys.add_unit(b));
        SimRng rng(77), ref(77);
        MoveOrder o;
        o.units = {1, 2};
        o.destination = cell_center(50, 20);
        sys.move(o, rng, 100);
        CHECK(rng.draws() == 0 && sys.pending_compositions() == 1);  // composed at the next coordinator update
        CHECK(sys.view(1).start_frame == kStartPending && sys.has_coordinator(1) && sys.has_coordinator(2));
        sys.coordinator_update(100, 100, rng);
        // Formation order = seed nearest the bucket centroid first (equal here: the earlier unit, id 1).
        const int32_t p1 = ref.range_int(0, 40), p2 = ref.range_int(0, 4);
        const int32_t d1 = ref.range_int(1, 30), d2 = ref.range_int(1, 30);
        CHECK(rng.draws() == ref.draws() && rng.state() == ref.state());  // two r', then two int(1, FPS)
        CHECK(d1 >= 1 && d1 <= 30 && d2 >= 1 && d2 <= 30 && sys.pending_compositions() == 0);
        CHECK(sys.view(1).start_frame == static_cast<uint32_t>(100 + p1 + 1));
        CHECK(sys.view(2).start_frame == static_cast<uint32_t>(100 + 40 + p2 + 1));
        // Queued orders keep the per-unit delay (no stagger accumulator) and create no formation: no ctor draw.
        MoveOrder q = o;
        q.flags = kMoveFlagQueue;
        SimRng r2(5), r2ref(5);
        sys.move(q, r2, 200);
        CHECK(sys.pending_compositions() == 0);
        CHECK(sys.view(1).start_frame == 200 + static_cast<uint32_t>(r2ref.range_int(0, 6)));
        CHECK(sys.view(2).start_frame == 200 + static_cast<uint32_t>(r2ref.range_int(0, 6)));
        CHECK(r2.draws() == r2ref.draws());
    }
    // cu8j t01 [M]: an unstaggered formation created at frame <= 2 FPS plans at frame 61 (F +0x1dc = 0); later, at once.
    {
        MovementSystem sys{MovementConfig{}};
        PassabilityGrid g;
        g.init(64, 64);
        sys.set_grid(std::move(g));
        CHECK(sys.add_unit(land_unit(1, cell_center(12, 10), 0, 1.0f, 6.0f, kUccInfantry)));
        SimRng rng(3), ref(3);
        MoveOrder o;
        o.units = {1};
        o.destination = cell_center(40, 10);
        sys.move(o, rng, 10);
        sys.coordinator_update(10, 10, rng);
        ref.range_int(1, 30);
        CHECK(sys.view(1).start_frame == 61 && rng.state() == ref.state() && rng.draws() == 1);
        sys.move(o, rng, 70);
        sys.coordinator_update(70, 70, rng);
        CHECK(sys.view(1).start_frame == 70 && rng.draws() == 2);
        // Global frame (the battle's) decides the gate, not the movement frame.
        sys.move(o, rng, 5);
        sys.coordinator_update(5, 200, rng);
        CHECK(sys.view(1).start_frame == 5 && rng.draws() == 3);
    }
    // cu8i t02 / cu8j t02: the FormationClass ctor draw is one int(1, FPS); a unit with an order has a coordinator, others
    // (and unknown ids) do not. Adopt (0x39bcb0) draws once for a coordinator-less unit and nothing for one with a
    // coordinator; stop-and-hold (0x3a9880) leaves the coordinator first, so it always draws.
    {
        MovementSystem sys{MovementConfig{}};
        PassabilityGrid g;
        g.init(64, 64);
        sys.set_grid(std::move(g));
        CHECK(sys.add_unit(land_unit(1, cell_center(12, 10), 0, 1.0f, 6.0f, kUccInfantry)) &&
              sys.add_unit(land_unit(2, cell_center(14, 10), 0, 1.0f, 6.0f, kUccInfantry)));
        SimRng rng(11), ref(11);
        MoveOrder o;
        o.units = {2};
        o.destination = cell_center(40, 10);
        sys.move(o, rng, 100);
        sys.coordinator_update(100, 100, rng);
        ref.range_int(1, 30);  // unit 2's formation
        CHECK(rng.state() == ref.state() && rng.draws() == ref.draws());
        CHECK(!sys.has_coordinator(1) && sys.has_coordinator(2) && !sys.has_coordinator(99));
        const int32_t r = MovementSystem::formation_ctor_draw(rng);
        CHECK(r == ref.range_int(1, 30) && r >= 1 && r <= 30 && rng.state() == ref.state());
        CHECK(sys.adopt(1, rng) && sys.has_coordinator(1));
        ref.range_int(1, 30);
        CHECK(!sys.adopt(1, rng) && !sys.adopt(2, rng) && !sys.adopt(99, rng) && rng.state() == ref.state());
        CHECK(sys.stop_and_hold(2, rng) && !sys.view(2).has_goal && sys.has_coordinator(2));
        CHECK(sys.stop_and_hold(1, rng) && sys.has_coordinator(1));
        ref.range_int(1, 30);
        ref.range_int(1, 30);
        CHECK(rng.state() == ref.state() && rng.draws() == ref.draws());
    }
    // cu8l t01 (CU8K_T02_T06_RE §4 rules 1 / 4): a land team container's own coordinator from birth (hold_from_birth with
    // its members): no draw at registration, the adopt then finds a coordinator; at the next coordinator update one
    // int(1, FPS) per held container, in registration order, no r' (even with a member PathFrameDelay) and no start
    // change; a container whose members were ordered first composes nothing (the order's own composition draws).
    {
        MovementSystem sys{MovementConfig{}};
        PassabilityGrid g;
        g.init(64, 64);
        sys.set_grid(std::move(g));
        for (int32_t i = 1; i <= 6; ++i) {
            UnitDesc d = land_unit(i, cell_center(10 + 2 * i, 10), 0, 1.0f, 6.0f, kUccInfantry);
            d.path_frame_delay = 4;
            d.team_member = true;
            CHECK(sys.add_unit(d));
        }
        SimRng rng(21), ref(21);
        const ObjectId c1[] = {1, 2}, c2[] = {3, 4, 99}, c3[] = {5, 6}, none[] = {98};
        CHECK(sys.hold_from_birth(c1) && sys.hold_from_birth(c2) && sys.hold_from_birth(c3) && !sys.hold_from_birth(none));
        CHECK(rng.draws() == 0 && sys.pending_compositions() == 3 && sys.has_coordinator(1) && sys.has_coordinator(4));
        CHECK(!sys.adopt(1, rng) && rng.draws() == 0);
        const uint32_t s1 = sys.view(1).start_frame;
        const ObjectId stop2[] = {3, 4};  // container 2 leaves its birth coordinator first (a stop): it composes nothing
        sys.stop(stop2);
        sys.coordinator_update(1, 1, rng);
        ref.range_int(1, 30);  // container 1
        ref.range_int(1, 30);  // container 3
        CHECK(rng.state() == ref.state() && rng.draws() == ref.draws() && sys.pending_compositions() == 0);
        CHECK(sys.view(1).start_frame == s1 && !sys.view(1).has_goal && !sys.has_coordinator(1) && !sys.has_coordinator(5));
    }
    {  // an order before the update: the birth coordinator composes nothing, the order's own composition draws as alone.
        const auto run = [](bool born) {
            MovementSystem sys{MovementConfig{}};
            PassabilityGrid g;
            g.init(64, 64);
            sys.set_grid(std::move(g));
            for (int32_t i = 1; i <= 2; ++i) CHECK(sys.add_unit(land_unit(i, cell_center(10 + 2 * i, 10), 0, 1.0f, 6.0f, kUccInfantry)));
            const ObjectId c[] = {1, 2};
            if (born) CHECK(sys.hold_from_birth(c));
            SimRng rng(9);
            MoveOrder o;
            o.units = {1, 2};
            o.destination = cell_center(40, 40);
            sys.move(o, rng, 1);
            sys.coordinator_update(1, 1, rng);
            CHECK(sys.pending_compositions() == 0 && sys.view(1).start_frame != kStartPending);
            return rng.state();
        };
        CHECK(run(true) == run(false));
    }
    {  // space: a SimpleSpace ship's birth coordinator is one space composition (one draw), its start untouched.
        MovementConfig cfg;
        cfg.mode = MoveMode::Space;
        MovementSystem sys{cfg};
        UnitDesc d;
        d.id = 7;
        d.pos = {100.0f, 100.0f};
        d.max_speed = 2.0f;
        d.radius = 20.0f;
        CHECK(sys.add_unit(d));
        SimRng rng(5), ref(5);
        const ObjectId one[] = {7};
        CHECK(sys.hold_from_birth(one) && rng.draws() == 0 && sys.has_coordinator(7));
        const uint32_t s0 = sys.view(7).start_frame;
        sys.coordinator_update(1, 1, rng);
        ref.range_int(1, 30);
        CHECK(rng.state() == ref.state() && rng.draws() == ref.draws() && sys.pending_compositions() == 0);
        CHECK(sys.view(7).start_frame == s0 && !sys.has_coordinator(7));
    }
    // cu8l t02 (CU8K_T02_T06_RE §3 rule 6, A1_READINGS §28.8 / §28.9): the first lock's split and the coordinator-system
    // stop. A composed group move: each member is in a formation, the split draws one int(1, FPS) and leaves the unit
    // alone with its move (still in a coordinator: no second split); waiting for the composition or held by an adopt: no
    // formation, no draw; stop_and_leave drops every coordinator without a draw.
    {
        MovementSystem sys{MovementConfig{}};
        PassabilityGrid g;
        g.init(64, 64);
        sys.set_grid(std::move(g));
        for (int32_t i = 1; i <= 3; ++i) CHECK(sys.add_unit(land_unit(i, cell_center(10 + 2 * i, 10), 0, 1.0f, 6.0f, kUccInfantry)));
        using Hold = MovementSystem::CoordinatorHold;
        SimRng rng(31), ref(31);
        MoveOrder o;
        o.units = {1, 2};
        o.destination = cell_center(40, 40);
        sys.move(o, rng, 100);
        CHECK(sys.coordinator_hold(1) == Hold::NoFormation && sys.coordinator_hold(3) == Hold::None);
        CHECK(sys.coordinator_hold(99) == Hold::None);
        CHECK(!sys.split_from_formation(1, rng) && rng.draws() == 0);  // waiting for its composition
        sys.coordinator_update(100, 100, rng);
        ref.range_int(1, 30);  // the order's formation
        CHECK(sys.coordinator_hold(1) == Hold::Formation && sys.view(1).has_goal);
        CHECK(sys.split_from_formation(1, rng));
        ref.range_int(1, 30);  // the split's formation
        CHECK(rng.state() == ref.state() && rng.draws() == ref.draws());
        CHECK(sys.view(1).has_goal && sys.has_coordinator(1) && sys.coordinator_hold(1) == Hold::NoFormation);
        CHECK(!sys.split_from_formation(1, rng) && !sys.adopt(1, rng) && rng.draws() == ref.draws());
        CHECK(!sys.split_from_formation(3, rng) && sys.adopt(3, rng));  // no formation: the adopt draws
        ref.range_int(1, 30);
        CHECK(sys.coordinator_hold(3) == Hold::NoFormation && !sys.split_from_formation(3, rng));
        CHECK(sys.stop_and_leave(2) && !sys.view(2).has_goal && !sys.has_coordinator(2) && !sys.stop_and_leave(99));
        CHECK(sys.stop_and_leave(3) && !sys.has_coordinator(3));
        CHECK(rng.state() == ref.state() && rng.draws() == ref.draws());
    }
    // Formation jiggle (§3.5 step 6): 2 synced draws per moving member per frame, offset clamped to MaxJiggleDistance;
    // members without FormationRaggedness draw nothing.
    {
        MovementSystem sys{MovementConfig{}};
        PassabilityGrid g;
        g.init(80, 40);
        sys.set_grid(std::move(g));
        UnitDesc a = land_unit(1, cell_center(5, 18), 0, 1.0f, 6.0f, kUccInfantry);
        a.formation_raggedness = 2.5f;
        a.max_jiggle_distance = 3.0f;
        UnitDesc b = land_unit(2, cell_center(5, 21), 0, 1.0f, 6.0f, kUccInfantry);  // no raggedness
        CHECK(sys.add_unit(a) && sys.add_unit(b));
        SimRng rng(9);
        MoveOrder o;
        o.units = {1, 2};
        o.destination = cell_center(70, 20);
        sys.move(o, rng, 0);
        sys.coordinator_update(0, 0, rng);  // the composition's draws (cu8j t01) before the jiggle count
        const uint64_t after_move = rng.draws();
        uint32_t moving_frames = 0;
        float max_off = 0.0f;
        bool ground = true;
        for (uint32_t f = 0; f < 3000; ++f) {
            const bool was_moving = sys.view(1).has_goal && sys.view(1).speed > 0.0f && f >= sys.view(1).start_frame;
            sys.tick(f, rng, serial_for);
            if (was_moving) ++moving_frames;
            max_off = std::max({max_off, std::fabs(sys.view(1).jiggle.x), std::fabs(sys.view(1).jiggle.y)});
            if (!sys.grid().passable_world(sys.view(1).pos, sys.mask_of(1))) ground = false;
            if (!sys.view(1).has_goal && !sys.view(2).has_goal) break;
        }
        std::printf("jiggle: %u moving frames, %llu draws, max offset %.3f\n", moving_frames,
                    static_cast<unsigned long long>(rng.draws() - after_move), static_cast<double>(max_off));
        CHECK(moving_frames > 50 && rng.draws() - after_move == 2ull * moving_frames);
        CHECK(max_off > 0.0f && max_off <= 3.0f);
        CHECK(ground && sys.view(1).arrived && sys.view(2).arrived);
        CHECK(sys.view(2).jiggle == (V2{0.0f, 0.0f}));
    }
    // Side-error steering (§4.4): a member 12 units off its segment converges back; without it the unit heads
    // straight for the waypoint.
    {
        LocomotorParams p;
        p.max_speed = 2.0f;
        p.turn_rate = 30.0f;
        p.mask = kPassClear;
        LandMotion m;
        m.pos = {0.0f, 12.0f};
        m.path = {{0.0f, 0.0f}, {400.0f, 0.0f}};
        m.next = 1;
        LandMotion plain = m;
        WalkExtras x;
        x.side_error = true;
        float e_prev = 12.0f;
        bool monotone = true;
        for (int i = 0; i < 60; ++i) {
            walk_step(m, p, 3.0e38f, nullptr, &x);
            walk_step(plain, p, 3.0e38f, nullptr);
            if (!(m.pos.y <= e_prev + 1e-4f)) monotone = false;
            e_prev = m.pos.y;
        }
        CHECK(monotone && m.pos.y < plain.pos.y && m.pos.y >= 0.0f);
        // Inside the dead band (|error| <= 0.1) nothing changes.
        LandMotion band = plain;
        band.pos = {100.0f, 0.05f};
        LandMotion band2 = band;
        walk_step(band, p, 3.0e38f, nullptr, &x);
        walk_step(band2, p, 3.0e38f, nullptr);
        CHECK(band.pos == band2.pos);
    }
    // Crush (§5.5): a moving SQUASH unit crushes hostile squashable movers it overlaps (players ascending, ids
    // ascending); friendly or non-squashable units are left alone; supercrusher-only victims need a supercrusher.
    {
        MovementSystem sys{MovementConfig{}};
        PassabilityGrid g;
        g.init(80, 40);
        sys.set_grid(std::move(g));
        // (s78n t20, MOVEMENT 5.4.5 item 4: a victim whose faction scatters from crushers is pushed out of the way on a
        // crush-flee path; these victims belong to one that does not - FoC: the Empire - so the crusher drives over them)
        sys.set_scatters_fn([](int32_t) { return false; });
        UnitDesc atat = land_unit(1, cell_center(5, 20), 11, 1.0f, 30.0f, kUccGiantVehicle);
        atat.crusher = true;
        atat.owner = 0;
        CHECK(sys.add_unit(atat));
        auto inf = [&](ObjectId id, int32_t x, int32_t owner, bool sq, bool sq_super) {
            UnitDesc d = land_unit(id, cell_center(x, 20), 0, 1.0f, 5.0f, kUccInfantry);
            d.owner = owner;
            d.squashable = sq;
            d.squashable_by_supercrusher = sq_super;
            CHECK(sys.add_unit(d));
        };
        inf(10, 20, 1, true, false);   // hostile, squashable -> crushed
        inf(11, 30, 0, true, false);   // friendly -> not crushed
        inf(12, 40, 1, false, false);  // hostile, not squashable -> not crushed
        inf(13, 50, 1, false, true);   // supercrusher-only -> not crushed by a plain crusher
        SimRng rng(3);
        MoveOrder o;
        o.units = {1};
        o.destination = cell_center(70, 20);
        // (an ordinary order: on a forced path, flag 0x20, decision step 2 pushes every blocker aside instead)
        sys.move(o, rng, 0);
        std::vector<CrushEvent> all;
        for (uint32_t f = 0; f < 2000 && sys.view(1).has_goal; ++f) {
            sys.tick(f, rng, serial_for);
            for (const CrushEvent& e : sys.crush_events()) {
                all.push_back(e);
                if (e.victim == 10) {
                    // The victim turned to face away from the crusher.
                    const V2 away = sys.view(10).pos - sys.view(1).pos;
                    CHECK(std::fabs(wrap_deg(sys.view(10).yaw - heading_deg(away))) < 1e-3f);
                }
                sys.remove_unit(e.victim);  // the host kills it (1,000,000 damage)
            }
        }
        CHECK(all.size() == 1 && all[0].crusher == 1 && all[0].victim == 10);
        CHECK(sys.has_unit(11) && sys.has_unit(12) && sys.has_unit(13));
        // a1j t02 (A1_READINGS §36.4): the friendly trooper on the line was pushed aside, not crushed.
        const V2 p11 = sys.view(11).pos;
        CHECK(std::fabs(p11.y - cell_center(30, 20).y) > 5.0f);
        // Hostility comes from the host (combat relations).
        sys.set_hostility([](int32_t, int32_t) { return true; });
        sys.set_owner(1, 5);
        MoveOrder back = o;
        const V2 from = sys.view(1).pos;
        back.destination = from + (p11 - from) * 1.5f;  // straight back over the pushed trooper
        sys.move(back, rng, 3000);
        all.clear();
        for (uint32_t f = 3000; f < 6000 && sys.view(1).has_goal; ++f) {
            sys.tick(f, rng, serial_for);
            for (const CrushEvent& e : sys.crush_events()) {
                all.push_back(e);
                sys.remove_unit(e.victim);
            }
        }
        CHECK(all.size() == 1 && all[0].victim == 11);  // now hostile and squashable
    }
    // Secondary occupants (§1.1 step 3) and Fine_Tune trimming (§1.3).
    {
        PassabilityGrid g;
        g.init(20, 20);
        StateHasher h0;
        g.hash(h0);
        ObstacleFootprint fp = make_footprint({100.0f, 100.0f}, 0.0f, 40.0f, 40.0f, -20.0f, -20.0f, 0.0f, 0.0f, 1.0f, kPassShield);
        CHECK(g.stamp_secondary(7, fp) > 0);
        CHECK((g.effective(5, 5) & kPassShield) != 0 && (g.effective(5, 5) & kPassClear) == 0);
        CHECK(g.passable_cell(5, 5, MovementClassTable{}.mask(0)) && !g.passable_cell(5, 5, MovementClassTable{}.mask(1)));
        CHECK(g.stamp(7, make_footprint({100.0f, 100.0f}, 0.0f, 20.0f, 20.0f, -10.0f, -10.0f, 0.0f, 0.0f, 1.0f, 0)) > 0);
        CHECK((g.effective(5, 5) & (kPassShield | kPassObstacle)) == (kPassShield | kPassObstacle));
        g.unstamp(7);
        CHECK(g.effective(5, 5) == kPassShield);
        CHECK(g.unstamp_secondary(7) && g.effective(5, 5) == kPassClear);
        StateHasher h1;
        g.hash(h1);
        CHECK(h0.digest() == h1.digest());  // unchanged format and state without secondary occupants
        CHECK(g.stamp_secondary_circle(8, {200.0f, 200.0f}, 45.0f, kPassShield) == 21);  // cells within 2.25 cells
        // Fine tune: only samples whose ray hits the mesh (here: x < 100) are stamped.
        ObstacleFootprint ft = make_footprint({100.0f, 300.0f}, 0.0f, 60.0f, 20.0f, -30.0f, -10.0f, 0.0f, 0.0f, 1.0f, 0);
        PassabilityGrid g2;
        g2.init(20, 20);
        const uint32_t full = g2.stamp(1, ft);
        ft.fine_tune = [](V2 xy) { return xy.x < 100.0f; };
        const uint32_t trimmed = g2.stamp(1, ft);
        CHECK(trimmed > 0 && trimmed < full);
        CHECK(g2.effective(6, 15) == kPassClear);  // x = 120: trimmed away
    }
}

static void test_oi_space() {
    // Facing mode (§2.1 ops 10-24): the plan ends exactly on the goal with the goal facing (final arc, densified).
    SpaceOccupancy occ;
    occ.set_box({-3000.0f, -3000.0f}, {3000.0f, 3000.0f});
    MovementConstants k;
    SpaceRequest q;
    q.self = 1;
    q.start = {-1500.0f, 0.0f};
    q.goal = {1500.0f, 200.0f};
    q.speed = 5.0f;
    q.turn_rate = 1.5f;
    q.radius = 50.0f;
    q.goal_facing = 90.0f;
    SpaceScratch sc;
    SpacePath p;
    CHECK(plan_space_path(occ, k, q, sc, p) && p.ok && !p.fallback);
    const SpaceWaypoint& last = p.wps.back();
    std::printf("facing mode: tries %u, pushes %u, %zu waypoints, final heading %.3f\n", p.tries, p.pushes, p.wps.size(),
                static_cast<double>(last.heading));
    CHECK(p.tries == 1 && last.pos == q.goal && std::fabs(wrap_deg(last.heading - 90.0f)) <= 10.0f);
    bool times = true;
    for (size_t i = 1; i < p.wps.size(); ++i) times = times && p.wps[i].time >= p.wps[i - 1].time;
    CHECK(times);
    // Playback ends on the goal with the waypoint heading; consecutive poses stay close (no jumps).
    V2 prev = q.start;
    float max_step = 0.0f;
    for (float t = 0.0f; t <= last.time + 1.0f; t += 1.0f) {
        const SpacePose s = eval_space_path(p.wps, t);
        max_step = std::max(max_step, distance(prev, s.pos));
        prev = s.pos;
    }
    CHECK(max_step < 2.0f * q.speed && prev == q.goal);
    // Without a facing the plan is the plain one (no facing-mode ops): the goal is reached with any heading.
    q.goal_facing = kNoFacing;
    SpacePath p2;
    CHECK(plan_space_path(occ, k, q, sc, p2) && p2.ok && p2.wps.back().pos == q.goal);
    // Speed-change operators: waypoint speeds are the max or the wait speed only (ops 5 / 6 / 7).
    bool speeds_ok = true;
    for (const SpaceWaypoint& w : p2.wps) speeds_ok = speeds_ok && (w.speed == q.speed || w.speed == q.speed * k.wait_operator_speed_coefficient);
    CHECK(speeds_ok);
}

// ---------------------------------------------------------------- space fighters (FIGHTER_FLYING.md §1-§5)
static FighterKin fighter_kin() {
    FighterKin k;  // X-wing-like, space multiplier 1.2 applied where the getters do
    k.turn = 3.6f;
    k.roll = 6.0f;
    k.lift = 4.8f;
    k.bank = 40.0f;
    k.thrust = 1.0f;
    k.min_speed = 3.0f;
    k.max_speed = 4.8f;
    return k;
}

static UnitDesc fighter_desc(ObjectId id, V2 pos, float yaw, int32_t owner) {
    UnitDesc d;
    d.id = id;
    d.pos = pos;
    d.yaw_deg = yaw;
    d.max_speed = 4.0f;
    d.turn_rate = 3.0f;
    d.radius = 8.0f;
    d.space_layer = kLayerFighter;
    d.space_layer_type = 0;
    d.locomotor = LocomotorKind::Fighter;
    d.max_lift = 4.0f;
    d.max_rate_of_roll = 5.0f;
    d.bank_turn_angle = 40.0f;
    d.min_speed = 2.5f;
    d.strafe_distance = 500.0f;
    d.owner = owner;
    return d;
}

static MovementConfig fighter_cfg() {
    MovementConfig cfg;
    cfg.mode = MoveMode::Space;
    return cfg;
}
static void fighter_box(MovementSystem& sys) { sys.space().set_box({-4000.0f, -4000.0f}, {4000.0f, 4000.0f}); }

static void test_fighter_primitives() {
    const FighterKin k = fighter_kin();
    // §1.1 wraps and the angle helpers.
    CHECK(fighter_wrap180(180.0f) == 180.0f && fighter_wrap180(-180.0f) == 180.0f && fighter_wrap180(190.0f) == -170.0f);
    CHECK(fighter_wrap360(360.0f) == 0.0f && fighter_wrap360(-10.0f) == 350.0f);
    CHECK(fighter_sin(1.0e-30f) == 1.0e-30f && fighter_cos(1.0e-30f) == 1.0f);
    CHECK(fighter_vector_angles({}, {0.0f, 5.0f, 0.0f}).pitch == 0.0f && fighter_vector_angles({}, {0.0f, 5.0f, 3.0f}).pitch < 0.0f);
    CHECK(std::fabs(fighter_vector_angles({}, {1.0f, 0.0f, -1.0f}).pitch - 45.0f) < 1e-4f);  // positive = below
    // §3.9 cone: positive RelYaw = left, positive RelPitch = below; the range is 3-D.
    RelAngles rel;
    const FAttitude level{};
    CHECK(fighter_cone({100.0f, 50.0f, 0.0f}, {}, level, 45.0f, 45.0f, 1.0e6f, rel) && rel.yaw > 26.0f && rel.yaw < 27.0f);
    CHECK(!fighter_cone({100.0f, 150.0f, 0.0f}, {}, level, 45.0f, 45.0f, 1.0e6f, rel) && rel.yaw > 56.0f);
    CHECK(!fighter_cone({100.0f, 0.0f, -120.0f}, {}, level, 45.0f, 45.0f, 1.0e6f, rel) && rel.pitch > 50.0f);
    CHECK(!fighter_cone({10.0f, 0.0f, 0.0f}, {}, level, 45.0f, 45.0f, 25.0f, rel));  // 100 > 25: out of range
    FAttitude turned{0.0f, 0.0f, 90.0f};  // facing +y: a point at +y is dead ahead
    CHECK(fighter_cone({0.0f, 100.0f, 0.0f}, {}, turned, 1.0f, 1.0f, 1.0e6f, rel) && std::fabs(rel.yaw) < 1e-3f);
    // §2 step 4 pitch fold: >= 90 (not > 90).
    FAttitude o = fighter_fold_pitch360({10.0f, 90.0f, 30.0f}, 4.0f);
    CHECK(o.roll == 190.0f && o.pitch == 86.0f && o.yaw == 210.0f);
    o = fighter_fold_pitch360({0.0f, 270.0f, 0.0f}, 4.0f);  // -90 folds the other way
    CHECK(o.pitch == 274.0f && o.yaw == 180.0f && o.roll == 180.0f);
    CHECK(fighter_fold_pitch360({0.0f, 89.5f, 0.0f}, 4.0f).pitch == 89.5f);
    // §3.2 yaw with bank: the roll leads, the yaw follows only when banked to the turning side.
    FAttitude yb{};
    float b = k.turn;
    fighter_yaw_bank_step(yb, k, 30.0f, b);
    CHECK(yb.roll == 354.0f && yb.yaw == 3.6f && b == 0.0f);
    FAttitude wrong{20.0f, 0.0f, 0.0f};  // banked right for a left turn: no yaw this frame
    b = k.turn;
    fighter_yaw_bank_step(wrong, k, 30.0f, b);
    CHECK(wrong.yaw == 0.0f && b == 0.0f && wrong.roll == 14.0f);
    // §3.4 speed step: +thrust per frame along the forward.
    FighterBody body;
    b = k.thrust;
    fighter_speed_step(body, 4.8f, b);
    CHECK(body.v.x == 1.0f && body.v.y == 0.0f && body.v.z == 0.0f && b == 0.0f);
    // §3.1 loop rule: a target more than 170 deg behind -> half loop in pitch (nose up for RelPitch <= 0), no yaw.
    FighterBody lb;
    lb.v = {4.8f, 0.0f, 0.0f};
    float tb = k.turn, thb = k.thrust, pb = k.lift;
    fighter_steer_to(lb, k, {-500.0f, 0.0f, 0.0f}, 4.8f, tb, thb, pb, false);
    CHECK(lb.loop && lb.o.yaw == 0.0f && std::fabs(fighter_wrap180(lb.o.pitch) + k.lift) < 1e-4f);
    // The flag keeps the loop going while the target stays behind (|RelYaw| >= 90), up to the fold.
    bool folded = false;
    for (int i = 0; i < 60 && !folded; ++i) {
        lb.o = fighter_fold_pitch360(lb.o, k.lift);
        folded = lb.o.yaw == 180.0f;
        if (folded) break;
        CHECK(lb.loop);
        tb = k.turn, thb = k.thrust, pb = k.lift;
        fighter_steer_to(lb, k, {-500.0f, 0.0f, 0.0f}, 4.8f, tb, thb, pb, false);
        lb.p = f3_add(lb.p, lb.v);
    }
    CHECK(folded);  // the Immelmann reverses the heading (yaw + 180 at the fold)
    // Idle (state 0x1C) never loops; a target at 100 deg turns flat.
    FighterBody ib;
    tb = k.turn, thb = k.thrust, pb = k.lift;
    fighter_steer_to(ib, k, {-500.0f, 0.0f, 0.0f}, 4.8f, tb, thb, pb, true);
    CHECK(!ib.loop);
    FighterBody fb;
    tb = k.turn, thb = k.thrust, pb = k.lift;
    fighter_steer_to(fb, k, {-100.0f, 500.0f, 0.0f}, 4.8f, tb, thb, pb, false);
    CHECK(!fb.loop && fighter_wrap180(fb.o.roll) < 0.0f);  // left turn: rolls negative first
    // Turn radius and the look-ahead (no usable path: one turn radius in a squadron, max(1.1 la, 5) solo).
    CHECK(std::fabs(fighter_turn_radius(3.6f, 4.8f) - 76.3944f) < 1e-3f);
    const F3 la1 = fighter_lookahead({}, 0, {}, {2.0f, 0.0f, 0.0f}, false, 4.8f, 3.6f);
    const F3 la2 = fighter_lookahead({}, 0, {}, {2.0f, 0.0f, 0.0f}, true, 4.8f, 3.6f);
    CHECK(std::fabs(la1.x - 76.3944f) < 1e-2f && std::fabs(la2.x - 5.28f) < 1e-3f);
    // §4.1 pass test: the bisector plane at the corner.
    std::vector<SpaceWaypoint> path(3);
    path[1].pos = {100.0f, 0.0f};
    path[2].pos = {100.0f, 100.0f};
    CHECK(fighter_waypoint_passed(path, 0, {95.0f, 10.0f, 0.0f}) && !fighter_waypoint_passed(path, 0, {50.0f, 0.0f, 0.0f}));
    CHECK(!fighter_waypoint_passed(path, 2, {500.0f, 500.0f, 0.0f}));  // complete: nothing to pass
    // Solo on the second-to-last waypoint: aims exactly at the last one.
    const F3 last = fighter_lookahead(path, 1, {90.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, true, 4.8f, 3.6f);
    CHECK(last.x == 100.0f && last.y == 100.0f);
    // §3.6 station_speed: ahead of the slot -> slower, behind -> faster, within tolerance -> v.
    StationInput si;
    si.v = 4.8f;
    si.lo = 2.4f;
    si.hi = 7.2f;
    si.leader_v = {4.8f, 0.0f, 0.0f};
    si.slot_self = {-30.0f, 0.0f, 0.0f};
    si.speed = 4.8f;
    si.error = 100.0f;
    si.p = {10.0f, 0.0f, 0.0f};  // 40 ahead of its slot
    CHECK(fighter_station_speed(si) < 4.8f);
    si.p = {-80.0f, 0.0f, 0.0f};  // 50 behind
    CHECK(fighter_station_speed(si) > 4.8f);
    si.error = 10.0f;  // the formation is within tolerance
    CHECK(fighter_station_speed(si) == 4.8f);
    // §4.2 leader orbit aim: 60 units out, 10 / r radians ahead.
    F3 d{};
    const float v = fighter_squad_idle({60.0f, 0.0f, 0.0f}, {60.0f, 0.0f, 0.0f}, d, true, 3.0f);
    CHECK(v == 0.5f && std::fabs(f3_length(d) - 60.0f) < 1e-3f && d.y > 0.0f);
}

static void test_fighter_grids() {
    const FighterAlliedFn allied = [](int32_t a, int32_t b) { return a == b; };
    FighterCellGrids g;
    g.build({-1000.0f, -1000.0f}, {1000.0f, 1000.0f});
    CHECK(g.dogfight.nx() == 5 && g.dogfight.ny() == 5 && g.parking.nx() == 16 && g.parking.ny() == 16);
    // Staggered centres: odd rows shift by half a cell.
    const F3 c00 = g.dogfight.centre({0, 0}, 7.0f), c01 = g.dogfight.centre({0, 1}, 7.0f);
    CHECK(c00.x == -800.0f && c00.y == -800.0f && c00.z == 7.0f && c01.x == -600.0f && c01.y == -400.0f);
    CHECK((g.dogfight.home({-610.0f, -390.0f, 0.0f}) == FighterCell{0, 1}));  // x - size / 2 on odd rows
    CHECK((g.dogfight.home({-5000.0f, 5000.0f, 0.0f}) == FighterCell{0, 4}));  // clamped
    // Join order, leave, duplicates refused.
    FighterCell s5, s3, s9;
    CHECK(g.dogfight.join(5, 0, {2, 2}, s5, allied) && g.dogfight.join(3, 1, {2, 2}, s3, allied) &&
          g.dogfight.join(9, 0, {2, 2}, s9, allied));
    FighterCell dup = s5;
    CHECK(!g.dogfight.join(5, 0, {2, 2}, dup, allied));
    auto ids = [&](FighterCell c) {
        std::vector<ObjectId> r;
        for (const auto& e : g.dogfight.entries(c)) r.push_back(e.id);
        return r;
    };
    CHECK((ids({2, 2}) == std::vector<ObjectId>{5, 3, 9}));
    g.dogfight.leave(3, s3);
    CHECK(!s3.valid() && (ids({2, 2}) == std::vector<ObjectId>{5, 9}));
    CHECK(g.dogfight.join(3, 1, {2, 2}, s3, allied) && (ids({2, 2}) == std::vector<ObjectId>{5, 9, 3}));
    // Dogfight lookup snaps to an occupied neighbour (key 0).
    FighterCell none;
    const F3 near32 = f3_add(g.dogfight.centre({3, 2}, 0.0f), F3{100.0f, 0.0f, 0.0f});
    CHECK((g.dogfight.lookup(77, 0, near32, 0.0f, none, false, allied).cell == FighterCell{2, 2}) && !none.valid());
    // Parking: capacity 1, one squadron per cell; a second registration takes a neighbour and sets D.
    FighterCell pa, pb, pc;
    const F3 ref = g.parking.centre({8, 8}, 0.0f);
    const FighterCellGrid::Lookup la = g.parking.lookup(100, 0, ref, 0.0f, pa, true, allied);
    CHECK((la.cell == FighterCell{8, 8}) && la.set_dest && (pa == FighterCell{8, 8}));
    FighterCell tmp;
    CHECK(!g.parking.join(101, 0, {8, 8}, tmp, allied) && !g.parking.join(102, 1, {8, 8}, tmp, allied));
    const FighterCellGrid::Lookup lb = g.parking.lookup(101, 0, ref, 0.0f, pb, true, allied);
    CHECK(lb.cell.valid() && !(lb.cell == FighterCell{8, 8}) && std::abs(lb.cell.i - 8) <= 1 && std::abs(lb.cell.j - 8) <= 1 &&
          lb.set_dest && pb == lb.cell);
    // Already assigned: the stored cell is returned (D rewritten), nothing joins twice.
    const FighterCellGrid::Lookup again = g.parking.lookup(100, 0, {0.0f, 0.0f, 0.0f}, 0.0f, pa, true, allied);
    CHECK((again.cell == FighterCell{8, 8}) && again.set_dest && g.parking.entries({8, 8}).size() == 1);
    // A hostile neighbour's cell is not a candidate either.
    const FighterCellGrid::Lookup lc = g.parking.lookup(102, 1, ref, 0.0f, pc, true, allied);
    CHECK(lc.cell.valid() && !(lc.cell == pa) && !(lc.cell == pb));
    StateHasher h1, h2;
    g.hash(h1);
    g.parking.leave(102, pc);
    g.hash(h2);
    CHECK(h1.digest() != h2.digest());
}

// A solo fighter flies a planned path reactively (no Hermite playback) and idles past its destination.
static void test_fighter_moving() {
    MovementSystem sys(fighter_cfg());
    fighter_box(sys);
    CHECK(sys.add_unit(fighter_desc(1, {-2000.0f, 0.0f}, 0.0f, 0)));
    CHECK(sys.view(1).loco_state == LocoState::FighterIdle);
    SimRng rng(5);
    MoveOrder o;
    o.units = {1};
    o.destination = {2000.0f, 800.0f};
    sys.move(o, rng, 0);
    CHECK(sys.view(1).loco_state == LocoState::FighterMoving);
    uint32_t f = 0;
    bool hermite_free = true, speed_ok = true, att_ok = true;
    float min_roll = 0.0f;
    for (; f < 3000; ++f) {
        sys.tick(f, rng);
        const UnitView v = sys.view(1);
        speed_ok = speed_ok && v.speed <= 4.8f + 1e-4f;
        float ar = 1.0f, ap = 1.0f;  // S7 t05: the render attitude = the view's roll / pitch
        att_ok = att_ok && sys.attitude(1, ar, ap) && ar == v.roll && ap == v.pitch;
        min_roll = std::min(min_roll, ar);
        if (f == 1) {
            const auto wps = sys.space_path(1);
            hermite_free = !wps.empty() && distance(v.pos, eval_space_path(wps, 2.0f).pos) > 0.01f;
        }
        if (v.loco_state == LocoState::FighterIdle) break;
    }
    const UnitView v = sys.view(1);
    const FighterState* fs = sys.fighter_state(1);
    std::printf("fighter moving: idle at frame %u, %.1f from the destination\n", f, static_cast<double>(distance(v.pos, o.destination)));
    CHECK(v.arrived && !v.has_goal && v.loco_state == LocoState::FighterIdle && distance(v.pos, o.destination) < 60.0f);
    CHECK(hermite_free && speed_ok && fs && fs->dest.x == o.destination.x);
    CHECK(att_ok && min_roll < 0.0f);  // the left turn toward +y banks negative (FIGHTER_FLYING §3.2)
    float ur = 1.0f, up = 1.0f;
    CHECK(!sys.attitude(99, ur, up) && ur == 0.0f && up == 0.0f);
    for (uint32_t g = 1; g <= 30; ++g) sys.tick(f + g, rng);  // solo idle: brakes by Max_Thrust (SpaceIdleMovementSpeed 0)
    CHECK(sys.view(1).speed == 0.0f && std::fabs(sys.view(1).pitch) < 1e-3f);
}

static std::vector<ObjectId> fighter_squad(MovementSystem& sys, ObjectId container, ObjectId first, V2 at, float yaw, int32_t owner,
                                           uint32_t n) {
    FighterSquadDesc sq;
    sq.id = container;
    for (uint32_t i = 0; i < n; ++i) {
        const ObjectId id = first + static_cast<ObjectId>(i);
        const float side = (i % 2 ? 1.0f : -1.0f) * static_cast<float>((i + 1) / 2) * 30.0f;
        CHECK(sys.add_unit(fighter_desc(id, {at.x - std::fabs(side), at.y + side}, yaw, owner)));
        sq.members.push_back(id);
        sq.slots.push_back({-std::fabs(side), side, 0.0f});
    }
    CHECK(sys.set_fighter_squad(sq));
    return sq.members;
}

// §4.2 squadron idle: one parking cell, the leader orbits its centre at radius ~60, members hold their slots.
static void test_fighter_orbit() {
    MovementSystem sys(fighter_cfg());
    fighter_box(sys);
    const std::vector<ObjectId> m = fighter_squad(sys, 500, 1, {100.0f, 100.0f}, 0.0f, 0, 3);
    SimRng rng(1);
    sys.tick(0, rng);  // builds the grids
    sys.stop(m);
    const FighterSquad* sq = sys.fighter_squad(500);
    CHECK(sq && sq->park.valid());
    const F3 d = sys.fighter_state(1)->dest;
    CHECK(d == sq->dest && sys.fighter_state(2)->dest == d && sys.fighter_state(3)->dest == d);
    CHECK(sys.fighter_grids().parking.entries(sq->park).size() == 1);
    float rmin = 1e9f, rmax = 0.0f, member_max = 0.0f;
    for (uint32_t f = 1; f < 3000; ++f) {
        sys.tick(f, rng);
        if (f < 1500) continue;
        const UnitView l = sys.view(1);
        const float r = std::sqrt((l.pos.x - d.x) * (l.pos.x - d.x) + (l.pos.y - d.y) * (l.pos.y - d.y));
        rmin = std::min(rmin, r);
        rmax = std::max(rmax, r);
        for (ObjectId id : {ObjectId{2}, ObjectId{3}}) member_max = std::max(member_max, distance(sys.view(id).pos, l.pos));
    }
    std::printf("fighter orbit: leader radius %.1f..%.1f, members within %.1f of the leader\n", static_cast<double>(rmin),
                static_cast<double>(rmax), static_cast<double>(member_max));
    CHECK(rmin > 50.0f && rmax < 70.0f && member_max < 120.0f);
    CHECK(sys.view(1).loco_state == LocoState::FighterIdle && sys.view(1).speed < 0.6f);
}

// §4.4 attack run on a ship: approach, firing pass inside the strafe circle, break-off, re-attack.
static void test_fighter_attack_run() {
    MovementSystem sys(fighter_cfg());
    fighter_box(sys);
    UnitDesc ship;
    ship.id = 50;
    ship.radius = 100.0f;
    ship.max_speed = 0.0f;
    ship.owner = 1;
    CHECK(sys.add_unit(ship));
    CHECK(sys.add_unit(fighter_desc(1, {-2500.0f, 300.0f}, 0.0f, 0)));
    FighterTarget t;
    t.id = 50;
    CHECK(sys.set_fighter_target(1, t));
    SimRng rng(2);
    std::vector<FighterPhase> seq;
    uint32_t passes = 0;
    bool pass_inside = true, z_ok = true;
    for (uint32_t f = 0; f < 4000; ++f) {
        sys.tick(f, rng);
        const UnitView v = sys.view(1);
        if (seq.empty() || seq.back() != v.fighter_phase) {
            seq.push_back(v.fighter_phase);
            if (v.fighter_phase == FighterPhase::Pass) ++passes;
        }
        if (v.fighter_phase == FighterPhase::Pass || v.fighter_phase == FighterPhase::BreakOff)
            pass_inside = pass_inside && length(v.pos) <= 600.0f + 5.0f;
        z_ok = z_ok && std::fabs(v.z) < 600.0f;
    }
    // The cycle: ... Approach -> Pass -> BreakOff -> Approach -> Pass ...
    bool cycle = false;
    for (size_t i = 0; i + 3 < seq.size(); ++i)
        cycle = cycle || (seq[i] == FighterPhase::Approach && seq[i + 1] == FighterPhase::Pass &&
                          seq[i + 2] == FighterPhase::BreakOff && seq[i + 3] == FighterPhase::Approach);
    std::printf("fighter attack run: %zu phase changes, %u firing passes\n", seq.size(), passes);
    CHECK(sys.view(1).loco_state == LocoState::FighterDirectedCombat && cycle && passes >= 3 && pass_inside && z_ok);
    // The target dies: no target -> idle (solo: D = P + V).
    t.dying = true;
    CHECK(sys.set_fighter_target(1, t));
    sys.tick(4000, rng);
    CHECK(sys.view(1).loco_state == LocoState::FighterIdle);
}

// §4.5 dogfight: cell travel, provocation, the 300-frame mutual lock and the chase.
static void test_fighter_dogfight() {
    MovementSystem sys(fighter_cfg());
    fighter_box(sys);
    fighter_squad(sys, 600, 1, {-200.0f, 0.0f}, 0.0f, 0, 2);
    fighter_squad(sys, 700, 11, {200.0f, 0.0f}, 180.0f, 1, 2);
    FighterTarget ta, tb;
    ta.id = 700;
    tb.id = 600;
    CHECK(sys.set_fighter_target(600, ta) && sys.set_fighter_target(700, tb));
    SimRng rng(3);
    uint32_t lock_frame = 0, f = 0;
    ObjectId locker = -1, victim = -1;
    for (; f < 600 && locker < 0; ++f) {
        sys.tick(f, rng);
        for (ObjectId id : {ObjectId{1}, ObjectId{2}, ObjectId{11}, ObjectId{12}}) {
            const FighterState* fs = sys.fighter_state(id);
            if (fs->lock >= 0 && fs->lock_start == f) {
                locker = id;
                victim = fs->lock;
                lock_frame = f;
                break;
            }
        }
    }
    CHECK(locker >= 0);
    if (locker < 0) return;
    // Both squadrons travelled to one dogfight cell (A wrote B's cell, B joined it, then A joined at the fight).
    const FighterSquad* a = sys.fighter_squad(600);
    const FighterSquad* b = sys.fighter_squad(700);
    CHECK(a->dog.valid() && a->dog == b->dog && sys.fighter_grids().dogfight.entries(a->dog).size() == 2);
    bool held = true, victim_free = true, chased = false;
    for (f = lock_frame + 1; f < lock_frame + kFighterLockFrames; ++f) {
        sys.tick(f, rng);
        const FighterState* fa = sys.fighter_state(locker);
        const FighterState* fb = sys.fighter_state(victim);
        held = held && fa->lock == victim && fa->lock_start == lock_frame && fa->lock_frames == 300;
        victim_free = victim_free && fb->lock == -1 && fb->lock_start == lock_frame;
        chased = chased || sys.view(locker).fighter_phase == FighterPhase::Chase;
    }
    const FighterState* fa = sys.fighter_state(locker);
    std::printf("fighter dogfight: %d locked %d at frame %u (held %d, victim free %d)\n", locker, victim, lock_frame,
                held ? 1 : 0, victim_free ? 1 : 0);
    CHECK(held && victim_free && chased && fa->lock_start + fa->lock_frames == lock_frame + 300);
    // Provocation: B switches to a non-team target while still in the cell; A (lower ids, served first) orders B back.
    FighterTarget pt;
    pt.point = true;
    pt.pos = {0.0f, 3000.0f, 0.0f};
    CHECK(sys.set_fighter_target(700, pt));
    sys.tick(f++, rng);
    bool provoked = false;
    for (const FighterEvent& e : sys.fighter_events()) provoked = provoked || (e.kind == FighterEvent::Provoke && e.who == 700 && e.target == 600);
    CHECK(provoked && sys.fighter_squad(700)->target.id == 600);
    // Retarget: A (served first) loses its target inside the cell and takes the first hostile entry in join order (B).
    CHECK(sys.set_fighter_target(600, FighterTarget{}));
    sys.tick(f++, rng);
    bool retargeted = false;
    for (const FighterEvent& e : sys.fighter_events()) retargeted = retargeted || (e.kind == FighterEvent::Retarget && e.who == 600 && e.target == 700);
    CHECK(retargeted && sys.fighter_squad(600)->target.id == 700 && sys.view(1).loco_state == LocoState::FighterDirectedCombat);
    // Removing a squadron: the attacker's target becomes dying -> it retargets or idles.
    CHECK(sys.remove_fighter_squad(700) && sys.fighter_squad(600)->target.dying);
    sys.tick(f++, rng);
    CHECK(sys.view(1).loco_state == LocoState::FighterIdle && sys.fighter_grids().dogfight.entries(a->dog).empty());
}

static CrcLog fighter_crc_run(const ParallelFor& pf) {
    MovementSystem sys(fighter_cfg());
    fighter_box(sys);
    sys.space().add_static(-1, {0.0f, 1500.0f}, 300.0f, kSpaceStatic | kSpaceAsteroid);
    fighter_squad(sys, 600, 1, {-800.0f, 0.0f}, 0.0f, 0, 3);
    fighter_squad(sys, 700, 11, {800.0f, 0.0f}, 180.0f, 1, 3);
    UnitDesc ship;
    ship.id = 50;
    ship.pos = {0.0f, -1500.0f};
    ship.radius = 120.0f;
    ship.max_speed = 2.0f;
    ship.turn_rate = 1.0f;
    ship.owner = 1;
    CHECK(sys.add_unit(ship));
    CHECK(sys.add_unit(fighter_desc(30, {-2500.0f, -2500.0f}, 45.0f, 0)));
    CHECK(sys.add_unit(fighter_desc(31, {2500.0f, 2500.0f}, 225.0f, 1)));
    CrcRegistry reg;
    sys.register_crc(reg, "movement");
    CrcLog log;
    log.systems = reg.names();
    SimRng rng(17);
    FighterTarget t;
    t.id = 700;
    sys.set_fighter_target(600, t);
    t.id = 50;
    sys.set_fighter_target(30, t);
    MoveOrder o;
    o.units = {31};
    o.destination = {-2000.0f, 1500.0f};
    sys.move(o, rng, 0);
    MoveOrder os;
    os.units = {50};
    os.destination = {1500.0f, -1500.0f};
    sys.move(os, rng, 0);
    for (uint32_t f = 0; f < 900; ++f) {
        if (f == 450) {
            const ObjectId stop[] = {11, 12, 13};
            sys.stop(stop);
            sys.fighter_dead_stop(31);
        }
        sys.tick(f, rng, pf);
        log.records.push_back(reg.capture(f));
    }
    return log;
}

// Golden digest of the fighter scenario (FIGHTER_FLYING 2026-10-29); MSVC and clang must both produce it. s78f t02: the
// fighter 31's move now builds a fighter formation, hashed with the fighters ("FFOR"); without that section the digest is
// still the 2026-10-29 value 0x5a7df50c208f33b1 (the motion is unchanged).
// cu8j t01 (2026-11-08): the fighter formations compose at the next coordinator update (one draw each, first check at
// creation + 30 + r) and the hash gains the pending composition / adopt state.
static constexpr uint64_t kGoldenFighter = 0x9ffd10b74bf0428bull;

static void test_fighter_determinism() {
    const unsigned hw = std::max(2u, std::thread::hardware_concurrency());
    const CrcLog f1 = fighter_crc_run(serial_for);
    const CrcLog f4 = fighter_crc_run(make_thread_pf(4));
    const CrcLog fn = fighter_crc_run(make_thread_pf(hw));
    CHECK(!f1.records.empty() && !first_divergence(f1, f4).diverged && !first_divergence(f1, fn).diverged);
    const uint64_t g = f1.records.back().combined;
    std::printf("fighter determinism: %zu ticks final %016llx (1/4/%u threads identical)\n", f1.records.size(),
                static_cast<unsigned long long>(g), hw);
    if (kGoldenFighter != 0) CHECK(g == kGoldenFighter);
}

// ---------------------------------------------------------------- determinism
static CrcLog land_crc_run(const ParallelFor& pf) {
    MovementSystem sys{MovementConfig{}};
    build_land_scenario(sys, 36);
    CrcRegistry reg;
    sys.register_crc(reg, "movement");
    CrcLog log;
    log.systems = reg.names();
    SimRng rng(4321);
    MoveOrder o;
    o.units = sys.unit_ids();
    o.destination = cell_center(75, 80);
    o.flags = kMoveFlagQueue;
    sys.move(o, rng, 0);
    bool ground = true;
    run_land(sys, 0, 700, rng, pf, ground, &log, &reg);
    MoveOrder back;
    back.units = sys.unit_ids();
    back.destination = cell_center(12, 12);
    sys.move(back, rng, 700);
    run_land(sys, 700, 700, rng, pf, ground, &log, &reg);
    return log;
}

static CrcLog space_crc_run(const ParallelFor& pf) {
    MovementConfig cfg;
    cfg.mode = MoveMode::Space;
    MovementSystem sys(cfg);
    build_space_scenario(sys, 12);
    CrcRegistry reg;
    sys.register_crc(reg, "movement");
    CrcLog log;
    log.systems = reg.names();
    SimRng rng(9);
    MoveOrder o;
    o.units = sys.unit_ids();
    o.destination = {3800.0f, 3600.0f};
    sys.move(o, rng, 0);
    run_space(sys, 0, 1500, rng, pf, &log, &reg);
    return log;
}

// Golden digests of the final record (MSVC and clang must both produce these). OI tactical (2026-10-28): land moved
// (PathFrameDelay stagger draws on the plain return move, hash adds jiggle / formation flag / crush events), space moved
// (hash format; ops 5 / 6 and the doc's re-expansion list). S7 collision layer (2026-10-29): land moved (persistent
// collision objects hashed, priority-ordered resolution; units act on their own check frames); space unchanged.
// cu8i t02 (2026-11-08): land moved (the return move's formations draw the FormationClass ctor int(1, 30) and start at
// now + acc + r - 30, not before now); space unchanged.
// cu8j t01 (2026-11-08): both moved: the hash gains the units' pending composition / adopt state, the compositions (and
// their draws) come at the next coordinator update, land orders before frame 61 plan at 61, space ships draw per formation.
// s78n t20 F5 (2026-11-10): moved (checked orders take no cell snap; the exact single-member goal is hashed).
// s78n t20 batch A (2026-11-10): moved (the path stack top, the destination check, Path Around, the early-arrival cut,
// StoppedVsStopped; MOVEMENT 1.5.1 / 4.3.1 / 5.4.3 - 5.4.5). s78n t20 batch 2 item 2.1 (2026-11-10): moved - the return
// move's 11 formations fail all 41 passes of the destination check and now take its forced pass (MOVEMENT 4.3.1 item 3)
// instead of keeping the ordered point.
// s78n t20 batch 1 item 1.4 (2026-11-10): moved (the early-arrival cut 0x54e780 on the path stack top, MOVEMENT 5.4.4
// item 5); merged with batch 2.
// s78n t23 (2026-11-10): land moved (MovingVsMoving chooser and states for every pair, MOVEMENT 5.4.2 / 5.4.6; the
// finished records leave the speed fold).
// s78n t23 review fix 1: the re-decide stamp by role (MOVEMENT 5.4.1 "The stamp in full" items 2 - 3).
// s78n t23 review fix 2: a re-decided record waits (case 0 in its stamped frame; a set-up decision under quota).
// s78n t23 follow-ups (MOVEMENT 5.4.2 / 5.4.6 "State 2 service", RE 2): MovingVsMoving answers start at 0; case 6 resets
// before its chooser re-run.
static constexpr uint64_t kGoldenLand = 0x8b3364785ab4958full;  // s78n t23 follow-ups
static constexpr uint64_t kGoldenSpace = 0xd701881d8713d135ull;

static void test_determinism() {
    const unsigned hw = std::max(2u, std::thread::hardware_concurrency());
    const CrcLog l1 = land_crc_run(serial_for);
    const CrcLog l4 = land_crc_run(make_thread_pf(4));
    const CrcLog ln = land_crc_run(make_thread_pf(hw));
    CHECK(!l1.records.empty());
    CHECK(!first_divergence(l1, l4).diverged && !first_divergence(l1, ln).diverged);
    const CrcLog s1 = space_crc_run(serial_for);
    const CrcLog s4 = space_crc_run(make_thread_pf(4));
    const CrcLog sn = space_crc_run(make_thread_pf(hw));
    CHECK(!s1.records.empty());
    CHECK(!first_divergence(s1, s4).diverged && !first_divergence(s1, sn).diverged);
    const uint64_t gl = l1.records.back().combined, gs = s1.records.back().combined;
    std::printf("determinism: land %zu ticks final %016llx, space %zu ticks final %016llx (1/4/%u threads identical)\n",
                l1.records.size(), static_cast<unsigned long long>(gl), s1.records.size(), static_cast<unsigned long long>(gs), hw);
    if (kGoldenLand != 0) CHECK(gl == kGoldenLand);
    if (kGoldenSpace != 0) CHECK(gs == kGoldenSpace);
}

// ---------------------------------------------------------------- real data
#ifdef MOVEMENT_REAL_DATA
static void test_real_data() {
    const char* root = std::getenv("ALAMO_FOC_DATA");
    if (!root || !*root) {
        std::printf("real data: ALAMO_FOC_DATA not set, skipped\n");
        return;
    }
    vfs::VfsConfig cfg;
    cfg.foc_root = root;
    std::string err;
    auto fs = vfs::Vfs::create(cfg, &err);
    CHECK(fs != nullptr);
    if (!fs) return;
    const auto t0 = std::chrono::steady_clock::now();
    MovementConfig mc;
    {
        std::vector<uint8_t> bytes;
        CHECK(fs->read("Data\\XML\\MovementClassTypeDefs.xml", bytes));
        CHECK(mc.classes.load_xml(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size())));
        const MovementClassTable vanilla;
        for (int32_t c = 0; c < 18; ++c) CHECK(mc.classes.mask(c) == vanilla.mask(c));
    }
    objects::ObjectTypeDatabase raw;
    objects::GameObjectTypes types;
    const bool have_types = raw.load(*fs) && types.build(raw, fs.get());
    CHECK(have_types);
    if (have_types) {  // S7 t06: Uses_Multiple_Locomotors type data (FoC: the jetpack users; Bossk has no secondary)
        const auto desc = [&types](const char* name) { return unit_desc_from_type(types, types.resolve_name(name)); };
        const UnitDesc dt2 = desc("Dark_Trooper_PhaseII"), boba = desc("Boba_Fett"), bossk = desc("Bossk");
        const UnitDesc dt1 = desc("Dark_Trooper_PhaseI");
        CHECK(dt2.uses_multiple_locomotors && dt2.has_secondary_locomotor && dt2.secondary_locomotor == LocomotorKind::Jetpack &&
              dt2.max_speed == 1.0f && dt2.alt_max_speed == 2.5f && dt2.alt_turn_rate == 2.0f && dt2.hover_offset == 70.0f);  // t14: unset turn = 2.0
        CHECK(boba.uses_multiple_locomotors && boba.secondary_locomotor == LocomotorKind::Jetpack && boba.alt_max_speed == 2.5f &&
              boba.alt_turn_rate == 6.0f);
        CHECK(bossk.uses_multiple_locomotors && !bossk.has_secondary_locomotor);
        const UnitDesc dt3 = desc("Dark_Trooper_PhaseIII");  // S7 t14: Alternate 1.8, turn unset -> 2.0 (MOVEMENT §3.1.1)
        CHECK(dt3.has_secondary_locomotor && dt3.alt_max_speed == 1.8f && dt3.alt_turn_rate == 2.0f);
        CHECK(!dt1.uses_multiple_locomotors && !dt1.has_secondary_locomotor);
        CHECK(locomotor_kind_from_type(types, types.resolve_name("Dark_Trooper_PhaseII"), MoveMode::Land) == LocomotorKind::Default);
        // s78n t18 review (MOVEMENT 5.3 "Hover state"): the FoC hover types are the Lancet and the HTT (Hover_Height 80, Walk
        // locomotor: raised from their first move on - no MovingVsStopped record against grounded standing units, no
        // MovingVsMoving record with grounded movers). MAL_Rocket_Vehicle's tag is commented out in
        // UNITS_LAND_UNDERWORLD_MAL.XML (<!--<Hover_Height>3.0</Hover_Height> -->): not read, the MAL does not hover.
        const UnitDesc mal = desc("MAL_Rocket_Vehicle"), lancet = desc("Lancet_Air_Artillery"), htt = desc("Gallofree_HTT_Transport");
        CHECK(mal.hover_height == 0.0f && lancet.hover_height == 80.0f && htt.hover_height == 80.0f);
        for (const char* name : {"MAL_Rocket_Vehicle", "Lancet_Air_Artillery", "Gallofree_HTT_Transport"}) {
            CHECK(types.resolve_name(name) < types.size());
            CHECK(locomotor_kind_from_type(types, types.resolve_name(name), MoveMode::Land) == LocomotorKind::Default);
        }
    }
    const auto maps = fs->list("Data\\Art\\Maps\\*.ted");
    uint32_t land_maps = 0, space_maps = 0, land_units = 0, land_arrived = 0, off_ground = 0, stamped_total = 0;
    uint32_t space_units = 0, space_arrived = 0, space_hits = 0, space_obstacles = 0, plan_fail = 0, plan_invalid = 0, plans = 0;
    uint32_t group_units = 0, group_arrived = 0, space_fallbacks = 0;
    for (const std::string& path : maps) {
        std::vector<uint8_t> bytes;
        terrain::MapData map;
        if (!fs->read(path, bytes) || !terrain::parse_ted(bytes, map)) continue;
        if (!map.is_space()) {
            if (map.terrain.width < 8) continue;
            ++land_maps;
            PassabilityGrid g = grid_from_map(map);
            if (have_types) stamped_total += stamp_map_obstacles(g, map, types);
            MovementSystem sys(mc);
            sys.set_grid(std::move(g));
            const PassabilityGrid& grid = sys.grid();
            const RegionMap& rm = sys.regions();
            SimRng r(name_crc(path));
            const int32_t classes[3] = {0, 7, 9};
            ObjectId next_id = 1;
            std::vector<std::pair<ObjectId, V2>> goals;
            LandPathScratch sc;
            for (int32_t cls : classes) {
                const uint32_t mask = mc.classes.mask(cls);
                const int32_t slot = rm.mask_slot(mask);
                for (int k = 0; k < 8; ++k) {
                    int32_t sx = 0, sy = 0, tx = 0, ty = 0;
                    bool found = false;
                    for (int tries = 0; tries < 4000 && !found; ++tries) {
                        sx = r.range_int(0, static_cast<int32_t>(grid.width()) - 2);
                        sy = r.range_int(0, static_cast<int32_t>(grid.height()) - 2);
                        tx = r.range_int(0, static_cast<int32_t>(grid.width()) - 2);
                        ty = r.range_int(0, static_cast<int32_t>(grid.height()) - 2);
                        const int32_t a = rm.component_at(slot, sx, sy), b = rm.component_at(slot, tx, ty);
                        found = a >= 0 && a == b && grid.passable_cell(sx, sy, mask) && grid.passable_cell(tx, ty, mask) &&
                                (std::abs(sx - tx) + std::abs(sy - ty)) > 20;
                    }
                    if (!found) continue;
                    LandPathRequest q;
                    q.start = cell_center(sx, sy);
                    q.goal = cell_center(tx, ty);
                    q.mask = mask;
                    LandPath p;
                    ++plans;
                    if (!find_land_path(grid, rm, q, sc, p)) {
                        ++plan_fail;
                        continue;
                    }
                    if (!path_valid(grid, p.points, mask)) ++plan_invalid;
                    UnitDesc d = land_unit(next_id, q.start, cls, cls == 0 ? 1.0f : 1.6f, cls == 0 ? 6.0f : 16.0f,
                                           cls == 0 ? kUccInfantry : kUccVehicle);
                    sys.add_unit(d);
                    goals.push_back({next_id, q.goal});
                    ++next_id;
                }
            }
            for (const auto& [id, goal] : goals) {
                MoveOrder o;
                o.units = {id};
                o.destination = goal;
                sys.move(o, r, 0);
            }
            bool ground = true;
            SimRng tick_rng(1);
            run_land(sys, 0, 12000, tick_rng, serial_for, ground);
            if (!ground) ++off_ground;
            uint32_t arrived = 0;
            for (const auto& [id, goal] : goals) {
                const UnitView v = sys.view(id);
                // Arrived at the goal, or early at a blocked destination (contact + LandDestinationProximity).
                if (v.arrived && distance(v.pos, goal) < 160.0f) ++arrived;
            }
            land_units += static_cast<uint32_t>(goals.size());
            land_arrived += arrived;
            if (arrived != goals.size()) {
                std::printf("  %s: %u / %zu arrived (paths %u, replans %u, failures %u)\n", path.c_str(), arrived, goals.size(),
                            sys.stats().paths_planned, sys.stats().replans, sys.stats().path_failures);
                for (const auto& [id, goal] : goals) {
                    const UnitView v = sys.view(id);
                    if (v.arrived && distance(v.pos, goal) < 160.0f) continue;
                    std::printf("    unit %d mask %x at (%.1f,%.1f) cell %d,%d goal (%.1f,%.1f) used (%.1f,%.1f) arrived %d failed %d replans %u\n",
                                id, sys.mask_of(id), static_cast<double>(v.pos.x), static_cast<double>(v.pos.y), world_to_cell(v.pos.x),
                                world_to_cell(v.pos.y), static_cast<double>(goal.x), static_cast<double>(goal.y),
                                static_cast<double>(v.goal.x), static_cast<double>(v.goal.y), v.arrived ? 1 : 0, v.failed ? 1 : 0, v.replans);
                }
            }
            // Group move: 12 infantry from the first unit's start to the last unit's goal.
            if (!goals.empty()) {
                MovementSystem gs(mc);
                gs.set_grid(grid_from_map(map));
                if (have_types) stamp_map_obstacles(gs.grid(), map, types);
                gs.rebuild_regions_if_dirty();
                const V2 s0 = sys.view(goals.front().first).goal;  // arrival point of unit 1 (passable)
                const uint32_t mask = mc.classes.mask(0);
                const int32_t slot = gs.regions().mask_slot(mask);
                const int32_t comp = gs.regions().component_at(slot, world_to_cell(s0.x), world_to_cell(s0.y));
                std::vector<ObjectId> ids;
                for (int32_t dy = -3; dy <= 3 && ids.size() < 12; ++dy)
                    for (int32_t dx = -3; dx <= 3 && ids.size() < 12; ++dx) {
                        const int32_t cx = world_to_cell(s0.x) + dx * 2, cy = world_to_cell(s0.y) + dy * 2;
                        if (!gs.grid().passable_cell(cx, cy, mask) || gs.regions().component_at(slot, cx, cy) != comp) continue;
                        const ObjectId id = static_cast<ObjectId>(100 + ids.size());
                        gs.add_unit(land_unit(id, cell_center(cx, cy), 0, 1.0f, 6.0f, kUccInfantry));
                        ids.push_back(id);
                    }
                // Destination: a far passable cell of the same component.
                V2 dst = s0;
                for (int tries = 0; tries < 4000; ++tries) {
                    const int32_t tx = r.range_int(0, static_cast<int32_t>(grid.width()) - 2);
                    const int32_t ty = r.range_int(0, static_cast<int32_t>(grid.height()) - 2);
                    if (gs.regions().component_at(slot, tx, ty) == comp && distance(cell_center(tx, ty), s0) > 1500.0f) {
                        dst = cell_center(tx, ty);
                        break;
                    }
                }
                MoveOrder o;
                o.units = ids;
                o.destination = dst;
                gs.move(o, r, 0);
                bool gground = true;
                run_land(gs, 0, 12000, tick_rng, serial_for, gground);
                if (!gground) ++off_ground;
                uint32_t ga = 0;
                for (ObjectId id : ids)
                    if (gs.view(id).arrived) ++ga;
                group_units += static_cast<uint32_t>(ids.size());
                group_arrived += ga;
                if (ga != ids.size()) {
                    std::printf("  %s: group %u / %zu arrived\n", path.c_str(), ga, ids.size());
                    if (std::getenv("MOVEMENT_DEBUG"))
                        for (ObjectId id : ids) {
                            const UnitView v = gs.view(id);
                            if (v.arrived) continue;
                            std::printf("    unit %d at (%.1f,%.1f) goal (%.1f,%.1f) has_goal %d failed %d speed %.2f replans %u, "
                                        "collisions %zu\n",
                                        id, static_cast<double>(v.pos.x), static_cast<double>(v.pos.y), static_cast<double>(v.goal.x),
                                        static_cast<double>(v.goal.y), v.has_goal ? 1 : 0, v.failed ? 1 : 0,
                                        static_cast<double>(v.speed), v.replans, gs.collisions().list().size());
                            for (const CollisionObject& c : gs.collisions().list())
                                std::printf("      collision %u kind %u state %u %d-%d created %u retry %u\n", c.seq,
                                            static_cast<unsigned>(c.kind), static_cast<unsigned>(c.state), c.a, c.b, c.created, c.retry);
                        }
                }
            }
        } else {
            ++space_maps;
            MovementConfig scfg = mc;
            scfg.mode = MoveMode::Space;
            MovementSystem sys(scfg);
            space_obstacles += space_from_map(sys.space(), map, have_types ? &types : nullptr);
            const SpaceOccupancy& occ = sys.space();
            SimRng r(name_crc(path));
            const V2 bmin = occ.box_min(), bmax = occ.box_max();
            auto random_free = [&](float rad, V2& out) {
                for (int tries = 0; tries < 2000; ++tries) {
                    const V2 p{bmin.x + (bmax.x - bmin.x) * (0.1f + 0.8f * r.inline_unit_float()),
                               bmin.y + (bmax.y - bmin.y) * (0.1f + 0.8f * r.inline_unit_float())};
                    if (occ.query_static(p, p, rad * 2.0f) == 0) {
                        out = p;
                        return true;
                    }
                }
                return false;
            };
            std::vector<ObjectId> ids;
            for (uint32_t i = 0; i < 8; ++i) {
                UnitDesc d;
                d.id = static_cast<ObjectId>(1 + i);
                d.max_speed = i % 2 ? 3.0f : 5.0f;
                d.turn_rate = i % 2 ? 0.6f : 2.0f;
                d.radius = i % 2 ? 100.0f : 40.0f;
                d.space_layer = i % 2 ? kLayerCapital : kLayerCorvette;
                if (!random_free(d.radius, d.pos)) continue;
                sys.add_unit(d);
                ids.push_back(d.id);
            }
            for (ObjectId id : ids) {
                V2 g;
                if (!random_free(100.0f, g)) continue;
                MoveOrder o;
                o.units = {id};
                o.destination = g;
                sys.move(o, r, 0);
            }
            SimRng tick_rng(2);
            std::vector<V2> start(ids.size());
            for (size_t i = 0; i < ids.size(); ++i) start[i] = sys.view(ids[i]).pos;
            for (uint32_t f = 0; f < 15000; ++f) {
                sys.tick(f, tick_rng, serial_for);
                bool any = false;
                for (size_t i = 0; i < ids.size(); ++i) {
                    const UnitView v = sys.view(ids[i]);
                    any = any || v.has_goal;
                    // The ship's centre never enters a static obstacle (unless it started inside one).
                    if (occ.query_static(v.pos, v.pos, 0.0f) != 0 && occ.query_static(start[i], start[i], 0.0f) == 0) {
                        ++space_hits;
                        if (std::getenv("MOVEMENT_DEBUG"))
                            std::printf("  %s ship %d frame %u at (%.0f,%.0f) %.0f from start, fallbacks %u, wps %zu\n", path.c_str(),
                                        ids[i], f, static_cast<double>(v.pos.x), static_cast<double>(v.pos.y),
                                        static_cast<double>(distance(v.pos, start[i])), sys.stats().space_fallbacks,
                                        sys.space_path(ids[i]).size());
                    }
                }
                if (!any) break;
            }
            for (size_t i = 0; i < ids.size(); ++i) {
                const UnitView v = sys.view(ids[i]);
                if (v.arrived) ++space_arrived;
                if (v.space_fallback) {
                    ++space_fallbacks;
                    if (std::getenv("MOVEMENT_DEBUG"))
                        std::printf("  %s ship %d fell back: start (%.0f,%.0f) goal (%.0f,%.0f)\n", path.c_str(), ids[i],
                                    static_cast<double>(start[i].x), static_cast<double>(start[i].y), static_cast<double>(v.goal.x),
                                    static_cast<double>(v.goal.y));
                }
            }
            space_units += static_cast<uint32_t>(ids.size());
        }
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("real maps: %u land maps (%u obstacle stamps), %u plans (%u failed, %u invalid), %u / %u units arrived, "
                "group %u / %u, %u maps left passable ground; %u space maps (%u static obstacles), %u / %u ships arrived, "
                "%u obstacle-centre frames, %u planner fallbacks; %.1f s\n",
                land_maps, stamped_total, plans, plan_fail, plan_invalid, land_arrived, land_units, group_arrived, group_units,
                off_ground, space_maps, space_obstacles, space_arrived, space_units, space_hits, space_fallbacks, secs);
    CHECK(land_maps >= 40 && space_maps >= 20);
    CHECK(plan_fail == 0 && plan_invalid == 0 && off_ground == 0);
    CHECK(land_arrived == land_units && group_arrived == group_units);
    CHECK(space_arrived == space_units && space_hits == 0);
}
#endif

#ifdef ALAMO_MOVEMENT_HAVE_TERRAIN
// S7 t07 / t08 (MOVEMENT §2.2, SPACE_AVOIDANCE §5.5): SPACE_OBSTACLE footprints. XML radius first (Custom_Soft_Footprint_Radius,
// then Space_Obstacle_Radius), else the larger model half extent from the host hook, x Scale_Factor; no radius and no model
// data: no entry. A capture-point pod is an obstacle like any other (the collide switch is not consulted). Map placements
// add Space_Obstacle_Offset along the placement facing Rz(yaw) (0, -1).
static void test_space_obstacle_footprint() {
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "alamo_tactical_movement_space_obstacle";
    std::error_code ec;
    remove_all_long(root, ec);
    auto put = [](const std::filesystem::path& p, const std::string& s) {
        std::filesystem::create_directories(p.parent_path());
        std::ofstream(p, std::ios::binary) << s;
    };
    put(root / "Data/XML/GameObjectFiles.xml", "<L><File>SO.xml</File></L>");
    put(root / "Data/XML/SO.xml",
        "<R>\n"
        "<SpaceStructure Name=\"Pod\"><Behavior>SPACE_OBSTACLE</Behavior><Scale_Factor>1.75</Scale_Factor></SpaceStructure>\n"
        "<SpaceStructure Name=\"Cap_Pod\"><Behavior>SPACE_OBSTACLE, CAPTURE_POINT</Behavior><Space_Layer>StaticObject</Space_Layer>"
        "</SpaceStructure>\n"
        "<SpaceProp Name=\"Rock\"><Behavior>SPACE_OBSTACLE</Behavior><Is_Impassable_Asteroid>true</Is_Impassable_Asteroid>"
        "<Space_Obstacle_Radius>300</Space_Obstacle_Radius><Space_Obstacle_Offset>100 0 0</Space_Obstacle_Offset></SpaceProp>\n"
        "<SpaceStructure Name=\"Soft\"><Behavior>SPACE_OBSTACLE</Behavior><Custom_Soft_Footprint_Radius>60</Custom_Soft_Footprint_Radius>"
        "<Space_Obstacle_Radius>300</Space_Obstacle_Radius><Scale_Factor>2</Scale_Factor></SpaceStructure>\n"
        "<SpaceUnit Name=\"Ship\"><Space_Layer>Frigate</Space_Layer></SpaceUnit>\n"
        "</R>");
    DirectoryProvider dp(path_to_utf8(root));
    objects::ObjectTypeDatabase raw;
    objects::GameObjectTypes types;
    CHECK(raw.load(dp) && types.build(raw, &dp));
    const uint32_t pod = types.resolve_name("Pod"), cap = types.resolve_name("Cap_Pod"), rock = types.resolve_name("Rock"),
                   soft = types.resolve_name("Soft"), ship = types.resolve_name("Ship");
    CHECK(pod < types.size() && cap < types.size() && rock < types.size() && soft < types.size() && ship < types.size());
    if (pod >= types.size() || cap >= types.size() || rock >= types.size() || soft >= types.size() || ship >= types.size()) return;
    uint32_t hook_calls = 0;
    SpaceObstacleHooks hooks;
    hooks.model_half_extents = [&hook_calls, pod, cap](uint32_t t, float& hx, float& hy) {
        ++hook_calls;
        if (t == pod) {
            hx = 40.0f, hy = -55.0f;  // the larger |half extent| decides
            return true;
        }
        if (t == cap) {
            hx = hy = 1.0f;  // a model without a collision mesh: centre 0, half 1
            return true;
        }
        return false;
    };
    float r = 0.0f;
    uint32_t bits = 0;
    CHECK(!space_obstacle_footprint(types, pod, r, bits));  // no XML radius, no model data: skipped (as before)
    CHECK(space_obstacle_footprint(types, pod, r, bits, &hooks) && r == 55.0f * 1.75f && bits == kSpaceStatic);
    CHECK(space_obstacle_footprint(types, cap, r, bits, &hooks) && r == 1.0f && bits == kSpaceStatic);
    hook_calls = 0;
    CHECK(space_obstacle_footprint(types, rock, r, bits, &hooks) && r == 300.0f && bits == (kSpaceStatic | kSpaceImpassableAsteroid));
    CHECK(space_obstacle_footprint(types, soft, r, bits, &hooks) && r == 120.0f);  // Custom_Soft_Footprint_Radius first
    CHECK(hook_calls == 0);  // an XML radius never asks for the model
    CHECK(!space_obstacle_footprint(types, ship, r, bits, &hooks));  // no SPACE_OBSTACLE behaviour
    SpaceObstacleHooks unknown;
    unknown.model_half_extents = [](uint32_t, float&, float&) { return false; };
    CHECK(!space_obstacle_footprint(types, pod, r, bits, &unknown));
    // Map placements.
    terrain::MapData map;
    auto place = [&](uint32_t t, float x, float y, float yaw) {
        terrain::ObjectPlacement o;
        o.type_crc = types.raw().types().name_crc[t];
        o.position[0] = x, o.position[1] = y;
        o.rotation_deg[2] = yaw;
        map.objects.push_back(o);
    };
    place(pod, 0.0f, 0.0f, 0.0f);         // id -1, r 96.25 at (0, 0)
    place(rock, 1000.0f, 0.0f, 90.0f);    // id -2, facing (1, 0): centre (1100, 0)
    place(rock, -1000.0f, 0.0f, 0.0f);    // id -3, facing (0, -1): centre (-1000, -100)
    place(ship, 0.0f, 2000.0f, 0.0f);     // not an obstacle
    SpaceOccupancy plain;
    CHECK(space_from_map(plain, map, &types) == 2);  // the pod needs model data
    CHECK(!plain.has_static(-1) && plain.has_static(-2) && plain.has_static(-3));
    SpaceOccupancy occ;
    CHECK(space_from_map(occ, map, &types, &hooks) == 3);
    CHECK(occ.has_static(-1) && occ.has_static(-2) && occ.has_static(-3) && !occ.has_static(-4));
    CHECK((occ.query_static({90.0f, 0.0f}, {90.0f, 0.0f}, 1.0f) & kSpaceStatic) != 0);   // inside 96.25
    CHECK(occ.query_static({100.0f, 0.0f}, {100.0f, 0.0f}, 1.0f) == 0);                  // outside 96.25 + 1
    CHECK((occ.query_static({1390.0f, 0.0f}, {1390.0f, 0.0f}, 1.0f) & kSpaceImpassableAsteroid) != 0);  // 1100 + 290
    CHECK(occ.query_static({790.0f, 0.0f}, {790.0f, 0.0f}, 1.0f) == 0);  // 310 from the offset centre
    CHECK((occ.query_static({-1000.0f, -390.0f}, {-1000.0f, -390.0f}, 1.0f) & kSpaceStatic) != 0);
    CHECK(occ.query_static({-1000.0f, 210.0f}, {-1000.0f, 210.0f}, 1.0f) == 0);
    remove_all_long(root, ec);
    std::printf("space obstacle footprint: XML radius / model extent / offset placements ok\n");
}
#endif

#ifdef ALAMO_MOVEMENT_HAVE_TERRAIN
// S8 t02 (MOVEMENT §1.3): one object's LAND_OBSTACLE footprint (land_obstacle_footprint) is the footprint stamp_map_obstacles
// gives a placement of the type at the same spot (XML size, else model extents; none without either); the stamp ids of
// both map adapters are map_stamp_id(placement).
static void test_land_obstacle_footprint() {
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "alamo_tactical_movement_land_obstacle";
    std::error_code ec;
    remove_all_long(root, ec);
    auto put = [](const std::filesystem::path& p, const std::string& s) {
        std::filesystem::create_directories(p.parent_path());
        std::ofstream(p, std::ios::binary) << s;
    };
    put(root / "Data/XML/GameObjectFiles.xml", "<L><File>LO.xml</File></L>");
    put(root / "Data/XML/LO.xml",
        "<R>\n"
        "<GroundStructure Name=\"Bunker\"><LandBehavior>LAND_OBSTACLE</LandBehavior><Obstacle_Width>60</Obstacle_Width>"
        "<Obstacle_Height>40</Obstacle_Height><Obstacle_X_Offset>-30</Obstacle_X_Offset><Obstacle_Y_Offset>-20</Obstacle_Y_Offset>"
        "<OverridePassability>RebelWall</OverridePassability></GroundStructure>\n"
        "<GroundStructure Name=\"Modelled\"><LandBehavior>LAND_OBSTACLE</LandBehavior><Scale_Factor>2</Scale_Factor></GroundStructure>\n"
        "<GroundStructure Name=\"Bare\"><LandBehavior>LAND_OBSTACLE</LandBehavior></GroundStructure>\n"
        // S8 t09: a build pad (proxy target), a barracks stamped through it, a type without a land model, fine-tune off
        "<GroundStructure Name=\"Pad\"><LandBehavior>LAND_OBSTACLE</LandBehavior><Obstacle_Width>40</Obstacle_Width>"
        "<Obstacle_Height>40</Obstacle_Height><Obstacle_X_Offset>-20</Obstacle_X_Offset><Obstacle_Y_Offset>-20</Obstacle_Y_Offset>"
        "<Scale_Factor>3</Scale_Factor></GroundStructure>\n"
        "<GroundStructure Name=\"Barracks\"><LandBehavior>LAND_OBSTACLE</LandBehavior><Obstacle_Width>200</Obstacle_Width>"
        "<Obstacle_Height>200</Obstacle_Height><Obstacle_X_Offset>-100</Obstacle_X_Offset><Obstacle_Y_Offset>-100</Obstacle_Y_Offset>"
        "<Obstacle_Proxy_Type>Pad</Obstacle_Proxy_Type><OverridePassability>RebelWall</OverridePassability>"
        "<Scale_Factor>1.5</Scale_Factor></GroundStructure>\n"
        "<GroundStructure Name=\"NoModel\"><LandBehavior>LAND_OBSTACLE</LandBehavior><Obstacle_Width>40</Obstacle_Width>"
        "<Obstacle_Height>40</Obstacle_Height></GroundStructure>\n"
        "<GroundStructure Name=\"FineOff\"><LandBehavior>LAND_OBSTACLE</LandBehavior><Obstacle_Width>40</Obstacle_Width>"
        "<Obstacle_Height>40</Obstacle_Height><Fine_Tune_Occupied_Passability>No</Fine_Tune_Occupied_Passability></GroundStructure>\n"
        // S8 t13: no Land_Model_Name with fine-tune off (both sizes / one size), one XML size + model box, a wall, a proxy
        // with its own OverridePassability
        "<GroundStructure Name=\"OffBoth\"><LandBehavior>LAND_OBSTACLE</LandBehavior><Obstacle_Width>40</Obstacle_Width>"
        "<Obstacle_Height>20</Obstacle_Height><Obstacle_X_Offset>-20</Obstacle_X_Offset><Obstacle_Y_Offset>-10</Obstacle_Y_Offset>"
        "<Fine_Tune_Occupied_Passability>No</Fine_Tune_Occupied_Passability></GroundStructure>\n"
        "<GroundStructure Name=\"OffOne\"><LandBehavior>LAND_OBSTACLE</LandBehavior><Obstacle_Width>40</Obstacle_Width>"
        "<Fine_Tune_Occupied_Passability>No</Fine_Tune_Occupied_Passability></GroundStructure>\n"
        "<GroundStructure Name=\"WidthOnly\"><LandBehavior>LAND_OBSTACLE</LandBehavior><Obstacle_Width>80</Obstacle_Width>"
        "</GroundStructure>\n"
        "<GroundStructure Name=\"WallSeg\"><LandBehavior>LAND_OBSTACLE, WALL</LandBehavior><Obstacle_Width>40</Obstacle_Width>"
        "<Obstacle_Height>40</Obstacle_Height></GroundStructure>\n"
        "<GroundStructure Name=\"ShieldPad\"><LandBehavior>LAND_OBSTACLE</LandBehavior><Obstacle_Width>40</Obstacle_Width>"
        "<Obstacle_Height>40</Obstacle_Height><OverridePassability>Shield</OverridePassability></GroundStructure>\n"
        "<GroundStructure Name=\"PadUser\"><LandBehavior>LAND_OBSTACLE</LandBehavior><Obstacle_Proxy_Type>ShieldPad</Obstacle_Proxy_Type>"
        "</GroundStructure>\n"
        "</R>");
    DirectoryProvider dp(path_to_utf8(root));
    objects::ObjectTypeDatabase raw;
    objects::GameObjectTypes types;
    CHECK(raw.load(dp) && types.build(raw, &dp));
    const uint32_t bunker = types.resolve_name("Bunker"), modelled = types.resolve_name("Modelled"), bare = types.resolve_name("Bare");
    CHECK(bunker < types.size() && modelled < types.size() && bare < types.size());
    if (bunker >= types.size() || modelled >= types.size() || bare >= types.size()) return;
    MapStampHooks hooks;
    hooks.model_half_extents = [modelled](uint32_t t, float& hx, float& hy, float& hz) {
        if (t != modelled) return false;
        hx = 15.0f, hy = -10.0f, hz = 8.0f;
        return true;
    };
    ObstacleFootprint fp;
    CHECK(land_obstacle_footprint(types, bunker, {200.0f, 200.0f}, 90.0f, 3.0f, nullptr, fp));
    CHECK(fp.width == 60.0f && fp.height == 40.0f && fp.x_offset == -30.0f && fp.y_offset == -20.0f && fp.bits == kPassRebelWall &&
          fp.yaw_deg == 90.0f && fp.terrain_z == 3.0f && fp.obstacle_height == 1.0e6f && !fp.fine_tune);
    CHECK(!land_obstacle_footprint(types, modelled, {0.0f, 0.0f}, 0.0f, 0.0f, nullptr, fp));  // no XML size, no model data
    CHECK(land_obstacle_footprint(types, modelled, {300.0f, 100.0f}, 0.0f, 0.0f, &hooks, fp));
    CHECK(fp.width == 60.0f && fp.height == 40.0f && fp.x_offset == -30.0f && fp.y_offset == -20.0f && fp.bits == kPassObstacle &&
          fp.obstacle_height == 16.0f);
    CHECK(!land_obstacle_footprint(types, bare, {0.0f, 0.0f}, 0.0f, 0.0f, &hooks, fp));
    CHECK(!land_obstacle_footprint(types, static_cast<uint32_t>(types.size()), {0.0f, 0.0f}, 0.0f, 0.0f, &hooks, fp));
    // The same cells as the map adapter's stamp of a placement there.
    terrain::MapData map;
    auto place = [&](uint32_t t, float x, float y, float yaw) {
        terrain::ObjectPlacement o;
        o.type_crc = types.raw().types().name_crc[t];
        o.position[0] = x, o.position[1] = y;
        o.rotation_deg[2] = yaw;
        map.objects.push_back(o);
    };
    place(bare, 100.0f, 100.0f, 0.0f);    // no footprint: not stamped
    place(bunker, 200.0f, 200.0f, 30.0f);  // stamp id map_stamp_id(1) = -2
    place(modelled, 300.0f, 100.0f, 0.0f);
    PassabilityGrid g;
    g.init(32, 32);
    CHECK(stamp_map_obstacles(g, map, types, &hooks) == 2);
    CHECK(map_stamp_id(0) == -1 && map_stamp_id(1) == -2 && !g.stamped(map_stamp_id(0)) && g.stamped(map_stamp_id(1)) &&
          g.stamped(map_stamp_id(2)));
    PassabilityGrid h;
    h.init(32, 32);
    CHECK(land_obstacle_footprint(types, bunker, {200.0f, 200.0f}, 30.0f, 0.0f, &hooks, fp) && h.stamp(41, fp) > 0);
    const std::span<const uint32_t> a = g.stamp_cells(map_stamp_id(1)), b = h.stamp_cells(41);
    CHECK(!a.empty() && std::equal(a.begin(), a.end(), b.begin(), b.end()));
    CHECK(g.rekey(map_stamp_id(1), 41) && g.unstamp(map_stamp_id(2)));
    StateHasher hg, hh;
    g.hash(hg);
    h.hash(hh);
    CHECK(hg.digest() == hh.digest());  // the placement's stamp handed to its object = the object stamped at its spot

    // S8 t09 (MOVEMENT §1.3): Fine_Tune_Occupied_Passability on every stamp (one stamper for placements and run-time
    // objects), Obstacle_Proxy_Type, and the "fine-tune, no proxy, no model: not stamped" rule.
    const uint32_t pad = types.resolve_name("Pad"), barracks = types.resolve_name("Barracks"), nomodel = types.resolve_name("NoModel"),
                   fineoff = types.resolve_name("FineOff");
    CHECK(pad < types.size() && barracks < types.size() && nomodel < types.size() && fineoff < types.size());
    if (pad >= types.size() || barracks >= types.size() || nomodel >= types.size() || fineoff >= types.size()) return;
    CHECK(land_obstacle_footprint_type(types, barracks) == pad && land_obstacle_footprint_type(types, pad) == pad &&
          land_obstacle_footprint_type(types, modelled) == modelled);
    bool pad_has_model = true;
    uint32_t ray_type = 0xFFFFFFFFu;
    float ray_scale = 0.0f;
    uint32_t rays = 0;
    MapStampHooks ft = hooks;
    ft.ray_model = [&](uint32_t t) {
        if (t == modelled || t == fineoff) return StampRayModel::Model;
        if (t == pad) return pad_has_model ? StampRayModel::Model : StampRayModel::None;
        if (t == nomodel) return StampRayModel::None;
        return StampRayModel::Unknown;
    };
    // Fake collision mesh: the half of the model on the -x side of its origin (world x < object x at yaw 0).
    ft.mesh_ray_hit = [&](uint32_t t, const StampPose& pose, V2 xy) {
        ray_type = t;
        ray_scale = pose.scale;
        ++rays;
        return xy.x < pose.position.x;
    };
    // A run-time stamp of the modelled type is trimmed by the ray.
    ObstacleFootprint full, trimmed;
    CHECK(land_obstacle_footprint(types, modelled, {300.0f, 300.0f}, 0.0f, 0.0f, &hooks, full) && !full.fine_tune);
    CHECK(land_obstacle_footprint(types, modelled, {300.0f, 300.0f}, 0.0f, 0.0f, &ft, trimmed) && trimmed.fine_tune);
    PassabilityGrid rt;
    rt.init(32, 32);
    const uint32_t n_full = rt.stamp(50, full), n_trim = rt.stamp(51, trimmed);
    CHECK(rays > 0 && ray_type == modelled && ray_scale == 2.0f);
    CHECK(n_trim > 0 && n_trim < n_full);
    for (uint32_t c : rt.stamp_cells(51)) CHECK(static_cast<int32_t>(c % 32u) <= world_to_cell(300.0f));
    // The same trimmed cells for a map placement there (stamp_map_obstacles uses the same stamper and hooks).
    {
        terrain::MapData m2;
        terrain::ObjectPlacement o;
        o.type_crc = types.raw().types().name_crc[modelled];
        o.position[0] = 300.0f, o.position[1] = 300.0f;
        m2.objects.push_back(o);
        o.type_crc = types.raw().types().name_crc[nomodel];  // fine-tune, no proxy, no model: not stamped
        o.position[0] = 100.0f, o.position[1] = 500.0f;
        m2.objects.push_back(o);
        PassabilityGrid pg;
        pg.init(32, 32);
        CHECK(stamp_map_obstacles(pg, m2, types, &ft) == 1 && !pg.stamped(map_stamp_id(1)));
        const std::span<const uint32_t> pc = pg.stamp_cells(map_stamp_id(0)), rc = rt.stamp_cells(51);
        CHECK(std::equal(pc.begin(), pc.end(), rc.begin(), rc.end()));
    }
    CHECK(!land_obstacle_footprint(types, nomodel, {100.0f, 500.0f}, 0.0f, 0.0f, &ft, fp));
    CHECK(land_obstacle_footprint(types, nomodel, {100.0f, 500.0f}, 0.0f, 0.0f, &hooks, fp) && !fp.fine_tune);  // no ray hook: Unknown
    CHECK(land_obstacle_footprint(types, fineoff, {300.0f, 300.0f}, 0.0f, 0.0f, &ft, fp) && !fp.fine_tune);
    // Proxy: the pad's sizes / offsets / scale and fine-tune flag, the barracks' own bits, the ray against the pad's model.
    rays = 0;
    CHECK(land_obstacle_footprint(types, barracks, {400.0f, 400.0f}, 0.0f, 0.0f, &ft, fp));
    CHECK(fp.width == 40.0f && fp.height == 40.0f && fp.x_offset == -20.0f && fp.y_offset == -20.0f && fp.bits == kPassRebelWall &&
          fp.fine_tune);
    PassabilityGrid pr;
    pr.init(32, 32);
    const uint32_t n_proxy = pr.stamp(60, fp);
    CHECK(n_proxy > 0 && rays > 0 && ray_type == pad && ray_scale == 3.0f);
    pad_has_model = false;  // the proxy's model has no render object: no ray, every sample stamps
    rays = 0;
    CHECK(land_obstacle_footprint(types, barracks, {400.0f, 400.0f}, 0.0f, 0.0f, &ft, fp) && !fp.fine_tune);
    CHECK(pr.stamp(60, fp) > n_proxy && rays == 0);
    // A grid with no cells still records the stamp (header promise; it was dropped before).
    PassabilityGrid empty;
    CHECK(empty.stamp(7, fp) == 0 && empty.stamped(7) && empty.stamp_cells(7).empty() && empty.stamp_secondary(7, fp) == 0 &&
          empty.secondary_stamped(7) && empty.unstamp(7) && !empty.stamped(7));

    // S8 t13 (MOVEMENT §1.3 "A type without Land_Model_Name", "proxy bits").
    const uint32_t off_both = types.resolve_name("OffBoth"), off_one = types.resolve_name("OffOne"),
                   width_only = types.resolve_name("WidthOnly"), wall_seg = types.resolve_name("WallSeg"),
                   shield_pad = types.resolve_name("ShieldPad"), pad_user = types.resolve_name("PadUser");
    CHECK(off_both < types.size() && off_one < types.size() && width_only < types.size() && wall_seg < types.size() &&
          shield_pad < types.size() && pad_user < types.size());
    if (off_both >= types.size() || off_one >= types.size() || width_only >= types.size() || wall_seg >= types.size() ||
        shield_pad >= types.size() || pad_user >= types.size())
        return;
    MapStampHooks nm;  // every type known to have no land model except WidthOnly (model box 15 x 10, no collision ray)
    nm.ray_model = [&](uint32_t t) { return t == width_only ? StampRayModel::Model : StampRayModel::None; };
    nm.model_half_extents = [&](uint32_t t, float& hx, float& hy, float& hz) {
        if (t != width_only) return false;
        hx = 15.0f, hy = 10.0f, hz = 4.0f;
        return true;
    };
    PassabilityGrid ng;
    ng.init(32, 32);
    // Fine-tune on, no proxy, no model: never stamped (NoModel above). Fine-tune off: the explicit rectangle, no ray.
    CHECK(land_obstacle_footprint(types, off_both, {300.0f, 300.0f}, 0.0f, 0.0f, &nm, fp) && !fp.fine_tune && fp.width == 40.0f &&
          fp.height == 20.0f && fp.x_offset == -20.0f && fp.y_offset == -10.0f && fp.bits == kPassObstacle);
    CHECK(ng.stamp(70, fp) == 6);  // x 280..320 (cells 14..16) x y 290..310 (cells 15..16)
    // ... and with only one explicit size: zero samples, but stamped (retail sets "stamped" and never retries).
    CHECK(land_obstacle_footprint(types, off_one, {300.0f, 300.0f}, 0.0f, 0.0f, &nm, fp) && fp.width == 40.0f && fp.height == 0.0f);
    CHECK(ng.stamp(71, fp) == 0 && ng.stamped(71) && ng.stamp_cells(71).empty());
    // Without model data in the run (Unknown) one size is still no footprint [port].
    CHECK(!land_obstacle_footprint(types, off_one, {300.0f, 300.0f}, 0.0f, 0.0f, nullptr, fp));
    // Each size getter separately: XML width 80, model height 2 x 10, offsets from the model box (-15, -10).
    CHECK(land_obstacle_footprint(types, width_only, {300.0f, 300.0f}, 0.0f, 0.0f, &nm, fp) && fp.width == 80.0f &&
          fp.height == 20.0f && fp.x_offset == -15.0f && fp.y_offset == -10.0f && fp.obstacle_height == 4.0f && !fp.fine_tune);
    // Bits: the object's own behaviours (WALL adds EmpireWall), never the proxy's OverridePassability.
    CHECK(land_obstacle_footprint(types, wall_seg, {300.0f, 300.0f}, 0.0f, 0.0f, nullptr, fp) &&
          fp.bits == (kPassObstacle | kPassEmpireWall));
    CHECK(land_obstacle_footprint(types, shield_pad, {300.0f, 300.0f}, 0.0f, 0.0f, nullptr, fp) && fp.bits == kPassShield);
    CHECK(land_obstacle_footprint(types, pad_user, {300.0f, 300.0f}, 0.0f, 0.0f, &nm, fp) && fp.width == 40.0f &&
          fp.bits == kPassObstacle && !fp.fine_tune);  // a proxy without a model: no ray, every sample stamps
    CHECK(land_obstacle_bits(0, true, false) == kPassObstacle && land_obstacle_bits(0, true, true) == (kPassObstacle | kPassEmpireWall) &&
          land_obstacle_bits(kPassRebelWall, true, true) == kPassRebelWall && land_obstacle_bits(0, false, false) == 0);
    remove_all_long(root, ec);
    std::printf("land obstacle footprint: object footprint = placement stamp, fine-tune / proxy ok\n");
}
#endif

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // unbuffered: a crash keeps the log
    test_heap();
    test_classes();
    test_grid();
    test_regions();
    test_land_paths();
    test_locomotion();
    test_formations();
    test_double_click();
    test_collision_helpers();
    test_collision_layer();  // S7 collision priority sort
    test_path_around();          // s78n t20
    test_destination_offsets();  // s78n t20
    test_walk_step_t20();        // s78n t20
    test_collision_system();
    test_land_group();
    test_push_search();
    test_space();
    test_space_index();
    test_space_group();
    test_oi_land();  // OI tactical
    test_space_compose_boxes();  // s78n t10
    test_oi_space();
    test_determinism();
    test_fighter_primitives();  // FIGHTER_FLYING §1-§5
    test_fighter_grids();
    test_fighter_moving();
    test_fighter_orbit();
    test_fighter_attack_run();
    test_fighter_dogfight();
    test_fighter_determinism();
#ifdef ALAMO_MOVEMENT_HAVE_TERRAIN
    test_space_obstacle_footprint();  // S7 t07 / t08
    test_land_obstacle_footprint();   // S8 t02
#endif
#ifdef MOVEMENT_REAL_DATA
    test_real_data();
#endif
    if (g_fail) {
        std::printf("%d check(s) failed\n", g_fail);
        return 1;
    }
    std::printf("all movement tests passed\n");
    return 0;
}
