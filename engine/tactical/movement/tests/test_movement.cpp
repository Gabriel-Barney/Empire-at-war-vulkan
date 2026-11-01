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
#include <string>
#include <thread>
#include <vector>

#include "alamo/core/sim_crc.hpp"
#include "alamo/core/sim_rng.hpp"
#include "alamo/tactical/movement/movement_system.hpp"

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

static int g_fail = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);          \
            ++g_fail;                                                         \
        }                                                                     \
    } while (0)

static ParallelFor make_thread_pf(unsigned threads) {
    return [threads](uint32_t count, uint32_t grain, const RangeFn& fn) {
        if (count == 0) return;
        const uint32_t gr = std::max<uint32_t>(1, grain);
        const uint32_t chunks = (count + gr - 1) / gr;
        std::atomic<uint32_t> next{0};
        auto worker = [&] {
            for (;;) {
                const uint32_t c = next.fetch_add(1);
                if (c >= chunks) break;
                const uint32_t b = c * gr;
                fn(b, std::min(count, b + gr));
            }
        };
        const unsigned extra = std::min<unsigned>(std::max(1u, threads), chunks) - 1;
        std::vector<std::thread> ts;
        for (unsigned t = 0; t < extra; ++t) ts.emplace_back(worker);
        worker();
        for (auto& t : ts) t.join();
    };
}

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
    // Dynamic local pathfinder around a blocker on the straight line.
    PassabilityGrid g;
    g.init(64, 64);
    MovementConstants k;
    std::vector<DynamicObstacle> obs{{{400.0f, 400.0f}, 30.0f}};
    std::vector<V2> out;
    uint32_t cx = 0;
    CHECK(dynamic_repath(g, {200.0f, 400.0f}, {600.0f, 400.0f}, kPassClear, 10.0f, false, obs, k, out, cx));
    bool avoids = true;
    for (size_t i = 1; i < out.size(); ++i) {
        const V2 a = out[i - 1], b = out[i];
        for (float s = 0.0f; s <= 1.0f; s += 0.02f)
            if (distance(lerp(a, b, s), obs[0].pos) < 30.0f) avoids = false;
    }
    CHECK(avoids && out.front() == (V2{200.0f, 400.0f}) && out.back() == (V2{600.0f, 400.0f}) && cx == 6);
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
        L.run(10, keep_all, count, full_budget, no_resolve);
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
            L.run(f, keep_all, count, full_budget, no_resolve);
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
        L.run(4, keep, count, full_budget, resolve);
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
        L.run(20, keep_all, [](const CollisionObject&) { return 2u; }, budget, resolve);
        CHECK((seen == std::vector<std::pair<uint32_t, bool>>{{4, true}, {3, true}, {2, true}, {1, false}, {0, false}}));
        bool same = L.last_order().size() == seen.size();
        for (size_t i = 0; same && i < seen.size(); ++i)
            same = L.last_order()[i].seq == seen[i].first && L.last_order()[i].full == seen[i].second;
        CHECK(same);
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
        L2.run(1, keep_all, [](const CollisionObject&) { return 2u; }, full_budget, [](CollisionObject& c, bool) {
            c.state = CollisionState::TryAgainLater;
            c.retry = 5;
        });
        StateHasher h3;
        L2.hash(h3);
        CHECK(h1.digest() != h3.digest());
    }
}

// Collision objects inside MovementSystem: creation, crowd box / refresh, cancellation, budget fallback.
static void test_collision_system() {
    // StoppedVsStopped: two overlapping parked units; crowd = 5 x ground units in the 160 x 160 box on their midpoint.
    {
        MovementSystem s{MovementConfig{}};
        PassabilityGrid g;
        g.init(40, 40);
        s.set_grid(std::move(g));
        s.add_unit(land_unit(1, {400.0f, 400.0f}, 7, 1.0f, 40.0f, kUccVehicle));
        s.add_unit(land_unit(2, {405.0f, 400.0f}, 7, 1.0f, 40.0f, kUccVehicle));
        s.add_unit(land_unit(3, {402.0f, 470.0f}, 0, 1.0f, 6.0f, kUccInfantry));  // inside the box (y 320 .. 480)
        s.add_unit(land_unit(4, {600.0f, 400.0f}, 0, 1.0f, 6.0f, kUccInfantry));  // outside
        SimRng rng(1);
        s.tick(0, rng);
        CHECK(s.collisions().list().size() == 1);
        if (s.collisions().list().size() == 1) {
            const CollisionObject& c = s.collisions().list()[0];
            CHECK(c.kind == CollisionKind::StoppedVsStopped && c.a == 1 && c.b == 2 && c.created == 0);
            CHECK(c.crowd == 15.0f && c.crowd_frame == 30 && c.state == CollisionState::Active);
        }
        // Both were pushed apart (each on its own check frames: one push per FramesPerCollisionCheck frames).
        CHECK(s.view(1).pos.x < 400.0f && s.view(2).pos.x > 405.0f);
        // A unit entering the box is seen only at the 30-frame refresh.
        CHECK(s.teleport(4, {402.0f, 330.0f}, 0.0f, 0.0f, 10));
        bool before = true;
        for (uint32_t f = 1; f < 30; ++f) {
            s.tick(f, rng);
            before = before && s.collisions().list().size() == 1 && s.collisions().list()[0].crowd == 15.0f;
        }
        s.tick(30, rng);
        CHECK(before && s.collisions().list().size() == 1 && s.collisions().list()[0].crowd == 20.0f &&
              s.collisions().list()[0].priority == 20.0f - 30.0f);
        // Pushed apart until the overlap ratio drops to 0.15: the collision no longer applies and is cancelled.
        for (uint32_t f = 31; f < 200 && !s.collisions().list().empty(); ++f) s.tick(f, rng);
        CHECK(s.collisions().list().empty() && s.stats().collisions_cancelled == 1 && s.stats().collisions_created == 1);
        CHECK(overlap_ratio(s.view(1).pos, 40.0f, s.view(2).pos, 40.0f) <= 0.15f);
    }
    // MovingVsStopped kept while it applies (zero quota: it waits in try-again-later), cancelled once it no longer
    // does - the mover stops, or the blocker starts moving.
    for (int variant = 0; variant < 2; ++variant) {
        MovementConfig cfg;
        cfg.constants.dynamic_land_complexity_quota = 0;
        MovementSystem s(cfg);
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
        CHECK(seq != UINT32_MAX && kept == 3 && s.collisions().list()[0].state == CollisionState::TryAgainLater);
        const uint32_t cancelled0 = s.stats().collisions_cancelled;
        if (variant == 0) {
            const ObjectId one = 1;
            s.stop(std::span<const ObjectId>(&one, 1));
            s.tick(++f, rng);
            CHECK(s.collisions().list().empty() && s.stats().collisions_cancelled == cancelled0 + 1);
        } else {
            MoveOrder away;
            away.units = {2};
            away.destination = cell_center(12, 35);
            s.move(away, rng, f + 1);
            bool gone = false;
            for (uint32_t n = 0; n < 20 && !gone; ++n) {
                s.tick(++f, rng);
                gone = true;
                for (const CollisionObject& c : s.collisions().list()) gone = gone && c.seq != seq;
            }
            CHECK(gone && s.stats().collisions_cancelled >= cancelled0 + 1 && s.view(2).has_goal);
        }
    }
    // Budget fallback: with a zero quota every MovingVsStopped resolution is the fallback (try again later, then push
    // the blocker aside); no dynamic search runs, and the mover still gets through.
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
        CHECK(distance(s.view(2).pos, cell_center(30, 20)) > 1.0f);  // the blocker was pushed aside
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
    const uint32_t frames = run_land(sys, 0, 6000, rng, serial_for, ground);
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
    CHECK(ground && arrived == 36 && near == 36 && frames < 6000);

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

    // Destination occupied by a stopped unit: early arrival within LandDestinationProximity.
    MovementSystem s4{MovementConfig{}};
    PassabilityGrid g4;
    g4.init(40, 40);
    s4.set_grid(std::move(g4));
    s4.add_unit(land_unit(1, cell_center(5, 5), 7, 1.5f, 18.0f, kUccVehicle));
    s4.add_unit(land_unit(2, cell_center(30, 30), 7, 1.5f, 18.0f, kUccVehicle));
    MoveOrder d;
    d.units = {1};
    d.destination = cell_center(30, 30);
    d.flags = kMoveFlagSkipValidation;
    s4.move(d, rng, 0);
    bool g4ok = true;
    run_land(s4, 0, 2000, rng, serial_for, g4ok);
    CHECK(s4.view(1).arrived && distance(s4.view(1).pos, cell_center(30, 30)) < 36.0f + 60.0f + 1.0f && s4.stats().early_arrivals == 1);
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

// ---------------------------------------------------------------- OI tactical (PHASE4 S7 open items)
static void test_oi_land() {
    // PathFrameDelay stagger (§1.6): two Solo formations; one synced draw each, in formation order; the second starts
    // after the first formation's accumulated PathFrameDelay.
    {
        MovementSystem sys{MovementConfig{}};
        PassabilityGrid g;
        g.init(64, 64);
        sys.set_grid(std::move(g));
        UnitDesc a = land_unit(1, cell_center(10, 10), 0, 1.0f, 6.0f, kUccInfantry);
        a.grouping = kGroupingSolo;
        a.path_frame_delay = 10;
        UnitDesc b = land_unit(2, cell_center(10, 30), 0, 1.0f, 6.0f, kUccInfantry);
        b.grouping = kGroupingSolo;
        b.path_frame_delay = 4;
        CHECK(sys.add_unit(a) && sys.add_unit(b));
        SimRng rng(77), ref(77);
        MoveOrder o;
        o.units = {1, 2};
        o.destination = cell_center(50, 20);
        sys.move(o, rng, 100);
        const uint32_t d1 = static_cast<uint32_t>(ref.range_int(0, 6)), d2 = static_cast<uint32_t>(ref.range_int(0, 6));
        CHECK(rng.draws() == ref.draws());  // exactly two Get_Int(0, Max_Move_Frame_Delay)
        // Formation order = seed nearest the bucket centroid first (equal here: the earlier unit, id 1).
        CHECK(sys.view(1).start_frame == 100 + d1);
        CHECK(sys.view(2).start_frame == 100 + 10 + d2);
        // Queued orders keep the per-unit delay (no stagger accumulator).
        MoveOrder q = o;
        q.flags = kMoveFlagQueue;
        SimRng r2(5), r2ref(5);
        sys.move(q, r2, 200);
        CHECK(sys.view(1).start_frame == 200 + static_cast<uint32_t>(r2ref.range_int(0, 6)));
        CHECK(sys.view(2).start_frame == 200 + static_cast<uint32_t>(r2ref.range_int(0, 6)));
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
        o.flags = kMoveFlagSkipValidation;
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
        // Hostility comes from the host (combat relations).
        sys.set_hostility([](int32_t, int32_t) { return true; });
        sys.set_owner(1, 5);
        MoveOrder back = o;
        back.destination = cell_center(5, 20);
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

// Golden digest of the fighter scenario (FIGHTER_FLYING 2026-10-29); MSVC and clang must both produce it.
static constexpr uint64_t kGoldenFighter = 0x5a7df50c208f33b1ull;

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
static constexpr uint64_t kGoldenLand = 0x8077ca79b9713e8eull;
static constexpr uint64_t kGoldenSpace = 0x243123bd32d07accull;

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
    test_collision_system();
    test_land_group();
    test_space();
    test_space_group();
    test_oi_land();  // OI tactical
    test_oi_space();
    test_determinism();
    test_fighter_primitives();  // FIGHTER_FLYING §1-§5
    test_fighter_grids();
    test_fighter_moving();
    test_fighter_orbit();
    test_fighter_attack_run();
    test_fighter_dogfight();
    test_fighter_determinism();
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
